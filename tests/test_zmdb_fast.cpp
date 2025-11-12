#include <iostream>
#include <chrono>
#include <iomanip>
#include <fstream>
#include <sstream>
#include <sys/stat.h>
#include "lib/src/ZuneDevice.h"

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

void PrintLibrarySummary(const std::vector<ZuneArtistInfo>& artists, const std::string& label) {
    uint32_t total_albums = 0;
    uint32_t total_tracks = 0;
    uint32_t tracks_with_mtp_id = 0;

    for (const auto& artist : artists) {
        total_albums += artist.AlbumCount;
        for (uint32_t i = 0; i < artist.AlbumCount; ++i) {
            total_tracks += artist.Albums[i].TrackCount;
            for (uint32_t j = 0; j < artist.Albums[i].TrackCount; ++j) {
                if (artist.Albums[i].Tracks[j].MtpObjectId != 0) {
                    tracks_with_mtp_id++;
                }
            }
        }
    }

    std::cout << "\n" << label << ":\n";
    std::cout << "  Artists: " << artists.size() << "\n";
    std::cout << "  Albums: " << total_albums << "\n";
    std::cout << "  Tracks: " << total_tracks << "\n";
    std::cout << "  Tracks with MTP ID: " << tracks_with_mtp_id
              << " (" << std::fixed << std::setprecision(1)
              << (total_tracks > 0 ? (100.0 * tracks_with_mtp_id / total_tracks) : 0.0)
              << "%)\n";
}

void PrintFirstArtists(const std::vector<ZuneArtistInfo>& artists, int count = 5) {
    std::cout << "\nFirst " << std::min(count, (int)artists.size()) << " Artists:\n";
    std::cout << "-----------------------------------\n";

    for (int i = 0; i < std::min(count, (int)artists.size()); ++i) {
        const auto& artist = artists[i];
        std::cout << (i + 1) << ". " << artist.Name
                  << " (" << artist.AlbumCount << " albums)\n";

        // Show first 2 albums
        int albums_to_show = std::min(2, (int)artist.AlbumCount);
        for (int j = 0; j < albums_to_show; ++j) {
            const auto& album = artist.Albums[j];
            std::cout << "   - " << album.Title
                      << " (" << album.Year << ") - "
                      << album.TrackCount << " tracks\n";
        }

        if (artist.AlbumCount > 2) {
            std::cout << "   ... and " << (artist.AlbumCount - 2) << " more\n";
        }
    }
}

bool ExportLibraryToJson(const std::vector<ZuneArtistInfo>& artists, const std::string& output_file, const std::string& artwork_dir = "") {
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

    for (size_t i = 0; i < artists.size(); ++i) {
        const auto& artist = artists[i];

        json_file << "    {\n";
        json_file << "      \"name\": \"" << JsonEscape(artist.Name) << "\",\n";
        json_file << "      \"albumCount\": " << artist.AlbumCount << ",\n";
        json_file << "      \"albums\": [\n";

        for (uint32_t j = 0; j < artist.AlbumCount; ++j) {
            const auto& album = artist.Albums[j];

            json_file << "        {\n";
            json_file << "          \"title\": \"" << JsonEscape(album.Title) << "\",\n";
            json_file << "          \"artist\": \"" << JsonEscape(album.Artist) << "\",\n";
            json_file << "          \"year\": " << album.Year << ",\n";
            json_file << "          \"artworkObjectId\": " << album.ArtworkObjectId << ",\n";

            // Add artwork path if artwork directory is specified
            if (!artwork_dir.empty() && album.ArtworkObjectId != 0) {
                std::string artist_name = SanitizeFilename(album.Artist);
                std::string album_title = SanitizeFilename(album.Title);
                std::string artwork_filename = artist_name + " - " + album_title + ".jpg";
                std::string artwork_path = artwork_dir + "/" + artwork_filename;
                json_file << "          \"artworkPath\": \"" << JsonEscape(artwork_path) << "\",\n";
            }

            json_file << "          \"trackCount\": " << album.TrackCount << ",\n";
            json_file << "          \"tracks\": [\n";

            for (uint32_t k = 0; k < album.TrackCount; ++k) {
                const auto& track = album.Tracks[k];

                json_file << "            {\n";
                json_file << "              \"title\": \"" << JsonEscape(track.Title) << "\",\n";
                json_file << "              \"artist\": \"" << JsonEscape(track.Artist) << "\",\n";
                json_file << "              \"album\": \"" << JsonEscape(track.Album) << "\",\n";
                json_file << "              \"trackNumber\": " << track.TrackNumber << ",\n";
                json_file << "              \"mtpObjectId\": " << track.MtpObjectId << "\n";
                json_file << "            }" << (k < album.TrackCount - 1 ? "," : "") << "\n";
            }

            json_file << "          ]\n";
            json_file << "        }" << (j < artist.AlbumCount - 1 ? "," : "") << "\n";
        }

        json_file << "      ]\n";
        json_file << "    }" << (i < artists.size() - 1 ? "," : "") << "\n";
    }

    json_file << "  ]\n";
    json_file << "}\n";

    json_file.close();

    std::cout << "✓ Exported library to: " << output_file << "\n";
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
    std::cout << "  Tracks: " << total_tracks << "\n";

    return true;
}

int DownloadAllArtwork(ZuneDevice& device, const std::vector<ZuneArtistInfo>& artists, const std::string& output_dir) {
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

    for (const auto& artist : artists) {
        for (uint32_t i = 0; i < artist.AlbumCount; ++i) {
            const auto& album = artist.Albums[i];

            if (album.ArtworkObjectId == 0) {
                skipped++;
                continue;
            }

            // Create filename: "Artist - Album.jpg"
            std::string artist_name = SanitizeFilename(artist.Name);
            std::string album_title = SanitizeFilename(album.Title);
            std::string filename = artist_name + " - " + album_title + ".jpg";
            std::string filepath = output_dir + "/" + filename;

            std::cout << "Downloading: " << artist.Name << " - " << album.Title << "\n";
            std::cout << "  ObjectId: " << album.ArtworkObjectId << "\n";
            std::cout << "  File: " << filename << "\n";

            int result = device.DownloadFile(album.ArtworkObjectId, filepath);

            if (result == 0) {
                downloaded++;
                std::cout << "  ✓ Downloaded successfully\n\n";
            } else {
                failed++;
                std::cout << "  ✗ Download failed (error code: " << result << ")\n\n";
            }
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
    auto artists_fast = device.GetMusicLibraryFast();
    auto end_fast = std::chrono::high_resolution_clock::now();

    auto duration_fast = std::chrono::duration_cast<std::chrono::milliseconds>(end_fast - start_fast);

    std::cout << "\n⏱  Fast method completed in: " << duration_fast.count() << " ms\n";

    PrintLibrarySummary(artists_fast, "Fast Method Results");
    PrintFirstArtists(artists_fast, 5);

    // Export library to JSON (without artwork paths initially)
    ExportLibraryToJson(artists_fast, "library.json");

    // Download artwork
    std::cout << "\n========================================\n";
    std::cout << "Download album artwork? (y/n): ";

    std::string artwork_response;
    std::getline(std::cin, artwork_response);

    if (artwork_response == "y" || artwork_response == "Y") {
        std::string artwork_dir = "artwork";
        int downloaded = DownloadAllArtwork(device, artists_fast, artwork_dir);

        // Re-export JSON with artwork paths
        if (downloaded > 0) {
            ExportLibraryToJson(artists_fast, "library_with_artwork.json", artwork_dir);
        }
    }

    // Optional: Compare with slow method
    std::cout << "\n========================================\n";
    std::cout << "Compare with SLOW method? (y/n): ";

    std::string response;
    std::getline(std::cin, response);

    if (response == "y" || response == "Y") {
        std::cout << "\nTesting SLOW method (AFTL iteration)\n";
        std::cout << "This may take a while...\n";

        auto start_slow = std::chrono::high_resolution_clock::now();
        auto artists_slow = device.GetMusicLibrary();
        auto end_slow = std::chrono::high_resolution_clock::now();

        auto duration_slow = std::chrono::duration_cast<std::chrono::milliseconds>(end_slow - start_slow);

        std::cout << "\n⏱  Slow method completed in: " << duration_slow.count() << " ms\n";

        PrintLibrarySummary(artists_slow, "Slow Method Results");

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

    // Cleanup
    std::cout << "\n========================================\n";
    std::cout << "Disconnecting device...\n";
    device.Disconnect();

    std::cout << "\n✓ Test complete!\n";
    std::cout << "========================================\n";

    return 0;
}
