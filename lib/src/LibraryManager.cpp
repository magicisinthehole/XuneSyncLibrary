#include "LibraryManager.h"
#include "zmdb/ZMDBParserFactory.h"
#include <iostream>
#include <fstream>
#include <sstream>
#include <iomanip>
#include <algorithm>
#include <thread>
#include <chrono>
#include <cstring>
#include <cctype>
#include <regex>
#include <mtp/metadata/Metadata.h>
#include <mtp/ptp/ObjectPropertyListParser.h>
#include <mtp/ptp/ByteArrayObjectStream.h>
#include <cli/PosixStreams.h>

using namespace mtp;

// === Helper Functions ===

// Validate MusicBrainz GUID format
static bool IsValidGuid(const std::string& guid) {
    // MusicBrainz GUID format: xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx (case-insensitive)
    static const std::regex guid_pattern(
        "^[0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{12}$"
    );
    return std::regex_match(guid, guid_pattern);
}

// --- Zune Metadata Request Protocol Constants ---
constexpr size_t ZMDB_REQUEST_SIZE = 16;
constexpr uint8_t ZMDB_REQUEST_LENGTH_BYTE = 0x10;
constexpr uint8_t ZMDB_COMMAND_MARKER = 0x01;
constexpr uint8_t ZMDB_OPERATION_CODE_HIGH = 0x17;
constexpr uint8_t ZMDB_OPERATION_CODE_LOW = 0x92;
constexpr uint8_t ZMDB_OBJECT_ID_SIZE = 3;
constexpr uint8_t ZMDB_TRAILER_VALUE = 0x01;
constexpr size_t ZMDB_HEADER_SIZE = 12;
constexpr int ZMDB_DEVICE_PREPARE_DELAY_MS = 250;
constexpr int ZMDB_PIPE_DRAIN_TIMEOUT_MS = 100;

// --- Helper classes for bulk data streaming ---
class ByteArrayInputStream : public mtp::IObjectInputStream {
private:
    const mtp::ByteArray& _data;
    size_t _offset;
public:
    ByteArrayInputStream(const mtp::ByteArray& data) : _data(data), _offset(0) {}
    mtp::u64 GetSize() const override { return _data.size(); }
    size_t Read(mtp::u8 *data, size_t size) override {
        size_t remaining = _data.size() - _offset;
        size_t to_read = std::min(size, remaining);
        if (to_read > 0) {
            std::memcpy(data, _data.data() + _offset, to_read);
            _offset += to_read;
        }
        return to_read;
    }
    void Cancel() override {}
};

class ByteArrayOutputStream : public mtp::IObjectOutputStream {
public:
    mtp::ByteArray data;
    size_t Write(const uint8_t *buffer, size_t size) override {
        if (buffer && size > 0) {
            data.insert(data.end(), buffer, buffer + size);
        }
        return size;
    }
    void Cancel() override {
    }
};

LibraryManager::LibraryManager(std::shared_ptr<mtp::Session> mtp_session, std::shared_ptr<cli::Session> cli_session, LogCallback log_callback)
    : mtp_session_(mtp_session), cli_session_(cli_session), log_callback_(log_callback) {
}

LibraryManager::~LibraryManager() {
}

void LibraryManager::Log(const std::string& message) {
    if (log_callback_) {
        log_callback_(message);
    }
}

std::string LibraryManager::Utf16leToAscii(const ByteArray& data, bool is_guid) {
    std::string result;
    if (data.size() < 2) return result;

    size_t start = 1; // First byte is length, skip it
    for (size_t i = start; i < data.size() - 1; i += 2) {
        if (data[i+1] == 0 && data[i] != 0) {
            result += static_cast<char>(data[i]);
        }
    }

    if (is_guid) {
        result.erase(std::remove(result.begin(), result.end(), '{'), result.end());
        result.erase(std::remove(result.begin(), result.end(), '}'), result.end());
        result.erase(std::remove(result.begin(), result.end(), '\''), result.end());
    }
    return result;
}

mtp::ByteArray LibraryManager::HexToBytes(const std::string& hex_str) {
    ByteArray data;
    std::string hex = hex_str;
    hex.erase(std::remove_if(hex.begin(), hex.end(), ::isspace), hex.end());

    for (size_t i = 0; i < hex.length(); i += 2) {
        if (i + 1 < hex.length()) {
            std::string byte_str = hex.substr(i, 2);
            data.push_back(static_cast<u8>(std::stoul(byte_str, nullptr, 16)));
        }
    }
    return data;
}

void LibraryManager::EnsureLibraryInitialized() {
    if (!library_) {
        if (!mtp_session_) {
            throw std::runtime_error("MTP session not initialized");
        }
        Log("Initializing MTP Library for music management...");
        library_ = std::make_shared<mtp::Library>(mtp_session_);
        Log("  ✓ Library initialized and device scanned");
    }
}

std::vector<ZuneObjectInfoInternal> LibraryManager::ListStorage(uint32_t parent_handle) {
    std::vector<ZuneObjectInfoInternal> results;
    if (!mtp_session_) {
        Log("Error: Not connected to a device.");
        return results;
    }
    try {
        // If parent_handle is 0, list the root of all storages.
        // Otherwise, list the contents of the specified object handle.
        if (parent_handle == 0) {
            auto storageIds = mtp_session_->GetStorageIDs();
            for (auto storageId : storageIds.StorageIDs) {
                auto handles = mtp_session_->GetObjectHandles(storageId, mtp::ObjectFormat::Any, mtp::Session::Root);
                for (auto handle : handles.ObjectHandles) {
                    auto info = mtp_session_->GetObjectInfo(handle);
                    results.push_back({handle.Id, info.Filename, info.ObjectCompressedSize, info.ObjectFormat == ObjectFormat::Association});
                }
            }
        } else {
            auto handles = mtp_session_->GetObjectHandles(mtp::Session::AllStorages, mtp::ObjectFormat::Any, mtp::ObjectId(parent_handle));
            for (auto handle : handles.ObjectHandles) {
                auto info = mtp_session_->GetObjectInfo(handle);
                results.push_back({handle.Id, info.Filename, info.ObjectCompressedSize, info.ObjectFormat == ObjectFormat::Association});
            }
        }
    } catch (const std::exception& e) {
        Log("Error listing storage: " + std::string(e.what()));
    }
    return results;
}

int LibraryManager::DownloadFile(uint32_t object_handle, const std::string& destination_path) {
    if (!mtp_session_) return -1;
    try {
        // Retrieve album artwork from the RepresentativeSampleData property
        mtp::ByteArray artwork_data = mtp_session_->GetObjectProperty(
            mtp::ObjectId(object_handle),
            mtp::ObjectProperty::RepresentativeSampleData
        );

        // MTP property data has a 4-byte length prefix, skip it to get actual image data
        if (artwork_data.size() < 4) {
            Log("Error: Artwork data too small");
            return -1;
        }

        // Write to file (skip the 4-byte length prefix)
        std::ofstream file(destination_path, std::ios::binary);
        if (!file.is_open()) {
            Log("Error: Could not open file for writing: " + destination_path);
            return -1;
        }
        file.write(reinterpret_cast<const char*>(artwork_data.data() + 4), artwork_data.size() - 4);
        file.close();
    } catch (const std::exception& e) {
        Log("Error downloading file: " + std::string(e.what()));
        return -1;
    }
    return 0;
}

int LibraryManager::UploadFile(const std::string& source_path, const std::string& destination_folder) {
    if (!cli_session_) return -1;
    try {
        cli_session_->Put(source_path, destination_folder);
    } catch (const std::exception& e) {
        Log("Error uploading file: " + std::string(e.what()));
        return -1;
    }
    return 0;
}

int LibraryManager::DeleteFile(uint32_t object_handle) {
    if (!mtp_session_) return -1;
    try {
        mtp_session_->DeleteObject(mtp::ObjectId(object_handle));

        // Invalidate library cache after deletion to ensure fresh ObjectIds on next upload.
        // The device may reorganize album/artist objects after deletions, causing stale
        // ObjectIds in the cached library to produce InvalidObjectHandle errors.
        if (library_) {
            library_.reset();
            library_ = nullptr;
        }
    } catch (const std::exception& e) {
        Log("Error deleting file: " + std::string(e.what()));
        return -1;
    }
    return 0;
}

int LibraryManager::UploadWithArtwork(const std::string& media_path, const std::string& artwork_path) {
    if (!cli_session_) return -1;
    try {
        // NOTE: artwork_path parameter is currently unused
        (void)artwork_path;
        cli_session_->ZuneImport(media_path);
    } catch (const std::exception& e) {
        Log("Error uploading with artwork: " + std::string(e.what()));
        return -1;
    }
    return 0;
}

ZuneMusicLibrary* LibraryManager::GetMusicLibrary(const std::string& device_model) {
    if (!mtp_session_) {
        Log("Error: Not connected to a device.");
        return nullptr;
    }

    try {
        Log("Starting library retrieval using zmdb extraction...");

        // Step 1: Get zmdb binary from device
        std::vector<uint8_t> library_object_id = {0x03, 0x92, 0x1f};
        mtp::ByteArray zmdb_data = GetZuneMetadata(library_object_id);

        if (zmdb_data.empty()) {
            Log("Error: zmdb data is empty");
            return nullptr;
        }

        Log("Retrieved zmdb: " + std::to_string(zmdb_data.size()) + " bytes");

        // Step 2: Parse zmdb
        if (device_model.empty()) {
            Log("Error: Device model is empty");
            return nullptr;
        }

        zmdb::DeviceType device_type = (device_model.find("HD") != std::string::npos)
            ? zmdb::DeviceType::ZuneHD
            : zmdb::DeviceType::Zune30;

        auto parser = zmdb::ZMDBParserFactory::CreateParser(device_type);
        zmdb::ZMDBLibrary library = parser->ExtractLibrary(zmdb_data);

        Log("Extracted " + std::to_string(library.track_count) + " tracks, " +
            std::to_string(library.album_count) + " albums");

        // Step 3: Query MTP for album artwork ObjectIds
        Log("Querying album list for .alb references...");
        std::unordered_map<std::string, uint32_t> alb_to_objectid;

        try {
            mtp::ByteArray album_list = mtp_session_->GetObjectPropertyList(
                mtp::Session::Root,
                mtp::ObjectFormat::AbstractAudioAlbum,
                mtp::ObjectProperty::ObjectFilename,
                0,
                1
            );

            mtp::ObjectStringPropertyListParser::Parse(album_list,
                [&](mtp::ObjectId id, mtp::ObjectProperty property, const std::string &filename) {
                    alb_to_objectid[filename] = id.Id;
                    Log("  Found .alb: " + filename + " -> ObjectId " + std::to_string(id.Id));
                });

            Log("Found " + std::to_string(alb_to_objectid.size()) + " album objects");

        } catch (const std::exception& e) {
            Log("Warning: Could not query album list: " + std::string(e.what()));
        }

        // Step 4: Build flat data structure
        ZuneMusicLibrary* result = new ZuneMusicLibrary();

        // Copy tracks
        result->track_count = library.track_count;
        result->tracks = new ZuneMusicTrack[result->track_count];
        for (uint32_t i = 0; i < library.track_count; i++) {
            const auto& t = library.tracks[i];
            result->tracks[i].title = strdup(t.title.c_str());
            result->tracks[i].artist_name = strdup(t.artist_name.c_str());
            result->tracks[i].artist_guid = strdup(t.artist_guid.c_str());
            result->tracks[i].album_name = strdup(t.album_name.c_str());
            result->tracks[i].album_artist_name = strdup(t.album_artist_name.c_str());
            result->tracks[i].album_artist_guid = strdup(t.album_artist_guid.c_str());
            result->tracks[i].genre = strdup(t.genre.c_str());
            result->tracks[i].filename = strdup(t.filename.c_str());
            result->tracks[i].track_number = t.track_number;
            result->tracks[i].disc_number = t.disc_number;
            result->tracks[i].duration_ms = t.duration_ms;
            result->tracks[i].file_size_bytes = t.file_size_bytes;
            result->tracks[i].album_ref = t.album_ref;
            result->tracks[i].atom_id = t.atom_id;
            result->tracks[i].playcount = t.playcount;
            result->tracks[i].skip_count = t.skip_count;
            result->tracks[i].codec_id = t.codec_id;
            result->tracks[i].rating = t.rating;
            result->tracks[i].last_played_timestamp = t.last_played_timestamp;
        }

        // Copy albums
        result->album_count = library.album_metadata.size();
        result->albums = new ZuneMusicAlbum[result->album_count];
        size_t album_idx = 0;
        for (const auto& [atom_id, album] : library.album_metadata) {
            result->albums[album_idx].title = strdup(album.title.c_str());
            result->albums[album_idx].artist_name = strdup(album.artist_name.c_str());
            result->albums[album_idx].artist_guid = strdup(album.artist_guid.c_str());
            result->albums[album_idx].alb_reference = strdup(album.alb_reference.c_str());
            result->albums[album_idx].release_year = album.release_year;
            result->albums[album_idx].atom_id = album.atom_id;
            result->albums[album_idx].album_pid = album.album_pid;
            result->albums[album_idx].artist_ref = album.artist_ref;
            album_idx++;
        }

        // Copy artists
        result->artist_count = library.artist_metadata.size();
        result->artists = new ZuneMusicArtist[result->artist_count];
        size_t artist_idx = 0;
        for (const auto& [atom_id, artist] : library.artist_metadata) {
            result->artists[artist_idx].name = strdup(artist.name.c_str());
            result->artists[artist_idx].filename = strdup(artist.filename.c_str());
            result->artists[artist_idx].guid = strdup(artist.guid.c_str());
            result->artists[artist_idx].atom_id = artist.atom_id;
            artist_idx++;
        }

        // Build artwork array
        result->artwork_count = alb_to_objectid.size();
        result->artworks = new ZuneAlbumArtwork[result->artwork_count];
        size_t artwork_idx = 0;
        for (const auto& [alb_ref, object_id] : alb_to_objectid) {
            result->artworks[artwork_idx].alb_reference = strdup(alb_ref.c_str());
            result->artworks[artwork_idx].mtp_object_id = object_id;
            artwork_idx++;
        }

        // Copy playlists
        result->playlist_count = library.playlist_count;
        result->playlists = new ZuneMusicPlaylist[result->playlist_count];
        for (uint32_t i = 0; i < library.playlist_count; i++) {
            const auto& p = library.playlists[i];
            result->playlists[i].name = strdup(p.name.c_str());
            result->playlists[i].filename = strdup(p.filename.c_str());
            result->playlists[i].guid = strdup(p.guid.c_str());
            result->playlists[i].folder = strdup(p.folder.c_str());
            result->playlists[i].track_count = p.track_atom_ids.size();
            result->playlists[i].atom_id = p.atom_id;

            // Copy track atom_ids
            if (p.track_atom_ids.size() > 0) {
                result->playlists[i].track_atom_ids = new uint32_t[p.track_atom_ids.size()];
                for (size_t j = 0; j < p.track_atom_ids.size(); j++) {
                    result->playlists[i].track_atom_ids[j] = p.track_atom_ids[j];
                }
            } else {
                result->playlists[i].track_atom_ids = nullptr;
            }
        }

        Log("Library retrieval complete: " + std::to_string(result->track_count) + " tracks, " +
            std::to_string(result->album_count) + " albums, " +
            std::to_string(result->artist_count) + " artists, " +
            std::to_string(result->artwork_count) + " artworks, " +
            std::to_string(result->playlist_count) + " playlists");

        return result;

    } catch (const std::exception& e) {
        Log("Error in library retrieval: " + std::string(e.what()));
        return nullptr;
    }
}

ZuneMusicLibrary* LibraryManager::GetMusicLibrarySlow() {
    ZuneMusicLibrary* result = new ZuneMusicLibrary();
    result->tracks = nullptr;
    result->track_count = 0;
    result->albums = nullptr;
    result->album_count = 0;
    result->artworks = nullptr;
    result->artwork_count = 0;

    if (!mtp_session_) {
        Log("Error: Not connected to a device.");
        return result;
    }

    try {
        Library lib(mtp_session_);

        // Build flat arrays using AFTL enumeration
        std::vector<ZuneMusicTrack> tracks_vec;
        std::vector<ZuneMusicAlbum> albums_vec;
        std::map<std::string, uint32_t> artwork_map;

        uint32_t album_index = 0;

        // Iterate all albums
        for (auto const& [album_key, album_ptr] : lib._albums) {
            ZuneMusicAlbum album;
            album.title = strdup(album_ptr->Name.c_str());
            album.artist_name = strdup(album_ptr->Artist->Name.c_str());
            album.release_year = album_ptr->Year;
            album.alb_reference = strdup(album_key.second.c_str());
            album.atom_id = album_index;

            albums_vec.push_back(album);

            lib.LoadRefs(album_ptr);

            for (auto const& [track_name, track_index] : album_ptr->Tracks) {
                ZuneMusicTrack track;
                track.title = strdup(track_name.c_str());
                track.artist_name = strdup(album_ptr->Artist->Name.c_str());
                track.album_name = strdup(album_ptr->Name.c_str());
                track.genre = strdup("");
                track.track_number = track_index;
                track.duration_ms = 0;
                track.album_ref = album_index;
                track.atom_id = 0;

                for (auto const& ref : album_ptr->Refs) {
                    auto info = mtp_session_->GetObjectInfo(ref);
                    if (info.Filename == track_name) {
                        track.filename = strdup(info.Filename.c_str());
                        track.atom_id = ref.Id;
                        break;
                    }
                }

                if (track.atom_id == 0) {
                    track.filename = strdup(track_name.c_str());
                }

                tracks_vec.push_back(track);
            }

            if (!album_ptr->Refs.empty()) {
                auto first_ref = *album_ptr->Refs.begin();
                artwork_map[album_key.second] = first_ref.Id;
            }

            album_index++;
        }

        result->track_count = tracks_vec.size();
        if (result->track_count > 0) {
            result->tracks = new ZuneMusicTrack[result->track_count];
            std::copy(tracks_vec.begin(), tracks_vec.end(), result->tracks);
        }

        result->album_count = albums_vec.size();
        if (result->album_count > 0) {
            result->albums = new ZuneMusicAlbum[result->album_count];
            std::copy(albums_vec.begin(), albums_vec.end(), result->albums);
        }

        std::vector<ZuneAlbumArtwork> artworks_vec;
        for (const auto& [alb_ref, object_id] : artwork_map) {
            ZuneAlbumArtwork artwork;
            artwork.alb_reference = strdup(alb_ref.c_str());
            artwork.mtp_object_id = object_id;
            artworks_vec.push_back(artwork);
        }

        result->artwork_count = artworks_vec.size();
        if (result->artwork_count > 0) {
            result->artworks = new ZuneAlbumArtwork[result->artwork_count];
            std::copy(artworks_vec.begin(), artworks_vec.end(), result->artworks);
        }

        Log("GetMusicLibrarySlow: Retrieved " + std::to_string(result->track_count) +
            " tracks, " + std::to_string(result->album_count) + " albums (AFTL enumeration)");

    } catch (const std::exception& e) {
        Log("Error getting music library (slow): " + std::string(e.what()));
    }

    return result;
}

std::vector<ZunePlaylistInfo> LibraryManager::GetPlaylists() {
    std::vector<ZunePlaylistInfo> results;
    if (!mtp_session_) {
        Log("Error: Not connected to a device.");
        return results;
    }
    try {
        std::vector<ZuneObjectInfoInternal> all_files;
        std::function<void(uint32_t)> list_recursive = 
            [&](uint32_t parent_handle) {
            auto items = ListStorage(parent_handle);
            for (const auto& item : items) {
                all_files.push_back(item);
                if (item.is_folder) {
                    list_recursive(item.handle);
                }
            }
        };
        list_recursive(0);

        for (const auto& file : all_files) {
            if (file.filename.size() > 4 && file.filename.substr(file.filename.size() - 4) == ".wpl") {
                ZunePlaylistInfo playlist_info;
                playlist_info.Name = strdup(file.filename.c_str());
                playlist_info.MtpObjectId = file.handle;
                playlist_info.TrackCount = 0;
                playlist_info.TrackPaths = nullptr;
                results.push_back(playlist_info);
            }
        }
    } catch (const std::exception& e) {
        Log("Error getting playlists: " + std::string(e.what()));
    }
    return results;
}

// --- Upload Helper Methods ---

LibraryManager::UploadContext LibraryManager::PrepareAudiobookUpload(
    const std::string& audiobook_name,
    const std::string& author_name,
    int year,
    const std::string& track_title,
    int track_number,
    const std::string& filename,
    size_t file_size,
    uint32_t duration_ms,
    mtp::ObjectFormat format
) {
    UploadContext ctx;
    ctx.is_audiobook = true;

    Log("  Getting/creating audiobook: " + audiobook_name);
    ctx.audiobook_ptr = library_->GetAudiobook(audiobook_name);
    if (!ctx.audiobook_ptr) {
        ctx.audiobook_ptr = library_->CreateAudiobook(audiobook_name, author_name, year);
        if (!ctx.audiobook_ptr) {
            throw std::runtime_error("Failed to create audiobook");
        }
    }
    Log("  ✓ Audiobook: " + audiobook_name + " by " + author_name);

    Log("  Creating audiobook track entry...");
    if (duration_ms > 0) {
        Log("  Duration: " + std::to_string(duration_ms) + " ms");
    }
    ctx.track_info = library_->CreateAudiobookTrack(
        ctx.audiobook_ptr, format, track_title, track_number,
        filename, file_size, duration_ms
    );
    Log("  ✓ Audiobook track entry created");

    return ctx;
}

LibraryManager::UploadContext LibraryManager::PrepareMusicUpload(
    const std::string& artist_name,
    const std::string& album_name,
    int album_year,
    const std::string& track_title,
    const std::string& genre,
    int track_number,
    const std::string& filename,
    size_t file_size,
    uint32_t duration_ms,
    mtp::ObjectFormat format,
    const std::string& artist_guid,
    int rating
) {
    UploadContext ctx;
    ctx.is_audiobook = false;

    // Get or create artist
    ctx.artist_ptr = library_->GetArtist(artist_name);
    if (!ctx.artist_ptr) {
        Log("  Creating artist: " + artist_name);
        if (!artist_guid.empty()) {
            Log("  → Registering with Zune GUID for metadata fetching");
        }
        ctx.artist_ptr = library_->CreateArtist(artist_name, artist_guid);
        if (!ctx.artist_ptr) {
            throw std::runtime_error("Failed to create artist");
        }
    } else if (!artist_guid.empty() && ctx.artist_ptr->Guid.empty()) {
        Log("  Artist exists but has no GUID - updating with Zune GUID for metadata fetching");
        library_->UpdateArtistGuid(ctx.artist_ptr, artist_guid);
        Log("  ✓ Artist GUID updated");
    }
    Log("  ✓ Artist: " + artist_name);

    // Get or create album
    ctx.album_ptr = library_->GetAlbum(ctx.artist_ptr, album_name);
    if (!ctx.album_ptr) {
        Log("  Creating album: " + album_name);
        ctx.album_ptr = library_->CreateAlbum(ctx.artist_ptr, album_name, album_year);
        if (!ctx.album_ptr) {
            throw std::runtime_error("Failed to create album");
        }
    }
    Log("  ✓ Album: " + album_name + " (" + std::to_string(album_year) + ")");

    // Register artist GUID with device
    if (!artist_guid.empty()) {
        Log("  Registering artist GUID with device...");
        try {
            library_->ValidateArtistGuid(artist_name, track_title, artist_guid);
            Log("  ✓ Artist GUID registered - device should now recognize artist metadata");
        } catch (const std::exception& e) {
            Log("  Warning: GUID registration failed: " + std::string(e.what()));
            Log("  (Device may not request artist metadata)");
        }
    }

    // Create track entry
    Log("  Creating track entry...");
    if (rating >= 0) {
        Log("  → Including rating: " + std::to_string(rating));
    }
    ctx.track_info = library_->CreateTrack(
        ctx.artist_ptr, ctx.album_ptr, format, track_title, genre,
        track_number, filename, file_size, duration_ms, rating
    );
    Log("  ✓ Track entry created");

    return ctx;
}

void LibraryManager::UploadAudioData(std::shared_ptr<cli::ObjectInputStream> stream) {
    Log("  Uploading audio data...");
    mtp_session_->SendObject(stream);
    Log("  ✓ Audio data uploaded");
}

void LibraryManager::AddArtwork(const UploadContext& ctx, const uint8_t* artwork_data, size_t artwork_size) {
    if (!artwork_data || artwork_size == 0) return;

    mtp::ByteArray artwork(artwork_data, artwork_data + artwork_size);
    if (ctx.is_audiobook) {
        Log("  Adding audiobook track cover...");
        library_->AddAudiobookTrackCover(ctx.track_info.Id, artwork);
        Log("  ✓ Audiobook track cover added");
    } else {
        Log("  Adding album artwork...");
        library_->AddCover(ctx.album_ptr, artwork);
        Log("  ✓ Album artwork added");
    }
}

void LibraryManager::LinkTrackToContainer(const UploadContext& ctx) {
    if (ctx.is_audiobook) {
        Log("  Linking track to audiobook...");
        library_->AddAudiobookTrack(ctx.audiobook_ptr, ctx.track_info);
        Log("  ✓ Track linked to audiobook");
    } else {
        Log("  Linking track to album...");
        library_->AddTrack(ctx.album_ptr, ctx.track_info);
        Log("  ✓ Track linked to album");
    }
}

void LibraryManager::FinalizeUpload(const mtp::Library::NewTrackInfo& track_info) {
    Log("  Synchronizing device database (Operation 0x9217)...");
    try {
        mtp_session_->Operation9217(1);
        Log("  ✓ Database synchronized - device should now recognize new artist");
    } catch (const std::exception& e) {
        Log("  Warning: Database sync failed: " + std::string(e.what()));
        Log("  (Device may not request artist metadata)");
    }

    Log("  Executing post-upload metadata trigger sequence...");
    try {
        Log("    Using track handle: 0x" + std::to_string(track_info.Id.Id));
        Log("    Querying track properties...");
        mtp_session_->Operation9802(0xDC44, track_info.Id.Id);  // Name
        Log("    ✓ Track properties queried");
        Log("    ✓ Post-upload metadata trigger sequence complete");
    } catch (const std::exception& e) {
        Log("    Warning: Post-upload sequence failed: " + std::string(e.what()));
    }
}

// --- Main Upload Method ---

int LibraryManager::UploadTrackWithMetadata(
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
    const std::string& artist_guid,
    uint32_t duration_ms,
    int rating,
    uint32_t* out_track_id,
    uint32_t* out_album_id,
    uint32_t* out_artist_id
) {
    if (!mtp_session_) {
        Log("Error: Not connected to device");
        return -1;
    }

    bool is_audiobook = (media_type == MediaType::Audiobook);

    try {
        Log(is_audiobook
            ? "Uploading audiobook track: " + track_title + " by " + artist_name
            : "Uploading track: " + track_title + " by " + artist_name);
        if (!artist_guid.empty()) {
            Log("  Artist GUID: " + artist_guid);
        }

        EnsureLibraryInitialized();

        // Open audio file
        auto stream = std::make_shared<cli::ObjectInputStream>(audio_file_path);
        stream->SetTotal(stream->GetSize());
        Log("  ✓ Audio file opened: " + std::to_string(stream->GetSize()) + " bytes");

        auto slashpos = audio_file_path.rfind('/');
        auto filename = slashpos != audio_file_path.npos
            ? audio_file_path.substr(slashpos + 1) : audio_file_path;
        mtp::ObjectFormat format = mtp::ObjectFormatFromFilename(audio_file_path);

        // Prepare upload (create entities)
        UploadContext ctx = is_audiobook
            ? PrepareAudiobookUpload(album_name, artist_name, album_year, track_title,
                                     track_number, filename, stream->GetSize(), duration_ms, format)
            : PrepareMusicUpload(artist_name, album_name, album_year, track_title, genre,
                                 track_number, filename, stream->GetSize(), duration_ms, format, artist_guid, rating);

        // Upload, add artwork, link, and finalize
        UploadAudioData(stream);
        AddArtwork(ctx, artwork_data, artwork_size);
        LinkTrackToContainer(ctx);
        Log(is_audiobook ? "✓ Audiobook track uploaded successfully" : "✓ Track uploaded successfully");
        FinalizeUpload(ctx.track_info);

        // Set output parameters
        if (out_track_id) {
            *out_track_id = ctx.track_info.Id.Id;
        }
        if (out_album_id) {
            *out_album_id = ctx.is_audiobook && ctx.audiobook_ptr
                ? ctx.audiobook_ptr->Id.Id
                : (ctx.album_ptr ? ctx.album_ptr->Id.Id : 0);
        }
        if (out_artist_id) {
            *out_artist_id = ctx.artist_ptr ? ctx.artist_ptr->Id.Id : 0;
        }

        return 0;

    } catch (const std::exception& e) {
        Log("Error uploading track: " + std::string(e.what()));
        return -1;
    }
}

int LibraryManager::RetrofitArtistGuid(
    const std::string& artist_name,
    const std::string& guid
) {
    if (!mtp_session_) {
        Log("Error: Not connected to device");
        return -1;
    }

    if (guid.empty()) {
        Log("Error: GUID is empty");
        return -1;
    }

    if (!IsValidGuid(guid)) {
        Log("Error: Invalid GUID format: " + guid);
        Log("Expected format: xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx (hexadecimal)");
        return -1;
    }

    try {
        Log("=== Retrofit Artist GUID (Delete/Recreate Approach) ===");
        Log("Artist: " + artist_name);
        Log("GUID: " + guid);

        EnsureLibraryInitialized();

        auto artist = library_->GetArtist(artist_name);
        if (!artist) {
            Log("Error: Artist not found: " + artist_name);
            return -1;
        }

        bool hasValidGuid = !artist->Guid.empty();
        if (hasValidGuid) {
            hasValidGuid = false;
            for (unsigned char byte : artist->Guid) {
                if (byte != 0) {
                    hasValidGuid = true;
                    break;
                }
            }
        }

        if (hasValidGuid) {
            Log("Artist already has valid GUID - no retrofit needed");
            return 0;
        }

        Log("Original artist object ID: 0x" + std::to_string(artist->Id.Id));

        Log("Finding albums by artist...");
        auto albums = library_->GetAlbumsByArtist(artist);
        Log("Found " + std::to_string(albums.size()) + " albums");

        std::vector<std::vector<mtp::ObjectId>> album_tracks;
        for (auto& album : albums) {
            Log("  Album: " + album->Name);
            auto tracks = library_->GetTracksForAlbum(album);
            Log("    Tracks: " + std::to_string(tracks.size()));
            album_tracks.push_back(tracks);
        }

        Log("Creating new artist object with GUID...");
        auto new_artist = library_->CreateArtist(artist_name, guid);
        Log("✓ New artist object created (ID: 0x" + std::to_string(new_artist->Id.Id) + ")");

        Log("Updating album references to new artist...");
        for (size_t i = 0; i < albums.size(); ++i) {
            auto& album = albums[i];
            Log("  Updating album: " + album->Name);
            library_->UpdateAlbumArtist(album, new_artist);

            Log("  Updating " + std::to_string(album_tracks[i].size()) + " tracks");
            for (auto track_id : album_tracks[i]) {
                library_->UpdateTrackArtist(track_id, new_artist);
            }
        }

        Log("Deleting old artist object...");
        mtp_session_->DeleteObject(artist->Id);
        Log("✓ Old artist object deleted");

        Log("✓ Artist retrofit complete!");
        Log("Artist recreated with GUID, all album/track references updated");

        Log("Invalidating library cache to force reload after retrofit");
        library_.reset();
        library_ = nullptr;

        return 0;

    } catch (const std::exception& e) {
        Log("Error during artist retrofit: " + std::string(e.what()));
        return -1;
    }
}

LibraryManager::BatchRetrofitResult LibraryManager::RetrofitMultipleArtistGuids(
    const std::vector<ArtistGuidMapping>& mappings)
{
    BatchRetrofitResult result = {0, 0, 0, 0};

    if (mappings.empty()) {
        Log("Batch retrofit: No artists provided");
        return result;
    }

    if (!mtp_session_) {
        Log("Error: Not connected to device");
        result.error_count = mappings.size();
        return result;
    }

    Log("=== Starting Batch Artist GUID Retrofit ===");
    Log("Processing " + std::to_string(mappings.size()) + " artists");

    try {
        EnsureLibraryInitialized();

        for (const auto& mapping : mappings) {
            const std::string& artist_name = mapping.artist_name;
            const std::string& guid = mapping.guid;

            Log("Artist: " + artist_name);
            Log("GUID: " + guid);

            if (!IsValidGuid(guid)) {
                result.error_count++;
                Log("  ✗ Invalid GUID format: " + guid);
                Log("    Expected format: xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx (hexadecimal)");
                continue;
            }

            try {
                auto artist = library_->GetArtist(artist_name);

                if (!artist) {
                    result.not_found_count++;
                    Log("  Artist '" + artist_name + "' not found on device (will be created during upload)");
                    continue;
                }

                bool hasValidGuid = !artist->Guid.empty();
                if (hasValidGuid) {
                    hasValidGuid = false;
                    for (unsigned char byte : artist->Guid) {
                        if (byte != 0) {
                            hasValidGuid = true;
                            break;
                        }
                    }
                }

                if (hasValidGuid) {
                    result.already_had_guid_count++;
                    Log("  Artist '" + artist_name + "' already has valid GUID - no retrofit needed");
                    continue;
                }

                Log("  Original artist object ID: 0x" + std::to_string(artist->Id.Id));

                Log("  Finding albums by artist...");
                auto albums = library_->GetAlbumsByArtist(artist);
                Log("  Found " + std::to_string(albums.size()) + " albums");

                std::vector<std::vector<mtp::ObjectId>> album_tracks;
                for (auto& album : albums) {
                    Log("    Album: " + album->Name);
                    auto tracks = library_->GetTracksForAlbum(album);
                    Log("      Tracks: " + std::to_string(tracks.size()));
                    album_tracks.push_back(tracks);
                }

                Log("  Creating new artist object with GUID...");
                auto new_artist = library_->CreateArtist(artist_name, guid);
                Log("  ✓ New artist object created (ID: 0x" + std::to_string(new_artist->Id.Id) + ")");

                Log("  Updating album references to new artist...");
                for (size_t i = 0; i < albums.size(); ++i) {
                    auto& album = albums[i];
                    Log("    Updating album: " + album->Name);
                    library_->UpdateAlbumArtist(album, new_artist);

                    Log("    Updating " + std::to_string(album_tracks[i].size()) + " tracks");
                    for (auto track_id : album_tracks[i]) {
                        library_->UpdateTrackArtist(track_id, new_artist);
                    }
                }

                Log("  Deleting old artist object...");
                mtp_session_->DeleteObject(artist->Id);
                Log("  ✓ Old artist object deleted");

                result.retrofitted_count++;
                Log("  ✓ Artist '" + artist_name + "' retrofitted successfully");

            } catch (const std::exception& e) {
                result.error_count++;
                Log("  ✗ Error retrofitting artist '" + artist_name + "': " + std::string(e.what()));
            }
        }

        if (result.retrofitted_count > 0) {
            Log("Invalidating library cache after " +
                std::to_string(result.retrofitted_count) + " retrofits");
            library_.reset();
            library_ = nullptr;
        }

        Log("=== Batch Retrofit Complete ===");
        Log("Results: " +
            std::to_string(result.retrofitted_count) + " retrofitted, " +
            std::to_string(result.already_had_guid_count) + " already had GUID, " +
            std::to_string(result.not_found_count) + " not found, " +
            std::to_string(result.error_count) + " errors");

        return result;

    } catch (const std::exception& e) {
        Log("Fatal error during batch retrofit: " + std::string(e.what()));
        result.error_count = mappings.size();
        return result;
    }
}

mtp::ByteArray LibraryManager::GetPartialObject(uint32_t object_id, uint64_t offset, uint32_t size) {
    if (!mtp_session_) {
        Log("Error: Not connected to device");
        return mtp::ByteArray();
    }

    try {
        return mtp_session_->GetPartialObject(mtp::ObjectId(object_id), offset, size);
    } catch (const std::exception& e) {
        Log("Error reading partial object: " + std::string(e.what()));
        return mtp::ByteArray();
    }
}

uint64_t LibraryManager::GetObjectSize(uint32_t object_id) {
    if (!mtp_session_) {
        Log("Error: Not connected to device");
        return 0;
    }

    try {
        return mtp_session_->GetObjectIntegerProperty(
            mtp::ObjectId(object_id),
            mtp::ObjectProperty::ObjectSize
        );
    } catch (const std::exception& e) {
        Log("Error getting object size: " + std::string(e.what()));
        return 0;
    }
}

std::string LibraryManager::GetObjectFilename(uint32_t object_id) {
    if (!mtp_session_) {
        Log("Error: Not connected to device");
        return "";
    }

    try {
        auto info = mtp_session_->GetObjectInfo(mtp::ObjectId(object_id));
        return info.Filename;
    } catch (const std::exception& e) {
        Log("Error getting object filename: " + std::string(e.what()));
        return "";
    }
}

uint32_t LibraryManager::GetAudioTrackObjectId(const std::string& track_title, uint32_t album_object_id) {
    if (!mtp_session_) {
        Log("Error: Not connected to device");
        return 0;
    }

    if (track_title.empty()) {
        Log("Error: track_title is empty");
        return 0;
    }

    if (album_object_id == 0) {
        Log("Error: Album ObjectId is required (must be > 0). Cannot search for track without album context.");
        return 0;
    }

    std::string cache_key = std::to_string(album_object_id) + ":" + track_title;
    {
        std::lock_guard<std::mutex> lock(track_cache_mutex_);
        auto it = track_objectid_cache_.find(cache_key);
        if (it != track_objectid_cache_.end()) {
            Log("Cache hit: Track '" + track_title + "' -> ObjectId " + std::to_string(it->second));
            return it->second;
        }
    }

    try {
        Log("Searching for audio track: '" + track_title + "' in album ObjectId: " + std::to_string(album_object_id));

        Log("Querying object references for album ObjectId " + std::to_string(album_object_id) + "...");

        auto object_refs = mtp_session_->GetObjectReferences(mtp::ObjectId(album_object_id));

        Log("Found " + std::to_string(object_refs.ObjectHandles.size()) + " track references in album");

        uint32_t found_id = 0;

        for (const auto& handle : object_refs.ObjectHandles) {
            try {
                std::string file_name = mtp_session_->GetObjectStringProperty(
                    handle,
                    mtp::ObjectProperty::Name
                );

                size_t dot_pos = file_name.rfind('.');
                std::string track_name = (dot_pos != std::string::npos && dot_pos > 0)
                    ? file_name.substr(0, dot_pos)
                    : file_name;

                std::string track_cache_key = std::to_string(album_object_id) + ":" + track_name;
                {
                    std::lock_guard<std::mutex> lock(track_cache_mutex_);
                    track_objectid_cache_[track_cache_key] = handle.Id;
                }

                if (track_name == track_title) {
                    found_id = handle.Id;
                    Log("Matched track '" + track_title + "' to ObjectId " + std::to_string(handle.Id));
                }

            } catch (const std::exception& e) {
                Log("  Error reading Name property for track " + std::to_string(handle.Id) + ": " + std::string(e.what()));
                continue;
            }
        }

        if (found_id == 0) {
            Log("Audio track '" + track_title + "' not found in album ObjectId " + std::to_string(album_object_id));
        } else {
            Log("Cached " + std::to_string(object_refs.ObjectHandles.size()) + " tracks from album " + std::to_string(album_object_id));
        }
        return found_id;

    } catch (const std::exception& e) {
        Log("Error querying for audio track '" + track_title + "': " + std::string(e.what()));
        return 0;
    }
}

void LibraryManager::ClearTrackObjectIdCache() {
    std::lock_guard<std::mutex> lock(track_cache_mutex_);
    track_objectid_cache_.clear();
    Log("Track ObjectId cache cleared");
}

mtp::ByteArray LibraryManager::GetZuneMetadata(const std::vector<uint8_t>& object_id) {
    mtp::ByteArray result;
    if (!mtp_session_) {
        Log("Error: Not connected to a device.");
        return result;
    }

    try {
        mtp::ByteArray request_data(ZMDB_REQUEST_SIZE, 0);
        request_data[0] = ZMDB_REQUEST_LENGTH_BYTE;
        request_data[1] = 0x00;
        request_data[2] = 0x00;
        request_data[3] = 0x00;
        request_data[4] = ZMDB_COMMAND_MARKER;
        request_data[5] = 0x00;
        request_data[6] = ZMDB_OPERATION_CODE_HIGH;
        request_data[7] = ZMDB_OPERATION_CODE_LOW;
        for (size_t i = 0; i < object_id.size() && i < ZMDB_OBJECT_ID_SIZE; ++i) {
            request_data[8 + i] = object_id[i];
        }
        request_data[11] = 0x00;
        request_data[12] = ZMDB_TRAILER_VALUE;
        request_data[13] = 0x00;
        request_data[14] = 0x00;
        request_data[15] = 0x00;

        {
            std::ostringstream oss;
            oss << "Sending request bytes (hex): ";
            for (size_t i = 0; i < request_data.size(); ++i) {
                if (i > 0) oss << " ";
                oss << std::hex << std::setfill('0') << std::setw(2) << (int)request_data[i];
            }
            oss << std::dec;
            Log(oss.str());
        }

        auto pipe = mtp_session_->GetBulkPipe();
        if (!pipe) {
            Log("Error: Cannot access USB pipe");
            return result;
        }

        auto inputStream = std::make_shared<ByteArrayInputStream>(request_data);
        pipe->Write(inputStream, mtp::Session::DefaultTimeout);
        Log("Zune metadata request sent");

        std::this_thread::sleep_for(std::chrono::milliseconds(ZMDB_DEVICE_PREPARE_DELAY_MS));

        auto headerStream = std::make_shared<ByteArrayOutputStream>();
        pipe->Read(headerStream, mtp::Session::DefaultTimeout);

        if (headerStream->data.size() == 0) {
            Log("Error: No response received from device");
            return result;
        }

        Log("Response header received: " + std::to_string(headerStream->data.size()) + " bytes");

        if (headerStream->data.size() > 0) {
            std::ostringstream oss;
            oss << "Response header bytes (hex): ";
            for (size_t i = 0; i < headerStream->data.size(); ++i) {
                if (i > 0) oss << " ";
                oss << std::hex << std::setfill('0') << std::setw(2) << (int)headerStream->data[i];
            }
            oss << std::dec;
            Log(oss.str());
        }

        uint32_t total_size = 0;
        if (headerStream->data.size() >= 4) {
            total_size = headerStream->data[0] |
                        (headerStream->data[1] << 8) |
                        (headerStream->data[2] << 16) |
                        (headerStream->data[3] << 24);
            Log("Total size from header: " + std::to_string(total_size) + " bytes");
        }

        if (headerStream->data.size() == ZMDB_HEADER_SIZE && total_size <= ZMDB_HEADER_SIZE) {
            Log("Response is header-only (" + std::to_string(ZMDB_HEADER_SIZE) + " bytes)");
            result = headerStream->data;
            return result;
        }

        if (total_size > ZMDB_HEADER_SIZE) {
            Log("Reading metadata payload...");
            auto payloadStream = std::make_shared<ByteArrayOutputStream>();
            pipe->Read(payloadStream, mtp::Session::LongTimeout);
            Log("Metadata payload received: " + std::to_string(payloadStream->data.size()) + " bytes");

            result = payloadStream->data;
            Log("Returning ZMDB payload: " + std::to_string(result.size()) + " bytes (header stripped)");

            Log("Draining pipe to clear any remaining data...");
            try {
                auto drainStream = std::make_shared<ByteArrayOutputStream>();
                pipe->Read(drainStream, ZMDB_PIPE_DRAIN_TIMEOUT_MS);
                if (drainStream->data.size() > 0) {
                    Log("Drained " + std::to_string(drainStream->data.size()) + " bytes from pipe");
                }
            } catch (const std::exception& e) {
                Log("Pipe drain completed (timeout is expected)");
            }
        }

    } catch (const std::exception& e) {
        Log("Error during Zune metadata transfer: " + std::string(e.what()));
    }

    return result;
}

// --- Playlist Management ---

mtp::ByteArray LibraryManager::GuidStringToBytes(const std::string& guid_str) {
    // Convert GUID string "xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx" to 16-byte array
    // Uses mixed-endian format (same as Windows GUID):
    // - First 3 components: little-endian
    // - Last 2 components (8 bytes): big-endian
    mtp::ByteArray guid(16, 0);

    // Remove dashes and validate
    std::string hex;
    for (char c : guid_str) {
        if (c != '-') {
            hex += c;
        }
    }

    if (hex.length() != 32) {
        Log("Error: Invalid GUID format (expected 32 hex chars, got " + std::to_string(hex.length()) + ")");
        return guid;
    }

    // Component 1: 4 bytes (32-bit) - little-endian
    for (int i = 3; i >= 0; --i) {
        guid[3 - i] = static_cast<mtp::u8>(std::stoul(hex.substr(i * 2, 2), nullptr, 16));
    }

    // Component 2: 2 bytes (16-bit) - little-endian
    for (int i = 1; i >= 0; --i) {
        guid[4 + (1 - i)] = static_cast<mtp::u8>(std::stoul(hex.substr(8 + i * 2, 2), nullptr, 16));
    }

    // Component 3: 2 bytes (16-bit) - little-endian
    for (int i = 1; i >= 0; --i) {
        guid[6 + (1 - i)] = static_cast<mtp::u8>(std::stoul(hex.substr(12 + i * 2, 2), nullptr, 16));
    }

    // Component 4: 8 bytes - big-endian (as-is)
    for (size_t i = 0; i < 8; ++i) {
        guid[8 + i] = static_cast<mtp::u8>(std::stoul(hex.substr(16 + i * 2, 2), nullptr, 16));
    }

    return guid;
}

mtp::ObjectId LibraryManager::GetOrCreatePlaylistsFolder() {
    // Find or create the "Playlists" folder on the device
    if (!mtp_session_) {
        throw std::runtime_error("MTP session not initialized");
    }

    auto storageIds = mtp_session_->GetStorageIDs();
    if (storageIds.StorageIDs.empty()) {
        throw std::runtime_error("No storage found on device");
    }
    auto storage = storageIds.StorageIDs.front();

    // Search for existing "Playlists" folder in root
    auto handles = mtp_session_->GetObjectHandles(storage, mtp::ObjectFormat::Association, mtp::Session::Root);
    for (auto id : handles.ObjectHandles) {
        auto filename = mtp_session_->GetObjectStringProperty(id, mtp::ObjectProperty::ObjectFilename);
        if (filename == "Playlists") {
            Log("Found existing Playlists folder: ObjectId 0x" + std::to_string(id.Id));
            return id;
        }
    }

    // Create Playlists folder
    Log("Creating Playlists folder...");
    auto response = mtp_session_->CreateDirectory("Playlists", mtp::Session::Root, storage);
    Log("Created Playlists folder: ObjectId 0x" + std::to_string(response.ObjectId.Id));
    return response.ObjectId;
}

uint32_t LibraryManager::CreatePlaylist(
    const std::string& name,
    const std::string& guid,
    const std::vector<uint32_t>& track_mtp_ids
) {
    if (!mtp_session_) {
        Log("Error: Not connected to device");
        return 0;
    }

    try {
        Log("Creating playlist: " + name);

        // Get storage
        auto storageIds = mtp_session_->GetStorageIDs();
        if (storageIds.StorageIDs.empty()) {
            Log("Error: No storage found");
            return 0;
        }
        auto storage = storageIds.StorageIDs.front();

        // Get or create Playlists folder
        auto playlistsFolder = GetOrCreatePlaylistsFolder();

        // Convert GUID string to bytes
        auto guidBytes = GuidStringToBytes(guid);

        // Build property list (4 properties)
        mtp::ByteArray propList;
        mtp::OutputStream os(propList);

        os.Write32(4); // 4 properties

        // Property 1: 0xDAB0 (Zune_CollectionID) = 0 (Uint8)
        os.Write32(0); // object handle
        os.Write16(0xDAB0);
        os.Write16(static_cast<mtp::u16>(mtp::DataTypeCode::Uint8));
        os.Write8(0);

        // Property 2: ObjectFilename (0xDC07) = "{name}.pla"
        os.Write32(0); // object handle
        os.Write16(static_cast<mtp::u16>(mtp::ObjectProperty::ObjectFilename));
        os.Write16(static_cast<mtp::u16>(mtp::DataTypeCode::String));
        os.WriteString(name + ".pla");

        // Property 3: ContentTypeUUID (0xDA97) = GUID bytes (Uint128)
        os.Write32(0); // object handle
        os.Write16(0xDA97);
        os.Write16(static_cast<mtp::u16>(mtp::DataTypeCode::Uint128));
        for (const auto& byte : guidBytes) {
            os.Write8(byte);
        }

        // Property 4: Name (0xDC44) = "{name}"
        os.Write32(0); // object handle
        os.Write16(static_cast<mtp::u16>(mtp::ObjectProperty::Name));
        os.Write16(static_cast<mtp::u16>(mtp::DataTypeCode::String));
        os.WriteString(name);

        // Send object property list
        Log("Sending playlist object properties...");
        auto response = mtp_session_->SendObjectPropList(
            storage,
            playlistsFolder,
            mtp::ObjectFormat::AbstractAVPlaylist,
            0,
            propList
        );
        auto playlistId = response.ObjectId;
        Log("Playlist object created: ObjectId 0x" + std::to_string(playlistId.Id));

        // Send empty object data (required by MTP protocol)
        mtp::ByteArray empty_data;
        auto empty_stream = std::make_shared<ByteArrayInputStream>(empty_data);
        mtp_session_->SendObject(empty_stream);
        Log("Empty object data sent");

        // Set object references (track IDs)
        if (!track_mtp_ids.empty()) {
            Log("Setting " + std::to_string(track_mtp_ids.size()) + " track references...");
            mtp::msg::ObjectHandles handles;
            for (uint32_t track_id : track_mtp_ids) {
                handles.ObjectHandles.push_back(mtp::ObjectId(track_id));
            }
            mtp_session_->SetObjectReferences(playlistId, handles);
            Log("Track references set");
        }

        Log("✓ Playlist created successfully: " + name);
        return playlistId.Id;

    } catch (const std::exception& e) {
        Log("Error creating playlist: " + std::string(e.what()));
        return 0;
    }
}

bool LibraryManager::UpdatePlaylistTracks(
    uint32_t playlist_mtp_id,
    const std::vector<uint32_t>& track_mtp_ids
) {
    if (!mtp_session_) {
        Log("Error: Not connected to device");
        return false;
    }

    try {
        Log("Updating playlist tracks for ObjectId 0x" + std::to_string(playlist_mtp_id));

        // Build object handles from track IDs
        mtp::msg::ObjectHandles handles;
        for (uint32_t track_id : track_mtp_ids) {
            handles.ObjectHandles.push_back(mtp::ObjectId(track_id));
        }

        // Set object references (replaces entire track list)
        mtp_session_->SetObjectReferences(mtp::ObjectId(playlist_mtp_id), handles);

        Log("✓ Updated playlist with " + std::to_string(track_mtp_ids.size()) + " tracks");
        return true;

    } catch (const std::exception& e) {
        Log("Error updating playlist tracks: " + std::string(e.what()));
        return false;
    }
}

bool LibraryManager::DeletePlaylist(uint32_t playlist_mtp_id) {
    if (!mtp_session_) {
        Log("Error: Not connected to device");
        return false;
    }

    try {
        Log("Deleting playlist ObjectId 0x" + std::to_string(playlist_mtp_id));

        mtp_session_->DeleteObject(mtp::ObjectId(playlist_mtp_id));

        Log("✓ Playlist deleted");
        return true;

    } catch (const std::exception& e) {
        Log("Error deleting playlist: " + std::string(e.what()));
        return false;
    }
}
