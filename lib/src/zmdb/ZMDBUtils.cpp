#include "ZMDBUtils.h"
#include <cstring>
#include <algorithm>
#include <sstream>
#include <iomanip>

namespace zmdb {

std::vector<BackwardsVarintField> parse_backwards_varints(
    const std::vector<uint8_t>& record_data,
    size_t entry_size
) {
    std::vector<BackwardsVarintField> fields;

    if (record_data.size() <= entry_size) {
        return fields;  // No varint section
    }

    // Start from end of record and read backwards
    size_t pos = record_data.size() - 1;

    while (pos >= entry_size) {
        // Read field_id (1-2 bytes)
        if (pos < entry_size) break;

        uint8_t field_id_byte1 = record_data[pos--];

        if (field_id_byte1 == 0) {  // End marker
            break;
        }

        uint32_t field_id = field_id_byte1;

        if (field_id_byte1 & 0x80) {  // Multi-byte encoding
            if (pos < entry_size) break;
            uint8_t field_id_byte2 = record_data[pos--];
            field_id = (field_id_byte2 << 7) | (field_id_byte1 & 0x7F);
        }

        // Read field_size (1-3 bytes)
        if (pos < entry_size) break;

        uint8_t size_byte1 = record_data[pos--];
        uint32_t field_size = size_byte1;

        if (size_byte1 & 0x80) {  // Multi-byte encoding
            if (pos < entry_size) break;
            uint8_t size_byte2 = record_data[pos--];
            field_size = (size_byte2 << 7) | (size_byte1 & 0x7F);

            if (size_byte2 != 0) {  // 3-byte encoding
                if (pos < entry_size) break;
                uint8_t size_byte3 = record_data[pos--];
                field_size = (size_byte3 << 14) | (field_size & 0x3FFF);
            }
        }

        // Extract field data
        size_t field_end = pos + 1;
        if (field_size > field_end || field_end - field_size < entry_size) {
            break;  // Invalid field size
        }

        size_t field_start = field_end - field_size;

        BackwardsVarintField field;
        field.field_id = field_id;
        field.field_size = field_size;
        field.offset = field_start;
        field.field_data.assign(
            record_data.begin() + field_start,
            record_data.begin() + field_end
        );

        fields.push_back(field);

        pos = field_start - 1;
    }

    // Reverse to get original order
    std::reverse(fields.begin(), fields.end());

    return fields;
}

size_t get_entry_size_for_schema(uint8_t schema_type) {
    // Based on Python parser analysis and ZMDB wiki documentation
    switch (schema_type) {
        case Schema::Music:         return 32;  // 0x01
        case Schema::Video:         return 32;  // 0x02 (variable, use conservative estimate)
        case Schema::Picture:       return 24;  // 0x03
        case Schema::Filename:      return 8;   // 0x05
        case Schema::Album:         return 20;  // 0x06
        case Schema::Playlist:      return 12;  // 0x07
        case Schema::Artist:        return 4;   // 0x08
        case Schema::Genre:         return 1;   // 0x09
        case Schema::VideoTitle:    return 4;   // 0x0a
        case Schema::PhotoAlbum:    return 12;  // 0x0b
        case Schema::Collection:    return 12;  // 0x0c
        case Schema::PodcastShow:   return 8;   // 0x0f
        case Schema::PodcastEpisode: return 32; // 0x10
        default:
            return 0;  // Unknown schema
    }
}

std::string utf16le_to_utf8(const std::vector<uint8_t>& data) {
    std::string result;
    result.reserve(data.size() / 2);  // Estimate

    for (size_t i = 0; i + 1 < data.size(); i += 2) {
        uint16_t code_unit = data[i] | (data[i + 1] << 8);

        if (code_unit == 0) {
            break;  // Null terminator
        }

        // Basic UTF-16 to UTF-8 conversion (BMP only)
        if (code_unit < 0x80) {
            result += static_cast<char>(code_unit);
        } else if (code_unit < 0x800) {
            result += static_cast<char>(0xC0 | (code_unit >> 6));
            result += static_cast<char>(0x80 | (code_unit & 0x3F));
        } else {
            result += static_cast<char>(0xE0 | (code_unit >> 12));
            result += static_cast<char>(0x80 | ((code_unit >> 6) & 0x3F));
            result += static_cast<char>(0x80 | (code_unit & 0x3F));
        }
    }

    return result;
}

std::string read_null_terminated_utf8(
    const std::vector<uint8_t>& data,
    size_t offset,
    size_t max_length
) {
    if (offset >= data.size()) {
        return "";
    }

    size_t end = offset;
    size_t limit = std::min(offset + max_length, data.size());

    while (end < limit && data[end] != 0) {
        end++;
    }

    if (end == offset) {
        return "";  // Empty string or immediate null
    }

    return std::string(
        reinterpret_cast<const char*>(&data[offset]),
        end - offset
    );
}

std::string read_utf16le_until_double_null(
    const std::vector<uint8_t>& data,
    size_t offset,
    size_t max_length
) {
    if (offset + 1 >= data.size()) {
        return "";
    }

    std::vector<uint8_t> utf16_data;
    size_t pos = offset;
    size_t limit = std::min(offset + max_length, data.size() - 1);

    while (pos + 1 < limit) {
        uint16_t code_unit = data[pos] | (data[pos + 1] << 8);

        if (code_unit == 0) {
            break;  // Double-null terminator
        }

        utf16_data.push_back(data[pos]);
        utf16_data.push_back(data[pos + 1]);
        pos += 2;
    }

    // Strip leading/trailing null bytes (padding)
    while (!utf16_data.empty() && utf16_data.front() == 0) {
        utf16_data.erase(utf16_data.begin());
    }
    while (!utf16_data.empty() && utf16_data.back() == 0) {
        utf16_data.pop_back();
    }

    return utf16le_to_utf8(utf16_data);
}

uint32_t read_uint32_le(const std::vector<uint8_t>& data, size_t offset) {
    if (offset + 4 > data.size()) {
        return 0;
    }

    uint32_t value;
    std::memcpy(&value, &data[offset], 4);
    return value;  // Assuming little-endian host
}

uint64_t read_uint64_le(const std::vector<uint8_t>& data, size_t offset) {
    if (offset + 8 > data.size()) {
        return 0;
    }

    uint64_t value;
    std::memcpy(&value, &data[offset], 8);
    return value;  // Assuming little-endian host
}

int32_t read_int32_le(const std::vector<uint8_t>& data, size_t offset) {
    if (offset + 4 > data.size()) {
        return 0;
    }

    int32_t value;
    std::memcpy(&value, &data[offset], 4);
    return value;  // Assuming little-endian host
}

uint16_t read_uint16_le(const std::vector<uint8_t>& data, size_t offset) {
    if (offset + 2 > data.size()) {
        return 0;
    }

    uint16_t value;
    std::memcpy(&value, &data[offset], 2);
    return value;  // Assuming little-endian host
}

std::string parse_windows_guid(const std::vector<uint8_t>& data) {
    if (data.size() < 16) {
        return "";
    }

    // Parse Windows GUID structure (little-endian for first 3 fields)
    uint32_t data1 = read_uint32_le(data, 0);
    uint16_t data2 = read_uint16_le(data, 4);
    uint16_t data3 = read_uint16_le(data, 6);

    // Format as GUID string: xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx
    std::stringstream ss;
    ss << std::hex << std::setfill('0')
       << std::setw(8) << data1 << "-"
       << std::setw(4) << data2 << "-"
       << std::setw(4) << data3 << "-"
       << std::setw(2) << static_cast<int>(data[8])
       << std::setw(2) << static_cast<int>(data[9]) << "-";

    for (int i = 10; i < 16; i++) {
        ss << std::setw(2) << static_cast<int>(data[i]);
    }

    return ss.str();
}

} // namespace zmdb
