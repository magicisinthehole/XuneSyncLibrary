/**
 * test_dns_handler.cpp
 *
 * Unit tests for DNSHandler (Phase 5.2)
 * Tests DNS request/response handling
 */

#include "lib/src/protocols/handlers/DNSHandler.h"
#include <iostream>
#include <map>

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

// Test: DNS query for www.zunerama.com
bool TestDNSQueryZunerama() {
    std::cout << "Testing DNS query for www.zunerama.com..." << std::endl;

    // Create hostname map
    std::map<std::string, uint32_t> hostname_map = {
        {"www.zunerama.com", 0xC0A83764},  // 192.168.55.100
        {"zune.net", 0xC0A83764}
    };

    DNSHandler handler(hostname_map);

    // Build minimal IP packet containing UDP/DNS query
    // For testing purposes, we'll create a simplified packet structure
    mtp::ByteArray ip_packet;

    // IP header (minimal - 20 bytes)
    ip_packet.insert(ip_packet.end(), {
        0x45, 0x00,  // Version (4) + IHL (5), DSCP + ECN
        0x00, 0x3C,  // Total length (60 bytes)
        0x00, 0x01,  // Identification
        0x00, 0x00,  // Flags + Fragment offset
        0x40, 0x11,  // TTL (64), Protocol (UDP = 17)
        0x00, 0x00,  // Header checksum (not validated)
        0xC0, 0xA8, 0x37, 0x65,  // Source IP: 192.168.55.101
        0xC0, 0xA8, 0x37, 0x64   // Dest IP: 192.168.55.100
    });

    // UDP header (8 bytes)
    ip_packet.insert(ip_packet.end(), {
        0xC0, 0x00,  // Source port: 49152
        0x00, 0x35,  // Dest port: 53 (DNS)
        0x00, 0x28,  // Length: 40 bytes (UDP header + DNS)
        0x00, 0x00   // Checksum (not validated)
    });

    // DNS header (12 bytes)
    ip_packet.insert(ip_packet.end(), {
        0x12, 0x34,  // Transaction ID
        0x01, 0x00,  // Flags (standard query)
        0x00, 0x01,  // Questions: 1
        0x00, 0x00,  // Answer RRs: 0
        0x00, 0x00,  // Authority RRs: 0
        0x00, 0x00   // Additional RRs: 0
    });

    // Question: www.zunerama.com (encoded as DNS name)
    std::vector<uint8_t> domain_name = {
        3, 'w', 'w', 'w',
        8, 'z', 'u', 'n', 'e', 'r', 'a', 'm', 'a',
        3, 'c', 'o', 'm',
        0  // null terminator
    };
    ip_packet.insert(ip_packet.end(), domain_name.begin(), domain_name.end());

    // Type A (1) and Class IN (1)
    ip_packet.insert(ip_packet.end(), {0x00, 0x01, 0x00, 0x01});

    auto response = handler.HandleQuery(ip_packet);

    ASSERT_TRUE(response.has_value(), "Should generate DNS response");
    ASSERT_TRUE(response->size() > 20, "Response should contain IP header + data");

    std::cout << "  PASS (response size: " << response->size() << " bytes)" << std::endl;
    return true;
}

// Test: DNS query for zune.net
bool TestDNSQueryZuneNet() {
    std::cout << "Testing DNS query for zune.net..." << std::endl;

    std::map<std::string, uint32_t> hostname_map = {
        {"www.zunerama.com", 0xC0A83764},
        {"zune.net", 0xC0A83765}  // 192.168.55.101
    };

    DNSHandler handler(hostname_map);

    mtp::ByteArray ip_packet;

    // IP header
    ip_packet.insert(ip_packet.end(), {
        0x45, 0x00, 0x00, 0x30, 0x00, 0x02, 0x00, 0x00,
        0x40, 0x11, 0x00, 0x00,
        0xC0, 0xA8, 0x37, 0x65,
        0xC0, 0xA8, 0x37, 0x64
    });

    // UDP header
    ip_packet.insert(ip_packet.end(), {
        0xC0, 0x01, 0x00, 0x35, 0x00, 0x1C, 0x00, 0x00
    });

    // DNS header
    ip_packet.insert(ip_packet.end(), {
        0xAB, 0xCD, 0x01, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
    });

    // Question: zune.net
    std::vector<uint8_t> domain_name = {
        4, 'z', 'u', 'n', 'e',
        3, 'n', 'e', 't',
        0
    };
    ip_packet.insert(ip_packet.end(), domain_name.begin(), domain_name.end());

    // Type A, Class IN
    ip_packet.insert(ip_packet.end(), {0x00, 0x01, 0x00, 0x01});

    auto response = handler.HandleQuery(ip_packet);

    ASSERT_TRUE(response.has_value(), "Should generate DNS response for zune.net");

    std::cout << "  PASS (response size: " << response->size() << " bytes)" << std::endl;
    return true;
}

// Test: Malformed DNS query (too short)
bool TestMalformedDNSQuery() {
    std::cout << "Testing malformed DNS query..." << std::endl;

    std::map<std::string, uint32_t> hostname_map = {
        {"www.zunerama.com", 0xC0A83764}
    };

    DNSHandler handler(hostname_map);

    // Incomplete IP packet (only 5 bytes)
    mtp::ByteArray malformed_query = {0x45, 0x00, 0x00, 0x00, 0x00};

    auto response = handler.HandleQuery(malformed_query);

    // Handler should either return nullopt or an error response
    // Both are acceptable behaviors for malformed input
    std::cout << "  PASS (handled malformed query gracefully)";
    if (response.has_value()) {
        std::cout << " - returned response of " << response->size() << " bytes";
    } else {
        std::cout << " - returned nullopt";
    }
    std::cout << std::endl;
    return true;
}

// Test: DNS query for unknown domain
bool TestUnknownDomain() {
    std::cout << "Testing DNS query for unknown domain..." << std::endl;

    std::map<std::string, uint32_t> hostname_map = {
        {"www.zunerama.com", 0xC0A83764}
    };

    DNSHandler handler(hostname_map);

    mtp::ByteArray ip_packet;

    // IP header
    ip_packet.insert(ip_packet.end(), {
        0x45, 0x00, 0x00, 0x40, 0x00, 0x03, 0x00, 0x00,
        0x40, 0x11, 0x00, 0x00,
        0xC0, 0xA8, 0x37, 0x65,
        0xC0, 0xA8, 0x37, 0x64
    });

    // UDP header
    ip_packet.insert(ip_packet.end(), {
        0xC0, 0x02, 0x00, 0x35, 0x00, 0x2C, 0x00, 0x00
    });

    // DNS header
    ip_packet.insert(ip_packet.end(), {
        0x99, 0x88, 0x01, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
    });

    // Question: unknown.example.com
    std::vector<uint8_t> domain_name = {
        7, 'u', 'n', 'k', 'n', 'o', 'w', 'n',
        7, 'e', 'x', 'a', 'm', 'p', 'l', 'e',
        3, 'c', 'o', 'm',
        0
    };
    ip_packet.insert(ip_packet.end(), domain_name.begin(), domain_name.end());

    // Type A, Class IN
    ip_packet.insert(ip_packet.end(), {0x00, 0x01, 0x00, 0x01});

    auto response = handler.HandleQuery(ip_packet);

    // Handler behavior for unknown domains may vary - just ensure no crash
    std::cout << "  PASS (handled unknown domain gracefully)";
    if (response.has_value()) {
        std::cout << " - returned response of " << response->size() << " bytes";
    } else {
        std::cout << " - returned nullopt";
    }
    std::cout << std::endl;
    return true;
}

// Test: Update hostname mappings
bool TestUpdateHostnameMap() {
    std::cout << "Testing hostname map updates..." << std::endl;

    std::map<std::string, uint32_t> hostname_map = {
        {"www.zunerama.com", 0xC0A83764}
    };

    DNSHandler handler(hostname_map);

    // Update with new map
    std::map<std::string, uint32_t> new_map = {
        {"www.zunerama.com", 0xC0A83764},
        {"zune.net", 0xC0A83765},
        {"microsoft.com", 0xC0A83766}
    };

    handler.UpdateHostnameMap(new_map);

    std::cout << "  PASS (updated hostname map with " << new_map.size() << " entries)" << std::endl;
    return true;
}

// Test: DNS response structure validation (transaction ID, flags, counts)
bool TestDNSResponseStructure() {
    std::cout << "Testing DNS response structure validation..." << std::endl;

    std::map<std::string, uint32_t> hostname_map = {
        {"www.zunerama.com", 0xC0A83764}
    };
    DNSHandler handler(hostname_map);

    // Build DNS query
    mtp::ByteArray ip_packet;

    // IP header
    ip_packet.insert(ip_packet.end(), {
        0x45, 0x00, 0x00, 0x3C, 0x00, 0x01, 0x00, 0x00,
        0x40, 0x11, 0x00, 0x00,
        0xC0, 0xA8, 0x37, 0x65,
        0xC0, 0xA8, 0x37, 0x64
    });

    // UDP header
    ip_packet.insert(ip_packet.end(), {
        0xC0, 0x00, 0x00, 0x35, 0x00, 0x28, 0x00, 0x00
    });

    // DNS header with transaction ID 0x1234
    ip_packet.insert(ip_packet.end(), {
        0x12, 0x34,  // Transaction ID
        0x01, 0x00,  // Flags (standard query)
        0x00, 0x01,  // Questions: 1
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00
    });

    // Question: www.zunerama.com
    std::vector<uint8_t> domain_name = {
        3, 'w', 'w', 'w',
        8, 'z', 'u', 'n', 'e', 'r', 'a', 'm', 'a',
        3, 'c', 'o', 'm',
        0
    };
    ip_packet.insert(ip_packet.end(), domain_name.begin(), domain_name.end());
    ip_packet.insert(ip_packet.end(), {0x00, 0x01, 0x00, 0x01});

    auto response = handler.HandleQuery(ip_packet);
    ASSERT_TRUE(response.has_value(), "Should generate response");

    // DNSHandler returns PPP-framed response
    // PPP frame: 0x7E | protocol(2-3 bytes) | IP packet | FCS(2) | 0x7E
    // IP packet: IP header(20) | UDP header(8) | DNS data

    // Find the DNS header by searching for transaction ID 0x1234
    bool found_txid = false;
    size_t dns_offset = 0;
    for (size_t i = 0; i + 1 < response->size(); i++) {
        if ((*response)[i] == 0x12 && (*response)[i+1] == 0x34) {
            dns_offset = i;
            found_txid = true;
            break;
        }
    }

    ASSERT_TRUE(found_txid, "Response should contain transaction ID 0x1234");

    // Validate DNS header flags
    uint8_t flags_high = (*response)[dns_offset + 2];
    ASSERT_TRUE((flags_high & 0x80) != 0, "QR flag should be 1 (response)");

    std::cout << "  PASS (transaction ID preserved at offset " << dns_offset << ", QR flag set)" << std::endl;
    return true;
}

// Test: DNS response contains correct IP address
bool TestDNSResponseIPAddress() {
    std::cout << "Testing DNS response contains correct IP..." << std::endl;

    uint32_t expected_ip = 0xC0A83765;  // 192.168.55.101
    std::map<std::string, uint32_t> hostname_map = {
        {"test.example.com", expected_ip}
    };
    DNSHandler handler(hostname_map);

    // Build DNS query for test.example.com
    mtp::ByteArray ip_packet;

    // IP header
    ip_packet.insert(ip_packet.end(), {
        0x45, 0x00, 0x00, 0x40, 0x00, 0x01, 0x00, 0x00,
        0x40, 0x11, 0x00, 0x00,
        0xC0, 0xA8, 0x37, 0x65,
        0xC0, 0xA8, 0x37, 0x64
    });

    // UDP header
    ip_packet.insert(ip_packet.end(), {
        0xC0, 0x00, 0x00, 0x35, 0x00, 0x2C, 0x00, 0x00
    });

    // DNS header
    ip_packet.insert(ip_packet.end(), {
        0xAB, 0xCD, 0x01, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
    });

    // Question: test.example.com
    std::vector<uint8_t> domain = {
        4, 't', 'e', 's', 't',
        7, 'e', 'x', 'a', 'm', 'p', 'l', 'e',
        3, 'c', 'o', 'm',
        0
    };
    ip_packet.insert(ip_packet.end(), domain.begin(), domain.end());
    ip_packet.insert(ip_packet.end(), {0x00, 0x01, 0x00, 0x01});

    auto response = handler.HandleQuery(ip_packet);
    ASSERT_TRUE(response.has_value(), "Should generate response");

    // Search for expected IP in response (should appear in answer section)
    bool found_ip = false;
    for (size_t i = 0; i + 3 < response->size(); i++) {
        if ((*response)[i] == ((expected_ip >> 24) & 0xFF) &&
            (*response)[i+1] == ((expected_ip >> 16) & 0xFF) &&
            (*response)[i+2] == ((expected_ip >> 8) & 0xFF) &&
            (*response)[i+3] == (expected_ip & 0xFF)) {
            found_ip = true;
            break;
        }
    }

    ASSERT_TRUE(found_ip, "Response should contain the resolved IP address (192.168.55.101)");

    std::cout << "  PASS (response contains correct IP)" << std::endl;
    return true;
}

// Test: DNS case insensitivity (RFC 1035 Section 2.3.3 - MUST be case-insensitive)
bool TestDNSCaseInsensitivity() {
    std::cout << "Testing DNS case insensitivity (RFC 1035 Section 2.3.3)..." << std::endl;

    uint32_t expected_ip = 0xC0A83764;  // 192.168.55.100
    std::map<std::string, uint32_t> hostname_map = {
        {"www.zunerama.com", expected_ip}  // Lowercase in map
    };
    DNSHandler handler(hostname_map);

    // Query for WWW.ZUNERAMA.COM (uppercase) - must match lowercase entry
    mtp::ByteArray ip_packet;

    // IP/UDP headers
    ip_packet.insert(ip_packet.end(), {
        0x45, 0x00, 0x00, 0x3C, 0x00, 0x01, 0x00, 0x00,
        0x40, 0x11, 0x00, 0x00,
        0xC0, 0xA8, 0x37, 0x65, 0xC0, 0xA8, 0x37, 0x64,
        0xC0, 0x00, 0x00, 0x35, 0x00, 0x28, 0x00, 0x00
    });

    // DNS header
    ip_packet.insert(ip_packet.end(), {
        0x99, 0x99, 0x01, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
    });

    // Question: WWW.ZUNERAMA.COM (uppercase)
    std::vector<uint8_t> domain_uppercase = {
        3, 'W', 'W', 'W',
        8, 'Z', 'U', 'N', 'E', 'R', 'A', 'M', 'A',
        3, 'C', 'O', 'M',
        0
    };
    ip_packet.insert(ip_packet.end(), domain_uppercase.begin(), domain_uppercase.end());
    ip_packet.insert(ip_packet.end(), {0x00, 0x01, 0x00, 0x01});

    auto response = handler.HandleQuery(ip_packet);

    // RFC 1035 Section 2.3.3: "domain names are case insensitive"
    // Implementation MUST resolve uppercase query to lowercase map entry
    ASSERT_TRUE(response.has_value(), "RFC 1035: DNS MUST be case-insensitive (should resolve)");

    // Verify the response contains the correct IP
    bool found_ip = false;
    for (size_t i = 0; i + 3 < response->size(); i++) {
        if ((*response)[i] == ((expected_ip >> 24) & 0xFF) &&
            (*response)[i+1] == ((expected_ip >> 16) & 0xFF) &&
            (*response)[i+2] == ((expected_ip >> 8) & 0xFF) &&
            (*response)[i+3] == (expected_ip & 0xFF)) {
            found_ip = true;
            break;
        }
    }

    ASSERT_TRUE(found_ip, "Case-insensitive lookup must return correct IP");

    std::cout << "  PASS (RFC 1035 compliant: WWW.ZUNERAMA.COM resolved to www.zunerama.com)" << std::endl;
    return true;
}

// Test: DNS query for unsupported record type (AAAA, MX, etc.)
bool TestUnsupportedRecordType() {
    std::cout << "Testing unsupported DNS record type (RFC 1035)..." << std::endl;

    std::map<std::string, uint32_t> hostname_map = {
        {"www.zunerama.com", 0xC0A83764}
    };
    DNSHandler handler(hostname_map);

    // Build query for AAAA record (IPv6)
    mtp::ByteArray ip_packet;

    // IP/UDP headers
    ip_packet.insert(ip_packet.end(), {
        0x45, 0x00, 0x00, 0x3C, 0x00, 0x01, 0x00, 0x00,
        0x40, 0x11, 0x00, 0x00,
        0xC0, 0xA8, 0x37, 0x65, 0xC0, 0xA8, 0x37, 0x64,
        0xC0, 0x00, 0x00, 0x35, 0x00, 0x28, 0x00, 0x00
    });

    // DNS header
    ip_packet.insert(ip_packet.end(), {
        0xEE, 0xEE, 0x01, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
    });

    // Question: www.zunerama.com Type AAAA (28)
    std::vector<uint8_t> domain = {
        3, 'w', 'w', 'w',
        8, 'z', 'u', 'n', 'e', 'r', 'a', 'm', 'a',
        3, 'c', 'o', 'm',
        0
    };
    ip_packet.insert(ip_packet.end(), domain.begin(), domain.end());
    ip_packet.insert(ip_packet.end(), {
        0x00, 0x1C,  // Type: AAAA (28)
        0x00, 0x01   // Class: IN
    });

    auto response = handler.HandleQuery(ip_packet);

    // RFC 1035: Our simple DNS handler only supports Type A (1) queries
    // For unsupported types, it should return nullopt (no answer)
    ASSERT_FALSE(response.has_value(),
                 "Handler should return nullopt for unsupported record type (AAAA)");

    std::cout << "  PASS (correctly rejected unsupported AAAA query)" << std::endl;
    return true;
}

// Test: Empty hostname query (RFC 1035 - root domain)
bool TestEmptyHostname() {
    std::cout << "Testing empty hostname query (RFC 1035)..." << std::endl;

    std::map<std::string, uint32_t> hostname_map = {
        {"www.zunerama.com", 0xC0A83764}
    };
    DNSHandler handler(hostname_map);

    // Build query with empty hostname (just root label)
    mtp::ByteArray ip_packet;

    // IP/UDP headers
    ip_packet.insert(ip_packet.end(), {
        0x45, 0x00, 0x00, 0x2D, 0x00, 0x01, 0x00, 0x00,
        0x40, 0x11, 0x00, 0x00,
        0xC0, 0xA8, 0x37, 0x65, 0xC0, 0xA8, 0x37, 0x64,
        0xC0, 0x00, 0x00, 0x35, 0x00, 0x19, 0x00, 0x00
    });

    // DNS header
    ip_packet.insert(ip_packet.end(), {
        0xFF, 0xFF, 0x01, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
    });

    // Question: empty (just root)
    ip_packet.insert(ip_packet.end(), {
        0x00,  // Root label (empty hostname)
        0x00, 0x01,  // Type A
        0x00, 0x01   // Class IN
    });

    auto response = handler.HandleQuery(ip_packet);

    // Empty hostname (root domain) is not in our hostname map
    // Handler should return nullopt (no mapping found)
    ASSERT_FALSE(response.has_value(),
                 "Empty hostname (root) should return nullopt (not in hostname map)");

    std::cout << "  PASS (correctly returned nullopt for empty hostname)" << std::endl;
    return true;
}

int main() {
    std::cout << "======================================" << std::endl;
    std::cout << " DNSHandler Unit Tests" << std::endl;
    std::cout << " Phase 5.2 DNS Protocol Handler" << std::endl;
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

    run_test(TestDNSQueryZunerama, "DNS Query for www.zunerama.com");
    run_test(TestDNSQueryZuneNet, "DNS Query for zune.net");
    run_test(TestMalformedDNSQuery, "Malformed DNS Query");
    run_test(TestUnknownDomain, "Unknown Domain");
    run_test(TestUpdateHostnameMap, "Update Hostname Map");

    // DNS Response Validation and Edge Cases
    run_test(TestDNSResponseStructure, "DNS Response Structure Validation");
    run_test(TestDNSResponseIPAddress, "DNS Response Contains Correct IP");
    run_test(TestDNSCaseInsensitivity, "DNS Case Insensitivity (RFC 1035)");
    run_test(TestUnsupportedRecordType, "Unsupported DNS Record Type");
    run_test(TestEmptyHostname, "Empty Hostname Query");

    std::cout << "======================================" << std::endl;
    std::cout << " Test Results: " << passed << "/" << total << " passed" << std::endl;
    std::cout << "======================================" << std::endl;

    return (passed == total) ? 0 : 1;
}
