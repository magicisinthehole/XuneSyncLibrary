#include "StaticModeHandler.h"
#include <fstream>
#include <sstream>
#include <sys/stat.h>
#include <regex>
#include <ctime>
#include <iomanip>

StaticModeHandler::StaticModeHandler(const std::string& data_directory, bool test_mode)
    : data_directory_(data_directory), test_mode_(test_mode) {
}

HTTPParser::HTTPResponse StaticModeHandler::HandleRequest(const HTTPParser::HTTPRequest& request) {
    std::string endpoint_type = HTTPParser::DetermineEndpointType(
        request.GetHeader("Host"), request.path);

    Log("Static mode: " + request.method + " " + request.path + " [" + endpoint_type + "]");

    std::string artist_uuid = HTTPParser::ExtractArtistUUID(request.path);

    if (endpoint_type == "artist_overview") {
        if (artist_uuid.empty()) {
            return HTTPParser::BuildErrorResponse(400, "Invalid artist UUID in path");
        }
        std::string filepath = GetArtistPath(artist_uuid) + "/overview.xml";
        Log("Loading artist overview from: " + filepath);
        mtp::ByteArray content = LoadFile(filepath);
        if (content.empty()) {
            Log("Artist overview not found: " + filepath);
            return HTTPParser::BuildErrorResponse(404, "Artist overview not found");
        }
        // Update timestamp to current time
        content = UpdateXMLTimestamp(content);
        return HTTPParser::BuildSuccessResponse("application/xml", content);
    }
    else if (endpoint_type == "biography") {
        if (artist_uuid.empty()) {
            return HTTPParser::BuildErrorResponse(400, "Invalid artist UUID in path");
        }
        return HandleBiographyRequest(artist_uuid);
    }
    else if (endpoint_type == "images") {
        if (artist_uuid.empty()) {
            return HTTPParser::BuildErrorResponse(400, "Invalid artist UUID in path");
        }
        return HandleImagesRequest(artist_uuid);
    }
    else if (endpoint_type == "albums") {
        if (artist_uuid.empty()) {
            return HTTPParser::BuildErrorResponse(400, "Invalid artist UUID in path");
        }
        Log("Albums endpoint not available in static mode");
        return HTTPParser::BuildErrorResponse(404, "Albums endpoint not implemented");
    }
    else if (endpoint_type == "tracks") {
        if (artist_uuid.empty()) {
            return HTTPParser::BuildErrorResponse(400, "Invalid artist UUID in path");
        }
        Log("Tracks endpoint not available in static mode");
        return HTTPParser::BuildErrorResponse(404, "Tracks endpoint not implemented");
    }
    else if (endpoint_type == "similar") {
        if (artist_uuid.empty()) {
            return HTTPParser::BuildErrorResponse(400, "Invalid artist UUID in path");
        }
        Log("Similar artists endpoint not available in static mode");
        return HTTPParser::BuildErrorResponse(404, "Similar artists endpoint not implemented");
    }
    else if (endpoint_type == "background") {
        if (artist_uuid.empty()) {
            return HTTPParser::BuildErrorResponse(400, "Invalid artist UUID in path");
        }
        return HandleBackgroundImageRequest(artist_uuid);
    }
    else if (endpoint_type == "image") {
        std::string image_uuid = HTTPParser::ExtractImageUUID(request.path);
        if (image_uuid.empty()) {
            return HTTPParser::BuildErrorResponse(400, "Invalid image UUID in path");
        }
        return HandleImageRequest(image_uuid);
    }
    else {
        Log("Warning: Unknown endpoint type: " + endpoint_type);
        return HTTPParser::BuildErrorResponse(404, "Endpoint not implemented in static mode");
    }
}

void StaticModeHandler::SetLogCallback(LogCallback callback) {
    log_callback_ = callback;
}

bool StaticModeHandler::HasArtistData(const std::string& artist_uuid) const {
    std::string artist_path = GetArtistPath(artist_uuid);
    std::string bio_path = artist_path + "/biography.xml";

    struct stat st;
    return stat(bio_path.c_str(), &st) == 0;
}

// ============================================================================
// Private Methods - Request Handlers
// ============================================================================

HTTPParser::HTTPResponse StaticModeHandler::HandleBiographyRequest(const std::string& artist_uuid) {
    std::string filepath = GetArtistPath(artist_uuid) + "/biography.xml";
    Log("Loading biography from: " + filepath);

    mtp::ByteArray content = LoadFile(filepath);
    if (content.empty()) {
        Log("Biography not found: " + filepath);
        return HTTPParser::BuildErrorResponse(404, "Biography not found for artist");
    }

    // Update timestamp to current time
    content = UpdateXMLTimestamp(content);
    return HTTPParser::BuildSuccessResponse("application/xml", content);
}

HTTPParser::HTTPResponse StaticModeHandler::HandleImagesRequest(const std::string& artist_uuid) {
    std::string filepath = GetArtistPath(artist_uuid) + "/images.xml";
    Log("Loading images list from: " + filepath);

    mtp::ByteArray content = LoadFile(filepath);
    if (content.empty()) {
        Log("Images list not found: " + filepath);
        return HTTPParser::BuildErrorResponse(404, "Images list not found for artist");
    }

    // Update timestamp to current time
    content = UpdateXMLTimestamp(content);
    return HTTPParser::BuildSuccessResponse("application/xml", content);
}

HTTPParser::HTTPResponse StaticModeHandler::HandleBackgroundImageRequest(const std::string& artist_uuid) {
    // Server uses 7.jpg as background image
    std::string filepath = GetArtistPath(artist_uuid) + "/7.jpg";
    Log("Loading background image from: " + filepath);

    mtp::ByteArray content = LoadFile(filepath);
    if (content.empty()) {
        Log("Background image not found: " + filepath);
        return HTTPParser::BuildErrorResponse(404, "Background image not found for artist");
    }

    return HTTPParser::BuildSuccessResponse("image/jpeg", content);
}

HTTPParser::HTTPResponse StaticModeHandler::HandleImageRequest(const std::string& image_uuid) {
    // Image UUID format: {artist-code}-{image-num}-0000-0000-000000000000
    // Example: 00011002-0001-0000-0000-000000000000
    // Extract second segment to get image number

    size_t first_dash = image_uuid.find('-');
    if (first_dash == std::string::npos) {
        Log("Invalid image UUID format: " + image_uuid);
        return HTTPParser::BuildErrorResponse(400, "Invalid image UUID format");
    }

    size_t second_dash = image_uuid.find('-', first_dash + 1);
    if (second_dash == std::string::npos) {
        Log("Invalid image UUID format: " + image_uuid);
        return HTTPParser::BuildErrorResponse(400, "Invalid image UUID format");
    }

    std::string artist_code = image_uuid.substr(0, first_dash);
    std::string image_num_str = image_uuid.substr(first_dash + 1, second_dash - first_dash - 1);

    int image_num = 0;
    try {
        image_num = std::stoi(image_num_str, nullptr, 10);
    } catch (...) {
        Log("Invalid image number in UUID: " + image_uuid);
        return HTTPParser::BuildErrorResponse(400, "Invalid image number");
    }

    // Determine artist directory
    // In test mode, always use 00000000-0000-0000-0000-000000000000
    std::string artist_path;
    if (test_mode_) {
        artist_path = data_directory_ + "/00000000-0000-0000-0000-000000000000";
    } else {
        // Map artist code back to full UUID
        // For now, assume artist directories exist with proper UUIDs
        // TODO: Implement proper artist code -> UUID mapping
        artist_path = data_directory_ + "/" + artist_code + "-0000-0000-0000-000000000000";
    }

    // Map image number to filename
    std::string filepath;
    if (image_num >= 1 && image_num <= 6) {
        filepath = artist_path + "/" + std::to_string(image_num) + ".jpg";
    } else if (image_num == 10) {
        filepath = artist_path + "/thumb.jpg";
    } else {
        Log("Unsupported image number " + std::to_string(image_num) + " in UUID: " + image_uuid);
        return HTTPParser::BuildErrorResponse(404, "Image not found");
    }

    Log("Loading image from: " + filepath);
    mtp::ByteArray content = LoadFile(filepath);
    if (content.empty()) {
        return HTTPParser::BuildErrorResponse(404, "Image file not found");
    }

    return HTTPParser::BuildSuccessResponse("image/jpeg", content);
}

// ============================================================================
// Private Methods - Helpers
// ============================================================================

mtp::ByteArray StaticModeHandler::LoadFile(const std::string& filepath) {
    std::ifstream file(filepath, std::ios::binary);
    if (!file.is_open()) {
        return mtp::ByteArray();
    }

    // Get file size
    file.seekg(0, std::ios::end);
    size_t file_size = file.tellg();
    file.seekg(0, std::ios::beg);

    // Read entire file
    mtp::ByteArray content(file_size);
    file.read(reinterpret_cast<char*>(content.data()), file_size);

    if (!file) {
        Log("Error reading file: " + filepath);
        return mtp::ByteArray();
    }

    Log("Loaded file: " + filepath + " (" + std::to_string(file_size) + " bytes)");
    return content;
}

std::string StaticModeHandler::GetArtistPath(const std::string& artist_uuid) const {
    // In test mode, redirect all UUIDs to the test directory
    if (test_mode_) {
        return data_directory_ + "/00000000-0000-0000-0000-000000000000";
    }
    return data_directory_ + "/" + artist_uuid;
}

std::string StaticModeHandler::MapImageUUID(const std::string& artist_uuid,
                                           const std::string& image_uuid) {
    // Check cache first
    auto artist_it = image_uuid_cache_.find(artist_uuid);
    if (artist_it != image_uuid_cache_.end()) {
        auto image_it = artist_it->second.find(image_uuid);
        if (image_it != artist_it->second.end()) {
            return image_it->second;
        }
    }

    // Load mappings from images.xml
    auto mappings = LoadImageMappings(artist_uuid);
    image_uuid_cache_[artist_uuid] = mappings;

    auto it = mappings.find(image_uuid);
    return it != mappings.end() ? it->second : "";
}

std::map<std::string, std::string> StaticModeHandler::LoadImageMappings(
    const std::string& artist_uuid) {

    std::map<std::string, std::string> mappings;

    std::string filepath = GetArtistPath(artist_uuid) + "/images.xml";
    mtp::ByteArray content = LoadFile(filepath);
    if (content.empty()) {
        return mappings;
    }

    // Parse XML to extract UUID -> filename mappings
    // Format: <id>urn:uuid:{IMAGE_UUID}</id>
    // Corresponds to image_N.jpg where N is the entry order

    std::string xml_str(content.begin(), content.end());
    std::regex uuid_regex(R"(<id>urn:uuid:([0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12})</id>)",
                         std::regex::icase);

    auto words_begin = std::sregex_iterator(xml_str.begin(), xml_str.end(), uuid_regex);
    auto words_end = std::sregex_iterator();

    int image_index = 1;
    for (std::sregex_iterator i = words_begin; i != words_end; ++i) {
        std::smatch match = *i;
        if (match.size() > 1) {
            std::string uuid = match[1].str();
            std::string filename = std::to_string(image_index) + ".jpg";
            std::string fullpath = GetArtistPath(artist_uuid) + "/" + filename;
            mappings[uuid] = fullpath;
            image_index++;
        }
    }

    return mappings;
}

mtp::ByteArray StaticModeHandler::UpdateXMLTimestamp(const mtp::ByteArray& xml_content) {
    // Convert ByteArray to string
    std::string xml_str(xml_content.begin(), xml_content.end());

    // Get current UTC time
    std::time_t now = std::time(nullptr);
    std::tm* gmt = std::gmtime(&now);

    // Format timestamp as ISO 8601: 2025-11-11T12:34:56.000000Z
    std::ostringstream timestamp_stream;
    timestamp_stream << std::put_time(gmt, "%Y-%m-%dT%H:%M:%S");
    timestamp_stream << ".000000Z";
    std::string current_timestamp = timestamp_stream.str();

    // Replace <a:updated> timestamp using regex
    // Pattern matches: <a:updated>YYYY-MM-DDTHH:MM:SS.FFFFFFZ</a:updated>
    std::regex timestamp_regex(R"(<a:updated>[^<]+</a:updated>)");
    std::string replacement = "<a:updated>" + current_timestamp + "</a:updated>";
    std::string updated_xml = std::regex_replace(xml_str, timestamp_regex, replacement);

    Log("Updated XML timestamp to: " + current_timestamp);

    // Convert back to ByteArray
    return mtp::ByteArray(updated_xml.begin(), updated_xml.end());
}

void StaticModeHandler::Log(const std::string& message) {
    if (log_callback_) {
        log_callback_("[StaticModeHandler] " + message);
    }
}
