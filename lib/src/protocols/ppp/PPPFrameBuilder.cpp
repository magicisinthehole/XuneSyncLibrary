#include "PPPFrameBuilder.h"
#include "PPPParser.h"
#include "../http/HTTPParser.h"  // For TCPParser and IPParser
#include <cstdlib>

mtp::ByteArray PPPFrameBuilder::BuildTCPFrame(
    uint32_t src_ip, uint16_t src_port,
    uint32_t dst_ip, uint16_t dst_port,
    uint32_t seq_num, uint32_t ack_num,
    uint8_t flags,
    const mtp::ByteArray& payload
) {
    // Build TCP header
    TCPParser::TCPHeader tcp_header = {};
    tcp_header.src_port = src_port;
    tcp_header.dst_port = dst_port;
    tcp_header.seq_num = seq_num;
    tcp_header.ack_num = ack_num;
    tcp_header.data_offset = 5;  // 20 bytes (no options)
    tcp_header.flags = flags;
    tcp_header.window_size = 65535;
    tcp_header.checksum = 0;  // Will be calculated by BuildSegment
    tcp_header.urgent_pointer = 0;

    mtp::ByteArray tcp_segment = TCPParser::BuildSegment(
        tcp_header, payload, src_ip, dst_ip);

    // Build IP header
    IPParser::IPHeader ip_header = {};
    ip_header.version = 4;
    ip_header.header_length = 5;  // 20 bytes
    ip_header.dscp = 0;
    ip_header.ecn = 0;
    ip_header.total_length = 0;  // Will be calculated by BuildPacket
    ip_header.identification = rand() % 65536;
    ip_header.flags_offset = 0;  // Don't fragment
    ip_header.ttl = 64;
    ip_header.protocol = 6;  // TCP
    ip_header.checksum = 0;  // Will be calculated by BuildPacket
    ip_header.src_ip = src_ip;
    ip_header.dst_ip = dst_ip;

    mtp::ByteArray ip_packet = IPParser::BuildPacket(ip_header, tcp_segment);

    // Wrap in PPP frame
    return PPPParser::WrapPayload(ip_packet, 0x0021);
}

mtp::ByteArray PPPFrameBuilder::BuildUDPFrame(
    uint32_t src_ip, uint16_t src_port,
    uint32_t dst_ip, uint16_t dst_port,
    const mtp::ByteArray& payload
) {
    // Build UDP segment (8 byte header + payload)
    mtp::ByteArray udp_segment;
    udp_segment.reserve(8 + payload.size());

    // Source port (2 bytes)
    udp_segment.push_back((src_port >> 8) & 0xFF);
    udp_segment.push_back(src_port & 0xFF);

    // Destination port (2 bytes)
    udp_segment.push_back((dst_port >> 8) & 0xFF);
    udp_segment.push_back(dst_port & 0xFF);

    // Length (2 bytes) - header + payload
    uint16_t udp_length = 8 + payload.size();
    udp_segment.push_back((udp_length >> 8) & 0xFF);
    udp_segment.push_back(udp_length & 0xFF);

    // Checksum (2 bytes) - 0 for now (optional in IPv4)
    udp_segment.push_back(0x00);
    udp_segment.push_back(0x00);

    // Payload
    udp_segment.insert(udp_segment.end(), payload.begin(), payload.end());

    // Build IP header
    IPParser::IPHeader ip_header = {};
    ip_header.version = 4;
    ip_header.header_length = 5;
    ip_header.dscp = 0;
    ip_header.ecn = 0;
    ip_header.total_length = 0;  // Will be calculated
    ip_header.identification = rand() % 65536;
    ip_header.flags_offset = 0;
    ip_header.ttl = 64;
    ip_header.protocol = 17;  // UDP
    ip_header.checksum = 0;
    ip_header.src_ip = src_ip;
    ip_header.dst_ip = dst_ip;

    mtp::ByteArray ip_packet = IPParser::BuildPacket(ip_header, udp_segment);

    // Wrap in PPP frame
    return PPPParser::WrapPayload(ip_packet, 0x0021);
}

mtp::ByteArray PPPFrameBuilder::BuildIPFrame(
    const mtp::ByteArray& ip_packet,
    uint16_t ppp_protocol
) {
    return PPPParser::WrapPayload(ip_packet, ppp_protocol);
}
