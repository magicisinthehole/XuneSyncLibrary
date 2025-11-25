#pragma once

#include <chrono>
#include <cstdint>
#include <optional>

/**
 * RTOManager - Retransmission Timeout Manager (RFC 6298)
 *
 * Implements RFC 6298 "Computing TCP's Retransmission Timer" for reliable
 * TCP segment delivery. Tracks round-trip time (RTT) samples and computes
 * retransmission timeout (RTO) values.
 *
 * Key Features:
 * - Smoothed RTT (SRTT) and RTT variance (RTTVAR) tracking
 * - Karn's Algorithm: Ignores RTT samples from retransmitted segments
 * - Exponential backoff: Doubles RTO on each retransmission
 * - RFC 6298 compliant RTO calculation: RTO = SRTT + max(G, 4*RTTVAR)
 *
 * Usage:
 *   RTOManager rto;
 *   auto timeout = rto.GetRTO();
 *   // ... send segment with timer set to timeout ...
 *   // On ACK received:
 *   rto.UpdateRTT(measured_rtt);
 *   // On timeout:
 *   rto.OnRetransmit();
 */
class RTOManager {
public:
    using Milliseconds = std::chrono::milliseconds;
    using TimePoint = std::chrono::steady_clock::time_point;

    /**
     * Constructor
     * Initializes with RFC 6298 default values
     */
    RTOManager();

    /**
     * Update RTT estimates with a new sample
     * @param rtt_sample Measured round-trip time in milliseconds
     *
     * Implements RFC 6298 Section 2.3:
     * - First sample: SRTT = R, RTTVAR = R/2
     * - Subsequent: RTTVAR = (1-beta)*RTTVAR + beta*|SRTT-R|
     *               SRTT = (1-alpha)*SRTT + alpha*R
     *
     * Note: Only call this for non-retransmitted segments (Karn's Algorithm)
     */
    void UpdateRTT(Milliseconds rtt_sample);

    /**
     * Get current Retransmission Timeout value
     * @return Current RTO in milliseconds
     *
     * Implements RFC 6298 Section 2.4:
     * RTO = SRTT + max(G, K*RTTVAR)
     * where G = clock granularity (1ms), K = 4
     *
     * Bounds: 1 second minimum, 60 seconds maximum
     */
    Milliseconds GetRTO() const;

    /**
     * Handle retransmission event
     * Implements exponential backoff: RTO = RTO * 2
     *
     * Called when a segment times out and must be retransmitted.
     * Doubles the RTO per RFC 6298 Section 5.5.
     */
    void OnRetransmit();

    /**
     * Reset RTO to initial value
     * Used when starting a new connection or after idle period
     */
    void Reset();

    /**
     * Check if we have valid RTT samples
     * @return true if at least one RTT sample has been recorded
     */
    bool HasRTTSamples() const { return has_samples_; }

    /**
     * Get current smoothed RTT estimate
     * @return SRTT in milliseconds, or nullopt if no samples yet
     */
    std::optional<Milliseconds> GetSRTT() const;

    /**
     * Get current RTT variance estimate
     * @return RTTVAR in milliseconds, or nullopt if no samples yet
     */
    std::optional<Milliseconds> GetRTTVAR() const;

    /**
     * Get number of consecutive retransmissions
     * @return Retransmit count (0 = no retransmits)
     */
    uint32_t GetRetransmitCount() const { return retransmit_count_; }

private:
    // RFC 6298 constants
    static constexpr double ALPHA = 0.125;      // SRTT smoothing factor (1/8)
    static constexpr double BETA = 0.25;        // RTTVAR smoothing factor (1/4)
    static constexpr uint32_t K = 4;            // RTO variance multiplier
    static constexpr uint32_t G_MS = 1;         // Clock granularity (1ms)

    // RTO bounds (RFC 6298 Section 2.4)
    static constexpr uint32_t MIN_RTO_MS = 1000;    // 1 second minimum
    static constexpr uint32_t MAX_RTO_MS = 60000;   // 60 seconds maximum
    static constexpr uint32_t INITIAL_RTO_MS = 3000; // 3 seconds initial (RFC 6298 Section 2.1)

    // State variables
    double srtt_ms_;              // Smoothed round-trip time (milliseconds)
    double rttvar_ms_;            // RTT variance (milliseconds)
    uint32_t rto_ms_;             // Current RTO value (milliseconds)
    uint32_t retransmit_count_;   // Number of consecutive retransmissions
    bool has_samples_;            // True if we've received at least one RTT sample

    /**
     * Clamp RTO to valid range [MIN_RTO, MAX_RTO]
     * @param rto_ms RTO value to clamp
     * @return Clamped RTO value
     */
    uint32_t ClampRTO(double rto_ms) const;
};
