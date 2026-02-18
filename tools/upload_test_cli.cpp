/**
 * Upload Test CLI - Replicates exact Zune Desktop upload sequence
 *
 * Every MTP operation matches the pcap capture (uploadlossless2.pcapng)
 * operation-by-operation, in exact order. Pcap step numbers are annotated
 * as [P##] in the log output.
 *
 * Pcap 2 sequence (steps 5-73, skipping MTPZ auth at step 8):
 *   Phase 1: Pre-upload discovery (steps 5-12)
 *   Phase 2: Folder structure creation (steps 13-28)
 *   Phase 3: Track creation + file upload (steps 29-31)
 *   Phase 4: Post-upload verification (steps 32-35)
 *   Phase 5: Albums container (steps 36-39)
 *   Phase 6: Album metadata object creation (steps 40-42)
 *   Phase 7: Album verification (step 43)
 *   Phase 8: Album artwork (steps 44-48)
 *   Phase 9: Link & verify (steps 49-52)
 *   Phase 10: Post-upload session management (close, disable, reopen)
 *
 * Usage:
 *   upload_test_cli <audio_file> [options]
 */

#include <iostream>
#include <fstream>
#include <string>
#include <vector>
#include <filesystem>
#include <cstring>
#include <iomanip>
#include <chrono>
#include <thread>
#include <sstream>

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
#include <mtp/ptp/OutputStream.h>
#include <mtp/ptp/ByteArrayObjectStream.h>
#include <cli/PosixStreams.h>

namespace fs = std::filesystem;

static bool g_verbose = false;

// ── MTP Property Codes (from pcap analysis) ──────────────────────────────

// ObjectFormat codes
static constexpr uint16_t FMT_UNDEFINED          = 0x3000;
static constexpr uint16_t FMT_FOLDER             = 0x3001;
static constexpr uint16_t FMT_MP3                = 0x3009;
static constexpr uint16_t FMT_ASF                = 0x300C;
static constexpr uint16_t FMT_JPEG               = 0x3801;
static constexpr uint16_t FMT_WMA                = 0xB901;
static constexpr uint16_t FMT_AAC                = 0xB903;
static constexpr uint16_t FMT_ABSTRACT_ALBUM     = 0xBA03;
static constexpr uint16_t FMT_ABSTRACT_PLAYLIST  = 0xBA05;

// ObjectProperty codes
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
static constexpr uint16_t PROP_TRACK              = 0xDC8B;
static constexpr uint16_t PROP_GENRE              = 0xDC8C;
static constexpr uint16_t PROP_META_GENRE         = 0xDC95;
static constexpr uint16_t PROP_ALBUM_NAME         = 0xDC9A;
static constexpr uint16_t PROP_ALBUM_ARTIST       = 0xDC9B;
static constexpr uint16_t PROP_DISC_NUMBER        = 0xDC9D;
static constexpr uint16_t PROP_ZUNE_COLLECTION_ID = 0xDAB0;
static constexpr uint16_t PROP_ZUNE_DAB2          = 0xDAB2;

// DataType codes
static constexpr uint16_t DT_UINT8   = 0x0002;
static constexpr uint16_t DT_UINT16  = 0x0004;
static constexpr uint16_t DT_UINT32  = 0x0006;
static constexpr uint16_t DT_STRING  = 0xFFFF;

// All 17 formats Zune Desktop queries for PersistentUID/StorageID/ObjectFormat
// descriptors (from pcap analysis, exact order from zune-upload-sequence.md)
static const uint16_t ALL_17_FORMATS[] = {
    0x3009, // MP3
    0xB901, // WMA
    0x300C, // ASF
    0xB215, // (vendor)
    0xB903, // AAC
    0xB904, // (vendor)
    0xB301, // (vendor)
    0xB981, // (vendor)
    0x3801, // JPEG
    0x3001, // Folder
    0xBA03, // AbstractAudioAlbum
    0xBA05, // AbstractAVPlaylist
    0xB211, // (vendor)
    0xB213, // (vendor)
    0x3000, // Undefined
    0xB802, // (vendor)
    0xBA0B, // (vendor)
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

void log_phase(int phase, const std::string& name) {
    std::cout << std::endl;
    std::cout << "════════════════════════════════════════════════════════════" << std::endl;
    std::cout << "  Phase " << phase << ": " << name << std::endl;
    std::cout << "════════════════════════════════════════════════════════════" << std::endl;
}

void log_pcap(int pcap_step, const std::string& desc) {
    log_ts("  [P" + std::to_string(pcap_step) + "] " + desc);
}

void log_ok(const std::string& desc) {
    log_ts("    OK " + desc);
}

void log_warn(const std::string& desc) {
    log_ts("    WARN " + desc);
}

void log_fail(const std::string& desc) {
    log_ts("    FAIL " + desc);
}

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

// ── Artwork Extraction ───────────────────────────────────────────────────

std::vector<uint8_t> extract_artwork(const std::string& file_path) {
    std::vector<uint8_t> artwork;
    TagLib::FileRef fileRef(file_path.c_str());
    if (fileRef.isNull()) return artwork;

    if (auto* mpegFile = dynamic_cast<TagLib::MPEG::File*>(fileRef.file())) {
        if (mpegFile->ID3v2Tag()) {
            auto frames = mpegFile->ID3v2Tag()->frameList("APIC");
            if (!frames.isEmpty()) {
                if (auto* pic = dynamic_cast<TagLib::ID3v2::AttachedPictureFrame*>(frames.front())) {
                    TagLib::ByteVector data = pic->picture();
                    artwork.assign(data.begin(), data.end());
                    return artwork;
                }
            }
        }
    }

    if (auto* asfFile = dynamic_cast<TagLib::ASF::File*>(fileRef.file())) {
        if (asfFile->tag()) {
            auto& attrMap = asfFile->tag()->attributeListMap();
            if (attrMap.contains("WM/Picture")) {
                auto& pictures = attrMap["WM/Picture"];
                if (!pictures.isEmpty()) {
                    TagLib::ByteVector data = pictures[0].toPicture().picture();
                    artwork.assign(data.begin(), data.end());
                    return artwork;
                }
            }
        }
    }

    if (auto* mp4File = dynamic_cast<TagLib::MP4::File*>(fileRef.file())) {
        if (mp4File->tag() && mp4File->tag()->contains("covr")) {
            auto covers = mp4File->tag()->item("covr").toCoverArtList();
            if (!covers.isEmpty()) {
                TagLib::ByteVector data = covers[0].data();
                artwork.assign(data.begin(), data.end());
                return artwork;
            }
        }
    }

    if (auto* flacFile = dynamic_cast<TagLib::FLAC::File*>(fileRef.file())) {
        auto pictures = flacFile->pictureList();
        if (!pictures.isEmpty()) {
            TagLib::ByteVector data = pictures[0]->data();
            artwork.assign(data.begin(), data.end());
            return artwork;
        }
    }

    return artwork;
}

// ── Property List Building Helpers ───────────────────────────────────────

void write_prop_string(mtp::OutputStream& os, uint16_t prop, const std::string& value) {
    os.Write32(0);
    os.Write16(prop);
    os.Write16(DT_STRING);
    os.WriteString(value);
}

void write_prop_u8(mtp::OutputStream& os, uint16_t prop, uint8_t value) {
    os.Write32(0);
    os.Write16(prop);
    os.Write16(DT_UINT8);
    os.Write8(value);
}

void write_prop_u16(mtp::OutputStream& os, uint16_t prop, uint16_t value) {
    os.Write32(0);
    os.Write16(prop);
    os.Write16(DT_UINT16);
    os.Write16(value);
}

void write_prop_u32(mtp::OutputStream& os, uint16_t prop, uint32_t value) {
    os.Write32(0);
    os.Write16(prop);
    os.Write16(DT_UINT32);
    os.Write32(value);
}

// ── Date Formatting ──────────────────────────────────────────────────────

std::string format_date_authored(int year) {
    if (year <= 0) year = 2000;
    std::ostringstream ss;
    ss << std::setfill('0') << std::setw(4) << year << "0101T160000.0";
    return ss.str();
}

// ── GetObjPropDesc Batch Helpers ─────────────────────────────────────────

// Query a property descriptor across all 17 Zune-known formats
void query_prop_desc_all_formats(
    const std::shared_ptr<mtp::Session>& session,
    uint16_t prop_code, const std::string& prop_name, int pcap_step)
{
    log_pcap(pcap_step, "GetObjPropDesc x17: " + prop_name + " across 17 formats");
    for (auto fmt : ALL_17_FORMATS) {
        try {
            session->GetObjectPropertyDesc(
                mtp::ObjectProperty(prop_code),
                mtp::ObjectFormat(fmt));
        } catch (const std::exception& e) {
            if (g_verbose) log_warn("GetObjPropDesc " + prop_name +
                " fmt=" + hex16(fmt) + ": " + std::string(e.what()));
        }
    }
    log_ok("Queried " + prop_name + " across 17 formats");
}

// ── Folder Read-back Pattern ─────────────────────────────────────────────

// After creating a folder, read back PersistentUID, ParentObject+StorageID,
// and check children. Matches the post-creation sequence in pcap.
void folder_readback(
    const std::shared_ptr<mtp::Session>& session,
    mtp::ObjectId folder_id,
    mtp::StorageId storage_id,
    int& pcap_step)
{
    // Read PersistentUID
    log_pcap(pcap_step++, "GetObjPropList " + hex(folder_id.Id) + " -> PersistentUID");
    try {
        auto uid_data = session->GetObjectPropertyList(
            folder_id, mtp::ObjectFormat(0),
            mtp::ObjectProperty(PROP_PERSISTENT_UID), 0, 0);
        log_ok("PersistentUID: " + std::to_string(uid_data.size()) + " bytes");
    } catch (const std::exception& e) {
        log_warn("PersistentUID read failed: " + std::string(e.what()));
    }

    // Read ParentObject + StorageID (group 4)
    log_pcap(pcap_step++, "GetObjPropList " + hex(folder_id.Id) + " group=4 -> ParentObject+StorageID");
    try {
        auto parent_data = session->GetObjectPropertyList(
            folder_id, mtp::ObjectFormat(0),
            mtp::ObjectProperty(0), 4, 0);
        log_ok("ParentObject+StorageID: " + std::to_string(parent_data.size()) + " bytes");
    } catch (const std::exception& e) {
        log_warn("Group-4 read failed: " + std::string(e.what()));
    }

    // Check children
    log_pcap(pcap_step++, "GetObjectHandles parent=" + hex(folder_id.Id));
    try {
        auto children = session->GetObjectHandles(
            storage_id, mtp::ObjectFormat::Any, folder_id);
        log_ok("Children: " + std::to_string(children.ObjectHandles.size()));
    } catch (const std::exception& e) {
        log_warn("GetObjectHandles failed: " + std::string(e.what()));
    }
}

// ── Main ─────────────────────────────────────────────────────────────────

void print_usage(const char* prog) {
    std::cout << "Upload Test CLI - Replicates exact Zune Desktop upload sequence" << std::endl;
    std::cout << std::endl;
    std::cout << "Usage: " << prog << " <audio_file> [options]" << std::endl;
    std::cout << std::endl;
    std::cout << "Options:" << std::endl;
    std::cout << "  --artist NAME     Override artist name" << std::endl;
    std::cout << "  --album NAME      Override album name" << std::endl;
    std::cout << "  --title NAME      Override track title" << std::endl;
    std::cout << "  --no-artwork      Skip artwork steps" << std::endl;
    std::cout << "  --no-session      Skip post-upload session management (phase 10)" << std::endl;
    std::cout << "  --verbose         Show detailed logging" << std::endl;
    std::cout << "  --help            Show this help" << std::endl;
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        print_usage(argv[0]);
        return 1;
    }

    std::string file_path;
    std::string artist_override;
    std::string album_override;
    std::string title_override;
    bool no_artwork = false;
    bool no_session = false;

    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        if (arg == "--help") { print_usage(argv[0]); return 0; }
        else if (arg == "--verbose") g_verbose = true;
        else if (arg == "--no-artwork") no_artwork = true;
        else if (arg == "--no-session") no_session = true;
        else if (arg == "--artist" && i + 1 < argc) artist_override = argv[++i];
        else if (arg == "--album" && i + 1 < argc) album_override = argv[++i];
        else if (arg == "--title" && i + 1 < argc) title_override = argv[++i];
        else if (arg[0] != '-') file_path = arg;
    }

    if (file_path.empty()) {
        std::cerr << "ERROR: No audio file specified" << std::endl;
        print_usage(argv[0]);
        return 1;
    }
    if (!fs::exists(file_path)) {
        std::cerr << "ERROR: File not found: " << file_path << std::endl;
        return 1;
    }

    std::cout << "╔══════════════════════════════════════════════════════════╗" << std::endl;
    std::cout << "║  Upload Test CLI - Zune Desktop Sequence Replication    ║" << std::endl;
    std::cout << "╚══════════════════════════════════════════════════════════╝" << std::endl;
    std::cout << std::endl;

    // ── Extract metadata from file ───────────────────────────────────────

    std::cout << "Reading file metadata..." << std::endl;
    TagLib::FileRef fileRef(file_path.c_str());
    if (fileRef.isNull()) {
        std::cerr << "ERROR: Could not read file metadata" << std::endl;
        return 1;
    }

    std::string artist, album, title, genre;
    int year = 0, track_num = 0, disc_num = 0;
    uint32_t duration_ms = 0;

    if (fileRef.tag()) {
        TagLib::Tag* tag = fileRef.tag();
        artist = tag->artist().toCString(true);
        album = tag->album().toCString(true);
        title = tag->title().toCString(true);
        genre = tag->genre().toCString(true);
        year = tag->year();
        track_num = tag->track();
    }
    if (fileRef.audioProperties()) {
        duration_ms = static_cast<uint32_t>(fileRef.audioProperties()->lengthInMilliseconds());
    }

    TagLib::PropertyMap props = fileRef.file()->properties();
    if (props.contains("DISCNUMBER")) {
        disc_num = props["DISCNUMBER"][0].toInt();
    }

    if (!artist_override.empty()) artist = artist_override;
    if (!album_override.empty()) album = album_override;
    if (!title_override.empty()) title = title_override;
    if (title.empty()) title = fs::path(file_path).stem().string();
    if (artist.empty()) artist = "Unknown Artist";
    if (album.empty()) album = "Unknown Album";

    auto file_size = fs::file_size(file_path);
    auto filename = fs::path(file_path).filename().string();
    auto format = mtp::ObjectFormatFromFilename(file_path);
    uint16_t format_code = static_cast<uint16_t>(format);

    std::vector<uint8_t> artwork;
    if (!no_artwork) {
        artwork = extract_artwork(file_path);
    }

    std::cout << "  File:     " << file_path << std::endl;
    std::cout << "  Size:     " << file_size << " bytes" << std::endl;
    std::cout << "  Format:   " << hex16(format_code) << std::endl;
    std::cout << "  Artist:   " << artist << std::endl;
    std::cout << "  Album:    " << album << std::endl;
    std::cout << "  Title:    " << title << std::endl;
    std::cout << "  Genre:    " << (genre.empty() ? "(none)" : genre) << std::endl;
    std::cout << "  Year:     " << (year > 0 ? std::to_string(year) : "(none)") << std::endl;
    std::cout << "  Track #:  " << track_num << std::endl;
    std::cout << "  Disc #:   " << disc_num << " (pcap sends 0)" << std::endl;
    std::cout << "  Duration: " << duration_ms << " ms" << std::endl;
    std::cout << "  Artwork:  " << (artwork.empty() ? "(none)" : std::to_string(artwork.size()) + " bytes") << std::endl;
    std::cout << std::endl;

    // ── Connect to device ────────────────────────────────────────────────

    std::cout << "Connecting to Zune device..." << std::endl;

    ZuneDevice device;
    device.SetLogCallback([](const std::string& msg) {
        if (g_verbose) log_ts("  [device] " + msg);
    });

    if (!device.ConnectUSB()) {
        std::cerr << "ERROR: Failed to connect to Zune device" << std::endl;
        return 1;
    }

    auto session = device.GetMtpSession();
    if (!session) {
        std::cerr << "ERROR: No MTP session" << std::endl;
        return 1;
    }

    std::cout << "Connected to: " << device.GetName() << std::endl;
    auto start_time = std::chrono::steady_clock::now();
    uint32_t storage_id = device.GetDefaultStorageId();
    mtp::StorageId storageId(storage_id);

    // Track pcap step numbers for annotation
    // Pcap 2 upload starts at step 5 (steps 1-4 are previous session tail)
    // Step 8 (SetSessionGUID/MTPZ auth) is handled by ConnectUSB()

    // ═════════════════════════════════════════════════════════════════════
    // Phase 1: Pre-Upload Discovery  [pcap2 steps 5-12]
    // ═════════════════════════════════════════════════════════════════════

    log_phase(1, "Pre-Upload Discovery");

    // [P5-6] GetDevicePropValue (0xD217) x2 — read device sync status
    // Note: pcap parser mislabels 0x1015 as "ResetDevicePropValue" — it's GetDevicePropValue
    log_pcap(5, "GetDevicePropValue (0xD217) #1");
    try {
        auto prop1 = session->GetDeviceProperty(mtp::DeviceProperty(0xD217));
        log_ok("D217 read #1: " + std::to_string(prop1.size()) + " bytes");
    } catch (const std::exception& e) {
        log_warn("D217 #1 failed: " + std::string(e.what()));
    }

    log_pcap(6, "GetDevicePropValue (0xD217) #2");
    try {
        auto prop2 = session->GetDeviceProperty(mtp::DeviceProperty(0xD217));
        log_ok("D217 read #2: " + std::to_string(prop2.size()) + " bytes");
    } catch (const std::exception& e) {
        log_warn("D217 #2 failed: " + std::string(e.what()));
    }

    // [P7] SyncDeviceDB (0x9217) — pre-upload ZMDB database dump
    log_pcap(7, "SyncDeviceDB (0x9217) — pre-upload database dump");
    try {
        session->Operation9217(1);
        log_ok("SyncDeviceDB complete");
    } catch (const std::exception& e) {
        log_warn("SyncDeviceDB failed: " + std::string(e.what()));
    }

    // [P8] SetSessionGUID (0x9214) — MTPZ auth step, already done in ConnectUSB()
    log_ts("  [P8]  SetSessionGUID (0x9214) — already done during MTPZ auth in ConnectUSB()");

    // [P9] RegisterTrackCtx (0x922A) — register track context (530 bytes)
    log_pcap(9, "RegisterTrackCtx (0x922A) — \"" + title + "\"");
    try {
        session->Operation922a(title);
        log_ok("Track context registered");
    } catch (const std::exception& e) {
        log_warn("RegisterTrackCtx failed: " + std::string(e.what()));
    }

    // [P10] GetObjectHandles — enumerate root storage objects
    log_pcap(10, "GetObjectHandles root storage=" + hex(storage_id));
    auto root_handles = session->GetObjectHandles(storageId, mtp::ObjectFormat::Any, mtp::Session::Root);
    log_ok("Root objects: " + std::to_string(root_handles.ObjectHandles.size()));

    // [P11] GetObjPropList obj=0 property=StorageID — root StorageIDs
    log_pcap(11, "GetObjPropList obj=0 prop=StorageID — root StorageIDs");
    try {
        auto root_sids = session->GetObjectPropertyList(
            mtp::Session::Device, mtp::ObjectFormat(0),
            mtp::ObjectProperty(PROP_STORAGE_ID), 0, 0);
        log_ok("Root StorageIDs: " + std::to_string(root_sids.size()) + " bytes");
    } catch (const std::exception& e) {
        log_warn("Root StorageIDs failed: " + std::string(e.what()));
    }

    // [P12] GetObjPropList obj=0 property=ObjectFileName — root filenames
    log_pcap(12, "GetObjPropList obj=0 prop=ObjectFileName — root filenames");
    try {
        auto root_names = session->GetObjectPropertyList(
            mtp::Session::Device, mtp::ObjectFormat(0),
            mtp::ObjectProperty(PROP_OBJECT_FILENAME), 0, 0);
        log_ok("Root filenames: " + std::to_string(root_names.size()) + " bytes");
    } catch (const std::exception& e) {
        log_warn("Root filenames failed: " + std::string(e.what()));
    }

    // ═════════════════════════════════════════════════════════════════════
    // Phase 2: Folder Structure Discovery & Creation  [pcap2 steps 13-28]
    // ═════════════════════════════════════════════════════════════════════

    log_phase(2, "Folder Structure Discovery & Creation");

    // Build a map of existing root folders from the handles we already got
    mtp::ObjectId music_folder;
    mtp::ObjectId albums_folder;  // Will be used in Phase 5
    for (auto& h : root_handles.ObjectHandles) {
        try {
            auto info = session->GetObjectInfo(h);
            if (info.Filename == "Music" && info.ObjectFormat == mtp::ObjectFormat::Association)
                music_folder = h;
            if (info.Filename == "Albums")
                albums_folder = h;
        } catch (...) {}
    }

    // [P13] GetObjPropsSupported for Folder format
    log_pcap(13, "GetObjPropsSupported fmt=Folder (0x3001)");
    try {
        session->GetObjectPropertiesSupported(mtp::ObjectFormat(FMT_FOLDER));
        log_ok("Folder properties supported queried");
    } catch (const std::exception& e) {
        log_warn("GetObjPropsSupported failed: " + std::string(e.what()));
    }

    // [P14] GetObjPropDesc ObjectFileName for Folder format
    log_pcap(14, "GetObjPropDesc ObjectFileName fmt=Folder");
    try {
        session->GetObjectPropertyDesc(
            mtp::ObjectProperty(PROP_OBJECT_FILENAME),
            mtp::ObjectFormat(FMT_FOLDER));
        log_ok("ObjectFileName descriptor for Folder queried");
    } catch (const std::exception& e) {
        log_warn("GetObjPropDesc failed: " + std::string(e.what()));
    }

    // Track whether we've done the PersistentUID/StorageID GetObjPropDesc batches.
    // These are only needed once per session, after the FIRST folder creation.
    // Pcap shows the exact interleaving:
    //   1. Create folder
    //   2. PersistentUID desc batch x17
    //   3. PersistentUID read
    //   4. StorageID desc batch x17
    //   5. group-4 read (ParentObject + StorageID)
    //   6. GetObjectHandles children
    // Subsequent folders skip steps 2 and 4.
    bool desc_batches_done = false;
    int pcap_step = 15;

    // Helper lambda: first-folder readback (with interleaved desc batches)
    auto first_folder_readback = [&](mtp::ObjectId folder_id) {
        // PersistentUID desc batch x17
        query_prop_desc_all_formats(session, PROP_PERSISTENT_UID, "PersistentUID", pcap_step++);

        // PersistentUID read
        log_pcap(pcap_step++, "GetObjPropList " + hex(folder_id.Id) + " -> PersistentUID");
        try {
            session->GetObjectPropertyList(
                folder_id, mtp::ObjectFormat(0),
                mtp::ObjectProperty(PROP_PERSISTENT_UID), 0, 0);
            log_ok("PersistentUID read");
        } catch (const std::exception& e) {
            log_warn("PersistentUID read failed: " + std::string(e.what()));
        }

        // StorageID desc batch x17
        query_prop_desc_all_formats(session, PROP_STORAGE_ID, "StorageID", pcap_step++);

        // ParentObject + StorageID (group 4)
        log_pcap(pcap_step++, "GetObjPropList " + hex(folder_id.Id) + " group=4 -> ParentObject+StorageID");
        try {
            session->GetObjectPropertyList(
                folder_id, mtp::ObjectFormat(0),
                mtp::ObjectProperty(0), 4, 0);
            log_ok("ParentObject+StorageID read");
        } catch (const std::exception& e) {
            log_warn("Group-4 read failed: " + std::string(e.what()));
        }

        // Check children
        log_pcap(pcap_step++, "GetObjectHandles parent=" + hex(folder_id.Id));
        try {
            session->GetObjectHandles(storageId, mtp::ObjectFormat::Any, folder_id);
            log_ok("Children checked");
        } catch (const std::exception& e) {
            log_warn("GetObjectHandles failed: " + std::string(e.what()));
        }

        desc_batches_done = true;
    };

    // ── Music folder ─────────────────────────────────────────────────────
    if (music_folder == mtp::ObjectId()) {
        // Music doesn't exist — create it (pcap2 fresh device path)
        log_pcap(pcap_step++, "SendObjPropList fmt=Folder parent=root — Create \"Music\"");
        {
            mtp::ByteArray propList;
            mtp::OutputStream os(propList);
            os.Write32(1);
            write_prop_string(os, PROP_OBJECT_FILENAME, "Music");

            auto resp = session->SendObjectPropList(
                storageId, mtp::Session::Root,
                mtp::ObjectFormat::Association, 0, propList);
            music_folder = resp.ObjectId;
        }
        log_ok("Music folder created: " + hex(music_folder.Id));

        // First folder created this session — do full readback with desc batches
        first_folder_readback(music_folder);
    } else {
        // Music exists — read all properties (pcap1 path: step 9)
        log_pcap(pcap_step++, "GetObjPropList " + hex(music_folder.Id) + " — Read existing Music folder (ALL props)");
        try {
            auto music_props = session->GetObjectPropertyList(
                music_folder, mtp::ObjectFormat(0),
                mtp::ObjectProperty(0xFFFFFFFF), 0, 0);
            log_ok("Music folder properties: " + std::to_string(music_props.size()) + " bytes");
        } catch (const std::exception& e) {
            log_warn("Music folder read failed: " + std::string(e.what()));
        }
    }

    // ── Artist folder ────────────────────────────────────────────────────
    // Check if artist folder already exists under Music
    mtp::ObjectId artist_folder;
    {
        auto music_children = session->GetObjectHandles(storageId, mtp::ObjectFormat::Any, music_folder);
        for (auto& h : music_children.ObjectHandles) {
            try {
                auto info = session->GetObjectInfo(h);
                if (info.Filename == artist && info.ObjectFormat == mtp::ObjectFormat::Association) {
                    artist_folder = h;
                    break;
                }
            } catch (...) {}
        }
    }

    if (artist_folder == mtp::ObjectId()) {
        log_pcap(pcap_step++, "SendObjPropList fmt=Folder parent=Music — Create \"" + artist + "\"");
        {
            mtp::ByteArray propList;
            mtp::OutputStream os(propList);
            os.Write32(1);
            write_prop_string(os, PROP_OBJECT_FILENAME, artist);

            auto resp = session->SendObjectPropList(
                storageId, music_folder,
                mtp::ObjectFormat::Association, 0, propList);
            artist_folder = resp.ObjectId;
        }
        log_ok("Artist folder created: " + hex(artist_folder.Id));

        if (!desc_batches_done) {
            // First folder created this session — do full readback with desc batches
            first_folder_readback(artist_folder);
        } else {
            // Subsequent folder — simple readback (no desc batches)
            folder_readback(session, artist_folder, storageId, pcap_step);
        }
    } else {
        log_pcap(pcap_step++, "Artist folder \"" + artist + "\" already exists: " + hex(artist_folder.Id));
        try {
            auto artist_props = session->GetObjectPropertyList(
                artist_folder, mtp::ObjectFormat(0),
                mtp::ObjectProperty(0xFFFFFFFF), 0, 0);
            log_ok("Artist properties: " + std::to_string(artist_props.size()) + " bytes");
        } catch (const std::exception& e) {
            log_warn("Artist read failed: " + std::string(e.what()));
        }
    }

    // ── Album folder ─────────────────────────────────────────────────────
    mtp::ObjectId album_folder;
    {
        auto artist_children = session->GetObjectHandles(storageId, mtp::ObjectFormat::Any, artist_folder);
        for (auto& h : artist_children.ObjectHandles) {
            try {
                auto info = session->GetObjectInfo(h);
                if (info.Filename == album && info.ObjectFormat == mtp::ObjectFormat::Association) {
                    album_folder = h;
                    break;
                }
            } catch (...) {}
        }
    }

    if (album_folder == mtp::ObjectId()) {
        log_pcap(pcap_step++, "SendObjPropList fmt=Folder parent=Artist — Create \"" + album + "\"");
        {
            mtp::ByteArray propList;
            mtp::OutputStream os(propList);
            os.Write32(1);
            write_prop_string(os, PROP_OBJECT_FILENAME, album);

            auto resp = session->SendObjectPropList(
                storageId, artist_folder,
                mtp::ObjectFormat::Association, 0, propList);
            album_folder = resp.ObjectId;
        }
        log_ok("Album folder created: " + hex(album_folder.Id));

        if (!desc_batches_done) {
            first_folder_readback(album_folder);
        } else {
            folder_readback(session, album_folder, storageId, pcap_step);
        }
    } else {
        log_pcap(pcap_step++, "Album folder \"" + album + "\" already exists: " + hex(album_folder.Id));
        try {
            auto album_dir_props = session->GetObjectPropertyList(
                album_folder, mtp::ObjectFormat(0),
                mtp::ObjectProperty(0xFFFFFFFF), 0, 0);
            log_ok("Album folder properties: " + std::to_string(album_dir_props.size()) + " bytes");
        } catch (const std::exception& e) {
            log_warn("Album folder read failed: " + std::string(e.what()));
        }
        // Check children even for existing folders
        auto album_children = session->GetObjectHandles(storageId, mtp::ObjectFormat::Any, album_folder);
        log_ok("Album folder children: " + std::to_string(album_children.ObjectHandles.size()));
    }

    // ═════════════════════════════════════════════════════════════════════
    // Phase 3: Track Creation & File Upload  [pcap2 steps 29-31]
    // ═════════════════════════════════════════════════════════════════════

    log_phase(3, "Track Creation & File Upload");
    pcap_step = 29;

    // [P29] GetObjPropDesc x13: all 13 WMA track properties
    log_pcap(29, "GetObjPropDesc x13: 13 track property descriptors for WMA");
    {
        const uint16_t wma_props[] = {
            PROP_OBJECT_FILENAME,    // DC07
            PROP_ZUNE_COLLECTION_ID, // DAB0
            PROP_META_GENRE,         // DC95
            PROP_ZUNE_DAB2,          // DAB2
            PROP_ALBUM_NAME,         // DC9A
            PROP_ALBUM_ARTIST,       // DC9B
            PROP_DISC_NUMBER,        // DC9D
            PROP_NAME,               // DC44
            PROP_DURATION,           // DC89
            PROP_TRACK,              // DC8B
            PROP_ARTIST,             // DC46
            PROP_GENRE,              // DC8C
            PROP_DATE_AUTHORED,      // DC47
        };
        for (auto prop : wma_props) {
            try {
                session->GetObjectPropertyDesc(
                    mtp::ObjectProperty(prop),
                    mtp::ObjectFormat(format_code));
            } catch (const std::exception& e) {
                if (g_verbose) log_warn("GetObjPropDesc " + hex16(prop) +
                    " fmt=" + hex16(format_code) + ": " + std::string(e.what()));
            }
        }
        log_ok("Queried 13 WMA property descriptors");
    }

    // [P30] SendObjPropList fmt=WMA — create track object with 13 properties
    log_pcap(30, "SendObjPropList fmt=" + hex16(format_code) +
             " parent=" + hex(album_folder.Id) + " size=" + std::to_string(file_size));
    mtp::ObjectId track_id;
    {
        mtp::ByteArray propList;
        mtp::OutputStream os(propList);

        os.Write32(13);  // 13 properties

        // Exact pcap order:
        write_prop_string(os, PROP_OBJECT_FILENAME, filename);          // 1. DC07
        write_prop_u8(os, PROP_ZUNE_COLLECTION_ID, 0);                 // 2. DAB0 = 0
        write_prop_u16(os, PROP_META_GENRE, 1);                        // 3. DC95 = 1
        write_prop_u8(os, PROP_ZUNE_DAB2, 0);                          // 4. DAB2 = 0
        write_prop_string(os, PROP_ALBUM_NAME, album);                 // 5. DC9A
        write_prop_string(os, PROP_ALBUM_ARTIST, artist);              // 6. DC9B
        write_prop_u16(os, PROP_DISC_NUMBER, 0);                       // 7. DC9D = 0 (pcap sends 0, not 1)
        write_prop_string(os, PROP_NAME, title);                       // 8. DC44
        write_prop_u32(os, PROP_DURATION, duration_ms);                // 9. DC89
        write_prop_u16(os, PROP_TRACK, static_cast<uint16_t>(track_num)); // 10. DC8B
        write_prop_string(os, PROP_ARTIST, artist);                    // 11. DC46
        write_prop_string(os, PROP_GENRE, genre.empty() ? "Unknown" : genre); // 12. DC8C
        write_prop_string(os, PROP_DATE_AUTHORED, format_date_authored(year)); // 13. DC47

        auto resp = session->SendObjectPropList(
            storageId, album_folder,
            static_cast<mtp::ObjectFormat>(format_code),
            file_size, propList);
        track_id = resp.ObjectId;
    }
    log_ok("Track object created: " + hex(track_id.Id));

    // [P31] SendObject — upload audio file data
    log_pcap(31, "SendObject — " + std::to_string(file_size) + " bytes");
    {
        auto file_stream = std::make_shared<cli::ObjectInputStream>(file_path);
        file_stream->SetTotal(file_stream->GetSize());
        session->SendObject(file_stream);
    }
    log_ok("Audio data uploaded");

    // ═════════════════════════════════════════════════════════════════════
    // Phase 4: Post-Upload Verification  [pcap2 steps 32-35]
    // ═════════════════════════════════════════════════════════════════════

    log_phase(4, "Post-Upload Verification");

    // [P32] GetObjPropDesc x17: ObjectFormat across 17 formats
    query_prop_desc_all_formats(session, PROP_OBJECT_FORMAT, "ObjectFormat", 32);

    // [P33] GetObjPropList obj=track — ALL 26 properties (verification read-back)
    log_pcap(33, "GetObjPropList " + hex(track_id.Id) + " — ALL track properties (verification)");
    try {
        auto track_props = session->GetObjectPropertyList(
            track_id, mtp::ObjectFormat(0),
            mtp::ObjectProperty(0xFFFFFFFF), 0, 0);
        log_ok("Track properties: " + std::to_string(track_props.size()) + " bytes");
    } catch (const std::exception& e) {
        log_warn("Track verification failed: " + std::string(e.what()));
    }

    // [P34] GetObjPropList obj=0 prop=StorageID — root re-enumeration
    log_pcap(34, "GetObjPropList obj=0 prop=StorageID — root re-enumeration");
    try {
        auto root_sids2 = session->GetObjectPropertyList(
            mtp::Session::Device, mtp::ObjectFormat(0),
            mtp::ObjectProperty(PROP_STORAGE_ID), 0, 0);
        log_ok("Root StorageIDs: " + std::to_string(root_sids2.size()) + " bytes");
    } catch (const std::exception& e) {
        log_warn("Root StorageIDs re-enum failed: " + std::string(e.what()));
    }

    // [P35] GetObjPropList obj=0 prop=ObjectFileName — root re-enumeration
    log_pcap(35, "GetObjPropList obj=0 prop=ObjectFileName — root re-enumeration");
    try {
        auto root_names2 = session->GetObjectPropertyList(
            mtp::Session::Device, mtp::ObjectFormat(0),
            mtp::ObjectProperty(PROP_OBJECT_FILENAME), 0, 0);
        log_ok("Root filenames: " + std::to_string(root_names2.size()) + " bytes");
    } catch (const std::exception& e) {
        log_warn("Root filenames re-enum failed: " + std::string(e.what()));
    }

    // ═════════════════════════════════════════════════════════════════════
    // Phase 5: Albums Container  [pcap2 steps 36-39]
    // ═════════════════════════════════════════════════════════════════════

    log_phase(5, "Albums Container");
    pcap_step = 36;

    // Re-scan root handles to find Albums (it might have been created by device
    // in response to the track upload, or might pre-exist)
    if (albums_folder == mtp::ObjectId()) {
        // Re-enumerate root to check
        auto root_handles2 = session->GetObjectHandles(storageId, mtp::ObjectFormat::Any, mtp::Session::Root);
        for (auto& h : root_handles2.ObjectHandles) {
            try {
                auto info = session->GetObjectInfo(h);
                if (info.Filename == "Albums") {
                    albums_folder = h;
                    break;
                }
            } catch (...) {}
        }
    }

    mtp::ObjectId albums_container;
    if (albums_folder == mtp::ObjectId()) {
        // Albums doesn't exist — create it (pcap2 fresh device path)
        // [P36] SendObjPropList fmt=Folder parent=root — create Albums
        log_pcap(pcap_step++, "SendObjPropList fmt=Folder parent=root — Create \"Albums\"");
        {
            mtp::ByteArray propList;
            mtp::OutputStream os(propList);
            os.Write32(1);
            write_prop_string(os, PROP_OBJECT_FILENAME, "Albums");

            auto resp = session->SendObjectPropList(
                storageId, mtp::Session::Root,
                mtp::ObjectFormat::Association, 0, propList);
            albums_container = resp.ObjectId;
        }
        log_ok("Albums container created: " + hex(albums_container.Id));

        // [P37-39] Read-back: PersistentUID, ParentObject+StorageID, children
        folder_readback(session, albums_container, storageId, pcap_step);
    } else {
        albums_container = albums_folder;
        // Albums exists — read properties (pcap1 path: group=2, children, ALL props)
        log_pcap(pcap_step++, "GetObjPropList " + hex(albums_container.Id) + " group=2 — Albums subset");
        try {
            auto albums_subset = session->GetObjectPropertyList(
                albums_container, mtp::ObjectFormat(0),
                mtp::ObjectProperty(0), 2, 0);
            log_ok("Albums subset: " + std::to_string(albums_subset.size()) + " bytes");
        } catch (const std::exception& e) {
            log_warn("Albums subset read failed: " + std::string(e.what()));
        }

        log_pcap(pcap_step++, "GetObjectHandles parent=Albums — check children");
        try {
            auto albums_children = session->GetObjectHandles(
                storageId, mtp::ObjectFormat::Any, albums_container);
            log_ok("Albums children: " + std::to_string(albums_children.ObjectHandles.size()));
        } catch (const std::exception& e) {
            log_warn("Albums children failed: " + std::string(e.what()));
        }

        log_pcap(pcap_step++, "GetObjPropList " + hex(albums_container.Id) + " — ALL Albums properties");
        try {
            auto albums_all = session->GetObjectPropertyList(
                albums_container, mtp::ObjectFormat(0),
                mtp::ObjectProperty(0xFFFFFFFF), 0, 0);
            log_ok("Albums all properties: " + std::to_string(albums_all.size()) + " bytes");
        } catch (const std::exception& e) {
            log_warn("Albums all properties failed: " + std::string(e.what()));
        }
    }

    // ═════════════════════════════════════════════════════════════════════
    // Phase 6: Album Metadata Object Creation  [pcap2 steps 40-42]
    // ═════════════════════════════════════════════════════════════════════

    log_phase(6, "Album Metadata Object Creation");

    // [P40] GetObjPropDesc x4: 4 property descriptors for AbstractAudioAlbum
    log_pcap(40, "GetObjPropDesc x4: 4 album property descriptors for AbstractAudioAlbum");
    {
        const uint16_t album_props[] = {
            PROP_ARTIST,             // DC46
            PROP_ZUNE_COLLECTION_ID, // DAB0
            PROP_OBJECT_FILENAME,    // DC07
            PROP_NAME,               // DC44
        };
        for (auto prop : album_props) {
            try {
                session->GetObjectPropertyDesc(
                    mtp::ObjectProperty(prop),
                    mtp::ObjectFormat(FMT_ABSTRACT_ALBUM));
            } catch (const std::exception& e) {
                if (g_verbose) log_warn("GetObjPropDesc " + hex16(prop) +
                    " fmt=BA03: " + std::string(e.what()));
            }
        }
        log_ok("Queried 4 AbstractAudioAlbum property descriptors");
    }

    // [P41] SendObjPropList fmt=AbstractAudioAlbum — create album metadata object
    log_pcap(41, "SendObjPropList fmt=AbstractAudioAlbum parent=" + hex(albums_container.Id));
    mtp::ObjectId album_obj_id;
    {
        mtp::ByteArray propList;
        mtp::OutputStream os(propList);

        os.Write32(4);  // 4 properties

        // Exact pcap order:
        write_prop_string(os, PROP_ARTIST, artist);                                         // 1. DC46
        write_prop_u8(os, PROP_ZUNE_COLLECTION_ID, 0);                                     // 2. DAB0 = 0
        write_prop_string(os, PROP_OBJECT_FILENAME, artist + "--" + album + ".alb");        // 3. DC07
        write_prop_string(os, PROP_NAME, album);                                            // 4. DC44

        auto resp = session->SendObjectPropList(
            storageId, albums_container,
            static_cast<mtp::ObjectFormat>(FMT_ABSTRACT_ALBUM),
            0, propList);
        album_obj_id = resp.ObjectId;
    }
    log_ok("Album metadata object created: " + hex(album_obj_id.Id));

    // [P42] SendObject — empty body (required by MTP for metadata objects)
    log_pcap(42, "SendObject — EMPTY (0 bytes, required for album metadata)");
    {
        mtp::ByteArray empty;
        session->SendObject(std::make_shared<mtp::ByteArrayObjectInputStream>(empty));
    }
    log_ok("Empty SendObject sent");

    // ═════════════════════════════════════════════════════════════════════
    // Phase 7: Album Verification  [pcap2 step 43]
    // ═════════════════════════════════════════════════════════════════════

    log_phase(7, "Album Verification");

    // [P43] GetObjPropList obj=album — ALL 13 properties (confirmation read)
    log_pcap(43, "GetObjPropList " + hex(album_obj_id.Id) + " — ALL album properties (confirmation)");
    try {
        auto album_props = session->GetObjectPropertyList(
            album_obj_id, mtp::ObjectFormat(0),
            mtp::ObjectProperty(0xFFFFFFFF), 0, 0);
        log_ok("Album properties: " + std::to_string(album_props.size()) + " bytes");
    } catch (const std::exception& e) {
        log_warn("Album confirmation read failed: " + std::string(e.what()));
    }

    // ═════════════════════════════════════════════════════════════════════
    // Phase 8: Album Artwork  [pcap2 steps 44-48]
    // ═════════════════════════════════════════════════════════════════════

    log_phase(8, "Album Artwork");

    // [P44] GetObjPropDesc RepSampleData for AbstractAudioAlbum
    log_pcap(44, "GetObjPropDesc RepSampleData (DC86) fmt=AbstractAudioAlbum");
    try {
        session->GetObjectPropertyDesc(
            mtp::ObjectProperty(PROP_REP_SAMPLE_DATA),
            mtp::ObjectFormat(FMT_ABSTRACT_ALBUM));
        log_ok("RepSampleData descriptor queried");
    } catch (const std::exception& e) {
        log_warn("RepSampleData desc failed: " + std::string(e.what()));
    }

    // [P45] GetObjPropValue obj=album RepSampleData — read current value (empty)
    log_pcap(45, "GetObjPropValue " + hex(album_obj_id.Id) + " RepSampleData — read current");
    try {
        auto current_art = session->GetObjectProperty(
            album_obj_id, mtp::ObjectProperty(PROP_REP_SAMPLE_DATA));
        log_ok("Current RepSampleData: " + std::to_string(current_art.size()) + " bytes");
    } catch (const std::exception& e) {
        log_warn("RepSampleData read failed: " + std::string(e.what()));
    }

    // [P46] GetObjPropDesc RepSampleFormat for AbstractAudioAlbum
    log_pcap(46, "GetObjPropDesc RepSampleFormat (DC81) fmt=AbstractAudioAlbum");
    try {
        session->GetObjectPropertyDesc(
            mtp::ObjectProperty(PROP_REP_SAMPLE_FORMAT),
            mtp::ObjectFormat(FMT_ABSTRACT_ALBUM));
        log_ok("RepSampleFormat descriptor queried");
    } catch (const std::exception& e) {
        log_warn("RepSampleFormat desc failed: " + std::string(e.what()));
    }

    // [P47] SetObjPropValue RepSampleData — set album artwork
    if (!artwork.empty()) {
        log_pcap(47, "SetObjPropValue " + hex(album_obj_id.Id) +
                 " RepSampleData — " + std::to_string(artwork.size()) + " bytes");
        try {
            mtp::ByteArray art_data(artwork.begin(), artwork.end());
            session->SetObjectPropertyAsArray(
                album_obj_id,
                mtp::ObjectProperty(PROP_REP_SAMPLE_DATA),
                art_data);
            log_ok("Album artwork set: " + std::to_string(artwork.size()) + " bytes");
        } catch (const std::exception& e) {
            log_warn("Artwork set failed: " + std::string(e.what()));
        }

        // [P48] SetObjPropValue RepSampleFormat = JPEG (0x3801)
        log_pcap(48, "SetObjPropValue " + hex(album_obj_id.Id) + " RepSampleFormat = JPEG (0x3801)");
        try {
            mtp::ByteArray fmt_val;
            mtp::OutputStream fmt_os(fmt_val);
            fmt_os.Write16(FMT_JPEG);
            session->SetObjectProperty(
                album_obj_id,
                mtp::ObjectProperty(PROP_REP_SAMPLE_FORMAT),
                fmt_val);
            log_ok("RepSampleFormat set to JPEG");
        } catch (const std::exception& e) {
            log_warn("RepSampleFormat set failed: " + std::string(e.what()));
        }
    } else {
        log_pcap(47, "SetObjPropValue RepSampleData — SKIPPED (no artwork)");
        log_pcap(48, "SetObjPropValue RepSampleFormat — SKIPPED (no artwork)");
    }

    // ═════════════════════════════════════════════════════════════════════
    // Phase 9: Link & Verify  [pcap2 steps 49-52]
    // ═════════════════════════════════════════════════════════════════════

    log_phase(9, "Link & Verify");

    // [P49] SetObjectReferences album → [track]
    log_pcap(49, "SetObjectReferences " + hex(album_obj_id.Id) + " -> [" + hex(track_id.Id) + "]");
    {
        mtp::msg::ObjectHandles refs;
        refs.ObjectHandles.push_back(track_id);
        session->SetObjectReferences(album_obj_id, refs);
    }
    log_ok("Track linked to album");

    // [P50] GetObjPropList obj=album group=2 — 6-property subset
    log_pcap(50, "GetObjPropList " + hex(album_obj_id.Id) + " group=2 — subset verification");
    try {
        auto album_subset = session->GetObjectPropertyList(
            album_obj_id, mtp::ObjectFormat(0),
            mtp::ObjectProperty(0), 2, 0);
        log_ok("Album subset: " + std::to_string(album_subset.size()) + " bytes");
    } catch (const std::exception& e) {
        log_warn("Album subset read failed: " + std::string(e.what()));
    }

    // [P51] GetObjPropDesc ParentObject for AbstractAudioAlbum
    log_pcap(51, "GetObjPropDesc ParentObject (DC0B) fmt=AbstractAudioAlbum");
    try {
        session->GetObjectPropertyDesc(
            mtp::ObjectProperty(PROP_PARENT_OBJECT),
            mtp::ObjectFormat(FMT_ABSTRACT_ALBUM));
        log_ok("ParentObject descriptor queried");
    } catch (const std::exception& e) {
        log_warn("ParentObject desc failed: " + std::string(e.what()));
    }

    // [P52] GetObjPropList obj=album — ALL 13 properties (final full read)
    log_pcap(52, "GetObjPropList " + hex(album_obj_id.Id) + " — ALL album properties (final)");
    try {
        auto album_final = session->GetObjectPropertyList(
            album_obj_id, mtp::ObjectFormat(0),
            mtp::ObjectProperty(0xFFFFFFFF), 0, 0);
        log_ok("Final album properties: " + std::to_string(album_final.size()) + " bytes");
    } catch (const std::exception& e) {
        log_warn("Final album read failed: " + std::string(e.what()));
    }

    // ═════════════════════════════════════════════════════════════════════
    // Phase 10: Post-Upload Session Management
    // ═════════════════════════════════════════════════════════════════════

    log_phase(10, "Post-Upload Session Management");

    // Op9215 — disable trusted files (signals device that upload is complete)
    log_ts("  Op9215 — disable trusted files");
    try {
        session->Operation9215();
        log_ok("Trusted files disabled");
    } catch (const std::exception& e) {
        log_warn("DisableTrustedFiles failed: " + std::string(e.what()));
    }

    // Op922b(3,1,0) — open new session (leave device ready)
    log_ts("  Op922b(3,1,0) — open new session");
    try {
        auto open_resp = session->Operation922b(3, 1, 0);
        log_ok("New session opened: " + std::to_string(open_resp.size()) + " bytes");
    } catch (const std::exception& e) {
        log_warn("Session open failed: " + std::string(e.what()));
    }

    // ═════════════════════════════════════════════════════════════════════
    // Results
    // ═════════════════════════════════════════════════════════════════════

    auto end_time = std::chrono::steady_clock::now();
    auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time).count();

    std::cout << std::endl;
    std::cout << "╔══════════════════════════════════════════════════════════╗" << std::endl;
    std::cout << "║  Upload Complete                                        ║" << std::endl;
    std::cout << "╚══════════════════════════════════════════════════════════╝" << std::endl;
    std::cout << std::endl;
    std::cout << "  Track:    " << title << " by " << artist << std::endl;
    std::cout << "  Album:    " << album << std::endl;
    std::cout << "  Track ID: " << hex(track_id.Id) << std::endl;
    std::cout << "  Album ID: " << hex(album_obj_id.Id) << std::endl;
    std::cout << "  Elapsed:  " << elapsed_ms << " ms" << std::endl;
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
