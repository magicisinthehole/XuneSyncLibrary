#pragma once

#include <string>
#include <memory>
#include <thread>
#include <atomic>
#include <mutex>
#include <functional>
#include <map>
#include <queue>
#include <set>
#include <condition_variable>
#include <chrono>
#include <mtp/ptp/Device.h>
#include <mtp/ptp/Session.h>
#include <mtp/ByteArray.h>
#include <usb/Device.h>
#include <usb/Interface.h>
#include <curl/curl.h>

// Forward declarations
class StaticModeHandler;
class ProxyModeHandler;
class PPPParser;

// Need full HTTPParser definition for nested HTTPResponse type
#include "HTTPParser.h"

// Configuration enums
enum class InterceptionMode {
    Disabled = 0,
    Static = 1,
    Proxy = 2
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

// TCP Connection State
struct TCPConnectionState {
    uint32_t seq_num;
    uint32_t ack_num;
    uint32_t last_received_seq = 0;  // Track last received SEQ for retransmission detection
    bool syn_received = false;
    bool connected = false;
    mtp::ByteArray http_buffer;  // Buffer for TCP stream reassembly
};

// HTTP Response Transmission State (for TCP flow-controlled segment delivery)
// Tracks the transmission of a single HTTP response split into multiple TCP segments.
// Official Zune software sends 2-4 segments at a time, waits for ACKs, then sends next batch.
// This prevents overwhelming the device's ~33KB TCP receive buffer.
struct HTTPResponseTransmissionState {
    std::vector<mtp::ByteArray> queued_segments;  // All PPP frames (TCP/IP/PPP wrapped) for this response
    std::vector<size_t> segment_payload_sizes;    // HTTP payload size for each segment (for flow control)
    size_t next_segment_index = 0;                // Next segment to transmit
    uint32_t last_acked_seq = 0;                  // Last sequence number ACKed by device
    uint32_t base_seq = 0;                        // Starting SEQ number for this response
    uint32_t window_size = 33580;                 // Device's advertised TCP window (from ACK packets)
    bool transmission_complete = false;            // True when all segments sent and ACKed
    std::chrono::steady_clock::time_point last_ack_time;  // For timeout detection
    size_t bytes_in_flight = 0;                   // HTTP payload bytes sent but not yet ACKed

    // Fast retransmit support (RFC 2581)
    uint32_t last_received_ack = 0;               // Last ACK number received (for duplicate detection)
    size_t duplicate_ack_count = 0;               // Count of consecutive duplicate ACKs
    bool retransmit_needed = false;               // True when fast retransmit should occur
    size_t retransmit_segment_index = 0;          // Which segment to retransmit
    uint32_t last_window_right_edge = 0;          // Last window right edge (ACK + window) for duplicate detection (lwIP pattern)
    bool in_fast_recovery = false;                // True when in fast recovery (prevents multiple fast retransmits, matches lwIP TF_INFR flag)

    // Congestion control state (RFC 3465 - Appropriate Byte Counting, matches lwIP)
    size_t cwnd = 0;                              // Congestion window (bytes)
    size_t ssthresh = 65535;                      // Slow start threshold (bytes)
    size_t bytes_acked_accumulator = 0;           // Accumulator for congestion avoidance (RFC 3465)
    static constexpr size_t mss = 1460;           // Maximum segment size (matches typical segment payload)
};

/**
 * ZuneHTTPInterceptor
 *
 * Intercepts HTTP requests from Zune device at USB level and responds with
 * artist metadata (biography, images) either from local files or by proxying
 * to an HTTP server.
 *
 * Architecture:
 * - Monitors USB endpoint 0x81 (IN) for device requests
 * - Parses PPP frames containing IP/TCP/HTTP packets
 * - Routes requests to StaticModeHandler or ProxyModeHandler
 * - Builds HTTP responses and wraps in TCP/IP/PPP frames
 * - Sends responses via USB endpoint 0x01 (OUT)
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
     * @param endpoint_in HTTP IN endpoint (0x01)
     * @param endpoint_out HTTP OUT endpoint (0x01)
     * @param claim_interface If true, claim the interface (set false if MTP is still active)
     */
    ZuneHTTPInterceptor(mtp::usb::DevicePtr device, mtp::usb::InterfacePtr interface,
                        mtp::usb::EndpointPtr endpoint_in, mtp::usb::EndpointPtr endpoint_out,
                        bool claim_interface = true);

    /**
     * Destructor - stops monitoring thread if running
     */
    ~ZuneHTTPInterceptor();

    /**
     * Start the HTTP interceptor with specified configuration
     * @param config Configuration specifying mode and parameters
     * @throws std::runtime_error if USB endpoints not found or handlers cannot be initialized
     */
    void Start(const InterceptorConfig& config);

    /**
     * Stop the HTTP interceptor
     * Gracefully shuts down monitoring thread and cleans up resources
     */
    void Stop();

    /**
     * Check if interceptor is currently running
     * @return true if monitoring thread is active
     */
    bool IsRunning() const;

    /**
     * Get current configuration
     * @return Current interceptor configuration
     */
    InterceptorConfig GetConfig() const;

    /**
     * Set logging callback for diagnostic messages
     * @param callback Function to receive log messages
     */
    void SetLogCallback(LogCallback callback);

    /**
     * Initialize HTTP subsystem on device
     * Sends the initialization command sequence to activate HTTP functionality
     * @return true if initialization succeeded
     */
    bool InitializeHTTPSubsystem();

    /**
     * Handle IPCP packet for network configuration (PUBLIC FOR TESTING)
     * @param ipcp_data IPCP packet data (without PPP framing)
     */
    void HandleIPCPPacket(const mtp::ByteArray& ipcp_data);

    /**
     * Handle DNS query for hostname resolution (PUBLIC FOR TESTING)
     * @param ip_packet Complete IP packet containing UDP DNS query
     */
    void HandleDNSQuery(const mtp::ByteArray& ip_packet);

    /**
     * Send a vendor command on HTTP endpoint OUT (PUBLIC FOR TESTING)
     * @param data Command data to send
     * @return true if write succeeded
     */
    virtual bool SendVendorCommand(const mtp::ByteArray& data);

    /**
     * Initialize DNS hostname map for testing (PUBLIC FOR TESTING)
     * @param server_ip IP address to resolve all Zune hostnames to
     */
    void InitializeDNSForTesting(const std::string& server_ip);

    /**
     * Enable network mode polling
     * Call this AFTER TriggerNetworkMode() has initialized PPP/IPCP
     * This allows the monitoring thread to start polling with 0x922d
     */
    void EnableNetworkPolling();

private:
    /**
     * Main monitoring thread function
     * Polls USB endpoint 0x81 for incoming packets from device
     */
    void MonitorThread();

    /**
     * Discover USB endpoints for HTTP traffic (0x01 OUT, 0x81 IN)
     * @return true if endpoints found
     */
    bool DiscoverEndpoints();

    /**
     * Process a USB packet received from device (may contain multiple PPP frames)
     * @param usb_data Raw USB bulk transfer data
     */
    void ProcessPacket(const mtp::ByteArray& usb_data);

    /**
     * Process a single PPP frame extracted from USB packet
     * @param frame_data PPP frame data (0x7E...0x7E)
     */
    void ProcessPPPFrame(const mtp::ByteArray& frame_data);

    /**
     * Handle a parsed HTTP request
     * @param request Parsed HTTP request structure
     */
    void HandleHTTPRequest(const HTTPRequest& request);

    /**
     * Request worker thread function
     * Processes queued HTTP requests sequentially to prevent frame interleaving
     */
    void RequestWorkerThread();

    /**
     * Build and send HTTP response to device
     * @param request Original HTTP request (for TCP info)
     * @param response HTTP response to send
     */
    void SendHTTPResponse(const HTTPRequest& request, const HTTPParser::HTTPResponse& response);

    /**
     * Send next batch of TCP segments for an active transmission
     * Implements TCP flow control by respecting window size and bytes in flight
     * @param conn_key Connection key identifying the transmission
     */
    void SendNextBatch(const std::string& conn_key);

    /**
     * Proxy HTTP request to external server using libcurl
     * @param host Hostname to proxy to (e.g., "go.microsoft.com")
     * @param request HTTP request to proxy
     * @return HTTP response from external server
     */
    HTTPParser::HTTPResponse ProxyExternalHTTPRequest(const std::string& host, const HTTPRequest& request);

    /**
     * Build PPP frame containing IP/TCP/HTTP response
     * @param request Original request (for TCP/IP info)
     * @param response HTTP response data
     * @return PPP-framed packet ready for USB transmission
     */
    mtp::ByteArray BuildPPPFrame(const HTTPRequest& request, const HTTPParser::HTTPResponse& response);

    /**
     * Send TCP response packet (without HTTP data, for handshake)
     * @param src_ip Source IP address
     * @param src_port Source TCP port
     * @param dst_ip Destination IP address
     * @param dst_port Destination TCP port
     * @param seq_num Sequence number
     * @param ack_num Acknowledgment number
     * @param flags TCP flags (SYN, ACK, etc.)
     */
    void SendTCPResponse(uint32_t src_ip, uint16_t src_port,
                        uint32_t dst_ip, uint16_t dst_port,
                        uint32_t seq_num, uint32_t ack_num,
                        uint8_t flags);

    /**
     * Send TCP response packet with data payload (for DNS responses, etc.)
     * @param src_ip Source IP address
     * @param src_port Source TCP port
     * @param dst_ip Destination IP address
     * @param dst_port Destination TCP port
     * @param seq_num Sequence number
     * @param ack_num Acknowledgment number
     * @param flags TCP flags (ACK, PSH, etc.)
     * @param data TCP payload data
     */
    void SendTCPResponseWithData(uint32_t src_ip, uint16_t src_port,
                                 uint32_t dst_ip, uint16_t dst_port,
                                 uint32_t seq_num, uint32_t ack_num,
                                 uint8_t flags, const mtp::ByteArray& data);

    /**
     * Get or create TCP connection state for a connection
     * @param connection_key Unique key for this TCP connection
     * @return Reference to connection state
     */
    TCPConnectionState& GetConnectionState(const std::string& connection_key);

    /**
     * Create connection key from TCP connection parameters
     */
    std::string MakeConnectionKey(uint32_t src_ip, uint16_t src_port,
                                   uint32_t dst_ip, uint16_t dst_port) const;

    /**
     * Drain queued PPP response frames immediately
     * Sends all queued frames back-to-back via USB to prevent interleaving
     * Thread-safe: can be called from any thread
     */
    void DrainResponseQueue();

    /**
     * Log a message via callback if set
     */
    void Log(const std::string& message);

    // Member variables
    mtp::SessionPtr session_;
    InterceptorConfig config_;
    LogCallback log_callback_;

    // USB infrastructure from AFTL (extracted from session)
    mtp::usb::DevicePtr usb_device_;
    mtp::usb::InterfacePtr usb_interface_;
    mtp::usb::InterfaceTokenPtr interface_token_;  // Token to keep interface claimed
    mtp::usb::EndpointPtr endpoint_in_;   // Device to host (0x81)
    mtp::usb::EndpointPtr endpoint_out_;  // Host to device (0x01)
    mtp::usb::EndpointPtr endpoint_interrupt_;  // Interrupt endpoint (0x82) for event notifications
    bool endpoints_discovered_ = false;

    // Monitoring thread
    std::unique_ptr<std::thread> monitor_thread_;
    std::atomic<bool> running_{false};
    std::atomic<bool> network_polling_enabled_{false};  // Only poll 0x922d when network mode is active

    // Parsers
    std::unique_ptr<PPPParser> ppp_parser_;
    std::unique_ptr<HTTPParser> http_parser_;

    // Mode handlers
    std::unique_ptr<StaticModeHandler> static_handler_;
    std::unique_ptr<ProxyModeHandler> proxy_handler_;

    // TCP connection tracking
    std::map<std::string, TCPConnectionState> connections_;
    mutable std::mutex connections_mutex_;

    // HTTP response transmission tracking (for flow-controlled segment delivery)
    // Maps connection key to transmission state for active multi-segment HTTP responses
    std::map<std::string, HTTPResponseTransmissionState> active_transmissions_;
    mutable std::mutex transmissions_mutex_;

    // Configuration mutex
    mutable std::mutex config_mutex_;

    // Network configuration for PPP/IPCP
    uint32_t device_ip_ = 0xC0A83765;      // 192.168.55.101
    uint32_t host_ip_ = 0xC0A83764;        // 192.168.55.100
    uint32_t dns_ip_ = 0x7F000001;         // 127.0.0.1 (matches capture)

    // DNS hostname mappings
    std::map<std::string, uint32_t> dns_hostname_map_;

    // IPCP negotiation state
    bool ipcp_configured_ = false;

    // Response queue for network packets
    // After network mode is established, ALL responses must be queued and sent via 0x922d polls
    // (not via 0x922c which is only used during IPCP setup)
    std::vector<mtp::ByteArray> response_queue_;
    std::mutex response_queue_mutex_;
    std::mutex drain_mutex_;  // Serializes DrainResponseQueue() calls to prevent frame interleaving

    // Buffer for incomplete PPP frames spanning multiple USB packets
    // When a PPP frame starts with 0x7E but doesn't end with 0x7E in the same USB packet,
    // we store it here and prepend it to the next USB packet
    mtp::ByteArray incomplete_ppp_frame_buffer_;

    // Request queue for serialized HTTP request processing
    // Prevents frame interleaving when multiple requests arrive simultaneously
    std::queue<HTTPRequest> request_queue_;
    std::mutex request_queue_mutex_;
    std::condition_variable request_queue_cv_;
    std::unique_ptr<std::thread> request_worker_thread_;

    // Pending sends: connections that need SendNextBatch() called after frame processing
    // This prevents calling SendNextBatch() multiple times when a single USB packet
    // contains multiple ACK frames (which would send too many batches at once)
    std::set<std::string> pending_sends_;
    std::mutex pending_sends_mutex_;

    // Connection reuse throttling (matches official Zune software behavior)
    // Official software waits 1-4 seconds between HTTP responses on the same TCP connection
    // to give device's application layer time to process/save the previous response.
    // Maps connection key to last HTTP response completion time.
    std::map<std::string, std::chrono::steady_clock::time_point> connection_last_completion_time_;
    std::mutex connection_completion_mutex_;

    // Global large image throttling (applies across ALL connections)
    // Device needs time to process/save large downloads regardless of TCP connection.
    // Enforce minimum gap between when large transmissions START (not when they complete).
    // This prevents overwhelming the device with concurrent large downloads.
    // Official software: median 1.278s gap between large image requests.
    std::chrono::steady_clock::time_point last_large_response_start_time_;
    std::mutex large_response_throttle_mutex_;
};
