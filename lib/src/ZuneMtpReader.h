#pragma once

/**
 * ZuneMtpReader — Stateless read-only MTP operations for Zune devices.
 *
 * Follows the same pattern as ZuneMtpWriter: all-static, SessionPtr
 * as first parameter, no cached state. The caller owns caching and sequencing.
 */

#include <mtp/ptp/Session.h>
#include "xune_sync/xune_sync_api.h"
#include "ZuneDeviceIdentification.h"

#include <string>
#include <vector>
#include <cstdint>
#include <utility>

namespace zune {

struct TrackReference {
    std::string name;       // Track name (without file extension)
    uint32_t object_id;
};

class MtpReader {
public:
    using SessionPtr = std::shared_ptr<mtp::Session>;

    // --- Object Properties ---
    static uint64_t GetObjectSize(const SessionPtr& session, uint32_t object_id);
    static std::string GetObjectFilename(const SessionPtr& session, uint32_t object_id);

    // --- Streaming / Partial Downloads ---
    static mtp::ByteArray GetPartialObject(
        const SessionPtr& session, uint32_t object_id,
        uint64_t offset, uint32_t size);

    // --- Artwork Download ---
    // Downloads album artwork via RepresentativeSampleData property, writes to file.
    // Returns 0 on success, -1 on error.
    static int DownloadArtwork(
        const SessionPtr& session, uint32_t object_handle,
        const std::string& destination_path);

    // --- Track Lookup ---
    // Queries album's object references, matches by track title (sans extension).
    // Returns matching track's ObjectId (0 if not found).
    // If siblings_out is non-null, populates it with ALL tracks in the album
    // (name sans extension → objectId) for caller-side caching.
    static uint32_t FindTrackObjectId(
        const SessionPtr& session,
        const std::string& track_title,
        uint32_t album_object_id,
        std::vector<TrackReference>* siblings_out = nullptr);

    // --- ZMDB (Zune Metadata Database) ---
    // Reads raw ZMDB data from device via bulk pipe protocol (Op9217).
    static mtp::ByteArray ReadZuneMetadata(
        const SessionPtr& session,
        const std::vector<uint8_t>& object_id);

    // --- Full Library Read ---
    // Reads ZMDB + queries MTP album artwork ObjectIds → builds ZuneMusicLibrary.
    // Caller owns the returned pointer (free with FreeLibrary or zune_device_free_music_library).
    static ZuneMusicLibrary* ReadMusicLibrary(
        const SessionPtr& session,
        zune::DeviceFamily device_family);

    // --- Library Cleanup ---
    // Frees a ZuneMusicLibrary allocated by ReadMusicLibrary.
    static void FreeLibrary(ZuneMusicLibrary* library);
};

} // namespace zune
