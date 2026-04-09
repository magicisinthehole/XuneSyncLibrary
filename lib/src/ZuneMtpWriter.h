#pragma once

/**
 * ZuneMtpWriter — Stateless MTP write operations for Zune devices.
 *
 * Each method corresponds to an exact operation pattern observed in Zune Desktop
 * USB captures. See redocs/multi-album-upload-analysis.md for the complete
 * verified sequence.
 *
 * Follows the same pattern as ZuneMtpReader: all-static, SessionPtr as first
 * parameter, no cached state. The caller (C# DeviceSyncSession) owns sequencing.
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
    uint32_t playlists_folder = 0;
    uint32_t series_folder = 0;
    uint32_t podcasts_folder = 0;
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
    uint32_t disc_number = 0;    // 0xDAB8 disc number (HD only, Uint32: 1=disc1, 2=disc2)
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

struct PodcastSeriesProperties {
    std::string name;
    std::string artist;          // Podcast author
    std::string feed_url;        // RSS feed URL (written as AUINT16)
    std::string filename;        // e.g. "Series Name.ser"
};

struct PodcastEpisodeProperties {
    std::string title;
    std::string artist;          // Episode author
    std::string series_name;     // Parent series name (0xDA9A)
    std::string date_authored;   // Format: "YYYYMMDDTHHMMSS.0"
    std::string description;     // Episode description (written as AUINT16)
    std::string source_url;      // Episode download URL (written as AUINT16)
    std::string filename;        // e.g. "Episode Title.mp3"
    uint32_t duration_ms = 0;
    uint32_t series_handle = 0;  // MTP handle of parent 0xBA0B object
    uint16_t format_code = 0;    // MTP format: 0x3009 (MP3), 0xB981 (WMV), etc.
    bool is_video = false;       // false=MetaGenre 64 (audio), true=MetaGenre 65 (video)
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
    constexpr uint16_t DC9D             = 0xDC9D;  // MTP "DiscNumber" but always 0 on Zune
    constexpr uint16_t ZuneCollectionId = 0xDAB0;
    constexpr uint16_t ZunePropDAB2     = 0xDAB2;
    constexpr uint16_t DiscNumber       = 0xDAB8;  // HD disc number (Uint32: 1, 2, ...)
    constexpr uint16_t ArtistId        = 0xDAB9;  // HD artist metadata reference
    constexpr uint16_t DA97             = 0xDA97;
    // Podcast-specific properties
    constexpr uint16_t SeriesName       = 0xDA9A;
    constexpr uint16_t DA9B             = 0xDA9B;  // Always 0 on episodes
    constexpr uint16_t IsPodcast        = 0xDA9C;  // UINT8, always 1 on series
    constexpr uint16_t DA9D             = 0xDA9D;  // UINT32, always 1 on series
    constexpr uint16_t SeriesHandle     = 0xDA9E;  // UINT32, parent series MTP handle
    constexpr uint16_t Description      = 0xDC48;  // AUINT16, episode description
    constexpr uint16_t SourceURL        = 0xDD60;  // AUINT16, feed URL (series) or episode URL
    constexpr uint16_t DD62             = 0xDD62;  // UINT32, always 0 on episodes
}

namespace MtpFmt {
    constexpr uint16_t Folder           = 0x3001;
    constexpr uint16_t JPEG             = 0x3801;
    constexpr uint16_t AbstractAlbum    = 0xBA03;
    constexpr uint16_t ArtistMeta       = 0xB218;
    constexpr uint16_t PodcastSeries    = 0xBA0B;
}

namespace MtpType {
    constexpr uint16_t Uint8   = 0x0002;
    constexpr uint16_t Uint16  = 0x0004;
    constexpr uint16_t Uint32  = 0x0006;
    constexpr uint16_t Uint128 = 0x000A;
    constexpr uint16_t Auint16 = 0x4004;  // Array of uint16 — UTF-16LE string encoding
    constexpr uint16_t String  = 0xFFFF;
}

namespace MetaGenre {
    constexpr uint16_t Music        = 1;
    constexpr uint16_t AudioPodcast = 64;
    constexpr uint16_t VideoPodcast = 65;
}

// ── Upload Primitives Class ──────────────────────────────────────────────

class MtpWriter {
public:
    using SessionPtr = std::shared_ptr<mtp::Session>;

    // ── Pre-Upload ───────────────────────────────────────────────
    static void QueryStorageInfo(const SessionPtr& session, uint32_t storageId);
    static RootDiscoveryResult DiscoverRoot(const SessionPtr& session, uint32_t storageId, bool isHD);
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

    // ── Property Updates (SetObjectPropList 0x9806) ───────────────
    static void UpdateTrackProperties(
        const SessionPtr& session, uint32_t trackMtpId,
        const TrackProperties& props);
    static void UpdateAlbumProperties(
        const SessionPtr& session, uint32_t albumMtpId,
        const AlbumProperties& props);

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
    static void RegisterTrackContext(const SessionPtr& session, const std::string& trackName);

    // ── Podcast Operations ────────────────────────────────────────
    // Create a podcast series (.ser object, format 0xBA0B) on the device.
    // Returns new series MTP object handle, or 0 on failure.
    static uint32_t CreatePodcastSeries(
        const SessionPtr& session, uint32_t storageId, uint32_t seriesFolderId,
        const PodcastSeriesProperties& props);

    // Upload a podcast episode (MP3 or WMV) to the device.
    // Returns new episode MTP object handle, or 0 on failure.
    static uint32_t CreatePodcastEpisode(
        const SessionPtr& session, uint32_t storageId, uint32_t episodeFolderId,
        const PodcastEpisodeProperties& props, uint64_t fileSize);

    // Set artwork on a podcast series object (same pattern as album artwork).
    static void SetSeriesArtwork(
        const SessionPtr& session, uint32_t seriesObjId,
        const uint8_t* data, size_t size);

    // Verification readback for a newly created series object.
    static void VerifySeries(const SessionPtr& session, uint32_t seriesObjId);

    // Query property descriptors for podcast series format (0xBA0B).
    static void QuerySeriesDescriptors(const SessionPtr& session);

    // Query property descriptors for podcast episode format (MP3 or WMV).
    static void QueryEpisodeDescriptors(
        const SessionPtr& session, uint16_t formatCode);

    // ── Playlist Operations ──────────────────────────────────────
    // Create a playlist (.pla object) on the device.
    // Returns new playlist MTP object ID, or 0 on failure.
    static uint32_t CreatePlaylist(
        const SessionPtr& session, uint32_t storageId, uint32_t playlistsFolderId,
        const std::string& name, const std::string& guid,
        const uint32_t* trackIds, size_t trackCount);

    // Replace all track references on an existing playlist.
    static bool UpdatePlaylistTracks(
        const SessionPtr& session, uint32_t playlistMtpId,
        const uint32_t* trackIds, size_t trackCount);

    // Delete a playlist from the device.
    static bool DeletePlaylist(const SessionPtr& session, uint32_t playlistMtpId);

    // ── Object Deletion ─────────────────────────────────────────
    static int DeleteObject(const SessionPtr& session, uint32_t objectHandle);

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
    // Property list writing helpers — handle defaults to 0 for creation (SendObjPropList)
    static void WritePropString(mtp::OutputStream& os, uint16_t prop, const std::string& value, uint32_t handle = 0);
    static void WritePropU8(mtp::OutputStream& os, uint16_t prop, uint8_t value, uint32_t handle = 0);
    static void WritePropU16(mtp::OutputStream& os, uint16_t prop, uint16_t value, uint32_t handle = 0);
    static void WritePropU32(mtp::OutputStream& os, uint16_t prop, uint32_t value, uint32_t handle = 0);
    static void WritePropU128(mtp::OutputStream& os, uint16_t prop, const uint8_t* bytes, size_t len, uint32_t handle = 0);
    // Write a UTF-8 string as AUINT16 (array of uint16, UTF-16LE encoded)
    static void WritePropAuint16String(mtp::OutputStream& os, uint16_t prop, const std::string& value, uint32_t handle = 0);

    // Batch format helpers
    static const uint16_t* GetBatchFormats(bool isHD);
    static size_t GetBatchFormatCount(bool isHD);

    // Parse MTP ObjectPropertyList response into (handle, name) pairs
    static std::vector<std::pair<uint32_t, std::string>> ParsePropertyListNames(
        const mtp::ByteArray& data);

    // GUID conversion (mixed-endian Windows format)
    static mtp::ByteArray GuidStringToBytes(const std::string& guid_str);
};

} // namespace zune
