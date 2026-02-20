#include "ZuneHTTPInterceptor.h"
#include "../ppp/PPPParser.h"
#include "../ppp/PPPFrameBuilder.h"
#include "HTTPParser.h"
#include "HttpClient.h"
#include "StaticModeHandler.h"
#include "ProxyModeHandler.h"
#include "HybridModeHandler.h"
#include "../handlers/CCPHandler.h"
#include "../handlers/DNSHandler.h"
#include "../tcp/TCPConnectionManager.h"
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

    // Initialize protocol handlers (Phase 5.2 extraction)
    ccp_handler_ = std::make_unique<CCPHandler>();
    ccp_handler_->SetLogCallback(log_callback_);

    tcp_manager_ = std::make_unique<TCPConnectionManager>();
    tcp_manager_->SetLogCallback(log_callback_);

    // Initialize DNS hostname mappings
    // Resolve to the configured server IP (we intercept all traffic anyway)
    uint32_t dns_target_ip;
    if (!config_.server_ip.empty()) {
        dns_target_ip = IPParser::StringToIP(config_.server_ip);
        Log("DNS target: " + IPParser::IPToString(dns_target_ip) +
            " (from: " + config_.server_ip + ")");
    } else {
        dns_target_ip = IPParser::StringToIP("192.168.0.30");
        Log("DNS target: 192.168.0.30 (default)");
    }

    InitializeDNSHostnameMap(dns_target_ip);
    Log("DNS server initialized with " + std::to_string(dns_hostname_map_.size()) + " hostname mappings");

    // Initialize DNS handler with hostname map
    dns_handler_ = std::make_unique<DNSHandler>(dns_hostname_map_);
    dns_handler_->SetLogCallback(log_callback_);

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

    // Note: C# drives polling via PollOnce() after EnableNetworkPolling() sets the flag.
    // TriggerNetworkMode() must complete LCP negotiation before polling begins.
    running_.store(true);

    // Start request worker thread pool for concurrent HTTP request processing
    Log("Starting " + std::to_string(NUM_WORKER_THREADS) + " request worker threads");
    request_worker_threads_.reserve(NUM_WORKER_THREADS);
    for (size_t i = 0; i < NUM_WORKER_THREADS; ++i) {
        request_worker_threads_.push_back(
            std::make_unique<std::thread>(&ZuneHTTPInterceptor::RequestWorkerThread, this)
        );
    }

    Log("HTTP interceptor started successfully");
}

void ZuneHTTPInterceptor::Stop() {
    if (!running_.load()) {
        return;
    }

    Log("Stopping HTTP interceptor...");
    running_.store(false);
    network_polling_enabled_.store(false);

    // Stop timeout checker thread (Phase 2: RTO integration)
    timeout_checker_running_.store(false);
    if (timeout_checker_thread_ && timeout_checker_thread_->joinable()) {
        timeout_checker_thread_->join();
    }

    // Notify all request worker threads to wake up and check running_ flag
    request_queue_cv_.notify_all();

    // Wait for all request worker threads to finish
    for (auto& worker_thread : request_worker_threads_) {
        if (worker_thread && worker_thread->joinable()) {
            worker_thread->join();
        }
    }
    request_worker_threads_.clear();

    // No monitoring thread to join — C# drives polling via PollOnce()

    // Clean up handlers
    static_handler_.reset();
    proxy_handler_.reset();
    ppp_parser_.reset();
    http_parser_.reset();

    // Connection states managed by TCPConnectionManager (Phase 5.3)

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

            // Match bulk endpoints by type and direction (address varies by device model)
            // Zune HD (Pavo): OUT=0x01, IN=0x01 (0x81 on wire)
            // Zune Classic (Keel/Scorpius/Draco): OUT=0x02, IN=0x01 (0x81 on wire)
            if (type == mtp::usb::EndpointType::Bulk) {
                if (direction == mtp::usb::EndpointDirection::Out) {
                    endpoint_out_ = ep;
                    Log("  → Found HTTP OUT endpoint: 0x" + std::to_string(address));
                }
                else if (direction == mtp::usb::EndpointDirection::In) {
                    endpoint_in_ = ep;
                    Log("  → Found HTTP IN endpoint: 0x" + std::to_string(address));
                }
            }

            // Look for interrupt endpoint (event notifications)
            if (type == mtp::usb::EndpointType::Interrupt &&
                direction == mtp::usb::EndpointDirection::In) {
                endpoint_interrupt_ = ep;
                Log("  → Found interrupt endpoint: 0x" + std::to_string(address));
            }
        }

        if (!endpoint_in_ || !endpoint_out_) {
            Log("Error: HTTP endpoints not found (need one Bulk OUT and one Bulk IN)");
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

int ZuneHTTPInterceptor::PollOnce(int timeout_ms) {
    if (!running_.load()) {
        return -1;
    }

    if (!network_polling_enabled_.load()) {
        return 0;
    }

    if (!session_) {
        return -2;
    }

    try {
        // Wait for interrupt from device (blocks up to timeout_ms)
        session_->PollEvent(timeout_ms);

        // Retrieve network data
        mtp::ByteArray response_data = session_->Operation922d(3, 3);

        if (response_data.empty()) {
            return 0;
        }

        // Skip "CLIENT" response (6 bytes: 43 4c 49 45 4e 54)
        if (response_data.size() == 6 &&
            response_data[0] == 0x43 && response_data[1] == 0x4c &&
            response_data[2] == 0x49 && response_data[3] == 0x45 &&
            response_data[4] == 0x4e && response_data[5] == 0x54) {
            return 0;
        }

        // Process network packet
        VerboseLog("PollOnce: received " + std::to_string(response_data.size()) + " bytes");

        ProcessPacket(response_data);

        // Process pending sends - ACKs may have opened up window for more data
        ProcessPendingSends();

        return 1;

    } catch (const std::exception& e) {
        if (running_.load()) {
            Log("PollOnce error: " + std::string(e.what()));
        }
        return -1;
    }
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

        // Extract PPP frames from USB packet (handles incomplete frames spanning packets)
        std::vector<mtp::ByteArray> frames = PPPParser::ExtractFramesWithBuffer(
            usb_data, incomplete_ppp_frame_buffer_);

        if (!incomplete_ppp_frame_buffer_.empty()) {
            VerboseLog("Buffering incomplete PPP frame (" +
                std::to_string(incomplete_ppp_frame_buffer_.size()) + " bytes)");
        }

        VerboseLog("Found " + std::to_string(frames.size()) + " PPP frame(s) in USB packet");

        // Process each PPP frame - this accumulates pending_sends_ but doesn't trigger sends
        // The caller (monitoring thread or DrainResponseQueue) decides when to trigger sends
        for (size_t frame_idx = 0; frame_idx < frames.size(); frame_idx++) {
            VerboseLog("Processing PPP frame " + std::to_string(frame_idx + 1) + "/" +
                std::to_string(frames.size()) + " (" + std::to_string(frames[frame_idx].size()) + " bytes)");
            ProcessPPPFrame(frames[frame_idx]);
        }

        // NOTE: pending_sends_ is processed by the caller, not here
        // This prevents recursive drain loops when ProcessPacket is called from DrainResponseQueue

    } catch (const std::exception& e) {
        Log("Error processing packet: " + std::string(e.what()));
    }
}

void ZuneHTTPInterceptor::ProcessPendingSends() {
    // Process all pending sends accumulated by ACK processing
    // This is called from the monitoring thread (NOT from within DrainResponseQueue)
    // to prevent recursive drain loops

    std::set<std::string> trans_keys_to_send;
    {
        std::lock_guard<std::mutex> lock(pending_sends_mutex_);
        trans_keys_to_send = pending_sends_;
        pending_sends_.clear();
    }

    for (const std::string& trans_key : trans_keys_to_send) {
        // Parse trans_key format: "conn_key:base_seq"
        // conn_key format: "192.168.55.101:50120->192.168.0.30:80"
        // trans_key format: "192.168.55.101:50120->192.168.0.30:80:3052870019"
        size_t arrow_pos = trans_key.find("->");
        if (arrow_pos == std::string::npos) {
            continue;
        }

        // Find the second colon after the arrow (port separator is first, base_seq separator is second)
        size_t first_colon = trans_key.find(':', arrow_pos);
        if (first_colon == std::string::npos) {
            continue;
        }
        size_t second_colon = trans_key.find(':', first_colon + 1);
        if (second_colon == std::string::npos) {
            continue;
        }

        std::string conn_key = trans_key.substr(0, second_colon);
        uint32_t base_seq = std::stoul(trans_key.substr(second_colon + 1));

        SendNextBatch(conn_key, base_seq);
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
            // CCP (Compression Control Protocol): Handled by CCPHandler (Phase 5.2)
            auto ccp_response = ccp_handler_->HandlePacket(payload);
            if (ccp_response.has_value()) {
                {
                    std::lock_guard<std::mutex> lock(response_queue_mutex_);
                    response_queue_.push_back(ccp_response.value());
                    VerboseLog("CCP response queued (" + std::to_string(response_queue_.size()) + " total in queue)");
                }  // Release mutex before draining

                // Drain immediately after queueing CCP response
                DrainResponseQueue();
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
        std::string conn_key = TCPConnectionManager::MakeConnectionKey(
            ip_header.src_ip, tcp_header.src_port,
            ip_header.dst_ip, tcp_header.dst_port);

        // TCP handshake and connection management delegated to TCPConnectionManager (Phase 5.2)
        // Extract payload for manager
        mtp::ByteArray tcp_payload = TCPParser::ExtractPayload(tcp_segment);

        auto tcp_response = tcp_manager_->HandlePacket(
            ip_header.src_ip, tcp_header.src_port,
            ip_header.dst_ip, tcp_header.dst_port,
            tcp_header.seq_num, tcp_header.ack_num,
            tcp_header.flags, tcp_header.window_size,
            tcp_payload
        );

        // Send TCP response if manager generated one
        if (tcp_response.has_value()) {
            SendTCPPacket(tcp_response.value());
        }

        // Phase 5.3: Use TCPConnectionManager directly (no backwards compatibility adapter)
        TCPConnectionInfo* tcp_conn = tcp_manager_->GetConnection(conn_key);
        if (!tcp_conn) {
            return;  // No connection state found
        }

        // Early return if connection isn't established yet (handshake in progress)
        if (tcp_conn->state != TCPState::ESTABLISHED) {
            return;
        }

        // NOTE: TCPConnectionManager already processed this packet and added it to the reassembler.
        // We just need to check if there's enough data in the buffer to parse HTTP.
        // DO NOT call AddSegment again - that would be duplicate processing!

        if (tcp_payload.empty()) {
            // No payload - this is an ACK for flow control
            // Delegate ALL ACK processing to TCPConnectionManager (SINGLE SOURCE OF TRUTH)

            uint32_t base_seq = tcp_manager_->ProcessACKForTransmission(
                conn_key, tcp_header.ack_num, tcp_header.window_size);

            if (base_seq != 0) {
                // TCPConnectionManager says we should send more segments
                std::string trans_key = conn_key + ":" + std::to_string(base_seq);

                VerboseLog("ACK processed by TCPConnectionManager: trans_key=" + trans_key);

                // Check if fast retransmit is needed
                uint32_t retransmit_base_seq;
                size_t retransmit_segment_index;
                if (tcp_manager_->CheckRetransmitNeeded(conn_key, retransmit_base_seq, retransmit_segment_index)) {
                    // Handle fast retransmit
                    mtp::ByteArray retransmit_frame = tcp_manager_->GetRetransmitSegment(
                        conn_key, retransmit_base_seq, retransmit_segment_index);

                    if (!retransmit_frame.empty()) {
                        Log("Fast retransmit: segment " + std::to_string(retransmit_segment_index));
                        {
                            std::lock_guard<std::mutex> lock(response_queue_mutex_);
                            response_queue_.push_back(retransmit_frame);
                        }
                        // CRITICAL: Actually send the retransmit frame!
                        DrainResponseQueue();
                    }
                    tcp_manager_->ClearRetransmitFlag(conn_key);
                }

                // Add to pending sends for next batch
                std::lock_guard<std::mutex> lock(pending_sends_mutex_);
                pending_sends_.insert(trans_key);
            }

            return;
        }

        // At this point, TCPConnectionManager has already added the payload to the reassembler.
        // The reassembler was initialized during HandleSYN with the correct initial sequence (seq_num + 1).
        // We just use GetBuffer() to access the accumulated HTTP data.

        // Sanity check: reassembler should always exist for ESTABLISHED connections
        if (!tcp_conn->reassembler) {
            Log("ERROR: No reassembler for ESTABLISHED connection! This should never happen.");
            return;
        }

        // Check for Zune custom DNS protocol BEFORE HTTP (delegated to DNSHandler)
        if (dns_handler_ && dns_handler_->IsZuneTCPDNSQuery(tcp_conn->reassembler->GetBuffer())) {
            size_t bytes_consumed = 0;
            auto dns_response = dns_handler_->HandleTCPQuery(tcp_conn->reassembler->GetBuffer(), bytes_consumed);

            if (dns_response) {
                // Update connection state for response
                tcp_conn->ack_num = tcp_header.seq_num + bytes_consumed;

                // Send TCP response with DNS data
                Log("Sending DNS response (" + std::to_string(dns_response->size()) + " bytes)");
                SendTCPResponseWithData(
                    ip_header.dst_ip, tcp_header.dst_port,
                    ip_header.src_ip, tcp_header.src_port,
                    tcp_conn->seq_num,
                    tcp_conn->ack_num,
                    TCPParser::TCP_FLAG_ACK | TCPParser::TCP_FLAG_PSH,
                    *dns_response
                );

                // Update SEQ number for data sent
                tcp_conn->seq_num += dns_response->size();
            }

            // Clear buffer after DNS handling (success or failure)
            tcp_conn->reassembler->ClearContiguousBuffer();
            return;
        }

        // Process all complete HTTP requests in buffer (support HTTP pipelining)
        int pipelined_request_count = 0;
        while (true) {
            HTTPParser::HTTPRequest parsed_request;
            size_t bytes_consumed = 0;

            auto result = HTTPParser::TryExtractRequest(
                tcp_conn->reassembler->GetBuffer(), parsed_request, bytes_consumed);

            if (result == HTTPParser::ExtractResult::INCOMPLETE) {
                if (pipelined_request_count == 0 && !tcp_payload.empty()) {
                    VerboseLog("TCP stream: buffering " + std::to_string(tcp_payload.size()) +
                        " bytes (total: " + std::to_string(tcp_conn->reassembler->GetBuffer().size()) + " bytes)");
                }
                break;
            }

            if (result == HTTPParser::ExtractResult::INVALID_DATA) {
                VerboseLog("TCP stream: clearing stale data (" +
                    std::to_string(tcp_conn->reassembler->GetBuffer().size()) + " bytes)");
                tcp_conn->reassembler->ClearContiguousBuffer();
                break;
            }

            // SUCCESS - process the request
            pipelined_request_count++;
            VerboseLog("HTTP request received (" + std::to_string(bytes_consumed) + " bytes)" +
                (pipelined_request_count > 1 ? " [pipelined #" + std::to_string(pipelined_request_count) + "]" : ""));

            // Remove processed request from buffer
            tcp_conn->reassembler->EraseProcessedBytes(bytes_consumed);

            // Build interceptor request with TCP/IP context
            HTTPRequest interceptor_request;
            interceptor_request.method = parsed_request.method;
            interceptor_request.path = parsed_request.path;
            interceptor_request.protocol = parsed_request.protocol;
            interceptor_request.headers = parsed_request.headers;
            interceptor_request.host = parsed_request.GetHeader("Host");
            interceptor_request.src_ip = ip_header.src_ip;
            interceptor_request.src_port = tcp_header.src_port;
            interceptor_request.dst_ip = ip_header.dst_ip;
            interceptor_request.dst_port = tcp_header.dst_port;
            interceptor_request.seq_num = tcp_header.seq_num;
            interceptor_request.ack_num = tcp_header.ack_num;
            interceptor_request.http_request_size = bytes_consumed;

            // Reconstruct query string from query_params
            if (!parsed_request.query_params.empty()) {
                interceptor_request.query_string = "?";
                bool first = true;
                for (const auto& param : parsed_request.query_params) {
                    if (!first) interceptor_request.query_string += "&";
                    interceptor_request.query_string += param.first + "=" + param.second;
                    first = false;
                }
            }

            HandleHTTPRequest(interceptor_request);
        }

    } catch (const std::exception& e) {
        Log("Error processing packet: " + std::string(e.what()));
    }
}

void ZuneHTTPInterceptor::HandleHTTPRequest(const HTTPRequest& request) {
    Log("HTTP Request: " + request.method + " " + request.path);
    Log("  Host: " + request.host);

    // Queue request for concurrent processing by worker thread pool
    // TCP flow control prevents segment interleaving per connection
    {
        std::lock_guard<std::mutex> lock(request_queue_mutex_);
        request_queue_.push(request);
        request_queue_cv_.notify_one();  // Wake one worker to process this request
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

        // Check if this is a request to an external server (go.microsoft.com, etc.)
        if (request.host == "go.microsoft.com" || request.host.find("microsoft.com") != std::string::npos) {
            std::string url = "http://" + request.host + request.path + request.query_string;
            Log("Proxying external request: " + url);
            response = HttpClient::FetchExternal(url);
        }
        else {
            // Build simple_request ONCE for all handlers (was duplicated 3 times)
            HTTPParser::HTTPRequest simple_request;
            simple_request.method = request.method;
            simple_request.path = request.path;
            simple_request.protocol = request.protocol;
            simple_request.headers = request.headers;
            if (!request.query_string.empty() && request.query_string[0] == '?') {
                simple_request.query_params = HTTPParser::ParseQueryString(request.query_string.substr(1));
            }

            // Route to appropriate handler
            if (config_.mode == InterceptionMode::Static && static_handler_) {
                response = static_handler_->HandleRequest(simple_request);
            } else if (config_.mode == InterceptionMode::Proxy && proxy_handler_) {
                response = proxy_handler_->HandleRequest(simple_request);
            } else if (config_.mode == InterceptionMode::Hybrid && hybrid_handler_) {
                response = hybrid_handler_->HandleRequest(simple_request);
            } else {
                response = HTTPParser::BuildErrorResponse(503, "Service not configured");
            }
        }

        // Send HTTP response (flow control is handled by TCP layer)
        SendHTTPResponse(request, response);

    }
}

void ZuneHTTPInterceptor::SendNextBatch(const std::string& conn_key, uint32_t base_seq) {
    try {
        // Check for fast retransmit first
        uint32_t retransmit_base_seq;
        size_t retransmit_segment_index;
        if (tcp_manager_->CheckRetransmitNeeded(conn_key, retransmit_base_seq, retransmit_segment_index)) {
            VerboseLog("SendNextBatch: Fast retransmit needed for segment " +
                std::to_string(retransmit_segment_index));

            mtp::ByteArray retransmit_frame = tcp_manager_->GetRetransmitSegment(
                conn_key, retransmit_base_seq, retransmit_segment_index);

            if (!retransmit_frame.empty()) {
                // Queue and send retransmit immediately
                {
                    std::lock_guard<std::mutex> queue_lock(response_queue_mutex_);
                    response_queue_.push_back(retransmit_frame);
                    VerboseLog("SendNextBatch: Queued retransmit frame (total in queue: " +
                        std::to_string(response_queue_.size()) + ")");
                }
                DrainResponseQueue();

                // Clear retransmit flag
                tcp_manager_->ClearRetransmitFlag(conn_key);
            }
            // After retransmit, continue to send new segments (RFC 5681)
        }

        // Get next batch of segments from TCPConnectionManager
        std::vector<mtp::ByteArray> frames_to_send;
        bool is_last_batch = false;

        size_t num_segments = tcp_manager_->GetNextBatch(conn_key, base_seq, frames_to_send, is_last_batch);

        if (num_segments == 0) {
            VerboseLog("SendNextBatch: No segments to send (window full or complete)");
            return;
        }

        VerboseLog("SendNextBatch: Sending batch of " + std::to_string(num_segments) +
            " segments for " + conn_key + (is_last_batch ? " (LAST BATCH)" : ""));

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

        // TCP segmentation: split into header segment + body chunks
        std::vector<mtp::ByteArray> segments = TCPConnectionManager::SegmentHTTPPayload(http_data);

        VerboseLog("TCP segmentation: " + std::to_string(segments.size()) + " segments " +
            "(header: " + std::to_string(segments[0].size()) + " bytes, " +
            "body: " + std::to_string(http_data.size() - segments[0].size()) + " bytes in " +
            std::to_string(segments.size() - 1) + " segments)");

        // Calculate total payload size for atomic sequence number range reservation
        size_t total_payload_size = 0;
        for (const auto& segment : segments) {
            total_payload_size += segment.size();
        }

        std::string conn_key = TCPConnectionManager::MakeConnectionKey(
            request.src_ip, request.src_port,  // Client (request source)
            request.dst_ip, request.dst_port   // Server (request destination)
        );

        // Phase 5.3: Get TCP connection info for seq/ack numbers
        TCPConnectionInfo* tcp_conn = tcp_manager_->GetConnection(conn_key);
        if (!tcp_conn) {
            Log("Error: TCP connection not found for HTTP response");
            return;
        }

        // CRITICAL: Atomically reserve the entire sequence number range
        // This allows multiple threads to send responses on the same connection concurrently
        // Each thread gets a non-overlapping sequence number range
        uint32_t current_seq;
        uint32_t final_ack_num;
        {
            std::lock_guard<std::mutex> seq_lock(tcp_conn->seq_num_mutex);
            current_seq = tcp_conn->seq_num;  // Read current SEQ
            final_ack_num = request.seq_num + request.http_request_size;
            tcp_conn->seq_num = current_seq + total_payload_size;  // Reserve range
            tcp_conn->ack_num = final_ack_num;
        }

        std::lock_guard<std::mutex> drain_lock(drain_mutex_);


        // CCP Config-Reject frames can be queued asynchronously from the monitoring thread.
        // If we don't drain them first, they'll get mixed with HTTP segments causing corruption.
        DrainResponseQueue();

        // Build all PPP frames for this HTTP response using PPPFrameBuilder
        std::vector<mtp::ByteArray> ppp_frames;
        std::vector<size_t> payload_sizes;
        uint32_t base_seq = current_seq;

        for (size_t i = 0; i < segments.size(); i++) {
            const auto& segment_data = segments[i];

            // TCP flags: ACK on all, PSH only on last segment
            uint8_t flags = TCPParser::TCP_FLAG_ACK;
            if (i == segments.size() - 1) {
                flags |= TCPParser::TCP_FLAG_PSH;
            }

            // Use PPPFrameBuilder (SINGLE SOURCE OF TRUTH for frame building)
            mtp::ByteArray ppp_frame = PPPFrameBuilder::BuildTCPFrame(
                request.dst_ip, request.dst_port,  // Swap: server as source
                request.src_ip, request.src_port,  // Swap: client as dest
                current_seq, final_ack_num,
                flags, segment_data
            );

            ppp_frames.push_back(ppp_frame);
            payload_sizes.push_back(segment_data.size());

            VerboseLog("  Segment " + std::to_string(i+1) + "/" + std::to_string(segments.size()) +
                ": SEQ=" + std::to_string(current_seq) + ", " + std::to_string(segment_data.size()) +
                " bytes payload, " + std::to_string(ppp_frame.size()) + " bytes PPP frame");

            // Diagnostic: dump frame header and FCS for segments around index 20-24
            if (i >= 20 && i <= 25 && ppp_frame.size() >= 10) {
                std::ostringstream dump;
                dump << "  [DEBUG] Segment " << (i+1) << " frame: start=[";
                for (size_t j = 0; j < std::min(size_t(10), ppp_frame.size()); j++) {
                    dump << std::hex << std::setw(2) << std::setfill('0') << (int)ppp_frame[j] << " ";
                }
                dump << "], end=[";
                size_t end_start = (ppp_frame.size() > 10) ? ppp_frame.size() - 10 : 0;
                for (size_t j = end_start; j < ppp_frame.size(); j++) {
                    dump << std::hex << std::setw(2) << std::setfill('0') << (int)ppp_frame[j] << " ";
                }
                dump << "]";
                Log(dump.str());
            }

            current_seq += segment_data.size();
        }

        VerboseLog("HTTP response prepared: " + std::to_string(segments.size()) + " segments ready for transmission");

        // Register transmission with TCPConnectionManager (SINGLE SOURCE OF TRUTH)
        // The manager handles all flow control, congestion control, and retransmission
        tcp_manager_->StartHTTPTransmission(conn_key, base_seq,
                                            std::move(ppp_frames), std::move(payload_sizes));

        VerboseLog("Transmission registered with TCPConnectionManager: base_seq=" + std::to_string(base_seq));

        // Send the first batch immediately
        SendNextBatch(conn_key, base_seq);

    } catch (const std::exception& e) {
        Log("Error queueing response: " + std::string(e.what()));
    }
}

void ZuneHTTPInterceptor::SendTCPResponse(uint32_t src_ip, uint16_t src_port,
                                          uint32_t dst_ip, uint16_t dst_port,
                                          uint32_t seq_num, uint32_t ack_num,
                                          uint8_t flags) {
    try {
        mtp::ByteArray ppp_frame = PPPFrameBuilder::BuildTCPFrame(
            src_ip, src_port, dst_ip, dst_port, seq_num, ack_num, flags);

        {
            std::lock_guard<std::mutex> lock(response_queue_mutex_);
            response_queue_.push_back(ppp_frame);
            VerboseLog("TCP " + TCPParser::FlagsToString(flags) + " queued: " +
                IPParser::IPToString(src_ip) + ":" + std::to_string(src_port) + " -> " +
                IPParser::IPToString(dst_ip) + ":" + std::to_string(dst_port) +
                " (" + std::to_string(response_queue_.size()) + " total in queue)");
        }
        DrainResponseQueue();
    } catch (const std::exception& e) {
        Log("Error queueing TCP response: " + std::string(e.what()));
    }
}

void ZuneHTTPInterceptor::SendTCPPacket(const TCPPacket& packet) {
    try {
        mtp::ByteArray ppp_frame = PPPFrameBuilder::BuildTCPFrame(
            packet.src_ip, packet.src_port, packet.dst_ip, packet.dst_port,
            packet.seq_num, packet.ack_num, packet.flags, packet.payload);

        {
            std::lock_guard<std::mutex> lock(response_queue_mutex_);
            response_queue_.push_back(ppp_frame);
            VerboseLog("TCP " + TCPParser::FlagsToString(packet.flags) + " queued: " +
                IPParser::IPToString(packet.src_ip) + ":" + std::to_string(packet.src_port) + " -> " +
                IPParser::IPToString(packet.dst_ip) + ":" + std::to_string(packet.dst_port) +
                " (" + std::to_string(response_queue_.size()) + " total in queue)");
        }
        DrainResponseQueue();
    } catch (const std::exception& e) {
        Log("Error sending TCP packet: " + std::string(e.what()));
    }
}

void ZuneHTTPInterceptor::SendTCPResponseWithData(uint32_t src_ip, uint16_t src_port,
                                                   uint32_t dst_ip, uint16_t dst_port,
                                                   uint32_t seq_num, uint32_t ack_num,
                                                   uint8_t flags, const mtp::ByteArray& data) {
    try {
        mtp::ByteArray ppp_frame = PPPFrameBuilder::BuildTCPFrame(
            src_ip, src_port, dst_ip, dst_port, seq_num, ack_num, flags, data);

        {
            std::lock_guard<std::mutex> lock(response_queue_mutex_);
            response_queue_.push_back(ppp_frame);
            VerboseLog("TCP " + TCPParser::FlagsToString(flags) + " with data queued: " +
                IPParser::IPToString(src_ip) + ":" + std::to_string(src_port) + " -> " +
                IPParser::IPToString(dst_ip) + ":" + std::to_string(dst_port) +
                " (data: " + std::to_string(data.size()) + " bytes, " +
                std::to_string(response_queue_.size()) + " total in queue)");
        }
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

    // REACTIVE SEND LOOP - matches official Zune software behavior:
    // Analysis of official captures shows:
    // - Max ~7.6KB per 922c operation (~4 TCP segments of ~1460 bytes each)
    // - Poll with 922d AFTER each send
    // - 86.5% of sends happen AFTER receiving an ACK
    // - Max 2-3 back-to-back sends without ACK

    constexpr size_t USB_MAX_TRANSFER = 7680;  // Match official: ~7600-7627 bytes per operation
    int consecutive_sends = 0;
    constexpr int MAX_CONSECUTIVE_SENDS = 2;  // Official does max 2-3 back-to-back

    while (true) {
        mtp::ByteArray combined_payload;

        {
            std::lock_guard<std::mutex> lock(response_queue_mutex_);
            if (response_queue_.empty()) {
                break;  // Queue drained
            }

            while (!response_queue_.empty()) {
                const size_t space_left = USB_MAX_TRANSFER - combined_payload.size();
                const auto& front_frame = response_queue_.front();

                if (front_frame.size() <= space_left) {
                    // Whole frame fits - append and pop
                    combined_payload.insert(combined_payload.end(),
                                           front_frame.begin(),
                                           front_frame.end());
                    response_queue_.pop_front();  // O(1) with deque
                } else if (space_left > 0) {
                    // Frame too large - split it
                    combined_payload.insert(combined_payload.end(),
                                           front_frame.begin(),
                                           front_frame.begin() + space_left);

                    // Replace front with remainder
                    response_queue_.front() = mtp::ByteArray(
                        front_frame.begin() + space_left,
                        front_frame.end());

                    VerboseLog("  Split frame: sent " + std::to_string(space_left) +
                              " bytes, " + std::to_string(response_queue_.front().size()) +
                              " bytes remaining");
                    break;
                } else {
                    break;  // Transfer full
                }
            }
        }

        if (!combined_payload.empty()) {
            try {
                // Send via Operation922c
                session_->Operation922c(combined_payload, 3, 3);
                consecutive_sends++;

                size_t remaining_frames = 0;
                {
                    std::lock_guard<std::mutex> lock(response_queue_mutex_);
                    remaining_frames = response_queue_.size();
                }

                VerboseLog("  ✓ Sent " + std::to_string(combined_payload.size()) + " bytes via 0x922c" +
                    " (send #" + std::to_string(consecutive_sends) + ")" +
                    (remaining_frames == 0 ? "" : " (" + std::to_string(remaining_frames) + " frames remaining)"));

                // REACTIVE: Poll for incoming data after each send
                // This matches official software behavior where 922d polls happen after 922c sends
                if (remaining_frames > 0 && session_) {
                    try {
                        mtp::ByteArray poll_response = session_->Operation922d(3, 3);

                        if (!poll_response.empty() && poll_response.size() > 6) {
                            // Process any incoming data (ACKs will update TCP window)
                            VerboseLog("  Poll returned " + std::to_string(poll_response.size()) + " bytes");
                            ProcessPacket(poll_response);
                            consecutive_sends = 0;  // Reset counter after receiving data
                        }
                    } catch (const std::exception& e) {
                        // Poll failed - continue sending
                        VerboseLog("  Poll failed: " + std::string(e.what()));
                    }

                    // After MAX_CONSECUTIVE_SENDS, do an extra poll to allow device to catch up
                    // This matches official behavior of rarely sending more than 2-3 back-to-back
                    if (consecutive_sends >= MAX_CONSECUTIVE_SENDS && remaining_frames > 0) {
                        VerboseLog("  Reached " + std::to_string(MAX_CONSECUTIVE_SENDS) +
                                  " consecutive sends, extra poll for device to catch up");
                        try {
                            mtp::ByteArray extra_response = session_->Operation922d(3, 3);
                            if (!extra_response.empty() && extra_response.size() > 6) {
                                ProcessPacket(extra_response);
                            }
                        } catch (...) {}
                        consecutive_sends = 0;
                    }
                }

            } catch (const std::exception& e) {
                Log("Error sending via 0x922c: " + std::string(e.what()));
                break;  // Stop draining on error
            }
        }
    }

    VerboseLog("Queue drained - reactive send loop complete");
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

void ZuneHTTPInterceptor::InitializeDNSHostnameMap(uint32_t dns_target_ip) {
    dns_hostname_map_["catalog.zune.net"] = dns_target_ip;
    dns_hostname_map_["image.catalog.zune.net"] = dns_target_ip;
    dns_hostname_map_["art.zune.net"] = dns_target_ip;
    dns_hostname_map_["mix.zune.net"] = dns_target_ip;
    dns_hostname_map_["social.zune.net"] = dns_target_ip;
    dns_hostname_map_["go.microsoft.com"] = dns_target_ip;
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
    uint32_t dns_target_ip = IPParser::StringToIP(server_ip);
    InitializeDNSHostnameMap(dns_target_ip);
}

void ZuneHTTPInterceptor::EnableNetworkPolling() {
    // Start timeout checker thread (Phase 2: RTO integration)
    if (!timeout_checker_thread_) {
        Log("Starting timeout checker thread...");
        timeout_checker_running_.store(true);
        timeout_checker_thread_ = std::make_unique<std::thread>(&ZuneHTTPInterceptor::TimeoutCheckerThread, this);
        Log("Timeout checker thread started");
    }

    // C# drives polling via PollOnce() — no native monitoring thread
    Log("Network polling enabled — C# will drive via PollOnce()");
    network_polling_enabled_.store(true);
}

// ============================================================================
// RTO Timeout Checking (Phase 2: RTO Integration)
// ============================================================================

void ZuneHTTPInterceptor::TimeoutCheckerThread() {
    Log("Timeout checker thread started - checking every 100ms");

    while (timeout_checker_running_.load()) {
        try {
            // Check all active connections for timeouts
            CheckAllConnectionTimeouts();

            // Sleep for 100ms (check 10 times per second)
            // RTO minimum is 1000ms, so 100ms granularity is sufficient
            std::this_thread::sleep_for(std::chrono::milliseconds(100));

        } catch (const std::exception& e) {
            if (timeout_checker_running_.load()) {
                Log("Error in timeout checker thread: " + std::string(e.what()));
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
        }
    }

    Log("Timeout checker thread stopped");
}

void ZuneHTTPInterceptor::CheckAllConnectionTimeouts() {
    // Delegate timeout checking to TCPConnectionManager
    auto timed_out_map = tcp_manager_->CheckAllTimeouts();

    // Handle each timed-out segment
    for (const auto& [conn_key, segments] : timed_out_map) {
        for (const auto& segment : segments) {
            RetransmitSegment(conn_key, segment);
        }
    }
}

void ZuneHTTPInterceptor::RetransmitSegment(const std::string& conn_key, const SentSegment& segment) {
    Log("RTO timeout - retransmitting segment: conn=" + conn_key +
        " SEQ=" + std::to_string(segment.seq_start) +
        " size=" + std::to_string(segment.data.size()) + " bytes");

    // Delegate to TCPConnectionManager to find and mark the segment
    uint32_t base_seq;
    size_t segment_index;
    if (tcp_manager_->HandleRTORetransmit(conn_key, segment, base_seq, segment_index)) {
        // Queue the send for this connection
        std::string trans_key = conn_key + ":" + std::to_string(base_seq);
        {
            std::lock_guard<std::mutex> pending_lock(pending_sends_mutex_);
            pending_sends_.insert(trans_key);
        }
    } else {
        Log("WARNING: Could not find transmission state for timed-out segment");
    }
}

void ZuneHTTPInterceptor::HandleDNSQuery(const mtp::ByteArray& ip_packet) {
    // DNS handling delegated to DNSHandler (Phase 5.2)
    auto dns_response = dns_handler_->HandleQuery(ip_packet);

    if (dns_response.has_value()) {
        {
            std::lock_guard<std::mutex> lock(response_queue_mutex_);
            response_queue_.push_back(dns_response.value());
            VerboseLog("DNS response queued (" + std::to_string(response_queue_.size()) + " total in queue)");
        }  // Release mutex before draining

        // Drain immediately after queueing DNS response
        DrainResponseQueue();
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
