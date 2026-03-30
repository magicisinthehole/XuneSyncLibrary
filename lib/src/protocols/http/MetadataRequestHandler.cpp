#include "MetadataRequestHandler.h"
#include "HttpClient.h"
#include "ZuneHTTPInterceptor.h"  // InterceptionMode, ProxyModeConfig
#include <fstream>
#include <sstream>
#include <algorithm>
#include <chrono>
#include <ctime>
#include <sys/stat.h>
#include <functional>

#ifdef _WIN32
inline struct tm* gmtime_r(const time_t* timer, struct tm* buf) {
    return gmtime_s(buf, timer) == 0 ? buf : nullptr;
}
#endif

const char* EndpointTypeToString(EndpointType type) {
    switch (type) {
        case EndpointType::Overview:               return "overview";
        case EndpointType::Biography:              return "biography";
        case EndpointType::Images:                 return "images";
        case EndpointType::Artwork:                return "artwork";
        case EndpointType::DeviceBackgroundImage:  return "devicebackgroundimage";
        case EndpointType::SimilarArtists:         return "similarartists";
        case EndpointType::Albums:                 return "albums";
        case EndpointType::Tracks:                 return "tracks";
        case EndpointType::Unknown:                return "unknown";
    }
    return "unknown";
}

bool IsImageEndpoint(EndpointType type) {
    return type == EndpointType::Artwork || type == EndpointType::DeviceBackgroundImage;
}

MetadataRequestHandler::MetadataRequestHandler(
    InterceptionMode mode,
    const ProxyModeConfig& proxy_config)
    : mode_(mode)
{
    if (mode != InterceptionMode::Static) {
        HttpClient::ServerConfig config;
        config.catalog_server = proxy_config.catalog_server;
        config.image_server = proxy_config.image_server.empty() ? proxy_config.catalog_server : proxy_config.image_server;
        config.art_server = proxy_config.art_server.empty() ? proxy_config.catalog_server : proxy_config.art_server;
        config.mix_server = proxy_config.mix_server.empty() ? proxy_config.catalog_server : proxy_config.mix_server;
        config.timeout_ms = proxy_config.timeout_ms;

        http_client_ = std::make_unique<HttpClient>(config);
    }
}

MetadataRequestHandler::~MetadataRequestHandler() = default;

HTTPParser::HTTPResponse MetadataRequestHandler::HandleRequest(const HTTPParser::HTTPRequest& request) {
    if (HttpClient::IsConnectivityCheck(request.path)) {
        Log("Connectivity check: " + request.method + " " + request.path + " -> returning 200 OK");
        return HttpClient::BuildConnectivityResponse();
    }

    if (request.method != "GET") {
        Log("Warning: Unsupported HTTP method: " + request.method);
        return HTTPParser::BuildErrorResponse(405, "Method not allowed");
    }

    std::string artist_uuid = HTTPParser::ExtractArtistUUID(request.path);
    auto endpoint_type = DetermineEndpointType(request.path);
    std::string resource_id = HTTPParser::ExtractImageUUID(request.path);

    std::string full_url;
    std::string server;
    if (http_client_) {
        std::string host = request.GetHeader("Host");
        server = http_client_->SelectServer(host);
        if (server.empty()) {
            Log("Error: No proxy server configured for host: " + host);
            return HTTPParser::BuildErrorResponse(502, "No proxy server configured");
        }
        full_url = HttpClient::BuildURL(server, request.path, request.query_params);
        Log(std::string(mode_ == InterceptionMode::Proxy ? "Proxy" : "Hybrid") +
            " mode: " + request.method + " " + full_url);
    } else {
        Log("Static mode: " + request.method + " " + request.path);
    }

    switch (mode_) {
        case InterceptionMode::Static:
            return HandleStatic(artist_uuid, endpoint_type, resource_id);
        case InterceptionMode::Proxy:
            return HandleProxy(request, full_url, artist_uuid, endpoint_type, resource_id);
        case InterceptionMode::Hybrid:
            return HandleHybrid(request, full_url, server, artist_uuid, endpoint_type, resource_id);
        default:
            return HTTPParser::BuildErrorResponse(503, "Service not configured");
    }
}

// ── Static Mode ──────────────────────────────────────────────────────────

HTTPParser::HTTPResponse MetadataRequestHandler::HandleStatic(
    const std::string& artist_uuid,
    EndpointType endpoint_type,
    const std::string& resource_id) {

    if (!path_resolver_callback_) {
        Log("Static mode: no path resolver callback registered");
        return HTTPParser::BuildErrorResponse(503, "Path resolver not configured");
    }

    if (artist_uuid.empty() && resource_id.empty()) {
        return HTTPParser::BuildErrorResponse(400, "No artist UUID or resource ID in request");
    }

    auto response = TryServeFromLocal(artist_uuid, endpoint_type, resource_id);
    if (response.status_code != 0) {
        return response;
    }

    Log("Static mode: file not found locally, returning 404");
    return HTTPParser::BuildErrorResponse(404, "Not found");
}

// ── Proxy Mode ───────────────────────────────────────────────────────────

HTTPParser::HTTPResponse MetadataRequestHandler::HandleProxy(
    const HTTPParser::HTTPRequest& request,
    const std::string& full_url,
    const std::string& artist_uuid,
    EndpointType endpoint_type,
    const std::string& resource_id) {

    auto response = http_client_->PerformGET(full_url, request.headers);

    if (cache_storage_callback_ && response.status_code >= 200 && response.status_code < 300
        && (!artist_uuid.empty() || !resource_id.empty())) {
        CacheResponse(artist_uuid, endpoint_type, resource_id, response);
    }

    return response;
}

// ── Hybrid Mode ──────────────────────────────────────────────────────────

HTTPParser::HTTPResponse MetadataRequestHandler::HandleHybrid(
    const HTTPParser::HTTPRequest& request,
    const std::string& full_url,
    const std::string& server,
    const std::string& artist_uuid,
    EndpointType endpoint_type,
    const std::string& resource_id) {

    if (path_resolver_callback_ && (!artist_uuid.empty() || !resource_id.empty())) {
        auto local_response = TryServeFromLocal(artist_uuid, endpoint_type, resource_id);
        if (local_response.status_code != 0) {
            Log("Served from local cache");
            return local_response;
        }
    }

    bool is_image = IsImageEndpoint(endpoint_type);
    bool can_cache = cache_storage_callback_ && (!artist_uuid.empty() || !resource_id.empty());

    if (is_image && can_cache && path_resolver_callback_) {
        // Fetch full resolution for caching, serve device-sized to device
        std::map<std::string, std::string> full_res_params;
        full_res_params["full"] = "true";
        std::string full_res_url = HttpClient::BuildURL(server, request.path, full_res_params);

        Log("Fetching full-resolution for caching: " + full_res_url);
        auto proxy_response = http_client_->PerformGET(full_res_url, request.headers);

        if (proxy_response.status_code >= 200 && proxy_response.status_code < 300) {
            CacheResponse(artist_uuid, endpoint_type, resource_id, proxy_response);

            auto local_response = TryServeFromLocal(artist_uuid, endpoint_type, resource_id);
            if (local_response.status_code != 0) {
                Log("Serving device-sized version after full-res cache");
                return local_response;
            }
        }

        return proxy_response;
    }

    Log("No local file, proxying to server");
    auto proxy_response = http_client_->PerformGET(full_url, request.headers);

    if (can_cache && proxy_response.status_code >= 200 && proxy_response.status_code < 300) {
        CacheResponse(artist_uuid, endpoint_type, resource_id, proxy_response);
    }

    return proxy_response;
}

// ── Shared Implementation ────────────────────────────────────────────────

void MetadataRequestHandler::SetPathResolverCallback(PathResolverCallback callback, void* user_data) {
    path_resolver_callback_ = callback;
    path_resolver_user_data_ = user_data;
}

void MetadataRequestHandler::SetCacheStorageCallback(CacheStorageCallback callback, void* user_data) {
    cache_storage_callback_ = callback;
    cache_storage_user_data_ = user_data;
}

void MetadataRequestHandler::SetLogCallback(LogCallback callback) {
    log_callback_ = callback;
    if (http_client_) {
        http_client_->SetLogCallback(callback);
    }
}

bool MetadataRequestHandler::TestConnection() {
    return http_client_ && http_client_->TestConnection();
}

HTTPParser::HTTPResponse MetadataRequestHandler::TryServeFromLocal(
    const std::string& artist_uuid,
    EndpointType endpoint_type,
    const std::string& resource_id) {

    HTTPParser::HTTPResponse response;
    response.status_code = 0;

    const char* type_str = EndpointTypeToString(endpoint_type);
    const char* file_path = path_resolver_callback_(
        artist_uuid.c_str(),
        type_str,
        resource_id.empty() ? nullptr : resource_id.c_str(),
        path_resolver_user_data_
    );

    if (!file_path) {
        Log("Path resolver returned null (file not found or artist not in DB)");
        return response;
    }

    std::string path_str(file_path);
    free(const_cast<char*>(file_path));
    auto file_data = ReadFile(path_str);
    if (file_data.empty()) {
        Log("File exists in path but couldn't be read: " + path_str);
        return response;
    }

    response.status_code = 200;
    response.status_message = "OK";
    response.body = file_data;

    std::string content_type = GetContentType(path_str);
    if (content_type.find("xml") != std::string::npos) {
        content_type += "; charset=utf-8";
    }

    response.headers["Content-Type"] = content_type;
    response.headers["Content-Length"] = std::to_string(file_data.size());
    response.headers["Connection"] = "keep-alive";
    response.headers["Server"] = "gunicorn";
    response.headers["Date"] = GetCurrentHttpDate();

    if (endpoint_type == EndpointType::DeviceBackgroundImage) {
        size_t last_slash = path_str.find_last_of("/\\");
        std::string filename = (last_slash != std::string::npos) ? path_str.substr(last_slash + 1) : path_str;
        response.headers["Content-Disposition"] = "inline; filename=" + filename;
        response.headers["Last-Modified"] = GetFileModificationDate(path_str);
        response.headers["Cache-Control"] = "no-cache";
        response.headers["ETag"] = GenerateETag(path_str, file_data.size());
    } else {
        response.headers["Cache-Control"] = "max-age=86400";
        response.headers["Access-Control-Allow-Origin"] = "*";
        response.headers["Expires"] = "Sun, 19 Apr 2071 10:00:00 GMT";
    }

    Log("Successfully served from local file: " + path_str);
    return response;
}

void MetadataRequestHandler::CacheResponse(
    const std::string& artist_uuid,
    EndpointType endpoint_type,
    const std::string& resource_id,
    const HTTPParser::HTTPResponse& response) {

    if (!cache_storage_callback_) {
        return;
    }

    std::string content_type = "application/octet-stream";
    for (const auto& [key, value] : response.headers) {
        if (key == "content-type") {
            content_type = value;
            break;
        }
    }

    const char* type_str = EndpointTypeToString(endpoint_type);
    bool cached = cache_storage_callback_(
        artist_uuid.empty() ? nullptr : artist_uuid.c_str(),
        type_str,
        resource_id.empty() ? nullptr : resource_id.c_str(),
        response.body.data(),
        response.body.size(),
        content_type.c_str(),
        cache_storage_user_data_
    );

    std::string identifier = !artist_uuid.empty() ? artist_uuid :
                            (!resource_id.empty() ? "resource:" + resource_id : "unknown");

    if (cached) {
        Log(std::string("Successfully cached ") + type_str + " for " + identifier +
            " (" + std::to_string(response.body.size()) + " bytes)");
    } else {
        Log(std::string("Cache callback returned false for ") + type_str + "/" + identifier);
    }
}

EndpointType MetadataRequestHandler::DetermineEndpointType(const std::string& path) {
    if (path.find("/biography") != std::string::npos)
        return EndpointType::Biography;
    if (path.find("/deviceBackgroundImage") != std::string::npos)
        return EndpointType::DeviceBackgroundImage;
    if (path.find("/similarArtists") != std::string::npos)
        return EndpointType::SimilarArtists;
    if (path.find("/albums") != std::string::npos)
        return EndpointType::Albums;
    if (path.find("/tracks") != std::string::npos)
        return EndpointType::Tracks;
    if (path.find("/images") != std::string::npos)
        return EndpointType::Images;
    if (path.find("/music/artist/") != std::string::npos)
        return EndpointType::Overview;
    if (path.find("/image/") != std::string::npos)
        return EndpointType::Artwork;
    return EndpointType::Unknown;
}

mtp::ByteArray MetadataRequestHandler::ReadFile(const std::string& file_path) {
    std::ifstream file(file_path, std::ios::binary | std::ios::ate);
    if (!file.is_open()) {
        return mtp::ByteArray();
    }

    std::streamsize size = file.tellg();
    const std::streamsize MAX_FILE_SIZE = 10 * 1024 * 1024;  // 10MB limit
    if (size < 0 || size > MAX_FILE_SIZE) {
        Log("File too large or invalid size: " + file_path);
        return mtp::ByteArray();
    }

    file.seekg(0, std::ios::beg);
    mtp::ByteArray buffer(size);
    if (!file.read(reinterpret_cast<char*>(buffer.data()), size)) {
        return mtp::ByteArray();
    }

    return buffer;
}

std::string MetadataRequestHandler::GetCurrentHttpDate() {
    auto now = std::chrono::system_clock::now();
    auto now_time_t = std::chrono::system_clock::to_time_t(now);

    struct tm tm_buf;
    gmtime_r(&now_time_t, &tm_buf);

    char buffer[100];
    strftime(buffer, sizeof(buffer), "%a, %d %b %Y %H:%M:%S GMT", &tm_buf);
    return std::string(buffer);
}

std::string MetadataRequestHandler::GetFileModificationDate(const std::string& file_path) {
    struct stat file_stat;
    if (stat(file_path.c_str(), &file_stat) != 0) {
        return GetCurrentHttpDate();
    }

    struct tm tm_buf;
    gmtime_r(&file_stat.st_mtime, &tm_buf);

    char buffer[100];
    strftime(buffer, sizeof(buffer), "%a, %d %b %Y %H:%M:%S GMT", &tm_buf);
    return std::string(buffer);
}

std::string MetadataRequestHandler::GenerateETag(const std::string& file_path, size_t file_size) {
    struct stat file_stat;
    if (stat(file_path.c_str(), &file_stat) != 0) {
        return "\"" + std::to_string(file_size) + "\"";
    }

    double mtime = static_cast<double>(file_stat.st_mtime);
    std::hash<std::string> hasher;
    size_t path_hash = hasher(file_path);

    std::ostringstream etag;
    etag << "\"" << std::fixed << mtime << "-" << file_size << "-" << path_hash << "\"";
    return etag.str();
}

std::string MetadataRequestHandler::GetContentType(const std::string& file_path) {
    size_t dot_pos = file_path.find_last_of('.');
    if (dot_pos == std::string::npos) {
        return "application/octet-stream";
    }

    std::string ext = file_path.substr(dot_pos);
    for (char& c : ext) {
        c = std::tolower(c);
    }

    if (ext == ".xml") return "application/xml";
    if (ext == ".jpg" || ext == ".jpeg") return "image/jpeg";
    if (ext == ".png") return "image/png";
    if (ext == ".gif") return "image/gif";
    if (ext == ".webp") return "image/webp";

    return "application/octet-stream";
}

void MetadataRequestHandler::Log(const std::string& message) {
    if (log_callback_) {
        log_callback_("[MetadataRequestHandler] " + message);
    }
}
