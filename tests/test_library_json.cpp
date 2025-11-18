#include <iostream>
#include <fstream>
#include <sstream>
#include <vector>
#include <string>
#include <iomanip>
#include <cstring>
#include <cstdio>
#include <chrono>
#include <map>
#include <set>

#include "lib/src/ZuneDevice.h"
#include "zune_wireless/zune_wireless_api.h"

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

// Build JSON output from flat library data
std::string BuildLibraryJson(ZuneMusicLibrary* library,
                              const std::string& device_name = "",
                              const std::string& device_serial = "") {
    std::ostringstream json;

    // Group tracks by artist -> album
    std::map<std::string, std::map<std::string, std::vector<const ZuneMusicTrack*>>> grouped;

    for (uint32_t i = 0; i < library->track_count; ++i) {
        const auto& track = library->tracks[i];
        grouped[track.artist_name][track.album_name].push_back(&track);
    }

    // Start JSON object
    json << "{\n";
    json << "  \"metadata\": {\n";
    json << "    \"generated\": \"" << GetTimestamp() << "\",\n";
    json << "    \"device_name\": \"" << EscapeJsonString(device_name) << "\",\n";
    json << "    \"device_serial\": \"" << EscapeJsonString(device_serial) << "\",\n";
    json << "    \"total_artists\": " << grouped.size() << ",\n";
    json << "    \"total_albums\": " << library->album_count << ",\n";
    json << "    \"total_tracks\": " << library->track_count << "\n";
    json << "  },\n";

    // Start library array
    json << "  \"library\": [\n";

    size_t artist_idx = 0;
    for (const auto& [artist_name, albums] : grouped) {
        json << "    {\n";
        json << "      \"artist_name\": \"" << EscapeJsonString(artist_name) << "\",\n";
        json << "      \"album_count\": " << albums.size() << ",\n";
        json << "      \"albums\": [\n";

        size_t album_idx = 0;
        for (const auto& [album_name, tracks] : albums) {
            json << "        {\n";
            json << "          \"title\": \"" << EscapeJsonString(album_name) << "\",\n";
            json << "          \"artist\": \"" << EscapeJsonString(artist_name) << "\",\n";
            json << "          \"track_count\": " << tracks.size() << ",\n";
            json << "          \"tracks\": [\n";

            for (size_t track_idx = 0; track_idx < tracks.size(); ++track_idx) {
                const auto* track = tracks[track_idx];

                json << "            {\n";
                json << "              \"title\": \"" << EscapeJsonString(track->title) << "\",\n";
                json << "              \"artist\": \"" << EscapeJsonString(track->artist_name) << "\",\n";
                json << "              \"album\": \"" << EscapeJsonString(track->album_name) << "\",\n";
                json << "              \"track_number\": " << track->track_number << ",\n";
                json << "              \"duration_ms\": " << track->duration_ms << ",\n";
                json << "              \"filename\": \"" << EscapeJsonString(track->filename) << "\"\n";
                json << "            }";

                if (track_idx < tracks.size() - 1) {
                    json << ",";
                }
                json << "\n";
            }

            json << "          ]\n";
            json << "        }";

            if (album_idx < albums.size() - 1) {
                json << ",";
            }
            json << "\n";
            album_idx++;
        }

        json << "      ]\n";
        json << "    }";

        if (artist_idx < grouped.size() - 1) {
            json << ",";
        }
        json << "\n";
        artist_idx++;
    }

    json << "  ]\n";
    json << "}\n";

    return json.str();
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

    ZuneMusicLibrary* library = device.GetMusicLibrary();

    if (!library) {
        std::cerr << "ERROR: Failed to retrieve music library\n";
        device.Disconnect();
        return 1;
    }

    std::cout << "Library retrieval complete!\n";
    std::cout << "  Tracks: " << library->track_count << "\n";
    std::cout << "  Albums: " << library->album_count << "\n";
    std::cout << "  Artworks: " << library->artwork_count << "\n\n";

    // Group for summary display
    std::map<std::string, std::set<std::string>> artist_albums;
    for (uint32_t i = 0; i < library->track_count; ++i) {
        const auto& track = library->tracks[i];
        artist_albums[track.artist_name].insert(track.album_name);
    }

    // Print library summary
    std::cout << "Library Summary (First 10 Artists):\n";
    std::cout << "-----------------------------------\n";

    size_t count = 0;
    for (const auto& [artist_name, albums] : artist_albums) {
        if (count >= 10) break;

        std::cout << "\n" << (count + 1) << ". " << artist_name << " (" << albums.size() << " albums)\n";

        // Show first 3 albums
        size_t album_count = 0;
        for (const auto& album_name : albums) {
            if (album_count >= 3) break;
            std::cout << "   - " << album_name << "\n";
            album_count++;
        }

        if (albums.size() > 3) {
            std::cout << "   ... and " << (albums.size() - 3) << " more albums\n";
        }

        count++;
    }

    if (artist_albums.size() > 10) {
        std::cout << "\n... and " << (artist_albums.size() - 10) << " more artists\n";
    }

    std::cout << "\n-----------------------------------\n\n";

    // Generate JSON
    std::cout << "Generating JSON output...\n";
    std::string json_output = BuildLibraryJson(library, device_name, device_serial);

    // Write to file
    std::cout << "Writing to file: " << output_file << "\n";
    std::ofstream outfile(output_file);
    if (!outfile.is_open()) {
        std::cerr << "ERROR: Failed to open output file for writing\n";
        zune_device_free_music_library(library);
        device.Disconnect();
        return 1;
    }

    outfile << json_output;
    outfile.close();

    std::cout << "File written successfully!\n\n";

    // Cleanup
    std::cout << "Freeing library memory...\n";
    zune_device_free_music_library(library);

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
