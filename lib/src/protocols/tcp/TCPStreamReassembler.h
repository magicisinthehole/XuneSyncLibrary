#pragma once

#include <mtp/ByteArray.h>
#include <cstdint>
#include <map>
#include <optional>
#include <functional>
#include <string>

/**
 * TCPStreamReassembler
 *
 * Handles TCP stream reassembly with proper out-of-order packet buffering.
 *
 * RFC 793 compliant behavior:
 * - Tracks next expected SEQ (contiguous sequence)
 * - Buffers out-of-order packets until gaps are filled
 * - Rejects TRUE retransmissions (duplicate of already-received data)
 * - Handles sequence number wraparound (uint32_t overflow)
 *
 * Example:
 *   next_expected_seq = 1000
 *   Receive SEQ=1010, 10 bytes → Buffer out-of-order
 *   Receive SEQ=1000, 10 bytes → Accept, fill gap, next_expected=1010
 *   Deliver buffered packet (SEQ=1010), next_expected=1020
 *   Receive SEQ=1000, 10 bytes → Reject (true retransmit)
 */
class TCPStreamReassembler {
public:
    using LogCallback = std::function<void(const std::string& message)>;

    /**
     * Constructor
     * @param initial_seq Initial sequence number (from SYN handshake)
     */
    explicit TCPStreamReassembler(uint32_t initial_seq);

    /**
     * Add a TCP segment to the reassembler
     *
     * @param seq Sequence number of this segment
     * @param data Payload data
     * @return true if data was accepted (new or filled gap), false if rejected (duplicate)
     */
    bool AddSegment(uint32_t seq, const mtp::ByteArray& data);

    /**
     * Get the contiguous data buffer (all in-order data received so far)
     * @return Reference to the reassembled data buffer
     */
    const mtp::ByteArray& GetBuffer() const { return contiguous_buffer_; }

    /**
     * Get next expected sequence number
     * @return The next contiguous sequence number we expect
     */
    uint32_t GetNextExpectedSeq() const { return next_expected_seq_; }

    /**
     * Check if there are buffered out-of-order segments
     * @return true if there are gaps in the stream
     */
    bool HasOutOfOrderData() const { return !out_of_order_buffer_.empty(); }

    /**
     * Get statistics for debugging
     */
    struct Stats {
        size_t segments_accepted = 0;      // New segments added
        size_t segments_rejected = 0;      // Duplicates rejected
        size_t out_of_order_buffered = 0;  // Currently buffered out-of-order
        size_t gaps_filled = 0;            // Times out-of-order data filled a gap
    };

    Stats GetStats() const { return stats_; }

    /**
     * Set logging callback
     */
    void SetLogCallback(LogCallback callback) { log_callback_ = callback; }

    /**
     * Clear all buffers and reset to initial state
     */
    void Reset(uint32_t new_initial_seq);

    /**
     * Clear the contiguous buffer (used after HTTP request is processed)
     * Note: This does NOT clear out-of-order buffered data
     */
    void ClearContiguousBuffer();

    /**
     * Erase processed data from the front of the contiguous buffer
     * @param num_bytes Number of bytes to remove from the front
     */
    void EraseProcessedBytes(size_t num_bytes);

private:
    /**
     * Check if SEQ is before a reference SEQ (handles wraparound)
     * @param seq Sequence number to check
     * @param ref_seq Reference sequence number
     * @return true if seq is before ref_seq (considering wraparound)
     */
    static bool IsSeqBefore(uint32_t seq, uint32_t ref_seq);

    /**
     * Check if SEQ is in range [start, end) (handles wraparound)
     */
    static bool IsSeqInRange(uint32_t seq, uint32_t start, uint32_t end);

    /**
     * Try to flush buffered out-of-order data to contiguous buffer
     * Called after adding new data to see if gaps can be filled
     */
    void TryFlushOutOfOrderData();

    /**
     * Log a message via callback if set
     */
    void Log(const std::string& message);

    // Next contiguous sequence number we expect
    uint32_t next_expected_seq_;

    // Contiguous data received in order (ready for application)
    mtp::ByteArray contiguous_buffer_;

    // Out-of-order segments buffered until gaps are filled
    // Key: starting sequence number, Value: data
    std::map<uint32_t, mtp::ByteArray> out_of_order_buffer_;

    // Statistics
    Stats stats_;

    // Logging callback
    LogCallback log_callback_;
};
