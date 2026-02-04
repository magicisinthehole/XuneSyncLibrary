
#include <iostream>
#include <fstream>
#include <sstream>
#include <iomanip>
#include <vector>
#include <string>
#include <cstring>
#include <algorithm>

#include "lib/src/ZuneDevice.h"
#include "lib/src/ZuneDeviceIdentification.h"
#include "lib/src/ZMDBLibraryExtractor.h"

// JSON escaping helper
std::string JsonEscape(const std::string& str) {
    std::ostringstream escaped;
    for (unsigned char c : str) {
        switch (c) {
            case '"': escaped << "\\\""; break;
            case '\\': escaped << "\\\\"; break;
            case '\b': escaped << "\\b"; break;
            case '\f': escaped << "\\f"; break;
            case '\n': escaped << "\\n"; break;
            case '\r': escaped << "\\r"; break;
            case '\t': escaped << "\\t"; break;
            default:
                if (c < 32) {
                    escaped << "\\u" << std::hex << std::setw(4) << std::setfill('0') << (int)c;
                } else {
                    escaped << (char)c;
                }
        }
    }
    return escaped.str();
}

// Read binary file into ByteArray
mtp::ByteArray ReadBinaryFile(const std::string& filepath) {
    std::ifstream file(filepath, std::ios::binary | std::ios::ate);
    if (!file.is_open()) {
        std::cerr << "ERROR: Failed to open file: " << filepath << "\n";
        return mtp::ByteArray();
    }

    std::streamsize size = file.tellg();
    file.seekg(0, std::ios::beg);

    mtp::ByteArray data(size);
    if (!file.read(reinterpret_cast<char*>(data.data()), size)) {
        std::cerr << "ERROR: Failed to read file: " << filepath << "\n";
        return mtp::ByteArray();
    }

    file.close();
    return data;
}

// Export extracted library to JSON
bool ExportLibraryToJson(const zmdb::ZMDBLibrary& library, const std::string& output_file) {
    std::cout << "\n========================================\n";
    std::cout << "Exporting Library to JSON\n";
    std::cout << "========================================\n";

    std::ofstream json_file(output_file);
    if (!json_file.is_open()) {
        std::cerr << "ERROR: Failed to create JSON file: " << output_file << "\n";
        return false;
    }

    json_file << "{\n";
    json_file << "  \"artists\": [\n";

    // Group albums by artist
    std::vector<std::string> artist_names;
    for (const auto& [artist, _] : library.albums_by_artist) {
        artist_names.push_back(artist);
    }
    std::sort(artist_names.begin(), artist_names.end());

    for (size_t i = 0; i < artist_names.size(); ++i) {
        const auto& artist_name = artist_names[i];
        const auto& albums = library.albums_by_artist.at(artist_name);

        json_file << "    {\n";
        json_file << "      \"name\": \"" << JsonEscape(artist_name) << "\",\n";
        json_file << "      \"albumCount\": " << albums.size() << ",\n";
        json_file << "      \"albums\": [\n";

        for (size_t j = 0; j < albums.size(); ++j) {
            const auto& album = albums[j];

            json_file << "        {\n";
            json_file << "          \"title\": \"" << JsonEscape(album.title) << "\",\n";
            json_file << "          \"artist\": \"" << JsonEscape(album.artist_name) << "\",\n";
            json_file << "          \"year\": " << album.release_year << ",\n";
            json_file << "          \"artworkObjectId\": 0,\n";  // Not extracted from binary
            json_file << "          \"trackCount\": " << album.tracks.size() << ",\n";
            json_file << "          \"tracks\": [\n";

            for (size_t k = 0; k < album.tracks.size(); ++k) {
                const auto& track = album.tracks[k];

                json_file << "            {\n";
                json_file << "              \"title\": \"" << JsonEscape(track.title) << "\",\n";
                json_file << "              \"artist\": \"" << JsonEscape(track.artist_name) << "\",\n";
                json_file << "              \"album\": \"" << JsonEscape(album.title) << "\",\n";
                json_file << "              \"trackNumber\": " << track.track_number << ",\n";
                json_file << "              \"mtpObjectId\": 0\n";  // Not available from binary alone
                json_file << "            }" << (k < album.tracks.size() - 1 ? "," : "") << "\n";
            }

            json_file << "          ]\n";
            json_file << "        }" << (j < albums.size() - 1 ? "," : "") << "\n";
        }

        json_file << "      ]\n";
        json_file << "    }" << (i < artist_names.size() - 1 ? "," : "") << "\n";
    }

    json_file << "  ]\n";
    json_file << "}\n";

    json_file.close();

    std::cout << "✓ Exported library to: " << output_file << "\n";
    std::cout << "  Artists: " << library.artist_count << "\n";
    std::cout << "  Albums: " << library.album_count << "\n";
    std::cout << "  Tracks: " << library.track_count << "\n";

    return true;
}

void PrintLibrarySummary(const zmdb::ZMDBLibrary& library, const std::string& label) {
    std::cout << "\n" << label << ":\n";
    std::cout << "  Artists: " << library.artist_count << "\n";
    std::cout << "  Albums: " << library.album_count << "\n";
    std::cout << "  Tracks: " << library.track_count << "\n";
}

void PrintFirstArtists(const zmdb::ZMDBLibrary& library, int count = 5) {
    std::cout << "\nFirst " << std::min(count, (int)library.albums_by_artist.size()) << " Artists:\n";
    std::cout << "-----------------------------------\n";

    int shown = 0;
    for (const auto& [artist_name, albums] : library.albums_by_artist) {
        if (shown >= count) break;

        std::cout << (shown + 1) << ". " << artist_name
                  << " (" << albums.size() << " albums)\n";

        // Show first 2 albums
        int albums_to_show = std::min(2, (int)albums.size());
        for (int j = 0; j < albums_to_show; ++j) {
            const auto& album = albums[j];
            std::cout << "   - " << album.title
                      << " (" << album.release_year << ") - "
                      << album.tracks.size() << " tracks\n";
        }

        if (albums.size() > 2) {
            std::cout << "   ... and " << (albums.size() - 2) << " more\n";
        }

        shown++;
    }
}

void PrintUsage(const char* program_name) {
    std::cout << "Usage: " << program_name << " [OPTIONS] [FILE]\n\n";
    std::cout << "Options:\n";
    std::cout << "  file <path>              Read zmdb from binary file (default behavior if FILE given)\n";
    std::cout << "  device                   Connect to USB device and fetch zmdb\n";
    std::cout << "  --output <path>          Output JSON file (default: library.json)\n";
    std::cout << "  --device-type <type>     Override device type: Zune30 or ZuneHD\n";
    std::cout << "  --verbose                Enable verbose logging\n";
    std::cout << "  --help                   Show this help message\n\n";
    std::cout << "Examples:\n";
    std::cout << "  " << program_name << " file /path/to/zmdb.bin\n";
    std::cout << "  " << program_name << " device --output my_library.json\n";
    std::cout << "  " << program_name << " /path/to/zmdb.bin --device-type Zune30\n";
}

int main(int argc, char* argv[]) {
    std::cout << "========================================\n";
    std::cout << "   ZMDB Extractor Test\n";
    std::cout << "========================================\n\n";

    // Parse arguments
    std::string source_mode = "device";  // default
    std::string file_path;
    std::string output_file = "library.json";
    std::string device_type_override;
    bool verbose = false;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];

        if (arg == "--help") {
            PrintUsage(argv[0]);
            return 0;
        } else if (arg == "file" && i + 1 < argc) {
            source_mode = "file";
            file_path = argv[++i];
        } else if (arg == "device") {
            source_mode = "device";
        } else if (arg == "--output" && i + 1 < argc) {
            output_file = argv[++i];
        } else if (arg == "--device-type" && i + 1 < argc) {
            device_type_override = argv[++i];
        } else if (arg == "--verbose") {
            verbose = true;
        } else if (arg[0] != '-' && file_path.empty()) {
            // Treat as file path if no flag prefix
            source_mode = "file";
            file_path = arg;
        } else {
            std::cerr << "Unknown option: " << arg << "\n";
            PrintUsage(argv[0]);
            return 1;
        }
    }

    mtp::ByteArray zmdb_data;
    zune::DeviceFamily device_family = zune::DeviceFamily::Unknown;

    // Helper to parse device type string to enum
    auto parseDeviceType = [](const std::string& type) -> zune::DeviceFamily {
        if (type == "Zune30" || type == "Zune 30" || type == "Classic" || type == "Keel") {
            return zune::DeviceFamily::Keel;  // 1st Gen - Zune 30
        } else if (type == "Zune80" || type == "Zune 80" || type == "Draco") {
            return zune::DeviceFamily::Draco;  // 2nd Gen HDD - Zune 80/120
        } else if (type == "ZuneHD" || type == "Zune HD" || type == "HD" || type == "Pavo") {
            return zune::DeviceFamily::Pavo;  // HD - Zune HD
        }
        return zune::DeviceFamily::Keel;  // Default to Classic
    };

    // ===== SOURCE: Read zmdb from file =====
    if (source_mode == "file") {
        if (file_path.empty()) {
            std::cerr << "ERROR: No file path provided\n";
            PrintUsage(argv[0]);
            return 1;
        }

        std::cout << "Reading zmdb from file: " << file_path << "\n";
        zmdb_data = ReadBinaryFile(file_path);

        if (zmdb_data.empty()) {
            std::cerr << "ERROR: Failed to read zmdb file\n";
            return 1;
        }

        std::cout << "✓ Loaded " << zmdb_data.size() << " bytes\n";

        // Use override or default to Keel (Zune 30)
        device_family = device_type_override.empty() ? zune::DeviceFamily::Keel : parseDeviceType(device_type_override);
    }
    // ===== SOURCE: Connect to device =====
    else {
        std::cout << "Connecting to USB device...\n";
        ZuneDevice device;

        if (verbose) {
            device.SetLogCallback([](const std::string& msg) {
                std::cout << "[DEVICE] " << msg << "\n";
            });
        }

        if (!device.ConnectUSB()) {
            std::cerr << "\nERROR: Failed to connect to Zune device via USB\n";
            std::cerr << "Please ensure:\n";
            std::cerr << "  1. Device is connected via USB\n";
            std::cerr << "  2. Device is powered on\n";
            std::cerr << "  3. You have necessary permissions\n";
            return 1;
        }

        std::cout << "\n✓ Connected successfully!\n";

        // Get device info
        std::string device_name = device.GetName();
        std::string device_serial = device.GetSerialNumber();
        device_family = device.GetDeviceFamily();

        std::cout << "\nDevice Information:\n";
        std::cout << "  Name: " << device_name << "\n";
        std::cout << "  Serial: " << device_serial << "\n";
        std::cout << "  Model: " << zune::GetFamilyName(device_family) << "\n";

        // Fetch zmdb from device
        std::cout << "\nFetching zmdb from device...\n";
        std::vector<uint8_t> library_object_id = {0x03, 0x92, 0x1f};
        zmdb_data = device.GetZuneMetadata(library_object_id);

        if (zmdb_data.empty()) {
            std::cerr << "ERROR: Failed to fetch zmdb from device\n";
            device.Disconnect();
            return 1;
        }

        std::cout << "✓ Retrieved zmdb: " << zmdb_data.size() << " bytes\n";

        // Save zmdb binary for debugging
        std::ofstream zmdb_file("/tmp/device_zmdb.bin", std::ios::binary);
        zmdb_file.write(reinterpret_cast<const char*>(zmdb_data.data()), zmdb_data.size());
        zmdb_file.close();
        std::cout << "✓ Saved zmdb to /tmp/device_zmdb.bin\n";

        device.Disconnect();
    }

    // ===== PARSE zmdb =====
    std::cout << "\n========================================\n";
    std::cout << "Parsing ZMDB\n";
    std::cout << "========================================\n";

    // Use override if provided, otherwise use detected device_family
    zune::DeviceFamily parse_family = device_type_override.empty() ? device_family : parseDeviceType(device_type_override);
    std::cout << "Device type: " << zune::GetFamilyName(parse_family) << "\n";

    zmdb::ZMDBLibraryExtractor extractor;
    zmdb::ZMDBLibrary library = extractor.ExtractLibrary(zmdb_data, parse_family);

    PrintLibrarySummary(library, "Extraction Results");
    PrintFirstArtists(library, 5);

    // ===== EXPORT to JSON =====
    if (!ExportLibraryToJson(library, output_file)) {
        std::cerr << "ERROR: Failed to export library to JSON\n";
        return 1;
    }

    std::cout << "\n========================================\n";
    std::cout << "✓ Test complete!\n";
    std::cout << "========================================\n";

    return 0;
}
