/*
 * SSDP Discovery - Listen for Zune device advertisements
 *
 * The Zune device broadcasts SSDP NOTIFY packets on 239.255.255.250:1900
 * announcing its presence. This module listens for those packets and
 * extracts device information.
 */

#pragma once

#include <string>
#include <vector>
#include <functional>
#include <thread>
#include <atomic>
#include <mutex>
#include <map>

namespace ssdp {

struct ZuneDevice {
    std::string ip_address;       // Device IP (from packet source)
    std::string uuid;             // Device UUID (from NT/USN field)
    std::string mac_address;      // Extracted from UUID (last 12 hex chars)
    std::string location_url;     // HTTP URL to Device.xml
    uint16_t http_port;           // HTTP port from LOCATION
    time_t last_seen;             // Timestamp of last NOTIFY packet

    bool operator==(const ZuneDevice& other) const {
        return uuid == other.uuid;
    }
};

class SSDPDiscovery {
public:
    // Callback signature: void callback(const ZuneDevice& device, bool is_new)
    using DeviceCallback = std::function<void(const ZuneDevice&, bool)>;

    SSDPDiscovery();
    ~SSDPDiscovery();

    // Start listening for SSDP NOTIFY packets
    // Blocks until stop() is called or an error occurs
    void start_listening(DeviceCallback on_device_found);

    // Start listening in a background thread
    void start_background(DeviceCallback on_device_found);

    // Stop listening
    void stop();

    // Get all currently known devices
    std::vector<ZuneDevice> get_devices() const;

    // Check if currently listening
    bool is_running() const { return running_; }

private:
    // SSDP multicast address and port
    static constexpr const char* SSDP_ADDR = "239.255.255.250";
    static constexpr uint16_t SSDP_PORT = 1900;
    static constexpr int BUFFER_SIZE = 2048;
    static constexpr int DEVICE_TIMEOUT = 600; // 10 minutes (2x cache-control max-age)

    int socket_fd_;
    std::atomic<bool> running_;
    std::thread listen_thread_;
    mutable std::mutex devices_mutex_;
    std::map<std::string, ZuneDevice> devices_; // UUID -> Device

    // Create and configure socket
    bool create_socket();

    // Parse SSDP NOTIFY packet
    bool parse_notify(const char* data, size_t len, const std::string& source_ip, ZuneDevice& device);

    // Extract MAC address from UUID
    std::string extract_mac_from_uuid(const std::string& uuid);

    // Listening loop
    void listen_loop(DeviceCallback callback);

    // Cleanup expired devices
    void cleanup_expired_devices();
};

} // namespace ssdp
