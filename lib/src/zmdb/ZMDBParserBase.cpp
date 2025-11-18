#include "ZMDBParserBase.h"
#include "ZMDBUtils.h"
#include <cstring>

namespace zmdb {

std::optional<std::pair<RecordHeader, std::vector<uint8_t>>>
ZMDBParserBase::read_record_at_offset(const std::vector<uint8_t>& data, size_t offset) const {
    if (offset < 4 || offset >= data.size()) {
        return std::nullopt;
    }

    // Read 4-byte header at offset-4
    uint32_t header_value = read_uint32_le(data, offset - 4);

    // Check sign bit (bit 31 must be 0)
    if (header_value & 0x80000000) {
        return std::nullopt;
    }

    RecordHeader header;
    header.record_size = header_value & 0x00FFFFFF;  // 24 bits
    header.flags = (header_value >> 24) & 0xFF;      // 8 bits

    // Read record data
    if (offset + header.record_size > data.size()) {
        return std::nullopt;
    }

    std::vector<uint8_t> record_data(
        data.begin() + offset,
        data.begin() + offset + header.record_size
    );

    return std::make_pair(header, record_data);
}

uint32_t ZMDBParserBase::extract_atom_id(const std::vector<uint8_t>& record_data) const {
    if (record_data.size() < 4) {
        return 0;
    }
    return read_uint32_le(record_data, 0);
}

std::map<uint32_t, uint32_t> ZMDBParserBase::build_index_table(
    const std::vector<uint8_t>& data,
    size_t descriptor_offset,
    uint32_t entry_count
) const {
    std::map<uint32_t, uint32_t> index;

    for (uint32_t i = 0; i < entry_count; i++) {
        size_t entry_offset = descriptor_offset + (i * 8);

        if (entry_offset + 8 > data.size()) {
            break;
        }

        uint32_t atom_id = read_uint32_le(data, entry_offset);
        uint32_t record_offset = read_uint32_le(data, entry_offset + 4);

        index[atom_id] = record_offset;
    }

    return index;
}

std::string ZMDBParserBase::read_utf8_string(
    const std::vector<uint8_t>& data,
    size_t offset,
    size_t max_len
) const {
    return read_null_terminated_utf8(data, offset, max_len);
}

std::string ZMDBParserBase::read_utf16le_string(
    const std::vector<uint8_t>& data,
    size_t offset,
    size_t max_len
) const {
    return read_utf16le_until_double_null(data, offset, max_len);
}

} // namespace zmdb
