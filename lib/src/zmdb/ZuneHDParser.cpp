#include "ZuneHDParser.h"
#include "ZMDBUtils.h"
#include <cstring>
#include <ctime>
#include <iostream>
#include <sstream>
#include <iomanip>

namespace zmdb {

ZMDBLibrary ZuneHDParser::ExtractLibrary(const std::vector<uint8_t>& zmdb_data) {
    ZMDBLibrary library;
    library.device_family = zune::DeviceFamily::Pavo;

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
    if (descriptors_.size() > 26 && descriptors_[26].entry_count > 0) {
        library.audiobooks_capacity = descriptors_[26].entry_count;
        library.audiobooks = static_cast<ZMDBAudiobook*>(::operator new[](library.audiobooks_capacity * sizeof(ZMDBAudiobook)));
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

    try {
        extract_media_from_descriptor(26, Schema::AudiobookTrack, library); // Audiobook tracks (descriptor 26, not 25)
    } catch (const std::exception& e) {
        throw std::runtime_error(std::string("Audiobook parsing failed: ") + e.what());
    }

    // Move album metadata from cache (no tracks - consumer groups by album_ref)
    try {
        library.album_metadata = std::move(album_cache_);
        library.album_count = library.album_metadata.size();
    } catch (const std::exception& e) {
        throw std::runtime_error(std::string("Album metadata move failed: ") + e.what());
    }

    // Move artist metadata from cache
    try {
        library.artist_metadata = std::move(artist_cache_);
        library.artist_count = library.artist_metadata.size();
    } catch (const std::exception& e) {
        throw std::runtime_error(std::string("Artist metadata move failed: ") + e.what());
    }

    // Move genre metadata from cache
    try {
        library.genre_metadata = std::move(genre_cache_);
        library.genre_count = library.genre_metadata.size();
    } catch (const std::exception& e) {
        throw std::runtime_error(std::string("Genre metadata move failed: ") + e.what());
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

    // Note: GUID artists (ref0 == 0) are now included so albums can reference them

    // Filter 2: 32-byte placeholder tracks (schema 0x01, size == 32)
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
    track.playcount = read_uint16_le(record_data, 26);         // Play count
    track.codec_id = read_uint16_le(record_data, 28);          // Format code (e.g., 0xb901 = WMA)
    track.rating = record_data[30];                             // Rating: 0=neutral, 8=liked, 3=disliked

    // Store album_ref for grouping tracks into albums
    track.album_ref = album_ref;

    // Store genre_ref for genre entity tracking
    track.genre_ref = genre_ref;

    // Read title at offset 32
    if (record_data.size() > 32) {
        track.title = read_null_terminated_utf8(record_data, 32);
    }

    // Parse backwards varints for optional fields (0x62, 0x63, 0x6c, 0x70)
    size_t entry_size = get_entry_size_for_schema(Schema::Music);
    if (entry_size > 0) {
        auto fields = parse_backwards_varints(record_data, entry_size);
        for (const auto& field : fields) {
            switch (field.field_id) {
                case 0x63:  // Skip count
                    if (field.field_size >= 1 && field.field_size <= 4) {
                        track.skip_count = static_cast<uint16_t>(read_uint32_le(field.field_data, 0));
                    }
                    break;
                case 0x6c:  // Disc number
                    if (field.field_size >= 1 && field.field_size <= 4) {
                        track.disc_number = static_cast<int>(read_uint32_le(field.field_data, 0));
                    }
                    break;
                case 0x70:  // Last played/skipped timestamp (Windows FILETIME)
                    if (field.field_size == 8) {
                        track.last_played_timestamp = read_uint64_le(field.field_data, 0);
                    }
                    break;
                // 0x62 (redundant play count) intentionally skipped - we use offset 26-27
                default:
                    break;
            }
        }
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
                    // Include all artists (including GUID/root artists) so albums can reference them
                    auto artist = parse_artist(rec_data, artist_ref);
                    if (artist.has_value()) {
                        artist_cache_[artist_ref] = artist.value();
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

    // Playlist name at offset 12 (UTF-8, null-terminated)
    if (record_data.size() <= 12) {
        return playlist;
    }

    size_t null_pos = 12;
    while (null_pos < record_data.size() && record_data[null_pos] != 0) {
        null_pos++;
    }

    // Need at least 3 bytes after null_pos for format detection (pos, pos+1, and data)
    if (null_pos <= 12 || null_pos + 3 >= record_data.size()) {
        return playlist;
    }

    playlist.name = std::string(
        reinterpret_cast<const char*>(&record_data[12]),
        null_pos - 12
    );

    // Detect format: after name null, check if byte[1] == 0x00 (UTF-16LE) or not (GUID)
    // Device-created playlists: UTF-16LE filename starts immediately after name
    // Software-created playlists: 16-byte GUID + 2-byte field, then UTF-16LE filename
    size_t pos = null_pos + 1;
    bool has_guid = (record_data[pos + 1] != 0x00);

    if (has_guid) {
        // Software format: parse 16-byte GUID
        if (pos + 16 > record_data.size()) {
            return playlist;
        }
        std::stringstream guid_ss;
        for (size_t i = 0; i < 16; i++) {
            guid_ss << std::hex << std::setw(2) << std::setfill('0')
                    << static_cast<int>(record_data[pos + i]);
        }
        playlist.guid = guid_ss.str();
        pos += 16;

        // Skip 2-byte field after GUID
        pos += 2;
    }

    // Parse UTF-16LE filename (double-null terminated)
    size_t utf16_start = pos;
    while (pos + 1 < record_data.size()) {
        if (record_data[pos] == 0 && record_data[pos + 1] == 0) {
            break;
        }
        pos += 2;
    }

    if (pos > utf16_start) {
        std::vector<uint8_t> utf16_data(
            record_data.begin() + utf16_start,
            record_data.begin() + pos
        );
        playlist.filename = utf16le_to_utf8(utf16_data);
    }

    // Skip double-null terminator + 2-byte pre-track field
    pos += 4;

    // Parse track atom_ids
    while (pos + 4 <= record_data.size()) {
        uint32_t track_id = read_uint32_le(record_data, pos);
        if (track_id == 0) {
            break;
        }

        uint8_t track_schema = (track_id >> 24) & 0xFF;
        if (track_schema == Schema::Music) {
            playlist.track_atom_ids.push_back(track_id);
        }

        pos += 4;
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

std::optional<ZMDBAudiobook> ZuneHDParser::parse_audiobook_track(
    const std::vector<uint8_t>& record_data,
    uint32_t atom_id
) {
    // Schema 0x12 - Audiobook Track
    // Based on AUDIOBOOK_SCHEMA_ANALYSIS.md
    if (record_data.size() < 36) {
        return std::nullopt;
    }

    ZMDBAudiobook audiobook;
    audiobook.atom_id = atom_id;

    // Read reference fields (offsets 0x00-0x07)
    audiobook.filename_ref = read_uint32_le(record_data, 0);   // Folder ref (Schema 0x05)
    audiobook.title_ref = read_uint32_le(record_data, 4);      // Audiobook title ref (Schema 0x11)

    // Read fixed fields
    audiobook.duration_ms = read_uint32_le(record_data, 8);           // Offset 0x08
    audiobook.playback_position_ms = read_uint32_le(record_data, 12); // Offset 0x0C
    // Offset 0x10-0x17: uint64 timestamp (usually 0, actual timestamp in varint 0x70)
    audiobook.file_size_bytes = read_uint32_le(record_data, 24);      // Offset 0x18
    audiobook.track_number = read_uint16_le(record_data, 28);         // Offset 0x1C
    audiobook.playcount = read_uint16_le(record_data, 30);            // Offset 0x1E
    audiobook.format_code = read_uint16_le(record_data, 32);          // Offset 0x20
    // Offset 0x22: uint16 field (playback state related)

    // Read track title at offset 0x24 (UTF-8, null-terminated)
    if (record_data.size() > 36) {
        audiobook.title = read_null_terminated_utf8(record_data, 36);
    }

    // Resolve audiobook title from reference
    if (audiobook.title_ref != 0) {
        audiobook.audiobook_name = resolve_string_reference(audiobook.title_ref);
    }

    // Parse backwards varints for additional fields
    size_t entry_size = get_entry_size_for_schema(Schema::AudiobookTrack);
    if (entry_size > 0) {
        auto fields = parse_backwards_varints(record_data, entry_size);
        for (const auto& field : fields) {
            // Field 0x44: Filename (UTF-16LE)
            if (field.field_id == 0x44 && field.field_size > 2) {
                std::vector<uint8_t> utf16_data = field.field_data;
                // Handle padding bytes
                if (!utf16_data.empty() && utf16_data[0] == 0x00 && utf16_data.back() == 0x00) {
                    utf16_data.erase(utf16_data.begin());
                    utf16_data.pop_back();
                }
                audiobook.filename = utf16le_to_utf8(utf16_data);
            }
            // Field 0x46: Author name (UTF-16LE)
            else if (field.field_id == 0x46 && field.field_size > 2) {
                std::vector<uint8_t> utf16_data = field.field_data;
                if (!utf16_data.empty() && utf16_data[0] == 0x00 && utf16_data.back() == 0x00) {
                    utf16_data.erase(utf16_data.begin());
                    utf16_data.pop_back();
                }
                audiobook.author = utf16le_to_utf8(utf16_data);
            }
            // Field 0x70: Last played/skipped timestamp (Windows FILETIME)
            else if (field.field_id == 0x70 && field.field_size == 8) {
                audiobook.last_played_timestamp = read_uint64_le(field.field_data, 0);
            }
        }
    }

    return audiobook;
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

    // Release year from FILETIME at offset 12 (8 bytes)
    // FILETIME = 100-nanosecond intervals since January 1, 1601
    uint64_t filetime = read_uint64_le(record_data, 12);
    if (filetime > 0) {
        // Convert FILETIME to Unix timestamp, then extract year
        // FILETIME epoch: 1601-01-01, Unix epoch: 1970-01-01
        // Difference: 11644473600 seconds
        constexpr uint64_t TICKS_PER_SECOND = 10000000ULL;
        constexpr uint64_t EPOCH_DIFF_SECONDS = 11644473600ULL;

        uint64_t seconds_since_1601 = filetime / TICKS_PER_SECOND;
        if (seconds_since_1601 > EPOCH_DIFF_SECONDS) {
            time_t unix_time = static_cast<time_t>(seconds_since_1601 - EPOCH_DIFF_SECONDS);
            struct tm tm_result;
#ifdef _WIN32
            // Windows: gmtime_s has reversed parameter order and returns errno_t
            if (gmtime_s(&tm_result, &unix_time) == 0) {
                album.release_year = tm_result.tm_year + 1900;
            }
#else
            // POSIX (macOS/Linux): gmtime_r returns pointer to result
            if (gmtime_r(&unix_time, &tm_result) != nullptr) {
                album.release_year = tm_result.tm_year + 1900;
            }
#endif
        }
    }

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

        case Schema::AudiobookTitle:  // 0x11
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

    // Include all artists (including GUID/root artists) so albums can reference them
    auto artist = parse_artist(record_data, atom_id);
    if (artist.has_value()) {
        artist_cache_[atom_id] = artist.value();
        return artist->name;
    }

    return "";
}

std::string ZuneHDParser::resolve_genre(uint32_t atom_id) {
    // Check cache first
    if (genre_cache_.count(atom_id)) {
        return genre_cache_[atom_id].name;
    }

    std::string name = resolve_string_reference(atom_id);

    // Cache the genre for metadata collection
    if (!name.empty()) {
        ZMDBGenre genre;
        genre.atom_id = atom_id;
        genre.name = name;
        genre_cache_[atom_id] = genre;
    }

    return name;
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

            case Schema::AudiobookTrack:
            {
                if (library.audiobook_count < library.audiobooks_capacity) {
                    auto audiobook = parse_audiobook_track(record_data, atom_id);
                    if (audiobook.has_value()) {
                        new (&library.audiobooks[library.audiobook_count]) ZMDBAudiobook(std::move(audiobook.value()));
                        library.audiobook_count++;
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
