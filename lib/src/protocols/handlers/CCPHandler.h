#pragma once

#include <mtp/ByteArray.h>
#include <optional>
#include <functional>
#include <string>

/**
 * CCPHandler
 *
 * Handles CCP (Compression Control Protocol) negotiation for PPP.
 *
 * The Zune device may request compression capabilities via CCP Config-Request.
 * Since we don't support compression:
 * - If Config-Request has options, we respond with Config-Reject
 * - If Config-Request has no options (empty), we respond with Config-Ack
 *   to complete the negotiation (both sides agree: no compression)
 *
 * This is a stateless handler following Clean Architecture principles.
 */
class CCPHandler {
public:
    using LogCallback = std::function<void(const std::string& message)>;

    /**
     * Constructor
     */
    CCPHandler() = default;

    /**
     * Handle CCP packet
     *
     * @param ccp_packet CCP packet data (without PPP framing)
     * @return PPP-framed CCP response if a response is needed, std::nullopt otherwise
     */
    std::optional<mtp::ByteArray> HandlePacket(const mtp::ByteArray& ccp_packet);

    /**
     * Set logging callback for diagnostic messages
     * @param callback Function to receive log messages
     */
    void SetLogCallback(LogCallback callback);

private:
    /**
     * Build Config-Reject response for CCP Config-Request with options
     * @param identifier Identifier from Config-Request
     * @param original_packet Original CCP packet
     * @return CCP Config-Reject packet (without PPP framing)
     */
    mtp::ByteArray BuildConfigReject(uint8_t identifier, const mtp::ByteArray& original_packet);

    /**
     * Build Config-Ack response for empty CCP Config-Request (no options)
     * @param identifier Identifier from Config-Request
     * @return CCP Config-Ack packet (without PPP framing)
     */
    mtp::ByteArray BuildConfigAck(uint8_t identifier);

    /**
     * Log a message via callback if set
     */
    void Log(const std::string& message);

    LogCallback log_callback_;
};
