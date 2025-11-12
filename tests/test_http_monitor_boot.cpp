#include "lib/src/ZuneDevice.h"
#include "lib/src/protocols/http/ZuneHTTPInterceptor.h"
#include <iostream>
#include <iomanip>
#include <thread>
#include <chrono>
#include <csignal>

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
    std::cout << "=== Zune HTTP Monitor (Boot-time Traffic Capture) ===" << std::endl;
    std::cout << std::endl;

    // Parse command line arguments
    std::string mode = "proxy";
    std::string server_ip = "127.0.0.1";
    std::string data_directory = "/Users/andymoe/Documents/AppDevelopment/ZuneWirelessSync/ZuneArtistImages/artist_data";
    bool test_mode = false;

    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        if (arg == "--mode" && i + 1 < argc) {
            mode = argv[++i];
        }
        else if (arg == "--server" && i + 1 < argc) {
            server_ip = argv[++i];
        }
        else if (arg == "--data-dir" && i + 1 < argc) {
            data_directory = argv[++i];
        }
        else if (arg == "--test") {
            test_mode = true;
        }
        else if (arg == "--help") {
            std::cout << "Usage: " << argv[0] << " [options]" << std::endl;
            std::cout << std::endl;
            std::cout << "Options:" << std::endl;
            std::cout << "  --mode MODE     Interception mode: proxy or static (default: proxy)" << std::endl;
            std::cout << "  --server IP     Server IP address for proxy mode (default: 127.0.0.1)" << std::endl;
            std::cout << "                  Example: --server 185.165.44.133" << std::endl;
            std::cout << "  --data-dir PATH Artist data directory for static mode" << std::endl;
            std::cout << "                  (default: artist_data)" << std::endl;
            std::cout << "  --test          Enable test mode (redirects all UUIDs to test directory)" << std::endl;
            std::cout << "  --help          Show this help message" << std::endl;
            std::cout << std::endl;
            std::cout << "Examples:" << std::endl;
            std::cout << "  Proxy mode:  " << argv[0] << " --mode proxy --server 192.168.0.30" << std::endl;
            std::cout << "  Static mode: " << argv[0] << " --mode static --data-dir /path/to/artist_data" << std::endl;
            std::cout << "  Test mode:   " << argv[0] << " --mode static --test" << std::endl;
            std::cout << std::endl;
            return 0;
        }
    }

    // Install signal handlers
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    std::cout << "Waiting for Zune device to connect..." << std::endl;
    std::cout << "Connect your Zune device via USB to begin monitoring." << std::endl;
    std::cout << std::endl;

    try {
        // Create device
        ZuneDevice device;
        device.SetLogCallback(log_callback);

        // Poll for device connection
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

        std::cout << std::endl;
        std::cout << "=== Device Connected! ===" << std::endl;
        std::cout << "Device: " << device.GetName() << std::endl;
        std::cout << "Serial: " << device.GetSerialNumber() << std::endl;
        std::cout << "Model:  " << device.GetModel() << std::endl;
        std::cout << std::endl;

        // Configure interceptor based on mode
        InterceptorConfig config;

        if (mode == "static") {
            // Static mode - serve from local files
            config.mode = InterceptionMode::Static;
            config.static_config.data_directory = data_directory;
            config.static_config.test_mode = test_mode;
            config.server_ip = "192.168.0.30";  // DNS resolves all hosts to this IP
        } else {
            // Proxy mode - forward to remote server
            // Parse server_ip to extract IP and build URL
            std::string parsed_ip = server_ip;

            // Remove http:// or https:// prefix if present
            if (parsed_ip.find("http://") == 0) {
                parsed_ip = parsed_ip.substr(7);  // Remove "http://"
            } else if (parsed_ip.find("https://") == 0) {
                parsed_ip = parsed_ip.substr(8);  // Remove "https://"
            }

            // Build server URL
            std::string server_url = "http://" + parsed_ip;

            config.mode = InterceptionMode::Proxy;
            config.proxy_config.catalog_server = server_url;
            config.proxy_config.image_server = "";
            config.proxy_config.art_server = "";
            config.proxy_config.mix_server = "";
            config.proxy_config.timeout_ms = 10000;

            // Extract just the IP portion (without port) for DNS resolution
            std::string dns_ip = parsed_ip;
            size_t colon_pos = dns_ip.find(':');
            if (colon_pos != std::string::npos) {
                dns_ip = dns_ip.substr(0, colon_pos);
            }
            config.server_ip = dns_ip;
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
            std::cout << "  Data Directory: " << data_directory << std::endl;
            if (test_mode) {
                std::cout << "  Test Mode: Enabled (all UUIDs -> 00000000-0000-0000-0000-000000000000)" << std::endl;
            }
            std::cout << "  DNS resolves all hosts to: " << config.server_ip << std::endl;
        } else {
            std::cout << "  Mode: Proxy" << std::endl;
            std::cout << "  Server: " << config.proxy_config.catalog_server << std::endl;
            std::cout << "  DNS resolves to: " << config.server_ip << std::endl;
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

        // Trigger network mode to establish PPP/IPCP handshake
        std::cout << "=== Triggering Network Mode ===" << std::endl;
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

                // Enable continuous polling (after IPCP handshake is sent)
                device.EnableNetworkPolling();

                std::cout << "✓ Network mode established" << std::endl;
                std::cout << "Device should now start HTTP metadata requests..." << std::endl;

            } catch (const std::exception& e) {
                std::cerr << "Network mode trigger attempt " << retry << " failed: " << e.what() << std::endl;

                if (retry < max_network_retries) {
                    std::cout << "Waiting before retry..." << std::endl;
                } else {
                    std::cerr << "ERROR: Device did not enter network mode after " << max_network_retries << " attempts" << std::endl;
                }
            }
        }

        if (!network_mode_success) {
            std::cerr << "ERROR: Failed to establish network mode" << std::endl;
            return 1;
        }

        std::cout << std::endl;
        std::cout << "Monitoring for HTTP requests from device..." << std::endl;
        std::cout << std::endl;
        std::cout << "Expected traffic:" << std::endl;
        std::cout << "  - GET /v3.0/en-US/music/artist/{uuid}/biography" << std::endl;
        std::cout << "  - GET /v3.0/en-US/music/artist/{uuid}/images" << std::endl;
        std::cout << "  - GET /v3.0/en-US/music/artist/{uuid}/deviceBackgroundImage" << std::endl;
        if (mode == "static") {
            std::cout << std::endl;
            std::cout << "Serving from: " << data_directory << "/{uuid}/" << std::endl;
        }
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
        std::cout << "Monitoring completed!" << std::endl;
        return 0;

    } catch (const std::exception& e) {
        std::cerr << "ERROR: " << e.what() << std::endl;
        return 1;
    }
}
