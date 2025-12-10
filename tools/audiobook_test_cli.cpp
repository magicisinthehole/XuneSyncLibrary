/**
 * Audiobook Upload Test Tool
 *
 * Upload audiobook tracks from a directory or single file.
 * Multi-part chapters are automatically concatenated (lossless) before upload.
 * Chapter grouping uses ID3 title tags; ordering uses track numbers.
 *
 * Usage:
 *   audiobook_test_cli <directory>              - Upload all tracks from directory
 *   audiobook_test_cli <file> [name] [author]   - Upload single track with optional overrides
 */

#include <iostream>
#include <fstream>
#include <string>
#include <vector>
#include <map>
#include <algorithm>
#include <filesystem>
#include <cstring>
#include <cstdlib>

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

#include "lib/src/ZuneDevice.h"
#include "lib/src/zmdb/ZuneHDParser.h"
#include "lib/src/zmdb/ZMDBTypes.h"

namespace fs = std::filesystem;

// Global flag for logging
static bool g_show_logs = true;

struct AudioTrackInfo {
    std::string file_path;
    std::string title;           // Chapter name (used for grouping)
    std::string album;
    std::string artist;
    int track_number;            // Global track number (for ordering)
    int year;
    uint32_t duration_ms;
};

struct ChapterInfo {
    std::string chapter_title;   // Chapter name (from title tag)
    std::vector<AudioTrackInfo> parts;  // All parts of this chapter (sorted by track_number)
    std::string concat_file_path;       // Path to concatenated file (temp or original)
    uint32_t total_duration_ms;
    bool needs_cleanup;          // True if concat_file_path is a temp file
    int part_number;             // Book part number (1-4), 0 for intro/end
};

/**
 * Extracts the book part number from a chapter title.
 * "Part 1 - Chapter 5" -> 1
 * "Part 2 - Chapter 12" -> 2
 * "Introduction" -> 0
 * "The End" -> 0
 */
int extract_part_number(const std::string& title) {
    // Look for "Part X" pattern
    size_t pos = title.find("Part ");
    if (pos != std::string::npos && pos + 5 < title.length()) {
        char digit = title[pos + 5];
        if (digit >= '1' && digit <= '9') {
            return digit - '0';
        }
    }
    return 0;  // Introduction, The End, or no part found
}

/**
 * Strips "Part X - " prefix from chapter titles.
 * "Part 1 - Chapter 5" -> "Chapter 5"
 * "Part 2. Chapter 12" -> "Chapter 12"
 * "Introduction" -> "Introduction" (unchanged)
 */
std::string strip_part_prefix(const std::string& title) {
    // Look for "Part X - " or "Part X. " pattern
    size_t pos = title.find("Part ");
    if (pos != std::string::npos && pos + 5 < title.length()) {
        char digit = title[pos + 5];
        if (digit >= '1' && digit <= '9' && pos + 6 < title.length()) {
            // Check for " - " or ". " separator
            char sep1 = title[pos + 6];
            if (sep1 == ' ' && pos + 8 < title.length() && title[pos + 7] == '-' && title[pos + 8] == ' ') {
                // "Part X - " pattern (9 chars)
                return title.substr(pos + 9);
            } else if (sep1 == '.' && pos + 8 < title.length() && title[pos + 7] == ' ') {
                // "Part X. " pattern (8 chars)
                return title.substr(pos + 8);
            }
        }
    }
    return title;  // No pattern found, return unchanged
}

void log_callback(const std::string& message) {
    if (g_show_logs) {
        std::cout << "  [LOG] " << message << std::endl;
    }
}

void print_usage(const char* prog_name) {
    std::cout << "Audiobook Upload Test Tool" << std::endl;
    std::cout << std::endl;
    std::cout << "Usage:" << std::endl;
    std::cout << "  " << prog_name << " <directory> [cover_art]        Upload all audio files from directory" << std::endl;
    std::cout << "  " << prog_name << " <file> [name] [author]         Upload single file with optional metadata overrides" << std::endl;
    std::cout << std::endl;
    std::cout << "Features:" << std::endl;
    std::cout << "  - Automatically groups multi-part chapters by title tag" << std::endl;
    std::cout << "  - Concatenates parts losslessly (no re-encoding)" << std::endl;
    std::cout << "  - Orders parts by track number from metadata" << std::endl;
    std::cout << std::endl;
    std::cout << "Examples:" << std::endl;
    std::cout << "  " << prog_name << " \"/path/to/East of Eden\" \"/path/to/cover.jpg\"" << std::endl;
    std::cout << "  " << prog_name << " \"chapter1.mp3\" \"My Audiobook\" \"The Author\"" << std::endl;
}

std::vector<uint8_t> read_file(const std::string& path) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) {
        return {};
    }

    std::streamsize size = file.tellg();
    file.seekg(0, std::ios::beg);

    std::vector<uint8_t> buffer(size);
    if (!file.read(reinterpret_cast<char*>(buffer.data()), size)) {
        return {};
    }

    return buffer;
}

bool is_audio_file(const std::string& path) {
    std::string ext = fs::path(path).extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
    return ext == ".mp3" || ext == ".wma" || ext == ".m4a" || ext == ".m4b";
}

AudioTrackInfo extract_track_info(const std::string& file_path) {
    AudioTrackInfo info;
    info.file_path = file_path;
    info.track_number = 0;
    info.year = 0;
    info.duration_ms = 0;

    // Default title from filename
    info.title = fs::path(file_path).stem().string();

    TagLib::FileRef file(file_path.c_str());
    if (file.isNull()) {
        return info;
    }

    // Get duration
    if (file.audioProperties()) {
        info.duration_ms = static_cast<uint32_t>(file.audioProperties()->lengthInMilliseconds());
    }

    // Get basic tags
    if (file.tag()) {
        TagLib::Tag* tag = file.tag();

        if (!tag->title().isEmpty()) {
            info.title = tag->title().toCString(true);
        }
        if (!tag->album().isEmpty()) {
            info.album = tag->album().toCString(true);
        }
        if (!tag->artist().isEmpty()) {
            info.artist = tag->artist().toCString(true);
        }
        info.track_number = tag->track();
        info.year = tag->year();
    }

    return info;
}

std::vector<uint8_t> extract_embedded_artwork(const std::string& file_path) {
    std::vector<uint8_t> artwork;

    TagLib::FileRef fileRef(file_path.c_str());
    if (fileRef.isNull()) {
        return artwork;
    }

    // Try MP3/ID3v2
    TagLib::MPEG::File* mpegFile = dynamic_cast<TagLib::MPEG::File*>(fileRef.file());
    if (mpegFile && mpegFile->ID3v2Tag()) {
        auto frames = mpegFile->ID3v2Tag()->frameList("APIC");
        if (!frames.isEmpty()) {
            auto pictureFrame = dynamic_cast<TagLib::ID3v2::AttachedPictureFrame*>(frames.front());
            if (pictureFrame) {
                TagLib::ByteVector data = pictureFrame->picture();
                artwork.assign(data.begin(), data.end());
                return artwork;
            }
        }
    }

    // Try WMA/ASF
    TagLib::ASF::File* asfFile = dynamic_cast<TagLib::ASF::File*>(fileRef.file());
    if (asfFile && asfFile->tag()) {
        TagLib::ASF::Tag* asfTag = asfFile->tag();
        TagLib::ASF::AttributeListMap& attrMap = asfTag->attributeListMap();
        if (attrMap.contains("WM/Picture")) {
            TagLib::ASF::AttributeList& pictures = attrMap["WM/Picture"];
            if (!pictures.isEmpty()) {
                TagLib::ByteVector picture = pictures[0].toPicture().picture();
                artwork.assign(picture.begin(), picture.end());
                return artwork;
            }
        }
    }

    // Try MP4/M4A/M4B
    TagLib::MP4::File* mp4File = dynamic_cast<TagLib::MP4::File*>(fileRef.file());
    if (mp4File && mp4File->tag()) {
        TagLib::MP4::Tag* mp4Tag = mp4File->tag();
        if (mp4Tag->contains("covr")) {
            TagLib::MP4::CoverArtList covers = mp4Tag->item("covr").toCoverArtList();
            if (!covers.isEmpty()) {
                TagLib::ByteVector data = covers[0].data();
                artwork.assign(data.begin(), data.end());
                return artwork;
            }
        }
    }

    return artwork;
}

std::vector<uint8_t> find_cover_art(const std::string& directory, const std::vector<AudioTrackInfo>& tracks) {
    // Try common cover art filenames
    std::vector<std::string> cover_names = {
        "cover.jpg", "cover.jpeg", "cover.png",
        "Cover.jpg", "Cover.jpeg", "Cover.png",
        "folder.jpg", "folder.jpeg", "folder.png",
        "Folder.jpg", "Folder.jpeg", "Folder.png",
        "front.jpg", "front.jpeg", "front.png",
        "album.jpg", "album.jpeg", "album.png"
    };

    for (const auto& name : cover_names) {
        std::string path = directory + "/" + name;
        if (fs::exists(path)) {
            auto data = read_file(path);
            if (!data.empty()) {
                std::cout << "  Found cover art: " << name << std::endl;
                return data;
            }
        }
    }

    // Try to extract embedded artwork from first track
    if (!tracks.empty()) {
        auto artwork = extract_embedded_artwork(tracks[0].file_path);
        if (!artwork.empty()) {
            std::cout << "  Using embedded artwork from: " << fs::path(tracks[0].file_path).filename().string() << std::endl;
            return artwork;
        }
    }

    return {};
}

std::vector<AudioTrackInfo> scan_directory(const std::string& directory) {
    std::vector<AudioTrackInfo> tracks;

    for (const auto& entry : fs::directory_iterator(directory)) {
        if (entry.is_regular_file() && is_audio_file(entry.path().string())) {
            auto info = extract_track_info(entry.path().string());
            tracks.push_back(info);
        }
    }

    // Sort by track number, then by filename
    std::sort(tracks.begin(), tracks.end(), [](const AudioTrackInfo& a, const AudioTrackInfo& b) {
        if (a.track_number != b.track_number) {
            return a.track_number < b.track_number;
        }
        return a.file_path < b.file_path;
    });

    return tracks;
}

/**
 * Groups tracks by chapter title and sorts parts within each chapter by track number.
 * Returns chapters in order of their first track's position.
 */
std::vector<ChapterInfo> group_tracks_by_chapter(const std::vector<AudioTrackInfo>& tracks) {
    // Group by title tag
    std::map<std::string, std::vector<AudioTrackInfo>> chapter_map;
    std::map<std::string, int> chapter_first_track;  // Track first occurrence order

    for (const auto& track : tracks) {
        if (chapter_map.find(track.title) == chapter_map.end()) {
            chapter_first_track[track.title] = track.track_number;
        }
        chapter_map[track.title].push_back(track);
    }

    // Sort parts within each chapter by track number
    for (auto& [title, parts] : chapter_map) {
        std::sort(parts.begin(), parts.end(), [](const AudioTrackInfo& a, const AudioTrackInfo& b) {
            return a.track_number < b.track_number;
        });
    }

    // Create ChapterInfo objects
    std::vector<ChapterInfo> chapters;
    for (const auto& [title, parts] : chapter_map) {
        ChapterInfo chapter;
        chapter.chapter_title = strip_part_prefix(title);  // Strip "Part X - " for display
        chapter.parts = parts;
        chapter.needs_cleanup = false;
        chapter.total_duration_ms = 0;

        for (const auto& part : parts) {
            chapter.total_duration_ms += part.duration_ms;
        }

        // If only one part, use original file directly
        if (parts.size() == 1) {
            chapter.concat_file_path = parts[0].file_path;
        }

        // Extract book part number from title (Part 1, Part 2, etc.)
        chapter.part_number = extract_part_number(title);

        chapters.push_back(chapter);
    }

    // Sort chapters by their first track number (original order)
    std::sort(chapters.begin(), chapters.end(), [&](const ChapterInfo& a, const ChapterInfo& b) {
        // Use original title for lookup since chapter_title is now stripped
        std::string a_orig = a.parts.empty() ? "" : a.parts[0].title;
        std::string b_orig = b.parts.empty() ? "" : b.parts[0].title;
        return chapter_first_track[a_orig] < chapter_first_track[b_orig];
    });

    // Fix part numbers for chapters without explicit parts based on position
    // Find max part number
    int max_part = 0;
    for (const auto& ch : chapters) {
        if (ch.part_number > max_part) {
            max_part = ch.part_number;
        }
    }

    // Find the index of first chapter with a part number
    int first_part_idx = -1;
    for (size_t i = 0; i < chapters.size(); i++) {
        if (chapters[i].part_number > 0) {
            first_part_idx = static_cast<int>(i);
            break;
        }
    }

    // Chapters with part_number=0 that come after all Part N chapters get max_part+1
    if (first_part_idx >= 0) {
        // Find last chapter with a part number
        int last_part_idx = -1;
        for (int i = static_cast<int>(chapters.size()) - 1; i >= 0; i--) {
            if (chapters[i].part_number > 0) {
                last_part_idx = i;
                break;
            }
        }

        // Assign max_part+1 to chapters without parts that come after the last Part N
        for (size_t i = last_part_idx + 1; i < chapters.size(); i++) {
            if (chapters[i].part_number == 0) {
                chapters[i].part_number = max_part + 1;
            }
        }
    }

    return chapters;
}

/**
 * Concatenates multiple audio files into one using ffmpeg -c copy (lossless).
 * Returns path to concatenated file, or empty string on failure.
 */
std::string concatenate_audio_files(const std::vector<AudioTrackInfo>& parts, int chapter_index) {
    if (parts.empty()) return "";
    if (parts.size() == 1) return parts[0].file_path;

    // Create temp directory for concat list and output
    std::string temp_dir = fs::temp_directory_path().string();
    std::string list_file = temp_dir + "/audiobook_concat_" + std::to_string(chapter_index) + ".txt";
    std::string output_file = temp_dir + "/audiobook_chapter_" + std::to_string(chapter_index) + ".mp3";

    // Write concat list file
    std::ofstream list_stream(list_file);
    if (!list_stream) {
        std::cerr << "    ERROR: Could not create concat list file" << std::endl;
        return "";
    }

    for (const auto& part : parts) {
        // Escape single quotes in paths for ffmpeg concat demuxer
        std::string escaped_path = part.file_path;
        size_t pos = 0;
        while ((pos = escaped_path.find("'", pos)) != std::string::npos) {
            escaped_path.replace(pos, 1, "'\\''");
            pos += 4;
        }
        list_stream << "file '" << escaped_path << "'" << std::endl;
    }
    list_stream.close();

    // Run ffmpeg with -c copy (no re-encoding)
    std::string command = "ffmpeg -y -f concat -safe 0 -i \"" + list_file + "\" -c copy \"" + output_file + "\" 2>/dev/null";

    int result = std::system(command.c_str());

    // Clean up list file
    fs::remove(list_file);

    if (result != 0) {
        std::cerr << "    ERROR: ffmpeg concatenation failed (exit code " << result << ")" << std::endl;
        return "";
    }

    if (!fs::exists(output_file)) {
        std::cerr << "    ERROR: Output file not created" << std::endl;
        return "";
    }

    return output_file;
}

void print_audiobooks_from_zmdb(ZuneDevice& device) {
    std::cout << std::endl;
    std::cout << "=== ZMDB Audiobooks ===" << std::endl;

    std::vector<uint8_t> library_object_id = {0x03, 0x92, 0x1f};
    mtp::ByteArray zmdb_byte_array = device.GetZuneMetadata(library_object_id);

    if (zmdb_byte_array.empty()) {
        std::cerr << "  (Could not retrieve ZMDB)" << std::endl;
        return;
    }

    std::vector<uint8_t> zmdb_data(zmdb_byte_array.begin(), zmdb_byte_array.end());

    try {
        zmdb::ZuneHDParser parser;
        zmdb::ZMDBLibrary library = parser.ExtractLibrary(zmdb_data);

        std::cout << "  Total audiobook tracks: " << library.audiobook_count << std::endl;

        if (library.audiobook_count > 0) {
            for (int i = 0; i < library.audiobook_count; i++) {
                const auto& ab = library.audiobooks[i];
                std::cout << "  [" << ab.track_number << "] " << ab.title;
                if (ab.duration_ms > 0) {
                    int mins = ab.duration_ms / 1000 / 60;
                    int secs = (ab.duration_ms / 1000) % 60;
                    std::cout << " (" << mins << ":" << (secs < 10 ? "0" : "") << secs << ")";
                }
                std::cout << std::endl;
            }
        }
    } catch (const std::exception& e) {
        std::cerr << "  ERROR parsing ZMDB: " << e.what() << std::endl;
    }
}

std::string format_duration(uint32_t ms) {
    if (ms == 0) return "0:00";
    int hours = ms / 1000 / 3600;
    int mins = (ms / 1000 / 60) % 60;
    int secs = (ms / 1000) % 60;

    if (hours > 0) {
        return std::to_string(hours) + ":" +
               (mins < 10 ? "0" : "") + std::to_string(mins) + ":" +
               (secs < 10 ? "0" : "") + std::to_string(secs);
    }
    return std::to_string(mins) + ":" + (secs < 10 ? "0" : "") + std::to_string(secs);
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        print_usage(argv[0]);
        return 1;
    }

    std::string input_path = argv[1];
    bool is_directory = fs::is_directory(input_path);

    std::vector<AudioTrackInfo> tracks;
    std::string audiobook_name;
    std::string author;
    int year = 0;
    std::vector<uint8_t> artwork_data;

    if (is_directory) {
        // Directory mode - scan for all audio files
        std::cout << "=== Scanning Directory ===" << std::endl;
        std::cout << "Path: " << input_path << std::endl;
        std::cout << std::endl;

        tracks = scan_directory(input_path);

        if (tracks.empty()) {
            std::cerr << "ERROR: No audio files found in directory" << std::endl;
            return 1;
        }

        // Get audiobook name and author from first track's album/artist tags
        for (const auto& track : tracks) {
            if (!track.album.empty() && audiobook_name.empty()) {
                audiobook_name = track.album;
            }
            if (!track.artist.empty() && author.empty()) {
                author = track.artist;
            }
            if (track.year > 0 && year == 0) {
                year = track.year;
            }
        }

        // Fallback to directory name if no album tag
        if (audiobook_name.empty()) {
            audiobook_name = fs::path(input_path).filename().string();
        }
        if (author.empty()) {
            author = "Unknown Author";
        }

        // Find cover art - check for explicit argument first
        if (argc > 2) {
            std::string cover_path = argv[2];
            if (fs::exists(cover_path)) {
                artwork_data = read_file(cover_path);
                if (!artwork_data.empty()) {
                    std::cout << "  Using specified cover art: " << cover_path << std::endl;
                }
            } else {
                std::cerr << "WARNING: Cover art not found: " << cover_path << std::endl;
            }
        }
        if (artwork_data.empty()) {
            artwork_data = find_cover_art(input_path, tracks);
        }

        std::cout << "Found " << tracks.size() << " audio file(s)" << std::endl;
        std::cout << std::endl;
    } else {
        // Single file mode
        if (!fs::exists(input_path)) {
            std::cerr << "ERROR: File not found: " << input_path << std::endl;
            return 1;
        }

        auto info = extract_track_info(input_path);

        // Allow command-line overrides
        audiobook_name = (argc > 2) ? argv[2] : (info.album.empty() ? info.title : info.album);
        author = (argc > 3) ? argv[3] : (info.artist.empty() ? "Unknown Author" : info.artist);
        year = info.year;

        // Ensure track number is at least 1
        if (info.track_number == 0) {
            info.track_number = 1;
        }

        tracks.push_back(info);

        // Try to find cover art in same directory
        std::string dir = fs::path(input_path).parent_path().string();
        artwork_data = find_cover_art(dir, tracks);
    }

    // Display audiobook info
    std::cout << "=== Audiobook Info ===" << std::endl;
    std::cout << "Name:   " << audiobook_name << std::endl;
    std::cout << "Author: " << author << std::endl;
    std::cout << "Year:   " << (year > 0 ? std::to_string(year) : "(not set)") << std::endl;
    std::cout << "Cover:  " << (artwork_data.empty() ? "(none)" : std::to_string(artwork_data.size()) + " bytes") << std::endl;
    std::cout << std::endl;

    // Group tracks by chapter
    std::cout << "=== Grouping by Chapter ===" << std::endl;
    std::vector<ChapterInfo> chapters = group_tracks_by_chapter(tracks);

    std::cout << "Found " << chapters.size() << " chapter(s) from " << tracks.size() << " file(s)" << std::endl;
    std::cout << std::endl;

    uint32_t total_duration = 0;
    for (size_t i = 0; i < chapters.size(); i++) {
        const auto& ch = chapters[i];
        std::cout << "  " << (i+1) << ". " << ch.chapter_title;
        std::cout << " [Part " << ch.part_number << "]";
        std::cout << " (" << ch.parts.size() << " file" << (ch.parts.size() > 1 ? "s" : "") << ", ";
        std::cout << format_duration(ch.total_duration_ms) << ")" << std::endl;
        total_duration += ch.total_duration_ms;
    }
    std::cout << std::endl;
    std::cout << "Total duration: " << format_duration(total_duration) << std::endl;
    std::cout << std::endl;

    // Concatenate multi-part chapters
    std::cout << "=== Concatenating Multi-Part Chapters ===" << std::endl;
    int concat_count = 0;
    for (size_t i = 0; i < chapters.size(); i++) {
        auto& ch = chapters[i];
        if (ch.parts.size() > 1) {
            std::cout << "  Concatenating: " << ch.chapter_title << " (" << ch.parts.size() << " parts)..." << std::endl;

            std::string concat_path = concatenate_audio_files(ch.parts, static_cast<int>(i));
            if (concat_path.empty()) {
                std::cerr << "    FAILED - will skip this chapter" << std::endl;
                continue;
            }

            ch.concat_file_path = concat_path;
            ch.needs_cleanup = true;
            concat_count++;

            // Get actual duration from concatenated file
            TagLib::FileRef concat_file(concat_path.c_str());
            if (!concat_file.isNull() && concat_file.audioProperties()) {
                ch.total_duration_ms = static_cast<uint32_t>(concat_file.audioProperties()->lengthInMilliseconds());
            }

            std::cout << "    OK (" << format_duration(ch.total_duration_ms) << ")" << std::endl;
        }
    }

    if (concat_count == 0) {
        std::cout << "  (No multi-part chapters to concatenate)" << std::endl;
    }
    std::cout << std::endl;

    // Connect to device
    std::cout << "=== Connecting to Device ===" << std::endl;

    ZuneDevice device;
    device.SetLogCallback(log_callback);

    if (!device.ConnectUSB()) {
        std::cerr << "ERROR: Could not connect to Zune device" << std::endl;

        // Cleanup temp files
        for (const auto& ch : chapters) {
            if (ch.needs_cleanup && !ch.concat_file_path.empty()) {
                fs::remove(ch.concat_file_path);
            }
        }
        return 1;
    }

    std::cout << "Connected to: " << device.GetName() << std::endl;

    // Show current audiobooks
    print_audiobooks_from_zmdb(device);

    // Upload chapters
    std::cout << std::endl;
    std::cout << "=== Uploading Chapters ===" << std::endl;

    int success_count = 0;
    int fail_count = 0;

    for (size_t i = 0; i < chapters.size(); i++) {
        const auto& chapter = chapters[i];

        // Skip chapters that failed concatenation
        if (chapter.concat_file_path.empty()) {
            std::cout << std::endl;
            std::cout << "Skipping [" << (i+1) << "/" << chapters.size() << "]: " << chapter.chapter_title << " (concat failed)" << std::endl;
            fail_count++;
            continue;
        }

        std::cout << std::endl;
        std::cout << "Uploading [" << (i+1) << "/" << chapters.size() << "]: " << chapter.chapter_title;
        if (chapter.parts.size() > 1) {
            std::cout << " (concatenated from " << chapter.parts.size() << " parts)";
        }
        std::cout << std::endl;

        uint32_t track_id = 0, album_id = 0, artist_id = 0;

        // Send artwork with every track
        const uint8_t* art_ptr = artwork_data.empty() ? nullptr : artwork_data.data();
        size_t art_size = artwork_data.size();

        int result = device.UploadTrackWithMetadata(
            MediaType::Audiobook,
            chapter.concat_file_path,
            author,
            audiobook_name,
            year,
            chapter.chapter_title,
            "",  // genre
            chapter.part_number,  // Use book part number (1-4, or 0 for intro/end)
            art_ptr,
            art_size,
            "",  // artist_guid
            chapter.total_duration_ms,
            -1,  // rating (not used for audiobooks)
            &track_id,
            &album_id,
            &artist_id
        );

        if (result == 0) {
            std::cout << "  OK (track_id=0x" << std::hex << track_id << std::dec;
            std::cout << ", duration=" << format_duration(chapter.total_duration_ms) << ")" << std::endl;
            success_count++;
        } else {
            std::cerr << "  FAILED (code " << result << ")" << std::endl;
            fail_count++;
        }
    }

    // Clean up temp files
    std::cout << std::endl;
    std::cout << "=== Cleanup ===" << std::endl;
    int cleanup_count = 0;
    for (const auto& ch : chapters) {
        if (ch.needs_cleanup && !ch.concat_file_path.empty()) {
            if (fs::remove(ch.concat_file_path)) {
                cleanup_count++;
            }
        }
    }
    std::cout << "Removed " << cleanup_count << " temporary file(s)" << std::endl;

    // Show results
    std::cout << std::endl;
    std::cout << "=== Upload Complete ===" << std::endl;
    std::cout << "Successful: " << success_count << " chapter(s)" << std::endl;
    std::cout << "Failed:     " << fail_count << std::endl;

    // Show final state
    print_audiobooks_from_zmdb(device);

    device.Disconnect();
    std::cout << std::endl;
    std::cout << "Done." << std::endl;

    return (fail_count > 0) ? 1 : 0;
}
