#ifndef ZUNE_WIRELESS_API_H
#define ZUNE_WIRELESS_API_H

#ifdef __cplusplus
extern "C" {
#endif

#if defined(_WIN32)
    #ifdef ZUNE_WIRELESS_EXPORTS
        #define ZUNE_WIRELESS_API __declspec(dllexport)
    #else
        #define ZUNE_WIRELESS_API __declspec(dllimport)
    #endif
#else
    #define ZUNE_WIRELESS_API __attribute__((visibility("default")))
#endif

// Opaque handle to the ZuneDevice object
typedef void* zune_device_handle_t;

// Callback types
typedef void (*log_callback_t)(const char* message);
typedef void (*device_discovered_callback_t)(const char* ip_address, const char* uuid);

#include <stdint.h>

// Struct to pass file info between C++ and C#
struct ZuneObjectInfo {
    uint32_t handle;
    const char* filename;
    uint64_t size;
    bool is_folder;
};

// Flat music library structures - grouping done in C# with LINQ
struct ZuneMusicTrack {
    const char* title;
    const char* artist_name;
    const char* album_name;
    const char* genre;
    const char* filename;
    int track_number;
    int duration_ms;
    uint32_t album_ref;  // References album atom_id for grouping
    uint32_t atom_id;
};

struct ZuneMusicAlbum {
    const char* title;
    const char* artist_name;
    const char* alb_reference;  // For artwork lookup (e.g., "Artist--Album.alb")
    int release_year;
    uint32_t atom_id;
};

struct ZuneAlbumArtwork {
    const char* alb_reference;  // e.g., "Artist--Album.alb"
    uint32_t mtp_object_id;     // MTP ObjectId for .alb file
};

struct ZuneMusicLibrary {
    ZuneMusicTrack* tracks;
    uint32_t track_count;
    ZuneMusicAlbum* albums;
    uint32_t album_count;
    ZuneAlbumArtwork* artworks;
    uint32_t artwork_count;
};

struct ZunePlaylistInfo {
    const char* Name;
    uint32_t TrackCount;
    const char** TrackPaths;
    uint32_t MtpObjectId;
};


// API functions
ZUNE_WIRELESS_API zune_device_handle_t zune_device_create();
ZUNE_WIRELESS_API void zune_device_destroy(zune_device_handle_t handle);

ZUNE_WIRELESS_API bool zune_device_connect_usb(zune_device_handle_t handle);
ZUNE_WIRELESS_API void zune_device_disconnect(zune_device_handle_t handle);

ZUNE_WIRELESS_API void zune_device_set_log_callback(zune_device_handle_t handle, log_callback_t callback);

ZUNE_WIRELESS_API int zune_device_establish_sync_pairing(zune_device_handle_t handle, const char* device_name);
ZUNE_WIRELESS_API const char* zune_device_establish_wireless_pairing(zune_device_handle_t handle, const char* ssid, const char* password);
ZUNE_WIRELESS_API int zune_device_disable_wireless(zune_device_handle_t handle);

// Device Info Functions
ZUNE_WIRELESS_API const char* zune_device_get_name(zune_device_handle_t handle);
ZUNE_WIRELESS_API const char* zune_device_get_serial_number(zune_device_handle_t handle);
ZUNE_WIRELESS_API const char* zune_device_get_model(zune_device_handle_t handle);

// File System Functions
ZUNE_WIRELESS_API ZuneObjectInfo* zune_device_list_storage(zune_device_handle_t handle, uint32_t parent_handle, uint32_t* count);
ZUNE_WIRELESS_API void zune_device_free_object_info_array(ZuneObjectInfo* array, uint32_t count);
ZUNE_WIRELESS_API ZuneMusicLibrary* zune_device_get_music_library(zune_device_handle_t handle);
ZUNE_WIRELESS_API void zune_device_free_music_library(ZuneMusicLibrary* library);
ZUNE_WIRELESS_API ZunePlaylistInfo* zune_device_get_playlists(zune_device_handle_t handle, uint32_t* count);
ZUNE_WIRELESS_API void zune_device_free_playlists(ZunePlaylistInfo* playlists, uint32_t count);
ZUNE_WIRELESS_API int zune_device_download_file(zune_device_handle_t handle, uint32_t object_handle, const char* destination_path);
ZUNE_WIRELESS_API int zune_device_upload_file(zune_device_handle_t handle, const char* source_path, const char* destination_folder);
ZUNE_WIRELESS_API int zune_device_delete_file(zune_device_handle_t handle, uint32_t object_handle);

// Artwork/Metadata
// DEPRECATED: artwork_path parameter is not used. Use zune_device_upload_track instead.
// This function currently ignores artwork_path and only uploads the media file.
ZUNE_WIRELESS_API int zune_device_upload_with_artwork(zune_device_handle_t handle, const char* media_path, const char* artwork_path);

// Upload with metadata (uses Library for proper MTP structure)
ZUNE_WIRELESS_API int zune_device_upload_track(
    zune_device_handle_t handle,
    const char* audio_file_path,
    const char* artist_name,
    const char* album_name,
    int album_year,
    const char* track_title,
    const char* genre,
    int track_number,
    const uint8_t* artwork_data,
    uint32_t artwork_size
);

// Streaming/Partial Downloads
ZUNE_WIRELESS_API int zune_device_get_partial_object(
    zune_device_handle_t handle,
    uint32_t object_id,
    uint64_t offset,
    uint32_t size,
    uint8_t** out_data,
    uint32_t* out_size
);
ZUNE_WIRELESS_API void zune_device_free_partial_data(uint8_t* data);
ZUNE_WIRELESS_API uint64_t zune_device_get_object_size(zune_device_handle_t handle, uint32_t object_id);
ZUNE_WIRELESS_API const char* zune_device_get_object_filename(zune_device_handle_t handle, uint32_t object_id);

// Dynamic ObjectId resolution
// Query MTP for a specific audio track's ObjectId by title/filename within an album context.
// This is useful when ObjectIds may be stale or were not populated during initial library scan.
//
// Parameters:
//   handle - Device handle from zune_device_create()
//   track_title - The title/filename of the audio track (with or without file extension)
//   album_object_id - The MTP ObjectId of the album folder to search within (required, must be > 0)
// Returns:
//   The MTP ObjectId of the audio track, or 0 if not found or album_object_id is invalid
// Note:
//   - Comparison is exact match, case-sensitive
//   - This function may make multiple MTP queries; results should be cached in persistent storage
//   - Requires valid album context for efficient searching
ZUNE_WIRELESS_API uint32_t zune_device_get_audio_track_object_id(
    zune_device_handle_t handle,
    const char* track_title,
    uint32_t album_object_id
);

// USB Discovery functions
ZUNE_WIRELESS_API bool zune_device_find_on_usb(const char** uuid, const char** device_name);

// SSDP Discovery functions
ZUNE_WIRELESS_API void zune_ssdp_start_discovery(device_discovered_callback_t callback);
ZUNE_WIRELESS_API void zune_ssdp_stop_discovery();

// ============================================================================
// Artist Metadata HTTP Interception
// ============================================================================

/// Configuration mode for artist metadata interception
typedef enum {
    ZUNE_ARTIST_METADATA_MODE_DISABLED = 0,
    ZUNE_ARTIST_METADATA_MODE_STATIC = 1,    // Serve from local files
    ZUNE_ARTIST_METADATA_MODE_PROXY = 2      // Forward to HTTP server
} ZuneArtistMetadataMode;

/// Configuration for artist metadata interception
typedef struct {
    ZuneArtistMetadataMode mode;

    // Static mode configuration
    const char* static_data_directory;  // Path to artist_data/ folder

    // Proxy mode configuration
    const char* proxy_catalog_server;   // e.g. "http://192.168.0.30"
    const char* proxy_image_server;     // Can be NULL to use catalog_server
    const char* proxy_art_server;       // Can be NULL to use catalog_server
    const char* proxy_mix_server;       // Can be NULL to use catalog_server
    int proxy_timeout_ms;               // HTTP request timeout (default 5000)

    // Server IP for DNS resolution (e.g., "192.168.0.30")
    const char* server_ip;              // IP address that DNS will resolve to (port 80)
} ZuneArtistMetadataConfig;

/// Start the artist metadata HTTP interceptor
/// @param handle Device handle from zune_device_create()
/// @param config Configuration for interception mode
/// @return 0 on success, negative error code on failure
ZUNE_WIRELESS_API int zune_device_start_artist_metadata_interceptor(
    zune_device_handle_t handle,
    const ZuneArtistMetadataConfig* config
);

/// Stop the artist metadata HTTP interceptor
/// @param handle Device handle
ZUNE_WIRELESS_API void zune_device_stop_artist_metadata_interceptor(
    zune_device_handle_t handle
);

/// Check if interceptor is running
/// @param handle Device handle
/// @return true if interceptor is active
ZUNE_WIRELESS_API bool zune_device_is_artist_metadata_interceptor_running(
    zune_device_handle_t handle
);

/// Get current interceptor configuration
/// @param handle Device handle
/// @param config Output buffer for configuration (caller allocated)
/// @return 0 on success, negative error code on failure
ZUNE_WIRELESS_API int zune_device_get_artist_metadata_config(
    zune_device_handle_t handle,
    ZuneArtistMetadataConfig* config
);

/// Helper: Create default static mode config
/// @param data_directory Path to artist_data folder
/// @return Configuration structure (caller must free strings with zune_artist_metadata_config_free)
ZUNE_WIRELESS_API ZuneArtistMetadataConfig zune_artist_metadata_config_static(
    const char* data_directory
);

/// Helper: Create default proxy mode config
/// @param server_base_url Base URL for all servers (e.g. "http://localhost:8000")
/// @return Configuration structure (caller must free strings with zune_artist_metadata_config_free)
ZUNE_WIRELESS_API ZuneArtistMetadataConfig zune_artist_metadata_config_proxy(
    const char* server_base_url
);

/// Free strings allocated by config helpers
ZUNE_WIRELESS_API void zune_artist_metadata_config_free(ZuneArtistMetadataConfig* config);


#ifdef __cplusplus
}
#endif

#endif // ZUNE_WIRELESS_API_H
