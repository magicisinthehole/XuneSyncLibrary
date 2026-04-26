#include "lib/src/ZuneDevice.h"
#include "lib/src/protocols/http/ZuneHTTPInterceptor.h"
#include <atomic>
#include <chrono>
#include <csignal>
#include <iomanip>
#include <iostream>
#include <thread>

static std::atomic<bool> g_running{true};

void signal_handler(int signal) {
    std::cout << "\nReceived signal " << signal << ", shutting down..." << std::endl;
    g_running.store(false);
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
    std::string server_host = "catalog.xune.moe";

    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        if (arg == "--server" && i + 1 < argc) {
            server_host = argv[++i];
        }
        else if (arg == "--help") {
            std::cout << "Usage: " << argv[0] << " [options]\n"
                      << "\n"
                      << "Options:\n"
                      << "  --server HOST   Catalog host or IP (default: catalog.xune.moe)\n"
                      << "                  Example: --server 192.168.0.30\n"
                      << "  --help          Show this help message\n";
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
        while (g_running.load()) {
            if (device.ConnectUSB()) {
                break;
            }
            std::this_thread::sleep_for(std::chrono::seconds(2));
        }

        if (!g_running.load()) {
            return 0;
        }

        std::cout << std::endl;
        std::cout << "=== Device Connected! ===" << std::endl;
        std::cout << "Device: " << device.GetName() << std::endl;
        std::cout << "Serial: " << device.GetSerialNumber() << std::endl;
        std::cout << "Family: " << device.GetDeviceFamilyName() << std::endl;
        std::cout << std::endl;

        // Configure interceptor (proxy mode — Static is no longer wired up).
        InterceptorConfig config;
        std::string parsed_host = server_host;
        std::string scheme = "https://";
        if (parsed_host.rfind("http://", 0) == 0) {
            scheme = "http://";
            parsed_host = parsed_host.substr(7);
        } else if (parsed_host.rfind("https://", 0) == 0) {
            parsed_host = parsed_host.substr(8);
        }

        config.mode = InterceptionMode::Proxy;
        config.proxy_config.catalog_server = scheme + parsed_host;
        config.proxy_config.timeout_ms = 10000;

        // DNS resolution target: strip any :port suffix.
        std::string dns_host = parsed_host;
        if (auto colon_pos = dns_host.find(':'); colon_pos != std::string::npos) {
            dns_host = dns_host.substr(0, colon_pos);
        }
        config.server_ip = dns_host;

        std::cout << "=== Initializing HTTP Subsystem ===" << std::endl;
        std::cout << "Sending MTP vendor commands to device..." << std::endl;
        if (!device.InitializeHTTPSubsystem()) {
            std::cerr << "ERROR: Failed to initialize HTTP subsystem" << std::endl;
            return 1;
        }
        std::cout << std::endl;

        std::cout << "=== Starting HTTP Interceptor ===" << std::endl;
        std::cout << "  Mode: Proxy" << std::endl;
        std::cout << "  Server: " << config.proxy_config.catalog_server << std::endl;
        std::cout << "  DNS resolves to: " << config.server_ip << std::endl;
        std::cout << std::endl;

        std::cout << "Starting HTTP interceptor..." << std::endl;
        device.StartHTTPInterceptor(config);

        if (!device.IsHTTPInterceptorRunning()) {
            std::cerr << "ERROR: Failed to start HTTP interceptor" << std::endl;
            return 1;
        }

        std::cout << "[OK] HTTP interceptor running" << std::endl;
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

                std::cout << "[OK] Network mode established" << std::endl;
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
        std::cout << std::endl;
        std::cout << "Press Ctrl+C to stop..." << std::endl;
        std::cout << std::endl;

        // Drive the polling loop ourselves — EnableNetworkPolling() only flips a
        // flag; the production app polls from C# via PollNetworkData(). Without
        // this thread, no network packets are processed.
        std::thread poll_thread([&device]() {
            while (g_running.load()) {
                int rc = device.PollNetworkData(100);
                if (rc < 0) {
                    // Interceptor stopped or error — exit; the main loop will
                    // detect via IsHTTPInterceptorRunning().
                    break;
                }
            }
        });

        while (g_running.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(500));

            if (!device.IsHTTPInterceptorRunning()) {
                std::cerr << "WARNING: HTTP interceptor stopped unexpectedly" << std::endl;
                g_running.store(false);
                break;
            }
        }

        std::cout << std::endl;
        std::cout << "Stopping HTTP interceptor..." << std::endl;
        device.StopHTTPInterceptor();
        if (poll_thread.joinable()) {
            poll_thread.join();
        }
        std::cout << "[OK] Interceptor stopped" << std::endl;

        std::cout << "Disconnecting from device..." << std::endl;
        device.Disconnect();
        std::cout << "[OK] Disconnected" << std::endl;

        std::cout << std::endl;
        std::cout << "Monitoring completed!" << std::endl;
        return 0;

    } catch (const std::exception& e) {
        std::cerr << "ERROR: " << e.what() << std::endl;
        return 1;
    }
}
