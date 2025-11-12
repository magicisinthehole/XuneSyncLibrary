/*
 * PTP/IP Client Implementation
 */

#include "ptpip_client.h"
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <cstring>
#include <stdexcept>
#include <iostream>
#include <sstream>
#include <iomanip>
#include <errno.h>
#include <thread>
#include <chrono>

namespace ptpip {

PTPIPClient::PTPIPClient(const std::string& host, const std::string& session_guid, const std::string& pc_name)
    : host_(host)
    , session_guid_(session_guid)
    , pc_name_(pc_name)
    , cmd_socket_(-1)
    , event_socket_(-1)
    , connected_(false)
    , connection_number_(0)
    , session_id_(0)
    , transaction_id_(0)
{
}

PTPIPClient::~PTPIPClient() {
    disconnect();
}

bool PTPIPClient::connect() {
    try {
        // 1. Connect command channel with retry loop
        std::cout << "Connecting to " << host_ << ":" << PTPIP_PORT << " (command channel)..." << std::endl;

        struct sockaddr_in server_addr;
        memset(&server_addr, 0, sizeof(server_addr));
        server_addr.sin_family = AF_INET;
        server_addr.sin_port = htons(PTPIP_PORT);
        if (inet_pton(AF_INET, host_.c_str(), &server_addr.sin_addr) <= 0) {
            throw std::runtime_error("Invalid IP address");
        }

        std::cout << "  Attempting connection (press Ctrl+C to stop)..." << std::endl;

        // Retry connection until successful or interrupted
        bool connected = false;
        int attempt = 0;

        while (!connected) {
            attempt++;

            // Create new socket for each attempt
            if (cmd_socket_ >= 0) {
                close(cmd_socket_);
            }

            cmd_socket_ = socket(AF_INET, SOCK_STREAM, 0);
            if (cmd_socket_ < 0) {
                throw std::runtime_error("Failed to create command socket");
            }

            // Set socket to non-blocking
            int flags = fcntl(cmd_socket_, F_GETFL, 0);
            fcntl(cmd_socket_, F_SETFL, flags | O_NONBLOCK);

            int conn_result = ::connect(cmd_socket_, (struct sockaddr*)&server_addr, sizeof(server_addr));

            if (conn_result == 0) {
                // Connected immediately
                connected = true;
            } else if (errno == EINPROGRESS) {
                // Connection in progress, wait with short timeout
                fd_set write_fds;
                FD_ZERO(&write_fds);
                FD_SET(cmd_socket_, &write_fds);

                struct timeval timeout;
                timeout.tv_sec = 0;
                timeout.tv_usec = 500000;  // 500ms

                int select_result = select(cmd_socket_ + 1, NULL, &write_fds, NULL, &timeout);

                if (select_result > 0) {
                    // Check if connection succeeded
                    int so_error;
                    socklen_t len = sizeof(so_error);
                    getsockopt(cmd_socket_, SOL_SOCKET, SO_ERROR, &so_error, &len);

                    if (so_error == 0) {
                        connected = true;
                    }
                }
            }

            if (!connected) {
                if (attempt % 10 == 0) {
                    std::cout << "  Still trying... (" << (attempt / 2) << "s elapsed)" << std::endl;
                }
                // Wait 500ms before next attempt (non-blocking check)
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
            }
        }

        if (!connected) {
            throw std::runtime_error("Connection failed");
        }

        // Set socket back to blocking mode
        int flags = fcntl(cmd_socket_, F_GETFL, 0);
        fcntl(cmd_socket_, F_SETFL, flags & ~O_NONBLOCK);

        // Set receive/send timeouts for ongoing communication
        struct timeval io_timeout;
        io_timeout.tv_sec = 30;
        io_timeout.tv_usec = 0;
        setsockopt(cmd_socket_, SOL_SOCKET, SO_RCVTIMEO, &io_timeout, sizeof(io_timeout));
        setsockopt(cmd_socket_, SOL_SOCKET, SO_SNDTIMEO, &io_timeout, sizeof(io_timeout));

        std::cout << "  ✓ TCP connection established" << std::endl;

        // 2. Send Init Command Request
        auto init_req = build_init_command_request();
        send_raw(cmd_socket_, init_req);
        std::cout << "  ✓ Sent Init Command Request" << std::endl;

        // 3. Receive Init Command Ack
        auto init_ack = recv_packet(cmd_socket_);
        connection_number_ = parse_init_ack(init_ack);
        std::cout << "  ✓ Command channel established (ConnID=" << connection_number_ << ")" << std::endl;

        // 4. Connect event channel
        std::cout << "Connecting event channel..." << std::endl;
        event_socket_ = socket(AF_INET, SOCK_STREAM, 0);
        if (event_socket_ < 0) {
            throw std::runtime_error("Failed to create event socket");
        }

        if (::connect(event_socket_, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
            throw std::runtime_error("Failed to connect event socket");
        }

        // 5. Send Init Event Request
        auto init_event = build_init_event_request(connection_number_);
        send_raw(event_socket_, init_event);

        // 6. Receive Init Event Ack
        auto event_ack = recv_raw(event_socket_, 8);
        if (event_ack.size() >= 8) {
            uint32_t evt_type;
            memcpy(&evt_type, &event_ack[4], 4);
            if (evt_type == static_cast<uint32_t>(PacketType::INIT_EVENT_ACK)) {
                std::cout << "  ✓ Event channel established" << std::endl;
            }
        }

        connected_ = true;
        return true;

    } catch (const std::exception& e) {
        std::cerr << "Connection failed: " << e.what() << std::endl;
        disconnect();
        return false;
    }
}

void PTPIPClient::disconnect() {
    if (cmd_socket_ >= 0) {
        close(cmd_socket_);
        cmd_socket_ = -1;
    }
    if (event_socket_ >= 0) {
        close(event_socket_);
        event_socket_ = -1;
    }
    connected_ = false;
}

bool PTPIPClient::open_session(uint32_t session_id) {
    auto resp = send_operation(OperationCode::OPEN_SESSION, {session_id});
    if (resp.response_code == ResponseCode::OK) {
        session_id_ = session_id;
        std::cout << "  ✓ Session opened: " << session_id << std::endl;
        return true;
    }
    std::cerr << "Open session failed" << std::endl;
    return false;
}

bool PTPIPClient::close_session() {
    if (session_id_ == 0) {
        return true;
    }

    auto resp = send_operation(OperationCode::CLOSE_SESSION, {});
    if (resp.response_code == ResponseCode::OK) {
        session_id_ = 0;
        std::cout << "  ✓ Session closed" << std::endl;
        return true;
    }
    return false;
}

std::vector<uint8_t> PTPIPClient::get_device_info() {
    auto resp = send_operation(OperationCode::GET_DEVICE_INFO, {});
    return resp.data;
}

std::vector<uint32_t> PTPIPClient::get_storage_ids() {
    auto resp = send_operation(OperationCode::GET_STORAGE_IDS, {});

    std::vector<uint32_t> storage_ids;
    if (resp.response_code == ResponseCode::OK && resp.data.size() >= 4) {
        uint32_t count;
        memcpy(&count, &resp.data[0], 4);

        for (uint32_t i = 0; i < count && (4 + (i + 1) * 4) <= resp.data.size(); i++) {
            uint32_t storage_id;
            memcpy(&storage_id, &resp.data[4 + i * 4], 4);
            storage_ids.push_back(storage_id);
        }
    }
    return storage_ids;
}

std::vector<uint32_t> PTPIPClient::get_object_handles(uint32_t storage_id, uint32_t object_format, uint32_t parent) {
    auto resp = send_operation(OperationCode::GET_OBJECT_HANDLES, {storage_id, object_format, parent});

    std::vector<uint32_t> handles;
    if (resp.response_code == ResponseCode::OK && resp.data.size() >= 4) {
        uint32_t count;
        memcpy(&count, &resp.data[0], 4);

        for (uint32_t i = 0; i < count && (4 + (i + 1) * 4) <= resp.data.size(); i++) {
            uint32_t handle;
            memcpy(&handle, &resp.data[4 + i * 4], 4);
            handles.push_back(handle);
        }
    }
    return handles;
}

ObjectInfo PTPIPClient::get_object_info(uint32_t handle) {
    auto resp = send_operation(OperationCode::GET_OBJECT_INFO, {handle});

    ObjectInfo info = {};
    info.handle = handle;

    if (resp.response_code == ResponseCode::OK && resp.data.size() >= 52) {
        memcpy(&info.storage_id, &resp.data[0], 4);
        memcpy(&info.format, &resp.data[4], 2);
        memcpy(&info.protection, &resp.data[6], 2);
        memcpy(&info.object_size, &resp.data[8], 4);
        memcpy(&info.parent, &resp.data[20], 4);

        // Extract filename (PTP string at offset 52)
        if (resp.data.size() > 52) {
            uint8_t str_len = resp.data[52];
            if (str_len > 0 && 53 + (str_len * 2) <= resp.data.size()) {
                std::u16string u16str;
                for (uint8_t i = 0; i < str_len; i++) {
                    uint16_t ch;
                    memcpy(&ch, &resp.data[53 + i * 2], 2);
                    if (ch != 0) {
                        u16str.push_back(ch);
                    }
                }
                // Convert UTF-16LE to UTF-8 (simplified - ASCII only)
                for (char16_t ch : u16str) {
                    if (ch < 128) {
                        info.filename += static_cast<char>(ch);
                    }
                }
            }
        }

        if (info.filename.empty()) {
            std::ostringstream oss;
            oss << "Object_" << std::hex << std::setw(8) << std::setfill('0') << handle;
            info.filename = oss.str();
        }
    }

    return info;
}

std::vector<uint8_t> PTPIPClient::get_object(uint32_t handle) {
    auto resp = send_operation(OperationCode::GET_OBJECT, {handle});
    return resp.data;
}

OperationResponse PTPIPClient::send_operation(OperationCode opcode, const std::vector<uint32_t>& params, const std::vector<uint8_t>& send_data) {
    transaction_id_++;

    // Determine data phase
    uint32_t data_phase = send_data.empty() ? 0x00000000 : 0x00000001;

    // Send operation request
    auto op_req = build_operation_request(opcode, transaction_id_, params);
    send_raw(cmd_socket_, op_req);

    // If sending data, send it now
    if (!send_data.empty()) {
        auto start_data = build_start_data(transaction_id_, send_data.size());
        send_raw(cmd_socket_, start_data);

        auto data_pkt = build_data_packet(transaction_id_, send_data);
        send_raw(cmd_socket_, data_pkt);

        auto end_data = build_end_data(transaction_id_);
        send_raw(cmd_socket_, end_data);
    }

    // Receive response (may include data packets)
    std::vector<uint8_t> received_data;
    OperationResponse op_resp;

    while (true) {
        auto packet = recv_packet(cmd_socket_);
        auto pkt_type = get_packet_type(packet);

        if (pkt_type == PacketType::OPERATION_RESPONSE) {
            op_resp = parse_operation_response(packet);
            break;
        } else if (pkt_type == PacketType::DATA || pkt_type == PacketType::END_DATA) {
            uint32_t tid;
            auto data = parse_data_packet(packet, tid);
            received_data.insert(received_data.end(), data.begin(), data.end());

            if (pkt_type == PacketType::END_DATA) {
                // Get final response
                packet = recv_packet(cmd_socket_);
                op_resp = parse_operation_response(packet);
                break;
            }
        } else if (pkt_type == PacketType::START_DATA) {
            // Just skip, data follows
            continue;
        }
    }

    op_resp.data = received_data;
    return op_resp;
}

// Packet builders
std::vector<uint8_t> PTPIPClient::build_init_command_request() {
    // Convert Session GUID (0xd221) string to 16 bytes
    // This is the authentication token from the device
    std::vector<uint8_t> guid_bytes(16, 0);
    std::string guid_clean = session_guid_;
    guid_clean.erase(std::remove(guid_clean.begin(), guid_clean.end(), '-'), guid_clean.end());
    guid_clean.erase(std::remove(guid_clean.begin(), guid_clean.end(), '{'), guid_clean.end());
    guid_clean.erase(std::remove(guid_clean.begin(), guid_clean.end(), '}'), guid_clean.end());

    for (size_t i = 0; i < 16 && i * 2 < guid_clean.size(); i++) {
        std::string byte_str = guid_clean.substr(i * 2, 2);
        guid_bytes[i] = static_cast<uint8_t>(std::stoi(byte_str, nullptr, 16));
    }

    // Convert name to UTF-16LE with null terminator
    std::vector<uint8_t> name_utf16;
    for (char c : pc_name_) {
        name_utf16.push_back(c);
        name_utf16.push_back(0);
    }
    name_utf16.push_back(0);
    name_utf16.push_back(0);

    uint32_t length = 8 + 16 + name_utf16.size() + 4;
    uint32_t packet_type = static_cast<uint32_t>(PacketType::INIT_COMMAND_REQUEST);
    uint32_t protocol_version = 0x00010000;

    std::vector<uint8_t> packet;
    packet.resize(length);

    memcpy(&packet[0], &length, 4);
    memcpy(&packet[4], &packet_type, 4);
    memcpy(&packet[8], guid_bytes.data(), 16);
    memcpy(&packet[24], name_utf16.data(), name_utf16.size());
    memcpy(&packet[24 + name_utf16.size()], &protocol_version, 4);

    return packet;
}

std::vector<uint8_t> PTPIPClient::build_init_event_request(uint32_t connection_number) {
    uint32_t length = 12;
    uint32_t packet_type = static_cast<uint32_t>(PacketType::INIT_EVENT_REQUEST);

    std::vector<uint8_t> packet(12);
    memcpy(&packet[0], &length, 4);
    memcpy(&packet[4], &packet_type, 4);
    memcpy(&packet[8], &connection_number, 4);

    return packet;
}

std::vector<uint8_t> PTPIPClient::build_operation_request(OperationCode opcode, uint32_t transaction_id, const std::vector<uint32_t>& params) {
    uint32_t length = 18 + (params.size() * 4);
    uint32_t packet_type = static_cast<uint32_t>(PacketType::OPERATION_REQUEST);
    uint32_t data_phase = 0x00000000;
    uint16_t op = static_cast<uint16_t>(opcode);

    std::vector<uint8_t> packet(length);
    size_t offset = 0;

    memcpy(&packet[offset], &length, 4); offset += 4;
    memcpy(&packet[offset], &packet_type, 4); offset += 4;
    memcpy(&packet[offset], &data_phase, 4); offset += 4;
    memcpy(&packet[offset], &op, 2); offset += 2;
    memcpy(&packet[offset], &transaction_id, 4); offset += 4;

    for (uint32_t param : params) {
        memcpy(&packet[offset], &param, 4);
        offset += 4;
    }

    return packet;
}

std::vector<uint8_t> PTPIPClient::build_start_data(uint32_t transaction_id, uint64_t total_length) {
    uint32_t length = 20;
    uint32_t packet_type = static_cast<uint32_t>(PacketType::START_DATA);

    std::vector<uint8_t> packet(20);
    memcpy(&packet[0], &length, 4);
    memcpy(&packet[4], &packet_type, 4);
    memcpy(&packet[8], &transaction_id, 4);
    memcpy(&packet[12], &total_length, 8);

    return packet;
}

std::vector<uint8_t> PTPIPClient::build_data_packet(uint32_t transaction_id, const std::vector<uint8_t>& data) {
    uint32_t length = 12 + data.size();
    uint32_t packet_type = static_cast<uint32_t>(PacketType::DATA);

    std::vector<uint8_t> packet(length);
    memcpy(&packet[0], &length, 4);
    memcpy(&packet[4], &packet_type, 4);
    memcpy(&packet[8], &transaction_id, 4);
    if (!data.empty()) {
        memcpy(&packet[12], data.data(), data.size());
    }

    return packet;
}

std::vector<uint8_t> PTPIPClient::build_end_data(uint32_t transaction_id) {
    uint32_t length = 12;
    uint32_t packet_type = static_cast<uint32_t>(PacketType::END_DATA);

    std::vector<uint8_t> packet(12);
    memcpy(&packet[0], &length, 4);
    memcpy(&packet[4], &packet_type, 4);
    memcpy(&packet[8], &transaction_id, 4);

    return packet;
}

// Packet parsers
PacketType PTPIPClient::get_packet_type(const std::vector<uint8_t>& packet) {
    if (packet.size() < 8) {
        throw std::runtime_error("Packet too short");
    }
    uint32_t type;
    memcpy(&type, &packet[4], 4);
    return static_cast<PacketType>(type);
}

uint32_t PTPIPClient::parse_init_ack(const std::vector<uint8_t>& packet) {
    if (packet.size() < 12) {
        throw std::runtime_error("Init Ack packet too short");
    }
    uint32_t connection_number;
    memcpy(&connection_number, &packet[8], 4);
    return connection_number;
}

OperationResponse PTPIPClient::parse_operation_response(const std::vector<uint8_t>& packet) {
    if (packet.size() < 14) {
        throw std::runtime_error("Operation Response packet too short");
    }

    OperationResponse resp;
    uint16_t response_code;
    memcpy(&response_code, &packet[8], 2);
    resp.response_code = static_cast<ResponseCode>(response_code);

    memcpy(&resp.transaction_id, &packet[10], 4);

    // Parse parameters
    for (size_t i = 14; i + 4 <= packet.size(); i += 4) {
        uint32_t param;
        memcpy(&param, &packet[i], 4);
        resp.params.push_back(param);
    }

    return resp;
}

std::vector<uint8_t> PTPIPClient::parse_data_packet(const std::vector<uint8_t>& packet, uint32_t& transaction_id) {
    if (packet.size() < 12) {
        return {};
    }

    memcpy(&transaction_id, &packet[8], 4);

    std::vector<uint8_t> data;
    if (packet.size() > 12) {
        data.insert(data.end(), packet.begin() + 12, packet.end());
    }

    return data;
}

// Low-level I/O
void PTPIPClient::send_raw(int sock, const std::vector<uint8_t>& data) {
    size_t sent = 0;
    while (sent < data.size()) {
        ssize_t n = send(sock, data.data() + sent, data.size() - sent, 0);
        if (n < 0) {
            throw std::runtime_error("Send failed");
        }
        sent += n;
    }
}

std::vector<uint8_t> PTPIPClient::recv_raw(int sock, size_t size) {
    std::vector<uint8_t> data;
    data.reserve(size);

    while (data.size() < size) {
        uint8_t buffer[BUFFER_SIZE];
        ssize_t n = recv(sock, buffer, std::min(size - data.size(), BUFFER_SIZE), 0);
        if (n <= 0) {
            break;
        }
        data.insert(data.end(), buffer, buffer + n);
    }

    return data;
}

std::vector<uint8_t> PTPIPClient::recv_packet(int sock) {
    // Read header (8 bytes)
    auto header = recv_raw(sock, 8);
    if (header.size() < 8) {
        throw std::runtime_error("Connection closed");
    }

    uint32_t length;
    memcpy(&length, &header[0], 4);

    // Read rest of packet
    uint32_t remaining = length - 8;
    if (remaining > 0) {
        auto body = recv_raw(sock, remaining);
        header.insert(header.end(), body.begin(), body.end());
    }

    return header;
}

} // namespace ptpip
