#include "ZuneClassicParser.h"
#include "ZMDBUtils.h"
#include <cstring>
#include <iostream>
#include <sstream>
#include <iomanip>

namespace zmdb {

ZMDBLibrary ZuneClassicParser::ExtractLibrary(const std::vector<uint8_t>& zmdb_data) {
    ZMDBLibrary library;
    library.device_type = DeviceType::Zune30;

    std::cout << "[ZuneClassicParser] Starting ZMDB extraction, data size: " << zmdb_data.size() << " bytes" << std::endl;

    if (zmdb_data.empty()) {
        std::cout << "[ZuneClassicParser] ERROR: Empty ZMDB data" << std::endl;
        return library;
    }

    zmdb_data_ = zmdb_data;

    // Parse ZMDB header
    if (zmdb_data_.size() < 0x10) {
        std::cout << "[ZuneClassicParser] ERROR: ZMDB data too small for header (size: " << zmdb_data_.size() << ")" << std::endl;
        return library;
    }

    // Verify ZMDB magic
    if (zmdb_data_[0] != 'Z' || zmdb_data_[1] != 'M' ||
        zmdb_data_[2] != 'D' || zmdb_data_[3] != 'B') {
        std::cout << "[ZuneClassicParser] ERROR: Invalid ZMDB magic. Got: " 
                  << std::hex << (int)zmdb_data_[0] << " " << (int)zmdb_data_[1] 
                  << " " << (int)zmdb_data_[2] << " " << (int)zmdb_data_[3] << std::dec << std::endl;
        return library;
    }
    std::cout << "[ZuneClassicParser] ZMDB magic verified" << std::endl;

    // Parse ZMed header at offset 0x20
    if (zmdb_data_.size() < 0x30) {
        std::cout << "[ZuneClassicParser] ERROR: ZMDB data too small for ZMed header" << std::endl;
        return library;
    }

    if (zmdb_data_[0x20] != 'Z' || zmdb_data_[0x21] != 'M' ||
        zmdb_data_[0x22] != 'e' || zmdb_data_[0x23] != 'd') {
        std::cout << "[ZuneClassicParser] ERROR: Invalid ZMed magic at 0x20. Got: "
                  << std::hex << (int)zmdb_data_[0x20] << " " << (int)zmdb_data_[0x21] 
                  << " " << (int)zmdb_data_[0x22] << " " << (int)zmdb_data_[0x23] << std::dec << std::endl;
        return library;
    }
    std::cout << "[ZuneClassicParser] ZMed magic verified at offset 0x20" << std::endl;

    // Check ZMed version (should be 2 for Classic, 5 for HD)
    uint16_t zmed_version = read_uint16_le(zmdb_data_, 0x24);
    std::cout << "[ZuneClassicParser] ZMed version: " << zmed_version << " (expected 2 for Classic, 5 for HD)" << std::endl;
    if (zmed_version != 2) {
        std::cerr << "[ZuneClassicParser] Warning: Unexpected ZMed version " << zmed_version << " for Zune Classic" << std::endl;
    }

    // Find ZArr descriptors - search for first "ZArr" after ZMed header
    size_t descriptor_offset = 0;
    std::cout << "[ZuneClassicParser] Searching for ZArr descriptors starting at 0x30..." << std::endl;
    for (size_t offset = 0x30; offset < 0x100 && offset + 4 <= zmdb_data_.size(); offset += 4) {
        if (zmdb_data_[offset] == 'Z' && zmdb_data_[offset + 1] == 'A' &&
            zmdb_data_[offset + 2] == 'r' && zmdb_data_[offset + 3] == 'r') {
            descriptor_offset = offset;
            std::cout << "[ZuneClassicParser] Found ZArr descriptors at offset 0x" << std::hex << offset << std::dec << std::endl;
            break;
        }
    }

    if (descriptor_offset == 0) {
        std::cout << "[ZuneClassicParser] ERROR: No ZArr descriptors found" << std::endl;
        return library;
    }

    // Parse 96 descriptors
    descriptors_.resize(96);
    std::cout << "[ZuneClassicParser] Parsing 96 ZArr descriptors..." << std::endl;
    for (int i = 0; i < 96; i++) {
        size_t desc_offset = descriptor_offset + (i * 20);
        if (desc_offset + 20 > zmdb_data_.size()) {
            std::cout << "[ZuneClassicParser] Stopped at descriptor " << i << " (out of bounds)" << std::endl;
            break;
        }

        descriptors_[i].entry_size = read_uint16_le(zmdb_data_, desc_offset + 6);
        descriptors_[i].entry_count = read_uint32_le(zmdb_data_, desc_offset + 8);
        descriptors_[i].data_offset = read_uint32_le(zmdb_data_, desc_offset + 16);
        
        if (descriptors_[i].entry_count > 0) {
            std::cout << "[ZuneClassicParser] Descriptor " << i << ": entry_size=" << descriptors_[i].entry_size
                      << ", entry_count=" << descriptors_[i].entry_count
                      << ", data_offset=0x" << std::hex << descriptors_[i].data_offset << std::dec << std::endl;
        }
    }

    // Build index table from descriptor 0
    if (descriptors_[0].entry_count > 0 && descriptors_[0].entry_size == 8) {
        std::cout << "[ZuneClassicParser] Building index table from descriptor 0 with " 
                  << descriptors_[0].entry_count << " entries" << std::endl;
        index_table_ = build_index_table(
            zmdb_data_,
            descriptors_[0].data_offset,
            descriptors_[0].entry_count
        );
        std::cout << "[ZuneClassicParser] Index table built with " << index_table_.size() << " entries" << std::endl;
    } else {
        std::cout << "[ZuneClassicParser] ERROR: Descriptor 0 invalid for index table" << std::endl;
    }

    // Allocate arrays with exact sizes from descriptors (single allocation, no reallocation)
    // Note: Zune Classic uses different descriptor mappings than Zune HD
    if (descriptors_.size() > 1 && descriptors_[1].entry_count > 0) {
        library.tracks_capacity = descriptors_[1].entry_count;
        library.tracks = static_cast<ZMDBTrack*>(::operator new[](library.tracks_capacity * sizeof(ZMDBTrack)));
    }
    if (descriptors_.size() > 2 && descriptors_[2].entry_count > 0) {
        // Playlists on Classic use descriptor 2 (vs 11 on HD)
        library.playlists_capacity = descriptors_[2].entry_count;
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
    if (descriptors_.size() > 27 && descriptors_[27].entry_count > 0) {
        // Audiobooks on Classic use descriptor 27 (vs 26 on HD)
        library.audiobooks_capacity = descriptors_[27].entry_count;
        library.audiobooks = static_cast<ZMDBAudiobook*>(::operator new[](library.audiobooks_capacity * sizeof(ZMDBAudiobook)));
    }

    // Parse directly into arrays (no intermediate vectors, no reallocation)
    std::cout << "[ZuneClassicParser] Starting media extraction from descriptors..." << std::endl;
    
    try {
        std::cout << "[ZuneClassicParser] Extracting music tracks from descriptor 1..." << std::endl;
        extract_media_from_descriptor(1, Schema::Music, library);      // Music tracks
        std::cout << "[ZuneClassicParser] Extracted " << library.track_count << " tracks" << std::endl;
    } catch (const std::exception& e) {
        throw std::runtime_error(std::string("Music parsing failed: ") + e.what());
    }

    try {
        std::cout << "[ZuneClassicParser] Extracting playlists from descriptor 2 (Classic mapping)..." << std::endl;
        extract_media_from_descriptor(2, Schema::Playlist, library);   // Playlists (Classic: 2, HD: 11)
        std::cout << "[ZuneClassicParser] Extracted " << library.playlist_count << " playlists" << std::endl;
    } catch (const std::exception& e) {
        throw std::runtime_error(std::string("Playlist parsing failed: ") + e.what());
    }

    try {
        std::cout << "[ZuneClassicParser] Extracting videos from descriptor 12..." << std::endl;
        extract_media_from_descriptor(12, Schema::Video, library);     // Videos
        std::cout << "[ZuneClassicParser] Extracted " << library.video_count << " videos" << std::endl;
    } catch (const std::exception& e) {
        throw std::runtime_error(std::string("Video parsing failed: ") + e.what());
    }

    try {
        std::cout << "[ZuneClassicParser] Extracting pictures from descriptor 16..." << std::endl;
        extract_media_from_descriptor(16, Schema::Picture, library);   // Pictures
        std::cout << "[ZuneClassicParser] Extracted " << library.picture_count << " pictures" << std::endl;
    } catch (const std::exception& e) {
        throw std::runtime_error(std::string("Picture parsing failed: ") + e.what());
    }

    try {
        std::cout << "[ZuneClassicParser] Extracting podcasts from descriptor 19..." << std::endl;
        extract_media_from_descriptor(19, Schema::PodcastEpisode, library); // Podcast episodes
        std::cout << "[ZuneClassicParser] Extracted " << library.podcast_count << " podcasts" << std::endl;
    } catch (const std::exception& e) {
        throw std::runtime_error(std::string("Podcast parsing failed: ") + e.what());
    }

    try {
        std::cout << "[ZuneClassicParser] Extracting audiobooks from descriptor 27 (Classic mapping)..." << std::endl;
        extract_media_from_descriptor(27, Schema::AudiobookTrack, library); // Audiobook tracks (Classic: 27, HD: 26)
        std::cout << "[ZuneClassicParser] Extracted " << library.audiobook_count << " audiobooks" << std::endl;
    } catch (const std::exception& e) {
        throw std::runtime_error(std::string("Audiobook parsing failed: ") + e.what());
    }

    // Move album metadata from cache (no tracks - consumer groups by album_ref)
    try {
        std::cout << "[ZuneClassicParser] Moving album metadata from cache..." << std::endl;
        std::cout << "[ZuneClassicParser] Album cache contains " << album_cache_.size() << " albums:" << std::endl;
        
        // Debug: Print all albums in cache
        for (const auto& [atom_id, album] : album_cache_) {
            std::cout << "[ZuneClassicParser]   Album: atom_id=0x" << std::hex << atom_id << std::dec
                      << ", title=\"" << album.title << "\""
                      << ", artist=\"" << album.artist_name << "\""
                      << ", alb_ref=\"" << album.alb_reference << "\""
                      << ", album_pid=0x" << std::hex << album.album_pid << std::dec << std::endl;
        }
        
        library.album_metadata = std::move(album_cache_);
        library.album_count = library.album_metadata.size();
        std::cout << "[ZuneClassicParser] Moved " << library.album_count << " albums to library" << std::endl;
    } catch (const std::exception& e) {
        throw std::runtime_error(std::string("Album metadata move failed: ") + e.what());
    }

    std::cout << "[ZuneClassicParser] ZMDB extraction complete: "
              << library.track_count << " tracks, "
              << library.album_count << " albums, "
              << library.playlist_count << " playlists, "
              << library.video_count << " videos, "
              << library.picture_count << " pictures, "
              << library.podcast_count << " podcasts, "
              << library.audiobook_count << " audiobooks" << std::endl;

    return library;
}

bool ZuneClassicParser::should_filter_record(
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

size_t ZuneClassicParser::get_entry_size_for_schema(uint8_t schema_type) const {
    // Zune Classic uses the same entry sizes as documented in the wiki
    // These match what's in ZMDBUtils::get_entry_size_for_schema
    return ::zmdb::get_entry_size_for_schema(schema_type);
}

// All parsing methods are identical to ZuneHD since the record structures are the same
// The only difference is which descriptors contain which schemas

std::optional<ZMDBTrack> ZuneClassicParser::parse_music_track(
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

    // Read fixed fields - ZuneClassic structure differs from ZuneHD
    track.duration_ms = read_int32_le(record_data, 16);
    
    // Bytes 20-23: track_number (byte 20) + metadata_count (byte 22)
    track.track_number = record_data[20];  // Just byte 20, not uint32
    uint8_t metadata_record_count = record_data[22];  // Number of 6-byte metadata records after title
    
    track.codec_id = read_uint16_le(record_data, 24);
    track.rating = record_data[26];
    track.file_size_bytes = 0;  // Not stored in ZuneClassic fixed fields

    // Read title at offset 28
    size_t title_end = 28;
    if (record_data.size() > 28) {
        track.title = read_null_terminated_utf8(record_data, 28);
        title_end = 28 + track.title.length() + 1;  // +1 for null terminator
    }
    
    // Initialize default values
    track.playcount = 0;
    track.skip_count = 0;
    
    // Parse metadata records if present
    if (metadata_record_count > 0 && title_end + (metadata_record_count * 6) <= record_data.size()) {
        std::cout << "[ZuneClassicParser::parse_music_track] Track \"" << track.title 
                  << "\" has " << (int)metadata_record_count << " metadata records" << std::endl;
        
        size_t metadata_offset = title_end;
        for (uint8_t i = 0; i < metadata_record_count; i++) {
            if (metadata_offset + 6 > record_data.size()) {
                std::cout << "[ZuneClassicParser::parse_music_track] Insufficient data for metadata record " 
                          << (int)i << std::endl;
                break;
            }
            
            // Each record is 6 bytes: 4-byte count + 0x04 + type byte
            uint32_t count = read_uint32_le(record_data, metadata_offset);
            uint8_t marker = record_data[metadata_offset + 4];  // Should be 0x04
            uint8_t type = record_data[metadata_offset + 5];
            
            if (marker != 0x04) {
                std::cout << "[ZuneClassicParser::parse_music_track] WARNING: Expected marker 0x04, got 0x" 
                          << std::hex << (int)marker << std::dec << std::endl;
            }
            
            switch (type) {
                case 0x62:  // Play count
                    track.playcount = count;
                    std::cout << "[ZuneClassicParser::parse_music_track]   Play count: " << count << std::endl;
                    break;
                case 0x63:  // Skip count
                    track.skip_count = count;
                    std::cout << "[ZuneClassicParser::parse_music_track]   Skip count: " << count << std::endl;
                    break;
                default:
                    std::cout << "[ZuneClassicParser::parse_music_track]   Unknown metadata type 0x" 
                              << std::hex << (int)type << std::dec << " with value " << count << std::endl;
                    break;
            }
            
            metadata_offset += 6;
        }
    }

    // Resolve references
    if (album_ref != 0) {
        std::cout << "[ZuneClassicParser::parse_music_track] Resolving album_ref=0x" << std::hex << album_ref << std::dec << std::endl;
        auto album_info = resolve_album_info(album_ref);
        if (album_info.has_value()) {
            track.album_name = album_info->second;
            track.album_ref = album_ref;
            
            // Get album artist name and GUID from album (like ZuneHD does)
            if (album_cache_.count(album_ref)) {
                track.album_artist_name = album_cache_[album_ref].artist_name;
                track.album_artist_guid = album_cache_[album_ref].artist_guid;
                std::cout << "[ZuneClassicParser::parse_music_track] Set album artist: \"" << track.album_artist_name << "\" (GUID: \"" << track.album_artist_guid << "\")" << std::endl;
            }
            
            std::cout << "[ZuneClassicParser::parse_music_track] Resolved album: \"" << track.album_name << "\"" << std::endl;
        } else {
            std::cout << "[ZuneClassicParser::parse_music_track] Failed to resolve album_ref" << std::endl;
        }
    }

    if (artist_ref != 0) {
        std::cout << "[ZuneClassicParser::parse_music_track] Resolving artist_ref=0x" << std::hex << artist_ref << std::dec << std::endl;
        
        // Ensure artist is fully parsed and cached (like ZuneHD does)
        if (!artist_cache_.count(artist_ref)) {
            if (index_table_.count(artist_ref)) {
                uint32_t record_offset = index_table_[artist_ref];
                auto record_opt = read_record_at_offset(zmdb_data_, record_offset);
                if (record_opt.has_value()) {
                    const auto& rec_data = record_opt->second;
                    uint32_t ref0 = read_uint32_le(rec_data, 0);
                    if (ref0 != 0) {  // Skip GUID/root artists (like ZuneHD does)
                        auto artist = parse_artist(rec_data, artist_ref);
                        if (artist.has_value()) {
                            artist_cache_[artist_ref] = artist.value();
                            std::cout << "[ZuneClassicParser::parse_music_track] Cached artist: \"" << artist->name << "\" with GUID: \"" << artist->guid << "\"" << std::endl;
                        } else {
                            std::cout << "[ZuneClassicParser::parse_music_track] Failed to parse artist record" << std::endl;
                        }
                    } else {
                        std::cout << "[ZuneClassicParser::parse_music_track] Skipped GUID artist (ref0=0)" << std::endl;
                    }
                } else {
                    std::cout << "[ZuneClassicParser::parse_music_track] Failed to read artist record at offset" << std::endl;
                }
            } else {
                std::cout << "[ZuneClassicParser::parse_music_track] Artist ref not found in index table" << std::endl;
            }
        }
        
        // Get artist name and GUID from cache (like ZuneHD does)
        if (artist_cache_.count(artist_ref)) {
            track.artist_name = artist_cache_[artist_ref].name;
            track.artist_guid = artist_cache_[artist_ref].guid;
            std::cout << "[ZuneClassicParser::parse_music_track] Resolved artist: \"" << track.artist_name << "\" (GUID: \"" << track.artist_guid << "\")" << std::endl;
        } else {
            std::cout << "[ZuneClassicParser::parse_music_track] Artist not found in cache after resolution attempt" << std::endl;
        }
    }

    if (genre_ref != 0) {
        track.genre = resolve_genre(genre_ref);
    }

    if (album_filename_ref != 0) {
        track.filename = resolve_string_reference(album_filename_ref);
    }

    // Parse backwards varints for optional fields
    // TODO: ZuneClassic appears to store these fields differently - investigate
    /*
    size_t entry_size = get_entry_size_for_schema(Schema::Music);
    if (entry_size > 0) {
        auto fields = parse_backwards_varints(record_data, entry_size);
        for (const auto& field : fields) {
            switch (field.field_id) {
                case 0x6c:  // Disc number (optional, default=1)
                    if (field.field_size == 1) {
                        track.disc_number = field.field_data[0];
                    }
                    break;
                case 0x63:  // Skip count
                    if (field.field_size == 2) {
                        track.skip_count = read_uint16_le(field.field_data, 0);
                    }
                    break;
                case 0x70:  // Last played timestamp
                    if (field.field_size == 8) {
                        track.last_played_timestamp = read_uint64_le(field.field_data, 0);
                    }
                    break;
                case 0x44:  // Filename (UTF-16LE)
                    if (field.field_size > 2) {
                        // Handle padding bytes
                        if (field.field_data[0] == 0x00 && field.field_data[field.field_size - 1] == 0x00) {
                            track.filename = utf16le_to_utf8(std::vector<uint8_t>(
                                field.field_data.begin() + 1,
                                field.field_data.end() - 1
                            ));
                        } else {
                            track.filename = utf16le_to_utf8(field.field_data);
                        }
                    }
                    break;
            }
        }
    }
    */
    
    // Set defaults for fields not found in ZuneClassic track records
    track.disc_number = 1;  // Default disc number
    track.skip_count = 0;
    track.last_played_timestamp = 0;

    return track;
}

std::optional<ZMDBVideo> ZuneClassicParser::parse_video(
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

std::optional<ZMDBPicture> ZuneClassicParser::parse_picture(
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

std::optional<ZMDBPlaylist> ZuneClassicParser::parse_playlist(
    const std::vector<uint8_t>& record_data,
    uint32_t atom_id
) {
    if (record_data.size() < 12) {
        return std::nullopt;
    }

    ZMDBPlaylist playlist;
    playlist.atom_id = atom_id;

    // Read fixed fields
    playlist.track_count = read_uint32_le(record_data, 0);
    uint32_t ref1 = read_uint32_le(record_data, 4);  // Unknown
    uint32_t folder_ref = read_uint32_le(record_data, 8);

    // Resolve folder reference
    if (folder_ref != 0) {
        playlist.folder = resolve_string_reference(folder_ref);
    }

    // Parse variable section
    size_t offset = 12;
    if (offset >= record_data.size()) {
        return playlist;
    }

    // 1. Playlist name (UTF-8, null-terminated)
    playlist.name = read_null_terminated_utf8(record_data, offset);
    offset += playlist.name.length() + 1;

    // 2. GUID (16 bytes)
    if (offset + 16 <= record_data.size()) {
        std::vector<uint8_t> guid_bytes(
            record_data.begin() + offset,
            record_data.begin() + offset + 16
        );
        playlist.guid = parse_windows_guid(guid_bytes);
        offset += 16;
    }

    // 3. Padding (2 bytes)
    offset += 2;

    // 4. Filename (UTF-16LE, double-null terminated)
    if (offset < record_data.size()) {
        playlist.filename = read_utf16le_until_double_null(record_data, offset);
        // Move past filename and double null
        offset += (playlist.filename.length() + 1) * 2;
    }

    // 5. Padding (2 bytes)
    offset += 2;

    // 6. Track list (array of uint32 atom IDs)
    while (offset + 4 <= record_data.size()) {
        uint32_t track_atom_id = read_uint32_le(record_data, offset);
        if (track_atom_id == 0) {
            break;  // End marker
        }
        playlist.track_atom_ids.push_back(track_atom_id);
        offset += 4;
    }

    return playlist;
}

std::optional<ZMDBPodcast> ZuneClassicParser::parse_podcast_episode(
    const std::vector<uint8_t>& record_data,
    uint32_t atom_id
) {
    if (record_data.size() < 32) {
        return std::nullopt;
    }

    ZMDBPodcast podcast;
    podcast.atom_id = atom_id;

    // Read reference fields (0-15)
    uint32_t show_name_ref = read_uint32_le(record_data, 0);
    uint32_t podcast_show_ref = read_uint32_le(record_data, 4);
    podcast.duration_ms = read_uint32_le(record_data, 8);
    podcast.ref3 = read_uint32_le(record_data, 12);

    // Read timestamp and file size (16-27)
    podcast.timestamp = read_uint64_le(record_data, 16);
    podcast.file_size_bytes = read_uint32_le(record_data, 24);

    // Read codec at offset 30-31
    podcast.codec_id = read_uint16_le(record_data, 30);

    // Resolve show name
    if (show_name_ref != 0) {
        podcast.show_name = resolve_string_reference(show_name_ref);
    }

    // Parse variable section starting at offset 36 (after 4 null bytes at 32-35)
    size_t offset = 36;
    if (offset >= record_data.size()) {
        return podcast;
    }

    // 1. Episode title (UTF-8, null-terminated)
    podcast.title = read_null_terminated_utf8(record_data, offset);
    offset += podcast.title.length() + 1;

    // 2. Author/email (UTF-16LE, double-null terminated)
    if (offset < record_data.size()) {
        podcast.author = read_utf16le_until_double_null(record_data, offset);
        offset += (podcast.author.length() + 1) * 2;
    }

    // 3. Marker (2-3 bytes like "RF", "RFA", "RFC")
    if (offset + 2 <= record_data.size()) {
        std::string marker(reinterpret_cast<const char*>(&record_data[offset]), 2);
        offset += 2;
        if (offset < record_data.size() && std::isalpha(record_data[offset])) {
            marker += static_cast<char>(record_data[offset]);
            offset++;
        }

        // 4. Description (UTF-16LE starting with marker's last character)
        if (offset < record_data.size() && !marker.empty()) {
            // The description starts with the last character of the marker
            std::vector<uint8_t> desc_data;
            desc_data.push_back(marker.back());
            desc_data.push_back(0);  // UTF-16LE encoding

            // Read rest of description
            while (offset + 1 < record_data.size()) {
                if (record_data[offset] == 0 && record_data[offset + 1] == 0) {
                    break;  // Double null terminator
                }
                desc_data.push_back(record_data[offset]);
                desc_data.push_back(record_data[offset + 1]);
                offset += 2;
            }

            podcast.description = utf16le_to_utf8(desc_data);
        }
    }

    // Parse backwards varints for URL fields
    size_t entry_size = get_entry_size_for_schema(Schema::PodcastEpisode);
    if (entry_size > 0) {
        auto fields = parse_backwards_varints(record_data, entry_size);
        for (const auto& field : fields) {
            // Large varint fields typically contain URLs
            if (field.field_size > 100 && field.field_size < 1000) {
                // URL format: marker byte + UTF-16LE + padding byte
                if (field.field_data.size() > 3) {
                    std::vector<uint8_t> url_data(
                        field.field_data.begin() + 1,
                        field.field_data.end() - 1
                    );
                    std::string url = utf16le_to_utf8(url_data);
                    
                    if (url.find("http") != std::string::npos) {
                        if (url.find(".mp3") != std::string::npos || 
                            url.find(".m4a") != std::string::npos ||
                            url.find("/audio/") != std::string::npos) {
                            podcast.audio_url = url;
                        } else if (url.find(".rss") != std::string::npos || 
                                   url.find("/rss") != std::string::npos ||
                                   url.find("/feed") != std::string::npos) {
                            podcast.rss_url = url;
                        }
                    }
                }
            }
        }
    }

    return podcast;
}

std::optional<ZMDBAudiobook> ZuneClassicParser::parse_audiobook_track(
    const std::vector<uint8_t>& record_data,
    uint32_t atom_id
) {
    if (record_data.size() < 36) {
        return std::nullopt;
    }

    ZMDBAudiobook audiobook;
    audiobook.atom_id = atom_id;

    // Read reference fields (0-7)
    audiobook.title_ref = read_uint32_le(record_data, 0);
    audiobook.filename_ref = read_uint32_le(record_data, 4);

    // Read duration and playback position (8-15)
    audiobook.duration_ms = read_uint32_le(record_data, 8);
    audiobook.playback_position_ms = read_uint32_le(record_data, 12);

    // Skip unknown fields at 16-23

    // Read file size (24-27)
    audiobook.file_size_bytes = read_uint32_le(record_data, 24);

    // Read track number and playcount (28-31)
    audiobook.track_number = read_uint16_le(record_data, 28);
    audiobook.playcount = read_uint16_le(record_data, 30);

    // Read format code (32-33)
    audiobook.format_code = read_uint16_le(record_data, 32);

    // Read title at offset 36
    if (record_data.size() > 36) {
        audiobook.title = read_null_terminated_utf8(record_data, 36);
    }

    // Resolve audiobook name from title reference
    if (audiobook.title_ref != 0) {
        audiobook.audiobook_name = resolve_string_reference(audiobook.title_ref);
    }

    // Parse backwards varints for optional fields
    size_t entry_size = get_entry_size_for_schema(Schema::AudiobookTrack);
    if (entry_size > 0) {
        auto fields = parse_backwards_varints(record_data, entry_size);
        for (const auto& field : fields) {
            switch (field.field_id) {
                case 0x46:  // Author (UTF-16LE)
                    if (field.field_size > 2) {
                        audiobook.author = utf16le_to_utf8(field.field_data);
                    }
                    break;
                case 0x44:  // Filename (UTF-16LE)
                    if (field.field_size > 2) {
                        // Handle padding bytes
                        if (field.field_data[0] == 0x00 && field.field_data[field.field_size - 1] == 0x00) {
                            audiobook.filename = utf16le_to_utf8(std::vector<uint8_t>(
                                field.field_data.begin() + 1,
                                field.field_data.end() - 1
                            ));
                        } else {
                            audiobook.filename = utf16le_to_utf8(field.field_data);
                        }
                    }
                    break;
                case 0x70:  // Last played timestamp
                    if (field.field_size == 8) {
                        audiobook.last_played_timestamp = read_uint64_le(field.field_data, 0);
                    }
                    break;
            }
        }
    }

    return audiobook;
}

std::optional<ZMDBAlbum> ZuneClassicParser::parse_album(
    const std::vector<uint8_t>& record_data,
    uint32_t atom_id
) {
    std::cout << "[ZuneClassicParser::parse_album] Parsing album with atom_id=0x" << std::hex << atom_id << std::dec 
              << ", record_size=" << record_data.size() << std::endl;

    if (record_data.size() < 20) {
        std::cout << "[ZuneClassicParser::parse_album] ERROR: Record too small (" << record_data.size() << " < 20)" << std::endl;
        return std::nullopt;
    }

    ZMDBAlbum album;
    album.atom_id = atom_id;

    // Read artist reference at offset 0-3
    album.artist_ref = read_uint32_le(record_data, 0);

    // Read other reference fields
    uint32_t ref1 = read_uint32_le(record_data, 4);
    uint32_t category_ref = read_uint32_le(record_data, 8);

    // Read clean title at offset 12 (ZuneClassic structure differs from ZuneHD)
    // ZuneClassic: artist_ref, ref1, category_ref, then title
    // ZuneHD: artist_ref, ref1, category_ref, ref3, ref4, then title
    size_t title_end = 12;
    if (record_data.size() > 12) {
        album.title = read_null_terminated_utf8(record_data, 12);
        title_end = 12 + album.title.length() + 1; // +1 for null terminator
    }

    // Calculate album property ID in 0x0600xxxx format
    uint32_t entry_id = atom_id & 0x00FFFFFF;
    album.album_pid = 0x06000000 | entry_id;

    // Resolve artist name and GUID
    if (album.artist_ref != 0) {
        // Try to get artist info from cache first
        if (artist_cache_.count(album.artist_ref)) {
            auto& artist = artist_cache_[album.artist_ref];
            album.artist_name = artist.name;
            album.artist_guid = artist.guid;
        } else {
            // Lookup and parse artist record
            if (index_table_.count(album.artist_ref)) {
                uint32_t record_offset = index_table_[album.artist_ref];
                auto record_opt = read_record_at_offset(zmdb_data_, record_offset);
                if (record_opt.has_value()) {
                    auto artist = parse_artist(record_opt->second, album.artist_ref);
                    if (artist.has_value()) {
                        artist_cache_[album.artist_ref] = artist.value();
                        album.artist_name = artist->name;
                        album.artist_guid = artist->guid;
                    }
                }
            }
        }
    }

    // Parse album filename (.alb reference) - ZuneClassic stores this as UTF-16LE after the title
    // ZuneHD stores it in backwards varints, but ZuneClassic uses a different approach
    if (title_end < record_data.size()) {
        // Look for UTF-16LE string after the title
        size_t utf16_start = title_end;
        size_t utf16_end = utf16_start;
        
        // Find the end of the UTF-16LE string (double null terminator)
        while (utf16_end + 1 < record_data.size()) {
            if (record_data[utf16_end] == 0 && record_data[utf16_end + 1] == 0) {
                break;
            }
            utf16_end += 2;
        }
        
        if (utf16_end > utf16_start) {
            std::vector<uint8_t> utf16_data(record_data.begin() + utf16_start, record_data.begin() + utf16_end);
            album.alb_reference = utf16le_to_utf8(utf16_data);
            std::cout << "[ZuneClassicParser::parse_album] Found alb_reference after title: \"" << album.alb_reference << "\"" << std::endl;
        }
    }

    std::cout << "[ZuneClassicParser::parse_album] Album parsed: title=\"" << album.title 
              << "\", artist=\"" << album.artist_name 
              << "\", alb_ref=\"" << album.alb_reference 
              << "\", album_pid=0x" << std::hex << album.album_pid << std::dec << std::endl;

    return album;
}

std::optional<ZMDBArtist> ZuneClassicParser::parse_artist(
    const std::vector<uint8_t>& record_data,
    uint32_t atom_id
) {
    if (record_data.size() < 4) {
        return std::nullopt;
    }

    ZMDBArtist artist;
    artist.atom_id = atom_id;

    // Read category reference at offset 0-3
    uint32_t category_ref = read_uint32_le(record_data, 0);

    // Skip GUID artists (category_ref == 0)
    if (category_ref == 0) {
        return std::nullopt;
    }

    // Read clean name at offset 1
    if (record_data.size() > 1) {
        artist.name = read_null_terminated_utf8(record_data, 1);
    }

    // Parse artist filename and GUID from backwards varints
    size_t entry_size = get_entry_size_for_schema(Schema::Artist);
    if (entry_size > 0) {
        auto fields = parse_backwards_varints(record_data, entry_size);
        for (const auto& field : fields) {
            switch (field.field_id) {
                case 0x44:  // Artist filename with .art extension
                    if (field.field_size > 2) {
                        artist.filename = utf16le_to_utf8(field.field_data);
                    }
                    break;
                case 0x14:  // Artist GUID (16 bytes)
                    if (field.field_size == 16) {
                        artist.guid = parse_windows_guid(field.field_data);
                    }
                    break;
            }
        }
    }

    return artist;
}

// Reference resolution methods
std::string ZuneClassicParser::resolve_string_reference(uint32_t atom_id) {
    // Check cache first
    if (string_cache_.count(atom_id)) {
        return string_cache_[atom_id];
    }

    // Lookup record in index table
    if (!index_table_.count(atom_id)) {
        return "";
    }

    uint32_t record_offset = index_table_[atom_id];
    auto record_opt = read_record_at_offset(zmdb_data_, record_offset);
    if (!record_opt.has_value()) {
        return "";
    }

    const auto& record_data = record_opt->second;
    uint8_t schema_type = get_schema_type(atom_id);
    std::string result;

    switch (schema_type) {
        case Schema::Filename:      // 0x05
            if (record_data.size() > 8) {
                result = read_null_terminated_utf8(record_data, 8);
            }
            break;
        case Schema::VideoTitle:    // 0x0a
        case Schema::PhotoAlbum:    // 0x0b
        case Schema::Collection:    // 0x0c
            if (record_data.size() > 4) {
                result = read_null_terminated_utf8(record_data, 4);
            }
            break;
        case Schema::Genre:         // 0x09
            if (record_data.size() > 1) {
                result = read_null_terminated_utf8(record_data, 1);
            }
            break;
        case Schema::AudiobookTitle: // 0x11
            if (record_data.size() > 8) {
                result = read_null_terminated_utf8(record_data, 8);
            }
            break;
    }

    string_cache_[atom_id] = result;
    return result;
}

std::string ZuneClassicParser::resolve_artist_name(uint32_t atom_id) {
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

    // Check for GUID artist (ref0 == 0) - like ZuneHD does
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

std::string ZuneClassicParser::resolve_genre(uint32_t atom_id) {
    return resolve_string_reference(atom_id);
}

std::optional<std::pair<uint32_t, std::string>> ZuneClassicParser::resolve_album_info(uint32_t atom_id) {
    std::cout << "[ZuneClassicParser::resolve_album_info] Resolving album atom_id=0x" << std::hex << atom_id << std::dec << std::endl;
    
    // Check cache
    if (album_cache_.count(atom_id)) {
        auto& album = album_cache_[atom_id];
        std::cout << "[ZuneClassicParser::resolve_album_info] Found in cache: \"" << album.title 
                  << "\" (cache size: " << album_cache_.size() << ")" << std::endl;
        return std::make_pair(album.atom_id, album.title);
    }

    std::cout << "[ZuneClassicParser::resolve_album_info] Not in cache, looking up in index table..." << std::endl;
    
    // Lookup and parse album record
    if (!index_table_.count(atom_id)) {
        std::cout << "[ZuneClassicParser::resolve_album_info] ERROR: atom_id not found in index table" << std::endl;
        return std::nullopt;
    }

    uint32_t record_offset = index_table_[atom_id];
    std::cout << "[ZuneClassicParser::resolve_album_info] Found in index at offset=0x" << std::hex << record_offset << std::dec << std::endl;
    
    auto record_opt = read_record_at_offset(zmdb_data_, record_offset);
    if (!record_opt.has_value()) {
        std::cout << "[ZuneClassicParser::resolve_album_info] ERROR: Failed to read record" << std::endl;
        return std::nullopt;
    }

    auto album = parse_album(record_opt->second, atom_id);
    if (album.has_value()) {
        std::cout << "[ZuneClassicParser::resolve_album_info] Adding to cache: \"" << album->title 
                  << "\", alb_ref=\"" << album->alb_reference << "\" (cache size will be: " 
                  << (album_cache_.size() + 1) << ")" << std::endl;
        album_cache_[atom_id] = album.value();
        return std::make_pair(album->atom_id, album->title);
    }

    std::cout << "[ZuneClassicParser::resolve_album_info] ERROR: Failed to parse album" << std::endl;
    return std::nullopt;
}

std::optional<ZMDBTrack> ZuneClassicParser::resolve_track(uint32_t track_atom_id) {
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

void ZuneClassicParser::extract_media_from_descriptor(
    uint32_t descriptor_idx,
    uint8_t expected_schema,
    ZMDBLibrary& library
) {
    std::cout << "[ZuneClassicParser::extract_media] Starting extraction from descriptor " << descriptor_idx 
              << " for schema 0x" << std::hex << (int)expected_schema << std::dec << std::endl;

    if (descriptor_idx >= descriptors_.size()) {
        std::cout << "[ZuneClassicParser::extract_media] ERROR: Descriptor index " << descriptor_idx 
                  << " out of bounds (size: " << descriptors_.size() << ")" << std::endl;
        return;
    }

    const auto& desc = descriptors_[descriptor_idx];
    if (desc.entry_count == 0) {
        std::cout << "[ZuneClassicParser::extract_media] Descriptor " << descriptor_idx << " is empty" << std::endl;
        return;
    }
    
    std::cout << "[ZuneClassicParser::extract_media] Processing " << desc.entry_count 
              << " entries from descriptor " << descriptor_idx << std::endl;

    for (uint32_t i = 0; i < desc.entry_count; i++) {
        size_t entry_offset = desc.data_offset + (i * desc.entry_size);
        if (entry_offset + 4 > zmdb_data_.size()) {
            break;
        }

        uint32_t atom_id = read_uint32_le(zmdb_data_, entry_offset);
        uint8_t schema_type = (atom_id >> 24) & 0xFF;

        // Only log first few entries to avoid spam
        if (i < 5 || (i == desc.entry_count - 1)) {
            std::cout << "[ZuneClassicParser::extract_media] Entry " << i << ": atom_id=0x" << std::hex << atom_id 
                      << ", schema_type=0x" << (int)schema_type << std::dec;
            if (schema_type != expected_schema) {
                std::cout << " (WARNING: expected 0x" << std::hex << (int)expected_schema << std::dec << ")";
            }
            std::cout << std::endl;
        }

        // Lookup record in index table
        if (!index_table_.count(atom_id)) {
            if (i < 5) {
                std::cout << "[ZuneClassicParser::extract_media] Entry " << i << ": Not found in index table" << std::endl;
            }
            continue;
        }

        uint32_t record_offset = index_table_[atom_id];
        auto record_opt = read_record_at_offset(zmdb_data_, record_offset);
        if (!record_opt.has_value()) {
            if (i < 5) {
                std::cout << "[ZuneClassicParser::extract_media] Entry " << i << ": Failed to read record at offset 0x" 
                          << std::hex << record_offset << std::dec << std::endl;
            }
            continue;
        }

        const auto& record_data = record_opt->second;

        // Apply filters
        if (should_filter_record(record_data, schema_type)) {
            if (i < 5) {
                std::cout << "[ZuneClassicParser::extract_media] Entry " << i << ": Filtered out" << std::endl;
            }
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
        }
    }
}

} // namespace zmdb