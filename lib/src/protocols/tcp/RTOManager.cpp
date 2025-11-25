#include "RTOManager.h"
#include <algorithm>
#include <cmath>

RTOManager::RTOManager()
    : srtt_ms_(0.0),
      rttvar_ms_(0.0),
      rto_ms_(INITIAL_RTO_MS),
      retransmit_count_(0),
      has_samples_(false) {
}

void RTOManager::UpdateRTT(Milliseconds rtt_sample) {
    double R = static_cast<double>(rtt_sample.count());

    if (!has_samples_) {
        // First RTT measurement (RFC 6298 Section 2.2)
        srtt_ms_ = R;
        rttvar_ms_ = R / 2.0;
        has_samples_ = true;
    } else {
        // Subsequent RTT measurements (RFC 6298 Section 2.3)
        // RTTVAR = (1 - beta) * RTTVAR + beta * |SRTT - R|
        double abs_diff = std::abs(srtt_ms_ - R);
        rttvar_ms_ = (1.0 - BETA) * rttvar_ms_ + BETA * abs_diff;

        // SRTT = (1 - alpha) * SRTT + alpha * R
        srtt_ms_ = (1.0 - ALPHA) * srtt_ms_ + ALPHA * R;
    }

    // Calculate new RTO (RFC 6298 Section 2.4)
    // RTO = SRTT + max(G, K*RTTVAR)
    double variance_term = std::max(static_cast<double>(G_MS), K * rttvar_ms_);
    double new_rto = srtt_ms_ + variance_term;

    rto_ms_ = ClampRTO(new_rto);

    // Reset retransmit count on successful RTT measurement
    retransmit_count_ = 0;
}

RTOManager::Milliseconds RTOManager::GetRTO() const {
    return Milliseconds(rto_ms_);
}

void RTOManager::OnRetransmit() {
    // Exponential backoff (RFC 6298 Section 5.5)
    // RTO = RTO * 2
    uint32_t new_rto = rto_ms_ * 2;
    rto_ms_ = ClampRTO(new_rto);

    retransmit_count_++;
}

void RTOManager::Reset() {
    srtt_ms_ = 0.0;
    rttvar_ms_ = 0.0;
    rto_ms_ = INITIAL_RTO_MS;
    retransmit_count_ = 0;
    has_samples_ = false;
}

std::optional<RTOManager::Milliseconds> RTOManager::GetSRTT() const {
    if (!has_samples_) {
        return std::nullopt;
    }
    return Milliseconds(static_cast<uint32_t>(srtt_ms_));
}

std::optional<RTOManager::Milliseconds> RTOManager::GetRTTVAR() const {
    if (!has_samples_) {
        return std::nullopt;
    }
    return Milliseconds(static_cast<uint32_t>(rttvar_ms_));
}

uint32_t RTOManager::ClampRTO(double rto_ms) const {
    // Clamp to [MIN_RTO, MAX_RTO] range
    if (rto_ms < MIN_RTO_MS) {
        return MIN_RTO_MS;
    }
    if (rto_ms > MAX_RTO_MS) {
        return MAX_RTO_MS;
    }
    return static_cast<uint32_t>(rto_ms);
}
