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

// Backwards-varint field IDs used in the variable section of PodcastShow
// (0x0f), PodcastEpisode (0x10), and video-podcast Video (0x02) records.
namespace PodcastFieldId {
    constexpr uint8_t Description = 0x41;  // episodes only
    constexpr uint8_t Filename    = 0x44;  // shows and video episodes
    constexpr uint8_t Url         = 0x45;  // feed URL on shows, download URL on episodes
    constexpr uint8_t Author      = 0x46;  // shows and episodes
    constexpr uint8_t Constant    = 0x1e;  // shows only, UINT32=1
}

// Backwards-varint field IDs used on non-podcast Video records (Schema 0x02).
namespace VideoFieldId {
    constexpr uint8_t Description    = 0x41;  // UTF-16LE
    constexpr uint8_t Filename       = 0x44;  // UTF-16LE
    constexpr uint8_t Artist         = 0x46;  // UTF-16LE (Music category)
    constexpr uint8_t Season         = 0x4f;  // u32 (Series category)
    constexpr uint8_t Episode        = 0x50;  // u32 (Series category)
    constexpr uint8_t Unknown1e      = 0x1e;  // u32, always =1 in observed data
    constexpr uint8_t OnDevicePlays  = 0x62;  // u32 (shared with audio tracks)
    constexpr uint8_t LastPlayed     = 0x70;  // u64 FILETIME (shared with audio tracks)
}

/**
 * Walk the backwards-varint section of a Video record and populate the
 * relevant ZMDBVideo fields (filename, description, artist, season, episode,
 * on_device_playcount, last_played_timestamp). Called by both Classic and HD
 * parsers — the trailing-section format is family-independent.
 *
 * @param record_data Complete video record data
 * @param video Output struct; fields corresponding to discovered varints set
 */
void parse_video_trailing_fields(
    const std::vector<uint8_t>& record_data,
    ZMDBVideo& video
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
