#pragma once

#include <string>
#include <vector>
#include <functional>
#include <memory>
#include <mutex>
#include <unordered_map>

#include <mtp/ptp/Device.h>
#include <mtp/metadata/Library.h>
#include <usb/Context.h>
#include <cli/Session.h>
#include "zune_wireless/zune_wireless_api.h"
#include "ZuneTypes.h"
#include "ZuneDeviceIdentification.h"


// Forward declare Library for optional usage
namespace mtp {
    class Library;
    DECLARE_PTR(Library);
}

// Forward declarations
namespace zmdb {
    struct ZMDBAlbum;
}

class ZuneHTTPInterceptor;
struct InterceptorConfig;
class NetworkManager;
class LibraryManager;



// USB handles structure for raw monitoring (includes pre-discovered endpoints)


// Internal C++ struct for file info


using namespace mtp;

// Forward declarations
namespace mtp {
    class Device;
    class Session;
    DECLARE_PTR(Device);
    DECLARE_PTR(Session);
}

namespace cli {
    class Session;
    DECLARE_PTR(Session);
}

class ZuneDevice {
public:
    ZuneDevice();
    ~ZuneDevice();

    // --- Connection Management ---
    bool ConnectUSB();
    bool ConnectWireless(const std::string& ip_address);
    void Disconnect();
    bool IsConnected();
    bool ValidateConnection();  // Lightweight MTP operation to verify session is still valid

    // --- Pairing ---
    int EstablishSyncPairing(const std::string& device_name = "");
    std::string EstablishWirelessPairing(const std::string& ssid, const std::string& password);
    int DisableWireless();

    // --- Device Management ---
    // Erases all content on the device (performs MTP FormatStore operation)
    // WARNING: This will delete all music, playlists, and other content on the device
    // Returns 0 on success, -1 if not connected, -2 on MTP error
    int EraseAllContent();

    // --- Sync Partnership ---
    // Returns the device's sync partner GUID (MTP property 0xd401)
    // Returns empty string on error, "{00000000-0000-0000-0000-000000000000}" if not paired
    std::string GetSyncPartnerGuid();
    // Sets the device's friendly name (MTP property 0xd402)
    // Returns 0 on success, -1 on invalid input, -2 if not connected, -3 on MTP error
    int SetDeviceName(const std::string& name);
    const char* GetSyncPartnerGuidCached();

    // --- Device Info ---
    std::string GetName();
    std::string GetSerialNumber();

    // --- Storage ---
    // Returns total storage capacity in bytes (from MTP StorageInfo)
    uint64_t GetStorageCapacityBytes();
    // Returns free storage space in bytes (from MTP StorageInfo)
    uint64_t GetStorageFreeBytes();

    // --- Device Identification (from MTP property 0xd21a) ---
    // Returns the device family enum (Keel, Scorpius, Draco, Pavo)
    zune::DeviceFamily GetDeviceFamily();
    // Returns the raw color ID from 0xd21a byte 0
    uint8_t GetDeviceColorId();
    // Returns the color name (e.g., "Brown", "Platinum", "BlackBlack")
    std::string GetDeviceColorName();
    // Returns the family codename (e.g., "Keel", "Pavo")
    std::string GetDeviceFamilyName();
    // C API cached versions - pointer valid until next call or device destruction
    const char* GetDeviceFamilyNameCached();
    const char* GetDeviceColorNameCached();

    // --- Device Capabilities ---
    // Returns true if device supports network mode (HTTP-based artist metadata proxy).
    // Only Pavo family (Zune HD) devices have this capability.
    bool SupportsNetworkMode();

    // C API helpers - return cached strings (pointer valid until next call or device destruction)
    const char* GetNameCached();
    const char* GetSerialNumberCached();
    const char* GetSessionGuidCached();

    // --- Discovery ---
    std::vector<std::string> ScanWiFiNetworks();

    // --- File System Operations ---
    std::vector<ZuneObjectInfoInternal> ListStorage(uint32_t parent_handle = 0);
    ZuneMusicLibrary* GetMusicLibrary();  // Fast: Returns flat data (tracks, albums, artworks) using zmdb
    ZuneMusicLibrary* GetMusicLibrarySlow();  // Slow: For testing only (uses AFTL enumeration)
    std::vector<ZunePlaylistInfo> GetPlaylists();
    int DownloadFile(uint32_t object_handle, const std::string& destination_path);
    int UploadFile(const std::string& source_path, const std::string& destination_folder);
    int DeleteFile(uint32_t object_handle);

    // DEPRECATED: artwork_path parameter is not used. Use UploadTrackWithMetadata instead.
    // This function currently ignores artwork_path and only uploads the media file.
    int UploadWithArtwork(const std::string& media_path, const std::string& artwork_path);

    // --- Playlist Management ---
    // Create a playlist on the device
    // Returns new playlist MTP object ID, or 0 on failure
    uint32_t CreatePlaylist(
        const std::string& name,
        const std::string& guid,  // GUID as string "xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx"
        const std::vector<uint32_t>& track_mtp_ids
    );

    // Update playlist track list (replaces all tracks)
    bool UpdatePlaylistTracks(
        uint32_t playlist_mtp_id,
        const std::vector<uint32_t>& track_mtp_ids
    );

    // Delete a playlist from the device
    bool DeletePlaylist(uint32_t playlist_mtp_id);

    // --- Upload with Metadata (uses Library for proper MTP structure) ---
    // For Music: artist_name = artist, album_name = album
    // For Audiobook: artist_name = author, album_name = audiobook title
    // rating: -1 = unrated, 8 = liked, 3 = disliked (Zune format, set during upload)
    int UploadTrackWithMetadata(
        MediaType media_type,
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
        uint32_t duration_ms = 0,
        int rating = -1,
        uint32_t* out_track_id = nullptr,
        uint32_t* out_album_id = nullptr,
        uint32_t* out_artist_id = nullptr
    );

    // --- Artist GUID Retrofit (for existing artists without GUID) ---
    // Deletes existing artist and recreates with GUID, updating all album/track references
    // Device will automatically trigger metadata fetch after artist is recreated
    // Returns 0 on success, -1 on failure
    int RetrofitArtistGuid(
        const std::string& artist_name,
        const std::string& guid
    );

    // Batch retrofit result structure
    struct BatchRetrofitResult {
        int retrofitted_count;       // Artists actually modified (recreated with GUID)
        int already_had_guid_count;  // Artists that already had GUIDs (no changes)
        int not_found_count;         // Artists that don't exist on device yet
        int error_count;             // Actual errors during retrofit
    };

    // Artist GUID mapping structure for batch operations
    struct ArtistGuidMapping {
        std::string artist_name;
        std::string guid;
    };

    // Batch retrofit multiple artists efficiently (single library parse, single sync)
    // Processes multiple artists in one call - optimal for batch uploads
    // Returns detailed statistics about the retrofit operation
    BatchRetrofitResult RetrofitMultipleArtistGuids(
        const std::vector<ArtistGuidMapping>& mappings
    );

    // --- Streaming/Partial Downloads ---
    mtp::ByteArray GetPartialObject(uint32_t object_id, uint64_t offset, uint32_t size);
    uint64_t GetObjectSize(uint32_t object_id);
    std::string GetObjectFilename(uint32_t object_id);

    // --- Track Lookup ---
    // Query MTP for a specific audio track's ObjectId by title/filename within an album
    // Results are cached to avoid repeated MTP queries
    // Returns 0 if not found
    uint32_t GetAudioTrackObjectId(const std::string& track_title, uint32_t album_object_id);

    // Clear the track ObjectId cache (call when device library changes)
    void ClearTrackObjectIdCache();

    // --- Track User State (Play Count, Skip Count, Rating) ---
    // Sets track user state metadata via MTP SetObjectPropValue operations.
    // This updates the device's internal database (will be reflected in zmdb on next sync).
    //
    // Parameters:
    //   zmdb_atom_id  - The ZMDB atom_id of the track (from device library)
    //   play_count    - DISABLED: Not supported via MTP, needs pcap investigation
    //   skip_count    - DISABLED: Property not supported on Zune HD
    //   rating        - Rating value: 0=unrated, 8=liked, 2=disliked (-1 to skip)
    //
    // Returns: 0 on success, negative error code on failure
    //   -1: General MTP error
    //   -2: Device not connected
    //   -3: Invalid atom ID
    //
    // Note: Only rating is currently implemented using Zune vendor operations.
    // Play count and skip count are ignored pending protocol analysis.
    int SetTrackUserState(uint32_t zmdb_atom_id, int play_count, int skip_count, int rating);

    // --- Metadata Retrieval ---
    mtp::ByteArray GetZuneMetadata(const std::vector<uint8_t>& object_id);

    // --- Callbacks ---
    using LogCallback = std::function<void(const std::string& message)>;
    void SetLogCallback(LogCallback callback);

    // --- Artist Metadata HTTP Interception ---
    bool InitializeHTTPSubsystem();  // Must be called before StartHTTPInterceptor
    void StartHTTPInterceptor(const InterceptorConfig& config);
    void StopHTTPInterceptor();
    bool IsHTTPInterceptorRunning() const;
    InterceptorConfig GetHTTPInterceptorConfig() const;
    void TriggerNetworkMode();  // Send 0x922c(3,3) to initiate PPP/HTTP after track upload
    void EnableNetworkPolling();  // Enable polling flag - call AFTER TriggerNetworkMode()
    int PollNetworkData(int timeout_ms);  // Single poll cycle - called from C# in a loop
    void SetVerboseNetworkLogging(bool enable);  // Enable/disable verbose TCP/IP packet logging

    // Callback registration for hybrid mode
    using PathResolverCallback = const char* (*)(const char* artist_uuid, const char* endpoint_type, const char* resource_id, void* user_data);
    using CacheStorageCallback = bool (*)(const char* artist_uuid, const char* endpoint_type, const char* resource_id, const void* data, size_t data_length, const char* content_type, void* user_data);
    void SetPathResolverCallback(PathResolverCallback callback, void* user_data);
    void SetCacheStorageCallback(CacheStorageCallback callback, void* user_data);

    // --- Raw USB Access (for monitoring without MTP session) ---
    // Extracts USB device/interface/endpoints from MTP session for raw monitoring
    // Call this BEFORE Disconnect() - discovers endpoints while interface is still claimed
    // The handles and endpoints remain valid after MTP closes
    USBHandlesWithEndpoints ExtractUSBHandles();

    // --- Network Session State & Control ---
    // Reads device networking state via Op922f (ReadZmdbState).
    // Returns 4 values from the 1,036-byte response.
    bool ReadNetworkState(int32_t& active, int32_t& progress, int32_t& phase, int32_t& status);

    // Tears down the active network session (Op9230(2) END + Op922b(3,2,0) close).
    bool TeardownNetworkSession();

    // Re-enables trusted files via full MTPZ re-handshake
    // (Op9216 + Op9212/Op9213 + Op9214). The device invalidates
    // CMAC key material after DisableTrustedFiles, so a bare
    // SetSessionGUID won't work — full re-authentication is required.
    bool EnableTrustedFiles();

    // Disables trusted files via TrustedApp (Op9215).
    bool DisableTrustedFiles();

    // --- Low-Level MTP Access (for C# orchestration) ---
    // Returns the underlying MTP session for direct MTP primitive operations.
    // Returns nullptr if not connected.
    mtp::SessionPtr GetMtpSession() const { return mtp_session_; }

    // Get default storage ID (first storage or cached value)
    uint32_t GetDefaultStorageId();

    // Get well-known folder IDs for device content organization
    struct FolderIds {
        uint32_t artists_folder = 0;
        uint32_t albums_folder = 0;
        uint32_t music_folder = 0;
        uint32_t playlists_folder = 0;
        uint32_t storage_id = 0;
        bool artist_format_supported = false;
    };
    FolderIds GetWellKnownFolders();

private:
    // --- Internal Helper Methods ---
    bool LoadMacGuid();
    bool SaveSessionGuidBinary(const mtp::ByteArray& guid_data);
    bool LoadSessionGuid();
    mtp::ByteArray HexToBytes(const std::string& hex_str);
    mtp::ByteArray LoadPropertyFromFile(const std::string& filename);
    std::string Utf16leToAscii(const mtp::ByteArray& data, bool is_guid = false);
    void Log(const std::string& message);
    void VerboseLog(const std::string& message);  // Log only if verbose_logging_ is true



    // --- Member Variables ---
    std::string guid_file_;
    std::string device_guid_file_;
    std::string mac_guid_;
    std::string session_guid_;

    usb::ContextPtr usb_context_;
    usb::DeviceDescriptorPtr usb_descriptor_;
    mtp::DevicePtr device_;
    mtp::SessionPtr mtp_session_;
    cli::SessionPtr cli_session_;


    LogCallback log_callback_;
    bool verbose_logging_ = true;  // Verbose network logging enabled by default

    // Cached device info strings (handle-scoped, thread-safe)
    mutable std::string cached_name_;
    mutable std::string cached_serial_number_;
    mutable std::string cached_session_guid_;
    mutable std::string cached_sync_partner_guid_;
    mutable std::string cached_family_name_;
    mutable std::string cached_color_name_;
    mutable std::mutex cache_mutex_;

    // Device identification cache (from MTP property 0xd21a)
    mutable zune::DeviceIdentification cached_device_ident_;
    mutable bool device_ident_cached_ = false;
    void CacheDeviceIdentification() const;

    // Network Manager
    std::unique_ptr<NetworkManager> network_manager_;

    // Library Manager
    std::unique_ptr<LibraryManager> library_manager_;


};
