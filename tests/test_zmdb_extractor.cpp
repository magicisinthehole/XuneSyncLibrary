#include <iostream>
#include <fstream>
#include <sstream>
#include <iomanip>
#include <vector>
#include <string>
#include <cstring>
#include <algorithm>
#include <memory>

#include "lib/src/ZuneDevice.h"
#include "lib/src/ZuneDeviceIdentification.h"
#include "lib/src/zmdb/ZMDBTypes.h"
#include "lib/src/zmdb/ZMDBParserFactory.h"

static std::string JsonEscape(const std::string& str) {
    // Validate UTF-8 sequences as we go; any invalid byte is replaced with
    // the U+FFFD replacement character so the JSON stays well-formed.
    auto write_u4 = [](std::ostream& o, uint32_t cp) {
        o << "\\u" << std::hex << std::setw(4) << std::setfill('0') << cp << std::dec;
    };
    std::ostringstream out;
    const auto* bytes = reinterpret_cast<const unsigned char*>(str.data());
    size_t n = str.size(), i = 0;
    while (i < n) {
        unsigned char c = bytes[i];
        switch (c) {
            case '"':  out << "\\\""; ++i; continue;
            case '\\': out << "\\\\"; ++i; continue;
            case '\b': out << "\\b";  ++i; continue;
            case '\f': out << "\\f";  ++i; continue;
            case '\n': out << "\\n";  ++i; continue;
            case '\r': out << "\\r";  ++i; continue;
            case '\t': out << "\\t";  ++i; continue;
        }
        if (c < 0x20) { write_u4(out, c); ++i; continue; }
        if (c < 0x80) { out << (char)c;   ++i; continue; }
        // UTF-8 continuation: validate the multi-byte sequence
        size_t need = 0;
        if      ((c & 0xE0) == 0xC0) need = 1;
        else if ((c & 0xF0) == 0xE0) need = 2;
        else if ((c & 0xF8) == 0xF0) need = 3;
        else { write_u4(out, 0xFFFD); ++i; continue; }
        if (i + need >= n) { write_u4(out, 0xFFFD); ++i; continue; }
        bool valid = true;
        for (size_t k = 1; k <= need; ++k) {
            if ((bytes[i + k] & 0xC0) != 0x80) { valid = false; break; }
        }
        if (!valid) { write_u4(out, 0xFFFD); ++i; continue; }
        for (size_t k = 0; k <= need; ++k) out << (char)bytes[i + k];
        i += need + 1;
    }
    return out.str();
}

static mtp::ByteArray ReadBinaryFile(const std::string& filepath) {
    std::ifstream file(filepath, std::ios::binary | std::ios::ate);
    if (!file.is_open()) {
        std::cerr << "ERROR: Failed to open file: " << filepath << "\n";
        return {};
    }
    std::streamsize size = file.tellg();
    file.seekg(0, std::ios::beg);
    mtp::ByteArray data(size);
    if (!file.read(reinterpret_cast<char*>(data.data()), size)) {
        std::cerr << "ERROR: Failed to read file: " << filepath << "\n";
        return {};
    }
    // Archived ZMDBs from prior tools wrap the payload in a 12-byte envelope
    // before the `ZMDB` magic. Live device pulls start with `ZMDB` at offset 0.
    if (data.size() >= 16 && !(data[0]=='Z' && data[1]=='M' && data[2]=='D' && data[3]=='B')) {
        if (data[12]=='Z' && data[13]=='M' && data[14]=='D' && data[15]=='B') {
            std::cout << "[file] Stripping 12-byte archive envelope\n";
            data.erase(data.begin(), data.begin() + 12);
        }
    }
    return data;
}

static zune::DeviceFamily ParseDeviceType(const std::string& type) {
    if (type == "Zune30" || type == "Zune 30" || type == "Classic" || type == "Keel")
        return zune::DeviceFamily::Keel;
    if (type == "Zune80" || type == "Zune 80" || type == "Draco")
        return zune::DeviceFamily::Draco;
    if (type == "ZuneHD" || type == "Zune HD" || type == "HD" || type == "Pavo")
        return zune::DeviceFamily::Pavo;
    return zune::DeviceFamily::Unknown;
}

static std::string HexU32(uint32_t v) {
    std::ostringstream ss;
    ss << "0x" << std::hex << std::setw(8) << std::setfill('0') << v;
    return ss.str();
}

// ── JSON writer for the full zmdb::ZMDBLibrary ────────────────────────────

static void WriteLibraryJson(const zmdb::ZMDBLibrary& lib, std::ostream& out) {
    out << "{\n";

    // Counts at the top for quick inspection.
    out << "  \"counts\": {\n";
    out << "    \"tracks\": " << lib.track_count << ",\n";
    out << "    \"videos\": " << lib.video_count << ",\n";
    out << "    \"pictures\": " << lib.picture_count << ",\n";
    out << "    \"playlists\": " << lib.playlist_count << ",\n";
    out << "    \"podcasts\": " << lib.podcast_count << ",\n";
    out << "    \"podcast_shows\": " << lib.podcast_show_count << ",\n";
    out << "    \"audiobooks\": " << lib.audiobook_count << ",\n";
    out << "    \"album_metadata\": " << lib.album_count << ",\n";
    out << "    \"artist_metadata\": " << lib.artist_count << ",\n";
    out << "    \"genre_metadata\": " << lib.genre_count << "\n";
    out << "  },\n";

    // Podcast shows.
    out << "  \"podcast_shows\": [\n";
    {
        size_t i = 0, n = lib.podcast_show_metadata.size();
        for (const auto& [atom_id, show] : lib.podcast_show_metadata) {
            out << "    {\n";
            out << "      \"atom_id\": \"" << HexU32(show.atom_id) << "\",\n";
            out << "      \"name\": \"" << JsonEscape(show.name) << "\",\n";
            out << "      \"ser_filename\": \"" << JsonEscape(show.ser_filename) << "\",\n";
            out << "      \"author\": \"" << JsonEscape(show.author) << "\",\n";
            out << "      \"feed_url\": \"" << JsonEscape(show.feed_url) << "\",\n";
            out << "      \"filename_ref\": \"" << HexU32(show.filename_ref) << "\",\n";
            out << "      \"is_subscribed\": " << (show.is_subscribed ? "true" : "false") << "\n";
            out << "    }" << (++i < n ? "," : "") << "\n";
        }
    }
    out << "  ],\n";

    // Podcast episodes (audio + promoted video podcasts).
    out << "  \"podcast_episodes\": [\n";
    for (int i = 0; i < lib.podcast_count; ++i) {
        const auto& ep = lib.podcasts[i];
        out << "    {\n";
        out << "      \"atom_id\": \"" << HexU32(ep.atom_id) << "\",\n";
        out << "      \"media_type\": \""
            << (ep.media_type == zmdb::PodcastMediaType::Video ? "video" : "audio") << "\",\n";
        out << "      \"title\": \"" << JsonEscape(ep.title) << "\",\n";
        out << "      \"show_name\": \"" << JsonEscape(ep.show_name) << "\",\n";
        out << "      \"folder_name\": \"" << JsonEscape(ep.folder_name) << "\",\n";
        out << "      \"author\": \"" << JsonEscape(ep.author) << "\",\n";
        out << "      \"description\": \"" << JsonEscape(ep.description) << "\",\n";
        out << "      \"episode_url\": \"" << JsonEscape(ep.episode_url) << "\",\n";
        out << "      \"episode_filename\": \"" << JsonEscape(ep.episode_filename) << "\",\n";
        out << "      \"podcast_show_ref\": \"" << HexU32(ep.podcast_show_ref) << "\",\n";
        out << "      \"filename_ref\": \"" << HexU32(ep.filename_ref) << "\",\n";
        out << "      \"duration_ms\": " << ep.duration_ms << ",\n";
        out << "      \"bookmark_ms\": " << ep.bookmark_ms << ",\n";
        out << "      \"publish_date_filetime\": " << ep.publish_date << ",\n";
        out << "      \"file_size_bytes\": " << ep.file_size_bytes << ",\n";
        out << "      \"codec_id\": \"0x" << std::hex << std::setw(4)
            << std::setfill('0') << ep.codec_id << "\",\n" << std::dec;
        out << "      \"meta_genre\": " << ep.meta_genre() << ",\n";
        out << "      \"played_flag\": \"" << HexU32(ep.played_flag) << "\",\n";
        out << "      \"is_played\": " << (ep.is_played() ? "true" : "false") << "\n";
        out << "    }" << (i + 1 < lib.podcast_count ? "," : "") << "\n";
    }
    out << "  ],\n";

    // Tracks (every parsed field surfaced — for byte-level verification).
    out << "  \"tracks\": [\n";
    for (int i = 0; i < lib.track_count; ++i) {
        const auto& t = lib.tracks[i];
        out << "    {\n";
        out << "      \"atom_id\": \"" << HexU32(t.atom_id) << "\",\n";
        out << "      \"title\": \"" << JsonEscape(t.title) << "\",\n";
        out << "      \"artist\": \"" << JsonEscape(t.artist_name) << "\",\n";
        out << "      \"artist_guid\": \"" << JsonEscape(t.artist_guid) << "\",\n";
        out << "      \"album\": \"" << JsonEscape(t.album_name) << "\",\n";
        out << "      \"album_artist\": \"" << JsonEscape(t.album_artist_name) << "\",\n";
        out << "      \"album_artist_guid\": \"" << JsonEscape(t.album_artist_guid) << "\",\n";
        out << "      \"album_ref\": \"" << HexU32(t.album_ref) << "\",\n";
        out << "      \"genre\": \"" << JsonEscape(t.genre) << "\",\n";
        out << "      \"genre_ref\": \"" << HexU32(t.genre_ref) << "\",\n";
        out << "      \"filename\": \"" << JsonEscape(t.filename) << "\",\n";
        out << "      \"track_number\": " << t.track_number << ",\n";
        out << "      \"disc_number\": " << t.disc_number << ",\n";
        out << "      \"duration_ms\": " << t.duration_ms << ",\n";
        out << "      \"file_size_bytes\": " << t.file_size_bytes << ",\n";
        out << "      \"codec_id\": \"0x" << std::hex << std::setw(4)
            << std::setfill('0') << t.codec_id << "\",\n" << std::dec;
        out << "      \"rating\": " << static_cast<int>(t.rating) << ",\n";
        out << "      \"playcount\": " << t.playcount << ",\n";
        out << "      \"on_device_playcount\": " << t.on_device_playcount << ",\n";
        out << "      \"skip_count\": " << t.skip_count << ",\n";
        out << "      \"last_played_filetime\": " << t.last_played_timestamp << "\n";
        out << "    }" << (i + 1 < lib.track_count ? "," : "") << "\n";
    }
    out << "  ],\n";

    // Albums.
    out << "  \"albums\": [\n";
    {
        size_t i = 0, n = lib.album_metadata.size();
        for (const auto& [atom_id, a] : lib.album_metadata) {
            out << "    {\n";
            out << "      \"atom_id\": \"" << HexU32(a.atom_id) << "\",\n";
            out << "      \"title\": \"" << JsonEscape(a.title) << "\",\n";
            out << "      \"artist\": \"" << JsonEscape(a.artist_name) << "\",\n";
            out << "      \"artist_guid\": \"" << JsonEscape(a.artist_guid) << "\",\n";
            out << "      \"artist_ref\": \"" << HexU32(a.artist_ref) << "\",\n";
            out << "      \"release_year\": " << a.release_year << ",\n";
            out << "      \"alb_reference\": \"" << JsonEscape(a.alb_reference) << "\",\n";
            out << "      \"album_pid\": \"" << HexU32(a.album_pid) << "\"\n";
            out << "    }" << (++i < n ? "," : "") << "\n";
        }
    }
    out << "  ],\n";

    // Artists.
    out << "  \"artists\": [\n";
    {
        size_t i = 0, n = lib.artist_metadata.size();
        for (const auto& [atom_id, a] : lib.artist_metadata) {
            out << "    {\n";
            out << "      \"atom_id\": \"" << HexU32(a.atom_id) << "\",\n";
            out << "      \"name\": \"" << JsonEscape(a.name) << "\",\n";
            out << "      \"guid\": \"" << JsonEscape(a.guid) << "\",\n";
            out << "      \"filename\": \"" << JsonEscape(a.filename) << "\"\n";
            out << "    }" << (++i < n ? "," : "") << "\n";
        }
    }
    out << "  ],\n";

    // Genres.
    out << "  \"genres\": [\n";
    {
        size_t i = 0, n = lib.genre_metadata.size();
        for (const auto& [atom_id, g] : lib.genre_metadata) {
            out << "    {\n";
            out << "      \"atom_id\": \"" << HexU32(g.atom_id) << "\",\n";
            out << "      \"name\": \"" << JsonEscape(g.name) << "\"\n";
            out << "    }" << (++i < n ? "," : "") << "\n";
        }
    }
    out << "  ],\n";

    // Audiobooks.
    out << "  \"audiobooks\": [\n";
    for (int i = 0; i < lib.audiobook_count; ++i) {
        const auto& a = lib.audiobooks[i];
        out << "    {\n";
        out << "      \"atom_id\": \"" << HexU32(a.atom_id) << "\",\n";
        out << "      \"title\": \"" << JsonEscape(a.title) << "\",\n";
        out << "      \"audiobook_name\": \"" << JsonEscape(a.audiobook_name) << "\",\n";
        out << "      \"author\": \"" << JsonEscape(a.author) << "\",\n";
        out << "      \"filename\": \"" << JsonEscape(a.filename) << "\",\n";
        out << "      \"title_ref\": \"" << HexU32(a.title_ref) << "\",\n";
        out << "      \"filename_ref\": \"" << HexU32(a.filename_ref) << "\",\n";
        out << "      \"track_number\": " << a.track_number << ",\n";
        out << "      \"duration_ms\": " << a.duration_ms << ",\n";
        out << "      \"playback_position_ms\": " << a.playback_position_ms << ",\n";
        out << "      \"file_size_bytes\": " << a.file_size_bytes << ",\n";
        out << "      \"format_code\": \"0x" << std::hex << std::setw(4)
            << std::setfill('0') << a.format_code << "\",\n" << std::dec;
        out << "      \"playcount\": " << a.playcount << ",\n";
        out << "      \"last_played_filetime\": " << a.last_played_timestamp << "\n";
        out << "    }" << (i + 1 < lib.audiobook_count ? "," : "") << "\n";
    }
    out << "  ],\n";

    // Playlists.
    out << "  \"playlists\": [\n";
    for (int i = 0; i < lib.playlist_count; ++i) {
        const auto& p = lib.playlists[i];
        out << "    {\n";
        out << "      \"atom_id\": \"" << HexU32(p.atom_id) << "\",\n";
        out << "      \"name\": \"" << JsonEscape(p.name) << "\",\n";
        out << "      \"filename\": \"" << JsonEscape(p.filename) << "\",\n";
        out << "      \"folder\": \"" << JsonEscape(p.folder) << "\",\n";
        out << "      \"guid\": \"" << JsonEscape(p.guid) << "\",\n";
        out << "      \"track_count\": " << p.track_count << ",\n";
        out << "      \"track_atom_ids\": [";
        for (size_t k = 0; k < p.track_atom_ids.size(); ++k) {
            if (k) out << ", ";
            out << "\"" << HexU32(p.track_atom_ids[k]) << "\"";
        }
        out << "]\n";
        out << "    }" << (i + 1 < lib.playlist_count ? "," : "") << "\n";
    }
    out << "  ],\n";

    // Pictures.
    out << "  \"pictures\": [\n";
    for (int i = 0; i < lib.picture_count; ++i) {
        const auto& p = lib.pictures[i];
        out << "    {\n";
        out << "      \"atom_id\": \"" << HexU32(p.atom_id) << "\",\n";
        out << "      \"title\": \"" << JsonEscape(p.title) << "\",\n";
        out << "      \"photo_album\": \"" << JsonEscape(p.photo_album) << "\",\n";
        out << "      \"user_album\": \"" << JsonEscape(p.user_album) << "\",\n";
        out << "      \"collection\": \"" << JsonEscape(p.collection) << "\",\n";
        out << "      \"filename\": \"" << JsonEscape(p.filename) << "\",\n";
        out << "      \"timestamp_filetime\": " << p.timestamp << "\n";
        out << "    }" << (i + 1 < lib.picture_count ? "," : "") << "\n";
    }
    out << "  ],\n";

    // Videos (non-podcast videos only; podcast videos are in podcast_episodes).
    out << "  \"videos\": [\n";
    for (int i = 0; i < lib.video_count; ++i) {
        const auto& v = lib.videos[i];
        out << "    {\n";
        out << "      \"atom_id\": \"" << HexU32(v.atom_id) << "\",\n";
        out << "      \"title\": \"" << JsonEscape(v.title) << "\",\n";
        out << "      \"folder\": \"" << JsonEscape(v.folder) << "\",\n";
        out << "      \"filename\": \"" << JsonEscape(v.filename) << "\",\n";
        out << "      \"file_size_bytes\": " << v.file_size_bytes << ",\n";
        out << "      \"codec_id\": \"0x" << std::hex << std::setw(4)
            << std::setfill('0') << v.codec_id << "\"\n" << std::dec;
        out << "    }" << (i + 1 < lib.video_count ? "," : "") << "\n";
    }
    out << "  ]\n";

    out << "}\n";
}

static void PrintSummary(const zmdb::ZMDBLibrary& lib) {
    std::cout << "\nExtraction Results:\n";
    std::cout << "  Tracks:        " << lib.track_count << "\n";
    std::cout << "  Videos:        " << lib.video_count << "\n";
    std::cout << "  Pictures:      " << lib.picture_count << "\n";
    std::cout << "  Playlists:     " << lib.playlist_count << "\n";
    std::cout << "  Podcast shows: " << lib.podcast_show_count << "\n";
    std::cout << "  Podcast eps:   " << lib.podcast_count;
    int audio = 0, video = 0;
    for (int i = 0; i < lib.podcast_count; ++i) {
        if (lib.podcasts[i].media_type == zmdb::PodcastMediaType::Audio) ++audio; else ++video;
    }
    std::cout << " (" << audio << " audio, " << video << " video)\n";
    std::cout << "  Audiobooks:    " << lib.audiobook_count << "\n";

    if (lib.podcast_show_count > 0) {
        std::cout << "\nPodcast shows:\n";
        for (const auto& [atom_id, show] : lib.podcast_show_metadata) {
            std::cout << "  " << HexU32(show.atom_id)
                      << "  " << (show.is_subscribed ? "[sub]" : "[   ]")
                      << "  " << show.name
                      << "  (" << show.author << ")\n";
        }
    }
    if (lib.podcast_count > 0) {
        std::cout << "\nPodcast episodes:\n";
        for (int i = 0; i < lib.podcast_count; ++i) {
            const auto& ep = lib.podcasts[i];
            const char* kind = ep.media_type == zmdb::PodcastMediaType::Video ? "video" : "audio";
            double bookmark_min = ep.bookmark_ms / 60000.0;
            double duration_min = ep.duration_ms / 60000.0;
            std::cout << "  " << HexU32(ep.atom_id)
                      << "  [" << kind << "] "
                      << (ep.is_played() ? "[played]  " : "[      ]  ")
                      << std::fixed << std::setprecision(2)
                      << bookmark_min << "m/" << duration_min << "m  "
                      << ep.title << "\n";
        }
    }
}

static void PrintUsage(const char* prog) {
    std::cout << "Usage: " << prog << " [OPTIONS] [FILE]\n\n";
    std::cout << "Options:\n";
    std::cout << "  file <path>           Read ZMDB from binary file\n";
    std::cout << "  device                Fetch ZMDB from connected USB device\n";
    std::cout << "  --output <path>       JSON output path (default: library.json)\n";
    std::cout << "  --device-type <type>  Override: Zune30 / Zune80 / ZuneHD\n";
    std::cout << "  --verbose             Enable verbose device logging\n";
    std::cout << "  --help                Show this help\n";
}

int main(int argc, char* argv[]) {
    std::cout << "========================================\n";
    std::cout << "   ZMDB Extractor Test (modern parser)\n";
    std::cout << "========================================\n";

    std::string source_mode = "device";
    std::string file_path;
    std::string output_file = "library.json";
    std::string device_type_override;
    bool verbose = false;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--help") { PrintUsage(argv[0]); return 0; }
        else if (arg == "file" && i + 1 < argc) { source_mode = "file"; file_path = argv[++i]; }
        else if (arg == "device") { source_mode = "device"; }
        else if (arg == "--output" && i + 1 < argc) { output_file = argv[++i]; }
        else if (arg == "--device-type" && i + 1 < argc) { device_type_override = argv[++i]; }
        else if (arg == "--verbose") { verbose = true; }
        else if (!arg.empty() && arg[0] != '-' && file_path.empty()) {
            source_mode = "file"; file_path = arg;
        } else {
            std::cerr << "Unknown option: " << arg << "\n";
            PrintUsage(argv[0]); return 1;
        }
    }

    mtp::ByteArray zmdb_data;
    zune::DeviceFamily family = zune::DeviceFamily::Unknown;

    if (source_mode == "file") {
        if (file_path.empty()) {
            std::cerr << "ERROR: no file path\n"; PrintUsage(argv[0]); return 1;
        }
        std::cout << "Reading ZMDB from: " << file_path << "\n";
        zmdb_data = ReadBinaryFile(file_path);
        if (zmdb_data.empty()) return 1;
        std::cout << "[OK] Loaded " << zmdb_data.size() << " bytes\n";
        family = device_type_override.empty()
            ? zune::DeviceFamily::Keel
            : ParseDeviceType(device_type_override);
    } else {
        std::cout << "Connecting to USB device...\n";
        ZuneDevice device;
        if (verbose) {
            device.SetLogCallback([](const std::string& m){ std::cout << "[DEVICE] " << m << "\n"; });
        }
        if (!device.ConnectUSB()) {
            std::cerr << "ERROR: Failed to connect to Zune device via USB\n";
            return 1;
        }
        std::cout << "[OK] Connected: " << device.GetName()
                  << " (" << zune::GetFamilyName(device.GetDeviceFamily()) << ")\n";
        family = device.GetDeviceFamily();
        std::vector<uint8_t> library_object_id = {0x03, 0x92, 0x1f};
        zmdb_data = device.GetZuneMetadata(library_object_id);
        if (zmdb_data.empty()) {
            std::cerr << "ERROR: Failed to fetch ZMDB\n";
            device.Disconnect(); return 1;
        }
        std::cout << "[OK] Retrieved " << zmdb_data.size() << " bytes\n";
        std::ofstream dump("/tmp/device_zmdb.bin", std::ios::binary);
        dump.write(reinterpret_cast<const char*>(zmdb_data.data()), zmdb_data.size());
        std::cout << "[OK] Saved raw ZMDB to /tmp/device_zmdb.bin\n";
        device.Disconnect();
    }

    if (!device_type_override.empty()) {
        family = ParseDeviceType(device_type_override);
    }
    std::cout << "Parsing as: " << zune::GetFamilyName(family) << "\n";

    auto parser = zmdb::ZMDBParserFactory::CreateParser(family);
    if (!parser) {
        std::cerr << "ERROR: no parser available for this device family\n"; return 1;
    }

    zmdb::ZMDBLibrary library;
    try {
        library = parser->ExtractLibrary(zmdb_data);
    } catch (const std::exception& e) {
        std::cerr << "ERROR during extraction: " << e.what() << "\n";
        return 1;
    }

    PrintSummary(library);

    std::ofstream out(output_file);
    if (!out.is_open()) {
        std::cerr << "ERROR: cannot open output file: " << output_file << "\n"; return 1;
    }
    WriteLibraryJson(library, out);
    std::cout << "\n[OK] Wrote JSON to " << output_file << "\n";
    return 0;
}
