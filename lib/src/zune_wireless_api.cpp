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

ZUNE_WIRELESS_API ZuneArtistInfo* zune_device_get_music_library(zune_device_handle_t handle, uint32_t* count) {
    if (!handle) {
        *count = 0;
        return nullptr;
    }
    auto* device = static_cast<ZuneDevice*>(handle);
    auto artists = device->GetMusicLibrary();
    *count = artists.size();
    auto* c_artists = new ZuneArtistInfo[artists.size()];
    for (size_t i = 0; i < artists.size(); ++i) {
        c_artists[i] = artists[i];
    }
    return c_artists;
}

ZUNE_WIRELESS_API ZuneArtistInfo* zune_device_get_music_library_fast(zune_device_handle_t handle, uint32_t* count) {
    if (!handle) {
        *count = 0;
        return nullptr;
    }
    auto* device = static_cast<ZuneDevice*>(handle);
    auto artists = device->GetMusicLibraryFast();
    *count = artists.size();
    auto* c_artists = new ZuneArtistInfo[artists.size()];
    for (size_t i = 0; i < artists.size(); ++i) {
        c_artists[i] = artists[i];
    }
    return c_artists;
}

ZUNE_WIRELESS_API void zune_device_free_music_library(ZuneArtistInfo* artists, uint32_t count) {
    if (!artists) return;
    for (uint32_t i = 0; i < count; ++i) {
        free((void*)artists[i].Name);
        for (uint32_t j = 0; j < artists[i].AlbumCount; ++j) {
            free((void*)artists[i].Albums[j].Title);
            free((void*)artists[i].Albums[j].Artist);
            for (uint32_t k = 0; k < artists[i].Albums[j].TrackCount; ++k) {
                free((void*)artists[i].Albums[j].Tracks[k].Title);
                free((void*)artists[i].Albums[j].Tracks[k].Artist);
                free((void*)artists[i].Albums[j].Tracks[k].Album);
                free((void*)artists[i].Albums[j].Tracks[k].Filename);
            }
            delete[] artists[i].Albums[j].Tracks;
        }
        delete[] artists[i].Albums;
    }
    delete[] artists;
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
) {
    if (handle) {
        return static_cast<ZuneDevice*>(handle)->UploadTrackWithMetadata(
            audio_file_path ? audio_file_path : "",
            artist_name ? artist_name : "",
            album_name ? album_name : "",
            album_year,
            track_title ? track_title : "",
            genre ? genre : "",
            track_number,
            artwork_data,
            artwork_size
        );
    }
    return -1;
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
            auto session = device->OpenSession(1);
            if (session) {
                auto name_data = session->GetDeviceProperty(mtp::DeviceProperty(0xd402));
                thread_local std::string name;
                // The first byte is the length, the rest is a null-terminated UTF-16LE string
                if (name_data.size() > 1) {
                    std::string result;
                    for (size_t i = 1; i < name_data.size() - 1; i += 2) {
                        if (name_data[i+1] == 0 && name_data[i] != 0) {
                            result += static_cast<char>(name_data[i]);
                        }
                    }
                    name = result;
                }

                auto info = device->GetInfo();
                thread_local std::string serial = info.SerialNumber;
                *uuid = serial.c_str();
                *device_name = name.c_str();
                return true;
            }
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

} // extern "C"
