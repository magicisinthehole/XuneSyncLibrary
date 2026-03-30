#ifndef XUNE_DRIVER_MANAGER_H
#define XUNE_DRIVER_MANAGER_H

#ifdef _WIN32

#include <string>
#include <vector>
#include <cstdint>

namespace xune {

enum class DriverStatus : int
{
    Unknown     = 0,
    OfficialMTP = 1,    // wudfrd — Windows MTP class driver
    WinUSB      = 2,    // WinUSB — compatible with AFTL
    Other       = 3,
    NotFound    = 4
};

struct DeviceDriverInfo
{
    std::string instanceId;
    std::string description;
    uint16_t    vendorId  = 0;
    uint16_t    productId = 0;
    DriverStatus status   = DriverStatus::Unknown;
};

/// Enumerate connected Zune devices and their driver status via SetupAPI.
/// VID 045E: PID 0710 (Zune Classic) and PID 063E (Zune HD).
std::vector<DeviceDriverInfo> GetZuneDeviceDriverInfo();

/// Check whether a specific device instance has WinUSB installed.
bool IsWinUSBInstalled(const std::string& instanceId);

/// Human-readable description of a DriverStatus value.
const char* DriverStatusDescription(DriverStatus status);

} // namespace xune

#endif // _WIN32
#endif // XUNE_DRIVER_MANAGER_H
