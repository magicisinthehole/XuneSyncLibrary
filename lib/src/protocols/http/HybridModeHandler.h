#pragma once

#include <string>
#include <functional>
#include <mutex>
#include <mtp/ByteArray.h>
#include "HTTPParser.h"

// Forward declaration
class ZuneHTTPInterceptor;

/**
 * HybridModeHandler
 *
 * Handles HTTP requests in hybrid mode:
 * 1. Calls C# path resolver callback to check for local files
 * 2. If found locally → serves file (like StaticModeHandler)
 * 3. If not found → proxies to HTTP server (like ProxyModeHandler)
 * 4. If proxied → calls C# cache storage callback to cache response
 *
 * This mode allows building a local cache progressively by proxying on first request
 * and serving from cache on subsequent requests.
 */
class HybridModeHandler {
public:
    /**
     * Path resolver callback type - resolves MusicBrainz UUID to local file path
     *
     * MEMORY CONTRACT:
     * - Returns: File path as C string (owned by caller), or NULL if not found
     * - The returned string must remain valid until the callback returns
     * - C++ will copy the string immediately
     * - Caller (C#) is responsible for managing the string's lifetime and freeing it
     * - C++ will NOT call free() on the returned pointer
     *
     * @param artist_uuid MusicBrainz UUID from HTTP request
     * @param endpoint_type Endpoint type (biography, images, overview, artwork, etc.)
     * @param resource_id Optional resource ID for specific images (may be NULL)
     * @param user_data User-provided context pointer
     * @return File path if found (caller manages memory), NULL if not found
     */
    using PathResolverCallback = const char* (*)(
        const char* artist_uuid,
        const char* endpoint_type,
        const char* resource_id,
        void* user_data
    );

    /**
     * Cache storage callback type - stores proxy response to local filesystem
     */
    using CacheStorageCallback = bool (*)(
        const char* artist_uuid,
        const char* endpoint_type,
        const char* resource_id,
        const void* data,
        size_t data_length,
        const char* content_type,
        void* user_data
    );

    /**
     * Log callback type
     */
    using LogCallback = std::function<void(const std::string& message)>;

    /**
     * Constructor
     * @param proxy_catalog_server Base URL for catalog server (e.g., "http://192.168.0.30")
     * @param proxy_image_server Base URL for image server (optional, defaults to catalog_server)
     * @param proxy_art_server Base URL for art server (optional, defaults to catalog_server)
     * @param proxy_mix_server Base URL for mix server (optional, defaults to catalog_server)
     * @param proxy_timeout_ms HTTP request timeout in milliseconds
     */
    HybridModeHandler(
        const std::string& proxy_catalog_server,
        const std::string& proxy_image_server = "",
        const std::string& proxy_art_server = "",
        const std::string& proxy_mix_server = "",
        int proxy_timeout_ms = 5000
    );

    /**
     * Destructor
     */
    ~HybridModeHandler();

    /**
     * Handle an HTTP request in hybrid mode (matches ProxyModeHandler signature)
     * @param request Parsed HTTP request from device
     * @return HTTP response (status line + headers + body)
     */
    HTTPParser::HTTPResponse HandleRequest(const HTTPParser::HTTPRequest& request);

    /**
     * Set path resolver callback
     * @param callback Path resolver callback function
     * @param user_data User-provided context pointer
     */
    void SetPathResolverCallback(PathResolverCallback callback, void* user_data);

    /**
     * Set cache storage callback
     * @param callback Cache storage callback function
     * @param user_data User-provided context pointer
     */
    void SetCacheStorageCallback(CacheStorageCallback callback, void* user_data);

    /**
     * Set logging callback
     * @param callback Logging callback function
     */
    void SetLogCallback(LogCallback callback);

    /**
     * Test proxy server connectivity
     * @return true if server is reachable
     */
    bool TestConnection();

private:
    /**
     * Try to resolve and serve from local file
     * @param artist_uuid MusicBrainz UUID extracted from path
     * @param endpoint_type Endpoint type (biography, images, etc.)
     * @param resource_id Optional resource ID
     * @return Response if successful, empty response with status 0 if not found
     */
    HTTPParser::HTTPResponse TryServeFromLocal(const std::string& artist_uuid,
                                                const std::string& endpoint_type,
                                                const std::string& resource_id);

    /**
     * Proxy request to HTTP server
     * @param url Full URL to proxy to
     * @param headers Request headers
     * @return HTTP response from proxy server
     */
    HTTPParser::HTTPResponse ProxyRequest(const std::string& url,
                                          const std::map<std::string, std::string>& headers);

    /**
     * Cache a proxy response
     * @param artist_uuid MusicBrainz UUID
     * @param endpoint_type Endpoint type
     * @param resource_id Optional resource ID
     * @param response HTTP response to cache
     */
    void CacheResponse(const std::string& artist_uuid,
                       const std::string& endpoint_type,
                       const std::string& resource_id,
                       const HTTPParser::HTTPResponse& response);

    /**
     * Extract artist UUID from request path
     * @param path Request path (e.g., "/v3.0/en-US/music/artist/{uuid}/biography")
     * @return Artist UUID or empty string if not found
     */
    std::string ExtractArtistUUID(const std::string& path);

    /**
     * Determine endpoint type from path
     * @param path Request path
     * @return Endpoint type (biography, images, overview, etc.)
     */
    std::string DetermineEndpointType(const std::string& path);

    /**
     * Extract resource ID from path (for image requests)
     * @param path Request path
     * @return Resource ID or empty string
     */
    std::string ExtractResourceId(const std::string& path);

    /**
     * Read file from filesystem
     * @param file_path Full path to file
     * @return File contents or empty if file doesn't exist/can't be read
     */
    mtp::ByteArray ReadFile(const std::string& file_path);

    /**
     * Get current date/time in HTTP RFC 1123 format
     * @return Date string (e.g., "Thu, 20 Nov 2025 10:28:15 GMT")
     */
    std::string GetCurrentHttpDate();

    /**
     * Get file modification date in HTTP RFC 1123 format
     * @param file_path Path to file
     * @return Date string (e.g., "Thu, 20 Nov 2025 10:28:15 GMT")
     */
    std::string GetFileModificationDate(const std::string& file_path);

    /**
     * Generate ETag for file based on modification time and size
     * @param file_path Path to file
     * @param file_size Size of file in bytes
     * @return ETag string (e.g., "\"1763669677.4040117-63746-3653050671\"")
     */
    std::string GenerateETag(const std::string& file_path, size_t file_size);

    /**
     * Determine content type from file extension
     * @param file_path File path
     * @return Content type (text/xml, image/jpeg, etc.)
     */
    std::string GetContentType(const std::string& file_path);

    /**
     * Log a message via callback if set
     */
    void Log(const std::string& message);

    /**
     * Select appropriate server based on request host (matches ProxyModeHandler logic)
     * @param host Host header value (e.g., "catalog.zune.net", "image.catalog.zune.net")
     * @return Server base URL to use for proxying
     */
    std::string SelectServer(const std::string& host);

    /**
     * Build full URL for proxy request (matches ProxyModeHandler logic)
     * @param server Base server URL
     * @param path Request path
     * @return Full URL (e.g., "http://localhost:8000/v3.0/...")
     */
    std::string BuildURL(const std::string& server, const std::string& path);

    /**
     * Parse HTTP response from upstream server (matches ProxyModeHandler logic)
     * @param response_data Raw response body bytes
     * @param status_code HTTP status code
     * @param headers Response headers from upstream
     * @return Parsed HTTP response with cleaned headers and content-type detection
     */
    HTTPParser::HTTPResponse ParseResponse(
        const mtp::ByteArray& response_data,
        int status_code,
        std::map<std::string, std::string> headers
    );

    // Configuration
    std::string proxy_catalog_server_;
    std::string proxy_image_server_;
    std::string proxy_art_server_;
    std::string proxy_mix_server_;
    int proxy_timeout_ms_;

    // Callbacks
    PathResolverCallback path_resolver_callback_ = nullptr;
    void* path_resolver_user_data_ = nullptr;
    CacheStorageCallback cache_storage_callback_ = nullptr;
    void* cache_storage_user_data_ = nullptr;
    LogCallback log_callback_;

    // libcurl handle (reused for performance) and its mutex
    void* curl_handle_ = nullptr;  // CURL* but avoiding including curl.h in header
    std::mutex curl_mutex_;  // Protects curl_handle_ from concurrent access
};
