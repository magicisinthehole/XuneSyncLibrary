#pragma once

#include "HTTPParser.h"
#include <string>
#include <functional>

/**
 * StaticModeHandler
 *
 * Handles HTTP requests by serving static files from local directory.
 *
 * Directory Structure:
 * data_directory/
 * └── {artist-uuid}/
 *     ├── overview.xml    - Artist overview (name, thumb ID)
 *     ├── biography.xml   - Artist biography
 *     ├── images.xml      - List of image UUIDs
 *     ├── 7.jpg           - Device background (thumb.jpg)
 *     └── 1.jpg to 6.jpg  - Gallery images
 *
 * URL Mappings:
 * - /v3.0/model/artist/{uuid}                      -> {uuid}/overview.xml
 * - /v3.0/en-US/music/artist/{uuid}                -> {uuid}/overview.xml
 * - /v3.0/en-US/music/artist/{uuid}/biography      -> {uuid}/biography.xml
 * - /v3.0/en-US/music/artist/{uuid}/images         -> {uuid}/images.xml
 * - /v3.0/en-US/music/artist/{uuid}/deviceBackgroundImage -> {uuid}/7.jpg
 * - /v3.0/en-US/image/00011002-000N-0000-0000-000000000000 -> {uuid}/N.jpg
 * - /v3.0/en-US/image/00011002-0010-0000-0000-000000000000 -> {uuid}/thumb.jpg
 */
class StaticModeHandler {
public:
    using LogCallback = std::function<void(const std::string& message)>;

    /**
     * Constructor
     * @param data_directory Path to artist_data folder
     * @param test_mode If true, redirect all UUIDs to 00000000-0000-0000-0000-000000000000
     */
    explicit StaticModeHandler(const std::string& data_directory, bool test_mode = false);

    /**
     * Handle an HTTP request
     * @param request Parsed HTTP request
     * @return HTTP response with file contents or 404
     */
    HTTPParser::HTTPResponse HandleRequest(const HTTPParser::HTTPRequest& request);

    /**
     * Set logging callback
     * @param callback Function to receive log messages
     */
    void SetLogCallback(LogCallback callback);

    /**
     * Check if artist metadata exists
     * @param artist_uuid MusicBrainz artist UUID
     * @return true if directory exists with at least biography.xml
     */
    bool HasArtistData(const std::string& artist_uuid) const;

    /**
     * Get data directory path
     * @return Configured data directory
     */
    std::string GetDataDirectory() const { return data_directory_; }

private:
    /**
     * Handle biography request
     * @param artist_uuid Artist UUID
     * @return HTTP response with biography XML
     */
    HTTPParser::HTTPResponse HandleBiographyRequest(const std::string& artist_uuid);

    /**
     * Handle images list request
     * @param artist_uuid Artist UUID
     * @return HTTP response with images XML
     */
    HTTPParser::HTTPResponse HandleImagesRequest(const std::string& artist_uuid);

    /**
     * Handle background image request
     * @param artist_uuid Artist UUID
     * @return HTTP response with background JPEG
     */
    HTTPParser::HTTPResponse HandleBackgroundImageRequest(const std::string& artist_uuid);

    /**
     * Handle individual image request
     * @param image_uuid Image UUID
     * @return HTTP response with image JPEG
     */
    HTTPParser::HTTPResponse HandleImageRequest(const std::string& image_uuid);

    /**
     * Load file contents into ByteArray
     * @param filepath Full path to file
     * @return File contents or empty array if not found
     */
    mtp::ByteArray LoadFile(const std::string& filepath);

    /**
     * Build path to artist directory
     * @param artist_uuid Artist UUID
     * @return Full path to artist directory
     */
    std::string GetArtistPath(const std::string& artist_uuid) const;

    /**
     * Map image UUID to actual file path
     * @param artist_uuid Artist UUID
     * @param image_uuid Image UUID
     * @return Path to image file or empty string if not found
     */
    std::string MapImageUUID(const std::string& artist_uuid, const std::string& image_uuid);

    /**
     * Load image UUID mappings from images.xml
     * @param artist_uuid Artist UUID
     * @return Map of image UUID to file path
     */
    std::map<std::string, std::string> LoadImageMappings(const std::string& artist_uuid);

    /**
     * Update XML timestamp to current UTC time
     * @param xml_content Original XML content
     * @return XML with updated <a:updated> timestamp
     */
    mtp::ByteArray UpdateXMLTimestamp(const mtp::ByteArray& xml_content);

    /**
     * Log a message
     */
    void Log(const std::string& message);

    // Configuration
    std::string data_directory_;
    bool test_mode_;
    LogCallback log_callback_;

    // Cache for image UUID mappings (avoid parsing XML repeatedly)
    std::map<std::string, std::map<std::string, std::string>> image_uuid_cache_;
};
