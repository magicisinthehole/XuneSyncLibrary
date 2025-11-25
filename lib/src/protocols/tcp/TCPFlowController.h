#pragma once

#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>

/**
 * FlowControlState
 *
 * Explicit state machine for TCP congestion control (RFC 5681).
 *
 * State Transition Diagram:
 *
 * INITIAL
 *   ↓ (Start transmission)
 * SLOW_START (cwnd grows exponentially)
 *   ↓ (cwnd >= ssthresh)
 * CONGESTION_AVOIDANCE (cwnd grows linearly)
 *   ↓ (3 duplicate ACKs)
 * FAST_RECOVERY_PENDING (need to send retransmit)
 *   ↓ (Retransmit sent)
 * FAST_RECOVERY (cwnd inflated, waiting for ACKs)
 *   ↓ (New ACK received)
 * CONGESTION_AVOIDANCE
 *   ↓ (All segments ACKed)
 * COMPLETE
 */
enum class FlowControlState : uint8_t {
    INITIAL = 0,                  // Not started
    SLOW_START = 1,               // Exponential window growth
    CONGESTION_AVOIDANCE = 2,     // Linear window growth
    FAST_RECOVERY_PENDING = 3,    // 3 dup ACKs, retransmit needed
    FAST_RECOVERY = 4,            // Retransmit sent, inflating cwnd
    COMPLETE = 5,                 // All data transmitted and ACKed
};

/**
 * Convert FlowControlState to string for debugging
 */
inline std::string FlowControlStateToString(FlowControlState state) {
    switch (state) {
        case FlowControlState::INITIAL:              return "INITIAL";
        case FlowControlState::SLOW_START:           return "SLOW_START";
        case FlowControlState::CONGESTION_AVOIDANCE: return "CONGESTION_AVOIDANCE";
        case FlowControlState::FAST_RECOVERY_PENDING:return "FAST_RECOVERY_PENDING";
        case FlowControlState::FAST_RECOVERY:        return "FAST_RECOVERY";
        case FlowControlState::COMPLETE:             return "COMPLETE";
        default:                                     return "UNKNOWN";
    }
}

/**
 * TCPFlowController
 *
 * Manages TCP congestion control following RFC 5681.
 *
 * Implements:
 * - Slow start (exponential cwnd growth)
 * - Congestion avoidance (linear cwnd growth)
 * - Fast retransmit (3 duplicate ACKs)
 * - Fast recovery (cwnd inflation)
 * - ABC (Appropriate Byte Counting, RFC 3465)
 *
 * This class is NOT thread-safe. Caller must provide synchronization.
 */
class TCPFlowController {
public:
    static constexpr size_t MSS = 1460;  // Maximum Segment Size (bytes)

    /**
     * Constructor
     * @param initial_cwnd Initial congestion window (bytes)
     * @param initial_ssthresh Initial slow start threshold (bytes)
     */
    TCPFlowController(size_t initial_cwnd = 3 * MSS, size_t initial_ssthresh = 65535);

    /**
     * Process ACK packet
     *
     * Updates flow control state based on ACK number and window size.
     *
     * @param ack_num ACK number from packet
     * @param window_size Receiver window size (bytes)
     * @return true if new data was ACKed, false if duplicate/window update
     */
    bool ProcessACK(uint32_t ack_num, uint16_t window_size);

    /**
     * Record segment transmission
     * Call this when a segment is sent to track bytes in flight.
     *
     * @param payload_size Payload size of segment (bytes)
     * @param seq_num Sequence number of segment
     */
    void RecordSegmentSent(size_t payload_size, uint32_t seq_num);

    /**
     * Set segment boundaries for ACK-to-segment mapping
     * Call this once before transmission to enable segment index calculation.
     *
     * @param base_seq Starting sequence number for this transmission
     * @param segment_sizes Vector of payload sizes for each segment
     */
    void SetSegmentBoundaries(uint32_t base_seq, const std::vector<size_t>& segment_sizes);

    /**
     * Check if fast retransmit is needed
     * @return true if 3 duplicate ACKs received
     */
    bool NeedsRetransmit() const;

    /**
     * Get segment index to retransmit
     * @return Segment index for retransmission
     */
    size_t GetRetransmitSegmentIndex() const;

    /**
     * Set retransmit segment index (after calculating from base_seq)
     * @param index Segment index to retransmit
     */
    void SetRetransmitSegmentIndex(size_t index);

    /**
     * Clear retransmit flag after handling retransmission
     */
    void ClearRetransmitFlag();

    /**
     * Mark transmission as complete
     */
    void SetComplete();

    /**
     * Check if transmission is complete
     * @return true if all data sent and ACKed
     */
    bool IsComplete() const;

    // State queries
    FlowControlState GetState() const { return state_; }
    size_t GetCongestionWindow() const { return cwnd_; }
    size_t GetSlowStartThreshold() const { return ssthresh_; }
    size_t GetBytesInFlight() const { return bytes_in_flight_; }
    uint32_t GetLastAckedSeq() const { return last_acked_seq_; }
    uint32_t GetDuplicateAckCount() const { return duplicate_ack_count_; }
    bool IsInFastRecovery() const {
        return state_ == FlowControlState::FAST_RECOVERY_PENDING ||
               state_ == FlowControlState::FAST_RECOVERY;
    }

private:
    /**
     * Enter slow start state
     */
    void EnterSlowStart();

    /**
     * Enter congestion avoidance state
     */
    void EnterCongestionAvoidance();

    /**
     * Enter fast recovery state
     */
    void EnterFastRecovery();

    /**
     * Exit fast recovery state
     */
    void ExitFastRecovery();

    /**
     * Grow cwnd during slow start
     * @param bytes_acked Number of bytes ACKed
     */
    void GrowSlowStart(size_t bytes_acked);

    /**
     * Grow cwnd during congestion avoidance (ABC algorithm)
     * @param bytes_acked Number of bytes ACKed
     */
    void GrowCongestionAvoidance(size_t bytes_acked);

    // State
    FlowControlState state_;

    // Congestion control
    size_t cwnd_;                       // Congestion window (bytes)
    size_t ssthresh_;                   // Slow start threshold (bytes)
    size_t bytes_acked_accumulator_;    // For ABC algorithm (congestion avoidance)

    // Flow control
    size_t bytes_in_flight_;            // Bytes sent but not ACKed
    // NOTE: receiver_window is NOT stored here - TCPConnectionInfo is the single source of truth

    // ACK tracking
    uint32_t last_acked_seq_;           // Last ACK number received
    uint32_t duplicate_ack_count_;      // Count of duplicate ACKs
    uint32_t last_window_right_edge_;   // ACK + window from last packet

    // Retransmission (segment index only - state tracks need via FAST_RECOVERY_PENDING)
    size_t retransmit_segment_index_;

    // Segment tracking (for ACK-to-segment mapping)
    uint32_t base_seq_;                        // Starting SEQ for this transmission
    std::vector<size_t> segment_payload_sizes_; // Payload size for each segment
    size_t next_segment_sent_;                 // Number of segments sent so far
};
