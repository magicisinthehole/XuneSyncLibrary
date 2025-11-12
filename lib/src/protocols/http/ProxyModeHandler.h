#pragma once

#include "HTTPParser.h"
#include <string>
#include <functional>

/**
 * ProxyModeHandler
 *
 * Handles HTTP requests by forwarding them to an external HTTP server
 * and relaying the response back to the device.
 *
 * Server Mapping:
 * - catalog.zune.net -> proxy_catalog_server
 * - image.catalog.zune.net -> proxy_image_server (or catalog if not set)
 * - art.zune.net -> proxy_art_server (or catalog if not set)
 * - mix.zune.net -> proxy_mix_server (or catalog if not set)
 *
 * Example:
 * Device requests: GET /v3.0/en-US/music/artist/{uuid}/biography
 * Host: catalog.zune.net
 */
class ProxyModeHandler {
public:
    using LogCallback = std::function<void(const std::string& message)>;

    struct ProxyConfig {
        std::string catalog_server;  // e.g., "http://192.168.0.30" (port 80 default)
        std::string image_server;    // Optional, defaults to catalog_server
        std::string art_server;      // Optional, defaults to catalog_server
        std::string mix_server;      // Optional, defaults to catalog_server
        int timeout_ms = 5000;       // Request timeout
    };

    /**
     * Constructor
     * @param config Proxy server configuration
     */
    explicit ProxyModeHandler(const ProxyConfig& config);

    /**
     * Destructor - cleanup libcurl resources
     */
    ~ProxyModeHandler();

    /**
     * Handle an HTTP request by proxying to server
     * @param request Parsed HTTP request from device
     * @return HTTP response from server or error response
     */
    HTTPParser::HTTPResponse HandleRequest(const HTTPParser::HTTPRequest& request);

    /**
     * Set logging callback
     * @param callback Function to receive log messages
     */
    void SetLogCallback(LogCallback callback);

    /**
     * Test connection to proxy server
     * @return true if server is reachable
     */
    bool TestConnection();

    /**
     * Get current configuration
     * @return Proxy configuration
     */
    ProxyConfig GetConfig() const { return config_; }

private:
    /**
     * Select appropriate server based on request host
     * @param host Host header value (e.g., "catalog.zune.net")
     * @return Server base URL to forward to
     */
    std::string SelectServer(const std::string& host);

    /**
     * Build full URL for proxy request
     * @param server Base server URL
     * @param path Request path
     * @return Full URL (e.g., "http://localhost:8000/v3.0/...")
     */
    std::string BuildURL(const std::string& server, const std::string& path);

    /**
     * Perform HTTP GET request using libcurl
     * @param url Full URL to request
     * @param headers Request headers
     * @return HTTP response
     */
    HTTPParser::HTTPResponse PerformGET(const std::string& url,
                                       const std::map<std::string, std::string>& headers);

    /**
     * Parse HTTP response from libcurl
     * @param response_data Raw HTTP response bytes
     * @param status_code HTTP status code
     * @param headers Response headers captured from upstream server
     * @return Parsed HTTP response
     */
    HTTPParser::HTTPResponse ParseResponse(const mtp::ByteArray& response_data,
                                          int status_code,
                                          std::map<std::string, std::string> headers);

    /**
     * Log a message
     */
    void Log(const std::string& message);

    // Configuration
    ProxyConfig config_;
    LogCallback log_callback_;

    // libcurl handle (initialized once, reused for requests)
    void* curl_handle_ = nullptr;  // CURL* but avoiding header dependency
};
