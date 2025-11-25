#include "HttpClient.h"
#include <curl/curl.h>
#include <algorithm>
#include <cctype>

// libcurl write callback - accumulates response body
static size_t WriteCallback(void* contents, size_t size, size_t nmemb, void* userp) {
    size_t total_size = size * nmemb;
    auto* buffer = static_cast<mtp::ByteArray*>(userp);
    const auto* bytes = static_cast<const uint8_t*>(contents);
    buffer->insert(buffer->end(), bytes, bytes + total_size);
    return total_size;
}

// libcurl header callback - captures response headers
static size_t HeaderCallback(char* buffer, size_t size, size_t nitems, void* userp) {
    size_t total_size = size * nitems;
    auto* headers = static_cast<std::map<std::string, std::string>*>(userp);

    std::string header_line(buffer, total_size);

    // Remove trailing \r\n
    while (!header_line.empty() &&
           (header_line.back() == '\r' || header_line.back() == '\n')) {
        header_line.pop_back();
    }

    if (header_line.empty()) {
        return total_size;
    }

    // Parse "Key: Value" format
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

HttpClient::HttpClient(const ServerConfig& config)
    : config_(config) {

    curl_handle_ = curl_easy_init();
    if (!curl_handle_) {
        throw std::runtime_error("Failed to initialize libcurl");
    }
}

HttpClient::~HttpClient() {
    if (curl_handle_) {
        curl_easy_cleanup(static_cast<CURL*>(curl_handle_));
    }
}

HTTPParser::HTTPResponse HttpClient::PerformGET(
    const std::string& url,
    const std::map<std::string, std::string>& headers) {

    std::lock_guard<std::mutex> lock(curl_mutex_);

    if (!curl_handle_) {
        Log("libcurl error: Handle not initialized");
        return HTTPParser::BuildErrorResponse(502, "libcurl initialization failed");
    }

    CURL* curl = static_cast<CURL*>(curl_handle_);

    mtp::ByteArray response_data;
    std::map<std::string, std::string> response_headers;
    long http_code = 0;

    // Reset and configure curl
    curl_easy_reset(curl);
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response_data);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, HeaderCallback);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, &response_headers);

    // Set timeout (minimum 30 seconds for image fetches)
    long timeout_seconds = config_.timeout_ms / 1000;
    if (timeout_seconds < 1) timeout_seconds = 30;
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, timeout_seconds);

    // Forward request headers (skip Host - curl sets automatically)
    struct curl_slist* header_list = nullptr;
    for (const auto& [name, value] : headers) {
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

    Log("HTTP " + std::to_string(http_code) + " (" +
        std::to_string(response_data.size()) + " bytes) from " + url);

    return ParseResponse(response_data, http_code, response_headers);
}

std::string HttpClient::SelectServer(const std::string& host) const {
    // Convert to lowercase for case-insensitive matching
    std::string lower_host = host;
    std::transform(lower_host.begin(), lower_host.end(), lower_host.begin(),
                   [](unsigned char c) { return std::tolower(c); });

    if (lower_host.find("image.catalog.zune.net") != std::string::npos) {
        return !config_.image_server.empty() ? config_.image_server : config_.catalog_server;
    }
    else if (lower_host.find("catalog.zune.net") != std::string::npos) {
        return config_.catalog_server;
    }
    else if (lower_host.find("art.zune.net") != std::string::npos) {
        return !config_.art_server.empty() ? config_.art_server : config_.catalog_server;
    }
    else if (lower_host.find("mix.zune.net") != std::string::npos) {
        return !config_.mix_server.empty() ? config_.mix_server : config_.catalog_server;
    }

    // Default to catalog server
    return config_.catalog_server;
}

std::string HttpClient::BuildURL(
    const std::string& server,
    const std::string& path,
    const std::map<std::string, std::string>& query_params) {

    // Remove trailing slash from server
    std::string base = server;
    if (!base.empty() && base.back() == '/') {
        base.pop_back();
    }

    // Ensure path starts with /
    std::string full_path = path;
    if (full_path.empty() || full_path[0] != '/') {
        full_path = "/" + full_path;
    }

    std::string url = base + full_path;

    // Append query parameters
    if (!query_params.empty()) {
        url += "?";
        bool first = true;
        for (const auto& [key, value] : query_params) {
            if (!first) url += "&";
            url += key + "=" + value;
            first = false;
        }
    }

    return url;
}

bool HttpClient::IsConnectivityCheck(const std::string& path) {
    return path.find("/fwlink") == 0;
}

HTTPParser::HTTPResponse HttpClient::BuildConnectivityResponse() {
    HTTPParser::HTTPResponse response;
    response.status_code = 200;
    response.status_message = "OK";
    response.SetContentType("text/html");

    std::string body = "<html><body>OK</body></html>";
    response.body.assign(body.begin(), body.end());
    response.SetContentLength(response.body.size());

    return response;
}

bool HttpClient::TestConnection() {
    std::lock_guard<std::mutex> lock(curl_mutex_);

    if (config_.catalog_server.empty() || !curl_handle_) {
        return false;
    }

    try {
        std::string test_url = config_.catalog_server + "/";

        CURL* curl = static_cast<CURL*>(curl_handle_);
        curl_easy_reset(curl);
        curl_easy_setopt(curl, CURLOPT_URL, test_url.c_str());
        curl_easy_setopt(curl, CURLOPT_NOBODY, 1L);  // HEAD request
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
        curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);

        CURLcode res = curl_easy_perform(curl);

        return res == CURLE_OK;
    } catch (...) {
        return false;
    }
}

void HttpClient::SetLogCallback(LogCallback callback) {
    log_callback_ = callback;
}

HTTPParser::HTTPResponse HttpClient::ParseResponse(
    const mtp::ByteArray& response_data,
    int status_code,
    std::map<std::string, std::string> headers) {

    HTTPParser::HTTPResponse response;
    response.status_code = status_code;
    response.status_message = HTTPParser::GetStatusMessage(status_code);
    response.body = response_data;

    // Remove headers that libcurl already processed
    headers.erase("Transfer-Encoding");
    headers.erase("transfer-encoding");
    headers.erase("Content-Length");
    headers.erase("content-length");

    response.headers = headers;

    // Set correct Content-Length (libcurl decoded chunked data)
    response.SetContentLength(response_data.size());

    // Detect content type if not provided
    if (headers.find("Content-Type") == headers.end() &&
        headers.find("content-type") == headers.end()) {
        std::string detected_type = DetectContentType(response_data);
        if (!detected_type.empty()) {
            response.SetContentType(detected_type);
        }
    }

    return response;
}

std::string HttpClient::DetectContentType(const mtp::ByteArray& data) {
    if (data.size() < 4) {
        return "application/octet-stream";
    }

    // Check for JPEG magic bytes (0xFF 0xD8)
    if (data[0] == 0xFF && data[1] == 0xD8) {
        return "image/jpeg";
    }

    // Check for PNG magic bytes
    if (data[0] == 0x89 && data[1] == 'P' && data[2] == 'N' && data[3] == 'G') {
        return "image/png";
    }

    // Check for XML/HTML (possibly with UTF-8 BOM)
    bool has_bom = (data[0] == 0xEF && data[1] == 0xBB && data[2] == 0xBF);
    size_t start = has_bom ? 3 : 0;

    if (start < data.size() && data[start] == '<') {
        std::string content_start(data.begin() + start,
                                  data.begin() + std::min(data.size(), start + 100));
        if (content_start.find("<?xml") != std::string::npos) {
            return "application/xml";
        }
        return "text/html";
    }

    return "application/octet-stream";
}

HTTPParser::HTTPResponse HttpClient::FetchExternal(const std::string& url, int timeout_seconds) {
    CURL* curl = curl_easy_init();
    if (!curl) {
        return HTTPParser::BuildErrorResponse(502, "Failed to initialize curl");
    }

    mtp::ByteArray response_data;
    std::map<std::string, std::string> response_headers;
    long http_code = 0;

    // Configure curl
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response_data);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, HeaderCallback);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, &response_headers);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, static_cast<long>(timeout_seconds));
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "Mozilla/4.0 (compatible; ZuneHD 4.5)");

    // Perform request
    CURLcode res = curl_easy_perform(curl);

    if (res != CURLE_OK) {
        curl_easy_cleanup(curl);
        return HTTPParser::BuildErrorResponse(502, "Proxy connection failed: " + std::string(curl_easy_strerror(res)));
    }

    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    curl_easy_cleanup(curl);

    return ParseResponse(response_data, http_code, response_headers);
}

void HttpClient::Log(const std::string& message) {
    if (log_callback_) {
        log_callback_("[HttpClient] " + message);
    }
}
