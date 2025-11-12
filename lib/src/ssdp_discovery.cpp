/*
 * SSDP Discovery Implementation
 */

#include "ssdp_discovery.h"
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <cstring>
#include <iostream>
#include <sstream>
#include <algorithm>
#include <ctime>
#include <errno.h>

namespace ssdp {

SSDPDiscovery::SSDPDiscovery()
    : socket_fd_(-1)
    , running_(false)
{
}

SSDPDiscovery::~SSDPDiscovery() {
    stop();
}

bool SSDPDiscovery::create_socket() {
    // Create UDP socket
    socket_fd_ = socket(AF_INET, SOCK_DGRAM, 0);
    if (socket_fd_ < 0) {
        std::cerr << "Failed to create socket" << std::endl;
        return false;
    }

    // Allow multiple listeners on the same port (for multiple instances)
    int reuse = 1;
    if (setsockopt(socket_fd_, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) < 0) {
        std::cerr << "Failed to set SO_REUSEADDR" << std::endl;
        close(socket_fd_);
        socket_fd_ = -1;
        return false;
    }

#ifdef SO_REUSEPORT
    if (setsockopt(socket_fd_, SOL_SOCKET, SO_REUSEPORT, &reuse, sizeof(reuse)) < 0) {
        // Not fatal, continue
    }
#endif

    // Bind to SSDP port
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(SSDP_PORT);

    if (bind(socket_fd_, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        std::cerr << "Failed to bind to port " << SSDP_PORT << std::endl;
        close(socket_fd_);
        socket_fd_ = -1;
        return false;
    }

    // Join multicast group
    struct ip_mreq mreq;
    mreq.imr_multiaddr.s_addr = inet_addr(SSDP_ADDR);
    mreq.imr_interface.s_addr = INADDR_ANY;

    if (setsockopt(socket_fd_, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq)) < 0) {
        std::cerr << "Failed to join multicast group" << std::endl;
        close(socket_fd_);
        socket_fd_ = -1;
        return false;
    }

    // Set receive timeout so we can check running flag periodically
    struct timeval timeout;
    timeout.tv_sec = 1;   // 1 second timeout
    timeout.tv_usec = 0;
    if (setsockopt(socket_fd_, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) < 0) {
        std::cerr << "Warning: Failed to set socket timeout" << std::endl;
        // Not fatal, continue
    }

    return true;
}

std::string SSDPDiscovery::extract_mac_from_uuid(const std::string& uuid) {
    // UUID format: 00000000-0000-0000-ffff-001dd8feaba2
    // MAC is the last 12 hex chars: 001dd8feaba2

    // Find last dash
    size_t last_dash = uuid.rfind('-');
    if (last_dash == std::string::npos || last_dash + 1 >= uuid.size()) {
        return "";
    }

    std::string mac_part = uuid.substr(last_dash + 1);

    // Format as MAC: 00:1d:d8:fe:ab:a2
    if (mac_part.length() != 12) {
        return mac_part; // Return as-is if unexpected length
    }

    std::string formatted;
    for (size_t i = 0; i < 12; i += 2) {
        if (i > 0) formatted += ":";
        formatted += mac_part.substr(i, 2);
    }

    return formatted;
}

bool SSDPDiscovery::parse_notify(const char* data, size_t len, const std::string& source_ip, ZuneDevice& device) {
    std::string packet(data, len);

    // Check if it's a NOTIFY packet
    if (packet.find("NOTIFY * HTTP/1.1") == std::string::npos &&
        packet.find("NOTIFY *") == std::string::npos) {
        return false;
    }

    // Check for Zune server
    if (packet.find("Zune/2.0") == std::string::npos) {
        return false;
    }

    // Parse headers
    std::istringstream stream(packet);
    std::string line;

    std::string uuid;
    std::string location;

    while (std::getline(stream, line)) {
        // Remove \r if present
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }

        // Parse header: value
        size_t colon = line.find(':');
        if (colon == std::string::npos) continue;

        std::string header = line.substr(0, colon);
        std::string value = line.substr(colon + 1);

        // Trim leading whitespace from value
        size_t value_start = value.find_first_not_of(" \t");
        if (value_start != std::string::npos) {
            value = value.substr(value_start);
        }

        // Convert header to uppercase for comparison
        std::transform(header.begin(), header.end(), header.begin(), ::toupper);

        if (header == "NT" || header == "USN") {
            // Extract UUID from "uuid:00000000-0000-0000-ffff-001dd8feaba2"
            size_t uuid_start = value.find("uuid:");
            if (uuid_start != std::string::npos) {
                uuid = value.substr(uuid_start + 5);
            }
        } else if (header == "LOCATION") {
            location = value;
        }
    }

    // Validate we got required fields
    if (uuid.empty()) {
        return false;
    }

    // Fill device structure
    device.ip_address = source_ip;
    device.uuid = uuid;
    device.mac_address = extract_mac_from_uuid(uuid);
    device.location_url = location;
    device.last_seen = time(nullptr);

    // Extract HTTP port from LOCATION (format: http://ip:port/path)
    device.http_port = 0;
    if (!location.empty()) {
        size_t port_start = location.find(':', 7); // Skip "http://"
        if (port_start != std::string::npos) {
            size_t port_end = location.find('/', port_start);
            if (port_end != std::string::npos) {
                std::string port_str = location.substr(port_start + 1, port_end - port_start - 1);
                device.http_port = static_cast<uint16_t>(std::stoi(port_str));
            }
        }
    }

    return true;
}

void SSDPDiscovery::cleanup_expired_devices() {
    std::lock_guard<std::mutex> lock(devices_mutex_);

    time_t now = time(nullptr);
    auto it = devices_.begin();
    while (it != devices_.end()) {
        if (now - it->second.last_seen > DEVICE_TIMEOUT) {
            std::cout << "Device expired: " << it->second.ip_address
                      << " (UUID: " << it->second.uuid << ")" << std::endl;
            it = devices_.erase(it);
        } else {
            ++it;
        }
    }
}

void SSDPDiscovery::listen_loop(DeviceCallback callback) {
    char buffer[BUFFER_SIZE];
    struct sockaddr_in sender_addr;
    socklen_t sender_len = sizeof(sender_addr);

    std::cout << "SSDP Discovery listening on " << SSDP_ADDR << ":" << SSDP_PORT << std::endl;
    std::cout << "Waiting for Zune devices to announce themselves..." << std::endl;

    while (running_) {
        ssize_t recv_len = recvfrom(socket_fd_, buffer, BUFFER_SIZE - 1, 0,
                                     (struct sockaddr*)&sender_addr, &sender_len);

        if (recv_len < 0) {
            // Check if it's a timeout (expected) or real error
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                // Timeout - just continue loop to check running flags
                continue;
            }

            if (running_) {
                std::cerr << "Error receiving packet: " << strerror(errno) << std::endl;
            }
            break;
        }

        buffer[recv_len] = '\0';

        // Get source IP
        char sender_ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &sender_addr.sin_addr, sender_ip, sizeof(sender_ip));

        // Parse packet
        ZuneDevice device;
        if (parse_notify(buffer, recv_len, sender_ip, device)) {
            bool is_new = false;

            {
                std::lock_guard<std::mutex> lock(devices_mutex_);

                // Check if this is a new device
                auto it = devices_.find(device.uuid);
                if (it == devices_.end()) {
                    is_new = true;
                    std::cout << "\n=== Zune Device Discovered ===" << std::endl;
                    std::cout << "  IP Address: " << device.ip_address << std::endl;
                    std::cout << "  UUID:       " << device.uuid << std::endl;
                    std::cout << "  MAC:        " << device.mac_address << std::endl;
                    if (!device.location_url.empty()) {
                        std::cout << "  Location:   " << device.location_url << std::endl;
                    }
                    std::cout << "==============================\n" << std::endl;
                }

                devices_[device.uuid] = device;
            }

            // Call callback
            if (callback) {
                callback(device, is_new);
            }
        }

        // Periodically cleanup expired devices
        static time_t last_cleanup = time(nullptr);
        time_t now = time(nullptr);
        if (now - last_cleanup > 60) { // Every minute
            cleanup_expired_devices();
            last_cleanup = now;
        }
    }
}

void SSDPDiscovery::start_listening(DeviceCallback on_device_found) {
    if (running_) {
        std::cerr << "Already listening" << std::endl;
        return;
    }

    if (!create_socket()) {
        return;
    }

    running_ = true;
    listen_loop(on_device_found);
}

void SSDPDiscovery::start_background(DeviceCallback on_device_found) {
    if (running_) {
        std::cerr << "Already listening" << std::endl;
        return;
    }

    if (!create_socket()) {
        return;
    }

    running_ = true;
    listen_thread_ = std::thread(&SSDPDiscovery::listen_loop, this, on_device_found);
}

void SSDPDiscovery::stop() {
    if (!running_) {
        return;
    }

    running_ = false;

    if (socket_fd_ >= 0) {
        // Shutdown socket to unblock recvfrom
        shutdown(socket_fd_, SHUT_RDWR);
        close(socket_fd_);
        socket_fd_ = -1;
    }

    if (listen_thread_.joinable()) {
        listen_thread_.join();
    }

    std::cout << "SSDP Discovery stopped" << std::endl;
}

std::vector<ZuneDevice> SSDPDiscovery::get_devices() const {
    std::lock_guard<std::mutex> lock(devices_mutex_);

    std::vector<ZuneDevice> result;
    result.reserve(devices_.size());

    for (const auto& pair : devices_) {
        result.push_back(pair.second);
    }

    return result;
}

} // namespace ssdp
