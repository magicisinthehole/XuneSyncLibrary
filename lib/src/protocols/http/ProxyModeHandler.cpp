#include "ProxyModeHandler.h"
#include <curl/curl.h>
#include <sstream>

// libcurl write callback for receiving response data
static size_t WriteCallback(void* contents, size_t size, size_t nmemb, void* userp) {
    size_t total_size = size * nmemb;
    mtp::ByteArray* buffer = static_cast<mtp::ByteArray*>(userp);
    buffer->insert(buffer->end(),
                   static_cast<uint8_t*>(contents),
                   static_cast<uint8_t*>(contents) + total_size);
    return total_size;
}

// libcurl header callback for capturing response headers
static size_t HeaderCallback(char* buffer, size_t size, size_t nitems, void* userp) {
    size_t total_size = size * nitems;
    std::map<std::string, std::string>* headers =
        static_cast<std::map<std::string, std::string>*>(userp);

    std::string header_line(buffer, total_size);

    // Remove trailing \r\n
    while (!header_line.empty() &&
           (header_line.back() == '\r' || header_line.back() == '\n')) {
        header_line.pop_back();
    }

    // Parse "Key: Value" format
    size_t colon_pos = header_line.find(':');
    if (colon_pos != std::string::npos) {
        std::string key = header_line.substr(0, colon_pos);
        std::string value = header_line.substr(colon_pos + 1);

        // Trim leading whitespace from value
        while (!value.empty() && value[0] == ' ') {
            value.erase(0, 1);
        }

        (*headers)[key] = value;
    }

    return total_size;
}

ProxyModeHandler::ProxyModeHandler(const ProxyConfig& config)
    : config_(config) {

    // Initialize libcurl
    curl_handle_ = curl_easy_init();
    if (!curl_handle_) {
        throw std::runtime_error("Failed to initialize libcurl");
    }
}

ProxyModeHandler::~ProxyModeHandler() {
    if (curl_handle_) {
        curl_easy_cleanup(static_cast<CURL*>(curl_handle_));
    }
}

HTTPParser::HTTPResponse ProxyModeHandler::HandleRequest(const HTTPParser::HTTPRequest& request) {
    // Handle Microsoft connectivity check endpoint
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

    std::string full_url = BuildURL(server, request.path);

    // Append query parameters if present
    if (!request.query_params.empty()) {
        full_url += "?";
        bool first = true;
        for (const auto& param : request.query_params) {
            if (!first) full_url += "&";
            full_url += param.first + "=" + param.second;
            first = false;
        }
    }

    Log("Proxy mode: " + request.method + " " + full_url);

    if (request.method == "GET") {
        return PerformGET(full_url, request.headers);
    } else {
        Log("Warning: Unsupported HTTP method: " + request.method);
        return HTTPParser::BuildErrorResponse(405, "Method not allowed");
    }
}

void ProxyModeHandler::SetLogCallback(LogCallback callback) {
    log_callback_ = callback;
}

bool ProxyModeHandler::TestConnection() {
    if (config_.catalog_server.empty()) {
        return false;
    }

    try {
        std::string test_url = config_.catalog_server + "/";

        CURL* curl = static_cast<CURL*>(curl_handle_);
        curl_easy_setopt(curl, CURLOPT_URL, test_url.c_str());
        curl_easy_setopt(curl, CURLOPT_NOBODY, 1L);  // HEAD request
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);

        CURLcode res = curl_easy_perform(curl);

        // Reset options
        curl_easy_setopt(curl, CURLOPT_NOBODY, 0L);

        return res == CURLE_OK;
    } catch (...) {
        return false;
    }
}

// ============================================================================
// Private Methods
// ============================================================================

std::string ProxyModeHandler::SelectServer(const std::string& host) {
    std::string lower_host = host;
    std::transform(lower_host.begin(), lower_host.end(), lower_host.begin(), ::tolower);

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
    else {
        // Default to catalog server
        return config_.catalog_server;
    }
}

std::string ProxyModeHandler::BuildURL(const std::string& server, const std::string& path) {
    // Remove trailing slash from server if present
    std::string base = server;
    if (!base.empty() && base.back() == '/') {
        base.pop_back();
    }

    // Ensure path starts with /
    std::string full_path = path;
    if (full_path.empty() || full_path[0] != '/') {
        full_path = "/" + full_path;
    }

    return base + full_path;
}

HTTPParser::HTTPResponse ProxyModeHandler::PerformGET(
    const std::string& url,
    const std::map<std::string, std::string>& headers) {

    CURL* curl = curl_easy_init();
    if (!curl) {
        Log("libcurl error: Failed initialization");
        return HTTPParser::BuildErrorResponse(502, "libcurl initialization failed");
    }

    mtp::ByteArray response_data;
    std::map<std::string, std::string> response_headers;
    long http_code = 0;

    // Set URL
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());

    // Set write callback
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response_data);

    // Set header callback to capture response headers
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, HeaderCallback);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, &response_headers);

    // Set timeout (30 seconds minimum for image fetches)
    long timeout_seconds = config_.timeout_ms / 1000;
    if (timeout_seconds < 1) timeout_seconds = 30;
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, timeout_seconds);

    // Set headers
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
        curl_easy_cleanup(curl);  // Clean up before returning
        return HTTPParser::BuildErrorResponse(502, "Proxy request failed: " + error_msg);
    }

    Log("Proxy response: " + std::to_string(http_code) + " (" +
        std::to_string(response_data.size()) + " bytes)");

    // Clean up curl handle
    curl_easy_cleanup(curl);

    return ParseResponse(response_data, http_code, response_headers);
}

HTTPParser::HTTPResponse ProxyModeHandler::ParseResponse(
    const mtp::ByteArray& response_data,
    int status_code,
    std::map<std::string, std::string> headers) {

    HTTPParser::HTTPResponse response;
    response.status_code = status_code;
    response.status_message = HTTPParser::GetStatusMessage(status_code);
    response.body = response_data;

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

void ProxyModeHandler::Log(const std::string& message) {
    if (log_callback_) {
        log_callback_("[ProxyModeHandler] " + message);
    }
}
