#pragma once

#include "TCPState.h"
#include "TCPStreamReassembler.h"
#include "TCPFlowController.h"
#include "RTOManager.h"
#include <mtp/ByteArray.h>
#include <cstdint>
#include <string>
#include <map>
#include <mutex>
#include <functional>
#include <optional>
#include <memory>
#include <chrono>
#include <vector>

/**
 * SentSegment - Tracks unacknowledged segments for RTO
 */
struct SentSegment {
    uint32_t seq_start;     // Starting sequence number
    uint32_t seq_end;       // Ending sequence number (exclusive)
    std::chrono::steady_clock::time_point send_time;  // When segment was sent
    bool is_retransmit;     // True if this is a retransmission
    mtp::ByteArray data;    // Segment data for retransmission
};

/**
 * TransmissionState - State machine for HTTP response transmission
 *
 * State transitions:
 *   PENDING → IN_PROGRESS (first segment sent)
 *   IN_PROGRESS → AWAITING_ACKS (all segments sent)
 *   IN_PROGRESS → NEEDS_RETRANSMIT (3 duplicate ACKs)
 *   AWAITING_ACKS → NEEDS_RETRANSMIT (3 duplicate ACKs)
 *   NEEDS_RETRANSMIT → IN_PROGRESS (retransmit sent)
 *   AWAITING_ACKS → COMPLETE (all ACKed)
 */
enum class TransmissionState : uint8_t {
    PENDING = 0,          // Queued but not started
    IN_PROGRESS = 1,      // Sending segments
    AWAITING_ACKS = 2,    // All segments sent, waiting for ACKs
    NEEDS_RETRANSMIT = 3, // Fast retransmit triggered
    COMPLETE = 4          // All segments ACKed
};

inline std::string TransmissionStateToString(TransmissionState state) {
    switch (state) {
        case TransmissionState::PENDING:          return "PENDING";
        case TransmissionState::IN_PROGRESS:      return "IN_PROGRESS";
        case TransmissionState::AWAITING_ACKS:    return "AWAITING_ACKS";
        case TransmissionState::NEEDS_RETRANSMIT: return "NEEDS_RETRANSMIT";
        case TransmissionState::COMPLETE:         return "COMPLETE";
        default:                                  return "UNKNOWN";
    }
}

/**
 * HTTPTransmission - Tracks a single HTTP response transmission
 *
 * Each HTTP response is split into TCP segments and tracked separately.
 * Multiple transmissions can exist on the same connection (HTTP keep-alive).
 */
struct HTTPTransmission {
    uint32_t base_seq = 0;                            // Starting SEQ for this response
    std::vector<mtp::ByteArray> queued_segments;      // Pre-built PPP frames
    std::vector<size_t> segment_payload_sizes;        // HTTP payload size per segment
    size_t next_segment_index = 0;                    // Next segment to send
    TransmissionState state = TransmissionState::PENDING;  // Explicit state machine
    size_t retransmit_segment_index = 0;              // Which segment to retransmit
    std::chrono::steady_clock::time_point last_ack_time;

    // State query helpers
    bool IsComplete() const { return state == TransmissionState::COMPLETE; }
    bool NeedsRetransmit() const { return state == TransmissionState::NEEDS_RETRANSMIT; }
};

/**
 * TCPConnectionInfo
 *
 * Complete state for a single TCP connection with explicit state machine.
 * Includes flow control (RFC 5681) and HTTP transmission tracking.
 *
 * This is the SINGLE SOURCE OF TRUTH for all TCP connection state.
 */
struct TCPConnectionInfo {
    // ==== State machine ====
    TCPState state = TCPState::CLOSED;

    // ==== Sequence numbers ====
    uint32_t seq_num = 0;          // Our sequence number (next byte to send)
    uint32_t ack_num = 0;          // ACK number for remote (next expected byte)
    std::mutex seq_num_mutex;      // Protects seq_num/ack_num for concurrent sends

    // ==== Flow control (RFC 5681) - SINGLE SOURCE OF TRUTH ====
    std::unique_ptr<TCPFlowController> flow_controller;

    // ==== HTTP stream reassembler ====
    std::unique_ptr<TCPStreamReassembler> reassembler;

    // ==== RTO management (RFC 6298) ====
    RTOManager rto_manager;
    std::map<uint32_t, SentSegment> unacked_segments;  // Key: seq_start

    // ==== HTTP transmission tracking ====
    // Maps base_seq to transmission state (supports multiple HTTP responses on same connection)
    std::map<uint32_t, HTTPTransmission> active_transmissions;
    std::mutex transmissions_mutex;

    // ==== Receiver window ====
    uint16_t receiver_window = 65535;  // Advertised window from remote
    uint32_t last_window_right_edge = 0;  // For duplicate ACK detection

    // ==== Logging ====
    std::function<void(const std::string&)>* log_callback = nullptr;

    // ==== State transition methods ====
    bool TransitionTo(TCPState new_state);
    bool CanTransitionTo(TCPState new_state) const;

    // ==== RTO methods ====
    void RecordSentSegment(uint32_t seq_start, const mtp::ByteArray& data, bool is_retransmit = false);
    void ProcessACKForRTO(uint32_t ack_num);
    std::vector<SentSegment> CheckTimeouts();

    // ==== Flow control methods ====

    /**
     * Initialize flow controller for this connection
     */
    void InitializeFlowControl();

    /**
     * Process an ACK packet - updates flow control, detects duplicates, triggers retransmit
     * @param ack_num ACK number from packet
     * @param window_size Receiver's advertised window
     * @return true if new data was ACKed
     */
    bool ProcessACK(uint32_t ack_num, uint16_t window_size);

    /**
     * Record bytes sent for flow control tracking
     * @param payload_size Bytes sent
     * @param seq_num Sequence number
     */
    void RecordBytesSent(size_t payload_size, uint32_t seq_num);

    /**
     * Get number of bytes that can be sent now (respects cwnd and receiver window)
     * @return Available window space in bytes
     */
    size_t GetAvailableWindow() const;

    /**
     * Check if fast retransmit is needed
     */
    bool NeedsFastRetransmit() const;

    /**
     * Get segment index to retransmit
     * @param base_seq Base sequence of transmission to check
     */
    size_t GetRetransmitSegmentIndex(uint32_t base_seq) const;

    /**
     * Clear retransmit flag after handling
     */
    void ClearRetransmitFlag();

    // ==== HTTP transmission methods ====

    /**
     * Start a new HTTP response transmission
     * @param base_seq Starting sequence number
     * @param segments Pre-built PPP frames
     * @param payload_sizes HTTP payload size per segment
     */
    void StartTransmission(uint32_t base_seq,
                          std::vector<mtp::ByteArray> segments,
                          std::vector<size_t> payload_sizes);

    /**
     * Get the transmission that matches an ACK number
     * @param ack_num ACK number received
     * @return Pointer to transmission, or nullptr if not found
     */
    HTTPTransmission* GetTransmissionForACK(uint32_t ack_num);

    /**
     * Calculate how many segments can be sent for a transmission
     * @param trans Transmission to check
     * @param max_batch Maximum segments per batch
     * @return Number of segments that can be sent now
     */
    size_t CalculateSegmentsToSend(const HTTPTransmission& trans, size_t max_batch = 3) const;

    /**
     * Mark segments as sent and update tracking
     * @param base_seq Transmission base sequence
     * @param num_segments Number of segments sent
     * @param bytes_sent Total bytes sent
     */
    void MarkSegmentsSent(uint32_t base_seq, size_t num_segments, size_t bytes_sent);

    /**
     * Update transmission state after ACK
     * @param trans Transmission to update
     * @param bytes_acked Bytes acknowledged
     * @return true if transmission is complete
     */
    bool UpdateTransmissionAfterACK(HTTPTransmission& trans, uint32_t bytes_acked);

    /**
     * Remove completed transmission
     * @param base_seq Base sequence of transmission to remove
     */
    void RemoveTransmission(uint32_t base_seq);

    /**
     * Get bytes in flight for this connection
     */
    size_t GetBytesInFlight() const;
};

/**
 * TCPPacket
 *
 * Represents a TCP packet to be sent (without PPP/IP wrapping).
 */
struct TCPPacket {
    uint32_t src_ip;
    uint16_t src_port;
    uint32_t dst_ip;
    uint16_t dst_port;
    uint32_t seq_num;
    uint32_t ack_num;
    uint8_t flags;
    mtp::ByteArray payload;  // Optional payload (empty for handshake)
};

/**
 * TCPConnectionManager
 *
 * Manages TCP connections with explicit state machine.
 *
 * Responsibilities:
 * - Track TCP connection states
 * - Handle SYN/SYN-ACK/ACK handshake
 * - Handle FIN/RST connection termination
 * - Manage sequence/ACK numbers
 * - Flow control (RFC 5681) via TCPFlowController
 * - HTTP response transmission tracking
 *
 * This class is thread-safe.
 */
class TCPConnectionManager {
public:
    using LogCallback = std::function<void(const std::string& message)>;

    TCPConnectionManager() = default;

    /**
     * Handle incoming TCP packet
     * @return TCP response packet if needed, std::nullopt otherwise
     */
    std::optional<TCPPacket> HandlePacket(
        uint32_t src_ip, uint16_t src_port,
        uint32_t dst_ip, uint16_t dst_port,
        uint32_t seq_num, uint32_t ack_num,
        uint8_t flags, uint16_t window_size,
        const mtp::ByteArray& payload);

    /**
     * Process an ACK for flow control (called by interceptor)
     * This is the main entry point for ACK processing.
     *
     * @param conn_key Connection key
     * @param ack_num ACK number
     * @param window_size Receiver's advertised window
     * @return Transmission base_seq that needs SendNextBatch, or 0 if none
     */
    uint32_t ProcessACKForTransmission(const std::string& conn_key,
                                        uint32_t ack_num, uint16_t window_size);

    /**
     * Start HTTP response transmission
     * @param conn_key Connection key
     * @param base_seq Starting sequence number
     * @param segments Pre-built PPP frames
     * @param payload_sizes HTTP payload size per segment
     */
    void StartHTTPTransmission(const std::string& conn_key,
                               uint32_t base_seq,
                               std::vector<mtp::ByteArray> segments,
                               std::vector<size_t> payload_sizes);

    /**
     * Get next batch of segments to send
     * @param conn_key Connection key
     * @param base_seq Transmission base sequence
     * @param[out] segments_out Vector to fill with segments to send
     * @param[out] is_last_batch Set to true if this is the final batch
     * @return Number of segments to send (0 if window full or complete)
     */
    size_t GetNextBatch(const std::string& conn_key, uint32_t base_seq,
                        std::vector<mtp::ByteArray>& segments_out,
                        bool& is_last_batch);

    /**
     * Check if fast retransmit is needed for a connection
     * @param conn_key Connection key
     * @param[out] base_seq Transmission that needs retransmit
     * @param[out] segment_index Segment to retransmit
     * @return true if retransmit needed
     */
    bool CheckRetransmitNeeded(const std::string& conn_key,
                                uint32_t& base_seq, size_t& segment_index);

    /**
     * Get retransmit segment
     * @param conn_key Connection key
     * @param base_seq Transmission base sequence
     * @param segment_index Segment index
     * @return PPP frame to retransmit, or empty if not found
     */
    mtp::ByteArray GetRetransmitSegment(const std::string& conn_key,
                                         uint32_t base_seq, size_t segment_index);

    /**
     * Clear retransmit flag after handling
     * @param conn_key Connection key
     */
    void ClearRetransmitFlag(const std::string& conn_key);

    /**
     * Get connection information
     * @param conn_key Connection key
     * @return Connection info, or nullptr if not found
     */
    TCPConnectionInfo* GetConnection(const std::string& conn_key);

    /**
     * Get or create connection
     * @param conn_key Connection key
     * @return Reference to connection info
     */
    TCPConnectionInfo& GetOrCreateConnection(const std::string& conn_key);

    /**
     * Make connection key from TCP 4-tuple
     */
    static std::string MakeConnectionKey(uint32_t src_ip, uint16_t src_port,
                                         uint32_t dst_ip, uint16_t dst_port);

    /**
     * Segment HTTP response data into TCP-sized chunks
     *
     * Splits HTTP data into segments suitable for TCP transmission:
     * - First segment: HTTP headers (up to \r\n\r\n)
     * - Subsequent segments: Body chunks of max_segment_size bytes
     *
     * @param http_data Complete HTTP response bytes
     * @param max_segment_size Maximum segment size (default 1460 = MTU 1500 - IP 20 - TCP 20)
     * @return Vector of segments, with payload sizes stored in segment_sizes
     */
    static std::vector<mtp::ByteArray> SegmentHTTPPayload(
        const mtp::ByteArray& http_data,
        size_t max_segment_size = 1460);

    void SetLogCallback(LogCallback callback);

    /**
     * Get all active connection keys
     * @return Vector of connection keys with active transmissions
     */
    std::vector<std::string> GetActiveConnectionKeys();

    /**
     * Check all connections for RTO timeouts
     * @return Map of conn_key -> vector of timed-out segments
     */
    std::map<std::string, std::vector<SentSegment>> CheckAllTimeouts();

    /**
     * Handle RTO retransmission for a connection
     * @param conn_key Connection key
     * @param segment Timed-out segment
     * @param[out] base_seq Base sequence of transmission needing retransmit
     * @param[out] segment_index Index of segment to retransmit
     * @return true if retransmit can be performed
     */
    bool HandleRTORetransmit(const std::string& conn_key, const SentSegment& segment,
                             uint32_t& base_seq, size_t& segment_index);

private:
    std::optional<TCPPacket> HandleSYN(
        const std::string& conn_key,
        uint32_t src_ip, uint16_t src_port,
        uint32_t dst_ip, uint16_t dst_port,
        uint32_t seq_num, uint16_t window_size);

    std::optional<TCPPacket> HandleACK(
        const std::string& conn_key,
        uint32_t src_ip, uint16_t src_port,
        uint32_t dst_ip, uint16_t dst_port,
        uint32_t seq_num, uint32_t ack_num,
        const mtp::ByteArray& payload);

    std::optional<TCPPacket> HandleFIN(
        const std::string& conn_key,
        uint32_t src_ip, uint16_t src_port,
        uint32_t dst_ip, uint16_t dst_port,
        uint32_t seq_num);

    std::optional<TCPPacket> HandleRST(const std::string& conn_key);

    void Log(const std::string& message);

    std::map<std::string, TCPConnectionInfo> connections_;
    mutable std::mutex connections_mutex_;
    LogCallback log_callback_;
};
