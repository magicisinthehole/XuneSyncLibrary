#pragma once

#include "ZMDBTypes.h"
#include <vector>
#include <cstdint>
#include <optional>

namespace zmdb {

/**
 * Parse backwards varints from end of record.
 *
 * ZMDB uses variable-length integer encoding for optional fields stored
 * at the end of records. Fields are read backwards from record_end towards entry_size.
 *
 * Format (reading backwards):
 * [field_data...] [field_size varint] [field_id varint] [next field...]
 *
 * @param record_data Complete record data
 * @param entry_size Size of fixed/comparable section (varints start here)
 * @return Vector of parsed fields
 */
std::vector<BackwardsVarintField> parse_backwards_varints(
    const std::vector<uint8_t>& record_data,
    size_t entry_size
);

/**
 * Get entry size for a given schema type.
 *
 * Entry size defines where the backwards varint section begins.
 *
 * @param schema_type Schema type (0x01-0x10, etc.)
 * @return Entry size in bytes, or 0 if unknown schema
 */
size_t get_entry_size_for_schema(uint8_t schema_type);

/**
 * Convert UTF-16LE bytes to UTF-8 string.
 *
 * @param data UTF-16LE encoded bytes
 * @return UTF-8 string
 */
std::string utf16le_to_utf8(const std::vector<uint8_t>& data);

/**
 * Read null-terminated UTF-8 string from buffer.
 *
 * @param data Buffer
 * @param offset Starting offset
 * @param max_length Maximum bytes to read
 * @return UTF-8 string (empty if null found immediately or out of bounds)
 */
std::string read_null_terminated_utf8(
    const std::vector<uint8_t>& data,
    size_t offset,
    size_t max_length = 256
);

/**
 * Read UTF-16LE string until double-null terminator.
 *
 * @param data Buffer
 * @param offset Starting offset
 * @param max_length Maximum bytes to read
 * @return UTF-8 string (converted from UTF-16LE)
 */
std::string read_utf16le_until_double_null(
    const std::vector<uint8_t>& data,
    size_t offset,
    size_t max_length = 512
);

/**
 * Read uint32 little-endian from buffer.
 *
 * @param data Buffer
 * @param offset Offset to read from
 * @return uint32 value, or 0 if out of bounds
 */
uint32_t read_uint32_le(const std::vector<uint8_t>& data, size_t offset);

/**
 * Read uint64 little-endian from buffer.
 *
 * @param data Buffer
 * @param offset Offset to read from
 * @return uint64 value, or 0 if out of bounds
 */
uint64_t read_uint64_le(const std::vector<uint8_t>& data, size_t offset);

/**
 * Read int32 little-endian from buffer (signed).
 *
 * @param data Buffer
 * @param offset Offset to read from
 * @return int32 value, or 0 if out of bounds
 */
int32_t read_int32_le(const std::vector<uint8_t>& data, size_t offset);

/**
 * Read uint16 little-endian from buffer.
 *
 * @param data Buffer
 * @param offset Offset to read from
 * @return uint16 value, or 0 if out of bounds
 */
uint16_t read_uint16_le(const std::vector<uint8_t>& data, size_t offset);

/**
 * Parse Windows GUID from 16 bytes to string format.
 *
 * Windows GUID format (little-endian first 3 fields):
 * - Data1: 4 bytes (uint32, LE)
 * - Data2: 2 bytes (uint16, LE)
 * - Data3: 2 bytes (uint16, LE)
 * - Data4: 8 bytes (2+6 bytes, big-endian)
 *
 * Output format: "xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx"
 *
 * @param data 16-byte GUID data
 * @return GUID string or empty string if invalid
 */
std::string parse_windows_guid(const std::vector<uint8_t>& data);

} // namespace zmdb
