#include "HTTPParser.h"
#include <sstream>
#include <algorithm>
#include <cctype>
#include <regex>

// ============================================================================
// HTTPRequest Helper Methods
// ============================================================================

std::string HTTPParser::HTTPRequest::GetHeader(const std::string& name) const {
    std::string lower_name = ToLower(name);
    for (const auto& [key, value] : headers) {
        if (ToLower(key) == lower_name) {
            return value;
        }
    }
    return "";
}

std::string HTTPParser::HTTPRequest::GetQueryParam(const std::string& name) const {
    auto it = query_params.find(name);
    return it != query_params.end() ? it->second : "";
}

bool HTTPParser::HTTPRequest::HasHeader(const std::string& name) const {
    return !GetHeader(name).empty();
}

bool HTTPParser::HTTPRequest::HasQueryParam(const std::string& name) const {
    return query_params.find(name) != query_params.end();
}

// ============================================================================
// HTTPResponse Helper Methods
// ============================================================================

void HTTPParser::HTTPResponse::SetContentType(const std::string& content_type) {
    headers["Content-Type"] = content_type;
}

void HTTPParser::HTTPResponse::SetContentLength(size_t length) {
    headers["Content-Length"] = std::to_string(length);
}

void HTTPParser::HTTPResponse::SetHeader(const std::string& name, const std::string& value) {
    headers[name] = value;
}

// ============================================================================
// HTTPParser Implementation
// ============================================================================

HTTPParser::HTTPRequest HTTPParser::ParseRequest(const mtp::ByteArray& data) {
    HTTPRequest request;

    // Convert to string for parsing
    std::string http_str(data.begin(), data.end());
    std::istringstream stream(http_str);
    std::string line;

    // Parse request line: GET /path HTTP/1.1
    if (!std::getline(stream, line)) {
        throw std::runtime_error("Empty HTTP request");
    }

    // Remove \r if present
    if (!line.empty() && line.back() == '\r') {
        line.pop_back();
    }

    std::istringstream request_line(line);
    request_line >> request.method >> request.path >> request.protocol;

    if (request.method.empty() || request.path.empty()) {
        throw std::runtime_error("Invalid HTTP request line");
    }

    // Parse query parameters from path
    size_t query_pos = request.path.find('?');
    if (query_pos != std::string::npos) {
        std::string query_string = request.path.substr(query_pos + 1);
        request.path = request.path.substr(0, query_pos);
        request.query_params = ParseQueryString(query_string);
    }

    // Parse headers
    while (std::getline(stream, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }

        if (line.empty()) {
            // Empty line indicates end of headers
            break;
        }

        size_t colon_pos = line.find(':');
        if (colon_pos != std::string::npos) {
            std::string header_name = Trim(line.substr(0, colon_pos));
            std::string header_value = Trim(line.substr(colon_pos + 1));
            request.headers[header_name] = header_value;
        }
    }

    // Parse body (if any)
    std::string body_str;
    std::getline(stream, body_str, '\0');
    if (!body_str.empty()) {
        request.body.insert(request.body.end(), body_str.begin(), body_str.end());
    }

    return request;
}

mtp::ByteArray HTTPParser::BuildResponse(const HTTPResponse& response) {
    std::ostringstream oss;

    // Status line
    oss << response.protocol << " " << response.status_code << " "
        << response.status_message << "\r\n";

    // Headers
    for (const auto& [name, value] : response.headers) {
        oss << name << ": " << value << "\r\n";
    }

    // End of headers
    oss << "\r\n";

    // Convert to ByteArray
    std::string response_str = oss.str();
    mtp::ByteArray result;
    result.insert(result.end(), response_str.begin(), response_str.end());

    // Append body
    result.insert(result.end(), response.body.begin(), response.body.end());

    return result;
}

HTTPParser::HTTPResponse HTTPParser::BuildErrorResponse(int status_code, const std::string& message) {
    HTTPResponse response;
    response.status_code = status_code;
    response.status_message = GetStatusMessage(status_code);

    std::string body_html = "<html><body><h1>" + std::to_string(status_code) + " " +
                            response.status_message + "</h1><p>" + message + "</p></body></html>";

    response.body.insert(response.body.end(), body_html.begin(), body_html.end());
    response.SetContentType("text/html");
    response.SetContentLength(response.body.size());

    return response;
}

HTTPParser::HTTPResponse HTTPParser::BuildSuccessResponse(const std::string& content_type,
                                                          const mtp::ByteArray& body) {
    HTTPResponse response;
    response.status_code = 200;
    response.status_message = "OK";
    response.body = body;
    response.SetContentType(content_type);
    response.SetContentLength(body.size());

    // Set headers that match the Python server to ensure device compatibility
    response.SetHeader("Connection", "keep-alive");
    response.SetHeader("Keep-Alive", "timeout=150000, max=10");
    response.SetHeader("Cache-Control", "max-age=86400");
    response.SetHeader("Expires", "Sun, 19 Apr 2071 10:00:00 GMT");

    return response;
}

std::string HTTPParser::ExtractArtistUUID(const std::string& path) {
    // Match patterns like: /v3.0/en-US/music/artist/{uuid}/biography
    // or /v3.0/model/artist/{uuid}
    std::regex artist_regex(R"(/(?:music|model)/artist/([0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}))",
                           std::regex::icase);

    std::smatch match;
    if (std::regex_search(path, match, artist_regex) && match.size() > 1) {
        return match[1].str();
    }

    return "";
}

std::string HTTPParser::ExtractImageUUID(const std::string& path) {
    // Match pattern: /v3.0/en-US/image/{uuid}
    std::regex image_regex(R"(/image/([0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}))",
                          std::regex::icase);

    std::smatch match;
    if (std::regex_search(path, match, image_regex) && match.size() > 1) {
        return match[1].str();
    }

    return "";
}

std::string HTTPParser::DetermineEndpointType(const std::string& host, const std::string& path) {
    std::string lower_path = ToLower(path);

    // Check specific subresources first (most specific to least specific)
    if (lower_path.find("/devicebackgroundimage") != std::string::npos) {
        return "background";
    } else if (lower_path.find("/similarartists") != std::string::npos) {
        return "similar";
    } else if (lower_path.find("/biography") != std::string::npos) {
        return "biography";
    } else if (lower_path.find("/images") != std::string::npos) {
        // Must check /images before /image to avoid false positives
        return "images";
    } else if (lower_path.find("/image/") != std::string::npos) {
        // Match /v3.0/en-US/image/{uuid}
        return "image";
    } else if (lower_path.find("/albums") != std::string::npos) {
        return "albums";
    } else if (lower_path.find("/tracks") != std::string::npos) {
        return "tracks";
    }

    // Check for base artist endpoint (ends with UUID, no subresource)
    // Patterns: /v3.0/model/artist/{uuid} or /v3.0/en-US/music/artist/{uuid}
    if (lower_path.find("/artist/") != std::string::npos) {
        // Extract UUID pattern: {uuid} or ends with query params
        std::regex artist_endpoint_regex(R"(/artist/[0-9a-f-]+(?:\?|$))", std::regex::icase);
        if (std::regex_search(lower_path, artist_endpoint_regex)) {
            return "artist_overview";
        }
    }

    return "unknown";
}

// ============================================================================
// Private Helper Methods
// ============================================================================

std::map<std::string, std::string> HTTPParser::ParseQueryString(const std::string& query) {
    std::map<std::string, std::string> params;

    size_t pos = 0;
    while (pos < query.size()) {
        size_t amp_pos = query.find('&', pos);
        std::string pair = (amp_pos == std::string::npos)
                          ? query.substr(pos)
                          : query.substr(pos, amp_pos - pos);

        size_t eq_pos = pair.find('=');
        if (eq_pos != std::string::npos) {
            std::string key = URLDecode(pair.substr(0, eq_pos));
            std::string value = URLDecode(pair.substr(eq_pos + 1));
            params[key] = value;
        } else {
            params[URLDecode(pair)] = "";
        }

        pos = (amp_pos == std::string::npos) ? query.size() : amp_pos + 1;
    }

    return params;
}

std::string HTTPParser::URLDecode(const std::string& encoded) {
    std::string decoded;
    decoded.reserve(encoded.size());

    for (size_t i = 0; i < encoded.size(); ++i) {
        if (encoded[i] == '%' && i + 2 < encoded.size()) {
            // Hex decode
            int value;
            std::istringstream hex_stream(encoded.substr(i + 1, 2));
            if (hex_stream >> std::hex >> value) {
                decoded += static_cast<char>(value);
                i += 2;
            } else {
                decoded += encoded[i];
            }
        } else if (encoded[i] == '+') {
            decoded += ' ';
        } else {
            decoded += encoded[i];
        }
    }

    return decoded;
}

std::string HTTPParser::Trim(const std::string& str) {
    size_t first = str.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) {
        return "";
    }

    size_t last = str.find_last_not_of(" \t\r\n");
    return str.substr(first, last - first + 1);
}

std::string HTTPParser::ToLower(const std::string& str) {
    std::string result = str;
    std::transform(result.begin(), result.end(), result.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    return result;
}

std::string HTTPParser::GetStatusMessage(int status_code) {
    switch (status_code) {
        case 200: return "OK";
        case 201: return "Created";
        case 204: return "No Content";
        case 301: return "Moved Permanently";
        case 302: return "Found";
        case 304: return "Not Modified";
        case 400: return "Bad Request";
        case 401: return "Unauthorized";
        case 403: return "Forbidden";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 500: return "Internal Server Error";
        case 501: return "Not Implemented";
        case 502: return "Bad Gateway";
        case 503: return "Service Unavailable";
        default: return "Unknown";
    }
}
