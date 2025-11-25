#pragma once

#include <mtp/ByteArray.h>
#include <cstdint>

/**
 * PPPFrameBuilder
 *
 * Single source of truth for building TCP/IP/PPP frames.
 * Consolidates all frame construction that was previously duplicated
 * across SendTCPResponse, SendTCPPacket, SendTCPResponseWithData, and BuildPPPFrame.
 */
class PPPFrameBuilder {
public:
    /**
     * Build a complete PPP frame containing a TCP segment
     *
     * @param src_ip Source IP address
     * @param src_port Source TCP port
     * @param dst_ip Destination IP address
     * @param dst_port Destination TCP port
     * @param seq_num TCP sequence number
     * @param ack_num TCP acknowledgment number
     * @param flags TCP flags (SYN, ACK, PSH, FIN, RST)
     * @param payload TCP payload data (can be empty for control packets)
     * @return Complete PPP frame ready for transmission
     */
    static mtp::ByteArray BuildTCPFrame(
        uint32_t src_ip, uint16_t src_port,
        uint32_t dst_ip, uint16_t dst_port,
        uint32_t seq_num, uint32_t ack_num,
        uint8_t flags,
        const mtp::ByteArray& payload = mtp::ByteArray()
    );

    /**
     * Build a complete PPP frame containing a UDP segment
     *
     * @param src_ip Source IP address
     * @param src_port Source UDP port
     * @param dst_ip Destination IP address
     * @param dst_port Destination UDP port
     * @param payload UDP payload data
     * @return Complete PPP frame ready for transmission
     */
    static mtp::ByteArray BuildUDPFrame(
        uint32_t src_ip, uint16_t src_port,
        uint32_t dst_ip, uint16_t dst_port,
        const mtp::ByteArray& payload
    );

    /**
     * Build a PPP frame for a raw IP packet
     *
     * @param ip_packet Complete IP packet (header + payload)
     * @param ppp_protocol PPP protocol number (0x0021 for IPv4)
     * @return Complete PPP frame ready for transmission
     */
    static mtp::ByteArray BuildIPFrame(
        const mtp::ByteArray& ip_packet,
        uint16_t ppp_protocol = 0x0021
    );
};
