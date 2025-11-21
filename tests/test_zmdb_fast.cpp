#include <iostream>
#include <chrono>
#include <iomanip>
#include <fstream>
#include <sstream>
#include <sys/stat.h>
#include <map>
#include <set>
#include "lib/src/ZuneDevice.h"
#include "zune_wireless/zune_wireless_api.h"

// JSON escaping helper
std::string JsonEscape(const std::string& str) {
    std::ostringstream escaped;
    for (char c : str) {
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
                    escaped << c;
                }
        }
    }
    return escaped.str();
}

// Sanitize filename by replacing invalid characters
std::string SanitizeFilename(const std::string& name) {
    std::string sanitized = name;
    const std::string invalid_chars = "/:*?\"<>|\\";

    for (char& c : sanitized) {
        if (invalid_chars.find(c) != std::string::npos) {
            c = '_';
        }
    }

    // Trim to reasonable length
    if (sanitized.length() > 100) {
        sanitized = sanitized.substr(0, 100);
    }

    return sanitized;
}

// Create directory if it doesn't exist
bool CreateDirectory(const std::string& path) {
    struct stat st;
    if (stat(path.c_str(), &st) == 0) {
        return S_ISDIR(st.st_mode);
    }
    return mkdir(path.c_str(), 0755) == 0;
}

void PrintLibrarySummary(ZuneMusicLibrary* library, const std::string& label) {
    // Count unique artists
    std::set<std::string> unique_artists;
    for (uint32_t i = 0; i < library->track_count; ++i) {
        unique_artists.insert(library->tracks[i].artist_name);
    }

    std::cout << "\n" << label << ":\n";
    std::cout << "  Artists: " << unique_artists.size() << "\n";
    std::cout << "  Albums: " << library->album_count << "\n";
    std::cout << "  Tracks: " << library->track_count << "\n";
    std::cout << "  Artworks: " << library->artwork_count << "\n";
}

void PrintFirstArtists(ZuneMusicLibrary* library, int count = 5) {
    // Group tracks by artist
    std::map<std::string, std::set<std::string>> artist_albums;
    for (uint32_t i = 0; i < library->track_count; ++i) {
        const auto& track = library->tracks[i];
        artist_albums[track.artist_name].insert(track.album_name);
    }

    std::cout << "\nFirst " << std::min(count, (int)artist_albums.size()) << " Artists:\n";
    std::cout << "-----------------------------------\n";

    int idx = 0;
    for (const auto& [artist_name, albums] : artist_albums) {
        if (idx >= count) break;

        std::cout << (idx + 1) << ". " << artist_name
                  << " (" << albums.size() << " albums)\n";

        // Show first 2 albums
        int album_idx = 0;
        for (const auto& album_name : albums) {
            if (album_idx >= 2) break;
            std::cout << "   - " << album_name << "\n";
            album_idx++;
        }

        if (albums.size() > 2) {
            std::cout << "   ... and " << (albums.size() - 2) << " more\n";
        }

        idx++;
    }
}

bool ExportLibraryToJson(ZuneMusicLibrary* library, const std::string& output_file, const std::string& artwork_dir = "") {
    std::cout << "\n========================================\n";
    std::cout << "Exporting Library to JSON\n";
    std::cout << "========================================\n";

    std::ofstream json_file(output_file);
    if (!json_file.is_open()) {
        std::cerr << "ERROR: Failed to create JSON file: " << output_file << "\n";
        return false;
    }

    // Build artwork map
    std::map<std::string, uint32_t> artwork_map;
    for (uint32_t i = 0; i < library->artwork_count; ++i) {
        artwork_map[library->artworks[i].alb_reference] = library->artworks[i].mtp_object_id;
    }

    // Build album map
    std::map<uint32_t, const ZuneMusicAlbum*> album_map;
    for (uint32_t i = 0; i < library->album_count; ++i) {
        album_map[library->albums[i].atom_id] = &library->albums[i];
    }

    // Group tracks by artist -> album
    std::map<std::string, std::map<std::string, std::vector<const ZuneMusicTrack*>>> grouped;
    for (uint32_t i = 0; i < library->track_count; ++i) {
        const auto& track = library->tracks[i];
        grouped[track.artist_name][track.album_name].push_back(&track);
    }

    json_file << "{\n";
    json_file << "  \"artists\": [\n";

    size_t artist_idx = 0;
    for (const auto& [artist_name, albums] : grouped) {
        json_file << "    {\n";
        json_file << "      \"name\": \"" << JsonEscape(artist_name) << "\",\n";
        json_file << "      \"albumCount\": " << albums.size() << ",\n";
        json_file << "      \"albums\": [\n";

        size_t album_idx = 0;
        for (const auto& [album_name, tracks] : albums) {
            json_file << "        {\n";
            json_file << "          \"title\": \"" << JsonEscape(album_name) << "\",\n";
            json_file << "          \"artist\": \"" << JsonEscape(artist_name) << "\",\n";

            // Get album metadata if available
            if (!tracks.empty() && album_map.count(tracks[0]->album_ref)) {
                const auto* album = album_map[tracks[0]->album_ref];
                json_file << "          \"year\": " << album->release_year << ",\n";

                // Get artwork ObjectId
                auto artwork_it = artwork_map.find(album->alb_reference);
                uint32_t artwork_id = (artwork_it != artwork_map.end()) ? artwork_it->second : 0;
                json_file << "          \"artworkObjectId\": " << artwork_id << ",\n";

                // Add artwork path if artwork directory is specified
                if (!artwork_dir.empty() && artwork_id != 0) {
                    std::string artist_sanitized = SanitizeFilename(artist_name);
                    std::string album_sanitized = SanitizeFilename(album_name);
                    std::string artwork_filename = artist_sanitized + " - " + album_sanitized + ".jpg";
                    std::string artwork_path = artwork_dir + "/" + artwork_filename;
                    json_file << "          \"artworkPath\": \"" << JsonEscape(artwork_path) << "\",\n";
                }
            }

            json_file << "          \"trackCount\": " << tracks.size() << ",\n";
            json_file << "          \"tracks\": [\n";

            for (size_t track_idx = 0; track_idx < tracks.size(); ++track_idx) {
                const auto* track = tracks[track_idx];

                json_file << "            {\n";
                json_file << "              \"title\": \"" << JsonEscape(track->title) << "\",\n";
                json_file << "              \"artist\": \"" << JsonEscape(track->artist_name) << "\",\n";
                json_file << "              \"artistGuid\": \"" << JsonEscape(track->artist_guid) << "\",\n";
                json_file << "              \"album\": \"" << JsonEscape(track->album_name) << "\",\n";
                json_file << "              \"albumArtist\": \"" << JsonEscape(track->album_artist_name) << "\",\n";
                json_file << "              \"albumArtistGuid\": \"" << JsonEscape(track->album_artist_guid) << "\",\n";
                json_file << "              \"trackNumber\": " << track->track_number << ",\n";
                json_file << "              \"duration_ms\": " << track->duration_ms << ",\n";
                json_file << "              \"filename\": \"" << JsonEscape(track->filename) << "\"\n";
                json_file << "            }" << (track_idx < tracks.size() - 1 ? "," : "") << "\n";
            }

            json_file << "          ]\n";
            json_file << "        }" << (album_idx < albums.size() - 1 ? "," : "") << "\n";
            album_idx++;
        }

        json_file << "      ]\n";
        json_file << "    }" << (artist_idx < grouped.size() - 1 ? "," : "") << "\n";
        artist_idx++;
    }

    json_file << "  ]\n";
    json_file << "}\n";

    json_file.close();

    std::cout << "✓ Exported library to: " << output_file << "\n";

    std::set<std::string> unique_artists;
    for (uint32_t i = 0; i < library->track_count; ++i) {
        unique_artists.insert(library->tracks[i].artist_name);
    }

    std::cout << "  Artists: " << unique_artists.size() << "\n";
    std::cout << "  Albums: " << library->album_count << "\n";
    std::cout << "  Tracks: " << library->track_count << "\n";

    return true;
}

int DownloadAllArtwork(ZuneDevice& device, ZuneMusicLibrary* library, const std::string& output_dir) {
    std::cout << "\n========================================\n";
    std::cout << "Downloading Album Artwork\n";
    std::cout << "========================================\n";

    // Create output directory
    if (!CreateDirectory(output_dir)) {
        std::cerr << "ERROR: Failed to create directory: " << output_dir << "\n";
        return -1;
    }

    std::cout << "Output directory: " << output_dir << "\n\n";

    int downloaded = 0;
    int skipped = 0;
    int failed = 0;
    auto start_time = std::chrono::high_resolution_clock::now();

    // Build artwork map
    std::map<std::string, uint32_t> artwork_map;
    for (uint32_t i = 0; i < library->artwork_count; ++i) {
        artwork_map[library->artworks[i].alb_reference] = library->artworks[i].mtp_object_id;
    }

    // Download artwork for each album
    for (uint32_t i = 0; i < library->album_count; ++i) {
        const auto& album = library->albums[i];

        // Get artwork ObjectId
        auto artwork_it = artwork_map.find(album.alb_reference);
        if (artwork_it == artwork_map.end() || artwork_it->second == 0) {
            skipped++;
            continue;
        }

        uint32_t artwork_id = artwork_it->second;

        // Create filename: "Artist - Album.jpg"
        std::string artist_name = SanitizeFilename(album.artist_name);
        std::string album_title = SanitizeFilename(album.title);
        std::string filename = artist_name + " - " + album_title + ".jpg";
        std::string filepath = output_dir + "/" + filename;

        std::cout << "Downloading: " << album.artist_name << " - " << album.title << "\n";
        std::cout << "  ObjectId: " << artwork_id << "\n";
        std::cout << "  File: " << filename << "\n";

        int result = device.DownloadFile(artwork_id, filepath);

        if (result == 0) {
            downloaded++;
            std::cout << "  ✓ Downloaded successfully\n\n";
        } else {
            failed++;
            std::cout << "  ✗ Download failed (error code: " << result << ")\n\n";
        }
    }

    auto end_time = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time);

    std::cout << "========================================\n";
    std::cout << "Artwork Download Summary:\n";
    std::cout << "========================================\n";
    std::cout << "  Downloaded: " << downloaded << "\n";
    std::cout << "  Skipped: " << skipped << " (no artwork ObjectId)\n";
    std::cout << "  Failed: " << failed << "\n";
    std::cout << "  Total time: " << duration.count() << " ms\n";

    if (downloaded > 0) {
        std::cout << "  Avg time per download: " << (duration.count() / downloaded) << " ms\n";
    }

    return downloaded;
}

int main() {
    std::cout << "========================================\n";
    std::cout << "   Zune Fast Library Test (zmdb)\n";
    std::cout << "========================================\n\n";

    // Create device
    std::cout << "Creating Zune device object...\n";
    ZuneDevice device;

    device.SetLogCallback([](const std::string& msg) {
        std::cout << "[DEVICE] " << msg << "\n";
    });

    // Connect
    std::cout << "Connecting to USB device...\n";
    if (!device.ConnectUSB()) {
        std::cerr << "\nERROR: Failed to connect to Zune device via USB\n";
        std::cerr << "Please ensure:\n";
        std::cerr << "  1. Device is connected via USB\n";
        std::cerr << "  2. Device is powered on\n";
        std::cerr << "  3. You have necessary permissions\n";
        return 1;
    }

    std::cout << "\n✓ Connected successfully!\n\n";

    // Get device info
    std::string device_name = device.GetName();
    std::string device_serial = device.GetSerialNumber();

    std::cout << "Device Information:\n";
    std::cout << "  Name: " << device_name << "\n";
    std::cout << "  Serial: " << device_serial << "\n";

    // Test FAST method (zmdb extraction)
    std::cout << "\n========================================\n";
    std::cout << "Testing FAST method (zmdb extraction)\n";
    std::cout << "========================================\n";

    auto start_fast = std::chrono::high_resolution_clock::now();
    ZuneMusicLibrary* library_fast = device.GetMusicLibrary();
    auto end_fast = std::chrono::high_resolution_clock::now();

    if (!library_fast) {
        std::cerr << "ERROR: Failed to retrieve music library\n";
        device.Disconnect();
        return 1;
    }

    auto duration_fast = std::chrono::duration_cast<std::chrono::milliseconds>(end_fast - start_fast);

    std::cout << "\n⏱  Fast method completed in: " << duration_fast.count() << " ms\n";

    PrintLibrarySummary(library_fast, "Fast Method Results");
    PrintFirstArtists(library_fast, 5);

    // Export library to JSON (without artwork paths initially)
    ExportLibraryToJson(library_fast, "library.json");

    // Download artwork
    std::cout << "\n========================================\n";
    std::cout << "Download album artwork? (y/n): ";

    std::string artwork_response;
    std::getline(std::cin, artwork_response);

    if (artwork_response == "y" || artwork_response == "Y") {
        std::string artwork_dir = "artwork";
        int downloaded = DownloadAllArtwork(device, library_fast, artwork_dir);

        // Re-export JSON with artwork paths
        if (downloaded > 0) {
            ExportLibraryToJson(library_fast, "library_with_artwork.json", artwork_dir);
        }
    }

    // Optional: Compare with slow method
    std::cout << "\n========================================\n";
    std::cout << "Compare with SLOW method? (y/n): ";

    std::string response;
    std::getline(std::cin, response);

    ZuneMusicLibrary* library_slow = nullptr;
    if (response == "y" || response == "Y") {
        std::cout << "\nTesting SLOW method (AFTL iteration)\n";
        std::cout << "This may take a while...\n";

        auto start_slow = std::chrono::high_resolution_clock::now();
        library_slow = device.GetMusicLibrarySlow();
        auto end_slow = std::chrono::high_resolution_clock::now();

        if (library_slow) {
            auto duration_slow = std::chrono::duration_cast<std::chrono::milliseconds>(end_slow - start_slow);

            std::cout << "\n⏱  Slow method completed in: " << duration_slow.count() << " ms\n";

            PrintLibrarySummary(library_slow, "Slow Method Results");

            // Performance comparison
            std::cout << "\n========================================\n";
            std::cout << "Performance Comparison:\n";
            std::cout << "========================================\n";
            std::cout << "  Fast method: " << duration_fast.count() << " ms\n";
            std::cout << "  Slow method: " << duration_slow.count() << " ms\n";

            if (duration_slow.count() > 0) {
                double speedup = (double)duration_slow.count() / duration_fast.count();
                std::cout << "  Speedup: " << std::fixed << std::setprecision(1)
                          << speedup << "x faster\n";
            }
        }
    }

    // Cleanup
    std::cout << "\n========================================\n";
    std::cout << "Freeing library memory...\n";
    zune_device_free_music_library(library_fast);
    if (library_slow) {
        zune_device_free_music_library(library_slow);
    }

    std::cout << "Disconnecting device...\n";
    device.Disconnect();

    std::cout << "\n✓ Test complete!\n";
    std::cout << "========================================\n";

    return 0;
}
