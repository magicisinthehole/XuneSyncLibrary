#include "ZuneHTTPInterceptor.h"
#include "../ppp/PPPParser.h"
#include "HTTPParser.h"
#include "StaticModeHandler.h"
#include "ProxyModeHandler.h"
#include "HybridModeHandler.h"
#include <mtp/ptp/PipePacketer.h>
#include <mtp/usb/BulkPipe.h>
#include <mtp/ptp/ByteArrayObjectStream.h>
#include <chrono>
#include <sstream>
#include <iomanip>
#include <cstring>
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include <arpa/inet.h>

// --- Helper classes for bulk data streaming ---
class ByteArrayInputStream : public mtp::IObjectInputStream {
private:
    const mtp::ByteArray& _data;
    size_t _offset;
public:
    ByteArrayInputStream(const mtp::ByteArray& data) : _data(data), _offset(0) {}
    mtp::u64 GetSize() const override { return _data.size(); }
    size_t Read(mtp::u8 *data, size_t size) override {
        size_t remaining = _data.size() - _offset;
        size_t to_read = std::min(size, remaining);
        if (to_read > 0) {
            std::memcpy(data, _data.data() + _offset, to_read);
            _offset += to_read;
        }
        return to_read;
    }
    void Cancel() override {}
};

class ByteArrayOutputStream : public mtp::IObjectOutputStream {
public:
    mtp::ByteArray data;
    size_t Write(const uint8_t *buffer, size_t size) override {
        if (buffer && size > 0) {
            data.insert(data.end(), buffer, buffer + size);
        }
        return size;
    }
    void Cancel() override {
    }
};

// USB read timeout (milliseconds)
constexpr int USB_READ_TIMEOUT_MS = 100;

ZuneHTTPInterceptor::ZuneHTTPInterceptor(mtp::SessionPtr session)
    : session_(session) {
}

ZuneHTTPInterceptor::ZuneHTTPInterceptor(mtp::usb::DevicePtr device, mtp::usb::InterfacePtr interface,
                                         mtp::usb::EndpointPtr endpoint_in, mtp::usb::EndpointPtr endpoint_out,
                                         bool claim_interface)
    : session_(nullptr), usb_device_(device), usb_interface_(interface),
      endpoint_in_(endpoint_in), endpoint_out_(endpoint_out), endpoints_discovered_(true) {
    // When constructed with raw USB and pre-discovered endpoints, we skip endpoint discovery
    // Endpoints were discovered BEFORE MTP disconnect to ensure validity

    // Conditionally claim interface
    // If MTP is still active, do NOT claim (it's already claimed)
    // If MTP is closed, we MUST claim to get access
    if (claim_interface) {
        try {
            interface_token_ = usb_device_->ClaimInterface(usb_interface_);
            // Log will be called when SetLogCallback is invoked
        } catch (const std::exception& e) {
            throw std::runtime_error(std::string("Failed to claim USB interface: ") + e.what());
        }
    }
}

ZuneHTTPInterceptor::~ZuneHTTPInterceptor() {
    Stop();
}

void ZuneHTTPInterceptor::Start(const InterceptorConfig& config) {
    if (running_.load()) {
        throw std::runtime_error("HTTP interceptor is already running");
    }

    // Store configuration
    {
        std::lock_guard<std::mutex> lock(config_mutex_);
        config_ = config;
    }

    if (config_.mode == InterceptionMode::Disabled) {
        Log("Interceptor mode is disabled");
        return;
    }

    // Discover USB endpoints if not already discovered
    // (endpoints may have been pre-discovered before MTP disconnect)
    if (!endpoints_discovered_) {
        if (!DiscoverEndpoints()) {
            throw std::runtime_error("Failed to discover USB endpoints 0x01/0x81");
        }
    } else {
        Log("Using pre-discovered endpoints");
    }

    // Initialize parsers
    ppp_parser_ = std::make_unique<PPPParser>();
    http_parser_ = std::make_unique<HTTPParser>();

    // Initialize DNS hostname mappings
    // Resolve to the configured server IP (we intercept all traffic anyway)
    uint32_t dns_target_ip;
    if (!config_.server_ip.empty()) {
        dns_target_ip = IPParser::StringToIP(config_.server_ip);
        Log("DNS resolving to server IP: " + config_.server_ip);
    } else {
        // Default to a standard server IP if not configured
        dns_target_ip = IPParser::StringToIP("192.168.0.30");
        Log("DNS resolving to default server IP: 192.168.0.30");
    }

    dns_hostname_map_["catalog.zune.net"] = dns_target_ip;
    dns_hostname_map_["image.catalog.zune.net"] = dns_target_ip;
    dns_hostname_map_["art.zune.net"] = dns_target_ip;
    dns_hostname_map_["mix.zune.net"] = dns_target_ip;
    dns_hostname_map_["social.zune.net"] = dns_target_ip;
    dns_hostname_map_["go.microsoft.com"] = dns_target_ip;  // Proxy external requests
    Log("DNS server initialized with " + std::to_string(dns_hostname_map_.size()) + " hostname mappings");

    // Initialize mode-specific handler
    if (config_.mode == InterceptionMode::Static) {
        Log("Initializing static mode handler: " + config_.static_config.data_directory);
        if (config_.static_config.test_mode) {
            Log("  Test mode enabled - all UUIDs redirect to 00000000-0000-0000-0000-000000000000");
        }
        static_handler_ = std::make_unique<StaticModeHandler>(
            config_.static_config.data_directory,
            config_.static_config.test_mode);
        static_handler_->SetLogCallback(log_callback_);
    }
    else if (config_.mode == InterceptionMode::Proxy) {
        Log("Initializing proxy mode handler");
        ProxyModeHandler::ProxyConfig proxy_config;
        proxy_config.catalog_server = config_.proxy_config.catalog_server;
        proxy_config.image_server = config_.proxy_config.image_server;
        proxy_config.art_server = config_.proxy_config.art_server;
        proxy_config.mix_server = config_.proxy_config.mix_server;
        proxy_config.timeout_ms = config_.proxy_config.timeout_ms;

        proxy_handler_ = std::make_unique<ProxyModeHandler>(proxy_config);
        proxy_handler_->SetLogCallback(log_callback_);
    }
    else if (config_.mode == InterceptionMode::Hybrid) {
        Log("Initializing hybrid mode handler");
        hybrid_handler_ = std::make_unique<HybridModeHandler>(
            config_.proxy_config.catalog_server,
            config_.proxy_config.image_server,
            config_.proxy_config.art_server,
            config_.proxy_config.mix_server,
            config_.proxy_config.timeout_ms);
        hybrid_handler_->SetLogCallback(log_callback_);

        // Set callbacks (will be nullptr if not registered from C# yet)
        // Lock to safely read callback pointers
        {
            std::lock_guard<std::mutex> lock(callback_mutex_);
            hybrid_handler_->SetPathResolverCallback(path_resolver_callback_, path_resolver_user_data_);
            hybrid_handler_->SetCacheStorageCallback(cache_storage_callback_, cache_storage_user_data_);
        }

        Log("Hybrid mode handler initialized with proxy server: " + config_.proxy_config.catalog_server);
    }

    // Note: Monitoring thread will be started later by EnableNetworkPolling()
    // This allows TriggerNetworkMode() to complete LCP negotiation without interference
    running_.store(true);

    // Start request worker thread for serialized HTTP request processing
    request_worker_thread_ = std::make_unique<std::thread>(&ZuneHTTPInterceptor::RequestWorkerThread, this);

    Log("HTTP interceptor started successfully");
}

void ZuneHTTPInterceptor::Stop() {
    if (!running_.load()) {
        return;
    }

    Log("Stopping HTTP interceptor...");
    running_.store(false);

    // Notify request worker thread to wake up and check running_ flag
    request_queue_cv_.notify_one();

    // Wait for request worker thread to finish
    if (request_worker_thread_ && request_worker_thread_->joinable()) {
        request_worker_thread_->join();
    }

    // Wait for monitoring thread to finish
    if (monitor_thread_ && monitor_thread_->joinable()) {
        monitor_thread_->join();
    }

    // Clean up handlers
    static_handler_.reset();
    proxy_handler_.reset();
    ppp_parser_.reset();
    http_parser_.reset();

    // Clear connection states
    {
        std::lock_guard<std::mutex> lock(connections_mutex_);
        connections_.clear();
    }

    Log("HTTP interceptor stopped");
}

bool ZuneHTTPInterceptor::IsRunning() const {
    return running_.load();
}

InterceptorConfig ZuneHTTPInterceptor::GetConfig() const {
    std::lock_guard<std::mutex> lock(config_mutex_);
    return config_;
}

void ZuneHTTPInterceptor::SetLogCallback(LogCallback callback) {
    log_callback_ = callback;
}

bool ZuneHTTPInterceptor::InitializeHTTPSubsystem() {
    Log("Initializing HTTP subsystem on device (MTP vendor commands)...");

    if (!session_) {
        Log("Error: No active MTP session for initialization");
        return false;
    }

    try {
        // All commands from packet capture are MTP vendor commands
        // Using public Session wrapper methods for HTTP initialization

        // Cmd 1: OpCode 0x1002, Param1=1 - HTTP subsystem init
        session_->Operation1002(1);

        // Cmd 2: OpCode 0x1001 (Get Device Info) - already done in session init, skip

        // TODO: Cmd 3 needs user agent string data phase - implement when needed
        // For now, skip the user agent command

        // Cmd 4: OpCode 0x1014, Param1=0xd22f - Get device property
        session_->Operation1014(0xd22f);

        // Cmd 5: OpCode 0x1014, Param1=0xd402
        session_->Operation1014(0xd402);

        // Cmd 6: OpCode 0x9801, Param1=0x3009
        session_->Operation9801(0x3009);

        // Cmd 7: OpCode 0x9802, Param1=0xdc86, Param2=0x3009
        session_->Operation9802(0xdc86, 0x3009);

        // Cmd 8: OpCode 0x1004 (GetStorageIDs) - skip, likely not needed for HTTP
        // Cmd 9: OpCode 0x1005 (GetStorageInfo) - skip, likely not needed for HTTP

        std::this_thread::sleep_for(std::chrono::milliseconds(150));

        // Cmd 10: OpCode 0x9216
        session_->Operation9216();
        std::this_thread::sleep_for(std::chrono::milliseconds(100));

        // Cmd 11: OpCode 0x9216 again
        session_->Operation9216();

        // Cmd 12: OpCode 0x9212 (SSL setup)
        session_->Operation9212();

        // Cmd 13: OpCode 0x9213
        session_->Operation9213();

        // Cmd 14: OpCode 0x9212 again
        session_->Operation9212();

        // Cmd 15: OpCode 0x1005 - skip (storage info)

        // Cmd 16-19: OpCode 0x1014 with various device properties
        session_->Operation1014(0x5002);
        session_->Operation1014(0xd21c);
        session_->Operation1014(0xd225);
        session_->Operation1014(0xd401);
        std::this_thread::sleep_for(std::chrono::milliseconds(30));

        // Cmd 20: OpCode 0x9231 - FINAL TRIGGER COMMAND
        session_->Operation9231();

        Log("✓ HTTP subsystem initialization complete");
        Log("✓ Device should now accept HTTP interception");
        return true;

    } catch (const std::exception& e) {
        Log(std::string("HTTP initialization failed: ") + e.what());
        return false;
    }
}

// ============================================================================
// Private Methods
// ============================================================================

bool ZuneHTTPInterceptor::SendVendorCommand(const mtp::ByteArray& data) {
    try {
        if (!usb_device_ || !endpoint_out_ || !endpoint_in_) {
            Log("Error: Invalid USB device/endpoint for vendor command");
            return false;
        }

        // Send command
        auto input_stream = std::make_shared<ByteArrayInputStream>(data);
        usb_device_->WriteBulk(endpoint_out_, input_stream, 1000);

        // Read response from device (REQUEST-RESPONSE protocol)
        // The device expects us to read its response before sending the next command
        try {
            // Read initial response (usually 12 bytes)
            auto response_stream = std::make_shared<ByteArrayOutputStream>();
            usb_device_->ReadBulk(endpoint_in_, response_stream, 1000);

            // We'll try reading up to 3 more times with short timeout.
            for (int i = 0; i < 3; i++) {
                try {
                    response_stream = std::make_shared<ByteArrayOutputStream>();
                    usb_device_->ReadBulk(endpoint_in_, response_stream, 100);  // Short timeout
                } catch (...) {
                    // No more data available, that's fine
                    break;
                }
            }
        } catch (const std::exception& e) {
            // Some commands may not have responses, or timeout is expected
            // This is not a fatal error
            std::ostringstream oss;
            oss << "Note: No response or timeout reading response (this may be normal): " << e.what();
            Log(oss.str());
        }

        return true;
    } catch (const std::exception& e) {
        std::ostringstream oss;
        oss << "Error sending vendor command: " << e.what();
        Log(oss.str());
        return false;
    }
}

bool ZuneHTTPInterceptor::DiscoverEndpoints() {
    try {
        // If we have a session, extract USB from it
        // If we were constructed with raw USB, skip this step
        if (session_) {
            // Get the BulkPipe from the session (same pattern as GetZuneMetadata)
            auto pipe = session_->GetBulkPipe();
            if (!pipe) {
                Log("Error: Cannot access USB pipe from session");
                return false;
            }

            // Extract USB device and interface from the pipe
            usb_device_ = pipe->GetDevice();
            usb_interface_ = pipe->GetInterface();
        }

        // Verify we have USB access (either from session or constructor)
        if (!usb_device_ || !usb_interface_) {
            Log("Error: No USB device or interface available");
            return false;
        }

        // Iterate through all endpoints on this interface
        int endpoint_count = usb_interface_->GetEndpointsCount();
        Log("Scanning " + std::to_string(endpoint_count) + " endpoints for HTTP traffic");

        for (int i = 0; i < endpoint_count; ++i) {
            mtp::usb::EndpointPtr ep = usb_interface_->GetEndpoint(i);
            uint8_t address = ep->GetAddress();
            auto type = ep->GetType();
            auto direction = ep->GetDirection();

            // Debug: log all endpoints
            std::string type_str = (type == mtp::usb::EndpointType::Bulk) ? "Bulk" :
                                   (type == mtp::usb::EndpointType::Interrupt) ? "Interrupt" : "Other";
            std::string dir_str = (direction == mtp::usb::EndpointDirection::Out) ? "OUT" : "IN";

            Log("  Endpoint " + std::to_string(i) + ": address=0x" +
                std::to_string(address) + " type=" + type_str + " dir=" + dir_str);

            // Look for HTTP endpoints: address 0x01 with OUT and IN directions
            if (address == 0x01 && type == mtp::usb::EndpointType::Bulk) {
                if (direction == mtp::usb::EndpointDirection::Out) {
                    endpoint_out_ = ep;
                    Log("  → Found HTTP OUT endpoint: 0x01");
                }
                else if (direction == mtp::usb::EndpointDirection::In) {
                    endpoint_in_ = ep;
                    Log("  → Found HTTP IN endpoint: 0x01 (acts as 0x81)");
                }
            }

            // Look for interrupt endpoint 0x02 (shows as 0x82 on wire) for event notifications
            if (address == 0x02 && type == mtp::usb::EndpointType::Interrupt &&
                direction == mtp::usb::EndpointDirection::In) {
                endpoint_interrupt_ = ep;
                Log("  → Found interrupt endpoint: 0x02 (0x82) for event notifications");
            }
        }

        if (!endpoint_in_ || !endpoint_out_) {
            Log("Error: HTTP endpoints not found (need 0x01 OUT and 0x01 IN)");
            return false;
        }

        endpoints_discovered_ = true;
        Log("HTTP endpoints discovered successfully");
        return true;

    } catch (const std::exception& e) {
        Log("Error discovering endpoints: " + std::string(e.what()));
        return false;
    }
}

void ZuneHTTPInterceptor::MonitorThread() {
    Log("Monitoring thread started - continuous polling mode");
    Log("NOTE: Polling with 0x922d every 15ms (matches Windows Zune capture)");

    int poll_count = 0;
    int event_count = 0;

    while (running_.load()) {
        try {
            // Only poll if network mode is active
            // Calling 0x922d before network mode is triggered causes error 0x2002
            if (!network_polling_enabled_.load()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                continue;
            }

            if (!session_) {
                Log("ERROR: Session not available");
                std::this_thread::sleep_for(std::chrono::seconds(1));
                continue;
            }

            // Poll with 0x922d(3, 3) to check for network data
            // This matches the Windows Zune behavior: continuous polling every ~15-20ms
            poll_count++;
            mtp::ByteArray response_data = session_->Operation922d(3, 3);

            // Check if we received data (or just "CLIENT")
            if (response_data.empty()) {
                // No response - continue polling
                std::this_thread::sleep_for(std::chrono::milliseconds(15));
                continue;
            }

            // Check if it's just "CLIENT" (6 bytes: 43 4c 49 45 4e 54)
            if (response_data.size() == 6 &&
                response_data[0] == 0x43 && response_data[1] == 0x4c &&
                response_data[2] == 0x49 && response_data[3] == 0x45 &&
                response_data[4] == 0x4e && response_data[5] == 0x54) {
                // Device has no data ready - continue polling
                std::this_thread::sleep_for(std::chrono::milliseconds(15));
                continue;
            }

            // We got real data!
            event_count++;
            VerboseLog("Event #" + std::to_string(event_count) + ": 0x922d returned " +
                std::to_string(response_data.size()) + " bytes (after " +
                std::to_string(poll_count) + " polls)");

            // Log hex dump
            std::ostringstream hex_dump;
            for (size_t i = 0; i < std::min(response_data.size(), size_t(64)); ++i) {
                char buf[4];
                snprintf(buf, sizeof(buf), "%02x ", (unsigned char)response_data[i]);
                hex_dump << buf;
            }
            if (response_data.size() > 64) hex_dump << "...";
            VerboseLog("  Data: " + hex_dump.str());

            // Process the received packet (should contain PPP-framed HTTP data)
            ProcessPacket(response_data);

            // Reset poll count after successful event
            poll_count = 0;

            // Continue polling immediately (no sleep) after receiving data
            // to catch any additional packets

        } catch (const std::exception& e) {
            if (running_.load()) {
                Log("Error in monitoring thread: " + std::string(e.what()));
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
        }
    }

    Log("Monitoring thread stopped (" + std::to_string(event_count) + " network events received)");
}

void ZuneHTTPInterceptor::ProcessPacket(const mtp::ByteArray& usb_data) {
    try {
        // Log raw USB data received (truncated to 1024 bytes)
        std::ostringstream hex_dump;
        hex_dump << "USB DATA RECEIVED (" << usb_data.size() << " bytes): ";
        for (size_t i = 0; i < std::min(usb_data.size(), size_t(1024)); i++) {
            hex_dump << std::hex << std::setw(2) << std::setfill('0')
                     << (int)usb_data[i];
            if ((i + 1) % 32 == 0) hex_dump << "\n  ";
        }
        if (usb_data.size() > 1024) hex_dump << "... (truncated)";
        VerboseLog(hex_dump.str());

        // Step 0: Prepend any incomplete frame from previous USB packet
        mtp::ByteArray combined_data;
        if (!incomplete_ppp_frame_buffer_.empty()) {
            VerboseLog("Prepending " + std::to_string(incomplete_ppp_frame_buffer_.size()) +
                " bytes from incomplete PPP frame buffer");
            combined_data.insert(combined_data.end(),
                               incomplete_ppp_frame_buffer_.begin(),
                               incomplete_ppp_frame_buffer_.end());
            combined_data.insert(combined_data.end(), usb_data.begin(), usb_data.end());
        } else {
            combined_data = usb_data;
        }

        // Step 1: Split combined data into individual PPP frames
        // A single USB packet can contain MULTIPLE PPP frames separated by 0x7E
        // PPP frames can also span multiple USB packets
        std::vector<mtp::ByteArray> frames;
        size_t i = 0;
        while (i < combined_data.size()) {
            // Find frame start (0x7E)
            if (combined_data[i] == 0x7E) {
                size_t frame_start = i;
                // Find frame end (next 0x7E)
                i++;
                while (i < combined_data.size() && combined_data[i] != 0x7E) {
                    i++;
                }
                if (i < combined_data.size()) {
                    // Found complete frame (includes both 0x7E delimiters)
                    mtp::ByteArray frame(combined_data.begin() + frame_start,
                                        combined_data.begin() + i + 1);
                    frames.push_back(frame);
                } else {
                    // Incomplete frame - starts with 0x7E but no closing 0x7E found
                    // Save for next USB packet
                    incomplete_ppp_frame_buffer_.assign(combined_data.begin() + frame_start,
                                                        combined_data.end());
                    Log("Buffering incomplete PPP frame (" +
                        std::to_string(incomplete_ppp_frame_buffer_.size()) + " bytes)");
                    break;  // Don't process incomplete frame
                }
            }
            i++;
        }

        // Clear buffer if we successfully processed all data
        if (i >= combined_data.size() && !frames.empty()) {
            incomplete_ppp_frame_buffer_.clear();
        }

        VerboseLog("Found " + std::to_string(frames.size()) + " PPP frame(s) in USB packet");

        // Step 2: Process each PPP frame
        for (size_t frame_idx = 0; frame_idx < frames.size(); frame_idx++) {
            VerboseLog("Processing PPP frame " + std::to_string(frame_idx + 1) + "/" +
                std::to_string(frames.size()) + " (" + std::to_string(frames[frame_idx].size()) + " bytes)");
            ProcessPPPFrame(frames[frame_idx]);
        }

        // Step 3: Process all pending sends
        // After processing all PPP frames in this USB packet, send next batches for connections that need it
        // This prevents sending multiple batches when a single USB packet contains multiple ACKs
        std::set<std::string> connections_to_send;
        {
            std::lock_guard<std::mutex> lock(pending_sends_mutex_);
            connections_to_send = pending_sends_;
            pending_sends_.clear();
        }

        for (const std::string& conn_key : connections_to_send) {
            SendNextBatch(conn_key);
        }

    } catch (const std::exception& e) {
        Log("Error processing packet: " + std::string(e.what()));
    }
}

void ZuneHTTPInterceptor::ProcessPPPFrame(const mtp::ByteArray& frame_data) {
    try {
        // Step 1: Check if valid PPP frame
        if (!PPPParser::IsValidFrame(frame_data)) {
            return;  // Not a PPP frame, ignore
        }

        // Step 2: Extract payload from PPP frame
        uint16_t ppp_protocol = 0;
        mtp::ByteArray payload = PPPParser::ExtractPayload(frame_data, &ppp_protocol);

        // Step 3: Route based on PPP protocol
        if (ppp_protocol == 0x8021) {
            // IPCP: Handle network configuration
            HandleIPCPPacket(payload);
            return;
        }
        else if (ppp_protocol == 0x80fd) {
            // CCP (Compression Control Protocol): Reject compression
            // Device requests compression, but we don't support it
            if (payload.size() >= 4) {
                uint8_t code = payload[0];
                uint8_t identifier = payload[1];

                if (code == 1) {  // Config-Request
                    VerboseLog("CCP Config-Request received (id=" + std::to_string(identifier) + "), sending Config-Reject");

                    // Build Config-Reject response
                    mtp::ByteArray ccp_reject;
                    ccp_reject.push_back(0x04);  // Config-Reject
                    ccp_reject.push_back(identifier);  // Same identifier
                    uint16_t length = payload.size();
                    ccp_reject.push_back((length >> 8) & 0xFF);
                    ccp_reject.push_back(length & 0xFF);
                    // Copy original options
                    ccp_reject.insert(ccp_reject.end(), payload.begin() + 4, payload.end());

                    // Wrap in PPP frame and queue
                    mtp::ByteArray ccp_frame = PPPParser::WrapPayload(ccp_reject, 0x80fd);
                    {
                        std::lock_guard<std::mutex> lock(response_queue_mutex_);
                        response_queue_.push_back(ccp_frame);
                        VerboseLog("CCP Config-Reject queued (" + std::to_string(response_queue_.size()) + " total in queue)");
                    }
                    // NOTE: Do NOT drain here - causes deadlock! Monitoring loop will drain.
                }
            }
            return;
        }
        else if (ppp_protocol != 0x0021) {
            // Not IPv4, IPCP, or CCP, ignore (could be LCP, IPv6, etc.)
            std::ostringstream hex_proto;
            hex_proto << "0x" << std::hex << std::setw(4) << std::setfill('0') << ppp_protocol
                     << " (" << PPPParser::GetProtocolName(ppp_protocol) << ")";
            Log("Ignoring PPP protocol " + hex_proto.str());
            return;
        }

        // IPv4 packet - continue processing
        mtp::ByteArray ip_packet = payload;

        // Step 3: Parse IP header and extract TCP segment
        IPParser::IPHeader ip_header = IPParser::ParseHeader(ip_packet);

        // Log ALL IP traffic to see what's happening
        std::string protocol_name = (ip_header.protocol == 6) ? "TCP" :
                                   (ip_header.protocol == 17) ? "UDP" :
                                   (ip_header.protocol == 1) ? "ICMP" :
                                   "OTHER(" + std::to_string(ip_header.protocol) + ")";
        VerboseLog("IP packet: " + IPParser::IPToString(ip_header.src_ip) + " -> " +
            IPParser::IPToString(ip_header.dst_ip) + " protocol=" + protocol_name);

        // Handle UDP (protocol 17) for DNS queries
        if (ip_header.protocol == 17) {
            mtp::ByteArray udp_segment = IPParser::ExtractPayload(ip_packet);
            if (udp_segment.size() >= 8) {
                uint16_t src_port = (udp_segment[0] << 8) | udp_segment[1];
                uint16_t dst_port = (udp_segment[2] << 8) | udp_segment[3];

                if (dst_port == 53) {
                    // DNS query
                    Log("DNS query detected from port " + std::to_string(src_port));
                    HandleDNSQuery(ip_packet);
                }
            }
            return;
        }

        if (ip_header.protocol != 6) {
            // Not TCP or UDP, ignore (but we logged it above)
            return;
        }

        mtp::ByteArray tcp_segment = IPParser::ExtractPayload(ip_packet);

        // Step 4: Parse TCP header and extract HTTP data
        TCPParser::TCPHeader tcp_header = TCPParser::ParseHeader(tcp_segment);

        VerboseLog("TCP packet: " + IPParser::IPToString(ip_header.src_ip) + ":" +
            std::to_string(tcp_header.src_port) + " -> " +
            IPParser::IPToString(ip_header.dst_ip) + ":" +
            std::to_string(tcp_header.dst_port) + " [" +
            TCPParser::FlagsToString(tcp_header.flags) + "]");

        // Update connection state
        std::string conn_key = MakeConnectionKey(
            ip_header.src_ip, tcp_header.src_port,
            ip_header.dst_ip, tcp_header.dst_port);

        TCPConnectionState& conn_state = GetConnectionState(conn_key);

        // Handle TCP handshake
        if (tcp_header.flags & TCPParser::TCP_FLAG_SYN) {
            if (!(tcp_header.flags & TCPParser::TCP_FLAG_ACK)) {
                // Pure SYN packet - respond with SYN-ACK
                conn_state.syn_received = true;
                conn_state.connected = false;
                conn_state.http_buffer.clear();  // Clear buffer for new connection
                // Generate our initial sequence number
                conn_state.seq_num = rand() % 0xFFFFFFFF;
                // ACK their SYN (their SEQ + 1)
                conn_state.ack_num = tcp_header.seq_num + 1;

                Log("Sending SYN-ACK response");
                SendTCPResponse(
                    ip_header.dst_ip, tcp_header.dst_port,  // Swap: from server
                    ip_header.src_ip, tcp_header.src_port,  // Swap: to client
                    conn_state.seq_num,                     // Our initial SEQ
                    conn_state.ack_num,                     // ACK = their SEQ + 1
                    TCPParser::TCP_FLAG_SYN | TCPParser::TCP_FLAG_ACK
                );
                return;
            }
            // SYN-ACK from device - just note it
            return;
        }

        // Handle connection termination
        if (tcp_header.flags & (TCPParser::TCP_FLAG_FIN | TCPParser::TCP_FLAG_RST)) {
            Log("TCP connection closing (FIN or RST received)");
            conn_state.connected = false;
            conn_state.http_buffer.clear();  // Clear buffer on connection close

            // Send ACK for FIN
            if (tcp_header.flags & TCPParser::TCP_FLAG_FIN) {
                conn_state.ack_num = tcp_header.seq_num + 1;
                SendTCPResponse(
                    ip_header.dst_ip, tcp_header.dst_port,
                    ip_header.src_ip, tcp_header.src_port,
                    conn_state.seq_num,
                    conn_state.ack_num,
                    TCPParser::TCP_FLAG_ACK
                );
            }
            return;
        }

        // Handle final ACK of handshake
        if ((tcp_header.flags == TCPParser::TCP_FLAG_ACK) && conn_state.syn_received && !conn_state.connected) {
            conn_state.connected = true;
            conn_state.seq_num++;  // Increment after SYN
            Log("TCP connection established");
            return;
        }

        // Extract HTTP data
        mtp::ByteArray http_data = TCPParser::ExtractPayload(tcp_segment);

        if (http_data.empty()) {
         
            bool should_send_next_batch = false;
            std::string matching_trans_key;
            {
                std::lock_guard<std::mutex> lock(transmissions_mutex_);

                // Search all transmissions for this connection
                std::string conn_key_prefix = conn_key + ":";
                uint32_t ack_num = tcp_header.ack_num;

                for (auto& [key, trans_state] : active_transmissions_) {
                    // Check if this transmission belongs to this connection
                    if (key.rfind(conn_key_prefix, 0) == 0) {  // starts_with
                        // Calculate total payload size for this transmission
                        size_t total_payload = 0;
                        for (size_t payload_size : trans_state.segment_payload_sizes) {
                            total_payload += payload_size;
                        }

                        // Check if ACK is in this transmission's sequence range
                        // ACK acknowledges bytes up to (but not including) the ACK number
                        // Valid ACK range: base_seq < ack_num <= base_seq + total_payload
                        uint32_t trans_end_seq = trans_state.base_seq + total_payload;
                        if (ack_num > trans_state.base_seq && ack_num <= trans_end_seq) {
                            // This ACK belongs to this transmission!
                            matching_trans_key = key;
                            break;  // Found it!
                        }
                    }
                }

                if (!matching_trans_key.empty()) {
                    HTTPResponseTransmissionState& trans_state = active_transmissions_[matching_trans_key];

                    // Update transmission state with new ACK and window size
                    uint32_t new_ack = tcp_header.ack_num;
                    uint16_t new_window = tcp_header.window_size;
                    uint16_t old_window = trans_state.window_size;

                    Log("ACK received for transmission: ACK=" + std::to_string(new_ack) +
                        " window=" + std::to_string(new_window) +
                        " (last_acked=" + std::to_string(trans_state.last_acked_seq) +
                        ", old_window=" + std::to_string(old_window) + ")");

                    // A duplicate ACK must satisfy ALL these conditions:
                    // 1. ACK number doesn't advance
                    // 2. Window right edge unchanged (ACK + window)
                    // 3. Has unacknowledged data in flight
                    uint32_t old_right_edge = trans_state.last_acked_seq + old_window;
                    uint32_t new_right_edge = new_ack + new_window;
                    bool window_edge_unchanged = (new_right_edge == old_right_edge);
                    bool has_unacked_data = (trans_state.bytes_in_flight > 0);
                    bool is_duplicate_ack = (new_ack == trans_state.last_acked_seq) &&
                                           window_edge_unchanged &&
                                           has_unacked_data;

                    // Window update: ACK doesn't advance but right edge moves (window opened)
                    bool is_window_update = (new_ack == trans_state.last_acked_seq) && !window_edge_unchanged;

                    if (is_window_update) {
                        Log("Window update ACK detected: right edge " +
                            std::to_string(old_right_edge) + " -> " + std::to_string(new_right_edge) +
                            " (window " + std::to_string(old_window) + " -> " + std::to_string(new_window) + ")");
                    }

                    if (is_duplicate_ack) {
                        trans_state.duplicate_ack_count++;
                        Log("Duplicate ACK detected (count=" + std::to_string(trans_state.duplicate_ack_count) +
                            "): ACK=" + std::to_string(new_ack));

                        // For duplicate ACKs #1 and #2: Try to send more segments if we have data and window space
                        // Duplicate ACKs indicate the receiver is waiting for more data (has received out-of-order segments)
                        // We should continue sending to fill the available window, not stall waiting for the 3rd dupack
                        if (trans_state.duplicate_ack_count < 3) {
                            should_send_next_batch = true;
                        }

                        // Fast retransmit after 3 duplicate ACKs (RFC 2581)
                        // Only trigger if not already in fast recovery (matches lwIP's TF_INFR flag)
                        else if (trans_state.duplicate_ack_count == 3 && !trans_state.in_fast_recovery) {
                            Log("Fast retransmit triggered: 3 duplicate ACKs received for SEQ=" +
                                std::to_string(new_ack));
                            trans_state.in_fast_recovery = true;
                            trans_state.retransmit_needed = true;

                            // RFC 5681: Set ssthresh = max(FlightSize/2, 2*MSS)
                            trans_state.ssthresh = std::max(trans_state.bytes_in_flight / 2,
                                                             2 * HTTPResponseTransmissionState::mss);

                            // This accounts for the 3 segments buffered at receiver
                            trans_state.cwnd = trans_state.ssthresh + 3 * HTTPResponseTransmissionState::mss;

                            Log("Fast recovery entered: ssthresh=" + std::to_string(trans_state.ssthresh) +
                                ", cwnd=" + std::to_string(trans_state.cwnd) +
                                " (inflated from " + std::to_string(trans_state.bytes_in_flight) + " bytes in flight)");

                            // Calculate which segment index corresponds to last_acked_seq
                            // We'll retransmit starting from next_segment_index position where bytes stopped advancing
                            uint32_t expected_next_seq = trans_state.base_seq;
                            for (size_t i = 0; i < trans_state.next_segment_index && i < trans_state.queued_segments.size(); i++) {
                                if (expected_next_seq + trans_state.segment_payload_sizes[i] == new_ack) {
                                    trans_state.retransmit_segment_index = i + 1;  // Retransmit the NEXT segment
                                    Log("Will retransmit from segment " + std::to_string(trans_state.retransmit_segment_index) +
                                        "/" + std::to_string(trans_state.queued_segments.size()));
                                    break;
                                }
                                expected_next_seq += trans_state.segment_payload_sizes[i];
                            }

                            should_send_next_batch = true;  // Trigger retransmission
                        }
                        // RFC 5681: For each additional duplicate ACK (after the third),
                        // increment cwnd by MSS. This inflates the congestion window to reflect
                        // additional segments that have left the network (been buffered at receiver).
                        // Then try to send new segments using the inflated cwnd.
                        else if (trans_state.duplicate_ack_count > 3 && trans_state.in_fast_recovery) {
                            trans_state.cwnd += HTTPResponseTransmissionState::mss;

                            Log("Fast recovery: additional dupack (#" + std::to_string(trans_state.duplicate_ack_count) +
                                "), cwnd inflated to " + std::to_string(trans_state.cwnd));

                            // Try to send new segments with inflated cwnd
                            should_send_next_batch = true;
                        }
                    }

                    // Process if ACK advances (new data acknowledged) OR window update
                    if (new_ack > trans_state.last_acked_seq || is_window_update) {
                        // Reset duplicate ACK counter when ACK advances
                        trans_state.duplicate_ack_count = 0;
                        trans_state.retransmit_needed = false;

                        // Only exit fast recovery on NEW ACK (RFC 5681)
                        // Don't exit on pure window updates - stay in fast recovery until new data is ACKed
                        // This matches lwIP behavior: tcp_receive() only clears TF_INFR when ACK advances
                        if (new_ack > trans_state.last_acked_seq) {
                            trans_state.in_fast_recovery = false;  // Exit fast recovery

                            // RFC 5681: On exiting fast recovery, set cwnd to ssthresh
                            // (deflate the window after artificial inflation)
                            if (trans_state.cwnd > trans_state.ssthresh) {
                                trans_state.cwnd = trans_state.ssthresh;
                                Log("Fast recovery exited: cwnd deflated to ssthresh=" +
                                    std::to_string(trans_state.ssthresh));
                            }
                        }

                        // Calculate how many bytes were ACKed (0 for window updates)
                        uint32_t bytes_acked = (new_ack > trans_state.last_acked_seq) ?
                                              (new_ack - trans_state.last_acked_seq) : 0;

                        // Update transmission state
                        trans_state.last_acked_seq = new_ack;
                        trans_state.window_size = new_window;
                        trans_state.last_ack_time = std::chrono::steady_clock::now();

                        // Reduce bytes in flight by the amount ACKed
                        if (bytes_acked > 0) {
                            if (trans_state.bytes_in_flight >= bytes_acked) {
                                trans_state.bytes_in_flight -= bytes_acked;
                            } else {
                                trans_state.bytes_in_flight = 0;
                            }

                            Log("Transmission progress: " + std::to_string(bytes_acked) +
                                " bytes ACKed, " + std::to_string(trans_state.bytes_in_flight) +
                                " bytes in flight, segment " + std::to_string(trans_state.next_segment_index) +
                                "/" + std::to_string(trans_state.queued_segments.size()));

                            // RFC 3465: Grow cwnd when ACK advances (only outside of fast recovery)
                            // This matches lwIP's implementation exactly
                            if (!trans_state.in_fast_recovery) {
                                size_t old_cwnd = trans_state.cwnd;
                                if (trans_state.cwnd < trans_state.ssthresh) {
                                    // Slow start: increase cwnd by min(bytes_acked, 2*MSS)
                                    // RFC 3465, section 2.2: Allow up to 2 SMSS per ACK
                                    size_t increase = std::min(bytes_acked,
                                                              static_cast<uint32_t>(2 * HTTPResponseTransmissionState::mss));
                                    trans_state.cwnd += increase;
                                } else {
                                    // Congestion avoidance: accumulate bytes, grow by 1 MSS per cwnd bytes
                                    // RFC 3465, section 2.1: Appropriate Byte Counting
                                    trans_state.bytes_acked_accumulator += bytes_acked;
                                    if (trans_state.bytes_acked_accumulator >= trans_state.cwnd) {
                                        trans_state.bytes_acked_accumulator -= trans_state.cwnd;
                                        trans_state.cwnd += HTTPResponseTransmissionState::mss;
                                    }
                                }

                                // NOTE: Cwnd grows naturally via slow start and congestion avoidance (RFC 3465)
                                // No artificial cap - connection reuse throttling handles application-layer processing time
                                // Device window size (~33KB) is tracked via window_size and bytes_in_flight

                                if (trans_state.cwnd != old_cwnd) {
                                    Log("cwnd grown: " + std::to_string(old_cwnd) + " -> " + std::to_string(trans_state.cwnd) +
                                        " (ssthresh=" + std::to_string(trans_state.ssthresh) + ")");
                                }
                            }
                        } else {
                            Log("Window update processed: " + std::to_string(trans_state.bytes_in_flight) +
                                " bytes still in flight, segment " + std::to_string(trans_state.next_segment_index) +
                                "/" + std::to_string(trans_state.queued_segments.size()));
                        }

                        // Check if transmission is complete (all segments sent and ACKed)
                        if (trans_state.next_segment_index >= trans_state.queued_segments.size() &&
                            trans_state.bytes_in_flight == 0) {
                            trans_state.transmission_complete = true;
                            Log("Transmission complete for " + matching_trans_key + " - all segments sent and ACKed");

                            // FIX: Record completion time for connection reuse throttling
                            // Extract conn_key from transmission key (format: "conn_key:base_seq")
                            size_t last_colon = matching_trans_key.rfind(':');
                            if (last_colon != std::string::npos) {
                                std::string conn_key_only = matching_trans_key.substr(0, last_colon);
                                std::lock_guard<std::mutex> completion_lock(connection_completion_mutex_);
                                connection_last_completion_time_[conn_key_only] = std::chrono::steady_clock::now();
                            }

                            // Note: Global large image throttling now uses start-time-based delays,
                            // not completion tracking, to avoid deadlock issues.

                            // Clean up transmission state (erase from map)
                            active_transmissions_.erase(matching_trans_key);
                        }
                        // Check if we should send next batch (both for ACKs and window updates)
                        else if (!trans_state.transmission_complete &&
                                 trans_state.next_segment_index < trans_state.queued_segments.size()) {
                            should_send_next_batch = true;
                        }
                    }
                }
            }  // Release lock

            // Instead of sending immediately, add to pending_sends
            // This prevents sending multiple batches when a single USB packet contains multiple ACKs
            if (should_send_next_batch && !matching_trans_key.empty()) {
                std::lock_guard<std::mutex> lock(pending_sends_mutex_);
                pending_sends_.insert(matching_trans_key);
            }

            return;
        }

        // Step 5: TCP Retransmission Detection
        // Check if this is a duplicate SEQ number (retransmission)
        if (conn_state.last_received_seq != 0 && tcp_header.seq_num == conn_state.last_received_seq) {
            Log("TCP retransmission detected: SEQ=" + std::to_string(tcp_header.seq_num) +
                " (duplicate, ignoring " + std::to_string(http_data.size()) + " bytes)");
            return;  // Ignore retransmitted data
        }

        // Update last received SEQ
        conn_state.last_received_seq = tcp_header.seq_num;

        // Step 6: TCP Stream Reassembly
        // Append new data to connection buffer
        conn_state.http_buffer.insert(conn_state.http_buffer.end(),
                                      http_data.begin(),
                                      http_data.end());

        // Check for Zune custom DNS protocol BEFORE HTTP
        // DNS uses 8-byte custom framing: [ID1][0x0035][LEN][0x0000][DNS message]
        if (conn_state.http_buffer.size() >= 20) {
            uint16_t word0 = (conn_state.http_buffer[0] << 8) | conn_state.http_buffer[1];
            uint16_t word1 = (conn_state.http_buffer[2] << 8) | conn_state.http_buffer[3];
            uint16_t length_field = (conn_state.http_buffer[4] << 8) | conn_state.http_buffer[5];
            uint16_t reserved = (conn_state.http_buffer[6] << 8) | conn_state.http_buffer[7];

            // Check for DNS query: bytes[2:4] == 0x0035, reserved == 0x0000
            if (word1 == 0x0035 && reserved == 0x0000) {
                // Verify length field matches
                // NOTE: length_field IS the total TCP payload length (including 8-byte prefix)
                size_t expected_total_length = length_field;

                if (conn_state.http_buffer.size() >= expected_total_length) {
                    Log("DNS query detected in TCP payload");
                    Log("  8-byte prefix: " + std::string(1, (word0 >> 8) & 0xFF) +
                        std::string(1, word0 & 0xFF) + std::string(1, (word1 >> 8) & 0xFF) +
                        std::string(1, word1 & 0xFF));

                    // Parse DNS message (starts at byte 8)
                    mtp::ByteArray dns_message(conn_state.http_buffer.begin() + 8,
                                               conn_state.http_buffer.begin() + expected_total_length);

                    // Parse DNS query
                    if (dns_message.size() >= 12) {
                        uint16_t txn_id = (dns_message[0] << 8) | dns_message[1];
                        uint16_t flags = (dns_message[2] << 8) | dns_message[3];
                        uint16_t qdcount = (dns_message[4] << 8) | dns_message[5];
                        uint16_t ancount = (dns_message[6] << 8) | dns_message[7];

                        Log("  DNS Transaction ID: 0x" + std::to_string(txn_id));
                        Log("  Questions: " + std::to_string(qdcount) + ", Answers: " + std::to_string(ancount));

                        if (qdcount == 1 && ancount == 0) {
                            // Parse domain name from question section
                            std::string domain_name;
                            size_t i = 12;
                            while (i < dns_message.size() && dns_message[i] != 0) {
                                uint8_t label_len = dns_message[i];
                                i++;
                                if (i + label_len > dns_message.size()) break;

                                if (!domain_name.empty()) domain_name += ".";
                                domain_name += std::string(dns_message.begin() + i,
                                                          dns_message.begin() + i + label_len);
                                i += label_len;
                            }

                            Log("  Query: " + domain_name);

                            // Look up in DNS hostname map
                            if (dns_hostname_map_.find(domain_name) != dns_hostname_map_.end()) {
                                uint32_t resolved_ip = dns_hostname_map_[domain_name];
                                Log("  Resolved: " + domain_name + " -> " + IPParser::IPToString(resolved_ip));

                                // Build DNS response
                                // Start with DNS message header
                                mtp::ByteArray dns_response;
                                dns_response.push_back((txn_id >> 8) & 0xFF);
                                dns_response.push_back(txn_id & 0xFF);

                                // Flags: QR=1, RD=1, RA=1, RCODE=0 → 0x8580
                                dns_response.push_back(0x85);
                                dns_response.push_back(0x80);

                                // Counts: 1 question, 1 answer, 0 authority, 0 additional
                                dns_response.push_back(0x00);
                                dns_response.push_back(0x01);  // qdcount
                                dns_response.push_back(0x00);
                                dns_response.push_back(0x01);  // ancount
                                dns_response.push_back(0x00);
                                dns_response.push_back(0x00);  // nscount
                                dns_response.push_back(0x00);
                                dns_response.push_back(0x00);  // arcount

                                // Copy question section from query
                                size_t question_start = 12;
                                size_t question_end = question_start;
                                while (question_end < dns_message.size() && dns_message[question_end] != 0) {
                                    uint8_t len = dns_message[question_end];
                                    question_end += 1 + len;
                                }
                                question_end += 1;  // null terminator
                                question_end += 4;  // qtype + qclass

                                dns_response.insert(dns_response.end(),
                                                   dns_message.begin() + question_start,
                                                   dns_message.begin() + question_end);

                                // Answer section: name pointer to question
                                dns_response.push_back(0xC0);  // Pointer
                                dns_response.push_back(0x0C);  // Offset 12 (start of question)

                                // Type: A record (1)
                                dns_response.push_back(0x00);
                                dns_response.push_back(0x01);

                                // Class: IN (1)
                                dns_response.push_back(0x00);
                                dns_response.push_back(0x01);

                                // TTL: 1800 seconds (matches capture)
                                dns_response.push_back(0x00);
                                dns_response.push_back(0x00);
                                dns_response.push_back(0x07);
                                dns_response.push_back(0x08);

                                // RDLength: 4 bytes
                                dns_response.push_back(0x00);
                                dns_response.push_back(0x04);

                                // IP address (4 bytes)
                                dns_response.push_back((resolved_ip >> 24) & 0xFF);
                                dns_response.push_back((resolved_ip >> 16) & 0xFF);
                                dns_response.push_back((resolved_ip >> 8) & 0xFF);
                                dns_response.push_back(resolved_ip & 0xFF);

                                // Build complete TCP payload with 8-byte custom framing
                                mtp::ByteArray tcp_payload;

                                // 8-byte prefix (swap pattern for response)
                                // Query:    [ID1][0x0035][LEN][0x0000]
                                // Response: [0x0035][ID1][LEN][0x0000]
                                tcp_payload.push_back(0x00);  // 0x0035 high byte
                                tcp_payload.push_back(0x35);  // 0x0035 low byte
                                tcp_payload.push_back((word0 >> 8) & 0xFF);  // Echo query's ID1
                                tcp_payload.push_back(word0 & 0xFF);

                                // Length field = TOTAL TCP payload length (8-byte prefix + DNS message)
                                uint16_t response_length_field = dns_response.size() + 8;
                                tcp_payload.push_back((response_length_field >> 8) & 0xFF);
                                tcp_payload.push_back(response_length_field & 0xFF);

                                // Reserved (0x0000)
                                tcp_payload.push_back(0x00);
                                tcp_payload.push_back(0x00);

                                // Append DNS response
                                tcp_payload.insert(tcp_payload.end(), dns_response.begin(), dns_response.end());

                                // Update connection state for response
                                conn_state.ack_num = tcp_header.seq_num + conn_state.http_buffer.size();

                                // Send TCP response with DNS data
                                Log("Sending DNS response (" + std::to_string(tcp_payload.size()) + " bytes)");
                                SendTCPResponseWithData(
                                    ip_header.dst_ip, tcp_header.dst_port,  // Swap: from server
                                    ip_header.src_ip, tcp_header.src_port,  // Swap: to client
                                    conn_state.seq_num,
                                    conn_state.ack_num,
                                    TCPParser::TCP_FLAG_ACK | TCPParser::TCP_FLAG_PSH,
                                    tcp_payload
                                );

                                // Update SEQ number for data sent
                                conn_state.seq_num += tcp_payload.size();

                                // Clear buffer after DNS response
                                conn_state.http_buffer.clear();
                                return;
                            } else {
                                Log("  WARNING: No DNS mapping found for " + domain_name);
                            }
                        }
                    }

                    // If we get here, DNS parsing failed - clear buffer and continue
                    conn_state.http_buffer.clear();
                    return;
                }
            }
        }

        // Process all complete HTTP requests in buffer (support HTTP pipelining)
        // Multiple requests can arrive in a single TCP packet or be buffered across packets
        int pipelined_request_count = 0;
        while (true) {
            std::string buffer_str(conn_state.http_buffer.begin(), conn_state.http_buffer.end());
            size_t header_end = buffer_str.find("\r\n\r\n");

            if (header_end == std::string::npos) {
                // No complete request found
                if (pipelined_request_count == 0) {
                    // No requests processed yet - just buffering
                    VerboseLog("TCP stream: buffering " + std::to_string(http_data.size()) +
                        " bytes (total: " + std::to_string(conn_state.http_buffer.size()) + " bytes)");

                    // Log buffer contents for debugging
                    std::string preview = buffer_str.substr(0, std::min(size_t(100), buffer_str.size()));
                    for (char& c : preview) {
                        if (c < 32 && c != '\r' && c != '\n') c = '.';
                    }
                    Log("  Buffer preview: " + preview);
                }
                break;  // Wait for more data
            }

            // We found \r\n\r\n - but verify this is actually an HTTP request
            // Check if buffer starts with a valid HTTP method
            bool valid_http_start = (
                buffer_str.substr(0, 4) == "GET " ||
                buffer_str.substr(0, 5) == "POST " ||
                buffer_str.substr(0, 4) == "PUT " ||
                buffer_str.substr(0, 7) == "DELETE " ||
                buffer_str.substr(0, 5) == "HEAD " ||
                buffer_str.substr(0, 8) == "OPTIONS " ||
                buffer_str.substr(0, 6) == "PATCH "
            );

            if (!valid_http_start) {
                // This is stale data from a previous response, not a new request
                VerboseLog("TCP stream: clearing " + std::to_string(conn_state.http_buffer.size()) +
                    " bytes of stale response data (found \\r\\n\\r\\n but no valid HTTP method)");
                std::string preview = buffer_str.substr(0, std::min(size_t(50), buffer_str.size()));
                for (char& c : preview) {
                    if (c < 32 && c != '\r' && c != '\n') c = '.';
                }
                Log("  Cleared: " + preview);
                conn_state.http_buffer.clear();
                break;
            }

            // We have a complete HTTP request - parse it
            pipelined_request_count++;
            std::string pipeline_info = pipelined_request_count > 1 ?
                " (pipelined request #" + std::to_string(pipelined_request_count) + ")" : "";
            VerboseLog("TCP stream: complete HTTP request received (" +
                std::to_string(header_end + 4) + " bytes)" + pipeline_info);

            // Log full buffer contents for debugging
            std::string full_request = buffer_str.substr(0, header_end + 4);
            VerboseLog("  Full request: " + full_request.substr(0, std::min(size_t(200), full_request.size())));

            // Extract just this request from the buffer
            mtp::ByteArray request_data(conn_state.http_buffer.begin(),
                                        conn_state.http_buffer.begin() + header_end + 4);
            HTTPParser::HTTPRequest request = HTTPParser::ParseRequest(request_data);

            // Store HTTP request size for TCP ACK calculation
            size_t http_request_size = header_end + 4;

            // Remove this request from buffer, keep any remaining data (could be next pipelined request)
            conn_state.http_buffer.erase(conn_state.http_buffer.begin(),
                                         conn_state.http_buffer.begin() + header_end + 4);

            // Store TCP/IP info in request for response
            HTTPRequest interceptor_request;
            interceptor_request.method = request.method;
            interceptor_request.path = request.path;

            // Reconstruct query string from query_params
            if (!request.query_params.empty()) {
                interceptor_request.query_string = "?";
                bool first = true;
                for (const auto& param : request.query_params) {
                    if (!first) interceptor_request.query_string += "&";
                    interceptor_request.query_string += param.first + "=" + param.second;
                    first = false;
                }
            }

            interceptor_request.host = request.GetHeader("Host");
            interceptor_request.protocol = request.protocol;
            interceptor_request.headers = request.headers;
            interceptor_request.src_ip = ip_header.src_ip;
            interceptor_request.src_port = tcp_header.src_port;
            interceptor_request.dst_ip = ip_header.dst_ip;
            interceptor_request.dst_port = tcp_header.dst_port;
            interceptor_request.seq_num = tcp_header.seq_num;
            interceptor_request.ack_num = tcp_header.ack_num;
            interceptor_request.http_request_size = http_request_size;

            // Handle this HTTP request
            // NOTE: SendHTTPResponse() will ACK the request when it sends the first segment
            HandleHTTPRequest(interceptor_request);

            // Continue loop to check for more pipelined requests in buffer
        }

    } catch (const std::exception& e) {
        Log("Error processing packet: " + std::string(e.what()));
    }
}

void ZuneHTTPInterceptor::HandleHTTPRequest(const HTTPRequest& request) {
    Log("HTTP Request: " + request.method + " " + request.path);
    Log("  Host: " + request.host);

    // Queue request for sequential processing by worker thread
    // This prevents frame interleaving when multiple requests arrive simultaneously
    {
        std::lock_guard<std::mutex> lock(request_queue_mutex_);
        request_queue_.push(request);
        request_queue_cv_.notify_one();
    }
}

void ZuneHTTPInterceptor::RequestWorkerThread() {
    while (running_.load()) {
        HTTPRequest request;

        // Wait for a request to be queued
        {
            std::unique_lock<std::mutex> lock(request_queue_mutex_);
            request_queue_cv_.wait(lock, [this] {
                return !request_queue_.empty() || !running_.load();
            });

            // Check if we're shutting down
            if (!running_.load()) {
                break;
            }

            // Get the next request
            request = request_queue_.front();
            request_queue_.pop();
        }

        // Process the request (outside the lock to allow new requests to be queued)
        HTTPParser::HTTPResponse response;

        std::string conn_key = MakeConnectionKey(
            request.src_ip, request.src_port, request.dst_ip, request.dst_port
        );
        {
            std::lock_guard<std::mutex> completion_lock(connection_completion_mutex_);
            auto it = connection_last_completion_time_.find(conn_key);
            if (it != connection_last_completion_time_.end()) {
                auto now = std::chrono::steady_clock::now();
                auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - it->second);

                // If less than 1000ms has passed since last response completed, delay
                const int min_gap_ms = 1000;
                if (elapsed.count() < min_gap_ms) {
                    int delay_needed = min_gap_ms - elapsed.count();
                    VerboseLog("Connection reuse throttle: delaying " + std::to_string(delay_needed) + "ms on " + conn_key +
                        " (last completion " + std::to_string(elapsed.count()) + "ms ago)");
                    std::this_thread::sleep_for(std::chrono::milliseconds(delay_needed));
                }
            }
        }

        // Check if this is a request to an external server (go.microsoft.com, etc.)
        // For these, we proxy to the real server instead of handling locally
        if (request.host == "go.microsoft.com" || request.host.find("microsoft.com") != std::string::npos) {
            Log("Proxying external request to " + request.host);
            response = ProxyExternalHTTPRequest(request.host, request);
        }
        // Route to appropriate handler
        else if (config_.mode == InterceptionMode::Static && static_handler_) {
            HTTPParser::HTTPRequest simple_request;
            simple_request.method = request.method;
            simple_request.path = request.path;
            simple_request.protocol = request.protocol;
            simple_request.headers = request.headers;

            // FIX: Parse query string into query_params map
            if (!request.query_string.empty() && request.query_string[0] == '?') {
                simple_request.query_params = HTTPParser::ParseQueryString(request.query_string.substr(1));
            }

            response = static_handler_->HandleRequest(simple_request);
        }
        else if (config_.mode == InterceptionMode::Proxy && proxy_handler_) {
            HTTPParser::HTTPRequest simple_request;
            simple_request.method = request.method;
            simple_request.path = request.path;
            simple_request.protocol = request.protocol;
            simple_request.headers = request.headers;

            // Parse query string into query_params map
            if (!request.query_string.empty() && request.query_string[0] == '?') {
                simple_request.query_params = HTTPParser::ParseQueryString(request.query_string.substr(1));
            }

            response = proxy_handler_->HandleRequest(simple_request);
        }
        else if (config_.mode == InterceptionMode::Hybrid && hybrid_handler_) {
            HTTPParser::HTTPRequest simple_request;
            simple_request.method = request.method;
            simple_request.path = request.path;
            simple_request.protocol = request.protocol;
            simple_request.headers = request.headers;

            // Parse query string into query_params map
            if (!request.query_string.empty() && request.query_string[0] == '?') {
                simple_request.query_params = HTTPParser::ParseQueryString(request.query_string.substr(1));
            }

            response = hybrid_handler_->HandleRequest(simple_request);
        }
        else {
            response = HTTPParser::BuildErrorResponse(503, "Service not configured");
        }

        constexpr size_t LARGE_IMAGE_THRESHOLD = 20000;  // 20KB
        bool is_large_response = response.body.size() > LARGE_IMAGE_THRESHOLD;

        if (is_large_response) {
            std::lock_guard<std::mutex> large_lock(large_response_throttle_mutex_);

            // Enforce minimum gap since last large transmission started
            if (last_large_response_start_time_.time_since_epoch().count() > 0) {
                auto now = std::chrono::steady_clock::now();
                auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                    now - last_large_response_start_time_
                );

                // Require 1.5 second gap to match official software behavior and ensure device has time to process
                const int min_gap_ms = 1500;
                if (elapsed.count() < min_gap_ms) {
                    int delay_needed = min_gap_ms - elapsed.count();
                    VerboseLog("Global large image throttle: delaying " + std::to_string(delay_needed) + "ms for " +
                        std::to_string(response.body.size()) + " byte response " +
                        "(last large start " + std::to_string(elapsed.count()) + "ms ago)");
                    std::this_thread::sleep_for(std::chrono::milliseconds(delay_needed));
                }
            }

            // Record start time for this large transmission
            last_large_response_start_time_ = std::chrono::steady_clock::now();
            VerboseLog("Global large image throttle: Starting large transmission (" +
                std::to_string(response.body.size()) + " bytes)");
        }

        // Send HTTP response
        SendHTTPResponse(request, response);

    }
}

// libcurl write callback - accumulates response data
static size_t CurlWriteCallback(void* contents, size_t size, size_t nmemb, void* userp) {
    size_t total_size = size * nmemb;
    std::string* response_body = static_cast<std::string*>(userp);
    response_body->append(static_cast<char*>(contents), total_size);
    return total_size;
}

// libcurl header callback - captures response headers
static size_t CurlHeaderCallback(char* buffer, size_t size, size_t nitems, void* userp) {
    size_t total_size = size * nitems;
    std::map<std::string, std::string>* headers = static_cast<std::map<std::string, std::string>*>(userp);

    std::string header_line(buffer, total_size);
    // Remove trailing \r\n
    while (!header_line.empty() && (header_line.back() == '\r' || header_line.back() == '\n')) {
        header_line.pop_back();
    }

    // Parse "Key: Value" format
    size_t colon_pos = header_line.find(':');
    if (colon_pos != std::string::npos) {
        std::string key = header_line.substr(0, colon_pos);
        std::string value = header_line.substr(colon_pos + 1);
        // Trim leading space from value
        while (!value.empty() && value[0] == ' ') {
            value.erase(0, 1);
        }
        (*headers)[key] = value;
    }

    return total_size;
}

HTTPParser::HTTPResponse ZuneHTTPInterceptor::ProxyExternalHTTPRequest(
    const std::string& host, const HTTPRequest& request) {

    HTTPParser::HTTPResponse response;

    // Special handling for DRM/PlayReady requests (go.microsoft.com)
    if (host == "go.microsoft.com") {
        Log("Proxying DRM/PlayReady request to " + host + request.path + request.query_string);
        // Continue with normal proxy logic below
    }

    // Build full URL with query string
    std::string url = "http://" + host + request.path + request.query_string;
    Log("Fetching external URL: " + url);

    CURL* curl = curl_easy_init();
    if (!curl) {
        Log("Failed to initialize libcurl");
        return HTTPParser::BuildErrorResponse(502, "Proxy error");
    }

    std::string response_body;
    std::map<std::string, std::string> response_headers;
    long http_code = 0;

    // Set curl options
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, CurlWriteCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response_body);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, CurlHeaderCallback);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, &response_headers);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "Mozilla/4.0 (compatible; ZuneHD 4.5)");

    // Perform request
    CURLcode res = curl_easy_perform(curl);

    if (res != CURLE_OK) {
        Log("curl_easy_perform() failed: " + std::string(curl_easy_strerror(res)));
        curl_easy_cleanup(curl);
        return HTTPParser::BuildErrorResponse(502, "Proxy connection failed");
    }

    // Get HTTP status code
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    curl_easy_cleanup(curl);

    Log("External request completed: HTTP " + std::to_string(http_code) +
        " (" + std::to_string(response_body.size()) + " bytes)");

    // Log response headers
    VerboseLog("Response headers:");
    for (const auto& header : response_headers) {
        VerboseLog("  " + header.first + ": " + header.second);
    }

    // Log response body (first 200 chars)
    if (!response_body.empty()) {
        std::string body_preview = response_body.substr(0, std::min<size_t>(200, response_body.size()));
        VerboseLog("Response body preview: " + body_preview);
        if (response_body.size() > 200) {
            VerboseLog("  ... (+" + std::to_string(response_body.size() - 200) + " more bytes)");
        }
    }

    // Build response
    response.status_code = static_cast<int>(http_code);
    response.status_message = (http_code == 200) ? "OK" :
                             (http_code == 301 || http_code == 302) ? "Redirect" :
                             (http_code == 404) ? "Not Found" : "Error";
    response.protocol = "HTTP/1.1";
    response.headers = response_headers;
    response.body.assign(response_body.begin(), response_body.end());

    response.headers.erase("Transfer-Encoding");
    response.headers.erase("transfer-encoding");  // Case-insensitive check
    response.headers["Content-Length"] = std::to_string(response.body.size());

    return response;
}

void ZuneHTTPInterceptor::SendNextBatch(const std::string& conn_key) {
    try {
        // Copy data we need from transmission state
        std::vector<mtp::ByteArray> frames_to_send;
        bool is_last_batch = false;

        {
            // Lock transmissions map to get state
            std::lock_guard<std::mutex> trans_lock(transmissions_mutex_);
            auto trans_it = active_transmissions_.find(conn_key);
            if (trans_it == active_transmissions_.end()) {
                VerboseLog("SendNextBatch: No active transmission for connection " + conn_key);
                return;
            }

            HTTPResponseTransmissionState& trans_state = trans_it->second;

            // Check if transmission is complete
            if (trans_state.transmission_complete ||
                trans_state.next_segment_index >= trans_state.queued_segments.size()) {
                VerboseLog("SendNextBatch: Transmission already complete for " + conn_key);
                return;
            }

            // Handle fast retransmit (RFC 2581)
            if (trans_state.retransmit_needed) {
                VerboseLog("SendNextBatch: Fast retransmit - resending segment " +
                    std::to_string(trans_state.retransmit_segment_index) +
                    "/" + std::to_string(trans_state.queued_segments.size()));

                // Retransmit the single missing segment
                if (trans_state.retransmit_segment_index < trans_state.queued_segments.size()) {
                    frames_to_send.push_back(trans_state.queued_segments[trans_state.retransmit_segment_index]);

                    // DON'T add retransmitted bytes to bytes_in_flight - they're already counted
                    // from the original transmission. Double-counting would inflate bytes_in_flight
                    // above the window size and prevent further sending.
                    size_t retransmit_payload = trans_state.segment_payload_sizes[trans_state.retransmit_segment_index];

                    // DON'T reset next_segment_index - segments already sent should remain sent
                    // The device will ACK when it receives the retransmitted segment, advancing state normally
                    // Resetting next_segment_index backward causes loss of knowledge about segments already in flight

                    // Clear retransmit flag
                    trans_state.retransmit_needed = false;

                    VerboseLog("SendNextBatch: Retransmitted segment " +
                        std::to_string(trans_state.retransmit_segment_index) +
                        " (" + std::to_string(retransmit_payload) + " bytes), next_segment=" +
                        std::to_string(trans_state.next_segment_index) + ", bytes_in_flight=" +
                        std::to_string(trans_state.bytes_in_flight));
                }

                // Send the retransmitted frame immediately
                {
                    std::lock_guard<std::mutex> queue_lock(response_queue_mutex_);
                    for (const auto& frame : frames_to_send) {
                        response_queue_.push_back(frame);
                    }
                    VerboseLog("SendNextBatch: Queued retransmit frame (total in queue: " +
                        std::to_string(response_queue_.size()) + ")");
                }

                // Drain the queue
                DrainResponseQueue();
                // DON'T return - continue to send new segments using inflated cwnd (RFC 5681)
                // After retransmitting the lost segment, we should continue sending new segments
                // to fill the inflated congestion window. This preserves the "ACK clock" and
                // prevents transmission stalls.
            }

            // Calculate effective window (RFC 5681: min of congestion window and receiver window)
            // This is how lwIP implements TCP flow control - the sender's window is limited by
            // BOTH the receiver's advertised window AND the congestion control algorithm
            size_t effective_window = std::min(trans_state.cwnd, static_cast<size_t>(trans_state.window_size));

            // Calculate available window space
            size_t available_window = 0;
            if (trans_state.bytes_in_flight < effective_window) {
                available_window = effective_window - trans_state.bytes_in_flight;
            } else {
                VerboseLog("SendNextBatch: Window full (" + std::to_string(trans_state.bytes_in_flight) +
                    " in flight, effective_window=" + std::to_string(effective_window) +
                    " (cwnd=" + std::to_string(trans_state.cwnd) +
                    ", receiver_window=" + std::to_string(trans_state.window_size) + ")), waiting for ACK");
                return;
            }

            // Determine how many segments to send (up to 3 segments per batch, matching official Zune software)
            // Official software never sends more than 3 segments per batch (verified from captures)
            const size_t MAX_BATCH_SIZE = 3;

            // Use 100% of effective window (min of cwnd and receiver window) like lwIP
            size_t max_total_in_flight = effective_window;

            size_t segments_to_send = 0;
            size_t bytes_to_send = 0;

            // Calculate how many segments fit in available window
            for (size_t i = trans_state.next_segment_index;
                 i < trans_state.queued_segments.size() && segments_to_send < MAX_BATCH_SIZE;
                 i++) {
                // Use HTTP payload size (not PPP frame size) for flow control calculation
                size_t payload_size = trans_state.segment_payload_sizes[i];
                size_t total_after_send = trans_state.bytes_in_flight + bytes_to_send + payload_size;

                // Check both: fits in available window AND won't exceed safety threshold
                if ((bytes_to_send + payload_size <= available_window) &&
                    (total_after_send <= max_total_in_flight)) {
                    segments_to_send++;
                    bytes_to_send += payload_size;
                } else {
                    break;  // This segment doesn't fit or would exceed safety threshold
                }
            }

            // If no segments fit at all, we're done (window is too small or we're at the end)
            if (segments_to_send == 0) {
                VerboseLog("SendNextBatch: No segments fit in window");
                return;
            }

            VerboseLog("SendNextBatch: Sending batch of " + std::to_string(segments_to_send) +
                " segments (" + std::to_string(bytes_to_send) + " bytes) for " + conn_key);

            // Copy the PPP frames we need to send (they're pre-built and stored in queued_segments)
            frames_to_send.reserve(segments_to_send);
            for (size_t i = 0; i < segments_to_send; i++) {
                frames_to_send.push_back(trans_state.queued_segments[trans_state.next_segment_index + i]);
            }

            // Update transmission state
            trans_state.next_segment_index += segments_to_send;
            trans_state.bytes_in_flight += bytes_to_send;

            // Check if this is the last batch
            is_last_batch = (trans_state.next_segment_index >= trans_state.queued_segments.size());

            VerboseLog("SendNextBatch: Progress: " + std::to_string(trans_state.next_segment_index) +
                "/" + std::to_string(trans_state.queued_segments.size()) + " segments sent" +
                (is_last_batch ? " (LAST BATCH)" : ""));

        }  // Release transmissions_mutex

        // Queue the frames
        {
            std::lock_guard<std::mutex> queue_lock(response_queue_mutex_);
            for (const auto& frame : frames_to_send) {
                response_queue_.push_back(frame);
            }
            VerboseLog("SendNextBatch: Queued " + std::to_string(frames_to_send.size()) +
                " frames (total in queue: " + std::to_string(response_queue_.size()) + ")");
        }

        // Drain the queue to send frames immediately
        DrainResponseQueue();

    } catch (const std::exception& e) {
        Log("Error in SendNextBatch: " + std::string(e.what()));
    }
}

void ZuneHTTPInterceptor::SendHTTPResponse(const HTTPRequest& request,
                                          const HTTPParser::HTTPResponse& response) {
    VerboseLog("Queueing HTTP response: " + std::to_string(response.status_code) +
        " (" + std::to_string(response.body.size()) + " bytes)");

    try {
        // Build complete HTTP response (headers + body)
        mtp::ByteArray http_data = HTTPParser::BuildResponse(response);

        // TCP SEGMENTATION: Split HTTP response into multiple TCP segments
        // Segment 1: HTTP headers (up to and including \r\n\r\n)
        // Segment 2+: HTTP body in 1460 byte chunks (standard TCP MSS)

        const size_t MAX_SEGMENT_SIZE = 1460;  // Standard TCP MSS (MTU 1500 - IP 20 - TCP 20)

        // Find end of HTTP headers (\r\n\r\n)
        size_t header_end = 0;
        for (size_t i = 0; i + 3 < http_data.size(); i++) {
            if (http_data[i] == '\r' && http_data[i+1] == '\n' &&
                http_data[i+2] == '\r' && http_data[i+3] == '\n') {
                header_end = i + 4;  // Include the \r\n\r\n
                break;
            }
        }

        if (header_end == 0) {
            // No body separator found - treat entire response as single segment
            header_end = http_data.size();
        }

        // Segment the HTTP response
        std::vector<mtp::ByteArray> segments;

        // Segment 1: Headers
        mtp::ByteArray header_segment(http_data.begin(), http_data.begin() + header_end);
        segments.push_back(header_segment);

        // Segment 2+: Body chunks
        size_t body_offset = header_end;
        while (body_offset < http_data.size()) {
            size_t chunk_size = std::min(MAX_SEGMENT_SIZE, http_data.size() - body_offset);
            mtp::ByteArray body_segment(http_data.begin() + body_offset,
                                       http_data.begin() + body_offset + chunk_size);
            segments.push_back(body_segment);
            body_offset += chunk_size;
        }

        VerboseLog("TCP segmentation: " + std::to_string(segments.size()) + " segments " +
            "(header: " + std::to_string(segments[0].size()) + " bytes, " +
            "body: " + std::to_string(http_data.size() - header_end) + " bytes in " +
            std::to_string(segments.size() - 1) + " segments)");

        // Calculate total payload size for atomic sequence number range reservation
        size_t total_payload_size = 0;
        for (const auto& segment : segments) {
            total_payload_size += segment.size();
        }

        std::string conn_key = MakeConnectionKey(
            request.src_ip, request.src_port,  // Client (request source)
            request.dst_ip, request.dst_port   // Server (request destination)
        );

        uint32_t current_seq;
        uint32_t final_ack_num;
        {
            std::lock_guard<std::mutex> conn_lock(connections_mutex_);
            TCPConnectionState& conn_state = connections_[conn_key];
            current_seq = conn_state.seq_num;  // Read current SEQ
            final_ack_num = request.seq_num + request.http_request_size;

            // CRITICAL: Reserve the entire sequence number range IMMEDIATELY
            // Next thread will get a non-overlapping range
            conn_state.seq_num = current_seq + total_payload_size;
            conn_state.ack_num = final_ack_num;
        }

        std::lock_guard<std::mutex> drain_lock(drain_mutex_);


        // CCP Config-Reject frames can be queued asynchronously from the monitoring thread.
        // If we don't drain them first, they'll get mixed with HTTP segments causing corruption.
        DrainResponseQueue();

        // Build all PPP frames and store them in transmission state
        // (for flow-controlled batched transmission)
        HTTPResponseTransmissionState trans_state;
        trans_state.base_seq = current_seq;
        trans_state.last_acked_seq = current_seq;  // No data ACKed yet
        trans_state.last_ack_time = std::chrono::steady_clock::now();

        for (size_t i = 0; i < segments.size(); i++) {
            const auto& segment_data = segments[i];

            // Build TCP header for this segment
            TCPParser::TCPHeader tcp_header = {};
            tcp_header.src_port = request.dst_port;      // Swap
            tcp_header.dst_port = request.src_port;      // Swap
            tcp_header.seq_num = current_seq;
            tcp_header.ack_num = final_ack_num;  // Use calculated ACK number
            tcp_header.data_offset = 5;  // 20 bytes (no options)

            // Set TCP flags: ACK on all, PSH only on last segment (mimics official behavior)
            tcp_header.flags = TCPParser::TCP_FLAG_ACK;
            if (i == segments.size() - 1) {
                tcp_header.flags |= TCPParser::TCP_FLAG_PSH;
            }

            tcp_header.window_size = 65535;
            tcp_header.checksum = 0;  // Will be calculated
            tcp_header.urgent_pointer = 0;

            // Build TCP segment
            mtp::ByteArray tcp_segment = TCPParser::BuildSegment(
                tcp_header, segment_data,
                request.dst_ip,  // Swap
                request.src_ip   // Swap
            );

            // Build IP packet
            IPParser::IPHeader ip_header = {};
            ip_header.version = 4;
            ip_header.header_length = 5;  // 20 bytes
            ip_header.dscp = 0;
            ip_header.ecn = 0;
            ip_header.total_length = 0;  // Will be calculated
            ip_header.identification = rand() % 65536;
            ip_header.flags_offset = 0;  // Don't fragment
            ip_header.ttl = 64;
            ip_header.protocol = 6;  // TCP
            ip_header.checksum = 0;  // Will be calculated
            ip_header.src_ip = request.dst_ip;  // Swap
            ip_header.dst_ip = request.src_ip;  // Swap

            mtp::ByteArray ip_packet = IPParser::BuildPacket(ip_header, tcp_segment);

            // Wrap in PPP frame
            mtp::ByteArray ppp_frame = PPPParser::WrapPayload(ip_packet, 0x0021);

            // Store frame and payload size in transmission state
            trans_state.queued_segments.push_back(ppp_frame);
            trans_state.segment_payload_sizes.push_back(segment_data.size());

            VerboseLog("  Segment " + std::to_string(i+1) + "/" + std::to_string(segments.size()) +
                ": SEQ=" + std::to_string(current_seq) + ", " + std::to_string(segment_data.size()) +
                " bytes payload, " + std::to_string(ppp_frame.size()) + " bytes PPP frame");

            // Increment SEQ by this segment's payload size for next segment
            current_seq += segment_data.size();
        }

        VerboseLog("HTTP response prepared: " + std::to_string(segments.size()) + " segments ready for transmission");

        // Initialize congestion control state (RFC 5681)
        // Start with 3 MSS (conservative initial window, matches lwIP and RFC 5681)
        trans_state.cwnd = 3 * HTTPResponseTransmissionState::mss;
        trans_state.ssthresh = 65535;  // Large initial ssthresh
        VerboseLog("Congestion control initialized: cwnd=" + std::to_string(trans_state.cwnd) +
            ", ssthresh=" + std::to_string(trans_state.ssthresh));

        // FIX: Use transmission key that includes base_seq to support multiple HTTP responses
        // on the same TCP connection (HTTP keep-alive). Key format: "conn_key:base_seq"
        std::string trans_key = conn_key + ":" + std::to_string(trans_state.base_seq);

        // Store transmission state for this HTTP response (unique per response, not per connection)
        {
            std::lock_guard<std::mutex> trans_lock(transmissions_mutex_);
            active_transmissions_[trans_key] = trans_state;
        }

        VerboseLog("Transmission key: " + trans_key + " (allows concurrent HTTP responses on same TCP connection)");

        // Send the first batch immediately (2-4 segments)
        // Subsequent batches will be sent as ACKs arrive
        SendNextBatch(trans_key);

    } catch (const std::exception& e) {
        Log("Error queueing response: " + std::string(e.what()));
    }
}

void ZuneHTTPInterceptor::SendTCPResponse(uint32_t src_ip, uint16_t src_port,
                                          uint32_t dst_ip, uint16_t dst_port,
                                          uint32_t seq_num, uint32_t ack_num,
                                          uint8_t flags) {
    try {
        // Build TCP segment (no data payload for handshake)
        TCPParser::TCPHeader tcp_header = {};
        tcp_header.src_port = src_port;
        tcp_header.dst_port = dst_port;
        tcp_header.seq_num = seq_num;
        tcp_header.ack_num = ack_num;
        tcp_header.data_offset = 5;  // 20 bytes (no options)
        tcp_header.flags = flags;
        tcp_header.window_size = 65535;
        tcp_header.checksum = 0;  // Will be calculated
        tcp_header.urgent_pointer = 0;

        mtp::ByteArray empty_payload;  // No data for handshake packets
        mtp::ByteArray tcp_segment = TCPParser::BuildSegment(
            tcp_header, empty_payload, src_ip, dst_ip);

        // Build IP packet
        IPParser::IPHeader ip_header = {};
        ip_header.version = 4;
        ip_header.header_length = 5;  // 20 bytes
        ip_header.dscp = 0;
        ip_header.ecn = 0;
        ip_header.total_length = 0;  // Will be calculated
        ip_header.identification = rand() % 65536;
        ip_header.flags_offset = 0;  // Don't fragment
        ip_header.ttl = 64;
        ip_header.protocol = 6;  // TCP
        ip_header.checksum = 0;  // Will be calculated
        ip_header.src_ip = src_ip;
        ip_header.dst_ip = dst_ip;

        mtp::ByteArray ip_packet = IPParser::BuildPacket(ip_header, tcp_segment);

        // Wrap in PPP frame
        mtp::ByteArray ppp_frame = PPPParser::WrapPayload(ip_packet, 0x0021);

        // QUEUE response instead of sending directly
        // After network mode is established, all responses must be sent via 0x922d poll mechanism
        {
            std::lock_guard<std::mutex> lock(response_queue_mutex_);
            response_queue_.push_back(ppp_frame);
            VerboseLog("TCP " + TCPParser::FlagsToString(flags) + " queued: " +
                IPParser::IPToString(src_ip) + ":" + std::to_string(src_port) + " -> " +
                IPParser::IPToString(dst_ip) + ":" + std::to_string(dst_port) +
                " (" + std::to_string(response_queue_.size()) + " total in queue)");
        }
        // FIX: Drain immediately after queueing TCP response
        DrainResponseQueue();

    } catch (const std::exception& e) {
        Log("Error queueing TCP response: " + std::string(e.what()));
    }
}

void ZuneHTTPInterceptor::SendTCPResponseWithData(uint32_t src_ip, uint16_t src_port,
                                                   uint32_t dst_ip, uint16_t dst_port,
                                                   uint32_t seq_num, uint32_t ack_num,
                                                   uint8_t flags, const mtp::ByteArray& data) {
    try {
        // Build TCP segment with data payload
        TCPParser::TCPHeader tcp_header = {};
        tcp_header.src_port = src_port;
        tcp_header.dst_port = dst_port;
        tcp_header.seq_num = seq_num;
        tcp_header.ack_num = ack_num;
        tcp_header.data_offset = 5;  // 20 bytes (no options)
        tcp_header.flags = flags;
        tcp_header.window_size = 65535;
        tcp_header.checksum = 0;  // Will be calculated
        tcp_header.urgent_pointer = 0;

        mtp::ByteArray tcp_segment = TCPParser::BuildSegment(
            tcp_header, data, src_ip, dst_ip);

        // Build IP packet
        IPParser::IPHeader ip_header = {};
        ip_header.version = 4;
        ip_header.header_length = 5;  // 20 bytes
        ip_header.dscp = 0;
        ip_header.ecn = 0;
        ip_header.total_length = 0;  // Will be calculated
        ip_header.identification = rand() % 65536;
        ip_header.flags_offset = 0;  // Don't fragment
        ip_header.ttl = 64;
        ip_header.protocol = 6;  // TCP
        ip_header.checksum = 0;  // Will be calculated
        ip_header.src_ip = src_ip;
        ip_header.dst_ip = dst_ip;

        mtp::ByteArray ip_packet = IPParser::BuildPacket(ip_header, tcp_segment);

        // Wrap in PPP frame
        mtp::ByteArray ppp_frame = PPPParser::WrapPayload(ip_packet, 0x0021);

        // QUEUE response instead of sending directly
        {
            std::lock_guard<std::mutex> lock(response_queue_mutex_);
            response_queue_.push_back(ppp_frame);
            VerboseLog("TCP " + TCPParser::FlagsToString(flags) + " with data queued: " +
                IPParser::IPToString(src_ip) + ":" + std::to_string(src_port) + " -> " +
                IPParser::IPToString(dst_ip) + ":" + std::to_string(dst_port) +
                " (data: " + std::to_string(data.size()) + " bytes, " +
                std::to_string(response_queue_.size()) + " total in queue)");
        }
        // FIX: Drain immediately after queueing TCP response with data
        DrainResponseQueue();

    } catch (const std::exception& e) {
        Log("Error queueing TCP response with data: " + std::string(e.what()));
    }
}

void ZuneHTTPInterceptor::DrainResponseQueue() {

    // Check initial queue size outside lock to avoid holding mutex during entire drain
    size_t initial_queue_size = 0;
    {
        std::lock_guard<std::mutex> lock(response_queue_mutex_);
        initial_queue_size = response_queue_.size();
    }

    if (initial_queue_size == 0) {
        return;  // Nothing to drain
    }

    VerboseLog("Draining " + std::to_string(initial_queue_size) + " queued frame(s) via 0x922c");

    // Drain the ENTIRE queue, sending back-to-back without polling
    while (true) {
        mtp::ByteArray combined_payload;

        {
            std::lock_guard<std::mutex> lock(response_queue_mutex_);
            if (response_queue_.empty()) {
                break;  // Queue drained
            }

            // USB transfer size limit (confirmed from capture analysis)
            // Windows uses 7KB transfers (7168 bytes) for optimal throughput
            const size_t USB_MAX_TRANSFER = 7168;

            // Pack frames into 7KB chunks, splitting frames if necessary
            while (!response_queue_.empty()) {
                size_t space_left = USB_MAX_TRANSFER - combined_payload.size();

                if (response_queue_.front().size() <= space_left) {
                    // Whole frame fits in current transfer
                    combined_payload.insert(combined_payload.end(),
                                           response_queue_.front().begin(),
                                           response_queue_.front().end());
                    response_queue_.erase(response_queue_.begin());
                } else if (space_left > 0) {
                    // Frame doesn't fit - split it
                    // Take what fits in this transfer
                    combined_payload.insert(combined_payload.end(),
                                           response_queue_.front().begin(),
                                           response_queue_.front().begin() + space_left);

                    // Keep remainder for next transfer
                    mtp::ByteArray remainder(response_queue_.front().begin() + space_left,
                                            response_queue_.front().end());
                    response_queue_[0] = remainder;

                    VerboseLog("  Split frame: sent " + std::to_string(space_left) + " bytes, " +
                        std::to_string(remainder.size()) + " bytes remaining");
                    break;  // Transfer full, send it
                } else {
                    // No space left in current transfer
                    break;
                }
            }
        }

        if (!combined_payload.empty()) {
            try {
                // Send via Operation922c - NO POLLING between sends!
                session_->Operation922c(combined_payload, 3, 3);

                size_t remaining_frames = 0;
                {
                    std::lock_guard<std::mutex> lock(response_queue_mutex_);
                    remaining_frames = response_queue_.size();
                }

                VerboseLog("  ✓ Sent " + std::to_string(combined_payload.size()) + " bytes via 0x922c" +
                    (remaining_frames == 0 ? "" : " (" + std::to_string(remaining_frames) + " frames remaining)"));
            } catch (const std::exception& e) {
                Log("Error sending via 0x922c: " + std::string(e.what()));
                break;  // Stop draining on error
            }
        }
    }

    VerboseLog("Queue drained - sent all data back-to-back");
}

mtp::ByteArray ZuneHTTPInterceptor::BuildPPPFrame(const HTTPRequest& request,
                                                  const HTTPParser::HTTPResponse& response) {
    // Build HTTP response bytes
    HTTPParser::HTTPResponse http_response;
    http_response.status_code = response.status_code;
    http_response.status_message = response.status_message;
    http_response.headers = response.headers;
    http_response.body = response.body;

    mtp::ByteArray http_data = HTTPParser::BuildResponse(http_response);

    // Build TCP segment (reverse src/dst from request)
    TCPParser::TCPHeader tcp_header = {};
    tcp_header.src_port = request.dst_port;      // Swap
    tcp_header.dst_port = request.src_port;      // Swap
    tcp_header.seq_num = request.ack_num;        // Use their ACK as our SEQ
    tcp_header.ack_num = request.seq_num + request.http_request_size;  // ACK their request data
    tcp_header.data_offset = 5;  // 20 bytes (no options)
    tcp_header.flags = TCPParser::TCP_FLAG_ACK | TCPParser::TCP_FLAG_PSH;
    tcp_header.window_size = 65535;
    tcp_header.checksum = 0;  // Will be calculated
    tcp_header.urgent_pointer = 0;

    mtp::ByteArray tcp_segment = TCPParser::BuildSegment(
        tcp_header, http_data,
        request.dst_ip,  // Swap
        request.src_ip   // Swap
    );

    // Build IP packet (reverse src/dst from request)
    IPParser::IPHeader ip_header = {};
    ip_header.version = 4;
    ip_header.header_length = 5;  // 20 bytes
    ip_header.dscp = 0;
    ip_header.ecn = 0;
    ip_header.total_length = 0;  // Will be calculated
    ip_header.identification = rand() % 65536;
    ip_header.flags_offset = 0;  // Don't fragment
    ip_header.ttl = 64;
    ip_header.protocol = 6;  // TCP
    ip_header.checksum = 0;  // Will be calculated
    ip_header.src_ip = request.dst_ip;  // Swap
    ip_header.dst_ip = request.src_ip;  // Swap

    mtp::ByteArray ip_packet = IPParser::BuildPacket(ip_header, tcp_segment);

    // Wrap in PPP frame
    mtp::ByteArray ppp_frame = PPPParser::WrapPayload(ip_packet, 0x0021);

    return ppp_frame;
}

TCPConnectionState& ZuneHTTPInterceptor::GetConnectionState(const std::string& connection_key) {
    std::lock_guard<std::mutex> lock(connections_mutex_);
    return connections_[connection_key];
}

std::string ZuneHTTPInterceptor::MakeConnectionKey(uint32_t src_ip, uint16_t src_port,
                                                   uint32_t dst_ip, uint16_t dst_port) const {
    std::ostringstream oss;
    oss << IPParser::IPToString(src_ip) << ":" << src_port << "->"
        << IPParser::IPToString(dst_ip) << ":" << dst_port;
    return oss.str();
}

void ZuneHTTPInterceptor::Log(const std::string& message) {
    if (log_callback_) {
        log_callback_("[ZuneHTTPInterceptor] " + message);
    }
}

void ZuneHTTPInterceptor::VerboseLog(const std::string& message) {
    if (verbose_logging_) {
        Log(message);
    }
}

void ZuneHTTPInterceptor::SetVerboseLogging(bool enable) {
    verbose_logging_ = enable;
    if (!enable) {
        Log("Verbose network logging disabled");
    } else {
        Log("Verbose network logging enabled");
    }
}

void ZuneHTTPInterceptor::HandleIPCPPacket(const mtp::ByteArray& ipcp_data) {
    // IPCP negotiation is handled in TriggerNetworkMode() before monitoring thread starts.
    // If we receive IPCP here, it means the device is retransmitting because negotiation
    // wasn't completed properly. Just log it for debugging.
    try {
        IPCPParser::IPCPPacket packet = IPCPParser::ParsePacket(ipcp_data);
        VerboseLog("IPCP packet received in monitoring thread: code=" + std::to_string(packet.code) +
            " id=" + std::to_string(packet.identifier) + " (unexpected - negotiation should be complete)");
    } catch (const std::exception& e) {
        Log("Error parsing IPCP packet: " + std::string(e.what()));
    }
}

void ZuneHTTPInterceptor::InitializeDNSForTesting(const std::string& server_ip) {
    // Initialize DNS hostname mappings for testing
    uint32_t dns_target_ip = IPParser::StringToIP(server_ip);

    dns_hostname_map_["catalog.zune.net"] = dns_target_ip;
    dns_hostname_map_["image.catalog.zune.net"] = dns_target_ip;
    dns_hostname_map_["art.zune.net"] = dns_target_ip;
    dns_hostname_map_["mix.zune.net"] = dns_target_ip;
    dns_hostname_map_["social.zune.net"] = dns_target_ip;
}

void ZuneHTTPInterceptor::EnableNetworkPolling() {
    // Start monitoring thread if not already started
    if (!monitor_thread_) {
        Log("Starting monitoring thread...");
        monitor_thread_ = std::make_unique<std::thread>(&ZuneHTTPInterceptor::MonitorThread, this);
        Log("Monitoring thread started");
    }

    Log("Network polling enabled - monitoring thread will now poll with 0x922d");
    network_polling_enabled_.store(true);
}

void ZuneHTTPInterceptor::HandleDNSQuery(const mtp::ByteArray& ip_packet) {
    try {
        // Parse IP header
        IPParser::IPHeader ip_header = IPParser::ParseHeader(ip_packet);

        // Extract UDP segment
        mtp::ByteArray udp_segment = IPParser::ExtractPayload(ip_packet);

        if (udp_segment.size() < 8) {
            Log("UDP segment too small for DNS");
            return;
        }

        // Parse UDP header
        uint16_t src_port = (udp_segment[0] << 8) | udp_segment[1];
        uint16_t dst_port = (udp_segment[2] << 8) | udp_segment[3];
        uint16_t udp_length = (udp_segment[4] << 8) | udp_segment[5];

        // Extract DNS query (UDP payload)
        mtp::ByteArray dns_query(udp_segment.begin() + 8, udp_segment.end());

        // Parse hostname from query
        std::string hostname = DNSServer::ParseHostname(dns_query);
        Log("DNS query for: " + hostname);

        // Build DNS response
        mtp::ByteArray dns_response = DNSServer::BuildResponse(dns_query, dns_hostname_map_);

        if (dns_response.empty()) {
            Log("No DNS mapping found for " + hostname);
            return;
        }

        // Build UDP response
        mtp::ByteArray udp_response;
        udp_response.push_back((dst_port >> 8) & 0xFF);  // Src port (swap)
        udp_response.push_back(dst_port & 0xFF);
        udp_response.push_back((src_port >> 8) & 0xFF);  // Dst port (swap)
        udp_response.push_back(src_port & 0xFF);

        uint16_t response_length = 8 + dns_response.size();
        udp_response.push_back((response_length >> 8) & 0xFF);
        udp_response.push_back(response_length & 0xFF);

        udp_response.push_back(0x00);  // Checksum (0 = no checksum for UDP)
        udp_response.push_back(0x00);

        udp_response.insert(udp_response.end(), dns_response.begin(), dns_response.end());

        // Build IP response packet
        IPParser::IPHeader response_header = {};
        response_header.version = 4;
        response_header.header_length = 5;
        response_header.dscp = 0;
        response_header.ecn = 0;
        response_header.total_length = 0;  // Will be calculated
        response_header.identification = rand() % 65536;
        response_header.flags_offset = 0;
        response_header.ttl = 64;
        response_header.protocol = 17;  // UDP
        response_header.checksum = 0;  // Will be calculated
        response_header.src_ip = ip_header.dst_ip;  // Swap (our DNS server IP)
        response_header.dst_ip = ip_header.src_ip;  // Swap (device IP)

        mtp::ByteArray ip_response = IPParser::BuildPacket(response_header, udp_response);

        // Wrap in PPP frame and QUEUE (not send directly)
        // After network mode is established, all responses must be sent via 0x922d poll mechanism
        mtp::ByteArray ppp_frame = PPPParser::WrapPayload(ip_response, 0x0021);

        {
            std::lock_guard<std::mutex> lock(response_queue_mutex_);
            response_queue_.push_back(ppp_frame);
            VerboseLog("DNS response queued for " + hostname +
                " (" + std::to_string(response_queue_.size()) + " total in queue)");
        }
        // FIX: Drain immediately after queueing DNS response
        DrainResponseQueue();

    } catch (const std::exception& e) {
        Log("Error handling DNS query: " + std::string(e.what()));
    }
}

// ============================================================================
// Hybrid Mode Callback Setters
// ============================================================================

void ZuneHTTPInterceptor::SetPathResolverCallback(PathResolverCallback callback, void* user_data) {
    std::lock_guard<std::mutex> lock(callback_mutex_);

    path_resolver_callback_ = callback;
    path_resolver_user_data_ = user_data;

    // Forward to HybridModeHandler if it exists
    if (hybrid_handler_) {
        hybrid_handler_->SetPathResolverCallback(callback, user_data);
    }
}

void ZuneHTTPInterceptor::SetCacheStorageCallback(CacheStorageCallback callback, void* user_data) {
    std::lock_guard<std::mutex> lock(callback_mutex_);

    cache_storage_callback_ = callback;
    cache_storage_user_data_ = user_data;

    // Forward to HybridModeHandler if it exists
    if (hybrid_handler_) {
        hybrid_handler_->SetCacheStorageCallback(callback, user_data);
    }
}
