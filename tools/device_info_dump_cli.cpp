/**
 * Device Info Dump CLI - Dumps all MTP device information from a connected Zune
 */

#include <usb/Context.h>
#include <mtp/ptp/Device.h>
#include <mtp/ptp/Session.h>
#include <mtp/ptp/Messages.h>
#include <mtp/ptp/DeviceProperty.h>
#include <iostream>
#include <iomanip>
#include <sstream>
#include <fstream>
#include <map>

std::string format_bytes(uint64_t bytes) {
    const char* units[] = {"B", "KB", "MB", "GB", "TB"};
    int unit_index = 0;
    double size = static_cast<double>(bytes);
    while (size >= 1024.0 && unit_index < 4) {
        size /= 1024.0;
        unit_index++;
    }
    std::stringstream ss;
    ss << std::fixed << std::setprecision(2) << size << " " << units[unit_index];
    return ss.str();
}

// Property name lookup
std::string getPropertyName(uint16_t prop) {
    static std::map<uint16_t, std::string> names = {
        {0x5001, "BatteryLevel"},
        {0x5002, "FunctionalMode"},
        {0xd100, "Unknown_d100"},
        {0xd101, "SecureTime"},
        {0xd102, "DeviceCertificate"},
        {0xd103, "RevocationInfo"},
        {0xd131, "Unknown_d131"},  // From Zune Classic
        {0xd132, "Unknown_d132"},  // From Zune Classic
        {0xd181, "Unknown_d181"},  // From Zune Classic
        {0xd211, "Unknown_d211"},  // From Zune Classic
        {0xd215, "Unknown_d215"},  // From Zune Classic
        {0xd216, "Unknown_d216"},  // From Zune Classic
        {0xd217, "Unknown_d217"},
        {0xd218, "Unknown_d218"},
        {0xd219, "Unknown_d219"},
        {0xd21a, "DeviceIdent"},   // FamilyID/ColorID encoded
        {0xd21b, "Unknown_d21b"},
        {0xd21c, "Unknown_d21c"},
        {0xd21f, "Unknown_d21f"},
        {0xd220, "Unknown_d220"},
        {0xd221, "Unknown_d221"},
        {0xd225, "Unknown_d225"},
        {0xd226, "Unknown_d226"},
        {0xd227, "Unknown_d227"},
        {0xd228, "Unknown_d228"},
        {0xd229, "Unknown_d229"},
        {0xd22a, "Unknown_d22a"},
        {0xd22b, "Unknown_d22b"},
        {0xd22c, "Unknown_d22c"},
        {0xd22d, "Unknown_d22d"},
        {0xd22e, "Unknown_d22e"},
        {0xd22f, "Unknown_d22f"},
        {0xd230, "Unknown_d230"},
        {0xd231, "Unknown_d231"},
        {0xd232, "DeviceCapabilities"},  // Contains device caps bytes
        {0xd233, "FirmwareVersion"},
        {0xd234, "Unknown_d234"},
        {0xd235, "SerialNumber"},
        {0xd401, "SynchronizationPartner"},
        {0xd402, "DeviceFriendlyName"},
        {0xd405, "DeviceIcon"},
        {0xd406, "SessionInitiatorVersionInfo"},
        {0xd501, "Unknown_d501"},
    };
    auto it = names.find(prop);
    if (it != names.end()) return it->second;
    return "";
}

// Decode UTF-16LE to string (skip length byte at start)
std::string decodeUtf16le(const mtp::ByteArray& data, size_t start = 1) {
    std::string result;
    for (size_t i = start; i + 1 < data.size(); i += 2) {
        uint16_t ch = data[i] | (data[i + 1] << 8);
        if (ch == 0) break;
        if (ch < 128) result += static_cast<char>(ch);
        else result += '?';
    }
    return result;
}

// Decode as uint32 little-endian
uint32_t decodeUint32(const mtp::ByteArray& data, size_t offset = 0) {
    if (data.size() < offset + 4) return 0;
    return data[offset] | (data[offset+1] << 8) | (data[offset+2] << 16) | (data[offset+3] << 24);
}

// Decode as ASCII string (with length prefix)
std::string decodeAsciiWithLength(const mtp::ByteArray& data) {
    if (data.size() < 5) return "";
    uint32_t len = decodeUint32(data, 0);
    std::string result;
    for (size_t i = 4; i < data.size() && i < 4 + len; i++) {
        if (data[i] >= 32 && data[i] < 127) result += static_cast<char>(data[i]);
    }
    return result;
}

void printPropertyValue(uint16_t propId, const mtp::ByteArray& value) {
    std::string name = getPropertyName(propId);

    std::cout << "0x" << std::hex << std::setfill('0') << std::setw(4) << propId << std::dec;
    if (!name.empty()) std::cout << " (" << name << ")";
    std::cout << ": ";

    if (value.empty()) {
        std::cout << "(empty)" << std::endl;
        return;
    }

    // Special handling for known properties
    switch (propId) {
        case 0xd402: // DeviceFriendlyName - UTF-16LE string
        case 0xd233: // FirmwareVersion - UTF-16LE string
            std::cout << "\"" << decodeUtf16le(value) << "\"" << std::endl;
            return;

        case 0xd235: // SerialNumber - ASCII with length
            std::cout << "\"" << decodeAsciiWithLength(value) << "\"" << std::endl;
            return;

        case 0xd21b: // Appears to be a string on some devices
            if (value.size() > 2) {
                std::cout << "\"" << decodeUtf16le(value) << "\"" << std::endl;
                return;
            }
            break;

        case 0x5001: // BatteryLevel
            if (value.size() >= 1) {
                std::cout << static_cast<int>(value[0]) << "%" << std::endl;
                return;
            }
            break;

        case 0xd21a: // Device identification - parse FamilyID/ColorID
            if (value.size() >= 4) {
                uint32_t val = decodeUint32(value);
                uint8_t familyId = (val >> 24) & 0xFF;
                uint8_t byte2 = (val >> 16) & 0xFF;
                uint8_t byte1 = (val >> 8) & 0xFF;
                uint8_t colorId = val & 0xFF;

                std::cout << "0x" << std::hex << std::setfill('0') << std::setw(8) << val << std::dec << std::endl;
                std::cout << "         -> FamilyID (byte3): " << static_cast<int>(familyId);
                switch (familyId) {
                    case 0: std::cout << " (Keel/Zune30)"; break;
                    case 2: std::cout << " (Scorpius/Zune4-8-16)"; break;
                    case 3: std::cout << " (Draco/Zune80-120)"; break;
                    case 6: std::cout << " (Pavo/ZuneHD)"; break;
                }
                std::cout << std::endl;
                std::cout << "         -> Byte2: " << static_cast<int>(byte2) << " (0x" << std::hex << static_cast<int>(byte2) << std::dec << ")" << std::endl;
                std::cout << "         -> Byte1: " << static_cast<int>(byte1) << " (0x" << std::hex << static_cast<int>(byte1) << std::dec << ")" << std::endl;
                std::cout << "         -> ColorID (byte0): " << static_cast<int>(colorId) << " (0x" << std::hex << static_cast<int>(colorId) << std::dec << ")" << std::endl;
                return;
            }
            break;

        case 0xd232: // Device capabilities - parse byte structure
            if (value.size() >= 8) {
                std::cout << "[ ";
                for (size_t i = 0; i < value.size(); ++i) {
                    std::cout << std::hex << std::setfill('0') << std::setw(2) << static_cast<int>(value[i]) << " ";
                }
                std::cout << std::dec << "]" << std::endl;
                // Parse known fields if present (based on Zune HD dump)
                if (value.size() >= 8) {
                    uint8_t byte6 = value[6];  // Seen as 0x08 for ZuneHD
                    uint8_t byte7 = value[7];  // Seen as 0x06 for ZuneHD (FamilyID?)
                    std::cout << "         -> Byte[6]: " << static_cast<int>(byte6) << " (0x" << std::hex << static_cast<int>(byte6) << std::dec << ")" << std::endl;
                    std::cout << "         -> Byte[7]: " << static_cast<int>(byte7);
                    switch (byte7) {
                        case 0: std::cout << " (Keel?)"; break;
                        case 2: std::cout << " (Scorpius?)"; break;
                        case 3: std::cout << " (Draco?)"; break;
                        case 6: std::cout << " (Pavo?)"; break;
                    }
                    std::cout << std::dec << std::endl;
                }
                return;
            }
            break;

        case 0xd405: // DeviceIcon - show size and save to file
            std::cout << "(" << value.size() << " bytes) header: [ ";
            for (size_t i = 0; i < std::min(value.size(), (size_t)32); ++i) {
                std::cout << std::hex << std::setfill('0') << std::setw(2) << static_cast<int>(value[i]) << " ";
            }
            std::cout << std::dec << "]" << std::endl;
            // Save to file for analysis
            {
                std::string filename = "device_icon_raw.bin";
                std::ofstream outfile(filename, std::ios::binary);
                if (outfile) {
                    outfile.write(reinterpret_cast<const char*>(value.data()), value.size());
                    outfile.close();
                    std::cout << "         -> Saved raw icon to: " << filename << std::endl;

                    // Also try to find ICO header (00 00 01 00) within the data
                    for (size_t i = 0; i + 4 < value.size(); ++i) {
                        if (value[i] == 0x00 && value[i+1] == 0x00 &&
                            value[i+2] == 0x01 && value[i+3] == 0x00) {
                            std::string icoFilename = "device_icon.ico";
                            std::ofstream icoFile(icoFilename, std::ios::binary);
                            if (icoFile) {
                                icoFile.write(reinterpret_cast<const char*>(value.data() + i), value.size() - i);
                                icoFile.close();
                                std::cout << "         -> Found ICO at offset " << i << ", saved to: " << icoFilename << std::endl;
                            }
                            break;
                        }
                    }
                }
            }
            return;

        case 0xd102: // DeviceCertificate - handled separately
            std::cout << "(see certificate above)" << std::endl;
            return;
    }

    // For 4-byte values, show as uint32
    if (value.size() == 4) {
        uint32_t val = decodeUint32(value);
        std::cout << val << " (0x" << std::hex << val << std::dec << ")" << std::endl;
        return;
    }

    // For 2-byte values, show as uint16
    if (value.size() == 2) {
        uint16_t val = value[0] | (value[1] << 8);
        std::cout << val << " (0x" << std::hex << val << std::dec << ")" << std::endl;
        return;
    }

    // Try UTF-16LE interpretation first
    bool isUtf16 = true;
    std::string strValue;
    if (value.size() >= 2) {
        for (size_t i = 1; i + 1 < value.size(); i += 2) {
            uint16_t ch = value[i] | (value[i + 1] << 8);
            if (ch == 0) break;
            if (ch < 32 || ch > 126) { isUtf16 = false; break; }
            strValue += static_cast<char>(ch);
        }
    }

    if (isUtf16 && !strValue.empty() && strValue.length() > 2) {
        std::cout << "\"" << strValue << "\"" << std::endl;
        return;
    }

    // Hex dump
    std::cout << "[ ";
    for (size_t i = 0; i < value.size() && i < 32; ++i) {
        std::cout << std::hex << std::setfill('0') << std::setw(2) << static_cast<int>(value[i]) << " ";
    }
    if (value.size() > 32) std::cout << "... (" << std::dec << value.size() << " bytes)";
    std::cout << std::dec << "]" << std::endl;
}

int main() {
    std::cout << "=== Zune Device Info Dump ===" << std::endl << std::endl;

    try {
        auto ctx = std::make_shared<mtp::usb::Context>();
        auto devices = ctx->GetDevices();

        for (auto desc : devices) {
            mtp::u16 vendorId = desc->GetVendorId();
            mtp::u16 productId = desc->GetProductId();
            if (vendorId != 0x045E) continue;

            std::cout << "========================================" << std::endl;
            std::cout << "USB: VID=0x" << std::hex << std::setfill('0') << std::setw(4) << vendorId
                      << " PID=0x" << std::setw(4) << productId << std::dec << std::endl;
            std::cout << "========================================" << std::endl;

            try {
                auto device = mtp::Device::Open(ctx, desc, true, false);
                if (!device) continue;

                auto info = device->GetInfo();
                std::cout << std::endl << "MTP DeviceInfo:" << std::endl;
                std::cout << "  Manufacturer:   " << info.Manufacturer << std::endl;
                std::cout << "  Model:          " << info.Model << std::endl;
                std::cout << "  DeviceVersion:  " << info.DeviceVersion << std::endl;
                std::cout << "  SerialNumber:   " << info.SerialNumber << std::endl;
                std::cout << std::endl;

                auto session = device->OpenSession(1);
                if (session) {
                    // Device Certificate
                    std::cout << "Device Certificate:" << std::endl;
                    std::cout << "----------------------------------------" << std::endl;
                    try {
                        auto certData = session->GetDeviceProperty(mtp::DeviceProperty::DeviceCertificate);
                        if (certData.size() > 6) {
                            size_t start = (certData[4] == 0xFF && certData[5] == 0xFE) ? 6 : 4;
                            std::string xml;
                            for (size_t i = start; i + 1 < certData.size(); i += 2) {
                                uint16_t ch = certData[i] | (certData[i + 1] << 8);
                                if (ch == 0) break;
                                if (ch < 128) xml += static_cast<char>(ch);
                            }
                            // Pretty print - add newlines after closing tags
                            for (size_t i = 0; i < xml.size(); i++) {
                                std::cout << xml[i];
                                if (i + 1 < xml.size() && xml[i] == '>' && xml[i+1] == '<') {
                                    std::cout << std::endl;
                                }
                            }
                            std::cout << std::endl;
                        }
                    } catch (...) { std::cout << "(error reading)" << std::endl; }
                    std::cout << std::endl;

                    // Show list of supported properties
                    std::cout << "Supported Device Properties (" << info.DevicePropertiesSupported.size() << " total):" << std::endl;
                    std::cout << "----------------------------------------" << std::endl;
                    std::cout << "[ ";
                    for (size_t i = 0; i < info.DevicePropertiesSupported.size(); ++i) {
                        std::cout << "0x" << std::hex << std::setfill('0') << std::setw(4)
                                  << static_cast<uint16_t>(info.DevicePropertiesSupported[i]) << std::dec;
                        if (i < info.DevicePropertiesSupported.size() - 1) std::cout << ", ";
                        if ((i + 1) % 8 == 0 && i < info.DevicePropertiesSupported.size() - 1) std::cout << std::endl << "  ";
                    }
                    std::cout << " ]" << std::endl << std::endl;

                    // All properties
                    std::cout << "Device Properties:" << std::endl;
                    std::cout << "----------------------------------------" << std::endl;
                    for (const auto& prop : info.DevicePropertiesSupported) {
                        if (prop == mtp::DeviceProperty::DeviceCertificate) continue;
                        try {
                            auto value = session->GetDeviceProperty(prop);
                            printPropertyValue(static_cast<uint16_t>(prop), value);
                        } catch (const std::exception& e) {
                            std::cout << "0x" << std::hex << std::setfill('0') << std::setw(4)
                                      << static_cast<uint16_t>(prop) << std::dec
                                      << ": (error: " << e.what() << ")" << std::endl;
                        }
                    }
                    std::cout << std::endl;

                    // Try additional properties that might contain FamilyId/ColorId
                    std::cout << "Probing Additional Properties:" << std::endl;
                    std::cout << "----------------------------------------" << std::endl;
                    std::vector<uint16_t> probeProps = {
                        // From Part 2.txt (Zune Classic properties)
                        0xd131, 0xd132, 0xd181, 0xd211, 0xd215, 0xd216,
                        // Full d2xx range probe
                        0xd200, 0xd201, 0xd202, 0xd203, 0xd204, 0xd205, 0xd206, 0xd207,
                        0xd208, 0xd209, 0xd20a, 0xd20b, 0xd20c, 0xd20d, 0xd20e, 0xd20f,
                        0xd210, 0xd212, 0xd213, 0xd214,
                        // Skip d217-d235 as they're typically in supported list
                        0xd236, 0xd237, 0xd238, 0xd239, 0xd23a, 0xd23b, 0xd23c, 0xd23d,
                        0xd23e, 0xd23f, 0xd240,
                        // d1xx range (DRM-related)
                        0xd104, 0xd105, 0xd106, 0xd107, 0xd108, 0xd109, 0xd10a,
                        0xd110, 0xd111, 0xd112, 0xd113, 0xd114, 0xd115,
                        0xd130, 0xd133, 0xd134, 0xd135,
                        0xd180, 0xd182, 0xd183, 0xd184, 0xd185,
                        // Additional vendor ranges
                        0xd300, 0xd301, 0xd302, 0xd303,
                        0xd403, 0xd404, 0xd407, 0xd408, 0xd409, 0xd40a,
                        0xd500, 0xd502, 0xd503, 0xd504, 0xd505,
                    };
                    for (uint16_t propId : probeProps) {
                        // Skip if already in supported list
                        bool found = false;
                        for (const auto& p : info.DevicePropertiesSupported) {
                            if (static_cast<uint16_t>(p) == propId) { found = true; break; }
                        }
                        if (found) continue;

                        try {
                            auto value = session->GetDeviceProperty(static_cast<mtp::DeviceProperty>(propId));
                            if (!value.empty()) {
                                printPropertyValue(propId, value);
                            }
                        } catch (...) {
                            // Property not supported - skip silently
                        }
                    }
                    std::cout << std::endl;

                    // Storage
                    std::cout << "Storage:" << std::endl;
                    std::cout << "----------------------------------------" << std::endl;
                    auto storageIds = session->GetStorageIDs();
                    for (const auto& sid : storageIds.StorageIDs) {
                        auto si = session->GetStorageInfo(mtp::StorageId(sid));
                        std::cout << "  ID: 0x" << std::hex << sid << std::dec << std::endl;
                        std::cout << "  Description: " << si.StorageDescription << std::endl;
                        std::cout << "  Capacity: " << format_bytes(si.MaxCapacity) << std::endl;
                        std::cout << "  Free: " << format_bytes(si.FreeSpaceInBytes) << std::endl;
                    }
                }
                std::cout << std::endl;
            } catch (const std::exception& e) {
                std::cerr << "Error: " << e.what() << std::endl;
            }
        }
        std::cout << "Done." << std::endl;
    } catch (const std::exception& e) {
        std::cerr << "Fatal: " << e.what() << std::endl;
        return 1;
    }
    return 0;
}
