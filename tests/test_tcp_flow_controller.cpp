/**
 * test_tcp_flow_controller.cpp
 *
 * Unit tests for TCPFlowController (Phase 5.3)
 * Tests RFC 5681/3465 compliant congestion control implementation
 */

#include "lib/src/protocols/tcp/TCPFlowController.h"
#include "lib/src/protocols/tcp/TCPConnectionManager.h"
#include <iostream>
#include <cassert>
#include <vector>

// Enhanced assertion macros with detailed output
template<typename T>
bool AssertEqual(const T& actual, const T& expected, const std::string& msg) {
    if (actual != expected) {
        std::cerr << "FAIL: " << msg << std::endl;
        std::cerr << "  Expected: " << expected << std::endl;
        std::cerr << "  Got:      " << actual << std::endl;
        return false;
    }
    return true;
}

// Specialization for enum types that can't be printed
template<>
bool AssertEqual(const FlowControlState& actual, const FlowControlState& expected, const std::string& msg) {
    if (actual != expected) {
        std::cerr << "FAIL: " << msg << std::endl;
        std::cerr << "  Expected: " << FlowControlStateToString(expected) << std::endl;
        std::cerr << "  Got:      " << FlowControlStateToString(actual) << std::endl;
        return false;
    }
    return true;
}

template<>
bool AssertEqual(const TCPState& actual, const TCPState& expected, const std::string& msg) {
    if (actual != expected) {
        std::cerr << "FAIL: " << msg << std::endl;
        std::cerr << "  Expected: " << TCPStateToString(expected) << std::endl;
        std::cerr << "  Got:      " << TCPStateToString(actual) << std::endl;
        return false;
    }
    return true;
}

#define ASSERT_EQ(actual, expected, msg) \
    if (!AssertEqual(actual, expected, msg)) return false;

#define ASSERT_TRUE(condition, msg) \
    if (!(condition)) { \
        std::cerr << "FAIL: " << msg << std::endl; \
        std::cerr << "  Condition evaluated to false" << std::endl; \
        return false; \
    }

#define ASSERT_FALSE(condition, msg) \
    if (condition) { \
        std::cerr << "FAIL: " << msg << std::endl; \
        std::cerr << "  Condition evaluated to true (expected false)" << std::endl; \
        return false; \
    }

// Test: Initial state
bool TestInitialState() {
    std::cout << "Testing initial state..." << std::endl;

    TCPFlowController fc(3 * TCPFlowController::MSS, 65535);

    ASSERT_EQ(fc.GetState(), FlowControlState::INITIAL, "Should start in INITIAL state");
    ASSERT_EQ(fc.GetCongestionWindow(), 3 * TCPFlowController::MSS, "Should start with 3*MSS cwnd");
    ASSERT_EQ(fc.GetSlowStartThreshold(), size_t(65535), "Should start with default ssthresh");
    ASSERT_EQ(fc.GetBytesInFlight(), size_t(0), "Should start with 0 bytes in flight");
    ASSERT_FALSE(fc.NeedsRetransmit(), "Should not need retransmit initially");
    ASSERT_FALSE(fc.IsComplete(), "Should not be complete initially");

    std::cout << "  PASS" << std::endl;
    return true;
}

// Test: Slow start growth
bool TestSlowStartGrowth() {
    std::cout << "Testing slow start growth..." << std::endl;

    TCPFlowController fc(3 * TCPFlowController::MSS, 65535);

    // First ACK should enter slow start
    bool new_data = fc.ProcessACK(100, 65535);
    ASSERT_TRUE(new_data, "First ACK should be new data");
    ASSERT_EQ(fc.GetState(), FlowControlState::SLOW_START, "Should enter slow start");

    size_t initial_cwnd = fc.GetCongestionWindow();

    // Second ACK should grow cwnd (slow start: exponential growth)
    new_data = fc.ProcessACK(200, 65535);
    ASSERT_TRUE(new_data, "Second ACK should be new data");
    size_t new_cwnd = fc.GetCongestionWindow();
    ASSERT_TRUE(new_cwnd > initial_cwnd, "cwnd should grow in slow start");

    std::cout << "  PASS (cwnd grew from " << initial_cwnd << " to " << new_cwnd << ")" << std::endl;
    return true;
}

// Test: Congestion avoidance transition
bool TestCongestionAvoidance() {
    std::cout << "Testing congestion avoidance..." << std::endl;

    // Start with cwnd close to ssthresh
    size_t ssthresh = 10 * TCPFlowController::MSS;
    TCPFlowController fc(9 * TCPFlowController::MSS, ssthresh);

    // Enter slow start
    fc.ProcessACK(100, 65535);
    ASSERT_EQ(fc.GetState(), FlowControlState::SLOW_START, "Should start in slow start");

    // Keep ACKing until we exceed ssthresh
    uint32_t ack = 100;
    for (int i = 0; i < 10; i++) {
        ack += TCPFlowController::MSS;
        fc.ProcessACK(ack, 65535);
    }

    // Should transition to congestion avoidance
    ASSERT_EQ(fc.GetState(), FlowControlState::CONGESTION_AVOIDANCE, "Should transition to congestion avoidance");

    std::cout << "  PASS (transitioned at cwnd=" << fc.GetCongestionWindow() << ")" << std::endl;
    return true;
}

// Test: Fast retransmit on 3 duplicate ACKs
bool TestFastRetransmit() {
    std::cout << "Testing fast retransmit (3 duplicate ACKs)..." << std::endl;

    TCPFlowController fc(10 * TCPFlowController::MSS, 65535);

    // Enter slow start
    fc.ProcessACK(1000, 65535);

    // Record some bytes in flight
    fc.RecordSegmentSent(TCPFlowController::MSS, 1000);
    fc.RecordSegmentSent(TCPFlowController::MSS, 1000 + TCPFlowController::MSS);

    // Send 3 duplicate ACKs
    for (int i = 0; i < 3; i++) {
        bool new_data = fc.ProcessACK(1000, 65535);  // Same ACK number
        ASSERT_FALSE(new_data, "Duplicate ACK should not be new data");
    }

    // Should trigger fast retransmit
    ASSERT_TRUE(fc.NeedsRetransmit(), "Should need retransmit after 3 dupacks");
    ASSERT_EQ(fc.GetState(), FlowControlState::FAST_RECOVERY, "Should enter fast recovery");
    ASSERT_TRUE(fc.IsInFastRecovery(), "IsInFastRecovery should return true");

    // ssthresh should be reduced
    ASSERT_TRUE(fc.GetSlowStartThreshold() < 65535, "ssthresh should be reduced");

    std::cout << "  PASS (ssthresh reduced to " << fc.GetSlowStartThreshold() << ")" << std::endl;
    return true;
}

// Test: Fast recovery exit
bool TestFastRecoveryExit() {
    std::cout << "Testing fast recovery exit..." << std::endl;

    TCPFlowController fc(10 * TCPFlowController::MSS, 65535);

    // Enter slow start and fast recovery
    fc.ProcessACK(1000, 65535);
    fc.RecordSegmentSent(TCPFlowController::MSS, 1000);
    fc.RecordSegmentSent(TCPFlowController::MSS, 1000 + TCPFlowController::MSS);

    // Trigger fast retransmit
    for (int i = 0; i < 3; i++) {
        fc.ProcessACK(1000, 65535);
    }
    ASSERT_EQ(fc.GetState(), FlowControlState::FAST_RECOVERY, "Should be in fast recovery");

    // New ACK should exit fast recovery
    bool new_data = fc.ProcessACK(2000, 65535);
    ASSERT_TRUE(new_data, "New ACK should be new data");
    ASSERT_EQ(fc.GetState(), FlowControlState::CONGESTION_AVOIDANCE, "Should exit to congestion avoidance");
    ASSERT_FALSE(fc.NeedsRetransmit(), "Should not need retransmit anymore");

    std::cout << "  PASS (exited to congestion avoidance)" << std::endl;
    return true;
}

// Test: Segment boundaries and retransmit index calculation
bool TestSegmentBoundaries() {
    std::cout << "Testing segment boundaries and retransmit index..." << std::endl;

    TCPFlowController fc(10 * TCPFlowController::MSS, 65535);

    // Set up segment boundaries (3 segments: 1460, 1460, 500 bytes)
    uint32_t base_seq = 1000;
    std::vector<size_t> segment_sizes = {1460, 1460, 500};
    fc.SetSegmentBoundaries(base_seq, segment_sizes);

    // Enter slow start
    fc.ProcessACK(base_seq, 65535);

    // Record segments sent (so bytes are in flight)
    fc.RecordSegmentSent(1460, base_seq);
    fc.RecordSegmentSent(1460, base_seq + 1460);
    fc.RecordSegmentSent(500, base_seq + 2920);

    // Simulate ACK for first segment (1000 + 1460 = 2460)
    fc.ProcessACK(2460, 65535);

    // Send 3 duplicate ACKs for first segment
    for (int i = 0; i < 3; i++) {
        fc.ProcessACK(2460, 65535);
    }

    // Should trigger fast retransmit with correct segment index
    ASSERT_TRUE(fc.NeedsRetransmit(), "Should need retransmit");

    // Segment index should be 1 (retransmit second segment)
    size_t retransmit_index = fc.GetRetransmitSegmentIndex();
    ASSERT_EQ(retransmit_index, size_t(1), "Should retransmit segment 1 (0-indexed)");

    std::cout << "  PASS (retransmit_index=" << retransmit_index << ")" << std::endl;
    return true;
}

// NOTE: TestWindowCalculations was removed - GetEffectiveWindow and IsWindowFull
// are now only in TCPConnectionInfo (single source of truth for receiver_window)

// Test: State transition logging
bool TestStateTransitionLogging() {
    std::cout << "Testing state transition logging..." << std::endl;

    std::vector<std::string> log_messages;
    std::function<void(const std::string&)> log_callback = [&](const std::string& msg) {
        log_messages.push_back(msg);
    };

    TCPConnectionInfo conn;
    conn.log_callback = &log_callback;

    // Transition from CLOSED to SYN_RECEIVED
    bool success = conn.TransitionTo(TCPState::SYN_RECEIVED);
    ASSERT_TRUE(success, "Transition should succeed");
    ASSERT_EQ(log_messages.size(), size_t(1), "Should have logged transition");
    ASSERT_TRUE(log_messages[0].find("CLOSED") != std::string::npos, "Log should mention CLOSED");
    ASSERT_TRUE(log_messages[0].find("SYN_RECEIVED") != std::string::npos, "Log should mention SYN_RECEIVED");

    std::cout << "  PASS (logged: " << log_messages[0] << ")" << std::endl;
    return true;
}

// Test: Invalid state transitions
bool TestInvalidTransitions() {
    std::cout << "Testing invalid state transitions..." << std::endl;

    TCPConnectionInfo conn;

    // Try invalid transition: CLOSED -> ESTABLISHED (should fail)
    bool success = conn.TransitionTo(TCPState::ESTABLISHED);
    ASSERT_FALSE(success, "Invalid transition should fail");
    ASSERT_EQ(conn.state, TCPState::CLOSED, "State should remain CLOSED");

    // Valid transition: CLOSED -> SYN_RECEIVED
    success = conn.TransitionTo(TCPState::SYN_RECEIVED);
    ASSERT_TRUE(success, "Valid transition should succeed");
    ASSERT_EQ(conn.state, TCPState::SYN_RECEIVED, "State should be SYN_RECEIVED");

    std::cout << "  PASS" << std::endl;
    return true;
}

// Test: Sequence number wraparound (RFC 793 compliance)
bool TestSequenceWraparound() {
    std::cout << "Testing sequence number wraparound..." << std::endl;

    TCPFlowController fc(10 * TCPFlowController::MSS, 65535);

    // Start near end of uint32_t range
    fc.ProcessACK(0xFFFFFFF0, 65535);
    ASSERT_EQ(fc.GetState(), FlowControlState::SLOW_START, "Should enter slow start");

    // ACK wraps around to beginning of range
    fc.ProcessACK(0x00000010, 65535);
    ASSERT_EQ(fc.GetState(), FlowControlState::SLOW_START, "Should handle wraparound");

    // Verify cwnd continues to grow despite wraparound
    size_t cwnd_before_wrap = fc.GetCongestionWindow();
    fc.ProcessACK(0x00000020, 65535);
    ASSERT_TRUE(fc.GetCongestionWindow() >= cwnd_before_wrap, "cwnd should not decrease on wraparound");

    std::cout << "  PASS (handled sequence wraparound correctly)" << std::endl;
    return true;
}

// NOTE: TestZeroReceiverWindow was removed - GetEffectiveWindow and IsWindowFull
// are now only in TCPConnectionInfo (single source of truth for receiver_window)

// Test: Invalid ACK handling (backwards ACK)
bool TestInvalidACK() {
    std::cout << "Testing invalid ACK (backwards ACK)..." << std::endl;

    TCPFlowController fc(10 * TCPFlowController::MSS, 65535);

    // Normal ACK progression
    bool new_data = fc.ProcessACK(1000, 65535);
    ASSERT_TRUE(new_data, "First ACK should be new data");

    new_data = fc.ProcessACK(2000, 65535);
    ASSERT_TRUE(new_data, "Forward ACK should be new data");

    // Backwards ACK (violates RFC 793)
    new_data = fc.ProcessACK(1500, 65535);
    ASSERT_FALSE(new_data, "Backwards ACK should not be treated as new data");

    std::cout << "  PASS (rejected backwards ACK)" << std::endl;
    return true;
}

// Test: RFC 5681 cwnd minimum (cwnd should not go below 2*MSS)
bool TestCwndMinimum() {
    std::cout << "Testing RFC 5681 cwnd minimum (2*MSS)..." << std::endl;

    // Start with very small cwnd
    TCPFlowController fc(1 * TCPFlowController::MSS, 65535);

    // Enter slow start
    fc.ProcessACK(1000, 65535);

    // Trigger fast retransmit to reduce cwnd
    fc.RecordSegmentSent(TCPFlowController::MSS, 1000);
    for (int i = 0; i < 3; i++) {
        fc.ProcessACK(1000, 65535);  // 3 duplicate ACKs
    }

    // cwnd should be reduced, but not below 2*MSS per RFC 5681
    size_t min_cwnd = 2 * TCPFlowController::MSS;
    ASSERT_TRUE(fc.GetCongestionWindow() >= min_cwnd,
                "cwnd should not go below 2*MSS (RFC 5681). Got " +
                std::to_string(fc.GetCongestionWindow()) + ", min is " +
                std::to_string(min_cwnd));

    std::cout << "  PASS (cwnd=" << fc.GetCongestionWindow() << " >= " << min_cwnd << ")" << std::endl;
    return true;
}

// Test: RFC 5681 ssthresh calculation (max(FlightSize/2, 2*MSS))
bool TestSsthreshCalculation() {
    std::cout << "Testing RFC 5681 ssthresh calculation..." << std::endl;

    TCPFlowController fc(10 * TCPFlowController::MSS, 65535);

    // Enter slow start and record bytes in flight
    fc.ProcessACK(1000, 65535);
    fc.RecordSegmentSent(5 * TCPFlowController::MSS, 1000);

    size_t initial_ssthresh = fc.GetSlowStartThreshold();

    // Trigger fast retransmit
    for (int i = 0; i < 3; i++) {
        fc.ProcessACK(1000, 65535);
    }

    size_t new_ssthresh = fc.GetSlowStartThreshold();
    size_t min_ssthresh = 2 * TCPFlowController::MSS;

    // ssthresh should be reduced from initial
    ASSERT_TRUE(new_ssthresh < initial_ssthresh, "ssthresh should be reduced after loss");

    // ssthresh should be at least 2*MSS per RFC 5681
    ASSERT_TRUE(new_ssthresh >= min_ssthresh,
                "ssthresh should be at least 2*MSS. Got " +
                std::to_string(new_ssthresh) + ", min is " +
                std::to_string(min_ssthresh));

    std::cout << "  PASS (ssthresh reduced from " << initial_ssthresh
              << " to " << new_ssthresh << ")" << std::endl;
    return true;
}

// NOTE: TestReceiverWindowShrinking was removed - GetEffectiveWindow
// is now only in TCPConnectionInfo (single source of truth for receiver_window)

// Test: Multiple fast retransmit cycles
bool TestMultipleFastRetransmitCycles() {
    std::cout << "Testing multiple fast retransmit cycles..." << std::endl;

    TCPFlowController fc(10 * TCPFlowController::MSS, 65535);

    // Enter slow start
    fc.ProcessACK(1000, 65535);
    fc.RecordSegmentSent(TCPFlowController::MSS, 1000);

    // First fast retransmit cycle
    for (int i = 0; i < 3; i++) {
        fc.ProcessACK(1000, 65535);
    }
    ASSERT_TRUE(fc.NeedsRetransmit(), "First cycle should trigger retransmit");
    ASSERT_EQ(fc.GetState(), FlowControlState::FAST_RECOVERY, "Should be in fast recovery");

    // Exit fast recovery
    fc.ProcessACK(2000, 65535);
    ASSERT_EQ(fc.GetState(), FlowControlState::CONGESTION_AVOIDANCE, "Should exit to congestion avoidance");

    // Second fast retransmit cycle
    fc.RecordSegmentSent(TCPFlowController::MSS, 2000);
    for (int i = 0; i < 3; i++) {
        fc.ProcessACK(2000, 65535);
    }
    ASSERT_TRUE(fc.NeedsRetransmit(), "Second cycle should also trigger retransmit");
    ASSERT_EQ(fc.GetState(), FlowControlState::FAST_RECOVERY, "Should re-enter fast recovery");

    std::cout << "  PASS (handled multiple fast retransmit cycles)" << std::endl;
    return true;
}

int main() {
    std::cout << "======================================" << std::endl;
    std::cout << " TCPFlowController Unit Tests" << std::endl;
    std::cout << " Phase 5.3 Integration Tests" << std::endl;
    std::cout << "======================================" << std::endl << std::endl;

    int passed = 0;
    int total = 0;

    auto run_test = [&](bool (*test_func)(), const std::string& name) {
        total++;
        if (test_func()) {
            passed++;
        } else {
            std::cout << "  TEST FAILED: " << name << std::endl;
        }
        std::cout << std::endl;
    };

    run_test(TestInitialState, "Initial State");
    run_test(TestSlowStartGrowth, "Slow Start Growth");
    run_test(TestCongestionAvoidance, "Congestion Avoidance");
    run_test(TestFastRetransmit, "Fast Retransmit");
    run_test(TestFastRecoveryExit, "Fast Recovery Exit");
    run_test(TestSegmentBoundaries, "Segment Boundaries");
    // NOTE: TestWindowCalculations, TestZeroReceiverWindow, TestReceiverWindowShrinking removed
    // Those features now tested via TCPConnectionInfo (single source of truth for receiver_window)
    run_test(TestStateTransitionLogging, "State Transition Logging");
    run_test(TestInvalidTransitions, "Invalid State Transitions");

    // RFC Compliance and Edge Cases
    run_test(TestSequenceWraparound, "Sequence Number Wraparound (RFC 793)");
    run_test(TestInvalidACK, "Invalid ACK (Backwards ACK)");
    run_test(TestCwndMinimum, "RFC 5681 cwnd Minimum (2*MSS)");
    run_test(TestSsthreshCalculation, "RFC 5681 ssthresh Calculation");
    run_test(TestMultipleFastRetransmitCycles, "Multiple Fast Retransmit Cycles");

    std::cout << "======================================" << std::endl;
    std::cout << " Test Results: " << passed << "/" << total << " passed" << std::endl;
    std::cout << "======================================" << std::endl;

    return (passed == total) ? 0 : 1;
}
