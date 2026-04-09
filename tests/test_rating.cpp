#include "lib/src/ZuneDevice.h"
#include <iostream>
#include <iomanip>
#include <thread>
#include <chrono>
#include <csignal>
#include <map>

// Global flag for clean shutdown
static volatile bool g_running = true;

void signal_handler(int signal) {
    std::cout << "\nReceived signal " << signal << ", shutting down..." << std::endl;
    g_running = false;
}

void log_callback(const std::string& message) {
    auto now = std::chrono::system_clock::now();
    auto time = std::chrono::system_clock::to_time_t(now);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()) % 1000;

    std::cout << "[" << std::put_time(std::localtime(&time), "%H:%M:%S")
              << "." << std::setfill('0') << std::setw(3) << ms.count() << "] "
              << message << std::endl;
}

void print_usage(const char* program) {
    std::cout << "Usage: " << program << " [options]" << std::endl;
    std::cout << std::endl;
    std::cout << "Rating test tool - for iterating on Zune rating implementation" << std::endl;
    std::cout << std::endl;
    std::cout << "Options:" << std::endl;
    std::cout << "  --track PATH       Upload a track first, then set rating" << std::endl;
    std::cout << "  --find TITLE       Find existing track by title in library" << std::endl;
    std::cout << "  --rating N         Rating to set: 0=unrated, 2=dislike, 8=like (default: 2)" << std::endl;
    std::cout << "  --playcount N      Set UseCount (0xDC91) to N on the found/uploaded track" << std::endl;
    std::cout << "  --direct           Use direct SetObjectProperty method (simple approach)" << std::endl;
    std::cout << "  --list             List all tracks in library with MTP IDs" << std::endl;
    std::cout << "  --help             Show this help" << std::endl;
    std::cout << std::endl;
    std::cout << "Examples:" << std::endl;
    std::cout << "  " << program << " --list" << std::endl;
    std::cout << "  " << program << " --find \"Song Title\" --rating 8" << std::endl;
    std::cout << "  " << program << " --find \"Song Title\" --rating 8 --direct" << std::endl;
    std::cout << "  " << program << " --find \"Song Title\" --playcount 5" << std::endl;
    std::cout << "  " << program << " --track /path/to/song.wma --rating 8" << std::endl;
}

int main(int argc, char** argv) {
    std::cout << "=== Zune Rating Test Tool ===" << std::endl;
    std::cout << std::endl;

    // Parse arguments
    std::string track_path;
    std::string find_title;
    int rating = 2;  // Default to dislike for testing
    int play_count = -1;  // -1 = don't set
    int skip_count = -1;  // -1 = don't set
    bool list_tracks = false;
    bool use_direct = false;

    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        if (arg == "--track" && i + 1 < argc) {
            track_path = argv[++i];
        } else if (arg == "--find" && i + 1 < argc) {
            find_title = argv[++i];
        } else if (arg == "--rating" && i + 1 < argc) {
            rating = std::stoi(argv[++i]);
        } else if (arg == "--playcount" && i + 1 < argc) {
            play_count = std::stoi(argv[++i]);
        } else if (arg == "--skipcount" && i + 1 < argc) {
            skip_count = std::stoi(argv[++i]);
        } else if (arg == "--list") {
            list_tracks = true;
        } else if (arg == "--direct") {
            use_direct = true;
        } else if (arg == "--help") {
            print_usage(argv[0]);
            return 0;
        }
    }

    // Install signal handlers
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    try {
        // Create and connect device
        ZuneDevice device;
        device.SetLogCallback(log_callback);

        std::cout << "Connecting to Zune device via USB..." << std::endl;
        if (!device.ConnectUSB()) {
            std::cerr << "ERROR: Failed to connect to Zune device" << std::endl;
            return 1;
        }

        std::cout << std::endl;
        std::cout << "=== Device Connected ===" << std::endl;
        std::cout << "Device: " << device.GetName() << std::endl;
        std::cout << std::endl;

        // Get music library
        std::cout << "Loading music library..." << std::endl;
        ZuneMusicLibrary* library = device.GetMusicLibrary();
        if (!library) {
            std::cerr << "ERROR: Failed to get music library" << std::endl;
            return 1;
        }

        std::cout << "Library: " << library->track_count << " tracks, "
                  << library->album_count << " albums" << std::endl;
        std::cout << std::endl;

        // Build album map: atom_id -> album*
        std::map<uint32_t, const ZuneMusicAlbum*> album_map;
        for (size_t i = 0; i < library->album_count; i++) {
            album_map[library->albums[i].atom_id] = &library->albums[i];
        }

        uint32_t mtp_track_id = 0;
        uint32_t mtp_album_id = 0;

        // List mode
        if (list_tracks) {
            std::cout << "=== Track Listing ===" << std::endl;
            for (size_t i = 0; i < library->track_count; i++) {
                const auto& track = library->tracks[i];
                // track.atom_id IS the MTP object ID (e.g., 0x01000058)
                uint32_t track_mtp = track.atom_id;

                // Find album's MTP ID from album_ref
                // album->atom_id IS the MTP object ID (e.g., 0x0600005A)
                uint32_t album_mtp = 0;
                auto it = album_map.find(track.album_ref);
                if (it != album_map.end()) {
                    album_mtp = it->second->atom_id;
                }

                std::cout << "Track: 0x" << std::hex << std::setw(8) << std::setfill('0') << track_mtp
                          << " | Album: 0x" << std::setw(8) << album_mtp << std::dec
                          << " | Rating: " << (int)track.rating
                          << " | Plays: " << track.playcount
                          << " | Skips: " << track.skip_count
                          << " | \"" << track.title << "\" - " << track.artist_name << std::endl;
            }
            device.Disconnect();
            return 0;
        }

        // Upload track mode (disabled — API changed, use --find instead)
        if (!track_path.empty()) {
            std::cerr << "ERROR: --track upload is not supported in this build. Use --find instead." << std::endl;
            device.Disconnect();
            return 1;
        }

        // Find track mode
        if (!find_title.empty() && mtp_track_id == 0) {
            std::cout << "=== Finding Track ===" << std::endl;
            std::cout << "Searching for: \"" << find_title << "\"" << std::endl;

            for (size_t i = 0; i < library->track_count; i++) {
                const auto& track = library->tracks[i];
                std::string track_title = track.title;

                // Case-insensitive partial match
                if (track_title.find(find_title) != std::string::npos) {
                    // atom_id IS the MTP object ID
                    mtp_track_id = track.atom_id;

                    // Find album's MTP ID via album_map
                    auto it = album_map.find(track.album_ref);
                    if (it != album_map.end()) {
                        mtp_album_id = it->second->atom_id;
                    }

                    std::cout << "Found: \"" << track.title << "\" by " << track.artist_name << std::endl;
                    std::cout << "  Track MTP: 0x" << std::hex << mtp_track_id << std::dec << std::endl;
                    std::cout << "  Album MTP: 0x" << std::hex << mtp_album_id << std::dec << std::endl;
                    std::cout << "  Current rating: " << (int)track.rating << std::endl;
                    std::cout << "  Current play count: " << track.playcount << std::endl;
                    std::cout << "  Current skip count: " << track.skip_count << std::endl;
                    break;
                }
            }

            if (mtp_track_id == 0) {
                std::cerr << "ERROR: Track not found" << std::endl;
                device.Disconnect();
                return 1;
            }
            std::cout << std::endl;
        }

        // Set rating and/or play count
        if (mtp_track_id != 0) {
            std::cout << "=== Setting Track User State ===" << std::endl;
            std::cout << "Track MTP ID: 0x" << std::hex << mtp_track_id << std::dec << std::endl;
            std::cout << "Album MTP ID: 0x" << std::hex << mtp_album_id << std::dec << std::endl;
            if (play_count >= 0)
                std::cout << "Play count: " << play_count << std::endl;
            std::cout << "Rating: " << rating << " (0=unrated, 2=dislike, 8=like)" << std::endl;
            std::cout << std::endl;

            std::cout << "Calling SetTrackUserState..." << std::endl;
            int result = device.SetTrackUserState(mtp_track_id, play_count, skip_count, rating);

            if (result == 0) {
                std::cout << "SetTrackUserState succeeded!" << std::endl;
            } else {
                std::cerr << "ERROR: SetTrackUserState failed with code " << result << std::endl;
            }
        } else {
            std::cerr << "No track specified. Use --list, --find, or --track" << std::endl;
            print_usage(argv[0]);
            device.Disconnect();
            return 1;
        }

        // Disconnect
        std::cout << std::endl;
        std::cout << "Disconnecting..." << std::endl;
        device.Disconnect();
        std::cout << "Done." << std::endl;

        return 0;

    } catch (const std::exception& e) {
        std::cerr << "ERROR: " << e.what() << std::endl;
        return 1;
    }
}
