#include "TCPStreamReassembler.h"
#include <sstream>
#include <iomanip>

TCPStreamReassembler::TCPStreamReassembler(uint32_t initial_seq)
    : next_expected_seq_(initial_seq) {
}

bool TCPStreamReassembler::AddSegment(uint32_t seq, const mtp::ByteArray& data) {
    if (data.empty()) {
        return true;  // Empty segment is always "accepted" (no-op)
    }

    uint32_t seg_end = seq + data.size();

    // Case 1: Segment starts BEFORE next_expected_seq (overlap or retransmit)
    if (IsSeqBefore(seq, next_expected_seq_)) {
        // Check if this is entirely before next_expected (true retransmit)
        if (IsSeqBefore(seg_end, next_expected_seq_) || seg_end == next_expected_seq_) {
            std::ostringstream oss;
            oss << "TCP retransmit: SEQ=" << seq
                << " end=" << seg_end
                << " < expected=" << next_expected_seq_
                << " (" << data.size() << " bytes, duplicate)";
            Log(oss.str());
            stats_.segments_rejected++;
            return false;  // Reject: duplicate data
        }

        // Partial overlap: extract new data past next_expected_seq
        uint32_t overlap_bytes = next_expected_seq_ - seq;
        if (overlap_bytes >= data.size()) {
            // Should not happen (already handled above), but be defensive
            stats_.segments_rejected++;
            return false;
        }

        std::ostringstream oss;
        oss << "TCP partial retransmit: SEQ=" << seq
            << " overlaps by " << overlap_bytes << " bytes, accepting " << (data.size() - overlap_bytes) << " new bytes";
        Log(oss.str());

        // Create new segment with only the non-overlapping data
        mtp::ByteArray new_data(data.begin() + overlap_bytes, data.end());
        return AddSegment(next_expected_seq_, new_data);  // Recursively add non-overlapping part
    }

    // Case 2: Segment starts exactly at next_expected_seq (in-order)
    if (seq == next_expected_seq_) {
        // Append to contiguous buffer
        contiguous_buffer_.insert(contiguous_buffer_.end(), data.begin(), data.end());
        next_expected_seq_ = seg_end;
        stats_.segments_accepted++;

        std::ostringstream oss;
        oss << "TCP in-order: SEQ=" << seq
            << " (" << data.size() << " bytes), next_expected=" << next_expected_seq_;
        Log(oss.str());

        // Try to flush any buffered out-of-order data
        TryFlushOutOfOrderData();
        return true;
    }

    // Case 3: Segment starts AFTER next_expected_seq (out-of-order, gap exists)
    // Check if we already have this data buffered
    for (const auto& [buffered_seq, buffered_data] : out_of_order_buffer_) {
        uint32_t buffered_end = buffered_seq + buffered_data.size();

        // Check if new segment overlaps with buffered segment
        if (IsSeqInRange(seq, buffered_seq, buffered_end) ||
            IsSeqInRange(buffered_seq, seq, seg_end)) {
            // Overlap detected - for now, reject (could merge in future)
            std::ostringstream oss;
            oss << "TCP out-of-order overlap: SEQ=" << seq
                << " overlaps with buffered SEQ=" << buffered_seq
                << ", rejecting";
            Log(oss.str());
            stats_.segments_rejected++;
            return false;
        }
    }

    // Buffer this out-of-order segment
    out_of_order_buffer_[seq] = data;
    stats_.segments_accepted++;

    std::ostringstream oss;
    oss << "TCP out-of-order: SEQ=" << seq
        << " (" << data.size() << " bytes), gap of " << (seq - next_expected_seq_)
        << " bytes, buffered (total buffered: " << out_of_order_buffer_.size() << ")";
    Log(oss.str());

    stats_.out_of_order_buffered = out_of_order_buffer_.size();
    return true;
}

void TCPStreamReassembler::TryFlushOutOfOrderData() {
    // Repeatedly try to flush segments that now start at next_expected_seq
    while (!out_of_order_buffer_.empty()) {
        auto it = out_of_order_buffer_.find(next_expected_seq_);
        if (it == out_of_order_buffer_.end()) {
            break;  // No segment starts at next_expected_seq, gap still exists
        }

        // Found a buffered segment that fills the gap!
        const mtp::ByteArray& data = it->second;
        contiguous_buffer_.insert(contiguous_buffer_.end(), data.begin(), data.end());
        next_expected_seq_ += data.size();
        stats_.gaps_filled++;

        std::ostringstream oss;
        oss << "TCP gap filled: flushed " << data.size()
            << " bytes from buffer, next_expected=" << next_expected_seq_;
        Log(oss.str());

        out_of_order_buffer_.erase(it);
    }

    stats_.out_of_order_buffered = out_of_order_buffer_.size();
}

bool TCPStreamReassembler::IsSeqBefore(uint32_t seq, uint32_t ref_seq) {
    // Handle wraparound: sequence numbers wrap at 2^32
    // Use signed comparison: if (seq - ref_seq) is negative, seq is before ref_seq
    return static_cast<int32_t>(seq - ref_seq) < 0;
}

bool TCPStreamReassembler::IsSeqInRange(uint32_t seq, uint32_t start, uint32_t end) {
    // Check if seq is in range [start, end) considering wraparound
    // seq is in range if: start <= seq < end (with wraparound handling)
    if (start <= end) {
        // Normal case (no wraparound)
        return seq >= start && seq < end;
    } else {
        // Wraparound case
        return seq >= start || seq < end;
    }
}

void TCPStreamReassembler::Reset(uint32_t new_initial_seq) {
    next_expected_seq_ = new_initial_seq;
    contiguous_buffer_.clear();
    out_of_order_buffer_.clear();
    stats_ = Stats();
}

void TCPStreamReassembler::ClearContiguousBuffer() {
    contiguous_buffer_.clear();
    // Note: next_expected_seq stays the same - we're just clearing processed data
    // Out-of-order buffer remains intact
}

void TCPStreamReassembler::EraseProcessedBytes(size_t num_bytes) {
    if (num_bytes >= contiguous_buffer_.size()) {
        contiguous_buffer_.clear();
    } else {
        contiguous_buffer_.erase(contiguous_buffer_.begin(),
                                 contiguous_buffer_.begin() + num_bytes);
    }
}

void TCPStreamReassembler::Log(const std::string& message) {
    if (log_callback_) {
        log_callback_(message);
    }
}
