#pragma once

/**
 * ZuneUploadPrimitives — Pcap-verified MTP operations for Zune uploads.
 *
 * Each method corresponds to an exact operation pattern observed in Zune Desktop
 * USB captures. See redocs/multi-album-upload-analysis.md for the complete
 * verified sequence.
 *
 * This class is STATELESS — it executes operations on the MTP session without
 * caching. The caller (C# DeviceSyncSession) owns sequencing and state.
 */

#include <mtp/ptp/Session.h>
#include <mtp/ptp/ObjectFormat.h>
#include <mtp/ptp/ObjectProperty.h>
#include <mtp/ptp/OutputStream.h>
#include <mtp/ptp/ByteArrayObjectStream.h>
#include <string>
#include <vector>
#include <map>
#include <cstdint>

namespace zune {

// ── Result Structures ────────────────────────────────────────────────────

struct RootDiscoveryResult {
    uint32_t music_folder = 0;
    uint32_t albums_folder = 0;
    uint32_t artists_folder = 0;
    uint32_t storage_id = 0;
    int root_object_count = 0;
};

struct FolderChild {
    std::string name;
    uint32_t handle = 0;
};

struct TrackProperties {
    std::string filename;
    std::string title;
    std::string artist;
    std::string album_name;
    std::string album_artist;
    std::string genre;
    std::string date_authored;   // Format: "YYYYMMDDTHHMMSS.0"
    uint32_t duration_ms = 0;
    uint16_t track_number = 0;
    int rating = -1;             // -1 = omit, 0+ = include
    // HD-only
    uint32_t artist_meta_id = 0; // 0xDAB9 reference
    bool is_hd = false;
};

struct AlbumProperties {
    std::string artist;
    std::string album_name;
    std::string date_authored;   // Format: "YYYYMMDDTHHMMSS.0" (HD only)
    uint32_t artist_meta_id = 0; // 0xDAB9 reference (HD only)
    bool is_hd = false;
};

// ── Format Lists (from pcap) ─────────────────────────────────────────────

// Classic: 17 formats for batch GetObjPropDesc queries
static const uint16_t CLASSIC_BATCH_FORMATS[] = {
    0x3009, 0xB901, 0x300C, 0xB215, 0xB903, 0xB904, 0xB301,
    0xB981, 0x3801, 0x3001, 0xBA03, 0xBA05, 0xB211, 0xB213,
    0x3000, 0xB802, 0xBA0B,
};
static constexpr size_t CLASSIC_BATCH_FORMAT_COUNT = 17;

// HD: 21 formats for batch GetObjPropDesc queries
static const uint16_t HD_BATCH_FORMATS[] = {
    0x3009, 0xB901, 0x300C, 0xB215, 0xB903, 0xB904, 0xB301,
    0xB216, 0xB982, 0xB981, 0x300A, 0x3801, 0x3001, 0xBA03,
    0xBA05, 0xB211, 0x3000, 0xB802, 0xBA0B, 0xB218, 0xB217,
};
static constexpr size_t HD_BATCH_FORMAT_COUNT = 21;

// ── MTP Constants ────────────────────────────────────────────────────────

namespace MtpProp {
    constexpr uint16_t StorageID        = 0xDC01;
    constexpr uint16_t ObjectFormat     = 0xDC02;
    constexpr uint16_t ObjectFileName   = 0xDC07;
    constexpr uint16_t ParentObject     = 0xDC0B;
    constexpr uint16_t PersistentUID    = 0xDC41;
    constexpr uint16_t Name             = 0xDC44;
    constexpr uint16_t Artist           = 0xDC46;
    constexpr uint16_t DateAuthored     = 0xDC47;
    constexpr uint16_t RepSampleFormat  = 0xDC81;
    constexpr uint16_t RepSampleData    = 0xDC86;
    constexpr uint16_t Duration         = 0xDC89;
    constexpr uint16_t Rating           = 0xDC8A;
    constexpr uint16_t Track            = 0xDC8B;
    constexpr uint16_t Genre            = 0xDC8C;
    constexpr uint16_t MetaGenre        = 0xDC95;
    constexpr uint16_t AlbumName        = 0xDC9A;
    constexpr uint16_t AlbumArtist      = 0xDC9B;
    constexpr uint16_t DiscNumber       = 0xDC9D;
    constexpr uint16_t ZuneCollectionId = 0xDAB0;
    constexpr uint16_t ZunePropDAB2     = 0xDAB2;
    constexpr uint16_t DAB8             = 0xDAB8;
    constexpr uint16_t DAB9             = 0xDAB9;
    constexpr uint16_t DA97             = 0xDA97;
}

namespace MtpFmt {
    constexpr uint16_t Folder           = 0x3001;
    constexpr uint16_t JPEG             = 0x3801;
    constexpr uint16_t AbstractAlbum    = 0xBA03;
    constexpr uint16_t ArtistMeta       = 0xB218;
}

namespace MtpType {
    constexpr uint16_t Uint8   = 0x0002;
    constexpr uint16_t Uint16  = 0x0004;
    constexpr uint16_t Uint32  = 0x0006;
    constexpr uint16_t Uint128 = 0x000A;
    constexpr uint16_t String  = 0xFFFF;
}

// ── Upload Primitives Class ──────────────────────────────────────────────

class UploadPrimitives {
public:
    using SessionPtr = std::shared_ptr<mtp::Session>;

    // ── Pre-Upload ───────────────────────────────────────────────
    static void ReadDeviceSyncStatus(const SessionPtr& session);
    static void SyncDeviceDB(const SessionPtr& session);
    static void QueryStorageInfo(const SessionPtr& session, uint32_t storageId);
    static RootDiscoveryResult DiscoverRoot(const SessionPtr& session, uint32_t storageId);
    static void RootReEnum(const SessionPtr& session);

    // ── Folder Discovery & Creation ──────────────────────────────
    static std::vector<FolderChild> DiscoverFolderChildren(
        const SessionPtr& session, uint32_t storageId, uint32_t folderId);
    static uint32_t CreateFolder(
        const SessionPtr& session, uint32_t storageId, uint32_t parentId,
        const std::string& name);
    static void FolderReadback(
        const SessionPtr& session, uint32_t folderId, uint32_t storageId);
    static void FirstFolderReadback(
        const SessionPtr& session, uint32_t folderId, uint32_t storageId,
        bool isHD);

    // ── Artist Metadata (HD Only) ────────────────────────────────
    static uint32_t CreateArtistMetadata(
        const SessionPtr& session, uint32_t storageId, uint32_t artistsFolderId,
        const std::string& name, const uint8_t* guidBytes, size_t guidLen);

    // ── Track Operations ─────────────────────────────────────────
    static uint32_t CreateTrack(
        const SessionPtr& session, uint32_t storageId, uint32_t albumFolderId,
        const TrackProperties& props, uint16_t formatCode, uint64_t fileSize);
    static void UploadAudioData(const SessionPtr& session, const std::string& filePath);
    static void VerifyTrack(const SessionPtr& session, uint32_t trackId);

    // ── Album Metadata Operations ────────────────────────────────
    static uint32_t CreateAlbumMetadata(
        const SessionPtr& session, uint32_t storageId, uint32_t albumsFolderId,
        const AlbumProperties& props);
    static void SetAlbumArtwork(
        const SessionPtr& session, uint32_t albumObjId,
        const uint8_t* data, size_t size);
    static void ReadAlbumArtworkCurrent(
        const SessionPtr& session, uint32_t albumObjId);
    static void SetAlbumReferences(
        const SessionPtr& session, uint32_t albumObjId,
        const uint32_t* trackIds, size_t count);
    static void VerifyAlbum(
        const SessionPtr& session, uint32_t albumObjId, bool includeParentDesc);

    // ── Finalization ─────────────────────────────────────────────
    static void DisableTrustedFiles(const SessionPtr& session);
    static void OpenIdleSession(const SessionPtr& session);
    static void CloseSession(const SessionPtr& session);
    static void RegisterTrackContext(const SessionPtr& session, const std::string& trackName);

    // ── Property Descriptor Queries ──────────────────────────────
    static void QueryFolderDescriptors(const SessionPtr& session);
    static void QueryBatchDescriptors(
        const SessionPtr& session, uint16_t propCode, bool isHD);
    static void QueryTrackDescriptors(
        const SessionPtr& session, uint16_t formatCode, bool isHD);
    static void QueryAlbumDescriptors(const SessionPtr& session, bool isHD);
    static void QueryArtistDescriptors(const SessionPtr& session);
    static void QueryArtworkDescriptors(const SessionPtr& session);

private:
    // Property list writing helpers
    static void WritePropString(mtp::OutputStream& os, uint16_t prop, const std::string& value);
    static void WritePropU8(mtp::OutputStream& os, uint16_t prop, uint8_t value);
    static void WritePropU16(mtp::OutputStream& os, uint16_t prop, uint16_t value);
    static void WritePropU32(mtp::OutputStream& os, uint16_t prop, uint32_t value);
    static void WritePropU128(mtp::OutputStream& os, uint16_t prop, const uint8_t* bytes, size_t len);

    // Batch format helpers
    static const uint16_t* GetBatchFormats(bool isHD);
    static size_t GetBatchFormatCount(bool isHD);
};

} // namespace zune
