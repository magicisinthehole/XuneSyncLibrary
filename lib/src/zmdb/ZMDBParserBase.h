#pragma once

#include "ZMDBTypes.h"
#include <vector>
#include <cstdint>
#include <map>
#include <optional>

namespace zmdb {

/**
 * Abstract base class for ZMDB parsers.
 *
 * Provides common infrastructure for parsing ZMDB files from different
 * Zune device types (Zune HD, Zune 30/Classic, etc.).
 */
class ZMDBParserBase {
public:
    virtual ~ZMDBParserBase() = default;

    /**
     * Extract complete library from ZMDB data.
     *
     * @param zmdb_data Raw ZMDB file bytes
     * @return Parsed library with all media types
     */
    virtual ZMDBLibrary ExtractLibrary(const std::vector<uint8_t>& zmdb_data) = 0;

protected:
    /**
     * Read record header and data at given offset.
     *
     * ZMDB records have a 4-byte header at offset-4:
     * - Bits 0-23: record_size
     * - Bits 24-30: flags
     * - Bit 31: must be 0 (valid marker)
     *
     * @param data ZMDB file data
     * @param offset Offset to record data (header is at offset-4)
     * @return Record header and data, or nullopt if invalid
     */
    std::optional<std::pair<RecordHeader, std::vector<uint8_t>>>
    read_record_at_offset(const std::vector<uint8_t>& data, size_t offset) const;

    /**
     * Extract atom_id from record data at offset 0.
     *
     * Atom ID structure:
     * - Bits 0-23: entry_id
     * - Bits 24-31: schema_type
     *
     * @param record_data Record data bytes
     * @return atom_id, or 0 if record too small
     */
    uint32_t extract_atom_id(const std::vector<uint8_t>& record_data) const;

    /**
     * Extract schema type from atom_id.
     *
     * @param atom_id 32-bit atom identifier
     * @return Schema type (top 8 bits)
     */
    uint8_t get_schema_type(uint32_t atom_id) const {
        return (atom_id >> 24) & 0xFF;
    }

    /**
     * Extract entry ID from atom_id.
     *
     * @param atom_id 32-bit atom identifier
     * @return Entry ID (lower 24 bits)
     */
    uint32_t get_entry_id(uint32_t atom_id) const {
        return atom_id & 0x00FFFFFF;
    }

    /**
     * Build index table from descriptor 0.
     *
     * Descriptor 0 contains 8-byte entries:
     * [atom_id (4 bytes)][record_offset (4 bytes)]
     *
     * @param data ZMDB file data
     * @param descriptor_offset Offset to descriptor 0 data
     * @param entry_count Number of entries
     * @return Map of atom_id -> record_offset
     */
    std::map<uint32_t, uint32_t> build_index_table(
        const std::vector<uint8_t>& data,
        size_t descriptor_offset,
        uint32_t entry_count
    ) const;

    /**
     * Read null-terminated UTF-8 string.
     *
     * @param data Data buffer
     * @param offset Starting offset
     * @param max_len Maximum length to read
     * @return UTF-8 string
     */
    std::string read_utf8_string(
        const std::vector<uint8_t>& data,
        size_t offset,
        size_t max_len = 256
    ) const;

    /**
     * Read UTF-16LE string until double-null or delimiter.
     *
     * @param data Data buffer
     * @param offset Starting offset
     * @param max_len Maximum length in bytes
     * @return UTF-8 string (converted from UTF-16LE)
     */
    std::string read_utf16le_string(
        const std::vector<uint8_t>& data,
        size_t offset,
        size_t max_len = 512
    ) const;

    // ZMDB file data (set by derived class)
    std::vector<uint8_t> zmdb_data_;

    // Index table (atom_id -> record_offset)
    std::map<uint32_t, uint32_t> index_table_;
};

} // namespace zmdb
