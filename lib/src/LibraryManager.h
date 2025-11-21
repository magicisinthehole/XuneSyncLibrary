#pragma once

#include <memory>
#include <string>
#include <vector>
#include <functional>
#include <unordered_map>
#include <mutex>

#include <mtp/ptp/Session.h>
#include <mtp/metadata/Library.h>
#include <cli/Session.h>

#include "zune_wireless/zune_wireless_api.h"
#include "ZuneTypes.h"


// Forward declarations
namespace zmdb {
    struct ZMDBAlbum;
}

// Internal C++ struct for file info


class LibraryManager {
public:
    using LogCallback = std::function<void(const std::string& message)>;

    LibraryManager(std::shared_ptr<mtp::Session> mtp_session, std::shared_ptr<cli::Session> cli_session, LogCallback log_callback);
    ~LibraryManager();

    // --- File System Operations ---
    std::vector<ZuneObjectInfoInternal> ListStorage(uint32_t parent_handle = 0);
    ZuneMusicLibrary* GetMusicLibrary(const std::string& device_model);  // Fast: Returns flat data using zmdb
    ZuneMusicLibrary* GetMusicLibrarySlow();  // Slow: For testing only (uses AFTL enumeration)
    std::vector<ZunePlaylistInfo> GetPlaylists();
    int DownloadFile(uint32_t object_handle, const std::string& destination_path);
    int UploadFile(const std::string& source_path, const std::string& destination_folder);
    int DeleteFile(uint32_t object_handle);

    // DEPRECATED: artwork_path parameter is not used. Use UploadTrackWithMetadata instead.
    int UploadWithArtwork(const std::string& media_path, const std::string& artwork_path);

    // --- Upload with Metadata ---
    int UploadTrackWithMetadata(
        const std::string& audio_file_path,
        const std::string& artist_name,
        const std::string& album_name,
        int album_year,
        const std::string& track_title,
        const std::string& genre,
        int track_number,
        const uint8_t* artwork_data,
        size_t artwork_size,
        const std::string& artist_guid = "",
        uint32_t* out_track_id = nullptr,
        uint32_t* out_album_id = nullptr,
        uint32_t* out_artist_id = nullptr
    );

    // --- Artist GUID Retrofit ---
    int RetrofitArtistGuid(
        const std::string& artist_name,
        const std::string& guid
    );

    // Batch retrofit result structure
    struct BatchRetrofitResult {
        int retrofitted_count;
        int already_had_guid_count;
        int not_found_count;
        int error_count;
    };

    // Artist GUID mapping structure
    struct ArtistGuidMapping {
        std::string artist_name;
        std::string guid;
    };

    BatchRetrofitResult RetrofitMultipleArtistGuids(
        const std::vector<ArtistGuidMapping>& mappings
    );

    // --- Streaming/Partial Downloads ---
    mtp::ByteArray GetPartialObject(uint32_t object_id, uint64_t offset, uint32_t size);
    uint64_t GetObjectSize(uint32_t object_id);
    std::string GetObjectFilename(uint32_t object_id);

    // --- Track Lookup ---
    uint32_t GetAudioTrackObjectId(const std::string& track_title, uint32_t album_object_id);
    void ClearTrackObjectIdCache();

    // --- Metadata Retrieval ---
    mtp::ByteArray GetZuneMetadata(const std::vector<uint8_t>& object_id);

private:
    std::shared_ptr<mtp::Session> mtp_session_;
    std::shared_ptr<cli::Session> cli_session_;
    std::shared_ptr<mtp::Library> library_;
    LogCallback log_callback_;

    // Track ObjectId cache: key = "album_id:track_title", value = track_object_id
    std::unordered_map<std::string, uint32_t> track_objectid_cache_;
    mutable std::mutex track_cache_mutex_;

    void Log(const std::string& message);
    void EnsureLibraryInitialized();
    std::string Utf16leToAscii(const mtp::ByteArray& data, bool is_guid = false);
    mtp::ByteArray HexToBytes(const std::string& hex_str);
};
