#include "TCPConnectionManager.h"
#include "../ppp/PPPParser.h"  // Contains TCPParser, IPParser definitions
#include <sstream>
#include <iomanip>
#include <random>
#include <algorithm>

// ============================================================================
// TCPConnectionInfo - State Machine Implementation
// ============================================================================

bool TCPConnectionInfo::TransitionTo(TCPState new_state) {
    if (!CanTransitionTo(new_state)) {
        return false;
    }

    if (log_callback && *log_callback) {
        TCPState old_state = state;
        (*log_callback)("TCP state transition: " + TCPStateToString(old_state) +
                       " -> " + TCPStateToString(new_state));
    }

    state = new_state;
    return true;
}

bool TCPConnectionInfo::CanTransitionTo(TCPState new_state) const {
    switch (state) {
        case TCPState::CLOSED:
            return (new_state == TCPState::SYN_RECEIVED ||
                    new_state == TCPState::SYN_SENT ||
                    new_state == TCPState::LISTEN);

        case TCPState::SYN_RECEIVED:
            return (new_state == TCPState::ESTABLISHED ||
                    new_state == TCPState::CLOSED);

        case TCPState::ESTABLISHED:
            return (new_state == TCPState::FIN_WAIT_1 ||
                    new_state == TCPState::CLOSE_WAIT ||
                    new_state == TCPState::CLOSED);

        case TCPState::CLOSE_WAIT:
            return (new_state == TCPState::LAST_ACK ||
                    new_state == TCPState::CLOSED);

        case TCPState::LAST_ACK:
            return (new_state == TCPState::CLOSED);

        case TCPState::FIN_WAIT_1:
            return (new_state == TCPState::FIN_WAIT_2 ||
                    new_state == TCPState::CLOSING ||
                    new_state == TCPState::TIME_WAIT ||
                    new_state == TCPState::CLOSED);

        case TCPState::FIN_WAIT_2:
            return (new_state == TCPState::TIME_WAIT ||
                    new_state == TCPState::CLOSED);

        case TCPState::TIME_WAIT:
            return (new_state == TCPState::CLOSED);

        default:
            return false;
    }
}

// ============================================================================
// TCPConnectionInfo - RTO Implementation
// ============================================================================

void TCPConnectionInfo::RecordSentSegment(uint32_t seq_start, const mtp::ByteArray& data, bool is_retransmit) {
    SentSegment segment;
    segment.seq_start = seq_start;
    segment.seq_end = seq_start + data.size();
    segment.send_time = std::chrono::steady_clock::now();
    segment.is_retransmit = is_retransmit;
    segment.data = data;

    unacked_segments[seq_start] = segment;
}

void TCPConnectionInfo::ProcessACKForRTO(uint32_t ack_num) {
    auto now = std::chrono::steady_clock::now();
    bool found_non_retransmit = false;
    std::chrono::milliseconds rtt{0};

    auto it = unacked_segments.begin();
    while (it != unacked_segments.end()) {
        const SentSegment& segment = it->second;

        if (segment.seq_end <= ack_num) {
            if (!segment.is_retransmit && !found_non_retransmit) {
                auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                    now - segment.send_time);
                rtt = elapsed;
                found_non_retransmit = true;
            }
            it = unacked_segments.erase(it);
        } else {
            ++it;
        }
    }

    if (found_non_retransmit) {
        rto_manager.UpdateRTT(rtt);

        if (log_callback && *log_callback) {
            std::ostringstream oss;
            oss << "RTT sample: " << rtt.count() << "ms, RTO=" << rto_manager.GetRTO().count() << "ms";
            if (auto srtt = rto_manager.GetSRTT()) {
                oss << ", SRTT=" << srtt->count() << "ms";
            }
            (*log_callback)(oss.str());
        }
    }
}

std::vector<SentSegment> TCPConnectionInfo::CheckTimeouts() {
    std::vector<SentSegment> timed_out;
    auto now = std::chrono::steady_clock::now();
    auto rto = rto_manager.GetRTO();

    for (const auto& [seq, segment] : unacked_segments) {
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            now - segment.send_time);

        if (elapsed >= rto) {
            timed_out.push_back(segment);

            if (log_callback && *log_callback) {
                std::ostringstream oss;
                oss << "Segment timeout: SEQ=" << segment.seq_start
                    << " size=" << segment.data.size()
                    << " elapsed=" << elapsed.count() << "ms"
                    << " RTO=" << rto.count() << "ms";
                (*log_callback)(oss.str());
            }
        }
    }

    if (!timed_out.empty()) {
        rto_manager.OnRetransmit();
    }

    return timed_out;
}

// ============================================================================
// TCPConnectionInfo - Flow Control Implementation
// ============================================================================

void TCPConnectionInfo::InitializeFlowControl() {
    // Congestion control tuned from capture analysis of official Zune software:
    // - Official software sends max 4 segments per USB transfer (50% of cases)
    // - Max TCP payload observed: 5888 bytes = 4 * 1472
    //
    // cwnd = 1 segment: Start conservative, grow via ACK feedback
    // ssthresh = 4 segments: Matches observed device capacity limit
    //   - Slow start runs: 1 → 2 → 4 (hits ssthresh)
    //   - Then congestion avoidance: linear growth (+1 seg/RTT)
    flow_controller = std::make_unique<TCPFlowController>(
        1 * TCPFlowController::MSS,  // initial_cwnd: 1 segment
        4 * TCPFlowController::MSS   // initial_ssthresh: 4 segments (from capture)
    );
}

bool TCPConnectionInfo::ProcessACK(uint32_t ack_num, uint16_t window_size) {
    if (!flow_controller) {
        InitializeFlowControl();
    }

    // Update receiver window
    receiver_window = window_size;

    // Delegate to flow controller - it handles:
    // - Duplicate ACK detection (window edge check, bytes_in_flight > 0)
    // - Congestion window updates (slow start / congestion avoidance)
    // - Fast retransmit trigger (3 duplicate ACKs)
    bool new_data_acked = flow_controller->ProcessACK(ack_num, window_size);

    // Also process for RTO tracking
    ProcessACKForRTO(ack_num);

    // Check for fast retransmit trigger
    if (flow_controller->NeedsRetransmit()) {
        // Find which transmission this ACK belongs to and calculate segment index
        // using that transmission's segment boundaries (not the flow controller's
        // which might be from a different transmission)
        std::lock_guard<std::mutex> lock(transmissions_mutex);
        for (auto& [base_seq, trans] : active_transmissions) {
            // Check if ACK falls within this transmission's range
            uint32_t trans_end = base_seq;
            for (size_t payload : trans.segment_payload_sizes) {
                trans_end += payload;
            }

            // ACK must be within or at the end of this transmission's range
            if (ack_num > base_seq && ack_num <= trans_end) {
                // Found the transmission - calculate which segment to retransmit
                // The ACK indicates data received up to ack_num, so the lost segment
                // is the one starting at ack_num
                uint32_t expected_seq = base_seq;
                for (size_t i = 0; i < trans.segment_payload_sizes.size(); i++) {
                    uint32_t segment_end = expected_seq + trans.segment_payload_sizes[i];
                    if (segment_end == ack_num && (i + 1) < trans.segment_payload_sizes.size()) {
                        // Found: ACK acknowledges up to segment i, so segment i+1 is lost
                        trans.state = TransmissionState::NEEDS_RETRANSMIT;
                        trans.retransmit_segment_index = i + 1;
                        break;
                    }
                    expected_seq = segment_end;
                }
                break;  // Only process one transmission per ACK
            }
        }
    }

    return new_data_acked;
}

void TCPConnectionInfo::RecordBytesSent(size_t payload_size, uint32_t seq_num) {
    if (!flow_controller) {
        InitializeFlowControl();
    }
    flow_controller->RecordSegmentSent(payload_size, seq_num);
}

size_t TCPConnectionInfo::GetAvailableWindow() const {
    // Standard TCP: effective_window = min(cwnd, receiver_window)
    //
    // - receiver_window: total buffer capacity the device advertises (32KB)
    // - cwnd: congestion window that grows based on ACK feedback
    //
    // Even on USB, cwnd provides essential ACK-clocked pacing:
    // 1. Initial cwnd is small (3 segments = ~4KB)
    // 2. Each successful ACK grows cwnd (slow start, then linear)
    // 3. Duplicate ACKs shrink cwnd (fast recovery)
    //
    // This lets ACK feedback naturally control our send rate.
    // No hardcoded limits - the device's ACKs ARE the signal.

    size_t bytes_in_flight = flow_controller ? flow_controller->GetBytesInFlight() : 0;
    size_t cwnd = flow_controller ? flow_controller->GetCongestionWindow() : receiver_window;

    // Effective window is the minimum of cwnd and receiver_window
    size_t effective_window = std::min(cwnd, static_cast<size_t>(receiver_window));

    if (bytes_in_flight >= effective_window) {
        return 0;
    }
    return effective_window - bytes_in_flight;
}

bool TCPConnectionInfo::NeedsFastRetransmit() const {
    if (!flow_controller) return false;
    return flow_controller->NeedsRetransmit();
}

size_t TCPConnectionInfo::GetRetransmitSegmentIndex(uint32_t base_seq) const {
    std::lock_guard<std::mutex> lock(const_cast<std::mutex&>(transmissions_mutex));
    auto it = active_transmissions.find(base_seq);
    if (it != active_transmissions.end()) {
        return it->second.retransmit_segment_index;
    }
    return 0;
}

void TCPConnectionInfo::ClearRetransmitFlag() {
    if (flow_controller) {
        flow_controller->ClearRetransmitFlag();
    }

    std::lock_guard<std::mutex> lock(transmissions_mutex);
    for (auto& [base_seq, trans] : active_transmissions) {
        if (trans.state == TransmissionState::NEEDS_RETRANSMIT) {
            // If all segments already sent, go to AWAITING_ACKS, not IN_PROGRESS
            if (trans.next_segment_index >= trans.queued_segments.size()) {
                trans.state = TransmissionState::AWAITING_ACKS;
            } else {
                trans.state = TransmissionState::IN_PROGRESS;
            }
        }
    }
}

// ============================================================================
// TCPConnectionInfo - HTTP Transmission Implementation
// ============================================================================

void TCPConnectionInfo::StartTransmission(uint32_t base_seq,
                                          std::vector<mtp::ByteArray> segments,
                                          std::vector<size_t> payload_sizes) {
    std::lock_guard<std::mutex> lock(transmissions_mutex);

    HTTPTransmission trans;
    trans.base_seq = base_seq;
    trans.queued_segments = std::move(segments);
    trans.segment_payload_sizes = std::move(payload_sizes);
    trans.next_segment_index = 0;
    trans.state = TransmissionState::PENDING;
    trans.last_ack_time = std::chrono::steady_clock::now();

    // Configure flow controller with segment boundaries
    if (flow_controller) {
        flow_controller->SetSegmentBoundaries(base_seq, trans.segment_payload_sizes);
    }

    active_transmissions[base_seq] = std::move(trans);
}

HTTPTransmission* TCPConnectionInfo::GetTransmissionForACK(uint32_t ack_num) {
    std::lock_guard<std::mutex> lock(transmissions_mutex);

    for (auto& [base_seq, trans] : active_transmissions) {
        // Calculate the sequence range this transmission covers
        uint32_t end_seq = base_seq;
        for (size_t payload : trans.segment_payload_sizes) {
            end_seq += payload;
        }

        // ACK is within this transmission's range
        if (ack_num > base_seq && ack_num <= end_seq) {
            return &trans;
        }
    }
    return nullptr;
}

size_t TCPConnectionInfo::CalculateSegmentsToSend(const HTTPTransmission& trans, size_t max_batch) const {
    size_t available_window = GetAvailableWindow();
    if (available_window == 0) {
        return 0;
    }

    size_t segments_to_send = 0;
    size_t bytes_to_send = 0;

    for (size_t i = trans.next_segment_index;
         i < trans.queued_segments.size() && segments_to_send < max_batch;
         i++) {
        size_t payload_size = trans.segment_payload_sizes[i];

        if (bytes_to_send + payload_size <= available_window) {
            segments_to_send++;
            bytes_to_send += payload_size;
        } else {
            break;
        }
    }

    return segments_to_send;
}

void TCPConnectionInfo::MarkSegmentsSent(uint32_t base_seq, size_t num_segments, size_t bytes_sent) {
    std::lock_guard<std::mutex> lock(transmissions_mutex);

    auto it = active_transmissions.find(base_seq);
    if (it == active_transmissions.end()) return;

    HTTPTransmission& trans = it->second;

    // Calculate starting sequence for these segments
    uint32_t current_seq = base_seq;
    for (size_t i = 0; i < trans.next_segment_index; i++) {
        current_seq += trans.segment_payload_sizes[i];
    }

    // Record each segment with flow controller
    for (size_t i = 0; i < num_segments; i++) {
        size_t idx = trans.next_segment_index + i;
        if (idx < trans.segment_payload_sizes.size()) {
            size_t payload = trans.segment_payload_sizes[idx];
            RecordBytesSent(payload, current_seq);
            current_seq += payload;
        }
    }

    trans.next_segment_index += num_segments;
}

bool TCPConnectionInfo::UpdateTransmissionAfterACK(HTTPTransmission& trans, uint32_t bytes_acked) {
    trans.last_ack_time = std::chrono::steady_clock::now();

    // All segments sent - check if all ACKed
    if (trans.next_segment_index >= trans.queued_segments.size()) {
        trans.state = TransmissionState::COMPLETE;
        return true;
    }

    // Still sending segments
    if (trans.state == TransmissionState::PENDING) {
        trans.state = TransmissionState::IN_PROGRESS;
    }

    return false;
}

void TCPConnectionInfo::RemoveTransmission(uint32_t base_seq) {
    std::lock_guard<std::mutex> lock(transmissions_mutex);
    active_transmissions.erase(base_seq);
}

size_t TCPConnectionInfo::GetBytesInFlight() const {
    if (!flow_controller) return 0;
    return flow_controller->GetBytesInFlight();
}

// ============================================================================
// TCPConnectionManager Implementation
// ============================================================================

std::optional<TCPPacket> TCPConnectionManager::HandlePacket(
    uint32_t src_ip, uint16_t src_port,
    uint32_t dst_ip, uint16_t dst_port,
    uint32_t seq_num, uint32_t ack_num,
    uint8_t flags, uint16_t window_size,
    const mtp::ByteArray& payload) {

    std::string conn_key = MakeConnectionKey(src_ip, src_port, dst_ip, dst_port);

    // Priority order: RST > SYN > FIN > ACK
    if (flags & TCPParser::TCP_FLAG_RST) {
        return HandleRST(conn_key);
    }

    if (flags & TCPParser::TCP_FLAG_SYN) {
        if (!(flags & TCPParser::TCP_FLAG_ACK)) {
            return HandleSYN(conn_key, src_ip, src_port, dst_ip, dst_port, seq_num, window_size);
        }
        Log("SYN-ACK received from device");
        return std::nullopt;
    }

    if (flags & TCPParser::TCP_FLAG_FIN) {
        return HandleFIN(conn_key, src_ip, src_port, dst_ip, dst_port, seq_num);
    }

    if (flags & TCPParser::TCP_FLAG_ACK) {
        return HandleACK(conn_key, src_ip, src_port, dst_ip, dst_port, seq_num, ack_num, payload);
    }

    return std::nullopt;
}

std::optional<TCPPacket> TCPConnectionManager::HandleSYN(
    const std::string& conn_key,
    uint32_t src_ip, uint16_t src_port,
    uint32_t dst_ip, uint16_t dst_port,
    uint32_t seq_num, uint16_t window_size) {

    std::lock_guard<std::mutex> lock(connections_mutex_);

    TCPConnectionInfo& conn = connections_[conn_key];
    conn.log_callback = &log_callback_;

    if (!conn.TransitionTo(TCPState::SYN_RECEIVED)) {
        Log("Invalid state transition for SYN: " + TCPStateToString(conn.state) +
            " -> SYN_RECEIVED");
        return std::nullopt;
    }

    // Initialize stream reassembler
    conn.reassembler = std::make_unique<TCPStreamReassembler>(seq_num + 1);
    if (log_callback_) {
        conn.reassembler->SetLogCallback(log_callback_);
    }

    // Initialize flow controller for this connection
    conn.InitializeFlowControl();

    // CRITICAL: Set the receiver window from the SYN packet
    // This is the client's advertised window and must be respected to avoid buffer overflow!
    // TCPConnectionInfo::receiver_window is the SINGLE SOURCE OF TRUTH for receiver window.
    // TCPFlowController no longer stores its own copy (architectural cleanup).
    conn.receiver_window = window_size;
    Log("SYN window_size=" + std::to_string(window_size) + " - receiver window set");

    // Generate random ISN
    std::random_device rd;
    std::uniform_int_distribution<uint32_t> dist(0, 0xFFFFFFFF);
    conn.seq_num = dist(rd);
    conn.ack_num = seq_num + 1;

    Log("SYN received, sending SYN-ACK (state: " + TCPStateToString(conn.state) + ")");

    TCPPacket response;
    response.src_ip = dst_ip;
    response.src_port = dst_port;
    response.dst_ip = src_ip;
    response.dst_port = src_port;
    response.seq_num = conn.seq_num;
    response.ack_num = conn.ack_num;
    response.flags = TCPParser::TCP_FLAG_SYN | TCPParser::TCP_FLAG_ACK;

    return response;
}

std::optional<TCPPacket> TCPConnectionManager::HandleACK(
    const std::string& conn_key,
    uint32_t src_ip, uint16_t src_port,
    uint32_t dst_ip, uint16_t dst_port,
    uint32_t seq_num, uint32_t ack_num,
    const mtp::ByteArray& payload) {

    std::lock_guard<std::mutex> lock(connections_mutex_);

    auto it = connections_.find(conn_key);
    if (it == connections_.end()) {
        return std::nullopt;
    }

    TCPConnectionInfo& conn = it->second;
    conn.log_callback = &log_callback_;

    // Handle final ACK of handshake
    if (conn.state == TCPState::SYN_RECEIVED) {
        if (!conn.TransitionTo(TCPState::ESTABLISHED)) {
            Log("Invalid state transition for ACK: " + TCPStateToString(conn.state) +
                " -> ESTABLISHED");
            return std::nullopt;
        }

        conn.seq_num++;
        Log("TCP connection established (state: " + TCPStateToString(conn.state) + ")");
        return std::nullopt;
    }

    // Handle ACK in ESTABLISHED state
    if (conn.state == TCPState::ESTABLISHED) {
        if (!payload.empty()) {
            if (!conn.reassembler) {
                conn.reassembler = std::make_unique<TCPStreamReassembler>(seq_num);
                if (log_callback_) {
                    conn.reassembler->SetLogCallback(log_callback_);
                }
            }

            bool accepted = conn.reassembler->AddSegment(seq_num, payload);
            if (!accepted) {
                return std::nullopt;
            }

            conn.ack_num = conn.reassembler->GetNextExpectedSeq();
        }

        return std::nullopt;
    }

    // Handle ACK in LAST_ACK state
    if (conn.state == TCPState::LAST_ACK) {
        if (!conn.TransitionTo(TCPState::CLOSED)) {
            Log("Invalid state transition for ACK: " + TCPStateToString(conn.state) +
                " -> CLOSED");
            return std::nullopt;
        }

        Log("TCP connection closed (state: " + TCPStateToString(conn.state) + ")");
        conn.reassembler.reset();
        return std::nullopt;
    }

    return std::nullopt;
}

std::optional<TCPPacket> TCPConnectionManager::HandleFIN(
    const std::string& conn_key,
    uint32_t src_ip, uint16_t src_port,
    uint32_t dst_ip, uint16_t dst_port,
    uint32_t seq_num) {

    std::lock_guard<std::mutex> lock(connections_mutex_);

    auto it = connections_.find(conn_key);
    if (it == connections_.end()) {
        Log("FIN received for non-existent connection - ignoring");
        return std::nullopt;
    }

    TCPConnectionInfo& conn = it->second;
    conn.log_callback = &log_callback_;

    Log("FIN received (current state: " + TCPStateToString(conn.state) + ")");

    if (conn.state == TCPState::CLOSED) {
        Log("FIN received on CLOSED connection - ignoring");
        return std::nullopt;
    }

    if (!conn.TransitionTo(TCPState::CLOSE_WAIT)) {
        Log("Invalid state transition for FIN: " + TCPStateToString(conn.state) +
            " -> CLOSE_WAIT");
        return std::nullopt;
    }

    conn.reassembler.reset();
    conn.ack_num = seq_num + 1;

    Log("Sending ACK for FIN (state: " + TCPStateToString(conn.state) + ")");

    TCPPacket response;
    response.src_ip = dst_ip;
    response.src_port = dst_port;
    response.dst_ip = src_ip;
    response.dst_port = src_port;
    response.seq_num = conn.seq_num;
    response.ack_num = conn.ack_num;
    response.flags = TCPParser::TCP_FLAG_ACK;

    return response;
}

std::optional<TCPPacket> TCPConnectionManager::HandleRST(const std::string& conn_key) {
    std::lock_guard<std::mutex> lock(connections_mutex_);

    auto it = connections_.find(conn_key);
    if (it != connections_.end()) {
        TCPConnectionInfo& conn = it->second;
        conn.log_callback = &log_callback_;

        Log("RST received, closing connection (current state: " +
            TCPStateToString(conn.state) + ")");

        conn.TransitionTo(TCPState::CLOSED);
        conn.reassembler.reset();
        connections_.erase(it);
    }

    return std::nullopt;
}

// ============================================================================
// TCPConnectionManager - HTTP Transmission Methods
// ============================================================================

uint32_t TCPConnectionManager::ProcessACKForTransmission(const std::string& conn_key,
                                                          uint32_t ack_num, uint16_t window_size) {
    std::lock_guard<std::mutex> lock(connections_mutex_);

    auto it = connections_.find(conn_key);
    if (it == connections_.end()) {
        return 0;
    }

    TCPConnectionInfo& conn = it->second;

    // Log flow control state before processing
    size_t bytes_in_flight_before = conn.flow_controller ? conn.flow_controller->GetBytesInFlight() : 0;
    uint32_t last_acked_before = conn.flow_controller ? conn.flow_controller->GetLastAckedSeq() : 0;

    // Process ACK through flow controller
    bool new_data_acked = conn.ProcessACK(ack_num, window_size);

    // Log flow control state after processing
    size_t bytes_in_flight_after = conn.flow_controller ? conn.flow_controller->GetBytesInFlight() : 0;
    uint32_t last_acked_after = conn.flow_controller ? conn.flow_controller->GetLastAckedSeq() : 0;

    if (new_data_acked) {
        Log("ACK: ack=" + std::to_string(ack_num) + ", bytes_in_flight: " +
            std::to_string(bytes_in_flight_before) + " -> " + std::to_string(bytes_in_flight_after) +
            ", last_acked: " + std::to_string(last_acked_before) + " -> " + std::to_string(last_acked_after));
    }

    // Find transmission that needs more segments sent
    HTTPTransmission* trans = conn.GetTransmissionForACK(ack_num);
    if (trans && !trans->IsComplete()) {
        trans->last_ack_time = std::chrono::steady_clock::now();

        // Check if all segments have been sent
        bool all_segments_sent = (trans->next_segment_index >= trans->queued_segments.size());

        if (all_segments_sent) {
            // Calculate end sequence (base_seq + total payload)
            uint32_t end_seq = trans->base_seq;
            for (size_t payload : trans->segment_payload_sizes) {
                end_seq += payload;
            }

            // If ACK covers all data, transmission is complete
            // (regardless of current state - handles race with retransmit)
            if (ack_num >= end_seq) {
                trans->state = TransmissionState::COMPLETE;
                Log("Transmission COMPLETE: base_seq=" + std::to_string(trans->base_seq) +
                    ", end_seq=" + std::to_string(end_seq) + ", ack=" + std::to_string(ack_num));
                return 0;
            }

            // All sent but not all ACKed - ensure state is AWAITING_ACKS
            if (trans->state == TransmissionState::IN_PROGRESS) {
                trans->state = TransmissionState::AWAITING_ACKS;
            }
        } else {
            // More segments to send
            return trans->base_seq;  // Caller should send next batch
        }
    }

    return 0;
}

void TCPConnectionManager::StartHTTPTransmission(const std::string& conn_key,
                                                  uint32_t base_seq,
                                                  std::vector<mtp::ByteArray> segments,
                                                  std::vector<size_t> payload_sizes) {
    std::lock_guard<std::mutex> lock(connections_mutex_);

    auto it = connections_.find(conn_key);
    if (it == connections_.end()) {
        Log("StartHTTPTransmission: connection not found: " + conn_key);
        return;
    }

    TCPConnectionInfo& conn = it->second;
    conn.StartTransmission(base_seq, std::move(segments), std::move(payload_sizes));

    Log("Started HTTP transmission: base_seq=" + std::to_string(base_seq) +
        ", segments=" + std::to_string(conn.active_transmissions[base_seq].queued_segments.size()));
}

size_t TCPConnectionManager::GetNextBatch(const std::string& conn_key, uint32_t base_seq,
                                           std::vector<mtp::ByteArray>& segments_out,
                                           bool& is_last_batch) {
    std::lock_guard<std::mutex> lock(connections_mutex_);

    auto it = connections_.find(conn_key);
    if (it == connections_.end()) {
        return 0;
    }

    TCPConnectionInfo& conn = it->second;

    std::lock_guard<std::mutex> trans_lock(conn.transmissions_mutex);
    auto trans_it = conn.active_transmissions.find(base_seq);
    if (trans_it == conn.active_transmissions.end()) {
        return 0;
    }

    HTTPTransmission& trans = trans_it->second;

    if (trans.IsComplete() ||
        trans.next_segment_index >= trans.queued_segments.size()) {
        is_last_batch = true;
        return 0;
    }

    // Transition to IN_PROGRESS on first send
    if (trans.state == TransmissionState::PENDING) {
        trans.state = TransmissionState::IN_PROGRESS;
    }

    // Determine batch size based on receiver window (the device tells us what it can accept)
    size_t segments_to_send = conn.CalculateSegmentsToSend(trans, trans.queued_segments.size() - trans.next_segment_index);
    if (segments_to_send == 0) {
        // Debug: why is window full?
        size_t avail = conn.GetAvailableWindow();
        size_t bif = conn.flow_controller ? conn.flow_controller->GetBytesInFlight() : 0;
        Log("GetNextBatch: window full - avail=" + std::to_string(avail) +
            ", bytes_in_flight=" + std::to_string(bif) +
            ", receiver_window=" + std::to_string(conn.receiver_window) +
            ", next_idx=" + std::to_string(trans.next_segment_index) +
            ", total=" + std::to_string(trans.queued_segments.size()) +
            ", state=" + TransmissionStateToString(trans.state));
        return 0;
    }

    // Collect segments
    segments_out.clear();
    segments_out.reserve(segments_to_send);
    size_t bytes_to_send = 0;

    for (size_t i = 0; i < segments_to_send; i++) {
        size_t idx = trans.next_segment_index + i;
        segments_out.push_back(trans.queued_segments[idx]);
        bytes_to_send += trans.segment_payload_sizes[idx];
    }

    // Mark as sent (this updates flow controller)
    // Note: we need to release trans_lock first, then call MarkSegmentsSent
    // Actually, we're already holding both locks, so just update directly
    uint32_t current_seq = base_seq;
    for (size_t i = 0; i < trans.next_segment_index; i++) {
        current_seq += trans.segment_payload_sizes[i];
    }

    for (size_t i = 0; i < segments_to_send; i++) {
        size_t idx = trans.next_segment_index + i;
        size_t payload = trans.segment_payload_sizes[idx];
        conn.RecordBytesSent(payload, current_seq);
        current_seq += payload;
    }

    trans.next_segment_index += segments_to_send;

    is_last_batch = (trans.next_segment_index >= trans.queued_segments.size());

    // Transition to AWAITING_ACKS when all segments sent
    if (is_last_batch && trans.state == TransmissionState::IN_PROGRESS) {
        trans.state = TransmissionState::AWAITING_ACKS;
    }

    return segments_to_send;
}

bool TCPConnectionManager::CheckRetransmitNeeded(const std::string& conn_key,
                                                  uint32_t& base_seq, size_t& segment_index) {
    std::lock_guard<std::mutex> lock(connections_mutex_);

    auto it = connections_.find(conn_key);
    if (it == connections_.end()) {
        return false;
    }

    TCPConnectionInfo& conn = it->second;

    if (!conn.NeedsFastRetransmit()) {
        return false;
    }

    std::lock_guard<std::mutex> trans_lock(conn.transmissions_mutex);
    for (auto& [seq, trans] : conn.active_transmissions) {
        if (trans.NeedsRetransmit()) {
            base_seq = seq;
            segment_index = trans.retransmit_segment_index;
            return true;
        }
    }

    return false;
}

mtp::ByteArray TCPConnectionManager::GetRetransmitSegment(const std::string& conn_key,
                                                           uint32_t base_seq, size_t segment_index) {
    std::lock_guard<std::mutex> lock(connections_mutex_);

    auto it = connections_.find(conn_key);
    if (it == connections_.end()) {
        return {};
    }

    TCPConnectionInfo& conn = it->second;

    std::lock_guard<std::mutex> trans_lock(conn.transmissions_mutex);
    auto trans_it = conn.active_transmissions.find(base_seq);
    if (trans_it == conn.active_transmissions.end()) {
        return {};
    }

    HTTPTransmission& trans = trans_it->second;
    if (segment_index >= trans.queued_segments.size()) {
        return {};
    }

    return trans.queued_segments[segment_index];
}

void TCPConnectionManager::ClearRetransmitFlag(const std::string& conn_key) {
    std::lock_guard<std::mutex> lock(connections_mutex_);

    auto it = connections_.find(conn_key);
    if (it != connections_.end()) {
        it->second.ClearRetransmitFlag();
    }
}

// ============================================================================
// TCPConnectionManager - Utility Methods
// ============================================================================

TCPConnectionInfo* TCPConnectionManager::GetConnection(const std::string& conn_key) {
    std::lock_guard<std::mutex> lock(connections_mutex_);

    auto it = connections_.find(conn_key);
    if (it != connections_.end()) {
        return &it->second;
    }
    return nullptr;
}

TCPConnectionInfo& TCPConnectionManager::GetOrCreateConnection(const std::string& conn_key) {
    std::lock_guard<std::mutex> lock(connections_mutex_);
    TCPConnectionInfo& conn = connections_[conn_key];
    conn.log_callback = &log_callback_;
    return conn;
}

std::string TCPConnectionManager::MakeConnectionKey(
    uint32_t src_ip, uint16_t src_port,
    uint32_t dst_ip, uint16_t dst_port) {

    std::ostringstream oss;
    oss << IPParser::IPToString(src_ip) << ":" << src_port << "->"
        << IPParser::IPToString(dst_ip) << ":" << dst_port;
    return oss.str();
}

std::vector<mtp::ByteArray> TCPConnectionManager::SegmentHTTPPayload(
    const mtp::ByteArray& http_data,
    size_t max_segment_size) {

    std::vector<mtp::ByteArray> segments;

    if (http_data.empty()) {
        return segments;
    }

    // Find end of HTTP headers (\r\n\r\n)
    size_t header_end = 0;
    for (size_t i = 0; i + 3 < http_data.size(); i++) {
        if (http_data[i] == '\r' && http_data[i+1] == '\n' &&
            http_data[i+2] == '\r' && http_data[i+3] == '\n') {
            header_end = i + 4;  // Include the \r\n\r\n
            break;
        }
    }

    if (header_end == 0) {
        // No body separator found - treat entire response as single segment
        header_end = http_data.size();
    }

    // Segment 1: Headers (always sent as first segment)
    mtp::ByteArray header_segment(http_data.begin(), http_data.begin() + header_end);
    segments.push_back(std::move(header_segment));

    // Segment 2+: Body chunks of max_segment_size bytes
    size_t body_offset = header_end;
    while (body_offset < http_data.size()) {
        size_t chunk_size = std::min(max_segment_size, http_data.size() - body_offset);
        mtp::ByteArray body_segment(http_data.begin() + body_offset,
                                   http_data.begin() + body_offset + chunk_size);
        segments.push_back(std::move(body_segment));
        body_offset += chunk_size;
    }

    return segments;
}

void TCPConnectionManager::SetLogCallback(LogCallback callback) {
    log_callback_ = callback;
}

void TCPConnectionManager::Log(const std::string& message) {
    if (log_callback_) {
        log_callback_(message);
    }
}

std::vector<std::string> TCPConnectionManager::GetActiveConnectionKeys() {
    std::lock_guard<std::mutex> lock(connections_mutex_);

    std::vector<std::string> keys;
    keys.reserve(connections_.size());

    for (const auto& [key, conn] : connections_) {
        // Only include connections with active transmissions
        if (!conn.active_transmissions.empty()) {
            keys.push_back(key);
        }
    }

    return keys;
}

std::map<std::string, std::vector<SentSegment>> TCPConnectionManager::CheckAllTimeouts() {
    std::lock_guard<std::mutex> lock(connections_mutex_);

    std::map<std::string, std::vector<SentSegment>> result;

    for (auto& [conn_key, conn] : connections_) {
        std::vector<SentSegment> timed_out = conn.CheckTimeouts();
        if (!timed_out.empty()) {
            result[conn_key] = std::move(timed_out);
        }
    }

    return result;
}

bool TCPConnectionManager::HandleRTORetransmit(const std::string& conn_key, const SentSegment& segment,
                                                uint32_t& base_seq, size_t& segment_index) {
    std::lock_guard<std::mutex> lock(connections_mutex_);

    auto it = connections_.find(conn_key);
    if (it == connections_.end()) {
        return false;
    }

    TCPConnectionInfo& conn = it->second;
    std::lock_guard<std::mutex> trans_lock(conn.transmissions_mutex);

    // Find which transmission contains this segment
    for (auto& [seq, trans] : conn.active_transmissions) {
        // Check if SEQ falls within this transmission's range
        uint32_t trans_start = seq;
        uint32_t trans_end = trans_start;
        for (size_t payload_size : trans.segment_payload_sizes) {
            trans_end += payload_size;
        }

        if (segment.seq_start >= trans_start && segment.seq_start < trans_end) {
            // Found the transmission - find segment index
            uint32_t current_seq = trans_start;
            for (size_t i = 0; i < trans.queued_segments.size(); i++) {
                uint32_t seg_start = current_seq;
                uint32_t seg_end = current_seq + trans.segment_payload_sizes[i];

                if (segment.seq_start >= seg_start && segment.seq_start < seg_end) {
                    // Found the segment
                    base_seq = seq;
                    segment_index = i;

                    // Mark for retransmit
                    trans.state = TransmissionState::NEEDS_RETRANSMIT;
                    trans.retransmit_segment_index = i;

                    Log("RTO retransmit: conn=" + conn_key +
                        " segment " + std::to_string(i) + "/" +
                        std::to_string(trans.queued_segments.size()));

                    return true;
                }
                current_seq = seg_end;
            }
        }
    }

    return false;
}
