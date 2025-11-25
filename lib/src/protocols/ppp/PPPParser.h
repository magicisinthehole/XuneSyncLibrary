#pragma once

#include <mtp/ByteArray.h>
#include <cstdint>
#include <string>
#include <map>
#include <vector>

/**
 * PPPParser
 *
 * Parses Point-to-Point Protocol (PPP) frames from USB bulk transfers.
 *
 * Frame Structure:
 * ┌──────┬──────────┬─────────────────┬──────┐
 * │ 0x7e │ Protocol │   Payload       │ 0x7e │
 * └──────┴──────────┴─────────────────┴──────┘
 *
 * Protocol values:
 * - 0x21: IPv4 packet
 * - 0xC0 0x21: Link Control Protocol (LCP)
 * - 0x80 0x21: Internet Protocol Control Protocol (IPCP)
 *
 * For Zune HTTP traffic, we primarily care about 0x21 (IPv4).
 */
class PPPParser {
public:
    /**
     * Check if data contains a valid PPP frame
     * @param data USB packet data
     * @return true if frame starts with 0x7e and has valid structure
     */
    static bool IsValidFrame(const mtp::ByteArray& data);

    /**
     * Extract payload from PPP frame
     * @param data PPP-framed data
     * @param protocol Output: PPP protocol byte (e.g., 0x21 for IPv4)
     * @return Extracted payload (IP packet for protocol 0x21)
     * @throws std::runtime_error if frame is invalid
     */
    static mtp::ByteArray ExtractPayload(const mtp::ByteArray& data, uint16_t* protocol = nullptr);

    /**
     * Wrap payload in PPP frame
     * @param payload Data to wrap (typically an IP packet)
     * @param protocol PPP protocol byte (default 0x21 for IPv4)
     * @return PPP-framed data ready for USB transmission
     */
    static mtp::ByteArray WrapPayload(const mtp::ByteArray& payload, uint16_t protocol = 0x0021);

    /**
     * Get protocol name for debugging
     * @param protocol PPP protocol value
     * @return Human-readable protocol name
     */
    static std::string GetProtocolName(uint16_t protocol);

    /**
     * Extract individual PPP frames from a concatenated buffer
     * USB responses often contain multiple PPP frames packed together.
     * This function splits them into individual frames.
     *
     * @param data Buffer containing one or more PPP frames
     * @return Vector of individual PPP frames (each with 0x7E delimiters)
     */
    static std::vector<mtp::ByteArray> ExtractFrames(const mtp::ByteArray& data);

    /**
     * Extract PPP frames with support for incomplete frames spanning USB packets
     *
     * USB packets can contain multiple PPP frames, and a single PPP frame can
     * span multiple USB packets. This function handles both cases by:
     * 1. Prepending any incomplete frame data from the previous call
     * 2. Extracting all complete frames
     * 3. Storing any incomplete frame data for the next call
     *
     * @param data New USB packet data
     * @param incomplete_buffer In/out buffer for incomplete frame data
     * @return Vector of complete PPP frames extracted
     */
    static std::vector<mtp::ByteArray> ExtractFramesWithBuffer(
        const mtp::ByteArray& data,
        mtp::ByteArray& incomplete_buffer);

    /**
     * Parsed PPP frame with protocol information
     */
    struct ParsedFrame {
        uint16_t protocol;          // PPP protocol (0x0021 = IPv4, 0x8021 = IPCP, etc.)
        mtp::ByteArray payload;     // Payload after protocol field (without FCS)
        mtp::ByteArray raw_frame;   // Original frame with delimiters
    };

    /**
     * Try to extract payload from PPP frame (non-throwing version)
     * @param data PPP-framed data
     * @param frame Output: parsed frame if successful
     * @return true if frame was valid and parsed
     */
    static bool TryParseFrame(const mtp::ByteArray& data, ParsedFrame& frame);

private:
    // PPP frame delimiters and protocols
    static constexpr uint8_t PPP_FLAG = 0x7E;
    static constexpr uint16_t PPP_PROTO_IPV4 = 0x0021;
    static constexpr uint16_t PPP_PROTO_IPV6 = 0x0057;
    static constexpr uint16_t PPP_PROTO_LCP = 0xC021;
    static constexpr uint16_t PPP_PROTO_IPCP = 0x8021;
};

/**
 * IPParser
 *
 * Parses IPv4 packets extracted from PPP frames.
 */
class IPParser {
public:
    struct IPHeader {
        uint8_t version;          // Should be 4 for IPv4
        uint8_t header_length;    // In 32-bit words (typically 5 = 20 bytes)
        uint8_t dscp;             // Differentiated Services Code Point
        uint8_t ecn;              // Explicit Congestion Notification
        uint16_t total_length;    // Total packet length in bytes
        uint16_t identification;  // Fragment identification
        uint16_t flags_offset;    // Flags (3 bits) + Fragment offset (13 bits)
        uint8_t ttl;              // Time to live
        uint8_t protocol;         // 6 = TCP, 17 = UDP
        uint16_t checksum;        // Header checksum
        uint32_t src_ip;          // Source IP address
        uint32_t dst_ip;          // Destination IP address
    };

    /**
     * Parse IP header from packet
     * @param data IP packet data
     * @return Parsed IP header
     * @throws std::runtime_error if packet is invalid
     */
    static IPHeader ParseHeader(const mtp::ByteArray& data);

    /**
     * Extract payload from IP packet
     * @param data IP packet data
     * @return TCP/UDP/other payload
     */
    static mtp::ByteArray ExtractPayload(const mtp::ByteArray& data);

    /**
     * Build IP packet with payload
     * @param header IP header parameters
     * @param payload TCP/UDP/other payload
     * @return Complete IP packet with calculated checksum
     */
    static mtp::ByteArray BuildPacket(const IPHeader& header, const mtp::ByteArray& payload);

    /**
     * Calculate IP header checksum
     * @param header_data IP header bytes (without checksum)
     * @return Calculated checksum
     */
    static uint16_t CalculateChecksum(const uint8_t* header_data, size_t length);

    /**
     * Convert IP address to string (e.g., "192.168.55.101")
     * @param ip_addr IP address in network byte order
     * @return String representation
     */
    static std::string IPToString(uint32_t ip_addr);

    /**
     * Convert string to IP address
     * @param ip_str String representation (e.g., "192.168.55.101")
     * @return IP address in network byte order
     */
    static uint32_t StringToIP(const std::string& ip_str);
};

/**
 * TCPParser
 *
 * Parses TCP segments extracted from IP packets.
 */
class TCPParser {
public:
    struct TCPHeader {
        uint16_t src_port;        // Source port
        uint16_t dst_port;        // Destination port
        uint32_t seq_num;         // Sequence number
        uint32_t ack_num;         // Acknowledgment number
        uint8_t data_offset;      // Header length in 32-bit words (typically 5 = 20 bytes)
        uint8_t flags;            // Control flags (SYN, ACK, FIN, etc.)
        uint16_t window_size;     // Window size for flow control
        uint16_t checksum;        // Checksum
        uint16_t urgent_pointer;  // Urgent pointer
    };

    // TCP flags
    static constexpr uint8_t TCP_FLAG_FIN = 0x01;
    static constexpr uint8_t TCP_FLAG_SYN = 0x02;
    static constexpr uint8_t TCP_FLAG_RST = 0x04;
    static constexpr uint8_t TCP_FLAG_PSH = 0x08;
    static constexpr uint8_t TCP_FLAG_ACK = 0x10;
    static constexpr uint8_t TCP_FLAG_URG = 0x20;

    /**
     * Parse TCP header from segment
     * @param data TCP segment data
     * @return Parsed TCP header
     * @throws std::runtime_error if segment is invalid
     */
    static TCPHeader ParseHeader(const mtp::ByteArray& data);

    /**
     * Extract payload from TCP segment
     * @param data TCP segment data
     * @return HTTP/other application data
     */
    static mtp::ByteArray ExtractPayload(const mtp::ByteArray& data);

    /**
     * Build TCP segment with payload
     * @param header TCP header parameters
     * @param payload Application data (e.g., HTTP)
     * @param src_ip Source IP for pseudo-header checksum
     * @param dst_ip Destination IP for pseudo-header checksum
     * @return Complete TCP segment with calculated checksum
     */
    static mtp::ByteArray BuildSegment(const TCPHeader& header,
                                       const mtp::ByteArray& payload,
                                       uint32_t src_ip,
                                       uint32_t dst_ip);

    /**
     * Calculate TCP checksum (includes pseudo-header)
     * @param tcp_data Complete TCP segment
     * @param src_ip Source IP address
     * @param dst_ip Destination IP address
     * @return Calculated checksum
     */
    static uint16_t CalculateChecksum(const uint8_t* tcp_data, size_t length,
                                      uint32_t src_ip, uint32_t dst_ip);

    /**
     * Get string representation of TCP flags
     * @param flags TCP flag byte
     * @return String like "SYN", "ACK", "SYN,ACK", etc.
     */
    static std::string FlagsToString(uint8_t flags);
};

/**
 * IPCPParser
 *
 * Handles IPCP (IP Control Protocol) negotiation for PPP.
 * IPCP is used to configure IP addresses and DNS servers over PPP.
 */
class IPCPParser {
public:
    // IPCP Codes
    static constexpr uint8_t IPCP_CODE_CONFIG_REQUEST = 1;
    static constexpr uint8_t IPCP_CODE_CONFIG_ACK = 2;
    static constexpr uint8_t IPCP_CODE_CONFIG_NAK = 3;
    static constexpr uint8_t IPCP_CODE_CONFIG_REJECT = 4;

    // IPCP Option Types
    static constexpr uint8_t IPCP_OPT_IP_COMPRESSION = 2;
    static constexpr uint8_t IPCP_OPT_IP_ADDRESS = 3;
    static constexpr uint8_t IPCP_OPT_PRIMARY_DNS = 129;    // 0x81
    static constexpr uint8_t IPCP_OPT_SECONDARY_DNS = 131;  // 0x83

    struct IPCPPacket {
        uint8_t code;           // Config-Request, Config-Ack, Config-Nak, Config-Reject
        uint8_t identifier;     // Matches request with response
        uint16_t length;        // Total packet length
        mtp::ByteArray options; // Variable-length options
    };

    struct IPCPOption {
        uint8_t type;
        uint8_t length;
        mtp::ByteArray data;
    };

    /**
     * Parse IPCP packet
     * @param data IPCP packet data (without PPP framing)
     * @return Parsed IPCP packet
     */
    static IPCPPacket ParsePacket(const mtp::ByteArray& data);

    /**
     * Build IPCP packet
     * @param packet IPCP packet to build
     * @return IPCP packet bytes (ready to wrap in PPP frame)
     */
    static mtp::ByteArray BuildPacket(const IPCPPacket& packet);

    /**
     * Parse options from IPCP packet
     * @param options_data Raw options bytes
     * @return Vector of parsed options
     */
    static std::vector<IPCPOption> ParseOptions(const mtp::ByteArray& options_data);

    /**
     * Build Config-Request packet
     * @param request_id Identifier for this Config-Request
     * @param host_ip IP address we're requesting for ourselves
     * @param primary_dns Primary DNS server IP we're requesting
     * @return IPCP Config-Request packet
     */
    static mtp::ByteArray BuildConfigRequest(uint8_t request_id, uint32_t host_ip, uint32_t primary_dns);

    /**
     * Build Config-Nak response
     * @param request_id Identifier from Config-Request
     * @param device_ip IP address to assign to device
     * @param primary_dns Primary DNS server IP
     * @return IPCP Config-Nak packet
     */
    static mtp::ByteArray BuildConfigNak(uint8_t request_id, uint32_t device_ip, uint32_t primary_dns);

    /**
     * Build Config-Ack response
     * @param request Original Config-Request packet
     * @return IPCP Config-Ack packet
     */
    static mtp::ByteArray BuildConfigAck(const IPCPPacket& request);
};

// PPPParser IPCP helper functions (declared after IPCPParser is defined)
namespace PPPParserHelpers {
    /**
     * Find IPCP frame with specific code in a buffer of concatenated frames
     * @param data Buffer containing PPP frames
     * @param ipcp_code IPCP code to find (1=Config-Request, 2=Ack, 3=Nak, 4=Reject)
     * @param packet Output: parsed IPCP packet if found
     * @return true if found
     */
    bool FindIPCPFrame(const mtp::ByteArray& data, uint8_t ipcp_code,
                       IPCPParser::IPCPPacket& packet);

    /**
     * Check if buffer contains IPCP frame with specific code
     * @param data Buffer containing PPP frames
     * @param ipcp_code IPCP code to look for
     * @return true if found
     */
    bool ContainsIPCPCode(const mtp::ByteArray& data, uint8_t ipcp_code);
}

// Convenience aliases
inline bool PPPParser_FindIPCPFrame(const mtp::ByteArray& data, uint8_t ipcp_code,
                                    IPCPParser::IPCPPacket& packet) {
    return PPPParserHelpers::FindIPCPFrame(data, ipcp_code, packet);
}

inline bool PPPParser_ContainsIPCPCode(const mtp::ByteArray& data, uint8_t ipcp_code) {
    return PPPParserHelpers::ContainsIPCPCode(data, ipcp_code);
}

/**
 * DNSServer
 *
 * Minimal DNS server for resolving Zune service hostnames.
 * Responds to DNS queries with configured IP addresses.
 */
class DNSServer {
public:
    /**
     * Build DNS response for a query
     * @param query DNS query packet (from UDP payload)
     * @param hostname_map Map of hostnames to IP addresses
     * @return DNS response packet (to send as UDP payload)
     */
    static mtp::ByteArray BuildResponse(const mtp::ByteArray& query,
                                       const std::map<std::string, uint32_t>& hostname_map);

    /**
     * Parse hostname from DNS query
     * @param query DNS query packet
     * @return Queried hostname (e.g., "catalog.zune.net")
     */
    static std::string ParseHostname(const mtp::ByteArray& query);
};
