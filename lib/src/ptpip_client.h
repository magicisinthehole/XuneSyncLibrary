/*
 * PTP/IP Client - TCP/IP connection to Zune device
 *
 * Based on ISO 15740 (PTP/IP) and Python implementation
 */

#pragma once

#include <string>
#include <vector>
#include <memory>
#include <cstdint>

namespace ptpip {

// PTP/IP packet types
enum class PacketType : uint32_t {
    INIT_COMMAND_REQUEST = 0x0001,
    INIT_COMMAND_ACK = 0x0002,
    INIT_EVENT_REQUEST = 0x0003,
    INIT_EVENT_ACK = 0x0004,
    INIT_FAIL = 0x0005,
    OPERATION_REQUEST = 0x0006,
    OPERATION_RESPONSE = 0x0007,
    EVENT = 0x0008,
    START_DATA = 0x0009,
    DATA = 0x000A,
    CANCEL_TRANSACTION = 0x000B,
    END_DATA = 0x000C,
    PROBE_REQUEST = 0x000D,
    PROBE_RESPONSE = 0x000E
};

// MTP operation codes
enum class OperationCode : uint16_t {
    GET_DEVICE_INFO = 0x1001,
    OPEN_SESSION = 0x1002,
    CLOSE_SESSION = 0x1003,
    GET_STORAGE_IDS = 0x1004,
    GET_STORAGE_INFO = 0x1005,
    GET_NUM_OBJECTS = 0x1006,
    GET_OBJECT_HANDLES = 0x1007,
    GET_OBJECT_INFO = 0x1008,
    GET_OBJECT = 0x1009
};

// MTP response codes
enum class ResponseCode : uint16_t {
    OK = 0x2001,
    GENERAL_ERROR = 0x2002,
    SESSION_NOT_OPEN = 0x2003,
    INVALID_TRANSACTION_ID = 0x2004,
    OPERATION_NOT_SUPPORTED = 0x2005,
    PARAMETER_NOT_SUPPORTED = 0x2006,
    INCOMPLETE_TRANSFER = 0x2007,
    INVALID_STORAGE_ID = 0x2008,
    INVALID_OBJECT_HANDLE = 0x2009
};

struct OperationResponse {
    ResponseCode response_code;
    uint32_t transaction_id;
    std::vector<uint32_t> params;
    std::vector<uint8_t> data;
};

struct ObjectInfo {
    uint32_t storage_id;
    uint16_t format;
    uint16_t protection;
    uint32_t object_size;
    uint32_t parent;
    uint32_t handle;
    std::string filename;
};

class PTPIPClient {
public:
    static constexpr int PTPIP_PORT = 15740;
    static constexpr size_t BUFFER_SIZE = 65536;

    PTPIPClient(const std::string& host, const std::string& session_guid, const std::string& pc_name = "ZuneWirelessSync");
    ~PTPIPClient();

    // Connection management
    bool connect();
    void disconnect();
    bool is_connected() const { return connected_; }

    // Session management
    bool open_session(uint32_t session_id = 1);
    bool close_session();

    // MTP operations
    std::vector<uint8_t> get_device_info();
    std::vector<uint32_t> get_storage_ids();
    std::vector<uint32_t> get_object_handles(uint32_t storage_id, uint32_t object_format = 0, uint32_t parent = 0xFFFFFFFF);
    ObjectInfo get_object_info(uint32_t handle);
    std::vector<uint8_t> get_object(uint32_t handle);

private:
    // Low-level packet operations
    OperationResponse send_operation(OperationCode opcode, const std::vector<uint32_t>& params = {}, const std::vector<uint8_t>& send_data = {});

    void send_packet(int sock, const std::vector<uint8_t>& packet);
    std::vector<uint8_t> recv_packet(int sock);
    void send_raw(int sock, const std::vector<uint8_t>& data);
    std::vector<uint8_t> recv_raw(int sock, size_t size);

    // Packet builders
    std::vector<uint8_t> build_init_command_request();
    std::vector<uint8_t> build_init_event_request(uint32_t connection_number);
    std::vector<uint8_t> build_operation_request(OperationCode opcode, uint32_t transaction_id, const std::vector<uint32_t>& params);
    std::vector<uint8_t> build_start_data(uint32_t transaction_id, uint64_t total_length);
    std::vector<uint8_t> build_data_packet(uint32_t transaction_id, const std::vector<uint8_t>& data);
    std::vector<uint8_t> build_end_data(uint32_t transaction_id);

    // Packet parsers
    PacketType get_packet_type(const std::vector<uint8_t>& packet);
    uint32_t parse_init_ack(const std::vector<uint8_t>& packet);
    OperationResponse parse_operation_response(const std::vector<uint8_t>& packet);
    std::vector<uint8_t> parse_data_packet(const std::vector<uint8_t>& packet, uint32_t& transaction_id);

    std::string host_;
    std::string session_guid_;  // Session GUID from device property 0xd221 (hex string, 36 chars with dashes)
    std::string pc_name_;

    int cmd_socket_;
    int event_socket_;
    bool connected_;

    uint32_t connection_number_;
    uint32_t session_id_;
    uint32_t transaction_id_;
};

} // namespace ptpip
