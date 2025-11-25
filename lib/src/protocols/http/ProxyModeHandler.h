#pragma once

#include "HTTPParser.h"
#include <string>
#include <functional>
#include <memory>

// Forward declaration
class HttpClient;

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
 */
class ProxyModeHandler {
public:
    using LogCallback = std::function<void(const std::string& message)>;

    struct ProxyConfig {
        std::string catalog_server;  // e.g., "http://192.168.0.30"
        std::string image_server;    // Optional, defaults to catalog_server
        std::string art_server;      // Optional, defaults to catalog_server
        std::string mix_server;      // Optional, defaults to catalog_server
        int timeout_ms = 30000;      // Request timeout (30s for images)
    };

    /**
     * Constructor
     * @param config Proxy server configuration
     */
    explicit ProxyModeHandler(const ProxyConfig& config);

    /**
     * Destructor
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
    void Log(const std::string& message);

    ProxyConfig config_;
    LogCallback log_callback_;
    std::unique_ptr<HttpClient> http_client_;
};
