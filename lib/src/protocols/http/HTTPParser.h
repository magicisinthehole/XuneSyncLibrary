#pragma once

#include <string>
#include <map>
#include <mtp/ByteArray.h>

/**
 * HTTPParser
 *
 * Parses HTTP/1.1 requests and builds HTTP responses.
 *
 * Expected Zune HTTP requests:
 * - GET /v3.0/en-US/music/artist/{uuid}/biography
 * - GET /v3.0/en-US/music/artist/{uuid}/images?chunkSize=10
 * - GET /v3.0/en-US/music/artist/{uuid}/deviceBackgroundImage?width=480&resize=true&contenttype=image/jpeg
 * - GET /v3.0/en-US/image/{image-uuid}?width=480&resize=true&contenttype=image/jpeg
 */
class HTTPParser {
public:
    struct HTTPRequest {
        std::string method;           // GET, POST, etc.
        std::string path;             // /v3.0/...
        std::string protocol;         // HTTP/1.1
        std::map<std::string, std::string> headers;
        mtp::ByteArray body;          // POST data (if any)

        // Query parameters extracted from path
        std::map<std::string, std::string> query_params;

        // Helper methods
        std::string GetHeader(const std::string& name) const;
        std::string GetQueryParam(const std::string& name) const;
        bool HasHeader(const std::string& name) const;
        bool HasQueryParam(const std::string& name) const;
    };

    struct HTTPResponse {
        int status_code = 200;
        std::string status_message = "OK";
        std::string protocol = "HTTP/1.1";
        std::map<std::string, std::string> headers;
        mtp::ByteArray body;

        // Helper method to set common headers
        void SetContentType(const std::string& content_type);
        void SetContentLength(size_t length);
        void SetHeader(const std::string& name, const std::string& value);
    };

    /**
     * Parse HTTP request from TCP payload
     * @param data HTTP request bytes
     * @return Parsed HTTP request
     * @throws std::runtime_error if request is malformed
     */
    static HTTPRequest ParseRequest(const mtp::ByteArray& data);

    /**
     * Build HTTP response bytes
     * @param response Response structure
     * @return HTTP response ready for TCP transmission
     */
    static mtp::ByteArray BuildResponse(const HTTPResponse& response);

    /**
     * Build a simple error response
     * @param status_code HTTP status code (e.g., 404, 500)
     * @param message Error message
     * @return HTTP response
     */
    static HTTPResponse BuildErrorResponse(int status_code, const std::string& message);

    /**
     * Build a successful response with body
     * @param content_type MIME type (e.g., "application/xml", "image/jpeg")
     * @param body Response body data
     * @return HTTP response
     */
    static HTTPResponse BuildSuccessResponse(const std::string& content_type, const mtp::ByteArray& body);

    /**
     * Extract artist UUID from Zune catalog path
     * @param path Request path (e.g., "/v3.0/en-US/music/artist/{uuid}/biography")
     * @return Artist UUID or empty string if not found
     */
    static std::string ExtractArtistUUID(const std::string& path);

    /**
     * Extract image UUID from Zune art path
     * @param path Request path (e.g., "/v3.0/en-US/image/{uuid}")
     * @return Image UUID or empty string if not found
     */
    static std::string ExtractImageUUID(const std::string& path);

    /**
     * Determine endpoint type from host and path
     * @param host Host header value (e.g., "catalog.zune.net")
     * @param path Request path
     * @return Endpoint type ("biography", "images", "background", "image", "unknown")
     */
    static std::string DetermineEndpointType(const std::string& host, const std::string& path);

    /**
     * Get HTTP status message for code
     * @param status_code HTTP status code
     * @return Status message (e.g., 200 -> "OK", 404 -> "Not Found")
     */
    static std::string GetStatusMessage(int status_code);

    /**
     * Parse query string into key-value pairs
     * @param query Query string (e.g., "chunkSize=10&offset=0")
     * @return Map of query parameters
     */
    static std::map<std::string, std::string> ParseQueryString(const std::string& query);

private:

    /**
     * URL decode a string (e.g., "%20" -> " ")
     * @param encoded URL-encoded string
     * @return Decoded string
     */
    static std::string URLDecode(const std::string& encoded);

    /**
     * Trim whitespace from string
     */
    static std::string Trim(const std::string& str);

    /**
     * Convert string to lowercase
     */
    static std::string ToLower(const std::string& str);
};
