/**
 * Upload Test CLI - Multi-track upload following pcap-verified Zune Desktop sequence
 *
 * Accepts a single audio file or a directory of audio files. When given a directory,
 * groups tracks by artist/album and follows the exact multi-album upload sequence
 * observed in Zune Desktop pcap captures (see docs/multi-album-upload-analysis.md).
 *
 * Detects device type (HD vs Classic) and adjusts:
 *   - HD (Pavo):    Artist metadata (0xB218), 16 track props, 6 album props, 21-format batches
 *   - Classic:      No artist metadata,       14 track props, 4 album props, 17-format batches
 *
 * Usage:
 *   upload_test_cli <file_or_directory> [options]
 *
 *   --no-artwork    Skip artwork steps
 *   --verbose       Show detailed MTP logging
 *   --dry-run       Show upload plan without connecting to device
 *   --help          Show this help
 */

#include <iostream>
#include <fstream>
#include <string>
#include <vector>
#include <map>
#include <set>
#include <filesystem>
#include <cstring>
#include <iomanip>
#include <chrono>
#include <thread>
#include <sstream>
#include <algorithm>

#include <taglib/fileref.h>
#include <taglib/tag.h>
#include <taglib/tpropertymap.h>
#include <taglib/mpegfile.h>
#include <taglib/id3v2tag.h>
#include <taglib/attachedpictureframe.h>
#include <taglib/asffile.h>
#include <taglib/asftag.h>
#include <taglib/mp4file.h>
#include <taglib/mp4tag.h>
#include <taglib/mp4coverart.h>
#include <taglib/flacfile.h>
#include <taglib/flacpicture.h>

#include "lib/src/ZuneDevice.h"
#include "lib/src/ZuneDeviceIdentification.h"
#include <mtp/ptp/OutputStream.h>
#include <mtp/ptp/ByteArrayObjectStream.h>
#include <cli/PosixStreams.h>

namespace fs = std::filesystem;

static bool g_verbose = false;

// ── MTP Property Codes ───────────────────────────────────────────────────

static constexpr uint16_t FMT_FOLDER             = 0x3001;
static constexpr uint16_t FMT_MP3                = 0x3009;
static constexpr uint16_t FMT_JPEG               = 0x3801;
static constexpr uint16_t FMT_WMA                = 0xB901;
static constexpr uint16_t FMT_ABSTRACT_ALBUM     = 0xBA03;
static constexpr uint16_t FMT_ARTIST             = 0xB218;

static constexpr uint16_t PROP_STORAGE_ID         = 0xDC01;
static constexpr uint16_t PROP_OBJECT_FORMAT      = 0xDC02;
static constexpr uint16_t PROP_OBJECT_FILENAME    = 0xDC07;
static constexpr uint16_t PROP_PARENT_OBJECT      = 0xDC0B;
static constexpr uint16_t PROP_PERSISTENT_UID     = 0xDC41;
static constexpr uint16_t PROP_NAME               = 0xDC44;
static constexpr uint16_t PROP_ARTIST             = 0xDC46;
static constexpr uint16_t PROP_DATE_AUTHORED      = 0xDC47;
static constexpr uint16_t PROP_REP_SAMPLE_FORMAT  = 0xDC81;
static constexpr uint16_t PROP_REP_SAMPLE_DATA    = 0xDC86;
static constexpr uint16_t PROP_DURATION           = 0xDC89;
static constexpr uint16_t PROP_RATING             = 0xDC8A;
static constexpr uint16_t PROP_TRACK              = 0xDC8B;
static constexpr uint16_t PROP_GENRE              = 0xDC8C;
static constexpr uint16_t PROP_META_GENRE         = 0xDC95;
static constexpr uint16_t PROP_ALBUM_NAME         = 0xDC9A;
static constexpr uint16_t PROP_ALBUM_ARTIST       = 0xDC9B;
static constexpr uint16_t PROP_DISC_NUMBER        = 0xDC9D;
static constexpr uint16_t PROP_ZUNE_COLLECTION_ID = 0xDAB0;
static constexpr uint16_t PROP_ZUNE_DAB2          = 0xDAB2;
static constexpr uint16_t PROP_DAB8               = 0xDAB8;
static constexpr uint16_t PROP_DAB9               = 0xDAB9;
static constexpr uint16_t PROP_DA97               = 0xDA97;

static constexpr uint16_t DT_UINT8   = 0x0002;
static constexpr uint16_t DT_UINT16  = 0x0004;
static constexpr uint16_t DT_UINT32  = 0x0006;
static constexpr uint16_t DT_UINT128 = 0x000A;
static constexpr uint16_t DT_STRING  = 0xFFFF;

// Classic: 17 formats for batch GetObjPropDesc queries
static const uint16_t CLASSIC_FORMATS[] = {
    0x3009, 0xB901, 0x300C, 0xB215, 0xB903, 0xB904, 0xB301,
    0xB981, 0x3801, 0x3001, 0xBA03, 0xBA05, 0xB211, 0xB213,
    0x3000, 0xB802, 0xBA0B,
};

// HD: 21 formats for batch GetObjPropDesc queries
static const uint16_t HD_FORMATS[] = {
    0x3009, 0xB901, 0x300C, 0xB215, 0xB903, 0xB904, 0xB301,
    0xB216, 0xB982, 0xB981, 0x300A, 0x3801, 0x3001, 0xBA03,
    0xBA05, 0xB211, 0x3000, 0xB802, 0xBA0B, 0xB218, 0xB217,
};

// Supported audio extensions
static const std::set<std::string> AUDIO_EXTENSIONS = {
    ".mp3", ".wma", ".m4a", ".flac", ".aac", ".ogg",
};

// ── Data Structures ──────────────────────────────────────────────────────

struct TrackInfo {
    std::string file_path;
    std::string filename;
    std::string artist, album, title, genre;
    int year = 0, track_num = 0, disc_num = 0;
    int rating = -1;  // -1 = not set
    uint32_t duration_ms = 0;
    uint64_t file_size = 0;
    uint16_t format_code = 0;
    std::vector<uint8_t> artwork;
    std::string artist_guid;
};

struct AlbumGroup {
    std::string album_name;
    int year = 0;
    std::vector<TrackInfo> tracks;
    std::vector<uint8_t> artwork;  // best from tracks
};

struct ArtistGroup {
    std::string artist_name;
    std::string artist_guid;
    std::vector<AlbumGroup> albums;
};

// ── Logging ──────────────────────────────────────────────────────────────

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
void log_ok(const std::string& desc) { log_ts("    ✓ " + desc); }
void log_warn(const std::string& desc) { log_ts("    ⚠ " + desc); }

std::string hex(uint32_t v) {
    std::ostringstream ss;
    ss << "0x" << std::hex << std::uppercase << std::setfill('0') << std::setw(8) << v;
    return ss.str();
}

std::string hex16(uint16_t v) {
    std::ostringstream ss;
    ss << "0x" << std::hex << std::uppercase << std::setfill('0') << std::setw(4) << v;
    return ss.str();
}

// ── Property List Helpers ────────────────────────────────────────────────

void write_prop_string(mtp::OutputStream& os, uint16_t prop, const std::string& value) {
    os.Write32(0); os.Write16(prop); os.Write16(DT_STRING); os.WriteString(value);
}

void write_prop_u8(mtp::OutputStream& os, uint16_t prop, uint8_t value) {
    os.Write32(0); os.Write16(prop); os.Write16(DT_UINT8); os.Write8(value);
}

void write_prop_u16(mtp::OutputStream& os, uint16_t prop, uint16_t value) {
    os.Write32(0); os.Write16(prop); os.Write16(DT_UINT16); os.Write16(value);
}

void write_prop_u32(mtp::OutputStream& os, uint16_t prop, uint32_t value) {
    os.Write32(0); os.Write16(prop); os.Write16(DT_UINT32); os.Write32(value);
}

void write_prop_u128(mtp::OutputStream& os, uint16_t prop, const std::vector<uint8_t>& value) {
    os.Write32(0); os.Write16(prop); os.Write16(DT_UINT128);
    for (auto b : value) os.Write8(b);
}

// ── Date Formatting ──────────────────────────────────────────────────────

std::string format_track_date(int year) {
    if (year <= 0 || year < 1000) year = 2000;
    std::ostringstream ss;
    ss << std::setfill('0') << std::setw(4) << year << "0101T160100.0";
    return ss.str();
}

std::string format_album_date(int year) {
    if (year <= 0 || year < 1000) year = 2000;
    std::ostringstream ss;
    ss << std::setfill('0') << std::setw(4) << year << "0102T000100.0";
    return ss.str();
}

// ── GUID Parsing ─────────────────────────────────────────────────────────

// Convert UUID string (e.g. "45a663b5-b1cb-4a91-bff6-2bef7bbfdd76") to 16-byte binary.
// Uses mixed endianness: first 3 components little-endian, last 8 bytes big-endian.
std::vector<uint8_t> parse_guid(const std::string& guid_str) {
    std::string hex_str = guid_str;
    hex_str.erase(std::remove(hex_str.begin(), hex_str.end(), '-'), hex_str.end());
    if (hex_str.length() != 32) return {};

    std::vector<uint8_t> bytes(16);

    // Component 1: 4 bytes (32-bit) little-endian
    for (int i = 3; i >= 0; --i)
        bytes[3 - i] = static_cast<uint8_t>(std::stoul(hex_str.substr(i * 2, 2), nullptr, 16));

    // Component 2: 2 bytes (16-bit) little-endian
    for (int i = 1; i >= 0; --i)
        bytes[4 + (1 - i)] = static_cast<uint8_t>(std::stoul(hex_str.substr(8 + i * 2, 2), nullptr, 16));

    // Component 3: 2 bytes (16-bit) little-endian
    for (int i = 1; i >= 0; --i)
        bytes[6 + (1 - i)] = static_cast<uint8_t>(std::stoul(hex_str.substr(12 + i * 2, 2), nullptr, 16));

    // Component 4: 8 bytes big-endian (as-is)
    for (size_t i = 0; i < 8; ++i)
        bytes[8 + i] = static_cast<uint8_t>(std::stoul(hex_str.substr(16 + i * 2, 2), nullptr, 16));

    return bytes;
}

// ── Artwork Extraction ───────────────────────────────────────────────────

std::vector<uint8_t> extract_artwork(const std::string& file_path) {
    std::vector<uint8_t> artwork;
    TagLib::FileRef fileRef(file_path.c_str());
    if (fileRef.isNull()) return artwork;

    if (auto* f = dynamic_cast<TagLib::MPEG::File*>(fileRef.file())) {
        if (f->ID3v2Tag()) {
            auto frames = f->ID3v2Tag()->frameList("APIC");
            if (!frames.isEmpty()) {
                if (auto* pic = dynamic_cast<TagLib::ID3v2::AttachedPictureFrame*>(frames.front())) {
                    auto data = pic->picture();
                    artwork.assign(data.begin(), data.end());
                    return artwork;
                }
            }
        }
    }
    if (auto* f = dynamic_cast<TagLib::ASF::File*>(fileRef.file())) {
        if (f->tag()) {
            auto& m = f->tag()->attributeListMap();
            if (m.contains("WM/Picture") && !m["WM/Picture"].isEmpty()) {
                auto data = m["WM/Picture"][0].toPicture().picture();
                artwork.assign(data.begin(), data.end());
                return artwork;
            }
        }
    }
    if (auto* f = dynamic_cast<TagLib::MP4::File*>(fileRef.file())) {
        if (f->tag() && f->tag()->contains("covr")) {
            auto covers = f->tag()->item("covr").toCoverArtList();
            if (!covers.isEmpty()) {
                auto data = covers[0].data();
                artwork.assign(data.begin(), data.end());
                return artwork;
            }
        }
    }
    if (auto* f = dynamic_cast<TagLib::FLAC::File*>(fileRef.file())) {
        auto pictures = f->pictureList();
        if (!pictures.isEmpty()) {
            auto data = pictures[0]->data();
            artwork.assign(data.begin(), data.end());
            return artwork;
        }
    }
    return artwork;
}

// ── Metadata Extraction ──────────────────────────────────────────────────

TrackInfo extract_track_info(const std::string& path, bool extract_art) {
    TrackInfo t;
    t.file_path = path;
    t.filename = fs::path(path).filename().string();
    t.file_size = fs::file_size(path);
    t.format_code = static_cast<uint16_t>(mtp::ObjectFormatFromFilename(path));

    TagLib::FileRef fileRef(path.c_str());
    if (fileRef.isNull()) {
        t.title = fs::path(path).stem().string();
        t.artist = "Unknown Artist";
        t.album = "Unknown Album";
        return t;
    }

    if (fileRef.tag()) {
        auto* tag = fileRef.tag();
        t.artist = tag->artist().toCString(true);
        t.album = tag->album().toCString(true);
        t.title = tag->title().toCString(true);
        t.genre = tag->genre().toCString(true);
        t.year = tag->year();
        t.track_num = tag->track();
    }
    if (fileRef.audioProperties())
        t.duration_ms = static_cast<uint32_t>(fileRef.audioProperties()->lengthInMilliseconds());

    auto props = fileRef.file()->properties();
    if (props.contains("DISCNUMBER"))
        t.disc_num = props["DISCNUMBER"][0].toInt();

    // Rating: check for explicit rating tag
    if (props.contains("RATING"))
        t.rating = props["RATING"][0].toInt();

    // Artist GUID from WMA or ID3v2
    if (auto* asfFile = dynamic_cast<TagLib::ASF::File*>(fileRef.file())) {
        if (asfFile->tag()) {
            auto& m = asfFile->tag()->attributeListMap();
            if (m.contains("ZuneAlbumArtistMediaID"))
                t.artist_guid = m["ZuneAlbumArtistMediaID"][0].toString().toCString(true);
            else if (m.contains("MusicBrainz/Artist ID"))
                t.artist_guid = m["MusicBrainz/Artist ID"][0].toString().toCString(true);
        }
    } else if (auto* mpegFile = dynamic_cast<TagLib::MPEG::File*>(fileRef.file())) {
        if (mpegFile->tag()) {
            auto tagProps = mpegFile->tag()->properties();
            if (tagProps.contains("MUSICBRAINZ_ARTISTID"))
                t.artist_guid = tagProps["MUSICBRAINZ_ARTISTID"].front().to8Bit();
        }
    }

    if (extract_art)
        t.artwork = extract_artwork(path);

    if (t.title.empty()) t.title = fs::path(path).stem().string();
    if (t.artist.empty()) t.artist = "Unknown Artist";
    if (t.album.empty()) t.album = "Unknown Album";

    return t;
}

// ── File Scanning & Grouping ─────────────────────────────────────────────

std::vector<ArtistGroup> scan_and_group(const std::string& input_path, bool extract_art) {
    std::vector<TrackInfo> all_tracks;

    if (fs::is_directory(input_path)) {
        for (auto& entry : fs::recursive_directory_iterator(input_path)) {
            if (!entry.is_regular_file()) continue;
            auto ext = entry.path().extension().string();
            std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
            if (AUDIO_EXTENSIONS.count(ext)) {
                all_tracks.push_back(extract_track_info(entry.path().string(), extract_art));
            }
        }
    } else {
        all_tracks.push_back(extract_track_info(input_path, extract_art));
    }

    // Group: artist → album → tracks (sorted by track number)
    std::map<std::string, std::map<std::string, std::vector<TrackInfo>>> grouped;
    for (auto& t : all_tracks)
        grouped[t.artist][t.album].push_back(std::move(t));

    std::vector<ArtistGroup> artists;
    for (auto& [artist_name, albums_map] : grouped) {
        ArtistGroup ag;
        ag.artist_name = artist_name;

        for (auto& [album_name, tracks] : albums_map) {
            std::sort(tracks.begin(), tracks.end(),
                [](const TrackInfo& a, const TrackInfo& b) { return a.track_num < b.track_num; });

            AlbumGroup alg;
            alg.album_name = album_name;
            alg.year = tracks[0].year;
            alg.tracks = std::move(tracks);

            // Pick best artwork (first non-empty)
            for (auto& t : alg.tracks) {
                if (!t.artwork.empty()) { alg.artwork = t.artwork; break; }
            }

            // Pick artist GUID from first track that has one
            if (ag.artist_guid.empty()) {
                for (auto& t : alg.tracks) {
                    if (!t.artist_guid.empty()) { ag.artist_guid = t.artist_guid; break; }
                }
            }

            ag.albums.push_back(std::move(alg));
        }
        artists.push_back(std::move(ag));
    }
    return artists;
}

// ── MTP Helper Functions ─────────────────────────────────────────────────

void query_prop_desc_batch(
    const std::shared_ptr<mtp::Session>& session,
    uint16_t prop_code, const std::string& prop_name,
    const uint16_t* formats, size_t count)
{
    log_op("GetObjPropDesc x" + std::to_string(count) + ": " + prop_name);
    for (size_t i = 0; i < count; ++i) {
        try {
            session->GetObjectPropertyDesc(
                mtp::ObjectProperty(prop_code), mtp::ObjectFormat(formats[i]));
        } catch (const std::exception& e) {
            if (g_verbose) log_warn("GetObjPropDesc " + prop_name +
                " fmt=" + hex16(formats[i]) + ": " + e.what());
        }
    }
    log_ok("Queried " + prop_name + " across " + std::to_string(count) + " formats");
}

void root_re_enum(const std::shared_ptr<mtp::Session>& session) {
    log_op("Root re-enumeration (StorageIDs + ObjectFileNames)");
    // depth=1 is critical — pcap shows depth=1 for root re-enum (include immediate children)
    try {
        session->GetObjectPropertyList(
            mtp::Session::Device, mtp::ObjectFormat(0),
            mtp::ObjectProperty(PROP_STORAGE_ID), 0, 1);
    } catch (...) {}
    try {
        session->GetObjectPropertyList(
            mtp::Session::Device, mtp::ObjectFormat(0),
            mtp::ObjectProperty(PROP_OBJECT_FILENAME), 0, 1);
    } catch (...) {}
}

// Simple folder readback (subsequent folders — no desc batches)
void folder_readback(
    const std::shared_ptr<mtp::Session>& session,
    mtp::ObjectId folder_id, mtp::StorageId storage_id)
{
    try { session->GetObjectPropertyList(
        folder_id, mtp::ObjectFormat(0),
        mtp::ObjectProperty(PROP_PERSISTENT_UID), 0, 0); } catch (...) {}
    try { session->GetObjectPropertyList(
        folder_id, mtp::ObjectFormat(0),
        mtp::ObjectProperty(0), 4, 0); } catch (...) {}
    try { session->GetObjectHandles(
        storage_id, mtp::ObjectFormat::Any, folder_id); } catch (...) {}
}

// First-folder readback with interleaved desc batches.
// Pcap order: PersistentUID batch → PersistentUID read → StorageID batch → grp=4 read → GetObjectHandles
void first_folder_readback(
    const std::shared_ptr<mtp::Session>& session,
    mtp::ObjectId folder_id, mtp::StorageId storage_id,
    const uint16_t* batch_formats, size_t batch_format_count)
{
    query_prop_desc_batch(session, PROP_PERSISTENT_UID, "PersistentUID",
        batch_formats, batch_format_count);
    try { session->GetObjectPropertyList(
        folder_id, mtp::ObjectFormat(0),
        mtp::ObjectProperty(PROP_PERSISTENT_UID), 0, 0); } catch (...) {}
    query_prop_desc_batch(session, PROP_STORAGE_ID, "StorageID",
        batch_formats, batch_format_count);
    try { session->GetObjectPropertyList(
        folder_id, mtp::ObjectFormat(0),
        mtp::ObjectProperty(0), 4, 0); } catch (...) {}
    try { session->GetObjectHandles(
        storage_id, mtp::ObjectFormat::Any, folder_id); } catch (...) {}
}

// Create a folder via SendObjPropList.
mtp::ObjectId create_folder(
    const std::shared_ptr<mtp::Session>& session,
    mtp::StorageId storageId,
    mtp::ObjectId parent,
    const std::string& name)
{
    log_op("SendObjPropList fmt=Folder — Create \"" + name + "\"");
    mtp::ByteArray propList;
    mtp::OutputStream os(propList);
    os.Write32(1);
    write_prop_string(os, PROP_OBJECT_FILENAME, name);

    auto resp = session->SendObjectPropList(
        storageId, parent, mtp::ObjectFormat::Association, 0, propList);
    log_ok("Created \"" + name + "\" → " + hex(resp.ObjectId.Id));
    return resp.ObjectId;
}

// Discover children of an existing folder using the pcap's 3-operation pattern.
// Returns a map of child name → ObjectId.
// Pattern: GetObjPropList grp=2 + GetObjectHandles + GetObjPropList prop=ObjFileName depth=1
std::map<std::string, mtp::ObjectId> discover_folder_children(
    const std::shared_ptr<mtp::Session>& session,
    mtp::StorageId storageId,
    mtp::ObjectId folder_id)
{
    std::map<std::string, mtp::ObjectId> children;

    // Step 1: Read folder subset properties (grp=2)
    try { session->GetObjectPropertyList(
        folder_id, mtp::ObjectFormat(0), mtp::ObjectProperty(0), 2, 0); } catch (...) {}

    // Step 2: Enumerate children handles
    std::vector<mtp::ObjectId> handles;
    try {
        auto result = session->GetObjectHandles(storageId, mtp::ObjectFormat::Any, folder_id);
        handles = result.ObjectHandles;
    } catch (...) {}

    // Step 3: Read children filenames via GetObjPropList with depth=1
    // This returns ObjFileName for the folder AND its children
    try {
        auto data = session->GetObjectPropertyList(
            folder_id, mtp::ObjectFormat(0),
            mtp::ObjectProperty(PROP_OBJECT_FILENAME), 0, 1);

        // Parse the property list to extract filenames
        // Format: [count u32] [handle u32, prop u16, type u16, value...]...
        if (data.size() >= 4) {
            uint32_t n = *reinterpret_cast<const uint32_t*>(data.data());
            size_t off = 4;
            size_t child_idx = 0;
            for (uint32_t i = 0; i < n && off + 8 <= data.size(); ++i) {
                uint32_t handle = *reinterpret_cast<const uint32_t*>(data.data() + off);
                off += 4;
                uint16_t prop = *reinterpret_cast<const uint16_t*>(data.data() + off);
                off += 2;
                uint16_t dt = *reinterpret_cast<const uint16_t*>(data.data() + off);
                off += 2;

                std::string name;
                if (dt == DT_STRING && off < data.size()) {
                    uint8_t nchars = data[off++];
                    if (nchars > 0 && off + nchars * 2 <= data.size()) {
                        // UTF-16LE to UTF-8 (simplified — works for ASCII/Latin)
                        for (uint8_t c = 0; c < nchars; ++c) {
                            uint16_t ch = *reinterpret_cast<const uint16_t*>(data.data() + off + c * 2);
                            if (ch == 0) break;
                            if (ch < 128) name += static_cast<char>(ch);
                            else name += '?';
                        }
                        off += nchars * 2;
                    }
                }

                // First entry is the folder itself, rest are children
                if (handle != folder_id.Id && !name.empty()) {
                    // Match handle to our enumerated children
                    if (child_idx < handles.size()) {
                        children[name] = mtp::ObjectId(handle);
                    }
                    child_idx++;
                }
            }
        }
    } catch (...) {}

    log_ok("Discovered " + std::to_string(children.size()) + " children");
    return children;
}

// ── Main Upload Logic ────────────────────────────────────────────────────

void print_usage(const char* prog) {
    std::cout << "Upload Test CLI — Multi-track upload (pcap-verified sequence)" << std::endl;
    std::cout << std::endl;
    std::cout << "Usage: " << prog << " <file_or_directory> [options]" << std::endl;
    std::cout << std::endl;
    std::cout << "Options:" << std::endl;
    std::cout << "  --no-artwork    Skip artwork steps" << std::endl;
    std::cout << "  --verbose       Show detailed MTP logging" << std::endl;
    std::cout << "  --dry-run       Show upload plan without connecting" << std::endl;
    std::cout << "  --help          Show this help" << std::endl;
}

int main(int argc, char* argv[]) {
    if (argc < 2) { print_usage(argv[0]); return 1; }

    std::string input_path;
    bool no_artwork = false;
    bool dry_run = false;

    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        if (arg == "--help") { print_usage(argv[0]); return 0; }
        else if (arg == "--verbose") g_verbose = true;
        else if (arg == "--no-artwork") no_artwork = true;
        else if (arg == "--dry-run") dry_run = true;
        else if (arg[0] != '-') input_path = arg;
    }

    if (input_path.empty() || !fs::exists(input_path)) {
        std::cerr << "ERROR: Path not found: " << input_path << std::endl;
        return 1;
    }

    // ── Scan & Group ─────────────────────────────────────────────────────

    std::cout << "Scanning files..." << std::endl;
    auto artists = scan_and_group(input_path, !no_artwork);

    int total_tracks = 0, total_albums = 0;
    for (auto& ag : artists) {
        total_albums += ag.albums.size();
        for (auto& alg : ag.albums)
            total_tracks += alg.tracks.size();
    }

    std::cout << std::endl;
    std::cout << "╔══════════════════════════════════════════════════════════╗" << std::endl;
    std::cout << "║  Upload Plan                                            ║" << std::endl;
    std::cout << "╚══════════════════════════════════════════════════════════╝" << std::endl;
    std::cout << "  Artists: " << artists.size() << "  Albums: " << total_albums
              << "  Tracks: " << total_tracks << std::endl;
    std::cout << std::endl;

    for (auto& ag : artists) {
        std::cout << "  " << ag.artist_name;
        if (!ag.artist_guid.empty()) std::cout << " [GUID: " << ag.artist_guid << "]";
        std::cout << std::endl;
        for (auto& alg : ag.albums) {
            std::cout << "    " << alg.album_name << " (" << alg.year << ")"
                      << " [" << alg.tracks.size() << " track"
                      << (alg.tracks.size() != 1 ? "s" : "") << "]";
            if (!alg.artwork.empty()) std::cout << " [artwork]";
            std::cout << std::endl;
            for (auto& t : alg.tracks) {
                std::cout << "      " << t.track_num << ". " << t.title
                          << " (" << hex16(t.format_code) << ", "
                          << t.file_size / 1024 / 1024 << "MB, "
                          << t.duration_ms / 1000 << "s";
                if (t.rating >= 0) std::cout << ", rating=" << t.rating;
                std::cout << ")" << std::endl;
            }
        }
    }

    if (dry_run) {
        std::cout << std::endl << "Dry run — not connecting to device." << std::endl;
        return 0;
    }

    // ── Connect ──────────────────────────────────────────────────────────

    std::cout << std::endl << "Connecting to Zune device..." << std::endl;
    ZuneDevice device;
    device.SetLogCallback([](const std::string& msg) {
        if (g_verbose) log_ts("  [device] " + msg);
    });

    if (!device.ConnectUSB()) {
        std::cerr << "ERROR: Failed to connect" << std::endl;
        return 1;
    }

    auto session = device.GetMtpSession();
    if (!session) { std::cerr << "ERROR: No MTP session" << std::endl; return 1; }

    auto start_time = std::chrono::steady_clock::now();
    uint32_t storage_raw = device.GetDefaultStorageId();
    mtp::StorageId storageId(storage_raw);

    // Detect device type
    auto family = device.GetDeviceFamily();
    bool is_hd = (family == zune::DeviceFamily::Pavo);
    const uint16_t* batch_formats = is_hd ? HD_FORMATS : CLASSIC_FORMATS;
    size_t batch_format_count = is_hd ? std::size(HD_FORMATS) : std::size(CLASSIC_FORMATS);

    std::cout << "Connected to: " << device.GetName()
              << " (" << device.GetDeviceFamilyName() << ")"
              << (is_hd ? " [HD mode]" : " [Classic mode]") << std::endl;

    // ═══════════════════════════════════════════════════════════════════
    // Phase 1: Pre-Upload Discovery
    // ═══════════════════════════════════════════════════════════════════

    log_phase("Pre-Upload Discovery");

    // GetDevicePropValue (0xD217) x2
    for (int i = 1; i <= 2; ++i) {
        log_op("GetDevicePropValue (0xD217) #" + std::to_string(i));
        try {
            session->GetDeviceProperty(mtp::DeviceProperty(0xD217));
            log_ok("D217 read");
        } catch (const std::exception& e) {
            log_warn("D217 failed: " + std::string(e.what()));
        }
    }

    // SyncDeviceDB
    log_op("SyncDeviceDB (0x9217)");
    try { session->Operation9217(1); log_ok("SyncDeviceDB complete"); }
    catch (const std::exception& e) { log_warn("SyncDeviceDB failed: " + std::string(e.what())); }

    // SetSessionGUID — already done by ConnectUSB()
    log_op("SetSessionGUID (0x9214) — done during ConnectUSB");

    // ═══════════════════════════════════════════════════════════════════
    // Phase 2: Root Discovery
    // ═══════════════════════════════════════════════════════════════════

    log_phase("Root Discovery");

    log_op("GetObjectHandles root");
    auto root_handles = session->GetObjectHandles(storageId, mtp::ObjectFormat::Any, mtp::Session::Root);
    log_ok("Root objects: " + std::to_string(root_handles.ObjectHandles.size()));

    root_re_enum(session);

    log_op("GetObjPropsSupported(Folder) + GetObjPropDesc(ObjectFileName, Folder)");
    try { session->GetObjectPropertiesSupported(mtp::ObjectFormat(FMT_FOLDER)); } catch (...) {}
    try { session->GetObjectPropertyDesc(
        mtp::ObjectProperty(PROP_OBJECT_FILENAME), mtp::ObjectFormat(FMT_FOLDER)); } catch (...) {}

    // State tracking — folder handles cached, discovered via pcap's 3-op pattern
    mtp::ObjectId music_folder, albums_folder, artists_folder;
    std::map<std::string, mtp::ObjectId> music_children_known;  // cached Music subfolder names
    bool first_folder_created = false;
    bool track_desc_done_mp3 = false, track_desc_done_wma = false;
    bool album_desc_done = false;
    bool artwork_desc_done = false;
    bool parent_desc_done = false;

    // Match root folder names to handles from initial discovery.
    // The root_re_enum with depth=1 returns names for all root objects.
    // We parse the ObjectFileName response to build the mapping.
    if (!root_handles.ObjectHandles.empty()) {
        log_op("Matching root folder handles to names");
        try {
            auto name_data = session->GetObjectPropertyList(
                mtp::Session::Device, mtp::ObjectFormat(0),
                mtp::ObjectProperty(PROP_OBJECT_FILENAME), 0, 1);

            // Parse property list: each entry is [handle, prop, type, value]
            if (name_data.size() >= 4) {
                uint32_t n = *reinterpret_cast<const uint32_t*>(name_data.data());
                size_t off = 4;
                for (uint32_t i = 0; i < n && off + 8 <= name_data.size(); ++i) {
                    uint32_t handle = *reinterpret_cast<const uint32_t*>(name_data.data() + off);
                    off += 4 + 2 + 2;  // skip handle, prop, type
                    std::string name;
                    if (off < name_data.size()) {
                        uint8_t nchars = name_data[off++];
                        if (nchars > 0 && off + nchars * 2 <= name_data.size()) {
                            for (uint8_t c = 0; c < nchars; ++c) {
                                uint16_t ch = *reinterpret_cast<const uint16_t*>(name_data.data() + off + c * 2);
                                if (ch == 0) break;
                                if (ch < 128) name += static_cast<char>(ch);
                            }
                            off += nchars * 2;
                        }
                    }
                    if (name == "Music") music_folder = mtp::ObjectId(handle);
                    else if (name == "Albums") albums_folder = mtp::ObjectId(handle);
                    else if (name == "Artists") artists_folder = mtp::ObjectId(handle);
                }
            }
        } catch (...) {}

        if (music_folder != mtp::ObjectId()) log_ok("Music folder: " + hex(music_folder.Id));
        if (albums_folder != mtp::ObjectId()) log_ok("Albums folder: " + hex(albums_folder.Id));
        if (artists_folder != mtp::ObjectId()) log_ok("Artists folder: " + hex(artists_folder.Id));
    }

    // Track all created objects for summary
    struct UploadResult {
        std::string artist, album, title;
        mtp::ObjectId track_id, album_obj_id;
    };
    std::vector<UploadResult> results;

    // ═══════════════════════════════════════════════════════════════════
    // Upload Loop: Per-Artist → Per-Album → Per-Track
    // ═══════════════════════════════════════════════════════════════════

    for (auto& ag : artists) {
        log_phase("Artist: " + ag.artist_name);

        // ── [HD only] Artist Metadata ────────────────────────────────
        mtp::ObjectId artist_meta_id;
        if (is_hd) {
            // Create Artists root folder if needed
            if (artists_folder == mtp::ObjectId()) {
                artists_folder = create_folder(session, storageId,
                    mtp::Session::Root, "Artists");
                if (!first_folder_created) {
                    first_folder_readback(session, artists_folder, storageId,
                        batch_formats, batch_format_count);
                    first_folder_created = true;
                } else {
                    folder_readback(session, artists_folder, storageId);
                }
            }

            // GetObjPropDesc for artist format (first artist only)
            static bool artist_desc_done = false;
            if (!artist_desc_done) {
                log_op("GetObjPropDesc x4 for Artist (0xB218)");
                const uint16_t artist_props[] = { PROP_ZUNE_COLLECTION_ID, PROP_OBJECT_FILENAME,
                                                   PROP_DA97, PROP_NAME };
                for (auto p : artist_props) {
                    try { session->GetObjectPropertyDesc(
                        mtp::ObjectProperty(p), mtp::ObjectFormat(FMT_ARTIST)); } catch (...) {}
                }
                artist_desc_done = true;
            }

            // Create artist metadata object
            auto guid_bytes = parse_guid(ag.artist_guid);
            bool has_guid = (guid_bytes.size() == 16);

            uint32_t prop_count = has_guid ? 4 : 3;
            log_op("SendObjPropList fmt=0xB218 \"" + ag.artist_name + ".art\"" +
                   (has_guid ? " (with GUID)" : " (no GUID)"));

            mtp::ByteArray propList;
            mtp::OutputStream os(propList);
            os.Write32(prop_count);
            write_prop_u8(os, PROP_ZUNE_COLLECTION_ID, 0);
            write_prop_string(os, PROP_OBJECT_FILENAME, ag.artist_name + ".art");
            if (has_guid) write_prop_u128(os, PROP_DA97, guid_bytes);
            write_prop_string(os, PROP_NAME, ag.artist_name);

            auto resp = session->SendObjectPropList(
                storageId, artists_folder, mtp::ObjectFormat(FMT_ARTIST), 0, propList);
            artist_meta_id = resp.ObjectId;

            // Empty SendObject
            mtp::ByteArray empty;
            session->SendObject(std::make_shared<mtp::ByteArrayObjectInputStream>(empty));
            log_ok("Artist metadata created: " + hex(artist_meta_id.Id));

            // ObjectFormat batch (first artist only)
            static bool obj_fmt_desc_done = false;
            if (!obj_fmt_desc_done) {
                query_prop_desc_batch(session, PROP_OBJECT_FORMAT, "ObjectFormat",
                    batch_formats, batch_format_count);
                obj_fmt_desc_done = true;
            }

            // Verification read
            log_op("GetObjPropList ALL artist props (verification)");
            try {
                auto props = session->GetObjectPropertyList(
                    artist_meta_id, mtp::ObjectFormat(0),
                    mtp::ObjectProperty(0xFFFFFFFF), 0, 0);
                log_ok("Artist props: " + std::to_string(props.size()) + " bytes");
            } catch (...) {}
        }

        // Artist music folder — cached across albums for same artist
        mtp::ObjectId artist_music_folder;

        // ── Per-Album ────────────────────────────────────────────────
        for (auto& alg : ag.albums) {
            log_phase("Album: " + ag.artist_name + " — " + alg.album_name +
                      " (" + std::to_string(alg.tracks.size()) + " tracks)");

            mtp::ObjectId album_obj_id;
            mtp::ObjectId album_folder_id;
            std::vector<mtp::ObjectId> album_track_ids;

            for (size_t ti = 0; ti < alg.tracks.size(); ++ti) {
                auto& track = alg.tracks[ti];
                bool is_first_in_album = (ti == 0);

                log_phase("Track " + std::to_string(ti + 1) + "/" +
                          std::to_string(alg.tracks.size()) + ": " + track.title);

                // ── RegisterTrackCtx ─────────────────────────────────
                log_op("RegisterTrackCtx (0x922A) — \"" + track.title + "\"");
                try { session->Operation922a(track.title); log_ok("Registered"); }
                catch (const std::exception& e) { log_warn("RegisterTrackCtx: " + std::string(e.what())); }

                // ── Root re-enum ─────────────────────────────────────
                root_re_enum(session);

                // ── Folder creation (first track in album only) ──────
                if (is_first_in_album) {
                    // Music root folder — discover existing or create
                    if (music_folder == mtp::ObjectId()) {
                        // Check if Music exists in root (from initial root_re_enum)
                        // If root was empty, create it
                        if (root_handles.ObjectHandles.empty()) {
                            music_folder = create_folder(session, storageId,
                                mtp::Session::Root, "Music");
                            if (!first_folder_created) {
                                first_folder_readback(session, music_folder, storageId,
                                    batch_formats, batch_format_count);
                                first_folder_created = true;
                            } else {
                                folder_readback(session, music_folder, storageId);
                            }
                        }
                        // If root has objects, Music should already be set from discovery
                    }

                    // Artist folder under Music (first album for this artist only)
                    if (artist_music_folder == mtp::ObjectId()) {
                        // Discover Music children if this is first time seeing Music
                        if (music_children_known.empty() && music_folder != mtp::ObjectId()) {
                            log_op("Discovering Music folder children");
                            music_children_known = discover_folder_children(
                                session, storageId, music_folder);
                        }

                        auto it = music_children_known.find(ag.artist_name);
                        if (it != music_children_known.end()) {
                            artist_music_folder = it->second;
                            log_ok("Artist folder exists: " + hex(artist_music_folder.Id));
                        } else {
                            artist_music_folder = create_folder(session, storageId,
                                music_folder, ag.artist_name);
                            if (!first_folder_created) {
                                first_folder_readback(session, artist_music_folder, storageId,
                                    batch_formats, batch_format_count);
                                first_folder_created = true;
                            } else {
                                folder_readback(session, artist_music_folder, storageId);
                            }
                        }
                    }

                    // Album folder under artist (every new album — always create)
                    album_folder_id = create_folder(session, storageId,
                        artist_music_folder, alg.album_name);
                    folder_readback(session, album_folder_id, storageId);
                }

                // ── Track property descriptors (first per format) ────
                bool is_mp3 = (track.format_code == FMT_MP3);
                bool& desc_done = is_mp3 ? track_desc_done_mp3 : track_desc_done_wma;
                if (!desc_done) {
                    // Exact order from pcap
                    const uint16_t track_desc_props_classic[] = {
                        PROP_META_GENRE, PROP_OBJECT_FILENAME, PROP_ALBUM_NAME,
                        PROP_NAME, PROP_ALBUM_ARTIST, PROP_DISC_NUMBER,
                        PROP_ZUNE_COLLECTION_ID, PROP_ARTIST, PROP_DATE_AUTHORED,
                        PROP_ZUNE_DAB2, PROP_DURATION, PROP_RATING, PROP_TRACK, PROP_GENRE,
                    };
                    const uint16_t track_desc_props_hd[] = {
                        PROP_META_GENRE, PROP_OBJECT_FILENAME, PROP_ALBUM_NAME,
                        PROP_NAME, PROP_ALBUM_ARTIST, PROP_DISC_NUMBER,
                        PROP_ZUNE_COLLECTION_ID, PROP_ARTIST, PROP_DATE_AUTHORED,
                        PROP_ZUNE_DAB2, PROP_DAB8, PROP_DAB9,
                        PROP_DURATION, PROP_RATING, PROP_TRACK, PROP_GENRE,
                    };
                    auto* descs = is_hd ? track_desc_props_hd : track_desc_props_classic;
                    size_t count = is_hd ? std::size(track_desc_props_hd) : std::size(track_desc_props_classic);

                    log_op("GetObjPropDesc x" + std::to_string(count) + " for " +
                           (is_mp3 ? "MP3" : "WMA"));
                    for (size_t d = 0; d < count; ++d) {
                        try { session->GetObjectPropertyDesc(
                            mtp::ObjectProperty(descs[d]),
                            mtp::ObjectFormat(track.format_code)); } catch (...) {}
                    }
                    desc_done = true;
                }

                // ── Create track ─────────────────────────────────────
                bool has_rating = (track.rating >= 0);
                // Base: MetaGenre, ObjectFileName, AlbumName, Name, AlbumArtist,
                //       DiscNumber, ZuneCollectionId, Artist, DateAuthored, ZuneProp_DAB2,
                //       Duration, Track, Genre = 13
                // HD adds: DAB8, DAB9 = +2
                // Rating: +1 when present
                uint32_t prop_count;
                if (is_hd)
                    prop_count = 13 + 2 + (has_rating ? 1 : 0);  // 15 or 16
                else
                    prop_count = 13 + (has_rating ? 1 : 0);      // 13 or 14

                log_op("SendObjPropList fmt=" + hex16(track.format_code) +
                       " \"" + track.filename + "\" (" + std::to_string(prop_count) + " props, " +
                       std::to_string(track.file_size / 1024) + "KB)");

                mtp::ObjectId track_id;
                {
                    mtp::ByteArray propList;
                    mtp::OutputStream os(propList);
                    os.Write32(prop_count);

                    // Properties in pcap order
                    write_prop_u16(os, PROP_META_GENRE, 1);
                    write_prop_string(os, PROP_OBJECT_FILENAME, track.filename);
                    write_prop_string(os, PROP_ALBUM_NAME, alg.album_name);
                    write_prop_string(os, PROP_NAME, track.title);
                    write_prop_string(os, PROP_ALBUM_ARTIST, ag.artist_name);
                    write_prop_u16(os, PROP_DISC_NUMBER, 0);
                    write_prop_u8(os, PROP_ZUNE_COLLECTION_ID, 0);
                    write_prop_string(os, PROP_ARTIST, ag.artist_name);
                    write_prop_string(os, PROP_DATE_AUTHORED, format_track_date(track.year));
                    write_prop_u8(os, PROP_ZUNE_DAB2, 0);
                    if (is_hd) {
                        write_prop_u32(os, PROP_DAB8, 1);  // Uint32, not Uint8 (verified from pcap)
                        write_prop_u32(os, PROP_DAB9, artist_meta_id.Id);
                    }
                    write_prop_u32(os, PROP_DURATION, track.duration_ms);
                    if (has_rating)
                        write_prop_u16(os, PROP_RATING, static_cast<uint16_t>(track.rating));
                    write_prop_u16(os, PROP_TRACK, static_cast<uint16_t>(track.track_num));
                    write_prop_string(os, PROP_GENRE, track.genre.empty() ? "Unknown" : track.genre);

                    auto resp = session->SendObjectPropList(
                        storageId, album_folder_id,
                        static_cast<mtp::ObjectFormat>(track.format_code),
                        track.file_size, propList);
                    track_id = resp.ObjectId;
                }
                log_ok("Track created: " + hex(track_id.Id));
                album_track_ids.push_back(track_id);

                // ── SendObject (audio data) ──────────────────────────
                log_op("SendObject — " + std::to_string(track.file_size) + " bytes");
                {
                    auto file_stream = std::make_shared<cli::ObjectInputStream>(track.file_path);
                    file_stream->SetTotal(file_stream->GetSize());
                    session->SendObject(file_stream);
                }
                log_ok("Audio data uploaded");

                // ── Post-upload verification ──────────────────────────
                // ObjectFormat batch (Classic does it after first upload, HD did it earlier)
                if (!is_hd) {
                    static bool classic_obj_fmt_done = false;
                    if (!classic_obj_fmt_done) {
                        query_prop_desc_batch(session, PROP_OBJECT_FORMAT, "ObjectFormat",
                            batch_formats, batch_format_count);
                        classic_obj_fmt_done = true;
                    }
                }

                log_op("GetObjPropList ALL track props (verification)");
                try {
                    auto props = session->GetObjectPropertyList(
                        track_id, mtp::ObjectFormat(0),
                        mtp::ObjectProperty(0xFFFFFFFF), 0, 0);
                    log_ok("Track verification: " + std::to_string(props.size()) + " bytes");
                } catch (...) {}

                // ── Album creation (first track only) ────────────────
                if (is_first_in_album) {
                    root_re_enum(session);

                    // Albums container
                    if (albums_folder == mtp::ObjectId()) {
                        albums_folder = create_folder(session, storageId,
                            mtp::Session::Root, "Albums");
                        folder_readback(session, albums_folder, storageId);
                    }

                    // Album property descriptors (first album only)
                    if (!album_desc_done) {
                        if (is_hd) {
                            log_op("GetObjPropDesc x6 for AbstractAudioAlbum");
                            const uint16_t props[] = { PROP_ARTIST, PROP_DATE_AUTHORED,
                                PROP_ZUNE_COLLECTION_ID, PROP_OBJECT_FILENAME, PROP_DAB9, PROP_NAME };
                            for (auto p : props) {
                                try { session->GetObjectPropertyDesc(
                                    mtp::ObjectProperty(p), mtp::ObjectFormat(FMT_ABSTRACT_ALBUM)); } catch (...) {}
                            }
                        } else {
                            log_op("GetObjPropDesc x4 for AbstractAudioAlbum");
                            const uint16_t props[] = { PROP_ARTIST, PROP_ZUNE_COLLECTION_ID,
                                PROP_OBJECT_FILENAME, PROP_NAME };
                            for (auto p : props) {
                                try { session->GetObjectPropertyDesc(
                                    mtp::ObjectProperty(p), mtp::ObjectFormat(FMT_ABSTRACT_ALBUM)); } catch (...) {}
                            }
                        }
                        album_desc_done = true;
                    }

                    // Create album metadata
                    std::string alb_filename = ag.artist_name + "--" + alg.album_name + ".alb";
                    log_op("SendObjPropList AbstractAudioAlbum \"" + alb_filename + "\"");
                    {
                        mtp::ByteArray propList;
                        mtp::OutputStream os(propList);

                        if (is_hd) {
                            os.Write32(6);
                            write_prop_string(os, PROP_ARTIST, ag.artist_name);
                            write_prop_string(os, PROP_DATE_AUTHORED, format_album_date(alg.year));
                            write_prop_u8(os, PROP_ZUNE_COLLECTION_ID, 0);
                            write_prop_string(os, PROP_OBJECT_FILENAME, alb_filename);
                            write_prop_u32(os, PROP_DAB9, artist_meta_id.Id);
                            write_prop_string(os, PROP_NAME, alg.album_name);
                        } else {
                            os.Write32(4);
                            write_prop_string(os, PROP_ARTIST, ag.artist_name);
                            write_prop_u8(os, PROP_ZUNE_COLLECTION_ID, 0);
                            write_prop_string(os, PROP_OBJECT_FILENAME, alb_filename);
                            write_prop_string(os, PROP_NAME, alg.album_name);
                        }

                        auto resp = session->SendObjectPropList(
                            storageId, albums_folder,
                            static_cast<mtp::ObjectFormat>(FMT_ABSTRACT_ALBUM), 0, propList);
                        album_obj_id = resp.ObjectId;
                    }

                    // Empty SendObject (required for metadata objects)
                    mtp::ByteArray empty;
                    session->SendObject(std::make_shared<mtp::ByteArrayObjectInputStream>(empty));
                    log_ok("Album created: " + hex(album_obj_id.Id));

                    // Album verification
                    log_op("GetObjPropList ALL album props");
                    try {
                        session->GetObjectPropertyList(
                            album_obj_id, mtp::ObjectFormat(0),
                            mtp::ObjectProperty(0xFFFFFFFF), 0, 0);
                    } catch (...) {}

                    // Artwork
                    if (!no_artwork && !alg.artwork.empty()) {
                        if (!artwork_desc_done) {
                            log_op("GetObjPropDesc RepSampleData + RepSampleFormat");
                            try { session->GetObjectPropertyDesc(
                                mtp::ObjectProperty(PROP_REP_SAMPLE_DATA),
                                mtp::ObjectFormat(FMT_ABSTRACT_ALBUM)); } catch (...) {}
                        }

                        log_op("GetObjPropValue RepSampleData (current)");
                        try { session->GetObjectProperty(
                            album_obj_id, mtp::ObjectProperty(PROP_REP_SAMPLE_DATA)); } catch (...) {}

                        if (!artwork_desc_done) {
                            try { session->GetObjectPropertyDesc(
                                mtp::ObjectProperty(PROP_REP_SAMPLE_FORMAT),
                                mtp::ObjectFormat(FMT_ABSTRACT_ALBUM)); } catch (...) {}
                            artwork_desc_done = true;
                        }

                        log_op("SetObjPropValue RepSampleData (" +
                               std::to_string(alg.artwork.size()) + " bytes)");
                        try {
                            mtp::ByteArray art_data(alg.artwork.begin(), alg.artwork.end());
                            session->SetObjectPropertyAsArray(
                                album_obj_id, mtp::ObjectProperty(PROP_REP_SAMPLE_DATA), art_data);
                            log_ok("Artwork set");
                        } catch (const std::exception& e) {
                            log_warn("Artwork failed: " + std::string(e.what()));
                        }

                        log_op("SetObjPropValue RepSampleFormat = JPEG");
                        try {
                            mtp::ByteArray fmt_val;
                            mtp::OutputStream fmt_os(fmt_val);
                            fmt_os.Write16(FMT_JPEG);
                            session->SetObjectProperty(
                                album_obj_id, mtp::ObjectProperty(PROP_REP_SAMPLE_FORMAT), fmt_val);
                        } catch (...) {}
                    } else if (!artwork_desc_done) {
                        // Still query the property value even without artwork
                        log_op("GetObjPropValue RepSampleData (current, no artwork to set)");
                        try { session->GetObjectProperty(
                            album_obj_id, mtp::ObjectProperty(PROP_REP_SAMPLE_DATA)); } catch (...) {}
                    }

                    // SetObjReferences (first track)
                    log_op("SetObjReferences " + hex(album_obj_id.Id) + " → [" + hex(track_id.Id) + "]");
                    {
                        mtp::msg::ObjectHandles refs;
                        for (auto& tid : album_track_ids)
                            refs.ObjectHandles.push_back(tid);
                        session->SetObjectReferences(album_obj_id, refs);
                    }
                    log_ok("Track linked to album");

                    // Album verification reads
                    log_op("Album verification (subset + ALL)");
                    try { session->GetObjectPropertyList(
                        album_obj_id, mtp::ObjectFormat(0),
                        mtp::ObjectProperty(0), 2, 0); } catch (...) {}
                    if (!parent_desc_done) {
                        try { session->GetObjectPropertyDesc(
                            mtp::ObjectProperty(PROP_PARENT_OBJECT),
                            mtp::ObjectFormat(FMT_ABSTRACT_ALBUM)); } catch (...) {}
                        parent_desc_done = true;
                    }
                    try { session->GetObjectPropertyList(
                        album_obj_id, mtp::ObjectFormat(0),
                        mtp::ObjectProperty(0xFFFFFFFF), 0, 0); } catch (...) {}

                } else {
                    // ── Subsequent track in same album (shortened) ───
                    // Cumulative SetObjReferences
                    std::string refs_str;
                    for (auto& tid : album_track_ids) {
                        if (!refs_str.empty()) refs_str += ", ";
                        refs_str += hex(tid.Id);
                    }
                    log_op("SetObjReferences " + hex(album_obj_id.Id) + " → [" + refs_str + "]");
                    {
                        mtp::msg::ObjectHandles refs;
                        for (auto& tid : album_track_ids)
                            refs.ObjectHandles.push_back(tid);
                        session->SetObjectReferences(album_obj_id, refs);
                    }
                    log_ok("Cumulative refs updated (" + std::to_string(album_track_ids.size()) + " tracks)");

                    // Album verification reads
                    try { session->GetObjectPropertyList(
                        album_obj_id, mtp::ObjectFormat(0),
                        mtp::ObjectProperty(0), 2, 0); } catch (...) {}
                    try { session->GetObjectPropertyList(
                        album_obj_id, mtp::ObjectFormat(0),
                        mtp::ObjectProperty(0xFFFFFFFF), 0, 0); } catch (...) {}
                }

                results.push_back({ag.artist_name, alg.album_name, track.title,
                                   track_id, album_obj_id});
            }
        }
    }

    // ═══════════════════════════════════════════════════════════════════
    // Finalize
    // ═══════════════════════════════════════════════════════════════════

    log_phase("Finalize");

    log_op("DisableTrustedFiles (0x9215)");
    try { session->Operation9215(); log_ok("Upload finalized"); }
    catch (const std::exception& e) { log_warn("DisableTrustedFiles failed: " + std::string(e.what())); }

    // Open session (leave device in idle/ready state)
    log_op("Op922b(3,1,0) — open session (idle state)");
    try {
        session->Operation922b(3, 1, 0);
        log_ok("Session opened");
    } catch (const std::exception& e) {
        log_warn("Session open failed: " + std::string(e.what()));
    }

    // ═══════════════════════════════════════════════════════════════════
    // Results
    // ═══════════════════════════════════════════════════════════════════

    auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start_time).count();

    std::cout << std::endl;
    std::cout << "╔══════════════════════════════════════════════════════════╗" << std::endl;
    std::cout << "║  Upload Complete                                        ║" << std::endl;
    std::cout << "╚══════════════════════════════════════════════════════════╝" << std::endl;
    std::cout << std::endl;
    std::cout << "  Device:   " << device.GetName() << " (" << device.GetDeviceFamilyName() << ")" << std::endl;
    std::cout << "  Tracks:   " << results.size() << std::endl;
    std::cout << "  Elapsed:  " << elapsed_ms << " ms" << std::endl;
    std::cout << std::endl;

    for (auto& r : results) {
        std::cout << "  " << r.title << " by " << r.artist
                  << " [" << r.album << "]"
                  << "  track=" << hex(r.track_id.Id)
                  << "  album=" << hex(r.album_obj_id.Id) << std::endl;
    }

    std::cout << std::endl;
    std::cout << "Device remains connected. Press Enter to disconnect..." << std::endl;
    std::cin.get();

    // Close session before disconnect
    try {
        session->Operation922b(3, 2, 0);
        std::cout << "Session closed." << std::endl;
    } catch (...) {}

    device.Disconnect();
    std::cout << "Disconnected." << std::endl;

    return 0;
}
