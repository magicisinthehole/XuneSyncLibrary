#include "ZuneDeviceIdentification.h"
#include <unordered_map>

namespace zune {

// Color lookup tables extracted from ZuneNativeLib.dll via Ghidra decompilation
// See ZuneProtocol/ZuneDeviceIdentification.md for full documentation

// KEEL (FamilyID = 0) - Zune 30
static const std::unordered_map<uint8_t, const char*> kKeelColors = {
    {1, "White"},
    {2, "Black"},
    {3, "Brown"}
};

// SCORPIUS (FamilyID = 2) - Zune 4/8/16 (flash-based)
static const std::unordered_map<uint8_t, const char*> kScorpiusColors = {
    {2,  "Black"},
    {4,  "Pink"},
    {5,  "Camo"},
    {6,  "Red"},
    {7,  "Citron"},
    {20, "BlackBlack"},  // 0x14
    {22, "Blue"},        // 0x16
    {24, "RedBlackBack"},// 0x18
    {25, "White"}        // 0x19
};

// DRACO (FamilyID = 3) - Zune 80/120 (2nd gen HDD)
static const std::unordered_map<uint8_t, const char*> kDracoColors = {
    {2,  "Black"},
    {6,  "Red"},
    {20, "BlackBlack"},  // 0x14
    {21, "Black"},       // 0x15
    {22, "BlueSilver"},  // 0x16
    {23, "BlackBlack"},  // 0x17
    {24, "RedBlack"},    // 0x18
    {25, "WhiteSilver"}, // 0x19
    {26, "BlueBlack"},   // 0x1a
    {27, "WhiteBlack"},  // 0x1b
    {28, "BlackBlack"}   // 0x1c
};

// PAVO (FamilyID = 6) - Zune HD
// Note: Pavo uses device byte0 directly (verified: byte0=1 -> Platinum)
static const std::unordered_map<uint8_t, const char*> kPavoColors = {
    {0, "Black"},
    {1, "Platinum"},
    {3, "Pink"},
    {4, "Red"},
    {5, "Blue"},
    {6, "Purple"},
    {7, "Magenta"},
    {8, "Citron"},
    {9, "Atomic"}
};

const char* GetColorName(DeviceFamily family, uint8_t color_id) {
    const std::unordered_map<uint8_t, const char*>* color_map = nullptr;

    switch (family) {
        case DeviceFamily::Keel:
            color_map = &kKeelColors;
            break;
        case DeviceFamily::Scorpius:
            color_map = &kScorpiusColors;
            break;
        case DeviceFamily::Draco:
            color_map = &kDracoColors;
            break;
        case DeviceFamily::Pavo:
            color_map = &kPavoColors;
            break;
        default:
            return "Unknown";
    }

    auto it = color_map->find(color_id);
    if (it != color_map->end()) {
        return it->second;
    }
    return "Unknown";
}

const char* GetFamilyName(DeviceFamily family) {
    switch (family) {
        case DeviceFamily::Keel:     return "Keel (1st Gen)";
        case DeviceFamily::Scorpius: return "Scorpius (2nd Gen Flash)";
        case DeviceFamily::Draco:    return "Draco (2nd Gen HDD)";
        case DeviceFamily::Pavo:     return "Pavo (HD)";
        default:                     return "Unknown";
    }
}

bool FamilySupportsNetworkMode(DeviceFamily family) {
    // Only Pavo (Zune HD) has the USB endpoints required for
    // HTTP-based artist metadata proxy via network mode
    return family == DeviceFamily::Pavo;
}

DeviceIdentification ParseDeviceIdentification(uint32_t raw_d21a_value) {
    DeviceIdentification result;

    // 0xd21a byte layout (little-endian):
    // Byte 0 (LSB): ColorID
    // Byte 1: Unknown (always 0x00 in observed samples)
    // Byte 2: Unknown (varies)
    // Byte 3 (MSB): FamilyID

    uint8_t family_byte = (raw_d21a_value >> 24) & 0xFF;
    uint8_t color_id = raw_d21a_value & 0xFF;

    // Map family byte to enum
    switch (family_byte) {
        case 0: result.family = DeviceFamily::Keel; break;
        case 2: result.family = DeviceFamily::Scorpius; break;
        case 3: result.family = DeviceFamily::Draco; break;
        case 6: result.family = DeviceFamily::Pavo; break;
        default: result.family = DeviceFamily::Unknown; break;
    }

    result.color_id = color_id;
    result.family_name = GetFamilyName(result.family);
    result.color_name = GetColorName(result.family, color_id);
    result.valid = (result.family != DeviceFamily::Unknown);

    return result;
}

} // namespace zune
