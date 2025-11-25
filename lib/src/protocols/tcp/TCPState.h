#pragma once

#include <cstdint>
#include <string>

/**
 * TCPState
 *
 * Explicit state machine for TCP connections (RFC 793).
 *
 * State Transition Diagram:
 *
 * CLOSED
 *   ↓ (Receive SYN)
 * SYN_RECEIVED (Send SYN-ACK)
 *   ↓ (Receive ACK)
 * ESTABLISHED (Connection ready for data)
 *   ↓ (Receive FIN)
 * CLOSE_WAIT (Send ACK)
 *   ↓ (Send FIN)
 * LAST_ACK (Wait for final ACK)
 *   ↓ (Receive ACK)
 * CLOSED
 */
enum class TCPState : uint8_t {
    CLOSED = 0,       // No connection exists
    LISTEN = 1,       // Server is listening (not used in Zune client-server model)
    SYN_SENT = 2,     // SYN sent, waiting for SYN-ACK (client side, not used)
    SYN_RECEIVED = 3, // SYN received, SYN-ACK sent, waiting for ACK
    ESTABLISHED = 4,  // Connection established, data can be transferred
    FIN_WAIT_1 = 5,   // FIN sent, waiting for ACK (active close, not used)
    FIN_WAIT_2 = 6,   // FIN ACKed, waiting for FIN (active close, not used)
    CLOSE_WAIT = 7,   // FIN received, ACK sent, waiting to send FIN (passive close)
    CLOSING = 8,      // Both sides closing simultaneously (rare, not used)
    LAST_ACK = 9,     // FIN sent after CLOSE_WAIT, waiting for ACK
    TIME_WAIT = 10,   // Waiting for 2*MSL after active close (not used)
};

/**
 * Convert TCPState to string for debugging
 */
inline std::string TCPStateToString(TCPState state) {
    switch (state) {
        case TCPState::CLOSED:       return "CLOSED";
        case TCPState::LISTEN:       return "LISTEN";
        case TCPState::SYN_SENT:     return "SYN_SENT";
        case TCPState::SYN_RECEIVED: return "SYN_RECEIVED";
        case TCPState::ESTABLISHED:  return "ESTABLISHED";
        case TCPState::FIN_WAIT_1:   return "FIN_WAIT_1";
        case TCPState::FIN_WAIT_2:   return "FIN_WAIT_2";
        case TCPState::CLOSE_WAIT:   return "CLOSE_WAIT";
        case TCPState::CLOSING:      return "CLOSING";
        case TCPState::LAST_ACK:     return "LAST_ACK";
        case TCPState::TIME_WAIT:    return "TIME_WAIT";
        default:                     return "UNKNOWN";
    }
}
