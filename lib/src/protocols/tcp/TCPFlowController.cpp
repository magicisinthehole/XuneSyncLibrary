#include "TCPFlowController.h"
#include <algorithm>

TCPFlowController::TCPFlowController(size_t initial_cwnd, size_t initial_ssthresh)
    : state_(FlowControlState::INITIAL),
      cwnd_(initial_cwnd),
      ssthresh_(initial_ssthresh),
      bytes_acked_accumulator_(0),
      bytes_in_flight_(0),
      last_acked_seq_(0),
      duplicate_ack_count_(0),
      last_window_right_edge_(0),
      retransmit_segment_index_(0),
      base_seq_(0),
      next_segment_sent_(0) {
}

bool TCPFlowController::ProcessACK(uint32_t ack_num, uint16_t window_size) {
    // Start in slow start if this is the first ACK
    if (state_ == FlowControlState::INITIAL) {
        EnterSlowStart();
    }

    // Calculate window right edge (ACK + window)
    // Use stored last_window_right_edge_ instead of computing from receiver_window_
    // (TCPConnectionInfo is the single source of truth for receiver_window)
    uint32_t old_right_edge = last_window_right_edge_;
    uint32_t new_right_edge = ack_num + window_size;
    bool window_edge_unchanged = (new_right_edge == old_right_edge);
    bool has_unacked_data = (bytes_in_flight_ > 0);

    // Detect duplicate ACK (ACK doesn't advance + window edge unchanged + has unacked data)
    bool is_duplicate_ack = (ack_num == last_acked_seq_) &&
                            window_edge_unchanged &&
                            has_unacked_data;

    // Detect window update (ACK doesn't advance but window opens)
    bool is_window_update = (ack_num == last_acked_seq_) && !window_edge_unchanged;

    // Handle duplicate ACK
    if (is_duplicate_ack) {
        duplicate_ack_count_++;

        // Fast retransmit on 3rd duplicate ACK (only if not already in recovery)
        if (duplicate_ack_count_ == 3 && !IsInFastRecovery()) {
            EnterFastRecovery();  // Sets state to FAST_RECOVERY_PENDING
            // NOTE: Segment index calculation is done per-transmission in
            // TCPConnectionInfo::ProcessACK, not here. The flow controller
            // doesn't know which transmission the ACK belongs to.
            return false;  // No new data ACKed
        }

        // RFC 5681: Additional duplicate ACKs inflate cwnd during fast recovery
        if (duplicate_ack_count_ > 3 && IsInFastRecovery()) {
            cwnd_ += MSS;
        }

        return false;  // No new data ACKed
    }

    // Process new ACK or window update
    if (ack_num > last_acked_seq_ || is_window_update) {
        // Calculate bytes ACKed (0 for pure window updates)
        uint32_t bytes_acked = (ack_num > last_acked_seq_) ? (ack_num - last_acked_seq_) : 0;

        // Reset duplicate ACK counter when ACK advances
        if (ack_num > last_acked_seq_) {
            duplicate_ack_count_ = 0;

            // Exit fast recovery on new ACK (handles both PENDING and active recovery)
            if (IsInFastRecovery()) {
                ExitFastRecovery();
            }
        }

        // Update state
        last_acked_seq_ = ack_num;
        last_window_right_edge_ = new_right_edge;

        // Reduce bytes in flight
        if (bytes_acked > 0) {
            if (bytes_in_flight_ >= bytes_acked) {
                bytes_in_flight_ -= bytes_acked;
            } else {
                bytes_in_flight_ = 0;
            }

            // Grow cwnd if not in fast recovery
            if (!IsInFastRecovery()) {
                if (state_ == FlowControlState::SLOW_START || cwnd_ < ssthresh_) {
                    GrowSlowStart(bytes_acked);
                } else {
                    GrowCongestionAvoidance(bytes_acked);
                }
            }
        }

        return (bytes_acked > 0);  // Return true if new data was ACKed
    }

    return false;  // No change
}

void TCPFlowController::RecordSegmentSent(size_t payload_size, uint32_t seq_num) {
    bytes_in_flight_ += payload_size;
}

void TCPFlowController::SetSegmentBoundaries(uint32_t base_seq, const std::vector<size_t>& segment_sizes) {
    base_seq_ = base_seq;
    segment_payload_sizes_ = segment_sizes;
    next_segment_sent_ = 0;

    // Initialize last_acked_seq to base_seq so first ACK calculates bytes_acked correctly
    // Without this, first ACK would be: ack_num - 0 = huge number, causing incorrect flow
    // Only set if this is the first transmission (last_acked_seq still at 0) or if
    // the new base_seq is beyond what we've seen (normal progression)
    if (last_acked_seq_ == 0 || base_seq > last_acked_seq_) {
        last_acked_seq_ = base_seq;
    }
}

// NOTE: CalculateSegmentsToSend, GetEffectiveWindow, IsWindowFull were removed.
// TCPConnectionInfo::CalculateSegmentsToSend and GetAvailableWindow are the canonical implementations.
// receiver_window is now stored only in TCPConnectionInfo (single source of truth).

bool TCPFlowController::NeedsRetransmit() const {
    return state_ == FlowControlState::FAST_RECOVERY_PENDING;
}

size_t TCPFlowController::GetRetransmitSegmentIndex() const {
    return retransmit_segment_index_;
}

void TCPFlowController::SetRetransmitSegmentIndex(size_t index) {
    retransmit_segment_index_ = index;
}

void TCPFlowController::ClearRetransmitFlag() {
    // Transition from PENDING to active recovery after retransmit is sent
    if (state_ == FlowControlState::FAST_RECOVERY_PENDING) {
        state_ = FlowControlState::FAST_RECOVERY;
    }
}

void TCPFlowController::SetComplete() {
    state_ = FlowControlState::COMPLETE;
}

bool TCPFlowController::IsComplete() const {
    return state_ == FlowControlState::COMPLETE;
}

void TCPFlowController::EnterSlowStart() {
    state_ = FlowControlState::SLOW_START;
}

void TCPFlowController::EnterCongestionAvoidance() {
    state_ = FlowControlState::CONGESTION_AVOIDANCE;
}

void TCPFlowController::EnterFastRecovery() {
    // Enter pending state - retransmit needed before transitioning to active recovery
    state_ = FlowControlState::FAST_RECOVERY_PENDING;

    // RFC 5681: Set ssthresh = max(FlightSize/2, 2*MSS)
    ssthresh_ = std::max(bytes_in_flight_ / 2, 2 * MSS);

    // Inflate cwnd (accounts for 3 buffered segments at receiver)
    cwnd_ = ssthresh_ + 3 * MSS;
}

void TCPFlowController::ExitFastRecovery() {
    // RFC 5681: Deflate cwnd to ssthresh when exiting fast recovery
    // BUT: Don't set cwnd below bytes_in_flight - this would create a deadlock
    // where we can't send anything until the pipe drains (which may never happen
    // if the receiver is waiting for more data).
    size_t min_cwnd = std::max(ssthresh_, bytes_in_flight_);
    if (cwnd_ > min_cwnd) {
        cwnd_ = min_cwnd;
    }

    // Transition to congestion avoidance
    EnterCongestionAvoidance();
}

void TCPFlowController::GrowSlowStart(size_t bytes_acked) {
    // RFC 3465: Slow start increases cwnd by min(bytes_acked, 2*MSS)
    size_t increase = std::min(bytes_acked, 2 * MSS);
    cwnd_ += increase;

    // Transition to congestion avoidance when cwnd >= ssthresh
    if (cwnd_ >= ssthresh_) {
        EnterCongestionAvoidance();
    }
}

void TCPFlowController::GrowCongestionAvoidance(size_t bytes_acked) {
    // RFC 3465: Appropriate Byte Counting (ABC)
    // Accumulate bytes, grow by 1 MSS per cwnd bytes ACKed
    bytes_acked_accumulator_ += bytes_acked;

    if (bytes_acked_accumulator_ >= cwnd_) {
        bytes_acked_accumulator_ -= cwnd_;
        cwnd_ += MSS;
    }
}
