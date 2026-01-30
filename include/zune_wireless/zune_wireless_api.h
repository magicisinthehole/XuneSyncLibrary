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
#include <stddef.h>

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
    const char* artist_guid;        // Artist GUID (MusicBrainz UUID, optional)
    const char* album_name;
    const char* album_artist_name;  // Album artist name (may differ from track artist)
    const char* album_artist_guid;  // Album artist GUID (optional)
    const char* genre;
    const char* filename;
    int track_number;
    int disc_number;        // Disc number for multi-disc albums (field 0x6c, default=1)
    int duration_ms;
    int file_size_bytes;    // File size in bytes
    uint32_t album_ref;     // References album atom_id for grouping
    uint32_t atom_id;
    uint16_t playcount;     // Play count (offset 26-27)
    uint16_t skip_count;    // Skip count (field 0x63)
    uint16_t codec_id;      // Format code: 0xb901=WMA, 0x3009=MP3
    uint8_t rating;         // Rating: 0=neutral, 8=liked, 2=disliked (offset 30)
    uint64_t last_played_timestamp; // Windows FILETIME of last play/skip event (field 0x70)
};

struct ZuneMusicAlbum {
    const char* title;
    const char* artist_name;
    const char* artist_guid;    // Album artist GUID (optional)
    const char* alb_reference;  // For artwork lookup (e.g., "Artist--Album.alb")
    int release_year;
    uint32_t atom_id;
    uint32_t album_pid;         // Property ID (0x0600xxxx format)
    uint32_t artist_ref;        // Artist atom_id reference
};

struct ZuneMusicArtist {
    const char* name;           // Artist name (UTF-8)
    const char* filename;       // .art file reference (for artist artwork)
    const char* guid;           // MusicBrainz GUID (optional)
    uint32_t atom_id;           // Artist atom_id (MTP object ID)
};

struct ZuneAlbumArtwork {
    const char* alb_reference;  // e.g., "Artist--Album.alb"
    uint32_t mtp_object_id;     // MTP ObjectId for .alb file
};

struct ZuneMusicPlaylist {
    const char* name;           // Playlist name (UTF-8)
    const char* filename;       // .zpl filename on device
    const char* guid;           // Playlist GUID (hex string)
    const char* folder;         // Folder reference (e.g., "Playlists")
    uint32_t* track_atom_ids;   // Array of track atom_ids (references into tracks array)
    uint32_t track_count;       // Number of tracks in this playlist
    uint32_t atom_id;           // Playlist's own atom_id
};

struct ZuneMusicLibrary {
    ZuneMusicTrack* tracks;
    uint32_t track_count;
    ZuneMusicAlbum* albums;
    uint32_t album_count;
    ZuneMusicArtist* artists;
    uint32_t artist_count;
    ZuneAlbumArtwork* artworks;
    uint32_t artwork_count;
    ZuneMusicPlaylist* playlists;
    uint32_t playlist_count;
};

struct ZunePlaylistInfo {
    const char* Name;
    uint32_t TrackCount;
    const char** TrackPaths;
    uint32_t MtpObjectId;
};

// Result struct for upload operations
struct ZuneUploadResult {
    uint32_t track_object_id;   // MTP ObjectId of uploaded track
    uint32_t album_object_id;   // MTP ObjectId of album metadata object
    uint32_t artist_object_id;  // MTP ObjectId of artist metadata object
    int status;                  // 0 = success, negative = error code
};


// API functions
ZUNE_WIRELESS_API zune_device_handle_t zune_device_create();
ZUNE_WIRELESS_API void zune_device_destroy(zune_device_handle_t handle);

ZUNE_WIRELESS_API bool zune_device_connect_usb(zune_device_handle_t handle);
ZUNE_WIRELESS_API void zune_device_disconnect(zune_device_handle_t handle);

// Connection validation - lightweight MTP operation to verify session is still valid
ZUNE_WIRELESS_API bool zune_device_is_connected(zune_device_handle_t handle);
ZUNE_WIRELESS_API bool zune_device_validate_connection(zune_device_handle_t handle);

ZUNE_WIRELESS_API void zune_device_set_log_callback(zune_device_handle_t handle, log_callback_t callback);

ZUNE_WIRELESS_API int zune_device_establish_sync_pairing(zune_device_handle_t handle, const char* device_name);
ZUNE_WIRELESS_API const char* zune_device_establish_wireless_pairing(zune_device_handle_t handle, const char* ssid, const char* password);
ZUNE_WIRELESS_API int zune_device_disable_wireless(zune_device_handle_t handle);

// Sync Partnership Functions
// GUID returned when device has no sync partnership (unpaired or pairing incomplete)
#define ZUNE_NULL_GUID "{00000000-0000-0000-0000-000000000000}"
// Returns device's sync partner GUID (MTP property 0xd401)
// Returns nullptr on error, ZUNE_NULL_GUID if not paired, real GUID if paired
ZUNE_WIRELESS_API const char* zune_device_get_sync_partner_guid(zune_device_handle_t handle);
// Sets device friendly name (MTP property 0xd402) without full pairing
// Returns 0 on success, -1 on invalid input, -2 if not connected, -3 on MTP error
ZUNE_WIRELESS_API int zune_device_set_device_name(zune_device_handle_t handle, const char* name);

// Device Info Functions
ZUNE_WIRELESS_API const char* zune_device_get_name(zune_device_handle_t handle);
ZUNE_WIRELESS_API const char* zune_device_get_serial_number(zune_device_handle_t handle);
ZUNE_WIRELESS_API const char* zune_device_get_model(zune_device_handle_t handle);

// Device Capability Functions
// Returns true if device supports network mode (HTTP-based artist metadata proxy).
// Only Zune HD devices (USB Product ID 0x063e) support this feature.
// Classic devices (Zune 30/80/120/Flash) do not have the required hardware.
ZUNE_WIRELESS_API bool zune_device_supports_network_mode(zune_device_handle_t handle);

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

// Upload music track with metadata (uses Library for proper MTP structure)
// rating: -1 = unrated, 8 = liked, 3 = disliked (Zune format, set during upload)
ZUNE_WIRELESS_API ZuneUploadResult zune_device_upload_track(
    zune_device_handle_t handle,
    const char* audio_file_path,
    const char* artist_name,
    const char* album_name,
    int album_year,
    const char* track_title,
    const char* genre,
    int track_number,
    const uint8_t* artwork_data,
    uint32_t artwork_size,
    const char* artist_guid,
    uint32_t duration_ms,
    int rating
);

// Upload audiobook track with metadata
// author_name maps to artist_name, audiobook_name maps to album_name
ZUNE_WIRELESS_API ZuneUploadResult zune_device_upload_audiobook_track(
    zune_device_handle_t handle,
    const char* audio_file_path,
    const char* author_name,
    const char* audiobook_name,
    int release_year,
    const char* track_title,
    int track_number,
    const uint8_t* artwork_data,
    uint32_t artwork_size,
    uint32_t duration_ms
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

// ============================================================================
// Playlist Management
// ============================================================================

/// Create a playlist on the device
/// @param handle Device handle from zune_device_create()
/// @param name Playlist name (UTF-8)
/// @param guid Playlist GUID as string (format: "xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx")
/// @param track_ids Array of track MTP object IDs (atom_ids from zmdb)
/// @param track_count Number of tracks in the array
/// @return New playlist MTP object ID, or 0 on failure
ZUNE_WIRELESS_API uint32_t zune_device_create_playlist(
    zune_device_handle_t handle,
    const char* name,
    const char* guid,
    const uint32_t* track_ids,
    size_t track_count
);

/// Update playlist tracks (replaces entire track list)
/// @param handle Device handle from zune_device_create()
/// @param playlist_id MTP object ID of the playlist to update
/// @param track_ids Array of track MTP object IDs (atom_ids from zmdb)
/// @param track_count Number of tracks in the array
/// @return 0 on success, negative on error
ZUNE_WIRELESS_API int zune_device_update_playlist_tracks(
    zune_device_handle_t handle,
    uint32_t playlist_id,
    const uint32_t* track_ids,
    size_t track_count
);

/// Delete a playlist from the device
/// @param handle Device handle from zune_device_create()
/// @param playlist_id MTP object ID of the playlist to delete
/// @return 0 on success, negative on error
ZUNE_WIRELESS_API int zune_device_delete_playlist(
    zune_device_handle_t handle,
    uint32_t playlist_id
);

// Retrofit existing artist with MusicBrainz GUID
// Updates an existing artist on the device to include a MusicBrainz GUID.
// This operation:
//   1. Creates a new artist object with the GUID
//   2. Updates all albums and tracks to reference the new artist
//   3. Deletes the old artist object
//   4. Triggers device to auto-fetch metadata
//
// Parameters:
//   handle - Device handle from zune_device_create()
//   artist_name - Exact name of the artist to retrofit (must match existing artist)
//   guid - MusicBrainz GUID in UUID format (e.g., "7fb57fba-a6ef-44c2-abab-2fa3bdee607e")
// Returns:
//   0 on success, negative error code on failure
// Note:
//   - If artist already has a GUID, this is a no-op (returns 0)
//   - If artist doesn't exist on device, returns error
//   - All album and track references are automatically updated
ZUNE_WIRELESS_API int zune_device_retrofit_artist_guid(
    zune_device_handle_t handle,
    const char* artist_name,
    const char* guid
);

// Artist GUID mapping structure for batch retrofit operations
typedef struct {
    const char* artist_name;  // Artist name (must match existing artist on device)
    const char* guid;         // MusicBrainz GUID in UUID format
} ZuneArtistGuidMapping;

// Batch retrofit result structure
typedef struct {
    int retrofitted_count;       // Number of artists successfully retrofitted (modified)
    int already_had_guid_count;  // Number of artists that already had GUIDs (no changes)
    int not_found_count;         // Number of artists not found on device (will be created during upload)
    int error_count;             // Number of errors encountered
} ZuneBatchRetrofitResult;

// Batch Artist GUID Retrofit
//
// Retrofits multiple artists with MusicBrainz GUIDs in a single optimized operation.
// This is significantly faster than calling zune_device_retrofit_artist_guid() multiple
// times because it parses the device library only once and syncs only once.
//
// Parameters:
//   handle - Device handle from zune_device_create()
//   mappings - Array of artist name/GUID pairs
//   mapping_count - Number of mappings in the array
//   result - Output structure with detailed statistics (caller allocated)
//
// Returns:
//   0 on success (even if some artists fail - check result for details)
//   negative error code on fatal failure
//
// Performance: For N artists, this is approximately N times faster than individual retrofits
// because it performs only 1 library parse and 1 library sync instead of N of each.
//
// Example:
//   ZuneArtistGuidMapping mappings[] = {
//       {"Radiohead", "a74b1b7f-71a5-4011-9441-d0b5e4122711"},
//       {"The Beatles", "b10bbbfc-cf9e-42e0-be17-e2c3e1d2600d"}
//   };
//   ZuneBatchRetrofitResult result;
//   zune_device_retrofit_multiple_artist_guids(handle, mappings, 2, &result);
ZUNE_WIRELESS_API int zune_device_retrofit_multiple_artist_guids(
    zune_device_handle_t handle,
    const ZuneArtistGuidMapping* mappings,
    int mapping_count,
    ZuneBatchRetrofitResult* result
);

// Track User State (Play Count, Skip Count, Rating)
// Sets track user state metadata via MTP SetObjectPropValue operations.
// This updates the device's internal database (will be reflected in zmdb on next sync).
//
// Parameters:
//   handle        - Device handle from zune_device_create()
//   zmdb_atom_id  - The ZMDB atom_id of the track (from device library)
//   play_count    - DISABLED: Not supported via MTP, needs pcap investigation (-1 to skip)
//   skip_count    - DISABLED: Property not supported on Zune HD (-1 to skip)
//   rating        - Rating value: 0=unrated, 8=liked, 2=disliked (-1 to skip)
//
// Returns: 0 on success, negative error code on failure
//   -1: General MTP error
//   -2: Device not connected
//   -3: Invalid atom ID
//
// Note: Rating is implemented via MTP SetObjectProperty with UserRating (0xDC8A).
// Play count and skip count are ignored pending protocol analysis.
ZUNE_WIRELESS_API int zune_device_set_track_user_state(
    zune_device_handle_t handle,
    uint32_t zmdb_atom_id,
    int play_count,
    int skip_count,
    int rating
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
    ZUNE_ARTIST_METADATA_MODE_PROXY = 2,     // Forward to HTTP server
    ZUNE_ARTIST_METADATA_MODE_HYBRID = 3     // Try local files first, proxy if not found, cache responses
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

// ============================================================================
// Artist Metadata Hybrid Mode Callbacks (C# interop)
// ============================================================================

/// Path resolver callback - resolves MusicBrainz UUID to local file path
///
/// MEMORY CONTRACT:
/// - Returns: File path as C string (owned by C# caller), or NULL if not found
/// - The returned string must remain valid until the callback returns
/// - C++ will copy the string immediately during the callback
/// - C# is responsible for managing the string's lifetime and freeing it
/// - C++ will NOT call free() on the returned pointer
///
/// @param artist_uuid MusicBrainz UUID from HTTP request
/// @param endpoint_type Endpoint type (biography, images, overview, artwork, etc.)
/// @param resource_id Optional resource ID for specific images (may be NULL)
/// @param user_data User-provided context pointer
/// @return File path if found (C# manages memory), NULL if not found (will proxy)
typedef const char* (*zune_path_resolver_callback_t)(
    const char* artist_uuid,
    const char* endpoint_type,
    const char* resource_id,
    void* user_data
);

/// Cache storage callback - stores proxy response to local filesystem
/// @param artist_uuid MusicBrainz UUID from HTTP request
/// @param endpoint_type Endpoint type (biography, images, overview, artwork, etc.)
/// @param resource_id Optional resource ID for specific images
/// @param data Response data (XML or binary)
/// @param data_length Length of response data
/// @param content_type Content type (text/xml, image/jpeg, etc.)
/// @param user_data User-provided context pointer
/// @return true if cached successfully, false if should not cache
typedef bool (*zune_cache_storage_callback_t)(
    const char* artist_uuid,
    const char* endpoint_type,
    const char* resource_id,
    const void* data,
    size_t data_length,
    const char* content_type,
    void* user_data
);

/// Register path resolver callback for hybrid mode
/// @param handle Device handle
/// @param callback Path resolver callback function
/// @param user_data User-provided context pointer passed to callback
ZUNE_WIRELESS_API void zune_device_set_path_resolver_callback(
    zune_device_handle_t handle,
    zune_path_resolver_callback_t callback,
    void* user_data
);

/// Register cache storage callback for hybrid mode
/// @param handle Device handle
/// @param callback Cache storage callback function
/// @param user_data User-provided context pointer passed to callback
ZUNE_WIRELESS_API void zune_device_set_cache_storage_callback(
    zune_device_handle_t handle,
    zune_cache_storage_callback_t callback,
    void* user_data
);

// ============================================================================
// HTTP Network Operations
// ============================================================================

/// Initialize HTTP subsystem on device (MTP operations)
/// @param handle Device handle
/// @return true on success, false on failure
ZUNE_WIRELESS_API bool zune_device_initialize_http_subsystem(
    zune_device_handle_t handle
);

/// Trigger network mode (PPP/IPCP negotiation)
/// @param handle Device handle
/// @return true on success, false on failure (allows C# to retry)
ZUNE_WIRELESS_API bool zune_device_trigger_network_mode(
    zune_device_handle_t handle
);

/// Enable network polling (continuous 0x922d at 15ms intervals)
/// @param handle Device handle
ZUNE_WIRELESS_API void zune_device_enable_network_polling(
    zune_device_handle_t handle
);

/// Enable or disable verbose network logging
/// @param handle Device handle
/// @param enable true to enable verbose TCP/IP packet logging, false for errors only
ZUNE_WIRELESS_API void zune_device_set_verbose_network_logging(
    zune_device_handle_t handle,
    bool enable
);

// ============================================================================
// Low-Level MTP Primitives
// ============================================================================
// These primitives expose raw MTP operations for C# orchestration.
// They enable ID-based entity management and support for generic MTP devices.

/// MTP data type codes (for property list building)
typedef enum {
    ZUNE_MTP_TYPE_UINT8   = 0x0002,
    ZUNE_MTP_TYPE_UINT16  = 0x0004,
    ZUNE_MTP_TYPE_UINT32  = 0x0006,
    ZUNE_MTP_TYPE_UINT64  = 0x0008,
    ZUNE_MTP_TYPE_UINT128 = 0x000A,
    ZUNE_MTP_TYPE_STRING  = 0xFFFF
} ZuneMtpDataType;

/// MTP object format codes
typedef enum {
    ZUNE_FORMAT_UNDEFINED         = 0x3000,
    ZUNE_FORMAT_ASSOCIATION       = 0x3001,  // Folder/directory
    ZUNE_FORMAT_MP3               = 0x3009,
    ZUNE_FORMAT_WAV               = 0x3008,
    ZUNE_FORMAT_WMA               = 0xB901,
    ZUNE_FORMAT_AAC               = 0xB903,
    ZUNE_FORMAT_FLAC              = 0xB906,
    ZUNE_FORMAT_ARTIST            = 0xB218,  // Metadata artist object (with GUID)
    ZUNE_FORMAT_ABSTRACT_ALBUM    = 0xBA03,  // Album container
    ZUNE_FORMAT_AV_PLAYLIST       = 0xBA09   // Playlist container
} ZuneMtpObjectFormat;

/// MTP property codes commonly used for Zune
typedef enum {
    ZUNE_PROP_STORAGE_ID              = 0xDC01,
    ZUNE_PROP_OBJECT_FORMAT           = 0xDC02,
    ZUNE_PROP_OBJECT_SIZE             = 0xDC04,
    ZUNE_PROP_OBJECT_FILENAME         = 0xDC07,
    ZUNE_PROP_DATE_CREATED            = 0xDC08,
    ZUNE_PROP_DATE_MODIFIED           = 0xDC09,
    ZUNE_PROP_PARENT_OBJECT           = 0xDC0B,
    ZUNE_PROP_NAME                    = 0xDC44,
    ZUNE_PROP_ARTIST                  = 0xDC46,  // Artist name string (fallback)
    ZUNE_PROP_DATE_AUTHORED           = 0xDC47,  // Album year
    ZUNE_PROP_DESCRIPTION             = 0xDC48,
    ZUNE_PROP_REPRESENTATIVE_SAMPLE   = 0xDC86,  // Artwork data
    ZUNE_PROP_DURATION                = 0xDC89,  // Duration in ms
    ZUNE_PROP_USER_RATING             = 0xDC8A,  // Rating value
    ZUNE_PROP_TRACK                   = 0xDC8B,  // Track number
    ZUNE_PROP_GENRE                   = 0xDC8C,  // Genre string
    ZUNE_PROP_USE_COUNT               = 0xDC91,  // Play count
    ZUNE_PROP_SKIP_COUNT              = 0xDC92,
    ZUNE_PROP_ALBUM_NAME              = 0xDC9A,
    ZUNE_PROP_ALBUM_ARTIST            = 0xDC9B,
    ZUNE_PROP_CONTENT_TYPE_UUID       = 0xDA97,  // MusicBrainz GUID (Uint128)
    ZUNE_PROP_AUDIOBOOK_NAME          = 0xDA9A,
    ZUNE_PROP_ZUNE_COLLECTION_ID      = 0xDAB0,  // Always 0 for artist/playlist
    ZUNE_PROP_ARTIST_ID               = 0xDAB9,  // Artist MTP ObjectId reference
    ZUNE_PROP_ALBUM_ID                = 0xDABB
} ZuneMtpPropertyCode;

/// Property element for building MTP SendObjectPropList requests
typedef struct {
    uint32_t object_handle;     // Usually 0 for new objects
    uint16_t property_code;     // ZuneMtpPropertyCode
    uint16_t data_type;         // ZuneMtpDataType
    const void* value;          // Pointer to value data
    uint32_t value_size;        // Size of value data in bytes
} ZuneMtpProperty;

/// Result of SendObjectPropList operation
typedef struct {
    uint32_t object_id;         // Created object's MTP ObjectId
    uint32_t storage_id;        // Storage where object was created
    uint32_t parent_id;         // Parent object ID
    int status;                 // 0 = success, negative = error code
} ZuneMtpCreateResult;

/// Well-known folder IDs on Zune device
typedef struct {
    uint32_t artists_folder;    // Artists metadata folder
    uint32_t albums_folder;     // Albums metadata folder
    uint32_t music_folder;      // Music content folder (parent for artist subfolders)
    uint32_t playlists_folder;  // Playlists folder (may be 0 if not found)
    uint32_t storage_id;        // Default storage ID
} ZuneMtpFolderIds;

// --- Core MTP Operations ---

/// Send object property list (create metadata object)
/// @param handle Device handle
/// @param storage_id Storage ID (use 0 for default)
/// @param parent_id Parent folder ObjectId
/// @param format Object format (ZuneMtpObjectFormat)
/// @param object_size Size of following object data (0 for metadata-only objects)
/// @param properties Array of property elements
/// @param property_count Number of properties
/// @return Created object info with status
ZUNE_WIRELESS_API ZuneMtpCreateResult zune_mtp_send_object_prop_list(
    zune_device_handle_t handle,
    uint32_t storage_id,
    uint32_t parent_id,
    uint16_t format,
    uint64_t object_size,
    const ZuneMtpProperty* properties,
    uint32_t property_count
);

/// Send object data after SendObjectPropList
/// @param handle Device handle
/// @param data Object data (audio file bytes, or NULL for empty metadata objects)
/// @param size Data size (0 for empty object)
/// @return 0 on success, negative on error
ZUNE_WIRELESS_API int zune_mtp_send_object(
    zune_device_handle_t handle,
    const void* data,
    uint64_t size
);

/// Send object data from file path (streaming, avoids memory copy)
/// @param handle Device handle
/// @param file_path Path to file to upload
/// @return 0 on success, negative on error
ZUNE_WIRELESS_API int zune_mtp_send_object_from_file(
    zune_device_handle_t handle,
    const char* file_path
);

/// Set object references (link tracks to album/playlist)
/// @param handle Device handle
/// @param object_id Album or playlist ObjectId
/// @param ref_ids Array of track ObjectIds to link
/// @param ref_count Number of references
/// @return 0 on success, negative on error
ZUNE_WIRELESS_API int zune_mtp_set_object_references(
    zune_device_handle_t handle,
    uint32_t object_id,
    const uint32_t* ref_ids,
    uint32_t ref_count
);

/// Get object references (tracks in album/playlist)
/// @param handle Device handle
/// @param object_id Album or playlist ObjectId
/// @param out_ref_ids Output array (caller allocates)
/// @param max_refs Maximum refs to return
/// @param out_count Actual number of refs returned
/// @return 0 on success, negative on error
ZUNE_WIRELESS_API int zune_mtp_get_object_references(
    zune_device_handle_t handle,
    uint32_t object_id,
    uint32_t* out_ref_ids,
    uint32_t max_refs,
    uint32_t* out_count
);

/// Set single object property (string value)
/// @return 0 on success, negative on error
ZUNE_WIRELESS_API int zune_mtp_set_object_property_string(
    zune_device_handle_t handle,
    uint32_t object_id,
    uint16_t property_code,
    const char* value
);

/// Set single object property (integer value, any size up to 64-bit)
/// @return 0 on success, negative on error
ZUNE_WIRELESS_API int zune_mtp_set_object_property_int(
    zune_device_handle_t handle,
    uint32_t object_id,
    uint16_t property_code,
    uint64_t value
);

/// Set object property as byte array (for artwork)
/// @return 0 on success, negative on error
ZUNE_WIRELESS_API int zune_mtp_set_object_property_array(
    zune_device_handle_t handle,
    uint32_t object_id,
    uint16_t property_code,
    const void* data,
    uint32_t data_size
);

/// Get object handles matching criteria
/// @param handle Device handle
/// @param storage_id Storage ID (0 = all storages)
/// @param format Object format filter (0 = any)
/// @param parent_id Parent filter (0xFFFFFFFF = root/all)
/// @param out_handles Output array (caller allocates)
/// @param max_handles Maximum handles to return
/// @param out_count Actual count returned
/// @return 0 on success, negative on error
ZUNE_WIRELESS_API int zune_mtp_get_object_handles(
    zune_device_handle_t handle,
    uint32_t storage_id,
    uint16_t format,
    uint32_t parent_id,
    uint32_t* out_handles,
    uint32_t max_handles,
    uint32_t* out_count
);

/// Create directory/folder
/// @param handle Device handle
/// @param name Directory name
/// @param parent_id Parent folder ObjectId
/// @param storage_id Storage ID (0 for default)
/// @return ObjectId of created folder, or 0 on error
ZUNE_WIRELESS_API uint32_t zune_mtp_create_directory(
    zune_device_handle_t handle,
    const char* name,
    uint32_t parent_id,
    uint32_t storage_id
);

/// Get storage IDs
/// @param handle Device handle
/// @param out_storage_ids Output array (caller allocates)
/// @param max_storages Maximum storages to return
/// @return Number of storages, or negative on error
ZUNE_WIRELESS_API int zune_mtp_get_storage_ids(
    zune_device_handle_t handle,
    uint32_t* out_storage_ids,
    uint32_t max_storages
);

/// Get default storage ID
/// @return Storage ID, or 0 on error
ZUNE_WIRELESS_API uint32_t zune_mtp_get_default_storage(
    zune_device_handle_t handle
);

/// Get well-known folder IDs (Artists, Albums, Music, Playlists)
/// Call this once per session to get folder ObjectIds for creating content.
/// @param handle Device handle
/// @param out_folders Output structure with folder IDs
/// @return 0 on success, negative on error
ZUNE_WIRELESS_API int zune_mtp_get_well_known_folders(
    zune_device_handle_t handle,
    ZuneMtpFolderIds* out_folders
);

/// Delete an MTP object
/// @param handle Device handle
/// @param object_id ObjectId to delete
/// @return 0 on success, negative on error
ZUNE_WIRELESS_API int zune_mtp_delete_object(
    zune_device_handle_t handle,
    uint32_t object_id
);

// --- Zune Vendor Operations ---

/// Zune DB sync operation (Operation 0x9217)
/// Triggers device to sync its internal database after uploads.
/// @param handle Device handle
/// @param param Usually 1
/// @return 0 on success, negative on error
ZUNE_WIRELESS_API int zune_mtp_operation_9217(
    zune_device_handle_t handle,
    uint32_t param
);

/// Zune track context registration (Operation 0x922A)
/// Registers a track for metadata retrieval.
/// @param handle Device handle
/// @param track_name Track name to register
/// @return 0 on success, negative on error
ZUNE_WIRELESS_API int zune_mtp_operation_922a(
    zune_device_handle_t handle,
    const char* track_name
);

/// Zune property query (Operation 0x9802)
/// Query property description for Windows compatibility.
/// @param handle Device handle
/// @param property_code Property to query
/// @param format_type Object format type
/// @return 0 on success, negative on error
ZUNE_WIRELESS_API int zune_mtp_operation_9802(
    zune_device_handle_t handle,
    uint16_t property_code,
    uint16_t format_type
);


#ifdef __cplusplus
}
#endif

#endif // ZUNE_WIRELESS_API_H
