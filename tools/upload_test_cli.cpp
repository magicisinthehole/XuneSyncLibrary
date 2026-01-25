/**
 * Upload Test CLI - Replicates exact Xune upload flow
 *
 * This tool mimics the exact parameter passing from Xune's DeviceUploadService.cs
 * to help diagnose upload hang issues.
 *
 * Usage:
 *   upload_test_cli <audio_file> [options]
 *
 * Options:
 *   --duration MS     Set duration in milliseconds (default: from file metadata)
 *   --rating N        Rating: -1=skip, 0=unrated, 2=dislike, 8=like (default: -1)
 *   --no-duration     Don't send duration (simulates old behavior)
 *   --no-guid         Don't send artist GUID
 *   --verbose         Show detailed logging
 */

#include <iostream>
#include <fstream>
#include <string>
#include <vector>
#include <filesystem>
#include <cstring>
#include <iomanip>
#include <chrono>

#include <taglib/fileref.h>
#include <taglib/tag.h>
#include <taglib/tpropertymap.h>
#include <taglib/mpegfile.h>
#include <taglib/id3v2tag.h>
#include <taglib/attachedpictureframe.h>
#include <taglib/asffile.h>
#include <taglib/asftag.h>
#include <taglib/mp4file.h>
#include <taglib/mp4tag.h>
#include <taglib/mp4coverart.h>
#include <taglib/flacfile.h>
#include <taglib/flacpicture.h>

#include "lib/src/ZuneDevice.h"

namespace fs = std::filesystem;

static bool g_verbose = false;

void log_callback(const std::string& message) {
    auto now = std::chrono::system_clock::now();
    auto time = std::chrono::system_clock::to_time_t(now);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()) % 1000;

    std::cout << "[" << std::put_time(std::localtime(&time), "%H:%M:%S")
              << "." << std::setfill('0') << std::setw(3) << ms.count() << "] "
              << message << std::endl;
}

void print_usage(const char* prog) {
    std::cout << "Upload Test CLI - Replicates exact Xune upload flow" << std::endl;
    std::cout << std::endl;
    std::cout << "Usage: " << prog << " <audio_file> [options]" << std::endl;
    std::cout << std::endl;
    std::cout << "Options:" << std::endl;
    std::cout << "  --duration MS     Set duration in milliseconds (default: from file)" << std::endl;
    std::cout << "  --rating N        Rating: -1=skip, 2=dislike, 8=like (default: -1)" << std::endl;
    std::cout << "  --no-duration     Don't send duration (duration_ms=0, old behavior)" << std::endl;
    std::cout << "  --no-guid         Don't send artist GUID" << std::endl;
    std::cout << "  --no-artwork      Don't send artwork (avoids large image failures)" << std::endl;
    std::cout << "  --artist NAME     Override artist name" << std::endl;
    std::cout << "  --album NAME      Override album name" << std::endl;
    std::cout << "  --title NAME      Override track title" << std::endl;
    std::cout << "  --verbose         Show detailed logging" << std::endl;
    std::cout << "  --help            Show this help" << std::endl;
    std::cout << std::endl;
    std::cout << "Test scenarios:" << std::endl;
    std::cout << "  # Old behavior (no duration, no rating) - should work if that's the issue" << std::endl;
    std::cout << "  " << prog << " track.wma --no-duration --rating -1" << std::endl;
    std::cout << std::endl;
    std::cout << "  # With duration only" << std::endl;
    std::cout << "  " << prog << " track.wma --rating -1" << std::endl;
    std::cout << std::endl;
    std::cout << "  # With rating only (no duration)" << std::endl;
    std::cout << "  " << prog << " track.wma --no-duration --rating 8" << std::endl;
    std::cout << std::endl;
    std::cout << "  # Full Xune behavior (duration + rating)" << std::endl;
    std::cout << "  " << prog << " track.wma --rating 8" << std::endl;
}

std::vector<uint8_t> extract_artwork(const std::string& file_path) {
    std::vector<uint8_t> artwork;

    TagLib::FileRef fileRef(file_path.c_str());
    if (fileRef.isNull()) {
        return artwork;
    }

    // Try MP3/ID3v2
    if (auto* mpegFile = dynamic_cast<TagLib::MPEG::File*>(fileRef.file())) {
        if (mpegFile->ID3v2Tag()) {
            auto frames = mpegFile->ID3v2Tag()->frameList("APIC");
            if (!frames.isEmpty()) {
                if (auto* pic = dynamic_cast<TagLib::ID3v2::AttachedPictureFrame*>(frames.front())) {
                    TagLib::ByteVector data = pic->picture();
                    artwork.assign(data.begin(), data.end());
                    return artwork;
                }
            }
        }
    }

    // Try WMA/ASF
    if (auto* asfFile = dynamic_cast<TagLib::ASF::File*>(fileRef.file())) {
        if (asfFile->tag()) {
            auto& attrMap = asfFile->tag()->attributeListMap();
            if (attrMap.contains("WM/Picture")) {
                auto& pictures = attrMap["WM/Picture"];
                if (!pictures.isEmpty()) {
                    TagLib::ByteVector data = pictures[0].toPicture().picture();
                    artwork.assign(data.begin(), data.end());
                    return artwork;
                }
            }
        }
    }

    // Try MP4/M4A
    if (auto* mp4File = dynamic_cast<TagLib::MP4::File*>(fileRef.file())) {
        if (mp4File->tag() && mp4File->tag()->contains("covr")) {
            auto covers = mp4File->tag()->item("covr").toCoverArtList();
            if (!covers.isEmpty()) {
                TagLib::ByteVector data = covers[0].data();
                artwork.assign(data.begin(), data.end());
                return artwork;
            }
        }
    }

    // Try FLAC
    if (auto* flacFile = dynamic_cast<TagLib::FLAC::File*>(fileRef.file())) {
        auto pictures = flacFile->pictureList();
        if (!pictures.isEmpty()) {
            TagLib::ByteVector data = pictures[0]->data();
            artwork.assign(data.begin(), data.end());
            return artwork;
        }
    }

    return artwork;
}

std::string extract_artist_guid(const std::string& file_path) {
    TagLib::FileRef fileRef(file_path.c_str());
    if (fileRef.isNull()) {
        return "";
    }

    // Try WMA/ASF - ZuneAlbumArtistMediaID
    if (auto* asfFile = dynamic_cast<TagLib::ASF::File*>(fileRef.file())) {
        if (asfFile->tag()) {
            auto& attrMap = asfFile->tag()->attributeListMap();
            if (attrMap.contains("ZuneAlbumArtistMediaID")) {
                return attrMap["ZuneAlbumArtistMediaID"][0].toString().toCString(true);
            }
        }
    }

    // Try MP4 - could have custom atom
    // For now, return empty - most files won't have this

    return "";
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        print_usage(argv[0]);
        return 1;
    }

    // Parse arguments
    std::string file_path;
    std::string artist_override;
    std::string album_override;
    std::string title_override;
    int rating = -1;  // Default: skip rating property (Xune behavior for unrated)
    int duration_override = -1;  // -1 means use file metadata
    bool no_duration = false;
    bool no_guid = false;
    bool no_artwork = false;

    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        if (arg == "--help") {
            print_usage(argv[0]);
            return 0;
        } else if (arg == "--verbose") {
            g_verbose = true;
        } else if (arg == "--no-duration") {
            no_duration = true;
        } else if (arg == "--no-guid") {
            no_guid = true;
        } else if (arg == "--no-artwork") {
            no_artwork = true;
        } else if (arg == "--duration" && i + 1 < argc) {
            duration_override = std::stoi(argv[++i]);
        } else if (arg == "--rating" && i + 1 < argc) {
            rating = std::stoi(argv[++i]);
        } else if (arg == "--artist" && i + 1 < argc) {
            artist_override = argv[++i];
        } else if (arg == "--album" && i + 1 < argc) {
            album_override = argv[++i];
        } else if (arg == "--title" && i + 1 < argc) {
            title_override = argv[++i];
        } else if (arg[0] != '-') {
            file_path = arg;
        }
    }

    if (file_path.empty()) {
        std::cerr << "ERROR: No audio file specified" << std::endl;
        print_usage(argv[0]);
        return 1;
    }

    if (!fs::exists(file_path)) {
        std::cerr << "ERROR: File not found: " << file_path << std::endl;
        return 1;
    }

    std::cout << "=== Upload Test CLI ===" << std::endl;
    std::cout << "Replicating exact Xune upload flow" << std::endl;
    std::cout << std::endl;

    // Extract metadata from file
    std::cout << "=== Reading File Metadata ===" << std::endl;
    std::cout << "File: " << file_path << std::endl;

    TagLib::FileRef fileRef(file_path.c_str());
    if (fileRef.isNull()) {
        std::cerr << "ERROR: Could not read file metadata" << std::endl;
        return 1;
    }

    std::string artist, album, title, genre;
    int year = 0, track_num = 0;
    uint32_t duration_ms = 0;

    if (fileRef.tag()) {
        TagLib::Tag* tag = fileRef.tag();
        artist = tag->artist().toCString(true);
        album = tag->album().toCString(true);
        title = tag->title().toCString(true);
        genre = tag->genre().toCString(true);
        year = tag->year();
        track_num = tag->track();
    }

    if (fileRef.audioProperties()) {
        duration_ms = static_cast<uint32_t>(fileRef.audioProperties()->lengthInMilliseconds());
    }

    // Apply overrides
    if (!artist_override.empty()) artist = artist_override;
    if (!album_override.empty()) album = album_override;
    if (!title_override.empty()) title = title_override;

    // Handle duration
    uint32_t final_duration_ms = 0;
    if (no_duration) {
        final_duration_ms = 0;  // Old behavior: don't include duration property
    } else if (duration_override >= 0) {
        final_duration_ms = static_cast<uint32_t>(duration_override);
    } else {
        final_duration_ms = duration_ms;  // Use file metadata
    }

    // Fallbacks
    if (title.empty()) {
        title = fs::path(file_path).stem().string();
    }
    if (artist.empty()) artist = "Unknown Artist";
    if (album.empty()) album = "Unknown Album";

    std::cout << "  Artist:   " << artist << std::endl;
    std::cout << "  Album:    " << album << std::endl;
    std::cout << "  Title:    " << title << std::endl;
    std::cout << "  Genre:    " << (genre.empty() ? "(none)" : genre) << std::endl;
    std::cout << "  Year:     " << (year > 0 ? std::to_string(year) : "(none)") << std::endl;
    std::cout << "  Track #:  " << track_num << std::endl;
    std::cout << "  Duration: " << duration_ms << " ms (from file)" << std::endl;
    std::cout << std::endl;

    // Extract artwork (unless --no-artwork specified)
    std::vector<uint8_t> artwork;
    if (!no_artwork) {
        artwork = extract_artwork(file_path);
        if (!artwork.empty() && artwork.size() > 500000) {
            std::cout << "  Artwork:  " << artwork.size() << " bytes (WARNING: >500KB, may fail - use --no-artwork)" << std::endl;
        } else {
            std::cout << "  Artwork:  " << (artwork.empty() ? "(none)" : std::to_string(artwork.size()) + " bytes") << std::endl;
        }
    } else {
        std::cout << "  Artwork:  (skipped with --no-artwork)" << std::endl;
    }

    // Extract artist GUID
    std::string artist_guid;
    if (!no_guid) {
        artist_guid = extract_artist_guid(file_path);
    }
    std::cout << "  GUID:     " << (artist_guid.empty() ? "(none)" : artist_guid) << std::endl;
    std::cout << std::endl;

    // Show what will be sent
    std::cout << "=== Upload Parameters (Xune-style) ===" << std::endl;
    std::cout << "  duration_ms: " << final_duration_ms;
    if (final_duration_ms == 0) {
        std::cout << " (Duration property will NOT be included)";
    }
    std::cout << std::endl;

    std::cout << "  rating:      " << rating;
    if (rating < 0) {
        std::cout << " (Rating property will NOT be included)";
    } else if (rating == 0) {
        std::cout << " (unrated - WARNING: might be invalid)";
    } else if (rating == 2) {
        std::cout << " (disliked)";
    } else if (rating == 8) {
        std::cout << " (liked)";
    }
    std::cout << std::endl;
    std::cout << std::endl;

    // Property count prediction
    int prop_count = 3;  // Artist, Name, ObjectFilename
    if (!genre.empty()) prop_count++;
    if (track_num > 0) prop_count++;
    if (final_duration_ms > 0) prop_count++;
    if (rating >= 0) prop_count++;

    std::cout << "  Expected property count: " << prop_count << std::endl;
    std::cout << "    - Artist (always)" << std::endl;
    std::cout << "    - Name (always)" << std::endl;
    std::cout << "    - ObjectFilename (always)" << std::endl;
    if (track_num > 0) std::cout << "    - Track" << std::endl;
    if (!genre.empty()) std::cout << "    - Genre" << std::endl;
    if (final_duration_ms > 0) std::cout << "    - Duration" << std::endl;
    if (rating >= 0) std::cout << "    - UserRating" << std::endl;
    std::cout << std::endl;

    // Connect to device
    std::cout << "=== Connecting to Device ===" << std::endl;

    ZuneDevice device;
    device.SetLogCallback(log_callback);

    if (!device.ConnectUSB()) {
        std::cerr << "ERROR: Failed to connect to Zune device" << std::endl;
        return 1;
    }

    std::cout << "Connected to: " << device.GetName() << std::endl;
    std::cout << "Model: " << device.GetModel() << std::endl;
    std::cout << std::endl;

    // Perform upload
    std::cout << "=== Starting Upload ===" << std::endl;
    std::cout << "Calling UploadTrackWithMetadata with:" << std::endl;
    std::cout << "  media_type:    Music" << std::endl;
    std::cout << "  audio_path:    " << file_path << std::endl;
    std::cout << "  artist_name:   " << artist << std::endl;
    std::cout << "  album_name:    " << album << std::endl;
    std::cout << "  album_year:    " << year << std::endl;
    std::cout << "  track_title:   " << title << std::endl;
    std::cout << "  genre:         " << genre << std::endl;
    std::cout << "  track_number:  " << track_num << std::endl;
    std::cout << "  artwork_size:  " << artwork.size() << std::endl;
    std::cout << "  artist_guid:   " << (artist_guid.empty() ? "(empty)" : artist_guid) << std::endl;
    std::cout << "  duration_ms:   " << final_duration_ms << std::endl;
    std::cout << "  rating:        " << rating << std::endl;
    std::cout << std::endl;

    uint32_t out_track_id = 0, out_album_id = 0, out_artist_id = 0;

    auto start_time = std::chrono::steady_clock::now();

    int result = device.UploadTrackWithMetadata(
        MediaType::Music,
        file_path,
        artist,
        album,
        year,
        title,
        genre,
        track_num,
        artwork.empty() ? nullptr : artwork.data(),
        artwork.size(),
        artist_guid,
        final_duration_ms,
        rating,
        &out_track_id,
        &out_album_id,
        &out_artist_id
    );

    auto end_time = std::chrono::steady_clock::now();
    auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time).count();

    std::cout << std::endl;
    std::cout << "=== Result ===" << std::endl;
    std::cout << "Return code: " << result << std::endl;
    std::cout << "Elapsed time: " << elapsed_ms << " ms" << std::endl;

    if (result == 0) {
        std::cout << "Status: SUCCESS" << std::endl;
        std::cout << "  Track ID:  0x" << std::hex << out_track_id << std::dec << std::endl;
        std::cout << "  Album ID:  0x" << std::hex << out_album_id << std::dec << std::endl;
        std::cout << "  Artist ID: 0x" << std::hex << out_artist_id << std::dec << std::endl;
    } else {
        std::cout << "Status: FAILED" << std::endl;
    }

    // Disconnect
    std::cout << std::endl;
    std::cout << "Disconnecting..." << std::endl;
    device.Disconnect();
    std::cout << "Done." << std::endl;

    return result;
}
