#include "ZuneDevice.h"
#include "ptpip_client.h"
#include "ssdp_discovery.h"
#include "protocols/http/ZuneHTTPInterceptor.h"
#include "protocols/ppp/PPPParser.h"
#include <iostream>
#include <fstream>
#include <sstream>
#include <iomanip>
#include <memory>
#include <algorithm>
#include <thread>
#include <chrono>
#include <cstring>
#include <cctype>
#include <regex>
#include <mtp/metadata/Metadata.h>
#include <mtp/metadata/Library.h>
#include <mtp/ptp/ObjectPropertyListParser.h>
#include <cli/PosixStreams.h>
#include <unordered_map>
#include "zmdb/ZMDBParserFactory.h"

using namespace mtp;

// === Helper Functions ===

// Validate MusicBrainz GUID format
static bool IsValidGuid(const std::string& guid) {
    // MusicBrainz GUID format: xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx (case-insensitive)
    static const std::regex guid_pattern(
        "^[0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{12}$"
    );
    return std::regex_match(guid, guid_pattern);
}

// --- Embedded Phase 1 Property Data ---
static const uint8_t prop_d230_data[] = { 0x7a, 0x24, 0xec, 0x12, 0x00, 0x00, 0x00, 0x00 };
static const uint8_t prop_d229_data[] = { 0xf4, 0x00, 0x00, 0x00 };
static const uint8_t prop_d22a_data[] = { 0x5c, 0xfe, 0xff, 0xff };

// --- Zune Metadata Request Protocol Constants ---
constexpr size_t ZMDB_REQUEST_SIZE = 16;
constexpr uint8_t ZMDB_REQUEST_LENGTH_BYTE = 0x10;
constexpr uint8_t ZMDB_COMMAND_MARKER = 0x01;
constexpr uint8_t ZMDB_OPERATION_CODE_HIGH = 0x17;
constexpr uint8_t ZMDB_OPERATION_CODE_LOW = 0x92;
constexpr uint8_t ZMDB_OBJECT_ID_SIZE = 3;
constexpr uint8_t ZMDB_TRAILER_VALUE = 0x01;
constexpr size_t ZMDB_HEADER_SIZE = 12;
constexpr int ZMDB_DEVICE_PREPARE_DELAY_MS = 250;
constexpr int ZMDB_PIPE_DRAIN_TIMEOUT_MS = 100;

// --- Helper classes for bulk data streaming ---
class ByteArrayInputStream : public mtp::IObjectInputStream {
private:
    const mtp::ByteArray& _data;
    size_t _offset;
public:
    ByteArrayInputStream(const mtp::ByteArray& data) : _data(data), _offset(0) {}
    mtp::u64 GetSize() const override { return _data.size(); }
    size_t Read(mtp::u8 *data, size_t size) override {
        size_t remaining = _data.size() - _offset;
        size_t to_read = std::min(size, remaining);
        if (to_read > 0) {
            std::memcpy(data, _data.data() + _offset, to_read);
            _offset += to_read;
        }
        return to_read;
    }
    void Cancel() override {}
};

class ByteArrayOutputStream : public mtp::IObjectOutputStream {
public:
    mtp::ByteArray data;
    size_t Write(const uint8_t *buffer, size_t size) override {
        if (buffer && size > 0) {
            data.insert(data.end(), buffer, buffer + size);
        }
        return size;
    }
    void Cancel() override {
    }
};

ZuneDevice::ZuneDevice()
    : guid_file_(".mac-zune-guid"), device_guid_file_(".device-zune-guid") {
}

ZuneDevice::~ZuneDevice() {
    Disconnect();
}

bool ZuneDevice::ConnectUSB() {
    try {
        Log("Connecting to Zune device via USB...");
        usb_context_ = std::make_shared<usb::Context>();

        // Find Zune device and store descriptor for later product ID lookup
        auto devices = usb_context_->GetDevices();
        for (auto desc : devices) {
            if (desc->GetVendorId() == 0x045E) {  // Microsoft vendor ID
                try {
                    device_ = Device::Open(usb_context_, desc, true, false);
                    if (device_) {
                        usb_descriptor_ = desc;
                        break;
                    }
                } catch (const std::exception& e) {
                    Log("Failed to open device: " + std::string(e.what()));
                }
            }
        }

        if (!device_) {
            Log("Error: No MTP device found on USB");
            return false;
        }
        Log("  ✓ Device found");

        Log("Opening MTP session...");
        mtp_session_ = device_->OpenSession(1);
        if (!mtp_session_) {
            Log("Error: Failed to open MTP session");
            return false;
        }
        Log("  ✓ Session opened");

        // NOTE: Do NOT call Operation1002 here - HTTP init happens later via InitializeHTTPSubsystem()
        // The Windows Zune software does NOT call Operation1002 during initial connection

        Log("Initializing MTPZ authentication...");
        cli_session_ = std::make_shared<cli::Session>(mtp_session_, false);
        Log("  ✓ MTPZ authentication complete");

        // NOTE: Do NOT scan library here - Windows Zune doesn't do this during connect
        // Library scanning might interfere with the device's autonomous metadata fetching
        // Library will be initialized lazily when needed for uploads

        return true;
    } catch (const std::exception& e) {
        Log("Error connecting to USB device: " + std::string(e.what()));
        return false;
    }
}

bool ZuneDevice::ConnectWireless(const std::string& ip_address) {
    // Note: This is a placeholder for the actual wireless connection logic
    // which would use the ptpip_client. 
    Log("Wireless connection is not yet implemented.");
    return false;
}

void ZuneDevice::Disconnect() {
    // Clear track ObjectId cache to prevent stale data on reconnection
    ClearTrackObjectIdCache();

    if (library_) {
        library_.reset();
    }
    if (cli_session_) {
        cli_session_.reset();
    }
    if (mtp_session_) {
        mtp_session_.reset();
    }
    if (device_) {
        device_.reset();
    }
    if (usb_descriptor_) {
        usb_descriptor_.reset();
    }
    if (usb_context_) {
        usb_context_.reset();
    }
    Log("Device disconnected.");
}

bool ZuneDevice::IsConnected() {
    return mtp_session_ != nullptr;
}

void ZuneDevice::SetLogCallback(LogCallback callback) {
    log_callback_ = callback;
}

void ZuneDevice::Log(const std::string& message) {
    if (log_callback_) {
        log_callback_(message);
    }
}

void ZuneDevice::VerboseLog(const std::string& message) {
    if (verbose_logging_ && log_callback_) {
        log_callback_(message);
    }
}

bool ZuneDevice::LoadMacGuid() {
    std::ifstream file(guid_file_);
    if (!file.is_open()) {
        Log("Error: Could not open " + guid_file_);
        return false;
    }
    std::getline(file, mac_guid_);
    mac_guid_.erase(mac_guid_.find_last_not_of(" \n\r\t") + 1);
    return !mac_guid_.empty();
}

std::string ZuneDevice::Utf16leToAscii(const ByteArray& data, bool is_guid) {
    std::string result;
    if (data.size() < 2) return result;

    size_t start = 1; // First byte is length, skip it
    for (size_t i = start; i < data.size() - 1; i += 2) {
        if (data[i+1] == 0 && data[i] != 0) {
            result += static_cast<char>(data[i]);
        }
    }

    if (is_guid) {
        result.erase(std::remove(result.begin(), result.end(), '{'), result.end());
        result.erase(std::remove(result.begin(), result.end(), '}'), result.end());
        result.erase(std::remove(result.begin(), result.end(), '\''), result.end());
    }
    return result;
}

bool ZuneDevice::SaveSessionGuidBinary(const ByteArray& guid_data) {
    std::ofstream file(device_guid_file_, std::ios::binary);
    if (!file.is_open()) {
        Log("Error: Could not write to " + device_guid_file_);
        return false;
    }
    file.write(reinterpret_cast<const char*>(guid_data.data()), guid_data.size());
    return true;
}

bool ZuneDevice::LoadSessionGuid() {
    std::ifstream file(device_guid_file_, std::ios::binary);
    if (!file.is_open()) {
        return false;
    }

    file.seekg(0, std::ios::end);
    size_t size = file.tellg();
    file.seekg(0, std::ios::beg);

    ByteArray guid_data(size);
    file.read(reinterpret_cast<char*>(guid_data.data()), size);

    session_guid_ = Utf16leToAscii(guid_data, true);
    return !session_guid_.empty();
}

ByteArray ZuneDevice::HexToBytes(const std::string& hex_str) {
    ByteArray data;
    std::string hex = hex_str;
    hex.erase(std::remove_if(hex.begin(), hex.end(), ::isspace), hex.end());

    for (size_t i = 0; i < hex.length(); i += 2) {
        if (i + 1 < hex.length()) {
            std::string byte_str = hex.substr(i, 2);
            data.push_back(static_cast<u8>(std::stoul(byte_str, nullptr, 16)));
        }
    }
    return data;
}

ByteArray ZuneDevice::LoadPropertyFromFile(const std::string& filename) {
    std::ifstream file(filename);
    if (!file.is_open()) {
        throw std::runtime_error("Could not open property file: " + filename);
    }
    std::string hex_str((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    return HexToBytes(hex_str);
}

int ZuneDevice::EstablishSyncPairing(const std::string& device_name) {
    if (!IsConnected()) {
        Log("Error: Not connected to a device.");
        return 1;
    }

    Log("Phase 1: USB Sync Pairing");
    Log("Establishing USB synchronization partnership...");

    try {
        // Step 3: Set driver version string
        Log("Setting MTP driver version...");
        std::string driver_str = "macOS/11.0 ZuneWirelessSync/1.0.0";
        ByteArray driver_data;
        OutputStream stream(driver_data);
        stream << driver_str;
        mtp_session_->SetDeviceProperty((DeviceProperty)0xd406, driver_data);
        Log("  ✓ Property 0xd406 set");

        // Step 4: Query property descriptors
        Log("Querying property descriptors...");
        try { mtp_session_->GetDevicePropertyDesc((DeviceProperty)0xd22f); } catch (const std::exception& e) { Log("  → Failed to query 0xd22f (non-critical): " + std::string(e.what())); }
        try { mtp_session_->GetDevicePropertyDesc((DeviceProperty)0xd402); } catch (const std::exception& e) { Log("  → Failed to query 0xd402 (non-critical): " + std::string(e.what())); }
        try { mtp_session_->GetDevicePropertyDesc((DeviceProperty)0x5002); } catch (const std::exception& e) { Log("  → Failed to query 0x5002 (non-critical): " + std::string(e.what())); }
        Log("  ✓ Queried initial descriptors");

        // Set device name if provided
        if (!device_name.empty()) {
            Log("Setting device name...");
            ByteArray name_data;
            OutputStream name_stream(name_data);
            name_stream << device_name;
            mtp_session_->SetDeviceProperty((DeviceProperty)0xd402, name_data);
            Log("  ✓ Device name set to: \"" + device_name + "\"");
        }

        // Step 6: Query more property descriptors
        Log("Querying pairing property descriptors...");
        try { mtp_session_->GetDevicePropertyDesc((DeviceProperty)0xd231); } catch (const std::exception& e) { Log("  → Failed to query 0xd231 (non-critical): " + std::string(e.what())); }
        try { mtp_session_->GetDevicePropertyDesc((DeviceProperty)0xd232); } catch (const std::exception& e) { Log("  → Failed to query 0xd232 (non-critical): " + std::string(e.what())); }
        try { mtp_session_->GetDevicePropertyDesc((DeviceProperty)0xd21c); } catch (const std::exception& e) { Log("  → Failed to query 0xd21c (non-critical): " + std::string(e.what())); }
        try { mtp_session_->GetDevicePropertyDesc((DeviceProperty)0xd225); } catch (const std::exception& e) { Log("  → Failed to query 0xd225 (non-critical): " + std::string(e.what())); }
        try { mtp_session_->GetDevicePropertyDesc((DeviceProperty)0xd401); } catch (const std::exception& e) { Log("  → Failed to query 0xd401 (non-critical): " + std::string(e.what())); }
        Log("  ✓ Queried pairing descriptors");

        // Step 8: Set pairing properties
        Log("Setting pairing properties...");

        try { mtp_session_->GetDevicePropertyDesc((DeviceProperty)0xd22c); } catch (const std::exception& e) { Log("  → Failed to query 0xd22c (non-critical): " + std::string(e.what())); }
        Log("  → Queried descriptor 0xd22c");

        cli_session_->SetDeviceProp("d225", "{00000000-0000-0000-0000-000000000000}");
        Log("  ✓ Set property 0xd225 (Null GUID)");

        ByteArray prop_d21c = {0x00};
        mtp_session_->SetDeviceProperty((DeviceProperty)0xd21c, prop_d21c);
        Log("  ✓ Set property 0xd21c");

        cli_session_->SetDeviceProp("d401", "{00000000-0000-0000-0000-000000000000}");
        Log("  ✓ Set property 0xd401 (SynchronizationPartner) ⭐ KEY PROPERTY");

        // Set remaining properties from embedded data
        mtp_session_->GetDevicePropertyDesc((DeviceProperty)0xd230);
        ByteArray prop_d230(prop_d230_data, prop_d230_data + sizeof(prop_d230_data));
        mtp_session_->SetDeviceProperty((DeviceProperty)0xd230, prop_d230);
        Log("  ✓ Set property 0xd230");

        mtp_session_->GetDevicePropertyDesc((DeviceProperty)0xd229);
        ByteArray prop_d229(prop_d229_data, prop_d229_data + sizeof(prop_d229_data));
        mtp_session_->SetDeviceProperty((DeviceProperty)0xd229, prop_d229);
        Log("  ✓ Set property 0xd229");

        mtp_session_->GetDevicePropertyDesc((DeviceProperty)0xd22a);
        ByteArray prop_d22a(prop_d22a_data, prop_d22a_data + sizeof(prop_d22a_data));
        mtp_session_->SetDeviceProperty((DeviceProperty)0xd22a, prop_d22a);
        Log("  ✓ Set property 0xd22a");

        // Step 9: Final sync operations
        Log("Running final sync operations...");
        mtp_session_->Operation9224();
        Log("  ✓ Operation 0x9224 complete");

        mtp_session_->GetDeviceProperty((DeviceProperty)0xd217);
        mtp_session_->GetDeviceProperty((DeviceProperty)0xd217);
        mtp_session_->GetDeviceProperty((DeviceProperty)0xd217);
        Log("  ✓ Property 0xd217 read 3x");

        mtp_session_->Operation9217(1);
        Log("  ✓ Operation 0x9217(1) complete");

        try {
            mtp_session_->Operation9227_Init();
            Log("  ✓ Operation 0x9227 succeeded");
        } catch (const std::exception& e) {
            Log("  → Operation 0x9227 failed (expected): " + std::string(e.what()));
        }

        mtp_session_->Operation9218(0, 0, 5000);
        Log("  ✓ Operation 0x9218(0, 0, 5000) complete");
        
        Log("\n✓ Phase 1 Complete!");
        return 0;

    } catch (const std::exception& e) {
        Log("\nError during Phase 1: " + std::string(e.what()));
        return 1;
    }
}

std::string ZuneDevice::EstablishWirelessPairing(const std::string& ssid, const std::string& password) {
    if (!IsConnected()) {
        Log("Error: Not connected to a device.");
        return "";
    }
    if (!LoadMacGuid()) {
        return "";
    }

    Log("Phase 2: Wireless Setup");

    try {
        Log("Network Subsystem Initialization...");
        mtp_session_->Operation9230(1);
        mtp_session_->Operation922b(3, 1, 0);

        Log("Setting GUID properties...");
        cli_session_->SetDeviceProp("d220", mac_guid_);
        ByteArray session_guid_data = mtp_session_->GetDeviceProperty((DeviceProperty)0xd221);
        session_guid_ = Utf16leToAscii(session_guid_data, true);
        SaveSessionGuidBinary(session_guid_data);

        Log("WiFi Subsystem Initialization...");
        cli_session_->SetWiFiNetwork(ssid, password);

        Log("Enabling wireless sync...");
        mtp_session_->Operation9230(1);

        Log("\n✓ Phase 2 Complete!");
        return session_guid_;

    } catch (const std::exception& e) {
        Log("\nError during Phase 2: " + std::string(e.what()));
        return "";
    }
}

int ZuneDevice::DisableWireless() {
    if (!IsConnected()) {
        Log("Error: Not connected to a device.");
        return 1;
    }
    try {
        Log("Disabling wireless sync...");
        cli_session_->DisableWireless();
        Log("✓ Wireless sync disabled");
        return 0;
    } catch (const std::exception& e) {
        Log("Error disabling wireless: " + std::string(e.what()));
        return 1;
    }
}

std::vector<std::string> ZuneDevice::ScanWiFiNetworks() {
    if (!IsConnected()) {
        Log("Error: Not connected to a device.");
        return {};
    }
    try {
        Log("Scanning for WiFi networks...");
        // This is a placeholder. The actual implementation would parse the return of GetWiFiNetworkList()
        mtp_session_->GetWiFiNetworkList();
        return {};
    } catch (const std::exception& e) {
        Log("Error scanning WiFi networks: " + std::string(e.what()));
        return {};
    }
}

std::vector<ZuneObjectInfoInternal> ZuneDevice::ListStorage(uint32_t parent_handle) {
    std::vector<ZuneObjectInfoInternal> results;
    if (!IsConnected()) {
        Log("Error: Not connected to a device.");
        return results;
    }
    try {
        // If parent_handle is 0, list the root of all storages.
        // Otherwise, list the contents of the specified object handle.
        if (parent_handle == 0) {
            auto storageIds = mtp_session_->GetStorageIDs();
            for (auto storageId : storageIds.StorageIDs) {
                auto handles = mtp_session_->GetObjectHandles(storageId, mtp::ObjectFormat::Any, mtp::Session::Root);
                for (auto handle : handles.ObjectHandles) {
                    auto info = mtp_session_->GetObjectInfo(handle);
                    results.push_back({handle.Id, info.Filename, info.ObjectCompressedSize, info.ObjectFormat == ObjectFormat::Association});
                }
            }
        } else {
            auto handles = mtp_session_->GetObjectHandles(mtp::Session::AllStorages, mtp::ObjectFormat::Any, mtp::ObjectId(parent_handle));
            for (auto handle : handles.ObjectHandles) {
                auto info = mtp_session_->GetObjectInfo(handle);
                results.push_back({handle.Id, info.Filename, info.ObjectCompressedSize, info.ObjectFormat == ObjectFormat::Association});
            }
        }
    } catch (const std::exception& e) {
        Log("Error listing storage: " + std::string(e.what()));
    }
    return results;
}

int ZuneDevice::DownloadFile(uint32_t object_handle, const std::string& destination_path) {
    if (!IsConnected()) return -1;
    try {
        // Retrieve album artwork from the RepresentativeSampleData property
        mtp::ByteArray artwork_data = mtp_session_->GetObjectProperty(
            mtp::ObjectId(object_handle),
            mtp::ObjectProperty::RepresentativeSampleData
        );

        // MTP property data has a 4-byte length prefix, skip it to get actual image data
        if (artwork_data.size() < 4) {
            Log("Error: Artwork data too small");
            return -1;
        }

        // Write to file (skip the 4-byte length prefix)
        std::ofstream file(destination_path, std::ios::binary);
        if (!file.is_open()) {
            Log("Error: Could not open file for writing: " + destination_path);
            return -1;
        }
        file.write(reinterpret_cast<const char*>(artwork_data.data() + 4), artwork_data.size() - 4);
        file.close();
    } catch (const std::exception& e) {
        Log("Error downloading file: " + std::string(e.what()));
        return -1;
    }
    return 0;
}

int ZuneDevice::UploadFile(const std::string& source_path, const std::string& destination_folder) {
    if (!IsConnected()) return -1;
    try {
        cli_session_->Put(source_path, destination_folder);
    } catch (const std::exception& e) {
        Log("Error uploading file: " + std::string(e.what()));
        return -1;
    }
    return 0;
}

int ZuneDevice::DeleteFile(uint32_t object_handle) {
    if (!IsConnected()) return -1;
    try {
        mtp_session_->DeleteObject(mtp::ObjectId(object_handle));
    } catch (const std::exception& e) {
        Log("Error deleting file: " + std::string(e.what()));
        return -1;
    }
    return 0;
}

int ZuneDevice::UploadWithArtwork(const std::string& media_path, const std::string& artwork_path) {
    if (!IsConnected()) return -1;
    try {
        // NOTE: artwork_path parameter is currently unused
        // The ZuneImport function handles artwork extraction from file metadata
        // For explicit artwork upload, use UploadTrackWithMetadata instead
        (void)artwork_path;  // Suppress unused parameter warning
        cli_session_->ZuneImport(media_path);
    } catch (const std::exception& e) {
        Log("Error uploading with artwork: " + std::string(e.what()));
        return -1;
    }
    return 0;
}

std::string ZuneDevice::GetName() {
    if (!IsConnected()) {
        Log("Error: Not connected to a device.");
        return "";
    }
    try {
        ByteArray name_data = mtp_session_->GetDeviceProperty((DeviceProperty)0xd402);
        return Utf16leToAscii(name_data);
    } catch (const std::exception& e) {
        Log("Error getting device name: " + std::string(e.what()));
        return "";
    }
}

std::string ZuneDevice::GetSerialNumber() {
    if (!device_) {
        Log("Error: Not connected to a device.");
        return "";
    }
    try {
        auto info = device_->GetInfo();
        return info.SerialNumber;
    } catch (const std::exception& e) {
        Log("Error getting serial number: " + std::string(e.what()));
        return "";
    }
}

std::string ZuneDevice::GetModel() {
    if (!usb_descriptor_) {
        Log("Error: Not connected to a device.");
        return "";
    }
    try {
        // Get USB product ID from descriptor
        u16 productId = usb_descriptor_->GetProductId();

        // Map Zune USB product IDs to model names
        switch (productId) {
            case 0x063e:  // Zune HD (32 GB)
                return "Zune HD";
            case 0x0710:  // Zune 30 (30 GB)
                return "Zune 30";
            default:
                // Unknown product ID, return generic "Zune"
                return "Zune";
        }
    } catch (const std::exception& e) {
        Log("Error getting device model: " + std::string(e.what()));
        return "";
    }
}

// C API cached helpers - these cache results and return pointers that remain valid
// until the next call to the same function or until the device object is destroyed
const char* ZuneDevice::GetNameCached() {
    std::lock_guard<std::mutex> lock(cache_mutex_);
    cached_name_ = GetName();
    return cached_name_.c_str();
}

const char* ZuneDevice::GetSerialNumberCached() {
    std::lock_guard<std::mutex> lock(cache_mutex_);
    cached_serial_number_ = GetSerialNumber();
    return cached_serial_number_.c_str();
}

const char* ZuneDevice::GetModelCached() {
    std::lock_guard<std::mutex> lock(cache_mutex_);
    cached_model_ = GetModel();
    return cached_model_.c_str();
}

const char* ZuneDevice::GetSessionGuidCached() {
    std::lock_guard<std::mutex> lock(cache_mutex_);
    cached_session_guid_ = session_guid_;
    return cached_session_guid_.c_str();
}

ZuneMusicLibrary* ZuneDevice::GetMusicLibrarySlow() {
    ZuneMusicLibrary* result = new ZuneMusicLibrary();
    result->tracks = nullptr;
    result->track_count = 0;
    result->albums = nullptr;
    result->album_count = 0;
    result->artworks = nullptr;
    result->artwork_count = 0;

    if (!IsConnected()) {
        Log("Error: Not connected to a device.");
        return result;
    }

    try {
        Library lib(mtp_session_);

        // Build flat arrays using AFTL enumeration
        std::vector<ZuneMusicTrack> tracks_vec;
        std::vector<ZuneMusicAlbum> albums_vec;
        std::map<std::string, uint32_t> artwork_map;  // alb_reference -> mtp_object_id

        uint32_t album_index = 0;

        // Iterate all albums
        for (auto const& [album_key, album_ptr] : lib._albums) {
            // Add album metadata
            ZuneMusicAlbum album;
            album.title = strdup(album_ptr->Name.c_str());
            album.artist_name = strdup(album_ptr->Artist->Name.c_str());
            album.release_year = album_ptr->Year;

            // album_key is pair<Artist*, string> - extract the string (album reference)
            album.alb_reference = strdup(album_key.second.c_str());
            album.atom_id = album_index;

            albums_vec.push_back(album);

            // Load tracks for this album
            lib.LoadRefs(album_ptr);

            // Iterate tracks and add to flat track array
            for (auto const& [track_name, track_index] : album_ptr->Tracks) {
                ZuneMusicTrack track;
                track.title = strdup(track_name.c_str());
                track.artist_name = strdup(album_ptr->Artist->Name.c_str());
                track.album_name = strdup(album_ptr->Name.c_str());
                track.genre = strdup("");  // AFTL doesn't provide genre
                track.track_number = track_index;
                track.duration_ms = 0;  // AFTL doesn't provide duration
                track.album_ref = album_index;  // Link to album by index
                track.atom_id = 0;  // AFTL doesn't provide atom_id

                // Find MTP ObjectId by filename
                for (auto const& ref : album_ptr->Refs) {
                    auto info = mtp_session_->GetObjectInfo(ref);
                    if (info.Filename == track_name) {
                        track.filename = strdup(info.Filename.c_str());
                        track.atom_id = ref.Id;
                        break;
                    }
                }

                if (track.atom_id == 0) {
                    track.filename = strdup(track_name.c_str());
                }

                tracks_vec.push_back(track);
            }

            // Store artwork reference if available (AFTL provides album artwork)
            // Refs is an unordered_set, so get first element via iterator
            if (!album_ptr->Refs.empty()) {
                auto first_ref = *album_ptr->Refs.begin();
                artwork_map[album_key.second] = first_ref.Id;
            }

            album_index++;
        }

        // Allocate and copy tracks
        result->track_count = tracks_vec.size();
        if (result->track_count > 0) {
            result->tracks = new ZuneMusicTrack[result->track_count];
            std::copy(tracks_vec.begin(), tracks_vec.end(), result->tracks);
        }

        // Allocate and copy albums
        result->album_count = albums_vec.size();
        if (result->album_count > 0) {
            result->albums = new ZuneMusicAlbum[result->album_count];
            std::copy(albums_vec.begin(), albums_vec.end(), result->albums);
        }

        // Build artwork array
        std::vector<ZuneAlbumArtwork> artworks_vec;
        for (const auto& [alb_ref, object_id] : artwork_map) {
            ZuneAlbumArtwork artwork;
            artwork.alb_reference = strdup(alb_ref.c_str());
            artwork.mtp_object_id = object_id;
            artworks_vec.push_back(artwork);
        }

        result->artwork_count = artworks_vec.size();
        if (result->artwork_count > 0) {
            result->artworks = new ZuneAlbumArtwork[result->artwork_count];
            std::copy(artworks_vec.begin(), artworks_vec.end(), result->artworks);
        }

        Log("GetMusicLibrarySlow: Retrieved " + std::to_string(result->track_count) +
            " tracks, " + std::to_string(result->album_count) + " albums (AFTL enumeration)");

    } catch (const std::exception& e) {
        Log("Error getting music library (slow): " + std::string(e.what()));
    }

    return result;
}

ZuneMusicLibrary* ZuneDevice::GetMusicLibrary() {
    if (!IsConnected()) {
        Log("Error: Not connected to a device.");
        return nullptr;
    }

    try {
        Log("Starting library retrieval using zmdb extraction...");

        // Step 1: Get zmdb binary from device
        std::vector<uint8_t> library_object_id = {0x03, 0x92, 0x1f};
        mtp::ByteArray zmdb_data = GetZuneMetadata(library_object_id);

        if (zmdb_data.empty()) {
            Log("Error: zmdb data is empty");
            return nullptr;
        }

        Log("Retrieved zmdb: " + std::to_string(zmdb_data.size()) + " bytes");

        // Step 2: Get device model and parse zmdb
        std::string device_model = GetModel();
        if (device_model.empty()) {
            Log("Error: Could not retrieve device model from AFTL");
            return nullptr;
        }

        zmdb::DeviceType device_type = (device_model.find("HD") != std::string::npos)
            ? zmdb::DeviceType::ZuneHD
            : zmdb::DeviceType::Zune30;

        auto parser = zmdb::ZMDBParserFactory::CreateParser(device_type);
        zmdb::ZMDBLibrary library = parser->ExtractLibrary(zmdb_data);

        Log("Extracted " + std::to_string(library.track_count) + " tracks, " +
            std::to_string(library.album_count) + " albums");

        // Step 3: Query MTP for album artwork ObjectIds
        Log("Querying album list for .alb references...");
        std::unordered_map<std::string, uint32_t> alb_to_objectid;

        try {
            mtp::ByteArray album_list = mtp_session_->GetObjectPropertyList(
                mtp::Session::Root,
                mtp::ObjectFormat::AbstractAudioAlbum,
                mtp::ObjectProperty::ObjectFilename,
                0,
                1
            );

            mtp::ObjectStringPropertyListParser::Parse(album_list,
                [&](mtp::ObjectId id, mtp::ObjectProperty property, const std::string &filename) {
                    alb_to_objectid[filename] = id.Id;
                    Log("  Found .alb: " + filename + " -> ObjectId " + std::to_string(id.Id));
                });

            Log("Found " + std::to_string(alb_to_objectid.size()) + " album objects");

        } catch (const std::exception& e) {
            Log("Warning: Could not query album list: " + std::string(e.what()));
        }

        // Step 4: Build flat data structure (no grouping - C# does that with LINQ)
        ZuneMusicLibrary* result = new ZuneMusicLibrary();

        // Copy tracks (already in array form, just convert std::string to char*)
        result->track_count = library.track_count;
        result->tracks = new ZuneMusicTrack[result->track_count];
        for (uint32_t i = 0; i < library.track_count; i++) {
            const auto& t = library.tracks[i];
            result->tracks[i].title = strdup(t.title.c_str());
            result->tracks[i].artist_name = strdup(t.artist_name.c_str());
            result->tracks[i].artist_guid = strdup(t.artist_guid.c_str());
            result->tracks[i].album_name = strdup(t.album_name.c_str());
            result->tracks[i].album_artist_name = strdup(t.album_artist_name.c_str());
            result->tracks[i].album_artist_guid = strdup(t.album_artist_guid.c_str());
            result->tracks[i].genre = strdup(t.genre.c_str());
            result->tracks[i].filename = strdup(t.filename.c_str());
            result->tracks[i].track_number = t.track_number;
            result->tracks[i].duration_ms = t.duration_ms;
            result->tracks[i].album_ref = t.album_ref;
            result->tracks[i].atom_id = t.atom_id;
        }

        // Allocate and copy albums
        result->album_count = library.album_metadata.size();
        result->albums = new ZuneMusicAlbum[result->album_count];
        size_t album_idx = 0;
        for (const auto& [atom_id, album] : library.album_metadata) {
            result->albums[album_idx].title = strdup(album.title.c_str());
            result->albums[album_idx].artist_name = strdup(album.artist_name.c_str());
            result->albums[album_idx].artist_guid = strdup(album.artist_guid.c_str());
            result->albums[album_idx].alb_reference = strdup(album.alb_reference.c_str());
            result->albums[album_idx].release_year = album.release_year;
            result->albums[album_idx].atom_id = album.atom_id;
            album_idx++;
        }

        // Build artwork array from MTP query results
        result->artwork_count = alb_to_objectid.size();
        result->artworks = new ZuneAlbumArtwork[result->artwork_count];
        size_t artwork_idx = 0;
        for (const auto& [alb_ref, object_id] : alb_to_objectid) {
            result->artworks[artwork_idx].alb_reference = strdup(alb_ref.c_str());
            result->artworks[artwork_idx].mtp_object_id = object_id;
            artwork_idx++;
        }

        Log("Library retrieval complete: " + std::to_string(result->track_count) + " tracks, " +
            std::to_string(result->album_count) + " albums, " +
            std::to_string(result->artwork_count) + " artworks");

        return result;

    } catch (const std::exception& e) {
        Log("Error in library retrieval: " + std::string(e.what()));
        return nullptr;
    }
}

std::vector<ZunePlaylistInfo> ZuneDevice::GetPlaylists() {
    std::vector<ZunePlaylistInfo> results;
    if (!IsConnected()) {
        Log("Error: Not connected to a device.");
        return results;
    }
    try {
        std::vector<ZuneObjectInfoInternal> all_files;
        std::function<void(uint32_t)> list_recursive = 
            [&](uint32_t parent_handle) {
            auto items = ListStorage(parent_handle);
            for (const auto& item : items) {
                all_files.push_back(item);
                if (item.is_folder) {
                    list_recursive(item.handle);
                }
            }
        };
        list_recursive(0);

        for (const auto& file : all_files) {
            if (file.filename.size() > 4 && file.filename.substr(file.filename.size() - 4) == ".wpl") {
                ZunePlaylistInfo playlist_info;
                playlist_info.Name = strdup(file.filename.c_str());
                playlist_info.MtpObjectId = file.handle;
                // TODO: Parse playlist file to get track paths
                playlist_info.TrackCount = 0;
                playlist_info.TrackPaths = nullptr;
                results.push_back(playlist_info);
            }
        }
    } catch (const std::exception& e) {
        Log("Error getting playlists: " + std::string(e.what()));
    }
    return results;
}

void ZuneDevice::EnsureLibraryInitialized() {
    if (!library_) {
        if (!mtp_session_) {
            throw std::runtime_error("MTP session not initialized");
        }
        Log("Initializing MTP Library for music management...");
        library_ = std::make_shared<mtp::Library>(mtp_session_);
        Log("  ✓ Library initialized and device scanned");
    }
}

int ZuneDevice::UploadTrackWithMetadata(
    const std::string& audio_file_path,
    const std::string& artist_name,
    const std::string& album_name,
    int album_year,
    const std::string& track_title,
    const std::string& genre,
    int track_number,
    const uint8_t* artwork_data,
    size_t artwork_size,
    const std::string& artist_guid,
    uint32_t* out_track_id,
    uint32_t* out_album_id,
    uint32_t* out_artist_id
) {
    if (!IsConnected()) {
        Log("Error: Not connected to device");
        return -1;
    }

    try {
        Log("Uploading track: " + track_title + " by " + artist_name);
        if (!artist_guid.empty()) {
            Log("  Artist GUID: " + artist_guid);
        }

        // Ensure library is initialized
        EnsureLibraryInitialized();

        // Open audio file for streaming
        auto stream = std::make_shared<cli::ObjectInputStream>(audio_file_path);
        stream->SetTotal(stream->GetSize());
        Log("  ✓ Audio file opened: " + std::to_string(stream->GetSize()) + " bytes");

        // Get or create artist
        auto artist = library_->GetArtist(artist_name);
        if (!artist) {
            Log("  Creating artist: " + artist_name);
            if (!artist_guid.empty()) {
                Log("  → Registering with Zune GUID for metadata fetching");
            }
            artist = library_->CreateArtist(artist_name, artist_guid);
            if (!artist) {
                Log("Error: Failed to create artist");
                return -1;
            }
        } else {
            // Artist exists - check if we need to update the GUID
            if (!artist_guid.empty() && artist->Guid.empty()) {
                Log("  Artist exists but has no GUID - updating with Zune GUID for metadata fetching");
                library_->UpdateArtistGuid(artist, artist_guid);
                Log("  ✓ Artist GUID updated");
            }
        }
        Log("  ✓ Artist: " + artist_name);

        // Get or create album
        auto album = library_->GetAlbum(artist, album_name);
        if (!album) {
            Log("  Creating album: " + album_name);
            album = library_->CreateAlbum(artist, album_name, album_year);
            if (!album) {
                Log("Error: Failed to create album");
                return -1;
            }
        }
        Log("  ✓ Album: " + album_name + " (" + std::to_string(album_year) + ")");

        // Register artist GUID with device BEFORE track upload (critical timing!)
        // Windows creates metadata linkage object 11.7 seconds BEFORE track upload (Frame 1189 vs 1605)
        if (!artist_guid.empty()) {
            Log("  Registering artist GUID with device...");
            try {
                library_->ValidateArtistGuid(artist_name, track_title, artist_guid);
                Log("  ✓ Artist GUID registered - device should now recognize artist metadata");
            } catch (const std::exception& e) {
                Log("  Warning: GUID registration failed: " + std::string(e.what()));
                Log("  (Device may not request artist metadata)");
            }
        }

        // Determine object format from file extension
        auto slashpos = audio_file_path.rfind('/');
        auto filename = slashpos != audio_file_path.npos ?
            audio_file_path.substr(slashpos + 1) : audio_file_path;
        mtp::ObjectFormat format = mtp::ObjectFormatFromFilename(audio_file_path);

        // Create track entry with metadata
        Log("  Creating track entry...");
        auto track_info = library_->CreateTrack(
            artist,
            album,
            format,
            track_title,
            genre,
            track_number,
            filename,
            stream->GetSize()
        );
        Log("  ✓ Track entry created");

        // Upload audio data
        Log("  Uploading audio data...");
        mtp_session_->SendObject(stream);
        Log("  ✓ Audio data uploaded");

        // Add artwork if provided
        if (artwork_data && artwork_size > 0) {
            Log("  Adding album artwork...");
            mtp::ByteArray artwork(artwork_data, artwork_data + artwork_size);
            library_->AddCover(album, artwork);
            Log("  ✓ Album artwork added");
        }

        // Link track to album
        Log("  Linking track to album...");
        library_->AddTrack(album, track_info);
        Log("  ✓ Track linked to album");

        Log("✓ Track uploaded successfully");

        // Synchronize device database (ZMDB) after upload
        // This operation appears in the Windows Zune capture after track uploads
        // and is critical for the device to recognize new artists that need metadata
        Log("  Synchronizing device database (Operation 0x9217)...");
        try {
            mtp_session_->Operation9217(1);
            Log("  ✓ Database synchronized - device should now recognize new artist");
        } catch (const std::exception& e) {
            Log("  Warning: Database sync failed: " + std::string(e.what()));
            Log("  (Device may not request artist metadata)");
        }

        // Post-upload sequence: Query track properties and trigger metadata fetch
        // This sequence matches Windows capture (frames 6270-6390) and is CRITICAL for HTTP metadata requests
        Log("  Executing post-upload metadata trigger sequence...");
        try {
            // Use the track ObjectId returned from SendObjectInfo (in track_info.Id)
            mtp::ObjectId newTrackHandle = track_info.Id;
            Log("    Using track handle: 0x" + std::to_string(newTrackHandle.Id));

            // Phase 1: Query properties of the new track - only name property (0xDC44)
            Log("    Querying track properties...");
            mtp_session_->Operation9802(0xDC44, newTrackHandle.Id);  // Name
            Log("    ✓ Track properties queried");
            Log("    ✓ Post-upload metadata trigger sequence complete");

        } catch (const std::exception& e) {
            Log("    Warning: Post-upload sequence failed: " + std::string(e.what()));
        }

        // NOTE: Artist GUID registration now happens BEFORE track upload (see line 933)
        // This matches Windows behavior where metadata linkage object is created before track

        // Populate out-parameters with MTP ObjectIds
        if (out_track_id) {
            *out_track_id = track_info.Id.Id;
        }
        if (out_album_id) {
            *out_album_id = album->Id.Id;
        }
        if (out_artist_id) {
            *out_artist_id = artist->Id.Id;
        }

        return 0;

    } catch (const std::exception& e) {
        Log("Error uploading track: " + std::string(e.what()));
        return -1;
    }
}

int ZuneDevice::RetrofitArtistGuid(
    const std::string& artist_name,
    const std::string& guid
) {
    if (!IsConnected()) {
        Log("Error: Not connected to device");
        return -1;
    }

    if (guid.empty()) {
        Log("Error: GUID is empty");
        return -1;
    }

    if (!IsValidGuid(guid)) {
        Log("Error: Invalid GUID format: " + guid);
        Log("Expected format: xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx (hexadecimal)");
        return -1;
    }

    try {
        Log("=== Retrofit Artist GUID (Delete/Recreate Approach) ===");
        Log("Artist: " + artist_name);
        Log("GUID: " + guid);

        // Ensure library is initialized
        EnsureLibraryInitialized();

        // Get existing artist
        auto artist = library_->GetArtist(artist_name);
        if (!artist) {
            Log("Error: Artist not found: " + artist_name);
            return -1;
        }

        // Check if artist has a valid GUID (not empty and not all null bytes)
        bool hasValidGuid = !artist->Guid.empty();
        if (hasValidGuid) {
            // Check if GUID is not all zeros (null GUID = invalid)
            hasValidGuid = false;
            for (unsigned char byte : artist->Guid) {
                if (byte != 0) {
                    hasValidGuid = true;
                    break;
                }
            }
        }

        if (hasValidGuid) {
            Log("Artist already has valid GUID - no retrofit needed");
            return 0;
        }

        Log("Original artist object ID: 0x" + std::to_string(artist->Id.Id));

        // Find all albums by this artist
        Log("Finding albums by artist...");
        auto albums = library_->GetAlbumsByArtist(artist);
        Log("Found " + std::to_string(albums.size()) + " albums");

        // Load track references for each album
        std::vector<std::vector<mtp::ObjectId>> album_tracks;
        for (auto& album : albums) {
            Log("  Album: " + album->Name);
            auto tracks = library_->GetTracksForAlbum(album);
            Log("    Tracks: " + std::to_string(tracks.size()));
            album_tracks.push_back(tracks);
        }

        // Create new artist with GUID FIRST (before deleting old one)
        Log("Creating new artist object with GUID...");
        auto new_artist = library_->CreateArtist(artist_name, guid);
        Log("✓ New artist object created (ID: 0x" + std::to_string(new_artist->Id.Id) + ")");

        // Update all albums to reference new artist (while old artist still exists)
        Log("Updating album references to new artist...");
        for (size_t i = 0; i < albums.size(); ++i) {
            auto& album = albums[i];
            Log("  Updating album: " + album->Name);
            library_->UpdateAlbumArtist(album, new_artist);

            // Update all tracks in this album
            Log("  Updating " + std::to_string(album_tracks[i].size()) + " tracks");
            for (auto track_id : album_tracks[i]) {
                library_->UpdateTrackArtist(track_id, new_artist);
            }
        }

        // Now delete old artist object (after all references are updated)
        Log("Deleting old artist object...");
        mtp_session_->DeleteObject(artist->Id);
        Log("✓ Old artist object deleted");

        Log("✓ Artist retrofit complete!");
        Log("Artist recreated with GUID, all album/track references updated");

        // Invalidate library cache to force reload of updated device state
        Log("Invalidating library cache to force reload after retrofit");
        library_.reset();
        library_ = nullptr;

        return 0;

    } catch (const std::exception& e) {
        Log("Error during artist retrofit: " + std::string(e.what()));
        return -1;
    }
}

ZuneDevice::BatchRetrofitResult ZuneDevice::RetrofitMultipleArtistGuids(
    const std::vector<ArtistGuidMapping>& mappings)
{
    BatchRetrofitResult result = {0, 0, 0, 0};

    if (mappings.empty()) {
        Log("Batch retrofit: No artists provided");
        return result;
    }

    if (!IsConnected()) {
        Log("Error: Not connected to device");
        result.error_count = mappings.size();
        return result;
    }

    Log("=== Starting Batch Artist GUID Retrofit ===");
    Log("Processing " + std::to_string(mappings.size()) + " artists");

    try {
        // Parse library ONCE for entire batch
        EnsureLibraryInitialized();

        // Process each artist
        for (const auto& mapping : mappings) {
            const std::string& artist_name = mapping.artist_name;
            const std::string& guid = mapping.guid;

            Log("Artist: " + artist_name);
            Log("GUID: " + guid);

            // Validate GUID format
            if (!IsValidGuid(guid)) {
                result.error_count++;
                Log("  ✗ Invalid GUID format: " + guid);
                Log("    Expected format: xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx (hexadecimal)");
                continue;
            }

            try {
                // Get existing artist
                auto artist = library_->GetArtist(artist_name);

                if (!artist) {
                    // Artist doesn't exist yet - not an error, will be created during upload
                    result.not_found_count++;
                    Log("  Artist '" + artist_name + "' not found on device (will be created during upload)");
                    continue;
                }

                // Check if artist has a valid GUID (not empty and not all null bytes)
                bool hasValidGuid = !artist->Guid.empty();
                if (hasValidGuid) {
                    // Check if GUID is not all zeros (null GUID = invalid)
                    hasValidGuid = false;
                    for (unsigned char byte : artist->Guid) {
                        if (byte != 0) {
                            hasValidGuid = true;
                            break;
                        }
                    }
                }

                if (hasValidGuid) {
                    // Artist already has valid GUID - fast path, no work needed
                    result.already_had_guid_count++;
                    Log("  Artist '" + artist_name + "' already has valid GUID - no retrofit needed");
                    continue;
                }

                Log("  Original artist object ID: 0x" + std::to_string(artist->Id.Id));

                // Find all albums by this artist
                Log("  Finding albums by artist...");
                auto albums = library_->GetAlbumsByArtist(artist);
                Log("  Found " + std::to_string(albums.size()) + " albums");

                // Load track references for each album
                std::vector<std::vector<mtp::ObjectId>> album_tracks;
                for (auto& album : albums) {
                    Log("    Album: " + album->Name);
                    auto tracks = library_->GetTracksForAlbum(album);
                    Log("      Tracks: " + std::to_string(tracks.size()));
                    album_tracks.push_back(tracks);
                }

                // Create new artist with GUID FIRST (before deleting old one)
                Log("  Creating new artist object with GUID...");
                auto new_artist = library_->CreateArtist(artist_name, guid);
                Log("  ✓ New artist object created (ID: 0x" + std::to_string(new_artist->Id.Id) + ")");

                // Update all albums to reference new artist (while old artist still exists)
                Log("  Updating album references to new artist...");
                for (size_t i = 0; i < albums.size(); ++i) {
                    auto& album = albums[i];
                    Log("    Updating album: " + album->Name);
                    library_->UpdateAlbumArtist(album, new_artist);

                    // Update all tracks in this album
                    Log("    Updating " + std::to_string(album_tracks[i].size()) + " tracks");
                    for (auto track_id : album_tracks[i]) {
                        library_->UpdateTrackArtist(track_id, new_artist);
                    }
                }

                // Now delete old artist object (after all references are updated)
                Log("  Deleting old artist object...");
                mtp_session_->DeleteObject(artist->Id);
                Log("  ✓ Old artist object deleted");

                result.retrofitted_count++;
                Log("  ✓ Artist '" + artist_name + "' retrofitted successfully");

            } catch (const std::exception& e) {
                result.error_count++;
                Log("  ✗ Error retrofitting artist '" + artist_name + "': " + std::string(e.what()));
                // Continue with next artist
            }
        }

        // Invalidate library cache ONCE if any changes were made
        if (result.retrofitted_count > 0) {
            Log("Invalidating library cache after " +
                std::to_string(result.retrofitted_count) + " retrofits");
            library_.reset();
            library_ = nullptr;
        }

        Log("=== Batch Retrofit Complete ===");
        Log("Results: " +
            std::to_string(result.retrofitted_count) + " retrofitted, " +
            std::to_string(result.already_had_guid_count) + " already had GUID, " +
            std::to_string(result.not_found_count) + " not found, " +
            std::to_string(result.error_count) + " errors");

        return result;

    } catch (const std::exception& e) {
        Log("Fatal error during batch retrofit: " + std::string(e.what()));
        result.error_count = mappings.size();
        return result;
    }
}

mtp::ByteArray ZuneDevice::GetPartialObject(uint32_t object_id, uint64_t offset, uint32_t size) {
    if (!IsConnected()) {
        Log("Error: Not connected to device");
        return mtp::ByteArray();
    }

    try {
        return mtp_session_->GetPartialObject(mtp::ObjectId(object_id), offset, size);
    } catch (const std::exception& e) {
        Log("Error reading partial object: " + std::string(e.what()));
        return mtp::ByteArray();
    }
}

uint64_t ZuneDevice::GetObjectSize(uint32_t object_id) {
    if (!IsConnected()) {
        Log("Error: Not connected to device");
        return 0;
    }

    try {
        return mtp_session_->GetObjectIntegerProperty(
            mtp::ObjectId(object_id),
            mtp::ObjectProperty::ObjectSize
        );
    } catch (const std::exception& e) {
        Log("Error getting object size: " + std::string(e.what()));
        return 0;
    }
}

std::string ZuneDevice::GetObjectFilename(uint32_t object_id) {
    if (!IsConnected()) {
        Log("Error: Not connected to device");
        return "";
    }

    try {
        auto info = mtp_session_->GetObjectInfo(mtp::ObjectId(object_id));
        return info.Filename;
    } catch (const std::exception& e) {
        Log("Error getting object filename: " + std::string(e.what()));
        return "";
    }
}

uint32_t ZuneDevice::GetAudioTrackObjectId(const std::string& track_title, uint32_t album_object_id) {
    if (!IsConnected()) {
        Log("Error: Not connected to device");
        return 0;
    }

    if (track_title.empty()) {
        Log("Error: track_title is empty");
        return 0;
    }

    if (album_object_id == 0) {
        Log("Error: Album ObjectId is required (must be > 0). Cannot search for track without album context.");
        return 0;
    }

    // Check cache first
    std::string cache_key = std::to_string(album_object_id) + ":" + track_title;
    {
        std::lock_guard<std::mutex> lock(track_cache_mutex_);
        auto it = track_objectid_cache_.find(cache_key);
        if (it != track_objectid_cache_.end()) {
            Log("Cache hit: Track '" + track_title + "' -> ObjectId " + std::to_string(it->second));
            return it->second;
        }
    }

    try {
        Log("Searching for audio track: '" + track_title + "' in album ObjectId: " + std::to_string(album_object_id));

        Log("Querying object references for album ObjectId " + std::to_string(album_object_id) + "...");

        // Get all object references (child objects) within the album
        // This uses the MTP GetObjectReferences command which returns audio files referenced by the album
        auto object_refs = mtp_session_->GetObjectReferences(mtp::ObjectId(album_object_id));

        Log("Found " + std::to_string(object_refs.ObjectHandles.size()) + " track references in album");

        uint32_t found_id = 0;

        // Iterate through each track reference and check its Name metadata property
        // Cache ALL tracks from this album for future lookups (opportunistic caching)
        for (const auto& handle : object_refs.ObjectHandles) {
            try {
                // Get the Name property (0xdc44) for this object
                std::string file_name = mtp_session_->GetObjectStringProperty(
                    handle,
                    mtp::ObjectProperty::Name
                );

                // Strip extension from filename
                size_t dot_pos = file_name.rfind('.');
                std::string track_name = (dot_pos != std::string::npos && dot_pos > 0)
                    ? file_name.substr(0, dot_pos)
                    : file_name;

                // Cache this track (key = album_id:track_name_without_extension)
                std::string track_cache_key = std::to_string(album_object_id) + ":" + track_name;
                {
                    std::lock_guard<std::mutex> lock(track_cache_mutex_);
                    track_objectid_cache_[track_cache_key] = handle.Id;
                }

                // Check if this is the track we're looking for
                if (track_name == track_title) {
                    found_id = handle.Id;
                    Log("Matched track '" + track_title + "' to ObjectId " + std::to_string(handle.Id));
                }

            } catch (const std::exception& e) {
                Log("  Error reading Name property for track " + std::to_string(handle.Id) + ": " + std::string(e.what()));
                continue;
            }
        }

        if (found_id == 0) {
            Log("Audio track '" + track_title + "' not found in album ObjectId " + std::to_string(album_object_id));
        } else {
            Log("Cached " + std::to_string(object_refs.ObjectHandles.size()) + " tracks from album " + std::to_string(album_object_id));
        }
        return found_id;

    } catch (const std::exception& e) {
        Log("Error querying for audio track '" + track_title + "': " + std::string(e.what()));
        return 0;
    }
}

void ZuneDevice::ClearTrackObjectIdCache() {
    std::lock_guard<std::mutex> lock(track_cache_mutex_);
    track_objectid_cache_.clear();
    Log("Track ObjectId cache cleared");
}

mtp::ByteArray ZuneDevice::GetZuneMetadata(const std::vector<uint8_t>& object_id) {
    mtp::ByteArray result;
    if (!mtp_session_) {
        Log("Error: Not connected to a device.");
        return result;
    }

    try {
        // Build bulk OUT request data (16 bytes total)
        // Structure:
        //   0x10 0x00 0x00 0x00 - length (16 bytes)
        //   0x01 0x00 0x17 0x92 - command identifier
        //   [3 bytes object ID + 1 byte padding] - metadata type ID
        //   0x01 0x00 0x00 0x00 - trailer
        mtp::ByteArray request_data(ZMDB_REQUEST_SIZE, 0);
        request_data[0] = ZMDB_REQUEST_LENGTH_BYTE;  // Length low byte
        request_data[1] = 0x00;
        request_data[2] = 0x00;
        request_data[3] = 0x00;
        request_data[4] = ZMDB_COMMAND_MARKER;
        request_data[5] = 0x00;
        request_data[6] = ZMDB_OPERATION_CODE_HIGH;
        request_data[7] = ZMDB_OPERATION_CODE_LOW;
        // Copy object ID bytes
        for (size_t i = 0; i < object_id.size() && i < ZMDB_OBJECT_ID_SIZE; ++i) {
            request_data[8 + i] = object_id[i];
        }
        request_data[11] = 0x00;  // Padding
        request_data[12] = ZMDB_TRAILER_VALUE;
        request_data[13] = 0x00;
        request_data[14] = 0x00;
        request_data[15] = 0x00;

        // Log the request bytes being sent
        {
            std::ostringstream oss;
            oss << "Sending request bytes (hex): ";
            for (size_t i = 0; i < request_data.size(); ++i) {
                if (i > 0) oss << " ";
                oss << std::hex << std::setfill('0') << std::setw(2) << (int)request_data[i];
            }
            oss << std::dec;
            Log(oss.str());
        }

        // Send bulk OUT request to trigger metadata response
        auto pipe = mtp_session_->GetBulkPipe();
        if (!pipe) {
            Log("Error: Cannot access USB pipe");
            return result;
        }

        auto inputStream = std::make_shared<ByteArrayInputStream>(request_data);
        pipe->Write(inputStream, mtp::Session::DefaultTimeout);
        Log("Zune metadata request sent");

        // Wait briefly for device to prepare response
        std::this_thread::sleep_for(std::chrono::milliseconds(ZMDB_DEVICE_PREPARE_DELAY_MS));

        // Read bulk IN response header first (12 bytes)
        auto headerStream = std::make_shared<ByteArrayOutputStream>();
        pipe->Read(headerStream, mtp::Session::DefaultTimeout);

        if (headerStream->data.size() == 0) {
            Log("Error: No response received from device");
            return result;
        }

        Log("Response header received: " + std::to_string(headerStream->data.size()) + " bytes");

        // Log the response header bytes
        if (headerStream->data.size() > 0) {
            std::ostringstream oss;
            oss << "Response header bytes (hex): ";
            for (size_t i = 0; i < headerStream->data.size(); ++i) {
                if (i > 0) oss << " ";
                oss << std::hex << std::setfill('0') << std::setw(2) << (int)headerStream->data[i];
            }
            oss << std::dec;
            Log(oss.str());
        }

        // Parse header to get total size
        uint32_t total_size = 0;
        if (headerStream->data.size() >= 4) {
            total_size = headerStream->data[0] |
                        (headerStream->data[1] << 8) |
                        (headerStream->data[2] << 16) |
                        (headerStream->data[3] << 24);
            Log("Total size from header: " + std::to_string(total_size) + " bytes");
        }

        // If response is just the header, return it
        if (headerStream->data.size() == ZMDB_HEADER_SIZE && total_size <= ZMDB_HEADER_SIZE) {
            Log("Response is header-only (" + std::to_string(ZMDB_HEADER_SIZE) + " bytes)");
            result = headerStream->data;
            return result;
        }

        // Otherwise, read the payload data
        // Device will send the remaining data in subsequent bulk transfers
        if (total_size > ZMDB_HEADER_SIZE) {
            Log("Reading metadata payload...");
            auto payloadStream = std::make_shared<ByteArrayOutputStream>();
            pipe->Read(payloadStream, mtp::Session::LongTimeout);
            Log("Metadata payload received: " + std::to_string(payloadStream->data.size()) + " bytes");

            // Return only the payload (strip 12-byte header)
            // The header contains size/response metadata, but the parser expects raw ZMDB data
            result = payloadStream->data;
            Log("Returning ZMDB payload: " + std::to_string(result.size()) + " bytes (header stripped)");

            // Drain any remaining buffered data from the pipe to clean up state
            // This prevents the next request from receiving echoed data from the previous one
            Log("Draining pipe to clear any remaining data...");
            try {
                auto drainStream = std::make_shared<ByteArrayOutputStream>();
                // Try to read with a very short timeout to see if there's any leftover data
                pipe->Read(drainStream, ZMDB_PIPE_DRAIN_TIMEOUT_MS);
                if (drainStream->data.size() > 0) {
                    Log("Drained " + std::to_string(drainStream->data.size()) + " bytes from pipe");
                }
            } catch (const std::exception& e) {
                // It's OK if this times out - it just means the pipe is empty
                Log("Pipe drain completed (timeout is expected)");
            }
        }

    } catch (const std::exception& e) {
        Log("Error during Zune metadata transfer: " + std::string(e.what()));
    }

    return result;
}
