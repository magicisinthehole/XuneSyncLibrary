#pragma once

#include "ZMDBParserBase.h"
#include "ZMDBUtils.h"
#include <map>
#include <optional>

namespace zmdb {

/**
 * ZMDB parser for Zune Classic devices (Zune 30, Zune 80, Zune 120).
 *
 * Implements schema-based parsing for all media types:
 * - Music tracks (Schema 0x01)
 * - Videos (Schema 0x02)  
 * - Pictures (Schema 0x03)
 * - Playlists (Schema 0x07)
 * - Podcast episodes (Schema 0x10)
 * - Audiobook tracks (Schema 0x12)
 *
 * Key differences from Zune HD:
 * - ZMed version 2 (vs 5 on HD)
 * - Different descriptor-to-schema mappings:
 *   - Music: descriptor 1 (same as HD)
 *   - Videos: descriptor 12 (same as HD) 
 *   - Pictures: descriptor 16 (same as HD)
 *   - Playlists: descriptor 2 (vs 11 on HD)
 *   - Podcasts: descriptor 19 (same as HD)
 *   - Audiobooks: descriptor 27 (vs 26 on HD)
 */
class ZuneClassicParser : public ZMDBParserBase {
public:
    ZuneClassicParser() = default;
    ~ZuneClassicParser() override = default;

    /**
     * Extract complete library from Zune Classic ZMDB file.
     *
     * @param zmdb_data Raw ZMDB file bytes
     * @return Parsed library with all media types
     */
    ZMDBLibrary ExtractLibrary(const std::vector<uint8_t>& zmdb_data) override;

private:
    // Schema parsers (same as ZuneHD)
    std::optional<ZMDBTrack> parse_music_track(
        const std::vector<uint8_t>& record_data,
        uint32_t atom_id
    );

    std::optional<ZMDBVideo> parse_video(
        const std::vector<uint8_t>& record_data,
        uint32_t atom_id
    );

    std::optional<ZMDBPicture> parse_picture(
        const std::vector<uint8_t>& record_data,
        uint32_t atom_id
    );

    std::optional<ZMDBPlaylist> parse_playlist(
        const std::vector<uint8_t>& record_data,
        uint32_t atom_id
    );

    std::optional<ZMDBPodcast> parse_podcast_episode(
        const std::vector<uint8_t>& record_data,
        uint32_t atom_id
    );

    std::optional<ZMDBAudiobook> parse_audiobook_track(
        const std::vector<uint8_t>& record_data,
        uint32_t atom_id
    );

    std::optional<ZMDBAlbum> parse_album(
        const std::vector<uint8_t>& record_data,
        uint32_t atom_id
    );

    std::optional<ZMDBArtist> parse_artist(
        const std::vector<uint8_t>& record_data,
        uint32_t atom_id
    );

    // Reference resolution
    std::string resolve_string_reference(uint32_t atom_id);
    std::string resolve_artist_name(uint32_t atom_id);
    std::string resolve_genre(uint32_t atom_id);
    std::optional<std::pair<uint32_t, std::string>> resolve_album_info(uint32_t atom_id);

    // Track resolution for playlists
    std::optional<ZMDBTrack> resolve_track(uint32_t track_atom_id);

    // Filter implementation
    bool should_filter_record(
        const std::vector<uint8_t>& record_data,
        uint8_t schema_type
    ) const;

    // Descriptor extraction
    void extract_media_from_descriptor(
        uint32_t descriptor_idx,
        uint8_t expected_schema,
        ZMDBLibrary& library
    );

    // Helper to get entry size for schema (Classic-specific mappings)
    size_t get_entry_size_for_schema(uint8_t schema_type) const;

    // Caches for resolved references
    std::map<uint32_t, std::string> string_cache_;
    std::map<uint32_t, ZMDBArtist> artist_cache_;
    std::map<uint32_t, ZMDBAlbum> album_cache_;
    std::map<uint32_t, ZMDBGenre> genre_cache_;

    // Parsed descriptors
    std::vector<Descriptor> descriptors_;
};

} // namespace zmdb
