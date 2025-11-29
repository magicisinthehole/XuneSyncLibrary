#include "lib/src/ZuneDevice.h"
#include "lib/src/protocols/http/ZuneHTTPInterceptor.h"
#include <iostream>
#include <iomanip>
#include <thread>
#include <chrono>
#include <csignal>
#include <fstream>
#include <taglib/fileref.h>
#include <taglib/tag.h>
#include <taglib/tpropertymap.h>
#include <taglib/asffile.h>
#include <taglib/attachedpictureframe.h>
#include <taglib/id3v2tag.h>
#include <taglib/mpegfile.h>

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

int main(int argc, char** argv) {
    std::cout << "=== Zune HTTP Interceptor Test ===" << std::endl;
    std::cout << std::endl;

    // Parse command line arguments
    std::string mode = "proxy";  // Default to proxy mode
    std::string server_url = "http://localhost:80";
    std::string data_directory = "/Users/andymoe/Documents/AppDevelopment/ZuneWirelessSync/ZuneArtistImages/artist_data";
    bool test_mode = false;
    std::string track_path = "/Users/andymoe/Documents/AppDevelopment/ZuneWirelessSync/ZuneArtistImages/TestTrack/New/(You Drive Me) Crazy.wma";
    bool upload_test_track = true;  // Enable by default with test track
    bool wait_for_device = false;  // Don't wait by default (old behavior)
    int delay_after_upload = 0;    // Delay in seconds after track upload
    bool reinit_after_upload = false;    // Re-initialize HTTP subsystem after upload

    // Retrofit mode arguments
    std::string retrofit_artist;     // Artist name to retrofit with GUID
    std::string retrofit_guid;       // GUID to assign to existing artist
    std::string retrofit_track;      // Optional: specific track name for metadata trigger

    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        if (arg == "--mode" && i + 1 < argc) {
            mode = argv[++i];
        }
        else if (arg == "--server" && i + 1 < argc) {
            server_url = argv[++i];
        }
        else if (arg == "--data-dir" && i + 1 < argc) {
            data_directory = argv[++i];
        }
        else if (arg == "--test-mode") {
            test_mode = true;
        }
        else if (arg == "--track" && i + 1 < argc) {
            track_path = argv[++i];
            upload_test_track = true;
        }
        else if (arg == "--wait") {
            wait_for_device = true;
        }
        else if (arg == "--delay" && i + 1 < argc) {
            delay_after_upload = std::stoi(argv[++i]);
        }
        else if (arg == "--reinit") {
            reinit_after_upload = true;
        }
        else if (arg == "--retrofit-artist" && i + 1 < argc) {
            retrofit_artist = argv[++i];
        }
        else if (arg == "--retrofit-guid" && i + 1 < argc) {
            retrofit_guid = argv[++i];
        }
        else if (arg == "--retrofit-track" && i + 1 < argc) {
            retrofit_track = argv[++i];
        }
        else if (arg == "--help") {
            std::cout << "Usage: " << argv[0] << " [options] [track_path]" << std::endl;
            std::cout << std::endl;
            std::cout << "Options:" << std::endl;
            std::cout << "  --mode MODE        Interception mode: 'proxy' or 'static' (default: proxy)" << std::endl;
            std::cout << "  --server URL       Proxy server URL (default: http://localhost:80)" << std::endl;
            std::cout << "  --data-dir PATH    Static mode data directory (default: ZuneArtistImages/artist_data)" << std::endl;
            std::cout << "  --test-mode        Enable test mode for static responses" << std::endl;
            std::cout << "  --track PATH       Upload a test track to trigger metadata requests" << std::endl;
            std::cout << "  --wait             Wait for device to connect (poll mode)" << std::endl;
            std::cout << "  --delay SECONDS    Delay after track upload before network mode (default: 0)" << std::endl;
            std::cout << "  --reinit           Re-initialize HTTP subsystem after track upload" << std::endl;
            std::cout << std::endl;
            std::cout << "Retrofit mode (modify existing artist to add GUID):" << std::endl;
            std::cout << "  --retrofit-artist NAME   Artist name to retrofit with GUID" << std::endl;
            std::cout << "  --retrofit-guid GUID     MusicBrainz UUID to assign to artist" << std::endl;
            std::cout << "  --retrofit-track NAME    Optional: track name for metadata trigger" << std::endl;
            std::cout << std::endl;
            std::cout << "  --help             Show this help message" << std::endl;
            std::cout << std::endl;
            std::cout << "Positional arguments:" << std::endl;
            std::cout << "  track_path         Path to track file (same as --track)" << std::endl;
            std::cout << std::endl;
            std::cout << "Examples:" << std::endl;
            std::cout << "  Upload new track:" << std::endl;
            std::cout << "    " << argv[0] << " --delay 5 /path/to/song.mp3" << std::endl;
            std::cout << "    " << argv[0] << " /path/to/song.mp3" << std::endl;
            std::cout << "    " << argv[0] << " --mode static /path/to/song.mp3" << std::endl;
            std::cout << std::endl;
            std::cout << "  Retrofit existing artist:" << std::endl;
            std::cout << "    " << argv[0] << " --retrofit-artist \"Britney Spears\" --retrofit-guid \"45a663b5-b1cb-4a91-bff6-2bef7bbfdd76\"" << std::endl;
            std::cout << "    " << argv[0] << " --retrofit-artist \"The Beatles\" --retrofit-guid \"b10bbbfc-cf9e-42e0-be17-e2c3e1d2600d\" --mode static" << std::endl;
            return 0;
        }
        else {
            // Treat as positional argument (track path)
            track_path = arg;
            upload_test_track = true;
        }
    }

    // Install signal handlers
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    if (wait_for_device) {
        std::cout << "Waiting for Zune device to connect..." << std::endl;
        std::cout << "Connect your Zune device via USB to begin." << std::endl;
        std::cout << std::endl;
    }

    try {
        // Create device
        ZuneDevice device;
        device.SetLogCallback(log_callback);

        // Connect to device (poll if waiting, otherwise fail fast)
        if (wait_for_device) {
            std::cout << "Waiting for Zune device..." << std::endl;
            while (g_running) {
                if (device.ConnectUSB()) {
                    break;
                }
                std::this_thread::sleep_for(std::chrono::seconds(2));
            }

            if (!g_running) {
                return 0;
            }
        } else {
            std::cout << "Connecting to Zune device via USB..." << std::endl;
            if (!device.ConnectUSB()) {
                std::cerr << "ERROR: Failed to connect to Zune device" << std::endl;
                std::cerr << "Make sure a Zune is connected via USB" << std::endl;
                return 1;
            }
        }

        std::cout << std::endl;
        std::cout << "=== Device Connected! ===" << std::endl;
        std::cout << "Device: " << device.GetName() << std::endl;
        std::cout << "Serial: " << device.GetSerialNumber() << std::endl;
        std::cout << "Model:  " << device.GetModel() << std::endl;
        std::cout << std::endl;

        // Configure interceptor based on mode
        InterceptorConfig config;
        config.server_ip = "192.168.0.30";  // DNS resolves to this IP (from CLAUDE.md network config)

        if (mode == "static") {
            config.mode = InterceptionMode::Static;
            config.static_config.data_directory = data_directory;
            config.static_config.test_mode = test_mode;
        } else {
            config.mode = InterceptionMode::Proxy;
            config.proxy_config.catalog_server = server_url;
            config.proxy_config.image_server = "";
            config.proxy_config.art_server = "";
            config.proxy_config.mix_server = "";
            config.proxy_config.timeout_ms = 10000;
        }

        std::cout << "=== Initializing HTTP Subsystem ===" << std::endl;
        std::cout << "Sending MTP vendor commands to device..." << std::endl;
        if (!device.InitializeHTTPSubsystem()) {
            std::cerr << "ERROR: Failed to initialize HTTP subsystem" << std::endl;
            return 1;
        }
        std::cout << std::endl;

        std::cout << "=== Starting HTTP Interceptor ===" << std::endl;
        if (mode == "static") {
            std::cout << "  Mode: Static" << std::endl;
            std::cout << "  Data directory: " << data_directory << std::endl;
            std::cout << "  Test mode: " << (test_mode ? "enabled" : "disabled") << std::endl;
        } else {
            std::cout << "  Mode: Proxy" << std::endl;
            std::cout << "  Server: " << server_url << std::endl;
        }
        std::cout << std::endl;

        std::cout << "Starting HTTP interceptor..." << std::endl;
        device.StartHTTPInterceptor(config);

        if (!device.IsHTTPInterceptorRunning()) {
            std::cerr << "ERROR: Failed to start HTTP interceptor" << std::endl;
            return 1;
        }

        std::cout << "✓ HTTP interceptor running" << std::endl;
        std::cout << std::endl;

        // === ALWAYS TRIGGER NETWORK MODE FIRST ===
        // Establish network mode before any operations so device can make HTTP requests
        std::cout << "=== Establishing Network Mode ===" << std::endl;
        std::cout << "Triggering network mode before operations..." << std::endl;
            const int max_network_retries = 10;
            bool network_mode_success = false;

            for (int retry = 1; retry <= max_network_retries && !network_mode_success; retry++) {
                try {
                    if (retry > 1) {
                        std::cout << std::endl;
                        std::cout << "Retry attempt " << retry << "/" << max_network_retries << "..." << std::endl;
                        std::this_thread::sleep_for(std::chrono::milliseconds(500));
                    }

                    device.TriggerNetworkMode();
                    network_mode_success = true;
                    device.EnableNetworkPolling();

                    std::cout << "✓ Network mode established" << std::endl;
                    std::cout << "Device should now start HTTP metadata requests for " << retrofit_artist << std::endl;
                    std::cout << std::endl;

                } catch (const std::exception& e) {
                    std::cerr << "Network mode trigger attempt " << retry << " failed: " << e.what() << std::endl;
                    if (retry == max_network_retries) {
                        std::cerr << "ERROR: Failed to establish network mode" << std::endl;
                        return 1;
                    }
                }
            }

            if (!network_mode_success) {
                std::cerr << "ERROR: Failed to establish network mode" << std::endl;
                return 1;
            }

        std::cout << "✓ Network mode established" << std::endl;
        std::cout << std::endl;

        // === RETROFIT MODE: Modify existing artist to add GUID ===
        // Now that network mode is active, perform retrofit so device can immediately fetch metadata
        if (!retrofit_artist.empty() && !retrofit_guid.empty()) {
            std::cout << "=== Retrofit Mode Activated ===" << std::endl;
            std::cout << "Artist: " << retrofit_artist << std::endl;
            std::cout << "GUID: " << retrofit_guid << std::endl;
            if (!retrofit_track.empty()) {
                std::cout << "Track: " << retrofit_track << std::endl;
            }
            std::cout << std::endl;

            // Retrofit artist using delete/recreate approach
            // Device is already in network mode and can immediately make HTTP requests
            int result = device.RetrofitArtistGuid(retrofit_artist, retrofit_guid);

            if (result != 0) {
                std::cerr << "ERROR: Artist retrofit failed" << std::endl;
                return 1;
            }

            std::cout << std::endl;
            std::cout << "=== Retrofit Complete ===" << std::endl;
            std::cout << "Device should now make HTTP metadata requests for " << retrofit_artist << std::endl;
            std::cout << std::endl;

            // Skip track upload - we're in retrofit mode
            upload_test_track = false;

            // Jump to monitoring loop (skip track upload section)
            std::cout << "Monitoring for HTTP requests from device..." << std::endl;
            std::cout << std::endl;
            std::cout << "Expected traffic:" << std::endl;
            std::cout << "  - GET /v3.0/en-US/music/artist/" << retrofit_guid << "/biography" << std::endl;
            std::cout << "  - GET /v3.0/en-US/music/artist/" << retrofit_guid << "/images" << std::endl;
            std::cout << "  - GET /v3.0/en-US/music/artist/" << retrofit_guid << "/deviceBackgroundImage" << std::endl;
            std::cout << std::endl;
            std::cout << "Press Ctrl+C to stop..." << std::endl;
            std::cout << std::endl;

            // Keep monitoring until interrupted
            while (g_running) {
                std::this_thread::sleep_for(std::chrono::milliseconds(500));

                // Check if interceptor is still running
                if (!device.IsHTTPInterceptorRunning()) {
                    std::cerr << "WARNING: HTTP interceptor stopped unexpectedly" << std::endl;
                    break;
                }
            }

            // Clean shutdown
            std::cout << std::endl;
            std::cout << "Stopping HTTP interceptor..." << std::endl;
            device.StopHTTPInterceptor();
            std::cout << "✓ Interceptor stopped" << std::endl;

            std::cout << "Disconnecting from device..." << std::endl;
            device.Disconnect();
            std::cout << "✓ Disconnected" << std::endl;

            std::cout << std::endl;
            std::cout << "Retrofit test completed successfully!" << std::endl;
            return 0;
        }

        // === NORMAL MODE: Upload track and trigger metadata ===
        // Network mode already established above, just upload the track

        // Upload test track
        if (upload_test_track && !track_path.empty()) {
            std::cout << "=== Uploading Test Track ===" << std::endl;
            std::cout << "Track: " << track_path << std::endl;
            std::cout << std::endl;

            // Extract metadata from track using TagLib
            TagLib::FileRef fileRef(track_path.c_str());
            if (!fileRef.isNull() && fileRef.tag()) {
                TagLib::Tag* tag = fileRef.tag();

                std::string artist = tag->artist().toCString(true);
                std::string album = tag->album().toCString(true);
                std::string title = tag->title().toCString(true);
                std::string genre = tag->genre().toCString(true);
                int year = tag->year();
                int track_num = tag->track();

                std::cout << "  Artist: " << artist << std::endl;
                std::cout << "  Album:  " << album << std::endl;
                std::cout << "  Title:  " << title << std::endl;
                std::cout << "  Genre:  " << genre << std::endl;
                std::cout << "  Year:   " << year << std::endl;
                std::cout << "  Track:  " << track_num << std::endl;
                std::cout << std::endl;

                // Extract album artwork if present
                const uint8_t* artwork_data = nullptr;
                size_t artwork_size = 0;
                std::vector<uint8_t> artwork_buffer;

                // Extract Zune artist GUID
                std::string zune_artist_guid;

                // Try to extract artwork and GUID from ASF/WMA file
                TagLib::ASF::File* asfFile = dynamic_cast<TagLib::ASF::File*>(fileRef.file());
                if (asfFile) {
                    TagLib::ASF::Tag* asfTag = asfFile->tag();
                    if (asfTag) {
                        TagLib::ASF::AttributeListMap& attrMap = asfTag->attributeListMap();

                        // Extract artwork
                        if (attrMap.contains("WM/Picture")) {
                            TagLib::ASF::AttributeList& pictures = attrMap["WM/Picture"];
                            if (!pictures.isEmpty()) {
                                TagLib::ByteVector picture = pictures[0].toPicture().picture();
                                artwork_buffer.assign(picture.begin(), picture.end());
                                artwork_data = artwork_buffer.data();
                                artwork_size = artwork_buffer.size();
                                std::cout << "  Artwork: " << artwork_size << " bytes" << std::endl;
                            }
                        }

                        // Extract Zune artist GUID
                        if (attrMap.contains("ZuneAlbumArtistMediaID")) {
                            TagLib::ASF::AttributeList& guidAttrs = attrMap["ZuneAlbumArtistMediaID"];
                            if (!guidAttrs.isEmpty()) {
                                zune_artist_guid = guidAttrs[0].toString().toCString(true);
                                std::cout << "  Zune Artist GUID: " << zune_artist_guid << std::endl;
                            }
                        } else {
                            std::cout << "  Zune Artist GUID: NOT FOUND (metadata fetch will not work)" << std::endl;
                        }
                    }
                }

                std::cout << std::endl;
                std::cout << "Uploading track with metadata..." << std::endl;

                int result = device.UploadTrackWithMetadata(
                    MediaType::Music,
                    track_path,
                    artist,
                    album,
                    year,
                    title,
                    genre,
                    track_num,
                    artwork_data,
                    artwork_size,
                    zune_artist_guid
                );

                if (result == 0) {
                    std::cout << "✓ Track uploaded successfully!" << std::endl;
                    std::cout << std::endl;
                    std::cout << "NOTE: Watch for HTTP requests for artist: " << artist << std::endl;
                    std::cout << std::endl;

                    // STRATEGY 1: Add delay after track upload (if requested)
                    if (delay_after_upload > 0) {
                        std::cout << "=== Strategy: Delay After Upload ===" << std::endl;
                        std::cout << "Waiting " << delay_after_upload << " seconds for device to settle..." << std::endl;
                        std::this_thread::sleep_for(std::chrono::seconds(delay_after_upload));
                        std::cout << "✓ Delay complete" << std::endl;
                        std::cout << std::endl;
                    }

                    // STRATEGY 3: Re-initialize HTTP subsystem after track upload (if requested)
                    if (reinit_after_upload) {
                        std::cout << "=== Strategy: Re-initialize HTTP Subsystem ===" << std::endl;
                        std::cout << "Re-initializing HTTP subsystem to reset device state..." << std::endl;

                        // Stop interceptor temporarily
                        device.StopHTTPInterceptor();
                        std::this_thread::sleep_for(std::chrono::milliseconds(500));

                        // Re-initialize HTTP subsystem
                        if (!device.InitializeHTTPSubsystem()) {
                            std::cerr << "ERROR: Failed to re-initialize HTTP subsystem" << std::endl;
                            return 1;
                        }
                        std::cout << "✓ HTTP subsystem re-initialized" << std::endl;

                        // Restart interceptor
                        device.StartHTTPInterceptor(config);
                        if (!device.IsHTTPInterceptorRunning()) {
                            std::cerr << "ERROR: Failed to restart HTTP interceptor" << std::endl;
                            return 1;
                        }
                        std::cout << "✓ HTTP interceptor restarted" << std::endl;
                        std::cout << std::endl;

                        // Note: Network mode was already established at the beginning
                        // If re-init clears network state, user should restart the test
                    }

                } else {
                    std::cerr << "ERROR: Failed to upload track (code " << result << ")" << std::endl;
                    return 1;
                }
            } else {
                std::cerr << "ERROR: Failed to read metadata from track file" << std::endl;
                return 1;
            }
        }

        // Network mode was already established at the beginning of the program
        // Proceed directly to monitoring for HTTP requests
        std::cout << std::endl;
        std::cout << "Monitoring for HTTP requests from device..." << std::endl;
        std::cout << std::endl;
        std::cout << "Expected traffic:" << std::endl;
        std::cout << "  - GET /v3.0/en-US/music/artist/{uuid}/biography" << std::endl;
        std::cout << "  - GET /v3.0/en-US/music/artist/{uuid}/images" << std::endl;
        std::cout << "  - GET /v3.0/en-US/music/artist/{uuid}/deviceBackgroundImage" << std::endl;
        std::cout << std::endl;
        std::cout << "Press Ctrl+C to stop..." << std::endl;
        std::cout << std::endl;

        // Keep monitoring until interrupted
        while (g_running) {
            std::this_thread::sleep_for(std::chrono::milliseconds(500));

            // Check if interceptor is still running
            if (!device.IsHTTPInterceptorRunning()) {
                std::cerr << "WARNING: HTTP interceptor stopped unexpectedly" << std::endl;
                break;
            }
        }

        // Clean shutdown
        std::cout << std::endl;
        std::cout << "Stopping HTTP interceptor..." << std::endl;
        device.StopHTTPInterceptor();
        std::cout << "✓ Interceptor stopped" << std::endl;

        std::cout << "Disconnecting from device..." << std::endl;
        device.Disconnect();
        std::cout << "✓ Disconnected" << std::endl;

        std::cout << std::endl;
        std::cout << "Test completed successfully!" << std::endl;
        return 0;

    } catch (const std::exception& e) {
        std::cerr << "ERROR: " << e.what() << std::endl;
        return 1;
    }
}
