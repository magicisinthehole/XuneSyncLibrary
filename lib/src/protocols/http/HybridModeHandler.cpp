#include "HybridModeHandler.h"
#include "HTTPParser.h"
#include <fstream>
#include <sstream>
#include <regex>
#include <algorithm>
#include <curl/curl.h>
#include <cstring>
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
    // Initialize libcurl handle (reused for all requests) - matches ProxyModeHandler error checking
    curl_handle_ = curl_easy_init();
    if (!curl_handle_) {
        throw std::runtime_error("Failed to initialize libcurl");
    }
}

HybridModeHandler::~HybridModeHandler() {
    if (curl_handle_) {
        curl_easy_cleanup(static_cast<CURL*>(curl_handle_));
    }
}

HTTPParser::HTTPResponse HybridModeHandler::HandleRequest(const HTTPParser::HTTPRequest& request)
{
    // Handle Microsoft connectivity check endpoint (matches ProxyModeHandler)
    // Device uses /fwlink/ to verify internet connectivity before proceeding to catalog
    if (request.path.find("/fwlink") == 0) {
        Log("Connectivity check: " + request.method + " " + request.path + " -> returning 200 OK");

        HTTPParser::HTTPResponse response;
        response.status_code = 200;
        response.status_message = "OK";
        response.SetContentType("text/html");

        // Return minimal HTML response to satisfy connectivity check
        std::string body = "<html><body>OK</body></html>";
        response.body.assign(body.begin(), body.end());
        response.SetContentLength(response.body.size());

        return response;
    }

    std::string host = request.GetHeader("Host");
    std::string server = SelectServer(host);

    if (server.empty()) {
        Log("Error: No proxy server configured for host: " + host);
        return HTTPParser::BuildErrorResponse(502, "No proxy server configured");
    }

    // Build full URL with query parameters (matches ProxyModeHandler)
    std::string full_url = BuildURL(server, request.path);

    // Append query parameters if present (matches ProxyModeHandler)
    if (!request.query_params.empty()) {
        full_url += "?";
        bool first = true;
        for (const auto& param : request.query_params) {
            if (!first) full_url += "&";
            full_url += param.first + "=" + param.second;
            first = false;
        }
    }

    Log("Hybrid mode: " + request.method + " " + full_url);

    // Validate HTTP method (matches ProxyModeHandler)
    if (request.method != "GET") {
        Log("Warning: Unsupported HTTP method: " + request.method);
        return HTTPParser::BuildErrorResponse(405, "Method not allowed");
    }

    // Extract artist UUID and endpoint information for hybrid mode logic
    std::string artist_uuid = ExtractArtistUUID(request.path);
    std::string endpoint_type = DetermineEndpointType(request.path);
    std::string resource_id = ExtractResourceId(request.path);

    // Step 1: Try to serve from local cache (if callback is set)
    if (path_resolver_callback_ && !artist_uuid.empty()) {
        auto local_response = TryServeFromLocal(artist_uuid, endpoint_type, resource_id);
        if (local_response.status_code != 0) {
            Log("Served from local cache");
            return local_response;
        }
    }

    // Step 2: Local file not found or callback not set -> proxy to server
    Log("No local file, proxying to server");
    auto proxy_response = ProxyRequest(full_url, request.headers);

    // Step 3: Cache the proxy response (if callback is set and response is successful)
    // Allow caching even without artist UUID - C# can resolve artist from image/resource ID
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
}

bool HybridModeHandler::TestConnection() {
    std::lock_guard<std::mutex> lock(curl_mutex_);

    if (proxy_catalog_server_.empty()) {
        return false;
    }

    if (!curl_handle_) {
        return false;
    }

    try {
        std::string test_url = proxy_catalog_server_ + "/";

        CURL* curl = static_cast<CURL*>(curl_handle_);
        curl_easy_setopt(curl, CURLOPT_URL, test_url.c_str());
        curl_easy_setopt(curl, CURLOPT_NOBODY, 1L);  // HEAD request

        // Use 30 second timeout for connection test (matches ProxyModeHandler)
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
        curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);

        CURLcode res = curl_easy_perform(curl);

        // CRITICAL: Reset NOBODY option after test (matches ProxyModeHandler)
        // Without this, all subsequent requests would be HEAD requests!
        curl_easy_setopt(curl, CURLOPT_NOBODY, 0L);

        return res == CURLE_OK;
    } catch (...) {
        return false;
    }
}

HTTPParser::HTTPResponse HybridModeHandler::TryServeFromLocal(
    const std::string& artist_uuid,
    const std::string& endpoint_type,
    const std::string& resource_id)
{
    HTTPParser::HTTPResponse response;
    response.status_code = 0;  // Indicates "not found locally"

    // Call C# path resolver callback
    // MEMORY CONTRACT: C# must keep the returned string valid until this callback returns.
    // C++ will copy the string immediately. C# manages its own memory lifecycle.
    const char* file_path = path_resolver_callback_(
        artist_uuid.c_str(),
        endpoint_type.c_str(),
        resource_id.empty() ? nullptr : resource_id.c_str(),
        path_resolver_user_data_
    );

    if (!file_path) {
        Log("HybridModeHandler: Path resolver returned null (file not found or artist not in DB)");
        return response;
    }

    // Copy the path string immediately - C# retains ownership of the original
    std::string path_str(file_path);
    // NOTE: Do NOT free file_path here - C# manages its own memory

    auto file_data = ReadFile(path_str);
    if (file_data.empty()) {
        Log("HybridModeHandler: File exists in path but couldn't be read: " + path_str);
        return response;
    }

    // Build successful response - match real Zune catalog server headers exactly
    response.status_code = 200;
    response.status_message = "OK";
    response.body = file_data;

    // Get base content type
    std::string content_type = GetContentType(path_str);

    // Add charset for XML files to match real server (application/xml; charset=utf-8)
    if (content_type.find("xml") != std::string::npos) {
        content_type += "; charset=utf-8";
    }

    response.headers["Content-Type"] = content_type;
    response.headers["Content-Length"] = std::to_string(file_data.size());
    response.headers["Connection"] = "keep-alive";

    // Match additional headers from real Zune catalog server
    // These are critical for device compatibility
    response.headers["Server"] = "gunicorn";
    response.headers["Date"] = GetCurrentHttpDate();

    // Device background images have different headers than other endpoints
    if (endpoint_type == "devicebackgroundimage") {
        // Extract filename from path
        size_t last_slash = path_str.find_last_of("/\\");
        std::string filename = (last_slash != std::string::npos) ? path_str.substr(last_slash + 1) : path_str;

        response.headers["Content-Disposition"] = "inline; filename=" + filename;
        response.headers["Last-Modified"] = GetFileModificationDate(path_str);
        response.headers["Cache-Control"] = "no-cache";
        response.headers["ETag"] = GenerateETag(path_str, file_data.size());
    } else {
        // All other endpoints (biography, images, overview, artwork)
        response.headers["Cache-Control"] = "max-age=86400";
        response.headers["Access-Control-Allow-Origin"] = "*";
        response.headers["Expires"] = "Sun, 19 Apr 2071 10:00:00 GMT";
    }

    Log("HybridModeHandler: Successfully served from local file: " + path_str);
    Log("HybridModeHandler: Response Content-Type: " + content_type);
    return response;
}

// libcurl write callback
static size_t WriteCallback(void* contents, size_t size, size_t nmemb, void* userp) {
    size_t total_size = size * nmemb;
    auto* data = static_cast<mtp::ByteArray*>(userp);
    const char* bytes = static_cast<const char*>(contents);
    data->insert(data->end(), bytes, bytes + total_size);
    return total_size;
}

// libcurl header callback
static size_t HeaderCallback(char* buffer, size_t size, size_t nitems, void* userdata) {
    size_t total_size = size * nitems;
    auto* headers = static_cast<std::map<std::string, std::string>*>(userdata);

    std::string header_line(buffer, total_size);

    // Trim trailing \r\n
    while (!header_line.empty() && (header_line.back() == '\r' || header_line.back() == '\n')) {
        header_line.pop_back();
    }

    if (header_line.empty()) {
        return total_size;
    }

    // Parse "Header: Value"
    size_t colon_pos = header_line.find(':');
    if (colon_pos != std::string::npos) {
        std::string key = header_line.substr(0, colon_pos);
        std::string value = header_line.substr(colon_pos + 1);

        // Trim leading whitespace from value
        size_t first = value.find_first_not_of(" \t");
        if (first != std::string::npos) {
            value = value.substr(first);
        }

        (*headers)[key] = value;
    }

    return total_size;
}

HTTPParser::HTTPResponse HybridModeHandler::ProxyRequest(
    const std::string& url,
    const std::map<std::string, std::string>& headers)
{
    // Lock curl handle for thread safety (one request at a time)
    std::lock_guard<std::mutex> lock(curl_mutex_);

    if (!curl_handle_) {
        Log("libcurl error: Failed initialization");
        return HTTPParser::BuildErrorResponse(502, "libcurl initialization failed");
    }

    CURL* curl = static_cast<CURL*>(curl_handle_);

    mtp::ByteArray response_data;
    std::map<std::string, std::string> response_headers;
    long http_code = 0;

    // Configure CURL (matches ProxyModeHandler)
    curl_easy_reset(curl);
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response_data);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, HeaderCallback);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, &response_headers);

    // Set timeout (30 seconds minimum for image fetches - matches ProxyModeHandler)
    long timeout_seconds = proxy_timeout_ms_ / 1000;
    if (timeout_seconds < 1) timeout_seconds = 30;
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, timeout_seconds);

    // Forward request headers (matches ProxyModeHandler)
    struct curl_slist* header_list = nullptr;
    for (const auto& [name, value] : headers) {
        // Skip Host header (libcurl sets it automatically)
        if (name == "Host" || name == "host") continue;

        std::string header_str = name + ": " + value;
        header_list = curl_slist_append(header_list, header_str.c_str());
    }

    if (header_list) {
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, header_list);
    }

    // Follow redirects
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);

    // Perform request
    CURLcode res = curl_easy_perform(curl);

    // Clean up headers
    if (header_list) {
        curl_slist_free_all(header_list);
    }

    // Get HTTP response code
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);

    if (res != CURLE_OK) {
        std::string error_msg = curl_easy_strerror(res);
        Log("libcurl error: " + error_msg);
        return HTTPParser::BuildErrorResponse(502, "Proxy request failed: " + error_msg);
    }

    Log("Proxy response: " + std::to_string(http_code) + " (" +
        std::to_string(response_data.size()) + " bytes)");

    return ParseResponse(response_data, http_code, response_headers);
}

void HybridModeHandler::CacheResponse(
    const std::string& artist_uuid,
    const std::string& endpoint_type,
    const std::string& resource_id,
    const HTTPParser::HTTPResponse& response)
{
    if (!cache_storage_callback_) {
        return;
    }

    // Get content type from response headers
    std::string content_type = "application/octet-stream";
    auto it = response.headers.find("Content-Type");
    if (it != response.headers.end()) {
        content_type = it->second;
    }

    // Call C# cache storage callback (allow null for artist_uuid and resource_id)
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
        Log("Cache callback returned false for " + endpoint_type + "/" + identifier +
            " (not in DB or C# error)");
    }
}

std::string HybridModeHandler::ExtractArtistUUID(const std::string& path) {
    // Pattern: /v3.0/*/music/artist/{uuid}/...
    std::regex uuid_pattern(R"(/music/artist/([0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{12}))");
    std::smatch match;

    if (std::regex_search(path, match, uuid_pattern) && match.size() > 1) {
        return match[1].str();
    }

    return "";
}

std::string HybridModeHandler::DetermineEndpointType(const std::string& path) {
    if (path.find("/biography") != std::string::npos) {
        return "biography";
    } else if (path.find("/images") != std::string::npos) {
        return "images";
    } else if (path.find("/deviceBackgroundImage") != std::string::npos) {
        return "devicebackgroundimage";
    } else if (path.find("/music/artist/") != std::string::npos) {
        return "overview";
    } else if (path.find("/image/") != std::string::npos) {
        return "artwork";
    }

    return "unknown";
}

std::string HybridModeHandler::ExtractResourceId(const std::string& path) {
    // For image requests: /v3.0/*/image/{uuid}
    std::regex image_pattern(R"(/image/([0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{12}))");
    std::smatch match;

    if (std::regex_search(path, match, image_pattern) && match.size() > 1) {
        return match[1].str();
    }

    return "";
}

mtp::ByteArray HybridModeHandler::ReadFile(const std::string& file_path) {
    std::ifstream file(file_path, std::ios::binary | std::ios::ate);
    if (!file.is_open()) {
        return mtp::ByteArray();
    }

    std::streamsize size = file.tellg();

    // Limit file size to 10MB to prevent memory exhaustion
    const std::streamsize MAX_FILE_SIZE = 10 * 1024 * 1024;
    if (size < 0 || size > MAX_FILE_SIZE) {
        Log("HybridModeHandler: File too large or invalid size: " + file_path +
            " (size: " + std::to_string(size) + " bytes)");
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
    // Get current time
    auto now = std::chrono::system_clock::now();
    auto now_time_t = std::chrono::system_clock::to_time_t(now);

    // Format as RFC 1123 date (required by HTTP/1.1)
    // Example: "Thu, 20 Nov 2025 10:28:15 GMT"
    struct tm tm_buf;
    gmtime_r(&now_time_t, &tm_buf);

    char buffer[100];
    strftime(buffer, sizeof(buffer), "%a, %d %b %Y %H:%M:%S GMT", &tm_buf);

    return std::string(buffer);
}

std::string HybridModeHandler::GetFileModificationDate(const std::string& file_path) {
    // Get file modification time using stat()
    struct stat file_stat;
    if (stat(file_path.c_str(), &file_stat) != 0) {
        // If stat fails, return current time as fallback
        return GetCurrentHttpDate();
    }

    // Convert modification time to RFC 1123 format
    struct tm tm_buf;
    gmtime_r(&file_stat.st_mtime, &tm_buf);

    char buffer[100];
    strftime(buffer, sizeof(buffer), "%a, %d %b %Y %H:%M:%S GMT", &tm_buf);

    return std::string(buffer);
}

std::string HybridModeHandler::GenerateETag(const std::string& file_path, size_t file_size) {
    // Generate ETag based on file modification time and size
    // Format matches real server: "timestamp.subsec-size-hash"
    struct stat file_stat;
    if (stat(file_path.c_str(), &file_stat) != 0) {
        // If stat fails, generate simple ETag from size
        return "\"" + std::to_string(file_size) + "\"";
    }

    // Get modification time with subsecond precision
    double mtime = static_cast<double>(file_stat.st_mtime);

    // Generate a simple hash from the file path (for uniqueness)
    std::hash<std::string> hasher;
    size_t path_hash = hasher(file_path);

    // Format: "mtime.subsec-size-hash"
    std::ostringstream etag;
    etag << "\"" << std::fixed << mtime << "-" << file_size << "-" << path_hash << "\"";

    return etag.str();
}

std::string HybridModeHandler::GetContentType(const std::string& file_path) {
    // Get file extension
    size_t dot_pos = file_path.find_last_of('.');
    if (dot_pos == std::string::npos) {
        return "application/octet-stream";
    }

    std::string ext = file_path.substr(dot_pos);

    // Convert to lowercase
    for (char& c : ext) {
        c = std::tolower(c);
    }

    // Map extension to content type (matching real Zune catalog server)
    if (ext == ".xml") {
        return "application/xml";  // Match real server (was "text/xml")
    } else if (ext == ".jpg" || ext == ".jpeg") {
        return "image/jpeg";
    } else if (ext == ".png") {
        return "image/png";
    } else if (ext == ".gif") {
        return "image/gif";
    } else if (ext == ".webp") {
        return "image/webp";
    }

    return "application/octet-stream";
}

std::string HybridModeHandler::SelectServer(const std::string& host) {
    // Convert to lowercase for case-insensitive matching
    std::string lower_host = host;
    std::transform(lower_host.begin(), lower_host.end(), lower_host.begin(), ::tolower);

    // Match host to appropriate server (same logic as ProxyModeHandler)
    if (lower_host.find("image.catalog.zune.net") != std::string::npos) {
        return proxy_image_server_;
    }
    else if (lower_host.find("catalog.zune.net") != std::string::npos) {
        return proxy_catalog_server_;
    }
    else if (lower_host.find("art.zune.net") != std::string::npos) {
        return proxy_art_server_;
    }
    else if (lower_host.find("mix.zune.net") != std::string::npos) {
        return proxy_mix_server_;
    }
    else {
        // Default to catalog server for unknown hosts
        return proxy_catalog_server_;
    }
}

std::string HybridModeHandler::BuildURL(const std::string& server, const std::string& path) {
    // Remove trailing slash from server if present (matches ProxyModeHandler)
    std::string base = server;
    if (!base.empty() && base.back() == '/') {
        base.pop_back();
    }

    // Ensure path starts with / (matches ProxyModeHandler)
    std::string full_path = path;
    if (full_path.empty() || full_path[0] != '/') {
        full_path = "/" + full_path;
    }

    return base + full_path;
}

HTTPParser::HTTPResponse HybridModeHandler::ParseResponse(
    const mtp::ByteArray& response_data,
    int status_code,
    std::map<std::string, std::string> headers)
{
    // Matches ProxyModeHandler::ParseResponse logic exactly

    HTTPParser::HTTPResponse response;
    response.status_code = status_code;
    response.status_message = HTTPParser::GetStatusMessage(status_code);
    response.body = response_data;

    // Remove problematic headers that libcurl already processed
    headers.erase("Transfer-Encoding");
    headers.erase("transfer-encoding");
    headers.erase("Content-Length");
    headers.erase("content-length");

    response.headers = headers;

    // Ensure Content-Length matches actual body size (libcurl decoded chunked data)
    response.SetContentLength(response_data.size());

    // If Content-Type not provided by upstream, detect it from content
    if (headers.find("Content-Type") == headers.end() &&
        headers.find("content-type") == headers.end()) {
        if (response_data.size() > 4) {
            // Check for JPEG magic bytes (0xFF 0xD8)
            if (response_data[0] == 0xFF && response_data[1] == 0xD8) {
                response.SetContentType("image/jpeg");
            }
            // Check for XML/HTML
            else if (response_data[0] == '<' ||
                     (response_data[0] == 0xEF && response_data[1] == 0xBB && response_data[2] == 0xBF)) {
                // XML or HTML (possibly with UTF-8 BOM)
                std::string content_start(response_data.begin(),
                                         response_data.begin() + std::min(size_t(100), response_data.size()));
                if (content_start.find("<?xml") != std::string::npos) {
                    response.SetContentType("application/xml");
                } else {
                    response.SetContentType("text/html");
                }
            }
            else {
                response.SetContentType("application/octet-stream");
            }
        }
    }

    return response;
}

void HybridModeHandler::Log(const std::string& message) {
    if (log_callback_) {
        log_callback_("[HybridModeHandler] " + message);
    }
}
