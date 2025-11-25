#include "DNSHandler.h"
#include "../ppp/PPPParser.h"  // Contains IPParser, DNSServer definitions
#include <random>

DNSHandler::DNSHandler(const std::map<std::string, uint32_t>& hostname_map)
    : hostname_map_(hostname_map) {
}

std::optional<mtp::ByteArray> DNSHandler::HandleQuery(const mtp::ByteArray& ip_packet) {
    try {
        // Parse IP header
        IPParser::IPHeader ip_header = IPParser::ParseHeader(ip_packet);

        // Verify this is a UDP packet
        if (ip_header.protocol != 17) {
            return std::nullopt;
        }

        // Extract UDP segment
        mtp::ByteArray udp_segment = IPParser::ExtractPayload(ip_packet);

        if (udp_segment.size() < 8) {
            Log("UDP segment too small for DNS");
            return std::nullopt;
        }

        // Parse UDP header
        uint16_t src_port = (udp_segment[0] << 8) | udp_segment[1];
        uint16_t dst_port = (udp_segment[2] << 8) | udp_segment[3];

        // Verify this is a DNS query (destination port 53)
        if (dst_port != 53) {
            return std::nullopt;
        }

        // Extract DNS query (UDP payload)
        mtp::ByteArray dns_query(udp_segment.begin() + 8, udp_segment.end());

        // Parse hostname from query
        std::string hostname = DNSServer::ParseHostname(dns_query);
        Log("DNS query for: " + hostname);

        // Build DNS response
        mtp::ByteArray dns_response = DNSServer::BuildResponse(dns_query, hostname_map_);

        if (dns_response.empty()) {
            Log("No DNS mapping found for " + hostname);
            return std::nullopt;
        }

        // Build UDP response (swap source and destination)
        mtp::ByteArray udp_response;
        udp_response.push_back((dst_port >> 8) & 0xFF);  // Src port (swap)
        udp_response.push_back(dst_port & 0xFF);
        udp_response.push_back((src_port >> 8) & 0xFF);  // Dst port (swap)
        udp_response.push_back(src_port & 0xFF);

        // UDP length
        uint16_t response_length = 8 + dns_response.size();
        udp_response.push_back((response_length >> 8) & 0xFF);
        udp_response.push_back(response_length & 0xFF);

        // UDP checksum (0 = no checksum)
        udp_response.push_back(0x00);
        udp_response.push_back(0x00);

        // Append DNS response
        udp_response.insert(udp_response.end(), dns_response.begin(), dns_response.end());

        // Build IP response packet (swap source and destination)
        IPParser::IPHeader response_header = {};
        response_header.version = 4;
        response_header.header_length = 5;
        response_header.dscp = 0;
        response_header.ecn = 0;
        response_header.total_length = 0;  // Will be calculated by BuildPacket

        // Generate IP identification using proper random number generation
        static std::random_device rd;
        static std::mt19937 gen(rd());
        static std::uniform_int_distribution<uint16_t> dist(0, 65535);
        response_header.identification = dist(gen);

        response_header.flags_offset = 0;
        response_header.ttl = 64;
        response_header.protocol = 17;  // UDP
        response_header.checksum = 0;  // Will be calculated by BuildPacket
        response_header.src_ip = ip_header.dst_ip;  // Swap (our DNS server IP)
        response_header.dst_ip = ip_header.src_ip;  // Swap (device IP)

        mtp::ByteArray ip_response = IPParser::BuildPacket(response_header, udp_response);

        // Wrap in PPP frame
        mtp::ByteArray ppp_frame = PPPParser::WrapPayload(ip_response, 0x0021);

        Log("DNS response built for " + hostname + " (PPP frame size: " +
            std::to_string(ppp_frame.size()) + " bytes)");

        return ppp_frame;

    } catch (const std::exception& e) {
        Log("Error handling DNS query: " + std::string(e.what()));
        return std::nullopt;
    }
}

void DNSHandler::SetLogCallback(LogCallback callback) {
    log_callback_ = callback;
}

void DNSHandler::UpdateHostnameMap(const std::map<std::string, uint32_t>& hostname_map) {
    hostname_map_ = hostname_map;
}

void DNSHandler::Log(const std::string& message) {
    if (log_callback_) {
        log_callback_(message);
    }
}

bool DNSHandler::IsZuneTCPDNSQuery(const mtp::ByteArray& buffer) const {
    // Zune TCP DNS uses 8-byte framing: [ID1][0x0035][LEN][0x0000][DNS message]
    // Need at least 8 bytes for framing + 12 bytes for minimal DNS header
    if (buffer.size() < 20) {
        return false;
    }

    uint16_t word1 = (buffer[2] << 8) | buffer[3];  // Should be 0x0035
    uint16_t reserved = (buffer[6] << 8) | buffer[7];  // Should be 0x0000

    return (word1 == 0x0035 && reserved == 0x0000);
}

std::optional<mtp::ByteArray> DNSHandler::HandleTCPQuery(const mtp::ByteArray& buffer, size_t& bytes_consumed) {
    bytes_consumed = 0;

    if (!IsZuneTCPDNSQuery(buffer)) {
        return std::nullopt;
    }

    // Parse 8-byte Zune framing
    uint16_t word0 = (buffer[0] << 8) | buffer[1];  // ID1 (echo in response)
    uint16_t length_field = (buffer[4] << 8) | buffer[5];

    // Length field IS the total TCP payload length (including 8-byte prefix)
    if (buffer.size() < length_field) {
        // Incomplete - need more data
        return std::nullopt;
    }

    Log("Zune TCP DNS query detected (length=" + std::to_string(length_field) + ")");

    // Extract DNS message (starts at byte 8)
    mtp::ByteArray dns_query(buffer.begin() + 8, buffer.begin() + length_field);

    // Use existing DNSServer to build response
    mtp::ByteArray dns_response = DNSServer::BuildResponse(dns_query, hostname_map_);

    if (dns_response.empty()) {
        std::string hostname = DNSServer::ParseHostname(dns_query);
        Log("No DNS mapping for: " + hostname);
        bytes_consumed = length_field;  // Consume the query even if no response
        return std::nullopt;
    }

    // Build TCP payload with Zune 8-byte framing (swapped pattern for response)
    // Query:    [ID1][0x0035][LEN][0x0000]
    // Response: [0x0035][ID1][LEN][0x0000]
    mtp::ByteArray tcp_payload;

    tcp_payload.push_back(0x00);  // 0x0035 high byte
    tcp_payload.push_back(0x35);  // 0x0035 low byte
    tcp_payload.push_back((word0 >> 8) & 0xFF);  // Echo query's ID1
    tcp_payload.push_back(word0 & 0xFF);

    // Length field = TOTAL TCP payload length (8-byte prefix + DNS message)
    uint16_t response_length = dns_response.size() + 8;
    tcp_payload.push_back((response_length >> 8) & 0xFF);
    tcp_payload.push_back(response_length & 0xFF);

    // Reserved (0x0000)
    tcp_payload.push_back(0x00);
    tcp_payload.push_back(0x00);

    // Append DNS response
    tcp_payload.insert(tcp_payload.end(), dns_response.begin(), dns_response.end());

    std::string hostname = DNSServer::ParseHostname(dns_query);
    Log("Zune TCP DNS response built for " + hostname + " (" + std::to_string(tcp_payload.size()) + " bytes)");

    bytes_consumed = length_field;
    return tcp_payload;
}
