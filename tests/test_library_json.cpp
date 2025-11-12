#include <iostream>
#include <fstream>
#include <sstream>
#include <vector>
#include <string>
#include <iomanip>
#include <cstring>
#include <cstdio>
#include <chrono>

#include "lib/src/ZuneDevice.h"

// Simple JSON escaping function
std::string EscapeJsonString(const std::string& input) {
    std::string output;
    for (unsigned char c : input) {
        switch (c) {
            case '"':  output += "\\\""; break;
            case '\\': output += "\\\\"; break;
            case '\b': output += "\\b"; break;
            case '\f': output += "\\f"; break;
            case '\n': output += "\\n"; break;
            case '\r': output += "\\r"; break;
            case '\t': output += "\\t"; break;
            default:
                if (c < 32) {
                    // Properly format control characters as \uXXXX
                    char hex_buf[7];
                    snprintf(hex_buf, sizeof(hex_buf), "\\u%04x", c);
                    output += hex_buf;
                } else {
                    output += c;
                }
        }
    }
    return output;
}

// Format current timestamp
std::string GetTimestamp() {
    auto now = std::chrono::system_clock::now();
    auto time = std::chrono::system_clock::to_time_t(now);
    std::ostringstream oss;
    oss << std::put_time(std::localtime(&time), "%Y-%m-%d %H:%M:%S");
    return oss.str();
}

// Build JSON output from library data
std::string BuildLibraryJson(const std::vector<ZuneArtistInfo>& artists,
                              const std::string& device_name = "",
                              const std::string& device_serial = "") {
    std::ostringstream json;

    // Start JSON object
    json << "{\n";
    json << "  \"metadata\": {\n";
    json << "    \"generated\": \"" << GetTimestamp() << "\",\n";
    json << "    \"device_name\": \"" << EscapeJsonString(device_name) << "\",\n";
    json << "    \"device_serial\": \"" << EscapeJsonString(device_serial) << "\",\n";

    // Count totals
    uint32_t total_albums = 0;
    uint32_t total_tracks = 0;

    for (const auto& artist : artists) {
        total_albums += artist.AlbumCount;
        for (uint32_t i = 0; i < artist.AlbumCount; ++i) {
            total_tracks += artist.Albums[i].TrackCount;
        }
    }

    json << "    \"total_artists\": " << artists.size() << ",\n";
    json << "    \"total_albums\": " << total_albums << ",\n";
    json << "    \"total_tracks\": " << total_tracks << "\n";
    json << "  },\n";

    // Start library array
    json << "  \"library\": [\n";

    for (size_t artist_idx = 0; artist_idx < artists.size(); ++artist_idx) {
        const auto& artist = artists[artist_idx];

        json << "    {\n";
        json << "      \"artist_name\": \"" << EscapeJsonString(artist.Name) << "\",\n";
        json << "      \"album_count\": " << artist.AlbumCount << ",\n";
        json << "      \"albums\": [\n";

        for (uint32_t album_idx = 0; album_idx < artist.AlbumCount; ++album_idx) {
            const auto& album = artist.Albums[album_idx];

            json << "        {\n";
            json << "          \"title\": \"" << EscapeJsonString(album.Title) << "\",\n";
            json << "          \"artist\": \"" << EscapeJsonString(album.Artist) << "\",\n";
            json << "          \"year\": " << album.Year << ",\n";
            json << "          \"track_count\": " << album.TrackCount << ",\n";
            json << "          \"tracks\": [\n";

            for (uint32_t track_idx = 0; track_idx < album.TrackCount; ++track_idx) {
                const auto& track = album.Tracks[track_idx];

                json << "            {\n";
                json << "              \"title\": \"" << EscapeJsonString(track.Title) << "\",\n";
                json << "              \"artist\": \"" << EscapeJsonString(track.Artist) << "\",\n";
                json << "              \"album\": \"" << EscapeJsonString(track.Album) << "\",\n";
                json << "              \"track_number\": " << track.TrackNumber << ",\n";
                json << "              \"mtp_object_id\": " << track.MtpObjectId << "\n";
                json << "            }";

                if (track_idx < album.TrackCount - 1) {
                    json << ",";
                }
                json << "\n";
            }

            json << "          ]\n";
            json << "        }";

            if (album_idx < artist.AlbumCount - 1) {
                json << ",";
            }
            json << "\n";
        }

        json << "      ]\n";
        json << "    }";

        if (artist_idx < artists.size() - 1) {
            json << ",";
        }
        json << "\n";
    }

    json << "  ]\n";
    json << "}\n";

    return json.str();
}

// Logging callback
void LogCallback(const std::string& message) {
    std::cout << "[LOG] " << message << std::endl;
}

int main(int argc, char* argv[]) {
    std::string output_file = "zune_library.json";

    // Parse command line arguments
    if (argc > 1) {
        output_file = argv[1];
    }

    std::cout << "================================\n";
    std::cout << "   Zune Device Library Exporter\n";
    std::cout << "================================\n\n";

    std::cout << "Output file: " << output_file << "\n\n";

    // Create device
    std::cout << "Creating Zune device object...\n";
    ZuneDevice device;
    device.SetLogCallback([](const std::string& msg) {
        std::cout << "[DEVICE] " << msg << "\n";
    });

    // Connect
    std::cout << "Connecting to USB device...\n";
    if (!device.ConnectUSB()) {
        std::cerr << "ERROR: Failed to connect to Zune device via USB\n";
        std::cerr << "Please ensure:\n";
        std::cerr << "  1. Device is connected via USB\n";
        std::cerr << "  2. Device is powered on\n";
        std::cerr << "  3. Device has been previously paired with this computer\n";
        return 1;
    }

    std::cout << "Connected successfully!\n\n";

    // Get device info
    std::cout << "Retrieving device information...\n";
    std::string device_name = device.GetName();
    std::string device_serial = device.GetSerialNumber();

    std::cout << "  Device Name: " << device_name << "\n";
    std::cout << "  Serial Number: " << device_serial << "\n\n";

    // Retrieve music library
    std::cout << "Retrieving music library from device...\n";
    std::cout << "This may take a moment...\n\n";

    auto artists = device.GetMusicLibrary();

    std::cout << "Library retrieval complete!\n";
    std::cout << "  Artists: " << artists.size() << "\n";

    uint32_t total_albums = 0;
    uint32_t total_tracks = 0;

    for (const auto& artist : artists) {
        total_albums += artist.AlbumCount;
        for (uint32_t i = 0; i < artist.AlbumCount; ++i) {
            total_tracks += artist.Albums[i].TrackCount;
        }
    }

    std::cout << "  Albums: " << total_albums << "\n";
    std::cout << "  Tracks: " << total_tracks << "\n\n";

    // Print library summary
    std::cout << "Library Summary (First 10 Artists):\n";
    std::cout << "-----------------------------------\n";

    size_t artists_to_print = std::min(size_t(10), artists.size());
    for (size_t i = 0; i < artists_to_print; ++i) {
        const auto& artist = artists[i];
        std::cout << "\n" << (i + 1) << ". " << artist.Name << " ("
                  << artist.AlbumCount << " albums)\n";

        // Show first 3 albums of this artist
        size_t albums_to_print = std::min(size_t(3), size_t(artist.AlbumCount));
        for (uint32_t j = 0; j < albums_to_print; ++j) {
            const auto& album = artist.Albums[j];
            std::cout << "   - " << album.Title << " (" << album.TrackCount << " tracks)\n";
        }

        if (artist.AlbumCount > 3) {
            std::cout << "   ... and " << (artist.AlbumCount - 3) << " more albums\n";
        }
    }

    if (artists.size() > 10) {
        std::cout << "\n... and " << (artists.size() - 10) << " more artists\n";
    }

    std::cout << "\n-----------------------------------\n\n";

    // Generate JSON
    std::cout << "Generating JSON output...\n";
    std::string json_output = BuildLibraryJson(artists, device_name, device_serial);

    // Write to file
    std::cout << "Writing to file: " << output_file << "\n";
    std::ofstream outfile(output_file);
    if (!outfile.is_open()) {
        std::cerr << "ERROR: Failed to open output file for writing\n";
        device.Disconnect();
        return 1;
    }

    outfile << json_output;
    outfile.close();

    std::cout << "File written successfully!\n\n";

    // Cleanup
    std::cout << "Disconnecting device...\n";
    device.Disconnect();

    std::cout << "\n================================\n";
    std::cout << "   Export Complete!\n";
    std::cout << "================================\n";
    std::cout << "Output file: " << output_file << "\n";
    std::cout << "File size: " << std::ifstream(output_file, std::ios::binary | std::ios::ate).tellg()
              << " bytes\n";

    return 0;
}
