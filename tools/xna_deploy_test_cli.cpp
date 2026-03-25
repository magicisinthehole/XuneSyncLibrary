/// Deploy XNA apps (.ccgame packages tested) to Zune HD over USB.
/// XNAFTW RPC protocol over MTP vendor ops 0x9220-0x9223. Requires MTPZ.

#include <iostream>
#include <fstream>
#include <string>
#include <vector>
#include <array>
#include <filesystem>
#include <cstring>
#include <iomanip>
#include <chrono>
#include <thread>
#include <sstream>
#include <random>
#include <algorithm>
#include <deque>
#include <optional>
#include <unordered_map>
#include <cctype>
#include <cstdlib>
#include <stdexcept>
#include <cstdio>

#include <usb/Context.h>
#include <mtp/usb/DeviceNotFoundException.h>
#include <mtp/ptp/Device.h>
#include <mtp/ptp/Session.h>
#include <mtp/ptp/DeviceProperty.h>
#include <mtp/ptp/ByteArrayObjectStream.h>
#include <mtp/mtpz/TrustedApp.h>

namespace fs = std::filesystem;

static bool g_verbose = false;

// ── XNAFTW Constants ────────────────────────────────────────────────────

static constexpr size_t XNA_FRAME_SIZE = 1264;
static constexpr size_t XNA_HEADER_SIZE = 7;
static constexpr size_t XNA_MAX_PAYLOAD = 1230;
static constexpr size_t XNA_DELAYED_CHUNK_LIMIT = 0x1FFFB;
static constexpr int XNA_DELAYED_FRAME_DELAY_MS = 10;
static constexpr int XNA_DELAYED_DRAIN_MAX_POLLS = 20;
static constexpr int XNA_DELAYED_DRAIN_IDLE_POLLS = 2;
static constexpr int XNA_DELAYED_DRAIN_DELAY_MS = 10;
static constexpr const char* XNA_MAGIC = "XNAFTW";
static constexpr size_t XNA_MAGIC_LEN = 6;

static constexpr const char* RUNTIME_TOKEN = "Zune.v4.0.Beta";
static constexpr uint32_t RUNTIME_REVISION = 0x3102BB64;

// XNAFTW message types
static constexpr uint8_t XNAFTW_REQUEST  = 0x01;
static constexpr uint8_t XNAFTW_RESPONSE = 0x02;

// XNA initial hello magic
static constexpr uint8_t XNA_HELLO_MAGIC[] = { 0x58, 0x58, 0x00, 0x01 };

// Channel management message types (first byte of payload)
static constexpr uint8_t MSG_CHANNEL_OPEN  = 0xA1;
static constexpr uint8_t MSG_CHANNEL_ACK   = 0xA4;
static constexpr uint8_t MSG_CHANNEL_CLOSE = 0xC1;
// Registration message type
static constexpr uint8_t MSG_REGISTRATION  = 0xC5;
static constexpr uint8_t MSG_NAMED_CHANNEL = 0x01;

static constexpr int MAX_POLLS = 300;
static constexpr int POLL_DELAY_MS = 5;

// ── Logging ─────────────────────────────────────────────────────────────

void log_ts(const std::string& message) {
    auto now = std::chrono::system_clock::now();
    auto time = std::chrono::system_clock::to_time_t(now);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()) % 1000;
    std::cout << "[" << std::put_time(std::localtime(&time), "%H:%M:%S")
              << "." << std::setfill('0') << std::setw(3) << ms.count() << "] "
              << message << std::endl;
}

void log_phase(const std::string& name) {
    std::cout << std::endl;
    std::cout << "════════════════════════════════════════════════════════════" << std::endl;
    std::cout << "  " << name << std::endl;
    std::cout << "════════════════════════════════════════════════════════════" << std::endl;
}

void log_op(const std::string& desc) { log_ts("  " + desc); }
void log_ok(const std::string& desc) { log_ts("    [OK] " + desc); }
void log_err(const std::string& desc) { log_ts("    [ERR] " + desc); }

void hex_dump(const std::string& label, const mtp::ByteArray& data, size_t max_bytes = 64) {
    if (!g_verbose) return;
    std::cout << "    " << label << " (" << data.size() << " bytes):" << std::endl;
    size_t show = std::min(data.size(), max_bytes);
    for (size_t i = 0; i < show; i += 16) {
        std::cout << "      ";
        for (size_t j = i; j < std::min(i + 16, show); j++)
            printf("%02x ", data[j]);
        if (i + 16 > show) {
            for (size_t j = show; j < i + 16; j++)
                std::cout << "   ";
        }
        std::cout << " ";
        for (size_t j = i; j < std::min(i + 16, show); j++) {
            char c = static_cast<char>(data[j]);
            std::cout << (c >= 32 && c < 127 ? c : '.');
        }
        std::cout << std::endl;
    }
    if (data.size() > max_bytes)
        std::cout << "      ... (" << data.size() - max_bytes << " more bytes)" << std::endl;
}

// ── Local file helpers ───────────────────────────────────────────────────

struct CcgamePayload {
    struct FileEntry {
        std::string relative_path;
        std::vector<uint8_t> data;
    };

    std::string startup_assembly;
    std::string product_name;
    std::string description;
    std::string runtime_profile;
    std::array<uint8_t, 16> game_guid_bytes{};
    bool has_game_guid = false;
    std::vector<uint8_t> app_data;
    std::vector<uint8_t> thumbnail_data;
    std::vector<FileEntry> files;
};

struct RuntimePayload {
    fs::path directory;
    std::vector<CcgamePayload::FileEntry> files;
};

struct DotNetResourceEntry {
    std::string key;
    std::string type_name;
    std::vector<uint8_t> raw_data;
};

std::string to_lower(std::string value) {
    for (char& ch : value) {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    return value;
}

std::vector<uint8_t> read_binary_file(const fs::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) return {};
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(file)),
                                 std::istreambuf_iterator<char>());
}

std::optional<fs::path> normalize_existing_directory(const fs::path& path) {
    try {
        if (!path.empty() && fs::exists(path) && fs::is_directory(path)) {
            return fs::weakly_canonical(path);
        }
    } catch (...) {}
    return std::nullopt;
}

std::optional<fs::path> find_runtime_directory(const char* argv0,
                                               const std::string& runtime_dir_override) {
    if (!runtime_dir_override.empty()) {
        return normalize_existing_directory(fs::path(runtime_dir_override));
    }

    std::vector<fs::path> candidates;
    auto add_candidate = [&](const fs::path& candidate) {
        if (!candidate.empty()) {
            candidates.push_back(candidate);
        }
    };

    try {
        auto cwd = fs::current_path();
        add_candidate(cwd / "XuneSyncLibrary" / "redocs" / "xna_zune_runtime");
        add_candidate(cwd / "redocs" / "xna_zune_runtime");
    } catch (...) {}

    try {
        auto exe_dir = fs::absolute(fs::path(argv0)).parent_path();
        for (fs::path current = exe_dir; !current.empty(); current = current.parent_path()) {
            add_candidate(current / "redocs" / "xna_zune_runtime");
            add_candidate(current / "XuneSyncLibrary" / "redocs" / "xna_zune_runtime");
            if (current == current.parent_path()) {
                break;
            }
        }
    } catch (...) {}

    for (const auto& candidate : candidates) {
        if (auto normalized = normalize_existing_directory(candidate)) {
            return normalized;
        }
    }

    return std::nullopt;
}

std::optional<RuntimePayload> load_runtime_payload(const fs::path& runtime_dir) {
    static constexpr const char* kRuntimeFileNames[] = {
        "Microsoft.Xna.Framework.Game.dll",
        "Microsoft.Xna.Framework.dll",
        "mscoree3_5.dll",
        "mscorlib.dll",
        "runtimehost.dll",
        "system.core.dll",
        "system.dll",
        "system.sr.dll",
        "system.xml.dll",
        "system.xml.linq.dll",
    };

    RuntimePayload payload;
    payload.directory = runtime_dir;

    for (const char* file_name : kRuntimeFileNames) {
        fs::path file_path = runtime_dir / file_name;
        if (!fs::exists(file_path)) {
            std::cerr << "ERROR: Missing runtime file: " << file_path << std::endl;
            return std::nullopt;
        }

        auto data = read_binary_file(file_path);
        if (data.empty()) {
            std::cerr << "ERROR: Failed to read runtime file: " << file_path << std::endl;
            return std::nullopt;
        }

        payload.files.push_back({file_name, std::move(data)});
    }

    return payload;
}

bool read_u32le(const std::vector<uint8_t>& data, size_t& offset, uint32_t& value) {
    if (offset + 4 > data.size()) return false;
    value = static_cast<uint32_t>(data[offset]) |
            (static_cast<uint32_t>(data[offset + 1]) << 8) |
            (static_cast<uint32_t>(data[offset + 2]) << 16) |
            (static_cast<uint32_t>(data[offset + 3]) << 24);
    offset += 4;
    return true;
}

bool read_bool(const std::vector<uint8_t>& data, size_t& offset, bool& value) {
    if (offset >= data.size()) return false;
    value = data[offset++] != 0;
    return true;
}

std::string decode_utf16le_bytes(const uint8_t* data, size_t length) {
    std::string value;
    value.reserve(length / 2);
    for (size_t i = 0; i + 1 < length; i += 2) {
        uint16_t ch = static_cast<uint16_t>(data[i]) |
                      (static_cast<uint16_t>(data[i + 1]) << 8);
        value.push_back(ch < 0x80 ? static_cast<char>(ch) : '?');
    }
    return value;
}

bool read_7bit_encoded_uint(const std::vector<uint8_t>& data, size_t& offset, uint32_t& value) {
    value = 0;
    uint32_t shift = 0;
    while (offset < data.size() && shift < 35) {
        uint8_t byte = data[offset++];
        value |= static_cast<uint32_t>(byte & 0x7F) << shift;
        if ((byte & 0x80) == 0) return true;
        shift += 7;
    }
    return false;
}

bool read_prefixed_utf8_string(const std::vector<uint8_t>& data, size_t& offset,
                               std::string& value) {
    uint32_t length = 0;
    if (!read_7bit_encoded_uint(data, offset, length)) return false;
    if (offset + length > data.size()) return false;
    value.assign(reinterpret_cast<const char*>(data.data() + offset), length);
    offset += length;
    return true;
}

bool read_prefixed_utf16_string(const std::vector<uint8_t>& data, size_t& offset,
                                std::string& value) {
    uint32_t length = 0;
    if (!read_7bit_encoded_uint(data, offset, length)) return false;
    if ((length & 1u) != 0 || offset + length > data.size()) return false;
    value = decode_utf16le_bytes(data.data() + offset, length);
    offset += length;
    return true;
}

bool read_i32le(const std::vector<uint8_t>& data, size_t& offset, int32_t& value) {
    uint32_t raw = 0;
    if (!read_u32le(data, offset, raw)) return false;
    value = static_cast<int32_t>(raw);
    return true;
}

std::string describe_dotnet_resource_type(uint32_t type_code,
                                          const std::vector<std::string>& user_types) {
    switch (type_code) {
    case 0x00: return "ResourceTypeCode.Null";
    case 0x01: return "ResourceTypeCode.String";
    default:
        break;
    }

    static constexpr uint32_t kStartOfUserTypes = 0x40;
    if (type_code >= kStartOfUserTypes) {
        size_t index = static_cast<size_t>(type_code - kStartOfUserTypes);
        if (index < user_types.size()) {
            return user_types[index];
        }
    }

    std::ostringstream label;
    label << "ResourceTypeCode(" << type_code << ")";
    return label.str();
}

std::vector<DotNetResourceEntry> parse_dotnet_resources(const std::vector<uint8_t>& data) {
    static constexpr uint32_t kResourceMagic = 0xBEEFCACE;

    std::vector<DotNetResourceEntry> entries;
    size_t offset = 0;
    uint32_t magic = 0;
    uint32_t header_version = 0;
    uint32_t header_bytes = 0;
    if (!read_u32le(data, offset, magic) ||
        !read_u32le(data, offset, header_version) ||
        !read_u32le(data, offset, header_bytes) ||
        magic != kResourceMagic) {
        return entries;
    }

    if (offset + header_bytes > data.size()) {
        return {};
    }
    offset += header_bytes;

    uint32_t version = 0;
    uint32_t resource_count = 0;
    uint32_t user_type_count = 0;
    if (!read_u32le(data, offset, version) ||
        !read_u32le(data, offset, resource_count) ||
        !read_u32le(data, offset, user_type_count) ||
        version != 2) {
        return {};
    }

    std::vector<std::string> user_types;
    for (uint32_t i = 0; i < user_type_count; i++) {
        std::string type_name;
        if (!read_prefixed_utf8_string(data, offset, type_name)) {
            return {};
        }
        user_types.push_back(std::move(type_name));
    }

    offset = (offset + 7) & ~static_cast<size_t>(7);
    if (offset + static_cast<size_t>(resource_count) * 8 + 4 > data.size()) {
        return {};
    }

    offset += static_cast<size_t>(resource_count) * 4;  // hash table

    std::vector<uint32_t> name_positions(resource_count, 0);
    for (uint32_t i = 0; i < resource_count; i++) {
        if (!read_u32le(data, offset, name_positions[i])) {
            return {};
        }
    }

    uint32_t data_section_offset = 0;
    if (!read_u32le(data, offset, data_section_offset)) {
        return {};
    }
    size_t name_section_offset = offset;

    struct ResourceNameRecord {
        std::string key;
        size_t entry_offset = 0;
    };

    std::vector<ResourceNameRecord> name_records;
    name_records.reserve(resource_count);
    for (uint32_t i = 0; i < resource_count; i++) {
        size_t name_offset = name_section_offset + name_positions[i];
        if (name_offset >= data.size()) {
            return {};
        }

        std::string key;
        if (!read_prefixed_utf16_string(data, name_offset, key)) {
            return {};
        }

        uint32_t relative_data_offset = 0;
        if (!read_u32le(data, name_offset, relative_data_offset)) {
            return {};
        }

        size_t entry_offset = static_cast<size_t>(data_section_offset) + relative_data_offset;
        if (entry_offset >= data.size()) {
            return {};
        }

        name_records.push_back({std::move(key), entry_offset});
    }

    std::vector<size_t> sorted_offsets;
    sorted_offsets.reserve(name_records.size());
    for (const auto& record : name_records) {
        sorted_offsets.push_back(record.entry_offset);
    }
    std::sort(sorted_offsets.begin(), sorted_offsets.end());

    for (const auto& record : name_records) {
        auto current = std::lower_bound(sorted_offsets.begin(), sorted_offsets.end(),
                                        record.entry_offset);
        if (current == sorted_offsets.end()) {
            return {};
        }

        size_t entry_offset = *current;
        size_t next_offset = data.size();
        if (++current != sorted_offsets.end()) {
            next_offset = *current;
        }

        size_t value_offset = entry_offset;
        uint32_t type_code = 0;
        if (!read_7bit_encoded_uint(data, value_offset, type_code) ||
            value_offset > next_offset || next_offset > data.size()) {
            return {};
        }

        entries.push_back({
            record.key,
            describe_dotnet_resource_type(type_code, user_types),
            std::vector<uint8_t>(data.begin() + value_offset, data.begin() + next_offset)
        });
    }

    return entries;
}

const DotNetResourceEntry* find_dotnet_resource(const std::vector<DotNetResourceEntry>& entries,
                                                const std::string& key) {
    auto it = std::find_if(entries.begin(), entries.end(),
                           [&](const DotNetResourceEntry& entry) {
                               return entry.key == key;
                           });
    return it == entries.end() ? nullptr : &*it;
}

std::string decode_dotnet_resource_string(const DotNetResourceEntry* entry) {
    if (!entry || entry->type_name != "ResourceTypeCode.String") {
        return {};
    }

    size_t offset = 0;
    std::string value;
    if (!read_prefixed_utf8_string(entry->raw_data, offset, value)) {
        return {};
    }
    return value;
}

bool decode_dotnet_resource_guid(const DotNetResourceEntry* entry,
                                 std::array<uint8_t, 16>& guid_bytes) {
    if (!entry || entry->type_name.find("System.Guid") != 0 || entry->raw_data.size() < 17) {
        return false;
    }
    if (entry->raw_data.back() != 0x0B) {
        return false;
    }

    std::copy(entry->raw_data.end() - 17, entry->raw_data.end() - 1, guid_bytes.begin());
    return true;
}

enum class BinaryFormatterRecordType : uint8_t {
    SerializedStreamHeader = 0,
    ClassWithId = 1,
    SystemClassWithMembers = 2,
    ClassWithMembers = 3,
    SystemClassWithMembersAndTypes = 4,
    ClassWithMembersAndTypes = 5,
    BinaryObjectString = 6,
    BinaryArray = 7,
    MemberPrimitiveTyped = 8,
    MemberReference = 9,
    ObjectNull = 10,
    MessageEnd = 11,
    BinaryLibrary = 12,
    ObjectNullMultiple256 = 13,
    ObjectNullMultiple = 14,
    ArraySinglePrimitive = 15,
    ArraySingleObject = 16,
    ArraySingleString = 17,
    CrossAppDomainMap = 18,
    CrossAppDomainString = 19,
    CrossAppDomainAssembly = 20,
    MethodCall = 21,
    MethodReturn = 22,
};

enum class BinaryFormatterBinaryType : uint8_t {
    Primitive = 0,
    String = 1,
    Object = 2,
    SystemClass = 3,
    Class = 4,
    ObjectArray = 5,
    StringArray = 6,
    PrimitiveArray = 7,
};

enum class BinaryFormatterArrayType : uint8_t {
    Single = 0,
    Jagged = 1,
    Rectangular = 2,
    SingleOffset = 3,
    JaggedOffset = 4,
    RectangularOffset = 5,
};

struct BinaryFormatterStringMatrix {
    size_t rows = 0;
    size_t cols = 0;
    std::vector<std::string> values;

    const std::string& at(size_t row, size_t col) const {
        return values[row * cols + col];
    }
};

bool skip_binaryformatter_type_info(const std::vector<uint8_t>& data, size_t& offset,
                                    BinaryFormatterBinaryType type) {
    switch (type) {
    case BinaryFormatterBinaryType::Primitive:
    case BinaryFormatterBinaryType::PrimitiveArray:
        if (offset >= data.size()) return false;
        offset++;
        return true;
    case BinaryFormatterBinaryType::SystemClass: {
        std::string ignored;
        return read_prefixed_utf8_string(data, offset, ignored);
    }
    case BinaryFormatterBinaryType::Class: {
        std::string ignored_name;
        int32_t ignored_library_id = 0;
        return read_prefixed_utf8_string(data, offset, ignored_name) &&
               read_i32le(data, offset, ignored_library_id);
    }
    case BinaryFormatterBinaryType::String:
    case BinaryFormatterBinaryType::Object:
    case BinaryFormatterBinaryType::ObjectArray:
    case BinaryFormatterBinaryType::StringArray:
        return true;
    }

    return false;
}

bool skip_binaryformatter_library_record(const std::vector<uint8_t>& data, size_t& offset) {
    int32_t ignored_library_id = 0;
    std::string ignored_library_name;
    return read_i32le(data, offset, ignored_library_id) &&
           read_prefixed_utf8_string(data, offset, ignored_library_name);
}

std::optional<BinaryFormatterStringMatrix> decode_binaryformatter_string_matrix(
        const DotNetResourceEntry* entry) {
    if (!entry || entry->type_name.find("System.String[,") != 0) {
        return std::nullopt;
    }

    size_t offset = 0;
    std::unordered_map<int32_t, std::string> strings_by_object_id;

    while (offset < entry->raw_data.size()) {
        auto record_type = static_cast<BinaryFormatterRecordType>(entry->raw_data[offset++]);
        switch (record_type) {
        case BinaryFormatterRecordType::SerializedStreamHeader: {
            int32_t top_id = 0;
            int32_t header_id = 0;
            int32_t major_version = 0;
            int32_t minor_version = 0;
            if (!read_i32le(entry->raw_data, offset, top_id) ||
                !read_i32le(entry->raw_data, offset, header_id) ||
                !read_i32le(entry->raw_data, offset, major_version) ||
                !read_i32le(entry->raw_data, offset, minor_version)) {
                return std::nullopt;
            }
            break;
        }
        case BinaryFormatterRecordType::BinaryLibrary:
            if (!skip_binaryformatter_library_record(entry->raw_data, offset)) {
                return std::nullopt;
            }
            break;
        case BinaryFormatterRecordType::BinaryObjectString: {
            int32_t object_id = 0;
            std::string value;
            if (!read_i32le(entry->raw_data, offset, object_id) ||
                !read_prefixed_utf8_string(entry->raw_data, offset, value)) {
                return std::nullopt;
            }
            strings_by_object_id[object_id] = std::move(value);
            break;
        }
        case BinaryFormatterRecordType::BinaryArray: {
            int32_t object_id = 0;
            int32_t rank = 0;
            if (!read_i32le(entry->raw_data, offset, object_id) ||
                offset >= entry->raw_data.size()) {
                return std::nullopt;
            }

            auto array_type = static_cast<BinaryFormatterArrayType>(entry->raw_data[offset++]);
            if (!read_i32le(entry->raw_data, offset, rank) || rank != 2) {
                return std::nullopt;
            }

            std::vector<int32_t> lengths(static_cast<size_t>(rank), 0);
            for (int32_t i = 0; i < rank; i++) {
                if (!read_i32le(entry->raw_data, offset, lengths[static_cast<size_t>(i)]) ||
                    lengths[static_cast<size_t>(i)] < 0) {
                    return std::nullopt;
                }
            }

            if (array_type == BinaryFormatterArrayType::RectangularOffset ||
                array_type == BinaryFormatterArrayType::SingleOffset ||
                array_type == BinaryFormatterArrayType::JaggedOffset) {
                for (int32_t i = 0; i < rank; i++) {
                    int32_t ignored_lower_bound = 0;
                    if (!read_i32le(entry->raw_data, offset, ignored_lower_bound)) {
                        return std::nullopt;
                    }
                }
            }

            if (offset >= entry->raw_data.size()) {
                return std::nullopt;
            }
            auto element_type =
                static_cast<BinaryFormatterBinaryType>(entry->raw_data[offset++]);
            if (!skip_binaryformatter_type_info(entry->raw_data, offset, element_type) ||
                element_type != BinaryFormatterBinaryType::String) {
                return std::nullopt;
            }

            BinaryFormatterStringMatrix matrix;
            matrix.rows = static_cast<size_t>(lengths[0]);
            matrix.cols = static_cast<size_t>(lengths[1]);
            matrix.values.resize(matrix.rows * matrix.cols);

            size_t index = 0;
            while (index < matrix.values.size() && offset < entry->raw_data.size()) {
                auto item_record =
                    static_cast<BinaryFormatterRecordType>(entry->raw_data[offset++]);
                switch (item_record) {
                case BinaryFormatterRecordType::BinaryObjectString: {
                    int32_t string_object_id = 0;
                    std::string value;
                    if (!read_i32le(entry->raw_data, offset, string_object_id) ||
                        !read_prefixed_utf8_string(entry->raw_data, offset, value)) {
                        return std::nullopt;
                    }
                    strings_by_object_id[string_object_id] = value;
                    matrix.values[index++] = std::move(value);
                    break;
                }
                case BinaryFormatterRecordType::MemberReference: {
                    int32_t string_object_id = 0;
                    if (!read_i32le(entry->raw_data, offset, string_object_id)) {
                        return std::nullopt;
                    }
                    auto it = strings_by_object_id.find(string_object_id);
                    if (it == strings_by_object_id.end()) {
                        return std::nullopt;
                    }
                    matrix.values[index++] = it->second;
                    break;
                }
                case BinaryFormatterRecordType::ObjectNull:
                    matrix.values[index++].clear();
                    break;
                case BinaryFormatterRecordType::ObjectNullMultiple256: {
                    if (offset >= entry->raw_data.size()) return std::nullopt;
                    uint8_t null_count = entry->raw_data[offset++];
                    if (index + null_count > matrix.values.size()) {
                        return std::nullopt;
                    }
                    for (uint8_t i = 0; i < null_count; i++) {
                        matrix.values[index++].clear();
                    }
                    break;
                }
                case BinaryFormatterRecordType::ObjectNullMultiple: {
                    int32_t null_count = 0;
                    if (!read_i32le(entry->raw_data, offset, null_count) || null_count < 0 ||
                        index + static_cast<size_t>(null_count) > matrix.values.size()) {
                        return std::nullopt;
                    }
                    for (int32_t i = 0; i < null_count; i++) {
                        matrix.values[index++].clear();
                    }
                    break;
                }
                case BinaryFormatterRecordType::BinaryLibrary:
                    if (!skip_binaryformatter_library_record(entry->raw_data, offset)) {
                        return std::nullopt;
                    }
                    break;
                case BinaryFormatterRecordType::MessageEnd:
                    if (index == matrix.values.size()) {
                        return matrix;
                    }
                    return std::nullopt;
                default:
                    return std::nullopt;
                }
            }

            if (index != matrix.values.size()) {
                return std::nullopt;
            }
            return matrix;
        }
        case BinaryFormatterRecordType::MessageEnd:
            return std::nullopt;
        default:
            return std::nullopt;
        }
    }

    return std::nullopt;
}

bool looks_like_ccgame_file_path(const std::string& value) {
    auto trimmed = value;
    if (trimmed.size() < 3) return false;

    size_t separator = trimmed.find_last_of("\\/");
    size_t filename_start = separator == std::string::npos ? 0 : separator + 1;
    size_t dot = trimmed.find_last_of('.');
    if (dot == std::string::npos || dot <= filename_start || dot + 1 >= trimmed.size()) {
        return false;
    }

    auto extension = trimmed.substr(dot + 1);
    if (extension.size() > 12) return false;
    return std::all_of(extension.begin(), extension.end(), [](unsigned char ch) {
        return std::isalnum(ch) != 0;
    });
}

void dump_ccgame_file_matrix(const BinaryFormatterStringMatrix& matrix) {
    if (!g_verbose) return;

    std::cout << "[ccgame] Files matrix=" << matrix.rows << "x" << matrix.cols << std::endl;
    for (size_t row = 0; row < matrix.rows; row++) {
        for (size_t col = 0; col < matrix.cols; col++) {
            std::cout << "[ccgame] Files[" << row << "," << col << "]="
                      << matrix.at(row, col) << std::endl;
        }
    }
}

std::vector<std::string> extract_ccgame_file_paths(const DotNetResourceEntry* entry) {
    auto matrix = decode_binaryformatter_string_matrix(entry);
    if (!matrix || matrix->rows == 0 || matrix->cols == 0) {
        return {};
    }

    dump_ccgame_file_matrix(*matrix);

    size_t best_row = 0;
    size_t best_row_matches = 0;
    for (size_t row = 0; row < matrix->rows; row++) {
        size_t matches = 0;
        for (size_t col = 0; col < matrix->cols; col++) {
            if (looks_like_ccgame_file_path(matrix->at(row, col))) {
                matches++;
            }
        }
        if (matches > best_row_matches) {
            best_row_matches = matches;
            best_row = row;
        }
    }

    size_t best_col = 0;
    size_t best_col_matches = 0;
    for (size_t col = 0; col < matrix->cols; col++) {
        size_t matches = 0;
        for (size_t row = 0; row < matrix->rows; row++) {
            if (looks_like_ccgame_file_path(matrix->at(row, col))) {
                matches++;
            }
        }
        if (matches > best_col_matches) {
            best_col_matches = matches;
            best_col = col;
        }
    }

    std::vector<std::string> paths;
    if (best_row_matches == 0 && best_col_matches == 0) {
        return paths;
    }

    if (best_row_matches >= best_col_matches) {
        paths.reserve(matrix->cols);
        for (size_t col = 0; col < matrix->cols; col++) {
            const auto& value = matrix->at(best_row, col);
            if (looks_like_ccgame_file_path(value)) {
                paths.push_back(value);
            }
        }
        return paths;
    }

    paths.reserve(matrix->rows);
    for (size_t row = 0; row < matrix->rows; row++) {
        const auto& value = matrix->at(row, best_col);
        if (looks_like_ccgame_file_path(value)) {
            paths.push_back(value);
        }
    }
    return paths;
}

std::optional<std::pair<uint32_t, uint32_t>> read_png_dimensions(const std::vector<uint8_t>& data) {
    static constexpr uint8_t kPngSignature[8] = {
        0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A
    };
    if (data.size() < 24 || memcmp(data.data(), kPngSignature, sizeof(kPngSignature)) != 0) {
        return std::nullopt;
    }
    if (memcmp(data.data() + 12, "IHDR", 4) != 0) {
        return std::nullopt;
    }

    uint32_t width = (static_cast<uint32_t>(data[16]) << 24) |
                     (static_cast<uint32_t>(data[17]) << 16) |
                     (static_cast<uint32_t>(data[18]) << 8) |
                     static_cast<uint32_t>(data[19]);
    uint32_t height = (static_cast<uint32_t>(data[20]) << 24) |
                      (static_cast<uint32_t>(data[21]) << 16) |
                      (static_cast<uint32_t>(data[22]) << 8) |
                      static_cast<uint32_t>(data[23]);
    return std::make_pair(width, height);
}

std::vector<std::vector<uint8_t>> extract_embedded_pngs(const std::vector<uint8_t>& data) {
    static constexpr uint8_t kPngSignature[8] = {
        0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A
    };
    static constexpr uint8_t kPngEnd[12] = {
        0x00, 0x00, 0x00, 0x00, 0x49, 0x45, 0x4E, 0x44, 0xAE, 0x42, 0x60, 0x82
    };

    std::vector<std::vector<uint8_t>> images;
    for (size_t start = 0; start + sizeof(kPngSignature) <= data.size(); start++) {
        if (memcmp(data.data() + start, kPngSignature, sizeof(kPngSignature)) != 0) {
            continue;
        }
        for (size_t end = start + sizeof(kPngSignature);
             end + sizeof(kPngEnd) <= data.size(); end++) {
            if (memcmp(data.data() + end, kPngEnd, sizeof(kPngEnd)) == 0) {
                images.emplace_back(data.begin() + start, data.begin() + end + sizeof(kPngEnd));
                start = end + sizeof(kPngEnd) - 1;
                break;
            }
        }
    }
    return images;
}

std::string shell_quote(const std::string& value) {
    std::string quoted = "'";
    for (char ch : value) {
        if (ch == '\'') quoted += "'\\''";
        else quoted.push_back(ch);
    }
    quoted.push_back('\'');
    return quoted;
}

fs::path make_temp_directory(const std::string& prefix) {
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<uint32_t> dist;

    for (int attempt = 0; attempt < 32; attempt++) {
        std::ostringstream name;
        name << prefix << "_" << std::hex << dist(gen);
        auto candidate = fs::temp_directory_path() / name.str();
        if (!fs::exists(candidate)) {
            fs::create_directories(candidate);
            return candidate;
        }
    }
    throw std::runtime_error("Failed to create temporary directory");
}

std::optional<CcgamePayload> load_ccgame_payload(const fs::path& package_path) {
    fs::path temp_dir;
    try {
        temp_dir = make_temp_directory("xune_ccgame");
    } catch (const std::exception& e) {
        std::cerr << "ERROR: " << e.what() << std::endl;
        return std::nullopt;
    }

    auto cleanup = [&]() {
        std::error_code ec;
        fs::remove_all(temp_dir, ec);
    };

    std::string command = "cabextract -q -d " + shell_quote(temp_dir.string()) +
                          " " + shell_quote(package_path.string());
    if (std::system(command.c_str()) != 0) {
        std::cerr << "ERROR: Failed to extract package: " << package_path << std::endl;
        cleanup();
        return std::nullopt;
    }

    std::vector<fs::path> extracted_files;
    for (const auto& entry : fs::directory_iterator(temp_dir)) {
        if (!entry.is_regular_file()) continue;
        if (entry.path().filename() == "XCabInfo.resources") continue;
        extracted_files.push_back(entry.path());
    }
    std::sort(extracted_files.begin(), extracted_files.end());

    CcgamePayload payload;
    auto resource_path = temp_dir / "XCabInfo.resources";
    if (fs::exists(resource_path)) {
        auto resource_bytes = read_binary_file(resource_path);
        auto resource_entries = parse_dotnet_resources(resource_bytes);

        if (auto* entry = find_dotnet_resource(resource_entries, "GameTitle")) {
            payload.product_name = decode_dotnet_resource_string(entry);
        }
        if (auto* entry = find_dotnet_resource(resource_entries, "GameDescription")) {
            payload.description = decode_dotnet_resource_string(entry);
        }
        if (auto* entry = find_dotnet_resource(resource_entries, "StartupAssembly")) {
            payload.startup_assembly = decode_dotnet_resource_string(entry);
        }
        if (auto* entry = find_dotnet_resource(resource_entries, "RuntimeProfile")) {
            payload.runtime_profile = decode_dotnet_resource_string(entry);
        }
        if (auto* entry = find_dotnet_resource(resource_entries, "GameGuid")) {
            payload.has_game_guid = decode_dotnet_resource_guid(entry, payload.game_guid_bytes);
        }
        {
            auto* files_entry = find_dotnet_resource(resource_entries, "Files");
            if (!files_entry) {
                std::cerr << "ERROR: Package is missing Files resource: "
                          << package_path << std::endl;
                cleanup();
                return std::nullopt;
            }

            std::vector<std::string> manifest_paths = extract_ccgame_file_paths(files_entry);
            if (manifest_paths.empty()) {
                std::cerr << "ERROR: Failed to decode Files String[,] resource in package: "
                          << package_path << std::endl;
                cleanup();
                return std::nullopt;
            }

            std::vector<fs::path> indexed_files = extracted_files;
            std::sort(indexed_files.begin(), indexed_files.end(),
                      [](const fs::path& a, const fs::path& b) {
                          auto a_name = a.filename().string();
                          auto b_name = b.filename().string();
                          bool a_numeric = !a_name.empty() &&
                                           std::all_of(a_name.begin(), a_name.end(),
                                                       [](unsigned char ch) { return std::isdigit(ch) != 0; });
                          bool b_numeric = !b_name.empty() &&
                                           std::all_of(b_name.begin(), b_name.end(),
                                                       [](unsigned char ch) { return std::isdigit(ch) != 0; });
                          if (a_numeric && b_numeric) {
                              return std::stoul(a_name) < std::stoul(b_name);
                          }
                          if (a_numeric != b_numeric) {
                              return a_numeric;
                          }
                          return a_name < b_name;
                      });

            if (g_verbose) {
                std::cout << "[ccgame] manifest_paths=" << manifest_paths.size()
                          << " extracted_files=" << indexed_files.size() << std::endl;
            }

            if (manifest_paths.size() != indexed_files.size()) {
                std::cerr << "ERROR: Files String[,] count (" << manifest_paths.size()
                          << ") does not match extracted file count ("
                          << indexed_files.size() << ") in package: "
                          << package_path << std::endl;
                cleanup();
                return std::nullopt;
            }

            for (size_t i = 0; i < indexed_files.size(); i++) {
                auto bytes = read_binary_file(indexed_files[i]);
                if (bytes.empty()) continue;
                payload.files.push_back({manifest_paths[i], std::move(bytes)});
            }
        }
        if (auto* entry = find_dotnet_resource(resource_entries, "GameThumbnail")) {
            for (auto& image : extract_embedded_pngs(entry->raw_data)) {
                auto png = read_png_dimensions(image);
                if (png && png->first == 64 && png->second == 64) {
                    payload.thumbnail_data = std::move(image);
                    break;
                }
            }
        }
    } else {
        std::cerr << "ERROR: Package is missing XCabInfo.resources: "
                  << package_path << std::endl;
        cleanup();
        return std::nullopt;
    }

    if (payload.startup_assembly.empty()) {
        std::cerr << "ERROR: Package is missing StartupAssembly resource: "
                  << package_path << std::endl;
        cleanup();
        return std::nullopt;
    }

    for (const auto& file : payload.files) {
        if (to_lower(file.relative_path) == to_lower(payload.startup_assembly)) {
            payload.app_data = file.data;
            break;
        }
    }

    if (payload.app_data.empty()) {
        std::cerr << "ERROR: Startup assembly '" << payload.startup_assembly
                  << "' not found in package files: " << package_path << std::endl;
        cleanup();
        return std::nullopt;
    }

    if (payload.product_name.empty()) {
        payload.product_name = fs::path(payload.startup_assembly).stem().string();
    }
    cleanup();
    return payload;
}

bool is_retryable_xna_poll_error(const mtp::InvalidResponseException& ex) {
    return static_cast<uint16_t>(ex.Type) == 0xA222;
}

std::string effective_runtime_token(const std::string& runtime_profile) {
    if (runtime_profile == "Zune.v3.1") {
        return "Zune.v4.0.Beta";
    }
    if (!runtime_profile.empty()) {
        return runtime_profile;
    }
    return RUNTIME_TOKEN;
}

// ── GUID generation ─────────────────────────────────────────────────────

struct Guid {
    uint32_t parts[4];

    static Guid generate() {
        std::random_device rd;
        std::mt19937 gen(rd());
        std::uniform_int_distribution<uint32_t> dist;
        return {{ dist(gen), dist(gen), dist(gen), dist(gen) }};
    }

    static Guid zero() {
        return {{ 0, 0, 0, 0 }};
    }

    static Guid from_bytes(const std::array<uint8_t, 16>& bytes) {
        Guid guid{};
        for (int i = 0; i < 4; i++) {
            guid.parts[i] = static_cast<uint32_t>(bytes[i * 4]) |
                            (static_cast<uint32_t>(bytes[i * 4 + 1]) << 8) |
                            (static_cast<uint32_t>(bytes[i * 4 + 2]) << 16) |
                            (static_cast<uint32_t>(bytes[i * 4 + 3]) << 24);
        }
        return guid;
    }

    static Guid from_canonical_string(const std::string& text) {
        unsigned int d1 = 0;
        unsigned int d2 = 0;
        unsigned int d3 = 0;
        unsigned int b[8] = {};
        if (std::sscanf(text.c_str(),
                        "%8x-%4x-%4x-%2x%2x-%2x%2x%2x%2x%2x%2x",
                        &d1, &d2, &d3,
                        &b[0], &b[1], &b[2], &b[3],
                        &b[4], &b[5], &b[6], &b[7]) != 11) {
            return Guid::zero();
        }

        std::array<uint8_t, 16> bytes = {
            static_cast<uint8_t>(d1 & 0xFF),
            static_cast<uint8_t>((d1 >> 8) & 0xFF),
            static_cast<uint8_t>((d1 >> 16) & 0xFF),
            static_cast<uint8_t>((d1 >> 24) & 0xFF),
            static_cast<uint8_t>(d2 & 0xFF),
            static_cast<uint8_t>((d2 >> 8) & 0xFF),
            static_cast<uint8_t>(d3 & 0xFF),
            static_cast<uint8_t>((d3 >> 8) & 0xFF),
            static_cast<uint8_t>(b[0]),
            static_cast<uint8_t>(b[1]),
            static_cast<uint8_t>(b[2]),
            static_cast<uint8_t>(b[3]),
            static_cast<uint8_t>(b[4]),
            static_cast<uint8_t>(b[5]),
            static_cast<uint8_t>(b[6]),
            static_cast<uint8_t>(b[7]),
        };
        return from_bytes(bytes);
    }

    // .NET Guid.ToString("N") — 32 hex digits, no hyphens
    std::string to_string_n() const {
        std::array<uint8_t, 16> bytes{};
        for (int i = 0; i < 4; i++) {
            bytes[i * 4]     = parts[i] & 0xFF;
            bytes[i * 4 + 1] = (parts[i] >> 8) & 0xFF;
            bytes[i * 4 + 2] = (parts[i] >> 16) & 0xFF;
            bytes[i * 4 + 3] = (parts[i] >> 24) & 0xFF;
        }
        // .NET format: Data1(4B BE) Data2(2B BE) Data3(2B BE) rest(8B sequential)
        char buf[33];
        std::snprintf(buf, sizeof(buf),
            "%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x",
            bytes[3], bytes[2], bytes[1], bytes[0],   // Data1 BE
            bytes[5], bytes[4],                         // Data2 BE
            bytes[7], bytes[6],                         // Data3 BE
            bytes[8], bytes[9], bytes[10], bytes[11],
            bytes[12], bytes[13], bytes[14], bytes[15]);
        return std::string(buf);
    }

    void write_to(std::vector<uint8_t>& buf) const {
        for (int i = 0; i < 4; i++) {
            buf.push_back(parts[i] & 0xFF);
            buf.push_back((parts[i] >> 8) & 0xFF);
            buf.push_back((parts[i] >> 16) & 0xFF);
            buf.push_back((parts[i] >> 24) & 0xFF);
        }
    }
};

// ── XNAFTW Frame Builder ─────────────────────────────────────────────────

class XnaFrameBuilder {
    uint32_t seq_ = 0;

public:
    // Build a 1264-byte frame with 7-byte header
    // Header: seq(4 BE) + channel(1) + payload_length(u16 BE)
    // Payload starts at byte 7, zero-padded, MAC at bytes 1240-1263
    mtp::ByteArray build_frame(uint8_t channel, uint16_t payload_len,
                                const std::vector<uint8_t>& payload) {
        mtp::ByteArray frame(XNA_FRAME_SIZE, 0);

        // Sequence number (big-endian)
        frame[0] = (seq_ >> 24) & 0xFF;
        frame[1] = (seq_ >> 16) & 0xFF;
        frame[2] = (seq_ >> 8) & 0xFF;
        frame[3] = seq_ & 0xFF;
        seq_++;

        frame[4] = channel;
        frame[5] = static_cast<uint8_t>((payload_len >> 8) & 0xFF);
        frame[6] = static_cast<uint8_t>(payload_len & 0xFF);

        // Copy payload starting at byte 7
        size_t copy_len = std::min(payload.size(), XNA_MAX_PAYLOAD);
        memcpy(frame.data() + XNA_HEADER_SIZE, payload.data(), copy_len);

        return frame;
    }

    // Build a channel management frame (msg_type is first payload byte)
    mtp::ByteArray build_mgmt_frame(uint8_t channel, uint8_t msg_type,
                                     const std::vector<uint8_t>& data) {
        std::vector<uint8_t> payload;
        payload.push_back(msg_type);
        payload.insert(payload.end(), data.begin(), data.end());
        // Management frames exclude the opcode byte from the header length
        // field even though it is present in the body.
        uint16_t paylen = static_cast<uint16_t>(data.size());
        return build_frame(channel, paylen, payload);
    }

    // Build an XNAFTW RPC frame (XNAFTW magic is part of payload at byte 7)
    mtp::ByteArray build_xnaftw_frame(uint8_t channel,
                                       const std::vector<uint8_t>& xnaftw_payload) {
        uint16_t paylen = static_cast<uint16_t>(xnaftw_payload.size());
        return build_frame(channel, paylen, xnaftw_payload);
    }

    // Build a delayed-parameter payload frame.
    mtp::ByteArray build_binary_frame(uint8_t channel, const uint8_t* data, size_t len) {
        std::vector<uint8_t> payload(data, data + len);
        uint16_t paylen = static_cast<uint16_t>(len);
        return build_frame(channel, paylen, payload);
    }

    mtp::ByteArray build_binary_frame(uint8_t channel, const std::vector<uint8_t>& payload) {
        uint16_t paylen = static_cast<uint16_t>(payload.size());
        return build_frame(channel, paylen, payload);
    }

    uint32_t current_seq() const { return seq_; }
    void set_seq(uint32_t s) { seq_ = s; }

    // ── XNAFTW serialization helpers (.NET BinaryWriter-compatible) ──

    static void write_7bit_uint(std::vector<uint8_t>& buf, uint32_t value) {
        do {
            uint8_t byte = static_cast<uint8_t>(value & 0x7F);
            value >>= 7;
            if (value != 0) {
                byte |= 0x80;
            }
            buf.push_back(byte);
        } while (value != 0);
    }

    // Write a UTF-16LE string with a 7-bit encoded byte-length prefix.
    static void write_utf16(std::vector<uint8_t>& buf, const std::string& str) {
        uint32_t byte_len = static_cast<uint32_t>(str.size() * 2);
        write_7bit_uint(buf, byte_len);
        for (char c : str) {
            buf.push_back(static_cast<uint8_t>(c));
            buf.push_back(0x00);
        }
    }

    // Build complete XNAFTW request payload (magic + type + method + params)
    static std::vector<uint8_t> build_xnaftw_request(
            const std::string& method_name,
            const std::vector<uint8_t>& serialized_params) {
        std::vector<uint8_t> payload;

        // XNAFTW magic
        payload.insert(payload.end(), XNA_MAGIC, XNA_MAGIC + XNA_MAGIC_LEN);

        // Request type
        payload.push_back(XNAFTW_REQUEST);

        // Method name (7-bit encoded byte-length + UTF-16LE)
        write_utf16(payload, method_name);

        // Serialized parameters (already includes param count)
        payload.insert(payload.end(), serialized_params.begin(), serialized_params.end());

        return payload;
    }

    // Build channel open payload: msg_type(0xA1) + flag(u8) + name_len(u16 LE chars) + name(UTF-16LE)
    static std::vector<uint8_t> build_channel_open_payload(uint8_t channel_id,
                                                            const std::string& name) {
        std::vector<uint8_t> data;
        data.push_back(channel_id);  // channel setup flag
        // Name length in chars as uint16 LE
        uint16_t name_chars = static_cast<uint16_t>(name.size());
        data.push_back(name_chars & 0xFF);
        data.push_back((name_chars >> 8) & 0xFF);
        // Name in UTF-16LE
        for (char c : name) {
            data.push_back(static_cast<uint8_t>(c));
            data.push_back(0x00);
        }
        return data;
    }
};

// ── XNAFTW Parameter Builder ──────────────────────────────────────────────

class XnaParamBuilder {
    std::vector<uint8_t> buf_;
    uint8_t count_ = 0;

public:
    // Format: name_len(u8) name(UTF-16LE) value_type(u8) value...
    void add_string(const std::string& name, uint8_t value_type,
                    const std::string& value) {
        XnaFrameBuilder::write_utf16(buf_, name);
        buf_.push_back(value_type);
        XnaFrameBuilder::write_utf16(buf_, value);
        count_++;
    }

    // Add a GUID parameter: name_len(u8) name(UTF-16LE) 0x09 guid(16 bytes)
    void add_guid(const std::string& name, const Guid& guid) {
        XnaFrameBuilder::write_utf16(buf_, name);
        buf_.push_back(0x09);  // GUID value type
        guid.write_to(buf_);
        count_++;
    }

    void add_bool(const std::string& name, bool value) {
        XnaFrameBuilder::write_utf16(buf_, name);
        buf_.push_back(0x01);  // Boolean type
        buf_.push_back(value ? 0x01 : 0x00);
        count_++;
    }

    // Add a uint32 parameter: name_len(u8) name(UTF-16LE) value_type(u8) value(4 bytes LE)
    void add_uint32(const std::string& name, uint8_t value_type, uint32_t value) {
        XnaFrameBuilder::write_utf16(buf_, name);
        buf_.push_back(value_type);
        buf_.push_back(value & 0xFF);
        buf_.push_back((value >> 8) & 0xFF);
        buf_.push_back((value >> 16) & 0xFF);
        buf_.push_back((value >> 24) & 0xFF);
        count_++;
    }

    // Add a delayed BLOB parameter reference. The byte after 0x0A is the
    // 1-based parameter ordinal.
    uint8_t add_binary_ref(const std::string& name, uint32_t content_size) {
        uint8_t param_index = static_cast<uint8_t>(count_ + 1);
        XnaFrameBuilder::write_utf16(buf_, name);
        buf_.push_back(0x0A);  // Binary content type
        buf_.push_back(param_index);
        buf_.push_back(content_size & 0xFF);
        buf_.push_back((content_size >> 8) & 0xFF);
        buf_.push_back((content_size >> 16) & 0xFF);
        buf_.push_back((content_size >> 24) & 0xFF);
        count_++;
        return param_index;
    }

    // Serialize: param_count(u8) + all param data
    std::vector<uint8_t> serialize() const {
        std::vector<uint8_t> result;
        result.push_back(count_);
        result.insert(result.end(), buf_.begin(), buf_.end());
        return result;
    }
};

// ── XNAFTW Schema Parser ─────────────────────────────────────────────────

struct SchemaParam {
    std::string name;
    uint8_t type;
};

struct SchemaVerb {
    std::string name;
    std::vector<SchemaParam> params;
};

std::string param_type_name(uint8_t type) {
    switch (type) {
    case 0: return "Byte";
    case 1: return "Boolean";
    case 2: return "Int16";
    case 3: return "Int32";
    case 4: return "Int64";
    case 5: return "Single";
    case 6: return "Double";
    case 7: return "DateTime";
    case 8: return "String";
    case 9: return "Guid";
    case 10: return "Blob";
    default: return "Unknown(" + std::to_string(type) + ")";
    }
}

std::vector<SchemaVerb> parse_xnaftw_schema(const mtp::ByteArray& frame) {
    std::vector<SchemaVerb> verbs;
    if (frame.size() < XNA_HEADER_SIZE + XNA_MAGIC_LEN + 1) return verbs;
    if (memcmp(frame.data() + XNA_HEADER_SIZE, XNA_MAGIC, XNA_MAGIC_LEN) != 0) return verbs;
    // Schema type = 0x00
    if (frame[XNA_HEADER_SIZE + XNA_MAGIC_LEN] != 0x00) return verbs;

    size_t offset = XNA_HEADER_SIZE + XNA_MAGIC_LEN + 1;
    if (offset >= frame.size()) return verbs;

    uint8_t verb_count = frame[offset++];
    for (uint8_t v = 0; v < verb_count && offset < frame.size(); v++) {
        SchemaVerb verb;
        // Verb name: 7-bit encoded byte length + UTF-16LE
        uint32_t name_byte_len = 0;
        if (!read_7bit_encoded_uint(frame, offset, name_byte_len)) break;
        if (offset + name_byte_len > frame.size()) break;
        verb.name = decode_utf16le_bytes(frame.data() + offset, name_byte_len);
        offset += name_byte_len;

        if (offset >= frame.size()) break;
        uint8_t param_count = frame[offset++];
        for (uint8_t p = 0; p < param_count && offset < frame.size(); p++) {
            SchemaParam param;
            uint32_t pname_byte_len = 0;
            if (!read_7bit_encoded_uint(frame, offset, pname_byte_len)) break;
            if (offset + pname_byte_len > frame.size()) break;
            param.name = decode_utf16le_bytes(frame.data() + offset, pname_byte_len);
            offset += pname_byte_len;

            if (offset >= frame.size()) break;
            param.type = frame[offset++];
            verb.params.push_back(std::move(param));
        }
        verbs.push_back(std::move(verb));
    }
    return verbs;
}

void log_schema(const std::vector<SchemaVerb>& verbs) {
    for (const auto& verb : verbs) {
        std::string line = "  " + verb.name + "(";
        for (size_t i = 0; i < verb.params.size(); i++) {
            if (i > 0) line += ", ";
            line += param_type_name(verb.params[i].type) + " " + verb.params[i].name;
        }
        line += ")";
        log_ok(line);
    }
}

struct ParsedXnaResponse {
    bool faulted = false;
    bool requires_delayed_parameter = false;
    uint8_t value_type = 0xFF;
    bool has_byte_value = false;
    uint8_t byte_value = 0;
    bool has_bool_value = false;
    bool bool_value = false;
    int32_t fault_id = 0;
    std::string fault_message;
    std::string value_summary;
};

struct ParsedXnaSecureFrame {
    uint32_t sequence = 0;
    uint8_t channel = 0;
    uint16_t payload_length = 0;
    size_t payload_bytes = 0;
    bool has_xnaftw_magic = false;
    bool has_control_opcode = false;
    uint16_t control_opcode = 0;
    bool has_control_value = false;
    uint16_t control_value = 0;
    bool has_embedded_segment = false;
    uint8_t embedded_channel = 0;
    uint16_t embedded_payload_length = 0;
    bool embedded_has_xnaftw_magic = false;
};

bool is_xnaftw_response_frame(const mtp::ByteArray& data);

std::optional<ParsedXnaSecureFrame> parse_xna_secure_frame(const mtp::ByteArray& data) {
    if (data.size() != XNA_FRAME_SIZE) return std::nullopt;

    ParsedXnaSecureFrame frame;
    frame.sequence = (static_cast<uint32_t>(data[0]) << 24) |
                     (static_cast<uint32_t>(data[1]) << 16) |
                     (static_cast<uint32_t>(data[2]) << 8) |
                     static_cast<uint32_t>(data[3]);
    frame.channel = data[4];
    frame.payload_length = (static_cast<uint16_t>(data[5]) << 8) |
                           static_cast<uint16_t>(data[6]);
    frame.payload_bytes = std::min(static_cast<size_t>(frame.payload_length),
                                   data.size() - XNA_HEADER_SIZE);

    if (frame.payload_bytes >= XNA_MAGIC_LEN &&
        memcmp(data.data() + XNA_HEADER_SIZE, XNA_MAGIC, XNA_MAGIC_LEN) == 0) {
        frame.has_xnaftw_magic = true;
    }

    if (frame.channel == 0x00 && !frame.has_xnaftw_magic && frame.payload_bytes >= 2) {
        frame.has_control_opcode = true;
        frame.control_opcode = static_cast<uint16_t>(data[XNA_HEADER_SIZE]) |
                               (static_cast<uint16_t>(data[XNA_HEADER_SIZE + 1]) << 8);
        if (frame.payload_bytes >= 4) {
            frame.has_control_value = true;
            frame.control_value = static_cast<uint16_t>(data[XNA_HEADER_SIZE + 2]) |
                                  (static_cast<uint16_t>(data[XNA_HEADER_SIZE + 3]) << 8);
        }
    }

    size_t embedded_offset = XNA_HEADER_SIZE + frame.payload_bytes;
    if (embedded_offset + 3 <= data.size()) {
        uint8_t embedded_channel = data[embedded_offset];
        uint16_t embedded_payload_length =
            (static_cast<uint16_t>(data[embedded_offset + 1]) << 8) |
            static_cast<uint16_t>(data[embedded_offset + 2]);
        size_t embedded_payload_offset = embedded_offset + 3;
        if (embedded_payload_length > 0 &&
            embedded_channel <= 0x02 &&
            embedded_payload_offset + embedded_payload_length <= data.size()) {
            frame.has_embedded_segment = true;
            frame.embedded_channel = embedded_channel;
            frame.embedded_payload_length = embedded_payload_length;
            if (embedded_payload_length >= XNA_MAGIC_LEN &&
                memcmp(data.data() + embedded_payload_offset, XNA_MAGIC, XNA_MAGIC_LEN) == 0) {
                frame.embedded_has_xnaftw_magic = true;
            }
        }
    }

    return frame;
}

std::optional<mtp::ByteArray> extract_embedded_xnaftw_response(const mtp::ByteArray& data) {
    auto frame = parse_xna_secure_frame(data);
    if (!frame || !frame->has_embedded_segment || !frame->embedded_has_xnaftw_magic) {
        return std::nullopt;
    }

    size_t embedded_offset = XNA_HEADER_SIZE + frame->payload_bytes;
    size_t embedded_payload_offset = embedded_offset + 3;
    mtp::ByteArray synthetic(XNA_HEADER_SIZE + frame->embedded_payload_length, 0);
    synthetic[0] = data[0];
    synthetic[1] = data[1];
    synthetic[2] = data[2];
    synthetic[3] = data[3];
    synthetic[4] = frame->embedded_channel;
    synthetic[5] = static_cast<uint8_t>((frame->embedded_payload_length >> 8) & 0xFF);
    synthetic[6] = static_cast<uint8_t>(frame->embedded_payload_length & 0xFF);
    std::copy(data.begin() + static_cast<std::ptrdiff_t>(embedded_payload_offset),
              data.begin() + static_cast<std::ptrdiff_t>(
                  embedded_payload_offset + frame->embedded_payload_length),
              synthetic.begin() + static_cast<std::ptrdiff_t>(XNA_HEADER_SIZE));
    if (!is_xnaftw_response_frame(synthetic)) {
        return std::nullopt;
    }
    return synthetic;
}

std::string describe_xna_control_opcode(uint16_t opcode) {
    switch (opcode) {
    case 0x00B1:
        return "teardown-final?";
    case 0x00D1:
        return "teardown-pulse?";
    case 0x01D2:
        return "delayed-transfer-status?";
    default:
        return {};
    }
}

void maybe_log_non_xnaftw_frame(const mtp::ByteArray& data) {
    if (!g_verbose) return;

    auto frame = parse_xna_secure_frame(data);
    if (!frame || frame->has_xnaftw_magic) return;

    std::ostringstream oss;
    oss << "      [FRAME] seq=0x" << std::hex << std::setw(8) << std::setfill('0')
        << frame->sequence
        << " ch=" << std::dec << static_cast<int>(frame->channel)
        << " len=" << frame->payload_length;

    if (frame->has_control_opcode) {
        oss << " opcode=0x" << std::hex << std::setw(4) << std::setfill('0')
            << frame->control_opcode;
        if (auto label = describe_xna_control_opcode(frame->control_opcode); !label.empty()) {
            oss << " (" << label << ")";
        }
        if (frame->has_control_value) {
            oss << " value=0x" << std::hex << std::setw(4) << std::setfill('0')
                << frame->control_value;
        }
    }

    if (frame->has_embedded_segment) {
        oss << " embedded[ch=" << std::dec << static_cast<int>(frame->embedded_channel)
            << " len=" << frame->embedded_payload_length;
        if (frame->embedded_has_xnaftw_magic) {
            oss << " xnaftw";
        }
        oss << "]";
    }

    std::cout << oss.str() << std::endl;
}

bool is_xnaftw_response_frame(const mtp::ByteArray& data) {
    if (data.size() < XNA_HEADER_SIZE + XNA_MAGIC_LEN + 1) return false;
    if (memcmp(data.data() + XNA_HEADER_SIZE, XNA_MAGIC, XNA_MAGIC_LEN) != 0) return false;
    return data[XNA_HEADER_SIZE + XNA_MAGIC_LEN] == XNAFTW_RESPONSE;
}

std::optional<ParsedXnaResponse> parse_xnaftw_response(const mtp::ByteArray& data) {
    if (!is_xnaftw_response_frame(data)) return std::nullopt;

    size_t offset = XNA_HEADER_SIZE + XNA_MAGIC_LEN + 1;
    ParsedXnaResponse response;
    if (!read_bool(data, offset, response.faulted) ||
        !read_bool(data, offset, response.requires_delayed_parameter)) {
        return std::nullopt;
    }

    if (response.faulted) {
        uint32_t fault_id = 0;
        if (!read_u32le(data, offset, fault_id)) return std::nullopt;
        response.fault_id = static_cast<int32_t>(fault_id);
        if (!read_prefixed_utf16_string(data, offset, response.fault_message)) {
            response.fault_message = "<invalid fault string>";
        }
        return response;
    }

    if (offset >= data.size()) return std::nullopt;
    response.value_type = data[offset++];

    switch (response.value_type) {
    case 0x00:
        if (offset >= data.size()) return std::nullopt;
        response.has_byte_value = true;
        response.byte_value = data[offset++];
        response.value_summary = std::to_string(response.byte_value);
        break;
    case 0x01:
        if (!read_bool(data, offset, response.bool_value)) return std::nullopt;
        response.has_bool_value = true;
        response.value_summary = response.bool_value ? "true" : "false";
        break;
    case 0x03: {
        uint32_t value = 0;
        if (!read_u32le(data, offset, value)) return std::nullopt;
        response.value_summary = std::to_string(value);
        break;
    }
    case 0x08: {
        std::string value;
        if (!read_prefixed_utf16_string(data, offset, value)) return std::nullopt;
        response.value_summary = "\"" + value + "\"";
        break;
    }
    default: {
        std::ostringstream oss;
        oss << "type 0x" << std::hex << std::setw(2) << std::setfill('0')
            << static_cast<int>(response.value_type);
        response.value_summary = oss.str();
        break;
    }
    }

    return response;
}

std::string describe_xna_response(const ParsedXnaResponse& response) {
    if (response.faulted) {
        std::ostringstream oss;
        oss << "fault 0x" << std::hex << response.fault_id << std::dec;
        if (!response.fault_message.empty()) {
            oss << " \"" << response.fault_message << "\"";
        }
        return oss.str();
    }

    if (response.requires_delayed_parameter && response.has_byte_value) {
        return "delayed parameter #" + std::to_string(response.byte_value);
    }

    if (!response.value_summary.empty()) return response.value_summary;
    return "type 0x" + std::to_string(response.value_type);
}

bool is_success_xna_response(const mtp::ByteArray& data, bool* bool_value = nullptr) {
    auto parsed = parse_xnaftw_response(data);
    if (!parsed || parsed->faulted || parsed->requires_delayed_parameter) {
        return false;
    }
    if (bool_value && parsed->has_bool_value) {
        *bool_value = parsed->bool_value;
    }
    return true;
}

// ── XNA Session Controller ─────────────────────────────────────────────

class XnaSession {
    std::shared_ptr<mtp::Session> session_;
    XnaFrameBuilder builder_;
    std::deque<mtp::ByteArray> pending_rx_;
    std::vector<SchemaVerb> last_schema_;

    mtp::ByteArray try_read_frame_once() {
        mtp::ByteArray data;
        try {
            data = session_->Operation9223();
        } catch (const mtp::InvalidResponseException& ex) {
            if (!is_retryable_xna_poll_error(ex)) throw;
        }
        return data;
    }

    void log_inbound_frame(const mtp::ByteArray& data) {
        if (data.empty()) return;
        hex_dump("RX", data);
        maybe_log_non_xnaftw_frame(data);
        // Capture schema frames (XNAFTW type 0x00) as they pass through
        auto verbs = parse_xnaftw_schema(data);
        if (!verbs.empty()) {
            last_schema_ = verbs;
            log_ok("Channel schema (" + std::to_string(verbs.size()) + " verbs):");
            log_schema(verbs);
        }
    }

    // Channel close from device — high byte is channel ID, low byte is 0xC1
    bool frame_is_channel_close(const mtp::ByteArray& data) const {
        auto frame = parse_xna_secure_frame(data);
        if (!frame || !frame->has_control_opcode) return false;
        return (frame->control_opcode & 0xFF) == 0xC1;
    }

    enum class DelayedDrainState {
        Continue,
        ResponseQueued,
        DeviceClosing,
    };

    DelayedDrainState drain_delayed_transfer_feedback() {
        int idle_polls = 0;
        for (int i = 0; i < XNA_DELAYED_DRAIN_MAX_POLLS; ++i) {
            auto data = try_read_frame_once();
            if (data.empty()) {
                ++idle_polls;
                if (idle_polls >= XNA_DELAYED_DRAIN_IDLE_POLLS) {
                    return DelayedDrainState::Continue;
                }
                if (XNA_DELAYED_DRAIN_DELAY_MS > 0) {
                    std::this_thread::sleep_for(
                        std::chrono::milliseconds(XNA_DELAYED_DRAIN_DELAY_MS));
                }
                continue;
            }

            idle_polls = 0;
            log_inbound_frame(data);
            pending_rx_.push_back(data);

            if (auto embedded = extract_embedded_xnaftw_response(data)) {
                pending_rx_.push_back(*embedded);
                return DelayedDrainState::ResponseQueued;
            }
            if (is_xnaftw_response(data)) {
                return DelayedDrainState::ResponseQueued;
            }
            if (frame_is_channel_close(data)) {
                return DelayedDrainState::DeviceClosing;
            }
        }
        return DelayedDrainState::Continue;
    }

public:
    explicit XnaSession(std::shared_ptr<mtp::Session> session) : session_(session) {}

    void send_frame(const mtp::ByteArray& frame) {
        hex_dump("TX", frame);
        session_->Operation9222(frame);
    }

    mtp::ByteArray poll(int max_polls = MAX_POLLS) {
        if (!pending_rx_.empty()) {
            auto data = pending_rx_.front();
            pending_rx_.pop_front();
            return data;
        }
        for (int i = 0; i < max_polls; i++) {
            auto data = try_read_frame_once();
            if (!data.empty()) {
                log_inbound_frame(data);
                return data;
            }
            if (g_verbose && (i % 10 == 0) && i > 0)
                std::cout << "      (polling... " << i << "/" << max_polls << ")" << std::endl;
            std::this_thread::sleep_for(std::chrono::milliseconds(POLL_DELAY_MS));
        }
        return {};
    }

    mtp::ByteArray send_and_poll(const mtp::ByteArray& frame, int max_polls = MAX_POLLS) {
        send_frame(frame);
        return poll(max_polls);
    }

    mtp::ByteArray wait_for_xnaftw_response(int max_polls = MAX_POLLS) {
        mtp::ByteArray last;
        while (!pending_rx_.empty()) {
            auto data = pending_rx_.front();
            pending_rx_.pop_front();
            last = data;
            if (is_xnaftw_response(data)) {
                return data;
            }
            if (auto embedded = extract_embedded_xnaftw_response(data)) {
                return *embedded;
            }
        }
        for (int i = 0; i < max_polls; i++) {
            auto data = try_read_frame_once();
            if (!data.empty()) {
                log_inbound_frame(data);
                last = data;
                if (is_xnaftw_response(data)) {
                    return data;
                }
                if (auto embedded = extract_embedded_xnaftw_response(data)) {
                    return *embedded;
                }
            }
            if (g_verbose && (i % 10 == 0) && i > 0) {
                std::cout << "      (polling... " << i << "/" << max_polls << ")" << std::endl;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(POLL_DELAY_MS));
        }
        return last;
    }

    static bool is_xnaftw_response(const mtp::ByteArray& data) {
        return is_xnaftw_response_frame(data);
    }

    XnaFrameBuilder& builder() { return builder_; }
    const std::vector<SchemaVerb>& last_schema() const { return last_schema_; }

    // ── High-level protocol operations ──────────────────────────────

    bool open_xna_session_cmac(const std::array<uint32_t, 4>& cmac) {
        log_op("XnaSessionOpen (CMAC)");
        try {
            session_->Operation9220(cmac[0], cmac[1], cmac[2], cmac[3]);
        } catch (const std::exception& e) {
            // Op9220 returns vendor-specific 0xA221 on success, which RunTransaction
            // throws as InvalidResponseException.
            std::string err = e.what();
            if (err.find("0xa221") != std::string::npos || err.find("0xA221") != std::string::npos) {
                log_ok("XNA session opened (0xA221)");
                return true;
            }
            log_err(std::string("XnaSessionOpen failed: ") + err);
            return false;
        }
        log_ok("XNA session opened (0x2001)");
        return true;
    }

    bool wait_for_hello() {
        log_op("Waiting for device hello...");
        auto hello = poll();
        if (hello.empty()) {
            log_err("No hello from device");
            return false;
        }
        if (hello.size() >= 4 && memcmp(hello.data(), XNA_HELLO_MAGIC, 4) == 0) {
            log_ok("Received 'XX' hello from device");
            return true;
        }
        log_err("Unexpected hello data");
        return false;
    }

    bool send_registration(const std::string& device_name) {
        log_op("Sending device registration: " + device_name);

        // Registration is a short frame (112 bytes), NOT a full 1264-byte frame
        // Format: 7-byte header (paylen=0x50, msg_type=0xC5) + 80 bytes data + NO MAC
        mtp::ByteArray reg(112, 0);

        // Header
        // seq = 0 (already zero)
        // channel = 0, type = 0 (already zero)
        reg[6] = 0x50;  // payload_length = 80 bytes

        // Payload starts at byte 7
        reg[7] = MSG_REGISTRATION;  // 0xC5 message type

        // Device name in UTF-16LE at offset 13 (byte 7 + 6)
        size_t name_offset = 13;
        for (size_t i = 0; i < device_name.size() && name_offset + 1 < 112; i++) {
            reg[name_offset] = static_cast<uint8_t>(device_name[i]);
            reg[name_offset + 1] = 0x00;
            name_offset += 2;
        }

        session_->Operation9222(reg);
        log_ok("Registration sent");
        return true;
    }

    // Open the XnaChannelBroker — must be done before each CreateChannel RPC
    bool open_broker(uint8_t broker_channel_id) {
        log_op("Opening XnaChannelBroker (id=" + std::to_string(broker_channel_id) + ")");
        auto payload = XnaFrameBuilder::build_channel_open_payload(
            broker_channel_id, "XnaChannelBroker");
        auto frame = builder_.build_mgmt_frame(0x00, MSG_CHANNEL_OPEN, payload);
        auto resp = send_and_poll(frame);
        if (resp.empty()) {
            log_err("No broker channel response");
            return false;
        }
        log_ok("Broker channel opened");
        return true;
    }

    // Send channel ack
    bool send_ack(uint8_t ack_param) {
        log_op("Channel ack (param=" + std::to_string(ack_param) + ")");
        std::vector<uint8_t> data = { ack_param, 0x00 };
        auto frame = builder_.build_mgmt_frame(0x00, MSG_CHANNEL_ACK, data);
        auto resp = send_and_poll(frame);
        return !resp.empty();
    }

    // Send channel close
    bool send_close(uint8_t close_param) {
        log_op("Channel close (param=" + std::to_string(close_param) + ")");
        std::vector<uint8_t> data = { close_param, 0x00 };
        auto frame = builder_.build_mgmt_frame(0x00, MSG_CHANNEL_CLOSE, data);
        auto resp = send_and_poll(frame);
        return !resp.empty();
    }

    // Open a named channel (XNACHAN1 or XNACHAN2)
    bool open_named_channel(uint8_t setup_flag, const std::string& name) {
        log_op("Opening named channel: " + name);
        auto payload = XnaFrameBuilder::build_channel_open_payload(setup_flag, name);
        auto frame = builder_.build_mgmt_frame(0x00, MSG_CHANNEL_OPEN, payload);
        auto resp = send_and_poll(frame);
        if (resp.empty()) {
            log_err("No channel open response for " + name);
            return false;
        }
        log_ok("Channel " + name + " opened");
        return true;
    }

    // Full channel setup sequence:
    // open broker → ack → CreateChannel RPC → close broker → open named → ack
    bool setup_channel(uint8_t broker_id, uint8_t channel_num,
                       const std::string& channel_name, const Guid& channel_guid) {
        // 1. Open broker
        if (!open_broker(broker_id)) return false;

        // 2. Ack broker open using the broker id from the open frame
        if (!send_ack(broker_id)) return false;

        // 3. CreateChannel RPC on broker
        {
            XnaParamBuilder params;
            params.add_guid("ChannelId", channel_guid);

            auto xnaftw_payload = XnaFrameBuilder::build_xnaftw_request(
                "CreateChannel", params.serialize());
            auto frame = builder_.build_xnaftw_frame(channel_num, xnaftw_payload);
            auto resp = send_and_poll(frame);
            if (resp.empty()) {
                log_err("No CreateChannel response");
                return false;
            }
            log_ok("CreateChannel completed");
        }

        // 4. Close broker
        if (!send_close(broker_id)) return false;

        // 5. Open named channel
        if (!open_named_channel(0x01, channel_name)) return false;

        // 6. Ack named channel using the fixed named-channel id from the capture
        if (!send_ack(MSG_NAMED_CHANNEL)) return false;

        // 7. Schema arrives asynchronously on the named channel — it will be
        //    captured by log_inbound_frame() during the first RPC poll cycle.

        return true;
    }

    // Send XNAFTW RPC call and get response
    mtp::ByteArray xnaftw_call(uint8_t channel, const std::string& method,
                                const std::vector<uint8_t>& serialized_params) {
        log_op("XNAFTW: " + method);

        auto xnaftw_payload = XnaFrameBuilder::build_xnaftw_request(method, serialized_params);
        auto frame = builder_.build_xnaftw_frame(channel, xnaftw_payload);

        send_frame(frame);
        auto resp = wait_for_xnaftw_response();
        if (resp.empty()) {
            log_err("No response for " + method);
        } else if (auto parsed = parse_xnaftw_response(resp)) {
            if (parsed->faulted) {
                log_err(method + " failed: " + describe_xna_response(*parsed));
            } else {
                log_ok(method + " completed [" + describe_xna_response(*parsed) + "]");
            }
        } else {
            log_ok(method + " completed");
        }
        return resp;
    }

    // Send delayed parameter content when the device explicitly requests it.
    mtp::ByteArray send_binary_content(uint8_t channel, const std::string& method,
                                       const std::vector<uint8_t>& params_with_binary_ref,
                                       const std::vector<uint8_t>& content,
                                       uint8_t delayed_param_index) {
        log_op("XNAFTW: " + method + " (" + std::to_string(content.size()) + " bytes)");

        auto xnaftw_payload = XnaFrameBuilder::build_xnaftw_request(method, params_with_binary_ref);
        auto frame = builder_.build_xnaftw_frame(channel, xnaftw_payload);
        send_frame(frame);

        auto resp = wait_for_xnaftw_response();
        if (resp.empty()) {
            log_err("No pre-transfer response for " + method);
            return {};
        }

        auto parsed = parse_xnaftw_response(resp);
        if (!parsed) {
            log_err("Malformed response for " + method);
            return resp;
        }
        if (parsed->faulted) {
            log_err(method + " failed: " + describe_xna_response(*parsed));
            return resp;
        }
        if (!parsed->requires_delayed_parameter) {
            log_ok(method + " completed [" + describe_xna_response(*parsed) + "]");
            return resp;
        }
        if (!parsed->has_byte_value || parsed->byte_value != delayed_param_index) {
            log_err("Unexpected delayed parameter request for " + method + ": " +
                    describe_xna_response(*parsed));
            return resp;
        }

        log_ok(method + " awaiting delayed parameter #" +
               std::to_string(delayed_param_index));

        size_t content_offset = 0;
        while (parsed->requires_delayed_parameter) {
            if (content_offset >= content.size()) {
                log_err("Device requested more delayed content than is available");
                return resp;
            }

            // Send frames with inter-frame pacing, wait for device ACK between
            // 128KB chunks. The device sends 0x01d2 status frames as flow control —
            // we must read at least one before sending the next chunk, or the device's
            // buffer overflows and it closes the channel on large files.
            bool got_early_response = false;
            while (content_offset < content.size() && !got_early_response) {
                size_t delayed_size = std::min(content.size() - content_offset,
                                               static_cast<size_t>(XNA_DELAYED_CHUNK_LIMIT));
                std::vector<uint8_t> delayed_payload;
                delayed_payload.reserve(5 + delayed_size);
                delayed_payload.push_back(0x00);
                delayed_payload.push_back(static_cast<uint8_t>(delayed_size & 0xFF));
                delayed_payload.push_back(static_cast<uint8_t>((delayed_size >> 8) & 0xFF));
                delayed_payload.push_back(static_cast<uint8_t>((delayed_size >> 16) & 0xFF));
                delayed_payload.push_back(static_cast<uint8_t>((delayed_size >> 24) & 0xFF));
                delayed_payload.insert(
                    delayed_payload.end(),
                    content.begin() + static_cast<std::ptrdiff_t>(content_offset),
                    content.begin() + static_cast<std::ptrdiff_t>(content_offset + delayed_size));

                // Send all frames for this chunk with inter-frame pacing
                size_t frame_offset = 0;
                while (frame_offset < delayed_payload.size()) {
                    size_t frame_size = std::min(delayed_payload.size() - frame_offset,
                                                 XNA_MAX_PAYLOAD);
                    try {
                        send_frame(builder_.build_binary_frame(
                            channel,
                            delayed_payload.data() + frame_offset,
                            frame_size));
                    } catch (const std::exception& ex) {
                        log_err("Delayed transfer write failed for " + method + ": " + ex.what());
                        return resp;
                    }
                    frame_offset += frame_size;
                    if (XNA_DELAYED_FRAME_DELAY_MS > 0) {
                        std::this_thread::sleep_for(
                            std::chrono::milliseconds(XNA_DELAYED_FRAME_DELAY_MS));
                    }
                }

                content_offset += delayed_size;

                // Drain ALL pending device status frames before sending next chunk.
                if (content_offset < content.size()) {
                    int idle_count = 0;
                    for (int dp = 0; dp < MAX_POLLS && idle_count < 3; dp++) {
                        auto data = try_read_frame_once();
                        if (data.empty()) {
                            idle_count++;
                            std::this_thread::sleep_for(std::chrono::milliseconds(POLL_DELAY_MS));
                            continue;
                        }
                        idle_count = 0;
                        log_inbound_frame(data);
                        if (is_xnaftw_response(data)) {
                            pending_rx_.push_back(data);
                            got_early_response = true;
                            break;
                        }
                        if (auto embedded = extract_embedded_xnaftw_response(data)) {
                            pending_rx_.push_back(*embedded);
                            got_early_response = true;
                            break;
                        }
                    }
                }
            }

            if (got_early_response) {
                content_offset = content.size();
            }

            resp = wait_for_xnaftw_response();
            if (resp.empty()) {
                log_err("No final response for " + method);
                return {};
            }

            parsed = parse_xnaftw_response(resp);
            if (!parsed) {
                log_err("Malformed post-transfer response for " + method);
                return resp;
            }
            if (parsed->faulted) {
                log_err(method + " failed: " + describe_xna_response(*parsed));
                return resp;
            }
        }

        if (content_offset != content.size()) {
            log_err(method + " stopped requesting delayed content after " +
                    std::to_string(content_offset) + " of " +
                    std::to_string(content.size()) + " bytes");
        } else {
            log_ok(method + " completed [" + describe_xna_response(*parsed) + "]");
        }
        return resp;
    }

    void close_xna_session() {
        log_op("XnaSessionClose");
        session_->Operation9221();
        log_ok("XNA session closed");
    }
};

// ── Main ────────────────────────────────────────────────────────────────

void print_usage(const char* prog) {
    std::cout << "Usage: " << prog << " <app.exe|package.ccgame> [options]" << std::endl;
    std::cout << std::endl;
    std::cout << "Options:" << std::endl;
    std::cout << "  --name <name>         Game title (default: filename)" << std::endl;
    std::cout << "  --description <desc>  Game description" << std::endl;
    std::cout << "  --thumbnail <png>     Thumbnail PNG file" << std::endl;
    std::cout << "  --runtime-dir <dir>   Directory containing Zune runtime DLLs" << std::endl;
    std::cout << "  --launch              Launch app on device after deploy" << std::endl;
    std::cout << "  --verbose             Show protocol details" << std::endl;
    std::cout << "  --help                Show this help" << std::endl;
}

int main(int argc, char* argv[]) {
    if (argc < 2) { print_usage(argv[0]); return 1; }

    std::string app_path;
    std::string game_name;
    std::string description = "Deployed by Xune";
    std::string thumbnail_path;
    std::string runtime_dir_override;
    bool name_overridden = false;
    bool description_overridden = false;
    bool thumbnail_overridden = false;
    bool launch_after_deploy = false;

    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        if (arg == "--help") { print_usage(argv[0]); return 0; }
        else if (arg == "--verbose") g_verbose = true;
        else if (arg == "--name" && i + 1 < argc) {
            game_name = argv[++i];
            name_overridden = true;
        }
        else if (arg == "--description" && i + 1 < argc) {
            description = argv[++i];
            description_overridden = true;
        }
        else if (arg == "--thumbnail" && i + 1 < argc) {
            thumbnail_path = argv[++i];
            thumbnail_overridden = true;
        }
        else if (arg == "--runtime-dir" && i + 1 < argc) {
            runtime_dir_override = argv[++i];
        }
        else if (arg == "--launch") launch_after_deploy = true;
        else if (arg[0] != '-') app_path = arg;
    }

    if (app_path.empty() || !fs::exists(app_path)) {
        std::cerr << "ERROR: App file not found: " << app_path << std::endl;
        return 1;
    }

    auto source_path = fs::path(app_path);
    auto source_filename = source_path.filename().string();
    uintmax_t source_size = fs::file_size(source_path);

    std::string app_filename = source_filename;
    std::vector<uint8_t> app_data;
    std::vector<uint8_t> thumbnail_data;
    std::vector<CcgamePayload::FileEntry> deploy_files;
    std::string package_runtime_profile;
    std::string runtime_token = RUNTIME_TOKEN;
    Guid container_id = Guid::zero();
    std::optional<RuntimePayload> runtime_payload;

    if (to_lower(source_path.extension().string()) == ".ccgame") {
        auto package = load_ccgame_payload(source_path);
        if (!package) return 1;
        app_filename = package->startup_assembly;
        app_data = std::move(package->app_data);
        deploy_files = std::move(package->files);
        if (!name_overridden && !package->product_name.empty()) {
            game_name = package->product_name;
        }
        if (!description_overridden && !package->description.empty()) {
            description = package->description;
        }
        if (!thumbnail_overridden && !package->thumbnail_data.empty()) {
            thumbnail_data = std::move(package->thumbnail_data);
        }
        package_runtime_profile = package->runtime_profile;
        runtime_token = effective_runtime_token(package->runtime_profile);
        if (package->has_game_guid) {
            container_id = Guid::from_bytes(package->game_guid_bytes);
        }
    } else {
        app_data = read_binary_file(source_path);
        deploy_files.push_back({app_filename, app_data});
    }

    if (app_data.empty()) {
        std::cerr << "ERROR: Failed to read app payload: " << app_path << std::endl;
        return 1;
    }

    if (game_name.empty()) {
        game_name = fs::path(app_filename).stem().string();
    }

    if (!thumbnail_path.empty()) {
        if (!fs::exists(thumbnail_path)) {
            std::cerr << "ERROR: Thumbnail not found: " << thumbnail_path << std::endl;
            return 1;
        }
        thumbnail_data = read_binary_file(thumbnail_path);
    }

    if (deploy_files.empty()) {
        deploy_files.push_back({app_filename, app_data});
    }

    std::cout << "╔══════════════════════════════════════════════════════════╗" << std::endl;
    std::cout << "║  XNA Deploy Test CLI                                     ║" << std::endl;
    std::cout << "╚══════════════════════════════════════════════════════════╝" << std::endl;
    std::cout << "  App:         " << app_filename << " (" << app_data.size() << " bytes)" << std::endl;
    if (app_filename != source_filename) {
        std::cout << "  Package:     " << source_filename << " (" << source_size << " bytes)" << std::endl;
    }
    std::cout << "  Name:        " << game_name << std::endl;
    std::cout << "  Description: " << description << std::endl;
    if (to_lower(source_path.extension().string()) == ".ccgame") {
        std::cout << "  Files:       " << deploy_files.size() << std::endl;
    }
    if (!package_runtime_profile.empty()) {
        std::cout << "  Runtime:     " << package_runtime_profile;
        if (package_runtime_profile != runtime_token) {
            std::cout << " -> " << runtime_token;
        }
        std::cout << std::endl;
    }
    if (!thumbnail_data.empty()) {
        if (!thumbnail_path.empty()) {
            std::cout << "  Thumbnail:   " << thumbnail_path << " (" << thumbnail_data.size() << " bytes)" << std::endl;
        } else {
            std::cout << "  Thumbnail:   packaged image (" << thumbnail_data.size() << " bytes)" << std::endl;
        }
    }
    if (!runtime_dir_override.empty()) {
        if (auto runtime_dir = normalize_existing_directory(fs::path(runtime_dir_override))) {
            std::cout << "  Runtime Dir: " << runtime_dir->string() << std::endl;
        } else {
            std::cerr << "ERROR: Runtime directory not found: " << runtime_dir_override << std::endl;
            return 1;
        }
    }
    std::cout << std::endl;

    try {
    // ── Connect ─────────────────────────────────────────────────────

    log_phase("Connect to Zune");

    auto usb_ctx = std::make_shared<mtp::usb::Context>();
    auto devices = usb_ctx->GetDevices();
    mtp::DevicePtr device;

    for (auto desc : devices) {
        if (desc->GetVendorId() != 0x045E) continue;
        try {
            device = mtp::Device::Open(usb_ctx, desc, true, false);
            if (device) break;
        } catch (...) {}
    }

    if (!device) {
        std::cerr << "ERROR: No Zune device found" << std::endl;
        return 1;
    }

    auto session = device->OpenSession(1);
    if (!session) {
        std::cerr << "ERROR: Failed to open MTP session" << std::endl;
        return 1;
    }
    log_ok("MTP session opened");

    auto devinfo = session->GetDeviceInfo();
    log_ok("Device: " + devinfo.Manufacturer + " " + devinfo.Model);
    log_ok("Version: " + devinfo.DeviceVersion);
    log_ok("Serial: " + devinfo.SerialNumber);

    if (g_verbose) {
        bool has_9220 = false, has_9221 = false, has_9222 = false, has_9223 = false;
        for (auto op : devinfo.OperationsSupported) {
            if (static_cast<uint16_t>(op) == 0x9220) has_9220 = true;
            if (static_cast<uint16_t>(op) == 0x9221) has_9221 = true;
            if (static_cast<uint16_t>(op) == 0x9222) has_9222 = true;
            if (static_cast<uint16_t>(op) == 0x9223) has_9223 = true;
        }
        std::cout << "  XNA ops: 9220=" << has_9220 << " 9221=" << has_9221
                  << " 9222=" << has_9222 << " 9223=" << has_9223 << std::endl;
    }

    // ── XNA Session Setup ──────────────────────────────────────────

    log_phase("XNA Session Setup");

    // GetDevicePropValue(0xD21A)
    log_op("GetDevicePropValue(0xD21A)");
    try { session->GetDeviceProperty(mtp::DeviceProperty(0xD21A)); } catch (...) {}
    log_ok("DeviceInfo read");

    // Op9220 probe
    log_op("Op9220 probe (expect failure)");
    try { session->Operation9220(); } catch (...) {}
    log_ok("Probe done");

    // MTPZ handshake — TrustedApp must outlive Op9220
    std::string mtpz_path = std::string(getenv("HOME") ? getenv("HOME") : ".") + "/.mtpz-data";
    auto trustedApp = mtp::TrustedApp::Create(session, mtpz_path);
    if (!trustedApp || !trustedApp->KeysLoaded()) {
        std::cerr << "ERROR: MTPZ keys not found" << std::endl;
        return 1;
    }

    // MTPZ handshake — returns CMAC for Op9220
    log_op("MTPZ AuthenticateForXna");
    std::array<uint32_t, 4> cmac;
    try {
        cmac = trustedApp->AuthenticateForXna();
        log_ok("MTPZ OK — CMAC ready");
        if (g_verbose) {
            printf("    CMAC: %08X-%08X-%08X-%08X\n", cmac[0], cmac[1], cmac[2], cmac[3]);
        }
    }
    catch (const std::exception& e) { log_err(std::string("Authenticate: ") + e.what()); return 1; }

    // GetStorageInfo — required between MTPZ handshake and Op9220
    log_op("GetStorageInfo(0x10001)");
    try { session->GetStorageInfo(mtp::StorageId(0x10001)); } catch (...) {}
    log_ok("StorageInfo retrieved");

    // Open XNA session with CMAC from MTPZ handshake
    XnaSession xna(session);

    if (!xna.open_xna_session_cmac(cmac)) return 1;
    if (!xna.wait_for_hello()) return 1;
    if (!xna.send_registration("Zune HD")) return 1;

    // ── XNACHAN1: Runtime Check ────────────────────────────────────

    log_phase("Runtime Check (XNACHAN1)");

    Guid chan1_guid = Guid::from_canonical_string("30D0E81E-D272-4735-ABD3-918ADAD29FD3");
    if (!xna.setup_channel(0x01, 0x01, "XNACHAN1", chan1_guid)) return 1;

    // IsRuntimeAvailable on XNACHAN1
    {
        XnaParamBuilder params;
        params.add_string("runtimeToken", 0x08, runtime_token);
        params.add_uint32("exactVersion", 0x03, RUNTIME_REVISION);

        bool runtime_available = false;
        auto resp = xna.xnaftw_call(0x01, "IsRuntimeAvailable", params.serialize());
        if (is_success_xna_response(resp, &runtime_available) && runtime_available) {
            log_ok("Runtime '" + runtime_token + "' is available");
        } else {
            if (!runtime_payload) {
                auto runtime_dir = find_runtime_directory(argv[0], runtime_dir_override);
                if (!runtime_dir) {
                    log_err("Runtime not available and no runtime directory was found");
                    std::cerr << "ERROR: Expected runtime DLLs under XuneSyncLibrary/redocs/xna_zune_runtime"
                              << " or via --runtime-dir" << std::endl;
                    return 1;
                }

                runtime_payload = load_runtime_payload(*runtime_dir);
                if (!runtime_payload) {
                    return 1;
                }
            }

            log_ok("Runtime '" + runtime_token + "' is missing; deploying " +
                   std::to_string(runtime_payload->files.size()) +
                   " files from " + runtime_payload->directory.string());

            XnaParamBuilder open_params;
            open_params.add_string("runtimeToken", 0x08, runtime_token);
            open_params.add_uint32("exactVersion", 0x03, RUNTIME_REVISION);

            auto open_resp = xna.xnaftw_call(MSG_NAMED_CHANNEL, "OpenRuntimeContainer",
                                             open_params.serialize());
            if (!is_success_xna_response(open_resp)) return 1;

            for (const auto& file : runtime_payload->files) {
                XnaParamBuilder file_params;
                file_params.add_string("filePath", 0x08, file.relative_path);
                uint8_t file_content_index =
                    file_params.add_binary_ref("fileContent", static_cast<uint32_t>(file.data.size()));
                auto file_resp = xna.send_binary_content(MSG_NAMED_CHANNEL, "PutFileInContainer",
                                                         file_params.serialize(), file.data,
                                                         file_content_index);
                if (!is_success_xna_response(file_resp)) return 1;
            }

            XnaParamBuilder close_params;
            auto close_resp = xna.xnaftw_call(MSG_NAMED_CHANNEL, "CloseRuntimeContainer",
                                              close_params.serialize());
            if (!is_success_xna_response(close_resp)) return 1;

            log_ok("Runtime deployment completed");
        }
    }

    // Close XNACHAN1
    xna.send_close(0x01);

    // ── XNACHAN2: Game Deploy (re-open broker for second channel) ───

    log_phase("Game Deploy (XNACHAN2)");

    Guid chan2_guid = Guid::from_canonical_string("AA3C2881-4EB9-4AF6-8137-635C2E64CE4A");
    if (!xna.setup_channel(0x02, 0x02, "XNACHAN2", chan2_guid)) return 1;

    // OpenGameContainer
    {
        XnaParamBuilder params;
        params.add_guid("containerId", container_id);
        params.add_string("titleName", 0x08, game_name);

        auto resp = xna.xnaftw_call(MSG_NAMED_CHANNEL, "OpenGameContainer", params.serialize());
        if (!is_success_xna_response(resp)) return 1;
    }

    // PutFileInContainer
    for (size_t fi = 0; fi < deploy_files.size(); fi++) {
        const auto& file = deploy_files[fi];
        log_op("[" + std::to_string(fi + 1) + "/" + std::to_string(deploy_files.size()) +
               "] " + file.relative_path);
        XnaParamBuilder params;
        params.add_string("filePath", 0x08, file.relative_path);
        uint8_t file_content_index =
            params.add_binary_ref("fileContent", static_cast<uint32_t>(file.data.size()));
        auto resp = xna.send_binary_content(MSG_NAMED_CHANNEL, "PutFileInContainer",
                                            params.serialize(), file.data, file_content_index);
        if (!is_success_xna_response(resp)) return 1;
    }

    // PutThumbnailInContainer
    if (!thumbnail_data.empty()) {
        XnaParamBuilder params;
        uint8_t thumbnail_content_index =
            params.add_binary_ref("thumbnailContent", static_cast<uint32_t>(thumbnail_data.size()));

        auto resp = xna.send_binary_content(MSG_NAMED_CHANNEL, "PutThumbnailInContainer",
                                            params.serialize(), thumbnail_data, thumbnail_content_index);
        if (!is_success_xna_response(resp)) return 1;
    }

    // PutGamePropertiesEx
    {
        XnaParamBuilder params;
        params.add_guid("containerId", container_id);
        params.add_string("name", 0x08, game_name);
        params.add_string("description", 0x08, description);
        params.add_string("startupAssembly", 0x08, app_filename);
        params.add_string("runtimeProfile", 0x08, runtime_token);

        auto resp = xna.xnaftw_call(MSG_NAMED_CHANNEL, "PutGamePropertiesEx", params.serialize());
        if (!is_success_xna_response(resp)) return 1;
    }

    // CloseGameContainer
    {
        XnaParamBuilder params;  // no params
        auto resp = xna.xnaftw_call(MSG_NAMED_CHANNEL, "CloseGameContainer", params.serialize());
        if (!is_success_xna_response(resp)) return 1;
    }

    // Close XNACHAN2
    xna.send_close(MSG_NAMED_CHANNEL);

    // ── XNACHAN3: Launch (optional) ─────────────────────────────────

    if (launch_after_deploy) {
        log_phase("Launch (XNACHAN3)");

        Guid chan3_guid = Guid::from_canonical_string("A40D216D-FBD3-40D4-B852-DE77478C1475");
        if (!xna.setup_channel(0x03, 0x03, "XNACHAN3", chan3_guid)) return 1;

        // LaunchTestMode(String titleID, String cmdLn, Boolean returnToDevMode)
        {
            XnaParamBuilder params;
            params.add_string("titleID", 0x08, container_id.to_string_n());
            params.add_string("cmdLn", 0x08, "");
            params.add_bool("returnToDevMode", true);

            auto resp = xna.xnaftw_call(MSG_NAMED_CHANNEL, "LaunchTestMode", params.serialize());
            if (!is_success_xna_response(resp)) {
                log_err("LaunchTestMode failed — game was deployed but could not be launched");
            }
        }

        xna.send_close(MSG_NAMED_CHANNEL);
    }

    // ── Teardown ────────────────────────────────────────────────────

    log_phase("Teardown");
    xna.poll(100);

    // Disconnect frame (msg_type 0xB1)
    {
        std::vector<uint8_t> data = { 0x00, 0x00 };
        auto frame = xna.builder().build_mgmt_frame(0x00, 0xB1, data);
        xna.send_frame(frame);
    }

    xna.close_xna_session();

    // MTPZ cleanup
    log_op("Op9216 cleanup");
    try {
        session->Operation9216();
        log_ok("MTPZ cleanup done");
    } catch (...) {
        log_err("MTPZ cleanup failed (non-fatal)");
    }

    log_phase("Done");
    std::cout << "  Game '" << game_name << "' deployed to Zune HD";
    if (launch_after_deploy) std::cout << " and launched";
    std::cout << std::endl;
    std::cout << std::endl;

    return 0;
    } catch (const mtp::usb::DeviceNotFoundException& e) {
        std::cerr << "ERROR: " << e.what() << std::endl;
        return 1;
    } catch (const std::exception& e) {
        std::cerr << "ERROR: " << e.what() << std::endl;
        return 1;
    } catch (...) {
        std::cerr << "ERROR: Unexpected failure during deploy" << std::endl;
        return 1;
    }
}
