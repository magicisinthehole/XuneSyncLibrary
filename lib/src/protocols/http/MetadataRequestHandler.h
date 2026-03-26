#pragma once

#include <string>
#include <functional>
#include <memory>
#include <mtp/ByteArray.h>
#include "HTTPParser.h"

// Forward declarations
class HttpClient;
enum class InterceptionMode;
struct ProxyModeConfig;

enum class EndpointType {
    Overview,
    Biography,
    Images,
    Artwork,
    DeviceBackgroundImage,
    SimilarArtists,
    Albums,
    Tracks,
    Unknown
};

const char* EndpointTypeToString(EndpointType type);
bool IsImageEndpoint(EndpointType type);

/**
 * Unified handler for all artist metadata interception modes.
 *
 * - Static:  Resolve via C# callbacks only, 404 on miss.
 * - Proxy:   Forward to HTTP server, no local resolution.
 * - Hybrid:  Try local via callbacks, proxy on miss, cache response.
 */
class MetadataRequestHandler {
public:
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

    using LogCallback = std::function<void(const std::string& message)>;

    /// @param mode Determines local/proxy/hybrid behavior
    MetadataRequestHandler(InterceptionMode mode, const ProxyModeConfig& proxy_config);

    ~MetadataRequestHandler();

    HTTPParser::HTTPResponse HandleRequest(const HTTPParser::HTTPRequest& request);

    void SetPathResolverCallback(PathResolverCallback callback, void* user_data);
    void SetCacheStorageCallback(CacheStorageCallback callback, void* user_data);
    void SetLogCallback(LogCallback callback);

    bool TestConnection();

private:
    HTTPParser::HTTPResponse HandleStatic(
        const std::string& artist_uuid,
        EndpointType endpoint_type,
        const std::string& resource_id);

    HTTPParser::HTTPResponse HandleProxy(
        const HTTPParser::HTTPRequest& request,
        const std::string& full_url,
        const std::string& artist_uuid,
        EndpointType endpoint_type,
        const std::string& resource_id);

    HTTPParser::HTTPResponse HandleHybrid(
        const HTTPParser::HTTPRequest& request,
        const std::string& full_url,
        const std::string& server,
        const std::string& artist_uuid,
        EndpointType endpoint_type,
        const std::string& resource_id);

    HTTPParser::HTTPResponse TryServeFromLocal(
        const std::string& artist_uuid,
        EndpointType endpoint_type,
        const std::string& resource_id);

    void CacheResponse(
        const std::string& artist_uuid,
        EndpointType endpoint_type,
        const std::string& resource_id,
        const HTTPParser::HTTPResponse& response);

    static EndpointType DetermineEndpointType(const std::string& path);
    mtp::ByteArray ReadFile(const std::string& file_path);
    std::string GetCurrentHttpDate();
    std::string GetFileModificationDate(const std::string& file_path);
    std::string GenerateETag(const std::string& file_path, size_t file_size);
    std::string GetContentType(const std::string& file_path);
    void Log(const std::string& message);

    InterceptionMode mode_;

    // Callbacks (used by Static + Hybrid)
    PathResolverCallback path_resolver_callback_ = nullptr;
    void* path_resolver_user_data_ = nullptr;
    CacheStorageCallback cache_storage_callback_ = nullptr;
    void* cache_storage_user_data_ = nullptr;
    LogCallback log_callback_;

    // HttpClient (used by Proxy + Hybrid)
    std::unique_ptr<HttpClient> http_client_;
};
