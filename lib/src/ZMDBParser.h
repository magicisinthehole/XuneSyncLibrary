#ifndef ZMDB_PARSER_H
#define ZMDB_PARSER_H

#include <vector>
#include <string>
#include <cstdint>
#include <mtp/ByteArray.h>

namespace zmdb {

/**
 * ZMDB Binary Format Parser
 *
 * Parses Zune Metadata Database files (469,056 bytes)
 * Structure:
 *   - ZMDB Header (32 bytes)
 *   - ZMed Section (24 bytes)
 *   - ZArr Descriptors (660 bytes, 33 descriptors × 20 bytes each)
 *   - Index Table (variable, ~32KB)
 *   - Data Section (variable, ~438KB)
 */
class ZMDBParser {
public:
    /**
     * ZMDB File Header (32 bytes at offset 0x00)
     */
    struct ZMDBHeader {
        char magic[4];          // "ZMDB"
        uint32_t version;       // 0x01000000 (version 1)
        uint32_t file_size;     // Total size (469,056)
        uint32_t field_0C;      // Unknown (684)
        uint32_t field_10;      // Unknown (2980)
        uint32_t field_14;      // Unknown (468,340)
    };

    /**
     * ZMed Metadata Section (24 bytes at offset 0x20)
     */
    struct ZMedSection {
        char magic[4];          // "ZMed"
        uint32_t version;       // 0x02000000 (version 2)
        uint32_t entry_count;   // Number of metadata entries (2202)
        uint32_t field_2C;      // Unknown (1033)
        uint64_t field_30;      // Checksum/timestamp data
    };

    /**
     * ZArr Array Descriptor (20 bytes each, 33 total)
     * Located at offset 0x38 to 0x2CB
     */
    struct ZArrDescriptor {
        char magic[4];          // "ZArr"
        uint32_t array_id;      // Array type ID
        uint32_t element_count; // Number of elements
        uint32_t capacity;      // Allocated capacity
        uint32_t data_offset;   // Offset to array data in DATA_SECTION
    };

    /**
     * Index Table Entry (8 bytes each)
     * Located at offset 0x2D0 onwards
     */
    struct IndexEntry {
        uint16_t record_id;     // Record identifier
        uint16_t flags;         // Type/control flags
        uint32_t offset;        // Offset to record data in DATA_SECTION
    };

    /**
     * Constructor
     */
    ZMDBParser();

    /**
     * Destructor
     */
    ~ZMDBParser();

    /**
     * Parse a complete ZMDB file
     *
     * @param zmdb_data The raw ZMDB data (469,056 bytes)
     * @return true if parsing succeeded, false otherwise
     */
    bool Parse(const mtp::ByteArray& zmdb_data);

    /**
     * Check if parsing was successful
     */
    bool IsValid() const { return is_valid_; }

    // Accessors
    const ZMDBHeader& GetHeader() const { return header_; }
    const ZMedSection& GetZMedSection() const { return zmed_; }
    const std::vector<ZArrDescriptor>& GetArrayDescriptors() const { return arrays_; }
    const std::vector<IndexEntry>& GetIndexTable() const { return index_table_; }

    /**
     * Get number of metadata records
     */
    size_t GetRecordCount() const { return index_table_.size(); }

    /**
     * Get data section start offset
     */
    size_t GetDataSectionOffset() const { return data_section_offset_; }

    /**
     * Get raw data at offset
     */
    const mtp::ByteArray& GetData() const { return data_; }

    /**
     * Log parsing results for debugging
     */
    void LogStructure() const;

private:
    // Parsing state
    bool is_valid_;
    mtp::ByteArray data_;

    // Parsed structures
    ZMDBHeader header_;
    ZMedSection zmed_;
    std::vector<ZArrDescriptor> arrays_;
    std::vector<IndexEntry> index_table_;
    size_t data_section_offset_;

    // Internal parsing methods
    bool ParseHeader(size_t offset);
    bool ParseZMedSection(size_t offset);
    bool ParseArrayDescriptors(size_t offset, size_t count);
    bool ParseIndexTable(size_t offset);

    // Logging callback
    void Log(const std::string& message) const;
};

} // namespace zmdb

#endif // ZMDB_PARSER_H
