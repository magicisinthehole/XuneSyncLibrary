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
#include "NetworkManager.h"
#include "LibraryManager.h"



using namespace mtp;

// === Helper Functions ===



// --- Embedded Phase 1 Property Data ---
static const uint8_t prop_d230_data[] = { 0x7a, 0x24, 0xec, 0x12, 0x00, 0x00, 0x00, 0x00 };
static const uint8_t prop_d229_data[] = { 0xf4, 0x00, 0x00, 0x00 };
static const uint8_t prop_d22a_data[] = { 0x5c, 0xfe, 0xff, 0xff };



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

        // Initialize NetworkManager
        network_manager_ = std::make_unique<NetworkManager>(mtp_session_, [this](const std::string& msg) {
            this->Log(msg);
        });

        // Initialize LibraryManager
        library_manager_ = std::make_unique<LibraryManager>(mtp_session_, cli_session_, [this](const std::string& msg) {
            this->Log(msg);
        });



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

    if (network_manager_) {
        network_manager_.reset();
    }
    if (library_manager_) {
        library_manager_.reset();
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

// ============================================================================
// Library Manager Delegation
// ============================================================================

std::vector<ZuneObjectInfoInternal> ZuneDevice::ListStorage(uint32_t parent_handle) {
    if (library_manager_) {
        return library_manager_->ListStorage(parent_handle);
    }
    return {};
}

ZuneMusicLibrary* ZuneDevice::GetMusicLibrary() {
    if (library_manager_) {
        return library_manager_->GetMusicLibrary(GetModel());
    }
    return nullptr;
}

ZuneMusicLibrary* ZuneDevice::GetMusicLibrarySlow() {
    if (library_manager_) {
        return library_manager_->GetMusicLibrarySlow();
    }
    return nullptr;
}

std::vector<ZunePlaylistInfo> ZuneDevice::GetPlaylists() {
    if (library_manager_) {
        return library_manager_->GetPlaylists();
    }
    return {};
}

int ZuneDevice::DownloadFile(uint32_t object_handle, const std::string& destination_path) {
    if (library_manager_) {
        return library_manager_->DownloadFile(object_handle, destination_path);
    }
    return -1;
}

int ZuneDevice::UploadFile(const std::string& source_path, const std::string& destination_folder) {
    if (library_manager_) {
        return library_manager_->UploadFile(source_path, destination_folder);
    }
    return -1;
}

int ZuneDevice::DeleteFile(uint32_t object_handle) {
    if (library_manager_) {
        return library_manager_->DeleteFile(object_handle);
    }
    return -1;
}

int ZuneDevice::UploadWithArtwork(const std::string& media_path, const std::string& artwork_path) {
    if (library_manager_) {
        return library_manager_->UploadWithArtwork(media_path, artwork_path);
    }
    return -1;
}

int ZuneDevice::UploadTrackWithMetadata(
    MediaType media_type,
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
    uint32_t duration_ms,
    int rating,
    uint32_t* out_track_id,
    uint32_t* out_album_id,
    uint32_t* out_artist_id
) {
    if (library_manager_) {
        return library_manager_->UploadTrackWithMetadata(
            media_type, audio_file_path, artist_name, album_name, album_year,
            track_title, genre, track_number, artwork_data, artwork_size,
            artist_guid, duration_ms, rating, out_track_id, out_album_id, out_artist_id
        );
    }
    return -1;
}

int ZuneDevice::RetrofitArtistGuid(const std::string& artist_name, const std::string& guid) {
    if (library_manager_) {
        return library_manager_->RetrofitArtistGuid(artist_name, guid);
    }
    return -1;
}

ZuneDevice::BatchRetrofitResult ZuneDevice::RetrofitMultipleArtistGuids(
    const std::vector<ArtistGuidMapping>& mappings)
{
    if (library_manager_) {
        // Convert ZuneDevice::ArtistGuidMapping to LibraryManager::ArtistGuidMapping
        std::vector<LibraryManager::ArtistGuidMapping> lib_mappings;
        lib_mappings.reserve(mappings.size());
        for (const auto& m : mappings) {
            lib_mappings.push_back({m.artist_name, m.guid});
        }

        auto result = library_manager_->RetrofitMultipleArtistGuids(lib_mappings);
        return {result.retrofitted_count, result.already_had_guid_count, result.not_found_count, result.error_count};
    }
    return {0, 0, 0, 0};
}

mtp::ByteArray ZuneDevice::GetPartialObject(uint32_t object_id, uint64_t offset, uint32_t size) {
    if (library_manager_) {
        return library_manager_->GetPartialObject(object_id, offset, size);
    }
    return mtp::ByteArray();
}

uint64_t ZuneDevice::GetObjectSize(uint32_t object_id) {
    if (library_manager_) {
        return library_manager_->GetObjectSize(object_id);
    }
    return 0;
}

std::string ZuneDevice::GetObjectFilename(uint32_t object_id) {
    if (library_manager_) {
        return library_manager_->GetObjectFilename(object_id);
    }
    return "";
}

uint32_t ZuneDevice::GetAudioTrackObjectId(const std::string& track_title, uint32_t album_object_id) {
    if (library_manager_) {
        return library_manager_->GetAudioTrackObjectId(track_title, album_object_id);
    }
    return 0;
}

void ZuneDevice::ClearTrackObjectIdCache() {
    if (library_manager_) {
        library_manager_->ClearTrackObjectIdCache();
    }
}

int ZuneDevice::SetTrackUserState(uint32_t zmdb_atom_id, int play_count, int skip_count, int rating) {
    if (!mtp_session_) {
        Log("SetTrackUserState: Device not connected");
        return -2;
    }

    if (zmdb_atom_id == 0) {
        Log("SetTrackUserState: Invalid ZMDB atom_id (0)");
        return -3;
    }

    Log("SetTrackUserState: atom_id=" + std::to_string(zmdb_atom_id) +
        " (play_count=" + std::to_string(play_count) +
        ", skip_count=" + std::to_string(skip_count) +
        ", rating=" + std::to_string(rating) + ")");

    // DISABLED: Play count via SetObjectProperty - needs pcap investigation
    // DC91 (UseCount) works but we don't know if it's the correct protocol
    if (play_count >= 0) {
        Log("  [DISABLED] play_count update - pending protocol analysis");
    }

    // DISABLED: Skip count - property not supported on Zune HD (InvalidObjectPropCode 0xa801)
    if (skip_count >= 0) {
        Log("  [DISABLED] skip_count update - property not supported");
    }

    // Rating: Must use SetTrackRatingsByAlbum for batch updates with album grouping
    // Single-track rating updates are not supported - use the batch API instead
    if (rating >= 0) {
        Log("  [DEPRECATED] Single-track rating not supported. Use SetTrackRatingsByAlbum with album MTP IDs.");
        return -1;
    }

    return 0;
}

int ZuneDevice::SetTrackRatingsByAlbum(
    const std::vector<uint32_t>& album_mtp_ids,
    const std::vector<std::vector<uint32_t>>& tracks_per_album,
    uint8_t rating)
{
    if (!mtp_session_) {
        Log("SetTrackRatingsByAlbum: Device not connected");
        return -2;
    }

    if (album_mtp_ids.empty() || album_mtp_ids.size() != tracks_per_album.size()) {
        Log("SetTrackRatingsByAlbum: Invalid parameters");
        return -1;
    }

    // Log the operation
    size_t total_tracks = 0;
    for (const auto& tracks : tracks_per_album) {
        total_tracks += tracks.size();
    }
    Log("SetTrackRatingsByAlbum: " + std::to_string(album_mtp_ids.size()) + " albums, " +
        std::to_string(total_tracks) + " tracks, rating=" + std::to_string(rating));

    try {
        mtp_session_->SetTrackRatingsByAlbum(album_mtp_ids, tracks_per_album, rating);
        Log("SetTrackRatingsByAlbum: SUCCESS");
        return 0;
    } catch (const std::exception& e) {
        Log("SetTrackRatingsByAlbum: FAILED - " + std::string(e.what()));
        return -1;
    }
}

int ZuneDevice::SetTrackRatingDirect(uint32_t track_mtp_id, uint8_t rating) {
    if (!mtp_session_) {
        Log("SetTrackRatingDirect: Device not connected");
        return -2;
    }

    Log("SetTrackRatingDirect: track_id=0x" + std::to_string(track_mtp_id) +
        " rating=" + std::to_string(rating));

    try {
        // DC8A (UserRating) uses UINT16 per MTP spec
        mtp::ByteArray value(2);
        value[0] = rating;
        value[1] = 0;

        mtp_session_->SetObjectProperty(
            mtp::ObjectId(track_mtp_id),
            mtp::ObjectProperty::UserRating,
            value);

        Log("SetTrackRatingDirect: SUCCESS");
        return 0;
    } catch (const std::exception& e) {
        Log("SetTrackRatingDirect: FAILED - " + std::string(e.what()));
        return -1;
    }
}

mtp::ByteArray ZuneDevice::GetZuneMetadata(const std::vector<uint8_t>& object_id) {
    if (library_manager_) {
        return library_manager_->GetZuneMetadata(object_id);
    }
    return mtp::ByteArray();
}

// ============================================================================
// Network Manager Delegation
// ============================================================================

bool ZuneDevice::InitializeHTTPSubsystem() {
    if (network_manager_) {
        return network_manager_->InitializeHTTPSubsystem();
    }
    return false;
}

void ZuneDevice::StartHTTPInterceptor(const InterceptorConfig& config) {
    if (network_manager_) {
        network_manager_->StartHTTPInterceptor(config);
    }
}

void ZuneDevice::StopHTTPInterceptor() {
    if (network_manager_) {
        network_manager_->StopHTTPInterceptor();
    }
}

void ZuneDevice::EnableNetworkPolling() {
    if (network_manager_) {
        network_manager_->EnableNetworkPolling();
    }
}

bool ZuneDevice::IsHTTPInterceptorRunning() const {
    if (network_manager_) {
        return network_manager_->IsHTTPInterceptorRunning();
    }
    return false;
}

InterceptorConfig ZuneDevice::GetHTTPInterceptorConfig() const {
    if (network_manager_) {
        return network_manager_->GetHTTPInterceptorConfig();
    }
    return InterceptorConfig{};
}

void ZuneDevice::TriggerNetworkMode() {
    if (network_manager_) {
        network_manager_->TriggerNetworkMode();
    }
}

USBHandlesWithEndpoints ZuneDevice::ExtractUSBHandles() {
    if (network_manager_) {
        return network_manager_->ExtractUSBHandles();
    }
    throw std::runtime_error("Network manager not initialized");
}

void ZuneDevice::SetPathResolverCallback(PathResolverCallback callback, void* user_data) {
    if (network_manager_) {
        network_manager_->SetPathResolverCallback(callback, user_data);
    }
}

void ZuneDevice::SetCacheStorageCallback(CacheStorageCallback callback, void* user_data) {
    if (network_manager_) {
        network_manager_->SetCacheStorageCallback(callback, user_data);
    }
}

void ZuneDevice::SetVerboseNetworkLogging(bool enable) {
    verbose_logging_ = enable;
    if (network_manager_) {
        network_manager_->SetVerboseNetworkLogging(enable);
    }
}

