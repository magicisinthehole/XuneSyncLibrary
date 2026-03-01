#include "ZuneMtpReader.h"
#include "zmdb/ZMDBParserFactory.h"
#include <mtp/ptp/ObjectPropertyListParser.h>
#include <mtp/ptp/ByteArrayObjectStream.h>
#include <fstream>
#include <sstream>
#include <iomanip>
#include <thread>
#include <chrono>
#include <memory>

using namespace mtp;

// --- ZMDB Bulk Pipe Protocol Constants ---
static constexpr size_t ZMDB_REQUEST_SIZE = 16;
static constexpr uint8_t ZMDB_REQUEST_LENGTH_BYTE = 0x10;
static constexpr uint8_t ZMDB_COMMAND_MARKER = 0x01;
static constexpr uint8_t ZMDB_OPERATION_CODE_HIGH = 0x17;
static constexpr uint8_t ZMDB_OPERATION_CODE_LOW = 0x92;
static constexpr uint8_t ZMDB_OBJECT_ID_SIZE = 3;
static constexpr uint8_t ZMDB_TRAILER_VALUE = 0x01;
static constexpr size_t ZMDB_HEADER_SIZE = 12;
static constexpr int ZMDB_DEVICE_PREPARE_DELAY_MS = 250;
static constexpr int ZMDB_PIPE_DRAIN_TIMEOUT_MS = 100;

namespace zune {

// ── Object Properties ────────────────────────────────────────────────────

uint64_t MtpReader::GetObjectSize(const SessionPtr& session, uint32_t object_id) {
    try {
        return session->GetObjectIntegerProperty(
            mtp::ObjectId(object_id), mtp::ObjectProperty::ObjectSize);
    } catch (...) {
        return 0;
    }
}

std::string MtpReader::GetObjectFilename(const SessionPtr& session, uint32_t object_id) {
    try {
        auto info = session->GetObjectInfo(mtp::ObjectId(object_id));
        return info.Filename;
    } catch (...) {
        return "";
    }
}

// ── Streaming / Partial Downloads ────────────────────────────────────────

mtp::ByteArray MtpReader::GetPartialObject(
    const SessionPtr& session, uint32_t object_id,
    uint64_t offset, uint32_t size)
{
    try {
        return session->GetPartialObject(mtp::ObjectId(object_id), offset, size);
    } catch (...) {
        return mtp::ByteArray();
    }
}

// ── Artwork Download ─────────────────────────────────────────────────────

int MtpReader::DownloadArtwork(
    const SessionPtr& session, uint32_t object_handle,
    const std::string& destination_path)
{
    try {
        mtp::ByteArray artwork_data = session->GetObjectProperty(
            mtp::ObjectId(object_handle),
            mtp::ObjectProperty::RepresentativeSampleData);

        // MTP property data has a 4-byte length prefix
        if (artwork_data.size() < 4)
            return -1;

        std::ofstream file(destination_path, std::ios::binary);
        if (!file.is_open())
            return -1;

        file.write(reinterpret_cast<const char*>(artwork_data.data() + 4),
                   artwork_data.size() - 4);
        return 0;
    } catch (...) {
        return -1;
    }
}

// ── Track Lookup ─────────────────────────────────────────────────────────

uint32_t MtpReader::FindTrackObjectId(
    const SessionPtr& session,
    const std::string& track_title,
    uint32_t album_object_id,
    std::vector<TrackReference>* siblings_out)
{
    if (track_title.empty() || album_object_id == 0)
        return 0;

    try {
        auto object_refs = session->GetObjectReferences(mtp::ObjectId(album_object_id));
        uint32_t found_id = 0;

        for (const auto& handle : object_refs.ObjectHandles) {
            try {
                std::string file_name = session->GetObjectStringProperty(
                    handle, mtp::ObjectProperty::Name);

                // Strip file extension
                size_t dot_pos = file_name.rfind('.');
                std::string track_name = (dot_pos != std::string::npos && dot_pos > 0)
                    ? file_name.substr(0, dot_pos)
                    : file_name;

                if (siblings_out)
                    siblings_out->push_back({track_name, handle.Id});

                if (track_name == track_title)
                    found_id = handle.Id;
            } catch (...) {
                continue;
            }
        }

        return found_id;
    } catch (...) {
        return 0;
    }
}

// ── ZMDB (Zune Metadata Database) ────────────────────────────────────────

mtp::ByteArray MtpReader::ReadZuneMetadata(
    const SessionPtr& session,
    const std::vector<uint8_t>& object_id)
{
    mtp::ByteArray result;

    try {
        // Build 16-byte request
        mtp::ByteArray request_data(ZMDB_REQUEST_SIZE, 0);
        request_data[0] = ZMDB_REQUEST_LENGTH_BYTE;
        request_data[4] = ZMDB_COMMAND_MARKER;
        request_data[6] = ZMDB_OPERATION_CODE_HIGH;
        request_data[7] = ZMDB_OPERATION_CODE_LOW;
        for (size_t i = 0; i < object_id.size() && i < ZMDB_OBJECT_ID_SIZE; ++i)
            request_data[8 + i] = object_id[i];
        request_data[12] = ZMDB_TRAILER_VALUE;

        auto pipe = session->GetBulkPipe();
        if (!pipe)
            return result;

        // Send request
        auto input = std::make_shared<mtp::ByteArrayObjectInputStream>(request_data);
        pipe->Write(input, mtp::Session::DefaultTimeout);

        // Wait for device to prepare response
        std::this_thread::sleep_for(std::chrono::milliseconds(ZMDB_DEVICE_PREPARE_DELAY_MS));

        // Read header
        auto header = std::make_shared<mtp::ByteArrayObjectOutputStream>();
        pipe->Read(header, mtp::Session::DefaultTimeout);

        if (header->GetData().empty())
            return result;

        // Parse total size from header
        uint32_t total_size = 0;
        if (header->GetData().size() >= 4) {
            total_size = header->GetData()[0] |
                        (header->GetData()[1] << 8) |
                        (header->GetData()[2] << 16) |
                        (header->GetData()[3] << 24);
        }

        // Header-only response
        if (header->GetData().size() == ZMDB_HEADER_SIZE && total_size <= ZMDB_HEADER_SIZE)
            return header->GetData();

        // Read payload
        if (total_size > ZMDB_HEADER_SIZE) {
            auto payload = std::make_shared<mtp::ByteArrayObjectOutputStream>();
            pipe->Read(payload, mtp::Session::LongTimeout);
            result = payload->GetData();

            // Drain pipe
            try {
                auto drain = std::make_shared<mtp::ByteArrayObjectOutputStream>();
                pipe->Read(drain, ZMDB_PIPE_DRAIN_TIMEOUT_MS);
            } catch (...) {}
        }
    } catch (...) {}

    return result;
}

// ── Full Library Read ────────────────────────────────────────────────────

ZuneMusicLibrary* MtpReader::ReadMusicLibrary(
    const SessionPtr& session,
    zune::DeviceFamily device_family)
{
    try {
        // Step 1: Read ZMDB binary from device
        std::vector<uint8_t> library_object_id = {0x03, 0x92, 0x1f};
        mtp::ByteArray zmdb_data = ReadZuneMetadata(session, library_object_id);

        if (zmdb_data.empty())
            return nullptr;

        // Step 2: Parse ZMDB
        auto parser = zmdb::ZMDBParserFactory::CreateParser(device_family);
        zmdb::ZMDBLibrary library = parser->ExtractLibrary(zmdb_data);
        library.device_family = device_family;

        // Step 3: Query MTP for album artwork ObjectIds
        std::unordered_map<std::string, uint32_t> alb_to_objectid;
        try {
            mtp::ByteArray album_list = session->GetObjectPropertyList(
                mtp::Session::Root,
                mtp::ObjectFormat::AbstractAudioAlbum,
                mtp::ObjectProperty::ObjectFilename,
                0, 1);

            mtp::ObjectStringPropertyListParser::Parse(album_list,
                [&](mtp::ObjectId id, mtp::ObjectProperty, const std::string& filename) {
                    alb_to_objectid[filename] = id.Id;
                });
        } catch (...) {}

        // Step 4: Build flat C data structure (zero-initialized for safe partial cleanup)
        auto result = std::unique_ptr<ZuneMusicLibrary, decltype(&MtpReader::FreeLibrary)>(
            new ZuneMusicLibrary{}, &MtpReader::FreeLibrary);

        // Copy tracks
        result->track_count = library.track_count;
        result->tracks = new ZuneMusicTrack[result->track_count]{};
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
            result->tracks[i].genre_ref = t.genre_ref;
            result->tracks[i].playcount = t.playcount;
            result->tracks[i].skip_count = t.skip_count;
            result->tracks[i].codec_id = t.codec_id;
            result->tracks[i].rating = t.rating;
            result->tracks[i].last_played_timestamp = t.last_played_timestamp;
        }

        // Copy albums
        result->album_count = library.album_metadata.size();
        result->albums = new ZuneMusicAlbum[result->album_count]{};
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
        result->artists = new ZuneMusicArtist[result->artist_count]{};
        size_t artist_idx = 0;
        for (const auto& [atom_id, artist] : library.artist_metadata) {
            result->artists[artist_idx].name = strdup(artist.name.c_str());
            result->artists[artist_idx].filename = strdup(artist.filename.c_str());
            result->artists[artist_idx].guid = strdup(artist.guid.c_str());
            result->artists[artist_idx].atom_id = artist.atom_id;
            artist_idx++;
        }

        // Copy genres
        result->genre_count = library.genre_metadata.size();
        result->genres = new ZuneMusicGenre[result->genre_count]{};
        size_t genre_idx = 0;
        for (const auto& [atom_id, genre] : library.genre_metadata) {
            result->genres[genre_idx].name = strdup(genre.name.c_str());
            result->genres[genre_idx].atom_id = genre.atom_id;
            genre_idx++;
        }

        // Build artwork array
        result->artwork_count = alb_to_objectid.size();
        result->artworks = new ZuneAlbumArtwork[result->artwork_count]{};
        size_t artwork_idx = 0;
        for (const auto& [alb_ref, object_id] : alb_to_objectid) {
            result->artworks[artwork_idx].alb_reference = strdup(alb_ref.c_str());
            result->artworks[artwork_idx].mtp_object_id = object_id;
            artwork_idx++;
        }

        // Copy playlists
        result->playlist_count = library.playlist_count;
        result->playlists = new ZuneMusicPlaylist[result->playlist_count]{};
        for (uint32_t i = 0; i < library.playlist_count; i++) {
            const auto& p = library.playlists[i];
            result->playlists[i].name = strdup(p.name.c_str());
            result->playlists[i].filename = strdup(p.filename.c_str());
            result->playlists[i].guid = strdup(p.guid.c_str());
            result->playlists[i].folder = strdup(p.folder.c_str());
            result->playlists[i].track_count = p.track_atom_ids.size();
            result->playlists[i].atom_id = p.atom_id;

            if (p.track_atom_ids.size() > 0) {
                result->playlists[i].track_atom_ids = new uint32_t[p.track_atom_ids.size()];
                for (size_t j = 0; j < p.track_atom_ids.size(); j++)
                    result->playlists[i].track_atom_ids[j] = p.track_atom_ids[j];
            }
        }

        return result.release();

    } catch (...) {
        return nullptr;
    }
}

// ── Library Cleanup ─────────────────────────────────────────────────────

void MtpReader::FreeLibrary(ZuneMusicLibrary* library) {
    if (!library) return;

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

    for (uint32_t i = 0; i < library->album_count; ++i) {
        free((void*)library->albums[i].title);
        free((void*)library->albums[i].artist_name);
        free((void*)library->albums[i].artist_guid);
        free((void*)library->albums[i].alb_reference);
    }
    delete[] library->albums;

    for (uint32_t i = 0; i < library->artist_count; ++i) {
        free((void*)library->artists[i].name);
        free((void*)library->artists[i].filename);
        free((void*)library->artists[i].guid);
    }
    delete[] library->artists;

    for (uint32_t i = 0; i < library->genre_count; ++i) {
        free((void*)library->genres[i].name);
    }
    delete[] library->genres;

    for (uint32_t i = 0; i < library->artwork_count; ++i) {
        free((void*)library->artworks[i].alb_reference);
    }
    delete[] library->artworks;

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

} // namespace zune
