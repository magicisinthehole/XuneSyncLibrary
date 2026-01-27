/**
 * Playlist Test CLI - Tests playlist create/update/delete operations
 *
 * This tool verifies the playlist management MTP operations work correctly
 * with a connected Zune device.
 *
 * Usage:
 *   playlist_test_cli
 *
 * Operations tested:
 *   1. Create playlist with tracks
 *   2. Update playlist (add/remove tracks)
 *   3. Delete playlist
 */

#include "lib/src/ZuneDevice.h"
#include "zune_wireless/zune_wireless_api.h"
#include <iostream>
#include <iomanip>
#include <chrono>
#include <vector>
#include <string>
#include <random>

void log_callback(const std::string& message) {
    auto now = std::chrono::system_clock::now();
    auto time = std::chrono::system_clock::to_time_t(now);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()) % 1000;

    std::cout << "[" << std::put_time(std::localtime(&time), "%H:%M:%S")
              << "." << std::setfill('0') << std::setw(3) << ms.count() << "] "
              << message << std::endl;
}

std::string trim(const std::string& str) {
    size_t first = str.find_first_not_of(" \t\n\r\f\v");
    if (first == std::string::npos) return "";
    size_t last = str.find_last_not_of(" \t\n\r\f\v");
    return str.substr(first, last - first + 1);
}

std::string read_input(const std::string& prompt = "") {
    if (!prompt.empty()) {
        std::cout << prompt;
    }
    std::string input;
    std::getline(std::cin, input);
    return trim(input);
}

bool read_yes_no(const std::string& prompt) {
    while (true) {
        std::string input = read_input(prompt + " (y/n): ");
        if (input == "y" || input == "Y") return true;
        if (input == "n" || input == "N") return false;
        std::cout << "Invalid input. Please enter 'y' or 'n'." << std::endl;
    }
}

// Generate a random GUID string
std::string generate_guid() {
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> dis(0, 15);

    const char* hex = "0123456789abcdef";
    std::string guid;

    // Format: xxxxxxxx-xxxx-4xxx-yxxx-xxxxxxxxxxxx
    for (int i = 0; i < 36; ++i) {
        if (i == 8 || i == 13 || i == 18 || i == 23) {
            guid += '-';
        } else if (i == 14) {
            guid += '4';  // Version 4 UUID
        } else if (i == 19) {
            guid += hex[(dis(gen) & 0x3) | 0x8];  // Variant
        } else {
            guid += hex[dis(gen)];
        }
    }

    return guid;
}

int main() {
    std::cout << "=== Playlist Test CLI ===" << std::endl;
    std::cout << std::endl;

    // Step 1: Connect to device
    std::cout << "Connecting to device via USB..." << std::endl;
    ZuneDevice device;
    device.SetLogCallback(log_callback);

    if (!device.ConnectUSB()) {
        std::cerr << "ERROR: Failed to connect to device via USB" << std::endl;
        std::cerr << "Make sure a Zune device is connected." << std::endl;
        return 1;
    }

    std::cout << "Connected to: " << device.GetName() << std::endl;
    std::cout << "Model: " << device.GetModel() << std::endl;
    std::cout << std::endl;

    // Step 2: Get music library to find track IDs
    std::cout << "Retrieving music library..." << std::endl;
    ZuneMusicLibrary* library = device.GetMusicLibrary();

    if (!library || library->track_count == 0) {
        std::cerr << "ERROR: No tracks found on device." << std::endl;
        std::cerr << "Upload some music first to test playlist operations." << std::endl;
        if (library) zune_device_free_music_library(library);
        device.Disconnect();
        return 1;
    }

    std::cout << "Found " << library->track_count << " tracks on device." << std::endl;
    std::cout << std::endl;

    // Display first 10 tracks
    std::cout << "Sample tracks (first 10):" << std::endl;
    uint32_t display_count = std::min(library->track_count, 10u);
    for (uint32_t i = 0; i < display_count; ++i) {
        std::cout << "  " << (i + 1) << ". " << library->tracks[i].title
                  << " - " << library->tracks[i].artist_name
                  << " [atom_id: " << library->tracks[i].atom_id << "]" << std::endl;
    }
    std::cout << std::endl;

    // Collect track atom IDs for testing
    std::vector<uint32_t> all_track_ids;
    for (uint32_t i = 0; i < library->track_count; ++i) {
        if (library->tracks[i].atom_id != 0) {
            all_track_ids.push_back(library->tracks[i].atom_id);
        }
    }

    if (all_track_ids.size() < 2) {
        std::cerr << "ERROR: Need at least 2 tracks with valid atom_ids." << std::endl;
        zune_device_free_music_library(library);
        device.Disconnect();
        return 1;
    }

    // Free library - we have what we need
    zune_device_free_music_library(library);
    library = nullptr;

    std::cout << "Found " << all_track_ids.size() << " tracks with valid atom IDs." << std::endl;
    std::cout << std::endl;

    // ===== TEST 1: Create Playlist =====
    std::cout << "========================================" << std::endl;
    std::cout << "TEST 1: Create Playlist" << std::endl;
    std::cout << "========================================" << std::endl;

    std::string playlist_name = "Test Playlist " + std::to_string(std::time(nullptr) % 10000);
    std::string playlist_guid = generate_guid();

    // Use first 3 tracks (or all if less than 3)
    std::vector<uint32_t> initial_tracks;
    for (size_t i = 0; i < std::min(all_track_ids.size(), size_t(3)); ++i) {
        initial_tracks.push_back(all_track_ids[i]);
    }

    std::cout << "Creating playlist: " << playlist_name << std::endl;
    std::cout << "GUID: " << playlist_guid << std::endl;
    std::cout << "Initial tracks: " << initial_tracks.size() << std::endl;
    for (auto id : initial_tracks) {
        std::cout << "  - atom_id: " << id << std::endl;
    }
    std::cout << std::endl;

    if (!read_yes_no("Proceed with playlist creation?")) {
        std::cout << "Test cancelled." << std::endl;
        device.Disconnect();
        return 0;
    }

    uint32_t playlist_id = device.CreatePlaylist(playlist_name, playlist_guid, initial_tracks);

    if (playlist_id == 0) {
        std::cerr << "FAILED: CreatePlaylist returned 0" << std::endl;
        device.Disconnect();
        return 1;
    }

    std::cout << "SUCCESS: Playlist created with MTP ObjectId: " << playlist_id << std::endl;
    std::cout << std::endl;

    // ===== TEST 2: Update Playlist (add more tracks) =====
    std::cout << "========================================" << std::endl;
    std::cout << "TEST 2: Update Playlist Tracks" << std::endl;
    std::cout << "========================================" << std::endl;

    // Add more tracks (use up to 5 total)
    std::vector<uint32_t> updated_tracks;
    for (size_t i = 0; i < std::min(all_track_ids.size(), size_t(5)); ++i) {
        updated_tracks.push_back(all_track_ids[i]);
    }

    std::cout << "Updating playlist to have " << updated_tracks.size() << " tracks" << std::endl;
    for (auto id : updated_tracks) {
        std::cout << "  - atom_id: " << id << std::endl;
    }
    std::cout << std::endl;

    if (!read_yes_no("Proceed with playlist update?")) {
        std::cout << "Skipping update test." << std::endl;
    } else {
        bool update_result = device.UpdatePlaylistTracks(playlist_id, updated_tracks);

        if (!update_result) {
            std::cerr << "FAILED: UpdatePlaylistTracks returned false" << std::endl;
        } else {
            std::cout << "SUCCESS: Playlist tracks updated" << std::endl;
        }
    }
    std::cout << std::endl;

    // ===== TEST 3: Update Playlist (reduce tracks) =====
    std::cout << "========================================" << std::endl;
    std::cout << "TEST 3: Update Playlist (reduce tracks)" << std::endl;
    std::cout << "========================================" << std::endl;

    // Reduce to just 1 track
    std::vector<uint32_t> reduced_tracks = { all_track_ids[0] };

    std::cout << "Reducing playlist to " << reduced_tracks.size() << " track" << std::endl;
    std::cout << "  - atom_id: " << reduced_tracks[0] << std::endl;
    std::cout << std::endl;

    if (!read_yes_no("Proceed with track reduction?")) {
        std::cout << "Skipping reduction test." << std::endl;
    } else {
        bool reduce_result = device.UpdatePlaylistTracks(playlist_id, reduced_tracks);

        if (!reduce_result) {
            std::cerr << "FAILED: UpdatePlaylistTracks (reduce) returned false" << std::endl;
        } else {
            std::cout << "SUCCESS: Playlist tracks reduced" << std::endl;
        }
    }
    std::cout << std::endl;

    // ===== TEST 4: Delete Playlist =====
    std::cout << "========================================" << std::endl;
    std::cout << "TEST 4: Delete Playlist" << std::endl;
    std::cout << "========================================" << std::endl;

    std::cout << "Deleting playlist with MTP ObjectId: " << playlist_id << std::endl;
    std::cout << std::endl;

    if (!read_yes_no("Proceed with playlist deletion?")) {
        std::cout << "Skipping deletion (playlist will remain on device)." << std::endl;
    } else {
        bool delete_result = device.DeletePlaylist(playlist_id);

        if (!delete_result) {
            std::cerr << "FAILED: DeletePlaylist returned false" << std::endl;
        } else {
            std::cout << "SUCCESS: Playlist deleted" << std::endl;
        }
    }
    std::cout << std::endl;

    // ===== Summary =====
    std::cout << "========================================" << std::endl;
    std::cout << "Test Complete" << std::endl;
    std::cout << "========================================" << std::endl;
    std::cout << std::endl;
    std::cout << "Verify results by checking the device:" << std::endl;
    std::cout << "  - If deletion was skipped, playlist should appear on device" << std::endl;
    std::cout << "  - If all tests passed, playlist operations are working" << std::endl;
    std::cout << std::endl;

    // Cleanup
    device.Disconnect();
    std::cout << "Disconnected from device." << std::endl;

    return 0;
}
