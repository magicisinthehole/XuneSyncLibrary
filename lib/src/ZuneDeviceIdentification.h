#pragma once

#include <cstdint>
#include <string>

namespace zune {

/// Device family enumeration (from MTP property 0xd21a byte 3)
/// Values match the Zune firmware's internal family IDs
enum class DeviceFamily : uint8_t {
    Unknown = 0xFF,
    Keel = 0,      // 1st Gen - Zune 30
    Scorpius = 2,  // 2nd Gen Flash - Zune 4/8/16
    Draco = 3,     // 2nd Gen HDD - Zune 80/120
    Pavo = 6       // HD - Zune HD 16/32/64
};

/// Complete device identification parsed from MTP property 0xd21a
struct DeviceIdentification {
    DeviceFamily family = DeviceFamily::Unknown;
    uint8_t color_id = 0;
    std::string family_name;   // "Keel (1st Gen)", "Draco (2nd Gen HDD)", etc.
    std::string color_name;    // "Brown", "Platinum", etc.
    bool valid = false;
};

/// Parse the raw 0xd21a MTP property value into device identification
/// @param raw_d21a_value The 32-bit value from MTP property 0xd21a
/// @return DeviceIdentification with family, color_id, names, and valid flag
DeviceIdentification ParseDeviceIdentification(uint32_t raw_d21a_value);

/// Get color name for a specific family and color ID
/// @return Color name string, or "Unknown" if not found
const char* GetColorName(DeviceFamily family, uint8_t color_id);

/// Get family display name with generation
/// @return Family name with generation ("Keel (1st Gen)", "Draco (2nd Gen HDD)", etc.) or "Unknown"
const char* GetFamilyName(DeviceFamily family);

/// Check if a device family supports network mode (HTTP-based artist metadata)
/// @return true only for Pavo (Zune HD)
bool FamilySupportsNetworkMode(DeviceFamily family);

} // namespace zune
