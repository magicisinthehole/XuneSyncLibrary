#include "zune_wireless/zune_wireless_api.h"
#include "ZuneDevice.h"
#include "ZuneDeviceIdentification.h"
#include "ZuneUploadPrimitives.h"
#include "protocols/http/ZuneHTTPInterceptor.h"
#include "ssdp_discovery.h"
#include <vector>
#include <cstring>
#include <iostream>
#include <mtp/ptp/Device.h>
#include <mtp/ptp/Session.h>
#include <mtp/ptp/OutputStream.h>
#include <mtp/ptp/ByteArrayObjectStream.h>
#include <cli/PosixStreams.h>
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

ZUNE_WIRELESS_API bool zune_device_is_connected(zune_device_handle_t handle) {
    if (handle) {
        return static_cast<ZuneDevice*>(handle)->IsConnected();
    }
    return false;
}

ZUNE_WIRELESS_API bool zune_device_validate_connection(zune_device_handle_t handle) {
    if (handle) {
        return static_cast<ZuneDevice*>(handle)->ValidateConnection();
    }
    return false;
}

ZUNE_WIRELESS_API void zune_device_set_log_callback(zune_device_handle_t handle, log_callback_t callback) {
    if (handle) {
        if (callback) {
            static_cast<ZuneDevice*>(handle)->SetLogCallback([callback](const std::string& message) {
                callback(message.c_str());
            });
        } else {
            // Clear the callback - don't create a lambda that captures null
            static_cast<ZuneDevice*>(handle)->SetLogCallback(nullptr);
        }
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

ZUNE_WIRELESS_API uint64_t zune_device_get_storage_capacity_bytes(zune_device_handle_t handle) {
    if (handle) {
        return static_cast<ZuneDevice*>(handle)->GetStorageCapacityBytes();
    }
    return 0;
}

ZUNE_WIRELESS_API uint64_t zune_device_get_storage_free_bytes(zune_device_handle_t handle) {
    if (handle) {
        return static_cast<ZuneDevice*>(handle)->GetStorageFreeBytes();
    }
    return 0;
}

ZUNE_WIRELESS_API bool zune_device_supports_network_mode(zune_device_handle_t handle) {
    if (handle) {
        return static_cast<ZuneDevice*>(handle)->SupportsNetworkMode();
    }
    return false;
}

ZUNE_WIRELESS_API zune_device_family_t zune_device_get_family(zune_device_handle_t handle) {
    if (handle) {
        return static_cast<zune_device_family_t>(static_cast<ZuneDevice*>(handle)->GetDeviceFamily());
    }
    return ZUNE_FAMILY_UNKNOWN;
}

ZUNE_WIRELESS_API uint8_t zune_device_get_color_id(zune_device_handle_t handle) {
    if (handle) {
        return static_cast<ZuneDevice*>(handle)->GetDeviceColorId();
    }
    return 0xFF;
}

ZUNE_WIRELESS_API const char* zune_device_get_color_name(zune_device_handle_t handle) {
    if (handle) {
        return static_cast<ZuneDevice*>(handle)->GetDeviceColorNameCached();
    }
    return nullptr;
}

ZUNE_WIRELESS_API const char* zune_device_get_family_name(zune_device_handle_t handle) {
    if (handle) {
        return static_cast<ZuneDevice*>(handle)->GetDeviceFamilyNameCached();
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

ZUNE_WIRELESS_API int zune_device_erase_all_content(zune_device_handle_t handle) {
    if (handle) {
        return static_cast<ZuneDevice*>(handle)->EraseAllContent();
    }
    return -1;
}

ZUNE_WIRELESS_API const char* zune_device_get_sync_partner_guid(zune_device_handle_t handle) {
    if (handle) {
        return static_cast<ZuneDevice*>(handle)->GetSyncPartnerGuidCached();
    }
    return nullptr;
}

ZUNE_WIRELESS_API int zune_device_set_device_name(zune_device_handle_t handle, const char* name) {
    if (!handle) {
        return -2;
    }
    if (!name) {
        return -1;
    }
    return static_cast<ZuneDevice*>(handle)->SetDeviceName(std::string(name));
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

    // Free artists
    for (uint32_t i = 0; i < library->artist_count; ++i) {
        free((void*)library->artists[i].name);
        free((void*)library->artists[i].filename);
        free((void*)library->artists[i].guid);
    }
    delete[] library->artists;

    // Free genres
    for (uint32_t i = 0; i < library->genre_count; ++i) {
        free((void*)library->genres[i].name);
    }
    delete[] library->genres;

    // Free artworks
    for (uint32_t i = 0; i < library->artwork_count; ++i) {
        free((void*)library->artworks[i].alb_reference);
    }
    delete[] library->artworks;

    // Free playlists
    for (uint32_t i = 0; i < library->playlist_count; ++i) {
        free((void*)library->playlists[i].name);
        free((void*)library->playlists[i].filename);
        free((void*)library->playlists[i].guid);
        free((void*)library->playlists[i].folder);
        delete[] library->playlists[i].track_atom_ids;
    }
    delete[] library->playlists;

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
    uint32_t duration_ms,
    int rating
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
            rating,
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
            -1,  // rating (not used for audiobooks)
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

// ============================================================================
// Playlist Management API
// ============================================================================

ZUNE_WIRELESS_API uint32_t zune_device_create_playlist(
    zune_device_handle_t handle,
    const char* name,
    const char* guid,
    const uint32_t* track_ids,
    size_t track_count
) {
    if (!handle || !name || !guid) {
        return 0;
    }

    try {
        std::vector<uint32_t> track_vec;
        if (track_ids && track_count > 0) {
            track_vec.assign(track_ids, track_ids + track_count);
        }

        return static_cast<ZuneDevice*>(handle)->CreatePlaylist(
            std::string(name),
            std::string(guid),
            track_vec
        );
    } catch (const std::exception& e) {
        return 0;
    }
}

ZUNE_WIRELESS_API int zune_device_update_playlist_tracks(
    zune_device_handle_t handle,
    uint32_t playlist_id,
    const uint32_t* track_ids,
    size_t track_count
) {
    if (!handle || playlist_id == 0) {
        return -1;
    }

    try {
        std::vector<uint32_t> track_vec;
        if (track_ids && track_count > 0) {
            track_vec.assign(track_ids, track_ids + track_count);
        }

        bool success = static_cast<ZuneDevice*>(handle)->UpdatePlaylistTracks(
            playlist_id,
            track_vec
        );
        return success ? 0 : -1;
    } catch (const std::exception& e) {
        return -1;
    }
}

ZUNE_WIRELESS_API int zune_device_delete_playlist(
    zune_device_handle_t handle,
    uint32_t playlist_id
) {
    if (!handle || playlist_id == 0) {
        return -1;
    }

    try {
        bool success = static_cast<ZuneDevice*>(handle)->DeletePlaylist(playlist_id);
        return success ? 0 : -1;
    } catch (const std::exception& e) {
        return -1;
    }
}

ZUNE_WIRELESS_API int zune_device_set_track_user_state(
    zune_device_handle_t handle,
    uint32_t zmdb_atom_id,
    int play_count,
    int skip_count,
    int rating
) {
    if (!handle) {
        return -2;  // Device not connected
    }

    try {
        return static_cast<ZuneDevice*>(handle)->SetTrackUserState(zmdb_atom_id, play_count, skip_count, rating);
    } catch (const std::exception& e) {
        return -1;  // General MTP error
    }
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

        // Find Zune device on USB (Microsoft vendor ID)
        // This is a lightweight discovery check - actual model detection
        // happens after full connection via ZuneDevice::GetModel()
        auto devices = ctx->GetDevices();
        for (auto desc : devices) {
            if (desc->GetVendorId() == 0x045E) {  // Microsoft vendor ID
                try {
                    auto device = mtp::Device::Open(ctx, desc, true, false);
                    if (device) {
                        auto info = device->GetInfo();

                        thread_local std::string serial;
                        thread_local std::string name;

                        serial = info.SerialNumber;
                        name = "Zune";  // Generic name for discovery

                        *uuid = serial.c_str();
                        *device_name = name.c_str();

                        return true;
                    }
                } catch (const std::exception&) {
                    // Try next device
                }
            }
        }
    } catch (const std::exception&) {
        // Discovery failed
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

// ============================================================================
// Low-Level MTP Primitives Implementation
// ============================================================================

ZUNE_WIRELESS_API ZuneMtpCreateResult zune_mtp_send_object_prop_list(
    zune_device_handle_t handle,
    uint32_t storage_id,
    uint32_t parent_id,
    uint16_t format,
    uint64_t object_size,
    const ZuneMtpProperty* properties,
    uint32_t property_count
) {
    ZuneMtpCreateResult result = {0, 0, 0, -1};

    if (!handle || (property_count > 0 && !properties)) {
        return result;
    }

    try {
        auto* device = static_cast<ZuneDevice*>(handle);
        auto session = device->GetMtpSession();
        if (!session) {
            result.status = -2;  // Not connected
            return result;
        }

        // Get storage ID if not provided
        mtp::StorageId mtpStorage(storage_id ? storage_id : device->GetDefaultStorageId());

        // Build property list byte array
        mtp::ByteArray propList;
        mtp::OutputStream os(propList);
        os.Write32(property_count);

        for (uint32_t i = 0; i < property_count; i++) {
            const auto& prop = properties[i];
            os.Write32(prop.object_handle);
            os.Write16(prop.property_code);
            os.Write16(prop.data_type);

            switch (prop.data_type) {
                case ZUNE_MTP_TYPE_UINT8:
                    if (prop.value && prop.value_size >= 1) {
                        os.Write8(*static_cast<const uint8_t*>(prop.value));
                    }
                    break;
                case ZUNE_MTP_TYPE_UINT16:
                    if (prop.value && prop.value_size >= 2) {
                        os.Write16(*static_cast<const uint16_t*>(prop.value));
                    }
                    break;
                case ZUNE_MTP_TYPE_UINT32:
                    if (prop.value && prop.value_size >= 4) {
                        os.Write32(*static_cast<const uint32_t*>(prop.value));
                    }
                    break;
                case ZUNE_MTP_TYPE_UINT64:
                    if (prop.value && prop.value_size >= 8) {
                        os.Write64(*static_cast<const uint64_t*>(prop.value));
                    }
                    break;
                case ZUNE_MTP_TYPE_UINT128:
                    if (prop.value && prop.value_size >= 16) {
                        const uint8_t* bytes = static_cast<const uint8_t*>(prop.value);
                        for (int j = 0; j < 16; j++) {
                            os.Write8(bytes[j]);
                        }
                    }
                    break;
                case ZUNE_MTP_TYPE_STRING:
                    if (prop.value) {
                        os.WriteString(static_cast<const char*>(prop.value));
                    } else {
                        os.WriteString("");
                    }
                    break;
                default:
                    // Unknown type - write raw bytes if available
                    if (prop.value && prop.value_size > 0) {
                        const uint8_t* bytes = static_cast<const uint8_t*>(prop.value);
                        for (uint32_t j = 0; j < prop.value_size; j++) {
                            os.Write8(bytes[j]);
                        }
                    }
                    break;
            }
        }

        auto response = session->SendObjectPropList(
            mtpStorage,
            mtp::ObjectId(parent_id),
            static_cast<mtp::ObjectFormat>(format),
            object_size,
            propList);

        result.object_id = response.ObjectId.Id;
        result.storage_id = response.StorageId.Id;
        result.parent_id = response.ParentObjectId.Id;
        result.status = 0;

    } catch (const std::exception& e) {
        std::cerr << "[zune_mtp_send_object_prop_list] Exception: " << e.what() << std::endl;
        result.status = -1;
    }

    return result;
}

ZUNE_WIRELESS_API int zune_mtp_send_object(
    zune_device_handle_t handle,
    const void* data,
    uint64_t size
) {
    if (!handle) return -1;

    try {
        auto* device = static_cast<ZuneDevice*>(handle);
        auto session = device->GetMtpSession();
        if (!session) return -2;

        if (size == 0 || !data) {
            // Send empty object
            mtp::ByteArray empty;
            auto stream = std::make_shared<mtp::ByteArrayObjectInputStream>(empty);
            session->SendObject(stream);
        } else {
            mtp::ByteArray bytes(static_cast<const uint8_t*>(data),
                                  static_cast<const uint8_t*>(data) + size);
            auto stream = std::make_shared<mtp::ByteArrayObjectInputStream>(bytes);
            session->SendObject(stream);
        }

        return 0;
    } catch (const std::exception& e) {
        return -1;
    }
}

ZUNE_WIRELESS_API int zune_mtp_send_object_from_file(
    zune_device_handle_t handle,
    const char* file_path
) {
    if (!handle || !file_path) return -1;

    try {
        auto* device = static_cast<ZuneDevice*>(handle);
        auto session = device->GetMtpSession();
        if (!session) return -2;

        auto stream = std::make_shared<cli::ObjectInputStream>(file_path);
        session->SendObject(stream);

        return 0;
    } catch (const std::exception& e) {
        return -1;
    }
}

ZUNE_WIRELESS_API int zune_mtp_set_object_references(
    zune_device_handle_t handle,
    uint32_t object_id,
    const uint32_t* ref_ids,
    uint32_t ref_count
) {
    if (!handle || object_id == 0) return -1;

    try {
        auto* device = static_cast<ZuneDevice*>(handle);
        auto session = device->GetMtpSession();
        if (!session) return -2;

        mtp::msg::ObjectHandles handles;
        if (ref_ids && ref_count > 0) {
            handles.ObjectHandles.reserve(ref_count);
            for (uint32_t i = 0; i < ref_count; i++) {
                handles.ObjectHandles.push_back(mtp::ObjectId(ref_ids[i]));
            }
        }

        session->SetObjectReferences(mtp::ObjectId(object_id), handles);
        return 0;

    } catch (const std::exception& e) {
        return -1;
    }
}

ZUNE_WIRELESS_API int zune_mtp_get_object_references(
    zune_device_handle_t handle,
    uint32_t object_id,
    uint32_t* out_ref_ids,
    uint32_t max_refs,
    uint32_t* out_count
) {
    if (!handle || object_id == 0 || !out_count) return -1;

    *out_count = 0;

    try {
        auto* device = static_cast<ZuneDevice*>(handle);
        auto session = device->GetMtpSession();
        if (!session) return -2;

        auto handles = session->GetObjectReferences(mtp::ObjectId(object_id));

        uint32_t count = std::min(static_cast<uint32_t>(handles.ObjectHandles.size()), max_refs);
        if (out_ref_ids && count > 0) {
            for (uint32_t i = 0; i < count; i++) {
                out_ref_ids[i] = handles.ObjectHandles[i].Id;
            }
        }
        *out_count = static_cast<uint32_t>(handles.ObjectHandles.size());

        return 0;

    } catch (const std::exception& e) {
        return -1;
    }
}

ZUNE_WIRELESS_API int zune_mtp_set_object_property_string(
    zune_device_handle_t handle,
    uint32_t object_id,
    uint16_t property_code,
    const char* value
) {
    if (!handle || object_id == 0) return -1;

    try {
        auto* device = static_cast<ZuneDevice*>(handle);
        auto session = device->GetMtpSession();
        if (!session) return -2;

        session->SetObjectProperty(
            mtp::ObjectId(object_id),
            static_cast<mtp::ObjectProperty>(property_code),
            std::string(value ? value : ""));

        return 0;

    } catch (const std::exception& e) {
        return -1;
    }
}

ZUNE_WIRELESS_API int zune_mtp_set_object_property_int(
    zune_device_handle_t handle,
    uint32_t object_id,
    uint16_t property_code,
    uint64_t value
) {
    if (!handle || object_id == 0) return -1;

    try {
        auto* device = static_cast<ZuneDevice*>(handle);
        auto session = device->GetMtpSession();
        if (!session) return -2;

        session->SetObjectProperty(
            mtp::ObjectId(object_id),
            static_cast<mtp::ObjectProperty>(property_code),
            value);

        return 0;

    } catch (const std::exception& e) {
        return -1;
    }
}

ZUNE_WIRELESS_API int zune_mtp_set_object_property_array(
    zune_device_handle_t handle,
    uint32_t object_id,
    uint16_t property_code,
    const void* data,
    uint32_t data_size
) {
    if (!handle || object_id == 0) return -1;

    try {
        auto* device = static_cast<ZuneDevice*>(handle);
        auto session = device->GetMtpSession();
        if (!session) return -2;

        mtp::ByteArray bytes;
        if (data && data_size > 0) {
            const uint8_t* ptr = static_cast<const uint8_t*>(data);
            bytes.assign(ptr, ptr + data_size);
        }

        session->SetObjectPropertyAsArray(
            mtp::ObjectId(object_id),
            static_cast<mtp::ObjectProperty>(property_code),
            bytes);

        return 0;

    } catch (const std::exception& e) {
        return -1;
    }
}

ZUNE_WIRELESS_API int zune_mtp_get_object_handles(
    zune_device_handle_t handle,
    uint32_t storage_id,
    uint16_t format,
    uint32_t parent_id,
    uint32_t* out_handles,
    uint32_t max_handles,
    uint32_t* out_count
) {
    if (!handle || !out_count) return -1;

    *out_count = 0;

    try {
        auto* device = static_cast<ZuneDevice*>(handle);
        auto session = device->GetMtpSession();
        if (!session) return -2;

        mtp::StorageId mtpStorage(storage_id ? storage_id : device->GetDefaultStorageId());
        mtp::ObjectFormat mtpFormat = format ? static_cast<mtp::ObjectFormat>(format) : mtp::ObjectFormat::Any;
        mtp::ObjectId mtpParent = (parent_id == 0xFFFFFFFF) ? mtp::Session::Root : mtp::ObjectId(parent_id);

        auto handles = session->GetObjectHandles(mtpStorage, mtpFormat, mtpParent);

        uint32_t count = std::min(static_cast<uint32_t>(handles.ObjectHandles.size()), max_handles);
        if (out_handles && count > 0) {
            for (uint32_t i = 0; i < count; i++) {
                out_handles[i] = handles.ObjectHandles[i].Id;
            }
        }
        *out_count = static_cast<uint32_t>(handles.ObjectHandles.size());

        return 0;

    } catch (const std::exception& e) {
        return -1;
    }
}

ZUNE_WIRELESS_API uint32_t zune_mtp_create_directory(
    zune_device_handle_t handle,
    const char* name,
    uint32_t parent_id,
    uint32_t storage_id
) {
    if (!handle || !name) return 0;

    try {
        auto* device = static_cast<ZuneDevice*>(handle);
        auto session = device->GetMtpSession();
        if (!session) return 0;

        mtp::StorageId mtpStorage(storage_id ? storage_id : device->GetDefaultStorageId());
        mtp::ObjectId mtpParent = (parent_id == 0) ? mtp::Session::Root : mtp::ObjectId(parent_id);

        auto info = session->CreateDirectory(std::string(name), mtpParent, mtpStorage);
        return info.ObjectId.Id;

    } catch (const std::exception& e) {
        return 0;
    }
}

ZUNE_WIRELESS_API int zune_mtp_get_storage_ids(
    zune_device_handle_t handle,
    uint32_t* out_storage_ids,
    uint32_t max_storages
) {
    if (!handle) return -1;

    try {
        auto* device = static_cast<ZuneDevice*>(handle);
        auto session = device->GetMtpSession();
        if (!session) return -2;

        auto storages = session->GetStorageIDs();

        uint32_t count = std::min(static_cast<uint32_t>(storages.StorageIDs.size()), max_storages);
        if (out_storage_ids && count > 0) {
            for (uint32_t i = 0; i < count; i++) {
                out_storage_ids[i] = storages.StorageIDs[i].Id;
            }
        }

        return static_cast<int>(storages.StorageIDs.size());

    } catch (const std::exception& e) {
        return -1;
    }
}

ZUNE_WIRELESS_API uint32_t zune_mtp_get_default_storage(
    zune_device_handle_t handle
) {
    if (!handle) return 0;

    try {
        auto* device = static_cast<ZuneDevice*>(handle);
        return device->GetDefaultStorageId();
    } catch (const std::exception& e) {
        return 0;
    }
}

ZUNE_WIRELESS_API int zune_mtp_get_well_known_folders(
    zune_device_handle_t handle,
    ZuneMtpFolderIds* out_folders
) {
    if (!handle || !out_folders) return -1;

    try {
        auto* device = static_cast<ZuneDevice*>(handle);
        auto folders = device->GetWellKnownFolders();

        out_folders->artists_folder = folders.artists_folder;
        out_folders->albums_folder = folders.albums_folder;
        out_folders->music_folder = folders.music_folder;
        out_folders->playlists_folder = folders.playlists_folder;
        out_folders->storage_id = folders.storage_id;
        out_folders->artist_format_supported = folders.artist_format_supported ? 1 : 0;

        return 0;

    } catch (const std::exception& e) {
        return -1;
    }
}

ZUNE_WIRELESS_API int zune_mtp_delete_object(
    zune_device_handle_t handle,
    uint32_t object_id
) {
    if (!handle || object_id == 0) return -1;

    try {
        auto* device = static_cast<ZuneDevice*>(handle);
        auto session = device->GetMtpSession();
        if (!session) return -2;

        session->DeleteObject(mtp::ObjectId(object_id));
        return 0;

    } catch (const std::exception& e) {
        return -1;
    }
}

// --- Zune Vendor Operations ---

ZUNE_WIRELESS_API int zune_mtp_operation_9217(
    zune_device_handle_t handle,
    uint32_t param
) {
    if (!handle) return -1;

    try {
        auto* device = static_cast<ZuneDevice*>(handle);
        auto session = device->GetMtpSession();
        if (!session) return -2;

        // Use the Session's proper Operation9217 method which passes param
        // as an MTP operation parameter (not data payload)
        session->Operation9217(param);
        return 0;

    } catch (const std::exception& e) {
        std::cerr << "[zune_mtp_operation_9217] Exception: " << e.what() << std::endl;
        return -1;
    }
}

ZUNE_WIRELESS_API int zune_mtp_operation_922a(
    zune_device_handle_t handle,
    const char* track_name
) {
    if (!handle) return -1;

    try {
        auto* device = static_cast<ZuneDevice*>(handle);
        auto session = device->GetMtpSession();
        if (!session) return -2;

        // Use the Session's proper Operation922a method which builds the correct
        // 530-byte data structure with UTF-16LE encoding
        session->Operation922a(track_name ? track_name : "");
        return 0;

    } catch (const std::exception& e) {
        std::cerr << "[zune_mtp_operation_922a] Exception: " << e.what() << std::endl;
        return -1;
    }
}

ZUNE_WIRELESS_API int zune_mtp_operation_9802(
    zune_device_handle_t handle,
    uint16_t property_code,
    uint16_t format_type
) {
    if (!handle) return -1;

    try {
        auto* device = static_cast<ZuneDevice*>(handle);
        auto session = device->GetMtpSession();
        if (!session) return -2;

        // Use the Session's proper Operation9802 method which passes params
        // as MTP operation parameters (not data payload)
        session->Operation9802(property_code, format_type);
        return 0;

    } catch (const std::exception& e) {
        std::cerr << "[zune_mtp_operation_9802] Exception: " << e.what() << std::endl;
        return -1;
    }
}

// ═══════════════════════════════════════════════════════════════════════════
// Upload Primitives — Pcap-verified Zune upload operations
// ═══════════════════════════════════════════════════════════════════════════

// Helper: get session + determine if HD
#define UPLOAD_SESSION_GUARD(handle) \
    if (!handle) return -1; \
    auto* _device = static_cast<ZuneDevice*>(handle); \
    auto _session = _device->GetMtpSession(); \
    if (!_session) return -2; \
    bool _isHD = (_device->GetDeviceFamily() == zune::DeviceFamily::Pavo);

#define UPLOAD_SESSION_GUARD_VAL(handle, fail_val) \
    if (!handle) return fail_val; \
    auto* _device = static_cast<ZuneDevice*>(handle); \
    auto _session = _device->GetMtpSession(); \
    if (!_session) return fail_val; \
    bool _isHD = (_device->GetDeviceFamily() == zune::DeviceFamily::Pavo);

// --- Pre-Upload ---

ZUNE_WIRELESS_API int zune_upload_read_sync_status(zune_device_handle_t handle) {
    UPLOAD_SESSION_GUARD(handle);
    try { zune::UploadPrimitives::ReadDeviceSyncStatus(_session); return 0; }
    catch (...) { return -1; }
}

ZUNE_WIRELESS_API int zune_upload_sync_device_db(zune_device_handle_t handle) {
    UPLOAD_SESSION_GUARD(handle);
    try { zune::UploadPrimitives::SyncDeviceDB(_session); return 0; }
    catch (...) { return -1; }
}

ZUNE_WIRELESS_API ZuneRootDiscovery zune_upload_discover_root(zune_device_handle_t handle) {
    ZuneRootDiscovery result = {};
    if (!handle) return result;
    auto* device = static_cast<ZuneDevice*>(handle);
    auto session = device->GetMtpSession();
    if (!session) return result;
    try {
        auto r = zune::UploadPrimitives::DiscoverRoot(session, device->GetDefaultStorageId());
        result.music_folder = r.music_folder;
        result.albums_folder = r.albums_folder;
        result.artists_folder = r.artists_folder;
        result.storage_id = r.storage_id;
        result.root_object_count = r.root_object_count;
    } catch (...) {}
    return result;
}

ZUNE_WIRELESS_API int zune_upload_root_re_enum(zune_device_handle_t handle) {
    UPLOAD_SESSION_GUARD(handle);
    try { zune::UploadPrimitives::RootReEnum(_session); return 0; }
    catch (...) { return -1; }
}

// --- Folder Discovery & Creation ---

ZUNE_WIRELESS_API ZuneFolderDiscovery zune_upload_discover_folder(
    zune_device_handle_t handle, uint32_t folder_id)
{
    ZuneFolderDiscovery result = {};
    if (!handle) return result;
    auto* device = static_cast<ZuneDevice*>(handle);
    auto session = device->GetMtpSession();
    if (!session) return result;
    try {
        auto children = zune::UploadPrimitives::DiscoverFolderChildren(
            session, device->GetDefaultStorageId(), folder_id);
        result.count = std::min(static_cast<int>(children.size()), 64);
        for (int i = 0; i < result.count; ++i) {
            result.children[i].handle = children[i].handle;
            strncpy(result.children[i].name, children[i].name.c_str(), 255);
            result.children[i].name[255] = '\0';
        }
    } catch (...) {}
    return result;
}

ZUNE_WIRELESS_API uint32_t zune_upload_create_folder(
    zune_device_handle_t handle, uint32_t parent_id, const char* name)
{
    UPLOAD_SESSION_GUARD_VAL(handle, 0);
    try {
        return zune::UploadPrimitives::CreateFolder(
            _session, _device->GetDefaultStorageId(), parent_id, name ? name : "");
    } catch (...) { return 0; }
}

ZUNE_WIRELESS_API int zune_upload_folder_readback(
    zune_device_handle_t handle, uint32_t folder_id)
{
    UPLOAD_SESSION_GUARD(handle);
    try {
        zune::UploadPrimitives::FolderReadback(
            _session, folder_id, _device->GetDefaultStorageId());
        return 0;
    } catch (...) { return -1; }
}

ZUNE_WIRELESS_API int zune_upload_first_folder_readback(
    zune_device_handle_t handle, uint32_t folder_id)
{
    UPLOAD_SESSION_GUARD(handle);
    try {
        zune::UploadPrimitives::FirstFolderReadback(
            _session, folder_id, _device->GetDefaultStorageId(), _isHD);
        return 0;
    } catch (...) { return -1; }
}

// --- Artist Metadata ---

ZUNE_WIRELESS_API uint32_t zune_upload_create_artist_metadata(
    zune_device_handle_t handle, uint32_t artists_folder,
    const char* name, const uint8_t* guid_bytes, uint32_t guid_len)
{
    UPLOAD_SESSION_GUARD_VAL(handle, 0);
    try {
        return zune::UploadPrimitives::CreateArtistMetadata(
            _session, _device->GetDefaultStorageId(), artists_folder,
            name ? name : "", guid_bytes, guid_len);
    } catch (...) { return 0; }
}

// --- Track Operations ---

ZUNE_WIRELESS_API uint32_t zune_upload_create_track(
    zune_device_handle_t handle, uint32_t album_folder,
    const ZuneTrackProps* props, uint16_t format_code, uint64_t file_size)
{
    UPLOAD_SESSION_GUARD_VAL(handle, 0);
    if (!props) return 0;
    try {
        zune::TrackProperties tp;
        tp.filename = props->filename ? props->filename : "";
        tp.title = props->title ? props->title : "";
        tp.artist = props->artist ? props->artist : "";
        tp.album_name = props->album_name ? props->album_name : "";
        tp.album_artist = props->album_artist ? props->album_artist : "";
        tp.genre = props->genre ? props->genre : "";
        tp.date_authored = props->date_authored ? props->date_authored : "";
        tp.duration_ms = props->duration_ms;
        tp.track_number = props->track_number;
        tp.rating = props->rating;
        tp.disc_number = props->disc_number;
        tp.artist_meta_id = props->artist_meta_id;
        tp.is_hd = props->is_hd;
        return zune::UploadPrimitives::CreateTrack(
            _session, _device->GetDefaultStorageId(), album_folder,
            tp, format_code, file_size);
    } catch (...) { return 0; }
}

ZUNE_WIRELESS_API int zune_upload_send_audio(
    zune_device_handle_t handle, const char* file_path)
{
    UPLOAD_SESSION_GUARD(handle);
    if (!file_path) return -1;
    try {
        zune::UploadPrimitives::UploadAudioData(_session, file_path);
        return 0;
    } catch (...) { return -1; }
}

ZUNE_WIRELESS_API int zune_upload_verify_track(
    zune_device_handle_t handle, uint32_t track_id)
{
    UPLOAD_SESSION_GUARD(handle);
    try { zune::UploadPrimitives::VerifyTrack(_session, track_id); return 0; }
    catch (...) { return -1; }
}

// --- Album Metadata ---

ZUNE_WIRELESS_API uint32_t zune_upload_create_album(
    zune_device_handle_t handle, uint32_t albums_folder,
    const ZuneAlbumProps* props)
{
    UPLOAD_SESSION_GUARD_VAL(handle, 0);
    if (!props) return 0;
    try {
        zune::AlbumProperties ap;
        ap.artist = props->artist ? props->artist : "";
        ap.album_name = props->album_name ? props->album_name : "";
        ap.date_authored = props->date_authored ? props->date_authored : "";
        ap.artist_meta_id = props->artist_meta_id;
        ap.is_hd = props->is_hd;
        return zune::UploadPrimitives::CreateAlbumMetadata(
            _session, _device->GetDefaultStorageId(), albums_folder, ap);
    } catch (...) { return 0; }
}

ZUNE_WIRELESS_API int zune_upload_set_artwork(
    zune_device_handle_t handle, uint32_t album_id,
    const uint8_t* data, uint32_t size)
{
    UPLOAD_SESSION_GUARD(handle);
    if (!data || size == 0) return 0;
    try {
        zune::UploadPrimitives::SetAlbumArtwork(_session, album_id, data, size);
        return 0;
    } catch (...) { return -1; }
}

ZUNE_WIRELESS_API int zune_upload_set_album_refs(
    zune_device_handle_t handle, uint32_t album_id,
    const uint32_t* track_ids, uint32_t count)
{
    UPLOAD_SESSION_GUARD(handle);
    if (!track_ids || count == 0) return 0;
    try {
        zune::UploadPrimitives::SetAlbumReferences(_session, album_id, track_ids, count);
        return 0;
    } catch (...) { return -1; }
}

ZUNE_WIRELESS_API int zune_upload_verify_album(
    zune_device_handle_t handle, uint32_t album_id, bool include_parent_desc)
{
    UPLOAD_SESSION_GUARD(handle);
    try {
        zune::UploadPrimitives::VerifyAlbum(_session, album_id, include_parent_desc);
        return 0;
    } catch (...) { return -1; }
}

ZUNE_WIRELESS_API int zune_upload_read_album_subset(
    zune_device_handle_t handle, uint32_t album_id)
{
    UPLOAD_SESSION_GUARD(handle);
    try {
        _session->GetObjectPropertyList(
            mtp::ObjectId(album_id), mtp::ObjectFormat(0),
            mtp::ObjectProperty(0), 2, 0);
        return 0;
    } catch (...) { return -1; }
}

// --- Finalization ---

ZUNE_WIRELESS_API int zune_upload_disable_trusted_files(zune_device_handle_t handle) {
    UPLOAD_SESSION_GUARD(handle);
    try { zune::UploadPrimitives::DisableTrustedFiles(_session); return 0; }
    catch (...) { return -1; }
}

ZUNE_WIRELESS_API int zune_upload_open_idle_session(zune_device_handle_t handle) {
    UPLOAD_SESSION_GUARD(handle);
    try { zune::UploadPrimitives::OpenIdleSession(_session); return 0; }
    catch (...) { return -1; }
}

ZUNE_WIRELESS_API int zune_upload_close_session(zune_device_handle_t handle) {
    UPLOAD_SESSION_GUARD(handle);
    try { zune::UploadPrimitives::CloseSession(_session); return 0; }
    catch (...) { return -1; }
}

ZUNE_WIRELESS_API int zune_upload_register_track_ctx(
    zune_device_handle_t handle, const char* track_name)
{
    UPLOAD_SESSION_GUARD(handle);
    if (!track_name) return -1;
    try {
        zune::UploadPrimitives::RegisterTrackContext(_session, track_name);
        return 0;
    } catch (...) { return -1; }
}

// --- Property Descriptor Queries ---

ZUNE_WIRELESS_API int zune_upload_query_folder_descs(zune_device_handle_t handle) {
    UPLOAD_SESSION_GUARD(handle);
    try { zune::UploadPrimitives::QueryFolderDescriptors(_session); return 0; }
    catch (...) { return -1; }
}

ZUNE_WIRELESS_API int zune_upload_query_batch_descs(
    zune_device_handle_t handle, uint16_t prop_code)
{
    UPLOAD_SESSION_GUARD(handle);
    try { zune::UploadPrimitives::QueryBatchDescriptors(_session, prop_code, _isHD); return 0; }
    catch (...) { return -1; }
}

ZUNE_WIRELESS_API int zune_upload_query_object_format_descs(zune_device_handle_t handle) {
    UPLOAD_SESSION_GUARD(handle);
    try { zune::UploadPrimitives::QueryBatchDescriptors(_session, 0xDC02, _isHD); return 0; }
    catch (...) { return -1; }
}

ZUNE_WIRELESS_API int zune_upload_query_track_descs(
    zune_device_handle_t handle, uint16_t format_code)
{
    UPLOAD_SESSION_GUARD(handle);
    try { zune::UploadPrimitives::QueryTrackDescriptors(_session, format_code, _isHD); return 0; }
    catch (...) { return -1; }
}

ZUNE_WIRELESS_API int zune_upload_query_album_descs(zune_device_handle_t handle) {
    UPLOAD_SESSION_GUARD(handle);
    try { zune::UploadPrimitives::QueryAlbumDescriptors(_session, _isHD); return 0; }
    catch (...) { return -1; }
}

ZUNE_WIRELESS_API int zune_upload_query_artist_descs(zune_device_handle_t handle) {
    UPLOAD_SESSION_GUARD(handle);
    try { zune::UploadPrimitives::QueryArtistDescriptors(_session); return 0; }
    catch (...) { return -1; }
}

ZUNE_WIRELESS_API int zune_upload_query_artwork_descs(zune_device_handle_t handle) {
    UPLOAD_SESSION_GUARD(handle);
    try { zune::UploadPrimitives::QueryArtworkDescriptors(_session); return 0; }
    catch (...) { return -1; }
}

} // extern "C"
