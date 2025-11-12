#include "lib/src/ZuneDevice.h"
#include "lib/src/protocols/http/ZuneHTTPInterceptor.h"
#include <iostream>
#include <iomanip>
#include <thread>
#include <chrono>
#include <csignal>
#include <fstream>
#include <sstream>
#include <taglib/fileref.h>
#include <taglib/tag.h>
#include <taglib/tpropertymap.h>
#include <taglib/asffile.h>
#include <taglib/attachedpictureframe.h>
#include <taglib/id3v2tag.h>
#include <taglib/id3v2frame.h>
#include <taglib/mpegfile.h>

// Global flags for control
static volatile bool g_running = true;
static volatile bool g_show_logs = false;  // Only show logs during operations

void signal_handler(int signal) {
    std::cout << "\nReceived signal " << signal << ", shutting down..." << std::endl;
    // Exit immediately - Ctrl+C should always terminate instantly
    exit(0);
}

void log_callback(const std::string& message) {
    if (!g_show_logs) return;  // Only display if logging is enabled

    auto now = std::chrono::system_clock::now();
    auto time = std::chrono::system_clock::to_time_t(now);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()) % 1000;

    std::cout << "[" << std::put_time(std::localtime(&time), "%H:%M:%S")
              << "." << std::setfill('0') << std::setw(3) << ms.count() << "] "
              << message << std::endl;
}

void print_main_menu() {
    std::cout << std::endl;
    std::cout << "========================================" << std::endl;
    std::cout << "Artist Images Tool" << std::endl;
    std::cout << "========================================" << std::endl;
    std::cout << "Select an operation:" << std::endl;
    std::cout << std::endl;
    std::cout << "  1) Network Monitor" << std::endl;
    std::cout << "     Monitor HTTP requests from device" << std::endl;
    std::cout << std::endl;
    std::cout << "  2) Upload Track" << std::endl;
    std::cout << "     Upload an audio track with metadata" << std::endl;
    std::cout << std::endl;
    std::cout << "  3) Retrofit Artist" << std::endl;
    std::cout << "     Add MusicBrainz GUID to existing artist" << std::endl;
    std::cout << std::endl;
    std::cout << "  4) Exit" << std::endl;
    std::cout << std::endl;
    std::cout << "Enter choice (1-4): ";
}

// Trim whitespace from string
std::string trim(const std::string& str) {
    size_t first = str.find_first_not_of(" \t\n\r\f\v");
    if (first == std::string::npos) return "";
    size_t last = str.find_last_not_of(" \t\n\r\f\v");
    return str.substr(first, last - first + 1);
}

// Unescape shell-escaped paths (e.g., \( becomes (, \ becomes space)
std::string unescape_path(const std::string& str) {
    std::string result;
    for (size_t i = 0; i < str.length(); ++i) {
        if (str[i] == '\\' && i + 1 < str.length()) {
            // Skip the backslash and add the next character literally
            result += str[++i];
        } else {
            result += str[i];
        }
    }
    return result;
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

// ============================================================================
// Network Mode Setup (ONE TIME, before all operations)
// ============================================================================

int establish_network_mode(ZuneDevice& device) {
    const int max_network_retries = 10;
    bool network_mode_success = false;

    std::cout << std::endl;
    std::cout << "=== Establishing Network Mode ===" << std::endl;

    for (int retry = 1; retry <= max_network_retries && !network_mode_success; retry++) {
        try {
            if (retry > 1) {
                std::cout << "Retry attempt " << retry << "/" << max_network_retries << "..." << std::endl;
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
            }

            device.TriggerNetworkMode();
            network_mode_success = true;
            device.EnableNetworkPolling();

            std::cout << "✓ Network mode established" << std::endl;
            return 0;

        } catch (const std::exception& e) {
            std::cerr << "Network mode trigger attempt " << retry << " failed: " << e.what() << std::endl;
            if (retry == max_network_retries) {
                std::cerr << "ERROR: Failed to establish network mode" << std::endl;
                return 1;
            }
        }
    }

    return 1;
}

// ============================================================================
// Operation: Network Monitor
// ============================================================================

void operation_network_monitor(ZuneDevice& device) {
    std::cout << std::endl;
    std::cout << "=== Network Monitor ===" << std::endl;
    std::cout << "Monitoring HTTP requests from device..." << std::endl;
    std::cout << "Press Ctrl+C to return to menu" << std::endl;
    std::cout << std::endl;

    g_show_logs = true;  // Enable logging for network monitoring
    while (g_running) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    // Keep g_show_logs enabled so network activity continues to show
    // (User pressed Ctrl+C to exit monitor, but network activity should continue visible in menu)

    std::cout << std::endl;
    std::cout << "Network monitor stopped." << std::endl;
}

// ============================================================================
// Operation: Retrofit Artist
// ============================================================================

void operation_retrofit_artist(ZuneDevice& device) {
    std::cout << std::endl;
    std::cout << "=== Retrofit Artist ===" << std::endl;

    // Get list of artists from device
    std::cout << "Querying device for artists..." << std::endl;
    std::vector<ZuneArtistInfo> artists;
    try {
        artists = device.GetMusicLibrary();
    } catch (const std::exception& e) {
        std::cerr << "ERROR: Failed to get artist list: " << e.what() << std::endl;
        return;
    }

    if (artists.empty()) {
        std::cout << "No artists found on device." << std::endl;
        return;
    }

    // Display numbered list of artists
    std::cout << std::endl;
    std::cout << "Artists on device:" << std::endl;
    for (size_t i = 0; i < artists.size(); ++i) {
        std::cout << "  " << (i + 1) << ". " << artists[i].Name
                  << " (" << artists[i].AlbumCount << " album";
        if (artists[i].AlbumCount != 1) std::cout << "s";
        std::cout << ")" << std::endl;
    }

    // Get user selection
    std::cout << std::endl;
    std::string choice_str = read_input("Select artist number (1-" + std::to_string(artists.size()) + "): ");

    size_t choice = 0;
    try {
        choice = std::stoul(choice_str);
    } catch (...) {
        std::cerr << "Invalid selection" << std::endl;
        return;
    }

    if (choice < 1 || choice > artists.size()) {
        std::cerr << "Invalid selection" << std::endl;
        return;
    }

    std::string selected_artist_name = artists[choice - 1].Name;

    // Get GUID from user
    std::cout << std::endl;
    std::cout << "Selected artist: " << selected_artist_name << std::endl;
    std::string guid = read_input("Enter MusicBrainz GUID (UUID format): ");

    if (guid.empty()) {
        std::cout << "GUID cannot be empty." << std::endl;
        return;
    }

    // Confirm before retrofitting
    std::cout << std::endl;
    std::cout << "This will:" << std::endl;
    std::cout << "  1. Delete the artist '" << selected_artist_name << "'" << std::endl;
    std::cout << "  2. Recreate it with GUID: " << guid << std::endl;
    std::cout << "  3. Device will auto-refresh metadata" << std::endl;
    std::cout << std::endl;

    if (!read_yes_no("Continue?")) {
        std::cout << "Retrofit cancelled." << std::endl;
        return;
    }

    // Perform retrofit
    std::cout << std::endl;
    std::cout << "Retrofitting artist..." << std::endl;
    g_show_logs = true;  // Enable logging for retrofit operation
    try {
        int result = device.RetrofitArtistGuid(selected_artist_name, guid);
        if (result == 0) {
            std::cout << "✓ Retrofit successful" << std::endl;
            std::cout << "Device will automatically fetch metadata from server" << std::endl;
        } else {
            std::cerr << "ERROR: Retrofit failed with code " << result << std::endl;
        }
    } catch (const std::exception& e) {
        std::cerr << "ERROR: Retrofit failed: " << e.what() << std::endl;
    }
    // Keep g_show_logs enabled so network activity continues to show
}

// ============================================================================
// Operation: Upload Track
// ============================================================================

struct TrackMetadata {
    std::string title;
    std::string artist;
    std::string album;
    int year = 0;
    std::string genre;
    int track_number = 0;
    std::vector<uint8_t> artwork;
    std::string zune_artist_guid;  // For WMA files with embedded GUID
};

TrackMetadata extract_track_metadata(const std::string& audio_file_path) {
    TrackMetadata metadata;

    try {
        TagLib::FileRef file(audio_file_path.c_str());

        if (file.isNull()) {
            throw std::runtime_error("Could not identify file format");
        }

        if (file.tag()) {
            TagLib::Tag* tag = file.tag();
            metadata.title = tag->title().to8Bit();
            metadata.artist = tag->artist().to8Bit();
            metadata.album = tag->album().to8Bit();
            metadata.year = tag->year();
            metadata.genre = tag->genre().to8Bit();
            metadata.track_number = tag->track();
        }

        // Try ID3v2 (MP3, MPEG files)
        TagLib::MPEG::File* mpegFile = dynamic_cast<TagLib::MPEG::File*>(file.file());
        if (mpegFile && mpegFile->ID3v2Tag()) {
            auto frames = mpegFile->ID3v2Tag()->frameList("APIC");
            if (!frames.isEmpty()) {
                auto pictureFrame = dynamic_cast<TagLib::ID3v2::AttachedPictureFrame*>(frames.front());
                if (pictureFrame) {
                    TagLib::ByteVector data = pictureFrame->picture();
                    metadata.artwork.assign(data.begin(), data.end());
                }
            }

            // Extract GUID from ID3v2 tags - check for MusicBrainz Artist ID
            TagLib::Tag* tag = mpegFile->tag();
            if (tag) {
                TagLib::PropertyMap props = tag->properties();
                if (props.contains("MUSICBRAINZ_ARTISTID")) {
                    auto values = props["MUSICBRAINZ_ARTISTID"];
                    if (!values.isEmpty()) {
                        metadata.zune_artist_guid = values.front().to8Bit();
                    }
                }
            }
        }

        // Try ASF/WMA (Windows Media Audio files)
        TagLib::ASF::File* asfFile = dynamic_cast<TagLib::ASF::File*>(file.file());
        if (asfFile) {
            TagLib::ASF::Tag* asfTag = asfFile->tag();
            if (asfTag) {
                TagLib::ASF::AttributeListMap& attrMap = asfTag->attributeListMap();

                // Extract artwork from WM/Picture
                if (attrMap.contains("WM/Picture")) {
                    TagLib::ASF::AttributeList& pictures = attrMap["WM/Picture"];
                    if (!pictures.isEmpty()) {
                        TagLib::ByteVector picture = pictures[0].toPicture().picture();
                        metadata.artwork.assign(picture.begin(), picture.end());
                    }
                }

                // Extract Zune artist GUID from custom attributes
                if (attrMap.contains("ZuneAlbumArtistMediaID")) {
                    TagLib::ASF::AttributeList& guidAttrs = attrMap["ZuneAlbumArtistMediaID"];
                    if (!guidAttrs.isEmpty()) {
                        metadata.zune_artist_guid = guidAttrs[0].toString().toCString(true);
                    }
                }
                // Also check for MusicBrainz artist ID
                else if (attrMap.contains("MusicBrainz/Artist ID")) {
                    TagLib::ASF::AttributeList& mbAttrs = attrMap["MusicBrainz/Artist ID"];
                    if (!mbAttrs.isEmpty()) {
                        metadata.zune_artist_guid = mbAttrs[0].toString().toCString(true);
                    }
                }
            }
        }

    } catch (const std::exception& e) {
        std::cerr << "WARNING: Could not extract metadata: " << e.what() << std::endl;
    }

    return metadata;
}

void operation_upload_track(ZuneDevice& device) {
    std::cout << std::endl;
    std::cout << "=== Upload Track ===" << std::endl;

    // Get track path from user
    std::string track_path = read_input("Enter path to audio file: ");
    track_path = unescape_path(track_path);  // Handle shell-escaped paths

    // Check if file exists
    std::ifstream file(track_path);
    if (!file.good()) {
        std::cerr << "ERROR: File not found: " << track_path << std::endl;
        return;
    }
    file.close();

    // Extract metadata
    std::cout << "Extracting metadata from file..." << std::endl;
    TrackMetadata metadata = extract_track_metadata(track_path);

    // Display extracted metadata
    std::cout << std::endl;
    std::cout << "Extracted metadata:" << std::endl;
    std::cout << "  Title:  " << (metadata.title.empty() ? "(empty)" : metadata.title) << std::endl;
    std::cout << "  Artist: " << (metadata.artist.empty() ? "(empty)" : metadata.artist) << std::endl;
    std::cout << "  Album:  " << (metadata.album.empty() ? "(empty)" : metadata.album) << std::endl;
    std::cout << "  Year:   " << (metadata.year > 0 ? std::to_string(metadata.year) : "(empty)") << std::endl;
    std::cout << "  Genre:  " << (metadata.genre.empty() ? "(empty)" : metadata.genre) << std::endl;
    std::cout << "  Track:  " << (metadata.track_number > 0 ? std::to_string(metadata.track_number) : "(empty)") << std::endl;
    std::cout << "  Artwork:" << (metadata.artwork.empty() ? " (none)" : " (present)") << std::endl;
    std::cout << "  GUID:   " << (metadata.zune_artist_guid.empty() ? "(not embedded)" : metadata.zune_artist_guid) << std::endl;

    // Allow user to override metadata
    std::cout << std::endl;
    if (read_yes_no("Modify metadata?")) {
        std::string input;

        input = read_input("  Title [" + metadata.title + "]: ");
        if (!input.empty()) metadata.title = input;

        input = read_input("  Artist [" + metadata.artist + "]: ");
        if (!input.empty()) metadata.artist = input;

        input = read_input("  Album [" + metadata.album + "]: ");
        if (!input.empty()) metadata.album = input;

        input = read_input("  Year [" + std::to_string(metadata.year) + "]: ");
        if (!input.empty()) {
            try { metadata.year = std::stoi(input); } catch (...) {}
        }

        input = read_input("  Genre [" + metadata.genre + "]: ");
        if (!input.empty()) metadata.genre = input;

        input = read_input("  Track Number [" + std::to_string(metadata.track_number) + "]: ");
        if (!input.empty()) {
            try { metadata.track_number = std::stoi(input); } catch (...) {}
        }

        input = read_input("  GUID [" + metadata.zune_artist_guid + "]: ");
        if (!input.empty()) metadata.zune_artist_guid = input;
    }

    // Confirm upload
    std::cout << std::endl;
    std::cout << "Final metadata:" << std::endl;
    std::cout << "  Title:  " << metadata.title << std::endl;
    std::cout << "  Artist: " << metadata.artist << std::endl;
    std::cout << "  Album:  " << metadata.album << std::endl;
    std::cout << "  Year:   " << metadata.year << std::endl;
    std::cout << "  Genre:  " << metadata.genre << std::endl;
    std::cout << "  Track:  " << metadata.track_number << std::endl;
    std::cout << "  GUID:   " << (metadata.zune_artist_guid.empty() ? "(none)" : metadata.zune_artist_guid) << std::endl;
    std::cout << std::endl;

    if (!read_yes_no("Upload track?")) {
        std::cout << "Upload cancelled." << std::endl;
        return;
    }

    // Perform upload
    std::cout << std::endl;
    std::cout << "Uploading track..." << std::endl;
    g_show_logs = true;  // Enable logging for upload operation
    try {
        int result = device.UploadTrackWithMetadata(
            track_path,
            metadata.artist,
            metadata.album,
            metadata.year,
            metadata.title,
            metadata.genre,
            metadata.track_number,
            metadata.artwork.empty() ? nullptr : metadata.artwork.data(),
            metadata.artwork.size(),
            metadata.zune_artist_guid
        );

        if (result == 0) {
            std::cout << "✓ Track uploaded successfully" << std::endl;
        } else {
            std::cerr << "ERROR: Upload failed with code " << result << std::endl;
        }
    } catch (const std::exception& e) {
        std::cerr << "ERROR: Upload failed: " << e.what() << std::endl;
    }
    // Keep g_show_logs enabled so network activity continues to show
}

// ============================================================================
// Main
// ============================================================================

int main() {
    // Setup signal handling for clean shutdown
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    std::cout << "=== Artist Images Tool CLI ===" << std::endl;
    std::cout << std::endl;

    // Step 1: Create and connect device
    std::cout << "Initializing device connection..." << std::endl;
    ZuneDevice device;
    device.SetLogCallback(log_callback);

    if (!device.ConnectUSB()) {
        std::cerr << "ERROR: Failed to connect to device via USB" << std::endl;
        return 1;
    }
    std::cout << "✓ Connected to device: " << device.GetName() << std::endl;

    // Step 2: Configure HTTP interceptor
    std::cout << std::endl;
    std::cout << "=== HTTP Interceptor Configuration ===" << std::endl;
    std::cout << "Select interception mode:" << std::endl;
    std::cout << "  1) Proxy - Forward requests to HTTP server" << std::endl;
    std::cout << "  2) Static - Serve from local files" << std::endl;
    std::cout << "  3) Disabled - No interception" << std::endl;
    std::string mode_choice = read_input("Enter choice (1-3): ");

    InterceptorConfig config;
    if (mode_choice == "1") {
        // Proxy mode
        std::string server_ip = read_input("Server IP address (default 192.168.0.30): ");
        if (server_ip.empty()) server_ip = "192.168.0.30";

        config.mode = InterceptionMode::Proxy;
        config.proxy_config.catalog_server = "http://" + server_ip;
        config.server_ip = server_ip;

        std::cout << "✓ Proxy mode configured" << std::endl;
    } else if (mode_choice == "2") {
        // Static mode
        std::string data_dir = read_input("Artist data directory (press Enter for default): ");
        if (data_dir.empty()) {
            data_dir = "/Users/andymoe/Documents/AppDevelopment/ZuneWirelessSync/ZuneArtistImages/artist_data";
        }

        config.mode = InterceptionMode::Static;
        config.static_config.data_directory = data_dir;
        config.static_config.test_mode = false;

        std::cout << "✓ Static mode configured" << std::endl;
    } else {
        // Disabled mode
        config.mode = InterceptionMode::Disabled;
        std::cout << "✓ Interception disabled" << std::endl;
    }

    // Step 3: Initialize HTTP subsystem
    std::cout << std::endl;
    std::cout << "Initializing HTTP subsystem..." << std::endl;
    if (!device.InitializeHTTPSubsystem()) {
        std::cerr << "ERROR: Failed to initialize HTTP subsystem" << std::endl;
        return 1;
    }
    std::cout << "✓ HTTP subsystem initialized" << std::endl;

    // Step 4: Start HTTP interceptor
    if (config.mode != InterceptionMode::Disabled) {
        std::cout << "Starting HTTP interceptor..." << std::endl;
        try {
            device.StartHTTPInterceptor(config);
            std::cout << "✓ HTTP interceptor started" << std::endl;
        } catch (const std::exception& e) {
            std::cerr << "ERROR: Failed to start HTTP interceptor: " << e.what() << std::endl;
            return 1;
        }
    }

    // Step 5: Establish network mode (ONCE, before all operations)
    if (establish_network_mode(device) != 0) {
        device.Disconnect();
        return 1;
    }

    // Step 6: Main menu loop
    while (g_running) {
        print_main_menu();
        std::string choice = read_input();

        // Check if signal was received during input
        if (!g_running) break;

        if (choice == "1") {
            operation_network_monitor(device);
        } else if (choice == "2") {
            operation_upload_track(device);
        } else if (choice == "3") {
            operation_retrofit_artist(device);
        } else if (choice == "4") {
            break;
        } else if (!choice.empty()) {
            std::cout << "Invalid choice. Please try again." << std::endl;
        }
    }

    // Cleanup
    std::cout << std::endl;
    std::cout << "Shutting down..." << std::endl;
    device.StopHTTPInterceptor();
    device.Disconnect();
    std::cout << "✓ Shutdown complete" << std::endl;

    return 0;
}
