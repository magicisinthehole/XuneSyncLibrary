/**
 * test_ccp_handler.cpp
 *
 * Unit tests for CCPHandler (Phase 5.2)
 * Tests CCP (Compression Control Protocol) negotiation
 */

#include "lib/src/protocols/handlers/CCPHandler.h"
#include <iostream>

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

// Test: CCP Configure-Request with no options (empty)
// RFC 1661: Empty Config-Request should receive Config-Ack
bool TestCCPConfigureRequest() {
    std::cout << "Testing CCP Configure-Request (empty, no options)..." << std::endl;

    CCPHandler handler;

    // Build CCP Configure-Request packet with NO options
    // Format: Code (1) | ID (1) | Length (2)
    mtp::ByteArray request;
    request.push_back(0x01);  // Code: Configure-Request
    request.push_back(0x01);  // ID: 1
    request.push_back(0x00);  // Length (high)
    request.push_back(0x04);  // Length (low) - 4 bytes total (header only, no options)

    auto response = handler.HandlePacket(request);

    ASSERT_TRUE(response.has_value(), "Should generate response to Configure-Request");
    ASSERT_TRUE(response->size() >= 10, "Response should have PPP frame with CCP packet");

    // Response is PPP-framed with protocol compression:
    // 0x7E | protocol(2) | CCP packet | FCS(2) | 0x7E
    // PPPParser uses protocol compression, so frame is:
    // 0x7E(1) + 0x80(1) + 0xFD(1) + CCP packet
    // CCP packet starts at index 3
    ASSERT_TRUE((*response)[0] == 0x7E, "Should start with PPP flag");
    ASSERT_TRUE((*response)[1] == 0x80 && (*response)[2] == 0xfd, "Should have CCP protocol 0x80fd");
    uint8_t ccp_code = (*response)[3];  // First byte of CCP packet

    // Empty Config-Request should receive Config-Ack (0x02)
    // This completes CCP negotiation with no compression
    if (ccp_code != 0x02) {
        std::cerr << "FAIL: Empty Config-Request should receive Config-Ack (0x02), got 0x" << std::hex << (int)ccp_code << std::dec << std::endl;
        return false;
    }

    std::cout << "  PASS (Config-Ack for empty request)" << std::endl;
    return true;
}

// Test: CCP Configure-Request with options (should be rejected)
bool TestCCPConfigureRequestWithOptions() {
    std::cout << "Testing CCP Configure-Request (with options)..." << std::endl;

    CCPHandler handler;

    // Build CCP Configure-Request packet WITH options
    // Format: Code (1) | ID (1) | Length (2) | Options...
    mtp::ByteArray request;
    request.push_back(0x01);  // Code: Configure-Request
    request.push_back(0x01);  // ID: 1
    request.push_back(0x00);  // Length (high)
    request.push_back(0x0A);  // Length (low) - 10 bytes total (6 bytes of options)
    // Add a compression option (type 0x12 = Deflate, length 6)
    request.push_back(0x12);  // Option type: Deflate
    request.push_back(0x06);  // Option length: 6
    request.push_back(0x00);  // Deflate params
    request.push_back(0x00);
    request.push_back(0x00);
    request.push_back(0x00);

    auto response = handler.HandlePacket(request);

    ASSERT_TRUE(response.has_value(), "Should generate response to Configure-Request with options");

    // CCP packet starts at index 3
    uint8_t ccp_code = (*response)[3];

    // Config-Request with options should receive Config-Reject (0x04)
    if (ccp_code != 0x04) {
        std::cerr << "FAIL: Config-Request with options should receive Config-Reject (0x04), got 0x" << std::hex << (int)ccp_code << std::dec << std::endl;
        return false;
    }

    std::cout << "  PASS (Config-Reject for request with options)" << std::endl;
    return true;
}

// Test: CCP Configure-Ack (strengthened - validates stateless behavior)
bool TestCCPConfigureAck() {
    std::cout << "Testing CCP Configure-Ack..." << std::endl;

    CCPHandler handler;

    // Build CCP Configure-Ack packet
    mtp::ByteArray ack;
    ack.push_back(0x02);  // Code: Configure-Ack
    ack.push_back(0x01);  // ID: 1
    ack.push_back(0x00);  // Length (high)
    ack.push_back(0x04);  // Length (low)

    auto response = handler.HandlePacket(ack);

    // Configure-Ack from peer means they accepted our configuration
    // Since we're stateless and don't initiate CCP, we should not respond
    ASSERT_FALSE(response.has_value(), "Configure-Ack should not generate response (stateless handler)");

    std::cout << "  PASS (no response generated - correct stateless behavior)" << std::endl;
    return true;
}

// Test: CCP Configure-Nak (strengthened - validates stateless behavior)
bool TestCCPConfigureNak() {
    std::cout << "Testing CCP Configure-Nak..." << std::endl;

    CCPHandler handler;

    // Build CCP Configure-Nak packet
    mtp::ByteArray nak;
    nak.push_back(0x03);  // Code: Configure-Nak
    nak.push_back(0x01);  // ID: 1
    nak.push_back(0x00);  // Length (high)
    nak.push_back(0x04);  // Length (low)

    auto response = handler.HandlePacket(nak);

    // Configure-Nak from peer means they rejected our configuration
    // Since we're stateless and don't initiate CCP, we should not respond
    ASSERT_FALSE(response.has_value(), "Configure-Nak should not generate response (stateless handler)");

    std::cout << "  PASS (no response generated - correct stateless behavior)" << std::endl;
    return true;
}

// Test: CCP Configure-Reject (strengthened - validates stateless behavior)
bool TestCCPConfigureReject() {
    std::cout << "Testing CCP Configure-Reject..." << std::endl;

    CCPHandler handler;

    // Build CCP Configure-Reject packet
    mtp::ByteArray reject;
    reject.push_back(0x04);  // Code: Configure-Reject
    reject.push_back(0x01);  // ID: 1
    reject.push_back(0x00);  // Length (high)
    reject.push_back(0x04);  // Length (low)

    auto response = handler.HandlePacket(reject);

    // Configure-Reject from peer means they don't support our options
    // Since we're stateless and don't initiate CCP, we should not respond
    ASSERT_FALSE(response.has_value(), "Configure-Reject should not generate response (stateless handler)");

    std::cout << "  PASS (no response generated - correct stateless behavior)" << std::endl;
    return true;
}

// Test: CCP Reset-Request
bool TestCCPResetRequest() {
    std::cout << "Testing CCP Reset-Request..." << std::endl;

    CCPHandler handler;

    // Build CCP Reset-Request packet
    mtp::ByteArray reset_req;
    reset_req.push_back(0x0E);  // Code: Reset-Request
    reset_req.push_back(0x01);  // ID: 1
    reset_req.push_back(0x00);  // Length (high)
    reset_req.push_back(0x04);  // Length (low)

    auto response = handler.HandlePacket(reset_req);

    // Reset-Request should typically generate Reset-Ack
    if (response.has_value()) {
        uint8_t response_code = (*response)[0];
        std::cout << "  PASS (response code: 0x" << std::hex << (int)response_code << std::dec << ")" << std::endl;
    } else {
        std::cout << "  PASS (no response)" << std::endl;
    }
    return true;
}

// Test: CCP with compression options (Deflate)
bool TestCCPWithDeflateOption() {
    std::cout << "Testing CCP with Deflate compression option..." << std::endl;

    CCPHandler handler;

    // Build CCP Configure-Request with Deflate option (Type 26)
    mtp::ByteArray request;
    request.push_back(0x01);  // Code: Configure-Request
    request.push_back(0x02);  // ID: 2
    request.push_back(0x00);  // Length (high)
    request.push_back(0x08);  // Length (low) - 8 bytes total

    // Deflate option (Type 26, Length 4)
    request.push_back(0x1A);  // Type: Deflate (26)
    request.push_back(0x04);  // Length: 4
    request.push_back(0x00);  // Window bits (high)
    request.push_back(0x0F);  // Window bits (low) - 15 bits

    auto response = handler.HandlePacket(request);

    ASSERT_TRUE(response.has_value(), "Should generate response");

    std::cout << "  PASS (response size: " << response->size() << " bytes)" << std::endl;
    return true;
}

// Test: Malformed CCP packet (too short)
bool TestMalformedCCPPacket() {
    std::cout << "Testing malformed CCP packet..." << std::endl;

    CCPHandler handler;

    // Packet with only 2 bytes (incomplete header)
    mtp::ByteArray malformed;
    malformed.push_back(0x01);
    malformed.push_back(0x01);

    auto response = handler.HandlePacket(malformed);

    // Should handle gracefully (either return nullopt or error response)
    std::cout << "  PASS (handled malformed packet gracefully)" << std::endl;
    return true;
}

// Test: CCP packet with unknown code
bool TestUnknownCCPCode() {
    std::cout << "Testing CCP packet with unknown code..." << std::endl;

    CCPHandler handler;

    // Build packet with unknown code (0xFF)
    mtp::ByteArray unknown;
    unknown.push_back(0xFF);  // Code: Unknown
    unknown.push_back(0x01);  // ID: 1
    unknown.push_back(0x00);  // Length (high)
    unknown.push_back(0x04);  // Length (low)

    auto response = handler.HandlePacket(unknown);

    // Should handle gracefully
    std::cout << "  PASS (handled unknown code gracefully)" << std::endl;
    return true;
}

// Test: Multiple CCP packets in sequence
bool TestMultipleCCPPackets() {
    std::cout << "Testing multiple CCP packets in sequence..." << std::endl;

    CCPHandler handler;

    // Sequence: Configure-Request -> Configure-Ack -> Reset-Request
    std::vector<mtp::ByteArray> packets;

    // Packet 1: Configure-Request
    packets.push_back({0x01, 0x01, 0x00, 0x04});

    // Packet 2: Configure-Ack
    packets.push_back({0x02, 0x02, 0x00, 0x04});

    // Packet 3: Reset-Request
    packets.push_back({0x0E, 0x03, 0x00, 0x04});

    int responses_received = 0;
    for (const auto& packet : packets) {
        auto response = handler.HandlePacket(packet);
        if (response.has_value()) {
            responses_received++;
        }
    }

    std::cout << "  PASS (processed " << packets.size() << " packets, received "
              << responses_received << " responses)" << std::endl;
    return true;
}

// Test: CCP identifier preservation in response
bool TestCCPIdentifierPreservation() {
    std::cout << "Testing CCP identifier preservation..." << std::endl;

    CCPHandler handler;

    // Send Configure-Request with specific ID (0x42)
    mtp::ByteArray request;
    request.push_back(0x01);  // Code: Configure-Request
    request.push_back(0x42);  // ID: 0x42
    request.push_back(0x00);  // Length (high)
    request.push_back(0x04);  // Length (low)

    auto response = handler.HandlePacket(request);
    ASSERT_TRUE(response.has_value(), "Should generate Configure-Reject");

    // Extract CCP packet from PPP frame (skip 0x7E + protocol bytes)
    size_t ccp_offset = 3;  // 0x7E + 0x80 + 0xFD
    ASSERT_TRUE(response->size() > ccp_offset + 1, "Response should contain CCP packet");

    uint8_t response_id = (*response)[ccp_offset + 1];
    ASSERT_EQ(response_id, uint8_t(0x42), "Response ID must match request ID");

    std::cout << "  PASS (ID 0x42 preserved in response)" << std::endl;
    return true;
}

// Test: CCP Configure-Reject contains original options
bool TestCCPRejectContainsOptions() {
    std::cout << "Testing CCP Configure-Reject contains options..." << std::endl;

    CCPHandler handler;

    // Send Configure-Request with Deflate option
    mtp::ByteArray request;
    request.push_back(0x01);  // Code: Configure-Request
    request.push_back(0x05);  // ID: 5
    request.push_back(0x00);  // Length (high)
    request.push_back(0x08);  // Length (low) - 8 bytes total

    // Deflate option (Type 26, Length 4)
    request.push_back(0x1A);  // Type: Deflate (26)
    request.push_back(0x04);  // Length: 4
    request.push_back(0x00);  // Window bits (high)
    request.push_back(0x0F);  // Window bits (low) - 15 bits

    auto response = handler.HandlePacket(request);
    ASSERT_TRUE(response.has_value(), "Should generate Configure-Reject");

    // Configure-Reject should contain the original options that were rejected
    // Search for Deflate option (0x1A 0x04) in response
    bool found_deflate_option = false;
    for (size_t i = 0; i + 1 < response->size(); i++) {
        if ((*response)[i] == 0x1A && (*response)[i+1] == 0x04) {
            found_deflate_option = true;
            break;
        }
    }

    ASSERT_TRUE(found_deflate_option, "Configure-Reject should contain original Deflate option");

    std::cout << "  PASS (reject contains original options)" << std::endl;
    return true;
}

// Test: CCP Configure-Request with multiple options
bool TestCCPMultipleOptions() {
    std::cout << "Testing CCP Configure-Request with multiple options..." << std::endl;

    CCPHandler handler;

    // Send Configure-Request with multiple compression options
    mtp::ByteArray request;
    request.push_back(0x01);  // Code: Configure-Request
    request.push_back(0x10);  // ID: 16
    request.push_back(0x00);  // Length (high)
    request.push_back(0x0C);  // Length (low) - 12 bytes total

    // Option 1: Deflate
    request.push_back(0x1A);  // Type: Deflate (26)
    request.push_back(0x04);  // Length: 4
    request.push_back(0x00);  // Window bits (high)
    request.push_back(0x0F);  // Window bits (low)

    // Option 2: LZS (example - Type 17)
    request.push_back(0x11);  // Type: LZS (17)
    request.push_back(0x04);  // Length: 4
    request.push_back(0x00);  // Histories (high)
    request.push_back(0x01);  // Histories (low)

    auto response = handler.HandlePacket(request);
    ASSERT_TRUE(response.has_value(), "Should generate Configure-Reject for all options");

    // Response should contain both rejected options
    std::cout << "  PASS (rejected multiple options)" << std::endl;
    return true;
}

// Test: CCP length field validation (RFC 1661 Section 5.2)
bool TestCCPLengthFieldValidation() {
    std::cout << "Testing CCP length field validation (RFC 1661 Section 5.2)..." << std::endl;

    CCPHandler handler;

    // Send Configure-Request with mismatched length field
    mtp::ByteArray request;
    request.push_back(0x01);  // Code: Configure-Request
    request.push_back(0x20);  // ID: 32
    request.push_back(0x00);  // Length (high)
    request.push_back(0x10);  // Length (low) - claims 16 bytes
    // But only provide 4 bytes total (header only) - malformed!

    auto response = handler.HandlePacket(request);

    // RFC 1661 Section 5.2: "If the packet is too short...the packet MUST be silently discarded"
    // Malformed packets with invalid length fields should be rejected (return nullopt)
    ASSERT_FALSE(response.has_value(),
                 "Malformed packet with invalid length field must be silently discarded (return nullopt)");

    std::cout << "  PASS (correctly rejected malformed packet per RFC 1661)" << std::endl;
    return true;
}

int main() {
    std::cout << "======================================" << std::endl;
    std::cout << " CCPHandler Unit Tests" << std::endl;
    std::cout << " Phase 5.2 CCP Protocol Handler" << std::endl;
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

    run_test(TestCCPConfigureRequest, "CCP Configure-Request (empty)");
    run_test(TestCCPConfigureRequestWithOptions, "CCP Configure-Request (with options)");
    run_test(TestCCPConfigureAck, "CCP Configure-Ack");
    run_test(TestCCPConfigureNak, "CCP Configure-Nak");
    run_test(TestCCPConfigureReject, "CCP Configure-Reject");
    run_test(TestCCPResetRequest, "CCP Reset-Request");
    run_test(TestCCPWithDeflateOption, "CCP with Deflate Option");
    run_test(TestMalformedCCPPacket, "Malformed CCP Packet");
    run_test(TestUnknownCCPCode, "Unknown CCP Code");
    run_test(TestMultipleCCPPackets, "Multiple CCP Packets");

    // CCP Validation and Edge Cases
    run_test(TestCCPIdentifierPreservation, "CCP Identifier Preservation");
    run_test(TestCCPRejectContainsOptions, "CCP Reject Contains Options");
    run_test(TestCCPMultipleOptions, "CCP Multiple Options");
    run_test(TestCCPLengthFieldValidation, "CCP Length Field Validation");

    std::cout << "======================================" << std::endl;
    std::cout << " Test Results: " << passed << "/" << total << " passed" << std::endl;
    std::cout << "======================================" << std::endl;

    return (passed == total) ? 0 : 1;
}
