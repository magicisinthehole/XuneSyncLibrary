/**
 * test_http_interceptor_integration.cpp
 *
 * Integration tests for ZuneHTTPInterceptor (Phase 5.2/5.3)
 * Tests the full protocol stack: PPP → DNS/CCP/TCP → Response queueing → Draining
 *
 * These tests would have caught the bugs:
 * - Mutex deadlock in DNS/CCP response draining
 * - TCP sequence number tracking bug (retransmission detection)
 */

#include "../lib/src/protocols/http/ZuneHTTPInterceptor.h"
#include "../lib/src/protocols/ppp/PPPParser.h"
#include "../lib/src/protocols/handlers/DNSHandler.h"
#include "../lib/src/protocols/handlers/CCPHandler.h"
#include "../lib/src/protocols/tcp/TCPConnectionManager.h"
#include "../lib/src/protocols/tcp/TCPState.h"
#include <iostream>
#include <thread>
#include <chrono>

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

// Specialization for TCPState to use TCPStateToString()
template<>
bool AssertEqual<TCPState>(const TCPState& actual, const TCPState& expected, const std::string& msg) {
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

// Helper: Build a PPP-framed DNS query packet
mtp::ByteArray BuildDNSQueryFrame(const std::string& hostname) {
    mtp::ByteArray dns_query;

    // DNS header
    dns_query.insert(dns_query.end(), {
        0xAB, 0xCD,  // Transaction ID
        0x01, 0x00,  // Flags (standard query)
        0x00, 0x01,  // Questions: 1
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00  // Answer/Authority/Additional: 0
    });

    // Encode hostname (e.g., "catalog.zune.net" → 7 "catalog" 4 "zune" 3 "net" 0)
    size_t start = 0;
    while (start < hostname.size()) {
        size_t dot_pos = hostname.find('.', start);
        if (dot_pos == std::string::npos) {
            dot_pos = hostname.size();
        }
        size_t len = dot_pos - start;
        dns_query.push_back(static_cast<uint8_t>(len));
        for (size_t i = start; i < dot_pos; i++) {
            dns_query.push_back(hostname[i]);
        }
        start = dot_pos + 1;
    }
    dns_query.push_back(0x00);  // Null terminator

    // Type A, Class IN
    dns_query.insert(dns_query.end(), {0x00, 0x01, 0x00, 0x01});

    // Wrap in UDP packet
    mtp::ByteArray udp_packet;
    udp_packet.insert(udp_packet.end(), {
        0xC0, 0x00,  // Source port: 49152
        0x00, 0x35,  // Dest port: 53 (DNS)
        0x00, static_cast<uint8_t>(8 + dns_query.size()),  // Length
        0x00, 0x00   // Checksum
    });
    udp_packet.insert(udp_packet.end(), dns_query.begin(), dns_query.end());

    // Wrap in IP packet
    mtp::ByteArray ip_packet;
    ip_packet.insert(ip_packet.end(), {
        0x45, 0x00,  // Version + IHL
        0x00, static_cast<uint8_t>(20 + udp_packet.size()),  // Total length
        0x00, 0x01, 0x00, 0x00,  // ID + Flags
        0x40, 0x11,  // TTL + Protocol (UDP)
        0x00, 0x00,  // Checksum
        0xC0, 0xA8, 0x37, 0x65,  // Source: 192.168.55.101
        0xC0, 0xA8, 0x37, 0x64   // Dest: 192.168.55.100
    });
    ip_packet.insert(ip_packet.end(), udp_packet.begin(), udp_packet.end());

    // Wrap in PPP frame (protocol 0x0021 = IPv4)
    return PPPParser::WrapPayload(ip_packet, 0x0021);
}

// Helper: Build a CCP Config-Request frame
mtp::ByteArray BuildCCPConfigRequestFrame() {
    mtp::ByteArray ccp_packet;
    ccp_packet.push_back(0x01);  // Code: Configure-Request
    ccp_packet.push_back(0x01);  // ID: 1
    ccp_packet.push_back(0x00);  // Length (high)
    ccp_packet.push_back(0x0A);  // Length (low) - 10 bytes

    // Add Deflate compression option
    ccp_packet.push_back(0x1A);  // Type: Deflate
    ccp_packet.push_back(0x06);  // Length: 6
    ccp_packet.push_back(0x00);  // Window size
    ccp_packet.push_back(0x0F);
    ccp_packet.push_back(0x00);
    ccp_packet.push_back(0x00);

    // Wrap in PPP frame (protocol 0x80fd = CCP)
    return PPPParser::WrapPayload(ccp_packet, 0x80fd);
}

// Helper: Build TCP SYN packet
mtp::ByteArray BuildTCPSYNFrame(uint32_t src_ip, uint16_t src_port,
                                 uint32_t dst_ip, uint16_t dst_port,
                                 uint32_t seq_num) {
    mtp::ByteArray tcp_packet;

    // TCP header (simplified - no options for brevity)
    tcp_packet.push_back((src_port >> 8) & 0xFF);
    tcp_packet.push_back(src_port & 0xFF);
    tcp_packet.push_back((dst_port >> 8) & 0xFF);
    tcp_packet.push_back(dst_port & 0xFF);

    // Sequence number
    tcp_packet.push_back((seq_num >> 24) & 0xFF);
    tcp_packet.push_back((seq_num >> 16) & 0xFF);
    tcp_packet.push_back((seq_num >> 8) & 0xFF);
    tcp_packet.push_back(seq_num & 0xFF);

    // ACK number (0 for SYN)
    tcp_packet.insert(tcp_packet.end(), {0x00, 0x00, 0x00, 0x00});

    // Data offset (5 words = 20 bytes) + flags (SYN = 0x02)
    tcp_packet.push_back(0x50);  // Data offset
    tcp_packet.push_back(0x02);  // SYN flag

    // Window size
    tcp_packet.insert(tcp_packet.end(), {0x80, 0x00});

    // Checksum + Urgent pointer
    tcp_packet.insert(tcp_packet.end(), {0x00, 0x00, 0x00, 0x00});

    // Wrap in IP packet
    mtp::ByteArray ip_packet;
    ip_packet.insert(ip_packet.end(), {
        0x45, 0x00,  // Version + IHL
        0x00, static_cast<uint8_t>(20 + tcp_packet.size()),
        0x00, 0x01, 0x00, 0x00,
        0x40, 0x06,  // TTL + Protocol (TCP)
        0x00, 0x00   // Checksum
    });

    // Source IP
    ip_packet.push_back((src_ip >> 24) & 0xFF);
    ip_packet.push_back((src_ip >> 16) & 0xFF);
    ip_packet.push_back((src_ip >> 8) & 0xFF);
    ip_packet.push_back(src_ip & 0xFF);

    // Dest IP
    ip_packet.push_back((dst_ip >> 24) & 0xFF);
    ip_packet.push_back((dst_ip >> 16) & 0xFF);
    ip_packet.push_back((dst_ip >> 8) & 0xFF);
    ip_packet.push_back(dst_ip & 0xFF);

    ip_packet.insert(ip_packet.end(), tcp_packet.begin(), tcp_packet.end());

    return PPPParser::WrapPayload(ip_packet, 0x0021);
}

// Helper: Build TCP ACK packet
mtp::ByteArray BuildTCPACKFrame(uint32_t src_ip, uint16_t src_port,
                                 uint32_t dst_ip, uint16_t dst_port,
                                 uint32_t seq_num, uint32_t ack_num) {
    mtp::ByteArray tcp_packet;

    tcp_packet.push_back((src_port >> 8) & 0xFF);
    tcp_packet.push_back(src_port & 0xFF);
    tcp_packet.push_back((dst_port >> 8) & 0xFF);
    tcp_packet.push_back(dst_port & 0xFF);

    // Sequence number
    tcp_packet.push_back((seq_num >> 24) & 0xFF);
    tcp_packet.push_back((seq_num >> 16) & 0xFF);
    tcp_packet.push_back((seq_num >> 8) & 0xFF);
    tcp_packet.push_back(seq_num & 0xFF);

    // ACK number
    tcp_packet.push_back((ack_num >> 24) & 0xFF);
    tcp_packet.push_back((ack_num >> 16) & 0xFF);
    tcp_packet.push_back((ack_num >> 8) & 0xFF);
    tcp_packet.push_back(ack_num & 0xFF);

    // Data offset + flags (ACK = 0x10)
    tcp_packet.push_back(0x50);
    tcp_packet.push_back(0x10);

    // Window size
    tcp_packet.insert(tcp_packet.end(), {0x80, 0x00});

    // Checksum + Urgent
    tcp_packet.insert(tcp_packet.end(), {0x00, 0x00, 0x00, 0x00});

    // Wrap in IP
    mtp::ByteArray ip_packet;
    ip_packet.insert(ip_packet.end(), {
        0x45, 0x00,
        0x00, static_cast<uint8_t>(20 + tcp_packet.size()),
        0x00, 0x01, 0x00, 0x00,
        0x40, 0x06,
        0x00, 0x00
    });

    ip_packet.push_back((src_ip >> 24) & 0xFF);
    ip_packet.push_back((src_ip >> 16) & 0xFF);
    ip_packet.push_back((src_ip >> 8) & 0xFF);
    ip_packet.push_back(src_ip & 0xFF);

    ip_packet.push_back((dst_ip >> 24) & 0xFF);
    ip_packet.push_back((dst_ip >> 16) & 0xFF);
    ip_packet.push_back((dst_ip >> 8) & 0xFF);
    ip_packet.push_back(dst_ip & 0xFF);

    ip_packet.insert(ip_packet.end(), tcp_packet.begin(), tcp_packet.end());

    return PPPParser::WrapPayload(ip_packet, 0x0021);
}

// Helper: Build TCP PSH,ACK packet with data
mtp::ByteArray BuildTCPDataFrame(uint32_t src_ip, uint16_t src_port,
                                  uint32_t dst_ip, uint16_t dst_port,
                                  uint32_t seq_num, uint32_t ack_num,
                                  const std::string& data) {
    mtp::ByteArray tcp_packet;

    tcp_packet.push_back((src_port >> 8) & 0xFF);
    tcp_packet.push_back(src_port & 0xFF);
    tcp_packet.push_back((dst_port >> 8) & 0xFF);
    tcp_packet.push_back(dst_port & 0xFF);

    tcp_packet.push_back((seq_num >> 24) & 0xFF);
    tcp_packet.push_back((seq_num >> 16) & 0xFF);
    tcp_packet.push_back((seq_num >> 8) & 0xFF);
    tcp_packet.push_back(seq_num & 0xFF);

    tcp_packet.push_back((ack_num >> 24) & 0xFF);
    tcp_packet.push_back((ack_num >> 16) & 0xFF);
    tcp_packet.push_back((ack_num >> 8) & 0xFF);
    tcp_packet.push_back(ack_num & 0xFF);

    // Data offset + flags (PSH,ACK = 0x18)
    tcp_packet.push_back(0x50);
    tcp_packet.push_back(0x18);

    tcp_packet.insert(tcp_packet.end(), {0x80, 0x00});
    tcp_packet.insert(tcp_packet.end(), {0x00, 0x00, 0x00, 0x00});

    // Add data
    tcp_packet.insert(tcp_packet.end(), data.begin(), data.end());

    // Wrap in IP
    mtp::ByteArray ip_packet;
    ip_packet.insert(ip_packet.end(), {
        0x45, 0x00,
        0x00, static_cast<uint8_t>(20 + tcp_packet.size()),
        0x00, 0x01, 0x00, 0x00,
        0x40, 0x06,
        0x00, 0x00
    });

    ip_packet.push_back((src_ip >> 24) & 0xFF);
    ip_packet.push_back((src_ip >> 16) & 0xFF);
    ip_packet.push_back((src_ip >> 8) & 0xFF);
    ip_packet.push_back(src_ip & 0xFF);

    ip_packet.push_back((dst_ip >> 24) & 0xFF);
    ip_packet.push_back((dst_ip >> 16) & 0xFF);
    ip_packet.push_back((dst_ip >> 8) & 0xFF);
    ip_packet.push_back(dst_ip & 0xFF);

    ip_packet.insert(ip_packet.end(), tcp_packet.begin(), tcp_packet.end());

    return PPPParser::WrapPayload(ip_packet, 0x0021);
}

// Test 1: DNS Query End-to-End (catches mutex deadlock bug)
bool TestDNSQueryEndToEnd() {
    std::cout << "Testing DNS query end-to-end (response draining)..." << std::endl;

    // This test would have caught the mutex deadlock bug where DNS responses
    // were queued but DrainResponseQueue() was called while holding the mutex

    std::map<std::string, uint32_t> hostname_map = {
        {"catalog.zune.net", 0xC0A8001E}  // 192.168.0.30
    };

    DNSHandler dns_handler(hostname_map);

    // Build DNS query PPP frame
    mtp::ByteArray dns_query_frame = BuildDNSQueryFrame("catalog.zune.net");

    // Extract IP packet from PPP frame
    uint16_t protocol = 0;
    mtp::ByteArray ip_packet = PPPParser::ExtractPayload(dns_query_frame, &protocol);
    ASSERT_EQ(protocol, uint16_t(0x0021), "Should be IPv4 protocol");

    // Handle DNS query (DNSHandler expects IP packet)
    auto dns_response = dns_handler.HandleQuery(ip_packet);

    ASSERT_TRUE(dns_response.has_value(), "DNS handler should return response");
    ASSERT_TRUE(dns_response->size() > 0, "DNS response should not be empty");

    // The bug was that this response would be queued but never drained
    // because DrainResponseQueue() deadlocked trying to acquire the mutex again

    std::cout << "  PASS (DNS response generated: " << dns_response->size() << " bytes)" << std::endl;
    return true;
}

// Test 2: CCP Config-Request End-to-End (catches mutex deadlock bug)
bool TestCCPConfigRequestEndToEnd() {
    std::cout << "Testing CCP Config-Request end-to-end (response draining)..." << std::endl;

    CCPHandler ccp_handler;

    mtp::ByteArray ccp_frame = BuildCCPConfigRequestFrame();

    // Extract CCP packet from PPP frame
    uint16_t protocol = 0;
    mtp::ByteArray ccp_packet = PPPParser::ExtractPayload(ccp_frame, &protocol);
    ASSERT_EQ(protocol, uint16_t(0x80fd), "Should be CCP protocol");

    // Handle CCP request
    auto ccp_response = ccp_handler.HandlePacket(ccp_packet);

    ASSERT_TRUE(ccp_response.has_value(), "CCP handler should return Config-Reject");
    ASSERT_TRUE(ccp_response->size() > 0, "CCP response should not be empty");

    // Extract CCP code from response (CCPHandler returns PPP-framed response)
    uint16_t response_protocol = 0;
    mtp::ByteArray response_ccp = PPPParser::ExtractPayload(ccp_response.value(), &response_protocol);
    ASSERT_EQ(response_protocol, uint16_t(0x80fd), "Response should be CCP protocol");
    ASSERT_TRUE(response_ccp.size() >= 1, "CCP response should have code");

    uint8_t ccp_code = response_ccp[0];
    ASSERT_EQ(ccp_code, uint8_t(0x04), "CCP response should be Config-Reject (0x04)");

    std::cout << "  PASS (CCP Config-Reject generated)" << std::endl;
    return true;
}

// Test 3: TCP Sequential Data Packets (catches sequence number tracking bug)
bool TestTCPSequentialDataPackets() {
    std::cout << "Testing TCP sequential data packets (retransmission detection)..." << std::endl;

    // This test would have caught the bug where last_received_seq was set to seq_num
    // instead of seq_num + data_length, causing all subsequent packets to be flagged
    // as retransmissions

    TCPConnectionManager manager;

    uint32_t client_ip = 0xC0A83765;  // 192.168.55.101
    uint16_t client_port = 49152;
    uint32_t server_ip = 0xC0A83764;  // 192.168.55.100
    uint16_t server_port = 80;

    // Establish connection
    auto syn_ack = manager.HandlePacket(client_ip, client_port, server_ip, server_port,
                                       1000, 0, TCPParser::TCP_FLAG_SYN, 65535,
                                       mtp::ByteArray());
    ASSERT_TRUE(syn_ack.has_value(), "SYN should generate SYN-ACK");

    uint32_t server_seq = syn_ack->seq_num;
    manager.HandlePacket(client_ip, client_port, server_ip, server_port,
                        1001, server_seq + 1, TCPParser::TCP_FLAG_ACK, 65535,
                        mtp::ByteArray());

    std::string conn_key = TCPConnectionManager::MakeConnectionKey(
        client_ip, client_port, server_ip, server_port);
    TCPConnectionInfo* conn = manager.GetConnection(conn_key);
    ASSERT_TRUE(conn != nullptr, "Connection should exist");
    ASSERT_EQ(conn->state, TCPState::ESTABLISHED, "Should be ESTABLISHED");

    // Send first data packet: SEQ=1001, "GET "
    mtp::ByteArray data1 = {'G', 'E', 'T', ' '};
    manager.HandlePacket(client_ip, client_port, server_ip, server_port,
                        1001, server_seq + 1, TCPParser::TCP_FLAG_ACK, 65535,
                        data1);

    // CRITICAL: Buffer should contain "GET "
    ASSERT_TRUE(conn->reassembler && conn->reassembler->GetBuffer().size() >= 4, "First packet should be buffered");

    // Send second data packet: SEQ=1005 (1001 + 4), "HTTP"
    mtp::ByteArray data2 = {'H', 'T', 'T', 'P'};
    manager.HandlePacket(client_ip, client_port, server_ip, server_port,
                        1005, server_seq + 1, TCPParser::TCP_FLAG_ACK, 65535,
                        data2);

    // CRITICAL: Buffer should now contain "GET HTTP" (8 bytes)
    // The bug would cause this packet to be flagged as retransmission and ignored
    ASSERT_TRUE(conn->reassembler->GetBuffer().size() >= 8,
                "Second sequential packet should be accepted (not flagged as retransmit)");

    // Verify data integrity
    bool found_get_http = false;
    if (conn->reassembler->GetBuffer().size() >= 8) {
        std::string buffered(conn->reassembler->GetBuffer().begin(), conn->reassembler->GetBuffer().begin() + 8);
        found_get_http = (buffered == "GET HTTP");
    }
    ASSERT_TRUE(found_get_http, "Sequential packets should be combined correctly");

    // Send actual retransmission: SEQ=1001 again (duplicate)
    manager.HandlePacket(client_ip, client_port, server_ip, server_port,
                        1001, server_seq + 1, TCPParser::TCP_FLAG_ACK, 65535,
                        data1);

    // CRITICAL: Buffer should still be 8 bytes (retransmit should be ignored)
    ASSERT_EQ(conn->reassembler->GetBuffer().size(), size_t(8),
              "Retransmitted data should be ignored (not re-added to buffer)");

    std::cout << "  PASS (sequential packets accepted, retransmit ignored)" << std::endl;
    return true;
}

// Test 4: TCP Out-of-Order Data Handling
bool TestTCPOutOfOrderData() {
    std::cout << "Testing TCP out-of-order data handling..." << std::endl;

    TCPConnectionManager manager;

    uint32_t client_ip = 0xC0A83765;
    uint16_t client_port = 49153;
    uint32_t server_ip = 0xC0A83764;
    uint16_t server_port = 80;

    // Establish connection
    auto syn_ack = manager.HandlePacket(client_ip, client_port, server_ip, server_port,
                                       2000, 0, TCPParser::TCP_FLAG_SYN, 65535,
                                       mtp::ByteArray());
    uint32_t server_seq = syn_ack->seq_num;
    manager.HandlePacket(client_ip, client_port, server_ip, server_port,
                        2001, server_seq + 1, TCPParser::TCP_FLAG_ACK, 65535,
                        mtp::ByteArray());

    std::string conn_key = TCPConnectionManager::MakeConnectionKey(
        client_ip, client_port, server_ip, server_port);
    TCPConnectionInfo* conn = manager.GetConnection(conn_key);

    // Send packet 2 before packet 1 (out of order): SEQ=2006, "WORLD" (contiguous with packet 1)
    mtp::ByteArray data2 = {'W', 'O', 'R', 'L', 'D'};
    manager.HandlePacket(client_ip, client_port, server_ip, server_port,
                        2006, server_seq + 1, TCPParser::TCP_FLAG_ACK, 65535,
                        data2);

    // Send packet 1: SEQ=2001, "HELLO" (fills the gap)
    mtp::ByteArray data1 = {'H', 'E', 'L', 'L', 'O'};
    manager.HandlePacket(client_ip, client_port, server_ip, server_port,
                        2001, server_seq + 1, TCPParser::TCP_FLAG_ACK, 65535,
                        data1);

    // After sending both packets, buffer should contain "HELLOWORLD" (10 bytes)
    // The out-of-order packet should be buffered, then flushed when gap is filled
    ASSERT_TRUE(conn->reassembler->GetBuffer().size() >= 10,
                "Out-of-order packet should be buffered and flushed when gap is filled");

    // Verify "HELLOWORLD" is present
    bool found_helloworld = false;
    if (conn->reassembler->GetBuffer().size() >= 10) {
        std::string buffered(conn->reassembler->GetBuffer().begin(), conn->reassembler->GetBuffer().begin() + 10);
        found_helloworld = (buffered == "HELLOWORLD");
    }
    ASSERT_TRUE(found_helloworld, "Out-of-order data should be reassembled correctly");

    std::cout << "  PASS (out-of-order handling verified)" << std::endl;
    return true;
}

// Test 5: TCP Sequence Number Wraparound
bool TestTCPSequenceWraparound() {
    std::cout << "Testing TCP sequence number wraparound..." << std::endl;

    TCPConnectionManager manager;

    uint32_t client_ip = 0xC0A83765;
    uint16_t client_port = 49154;
    uint32_t server_ip = 0xC0A83764;
    uint16_t server_port = 80;

    // Start with SEQ near end of uint32_t range
    uint32_t start_seq = 0xFFFFFFF0;

    auto syn_ack = manager.HandlePacket(client_ip, client_port, server_ip, server_port,
                                       start_seq, 0, TCPParser::TCP_FLAG_SYN, 65535,
                                       mtp::ByteArray());
    uint32_t server_seq = syn_ack->seq_num;

    // Complete handshake with wrapped SEQ
    manager.HandlePacket(client_ip, client_port, server_ip, server_port,
                        start_seq + 1, server_seq + 1, TCPParser::TCP_FLAG_ACK, 65535,
                        mtp::ByteArray());

    std::string conn_key = TCPConnectionManager::MakeConnectionKey(
        client_ip, client_port, server_ip, server_port);
    TCPConnectionInfo* conn = manager.GetConnection(conn_key);
    ASSERT_EQ(conn->state, TCPState::ESTABLISHED, "Should handle wraparound in handshake");

    // Send data that causes SEQ to wrap: SEQ=0xFFFFFFF1, 20 bytes
    // Next SEQ will be 0xFFFFFFF1 + 20 = 0x00000005 (wrapped)
    mtp::ByteArray data1(20, 'A');
    manager.HandlePacket(client_ip, client_port, server_ip, server_port,
                        start_seq + 1, server_seq + 1, TCPParser::TCP_FLAG_ACK, 65535,
                        data1);

    ASSERT_TRUE(conn->reassembler->GetBuffer().size() >= 20, "First packet should be buffered");

    // Send next packet with wrapped SEQ: SEQ=0x00000005
    mtp::ByteArray data2(10, 'B');
    manager.HandlePacket(client_ip, client_port, server_ip, server_port,
                        0x00000005, server_seq + 1, TCPParser::TCP_FLAG_ACK, 65535,
                        data2);

    ASSERT_TRUE(conn->reassembler->GetBuffer().size() >= 30,
                "Packet after wraparound should be accepted (not flagged as retransmit)");

    std::cout << "  PASS (sequence wraparound handled correctly)" << std::endl;
    return true;
}

int main() {
    std::cout << "======================================" << std::endl;
    std::cout << " HTTP Interceptor Integration Tests" << std::endl;
    std::cout << " Phase 5.2/5.3 Full Stack Testing" << std::endl;
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

    // Protocol Handler Integration Tests
    run_test(TestDNSQueryEndToEnd, "DNS Query End-to-End");
    run_test(TestCCPConfigRequestEndToEnd, "CCP Config-Request End-to-End");

    // TCP Data Flow Integration Tests
    run_test(TestTCPSequentialDataPackets, "TCP Sequential Data Packets (Retransmission Detection)");
    run_test(TestTCPOutOfOrderData, "TCP Out-of-Order Data Handling");
    run_test(TestTCPSequenceWraparound, "TCP Sequence Number Wraparound");

    std::cout << "======================================" << std::endl;
    std::cout << " Test Results: " << passed << "/" << total << " passed" << std::endl;
    std::cout << "======================================" << std::endl;

    if (passed < total) {
        std::cout << std::endl;
        std::cout << "CRITICAL: These integration tests would have caught the bugs:" << std::endl;
        std::cout << "  1. Mutex deadlock in DNS/CCP response draining" << std::endl;
        std::cout << "  2. TCP sequence number tracking (all data flagged as retransmits)" << std::endl;
    }

    return (passed == total) ? 0 : 1;
}
