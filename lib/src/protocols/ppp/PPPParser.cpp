#include "PPPParser.h"
#include <stdexcept>
#include <sstream>
#include <iomanip>
#include <cstring>
#include <arpa/inet.h>

// Forward declaration
static uint16_t CalculatePPPFCS(const uint8_t* data, size_t length);

// ============================================================================
// PPPParser Implementation
// ============================================================================

bool PPPParser::IsValidFrame(const mtp::ByteArray& data) {
    if (data.size() < 4) return false;  // Minimum: 0x7e + protocol + data + 0x7e
    return data[0] == PPP_FLAG && data[data.size() - 1] == PPP_FLAG;
}

mtp::ByteArray PPPParser::ExtractPayload(const mtp::ByteArray& data, uint16_t* protocol) {
    if (!IsValidFrame(data)) {
        throw std::runtime_error("Invalid PPP frame: missing 0x7e delimiters");
    }

    // PPP frame: 0x7E [escaped data: protocol + payload + FCS] 0x7E
    // IMPORTANT: Escape sequences apply to the ENTIRE frame content, including protocol field
    // Must un-stuff BEFORE reading protocol

    // Step 1: Extract raw frame content (between 0x7E delimiters)
    mtp::ByteArray raw_frame;
    raw_frame.insert(raw_frame.end(), data.begin() + 1, data.end() - 1);

    // Step 2: Un-stuff PPP escape sequences
    // PPP uses 0x7D as escape: 0x7D 0xXX → XX ^ 0x20
    // Common escapes: 0x7D 0x5D → 0x7D, 0x7D 0x5E → 0x7E, 0x7D 0x23 → 0xC0 (for LCP)
    mtp::ByteArray unstuffed_frame;
    for (size_t i = 0; i < raw_frame.size(); i++) {
        if (raw_frame[i] == 0x7D && i + 1 < raw_frame.size()) {
            // Un-stuff: next byte XOR 0x20
            unstuffed_frame.push_back(raw_frame[i + 1] ^ 0x20);
            i++;  // Skip the escaped byte
        } else {
            unstuffed_frame.push_back(raw_frame[i]);
        }
    }

    if (unstuffed_frame.size() < 3) {
        throw std::runtime_error("PPP frame too short after un-stuffing");
    }

    // Step 3: Read protocol from un-stuffed data (1 or 2 bytes)
    // PPP protocol compression: if first byte has LSB of first octet set to 1,
    // it's a single byte protocol. Otherwise it's 2 bytes.
    // Control protocols (IPCP=0x8021, LCP=0xC021) are always 2 bytes.
    size_t offset = 0;
    uint16_t proto = 0;
    uint8_t first_byte = unstuffed_frame[offset];

    // Check if this is a 2-byte protocol
    // LSB of first byte being 0 means it's 2 bytes
    // OR if first byte is 0x80, 0xC0 etc (control protocols), read 2 bytes
    if (offset + 1 < unstuffed_frame.size() &&
        ((first_byte & 0x01) == 0 || first_byte == 0x80 || first_byte == 0xC0)) {
        uint8_t second_byte = unstuffed_frame[offset + 1];
        proto = (static_cast<uint16_t>(first_byte) << 8) | second_byte;
        offset += 2;
    } else {
        // Single byte protocol (compressed)
        proto = first_byte;
        offset += 1;
    }

    if (protocol) {
        *protocol = proto;
    }

    // Step 4: Verify FCS (Frame Check Sequence)
    // Extract FCS from frame (last 2 bytes, little-endian)
    uint16_t received_fcs = static_cast<uint16_t>(unstuffed_frame[unstuffed_frame.size() - 2]) |
                           (static_cast<uint16_t>(unstuffed_frame[unstuffed_frame.size() - 1]) << 8);

    // Calculate FCS over frame data (everything except the FCS itself)
    uint16_t calculated_fcs = CalculatePPPFCS(unstuffed_frame.data(),
                                              unstuffed_frame.size() - 2);

    // Verify FCS matches
    if (received_fcs != calculated_fcs) {
        std::ostringstream err;
        err << "PPP FCS mismatch: received=0x" << std::hex << received_fcs
            << " calculated=0x" << calculated_fcs;
        throw std::runtime_error(err.str());
    }

    // Step 5: Extract payload (everything between protocol and FCS)
    // Un-stuffed frame: [protocol 1-2 bytes] [payload] [FCS 2 bytes]
    if (offset >= unstuffed_frame.size() - 2) {
        return mtp::ByteArray();  // Empty payload (only protocol + FCS)
    }

    // Extract payload (exclude FCS which is last 2 bytes)
    mtp::ByteArray payload;
    payload.insert(payload.end(),
                   unstuffed_frame.begin() + offset,
                   unstuffed_frame.end() - 2);

    return payload;
}

// Calculate PPP FCS (Frame Check Sequence) using CRC-16-CCITT
static uint16_t CalculatePPPFCS(const uint8_t* data, size_t length) {
    uint16_t fcs = 0xFFFF;  // Initial FCS value
    static const uint16_t fcstab[256] = {
        0x0000, 0x1189, 0x2312, 0x329b, 0x4624, 0x57ad, 0x6536, 0x74bf,
        0x8c48, 0x9dc1, 0xaf5a, 0xbed3, 0xca6c, 0xdbe5, 0xe97e, 0xf8f7,
        0x1081, 0x0108, 0x3393, 0x221a, 0x56a5, 0x472c, 0x75b7, 0x643e,
        0x9cc9, 0x8d40, 0xbfdb, 0xae52, 0xdaed, 0xcb64, 0xf9ff, 0xe876,
        0x2102, 0x308b, 0x0210, 0x1399, 0x6726, 0x76af, 0x4434, 0x55bd,
        0xad4a, 0xbcc3, 0x8e58, 0x9fd1, 0xeb6e, 0xfae7, 0xc87c, 0xd9f5,
        0x3183, 0x200a, 0x1291, 0x0318, 0x77a7, 0x662e, 0x54b5, 0x453c,
        0xbdcb, 0xac42, 0x9ed9, 0x8f50, 0xfbef, 0xea66, 0xd8fd, 0xc974,
        0x4204, 0x538d, 0x6116, 0x709f, 0x0420, 0x15a9, 0x2732, 0x36bb,
        0xce4c, 0xdfc5, 0xed5e, 0xfcd7, 0x8868, 0x99e1, 0xab7a, 0xbaf3,
        0x5285, 0x430c, 0x7197, 0x601e, 0x14a1, 0x0528, 0x37b3, 0x263a,
        0xdecd, 0xcf44, 0xfddf, 0xec56, 0x98e9, 0x8960, 0xbbfb, 0xaa72,
        0x6306, 0x728f, 0x4014, 0x519d, 0x2522, 0x34ab, 0x0630, 0x17b9,
        0xef4e, 0xfec7, 0xcc5c, 0xddd5, 0xa96a, 0xb8e3, 0x8a78, 0x9bf1,
        0x7387, 0x620e, 0x5095, 0x411c, 0x35a3, 0x242a, 0x16b1, 0x0738,
        0xffcf, 0xee46, 0xdcdd, 0xcd54, 0xb9eb, 0xa862, 0x9af9, 0x8b70,
        0x8408, 0x9581, 0xa71a, 0xb693, 0xc22c, 0xd3a5, 0xe13e, 0xf0b7,
        0x0840, 0x19c9, 0x2b52, 0x3adb, 0x4e64, 0x5fed, 0x6d76, 0x7cff,
        0x9489, 0x8500, 0xb79b, 0xa612, 0xd2ad, 0xc324, 0xf1bf, 0xe036,
        0x18c1, 0x0948, 0x3bd3, 0x2a5a, 0x5ee5, 0x4f6c, 0x7df7, 0x6c7e,
        0xa50a, 0xb483, 0x8618, 0x9791, 0xe32e, 0xf2a7, 0xc03c, 0xd1b5,
        0x2942, 0x38cb, 0x0a50, 0x1bd9, 0x6f66, 0x7eef, 0x4c74, 0x5dfd,
        0xb58b, 0xa402, 0x9699, 0x8710, 0xf3af, 0xe226, 0xd0bd, 0xc134,
        0x39c3, 0x284a, 0x1ad1, 0x0b58, 0x7fe7, 0x6e6e, 0x5cf5, 0x4d7c,
        0xc60c, 0xd785, 0xe51e, 0xf497, 0x8028, 0x91a1, 0xa33a, 0xb2b3,
        0x4a44, 0x5bcd, 0x6956, 0x78df, 0x0c60, 0x1de9, 0x2f72, 0x3efb,
        0xd68d, 0xc704, 0xf59f, 0xe416, 0x90a9, 0x8120, 0xb3bb, 0xa232,
        0x5ac5, 0x4b4c, 0x79d7, 0x685e, 0x1ce1, 0x0d68, 0x3ff3, 0x2e7a,
        0xe70e, 0xf687, 0xc41c, 0xd595, 0xa12a, 0xb0a3, 0x8238, 0x93b1,
        0x6b46, 0x7acf, 0x4854, 0x59dd, 0x2d62, 0x3ceb, 0x0e70, 0x1ff9,
        0xf78f, 0xe606, 0xd49d, 0xc514, 0xb1ab, 0xa022, 0x92b9, 0x8330,
        0x7bc7, 0x6a4e, 0x58d5, 0x495c, 0x3de3, 0x2c6a, 0x1ef1, 0x0f78
    };

    for (size_t i = 0; i < length; i++) {
        fcs = (fcs >> 8) ^ fcstab[(fcs ^ data[i]) & 0xFF];
    }

    return fcs ^ 0xFFFF;  // Final XOR
}

mtp::ByteArray PPPParser::WrapPayload(const mtp::ByteArray& payload, uint16_t protocol) {
    mtp::ByteArray frame_data;

    // Build frame data (protocol + payload) for FCS calculation (UNSTUFFED)
    if (protocol == PPP_PROTO_IPV4) {
        frame_data.push_back(0x21);
    } else if ((protocol & 0xFF00) == 0) {
        // Single-byte protocol
        frame_data.push_back(protocol & 0xFF);
    } else {
        // Two-byte protocol
        frame_data.push_back((protocol >> 8) & 0xFF);
        frame_data.push_back(protocol & 0xFF);
    }

    // Add payload (unstuffed)
    frame_data.insert(frame_data.end(), payload.begin(), payload.end());

    // Calculate FCS over unstuffed protocol + payload
    uint16_t fcs = CalculatePPPFCS(frame_data.data(), frame_data.size());

    // Add FCS to frame_data (little-endian) BEFORE byte stuffing
    // RFC 1662: The FCS is transmitted with the LSB first
    frame_data.push_back(fcs & 0xFF);        // FCS low byte
    frame_data.push_back((fcs >> 8) & 0xFF); // FCS high byte

    // Now stuff the ENTIRE frame_data (including FCS) for transmission
    // PPP byte stuffing: escape 0x7D and 0x7E as 0x7D 0xXX (XX = original ^ 0x20)
    // This is CRITICAL: FCS bytes containing 0x7D or 0x7E would corrupt the frame!
    mtp::ByteArray stuffed_frame_data;
    for (uint8_t byte : frame_data) {
        if (byte == 0x7D || byte == 0x7E) {
            stuffed_frame_data.push_back(0x7D);
            stuffed_frame_data.push_back(byte ^ 0x20);
        } else {
            stuffed_frame_data.push_back(byte);
        }
    }

    // Build final frame: 0x7E + stuffed(protocol + payload + FCS) + 0x7E
    mtp::ByteArray frame;
    frame.push_back(PPP_FLAG);
    frame.insert(frame.end(), stuffed_frame_data.begin(), stuffed_frame_data.end());
    frame.push_back(PPP_FLAG);

    return frame;
}

std::string PPPParser::GetProtocolName(uint16_t protocol) {
    switch (protocol) {
        case PPP_PROTO_IPV4: return "IPv4";
        case PPP_PROTO_IPV6: return "IPv6";
        case PPP_PROTO_LCP: return "LCP";
        case PPP_PROTO_IPCP: return "IPCP";
        default: return "Unknown";
    }
}

std::vector<mtp::ByteArray> PPPParser::ExtractFrames(const mtp::ByteArray& data) {
    std::vector<mtp::ByteArray> frames;

    if (data.empty()) {
        return frames;
    }

    size_t i = 0;
    while (i < data.size()) {
        // Find start of frame (0x7E)
        if (data[i] != PPP_FLAG) {
            i++;
            continue;
        }

        // Found start, now find end
        size_t start = i;
        i++;  // Skip start flag

        // Find end of frame (next 0x7E)
        while (i < data.size() && data[i] != PPP_FLAG) {
            i++;
        }

        if (i < data.size()) {
            // Include the end flag
            mtp::ByteArray frame(data.begin() + start, data.begin() + i + 1);
            if (frame.size() > 2) {  // More than just two flags
                frames.push_back(frame);
            }
            i++;  // Move past end flag (which might be start of next frame)
        }
    }

    return frames;
}

std::vector<mtp::ByteArray> PPPParser::ExtractFramesWithBuffer(
    const mtp::ByteArray& data,
    mtp::ByteArray& incomplete_buffer) {

    std::vector<mtp::ByteArray> frames;

    // Step 1: Combine incomplete buffer with new data
    mtp::ByteArray combined_data;
    if (!incomplete_buffer.empty()) {
        combined_data.insert(combined_data.end(),
                           incomplete_buffer.begin(),
                           incomplete_buffer.end());
        combined_data.insert(combined_data.end(), data.begin(), data.end());
        incomplete_buffer.clear();
    } else {
        combined_data = data;
    }

    if (combined_data.empty()) {
        return frames;
    }

    // Step 2: Extract complete frames, buffer incomplete ones
    size_t i = 0;
    while (i < combined_data.size()) {
        // Find frame start (0x7E)
        if (combined_data[i] != PPP_FLAG) {
            i++;
            continue;
        }

        size_t frame_start = i;
        i++;  // Skip start flag

        // Find frame end (next 0x7E)
        while (i < combined_data.size() && combined_data[i] != PPP_FLAG) {
            i++;
        }

        if (i < combined_data.size()) {
            // Found complete frame (includes both 0x7E delimiters)
            mtp::ByteArray frame(combined_data.begin() + frame_start,
                                combined_data.begin() + i + 1);
            if (frame.size() > 2) {  // More than just two flags
                frames.push_back(frame);
            }
            i++;  // Move past end flag
        } else {
            // Incomplete frame - save for next call
            incomplete_buffer.assign(combined_data.begin() + frame_start,
                                    combined_data.end());
            break;
        }
    }

    return frames;
}

bool PPPParser::TryParseFrame(const mtp::ByteArray& data, ParsedFrame& frame) {
    try {
        if (!IsValidFrame(data)) {
            return false;
        }

        frame.raw_frame = data;
        frame.payload = ExtractPayload(data, &frame.protocol);
        return true;

    } catch (const std::exception&) {
        return false;
    }
}

// PPPParserHelpers namespace functions (require IPCPParser to be defined)
namespace PPPParserHelpers {

bool FindIPCPFrame(const mtp::ByteArray& data, uint8_t ipcp_code,
                   IPCPParser::IPCPPacket& packet) {
    auto frames = PPPParser::ExtractFrames(data);

    for (const auto& frame : frames) {
        PPPParser::ParsedFrame parsed;
        if (!PPPParser::TryParseFrame(frame, parsed)) {
            continue;
        }

        // Check if this is an IPCP frame (protocol 0x8021)
        if (parsed.protocol == 0x8021 && !parsed.payload.empty()) {
            try {
                IPCPParser::IPCPPacket ipcp = IPCPParser::ParsePacket(parsed.payload);
                if (ipcp.code == ipcp_code) {
                    packet = ipcp;
                    return true;
                }
            } catch (const std::exception&) {
                // Invalid IPCP packet, continue searching
            }
        }
    }

    return false;
}

bool ContainsIPCPCode(const mtp::ByteArray& data, uint8_t ipcp_code) {
    IPCPParser::IPCPPacket dummy;
    return FindIPCPFrame(data, ipcp_code, dummy);
}

} // namespace PPPParserHelpers

// ============================================================================
// IPParser Implementation
// ============================================================================

IPParser::IPHeader IPParser::ParseHeader(const mtp::ByteArray& data) {
    if (data.size() < 20) {
        throw std::runtime_error("Invalid IP packet: too short");
    }

    IPHeader header = {};

    // Byte 0: Version (4 bits) + IHL (4 bits)
    header.version = (data[0] >> 4) & 0x0F;
    header.header_length = data[0] & 0x0F;

    if (header.version != 4) {
        throw std::runtime_error("Only IPv4 is supported");
    }

    // Byte 1: DSCP (6 bits) + ECN (2 bits)
    header.dscp = (data[1] >> 2) & 0x3F;
    header.ecn = data[1] & 0x03;

    // Bytes 2-3: Total Length
    header.total_length = (static_cast<uint16_t>(data[2]) << 8) | data[3];

    // Bytes 4-5: Identification
    header.identification = (static_cast<uint16_t>(data[4]) << 8) | data[5];

    // Bytes 6-7: Flags + Fragment Offset
    header.flags_offset = (static_cast<uint16_t>(data[6]) << 8) | data[7];

    // Byte 8: TTL
    header.ttl = data[8];

    // Byte 9: Protocol
    header.protocol = data[9];

    // Bytes 10-11: Header Checksum
    header.checksum = (static_cast<uint16_t>(data[10]) << 8) | data[11];

    // Bytes 12-15: Source IP
    header.src_ip = (static_cast<uint32_t>(data[12]) << 24) |
                    (static_cast<uint32_t>(data[13]) << 16) |
                    (static_cast<uint32_t>(data[14]) << 8) |
                    static_cast<uint32_t>(data[15]);

    // Bytes 16-19: Destination IP
    header.dst_ip = (static_cast<uint32_t>(data[16]) << 24) |
                    (static_cast<uint32_t>(data[17]) << 16) |
                    (static_cast<uint32_t>(data[18]) << 8) |
                    static_cast<uint32_t>(data[19]);

    return header;
}

mtp::ByteArray IPParser::ExtractPayload(const mtp::ByteArray& data) {
    IPHeader header = ParseHeader(data);
    size_t header_size = header.header_length * 4;

    if (data.size() < header_size) {
        throw std::runtime_error("Invalid IP packet: header size exceeds packet size");
    }

    mtp::ByteArray payload;
    payload.insert(payload.end(), data.begin() + header_size, data.end());
    return payload;
}

mtp::ByteArray IPParser::BuildPacket(const IPHeader& header, const mtp::ByteArray& payload) {
    mtp::ByteArray packet;
    size_t header_size = header.header_length * 4;

    // Byte 0: Version + IHL
    packet.push_back((header.version << 4) | header.header_length);

    // Byte 1: DSCP + ECN
    packet.push_back((header.dscp << 2) | header.ecn);

    // Bytes 2-3: Total Length
    uint16_t total_length = header_size + payload.size();
    packet.push_back((total_length >> 8) & 0xFF);
    packet.push_back(total_length & 0xFF);

    // Bytes 4-5: Identification
    packet.push_back((header.identification >> 8) & 0xFF);
    packet.push_back(header.identification & 0xFF);

    // Bytes 6-7: Flags + Fragment Offset
    packet.push_back((header.flags_offset >> 8) & 0xFF);
    packet.push_back(header.flags_offset & 0xFF);

    // Byte 8: TTL
    packet.push_back(header.ttl);

    // Byte 9: Protocol
    packet.push_back(header.protocol);

    // Bytes 10-11: Checksum (placeholder, will calculate)
    packet.push_back(0);
    packet.push_back(0);

    // Bytes 12-15: Source IP
    packet.push_back((header.src_ip >> 24) & 0xFF);
    packet.push_back((header.src_ip >> 16) & 0xFF);
    packet.push_back((header.src_ip >> 8) & 0xFF);
    packet.push_back(header.src_ip & 0xFF);

    // Bytes 16-19: Destination IP
    packet.push_back((header.dst_ip >> 24) & 0xFF);
    packet.push_back((header.dst_ip >> 16) & 0xFF);
    packet.push_back((header.dst_ip >> 8) & 0xFF);
    packet.push_back(header.dst_ip & 0xFF);

    // Calculate and insert checksum
    uint16_t checksum = CalculateChecksum(packet.data(), header_size);
    packet[10] = (checksum >> 8) & 0xFF;
    packet[11] = checksum & 0xFF;

    // Append payload
    packet.insert(packet.end(), payload.begin(), payload.end());

    return packet;
}

uint16_t IPParser::CalculateChecksum(const uint8_t* header_data, size_t length) {
    uint32_t sum = 0;

    for (size_t i = 0; i < length; i += 2) {
        if (i == 10) {
            // Skip checksum field itself
            continue;
        }

        uint16_t word;
        if (i + 1 < length) {
            word = (static_cast<uint16_t>(header_data[i]) << 8) | header_data[i + 1];
        } else {
            word = static_cast<uint16_t>(header_data[i]) << 8;
        }
        sum += word;
    }

    // Fold 32-bit sum to 16 bits
    while (sum >> 16) {
        sum = (sum & 0xFFFF) + (sum >> 16);
    }

    return ~sum;
}

std::string IPParser::IPToString(uint32_t ip_addr) {
    std::ostringstream oss;
    oss << ((ip_addr >> 24) & 0xFF) << "."
        << ((ip_addr >> 16) & 0xFF) << "."
        << ((ip_addr >> 8) & 0xFF) << "."
        << (ip_addr & 0xFF);
    return oss.str();
}

uint32_t IPParser::StringToIP(const std::string& ip_str) {
    uint32_t a, b, c, d;
    if (sscanf(ip_str.c_str(), "%u.%u.%u.%u", &a, &b, &c, &d) != 4) {
        throw std::runtime_error("Invalid IP address format");
    }
    return (a << 24) | (b << 16) | (c << 8) | d;
}

// ============================================================================
// TCPParser Implementation
// ============================================================================

TCPParser::TCPHeader TCPParser::ParseHeader(const mtp::ByteArray& data) {
    if (data.size() < 20) {
        throw std::runtime_error("Invalid TCP segment: too short");
    }

    TCPHeader header = {};

    // Bytes 0-1: Source Port
    header.src_port = (static_cast<uint16_t>(data[0]) << 8) | data[1];

    // Bytes 2-3: Destination Port
    header.dst_port = (static_cast<uint16_t>(data[2]) << 8) | data[3];

    // Bytes 4-7: Sequence Number
    header.seq_num = (static_cast<uint32_t>(data[4]) << 24) |
                     (static_cast<uint32_t>(data[5]) << 16) |
                     (static_cast<uint32_t>(data[6]) << 8) |
                     static_cast<uint32_t>(data[7]);

    // Bytes 8-11: Acknowledgment Number
    header.ack_num = (static_cast<uint32_t>(data[8]) << 24) |
                     (static_cast<uint32_t>(data[9]) << 16) |
                     (static_cast<uint32_t>(data[10]) << 8) |
                     static_cast<uint32_t>(data[11]);

    // Byte 12: Data Offset (4 bits) + Reserved (4 bits)
    header.data_offset = (data[12] >> 4) & 0x0F;

    // Byte 13: Flags
    header.flags = data[13];

    // Bytes 14-15: Window Size
    header.window_size = (static_cast<uint16_t>(data[14]) << 8) | data[15];

    // Bytes 16-17: Checksum
    header.checksum = (static_cast<uint16_t>(data[16]) << 8) | data[17];

    // Bytes 18-19: Urgent Pointer
    header.urgent_pointer = (static_cast<uint16_t>(data[18]) << 8) | data[19];

    return header;
}

mtp::ByteArray TCPParser::ExtractPayload(const mtp::ByteArray& data) {
    TCPHeader header = ParseHeader(data);
    size_t header_size = header.data_offset * 4;

    if (data.size() < header_size) {
        throw std::runtime_error("Invalid TCP segment: header size exceeds segment size");
    }

    mtp::ByteArray payload;
    payload.insert(payload.end(), data.begin() + header_size, data.end());
    return payload;
}

mtp::ByteArray TCPParser::BuildSegment(const TCPHeader& header,
                                       const mtp::ByteArray& payload,
                                       uint32_t src_ip,
                                       uint32_t dst_ip) {
    mtp::ByteArray segment;

    // Bytes 0-1: Source Port
    segment.push_back((header.src_port >> 8) & 0xFF);
    segment.push_back(header.src_port & 0xFF);

    // Bytes 2-3: Destination Port
    segment.push_back((header.dst_port >> 8) & 0xFF);
    segment.push_back(header.dst_port & 0xFF);

    // Bytes 4-7: Sequence Number
    segment.push_back((header.seq_num >> 24) & 0xFF);
    segment.push_back((header.seq_num >> 16) & 0xFF);
    segment.push_back((header.seq_num >> 8) & 0xFF);
    segment.push_back(header.seq_num & 0xFF);

    // Bytes 8-11: Acknowledgment Number
    segment.push_back((header.ack_num >> 24) & 0xFF);
    segment.push_back((header.ack_num >> 16) & 0xFF);
    segment.push_back((header.ack_num >> 8) & 0xFF);
    segment.push_back(header.ack_num & 0xFF);

    // Byte 12: Data Offset (4 bits) + Reserved (4 bits)
    segment.push_back((header.data_offset << 4) & 0xF0);

    // Byte 13: Flags
    segment.push_back(header.flags);

    // Bytes 14-15: Window Size
    segment.push_back((header.window_size >> 8) & 0xFF);
    segment.push_back(header.window_size & 0xFF);

    // Bytes 16-17: Checksum (placeholder)
    segment.push_back(0);
    segment.push_back(0);

    // Bytes 18-19: Urgent Pointer
    segment.push_back((header.urgent_pointer >> 8) & 0xFF);
    segment.push_back(header.urgent_pointer & 0xFF);

    // Append payload
    segment.insert(segment.end(), payload.begin(), payload.end());

    // Calculate and insert checksum
    uint16_t checksum = CalculateChecksum(segment.data(), segment.size(), src_ip, dst_ip);
    segment[16] = (checksum >> 8) & 0xFF;
    segment[17] = checksum & 0xFF;

    return segment;
}

uint16_t TCPParser::CalculateChecksum(const uint8_t* tcp_data, size_t length,
                                      uint32_t src_ip, uint32_t dst_ip) {
    uint32_t sum = 0;

    // Add pseudo-header
    // Source IP
    sum += (src_ip >> 16) & 0xFFFF;
    sum += src_ip & 0xFFFF;

    // Destination IP
    sum += (dst_ip >> 16) & 0xFFFF;
    sum += dst_ip & 0xFFFF;

    // Protocol (6 for TCP)
    sum += 6;

    // TCP Length
    sum += length;

    // Add TCP segment (excluding checksum field)
    for (size_t i = 0; i < length; i += 2) {
        if (i == 16) {
            // Skip checksum field
            continue;
        }

        uint16_t word;
        if (i + 1 < length) {
            word = (static_cast<uint16_t>(tcp_data[i]) << 8) | tcp_data[i + 1];
        } else {
            word = static_cast<uint16_t>(tcp_data[i]) << 8;
        }
        sum += word;
    }

    // Fold 32-bit sum to 16 bits
    while (sum >> 16) {
        sum = (sum & 0xFFFF) + (sum >> 16);
    }

    return ~sum;
}

std::string TCPParser::FlagsToString(uint8_t flags) {
    std::string result;

    if (flags & TCP_FLAG_FIN) result += "FIN,";
    if (flags & TCP_FLAG_SYN) result += "SYN,";
    if (flags & TCP_FLAG_RST) result += "RST,";
    if (flags & TCP_FLAG_PSH) result += "PSH,";
    if (flags & TCP_FLAG_ACK) result += "ACK,";
    if (flags & TCP_FLAG_URG) result += "URG,";

    if (!result.empty() && result.back() == ',') {
        result.pop_back();
    }

    return result.empty() ? "NONE" : result;
}

// ===== IPCPParser Implementation =====

IPCPParser::IPCPPacket IPCPParser::ParsePacket(const mtp::ByteArray& data) {
    IPCPPacket packet;

    if (data.size() < 4) {
        throw std::runtime_error("IPCP packet too small");
    }

    packet.code = data[0];
    packet.identifier = data[1];
    packet.length = (data[2] << 8) | data[3];

    if (data.size() < packet.length) {
        throw std::runtime_error("IPCP packet length mismatch");
    }

    // Extract options (everything after 4-byte header)
    if (packet.length > 4) {
        packet.options.assign(data.begin() + 4, data.begin() + packet.length);
    }

    return packet;
}

mtp::ByteArray IPCPParser::BuildPacket(const IPCPPacket& packet) {
    mtp::ByteArray result;

    result.push_back(packet.code);
    result.push_back(packet.identifier);

    uint16_t length = 4 + packet.options.size();
    result.push_back((length >> 8) & 0xFF);
    result.push_back(length & 0xFF);

    result.insert(result.end(), packet.options.begin(), packet.options.end());

    return result;
}

std::vector<IPCPParser::IPCPOption> IPCPParser::ParseOptions(const mtp::ByteArray& options_data) {
    std::vector<IPCPOption> options;
    size_t idx = 0;

    while (idx + 2 <= options_data.size()) {
        IPCPOption opt;
        opt.type = options_data[idx];
        opt.length = options_data[idx + 1];

        if (opt.length < 2 || idx + opt.length > options_data.size()) {
            break;  // Invalid option
        }

        opt.data.assign(options_data.begin() + idx + 2,
                       options_data.begin() + idx + opt.length);
        options.push_back(opt);

        idx += opt.length;
    }

    return options;
}

mtp::ByteArray IPCPParser::BuildConfigRequest(uint8_t request_id, uint32_t host_ip, uint32_t primary_dns) {
    IPCPPacket packet;
    packet.code = IPCP_CODE_CONFIG_REQUEST;
    packet.identifier = request_id;

    // Option 3: IP Address (6 bytes total: type, length, 4-byte IP)
    packet.options.push_back(IPCP_OPT_IP_ADDRESS);
    packet.options.push_back(6);
    packet.options.push_back((host_ip >> 24) & 0xFF);
    packet.options.push_back((host_ip >> 16) & 0xFF);
    packet.options.push_back((host_ip >> 8) & 0xFF);
    packet.options.push_back(host_ip & 0xFF);

    // Option 129 (0x81): Primary DNS (6 bytes total: type, length, 4-byte IP)
    packet.options.push_back(IPCP_OPT_PRIMARY_DNS);
    packet.options.push_back(6);
    packet.options.push_back((primary_dns >> 24) & 0xFF);
    packet.options.push_back((primary_dns >> 16) & 0xFF);
    packet.options.push_back((primary_dns >> 8) & 0xFF);
    packet.options.push_back(primary_dns & 0xFF);

    return BuildPacket(packet);
}

mtp::ByteArray IPCPParser::BuildConfigNak(uint8_t request_id, uint32_t device_ip, uint32_t primary_dns) {
    IPCPPacket packet;
    packet.code = IPCP_CODE_CONFIG_NAK;
    packet.identifier = request_id;

    // Option 3: IP Address (6 bytes total: type, length, 4-byte IP)
    packet.options.push_back(IPCP_OPT_IP_ADDRESS);
    packet.options.push_back(6);
    packet.options.push_back((device_ip >> 24) & 0xFF);
    packet.options.push_back((device_ip >> 16) & 0xFF);
    packet.options.push_back((device_ip >> 8) & 0xFF);
    packet.options.push_back(device_ip & 0xFF);

    // Option 129 (0x81): Primary DNS (6 bytes total: type, length, 4-byte IP)
    packet.options.push_back(IPCP_OPT_PRIMARY_DNS);
    packet.options.push_back(6);
    packet.options.push_back((primary_dns >> 24) & 0xFF);
    packet.options.push_back((primary_dns >> 16) & 0xFF);
    packet.options.push_back((primary_dns >> 8) & 0xFF);
    packet.options.push_back(primary_dns & 0xFF);

    return BuildPacket(packet);
}

mtp::ByteArray IPCPParser::BuildConfigAck(const IPCPPacket& request) {
    IPCPPacket ack;
    ack.code = IPCP_CODE_CONFIG_ACK;
    ack.identifier = request.identifier;
    ack.options = request.options;  // Echo back all options

    return BuildPacket(ack);
}

// ===== DNSServer Implementation =====

std::string DNSServer::ParseHostname(const mtp::ByteArray& query) {
    if (query.size() < 12) {
        return "";  // Too small for DNS header
    }

    // DNS query format:
    // 0-1: Transaction ID
    // 2-3: Flags
    // 4-5: Question count
    // 6-7: Answer count
    // 8-9: Authority count
    // 10-11: Additional count
    // 12+: Questions (name, type, class)

    std::string hostname;
    size_t idx = 12;

    while (idx < query.size()) {
        uint8_t label_len = query[idx++];

        if (label_len == 0) {
            break;  // End of name
        }

        if (label_len >= 64) {
            break;  // Compressed label (not supported here)
        }

        if (idx + label_len > query.size()) {
            break;  // Invalid
        }

        if (!hostname.empty()) {
            hostname += '.';
        }

        // RFC 1035 Section 2.3.3: DNS names are case-insensitive
        // Convert to lowercase for consistent matching
        for (size_t i = 0; i < label_len; i++) {
            char c = query[idx + i];
            hostname += std::tolower(c);
        }
        idx += label_len;
    }

    return hostname;
}

mtp::ByteArray DNSServer::BuildResponse(const mtp::ByteArray& query,
                                       const std::map<std::string, uint32_t>& hostname_map) {
    if (query.size() < 12) {
        return mtp::ByteArray();  // Invalid query
    }

    std::string hostname = ParseHostname(query);

    // Find where the question section ends to extract query type
    size_t question_start = 12;
    size_t question_end = question_start;
    while (question_end < query.size()) {
        if (query[question_end] == 0) {
            // Found end of name, type and class follow (4 bytes)
            question_end += 1;  // Skip null terminator
            break;
        }
        uint8_t len = query[question_end];
        if (len >= 64 || question_end + len + 1 > query.size()) {
            return mtp::ByteArray();  // Malformed query
        }
        question_end += len + 1;
    }

    // Check if we have enough bytes for type (2) + class (2)
    if (question_end + 4 > query.size()) {
        return mtp::ByteArray();  // Malformed query
    }

    // Extract query type (2 bytes after hostname null terminator)
    uint16_t query_type = (query[question_end] << 8) | query[question_end + 1];

    // We only support Type A (0x0001) queries
    if (query_type != 0x0001) {
        return mtp::ByteArray();  // Unsupported query type
    }

    // Check if we have a mapping for this hostname
    auto it = hostname_map.find(hostname);
    if (it == hostname_map.end()) {
        // No mapping found - return empty (no answer)
        return mtp::ByteArray();
    }

    uint32_t ip_addr = it->second;

    // Build DNS response
    mtp::ByteArray response;

    // Copy transaction ID and set response flags
    response.push_back(query[0]);  // Transaction ID (high)
    response.push_back(query[1]);  // Transaction ID (low)
    response.push_back(0x81);      // QR=1 (response), Opcode=0, AA=0, TC=0, RD=1
    response.push_back(0x80);      // RA=1, Z=0, RCODE=0 (No error)

    // Question count (1)
    response.push_back(0x00);
    response.push_back(0x01);

    // Answer count (1)
    response.push_back(0x00);
    response.push_back(0x01);

    // Authority count (0)
    response.push_back(0x00);
    response.push_back(0x00);

    // Additional count (0)
    response.push_back(0x00);
    response.push_back(0x00);

    // Copy question section from query (hostname + type + class)
    // question_end is already positioned at the start of type field
    size_t question_section_end = question_end + 4;  // type (2) + class (2)

    if (question_section_end > query.size()) {
        return mtp::ByteArray();  // Malformed
    }

    response.insert(response.end(), query.begin() + question_start, query.begin() + question_section_end);

    // Answer section
    // Name (pointer to question): 0xC00C (pointer to offset 12)
    response.push_back(0xC0);
    response.push_back(0x0C);

    // Type: A (0x0001)
    response.push_back(0x00);
    response.push_back(0x01);

    // Class: IN (0x0001)
    response.push_back(0x00);
    response.push_back(0x01);

    // TTL: 300 seconds
    response.push_back(0x00);
    response.push_back(0x00);
    response.push_back(0x01);
    response.push_back(0x2C);

    // Data length: 4 bytes
    response.push_back(0x00);
    response.push_back(0x04);

    // IP address (4 bytes)
    response.push_back((ip_addr >> 24) & 0xFF);
    response.push_back((ip_addr >> 16) & 0xFF);
    response.push_back((ip_addr >> 8) & 0xFF);
    response.push_back(ip_addr & 0xFF);

    return response;
}
