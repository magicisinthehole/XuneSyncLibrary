#include "ZMDBLibraryExtractor.h"
#include <cstring>
#include <iostream>
#include <algorithm>
#include <set>

namespace zmdb {

ZMDBLibraryExtractor::ZMDBLibraryExtractor() {
}

ZMDBLibrary ZMDBLibraryExtractor::ExtractLibrary(const mtp::ByteArray& zmdb_data, zune::DeviceFamily family) {
    ZMDBLibrary library;
    library.device_family = family;

    if (zmdb_data.empty()) {
        Log("Error: Empty zmdb data");
        return library;
    }

    Log("Starting zmdb extraction, size: " + std::to_string(zmdb_data.size()) + " bytes");
    Log("Device family: " + std::string(zune::GetFamilyName(family)) +
        " (" + std::string(family == zune::DeviceFamily::Pavo ? "ZuneHD" : "Classic") + ")");

    // 1. Build property map
    auto props = BuildPropertyMap(zmdb_data, 0x2F0);
    Log("Built property map: " + std::to_string(props.size()) + " properties");

    // 3. Scan all tracks first (collect track refs per album)
    std::map<uint32_t, std::set<uint32_t>> album_tracks_refs;  // album_pid -> set of track 0x0800 refs
    auto album_tracks = ScanTracks(zmdb_data, library.device_family, album_tracks_refs);
    Log("Scanned tracks: " + std::to_string(album_tracks.size()) + " albums with tracks");

    // 4. Extract albums - iterate over album_tracks (like Python parser)
    bool is_zunehd = (library.device_family == zune::DeviceFamily::Pavo);
    int albums_processed = 0;
    int albums_skipped_garbage = 0;
    int albums_added = 0;

    for (const auto& [album_pid, tracks] : album_tracks) {
        albums_processed++;
        uint16_t album_idx = GetPropertyIndex(album_pid);

        // ZuneHD: Skip albums lacking both direct metadata AND organizational properties (garbage data)
        if (is_zunehd) {
            uint32_t pid_0x0800 = (0x0800 << 16) | album_idx;
            uint32_t pid_0x0700 = (0x0700 << 16) | album_idx;
            uint32_t pid_0x0100_plus1 = (0x0100 << 16) | (album_idx + 1);
            uint32_t pid_0x0500 = (0x0500 << 16) | album_idx;

            bool has_direct = props.count(pid_0x0800) || props.count(pid_0x0700);
            bool has_0x0100_plus1 = props.count(pid_0x0100_plus1);
            bool has_0x0500 = props.count(pid_0x0500);

            // Albums without direct metadata need at least one organizational property to be valid
            if (!has_direct && !has_0x0100_plus1 && !has_0x0500) {
                Log("Album 0x" + std::to_string(album_pid) + " (idx=" + std::to_string(album_idx) +
                    "): SKIPPED - garbage data (no direct metadata or organizational properties)");
                albums_skipped_garbage++;
                continue;
            }
        }

        try {
            // Get track refs for this album
            std::set<uint32_t> track_refs;
            if (album_tracks_refs.count(album_pid)) {
                track_refs = album_tracks_refs.at(album_pid);
            }

            // Extract album metadata
            ZMDBAlbum album = ExtractAlbum(zmdb_data, album_pid, props, library.device_family, track_refs);

            // Attach tracks
            album.tracks = tracks;
            for (auto& track : album.tracks) {
                track.artist_name = album.artist_name;
                track.album_name = album.title;
            }

            // Add to library if valid
            if (!album.title.empty() && !album.artist_name.empty() && !album.tracks.empty()) {
                library.albums.push_back(album);
                library.albums_by_artist[album.artist_name].push_back(album);
                albums_added++;
            } else {
                Log("Album 0x" + std::to_string(album_pid) + " (idx=" + std::to_string(album_idx) +
                    "): SKIPPED - missing title or artist");
            }
        } catch (const std::exception& e) {
            Log("Error extracting album " + std::to_string(album_pid) + ": " + e.what());
        }
    }

    Log("Album extraction summary:");
    Log("  Albums processed: " + std::to_string(albums_processed));
    Log("  Skipped (garbage): " + std::to_string(albums_skipped_garbage));
    Log("  Added to library: " + std::to_string(albums_added));

    // 5. Sort albums by artist
    for (auto& [artist, albums] : library.albums_by_artist) {
        std::sort(albums.begin(), albums.end(),
            [](const ZMDBAlbum& a, const ZMDBAlbum& b) {
                return a.title < b.title;
            });
    }

    // 6. Compute statistics
    library.album_count = library.albums.size();
    library.artist_count = library.albums_by_artist.size();
    for (const auto& album : library.albums) {
        library.track_count += album.tracks.size();
    }

    Log("Extraction complete: " + std::to_string(library.album_count) + " albums, " +
        std::to_string(library.artist_count) + " artists, " +
        std::to_string(library.track_count) + " tracks");

    return library;
}

std::map<uint32_t, uint32_t> ZMDBLibraryExtractor::BuildPropertyMap(const mtp::ByteArray& blob, size_t start) {
    std::map<uint32_t, uint32_t> props;
    size_t offset = start;

    while (offset + 8 <= blob.size()) {
        uint32_t ptr, pid;
        std::memcpy(&ptr, &blob[offset], 4);
        std::memcpy(&pid, &blob[offset + 4], 4);

        if (ptr == 0 && pid == 0) {
            break;  // Terminator
        }

        // Only store first occurrence of each PID (like Python: if pid not in property_map)
        if (props.find(pid) == props.end()) {
            props[pid] = ptr;
        }
        offset += 8;
    }

    return props;
}

std::string ZMDBLibraryExtractor::ReadNullTerminatedAscii(const mtp::ByteArray& blob, size_t pos) const {
    if (pos >= blob.size()) return "";

    size_t end = pos;
    while (end < blob.size() && blob[end] != 0) {
        end++;
    }

    return std::string(reinterpret_cast<const char*>(&blob[pos]), end - pos);
}

std::string ZMDBLibraryExtractor::ReadUtf16LeUntilDelimiter(const mtp::ByteArray& blob, size_t start, size_t end) const {
    std::string result;
    size_t pos = start;

    // Check for 18-byte GUID prefix (marker 0x1410 at offset 16-17)
    if (start + 17 < blob.size()) {
        uint16_t guid_marker;
        std::memcpy(&guid_marker, &blob[start + 16], 2);
        if (guid_marker == 0x1410) {
            pos = start + 18;
        }
    }

    while (pos + 1 < end && pos + 1 < blob.size()) {
        uint16_t char_code;
        std::memcpy(&char_code, &blob[pos], 2);

        if (char_code == 0) break;

        // Convert UTF-16LE to UTF-8
        if (char_code < 0x80) {
            // ASCII range - single byte
            result += static_cast<char>(char_code);
        } else if (char_code < 0x800) {
            // 2-byte UTF-8
            result += static_cast<char>(0xC0 | (char_code >> 6));
            result += static_cast<char>(0x80 | (char_code & 0x3F));
        } else {
            // 3-byte UTF-8 (covers BMP - Basic Multilingual Plane)
            result += static_cast<char>(0xE0 | (char_code >> 12));
            result += static_cast<char>(0x80 | ((char_code >> 6) & 0x3F));
            result += static_cast<char>(0x80 | (char_code & 0x3F));
        }

        pos += 2;
    }

    return result;
}

size_t ZMDBLibraryExtractor::FindUtf16LePattern(const mtp::ByteArray& blob, size_t start, const std::string& pattern, size_t max_search) const {
    // Convert pattern to UTF-16LE bytes
    std::vector<uint8_t> pattern_bytes;
    for (char c : pattern) {
        pattern_bytes.push_back(static_cast<uint8_t>(c));
        pattern_bytes.push_back(0x00);
    }

    // Search for pattern
    for (size_t offset = 0; offset < max_search; offset++) {
        size_t pos = start + offset;
        if (pos + pattern_bytes.size() > blob.size()) break;

        bool match = true;
        for (size_t i = 0; i < pattern_bytes.size(); i++) {
            if (blob[pos + i] != pattern_bytes[i]) {
                match = false;
                break;
            }
        }

        if (match) return pos;
    }

    return 0;  // Not found
}

bool ZMDBLibraryExtractor::IsFMarker(const mtp::ByteArray& blob, size_t offset) const {
    if (offset + 4 > blob.size()) return false;

    uint32_t marker;
    std::memcpy(&marker, &blob[offset], 4);

    // F marker: byte3 = 0x46, byte2 = 0x00
    uint8_t byte3 = (marker >> 24) & 0xFF;
    uint8_t byte2 = (marker >> 16) & 0xFF;

    return byte3 == 0x46 && byte2 == 0x00;
}

size_t ZMDBLibraryExtractor::FindFMarker(const mtp::ByteArray& blob, size_t start, size_t max_search) const {
    for (size_t offset = 0; offset < max_search; offset += 4) {
        if (IsFMarker(blob, start + offset)) {
            return start + offset;
        }
    }
    return 0;
}

ZMDBLibraryExtractor::FMarkerData ZMDBLibraryExtractor::ExtractFromFMarker(
    const mtp::ByteArray& blob, size_t property_ptr, size_t f_offset, zune::DeviceFamily family) const {

    FMarkerData data;

    // F marker header size differs by device
    const int HEADER_SIZE = (family == zune::DeviceFamily::Pavo) ? 24 : 16;
    size_t ascii_start = property_ptr + f_offset + HEADER_SIZE;

    if (ascii_start >= blob.size()) {
        return data;
    }

    // Read ASCII album name
    data.album_name = ReadNullTerminatedAscii(blob, ascii_start);
    if (data.album_name.empty()) {
        return data;
    }

    // Find null terminator
    size_t null_pos = ascii_start + data.album_name.length();
    size_t utf16_start = null_pos + 1;

    // Check for 18-byte GUID variant
    if (utf16_start + 18 + 2 <= blob.size()) {
        uint16_t test_char, immediate_char;
        std::memcpy(&test_char, &blob[utf16_start + 18], 2);
        std::memcpy(&immediate_char, &blob[utf16_start], 2);

        // If +18 has valid ASCII-range UTF-16 but immediate position doesn't, skip GUID
        if (test_char < 0x0100 && test_char >= 0x20 && test_char < 0x7F &&
            !(immediate_char < 0x0100 && immediate_char >= 0x20 && immediate_char < 0x7F)) {
            utf16_start += 18;
        }
    }

    // Find "--" delimiter
    size_t delimiter_pos = FindUtf16LePattern(blob, utf16_start, "--", 200);

    if (delimiter_pos) {
        // Read artist name (between utf16_start and delimiter)
        data.artist_name = ReadUtf16LeUntilDelimiter(blob, utf16_start, delimiter_pos);

        // Look for .alb reference - extract the FULL UTF-16 string from utf16_start
        // This will be in format: "artist--album.alb"
        size_t alb_pos = FindUtf16LePattern(blob, delimiter_pos, ".alb", 200);
        if (alb_pos) {
            // Read the entire UTF-16 string from the beginning (includes "artist--album.alb")
            data.alb_reference = ReadUtf16LeUntilDelimiter(blob, utf16_start, alb_pos + 8);
        }
    }

    data.valid = !data.album_name.empty() && !data.artist_name.empty();
    return data;
}

ZMDBLibraryExtractor::MetadataResult ZMDBLibraryExtractor::ExtractMetadataDirect(
    const mtp::ByteArray& blob, uint32_t ptr, zune::DeviceFamily family) const {

    MetadataResult result;

    if (ptr + 200 > blob.size()) {
        return result;
    }

    // Use configured offset based on device family
    const int metadata_offset = (family == zune::DeviceFamily::Pavo) ? 32 : 24;

    // Read album name from ASCII section
    std::string album_name = ReadNullTerminatedAscii(blob, ptr + metadata_offset);
    if (album_name.empty()) {
        return result;
    }

    // Find null terminator
    size_t null_pos = ptr + metadata_offset;
    while (null_pos < blob.size() && null_pos < ptr + 200 && blob[null_pos] != 0) {
        null_pos++;
    }

    // Read UTF-16LE string after null terminator
    std::string utf16_str = ReadUtf16LeUntilDelimiter(blob, null_pos + 1, null_pos + 201);

    // Check for "artist--album.alb" format
    size_t delimiter_pos = utf16_str.find("--");
    size_t alb_pos = utf16_str.find(".alb");

    if (delimiter_pos != std::string::npos && alb_pos != std::string::npos) {
        result.artist_name = utf16_str.substr(0, delimiter_pos);
        result.album_name = album_name;
        result.alb_reference = utf16_str;
        result.valid = true;
    }

    return result;
}

ZMDBLibraryExtractor::MetadataResult ZMDBLibraryExtractor::FindFMarkerWithMatching(
    const mtp::ByteArray& blob,
    uint32_t ptr,
    const std::set<uint32_t>& track_0x0800_refs,
    zune::DeviceFamily family) const {

    MetadataResult result;

    // Search for F-markers (Python: searches to end of file, not just 200 bytes!)
    for (size_t f_offset = 0; ptr + f_offset + 4 < blob.size(); f_offset += 4) {
        if (IsFMarker(blob, ptr + f_offset)) {
            // Read F-marker's 0x0800 ref at bytes +4-7
            uint32_t f_marker_0x0800_ref = 0;
            if (ptr + f_offset + 8 <= blob.size()) {
                std::memcpy(&f_marker_0x0800_ref, &blob[ptr + f_offset + 4], 4);
            }

            // No refs = take first F-marker
            // Otherwise match refs
            if (track_0x0800_refs.empty() || track_0x0800_refs.count(f_marker_0x0800_ref) > 0) {
                FMarkerData f_data = ExtractFromFMarker(blob, ptr, f_offset, family);
                if (f_data.valid) {
                    result.album_name = f_data.album_name;
                    result.artist_name = f_data.artist_name;
                    result.alb_reference = f_data.alb_reference;
                    result.valid = true;
                    Log("FindFMarkerWithMatching: Found at offset " + std::to_string(f_offset) + " -> '" + result.album_name + "' / '" + result.artist_name + "'");
                    return result;
                }
            }
        }
    }

    return result;
}

std::map<uint32_t, std::vector<ZMDBTrack>> ZMDBLibraryExtractor::ScanTracks(
    const mtp::ByteArray& blob,
    zune::DeviceFamily family,
    std::map<uint32_t, std::set<uint32_t>>& album_tracks_refs) const {

    std::map<uint32_t, std::vector<ZMDBTrack>> album_tracks;

    // Device-specific offsets (Pavo = Zune HD, others = Classic)
    bool is_zunehd = (family == zune::DeviceFamily::Pavo);
    const int ALBUM_PID_OFFSET = is_zunehd ? -28 : -24;
    const int REF_0X0800_OFFSET = -20;  // Always -20 for both devices
    const size_t TRACK_REGION_START = is_zunehd ? 0 : 0x000312B0;  // ZuneHD: 0, Zune30: skip stale data
    const size_t TRACK_REGION_END = (blob.size() > 4) ? (blob.size() - 4) : 0;  // Only need 4 bytes for marker check

    int total_markers_found = 0;
    int valid_tracks = 0;

    for (size_t offset = TRACK_REGION_START; offset < TRACK_REGION_END; offset++) {
        uint16_t marker;
        std::memcpy(&marker, &blob[offset], 2);

        // Track markers: 0x3009 or 0xB901
        if (marker == 0x3009 || marker == 0xB901) {
            total_markers_found++;
            ZMDBTrack track;

            // Read track title (ASCII string at offset+4)
            if (offset + 4 < blob.size()) {
                track.title = ReadNullTerminatedAscii(blob, offset + 4);
            }

            // Skip if title is empty or too short
            if (track.title.length() < 1) {
                Log("ScanTracks: Marker at 0x" + std::to_string(offset) + " has empty title, skipping");
                continue;
            }

            valid_tracks++;

            // Read track number (byte at offset-4)
            if (offset >= 4) {
                track.track_number = blob[offset - 4];
            }

            // Read album PID (4 bytes at device-specific offset)
            if (offset >= static_cast<size_t>(-ALBUM_PID_OFFSET)) {
                uint32_t album_pid;
                std::memcpy(&album_pid, &blob[offset + ALBUM_PID_OFFSET], 4);

                uint16_t category = GetPropertyCategory(album_pid);
                // Validate it's an album PID (0x0600 category)
                if (category == 0x0600) {
                    // For ZuneHD: Convert album_pid (0x0600xxxx) to metadata_pid (0x0800xxxx)
                    // For Classic: Use album_pid as-is (0x0600xxxx)
                    uint32_t metadata_pid = album_pid;
                    if (is_zunehd) {
                        uint16_t album_idx = GetPropertyIndex(album_pid);
                        metadata_pid = (0x0800 << 16) | album_idx;
                    }

                    // Read ref_0x0800 from track offset -20 (for matching F-markers)
                    uint32_t ref_0x0800 = 0;
                    if (offset >= static_cast<size_t>(-REF_0X0800_OFFSET)) {
                        std::memcpy(&ref_0x0800, &blob[offset + REF_0X0800_OFFSET], 4);
                        // Store the ref for this album (keyed by metadata_pid!)
                        if (ref_0x0800 != 0) {
                            album_tracks_refs[metadata_pid].insert(ref_0x0800);
                        }
                    }

                    album_tracks[metadata_pid].push_back(track);
                    uint16_t album_cat = GetPropertyCategory(album_pid);
                    uint16_t album_idx = GetPropertyIndex(album_pid);
                    uint16_t meta_cat = GetPropertyCategory(metadata_pid);
                    uint16_t meta_idx = GetPropertyIndex(metadata_pid);
                    Log("ScanTracks: Track '" + track.title + "' -> album_pid=0x" + std::to_string(album_pid) +
                        " (cat=0x" + std::to_string(album_cat) + " idx=" + std::to_string(album_idx) +
                        "), metadata_pid=0x0800[" + std::to_string(meta_idx) + "]" +
                        " (ref=0x" + std::to_string(ref_0x0800) + ")");
                } else {
                    Log("ScanTracks: Track '" + track.title + "' has invalid album PID 0x" + std::to_string(album_pid) +
                        " (category 0x" + std::to_string(category) + ", not 0x0600)");
                }
            } else {
                Log("ScanTracks: Track '" + track.title + "' at 0x" + std::to_string(offset) +
                    " cannot read album PID (offset too small)");
            }
        }
    }

    Log("ScanTracks summary:");
    Log("  Total markers found: " + std::to_string(total_markers_found));
    Log("  Valid tracks (with titles): " + std::to_string(valid_tracks));
    Log("  Albums with tracks: " + std::to_string(album_tracks.size()));

    return album_tracks;
}

ZMDBAlbum ZMDBLibraryExtractor::ExtractAlbum(
    const mtp::ByteArray& blob,
    uint32_t album_pid,
    const std::map<uint32_t, uint32_t>& props,
    zune::DeviceFamily family,
    const std::set<uint32_t>& track_0x0800_refs) const {

    ZMDBAlbum album;
    album.album_pid = album_pid;

    uint16_t album_idx = GetPropertyIndex(album_pid);
    bool is_zunehd = (family == zune::DeviceFamily::Pavo);

    MetadataResult metadata;
    std::string source;

    // Python parser's 6-step search order:
    // 1. 0x0800[idx] - Direct metadata
    // 2. 0x0700[idx] - F-marker with matching (ZuneHD: direct, Zune30: F-marker)
    // 3. 0x0100[idx+1] OR 0x0600[idx] - Deterministic choice based on 0x0500[idx] (ZuneHD)
    //    OR 0x0100[idx+1] - First F-marker, no matching (Zune30)
    // 4. 0x0600[idx] - F-marker with matching (Zune30 only)
    // 5. 0x0600[idx+1] - F-marker with matching
    // 6. 0x0800[idx+1] - F-marker with matching

    // Try 1: 0x0800[idx] direct
    uint32_t pid = (0x0800 << 16) | album_idx;
    if (props.count(pid)) {
        metadata = ExtractMetadataDirect(blob, props.at(pid), family);
        if (metadata.valid) {
            source = "0x0800[idx]";
        }
    }

    // Try 2: 0x0700[idx]
    // For ZuneHD: direct metadata, For Zune30: F-marker with matching
    if (!metadata.valid) {
        pid = (0x0700 << 16) | album_idx;
        if (props.count(pid)) {
            if (is_zunehd) {
                metadata = ExtractMetadataDirect(blob, props.at(pid), family);
                if (metadata.valid) {
                    source = "0x0700[idx]";
                }
            } else {
                metadata = FindFMarkerWithMatching(blob, props.at(pid), track_0x0800_refs, family);
                if (metadata.valid) {
                    source = "0x0700[idx]";
                }
            }
        }
    }

    // Try 3: Deterministic choice for ZuneHD, 0x0100[idx+1] for Zune30
    if (!metadata.valid) {
        Log("Try 3: album_idx=" + std::to_string(album_idx) + " is_zunehd=" + std::string(is_zunehd ? "true" : "false"));
        if (is_zunehd) {
            // Check if 0x0500[idx] exists - indicates organizational structure
            uint32_t pid_0x0500 = (0x0500 << 16) | album_idx;
            bool has_0x0500 = props.count(pid_0x0500);

            if (has_0x0500) {
                // Albums with 0x0500[idx] use 0x0100[idx+1]
                pid = (0x0100 << 16) | (album_idx + 1);
                if (props.count(pid)) {
                    std::set<uint32_t> empty_refs;  // No matching
                    metadata = FindFMarkerWithMatching(blob, props.at(pid), empty_refs, family);
                    if (metadata.valid) {
                        source = "0x0100[idx+1]";
                    }
                }
            } else {
                // Albums without 0x0500[idx] use 0x0600[idx]
                pid = (0x0600 << 16) | album_idx;
                if (props.count(pid)) {
                    std::set<uint32_t> empty_refs;  // No matching
                    metadata = FindFMarkerWithMatching(blob, props.at(pid), empty_refs, family);
                    if (metadata.valid) {
                        source = "0x0600[idx]";
                    }
                }
            }

            // Fallback: try the other property if first choice didn't work
            if (!metadata.valid) {
                if (has_0x0500) {
                    pid = (0x0600 << 16) | album_idx;
                    if (props.count(pid)) {
                        std::set<uint32_t> empty_refs;
                        metadata = FindFMarkerWithMatching(blob, props.at(pid), empty_refs, family);
                        if (metadata.valid) {
                            source = "0x0600[idx]";
                        }
                    }
                } else {
                    pid = (0x0100 << 16) | (album_idx + 1);
                    if (props.count(pid)) {
                        std::set<uint32_t> empty_refs;
                        metadata = FindFMarkerWithMatching(blob, props.at(pid), empty_refs, family);
                        if (metadata.valid) {
                            source = "0x0100[idx+1]";
                        }
                    }
                }
            }
        } else {
            // Zune30: Try 3a: Search 0x0100[idx+1..idx+99] with matching if we have track refs
            if (!track_0x0800_refs.empty()) {
                for (int offset_val = 1; offset_val < 100 && !metadata.valid; offset_val++) {
                    pid = (0x0100 << 16) | (album_idx + offset_val);
                    if (props.count(pid)) {
                        metadata = FindFMarkerWithMatching(blob, props.at(pid), track_0x0800_refs, family);
                        if (metadata.valid) {
                            source = "0x0100[idx+" + std::to_string(offset_val) + "]";
                            break;
                        }
                    }
                }
            }

            // Try 3b: Fallback to 0x0100[idx+1] without matching (safety net)
            if (!metadata.valid) {
                pid = (0x0100 << 16) | (album_idx + 1);
                if (props.count(pid)) {
                    std::set<uint32_t> empty_refs;  // No matching
                    metadata = FindFMarkerWithMatching(blob, props.at(pid), empty_refs, family);
                    if (metadata.valid) {
                        source = "0x0100[idx+1]";
                    }
                }
            }
        }
    }

    // Try 4 (Zune30): 0x0600[idx] with matching
    if (!metadata.valid && !is_zunehd) {
        pid = (0x0600 << 16) | album_idx;
        if (props.count(pid)) {
            metadata = FindFMarkerWithMatching(blob, props.at(pid), track_0x0800_refs, family);
            if (metadata.valid) {
                source = "0x0600[idx]";
            }
        }
    }

    // Try 5: 0x0600[idx+1] with matching
    if (!metadata.valid) {
        pid = (0x0600 << 16) | (album_idx + 1);
        if (props.count(pid)) {
            metadata = FindFMarkerWithMatching(blob, props.at(pid), track_0x0800_refs, family);
            if (metadata.valid) {
                source = "0x0600[idx+1]";
            }
        }
    }

    // Try 6: 0x0800[idx+1] with matching
    if (!metadata.valid) {
        pid = (0x0800 << 16) | (album_idx + 1);
        if (props.count(pid)) {
            metadata = FindFMarkerWithMatching(blob, props.at(pid), track_0x0800_refs, family);
            if (metadata.valid) {
                source = "0x0800[idx+1]";
            }
        }
    }

    // Populate album data
    if (metadata.valid) {
        album.title = metadata.album_name;
        album.artist_name = metadata.artist_name;
        album.alb_reference = metadata.alb_reference;
        Log("ExtractAlbum 0x" + std::to_string(album_pid) + " (idx=" + std::to_string(album_idx) +
            "): album='" + album.title + "' artist='" + album.artist_name + "' source=" + source);
    } else {
        Log("ExtractAlbum 0x" + std::to_string(album_pid) + " (idx=" + std::to_string(album_idx) +
            "): NO METADATA FOUND");
    }

    return album;
}

void ZMDBLibraryExtractor::Log(const std::string& message) const {
    std::cout << "[ZMDBLibraryExtractor] " << message << std::endl;
}

} // namespace zmdb
