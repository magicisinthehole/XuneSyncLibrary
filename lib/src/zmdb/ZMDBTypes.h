#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <map>
#include "../ZuneDeviceIdentification.h"

namespace zmdb {

// Use zune::DeviceFamily for device identification
// DeviceFamily enum values:
//   Keel (0)     = Zune 30
//   Scorpius (2) = Zune 4/8/16 (flash)
//   Draco (3)    = Zune 80/120 (HDD)
//   Pavo (6)     = Zune HD

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
    int disc_number = 1;            // Disc number (varint field 0x6c, default=1 if absent)
    int duration_ms = 0;            // Duration in milliseconds (offset 16-19)
    int file_size_bytes = 0;        // File size in bytes (offset 20-23 on HD; not stored on Classic)
    uint16_t playcount = 0;         // Total play count (MTP UseCount — offset 26 HD, byte 22 Classic)
    uint16_t skip_count = 0;        // Skip count (varint field 0x63)
    uint32_t on_device_playcount = 0; // On-device plays only (varint field 0x62)
    uint16_t codec_id = 0;          // MTP ObjectFormat (offset 24 Classic, 28 HD) — 0x3009 MP3, 0xB901 WMA, 0xB215 M4a, etc.
    uint8_t rating = 0;             // Rating: 0=neutral, 8=liked, 3=disliked (offset 30)
    uint64_t last_played_timestamp = 0; // Windows FILETIME of last play/skip event (varint field 0x70)
    uint32_t atom_id = 0;
    uint32_t album_ref = 0;         // Album atom_id reference for grouping tracks
    uint32_t genre_ref = 0;         // Genre atom_id reference
    // Schema 0x05 reference at record offset 12 — resolves to the album's
    // .alb filename (e.g. "Unknown Album.alb"). ZMDB track records do NOT
    // store a per-file track filename; the authoritative track filename is
    // the MTP ObjectInfo.Filename property, outside ZMDB.
    std::string album_alb_ref;
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

// Genre structure
struct ZMDBGenre {
    std::string name;
    uint32_t atom_id = 0;
};

// Video structure (non-podcast videos; video podcasts promoted into ZMDBPodcast).
//
// Fixed-header layout (forensically verified — see
// research/format-fixtures/parser-forensics.md):
//   Classic (Zune 30, Zune 120 HDD)       HD (Zune HD)
//   +0x00 u32 folder_ref (Schema 0x05)    +0x00 same
//   +0x04 u32 title_ref  (Schema 0x0a)    +0x04 same
//   +0x08 u32 ref2 (always 0)             +0x08 same
//   +0x0c u32 duration_ms                 +0x0c same
//   +0x10 u32 unknown                     +0x10 u32 unknown
//   +0x14…0x17 zeros                      +0x14…0x17 zeros
//   +0x18 u64 release_date (FILETIME)     +0x18 same
//   +0x20 u16 codec_id                    +0x20 u32 file_size_bytes (HD-only)
//   +0x22 u16 playcount (MTP UseCount)    +0x24 u16 codec_id
//   +0x24 u16 reserved                    +0x26 u16 playcount (MTP UseCount)
//   +0x26 u16 category                    +0x28 u16 reserved
//   +0x28+   title UTF-8 NUL-terminated   +0x2a u16 category
//                                         +0x2c+   title UTF-8 NUL-terminated
//
//   Category is the on-device UI bucket, not the Zune-software UI category.
//   Firmware models only three content-type buckets plus a catch-all:
//     1 = Other (Specials, News, Personal — anything not in TV/Music/Movies)
//     2 = Movies
//     4 = TV (Series)
//     5 = Music
//   Code 3 not observed. Codes 0, 33-38 are MTP upload-side MetaGenre values
//   that the device remaps to the four ZMDB codes at storage time.
//
// After the title (NUL-terminated UTF-8) comes the backwards-varint section,
// then the trailing 6-byte records. Both walk backwards from the end:
//   0x44 UTF-16  filename
//   0x41 UTF-16  description
//   0x46 UTF-16  artist_name (Music category)
//   0x50 u32     episode_number (Series category)
//   0x4f u32     season_number (Series category)
//   0x1e u32     unknown (always value=1)
//   0x62 u32     on_device_playcount
//   0x70 u64     last_played_timestamp (Windows FILETIME)
struct ZMDBVideo {
    std::string title;          // Resolved title_ref → Schema 0x0a record. For Series-category
                                // videos this is the Series Title (MTP 0xDA9A); for other
                                // categories it equals episode_title.
    std::string episode_title;  // In-record UTF-8 title at offset 0x28 (Classic) / 0x2c (HD).
                                // Sourced from MTP 0xDC44 Name. For Series-category videos
                                // this is the Episode Title; for other categories it's the
                                // same as title.
    std::string folder;
    uint32_t ref2 = 0;                 // u32 at offset 8; always 0 in observed data
    uint32_t duration_ms = 0;          // ms (offset 12)
    uint32_t unknown_0x10 = 0;         // u32 at offset 16; varies, meaning not identified
    uint64_t release_date_filetime = 0; // Windows FILETIME at offset 24 (0x18); 0 = not set
    uint32_t file_size_bytes = 0;      // HD-only (offset 32); 0 on Classic
    uint16_t codec_id = 0;             // MTP ObjectFormat — 0xB981 WMV, 0xB216 MP4-video
    uint16_t playcount = 0;            // Total play count (MTP UseCount) — fixed-header u16
    uint16_t category = 0;             // On-device category: 1=Other, 2=Movies, 4=TV (Series), 5=Music
    std::string filename;              // UTF-16LE filename from backwards-varint field 0x44
    std::string description;           // UTF-16LE from field 0x41
    std::string artist_name;           // UTF-16LE from field 0x46 (Music category)
    uint32_t season_number = 0;        // u32 from field 0x4f (Series category)
    uint32_t episode_number = 0;       // u32 from field 0x50 (Series category)
    uint32_t on_device_playcount = 0;  // u32 from field 0x62
    uint64_t last_played_timestamp = 0; // FILETIME from field 0x70
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

// Audio (Schema 0x10) vs video-podcast (Schema 0x02 with non-zero show_ref).
enum class PodcastMediaType : uint8_t { Audio, Video };

// PodcastShow (Schema 0x0f).
//   +0x00 u32  filename_ref       Schema 0x05 atom_id (parent "Series" folder)
//   +0x05 u8   is_subscribed      1 = yes, 0 = no
//   +0x06 u8   is_podcast         always 1 in observed records
//   +0x08      UTF-8 name (NUL-terminated)
//   …         backwards-varint UTF-16LE fields keyed by PodcastFieldId
struct ZMDBPodcastShow {
    std::string name;
    std::string ser_filename;       // PodcastFieldId::Filename
    std::string author;             // PodcastFieldId::Author
    std::string feed_url;           // PodcastFieldId::Url
    uint32_t    filename_ref = 0;
    bool        is_subscribed = true;
    uint32_t    atom_id = 0;        // = MTP object handle of the .ser file
};

// PodcastEpisode covers audio (Schema 0x10) and video-podcast (Schema 0x02
// with non-zero podcast_show_ref). Per-device fixed-header offsets live in
// the parsers; common fields are described below.
struct ZMDBPodcast {
    PodcastMediaType media_type = PodcastMediaType::Audio;
    std::string title;
    std::string show_name;          // resolved from podcast_show_ref
    std::string author;
    std::string description;
    std::string episode_url;
    std::string folder_name;        // resolved from filename_ref
    std::string episode_filename;   // video records only

    uint32_t atom_id = 0;           // = MTP object handle
    uint32_t filename_ref = 0;      // Schema 0x05 (per-series subfolder)
    uint32_t podcast_show_ref = 0;  // Schema 0x0f (= MTP SeriesHandle)

    uint32_t duration_ms = 0;
    uint32_t bookmark_ms = 0;
    uint64_t publish_date = 0;      // Windows FILETIME
    uint32_t file_size_bytes = 0;   // HD only; absent from Classic fixed header
    uint16_t codec_id = 0;          // 0x3009 = MP3, 0xB981 = WMV
    uint32_t played_flag = 0;       // bit 0x200 = marked played; remaining bits
                                    // include codec/capability data we don't decode

    bool is_played() const { return (played_flag & 0x200u) != 0u; }
    // MTP 0xDC95 MetaGenre — used by the upload path; not stored in ZMDB.
    uint16_t meta_genre() const { return codec_id == 0xB981u ? 65u : 64u; }
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
    zune::DeviceFamily device_family = zune::DeviceFamily::Unknown;

    // Flat arrays of media types (allocated once, no reallocation)
    ZMDBTrack* tracks = nullptr;
    ZMDBVideo* videos = nullptr;
    ZMDBPicture* pictures = nullptr;
    ZMDBPlaylist* playlists = nullptr;
    ZMDBPodcast* podcasts = nullptr;
    ZMDBAudiobook* audiobooks = nullptr;

    // Album metadata (kept as map for O(1) lookups during parsing)
    std::map<uint32_t, ZMDBAlbum> album_metadata;

    // Artist metadata (kept as map for O(1) lookups during parsing)
    std::map<uint32_t, ZMDBArtist> artist_metadata;

    // Genre metadata (kept as map for O(1) lookups during parsing)
    std::map<uint32_t, ZMDBGenre> genre_metadata;

    // Podcast show metadata (atom_id = MTP handle of .ser object)
    std::map<uint32_t, ZMDBPodcastShow> podcast_show_metadata;

    // Counts
    int album_count = 0;
    int track_count = 0;
    int video_count = 0;
    int picture_count = 0;
    int playlist_count = 0;
    int podcast_count = 0;
    int podcast_show_count = 0;
    int artist_count = 0;
    int genre_count = 0;
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
        : device_family(other.device_family),
          tracks(other.tracks),
          videos(other.videos),
          pictures(other.pictures),
          playlists(other.playlists),
          podcasts(other.podcasts),
          audiobooks(other.audiobooks),
          album_metadata(std::move(other.album_metadata)),
          artist_metadata(std::move(other.artist_metadata)),
          genre_metadata(std::move(other.genre_metadata)),
          podcast_show_metadata(std::move(other.podcast_show_metadata)),
          album_count(other.album_count),
          track_count(other.track_count),
          video_count(other.video_count),
          picture_count(other.picture_count),
          playlist_count(other.playlist_count),
          podcast_count(other.podcast_count),
          podcast_show_count(other.podcast_show_count),
          artist_count(other.artist_count),
          genre_count(other.genre_count),
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
        other.podcast_show_count = 0;
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
