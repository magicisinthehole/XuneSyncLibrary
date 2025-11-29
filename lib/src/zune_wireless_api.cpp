#include "zune_wireless/zune_wireless_api.h"
#include "ZuneDevice.h"
#include "protocols/http/ZuneHTTPInterceptor.h"
#include "ssdp_discovery.h"
#include <vector>
#include <cstring>
#include <mtp/ptp/Device.h>
#include <usb/Context.h>

static std::unique_ptr<ssdp::SSDPDiscovery> g_discovery;

extern "C" {

ZUNE_WIRELESS_API zune_device_handle_t zune_device_create() {
    return new ZuneDevice();
}

ZUNE_WIRELESS_API void zune_device_destroy(zune_device_handle_t handle) {
    delete static_cast<ZuneDevice*>(handle);
}

ZUNE_WIRELESS_API bool zune_device_connect_usb(zune_device_handle_t handle) {
    if (handle) {
        return static_cast<ZuneDevice*>(handle)->ConnectUSB();
    }
    return false;
}

ZUNE_WIRELESS_API void zune_device_disconnect(zune_device_handle_t handle) {
    if (handle) {
        static_cast<ZuneDevice*>(handle)->Disconnect();
    }
}

ZUNE_WIRELESS_API void zune_device_set_log_callback(zune_device_handle_t handle, log_callback_t callback) {
    if (handle) {
        static_cast<ZuneDevice*>(handle)->SetLogCallback([callback](const std::string& message) {
            callback(message.c_str());
        });
    }
}

ZUNE_WIRELESS_API int zune_device_establish_sync_pairing(zune_device_handle_t handle, const char* device_name) {
    if (handle) {
        return static_cast<ZuneDevice*>(handle)->EstablishSyncPairing(device_name ? device_name : "");
    }
    return -1;
}

ZUNE_WIRELESS_API const char* zune_device_get_name(zune_device_handle_t handle) {
    if (handle) {
        return static_cast<ZuneDevice*>(handle)->GetNameCached();
    }
    return nullptr;
}

ZUNE_WIRELESS_API const char* zune_device_get_serial_number(zune_device_handle_t handle) {
    if (handle) {
        return static_cast<ZuneDevice*>(handle)->GetSerialNumberCached();
    }
    return nullptr;
}

ZUNE_WIRELESS_API const char* zune_device_get_model(zune_device_handle_t handle) {
    if (handle) {
        return static_cast<ZuneDevice*>(handle)->GetModelCached();
    }
    return nullptr;
}

ZUNE_WIRELESS_API const char* zune_device_establish_wireless_pairing(zune_device_handle_t handle, const char* ssid, const char* password) {
    if (handle) {
        auto* device = static_cast<ZuneDevice*>(handle);
        // EstablishWirelessPairing returns a std::string, cache it and return pointer
        std::string result = device->EstablishWirelessPairing(ssid, password);
        // Note: The session_guid is stored internally in the device, so we can return the cached version
        return device->GetSessionGuidCached();
    }
    return nullptr;
}

ZUNE_WIRELESS_API int zune_device_disable_wireless(zune_device_handle_t handle) {
    if (handle) {
        return static_cast<ZuneDevice*>(handle)->DisableWireless();
    }
    return -1;
}

ZUNE_WIRELESS_API ZuneObjectInfo* zune_device_list_storage(zune_device_handle_t handle, uint32_t parent_handle, uint32_t* count) {
    if (!handle) {
        *count = 0;
        return nullptr;
    }
    auto* device = static_cast<ZuneDevice*>(handle);
    auto objects = device->ListStorage(parent_handle);
    *count = objects.size();
    auto* c_objects = new ZuneObjectInfo[objects.size()];
    for (size_t i = 0; i < objects.size(); ++i) {
        c_objects[i].handle = objects[i].handle;
        c_objects[i].filename = strdup(objects[i].filename.c_str());
        c_objects[i].size = objects[i].size;
        c_objects[i].is_folder = objects[i].is_folder;
    }
    return c_objects;
}

ZUNE_WIRELESS_API void zune_device_free_object_info_array(ZuneObjectInfo* array, uint32_t count) {
    if (!array) return;
    for (uint32_t i = 0; i < count; ++i) {
        free((void*)array[i].filename);
    }
    delete[] array;
}

ZUNE_WIRELESS_API int zune_device_download_file(zune_device_handle_t handle, uint32_t object_handle, const char* destination_path) {
    if (handle) {
        return static_cast<ZuneDevice*>(handle)->DownloadFile(object_handle, destination_path);
    }
    return -1;
}

ZUNE_WIRELESS_API int zune_device_upload_file(zune_device_handle_t handle, const char* source_path, const char* destination_folder) {
    if (handle) {
        return static_cast<ZuneDevice*>(handle)->UploadFile(source_path, destination_folder);
    }
    return -1;
}

ZUNE_WIRELESS_API int zune_device_delete_file(zune_device_handle_t handle, uint32_t object_handle) {
    if (handle) {
        return static_cast<ZuneDevice*>(handle)->DeleteFile(object_handle);
    }
    return -1;
}

ZUNE_WIRELESS_API int zune_device_upload_with_artwork(zune_device_handle_t handle, const char* media_path, const char* artwork_path) {
    if (handle) {
        return static_cast<ZuneDevice*>(handle)->UploadWithArtwork(media_path, artwork_path);
    }
    return -1;
}

ZUNE_WIRELESS_API ZuneMusicLibrary* zune_device_get_music_library(zune_device_handle_t handle) {
    if (!handle) {
        return nullptr;
    }
    auto* device = static_cast<ZuneDevice*>(handle);
    return device->GetMusicLibrary();
}

ZUNE_WIRELESS_API void zune_device_free_music_library(ZuneMusicLibrary* library) {
    if (!library) return;

    // Free tracks
    for (uint32_t i = 0; i < library->track_count; ++i) {
        free((void*)library->tracks[i].title);
        free((void*)library->tracks[i].artist_name);
        free((void*)library->tracks[i].artist_guid);
        free((void*)library->tracks[i].album_name);
        free((void*)library->tracks[i].album_artist_name);
        free((void*)library->tracks[i].album_artist_guid);
        free((void*)library->tracks[i].genre);
        free((void*)library->tracks[i].filename);
    }
    delete[] library->tracks;

    // Free albums
    for (uint32_t i = 0; i < library->album_count; ++i) {
        free((void*)library->albums[i].title);
        free((void*)library->albums[i].artist_name);
        free((void*)library->albums[i].artist_guid);
        free((void*)library->albums[i].alb_reference);
    }
    delete[] library->albums;

    // Free artworks
    for (uint32_t i = 0; i < library->artwork_count; ++i) {
        free((void*)library->artworks[i].alb_reference);
    }
    delete[] library->artworks;

    delete library;
}

ZUNE_WIRELESS_API ZunePlaylistInfo* zune_device_get_playlists(zune_device_handle_t handle, uint32_t* count) {
    if (!handle) {
        *count = 0;
        return nullptr;
    }
    auto* device = static_cast<ZuneDevice*>(handle);
    auto playlists = device->GetPlaylists();
    *count = playlists.size();
    auto* c_playlists = new ZunePlaylistInfo[playlists.size()];
    for (size_t i = 0; i < playlists.size(); ++i) {
        c_playlists[i] = playlists[i];
    }
    return c_playlists;
}

ZUNE_WIRELESS_API void zune_device_free_playlists(ZunePlaylistInfo* playlists, uint32_t count) {
    if (!playlists) return;
    for (uint32_t i = 0; i < count; ++i) {
        free((void*)playlists[i].Name);
        for (uint32_t j = 0; j < playlists[i].TrackCount; ++j) {
            free((void*)playlists[i].TrackPaths[j]);
        }
        delete[] playlists[i].TrackPaths;
    }
    delete[] playlists;
}

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
    uint32_t duration_ms
) {
    ZuneUploadResult result = {0, 0, 0, -1};  // Initialize with failure status

    if (handle) {
        result.status = static_cast<ZuneDevice*>(handle)->UploadTrackWithMetadata(
            MediaType::Music,
            audio_file_path ? audio_file_path : "",
            artist_name ? artist_name : "",
            album_name ? album_name : "",
            album_year,
            track_title ? track_title : "",
            genre ? genre : "",
            track_number,
            artwork_data,
            artwork_size,
            artist_guid ? artist_guid : "",
            duration_ms,
            &result.track_object_id,
            &result.album_object_id,
            &result.artist_object_id
        );
    }

    return result;
}

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
) {
    ZuneUploadResult result = {0, 0, 0, -1};  // Initialize with failure status

    if (handle) {
        result.status = static_cast<ZuneDevice*>(handle)->UploadTrackWithMetadata(
            MediaType::Audiobook,
            audio_file_path ? audio_file_path : "",
            author_name ? author_name : "",
            audiobook_name ? audiobook_name : "",
            release_year,
            track_title ? track_title : "",
            "",  // genre (not used for audiobooks)
            track_number,
            artwork_data,
            artwork_size,
            "",  // artist_guid (not used for audiobooks)
            duration_ms,
            &result.track_object_id,
            &result.album_object_id,
            &result.artist_object_id
        );
    }

    return result;
}

ZUNE_WIRELESS_API int zune_device_get_partial_object(
    zune_device_handle_t handle,
    uint32_t object_id,
    uint64_t offset,
    uint32_t size,
    uint8_t** out_data,
    uint32_t* out_size
) {
    if (!handle || !out_data || !out_size) {
        return -1;
    }

    try {
        auto* device = static_cast<ZuneDevice*>(handle);
        auto data = device->GetPartialObject(object_id, offset, size);

        if (data.empty()) {
            *out_data = nullptr;
            *out_size = 0;
            return 0;
        }

        // Allocate memory for C caller
        *out_data = new uint8_t[data.size()];
        std::copy(data.begin(), data.end(), *out_data);
        *out_size = static_cast<uint32_t>(data.size());
        return 0;
    } catch (...) {
        return -1;
    }
}

ZUNE_WIRELESS_API void zune_device_free_partial_data(uint8_t* data) {
    if (data) {
        delete[] data;
    }
}

ZUNE_WIRELESS_API uint64_t zune_device_get_object_size(zune_device_handle_t handle, uint32_t object_id) {
    if (handle) {
        return static_cast<ZuneDevice*>(handle)->GetObjectSize(object_id);
    }
    return 0;
}

ZUNE_WIRELESS_API const char* zune_device_get_object_filename(zune_device_handle_t handle, uint32_t object_id) {
    if (handle) {
        static thread_local std::string filename;
        filename = static_cast<ZuneDevice*>(handle)->GetObjectFilename(object_id);
        return filename.c_str();
    }
    return "";
}

ZUNE_WIRELESS_API uint32_t zune_device_get_audio_track_object_id(zune_device_handle_t handle, const char* track_title, uint32_t album_object_id) {
    if (handle && track_title) {
        return static_cast<ZuneDevice*>(handle)->GetAudioTrackObjectId(track_title, album_object_id);
    }
    return 0;
}

ZUNE_WIRELESS_API int zune_device_retrofit_artist_guid(
    zune_device_handle_t handle,
    const char* artist_name,
    const char* guid
) {
    if (!handle) {
        return -1;
    }

    if (!artist_name || !guid) {
        return -1;
    }

    try {
        return static_cast<ZuneDevice*>(handle)->RetrofitArtistGuid(artist_name, guid);
    } catch (const std::exception& e) {
        // Log error if log callback is available
        // For now, return error code
        return -1;
    }
}

ZUNE_WIRELESS_API int zune_device_retrofit_multiple_artist_guids(
    zune_device_handle_t handle,
    const ZuneArtistGuidMapping* mappings,
    int mapping_count,
    ZuneBatchRetrofitResult* result
) {
    // Initialize result
    if (result) {
        result->retrofitted_count = 0;
        result->already_had_guid_count = 0;
        result->not_found_count = 0;
        result->error_count = 0;
    }

    if (!handle || !mappings || !result) {
        if (result) {
            result->error_count = mapping_count;
        }
        return -1;
    }

    if (mapping_count <= 0) {
        return 0;  // No mappings, success
    }

    try {
        // Convert C array to C++ vector
        std::vector<ZuneDevice::ArtistGuidMapping> cpp_mappings;
        cpp_mappings.reserve(mapping_count);

        for (int i = 0; i < mapping_count; i++) {
            if (mappings[i].artist_name && mappings[i].guid) {
                cpp_mappings.push_back({
                    std::string(mappings[i].artist_name),
                    std::string(mappings[i].guid)
                });
            } else {
                // Invalid mapping - count as error
                result->error_count++;
            }
        }

        // Call C++ batch retrofit method
        auto cpp_result = static_cast<ZuneDevice*>(handle)->RetrofitMultipleArtistGuids(cpp_mappings);

        // Copy results
        result->retrofitted_count = cpp_result.retrofitted_count;
        result->already_had_guid_count = cpp_result.already_had_guid_count;
        result->not_found_count = cpp_result.not_found_count;
        result->error_count += cpp_result.error_count;  // Add to existing errors from invalid mappings

        return 0;

    } catch (const std::exception& e) {
        // Fatal error - mark all as errors
        if (result) {
            result->error_count = mapping_count;
        }
        return -1;
    }
}

ZUNE_WIRELESS_API void zune_ssdp_start_discovery(device_discovered_callback_t callback) {
    if (!g_discovery) {
        g_discovery = std::make_unique<ssdp::SSDPDiscovery>();
    }
    g_discovery->start_background([callback](const ssdp::ZuneDevice& device, bool is_new) {
        if (is_new && callback) {
            callback(device.ip_address.c_str(), device.uuid.c_str());
        }
    });
}

ZUNE_WIRELESS_API void zune_ssdp_stop_discovery() {
    if (g_discovery) {
        g_discovery->stop();
        g_discovery.reset();
    }
}

ZUNE_WIRELESS_API bool zune_device_find_on_usb(const char** uuid, const char** device_name) {
    try {
        mtp::usb::ContextPtr ctx = std::make_shared<mtp::usb::Context>();
        mtp::DevicePtr device = mtp::Device::FindFirst(ctx, "Zune", true, false);
        if (device) {
            // Get device info from USB descriptor - NO MTP SESSION REQUIRED
            // This matches Windows Zune behavior: enumerate USB devices first,
            // then open session only once for all operations
            auto info = device->GetInfo();

            thread_local std::string serial;
            thread_local std::string model;

            serial = info.SerialNumber;
            model = info.Model;  // e.g., "Zune HD", "Zune 120"

            *uuid = serial.c_str();
            *device_name = model.c_str();  // USB model name, not device friendly name

            // Note: Device friendly name (property 0xd402, e.g., "Andy's Zune HD")
            // should be retrieved AFTER opening the main session using zune_device_get_name()

            return true;
        }
    } catch (const std::exception& e) {
        // Log the error?
    }
    return false;
}

// ============================================================================
// Artist Metadata HTTP Interception API
// ============================================================================

int zune_device_start_artist_metadata_interceptor(
    zune_device_handle_t handle,
    const ZuneArtistMetadataConfig* config
) {
    if (!handle || !config) return -1;

    auto* device = static_cast<ZuneDevice*>(handle);

    try {
        // Convert C config to C++ config
        InterceptorConfig cpp_config;
        cpp_config.mode = static_cast<InterceptionMode>(config->mode);

        if (config->mode == ZUNE_ARTIST_METADATA_MODE_STATIC) {
            cpp_config.static_config.data_directory =
                config->static_data_directory ? config->static_data_directory : "./artist_data";
        }
        else if (config->mode == ZUNE_ARTIST_METADATA_MODE_PROXY) {
            cpp_config.proxy_config.catalog_server =
                config->proxy_catalog_server ? config->proxy_catalog_server : "";
            cpp_config.proxy_config.image_server =
                config->proxy_image_server ? config->proxy_image_server : cpp_config.proxy_config.catalog_server;
            cpp_config.proxy_config.art_server =
                config->proxy_art_server ? config->proxy_art_server : cpp_config.proxy_config.catalog_server;
            cpp_config.proxy_config.mix_server =
                config->proxy_mix_server ? config->proxy_mix_server : cpp_config.proxy_config.catalog_server;
            cpp_config.proxy_config.timeout_ms =
                config->proxy_timeout_ms > 0 ? config->proxy_timeout_ms : 5000;
        }
        else if (config->mode == ZUNE_ARTIST_METADATA_MODE_HYBRID) {
            // Hybrid mode requires proxy configuration (for fallback)
            cpp_config.proxy_config.catalog_server =
                config->proxy_catalog_server ? config->proxy_catalog_server : "";
            cpp_config.proxy_config.image_server =
                config->proxy_image_server ? config->proxy_image_server : cpp_config.proxy_config.catalog_server;
            cpp_config.proxy_config.art_server =
                config->proxy_art_server ? config->proxy_art_server : cpp_config.proxy_config.catalog_server;
            cpp_config.proxy_config.mix_server =
                config->proxy_mix_server ? config->proxy_mix_server : cpp_config.proxy_config.catalog_server;
            cpp_config.proxy_config.timeout_ms =
                config->proxy_timeout_ms > 0 ? config->proxy_timeout_ms : 5000;
        }

        // Set server IP for DNS resolution
        if (config->server_ip) {
            cpp_config.server_ip = config->server_ip;
        }

        device->StartHTTPInterceptor(cpp_config);
        return 0;
    }
    catch (const std::exception& e) {
        // Log error (would need log callback)
        return -1;
    }
}

void zune_device_stop_artist_metadata_interceptor(zune_device_handle_t handle) {
    if (!handle) return;
    auto* device = static_cast<ZuneDevice*>(handle);
    device->StopHTTPInterceptor();
}

bool zune_device_is_artist_metadata_interceptor_running(zune_device_handle_t handle) {
    if (!handle) return false;
    auto* device = static_cast<ZuneDevice*>(handle);
    return device->IsHTTPInterceptorRunning();
}

int zune_device_get_artist_metadata_config(
    zune_device_handle_t handle,
    ZuneArtistMetadataConfig* config
) {
    if (!handle || !config) return -1;

    auto* device = static_cast<ZuneDevice*>(handle);

    try {
        InterceptorConfig cpp_config = device->GetHTTPInterceptorConfig();

        config->mode = static_cast<ZuneArtistMetadataMode>(cpp_config.mode);

        // Note: These are pointers to internal strings, valid until device reconfigured
        config->static_data_directory = cpp_config.static_config.data_directory.c_str();
        config->proxy_catalog_server = cpp_config.proxy_config.catalog_server.c_str();
        config->proxy_image_server = cpp_config.proxy_config.image_server.c_str();
        config->proxy_art_server = cpp_config.proxy_config.art_server.c_str();
        config->proxy_mix_server = cpp_config.proxy_config.mix_server.c_str();
        config->proxy_timeout_ms = cpp_config.proxy_config.timeout_ms;

        return 0;
    }
    catch (const std::exception& e) {
        return -1;
    }
}

ZuneArtistMetadataConfig zune_artist_metadata_config_static(const char* data_directory) {
    ZuneArtistMetadataConfig config = {};
    config.mode = ZUNE_ARTIST_METADATA_MODE_STATIC;
    config.static_data_directory = strdup(data_directory);
    return config;
}

ZuneArtistMetadataConfig zune_artist_metadata_config_proxy(const char* server_base_url) {
    ZuneArtistMetadataConfig config = {};
    config.mode = ZUNE_ARTIST_METADATA_MODE_PROXY;
    config.proxy_catalog_server = strdup(server_base_url);
    config.proxy_timeout_ms = 5000;
    return config;
}

void zune_artist_metadata_config_free(ZuneArtistMetadataConfig* config) {
    if (!config) return;

    if (config->static_data_directory) {
        free((void*)config->static_data_directory);
        config->static_data_directory = nullptr;
    }
    if (config->proxy_catalog_server) {
        free((void*)config->proxy_catalog_server);
        config->proxy_catalog_server = nullptr;
    }
    if (config->proxy_image_server) {
        free((void*)config->proxy_image_server);
        config->proxy_image_server = nullptr;
    }
    if (config->proxy_art_server) {
        free((void*)config->proxy_art_server);
        config->proxy_art_server = nullptr;
    }
    if (config->proxy_mix_server) {
        free((void*)config->proxy_mix_server);
        config->proxy_mix_server = nullptr;
    }
}

void zune_device_set_path_resolver_callback(
    zune_device_handle_t handle,
    zune_path_resolver_callback_t callback,
    void* user_data)
{
    if (!handle) return;

    try {
        auto* device = static_cast<ZuneDevice*>(handle);
        device->SetPathResolverCallback(callback, user_data);
    } catch (const std::exception& e) {
        // Log error if log callback is available
        // For now, silently fail since this is a callback registration
    }
}

void zune_device_set_cache_storage_callback(
    zune_device_handle_t handle,
    zune_cache_storage_callback_t callback,
    void* user_data)
{
    if (!handle) return;

    try {
        auto* device = static_cast<ZuneDevice*>(handle);
        device->SetCacheStorageCallback(callback, user_data);
    } catch (const std::exception& e) {
        // Log error if log callback is available
        // For now, silently fail since this is a callback registration
    }
}

bool zune_device_initialize_http_subsystem(zune_device_handle_t handle)
{
    if (!handle) return false;

    try {
        auto* device = static_cast<ZuneDevice*>(handle);
        return device->InitializeHTTPSubsystem();
    } catch (const std::exception& e) {
        return false;
    }
}

bool zune_device_trigger_network_mode(zune_device_handle_t handle)
{
    if (!handle) return false;

    try {
        auto* device = static_cast<ZuneDevice*>(handle);
        device->TriggerNetworkMode();
        return true;
    } catch (const std::exception& e) {
        // Return false so C# can retry
        return false;
    }
}

void zune_device_enable_network_polling(zune_device_handle_t handle)
{
    if (!handle) return;

    try {
        auto* device = static_cast<ZuneDevice*>(handle);
        device->EnableNetworkPolling();
    } catch (const std::exception& e) {
        // Silently fail - errors are logged by ZuneDevice
    }
}

void zune_device_set_verbose_network_logging(zune_device_handle_t handle, bool enable)
{
    if (!handle) return;

    try {
        auto* device = static_cast<ZuneDevice*>(handle);
        device->SetVerboseNetworkLogging(enable);
    } catch (const std::exception& e) {
        // Silently fail - errors are logged by ZuneDevice
    }
}

} // extern "C"
