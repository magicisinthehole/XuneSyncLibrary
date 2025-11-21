#include "ZuneHDParser.h"
#include "ZMDBUtils.h"
#include <cstring>
#include <iostream>
#include <sstream>
#include <iomanip>

namespace zmdb {

ZMDBLibrary ZuneHDParser::ExtractLibrary(const std::vector<uint8_t>& zmdb_data) {
    ZMDBLibrary library;
    library.device_type = DeviceType::ZuneHD;

    if (zmdb_data.empty()) {
        return library;
    }

    zmdb_data_ = zmdb_data;

    // Parse ZMDB header
    if (zmdb_data_.size() < 0x10) {
        return library;
    }

    // Verify ZMDB magic
    if (zmdb_data_[0] != 'Z' || zmdb_data_[1] != 'M' ||
        zmdb_data_[2] != 'D' || zmdb_data_[3] != 'B') {
        return library;
    }

    // Parse ZMed header at offset 0x20
    if (zmdb_data_.size() < 0x30) {
        return library;
    }

    if (zmdb_data_[0x20] != 'Z' || zmdb_data_[0x21] != 'M' ||
        zmdb_data_[0x22] != 'e' || zmdb_data_[0x23] != 'd') {
        return library;
    }

    // Find ZArr descriptors - search for first "ZArr" after ZMed header
    size_t descriptor_offset = 0;
    for (size_t offset = 0x30; offset < 0x100 && offset + 4 <= zmdb_data_.size(); offset += 4) {
        if (zmdb_data_[offset] == 'Z' && zmdb_data_[offset + 1] == 'A' &&
            zmdb_data_[offset + 2] == 'r' && zmdb_data_[offset + 3] == 'r') {
            descriptor_offset = offset;
            break;
        }
    }

    if (descriptor_offset == 0) {
        return library;
    }

    // Parse 96 descriptors
    descriptors_.resize(96);
    for (int i = 0; i < 96; i++) {
        size_t desc_offset = descriptor_offset + (i * 20);
        if (desc_offset + 20 > zmdb_data_.size()) {
            break;
        }

        descriptors_[i].entry_size = read_uint16_le(zmdb_data_, desc_offset + 6);
        descriptors_[i].entry_count = read_uint32_le(zmdb_data_, desc_offset + 8);
        descriptors_[i].data_offset = read_uint32_le(zmdb_data_, desc_offset + 16);
    }

    // Build index table from descriptor 0
    if (descriptors_[0].entry_count > 0 && descriptors_[0].entry_size == 8) {
        index_table_ = build_index_table(
            zmdb_data_,
            descriptors_[0].data_offset,
            descriptors_[0].entry_count
        );
    }

    // Allocate arrays with exact sizes from descriptors (single allocation, no reallocation)
    if (descriptors_.size() > 1 && descriptors_[1].entry_count > 0) {
        library.tracks_capacity = descriptors_[1].entry_count;
        library.tracks = static_cast<ZMDBTrack*>(::operator new[](library.tracks_capacity * sizeof(ZMDBTrack)));
    }
    if (descriptors_.size() > 11 && descriptors_[11].entry_count > 0) {
        library.playlists_capacity = descriptors_[11].entry_count;
        library.playlists = static_cast<ZMDBPlaylist*>(::operator new[](library.playlists_capacity * sizeof(ZMDBPlaylist)));
    }
    if (descriptors_.size() > 12 && descriptors_[12].entry_count > 0) {
        library.videos_capacity = descriptors_[12].entry_count;
        library.videos = static_cast<ZMDBVideo*>(::operator new[](library.videos_capacity * sizeof(ZMDBVideo)));
    }
    if (descriptors_.size() > 16 && descriptors_[16].entry_count > 0) {
        library.pictures_capacity = descriptors_[16].entry_count;
        library.pictures = static_cast<ZMDBPicture*>(::operator new[](library.pictures_capacity * sizeof(ZMDBPicture)));
    }
    if (descriptors_.size() > 19 && descriptors_[19].entry_count > 0) {
        library.podcasts_capacity = descriptors_[19].entry_count;
        library.podcasts = static_cast<ZMDBPodcast*>(::operator new[](library.podcasts_capacity * sizeof(ZMDBPodcast)));
    }

    // Parse directly into arrays (no intermediate vectors, no reallocation)
    try {
        extract_media_from_descriptor(1, Schema::Music, library);      // Music tracks
    } catch (const std::exception& e) {
        throw std::runtime_error(std::string("Music parsing failed: ") + e.what());
    }

    try {
        extract_media_from_descriptor(11, Schema::Playlist, library);  // Playlists
    } catch (const std::exception& e) {
        throw std::runtime_error(std::string("Playlist parsing failed: ") + e.what());
    }

    try {
        extract_media_from_descriptor(12, Schema::Video, library);     // Videos
    } catch (const std::exception& e) {
        throw std::runtime_error(std::string("Video parsing failed: ") + e.what());
    }

    try {
        extract_media_from_descriptor(16, Schema::Picture, library);   // Pictures
    } catch (const std::exception& e) {
        throw std::runtime_error(std::string("Picture parsing failed: ") + e.what());
    }

    try {
        extract_media_from_descriptor(19, Schema::PodcastEpisode, library); // Podcast episodes
    } catch (const std::exception& e) {
        throw std::runtime_error(std::string("Podcast parsing failed: ") + e.what());
    }

    // Move album metadata from cache (no tracks - consumer groups by album_ref)
    try {
        library.album_metadata = std::move(album_cache_);
        library.album_count = library.album_metadata.size();
    } catch (const std::exception& e) {
        throw std::runtime_error(std::string("Album metadata move failed: ") + e.what());
    }

    return library;
}

bool ZuneHDParser::should_filter_record(
    const std::vector<uint8_t>& record_data,
    uint8_t schema_type
) const {
    if (record_data.size() < 12) {
        return false;
    }

    // Filter 1: Root entries (all reference fields are zero)
    uint32_t ref0 = read_uint32_le(record_data, 0);
    uint32_t ref1 = read_uint32_le(record_data, 4);
    uint32_t ref2 = read_uint32_le(record_data, 8);

    if (ref0 == 0 && ref1 == 0 && ref2 == 0) {
        return true;  // Root/system entry
    }

    // Filter 2: GUID artists (schema 0x08, ref0 == 0)
    if (schema_type == Schema::Artist && ref0 == 0) {
        return true;  // GUID placeholder artist
    }

    // Filter 3: 32-byte placeholder tracks (schema 0x01, size == 32)
    if (schema_type == Schema::Music && record_data.size() == 32) {
        return true;  // Placeholder/miscategorized track
    }

    return false;
}

std::optional<ZMDBTrack> ZuneHDParser::parse_music_track(
    const std::vector<uint8_t>& record_data,
    uint32_t atom_id
) {
    if (record_data.size() < 32) {
        return std::nullopt;
    }

    ZMDBTrack track;
    track.atom_id = atom_id;

    // Read reference fields
    uint32_t album_ref = read_uint32_le(record_data, 0);
    uint32_t artist_ref = read_uint32_le(record_data, 4);
    uint32_t genre_ref = read_uint32_le(record_data, 8);
    uint32_t album_filename_ref = read_uint32_le(record_data, 12);

    // Read fixed fields (16-31)
    track.duration_ms = read_int32_le(record_data, 16);
    track.file_size_bytes = read_int32_le(record_data, 20);
    track.track_number = read_uint16_le(record_data, 24);
    track.unknown_field_26 = read_uint16_le(record_data, 26);  // Purpose unknown
    track.codec_id = read_uint16_le(record_data, 28);
    track.field_30 = record_data[30];
    track.reserved_31 = record_data[31];

    // Store album_ref for grouping tracks into albums
    track.album_ref = album_ref;

    // Read title at offset 32
    if (record_data.size() > 32) {
        track.title = read_null_terminated_utf8(record_data, 32);
    }

    // Resolve references
    if (album_ref != 0) {
        auto album_info = resolve_album_info(album_ref);
        if (album_info.has_value()) {
            track.album_name = album_info->second;
            // Get album artist name and GUID from album
            if (album_cache_.count(album_ref)) {
                track.album_artist_name = album_cache_[album_ref].artist_name;
                track.album_artist_guid = album_cache_[album_ref].artist_guid;
            }
        }
    }

    if (artist_ref != 0) {
        // Ensure artist is fully parsed and cached
        if (!artist_cache_.count(artist_ref)) {
            if (index_table_.count(artist_ref)) {
                uint32_t record_offset = index_table_[artist_ref];
                auto record_opt = read_record_at_offset(zmdb_data_, record_offset);
                if (record_opt.has_value()) {
                    const auto& rec_data = record_opt->second;
                    uint32_t ref0 = read_uint32_le(rec_data, 0);
                    if (ref0 != 0) {  // Skip GUID/root artists
                        auto artist = parse_artist(rec_data, artist_ref);
                        if (artist.has_value()) {
                            artist_cache_[artist_ref] = artist.value();
                        }
                    }
                }
            }
        }

        // Get artist name and GUID from cache
        if (artist_cache_.count(artist_ref)) {
            track.artist_name = artist_cache_[artist_ref].name;
            track.artist_guid = artist_cache_[artist_ref].guid;
        }
    }

    if (genre_ref != 0) {
        track.genre = resolve_genre(genre_ref);
    }

    if (album_filename_ref != 0) {
        track.filename = resolve_string_reference(album_filename_ref);
    }

    return track;
}

std::optional<ZMDBVideo> ZuneHDParser::parse_video(
    const std::vector<uint8_t>& record_data,
    uint32_t atom_id
) {
    if (record_data.size() < 16) {
        return std::nullopt;
    }

    ZMDBVideo video;
    video.atom_id = atom_id;

    // Read reference fields
    uint32_t folder_ref = read_uint32_le(record_data, 0);
    uint32_t title_ref = read_uint32_le(record_data, 4);
    video.ref2 = read_uint32_le(record_data, 8);  // Purpose unknown
    uint32_t file_ref = read_uint32_le(record_data, 12);

    // Read metadata at offset 32+
    if (record_data.size() >= 40) {
        video.file_size_bytes = read_uint32_le(record_data, 32);
        video.codec_id = read_uint32_le(record_data, 36);
    }

    // Resolve references
    if (folder_ref != 0) {
        video.folder = resolve_string_reference(folder_ref);
    }

    if (title_ref != 0) {
        video.title = resolve_string_reference(title_ref);
    }

    // Parse filename from backwards varints (field 0x44)
    size_t entry_size = get_entry_size_for_schema(Schema::Video);
    if (entry_size > 0) {
        auto fields = parse_backwards_varints(record_data, entry_size);
        for (const auto& field : fields) {
            if (field.field_id == 0x44 && field.field_size > 2) {
                // Handle padding bytes
                if (field.field_data[0] == 0x00 && field.field_data[field.field_size - 1] == 0x00) {
                    video.filename = utf16le_to_utf8(std::vector<uint8_t>(
                        field.field_data.begin() + 1,
                        field.field_data.end() - 1
                    ));
                } else {
                    video.filename = utf16le_to_utf8(field.field_data);
                }
                break;
            }
        }
    }

    return video;
}

std::optional<ZMDBPicture> ZuneHDParser::parse_picture(
    const std::vector<uint8_t>& record_data,
    uint32_t atom_id
) {
    if (record_data.size() < 24) {
        return std::nullopt;
    }

    ZMDBPicture picture;
    picture.atom_id = atom_id;

    // Read reference fields
    uint32_t folder_ref = read_uint32_le(record_data, 0);
    uint32_t photo_album_ref = read_uint32_le(record_data, 4);
    uint32_t collection_ref = read_uint32_le(record_data, 8);
    uint32_t file_ref = read_uint32_le(record_data, 12);

    // Read timestamp at offset 16-23
    picture.timestamp = read_uint64_le(record_data, 16);

    // Read title at offset 24
    if (record_data.size() > 24) {
        picture.title = read_null_terminated_utf8(record_data, 24);
    }

    // Resolve references
    if (photo_album_ref != 0) {
        picture.photo_album = resolve_string_reference(photo_album_ref);
    }

    if (folder_ref != 0) {
        picture.user_album = resolve_string_reference(folder_ref);
    }

    if (collection_ref != 0) {
        picture.collection = resolve_string_reference(collection_ref);
    }

    if (file_ref != 0) {
        picture.filename = resolve_string_reference(file_ref);
    }

    return picture;
}

std::optional<ZMDBPlaylist> ZuneHDParser::parse_playlist(
    const std::vector<uint8_t>& record_data,
    uint32_t atom_id
) {
    if (record_data.size() < 12) {
        return std::nullopt;
    }

    ZMDBPlaylist playlist;
    playlist.atom_id = atom_id;

    // Track count at offset 0
    uint32_t track_count = read_uint32_le(record_data, 0);
    playlist.track_count = track_count;

    // Folder reference at offset 8
    uint32_t folder_ref = read_uint32_le(record_data, 8);
    if (folder_ref != 0) {
        playlist.folder = resolve_string_reference(folder_ref);
    }

    // Playlist name at offset 12 (UTF-8)
    if (record_data.size() > 12) {
        size_t null_pos = 12;
        while (null_pos < record_data.size() && record_data[null_pos] != 0) {
            null_pos++;
        }

        if (null_pos > 12) {
            playlist.name = std::string(
                reinterpret_cast<const char*>(&record_data[12]),
                null_pos - 12
            );

            // Parse GUID (16 bytes after null terminator)
            size_t guid_start = null_pos + 1;
            if (guid_start + 16 <= record_data.size()) {
                // Extract GUID as hex string
                std::stringstream guid_ss;
                for (size_t i = 0; i < 16; i++) {
                    guid_ss << std::hex << std::setw(2) << std::setfill('0')
                            << static_cast<int>(record_data[guid_start + i]);
                }
                playlist.guid = guid_ss.str();

                // Skip GUID and 2-byte field to get to UTF-16LE filename
                size_t utf16_start = guid_start + 16 + 2;

                // Find double-null terminator for filename
                size_t filename_end = utf16_start;
                while (filename_end + 1 < record_data.size()) {
                    if (record_data[filename_end] == 0 && record_data[filename_end + 1] == 0) {
                        break;
                    }
                    filename_end += 2;
                }

                if (filename_end > utf16_start) {
                    std::vector<uint8_t> utf16_data(
                        record_data.begin() + utf16_start,
                        record_data.begin() + filename_end
                    );

                    // Handle padding bytes
                    if (!utf16_data.empty() && utf16_data[0] == 0x00 && utf16_data.back() == 0x00) {
                        utf16_data.erase(utf16_data.begin());
                        utf16_data.pop_back();
                    }

                    playlist.filename = utf16le_to_utf8(utf16_data);
                }

                // Parse track list after double-null + 2-byte field
                size_t track_list_start = filename_end + 4;
                while (track_list_start + 4 <= record_data.size()) {
                    uint32_t track_id = read_uint32_le(record_data, track_list_start);
                    if (track_id == 0) {
                        break;
                    }

                    uint8_t track_schema = (track_id >> 24) & 0xFF;
                    if (track_schema == Schema::Music) {
                        auto track = resolve_track(track_id);
                        if (track.has_value()) {
                            playlist.tracks.push_back(track.value());
                        }
                    }

                    track_list_start += 4;
                }
            }
        }
    }

    return playlist;
}

std::optional<ZMDBPodcast> ZuneHDParser::parse_podcast_episode(
    const std::vector<uint8_t>& record_data,
    uint32_t atom_id
) {
    if (record_data.size() < 32) {
        return std::nullopt;
    }

    ZMDBPodcast podcast;
    podcast.atom_id = atom_id;

    // Read reference fields
    uint32_t show_name_ref = read_uint32_le(record_data, 0);
    uint32_t podcast_show_ref = read_uint32_le(record_data, 4);
    podcast.duration_ms = read_uint32_le(record_data, 8);  // Duration at ref2 position
    podcast.ref3 = read_uint32_le(record_data, 12);  // Purpose unknown, might be file reference

    // Read fixed fields at offset 16+
    podcast.timestamp = read_uint64_le(record_data, 16);
    podcast.file_size_bytes = read_uint32_le(record_data, 24);
    podcast.codec_id = read_uint16_le(record_data, 30);

    // Parse strings starting at offset 36
    if (record_data.size() > 36) {
        size_t pos = 36;

        // Episode title (UTF-8)
        size_t null_pos = pos;
        while (null_pos < record_data.size() && record_data[null_pos] != 0) {
            null_pos++;
        }

        if (null_pos > pos) {
            podcast.title = std::string(
                reinterpret_cast<const char*>(&record_data[pos]),
                null_pos - pos
            );
            pos = null_pos + 1;
        }

        // Author/email (UTF-16LE)
        if (pos + 1 < record_data.size() && record_data[pos] != 0 && record_data[pos + 1] == 0) {
            size_t author_end = pos;
            while (author_end + 1 < record_data.size()) {
                if (record_data[author_end] == 0 && record_data[author_end + 1] == 0) {
                    break;
                }
                author_end += 2;
            }

            if (author_end > pos + 4) {
                podcast.author = utf16le_to_utf8(std::vector<uint8_t>(
                    record_data.begin() + pos,
                    record_data.begin() + author_end
                ));
                pos = author_end + 2;
            }
        }

        // Description (UTF-16LE after marker)
        if (pos + 10 < record_data.size()) {
            // Find marker
            size_t marker_pos = pos;
            while (marker_pos + 3 < record_data.size() && record_data[marker_pos] != 0) {
                marker_pos++;
                if (marker_pos - pos > 10) break;
            }

            if (marker_pos > pos && marker_pos + 1 < record_data.size()) {
                pos = marker_pos - 1;

                size_t desc_end = pos;
                while (desc_end + 1 < record_data.size() - 100) {
                    if (record_data[desc_end] == 0 && record_data[desc_end + 1] == 0) {
                        break;
                    }
                    desc_end += 2;
                }

                if (desc_end > pos + 10) {
                    podcast.description = utf16le_to_utf8(std::vector<uint8_t>(
                        record_data.begin() + pos,
                        record_data.begin() + desc_end
                    ));
                }
            }
        }
    }

    // Resolve references
    if (show_name_ref != 0) {
        podcast.show_name = resolve_string_reference(show_name_ref);
    }

    // Parse audio URL from backwards varints
    size_t entry_size = get_entry_size_for_schema(Schema::PodcastEpisode);
    if (entry_size > 0) {
        auto fields = parse_backwards_varints(record_data, entry_size);
        for (const auto& field : fields) {
            if (field.field_size > 100) {
                // URL in UTF-16LE with marker + padding bytes
                if (field.field_size > 2) {
                    podcast.audio_url = utf16le_to_utf8(std::vector<uint8_t>(
                        field.field_data.begin() + 1,
                        field.field_data.end() - 1
                    ));
                }
                break;
            }
        }
    }

    return podcast;
}

std::optional<ZMDBAlbum> ZuneHDParser::parse_album(
    const std::vector<uint8_t>& record_data,
    uint32_t atom_id
) {
    if (record_data.size() < 20) {
        return std::nullopt;
    }

    ZMDBAlbum album;
    album.atom_id = atom_id;

    // Album artist reference at offset 0
    album.artist_ref = read_uint32_le(record_data, 0);

    // Album title at offset 20 (UTF-8)
    if (record_data.size() > 20) {
        album.title = read_null_terminated_utf8(record_data, 20);
    }

    // Resolve artist name and GUID
    if (album.artist_ref != 0) {
        album.artist_name = resolve_artist_name(album.artist_ref);

        // Get artist GUID from cache if available
        if (artist_cache_.count(album.artist_ref)) {
            album.artist_guid = artist_cache_[album.artist_ref].guid;
        }
    }

    // Parse filename from backwards varints (field 0x44)
    size_t entry_size = get_entry_size_for_schema(Schema::Album);
    if (entry_size > 0) {
        auto fields = parse_backwards_varints(record_data, entry_size);
        for (const auto& field : fields) {
            if (field.field_id == 0x44 && field.field_size > 2) {
                // Handle padding bytes
                std::vector<uint8_t> utf16_data = field.field_data;
                if (!utf16_data.empty() && utf16_data[0] == 0x00 && utf16_data.back() == 0x00) {
                    utf16_data.erase(utf16_data.begin());
                    utf16_data.pop_back();
                }
                // Store as alb_reference for MTP correlation
                album.alb_reference = utf16le_to_utf8(utf16_data);
                break;
            }
        }
    }

    return album;
}

std::optional<ZMDBArtist> ZuneHDParser::parse_artist(
    const std::vector<uint8_t>& record_data,
    uint32_t atom_id
) {
    if (record_data.size() < 4) {
        return std::nullopt;
    }

    ZMDBArtist artist;
    artist.atom_id = atom_id;

    // Artist name at offset 4 (UTF-8)
    if (record_data.size() > 4) {
        artist.name = read_null_terminated_utf8(record_data, 4);
    }

    // Parse backwards varints for field 0x44 (filename) and 0x14 (GUID)
    size_t entry_size = get_entry_size_for_schema(Schema::Artist);
    if (entry_size > 0) {
        auto fields = parse_backwards_varints(record_data, entry_size);
        for (const auto& field : fields) {
            // Field 0x44: UTF-16LE filename (.art reference)
            if (field.field_id == 0x44 && field.field_size > 2) {
                std::vector<uint8_t> utf16_data = field.field_data;
                if (!utf16_data.empty() && utf16_data[0] == 0x00 && utf16_data.back() == 0x00) {
                    utf16_data.erase(utf16_data.begin());
                    utf16_data.pop_back();
                }
                artist.filename = utf16le_to_utf8(utf16_data);
            }
            // Field 0x14: Artist GUID (16 bytes, optional)
            else if (field.field_id == 0x14 && field.field_size == 16) {
                artist.guid = parse_windows_guid(field.field_data);
            }
        }
    }

    return artist;
}

std::string ZuneHDParser::resolve_string_reference(uint32_t atom_id) {
    // Check cache
    if (string_cache_.count(atom_id)) {
        return string_cache_[atom_id];
    }

    // Lookup in index table
    if (!index_table_.count(atom_id)) {
        return "";
    }

    uint32_t record_offset = index_table_[atom_id];
    auto record_opt = read_record_at_offset(zmdb_data_, record_offset);
    if (!record_opt.has_value()) {
        return "";
    }

    const auto& record_data = record_opt->second;
    uint8_t schema_type = (atom_id >> 24) & 0xFF;

    std::string result;

    // Schema-specific string extraction
    switch (schema_type) {
        case Schema::Filename:  // 0x05
            if (record_data.size() > 8) {
                result = read_null_terminated_utf8(record_data, 8);
            }
            break;

        case Schema::Genre:  // 0x09
            if (record_data.size() > 1) {
                result = read_null_terminated_utf8(record_data, 1);
            }
            break;

        case Schema::VideoTitle:  // 0x0a
            if (record_data.size() > 4) {
                result = read_null_terminated_utf8(record_data, 4);
            }
            break;

        case Schema::PhotoAlbum:  // 0x0b
        case Schema::Collection:  // 0x0c
            if (record_data.size() >= 20) {
                result = read_null_terminated_utf8(record_data, 12);
            }
            break;

        case Schema::PodcastShow:  // 0x0f
            if (record_data.size() > 8) {
                result = read_null_terminated_utf8(record_data, 8);
            }
            break;

        case Schema::Album:  // 0x06
        case Schema::Artist:  // 0x08
        {
            // These use backwards varint field 0x44
            size_t entry_size = get_entry_size_for_schema(schema_type);
            if (entry_size > 0) {
                auto fields = parse_backwards_varints(record_data, entry_size);
                for (const auto& field : fields) {
                    if (field.field_id == 0x44 && field.field_size > 2) {
                        std::vector<uint8_t> utf16_data = field.field_data;
                        if (!utf16_data.empty() && utf16_data[0] == 0x00 && utf16_data.back() == 0x00) {
                            utf16_data.erase(utf16_data.begin());
                            utf16_data.pop_back();
                        }
                        result = utf16le_to_utf8(utf16_data);
                        break;
                    }
                }
            }
            break;
        }

        default:
            break;
    }

    // Cache and return
    string_cache_[atom_id] = result;
    return result;
}

std::string ZuneHDParser::resolve_artist_name(uint32_t atom_id) {
    // Check cache
    if (artist_cache_.count(atom_id)) {
        return artist_cache_[atom_id].name;
    }

    // Lookup and parse artist record
    if (!index_table_.count(atom_id)) {
        return "";
    }

    uint32_t record_offset = index_table_[atom_id];
    auto record_opt = read_record_at_offset(zmdb_data_, record_offset);
    if (!record_opt.has_value()) {
        return "";
    }

    const auto& record_data = record_opt->second;

    // Check for GUID artist (ref0 == 0)
    if (record_data.size() >= 4) {
        uint32_t ref0 = read_uint32_le(record_data, 0);
        if (ref0 == 0) {
            return "";  // Skip GUID artists
        }
    }

    auto artist = parse_artist(record_data, atom_id);
    if (artist.has_value()) {
        artist_cache_[atom_id] = artist.value();
        return artist->name;
    }

    return "";
}

std::string ZuneHDParser::resolve_genre(uint32_t atom_id) {
    return resolve_string_reference(atom_id);
}

std::optional<std::pair<uint32_t, std::string>> ZuneHDParser::resolve_album_info(uint32_t atom_id) {
    // Check cache
    if (album_cache_.count(atom_id)) {
        auto& album = album_cache_[atom_id];
        return std::make_pair(album.atom_id, album.title);
    }

    // Lookup and parse album record
    if (!index_table_.count(atom_id)) {
        return std::nullopt;
    }

    uint32_t record_offset = index_table_[atom_id];
    auto record_opt = read_record_at_offset(zmdb_data_, record_offset);
    if (!record_opt.has_value()) {
        return std::nullopt;
    }

    auto album = parse_album(record_opt->second, atom_id);
    if (album.has_value()) {
        album_cache_[atom_id] = album.value();
        return std::make_pair(album->atom_id, album->title);
    }

    return std::nullopt;
}

std::optional<ZMDBTrack> ZuneHDParser::resolve_track(uint32_t track_atom_id) {
    // Lookup track record
    if (!index_table_.count(track_atom_id)) {
        return std::nullopt;
    }

    uint32_t record_offset = index_table_[track_atom_id];
    auto record_opt = read_record_at_offset(zmdb_data_, record_offset);
    if (!record_opt.has_value()) {
        return std::nullopt;
    }

    return parse_music_track(record_opt->second, track_atom_id);
}

void ZuneHDParser::extract_media_from_descriptor(
    uint32_t descriptor_idx,
    uint8_t expected_schema,
    ZMDBLibrary& library
) {
    if (descriptor_idx >= descriptors_.size()) {
        return;
    }

    const auto& desc = descriptors_[descriptor_idx];
    if (desc.entry_count == 0) {
        return;
    }

    for (uint32_t i = 0; i < desc.entry_count; i++) {
        size_t entry_offset = desc.data_offset + (i * desc.entry_size);
        if (entry_offset + 4 > zmdb_data_.size()) {
            break;
        }

        uint32_t atom_id = read_uint32_le(zmdb_data_, entry_offset);
        uint8_t schema_type = (atom_id >> 24) & 0xFF;

        // Lookup record in index table
        if (!index_table_.count(atom_id)) {
            continue;
        }

        uint32_t record_offset = index_table_[atom_id];
        auto record_opt = read_record_at_offset(zmdb_data_, record_offset);
        if (!record_opt.has_value()) {
            continue;
        }

        const auto& record_data = record_opt->second;

        // Apply filters
        if (should_filter_record(record_data, schema_type)) {
            continue;
        }

        // Parse based on schema type (use placement new to construct in pre-allocated array)
        switch (schema_type) {
            case Schema::Music:
            {
                if (library.track_count < library.tracks_capacity) {
                    auto track = parse_music_track(record_data, atom_id);
                    if (track.has_value()) {
                        new (&library.tracks[library.track_count]) ZMDBTrack(std::move(track.value()));
                        library.track_count++;
                    }
                }
                break;
            }

            case Schema::Video:
            {
                if (library.video_count < library.videos_capacity) {
                    auto video = parse_video(record_data, atom_id);
                    if (video.has_value()) {
                        new (&library.videos[library.video_count]) ZMDBVideo(std::move(video.value()));
                        library.video_count++;
                    }
                }
                break;
            }

            case Schema::Picture:
            {
                if (library.picture_count < library.pictures_capacity) {
                    auto picture = parse_picture(record_data, atom_id);
                    if (picture.has_value()) {
                        new (&library.pictures[library.picture_count]) ZMDBPicture(std::move(picture.value()));
                        library.picture_count++;
                    }
                }
                break;
            }

            case Schema::Playlist:
            {
                if (library.playlist_count < library.playlists_capacity) {
                    auto playlist = parse_playlist(record_data, atom_id);
                    if (playlist.has_value()) {
                        new (&library.playlists[library.playlist_count]) ZMDBPlaylist(std::move(playlist.value()));
                        library.playlist_count++;
                    }
                }
                break;
            }

            case Schema::PodcastEpisode:
            {
                if (library.podcast_count < library.podcasts_capacity) {
                    auto podcast = parse_podcast_episode(record_data, atom_id);
                    if (podcast.has_value()) {
                        new (&library.podcasts[library.podcast_count]) ZMDBPodcast(std::move(podcast.value()));
                        library.podcast_count++;
                    }
                }
                break;
            }

            default:
                break;
        }
    }
}

} // namespace zmdb
