#pragma once

#include <string>
#include <map>
#include <functional>
#include <mutex>
#include <mtp/ByteArray.h>
#include "HTTPParser.h"

/**
 * HttpClient
 *
 * Shared HTTP client for making proxied requests using libcurl.
 * Consolidates duplicate curl code from ProxyModeHandler, HybridModeHandler,
 * and ZuneHTTPInterceptor.
 *
 * Features:
 * - Thread-safe curl handle management
 * - Zune server routing (catalog, image, art, mix)
 * - Response parsing with content-type detection
 * - Connectivity checks (fwlink handling)
 */
class HttpClient {
public:
    using LogCallback = std::function<void(const std::string& message)>;

    struct ServerConfig {
        std::string catalog_server;  // Base URL (e.g., "http://192.168.0.30")
        std::string image_server;    // Optional, defaults to catalog_server
        std::string art_server;      // Optional, defaults to catalog_server
        std::string mix_server;      // Optional, defaults to catalog_server
        int timeout_ms = 30000;      // Request timeout (30s default for images)
    };

    /**
     * Constructor
     * @param config Server configuration
     */
    explicit HttpClient(const ServerConfig& config);

    /**
     * Destructor - cleanup libcurl resources
     */
    ~HttpClient();

    // Non-copyable
    HttpClient(const HttpClient&) = delete;
    HttpClient& operator=(const HttpClient&) = delete;

    /**
     * Perform HTTP GET request
     * @param url Full URL to request
     * @param headers Request headers to forward
     * @return HTTP response
     */
    HTTPParser::HTTPResponse PerformGET(
        const std::string& url,
        const std::map<std::string, std::string>& headers = {});

    /**
     * Select appropriate server based on Host header
     * Maps Zune hostnames to configured servers:
     * - image.catalog.zune.net -> image_server
     * - catalog.zune.net -> catalog_server
     * - art.zune.net -> art_server
     * - mix.zune.net -> mix_server
     *
     * @param host Host header value
     * @return Server base URL
     */
    std::string SelectServer(const std::string& host) const;

    /**
     * Build full URL for proxy request
     * @param server Base server URL
     * @param path Request path
     * @param query_params Optional query parameters
     * @return Full URL
     */
    static std::string BuildURL(
        const std::string& server,
        const std::string& path,
        const std::map<std::string, std::string>& query_params = {});

    /**
     * Check if request is a connectivity check (fwlink)
     * @param path Request path
     * @return true if this is a connectivity check request
     */
    static bool IsConnectivityCheck(const std::string& path);

    /**
     * Build connectivity check response (200 OK with minimal HTML)
     * @return HTTP response for connectivity check
     */
    static HTTPParser::HTTPResponse BuildConnectivityResponse();

    /**
     * Test connection to server
     * @return true if server is reachable
     */
    bool TestConnection();

    /**
     * One-shot fetch for external URLs (no persistent connection)
     * Used for proxying requests to external hosts like go.microsoft.com
     * @param url Full URL to fetch
     * @param timeout_seconds Request timeout
     * @return HTTP response
     */
    static HTTPParser::HTTPResponse FetchExternal(const std::string& url, int timeout_seconds = 10);

    /**
     * Set logging callback
     * @param callback Logging function
     */
    void SetLogCallback(LogCallback callback);

    /**
     * Get current configuration
     * @return Server configuration
     */
    const ServerConfig& GetConfig() const { return config_; }

private:
    /**
     * Parse HTTP response from curl with content-type detection
     * @param response_data Raw response body
     * @param status_code HTTP status code
     * @param headers Response headers from upstream
     * @return Parsed HTTP response
     */
    static HTTPParser::HTTPResponse ParseResponse(
        const mtp::ByteArray& response_data,
        int status_code,
        std::map<std::string, std::string> headers);

    /**
     * Detect content type from response data
     * @param data Response body
     * @return Detected MIME type
     */
    static std::string DetectContentType(const mtp::ByteArray& data);

    /**
     * Log a message via callback if set
     */
    void Log(const std::string& message);

    // Configuration
    ServerConfig config_;
    LogCallback log_callback_;

    // libcurl handle (reused for performance)
    void* curl_handle_ = nullptr;  // CURL*
    std::mutex curl_mutex_;  // Thread safety
};
