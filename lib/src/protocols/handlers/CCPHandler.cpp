#include "CCPHandler.h"
#include "../ppp/PPPParser.h"

// CCP (Compression Control Protocol) codes
constexpr uint8_t CCP_CODE_CONFIG_REQUEST = 1;
constexpr uint8_t CCP_CODE_CONFIG_ACK = 2;
constexpr uint8_t CCP_CODE_CONFIG_REJECT = 4;

std::optional<mtp::ByteArray> CCPHandler::HandlePacket(const mtp::ByteArray& ccp_packet) {
    // Validate minimum packet size (code + identifier + length)
    if (ccp_packet.size() < 4) {
        return std::nullopt;
    }

    uint8_t code = ccp_packet[0];
    uint8_t identifier = ccp_packet[1];

    // RFC 1661 Section 5.2: Validate length field matches actual packet size
    uint16_t claimed_length = (ccp_packet[2] << 8) | ccp_packet[3];
    if (claimed_length != ccp_packet.size()) {
        // Length mismatch - silently discard per RFC 1661
        return std::nullopt;
    }

    // Only handle Config-Request
    if (code == CCP_CODE_CONFIG_REQUEST) {
        // RFC 1661: If Config-Request has no options (length = 4, header only),
        // respond with Config-Ack to complete negotiation (no compression).
        // If it has options, reject them all.
        bool has_options = (ccp_packet.size() > 4);

        if (has_options) {
            Log("CCP Config-Request received (id=" + std::to_string(identifier) +
                ", " + std::to_string(ccp_packet.size() - 4) + " bytes of options), sending Config-Reject");

            // Build Config-Reject response
            mtp::ByteArray ccp_reject = BuildConfigReject(identifier, ccp_packet);

            // Wrap in PPP frame
            mtp::ByteArray ppp_frame = PPPParser::WrapPayload(ccp_reject, 0x80fd);

            Log("CCP Config-Reject built (PPP frame size: " + std::to_string(ppp_frame.size()) + " bytes)");

            return ppp_frame;
        } else {
            Log("CCP Config-Request received (id=" + std::to_string(identifier) +
                ", no options), sending Config-Ack (no compression)");

            // Build Config-Ack response for empty Config-Request
            mtp::ByteArray ccp_ack = BuildConfigAck(identifier);

            // Wrap in PPP frame
            mtp::ByteArray ppp_frame = PPPParser::WrapPayload(ccp_ack, 0x80fd);

            Log("CCP Config-Ack built (PPP frame size: " + std::to_string(ppp_frame.size()) + " bytes)");

            return ppp_frame;
        }
    }

    // Ignore other CCP codes (Config-Ack, Config-Nak, etc.)
    return std::nullopt;
}

void CCPHandler::SetLogCallback(LogCallback callback) {
    log_callback_ = callback;
}

mtp::ByteArray CCPHandler::BuildConfigReject(uint8_t identifier, const mtp::ByteArray& original_packet) {
    mtp::ByteArray ccp_reject;

    // CCP Config-Reject format:
    // - Code: 0x04 (Config-Reject)
    // - Identifier: Same as Config-Request
    // - Length: Same as Config-Request
    // - Options: Copy original options (we reject them all)

    ccp_reject.push_back(CCP_CODE_CONFIG_REJECT);  // Config-Reject
    ccp_reject.push_back(identifier);              // Same identifier

    // Length (2 bytes, big-endian)
    uint16_t length = original_packet.size();
    ccp_reject.push_back((length >> 8) & 0xFF);
    ccp_reject.push_back(length & 0xFF);

    // Copy original options (everything after the 4-byte header)
    ccp_reject.insert(ccp_reject.end(), original_packet.begin() + 4, original_packet.end());

    return ccp_reject;
}

mtp::ByteArray CCPHandler::BuildConfigAck(uint8_t identifier) {
    mtp::ByteArray ccp_ack;

    // CCP Config-Ack format (for empty Config-Request):
    // - Code: 0x02 (Config-Ack)
    // - Identifier: Same as Config-Request
    // - Length: 4 (header only, no options)

    ccp_ack.push_back(CCP_CODE_CONFIG_ACK);  // Config-Ack
    ccp_ack.push_back(identifier);            // Same identifier

    // Length (2 bytes, big-endian) - 4 bytes for header only
    ccp_ack.push_back(0x00);
    ccp_ack.push_back(0x04);

    return ccp_ack;
}

void CCPHandler::Log(const std::string& message) {
    if (log_callback_) {
        log_callback_(message);
    }
}
