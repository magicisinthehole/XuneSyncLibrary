#include "ZuneDevice.h"
#include "ZuneDeviceIdentification.h"
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
#include <random>
#include <mtp/ptp/ObjectPropertyListParser.h>
#include <cli/PosixStreams.h>
#include <unordered_map>
#include <sys/stat.h>
#include "NetworkManager.h"
#include "ZuneMtpReader.h"
#include "ZuneMtpWriter.h"
#include <mtp/mtpz/TrustedApp.h>



using namespace mtp;

// === Helper Functions ===

// Checks that the MTPZ data file exists at the given path.
static bool MtpzDataExists(const std::string& path, std::function<void(const std::string&)> log) {
    struct stat st;
    if (stat(path.c_str(), &st) == 0 && st.st_size > 0) {
        return true;
    }
    log("MTPZ data file not found at " + path);
    return false;
}



// --- Embedded Phase 1 Property Data ---
static const uint8_t prop_d230_data[] = { 0x7a, 0x24, 0xec, 0x12, 0x00, 0x00, 0x00, 0x00 };
static const uint8_t prop_d229_data[] = { 0xf4, 0x00, 0x00, 0x00 };
static const uint8_t prop_d22a_data[] = { 0x5c, 0xfe, 0xff, 0xff };

// --- UUID v4 Generator ---
static std::string GenerateUUID() {
    static std::random_device rd;
    static std::mt19937_64 gen(rd());
    static std::uniform_int_distribution<uint64_t> dist;

    uint64_t ab = dist(gen);
    uint64_t cd = dist(gen);

    // Set version to 4 (random UUID) - bits 12-15 of time_hi_and_version
    ab = (ab & 0xFFFFFFFFFFFF0FFFULL) | 0x0000000000004000ULL;
    // Set variant to RFC 4122 - bits 6-7 of clock_seq_hi_and_reserved
    cd = (cd & 0x3FFFFFFFFFFFFFFFULL) | 0x8000000000000000ULL;

    std::ostringstream ss;
    ss << std::hex << std::uppercase << std::setfill('0');
    ss << "{";
    ss << std::setw(8) << ((ab >> 32) & 0xFFFFFFFF) << "-";
    ss << std::setw(4) << ((ab >> 16) & 0xFFFF) << "-";
    ss << std::setw(4) << (ab & 0xFFFF) << "-";
    ss << std::setw(4) << ((cd >> 48) & 0xFFFF) << "-";
    ss << std::setw(12) << (cd & 0xFFFFFFFFFFFFULL);
    ss << "}";
    return ss.str();
}



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
        if (!mtpz_data_path_.empty()) {
            if (!MtpzDataExists(mtpz_data_path_, [this](const std::string& msg) { this->Log(msg); })) {
                Log("  ⚠ MTPZ keys unavailable — device may deny data read operations");
            }
        }
        cli_session_ = std::make_shared<cli::Session>(mtp_session_, false, mtpz_data_path_);
        Log("  ✓ MTPZ session initialized");

        // Initialize NetworkManager
        network_manager_ = std::make_unique<NetworkManager>(mtp_session_, [this](const std::string& msg) {
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
    Log("Wireless connection is not yet implemented.");
    return false;
}

void ZuneDevice::Disconnect() {
    if (network_manager_) {
        network_manager_->RequestShutdown();
        network_manager_.reset();
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

bool ZuneDevice::ValidateConnection() {
    if (!mtp_session_ || !device_) {
        return false;
    }

    try {
        // Lightweight MTP operation to verify session is still valid
        // GetInfo() queries basic device info without affecting device state
        device_->GetInfo();
        return true;
    } catch (const std::exception& e) {
        Log("ValidateConnection: MTP operation failed - " + std::string(e.what()));
        return false;
    } catch (...) {
        Log("ValidateConnection: Unknown exception during MTP operation");
        return false;
    }
}

void ZuneDevice::SetMtpzDataPath(const std::string& path) {
    mtpz_data_path_ = path;
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
        Log("Setting MTP driver version...");
        std::string driver_str = "macOS/11.0 ZuneWirelessSync/1.0.0";
        ByteArray driver_data;
        OutputStream stream(driver_data);
        stream << driver_str;
        mtp_session_->SetDeviceProperty((DeviceProperty)0xd406, driver_data);
        Log("  ✓ Property 0xd406 set");

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

        Log("Querying pairing property descriptors...");
        try { mtp_session_->GetDevicePropertyDesc((DeviceProperty)0xd231); } catch (const std::exception& e) { Log("  → Failed to query 0xd231 (non-critical): " + std::string(e.what())); }
        try { mtp_session_->GetDevicePropertyDesc((DeviceProperty)0xd232); } catch (const std::exception& e) { Log("  → Failed to query 0xd232 (non-critical): " + std::string(e.what())); }
        try { mtp_session_->GetDevicePropertyDesc((DeviceProperty)0xd21c); } catch (const std::exception& e) { Log("  → Failed to query 0xd21c (non-critical): " + std::string(e.what())); }
        try { mtp_session_->GetDevicePropertyDesc((DeviceProperty)0xd225); } catch (const std::exception& e) { Log("  → Failed to query 0xd225 (non-critical): " + std::string(e.what())); }
        try { mtp_session_->GetDevicePropertyDesc((DeviceProperty)0xd401); } catch (const std::exception& e) { Log("  → Failed to query 0xd401 (non-critical): " + std::string(e.what())); }
        Log("  ✓ Queried pairing descriptors");

        Log("Setting pairing properties...");

        try { mtp_session_->GetDevicePropertyDesc((DeviceProperty)0xd22c); } catch (const std::exception& e) { Log("  → Failed to query 0xd22c (non-critical): " + std::string(e.what())); }
        Log("  → Queried descriptor 0xd22c");

        // Generate a new sync partner GUID for this pairing
        std::string sync_partner_guid = GenerateUUID();
        Log("  Generated new sync partner GUID: " + sync_partner_guid);

        cli_session_->SetDeviceProp("d225", "{00000000-0000-0000-0000-000000000000}");
        Log("  ✓ Set property 0xd225 (Null GUID)");

        ByteArray prop_d21c = {0x00};
        mtp_session_->SetDeviceProperty((DeviceProperty)0xd21c, prop_d21c);
        Log("  ✓ Set property 0xd21c");

        cli_session_->SetDeviceProp("d401", sync_partner_guid);
        Log("  ✓ Set property 0xd401 (SynchronizationPartner) = " + sync_partner_guid + " ⭐ KEY PROPERTY");

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

int ZuneDevice::EraseAllContent() {
    if (!IsConnected()) {
        Log("Error: Not connected to a device.");
        return -1;
    }
    
    try {
        Log("WARNING: Erasing all content on device...");
        
        // Get the default storage ID (typically 0x00010001)
        uint32_t storageId = GetDefaultStorageId();
        if (storageId == 0) {
            Log("Error: Could not find default storage on device.");
            return -2;
        }

        Log("Executing FormatStore operation...");
        mtp_session_->FormatStore(mtp::StorageId(storageId), 0);
        Log("FormatStore completed (device content erased)");

        Log("Checking device state...");
        for (int i = 0; i < 2; i++) {
            try {
                auto prop = mtp_session_->GetDeviceProperty(mtp::DeviceProperty(0xd217));
                // Check if we got the expected value (0x7E) seen in captures
                if (prop.size() >= 4) {
                    uint32_t value = 0;
                    std::memcpy(&value, prop.data(), 4);
                    std::stringstream ss;
                    ss << "Property 0xd217 query " << (i+1) 
                       << " returned: 0x" << std::hex << value
                       << " (" << std::dec << value << ")";
                    VerboseLog(ss.str());
                } else {
                    VerboseLog("Property 0xd217 query " + std::to_string(i+1) + " complete");
                }
            } catch (const std::exception& e) {
                VerboseLog("Property 0xd217 not available: " + std::string(e.what()));
            }
        }

        Log("Executing device finalization...");
        mtp_session_->Operation9217(1);
        Log("Device finalization complete");

        Log("Verifying storage state...");
        auto storage_info = mtp_session_->GetStorageInfo(mtp::StorageId(storageId));
        Log("Storage verified - Free space: " + std::to_string(storage_info.FreeSpaceInBytes / 1024 / 1024) + " MB");

        Log("Performing final device state check...");
        try {
            auto prop = mtp_session_->GetDeviceProperty(mtp::DeviceProperty(0xd217));
            if (prop.size() >= 4) {
                uint32_t value = 0;
                std::memcpy(&value, prop.data(), 4);
                std::stringstream ss;
                ss << "Property 0xd217 final query returned: 0x" << std::hex << value
                   << " (" << std::dec << value << ")";
                VerboseLog(ss.str());
            }
        } catch (const std::exception& e) {
            VerboseLog("Property 0xd217 final query failed: " + std::string(e.what()));
        }

        Log("Rebooting device...");
        try {
            mtp_session_->RebootDevice();
            // Note: This command will not receive a response as the device is rebooting
        } catch (const std::exception& e) {
            // Expected - device won't respond after reboot command
            VerboseLog("RebootDevice exception (expected): " + std::string(e.what()));
        }
        
        Log("Device is rebooting. The device will be unavailable for a few seconds.");
        
        // Clear cached data since device content has been erased
        ClearTrackObjectIdCache();

        // Full disconnect — device is rebooting
        Disconnect();
        
        return 0;
    } catch (const std::exception& e) {
        Log("Error during erase operation: " + std::string(e.what()));
        return -2;
    }
}

std::string ZuneDevice::GetSyncPartnerGuid() {
    if (!IsConnected()) {
        Log("Error: Not connected to a device.");
        return "";
    }
    try {
        ByteArray guid_data = mtp_session_->GetDeviceProperty((DeviceProperty)0xd401);
        std::string guid = Utf16leToAscii(guid_data, true);  // is_guid=true for proper formatting
        // Ensure GUID has braces
        if (!guid.empty() && guid[0] != '{') {
            guid = "{" + guid + "}";
        }
        // Convert to uppercase for consistency
        std::transform(guid.begin(), guid.end(), guid.begin(), ::toupper);
        return guid;
    } catch (const std::exception& e) {
        Log("Error getting sync partner GUID: " + std::string(e.what()));
        return "";
    }
}

int ZuneDevice::SetDeviceName(const std::string& name) {
    if (name.empty()) {
        Log("Error: Device name cannot be empty.");
        return -1;
    }
    if (!IsConnected()) {
        Log("Error: Not connected to a device.");
        return -2;
    }
    try {
        std::string truncated = name.size() > 255 ? name.substr(0, 255) : name;
        ByteArray name_data;
        OutputStream stream(name_data);
        stream << truncated;
        mtp_session_->SetDeviceProperty((DeviceProperty)0xd402, name_data);
        Log("Device name set to: " + truncated);
        return 0;
    } catch (const std::exception& e) {
        Log("Error setting device name: " + std::string(e.what()));
        return -3;
    }
}

const char* ZuneDevice::GetSyncPartnerGuidCached() {
    auto result = GetSyncPartnerGuid();
    std::lock_guard<std::mutex> lock(cache_mutex_);
    cached_sync_partner_guid_ = std::move(result);
    return cached_sync_partner_guid_.c_str();
}

std::vector<std::string> ZuneDevice::ScanWiFiNetworks() {
    if (!IsConnected()) {
        Log("Error: Not connected to a device.");
        return {};
    }
    try {
        Log("Scanning for WiFi networks...");
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

uint64_t ZuneDevice::GetStorageCapacityBytes() {
    if (!IsConnected()) {
        Log("Error: Not connected to a device.");
        return 0;
    }

    try {
        auto storage_ids = mtp_session_->GetStorageIDs();
        if (storage_ids.StorageIDs.empty()) {
            Log("Error: No storage found on device.");
            return 0;
        }

        // Get capacity from first (primary) storage
        auto storage_info = mtp_session_->GetStorageInfo(storage_ids.StorageIDs[0]);
        return storage_info.MaxCapacity;
    } catch (const std::exception& e) {
        Log("Error getting storage capacity: " + std::string(e.what()));
        return 0;
    }
}

uint64_t ZuneDevice::GetStorageFreeBytes() {
    if (!IsConnected()) {
        Log("Error: Not connected to a device.");
        return 0;
    }

    try {
        auto storage_ids = mtp_session_->GetStorageIDs();
        if (storage_ids.StorageIDs.empty()) {
            Log("Error: No storage found on device.");
            return 0;
        }

        // Get free space from first (primary) storage
        auto storage_info = mtp_session_->GetStorageInfo(storage_ids.StorageIDs[0]);
        return storage_info.FreeSpaceInBytes;
    } catch (const std::exception& e) {
        Log("Error getting storage free space: " + std::string(e.what()));
        return 0;
    }
}

bool ZuneDevice::SupportsNetworkMode() {
    if (!IsConnected()) {
        return false;
    }

    // Only Pavo (Zune HD) supports network mode - determined by device family
    CacheDeviceIdentification();
    return zune::FamilySupportsNetworkMode(cached_device_ident_.family);
}

// C API cached helpers - these cache results and return pointers that remain valid
// until the next call to the same function or until the device object is destroyed
const char* ZuneDevice::GetNameCached() {
    auto result = GetName();
    std::lock_guard<std::mutex> lock(cache_mutex_);
    cached_name_ = std::move(result);
    return cached_name_.c_str();
}

const char* ZuneDevice::GetSerialNumberCached() {
    auto result = GetSerialNumber();
    std::lock_guard<std::mutex> lock(cache_mutex_);
    cached_serial_number_ = std::move(result);
    return cached_serial_number_.c_str();
}

const char* ZuneDevice::GetSessionGuidCached() {
    std::lock_guard<std::mutex> lock(cache_mutex_);
    cached_session_guid_ = session_guid_;
    return cached_session_guid_.c_str();
}

// ============================================================================
// Device Identification (from MTP property 0xd21a)
// ============================================================================

void ZuneDevice::CacheDeviceIdentification() const {
    if (device_ident_cached_) {
        return;
    }

    if (!mtp_session_) {
        // No MTP session - leave as Unknown
        return;
    }

    try {
        // Read MTP property 0xd21a (device identification)
        // All Zune models support this property
        ByteArray data = mtp_session_->GetDeviceProperty(static_cast<DeviceProperty>(0xd21a));

        if (data.size() >= 4) {
            // Build 32-bit value from little-endian bytes
            uint32_t raw = static_cast<uint32_t>(data[0]) |
                          (static_cast<uint32_t>(data[1]) << 8) |
                          (static_cast<uint32_t>(data[2]) << 16) |
                          (static_cast<uint32_t>(data[3]) << 24);

            cached_device_ident_ = zune::ParseDeviceIdentification(raw);
            device_ident_cached_ = true;

            // Log the identification result
            if (log_callback_) {
                std::ostringstream ss;
                ss << "Device identification: Family=" << cached_device_ident_.family_name
                   << ", Color=" << cached_device_ident_.color_name
                   << " (ID=" << static_cast<int>(cached_device_ident_.color_id) << ")";
                log_callback_(ss.str());
            }
        }
    } catch (const std::exception& e) {
        if (log_callback_) {
            log_callback_("Failed to read device identification (0xd21a): " + std::string(e.what()));
        }
    }
}

zune::DeviceFamily ZuneDevice::GetDeviceFamily() {
    CacheDeviceIdentification();
    return cached_device_ident_.family;
}

uint8_t ZuneDevice::GetDeviceColorId() {
    CacheDeviceIdentification();
    return cached_device_ident_.color_id;
}

std::string ZuneDevice::GetDeviceColorName() {
    CacheDeviceIdentification();
    return cached_device_ident_.color_name;
}

std::string ZuneDevice::GetDeviceFamilyName() {
    CacheDeviceIdentification();
    return cached_device_ident_.family_name;
}

const char* ZuneDevice::GetDeviceFamilyNameCached() {
    auto result = GetDeviceFamilyName();
    std::lock_guard<std::mutex> lock(cache_mutex_);
    cached_family_name_ = std::move(result);
    return cached_family_name_.c_str();
}

const char* ZuneDevice::GetDeviceColorNameCached() {
    auto result = GetDeviceColorName();
    std::lock_guard<std::mutex> lock(cache_mutex_);
    cached_color_name_ = std::move(result);
    return cached_color_name_.c_str();
}

// ============================================================================
// MTP Read Operations (via ZuneMtpReader primitives)
// ============================================================================

ZuneMusicLibrary* ZuneDevice::GetMusicLibrary() {
    if (!mtp_session_) return nullptr;
    return zune::MtpReader::ReadMusicLibrary(mtp_session_, GetDeviceFamily());
}

int ZuneDevice::DownloadFile(uint32_t object_handle, const std::string& destination_path) {
    if (!mtp_session_) return -1;
    return zune::MtpReader::DownloadArtwork(mtp_session_, object_handle, destination_path);
}

int ZuneDevice::DeleteFile(uint32_t object_handle) {
    if (!mtp_session_) return -1;
    return zune::MtpWriter::DeleteObject(mtp_session_, object_handle);
}

uint32_t ZuneDevice::CreatePlaylist(
    const std::string& name,
    const std::string& guid,
    const std::vector<uint32_t>& track_mtp_ids,
    uint32_t playlists_folder_id
) {
    if (!mtp_session_) return 0;
    return zune::MtpWriter::CreatePlaylist(
        mtp_session_, GetDefaultStorageId(), playlists_folder_id,
        name, guid, track_mtp_ids.data(), track_mtp_ids.size());
}

bool ZuneDevice::UpdatePlaylistTracks(
    uint32_t playlist_mtp_id,
    const std::vector<uint32_t>& track_mtp_ids
) {
    if (!mtp_session_) return false;
    return zune::MtpWriter::UpdatePlaylistTracks(
        mtp_session_, playlist_mtp_id,
        track_mtp_ids.data(), track_mtp_ids.size());
}

bool ZuneDevice::DeletePlaylist(uint32_t playlist_mtp_id) {
    if (!mtp_session_) return false;
    return zune::MtpWriter::DeletePlaylist(mtp_session_, playlist_mtp_id);
}

mtp::ByteArray ZuneDevice::GetPartialObject(uint32_t object_id, uint64_t offset, uint32_t size) {
    if (!mtp_session_) return mtp::ByteArray();
    return zune::MtpReader::GetPartialObject(mtp_session_, object_id, offset, size);
}

uint64_t ZuneDevice::GetObjectSize(uint32_t object_id) {
    if (!mtp_session_) return 0;
    return zune::MtpReader::GetObjectSize(mtp_session_, object_id);
}

std::string ZuneDevice::GetObjectFilename(uint32_t object_id) {
    if (!mtp_session_) return "";
    return zune::MtpReader::GetObjectFilename(mtp_session_, object_id);
}

uint32_t ZuneDevice::GetAudioTrackObjectId(const std::string& track_title, uint32_t album_object_id) {
    if (!mtp_session_ || track_title.empty() || album_object_id == 0) return 0;

    // Check cache first
    std::string cache_key = std::to_string(album_object_id) + ":" + track_title;
    {
        std::lock_guard<std::mutex> lock(track_cache_mutex_);
        auto it = track_objectid_cache_.find(cache_key);
        if (it != track_objectid_cache_.end())
            return it->second;
    }

    // Cache miss — query MTP and cache all siblings from the same album
    std::vector<zune::TrackReference> siblings;
    uint32_t found_id = zune::MtpReader::FindTrackObjectId(
        mtp_session_, track_title, album_object_id, &siblings);

    {
        std::lock_guard<std::mutex> lock(track_cache_mutex_);
        for (const auto& sibling : siblings) {
            std::string key = std::to_string(album_object_id) + ":" + sibling.name;
            track_objectid_cache_.emplace(key, sibling.object_id);
        }
    }

    return found_id;
}

void ZuneDevice::ClearTrackObjectIdCache() {
    std::lock_guard<std::mutex> lock(track_cache_mutex_);
    track_objectid_cache_.clear();
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

    if (play_count >= 0) {
        Log("  [DISABLED] play_count update - pending protocol analysis");
    }

    if (skip_count >= 0) {
        Log("  [DISABLED] skip_count update - property not supported");
    }

    if (rating >= 0) {
        try {
            // UserRating (0xDC8A) expects Uint16 (2 bytes, little-endian)
            mtp::ByteArray ratingData(2);
            ratingData[0] = static_cast<uint8_t>(rating & 0xFF);
            ratingData[1] = static_cast<uint8_t>((rating >> 8) & 0xFF);

            mtp_session_->SetObjectProperty(
                mtp::ObjectId(zmdb_atom_id),
                mtp::ObjectProperty::UserRating,
                ratingData
            );
            Log("  Rating set to " + std::to_string(rating) + " via SetObjectProperty(UserRating) as Uint16 - SUCCESS");
            return 0;
        } catch (const std::exception& e) {
            Log("  Rating update FAILED: " + std::string(e.what()));
            return -1;
        }
    }

    return 0;
}

mtp::ByteArray ZuneDevice::GetZuneMetadata(const std::vector<uint8_t>& object_id) {
    if (mtp_session_) {
        return zune::MtpReader::ReadZuneMetadata(mtp_session_, object_id);
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

int ZuneDevice::PollNetworkData(int timeout_ms) {
    if (network_manager_) {
        return network_manager_->PollNetworkData(timeout_ms);
    }
    return -1;
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

// === Low-Level MTP Access Methods ===

uint32_t ZuneDevice::GetDefaultStorageId() {
    if (!mtp_session_) {
        return 0;
    }

    try {
        auto storages = mtp_session_->GetStorageIDs();
        if (storages.StorageIDs.empty()) {
            return 0;
        }
        return storages.StorageIDs[0].Id;
    } catch (const std::exception& e) {
        Log("GetDefaultStorageId failed: " + std::string(e.what()));
        return 0;
    }
}

bool ZuneDevice::ReadNetworkState(int32_t& active, int32_t& progress, int32_t& phase, int32_t& status) {
    if (!IsConnected() || !mtp_session_) return false;

    try {
        ByteArray empty;
        ByteArray response = mtp_session_->Operation922f(empty);
        if (response.size() < 16) {
            Log("ReadNetworkState: response too short (" + std::to_string(response.size()) + " bytes)");
            return false;
        }
        if (response.size() != 1036) {
            Log("ReadNetworkState: unexpected response size (" + std::to_string(response.size()) + " bytes, expected 1036)");
        }

        // Extract 4 little-endian uint32 values at offsets 0, 4, 8, 12
        auto read_le32 = [](const u8* p) -> int32_t {
            return static_cast<int32_t>(p[0] | (p[1] << 8) | (p[2] << 16) | (p[3] << 24));
        };
        active   = read_le32(response.data());
        progress = read_le32(response.data() + 4);
        phase    = read_le32(response.data() + 8);
        status   = read_le32(response.data() + 12);
        return true;
    } catch (const std::exception& e) {
        Log("ReadNetworkState failed: " + std::string(e.what()));
        return false;
    }
}

bool ZuneDevice::TeardownNetworkSession() {
    if (!IsConnected() || !mtp_session_) return false;

    try {
        mtp_session_->Operation9230(2);  // END
        mtp_session_->Operation922b(3, 2, 0);  // Close session
        return true;
    } catch (const std::exception& e) {
        Log("TeardownNetworkSession failed: " + std::string(e.what()));
        return false;
    }
}

bool ZuneDevice::EnableTrustedFiles() {
    if (!IsConnected() || !cli_session_) return false;

    try {
        auto trustedApp = cli_session_->GetTrustedApp();
        if (!trustedApp) {
            Log("EnableTrustedFiles: TrustedApp not available");
            return false;
        }
        trustedApp->EnableTrustedFiles();
        return true;
    } catch (const std::exception& e) {
        Log("EnableTrustedFiles failed: " + std::string(e.what()));
        return false;
    }
}


bool ZuneDevice::DisableTrustedFiles() {
    if (!IsConnected() || !cli_session_) return false;

    try {
        auto trustedApp = cli_session_->GetTrustedApp();
        if (!trustedApp) {
            Log("DisableTrustedFiles: TrustedApp not available");
            return false;
        }
        trustedApp->DisableTrustedFiles();
        return true;
    } catch (const std::exception& e) {
        Log("DisableTrustedFiles failed: " + std::string(e.what()));
        return false;
    }
}

