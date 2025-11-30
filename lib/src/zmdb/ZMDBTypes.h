#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <map>

namespace zmdb {

// Device type enum
enum class DeviceType {
    Unknown,
    Zune30,
    ZuneHD
};

// Schema type constants (from ZMDB analysis)
namespace Schema {
    constexpr uint8_t Music = 0x01;
    constexpr uint8_t Video = 0x02;
    constexpr uint8_t Picture = 0x03;
    constexpr uint8_t Filename = 0x05;
    constexpr uint8_t Album = 0x06;
    constexpr uint8_t Playlist = 0x07;
    constexpr uint8_t Artist = 0x08;
    constexpr uint8_t Genre = 0x09;
    constexpr uint8_t VideoTitle = 0x0a;
    constexpr uint8_t PhotoAlbum = 0x0b;
    constexpr uint8_t Collection = 0x0c;
    constexpr uint8_t PodcastShow = 0x0f;
    constexpr uint8_t PodcastEpisode = 0x10;
    constexpr uint8_t AudiobookTitle = 0x11;
    constexpr uint8_t AudiobookTrack = 0x12;
    constexpr uint8_t AudiobookRef = 0x19;
}

// Music track structure
struct ZMDBTrack {
    std::string title;
    std::string artist_name;
    std::string artist_guid;        // Artist GUID from field 0x14 (optional)
    std::string album_name;
    std::string album_artist_name;
    std::string album_artist_guid;  // Album artist GUID (optional)
    std::string genre;
    int track_number = 0;           // Track number (offset 24-25)
    int disc_number = 0;            // Disc number (varint field 0x6c, default=1 if absent)
    int duration_ms = 0;            // Duration in milliseconds (offset 16-19)
    int file_size_bytes = 0;        // File size in bytes (offset 20-23)
    uint16_t playcount = 0;         // Play count (offset 26-27)
    uint16_t skip_count = 0;        // Skip count (varint field 0x63)
    uint16_t codec_id = 0;          // Format code e.g. 0xb901=WMA (offset 28-29)
    uint8_t rating = 0;             // Rating: 0=neutral, 8=liked, 3=disliked (offset 30)
    uint64_t last_played_timestamp = 0; // Windows FILETIME of last play/skip event (varint field 0x70)
    uint32_t atom_id = 0;
    uint32_t album_ref = 0;         // Album atom_id reference for grouping tracks
    std::string filename;
};

// Album structure (metadata only - tracks are grouped separately)
struct ZMDBAlbum {
    std::string title;
    std::string artist_name;
    std::string artist_guid;    // Album artist GUID (optional)
    int release_year = 0;
    std::string alb_reference;  // .alb file reference
    uint32_t album_pid = 0;  // Property ID (0x0600xxxx format)
    uint32_t atom_id = 0;
    uint32_t artist_ref = 0;    // Artist atom_id reference (offset 0-3 in record)
};

// Artist structure
struct ZMDBArtist {
    std::string name;
    std::string filename;  // .art file reference
    std::string guid;      // Artist GUID from field 0x14 (optional)
    uint32_t atom_id = 0;
};

// Video structure
struct ZMDBVideo {
    std::string title;
    std::string folder;
    uint32_t ref2 = 0;              // Reference field at offset 8, purpose unknown
    int file_size_bytes = 0;
    uint32_t codec_id = 0;
    std::string filename;
    uint32_t atom_id = 0;
};

// Picture structure
struct ZMDBPicture {
    std::string title;
    std::string photo_album;
    std::string user_album;
    std::string collection;
    std::string filename;           // File reference (ref3)
    uint64_t timestamp = 0;
    uint32_t atom_id = 0;
};

// Playlist structure
struct ZMDBPlaylist {
    std::string name;
    std::string filename;
    std::string guid;
    std::string folder;             // Folder reference (usually "Playlists")
    int track_count = 0;
    std::vector<uint32_t> track_atom_ids;  // Track atom_ids (no need to resolve full tracks)
    uint32_t atom_id = 0;
};

// Podcast episode structure
struct ZMDBPodcast {
    std::string title;
    std::string show_name;
    std::string author;
    std::string description;
    std::string audio_url;
    std::string rss_url;
    uint32_t ref3 = 0;              // Reference field at offset 12, purpose unknown
    int duration_ms = 0;
    uint64_t timestamp = 0;
    int file_size_bytes = 0;
    uint32_t codec_id = 0;
    uint32_t atom_id = 0;
};

// Audiobook track structure (Schema 0x12)
struct ZMDBAudiobook {
    std::string title;              // Track/chapter title (UTF-8 at offset 0x24)
    std::string audiobook_name;     // Audiobook title (resolved from title_ref)
    std::string author;             // Author name (backwards varint field 0x46)
    std::string filename;           // Filename (backwards varint field 0x44)
    uint32_t duration_ms = 0;       // Duration in milliseconds (offset 0x08)
    uint32_t playback_position_ms = 0; // Current playback position (offset 0x0C)
    uint32_t file_size_bytes = 0;   // File size in bytes (offset 0x18)
    uint16_t track_number = 0;      // Part/chapter number (offset 0x1C)
    uint16_t playcount = 0;         // Number of times played (offset 0x1E)
    uint16_t format_code = 0;       // 0x3009 (MP3) or 0xB901 (WMA) (offset 0x20)
    uint64_t last_played_timestamp = 0; // Windows FILETIME of last play/skip event (varint field 0x70)
    uint32_t atom_id = 0;           // This record's atom_id
    uint32_t title_ref = 0;         // Reference to AudiobookTitle (Schema 0x11)
    uint32_t filename_ref = 0;      // Reference to folder (Schema 0x05)
};

// Complete ZMDB library structure (uses raw C arrays for zero-copy to C API)
struct ZMDBLibrary {
    DeviceType device_type = DeviceType::Unknown;

    // Flat arrays of media types (allocated once, no reallocation)
    ZMDBTrack* tracks = nullptr;
    ZMDBVideo* videos = nullptr;
    ZMDBPicture* pictures = nullptr;
    ZMDBPlaylist* playlists = nullptr;
    ZMDBPodcast* podcasts = nullptr;
    ZMDBAudiobook* audiobooks = nullptr;

    // Album metadata (kept as map for O(1) lookups during parsing)
    std::map<uint32_t, ZMDBAlbum> album_metadata;

    // Counts
    int album_count = 0;
    int track_count = 0;
    int video_count = 0;
    int picture_count = 0;
    int playlist_count = 0;
    int podcast_count = 0;
    int artist_count = 0;
    int audiobook_count = 0;

    // Capacities (for tracking allocated sizes)
    int tracks_capacity = 0;
    int videos_capacity = 0;
    int pictures_capacity = 0;
    int playlists_capacity = 0;
    int podcasts_capacity = 0;
    int audiobooks_capacity = 0;

    ~ZMDBLibrary() {
        // Free all allocated arrays
        if (tracks) {
            for (int i = 0; i < track_count; i++) {
                tracks[i].~ZMDBTrack();
            }
            ::operator delete[](tracks);
        }
        if (videos) {
            for (int i = 0; i < video_count; i++) {
                videos[i].~ZMDBVideo();
            }
            ::operator delete[](videos);
        }
        if (pictures) {
            for (int i = 0; i < picture_count; i++) {
                pictures[i].~ZMDBPicture();
            }
            ::operator delete[](pictures);
        }
        if (playlists) {
            for (int i = 0; i < playlist_count; i++) {
                playlists[i].~ZMDBPlaylist();
            }
            ::operator delete[](playlists);
        }
        if (podcasts) {
            for (int i = 0; i < podcast_count; i++) {
                podcasts[i].~ZMDBPodcast();
            }
            ::operator delete[](podcasts);
        }
        if (audiobooks) {
            for (int i = 0; i < audiobook_count; i++) {
                audiobooks[i].~ZMDBAudiobook();
            }
            ::operator delete[](audiobooks);
        }
    }

    // Disable copy (use move semantics only)
    ZMDBLibrary(const ZMDBLibrary&) = delete;
    ZMDBLibrary& operator=(const ZMDBLibrary&) = delete;

    // Move constructor
    ZMDBLibrary(ZMDBLibrary&& other) noexcept
        : device_type(other.device_type),
          tracks(other.tracks),
          videos(other.videos),
          pictures(other.pictures),
          playlists(other.playlists),
          podcasts(other.podcasts),
          audiobooks(other.audiobooks),
          album_metadata(std::move(other.album_metadata)),
          album_count(other.album_count),
          track_count(other.track_count),
          video_count(other.video_count),
          picture_count(other.picture_count),
          playlist_count(other.playlist_count),
          podcast_count(other.podcast_count),
          artist_count(other.artist_count),
          audiobook_count(other.audiobook_count),
          tracks_capacity(other.tracks_capacity),
          videos_capacity(other.videos_capacity),
          pictures_capacity(other.pictures_capacity),
          playlists_capacity(other.playlists_capacity),
          podcasts_capacity(other.podcasts_capacity),
          audiobooks_capacity(other.audiobooks_capacity)
    {
        other.tracks = nullptr;
        other.videos = nullptr;
        other.pictures = nullptr;
        other.playlists = nullptr;
        other.podcasts = nullptr;
        other.audiobooks = nullptr;
        other.track_count = 0;
        other.video_count = 0;
        other.picture_count = 0;
        other.playlist_count = 0;
        other.podcast_count = 0;
        other.audiobook_count = 0;
        other.tracks_capacity = 0;
        other.videos_capacity = 0;
        other.pictures_capacity = 0;
        other.playlists_capacity = 0;
        other.podcasts_capacity = 0;
        other.audiobooks_capacity = 0;
    }

    // Move assignment
    ZMDBLibrary& operator=(ZMDBLibrary&& other) noexcept {
        if (this != &other) {
            // Free existing resources
            this->~ZMDBLibrary();

            // Move from other
            new (this) ZMDBLibrary(std::move(other));
        }
        return *this;
    }

    // Default constructor
    ZMDBLibrary() = default;
};

// Parsed field from backwards varint section
struct BackwardsVarintField {
    uint32_t field_id = 0;
    uint32_t field_size = 0;
    std::vector<uint8_t> field_data;
    size_t offset = 0;  // Offset within record
};

// ZMDB record header (4 bytes before record data)
struct RecordHeader {
    uint32_t record_size = 0;  // 24 bits
    uint8_t flags = 0;          // 8 bits
    bool valid = false;         // Bit 31 must be 0
};

// ZMDB descriptor (ZArr) - 20 bytes each, 96 total
struct Descriptor {
    uint16_t entry_size = 0;
    uint32_t entry_count = 0;
    uint32_t data_offset = 0;
};

} // namespace zmdb
