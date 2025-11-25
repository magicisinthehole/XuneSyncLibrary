#pragma once

#include <mtp/ByteArray.h>
#include <map>
#include <string>
#include <optional>
#include <functional>

/**
 * DNSHandler
 *
 * Handles DNS queries from Zune device for hostname resolution.
 *
 * The Zune device sends DNS queries via UDP (port 53) wrapped in IP/PPP frames.
 * This handler extracts the DNS query, builds a response using configured hostname
 * mappings, and returns a complete PPP-framed UDP/IP/DNS response packet.
 *
 * This is a stateless handler following Clean Architecture principles.
 */
class DNSHandler {
public:
    using LogCallback = std::function<void(const std::string& message)>;

    /**
     * Constructor
     * @param hostname_map Map of hostnames to IP addresses for DNS resolution
     */
    explicit DNSHandler(const std::map<std::string, uint32_t>& hostname_map);

    /**
     * Handle UDP DNS query
     *
     * @param ip_packet Complete IP packet containing UDP DNS query
     * @return PPP-framed DNS response if query can be resolved, std::nullopt otherwise
     */
    std::optional<mtp::ByteArray> HandleQuery(const mtp::ByteArray& ip_packet);

    /**
     * Check if TCP buffer contains a Zune TCP DNS query
     * Zune uses custom 8-byte framing: [ID1][0x0035][LEN][0x0000][DNS message]
     *
     * @param buffer TCP stream reassembly buffer
     * @return true if buffer starts with Zune DNS framing
     */
    bool IsZuneTCPDNSQuery(const mtp::ByteArray& buffer) const;

    /**
     * Handle Zune TCP DNS query with custom framing
     * Zune uses custom 8-byte framing: [ID1][0x0035][LEN][0x0000][DNS message]
     *
     * @param buffer TCP stream reassembly buffer
     * @param bytes_consumed Output: number of bytes consumed from buffer
     * @return TCP payload for response (with Zune framing), or empty if incomplete/failed
     */
    std::optional<mtp::ByteArray> HandleTCPQuery(const mtp::ByteArray& buffer, size_t& bytes_consumed);

    /**
     * Set logging callback for diagnostic messages
     * @param callback Function to receive log messages
     */
    void SetLogCallback(LogCallback callback);

    /**
     * Update hostname mappings
     * @param hostname_map New map of hostnames to IP addresses
     */
    void UpdateHostnameMap(const std::map<std::string, uint32_t>& hostname_map);

private:
    /**
     * Log a message via callback if set
     */
    void Log(const std::string& message);

    std::map<std::string, uint32_t> hostname_map_;
    LogCallback log_callback_;
};
