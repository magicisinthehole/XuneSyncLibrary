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

    // --- Device Info ---
    std::string GetName();
    std::string GetSerialNumber();
    std::string GetModel();

    // --- Device Capabilities ---
    // Returns true if device supports network mode (HTTP-based artist metadata proxy).
    // Only Zune HD devices (USB Product ID 0x063e) have this capability.
    bool SupportsNetworkMode();

    // C API helpers - return cached strings (pointer valid until next call or device destruction)
    const char* GetNameCached();
    const char* GetSerialNumberCached();
    const char* GetModelCached();
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
    void EnableNetworkPolling();  // Start 0x922d polling - call AFTER TriggerNetworkMode()
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
    mutable std::string cached_model_;
    mutable std::string cached_session_guid_;
    mutable std::mutex cache_mutex_;

    // Network Manager
    std::unique_ptr<NetworkManager> network_manager_;

    // Library Manager
    std::unique_ptr<LibraryManager> library_manager_;


};
