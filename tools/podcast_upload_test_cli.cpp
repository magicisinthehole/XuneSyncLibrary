/**
 * Podcast Upload Test CLI — RSS-driven podcast upload following pcap-verified sequence
 *
 * Fetches an RSS feed, downloads the N latest episodes, and uploads them to a
 * connected Zune device using the exact MTP sequence observed in Zune Desktop
 * pcap captures (see redocs/podcast-upload-analysis.md).
 *
 * For video podcasts (enclosure type video), episodes are converted to WMV
 * via ffmpeg before upload. Audio podcasts (MP3) are uploaded directly.
 *
 * Usage:
 *   podcast_upload_test_cli <rss_url> [options]
 *
 *   -n <count>      Number of latest episodes to upload (default: 1)
 *   --audio-only    Skip video episodes
 *   --no-artwork    Skip artwork steps
 *   --no-convert    Skip video conversion (upload MP4 as-is, will likely fail)
 *   --cache-dir <d> Directory for downloaded episodes (default: /tmp/zune_podcasts)
 *   --verbose       Show detailed MTP logging
 *   --dry-run       Fetch feed and show plan without connecting to device
 *   --help          Show this help
 */

#include <iostream>
#include <fstream>
#include <string>
#include <vector>
#include <map>
#include <set>
#include <filesystem>
#include <cstring>
#include <iomanip>
#include <chrono>
#include <thread>
#include <sstream>
#include <algorithm>
#include <regex>
#include <ctime>
#include <cstdlib>

#include <curl/curl.h>

#include "lib/src/ZuneDevice.h"
#include "lib/src/ZuneDeviceIdentification.h"
#include "lib/src/ZuneMtpWriter.h"
#include <mtp/ptp/OutputStream.h>
#include <mtp/ptp/ByteArrayObjectStream.h>
#include <cli/PosixStreams.h>

namespace fs = std::filesystem;

static bool g_verbose = false;

// Format codes for podcast episodes (caller-determined based on file type)
static constexpr uint16_t FMT_MP3 = 0x3009;
static constexpr uint16_t FMT_WMV = 0xB981;

// ── Data Structures ─────────────────────────────────────────────────────

struct PodcastEpisode {
    std::string title;
    std::string url;            // enclosure URL
    std::string description;
    std::string pub_date;       // raw pubDate from RSS
    std::string date_authored;  // formatted for MTP: "YYYYMMDDTHHMMSS.0"
    std::string filename;       // sanitized filename for device
    std::string local_path;     // path after download (+ conversion for video)
    uint32_t duration_ms = 0;
    uint64_t file_size = 0;
    bool is_video = false;
    uint16_t format_code = FMT_MP3;
};

struct PodcastFeed {
    std::string title;
    std::string author;
    std::string description;
    std::string feed_url;       // original RSS URL
    std::string artwork_url;
    std::vector<PodcastEpisode> episodes;
    bool is_video = false;
};

// ── Logging ─────────────────────────────────────────────────────────────

void log_ts(const std::string& message) {
    auto now = std::chrono::system_clock::now();
    auto time = std::chrono::system_clock::to_time_t(now);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()) % 1000;
    std::cout << "[" << std::put_time(std::localtime(&time), "%H:%M:%S")
              << "." << std::setfill('0') << std::setw(3) << ms.count() << "] "
              << message << std::endl;
}

void log_phase(const std::string& name) {
    std::cout << std::endl;
    std::cout << "════════════════════════════════════════════════════════════" << std::endl;
    std::cout << "  " << name << std::endl;
    std::cout << "════════════════════════════════════════════════════════════" << std::endl;
}

void log_op(const std::string& desc) { log_ts("  " + desc); }
void log_ok(const std::string& desc) { log_ts("    [OK] " + desc); }
void log_warn(const std::string& desc) { log_ts("    [WARN] " + desc); }

std::string hex(uint32_t v) {
    std::ostringstream ss;
    ss << "0x" << std::hex << std::uppercase << std::setfill('0') << std::setw(8) << v;
    return ss.str();
}

std::string hex16(uint16_t v) {
    std::ostringstream ss;
    ss << "0x" << std::hex << std::uppercase << std::setfill('0') << std::setw(4) << v;
    return ss.str();
}

std::string format_size(uint64_t n) {
    std::ostringstream ss;
    if (n >= 1'000'000) ss << n << "B (" << std::fixed << std::setprecision(1) << (n / 1'000'000.0) << "MB)";
    else if (n >= 1'000) ss << n << "B (" << std::fixed << std::setprecision(1) << (n / 1'000.0) << "KB)";
    else ss << n << "B";
    return ss.str();
}

// ── CURL Helpers ────────────────────────────────────────────────────────

static size_t curl_write_string(void* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* str = static_cast<std::string*>(userdata);
    str->append(static_cast<char*>(ptr), size * nmemb);
    return size * nmemb;
}

static size_t curl_write_file(void* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* file = static_cast<std::ofstream*>(userdata);
    file->write(static_cast<char*>(ptr), static_cast<std::streamsize>(size * nmemb));
    return size * nmemb;
}

struct CurlProgress {
    std::string label;
    std::chrono::steady_clock::time_point last_print;
};

static int curl_progress_cb(void* clientp, curl_off_t dltotal, curl_off_t dlnow,
                             curl_off_t /*ultotal*/, curl_off_t /*ulnow*/) {
    auto* prog = static_cast<CurlProgress*>(clientp);
    auto now = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - prog->last_print);
    if (elapsed.count() >= 1000 && dltotal > 0) {
        int pct = static_cast<int>((dlnow * 100) / dltotal);
        double mb = dlnow / 1'000'000.0;
        std::cout << "\r  " << prog->label << ": " << pct << "% ("
                  << std::fixed << std::setprecision(1) << mb << " MB)" << std::flush;
        prog->last_print = now;
    }
    return 0;
}

std::string fetch_url(const std::string& url) {
    std::string body;
    CURL* curl = curl_easy_init();
    if (!curl) throw std::runtime_error("curl_easy_init failed");

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_string);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "Zune/4.8");

    CURLcode res = curl_easy_perform(curl);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK)
        throw std::runtime_error("curl fetch failed: " + std::string(curl_easy_strerror(res)));
    return body;
}

bool download_file(const std::string& url, const std::string& path, const std::string& label) {
    std::ofstream file(path, std::ios::binary);
    if (!file) return false;

    CURL* curl = curl_easy_init();
    if (!curl) return false;

    CurlProgress prog{label, std::chrono::steady_clock::now()};

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_file);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &file);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 600L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "Zune/4.8");
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, curl_progress_cb);
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &prog);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);

    CURLcode res = curl_easy_perform(curl);
    curl_easy_cleanup(curl);
    file.close();

    std::cout << std::endl;  // newline after progress

    if (res != CURLE_OK) {
        std::cerr << "  Download failed: " << curl_easy_strerror(res) << std::endl;
        fs::remove(path);
        return false;
    }
    return true;
}

std::vector<uint8_t> download_bytes(const std::string& url) {
    std::string body = fetch_url(url);
    return std::vector<uint8_t>(body.begin(), body.end());
}

// ── Minimal RSS Parser ─────────────────────────────────────────────────

/// Extract text content between <tag> and </tag> (first occurrence)
std::string xml_text(const std::string& xml, const std::string& tag,
                     size_t start = 0, size_t end = std::string::npos) {
    std::string open = "<" + tag;
    size_t search_end = (end == std::string::npos) ? xml.size() : end;
    size_t pos = xml.find(open, start);
    if (pos == std::string::npos || pos >= search_end) return "";

    // Find end of opening tag (handles attributes)
    size_t tag_end = xml.find('>', pos);
    if (tag_end == std::string::npos) return "";

    // Self-closing tag
    if (xml[tag_end - 1] == '/') return "";

    size_t content_start = tag_end + 1;
    std::string close = "</" + tag + ">";
    size_t content_end = xml.find(close, content_start);
    if (content_end == std::string::npos) return "";

    std::string text = xml.substr(content_start, content_end - content_start);

    // Trim leading whitespace before checking for CDATA
    size_t first_non_ws = text.find_first_not_of(" \t\n\r");
    if (first_non_ws != std::string::npos && first_non_ws > 0)
        text = text.substr(first_non_ws);

    // Strip CDATA wrapper
    if (text.size() > 12 && text.substr(0, 9) == "<![CDATA[") {
        text = text.substr(9);
        size_t cdata_end = text.rfind("]]>");
        if (cdata_end != std::string::npos)
            text = text.substr(0, cdata_end);
    }
    return text;
}

/// Extract attribute value from a tag
std::string xml_attr(const std::string& xml, const std::string& tag,
                     const std::string& attr, size_t start = 0, size_t end = std::string::npos) {
    std::string open = "<" + tag;
    size_t search_end = (end == std::string::npos) ? xml.size() : end;
    size_t pos = xml.find(open, start);
    if (pos == std::string::npos || pos >= search_end) return "";

    size_t tag_end = xml.find('>', pos);
    if (tag_end == std::string::npos) return "";

    std::string tag_content = xml.substr(pos, tag_end - pos + 1);
    std::string search = attr + "=\"";
    size_t attr_pos = tag_content.find(search);
    if (attr_pos == std::string::npos) return "";

    size_t val_start = attr_pos + search.size();
    size_t val_end = tag_content.find('"', val_start);
    if (val_end == std::string::npos) return "";

    return tag_content.substr(val_start, val_end - val_start);
}

/// Find all <item> regions in the XML
std::vector<std::pair<size_t, size_t>> find_items(const std::string& xml) {
    std::vector<std::pair<size_t, size_t>> items;
    size_t pos = 0;
    while (true) {
        size_t item_start = xml.find("<item>", pos);
        if (item_start == std::string::npos)
            item_start = xml.find("<item ", pos);
        if (item_start == std::string::npos) break;

        size_t item_end = xml.find("</item>", item_start);
        if (item_end == std::string::npos) break;
        item_end += 7;

        items.emplace_back(item_start, item_end);
        pos = item_end;
    }
    return items;
}

/// Parse "HH:MM:SS" or "MM:SS" or raw seconds into milliseconds
uint32_t parse_duration(const std::string& dur) {
    if (dur.empty()) return 0;

    // Try raw seconds
    bool all_digits = std::all_of(dur.begin(), dur.end(), ::isdigit);
    if (all_digits) return static_cast<uint32_t>(std::stoi(dur)) * 1000;

    // Parse HH:MM:SS or MM:SS
    std::vector<int> parts;
    std::istringstream ss(dur);
    std::string segment;
    while (std::getline(ss, segment, ':'))
        parts.push_back(std::stoi(segment));

    uint32_t secs = 0;
    if (parts.size() == 3) secs = parts[0] * 3600 + parts[1] * 60 + parts[2];
    else if (parts.size() == 2) secs = parts[0] * 60 + parts[1];
    else if (parts.size() == 1) secs = parts[0];
    return secs * 1000;
}

/// Parse RFC 2822 date to "YYYYMMDDTHHMMSS.0"
std::string parse_pub_date(const std::string& date_str) {
    if (date_str.empty()) return "20250101T000000.0";

    static const std::map<std::string, int> months = {
        {"Jan",1},{"Feb",2},{"Mar",3},{"Apr",4},{"May",5},{"Jun",6},
        {"Jul",7},{"Aug",8},{"Sep",9},{"Oct",10},{"Nov",11},{"Dec",12}
    };

    // Format: "Mon, 6 Apr 2026 04:05:00 +0000"
    std::istringstream ss(date_str);
    std::string dow, month_str;
    int day, year, hour, minute, second;
    char sep;

    ss >> dow;  // "Mon,"
    ss >> day >> month_str >> year >> hour >> sep >> minute >> sep >> second;

    int month = 1;
    auto it = months.find(month_str);
    if (it != months.end()) month = it->second;

    std::ostringstream out;
    out << std::setfill('0')
        << std::setw(4) << year
        << std::setw(2) << month
        << std::setw(2) << day
        << "T"
        << std::setw(2) << hour
        << std::setw(2) << minute
        << std::setw(2) << second
        << ".0";
    return out.str();
}

/// Decode XML entities
std::string xml_decode(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '&') {
            if (s.compare(i, 4, "&lt;") == 0) { out += '<'; i += 3; }
            else if (s.compare(i, 4, "&gt;") == 0) { out += '>'; i += 3; }
            else if (s.compare(i, 5, "&amp;") == 0) { out += '&'; i += 4; }
            else if (s.compare(i, 6, "&apos;") == 0) { out += '\''; i += 5; }
            else if (s.compare(i, 6, "&quot;") == 0) { out += '"'; i += 5; }
            else out += s[i];
        } else {
            out += s[i];
        }
    }
    return out;
}

/// Strip HTML tags from description
std::string strip_html(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    bool in_tag = false;
    for (char c : s) {
        if (c == '<') in_tag = true;
        else if (c == '>') in_tag = false;
        else if (!in_tag) out += c;
    }
    return out;
}

/// Sanitize filename for device (replace problematic characters)
std::string sanitize_filename(const std::string& name, const std::string& ext) {
    std::string safe;
    for (char c : name) {
        if (c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' ||
            c == '"' || c == '<' || c == '>' || c == '|')
            safe += ' ';
        else
            safe += c;
    }
    // Trim and limit length (MTP filename limit)
    while (!safe.empty() && safe.back() == ' ') safe.pop_back();
    while (!safe.empty() && safe.front() == ' ') safe.erase(safe.begin());
    if (safe.size() > 200) safe = safe.substr(0, 200);
    return safe + ext;
}

PodcastFeed parse_rss(const std::string& xml, const std::string& feed_url) {
    PodcastFeed feed;
    feed.feed_url = feed_url;

    // Channel-level metadata
    feed.title = xml_decode(xml_text(xml, "title"));
    feed.author = xml_decode(xml_text(xml, "itunes:author"));
    if (feed.author.empty())
        feed.author = xml_decode(xml_text(xml, "author"));
    feed.description = xml_decode(strip_html(xml_text(xml, "description")));

    // Artwork: prefer itunes:image href, fall back to image/url
    feed.artwork_url = xml_attr(xml, "itunes:image", "href");
    if (feed.artwork_url.empty())
        feed.artwork_url = xml_text(xml, "url");

    // Episodes
    auto items = find_items(xml);
    for (auto& [item_start, item_end] : items) {
        PodcastEpisode ep;
        ep.title = xml_decode(xml_text(xml, "title", item_start, item_end));
        ep.url = xml_attr(xml, "enclosure", "url", item_start, item_end);
        if (ep.url.empty()) continue;  // no enclosure = not downloadable

        ep.pub_date = xml_text(xml, "pubDate", item_start, item_end);
        ep.date_authored = parse_pub_date(ep.pub_date);

        std::string dur_str = xml_text(xml, "itunes:duration", item_start, item_end);
        ep.duration_ms = parse_duration(dur_str);

        // Description: try itunes:summary first, then description
        ep.description = xml_decode(strip_html(
            xml_text(xml, "itunes:summary", item_start, item_end)));
        if (ep.description.empty())
            ep.description = xml_decode(strip_html(
                xml_text(xml, "description", item_start, item_end)));

        // Detect video from enclosure type
        std::string enc_type = xml_attr(xml, "enclosure", "type", item_start, item_end);
        ep.is_video = (enc_type.find("video") != std::string::npos);
        if (!ep.is_video) {
            // Also check media:content
            std::string media_type = xml_attr(xml, "media:content", "type", item_start, item_end);
            if (media_type.find("video") != std::string::npos) ep.is_video = true;
        }

        if (ep.is_video) {
            feed.is_video = true;
            ep.format_code = FMT_WMV;
            ep.filename = sanitize_filename(ep.title, ".wmv");
        } else {
            ep.format_code = FMT_MP3;
            ep.filename = sanitize_filename(ep.title, ".mp3");
        }

        // File size from enclosure length attribute (may be empty for video feeds)
        std::string enc_len = xml_attr(xml, "enclosure", "length", item_start, item_end);
        if (!enc_len.empty()) {
            try { ep.file_size = std::stoull(enc_len); } catch (...) {}
        }

        feed.episodes.push_back(std::move(ep));
    }

    return feed;
}

// ── Video Conversion ────────────────────────────────────────────────────

/// Convert video to WMV using ffmpeg. Returns path to converted file.
std::string convert_to_wmv(const std::string& input_path, const std::string& output_dir) {
    std::string stem = fs::path(input_path).stem().string();
    std::string output_path = output_dir + "/" + stem + ".wmv";

    if (fs::exists(output_path)) {
        std::cout << "  Using cached WMV: " << output_path << std::endl;
        return output_path;
    }

    // WMV encoding matching Zune Desktop ASF layout:
    // Audio stream first (-map 0:a -map 0:v), 320x180, ≤30fps, 48kHz audio.
    std::string cmd = "ffmpeg -i \"" + input_path + "\" "
        "-map 0:a:0 -map 0:v:0 "
        "-c:v wmv2 -b:v 736k -r 25 "
        "-vf \"scale=320:180:force_original_aspect_ratio=decrease,pad=320:180:(ow-iw)/2:(oh-ih)/2\" "
        "-c:a wmav2 -b:a 128k -ar 48000 -ac 2 "
        "-y \"" + output_path + "\" 2>&1";

    std::cout << "  Converting to WMV..." << std::endl;
    if (g_verbose) std::cout << "    " << cmd << std::endl;

    int ret = std::system(cmd.c_str());
    if (ret != 0 || !fs::exists(output_path)) {
        std::cerr << "  ERROR: ffmpeg conversion failed (exit code " << ret << ")" << std::endl;
        std::cerr << "  Ensure ffmpeg is installed: brew install ffmpeg" << std::endl;
        return "";
    }

    std::cout << "  Converted: " << format_size(fs::file_size(output_path)) << std::endl;
    return output_path;
}

// ── MTP Helpers ─────────────────────────────────────────────────────────

using SessionPtr = std::shared_ptr<mtp::Session>;

/// Read the SeriesHandle (0xDA9E) from an existing episode via GetObjectProperty.
/// The .ser objects are not enumerable through GetObjectHandles (format 0xBA0B),
/// so we discover the handle by reading it from an episode that references it.
uint32_t find_series_handle_from_episode(
    const SessionPtr& session,
    uint32_t storageId, uint32_t episode_folder_id) {

    std::vector<mtp::ObjectId> handles;
    try {
        auto result = session->GetObjectHandles(
            mtp::StorageId(storageId), mtp::ObjectFormat::Any, mtp::ObjectId(episode_folder_id));
        handles = result.ObjectHandles;
    } catch (...) {}

    for (auto& h : handles) {
        try {
            auto data = session->GetObjectProperty(
                h, mtp::ObjectProperty(zune::MtpProp::SeriesHandle));
            if (data.size() >= 4) {
                uint32_t handle = *reinterpret_cast<const uint32_t*>(data.data());
                if (handle != 0) {
                    log_ok("SeriesHandle from episode " + hex(h.Id) + " = " + hex(handle));
                    return handle;
                }
            }
        } catch (...) {}
    }

    return 0;
}

// ── Main ────────────────────────────────────────────────────────────────

void print_usage(const char* prog) {
    std::cout << "Podcast Upload Test CLI — RSS-driven podcast upload (pcap-verified sequence)" << std::endl;
    std::cout << std::endl;
    std::cout << "Usage: " << prog << " <rss_url> [options]" << std::endl;
    std::cout << std::endl;
    std::cout << "Options:" << std::endl;
    std::cout << "  -n <count>      Number of latest episodes (default: 1)" << std::endl;
    std::cout << "  --skip <count>  Skip the N most recent episodes" << std::endl;
    std::cout << "  --audio-only    Skip video episodes" << std::endl;
    std::cout << "  --no-artwork    Skip artwork steps" << std::endl;
    std::cout << "  --no-convert    Skip video conversion (upload source format as-is)" << std::endl;
    std::cout << "  --cache-dir <d> Download directory (default: /tmp/zune_podcasts)" << std::endl;
    std::cout << "  --verbose       Show detailed MTP logging" << std::endl;
    std::cout << "  --dry-run       Fetch feed + download, no device connection" << std::endl;
    std::cout << "  --help          Show this help" << std::endl;
}

int main(int argc, char* argv[]) {
    if (argc < 2) { print_usage(argv[0]); return 1; }

    std::string rss_url;
    int episode_count = 1;
    int skip_count = 0;
    bool audio_only = false;
    bool no_artwork = false;
    bool no_convert = false;
    bool dry_run = false;
    std::string cache_dir = "/tmp/zune_podcasts";

    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        if (arg == "--help") { print_usage(argv[0]); return 0; }
        else if (arg == "--verbose") g_verbose = true;
        else if (arg == "--dry-run") dry_run = true;
        else if (arg == "--audio-only") audio_only = true;
        else if (arg == "--no-artwork") no_artwork = true;
        else if (arg == "--no-convert") no_convert = true;
        else if (arg == "-n" && i + 1 < argc) episode_count = std::stoi(argv[++i]);
        else if (arg == "--skip" && i + 1 < argc) skip_count = std::stoi(argv[++i]);
        else if (arg == "--cache-dir" && i + 1 < argc) cache_dir = argv[++i];
        else if (arg[0] != '-') rss_url = arg;
    }

    if (rss_url.empty()) {
        std::cerr << "ERROR: No RSS URL provided" << std::endl;
        return 1;
    }

    curl_global_init(CURL_GLOBAL_DEFAULT);

    // ═══════════════════════════════════════════════════════════════════
    // Phase 1: Fetch & Parse RSS Feed
    // ═══════════════════════════════════════════════════════════════════

    log_phase("Fetch RSS Feed");
    std::cout << "  URL: " << rss_url << std::endl;

    std::string rss_xml;
    try {
        rss_xml = fetch_url(rss_url);
    } catch (const std::exception& e) {
        std::cerr << "ERROR: Failed to fetch feed: " << e.what() << std::endl;
        curl_global_cleanup();
        return 1;
    }
    std::cout << "  Feed size: " << format_size(rss_xml.size()) << std::endl;

    auto feed = parse_rss(rss_xml, rss_url);
    std::cout << "  Title: " << feed.title << std::endl;
    std::cout << "  Author: " << feed.author << std::endl;
    std::cout << "  Episodes in feed: " << feed.episodes.size() << std::endl;
    std::cout << "  Type: " << (feed.is_video ? "Video" : "Audio") << std::endl;
    if (!feed.artwork_url.empty())
        std::cout << "  Artwork: " << feed.artwork_url.substr(0, 80) << "..." << std::endl;

    // Filter and limit episodes
    std::vector<PodcastEpisode*> selected;
    int skipped = 0;
    for (auto& ep : feed.episodes) {
        if (audio_only && ep.is_video) continue;
        if (skipped < skip_count) { ++skipped; continue; }
        selected.push_back(&ep);
        if (static_cast<int>(selected.size()) >= episode_count) break;
    }

    if (selected.empty()) {
        std::cerr << "ERROR: No episodes match criteria" << std::endl;
        curl_global_cleanup();
        return 1;
    }

    std::cout << std::endl;
    std::cout << "  Selected " << selected.size() << " episode(s):" << std::endl;
    for (size_t i = 0; i < selected.size(); ++i) {
        auto& ep = *selected[i];
        std::cout << "    " << (i + 1) << ". " << ep.title << std::endl;
        std::cout << "       " << (ep.is_video ? "VIDEO" : "AUDIO")
                  << "  " << (ep.duration_ms / 1000) << "s"
                  << "  " << ep.pub_date << std::endl;
    }

    // ═══════════════════════════════════════════════════════════════════
    // Phase 2: Download Episodes
    // ═══════════════════════════════════════════════════════════════════

    log_phase("Download Episodes");

    fs::create_directories(cache_dir);
    std::string series_dir = cache_dir + "/" + sanitize_filename(feed.title, "");
    // Remove trailing space from sanitize_filename with empty ext
    while (!series_dir.empty() && series_dir.back() == ' ') series_dir.pop_back();
    fs::create_directories(series_dir);

    for (auto* ep : selected) {
        // Determine download extension from URL or type
        std::string dl_ext;
        if (ep->is_video) {
            // Extract extension from URL
            std::string url_path = ep->url.substr(0, ep->url.find('?'));
            dl_ext = fs::path(url_path).extension().string();
            if (dl_ext.empty()) dl_ext = ".mp4";
        } else {
            dl_ext = ".mp3";
        }

        std::string dl_filename = sanitize_filename(ep->title, dl_ext);
        std::string dl_path = series_dir + "/" + dl_filename;

        if (fs::exists(dl_path) && fs::file_size(dl_path) > 0) {
            std::cout << "  Cached: " << dl_filename << " ("
                      << format_size(fs::file_size(dl_path)) << ")" << std::endl;
        } else {
            std::cout << "  Downloading: " << ep->title << std::endl;
            if (!download_file(ep->url, dl_path, ep->title.substr(0, 40))) {
                std::cerr << "  ERROR: Failed to download " << ep->title << std::endl;
                curl_global_cleanup();
                return 1;
            }
        }

        // Convert video if needed
        if (ep->is_video && !no_convert) {
            std::string wmv_path = convert_to_wmv(dl_path, series_dir);
            if (wmv_path.empty()) {
                std::cerr << "  ERROR: Video conversion failed for " << ep->title << std::endl;
                curl_global_cleanup();
                return 1;
            }
            ep->local_path = wmv_path;
        } else {
            ep->local_path = dl_path;
        }

        ep->file_size = fs::file_size(ep->local_path);
        ep->filename = fs::path(ep->local_path).filename().string();

        std::cout << "  Ready: " << ep->filename << " (" << format_size(ep->file_size) << ")" << std::endl;
    }

    // Download artwork, resize to 300x300 max for device compatibility
    std::vector<uint8_t> artwork;
    if (!no_artwork && !feed.artwork_url.empty()) {
        std::cout << std::endl << "  Downloading artwork..." << std::endl;
        std::string art_orig = series_dir + "/artwork_orig";
        std::string art_resized = series_dir + "/artwork.jpg";

        try {
            if (fs::exists(art_resized) && fs::file_size(art_resized) > 0) {
                std::cout << "  Using cached artwork" << std::endl;
            } else {
                // Determine extension from URL
                std::string url_path = feed.artwork_url.substr(0, feed.artwork_url.find('?'));
                std::string ext = fs::path(url_path).extension().string();
                if (ext.empty()) ext = ".jpg";
                art_orig += ext;

                if (!download_file(feed.artwork_url, art_orig, "artwork"))
                    throw std::runtime_error("download failed");

                // Resize to 300x300 with ffmpeg, output as JPEG
                std::string cmd = "ffmpeg -i \"" + art_orig + "\" "
                    "-vf \"scale='min(300,iw)':'min(300,ih)':force_original_aspect_ratio=decrease\" "
                    "-q:v 5 -y \"" + art_resized + "\" 2>&1";
                if (g_verbose) std::cout << "  " << cmd << std::endl;
                if (std::system(cmd.c_str()) != 0)
                    throw std::runtime_error("ffmpeg resize failed");
            }

            std::ifstream f(art_resized, std::ios::binary | std::ios::ate);
            auto size = f.tellg();
            f.seekg(0);
            artwork.resize(static_cast<size_t>(size));
            f.read(reinterpret_cast<char*>(artwork.data()), size);
            std::cout << "  Artwork: " << format_size(artwork.size()) << std::endl;
        } catch (const std::exception& e) {
            std::cout << "  Artwork failed: " << e.what() << std::endl;
        }
    }

    if (dry_run) {
        std::cout << std::endl << "Dry run — not connecting to device." << std::endl;
        curl_global_cleanup();
        return 0;
    }

    // ═══════════════════════════════════════════════════════════════════
    // Phase 3: Connect to Device
    // ═══════════════════════════════════════════════════════════════════

    log_phase("Connect to Device");

    ZuneDevice device;
    device.SetLogCallback([](const std::string& msg) {
        if (g_verbose) log_ts("  [device] " + msg);
    });

    if (!device.ConnectUSB()) {
        std::cerr << "ERROR: Failed to connect to Zune device" << std::endl;
        curl_global_cleanup();
        return 1;
    }

    auto session = device.GetMtpSession();
    if (!session) {
        std::cerr << "ERROR: No MTP session" << std::endl;
        curl_global_cleanup();
        return 1;
    }

    auto start_time = std::chrono::steady_clock::now();
    uint32_t storage_raw = device.GetDefaultStorageId();

    auto family = device.GetDeviceFamily();
    bool is_hd = (family == zune::DeviceFamily::Pavo);

    std::cout << "  Device: " << device.GetName()
              << " (" << device.GetDeviceFamilyName() << ")"
              << (is_hd ? " [HD]" : " [Classic]") << std::endl;

    // ═══════════════════════════════════════════════════════════════════
    // Phase 4: Pre-Upload Discovery (matches pcap)
    // ═══════════════════════════════════════════════════════════════════

    log_phase("Pre-Upload Discovery");

    for (int i = 1; i <= 2; ++i) {
        log_op("GetDevicePropValue (0xD217) #" + std::to_string(i));
        try { session->GetDeviceProperty(mtp::DeviceProperty(0xD217)); }
        catch (...) {}
    }

    log_op("SyncDeviceDB (0x9217)");
    try { session->Operation9217(1); log_ok("SyncDeviceDB complete"); }
    catch (const std::exception& e) { log_warn("SyncDeviceDB: " + std::string(e.what())); }

    log_op("SetSessionGUID — done during ConnectUSB");

    // Discover root folders via MtpWriter
    log_op("DiscoverRoot");
    auto root = zune::MtpWriter::DiscoverRoot(session, storage_raw, is_hd);
    if (root.series_folder) log_ok("Series folder: " + hex(root.series_folder));
    if (root.podcasts_folder) log_ok("Podcasts folder: " + hex(root.podcasts_folder));

    bool first_folder_created = false;

    // Detect existing state:
    //   1. Nothing exists → full fresh (create Series/, .ser, Podcasts/<series>/)
    //   2. Subfolder exists, no .ser → create .ser + artwork, reuse subfolder
    //   3. Both exist → pure addition (RegisterTrackCtx → upload)
    uint32_t series_folder_id = root.series_folder;
    uint32_t podcasts_folder_id = root.podcasts_folder;
    uint32_t episode_folder_id = 0;
    uint32_t series_obj_id = 0;
    bool episode_folder_exists = false;
    bool series_obj_exists = false;

    if (podcasts_folder_id != 0) {
        auto podcasts_children = zune::MtpWriter::DiscoverFolderChildren(
            session, storage_raw, podcasts_folder_id);
        log_ok("Podcasts has " + std::to_string(podcasts_children.size()) + " children");
        for (auto& child : podcasts_children) {
            if (g_verbose) log_ok("  Child: \"" + child.name + "\" = " + hex(child.handle));
            if (child.name == feed.title) {
                episode_folder_exists = true;
                episode_folder_id = child.handle;
                log_ok("Existing episode folder: " + hex(episode_folder_id));
                break;
            }
        }
    }

    // .ser objects are NOT enumerable — read SeriesHandle from an existing episode
    if (episode_folder_exists) {
        series_obj_id = find_series_handle_from_episode(
            session, storage_raw, episode_folder_id);
        if (series_obj_id != 0)
            series_obj_exists = true;
    }

    if (episode_folder_exists && series_obj_exists) {
        // ═══════════════════════════════════════════════════════════════
        // Pure Addition (pcap steps 56-67): both .ser and subfolder exist
        // ═══════════════════════════════════════════════════════════════

        log_phase("Addition Mode — Series Already on Device");

        log_op("RegisterTrackCtx (0x922A) — \"" + feed.title + "\"");
        zune::MtpWriter::RegisterTrackContext(session, feed.title);
        log_ok("Registered");

        zune::MtpWriter::RootReEnum(session);

        zune::MtpWriter::DiscoverFolderChildren(session, storage_raw, episode_folder_id);

    } else {
        // ═══════════════════════════════════════════════════════════════
        // Fresh Flow: first upload for this series (pcap-verified)
        // ═══════════════════════════════════════════════════════════════

        log_phase("Fresh Upload — Creating Series");

        zune::MtpWriter::QueryFolderDescriptors(session);

        // Create "Series" root folder if needed
        if (series_folder_id != 0) {
            log_ok("Series folder exists: " + hex(series_folder_id));
        } else {
            series_folder_id = zune::MtpWriter::CreateFolder(
                session, storage_raw, 0, "Series");
            log_ok("Created Series folder: " + hex(series_folder_id));
            zune::MtpWriter::FirstFolderReadback(
                session, series_folder_id, storage_raw, is_hd);
            first_folder_created = true;
        }

        try { session->GetObjectHandles(
            mtp::StorageId(storage_raw), mtp::ObjectFormat::Any,
            mtp::ObjectId(series_folder_id)); } catch (...) {}

        // Query property descriptors for Series format (0xBA0B)
        log_op("QuerySeriesDescriptors");
        zune::MtpWriter::QuerySeriesDescriptors(session);

        // Create series metadata object via MtpWriter
        zune::PodcastSeriesProperties series_props;
        series_props.name = feed.title;
        series_props.artist = feed.author;
        series_props.feed_url = feed.feed_url;
        series_props.filename = feed.title + ".ser";

        log_op("CreatePodcastSeries \"" + series_props.filename + "\"");
        series_obj_id = zune::MtpWriter::CreatePodcastSeries(
            session, storage_raw, series_folder_id, series_props);
        log_ok("Series created: " + hex(series_obj_id));

        // Series artwork via MtpWriter
        if (!artwork.empty()) {
            log_op("SetSeriesArtwork (" + std::to_string(artwork.size()) + " bytes)");
            try {
                zune::MtpWriter::SetSeriesArtwork(
                    session, series_obj_id, artwork.data(), artwork.size());
                log_ok("Artwork set");
            } catch (const std::exception& e) {
                log_warn("Artwork failed: " + std::string(e.what()));
            }
        }

        // RegisterTrackCtx (between series creation and episode folder)
        log_op("RegisterTrackCtx (0x922A) — \"" + feed.title + "\"");
        zune::MtpWriter::RegisterTrackContext(session, feed.title);
        log_ok("Registered");

        // Re-enumerate root after Series creation
        zune::MtpWriter::RootReEnum(session);

        // Create "Podcasts" root folder if needed
        if (podcasts_folder_id == 0) {
            // Re-discover after root re-enum to pick up newly created folders
            auto updated_root = zune::MtpWriter::DiscoverRoot(session, storage_raw, is_hd);
            podcasts_folder_id = updated_root.podcasts_folder;
        }
        if (podcasts_folder_id == 0) {
            podcasts_folder_id = zune::MtpWriter::CreateFolder(
                session, storage_raw, 0, "Podcasts");
            log_ok("Created Podcasts folder: " + hex(podcasts_folder_id));
            if (!first_folder_created) {
                zune::MtpWriter::FirstFolderReadback(
                    session, podcasts_folder_id, storage_raw, is_hd);
                first_folder_created = true;
            } else {
                zune::MtpWriter::FolderReadback(
                    session, podcasts_folder_id, storage_raw);
            }
        } else {
            log_ok("Podcasts folder exists: " + hex(podcasts_folder_id));
        }

        // Discover or create per-series subfolder under Podcasts
        if (episode_folder_exists) {
            log_ok("Reusing existing episode folder: " + hex(episode_folder_id));
            zune::MtpWriter::DiscoverFolderChildren(session, storage_raw, episode_folder_id);
        } else {
            episode_folder_id = zune::MtpWriter::CreateFolder(
                session, storage_raw, podcasts_folder_id, feed.title);
            log_ok("Created episode folder: " + hex(episode_folder_id));
            zune::MtpWriter::FolderReadback(session, episode_folder_id, storage_raw);
        }
    }

    // ═══════════════════════════════════════════════════════════════════
    // Phase 8: Upload Episodes
    // ═══════════════════════════════════════════════════════════════════

    log_phase("Upload Episodes");

    bool episode_desc_done = false;
    std::vector<std::pair<std::string, uint32_t>> uploaded;

    for (size_t ei = 0; ei < selected.size(); ++ei) {
        auto& ep = *selected[ei];

        log_phase("Episode " + std::to_string(ei + 1) + "/" +
                  std::to_string(selected.size()) + ": " + ep.title);

        // Episode property descriptors (first episode per format only)
        if (!episode_desc_done) {
            log_op("QueryEpisodeDescriptors for fmt=" + hex16(ep.format_code));
            zune::MtpWriter::QueryEpisodeDescriptors(session, ep.format_code);
            episode_desc_done = true;
        }

        // Build episode properties and create via MtpWriter
        zune::PodcastEpisodeProperties ep_props;
        ep_props.title = ep.title;
        ep_props.artist = feed.author;
        ep_props.series_name = feed.title;
        ep_props.date_authored = ep.date_authored;
        ep_props.description = ep.description;
        ep_props.source_url = ep.url;
        ep_props.filename = ep.filename;
        ep_props.duration_ms = ep.duration_ms;
        ep_props.series_handle = series_obj_id;
        ep_props.format_code = ep.format_code;
        ep_props.is_video = ep.is_video;

        log_op("CreatePodcastEpisode \"" + ep.filename +
               "\" (" + format_size(ep.file_size) + ")");
        uint32_t episode_id = zune::MtpWriter::CreatePodcastEpisode(
            session, storage_raw, episode_folder_id, ep_props, ep.file_size);
        log_ok("Episode object created: " + hex(episode_id));

        // SendObject (episode file data)
        log_op("SendObject — " + format_size(ep.file_size));
        zune::MtpWriter::UploadAudioData(session, ep.local_path);
        log_ok("Episode data uploaded");

        // Post-upload verification
        log_op("VerifyTrack (readback)");
        zune::MtpWriter::VerifyTrack(session, episode_id);

        log_op("GetObjReferences");
        try { session->GetObjectReferences(mtp::ObjectId(episode_id)); } catch (...) {}

        uploaded.emplace_back(ep.title, episode_id);
    }

    // ═══════════════════════════════════════════════════════════════════
    // Phase 9: Finalize (pcap Phase 7)
    // ═══════════════════════════════════════════════════════════════════

    log_phase("Finalize");

    log_op("DisableTrustedFiles (0x9215)");
    try { session->Operation9215(); log_ok("Trusted files disabled"); }
    catch (const std::exception& e) { log_warn("DisableTrustedFiles: " + std::string(e.what())); }

    log_op("Op922b(3,1,0) — open idle session");
    try { session->Operation922b(3, 1, 0); log_ok("Session opened"); }
    catch (const std::exception& e) { log_warn("Session open: " + std::string(e.what())); }

    log_op("Op9230(1) — begin idle monitoring");
    try { session->Operation9230(1); log_ok("Idle monitoring started"); }
    catch (const std::exception& e) { log_warn("Op9230: " + std::string(e.what())); }

    // ═══════════════════════════════════════════════════════════════════
    // Results
    // ═══════════════════════════════════════════════════════════════════

    auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start_time).count();

    std::cout << std::endl;
    std::cout << "╔══════════════════════════════════════════════════════════╗" << std::endl;
    std::cout << "║  Podcast Upload Complete                                ║" << std::endl;
    std::cout << "╚══════════════════════════════════════════════════════════╝" << std::endl;
    std::cout << std::endl;
    std::cout << "  Device:   " << device.GetName() << " (" << device.GetDeviceFamilyName() << ")" << std::endl;
    std::cout << "  Series:   " << feed.title << " (" + hex(series_obj_id) << ")" << std::endl;
    std::cout << "  Episodes: " << uploaded.size() << std::endl;
    std::cout << "  Elapsed:  " << elapsed_ms << " ms" << std::endl;
    std::cout << std::endl;

    for (auto& [title, id] : uploaded) {
        std::cout << "  " << title << "  episode=" << hex(id) << std::endl;
    }

    // Cleanup
    std::cout << std::endl << "Press Enter to disconnect..." << std::endl;
    std::cin.get();

    try { session->Operation922b(3, 2, 0); } catch (...) {}
    device.Disconnect();
    std::cout << "Disconnected." << std::endl;

    curl_global_cleanup();
    return 0;
}
