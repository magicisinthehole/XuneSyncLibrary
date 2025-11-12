# Zune Wireless Native Library

**WARNING: This library is in early active development. Use of this library with your device may result in corruption of your Zune device's media library or other unknown issues. Always maintain backups of your media and use at your own risk.**

A C++17 native library for USB communication with Microsoft Zune devices. Provides MTP-based device control, fast library retrieval via ZMDB parsing, track uploading with metadata, and HTTP interception for artist metadata delivery.

## What This Library Does

- Connects to Zune devices over USB using the MTP protocol
- Establishes sync pairing (USB and wireless configuration)
- Retrieves complete music library in 3-10 seconds using ZMDB binary parser
- Uploads audio files with full metadata (artist, album, track info, artwork)
- Intercepts HTTP requests for artist metadata (biography, images) over a PPP network stack
- Manages artist MusicBrainz GUIDs to enable metadata fetching

The library implements a complete PPP/TCP/IP/HTTP stack to act as a network server over USB, responding to the device's metadata requests. This allows modern computers without Windows Zune software to provide artist information that the device displays during playback.

## Requirements

### All Platforms

- CMake 3.10 or higher
- C++17 compatible compiler
- OpenSSL development libraries
- libcurl development libraries
- TagLib development libraries

### macOS

```bash
brew install cmake openssl curl taglib
```

Platform frameworks (included with Xcode):
- IOKit
- CoreFoundation

### Linux

```bash
# Debian/Ubuntu
sudo apt-get install cmake build-essential libssl-dev libcurl4-openssl-dev libtag1-dev libusb-1.0-0-dev

# Fedora/RHEL
sudo dnf install cmake gcc-c++ openssl-devel libcurl-devel taglib-devel libusb-devel

# Arch Linux
sudo pacman -S cmake gcc openssl curl taglib libusb
```

### Windows

Windows is not currently supported. The Android File Transfer Linux (AFTL) dependency has a Windows backend, but it has not been integrated or tested with this library.

## Getting Started

### Clone the Repository

```bash
git clone https://github.com/magicisinthehole/XuneSyncLibrary.git
cd XuneSyncLibrary
git submodule update --init --recursive
```

The `--recursive` flag is required to initialize the AFTL submodule in `vendor/android-file-transfer-linux`.

### Build

The build system automatically compiles the AFTL library if it hasn't been built yet.

```bash
# Configure
cmake -S . -B build

# Build
cmake --build build
```

Build output:
- `build/libzune_wireless.dylib` (or `.so` on Linux) - Main shared library
- `build/test_*` - Test executables
- `build/artist_images_tool_cli` - Artist metadata tool

### Clean Rebuild

```bash
rm -rf build
cmake -S . -B build
cmake --build build
```

Do not run CMake commands inside the `vendor/android-file-transfer-linux` directory. The top-level build system manages the AFTL build lifecycle.

## Test Programs

All test programs are located in `build/` after building.

### test_library_json

Exports the complete music library to a JSON file.

```bash
./build/test_library_json [output.json]
```

Uses the traditional MTP iteration method (slow). Takes 30 seconds to 5+ minutes depending on library size.

### test_zmdb_fast

Tests fast library retrieval using the ZMDB binary parser.

```bash
./build/test_zmdb_fast
```

Extracts and parses the device's ZMDB file. Takes 3-10 seconds regardless of library size.

### test_zmdb_extractor

ZMDB parser that works with a connected device or a saved ZMDB binary file.

```bash
# From connected device
./build/test_zmdb_extractor

# From file
./build/test_zmdb_extractor /path/to/zmdb.bin
```

### test_http_interceptor

Full HTTP interception test with optional track upload.

```bash
# Proxy mode (forwards requests to external server)
./build/test_http_interceptor --mode proxy --server http://192.168.0.30

# Static mode (serves from local filesystem)
./build/test_http_interceptor --mode static --data-dir /path/to/artist_data

# With track upload
./build/test_http_interceptor --mode proxy --track "song.wma"

# Retrofit artist with MusicBrainz GUID
./build/test_http_interceptor --retrofit-artist "Artist Name" --retrofit-guid "uuid-here"
```

Options:
- `--mode proxy|static` - Interception mode
- `--server URL` - Proxy server URL (proxy mode)
- `--data-dir PATH` - Data directory (static mode)
- `--track PATH` - Upload test track to trigger metadata requests
- `--retrofit-artist NAME` - Artist to retrofit with GUID
- `--retrofit-guid UUID` - GUID to assign

### test_http_monitor_boot

Passive HTTP traffic monitoring without track upload.

```bash
./build/test_http_monitor_boot --mode proxy --server 192.168.0.30
```

Waits for device connection, initializes the HTTP subsystem, and monitors traffic.

### test_network_stack

Unit tests for PPP/IPCP/DNS/TCP parsers. Uses hardcoded packet data from captures.

```bash
./build/test_network_stack
```

No device required.

## Artist Images Tool

Interactive command-line tool for artist metadata operations.

```bash
./build/artist_images_tool_cli
```

### Menu Options

1. **Network Monitor** - Monitors HTTP requests from the device in real-time
2. **Upload Track** - Uploads an audio file with metadata extraction
3. **Retrofit Artist** - Adds a MusicBrainz GUID to an existing artist on the device
4. **Exit**

### How to Use

#### Network Monitor

Starts the HTTP interceptor and displays all requests from the device. Runs until you press Ctrl+C, then returns to the menu.

#### Upload Track

1. Select option 2 from the menu
2. Enter the path to an audio file (WMA, MP3, etc.)
3. The tool extracts metadata using TagLib
4. Optionally enter a MusicBrainz GUID for the artist
5. The track is uploaded to the device
6. The device automatically fetches artist metadata if a GUID was provided

#### Retrofit Artist

Use this to add a MusicBrainz GUID to an artist already on your device:

1. Select option 3 from the menu
2. The tool lists all artists currently on the device
3. Enter the artist number to retrofit
4. Enter the MusicBrainz GUID
5. The tool deletes the old artist entry and creates a new one with the GUID
6. All albums and tracks are automatically reassociated
7. The device fetches metadata for the artist

After retrofitting, you can use Network Monitor to observe the HTTP requests.

### Artist Metadata Directory Structure (Static Mode)

If using static mode, organize files as:

```
artist_data/
└── {musicbrainz-uuid}/
    ├── biography.xml
    ├── image_small.jpg
    ├── image_medium.jpg
    └── image_large.jpg
```

The device requests URLs like `/v3.0/en-US/music/artist/{uuid}/...`, and the static handler maps these to the filesystem.

## Library API

The library provides a C-compatible API for integration with other languages (.NET, Python, etc).

### Key Functions

**Device Management:**
- `zune_device_create()` / `zune_device_destroy()`
- `zune_device_connect_usb()` / `zune_device_disconnect()`

**Pairing:**
- `zune_device_establish_sync_pairing()` - Phase 1 (USB sync pairing)
- `zune_device_establish_wireless_pairing()` - Phase 2 (WiFi configuration)
- `zune_device_disable_wireless()`

**Library Access:**
- `zune_device_get_music_library()` - Slow MTP iteration
- `zune_device_get_music_library_fast()` - Fast ZMDB parsing
- `zune_device_free_music_library()`

**File Operations:**
- `zune_device_upload_track()` - Upload with full metadata
- `zune_device_download_file()` - Download files (e.g., artwork)
- `zune_device_delete_file()`
- `zune_device_get_partial_object()` - Streaming/range requests

**HTTP Interceptor:**
- `zune_device_start_artist_metadata_interceptor()`
- `zune_device_stop_artist_metadata_interceptor()`
- `zune_device_is_artist_metadata_interceptor_running()`

See `include/zune_wireless/zune_wireless_api.h` for complete API documentation.

## Architecture

### Core Components

**ZuneDevice** (`lib/src/ZuneDevice.h/cpp`)
High-level device interface. Manages MTP sessions, pairing, library access, and file operations.

**ZMDB Parser** (`lib/src/ZMDBLibraryExtractor.h/cpp`, `lib/src/ZMDBParser.h`)
Binary parser for Zune Media Database files. Extracts complete library metadata in seconds using an F-marker extraction algorithm.

**HTTP Interceptor** (`lib/src/protocols/http/ZuneHTTPInterceptor.h/cpp`)
Implements a complete network stack over USB:
- PPP/LCP/IPCP for link and IP configuration
- DNS server for hostname resolution
- TCP connection management with flow control
- HTTP request/response handling

**Protocol Parsers** (`lib/src/protocols/`)
- `ppp/PPPParser` - PPP frame and IP/TCP packet parsing
- `http/HTTPParser` - HTTP request/response parsing
- `http/StaticModeHandler` - Serves from local filesystem
- `http/ProxyModeHandler` - Forwards to external HTTP server

**SSDP Discovery** (`lib/src/ssdp_discovery.h/cpp`)
Listens for Zune device broadcasts on the local network. Not actively used (library focuses on USB).

**PTP/IP Client** (`lib/src/ptpip_client.h/cpp`)
TCP/IP-based MTP connection for wireless sync. Implemented but not actively tested.

### AFTL Integration

The library uses Android File Transfer Linux (AFTL) as a git submodule for MTP protocol support:

- Located in `vendor/android-file-transfer-linux`
- Built automatically by CMake into `build/vendor/android-file-transfer-linux-build/`
- Provides USB enumeration, MTP operations, and MTPZ encryption
- Configuration: Static library, MTPZ enabled, Qt/Python/FUSE disabled

## How HTTP Interception Works

The device's network stack expects a PPP connection over USB:

1. **USB Communication**: Uses MTP vendor commands 0x922c (send) and 0x922d (poll) to exchange data
2. **PPP Negotiation**: Establishes link control and negotiates IP addresses
3. **IP Configuration**: Device gets 192.168.55.101, host gets 192.168.55.100
4. **DNS Resolution**: Device queries catalog.zune.net, interceptor returns host IP
5. **TCP Connection**: Device connects on port 80
6. **HTTP Requests**: Device sends GET requests for artist metadata
7. **HTTP Responses**: Interceptor serves biography XML and JPEG images

The TCP implementation includes:
- Flow control (33580 byte window)
- Congestion control (slow start, congestion avoidance)
- Fast retransmit
- Proper checksum calculation

## Platform Notes

### macOS

Uses IOKit for USB communication. Tested on macOS 10.15+.

If the parent Xune project is detected at `../Xune/src/Xune.App`, the library is automatically copied to the app's runtime directories after building.

### Linux

Uses libusb for USB communication. Development and testing have focused on macOS, so Linux support may require additional work.

You may need udev rules for device access:

```bash
# Create /etc/udev/rules.d/51-zune.rules
SUBSYSTEM=="usb", ATTR{idVendor}=="045e", ATTR{idProduct}=="063e", MODE="0666"
```

Run `sudo udevadm control --reload-rules` after creating the file.

### Windows

Not supported. AFTL has Windows USB backend code, but integration with this library has not been attempted.

## Known Limitations

- Wireless sync (PTP/IP) is implemented but not actively tested
- Playlist parsing (.wpl files) is not implemented
- Creating/modifying playlists is not supported
- Video and podcast sync is not implemented
- Two-way wireless sync is not implemented

## License

This library is licensed under the GNU Lesser General Public License v2.1 (LGPL-2.1).

This means you can:
- Use this library in your own projects (commercial or non-commercial)
- Link against this library without your application being subject to LGPL
- Modify this library, but modifications to the library itself must be released under LGPL-2.1

See the LICENSE file for full details.

## Acknowledgments

This library builds on Android File Transfer Linux (AFTL) for MTP protocol support and uses TagLib for audio metadata extraction.
