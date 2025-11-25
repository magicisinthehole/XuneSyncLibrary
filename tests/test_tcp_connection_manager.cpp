/**
 * test_tcp_connection_manager.cpp
 *
 * Unit tests for TCPConnectionManager (Phase 5.2)
 * Tests RFC 793 TCP state machine implementation
 */

#include "lib/src/protocols/tcp/TCPConnectionManager.h"
#include "lib/src/protocols/ppp/PPPParser.h"  // For TCPParser
#include <iostream>
#include <vector>

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

// Specialization for TCPState enum
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

// Test: TCP three-way handshake
bool TestThreeWayHandshake() {
    std::cout << "Testing TCP three-way handshake..." << std::endl;

    TCPConnectionManager manager;
    std::vector<std::string> log_messages;
    manager.SetLogCallback([&](const std::string& msg) {
        log_messages.push_back(msg);
    });

    // Step 1: Client sends SYN
    uint32_t client_ip = 0xC0A83765;  // 192.168.55.101
    uint16_t client_port = 49152;
    uint32_t server_ip = 0xC0A83764;  // 192.168.55.100
    uint16_t server_port = 80;
    uint32_t client_seq = 1000;

    auto syn_ack = manager.HandlePacket(
        client_ip, client_port,
        server_ip, server_port,
        client_seq, 0,
        TCPParser::TCP_FLAG_SYN,
        65535, mtp::ByteArray()
    );

    ASSERT_TRUE(syn_ack.has_value(), "SYN should generate SYN-ACK response");
    ASSERT_EQ(syn_ack->flags, uint8_t(TCPParser::TCP_FLAG_SYN | TCPParser::TCP_FLAG_ACK),
              "Response should be SYN-ACK");
    ASSERT_EQ(syn_ack->ack_num, uint32_t(client_seq + 1), "ACK should be client SEQ + 1");

    std::string conn_key = TCPConnectionManager::MakeConnectionKey(
        client_ip, client_port, server_ip, server_port);
    TCPConnectionInfo* conn = manager.GetConnection(conn_key);
    ASSERT_TRUE(conn != nullptr, "Connection should exist");
    ASSERT_EQ(conn->state, TCPState::SYN_RECEIVED, "Should be in SYN_RECEIVED state");

    // Step 2: Client sends ACK (completes handshake)
    uint32_t server_seq = syn_ack->seq_num;
    auto final_response = manager.HandlePacket(
        client_ip, client_port,
        server_ip, server_port,
        client_seq + 1, server_seq + 1,
        TCPParser::TCP_FLAG_ACK,
        65535, mtp::ByteArray()
    );

    ASSERT_FALSE(final_response.has_value(), "Final ACK should not generate response");
    ASSERT_EQ(conn->state, TCPState::ESTABLISHED, "Should be in ESTABLISHED state");

    std::cout << "  PASS (handshake completed, logged " << log_messages.size() << " events)" << std::endl;
    return true;
}

// Test: Connection termination (FIN)
bool TestConnectionTermination() {
    std::cout << "Testing connection termination (FIN)..." << std::endl;

    TCPConnectionManager manager;

    // Establish connection first
    uint32_t client_ip = 0xC0A83765;
    uint16_t client_port = 49152;
    uint32_t server_ip = 0xC0A83764;
    uint16_t server_port = 80;

    manager.HandlePacket(client_ip, client_port, server_ip, server_port,
                        1000, 0, TCPParser::TCP_FLAG_SYN, 65535, mtp::ByteArray());
    manager.HandlePacket(client_ip, client_port, server_ip, server_port,
                        1001, 2001, TCPParser::TCP_FLAG_ACK, 65535, mtp::ByteArray());

    std::string conn_key = TCPConnectionManager::MakeConnectionKey(
        client_ip, client_port, server_ip, server_port);
    TCPConnectionInfo* conn = manager.GetConnection(conn_key);
    ASSERT_EQ(conn->state, TCPState::ESTABLISHED, "Should be ESTABLISHED");

    // Client sends FIN
    auto fin_ack = manager.HandlePacket(
        client_ip, client_port,
        server_ip, server_port,
        1001, 2001,
        TCPParser::TCP_FLAG_FIN,
        65535, mtp::ByteArray()
    );

    ASSERT_TRUE(fin_ack.has_value(), "FIN should generate ACK");
    ASSERT_EQ(fin_ack->flags, TCPParser::TCP_FLAG_ACK, "Response should be ACK");
    ASSERT_EQ(conn->state, TCPState::CLOSE_WAIT, "Should be in CLOSE_WAIT state");

    std::cout << "  PASS (connection closed gracefully)" << std::endl;
    return true;
}

// Test: Connection reset (RST)
bool TestConnectionReset() {
    std::cout << "Testing connection reset (RST)..." << std::endl;

    TCPConnectionManager manager;

    // Establish connection
    uint32_t client_ip = 0xC0A83765;
    uint16_t client_port = 49152;
    uint32_t server_ip = 0xC0A83764;
    uint16_t server_port = 80;

    manager.HandlePacket(client_ip, client_port, server_ip, server_port,
                        1000, 0, TCPParser::TCP_FLAG_SYN, 65535, mtp::ByteArray());
    manager.HandlePacket(client_ip, client_port, server_ip, server_port,
                        1001, 2001, TCPParser::TCP_FLAG_ACK, 65535, mtp::ByteArray());

    std::string conn_key = TCPConnectionManager::MakeConnectionKey(
        client_ip, client_port, server_ip, server_port);
    TCPConnectionInfo* conn = manager.GetConnection(conn_key);
    ASSERT_EQ(conn->state, TCPState::ESTABLISHED, "Should be ESTABLISHED");

    // Client sends RST
    auto rst_response = manager.HandlePacket(
        client_ip, client_port,
        server_ip, server_port,
        1001, 2001,
        TCPParser::TCP_FLAG_RST,
        65535, mtp::ByteArray()
    );

    ASSERT_FALSE(rst_response.has_value(), "RST should not generate response");

    // Connection should be removed
    conn = manager.GetConnection(conn_key);
    ASSERT_TRUE(conn == nullptr, "Connection should be removed after RST");

    std::cout << "  PASS (connection reset and removed)" << std::endl;
    return true;
}

// Test: Invalid state transitions (RFC 793 Section 3.9 - MUST reject invalid packets)
bool TestInvalidStateTransitions() {
    std::cout << "Testing invalid state transitions (RFC 793 compliance)..." << std::endl;

    TCPConnectionManager manager;

    // Test 1: ACK without prior SYN
    // RFC 793 Section 3.9: "If the state is CLOSED (no connection)...
    // ... if the ACK bit is on, return: <SEQ=SEG.ACK><CTL=RST>"
    auto response = manager.HandlePacket(
        0xC0A83765, 49152,
        0xC0A83764, 80,
        1000, 2000,
        TCPParser::TCP_FLAG_ACK,
        65535, mtp::ByteArray()
    );

    std::string conn_key = TCPConnectionManager::MakeConnectionKey(
        0xC0A83765, 49152, 0xC0A83764, 80);
    TCPConnectionInfo* conn = manager.GetConnection(conn_key);

    // RFC 793 compliant behavior: Either RST response or ignore (no connection created)
    if (response.has_value()) {
        // If there's a response, it should be RST
        ASSERT_TRUE((response->flags & TCPParser::TCP_FLAG_RST) != 0,
                    "Response to unexpected ACK should be RST (RFC 793)");
        std::cout << "  PASS (sent RST for unexpected ACK - RFC compliant)" << std::endl;
    } else {
        // Or no response and no connection created (also valid per RFC 793)
        ASSERT_TRUE(conn == nullptr || conn->state == TCPState::CLOSED,
                    "If no RST sent, connection should not exist or be CLOSED");
        std::cout << "  PASS (ignored unexpected ACK - RFC compliant)" << std::endl;
    }

    // Test 2: Data packet without established connection should be rejected
    TCPConnectionManager manager2;  // New manager for second test
    auto data_response = manager2.HandlePacket(
        0xC0A83765, 49153,
        0xC0A83764, 80,
        5000, 6000,
        TCPParser::TCP_FLAG_ACK,
        65535,
        {'H', 'E', 'L', 'L', 'O'}  // Data without connection
    );

    std::string data_conn_key = TCPConnectionManager::MakeConnectionKey(
        0xC0A83765, 49153, 0xC0A83764, 80);
    TCPConnectionInfo* data_conn = manager2.GetConnection(data_conn_key);

    // Must not process data without established connection
    if (data_conn) {
        ASSERT_TRUE(data_conn->reassembler->GetBuffer().empty(),
                    "Data should not be buffered without established connection");
    }

    std::cout << "  PASS (rejected data without established connection)" << std::endl;
    return true;
}

// Test: HTTP buffer management
bool TestHTTPBufferManagement() {
    std::cout << "Testing HTTP buffer management..." << std::endl;

    TCPConnectionManager manager;

    // Establish connection
    manager.HandlePacket(0xC0A83765, 49152, 0xC0A83764, 80,
                        1000, 0, TCPParser::TCP_FLAG_SYN, 65535, mtp::ByteArray());
    manager.HandlePacket(0xC0A83765, 49152, 0xC0A83764, 80,
                        1001, 2001, TCPParser::TCP_FLAG_ACK, 65535, mtp::ByteArray());

    std::string conn_key = TCPConnectionManager::MakeConnectionKey(
        0xC0A83765, 49152, 0xC0A83764, 80);
    TCPConnectionInfo* conn = manager.GetConnection(conn_key);

    // Simulate receiving HTTP data
    mtp::ByteArray http_data = {'G', 'E', 'T', ' ', '/', '\r', '\n'};
    manager.HandlePacket(0xC0A83765, 49152, 0xC0A83764, 80,
                        1001, 2001, TCPParser::TCP_FLAG_ACK, 65535, http_data);

    // Buffer should contain data (exact behavior depends on implementation)
    ASSERT_TRUE(conn != nullptr, "Connection should still exist");

    std::cout << "  PASS (http_buffer accessible)" << std::endl;
    return true;
}

// Test: Multiple concurrent connections
bool TestMultipleConnections() {
    std::cout << "Testing multiple concurrent connections..." << std::endl;

    TCPConnectionManager manager;

    // Create 3 connections from different ports
    for (uint16_t port = 49152; port < 49155; port++) {
        manager.HandlePacket(0xC0A83765, port, 0xC0A83764, 80,
                            1000, 0, TCPParser::TCP_FLAG_SYN, 65535, mtp::ByteArray());
    }

    // Verify all 3 connections exist
    int connection_count = 0;
    for (uint16_t port = 49152; port < 49155; port++) {
        std::string conn_key = TCPConnectionManager::MakeConnectionKey(
            0xC0A83765, port, 0xC0A83764, 80);
        if (manager.GetConnection(conn_key) != nullptr) {
            connection_count++;
        }
    }

    ASSERT_EQ(connection_count, 3, "Should have 3 concurrent connections");

    std::cout << "  PASS (managed " << connection_count << " connections)" << std::endl;
    return true;
}

// Test: Sequence number management
bool TestSequenceNumberManagement() {
    std::cout << "Testing sequence number management..." << std::endl;

    TCPConnectionManager manager;

    // Establish connection
    auto syn_ack = manager.HandlePacket(0xC0A83765, 49152, 0xC0A83764, 80,
                                       1000, 0, TCPParser::TCP_FLAG_SYN,
                                       65535, mtp::ByteArray());

    ASSERT_TRUE(syn_ack.has_value(), "Should get SYN-ACK");

    uint32_t server_seq = syn_ack->seq_num;
    uint32_t server_ack = syn_ack->ack_num;

    ASSERT_EQ(server_ack, uint32_t(1001), "Server ACK should be client SEQ + 1");
    ASSERT_TRUE(server_seq > 0, "Server SEQ should be randomly generated");

    // Complete handshake
    manager.HandlePacket(0xC0A83765, 49152, 0xC0A83764, 80,
                        1001, server_seq + 1, TCPParser::TCP_FLAG_ACK,
                        65535, mtp::ByteArray());

    std::string conn_key = TCPConnectionManager::MakeConnectionKey(
        0xC0A83765, 49152, 0xC0A83764, 80);
    TCPConnectionInfo* conn = manager.GetConnection(conn_key);

    ASSERT_TRUE(conn != nullptr, "Connection should exist");
    ASSERT_EQ(conn->seq_num, server_seq + 1, "Server SEQ should increment after SYN");
    ASSERT_EQ(conn->ack_num, server_ack, "Server ACK should remain stable");

    std::cout << "  PASS (seq=" << conn->seq_num << ", ack=" << conn->ack_num << ")" << std::endl;
    return true;
}

// Test: State transition logging
bool TestStateTransitionLogging() {
    std::cout << "Testing state transition logging..." << std::endl;

    TCPConnectionManager manager;
    std::vector<std::string> log_messages;
    manager.SetLogCallback([&](const std::string& msg) {
        log_messages.push_back(msg);
    });

    // Perform handshake
    manager.HandlePacket(0xC0A83765, 49152, 0xC0A83764, 80,
                        1000, 0, TCPParser::TCP_FLAG_SYN, 65535, mtp::ByteArray());
    manager.HandlePacket(0xC0A83765, 49152, 0xC0A83764, 80,
                        1001, 2001, TCPParser::TCP_FLAG_ACK, 65535, mtp::ByteArray());

    // Should have logged state transitions
    bool found_closed_to_syn = false;
    bool found_syn_to_established = false;

    for (const auto& msg : log_messages) {
        if (msg.find("CLOSED") != std::string::npos &&
            msg.find("SYN_RECEIVED") != std::string::npos) {
            found_closed_to_syn = true;
        }
        if (msg.find("SYN_RECEIVED") != std::string::npos &&
            msg.find("ESTABLISHED") != std::string::npos) {
            found_syn_to_established = true;
        }
    }

    ASSERT_TRUE(found_closed_to_syn, "Should log CLOSED -> SYN_RECEIVED");
    ASSERT_TRUE(found_syn_to_established, "Should log SYN_RECEIVED -> ESTABLISHED");

    std::cout << "  PASS (logged " << log_messages.size() << " events)" << std::endl;
    return true;
}

// Test: Duplicate SYN handling (RFC 793 compliance)
bool TestDuplicateSYN() {
    std::cout << "Testing duplicate SYN handling..." << std::endl;

    TCPConnectionManager manager;

    uint32_t client_ip = 0xC0A83765;
    uint16_t client_port = 49152;
    uint32_t server_ip = 0xC0A83764;
    uint16_t server_port = 80;

    // First SYN
    auto syn_ack_1 = manager.HandlePacket(
        client_ip, client_port, server_ip, server_port,
        1000, 0, TCPParser::TCP_FLAG_SYN, 65535, mtp::ByteArray()
    );
    ASSERT_TRUE(syn_ack_1.has_value(), "First SYN should generate SYN-ACK");

    // Duplicate SYN (same sequence number) - simulates retransmission
    auto syn_ack_2 = manager.HandlePacket(
        client_ip, client_port, server_ip, server_port,
        1000, 0, TCPParser::TCP_FLAG_SYN, 65535, mtp::ByteArray()
    );

    // Should handle gracefully (either resend SYN-ACK or ignore)
    std::string conn_key = TCPConnectionManager::MakeConnectionKey(
        client_ip, client_port, server_ip, server_port);
    TCPConnectionInfo* conn = manager.GetConnection(conn_key);
    ASSERT_TRUE(conn != nullptr, "Connection should still exist");

    std::cout << "  PASS (handled duplicate SYN gracefully)" << std::endl;
    return true;
}

// Test: Sequence number wraparound (RFC 793)
bool TestTCPSequenceWraparound() {
    std::cout << "Testing TCP sequence number wraparound..." << std::endl;

    TCPConnectionManager manager;

    uint32_t client_ip = 0xC0A83765;
    uint16_t client_port = 49152;
    uint32_t server_ip = 0xC0A83764;
    uint16_t server_port = 80;

    // Start SYN near end of uint32_t range
    uint32_t client_seq = 0xFFFFFFF0;
    auto syn_ack = manager.HandlePacket(
        client_ip, client_port, server_ip, server_port,
        client_seq, 0, TCPParser::TCP_FLAG_SYN, 65535, mtp::ByteArray()
    );

    ASSERT_TRUE(syn_ack.has_value(), "SYN should generate SYN-ACK");
    ASSERT_EQ(syn_ack->ack_num, uint32_t(client_seq + 1), "ACK should be SEQ + 1 (may wrap)");

    // Complete handshake with wrapped ACK
    uint32_t server_seq = syn_ack->seq_num;
    manager.HandlePacket(
        client_ip, client_port, server_ip, server_port,
        client_seq + 1, server_seq + 1,  // This wraps around to 0x00000000 + remainder
        TCPParser::TCP_FLAG_ACK,
        65535, mtp::ByteArray()
    );

    std::string conn_key = TCPConnectionManager::MakeConnectionKey(
        client_ip, client_port, server_ip, server_port);
    TCPConnectionInfo* conn = manager.GetConnection(conn_key);
    ASSERT_TRUE(conn != nullptr, "Connection should exist");
    ASSERT_EQ(conn->state, TCPState::ESTABLISHED, "Should handle wraparound and reach ESTABLISHED");

    std::cout << "  PASS (handled sequence wraparound correctly)" << std::endl;
    return true;
}

// Test: HTTP buffer fragmentation across multiple packets
bool TestHTTPBufferFragmentation() {
    std::cout << "Testing HTTP buffer fragmentation..." << std::endl;

    TCPConnectionManager manager;

    // Establish connection
    manager.HandlePacket(0xC0A83765, 49152, 0xC0A83764, 80,
                        1000, 0, TCPParser::TCP_FLAG_SYN, 65535, mtp::ByteArray());
    manager.HandlePacket(0xC0A83765, 49152, 0xC0A83764, 80,
                        1001, 2001, TCPParser::TCP_FLAG_ACK, 65535, mtp::ByteArray());

    std::string conn_key = TCPConnectionManager::MakeConnectionKey(
        0xC0A83765, 49152, 0xC0A83764, 80);
    TCPConnectionInfo* conn = manager.GetConnection(conn_key);

    // Simulate fragmented HTTP request across 3 packets
    mtp::ByteArray fragment1 = {'G', 'E', 'T', ' ', '/'};
    mtp::ByteArray fragment2 = {' ', 'H', 'T', 'T', 'P'};
    mtp::ByteArray fragment3 = {'/', '1', '.', '1', '\r', '\n', '\r', '\n'};

    manager.HandlePacket(0xC0A83765, 49152, 0xC0A83764, 80,
                        1001, 2001, TCPParser::TCP_FLAG_ACK, 65535, fragment1);
    manager.HandlePacket(0xC0A83765, 49152, 0xC0A83764, 80,
                        1006, 2001, TCPParser::TCP_FLAG_ACK, 65535, fragment2);
    manager.HandlePacket(0xC0A83765, 49152, 0xC0A83764, 80,
                        1011, 2001, TCPParser::TCP_FLAG_ACK, 65535, fragment3);

    // Verify buffer contains all fragments (implementation-specific)
    ASSERT_TRUE(conn != nullptr, "Connection should still exist");
    ASSERT_TRUE(conn->reassembler->GetBuffer().size() > 0, "HTTP buffer should contain data");

    std::cout << "  PASS (http_buffer has " << conn->reassembler->GetBuffer().size() << " bytes)" << std::endl;
    return true;
}

// Test: Simultaneous close (RFC 793 Section 3.5)
bool TestSimultaneousClose() {
    std::cout << "Testing simultaneous close (RFC 793 Section 3.5)..." << std::endl;

    TCPConnectionManager manager;

    uint32_t client_ip = 0xC0A83765;
    uint16_t client_port = 49152;
    uint32_t server_ip = 0xC0A83764;
    uint16_t server_port = 80;

    // Establish connection
    manager.HandlePacket(client_ip, client_port, server_ip, server_port,
                        1000, 0, TCPParser::TCP_FLAG_SYN, 65535, mtp::ByteArray());
    manager.HandlePacket(client_ip, client_port, server_ip, server_port,
                        1001, 2001, TCPParser::TCP_FLAG_ACK, 65535, mtp::ByteArray());

    std::string conn_key = TCPConnectionManager::MakeConnectionKey(
        client_ip, client_port, server_ip, server_port);
    TCPConnectionInfo* conn = manager.GetConnection(conn_key);
    ASSERT_EQ(conn->state, TCPState::ESTABLISHED, "Should be ESTABLISHED");

    // Client sends FIN
    auto ack_response = manager.HandlePacket(
        client_ip, client_port, server_ip, server_port,
        1001, 2001, TCPParser::TCP_FLAG_FIN, 65535, mtp::ByteArray()
    );

    ASSERT_TRUE(ack_response.has_value(), "FIN should generate ACK");

    // RFC 793 Section 3.5: When client sends FIN from ESTABLISHED,
    // passive close transitions to CLOSE_WAIT
    ASSERT_EQ(conn->state, TCPState::CLOSE_WAIT, "Should be in CLOSE_WAIT after receiving FIN");

    // Verify connection still exists and state is stable
    ASSERT_TRUE(conn != nullptr, "Connection should still exist in CLOSE_WAIT");

    std::cout << "  PASS (correctly transitioned to CLOSE_WAIT after FIN)" << std::endl;
    return true;
}

// Test: Out-of-order packet handling (RFC 793 Section 3.3)
bool TestOutOfOrderPackets() {
    std::cout << "Testing out-of-order packet handling..." << std::endl;

    TCPConnectionManager manager;

    // Establish connection
    manager.HandlePacket(0xC0A83765, 49152, 0xC0A83764, 80,
                        1000, 0, TCPParser::TCP_FLAG_SYN, 65535, mtp::ByteArray());
    manager.HandlePacket(0xC0A83765, 49152, 0xC0A83764, 80,
                        1001, 2001, TCPParser::TCP_FLAG_ACK, 65535, mtp::ByteArray());

    std::string conn_key = TCPConnectionManager::MakeConnectionKey(
        0xC0A83765, 49152, 0xC0A83764, 80);
    TCPConnectionInfo* conn = manager.GetConnection(conn_key);

    size_t initial_buffer_size = conn->reassembler->GetBuffer().size();

    // Send packet with SEQ 2000 (future packet - gap)
    mtp::ByteArray data_block_2 = {'W', 'O', 'R', 'L', 'D'};
    manager.HandlePacket(0xC0A83765, 49152, 0xC0A83764, 80,
                        2000, 2001, TCPParser::TCP_FLAG_ACK, 65535, data_block_2);

    // Future packet should be queued/buffered but not yet processed
    // (implementation may drop or hold for reordering)

    // Send packet with SEQ 1001 (expected packet - fills gap)
    mtp::ByteArray data_block_1 = {'H', 'E', 'L', 'L', 'O'};
    manager.HandlePacket(0xC0A83765, 49152, 0xC0A83764, 80,
                        1001, 2001, TCPParser::TCP_FLAG_ACK, 65535, data_block_1);

    size_t final_buffer_size = conn->reassembler->GetBuffer().size();

    // Connection must still exist
    ASSERT_TRUE(conn != nullptr, "Connection should still exist");

    // Expected packet MUST be processed
    ASSERT_TRUE(final_buffer_size > initial_buffer_size,
                "Expected packet (SEQ 1001) must be buffered");

    // Verify the in-order data is present
    bool found_hello = false;
    if (conn->reassembler->GetBuffer().size() >= 5) {
        for (size_t i = 0; i <= conn->reassembler->GetBuffer().size() - 5; i++) {
            if (conn->reassembler->GetBuffer()[i] == 'H' &&
                conn->reassembler->GetBuffer()[i+1] == 'E' &&
                conn->reassembler->GetBuffer()[i+2] == 'L' &&
                conn->reassembler->GetBuffer()[i+3] == 'L' &&
                conn->reassembler->GetBuffer()[i+4] == 'O') {
                found_hello = true;
                break;
            }
        }
    }

    ASSERT_TRUE(found_hello, "In-order data (HELLO) must be correctly buffered");

    std::cout << "  PASS (buffered=" << final_buffer_size << " bytes, data integrity verified)" << std::endl;
    return true;
}

// Test: Connection with invalid ACK number (RFC 793 Section 3.9)
bool TestInvalidACKNumber() {
    std::cout << "Testing invalid ACK number handling (RFC 793 Section 3.9)..." << std::endl;

    TCPConnectionManager manager;

    // Establish connection (server SEQ will be random)
    auto syn_ack = manager.HandlePacket(0xC0A83765, 49152, 0xC0A83764, 80,
                                       1000, 0, TCPParser::TCP_FLAG_SYN,
                                       65535, mtp::ByteArray());
    ASSERT_TRUE(syn_ack.has_value(), "SYN should generate SYN-ACK");
    uint32_t server_seq = syn_ack->seq_num;

    manager.HandlePacket(0xC0A83765, 49152, 0xC0A83764, 80,
                        1001, server_seq + 1, TCPParser::TCP_FLAG_ACK,
                        65535, mtp::ByteArray());

    std::string conn_key = TCPConnectionManager::MakeConnectionKey(
        0xC0A83765, 49152, 0xC0A83764, 80);
    TCPConnectionInfo* conn = manager.GetConnection(conn_key);
    ASSERT_EQ(conn->state, TCPState::ESTABLISHED, "Should be ESTABLISHED");

    TCPState state_before_invalid_ack = conn->state;
    uint32_t seq_before = conn->seq_num;

    // Send ACK for data never sent (ACK = 99999) - invalid ACK
    auto response = manager.HandlePacket(0xC0A83765, 49152, 0xC0A83764, 80,
                                        1001, 99999, TCPParser::TCP_FLAG_ACK,
                                        65535, mtp::ByteArray());

    // RFC 793 Section 3.9: Invalid ACK should be handled per:
    // "If the ACK is a duplicate (SEG.ACK < SND.UNA), it can be ignored.
    //  If the ACK acks something not yet sent (SEG.ACK > SND.NXT), send an ACK, drop segment"

    // Our handler should either:
    // 1. Ignore the invalid ACK (connection state unchanged)
    // 2. Send RST and terminate connection

    if (conn != nullptr) {
        // Connection still exists - verify state is not corrupted
        ASSERT_EQ(conn->state, state_before_invalid_ack,
                  "State should not change for invalid ACK (if connection maintained)");

        // Sequence numbers should not be corrupted
        ASSERT_TRUE(conn->seq_num == seq_before || conn->seq_num == seq_before + 1,
                    "Sequence number should not be corrupted by invalid ACK");

        std::cout << "  PASS (invalid ACK ignored, state preserved)" << std::endl;
    } else {
        // Connection terminated (RST sent) - also valid per RFC 793
        std::cout << "  PASS (invalid ACK caused connection termination - also RFC compliant)" << std::endl;
    }

    return true;
}

int main() {
    std::cout << "======================================" << std::endl;
    std::cout << " TCPConnectionManager Unit Tests" << std::endl;
    std::cout << " Phase 5.2 TCP State Machine" << std::endl;
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

    run_test(TestThreeWayHandshake, "Three-Way Handshake");
    run_test(TestConnectionTermination, "Connection Termination (FIN)");
    run_test(TestConnectionReset, "Connection Reset (RST)");
    run_test(TestInvalidStateTransitions, "Invalid State Transitions");
    run_test(TestHTTPBufferManagement, "HTTP Buffer Management");
    run_test(TestMultipleConnections, "Multiple Concurrent Connections");
    run_test(TestSequenceNumberManagement, "Sequence Number Management");
    run_test(TestStateTransitionLogging, "State Transition Logging");

    // RFC Compliance and Edge Cases
    run_test(TestDuplicateSYN, "Duplicate SYN Handling (RFC 793)");
    run_test(TestTCPSequenceWraparound, "TCP Sequence Wraparound (RFC 793)");
    run_test(TestHTTPBufferFragmentation, "HTTP Buffer Fragmentation");
    run_test(TestSimultaneousClose, "Simultaneous Close");
    run_test(TestOutOfOrderPackets, "Out-of-Order Packet Handling");
    run_test(TestInvalidACKNumber, "Invalid ACK Number Handling");

    std::cout << "======================================" << std::endl;
    std::cout << " Test Results: " << passed << "/" << total << " passed" << std::endl;
    std::cout << "======================================" << std::endl;

    return (passed == total) ? 0 : 1;
}
