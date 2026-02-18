#include "HybridModeHandler.h"
#include "HttpClient.h"
#include "HTTPParser.h"
#include <fstream>
#include <sstream>
#include <algorithm>
#include <chrono>
#include <ctime>
#include <sys/stat.h>
#include <functional>

HybridModeHandler::HybridModeHandler(
    const std::string& proxy_catalog_server,
    const std::string& proxy_image_server,
    const std::string& proxy_art_server,
    const std::string& proxy_mix_server,
    int proxy_timeout_ms)
    : proxy_catalog_server_(proxy_catalog_server)
    , proxy_image_server_(proxy_image_server.empty() ? proxy_catalog_server : proxy_image_server)
    , proxy_art_server_(proxy_art_server.empty() ? proxy_catalog_server : proxy_art_server)
    , proxy_mix_server_(proxy_mix_server.empty() ? proxy_catalog_server : proxy_mix_server)
    , proxy_timeout_ms_(proxy_timeout_ms)
{
    // Create HttpClient with server configuration
    HttpClient::ServerConfig config;
    config.catalog_server = proxy_catalog_server_;
    config.image_server = proxy_image_server_;
    config.art_server = proxy_art_server_;
    config.mix_server = proxy_mix_server_;
    config.timeout_ms = proxy_timeout_ms_;

    http_client_ = std::make_unique<HttpClient>(config);
}

HybridModeHandler::~HybridModeHandler() = default;

HTTPParser::HTTPResponse HybridModeHandler::HandleRequest(const HTTPParser::HTTPRequest& request) {
    // Handle Microsoft connectivity check endpoint
    if (HttpClient::IsConnectivityCheck(request.path)) {
        Log("Connectivity check: " + request.method + " " + request.path + " -> returning 200 OK");
        return HttpClient::BuildConnectivityResponse();
    }

    std::string host = request.GetHeader("Host");
    std::string server = http_client_->SelectServer(host);

    if (server.empty()) {
        Log("Error: No proxy server configured for host: " + host);
        return HTTPParser::BuildErrorResponse(502, "No proxy server configured");
    }

    std::string full_url = HttpClient::BuildURL(server, request.path, request.query_params);

    Log("Hybrid mode: " + request.method + " " + full_url);

    if (request.method != "GET") {
        Log("Warning: Unsupported HTTP method: " + request.method);
        return HTTPParser::BuildErrorResponse(405, "Method not allowed");
    }

    // Extract artist UUID and endpoint information using HTTPParser
    std::string artist_uuid = HTTPParser::ExtractArtistUUID(request.path);
    std::string endpoint_type = DetermineEndpointType(request.path);  // Keep local for C# compat
    std::string resource_id = HTTPParser::ExtractImageUUID(request.path);

    // Step 1: Try to serve from local cache (if callback is set)
    // Call PathResolver if we have artist UUID OR if we have resource ID (for image-by-ID lookups)
    if (path_resolver_callback_ && (!artist_uuid.empty() || !resource_id.empty())) {
        auto local_response = TryServeFromLocal(artist_uuid, endpoint_type, resource_id);
        if (local_response.status_code != 0) {
            Log("Served from local cache");
            return local_response;
        }
    }

    // Step 2: Local file not found or callback not set -> proxy to server
    Log("No local file, proxying to server");
    auto proxy_response = http_client_->PerformGET(full_url, request.headers);

    // Step 3: Cache the proxy response (if callback is set and response is successful)
    if (cache_storage_callback_ &&
        (!artist_uuid.empty() || !resource_id.empty()) &&
        proxy_response.status_code >= 200 && proxy_response.status_code < 300) {
        CacheResponse(artist_uuid, endpoint_type, resource_id, proxy_response);
    }

    return proxy_response;
}

void HybridModeHandler::SetPathResolverCallback(PathResolverCallback callback, void* user_data) {
    path_resolver_callback_ = callback;
    path_resolver_user_data_ = user_data;
}

void HybridModeHandler::SetCacheStorageCallback(CacheStorageCallback callback, void* user_data) {
    cache_storage_callback_ = callback;
    cache_storage_user_data_ = user_data;
}

void HybridModeHandler::SetLogCallback(LogCallback callback) {
    log_callback_ = callback;
    if (http_client_) {
        http_client_->SetLogCallback(callback);
    }
}

bool HybridModeHandler::TestConnection() {
    return http_client_ && http_client_->TestConnection();
}

HTTPParser::HTTPResponse HybridModeHandler::TryServeFromLocal(
    const std::string& artist_uuid,
    const std::string& endpoint_type,
    const std::string& resource_id) {

    HTTPParser::HTTPResponse response;
    response.status_code = 0;  // Indicates "not found locally"

    const char* file_path = path_resolver_callback_(
        artist_uuid.c_str(),
        endpoint_type.c_str(),
        resource_id.empty() ? nullptr : resource_id.c_str(),
        path_resolver_user_data_
    );

    if (!file_path) {
        Log("Path resolver returned null (file not found or artist not in DB)");
        return response;
    }

    std::string path_str(file_path);
    auto file_data = ReadFile(path_str);
    if (file_data.empty()) {
        Log("File exists in path but couldn't be read: " + path_str);
        return response;
    }

    // Build successful response
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

    if (endpoint_type == "devicebackgroundimage") {
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

void HybridModeHandler::CacheResponse(
    const std::string& artist_uuid,
    const std::string& endpoint_type,
    const std::string& resource_id,
    const HTTPParser::HTTPResponse& response) {

    if (!cache_storage_callback_) {
        return;
    }

    std::string content_type = "application/octet-stream";
    for (const auto& [key, value] : response.headers) {
        if (key == "Content-Type" || key == "content-type" || key == "Content-type") {
            content_type = value;
            break;
        }
    }

    bool cached = cache_storage_callback_(
        artist_uuid.empty() ? nullptr : artist_uuid.c_str(),
        endpoint_type.c_str(),
        resource_id.empty() ? nullptr : resource_id.c_str(),
        response.body.data(),
        response.body.size(),
        content_type.c_str(),
        cache_storage_user_data_
    );

    std::string identifier = !artist_uuid.empty() ? artist_uuid :
                            (!resource_id.empty() ? "resource:" + resource_id : "unknown");

    if (cached) {
        Log("Successfully cached " + endpoint_type + " for " + identifier +
            " (" + std::to_string(response.body.size()) + " bytes)");
    } else {
        Log("Cache callback returned false for " + endpoint_type + "/" + identifier);
    }
}

std::string HybridModeHandler::DetermineEndpointType(const std::string& path) {
    // Check specific sub-endpoints before the generic /music/artist/ match
    if (path.find("/biography") != std::string::npos) {
        return "biography";
    } else if (path.find("/deviceBackgroundImage") != std::string::npos) {
        return "devicebackgroundimage";
    } else if (path.find("/similarArtists") != std::string::npos) {
        return "similarartists";
    } else if (path.find("/albums") != std::string::npos) {
        return "albums";
    } else if (path.find("/tracks") != std::string::npos) {
        return "tracks";
    } else if (path.find("/images") != std::string::npos) {
        return "images";
    } else if (path.find("/music/artist/") != std::string::npos) {
        return "overview";
    } else if (path.find("/image/") != std::string::npos) {
        return "artwork";
    }
    return "unknown";
}

mtp::ByteArray HybridModeHandler::ReadFile(const std::string& file_path) {
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

std::string HybridModeHandler::GetCurrentHttpDate() {
    auto now = std::chrono::system_clock::now();
    auto now_time_t = std::chrono::system_clock::to_time_t(now);

    struct tm tm_buf;
    gmtime_r(&now_time_t, &tm_buf);

    char buffer[100];
    strftime(buffer, sizeof(buffer), "%a, %d %b %Y %H:%M:%S GMT", &tm_buf);
    return std::string(buffer);
}

std::string HybridModeHandler::GetFileModificationDate(const std::string& file_path) {
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

std::string HybridModeHandler::GenerateETag(const std::string& file_path, size_t file_size) {
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

std::string HybridModeHandler::GetContentType(const std::string& file_path) {
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

void HybridModeHandler::Log(const std::string& message) {
    if (log_callback_) {
        log_callback_("[HybridModeHandler] " + message);
    }
}
