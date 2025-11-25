#pragma once

#include <string>
#include <functional>
#include <memory>
#include <mtp/ByteArray.h>
#include "HTTPParser.h"

// Forward declarations
class HttpClient;

/**
 * HybridModeHandler
 *
 * Handles HTTP requests in hybrid mode:
 * 1. Calls C# path resolver callback to check for local files
 * 2. If found locally → serves file
 * 3. If not found → proxies to HTTP server (via HttpClient)
 * 4. If proxied → calls C# cache storage callback to cache response
 */
class HybridModeHandler {
public:
    /**
     * Path resolver callback - resolves artist UUID to local file path
     */
    using PathResolverCallback = const char* (*)(
        const char* artist_uuid,
        const char* endpoint_type,
        const char* resource_id,
        void* user_data
    );

    /**
     * Cache storage callback - stores proxy response to local filesystem
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

    using LogCallback = std::function<void(const std::string& message)>;

    /**
     * Constructor
     */
    HybridModeHandler(
        const std::string& proxy_catalog_server,
        const std::string& proxy_image_server = "",
        const std::string& proxy_art_server = "",
        const std::string& proxy_mix_server = "",
        int proxy_timeout_ms = 30000
    );

    ~HybridModeHandler();

    /**
     * Handle an HTTP request in hybrid mode
     */
    HTTPParser::HTTPResponse HandleRequest(const HTTPParser::HTTPRequest& request);

    void SetPathResolverCallback(PathResolverCallback callback, void* user_data);
    void SetCacheStorageCallback(CacheStorageCallback callback, void* user_data);
    void SetLogCallback(LogCallback callback);

    /**
     * Test proxy server connectivity
     */
    bool TestConnection();

private:
    HTTPParser::HTTPResponse TryServeFromLocal(
        const std::string& artist_uuid,
        const std::string& endpoint_type,
        const std::string& resource_id);

    void CacheResponse(
        const std::string& artist_uuid,
        const std::string& endpoint_type,
        const std::string& resource_id,
        const HTTPParser::HTTPResponse& response);

    std::string DetermineEndpointType(const std::string& path);
    mtp::ByteArray ReadFile(const std::string& file_path);
    std::string GetCurrentHttpDate();
    std::string GetFileModificationDate(const std::string& file_path);
    std::string GenerateETag(const std::string& file_path, size_t file_size);
    std::string GetContentType(const std::string& file_path);
    void Log(const std::string& message);

    // Configuration (kept for backwards compat with C# that might query these)
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

    // HttpClient (SINGLE SOURCE OF TRUTH for HTTP requests)
    std::unique_ptr<HttpClient> http_client_;
};
