#include "ZuneDevice.h"
#include "protocols/http/ZuneHTTPInterceptor.h"
#include "protocols/ppp/PPPParser.h"
#include <iostream>
#include <sstream>
#include <iomanip>
#include <thread>
#include <chrono>

using namespace mtp;

// ============================================================================
// HTTP Interceptor Methods
// ============================================================================

bool ZuneDevice::InitializeHTTPSubsystem() {
    if (!device_ || !mtp_session_) {
        Log("Error: Device not connected, cannot initialize HTTP subsystem");
        return false;
    }

    try {
        Log("Initializing HTTP subsystem on device...");

        // NOTE: 0x1002 already called in ConnectUSB() before MTPZ auth
        // Skip duplicate call here

        // HTTP trigger command (called twice in capture)
        Log("  → Sending 0x9231() - HTTP init trigger (1st)");
        mtp_session_->Operation9231();
        Log("  ✓ 0x9231 complete");

        Log("  → Sending 0x9231() - HTTP init trigger (2nd)");
        mtp_session_->Operation9231();
        Log("  ✓ 0x9231 complete");

        // Sync operations
        Log("  → Sending 0x9217(1)");
        mtp_session_->Operation9217(1);
        Log("  ✓ 0x9217 complete");

        Log("  → Sending 0x9218(0, 0, 5000)");
        mtp_session_->Operation9218(0, 0, 5000);
        Log("  ✓ 0x9218 complete");

        // Second sync
        Log("  → Sending 0x9217(1) again");
        mtp_session_->Operation9217(1);
        Log("  ✓ 0x9217 complete");

        // NOTE: 0x9214 is part of MTPZ auth, not HTTP init - skip it here

        Log("  → Sending 0x9219(0, 0, 5000)");
        mtp_session_->Operation9219(0, 0, 5000);
        Log("  ✓ 0x9219 complete");

        Log("  → Sending 0x922f");
        mtp::ByteArray empty;
        mtp_session_->Operation922f(empty);
        Log("  ✓ 0x922f complete");

        // Network subsystem setup
        Log("  → Sending 0x922b(3, 1, 0)");
        mtp_session_->Operation922b(3, 1, 0);
        Log("  ✓ 0x922b complete");

        // Enable wireless/network sync
        Log("  → Sending 0x9230(1)");
        mtp_session_->Operation9230(1);
        Log("  ✓ 0x9230 complete");

        // NOTE: Windows Zune software does NOT send 0x922c during HTTP init
        // The device will start PPP negotiation autonomously after 0x9230

        Log("✓ HTTP subsystem initialization complete");
        Log("✓ Device ready for 0x922d HTTP polling");
        return true;

    } catch (const std::exception& e) {
        Log(std::string("HTTP initialization failed: ") + e.what());
        return false;
    }
}

void ZuneDevice::StartHTTPInterceptor(const InterceptorConfig& config) {
    if (!device_) {
        throw std::runtime_error("Device not connected");
    }

    if (http_interceptor_ && http_interceptor_->IsRunning()) {
        Log("HTTP interceptor is already running");
        return;
    }

    Log("Starting HTTP interceptor...");
    http_interceptor_ = std::make_unique<ZuneHTTPInterceptor>(mtp_session_);
    http_interceptor_->SetLogCallback(log_callback_);
    http_interceptor_->Start(config);

    // NOTE: Don't enable network polling yet - caller must explicitly call EnableNetworkPolling()
    // AFTER TriggerNetworkMode() completes to avoid race condition during IPCP handshake
}

void ZuneDevice::StopHTTPInterceptor() {
    if (http_interceptor_) {
        Log("Stopping HTTP interceptor...");
        http_interceptor_->Stop();
        http_interceptor_.reset();
    }
}

void ZuneDevice::EnableNetworkPolling() {
    if (!http_interceptor_) {
        throw std::runtime_error("HTTP interceptor not running - call StartHTTPInterceptor() first");
    }

    Log("Enabling network polling (0x922d continuous at 15ms intervals)...");
    http_interceptor_->EnableNetworkPolling();
    Log("  ✓ Network polling enabled");
}

bool ZuneDevice::IsHTTPInterceptorRunning() const {
    return http_interceptor_ && http_interceptor_->IsRunning();
}

InterceptorConfig ZuneDevice::GetHTTPInterceptorConfig() const {
    if (http_interceptor_) {
        return http_interceptor_->GetConfig();
    }
    return InterceptorConfig{};
}

void ZuneDevice::TriggerNetworkMode() {
    if (!mtp_session_) {
        throw std::runtime_error("No active MTP session");
    }

    Log("Triggering network mode...");

    // Helper function to format byte array as hex string
    auto format_hex = [](const mtp::ByteArray& data, size_t max_bytes = 0) -> std::string {
        std::stringstream ss;
        size_t limit = (max_bytes > 0 && max_bytes < data.size()) ? max_bytes : data.size();
        for (size_t i = 0; i < limit; i++) {
            if (i > 0) ss << " ";
            ss << std::hex << std::setw(2) << std::setfill('0') << (int)data[i];
        }
        if (max_bytes > 0 && data.size() > max_bytes) {
            ss << " ... (" << std::dec << data.size() << " bytes total)";
        }
        return ss.str();
    };

    // Step 1: Send "CLIENTSERVER" to trigger network mode (frame 2219/2221/2223)
    Log("Sending CLIENTSERVER to trigger network mode...");
    const char* trigger_str = "CLIENTSERVER";
    mtp::ByteArray trigger_payload(trigger_str, trigger_str + 12);
    mtp_session_->Operation922c(trigger_payload, 3, 3);
    Log("  ✓ Network mode trigger sent");

    // Step 2: Poll with 0x922d until device sends LCP Config-Request
    // NOTE: Device first echoes back part of "CLIENTSERVER", then sends actual LCP
    Log("Polling for device LCP Config-Request...");
    mtp::ByteArray device_lcp;
    int poll_count = 0;
    const int max_polls = 100;  // Safety limit
    bool found_valid_lcp = false;

    while (!found_valid_lcp && poll_count < max_polls) {
        mtp::ByteArray response = mtp_session_->Operation922d(3, 3);
        poll_count++;

        if (!response.empty()) {
            // Valid PPP frames start with 0x7E
            if (response[0] == 0x7E) {
                Log("  ✓ Received PPP frame: " + std::to_string(response.size()) + " bytes (poll " +
                    std::to_string(poll_count) + ")");
                device_lcp = response;
                found_valid_lcp = true;
            } else {
                Log("  → Ignoring non-PPP response: " + std::to_string(response.size()) + " bytes (echo)");
            }
        }

        if (!found_valid_lcp) {
            std::this_thread::sleep_for(std::chrono::milliseconds(16));  // ~16ms between polls in capture
        }
    }

    if (!found_valid_lcp) {
        Log("  ✗ Device did not send LCP Config-Request after " + std::to_string(max_polls) + " polls");
        throw std::runtime_error("Device did not enter network mode");
    }

    Log("  ✓ Device LCP Config-Request received: " + std::to_string(device_lcp.size()) + " bytes");
    Log("    Data: " + format_hex(device_lcp, 50));

    // Step 3: Send our LCP response (frame 2315/2317/2319)
    Log("Sending LCP response...");
    const uint8_t lcp_response_data[] = {
        0x7e, 0xff, 0x7d, 0x23, 0xc0, 0x21, 0x7d, 0x22, 0x7d, 0x20, 0x7d, 0x20, 0x7d, 0x2e, 0x7d, 0x22,
        0x7d, 0x26, 0x7d, 0x20, 0x7d, 0x20, 0x7d, 0x20, 0x7d, 0x20, 0x7d, 0x27, 0x7d, 0x22, 0x7d, 0x28,
        0x7d, 0x22, 0xe3, 0xb2, 0x7e, 0x7e, 0xff, 0x7d, 0x23, 0xc0, 0x21, 0x7d, 0x21, 0x7d, 0x21, 0x7d,
        0x20, 0x7d, 0x2e, 0x7d, 0x22, 0x7d, 0x26, 0x7d, 0x20, 0x7d, 0x20, 0x7d, 0x20, 0x7d, 0x20, 0x7d,
        0x27, 0x7d, 0x22, 0x7d, 0x28, 0x7d, 0x22, 0x70, 0x34, 0x7e
    };

    mtp::ByteArray lcp_response_payload(lcp_response_data, lcp_response_data + sizeof(lcp_response_data));
    mtp_session_->Operation922c(lcp_response_payload, 3, 3);
    Log("  ✓ LCP response sent");

    // Step 4: Poll to get device LCP reply (77 bytes expected - frame 2328/2330)
    // NOTE: The device may send multiple PPP frames in one message, including IPCP Config-Request
    Log("Polling for device LCP reply...");
    mtp::ByteArray device_lcp_reply = mtp_session_->Operation922d(3, 3);
    Log("  ✓ Device LCP reply received: " + std::to_string(device_lcp_reply.size()) + " bytes");
    Log("    Data: " + format_hex(device_lcp_reply, 50));

    // Step 5: Check if device LCP reply already contains IPCP Config-Request
    // (This matches Windows capture where frame 2366 has multiple PPP frames)
    Log("Checking for device IPCP Config-Request in LCP reply...");
    mtp::ByteArray device_ipcp_request;
    poll_count = 0;
    bool found_device_ipcp_request = false;

    // First, check if the LCP reply already contains IPCP Config-Request
    if (!device_lcp_reply.empty()) {
        size_t i = 0;
        while (i < device_lcp_reply.size()) {
            if (device_lcp_reply[i] == 0x7E && i + 4 < device_lcp_reply.size()) {
                // Check for IPCP protocol (0x8021) and Config-Request (0x01)
                if (device_lcp_reply[i+1] == 0x80 && device_lcp_reply[i+2] == 0x21 && device_lcp_reply[i+3] == 0x01) {
                    Log("  ✓ Found IPCP Config-Request in LCP reply message!");
                    device_ipcp_request = device_lcp_reply;
                    found_device_ipcp_request = true;
                    break;
                }
            }
            i++;
        }
    }

    // If not found in LCP reply, poll for it separately
    if (!found_device_ipcp_request) {
        Log("Polling for device IPCP Config-Request...");
    }

    while (!found_device_ipcp_request && poll_count < max_polls) {
        mtp::ByteArray response = mtp_session_->Operation922d(3, 3);
        poll_count++;

        if (!response.empty() && response[0] == 0x7E) {
            // Check if this contains IPCP Config-Request (protocol 0x8021, code 0x01)
            // The response may contain multiple PPP frames, we need to find the IPCP Config-Request
            bool has_ipcp_request = false;
            size_t i = 0;
            while (i < response.size()) {
                if (response[i] == 0x7E && i + 4 < response.size()) {
                    // Check for IPCP protocol (0x8021) and Config-Request (0x01)
                    if (response[i+1] == 0x80 && response[i+2] == 0x21 && response[i+3] == 0x01) {
                        has_ipcp_request = true;
                        break;
                    }
                }
                i++;
            }

            if (has_ipcp_request) {
                Log("  ✓ Received device IPCP Config-Request: " + std::to_string(response.size()) + " bytes (poll " +
                    std::to_string(poll_count) + ")");
                Log("    Data: " + format_hex(response, 50));
                device_ipcp_request = response;
                found_device_ipcp_request = true;
            }
        }

        if (!found_device_ipcp_request) {
            std::this_thread::sleep_for(std::chrono::milliseconds(16));
        }
    }

    if (!found_device_ipcp_request) {
        Log("  ✗ Device did not send IPCP Config-Request after " + std::to_string(max_polls) + " polls");
        throw std::runtime_error("Device IPCP negotiation failed");
    }

    // Step 6: Parse device's IPCP Config-Request to extract the identifier
    // We need to extract all PPP frames from the device's response
    std::vector<mtp::ByteArray> device_ppp_frames;
    size_t i = 0;
    while (i < device_ipcp_request.size()) {
        if (device_ipcp_request[i] == 0x7E) {
            size_t j = i + 1;
            while (j < device_ipcp_request.size() && device_ipcp_request[j] != 0x7E) {
                j++;
            }
            if (j < device_ipcp_request.size()) {
                mtp::ByteArray frame(device_ipcp_request.begin() + i, device_ipcp_request.begin() + j + 1);
                device_ppp_frames.push_back(frame);
                i = j;
            }
        }
        i++;
    }

    // Find the IPCP Config-Request frame and extract its identifier
    uint8_t device_ipcp_request_id = 0;
    for (const auto& frame : device_ppp_frames) {
        if (frame.size() >= 5 && frame[1] == 0x80 && frame[2] == 0x21 && frame[3] == 0x01) {
            // This is IPCP Config-Request
            device_ipcp_request_id = frame[4];
            Log("  → Device IPCP Config-Request ID: " + std::to_string(device_ipcp_request_id));
            break;
        }
    }

    if (device_ipcp_request_id == 0) {
        throw std::runtime_error("Failed to parse device IPCP Config-Request identifier");
    }

    // Step 7: Parse device's Config-Request to check requested IP
    // If device requests 0.0.0.0, we need to send Config-Nak (not Config-Ack!)
    Log("Parsing device IPCP Config-Request...");

    // Network configuration
    const uint32_t host_ip = 0xC0A83764;      // 192.168.55.100
    const uint32_t device_ip = 0xC0A83765;    // 192.168.55.101
    const uint32_t dns_ip = host_ip;          // DNS server = host IP (device queries host for DNS)

    IPCPParser::IPCPPacket device_request;
    uint32_t device_requested_ip = 0;

    try {
        // Extract IPCP payload from the device's Config-Request frame
        for (const auto& frame : device_ppp_frames) {
            if (frame.size() >= 5 && frame[1] == 0x80 && frame[2] == 0x21 && frame[3] == 0x01) {
                mtp::ByteArray ipcp_payload(frame.begin() + 3, frame.end() - 3);  // Skip 0x7E, protocol, FCS, 0x7E
                device_request = IPCPParser::ParsePacket(ipcp_payload);

                // Extract requested IP from options
                std::vector<IPCPParser::IPCPOption> options = IPCPParser::ParseOptions(device_request.options);
                for (const auto& opt : options) {
                    if (opt.type == 3 && opt.data.size() == 4) {  // IP-Address option
                        device_requested_ip = (opt.data[0] << 24) | (opt.data[1] << 16) |
                                            (opt.data[2] << 8) | opt.data[3];
                        Log("  → Device requested IP: " + IPParser::IPToString(device_requested_ip));
                        break;
                    }
                }
                break;
            }
        }
    } catch (const std::exception& e) {
        throw std::runtime_error("Failed to parse device IPCP Config-Request: " + std::string(e.what()));
    }

    // Build our Config-Request (WITH compression option, ID=1)
    // Per Windows capture: must include IP-Compression option so device can reject it
    // NOTE: Build IPCP packet only (no protocol bytes) - WrapPayload adds protocol
    mtp::ByteArray our_config_request;
    our_config_request.push_back(0x01); // Config-Request
    our_config_request.push_back(0x01); // Identifier = 1 (first request)
    our_config_request.push_back(0x00); // Length high byte
    our_config_request.push_back(0x10); // Length low byte (16 bytes total)
    // Option 2: IP-Compression (exactly as Windows sends it)
    our_config_request.push_back(0x02); // Option type 2 (IP-Compression)
    our_config_request.push_back(0x06); // Option length (6 bytes)
    our_config_request.push_back(0x00); // Compression data
    our_config_request.push_back(0x2d);
    our_config_request.push_back(0x0f);
    our_config_request.push_back(0x01);
    // Option 3: IP-Address
    our_config_request.push_back(0x03); // Option type 3 (IP-Address)
    our_config_request.push_back(0x06); // Option length (6 bytes)
    our_config_request.push_back((host_ip >> 24) & 0xFF);
    our_config_request.push_back((host_ip >> 16) & 0xFF);
    our_config_request.push_back((host_ip >> 8) & 0xFF);
    our_config_request.push_back(host_ip & 0xFF);
    mtp::ByteArray our_config_request_ppp = PPPParser::WrapPayload(our_config_request, 0x8021);

    // Build CCP (Compression Control Protocol) Config-Request
    // Windows always sends this regardless of whether device IP is valid
    // NOTE: Build CCP packet only (no protocol bytes) - WrapPayload adds protocol
    mtp::ByteArray ccp_request;
    ccp_request.push_back(0x01); // Config-Request
    ccp_request.push_back(0x01); // Identifier = 1
    ccp_request.push_back(0x00); // Length high byte
    ccp_request.push_back(0x0a); // Length low byte (10 bytes total)
    ccp_request.push_back(0x12); // Option type
    ccp_request.push_back(0x06); // Option length
    ccp_request.push_back(0x00); // Option data
    ccp_request.push_back(0x00);
    ccp_request.push_back(0x00);
    ccp_request.push_back(0x01);
    mtp::ByteArray ccp_request_ppp = PPPParser::WrapPayload(ccp_request, 0x80fd);

    // Check if device requested invalid IP (0.0.0.0)
    if (device_requested_ip == 0) {
        // Send Config-Nak to tell device to request correct IP
        Log("Building initial IPCP response (Config-Request + CCP + Config-Nak)...");
        mtp::ByteArray config_nak_ipcp = IPCPParser::BuildConfigNak(device_ipcp_request_id, device_ip, dns_ip);
        mtp::ByteArray config_nak_ppp = PPPParser::WrapPayload(config_nak_ipcp, 0x8021);

        // Combine in Windows order: Config-Request + CCP + Config-Nak
        mtp::ByteArray initial_ipcp_payload;
        initial_ipcp_payload.insert(initial_ipcp_payload.end(),
                                   our_config_request_ppp.begin(), our_config_request_ppp.end());
        initial_ipcp_payload.insert(initial_ipcp_payload.end(),
                                   ccp_request_ppp.begin(), ccp_request_ppp.end());
        initial_ipcp_payload.insert(initial_ipcp_payload.end(),
                                   config_nak_ppp.begin(), config_nak_ppp.end());

        Log("Sending IPCP Config-Request + CCP + Config-Nak...");
        Log("  → Our Config-Request for " + IPParser::IPToString(host_ip));
        Log("  → CCP Config-Request");
        Log("  → Config-Nak suggesting device use " + IPParser::IPToString(device_ip));
        mtp_session_->Operation922c(initial_ipcp_payload, 3, 3);
        Log("  ✓ Initial IPCP sent");

        // Step 8: Wait for device's Config-Reject + NEW Config-Request
        // Per Windows capture: Device sends Config-Reject (ID=1, rejecting compression)
        // followed by NEW Config-Request (ID=3, with corrected IP) in SAME 922d response
        Log("Polling for device's Config-Reject + corrected IPCP Config-Request...");
        mtp::ByteArray device_new_request;
        poll_count = 0;
        bool found_config_reject = false;
        bool found_new_request = false;
        uint8_t new_request_id = 0;

        while (!found_new_request && poll_count < max_polls) {
            mtp::ByteArray response = mtp_session_->Operation922d(3, 3);
            poll_count++;

            if (!response.empty() && response[0] == 0x7E) {
                // Look for BOTH Config-Reject and new Config-Request in same response
                size_t i = 0;
                while (i < response.size()) {
                    if (response[i] == 0x7E && i + 5 < response.size()) {
                        // Check for IPCP protocol (0x8021)
                        if (response[i+1] == 0x80 && response[i+2] == 0x21) {
                            uint8_t code = response[i+3];
                            uint8_t id = response[i+4];

                            if (code == 0x04) {
                                // Config-Reject - device rejecting our compression option
                                found_config_reject = true;
                                Log("  ✓ Received Config-Reject (ID=" + std::to_string(id) +
                                    ") - device rejecting compression option");
                            } else if (code == 0x01 && id != device_ipcp_request_id) {
                                // This is a NEW Config-Request with different ID!
                                new_request_id = id;
                                device_new_request = response;
                                found_new_request = true;
                                Log("  ✓ Received device's new Config-Request (ID=" + std::to_string(new_request_id) +
                                    "): " + std::to_string(response.size()) + " bytes (poll " + std::to_string(poll_count) + ")");
                                Log("    Data: " + format_hex(response, 50));
                            }
                        }
                    }
                    i++;
                }
            }

            if (!found_new_request) {
                std::this_thread::sleep_for(std::chrono::milliseconds(16));
            }
        }

        if (!found_new_request) {
            Log("  ✗ Device did not send new IPCP Config-Request after Config-Nak");
            throw std::runtime_error("Device IPCP negotiation failed after Config-Nak");
        }

        // Parse the new Config-Request
        std::vector<mtp::ByteArray> new_ppp_frames;
        i = 0;
        while (i < device_new_request.size()) {
            if (device_new_request[i] == 0x7E) {
                size_t j = i + 1;
                while (j < device_new_request.size() && device_new_request[j] != 0x7E) {
                    j++;
                }
                if (j < device_new_request.size()) {
                    mtp::ByteArray frame(device_new_request.begin() + i, device_new_request.begin() + j + 1);
                    new_ppp_frames.push_back(frame);
                    i = j;
                }
            }
            i++;
        }

        // Parse new Config-Request
        for (const auto& frame : new_ppp_frames) {
            if (frame.size() >= 5 && frame[1] == 0x80 && frame[2] == 0x21 && frame[3] == 0x01) {
                mtp::ByteArray ipcp_payload(frame.begin() + 3, frame.end() - 3);
                device_request = IPCPParser::ParsePacket(ipcp_payload);
                break;
            }
        }

        // Step 9: Send SECOND Config-Request (ID=2, WITHOUT compression) + Config-Ack
        // Per Windows capture Frame 2387: After receiving Config-Reject, send new Config-Request without compression
        Log("Building second IPCP Config-Request (without compression) + Config-Ack...");

        // Build Config-Request WITHOUT compression (ID=2)
        // NOTE: Build IPCP packet only (no protocol bytes) - WrapPayload adds protocol
        mtp::ByteArray our_second_request;
        our_second_request.push_back(0x01); // Config-Request
        our_second_request.push_back(0x02); // Identifier = 2 (second request, after compression rejected)
        our_second_request.push_back(0x00); // Length high byte
        our_second_request.push_back(0x0A); // Length low byte (10 bytes total)
        // Option 3: IP-Address (NO compression option this time)
        our_second_request.push_back(0x03); // Option type 3 (IP-Address)
        our_second_request.push_back(0x06); // Option length (6 bytes)
        our_second_request.push_back((host_ip >> 24) & 0xFF);
        our_second_request.push_back((host_ip >> 16) & 0xFF);
        our_second_request.push_back((host_ip >> 8) & 0xFF);
        our_second_request.push_back(host_ip & 0xFF);
        mtp::ByteArray our_second_request_ppp = PPPParser::WrapPayload(our_second_request, 0x8021);

        // Build Config-Ack for device's new Config-Request
        mtp::ByteArray config_ack_ipcp = IPCPParser::BuildConfigAck(device_request);
        mtp::ByteArray config_ack_ppp = PPPParser::WrapPayload(config_ack_ipcp, 0x8021);

        // Combine: Second Config-Request + Config-Ack
        mtp::ByteArray second_ipcp_payload;
        second_ipcp_payload.insert(second_ipcp_payload.end(),
                                   our_second_request_ppp.begin(), our_second_request_ppp.end());
        second_ipcp_payload.insert(second_ipcp_payload.end(),
                                   config_ack_ppp.begin(), config_ack_ppp.end());

        Log("Sending second IPCP Config-Request (no compression) + Config-Ack...");
        Log("  → Our Config-Request (ID=2) for " + IPParser::IPToString(host_ip) + " (no compression)");
        Log("  → Config-Ack for device's Config-Request (ID=" + std::to_string(device_request.identifier) + ")");
        mtp_session_->Operation922c(second_ipcp_payload, 3, 3);
        Log("  ✓ Second IPCP Config-Request + Config-Ack sent");
    } else {
        // Device sent valid IP - send Config-Request + CCP + Config-Ack
        Log("Building initial IPCP response (Config-Request + CCP + Config-Ack)...");
        Log("  → Device requested valid IP: " + IPParser::IPToString(device_requested_ip));

        mtp::ByteArray config_ack_ipcp = IPCPParser::BuildConfigAck(device_request);
        mtp::ByteArray config_ack_ppp = PPPParser::WrapPayload(config_ack_ipcp, 0x8021);

        // Combine in Windows order: Config-Request + CCP + Config-Ack
        mtp::ByteArray initial_ipcp_payload;
        initial_ipcp_payload.insert(initial_ipcp_payload.end(),
                                   our_config_request_ppp.begin(), our_config_request_ppp.end());
        initial_ipcp_payload.insert(initial_ipcp_payload.end(),
                                   ccp_request_ppp.begin(), ccp_request_ppp.end());
        initial_ipcp_payload.insert(initial_ipcp_payload.end(),
                                   config_ack_ppp.begin(), config_ack_ppp.end());

        Log("Sending IPCP Config-Request + CCP + Config-Ack...");
        Log("  → Our Config-Request for " + IPParser::IPToString(host_ip));
        Log("  → CCP Config-Request");
        Log("  → Config-Ack for device's Config-Request");
        mtp_session_->Operation922c(initial_ipcp_payload, 3, 3);
        Log("  ✓ Initial IPCP sent");
    }

    // Step 10: Poll for device's Config-Ack to our Config-Request
    Log("Polling for device IPCP Config-Ack...");
    mtp::ByteArray device_config_ack;
    poll_count = 0;
    bool found_config_ack = false;

    while (!found_config_ack && poll_count < max_polls) {
        mtp::ByteArray response = mtp_session_->Operation922d(3, 3);
        poll_count++;

        if (!response.empty() && response[0] == 0x7E) {
            // Check if this contains IPCP Config-Ack (protocol 0x8021, code 0x02, identifier 0x02)
            // Note: After Config-Reject path, we're looking for ack to our SECOND request (ID=2)
            bool has_config_ack = false;
            size_t i = 0;
            while (i < response.size()) {
                if (response[i] == 0x7E && i + 5 < response.size()) {
                    // Check for IPCP protocol (0x8021), Config-Ack (0x02), and our identifier (0x02 or 0x01)
                    if (response[i+1] == 0x80 && response[i+2] == 0x21 && response[i+3] == 0x02) {
                        // Accept Config-Ack with either ID=1 or ID=2 depending on path taken
                        if (response[i+4] == 0x01 || response[i+4] == 0x02) {
                            has_config_ack = true;
                            break;
                        }
                    }
                }
                i++;
            }

            if (has_config_ack) {
                Log("  ✓ Received device IPCP Config-Ack: " + std::to_string(response.size()) + " bytes (poll " +
                    std::to_string(poll_count) + ")");
                Log("    Data: " + format_hex(response, 50));
                device_config_ack = response;
                found_config_ack = true;
            }
        }

        if (!found_config_ack) {
            std::this_thread::sleep_for(std::chrono::milliseconds(16));
        }
    }

    if (!found_config_ack) {
        Log("  ✗ Device did not send IPCP Config-Ack after " + std::to_string(max_polls) + " polls");
        throw std::runtime_error("Device did not complete IPCP negotiation");
    }

    Log("✓ Network mode fully established - Bidirectional LCP and IPCP handshakes complete!");
}

USBHandlesWithEndpoints ZuneDevice::ExtractUSBHandles() {
    if (!mtp_session_) {
        throw std::runtime_error("MTP session not initialized - call ConnectUSB() first");
    }

    Log("Extracting USB handles from MTP session...");

    // Get the BulkPipe from the session
    auto pipe = mtp_session_->GetBulkPipe();
    if (!pipe) {
        throw std::runtime_error("Cannot access USB pipe from session");
    }

    // Extract USB device and interface from the pipe
    usb::DevicePtr usb_device = pipe->GetDevice();
    usb::InterfacePtr usb_interface = pipe->GetInterface();

    if (!usb_device || !usb_interface) {
        throw std::runtime_error("Failed to extract USB handles from session");
    }

    Log("Discovering HTTP endpoints (0x01 IN/OUT) before disconnect...");

    // CRITICAL: Discover endpoints NOW while interface is still claimed by MTP
    // After disconnect, the interface becomes invalid for enumeration
    int endpoint_count = usb_interface->GetEndpointsCount();
    usb::EndpointPtr endpoint_in;
    usb::EndpointPtr endpoint_out;

    for (int i = 0; i < endpoint_count; ++i) {
        usb::EndpointPtr ep = usb_interface->GetEndpoint(i);
        uint8_t address = ep->GetAddress();
        auto type = ep->GetType();
        auto direction = ep->GetDirection();

        // Look for HTTP endpoints: address 0x01 with Bulk type
        if (address == 0x01 && type == mtp::usb::EndpointType::Bulk) {
            if (direction == mtp::usb::EndpointDirection::Out) {
                endpoint_out = ep;
                Log("  → Found HTTP OUT endpoint: 0x01");
            }
            else if (direction == mtp::usb::EndpointDirection::In) {
                endpoint_in = ep;
                Log("  → Found HTTP IN endpoint: 0x01");
            }
        }
    }

    if (!endpoint_in || !endpoint_out) {
        throw std::runtime_error("Failed to discover HTTP endpoints 0x01 IN/OUT");
    }

    Log("✓ USB handles and endpoints extracted");
    Log("  IMPORTANT: After MTP disconnect, you must re-claim the interface");
    Log("  Call usb_device->ClaimInterface(usb_interface) to regain access");

    USBHandlesWithEndpoints handles;
    handles.device = usb_device;
    handles.interface = usb_interface;
    handles.endpoint_in = endpoint_in;
    handles.endpoint_out = endpoint_out;

    return handles;
}
