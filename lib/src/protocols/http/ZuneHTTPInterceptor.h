#pragma once

#include <string>
#include <memory>
#include <thread>
#include <atomic>
#include <mutex>
#include <functional>
#include <map>
#include <queue>
#include <deque>
#include <set>
#include <condition_variable>
#include <chrono>
#include <mtp/ptp/Device.h>
#include <mtp/ptp/Session.h>
#include <mtp/ByteArray.h>
#include <usb/Device.h>
#include <usb/Interface.h>

// Forward declarations
class StaticModeHandler;
class ProxyModeHandler;
class HybridModeHandler;
class PPPParser;
class CCPHandler;
class DNSHandler;

// Need full definitions for used types
#include "HTTPParser.h"
#include "../tcp/TCPConnectionManager.h"  // Includes TCPFlowController, HTTPTransmission

// Configuration enums
enum class InterceptionMode {
    Disabled = 0,
    Static = 1,
    Proxy = 2,
    Hybrid = 3
};

// Configuration structures
struct StaticModeConfig {
    std::string data_directory;  // Path to artist_data folder
    bool test_mode = false;      // If true, redirect all UUIDs to 00000000-0000-0000-0000-000000000000
};

struct ProxyModeConfig {
    std::string catalog_server;  // e.g. "http://192.168.0.30" or "http://192.168.0.30:80"
    std::string image_server;    // Can be empty to use catalog_server
    std::string art_server;      // Can be empty to use catalog_server
    std::string mix_server;      // Can be empty to use catalog_server
    int timeout_ms = 30000;      // HTTP request timeout (30 seconds for proxied image fetches)
};

struct InterceptorConfig {
    InterceptionMode mode = InterceptionMode::Disabled;
    StaticModeConfig static_config;
    ProxyModeConfig proxy_config;
    std::string server_ip;  // IP address for DNS resolution (e.g., "192.168.0.30")
};

// HTTP Request structure
struct HTTPRequest {
    std::string method;          // GET, POST, etc.
    std::string path;            // /v3.0/en-US/music/artist/...
    std::string query_string;    // Query parameters (e.g., "?LinkId=25817")
    std::string host;            // catalog.zune.net, image.catalog.zune.net, etc.
    std::string protocol;        // HTTP/1.1
    std::map<std::string, std::string> headers;

    // TCP/IP info for response
    uint32_t src_ip;
    uint16_t src_port;
    uint32_t dst_ip;
    uint16_t dst_port;
    uint32_t seq_num;
    uint32_t ack_num;
    size_t http_request_size;  // Size of the HTTP request data for ACK calculation
};

// Zune device TCP window size (observed from ACK packets)
constexpr uint16_t ZUNE_TCP_WINDOW_SIZE = 33580;

/**
 * ZuneHTTPInterceptor
 *
 * Intercepts HTTP requests from Zune device at USB level and responds with
 * artist metadata (biography, images) either from local files or by proxying
 * to an HTTP server.
 *
 * Architecture (Clean Coordinator Pattern):
 * - Monitors USB bulk IN endpoint for device requests
 * - Routes PPP frames to appropriate handlers
 * - Delegates TCP state to TCPConnectionManager (SINGLE SOURCE OF TRUTH)
 * - Routes HTTP requests to mode handlers
 * - Coordinates responses back through USB
 *
 * All TCP connection state, flow control, and transmission tracking is
 * managed by TCPConnectionManager. This class is purely a coordinator.
 */
class ZuneHTTPInterceptor {
public:
    using LogCallback = std::function<void(const std::string& message)>;

    /**
     * Constructor (via MTP Session)
     * @param session MTP session for accessing USB infrastructure
     */
    explicit ZuneHTTPInterceptor(mtp::SessionPtr session);

    /**
     * Constructor (via Raw USB with pre-discovered endpoints)
     * @param device Raw USB device
     * @param interface Raw USB interface
     * @param endpoint_in HTTP bulk IN endpoint
     * @param endpoint_out HTTP bulk OUT endpoint
     * @param claim_interface If true, claim the interface (set false if MTP is still active)
     */
    ZuneHTTPInterceptor(mtp::usb::DevicePtr device, mtp::usb::InterfacePtr interface,
                        mtp::usb::EndpointPtr endpoint_in, mtp::usb::EndpointPtr endpoint_out,
                        bool claim_interface = true);

    ~ZuneHTTPInterceptor();

    void Start(const InterceptorConfig& config);
    void Stop();
    bool IsRunning() const;
    InterceptorConfig GetConfig() const;
    void SetLogCallback(LogCallback callback);
    void SetVerboseLogging(bool enable);
    // Public for testing
    void HandleIPCPPacket(const mtp::ByteArray& ipcp_data);
    void HandleDNSQuery(const mtp::ByteArray& ip_packet);
    virtual bool SendVendorCommand(const mtp::ByteArray& data);
    void InitializeDNSForTesting(const std::string& server_ip);
    void EnableNetworkPolling();

    /// Perform one polling cycle: PollEvent → Op922d → ProcessPacket → ProcessPendingSends.
    /// Called from C# in a loop for clean cancellation (no native monitoring thread).
    /// @param timeout_ms USB interrupt timeout in milliseconds (0 = non-blocking)
    /// @return 1 = processed data, 0 = timeout (no data), -1 = not running, -2 = session unavailable
    int PollOnce(int timeout_ms);

    // Hybrid mode callbacks (C# interop)
    using PathResolverCallback = const char* (*)(
        const char* artist_uuid,
        const char* endpoint_type,
        const char* resource_id,
        void* user_data
    );

    using CacheStorageCallback = bool (*)(
        const char* artist_uuid,
        const char* endpoint_type,
        const char* resource_id,
        const void* data,
        size_t data_length,
        const char* content_type,
        void* user_data
    );

    void SetPathResolverCallback(PathResolverCallback callback, void* user_data);
    void SetCacheStorageCallback(CacheStorageCallback callback, void* user_data);

private:
    void TimeoutCheckerThread();
    void CheckAllConnectionTimeouts();
    void RetransmitSegment(const std::string& conn_key, const SentSegment& segment);
    bool DiscoverEndpoints();
    void ProcessPacket(const mtp::ByteArray& usb_data);
    void ProcessPendingSends();
    void ProcessPPPFrame(const mtp::ByteArray& frame_data);
    void HandleHTTPRequest(const HTTPRequest& request);
    void RequestWorkerThread();

    /**
     * Build and queue HTTP response for transmission
     * Delegates all TCP state management to TCPConnectionManager
     */
    void SendHTTPResponse(const HTTPRequest& request, const HTTPParser::HTTPResponse& response);

    /**
     * Send next batch of segments for a transmission
     * Gets segments from TCPConnectionManager and queues them for USB transmission
     */
    void SendNextBatch(const std::string& conn_key, uint32_t base_seq);

    void SendTCPResponse(uint32_t src_ip, uint16_t src_port,
                        uint32_t dst_ip, uint16_t dst_port,
                        uint32_t seq_num, uint32_t ack_num,
                        uint8_t flags);

    void SendTCPPacket(const TCPPacket& packet);

    void SendTCPResponseWithData(uint32_t src_ip, uint16_t src_port,
                                 uint32_t dst_ip, uint16_t dst_port,
                                 uint32_t seq_num, uint32_t ack_num,
                                 uint8_t flags, const mtp::ByteArray& data);

    void DrainResponseQueue();
    void Log(const std::string& message);
    void VerboseLog(const std::string& message);
    void InitializeDNSHostnameMap(uint32_t dns_target_ip);

    // Member variables
    mtp::SessionPtr session_;
    InterceptorConfig config_;
    LogCallback log_callback_;
    bool verbose_logging_ = true;

    // USB infrastructure
    mtp::usb::DevicePtr usb_device_;
    mtp::usb::InterfacePtr usb_interface_;
    mtp::usb::InterfaceTokenPtr interface_token_;
    mtp::usb::EndpointPtr endpoint_in_;
    mtp::usb::EndpointPtr endpoint_out_;
    mtp::usb::EndpointPtr endpoint_interrupt_;
    bool endpoints_discovered_ = false;

    // Threads (worker threads for HTTP requests, timeout checker for RTO)
    // No monitoring thread — C# drives polling via PollOnce()
    std::atomic<bool> running_{false};
    std::atomic<bool> network_polling_enabled_{false};
    std::unique_ptr<std::thread> timeout_checker_thread_;
    std::atomic<bool> timeout_checker_running_{false};

    // Parsers
    std::unique_ptr<PPPParser> ppp_parser_;
    std::unique_ptr<HTTPParser> http_parser_;

    // Protocol handlers - THE ACTUAL COMPONENTS
    std::unique_ptr<CCPHandler> ccp_handler_;
    std::unique_ptr<DNSHandler> dns_handler_;
    std::unique_ptr<TCPConnectionManager> tcp_manager_;  // SINGLE SOURCE OF TRUTH for TCP state

    // Mode handlers
    std::unique_ptr<StaticModeHandler> static_handler_;
    std::unique_ptr<ProxyModeHandler> proxy_handler_;
    std::unique_ptr<HybridModeHandler> hybrid_handler_;

    // Configuration mutex
    mutable std::mutex config_mutex_;

    // DNS hostname mappings
    std::map<std::string, uint32_t> dns_hostname_map_;

    // Response queue for PPP frames (deque for O(1) pop_front)
    std::deque<mtp::ByteArray> response_queue_;
    std::mutex response_queue_mutex_;
    std::mutex drain_mutex_;

    // Buffer for incomplete PPP frames
    mtp::ByteArray incomplete_ppp_frame_buffer_;

    // HTTP request queue for worker threads
    std::queue<HTTPRequest> request_queue_;
    std::mutex request_queue_mutex_;
    std::condition_variable request_queue_cv_;
    std::vector<std::unique_ptr<std::thread>> request_worker_threads_;
    static constexpr size_t NUM_WORKER_THREADS = 4;

    // Pending sends tracking
    std::set<std::string> pending_sends_;
    std::mutex pending_sends_mutex_;

    // Hybrid mode callbacks
    PathResolverCallback path_resolver_callback_ = nullptr;
    void* path_resolver_user_data_ = nullptr;
    CacheStorageCallback cache_storage_callback_ = nullptr;
    void* cache_storage_user_data_ = nullptr;
    std::mutex callback_mutex_;
};
