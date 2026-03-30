#ifdef _WIN32

#include "driver_manager.h"

#include <windows.h>
#include <setupapi.h>
#include <cfgmgr32.h>
#include <initguid.h>
#include <usbiodef.h>
#include <algorithm>

#pragma comment(lib, "setupapi.lib")
#pragma comment(lib, "cfgmgr32.lib")

namespace xune {

// ── Query helpers ────────────────────────────────────────────────────────

static DriverStatus ServiceNameToStatus(const char* serviceName)
{
    std::string svc(serviceName);
    std::transform(svc.begin(), svc.end(), svc.begin(), ::tolower);

    if (svc == "winusb")  return DriverStatus::WinUSB;
    if (svc == "wudfrd")  return DriverStatus::OfficialMTP;
    return DriverStatus::Other;
}

// ── Public API ───────────────────────────────────────────────────────────

std::vector<DeviceDriverInfo> GetZuneDeviceDriverInfo()
{
    std::vector<DeviceDriverInfo> devices;

    HDEVINFO devInfoSet = SetupDiGetClassDevsA(
        nullptr, "USB", nullptr, DIGCF_ALLCLASSES | DIGCF_PRESENT);
    if (devInfoSet == INVALID_HANDLE_VALUE)
        return devices;

    SP_DEVINFO_DATA devData{};
    devData.cbSize = sizeof(SP_DEVINFO_DATA);

    for (DWORD idx = 0; SetupDiEnumDeviceInfo(devInfoSet, idx, &devData); ++idx)
    {
        char hwId[1024]{};
        if (!SetupDiGetDeviceRegistryPropertyA(devInfoSet, &devData,
                SPDRP_HARDWAREID, nullptr, reinterpret_cast<PBYTE>(hwId),
                sizeof(hwId), nullptr))
            continue;

        bool isZune = strstr(hwId, "VID_045E&PID_0710") ||
                      strstr(hwId, "VID_045E&PID_063E");
        if (!isZune)
            continue;

        DeviceDriverInfo info{};

        // Instance ID
        DWORD reqSize = 0;
        SetupDiGetDeviceInstanceIdA(devInfoSet, &devData, nullptr, 0, &reqSize);
        info.instanceId.resize(reqSize);
        SetupDiGetDeviceInstanceIdA(devInfoSet, &devData,
            info.instanceId.data(), reqSize, nullptr);
        // Trim embedded null
        auto nul = info.instanceId.find('\0');
        if (nul != std::string::npos) info.instanceId.erase(nul);

        // Description
        char desc[256]{};
        if (SetupDiGetDeviceRegistryPropertyA(devInfoSet, &devData,
                SPDRP_DEVICEDESC, nullptr, reinterpret_cast<PBYTE>(desc),
                sizeof(desc), nullptr))
            info.description = desc;

        // VID / PID
        if (strstr(hwId, "VID_045E&PID_0710"))
        {
            info.vendorId  = 0x045E;
            info.productId = 0x0710;
        }
        else
        {
            info.vendorId  = 0x045E;
            info.productId = 0x063E;
        }

        // Driver status via SPDRP_SERVICE
        char svcName[256]{};
        if (SetupDiGetDeviceRegistryPropertyA(devInfoSet, &devData,
                SPDRP_SERVICE, nullptr, reinterpret_cast<PBYTE>(svcName),
                sizeof(svcName), nullptr))
            info.status = ServiceNameToStatus(svcName);
        else
            info.status = DriverStatus::Unknown;

        devices.push_back(std::move(info));
    }

    SetupDiDestroyDeviceInfoList(devInfoSet);
    return devices;
}

bool IsWinUSBInstalled(const std::string& instanceId)
{
    HDEVINFO devInfoSet = SetupDiGetClassDevsA(
        nullptr, "USB", nullptr, DIGCF_ALLCLASSES | DIGCF_PRESENT);
    if (devInfoSet == INVALID_HANDLE_VALUE)
        return false;

    SP_DEVINFO_DATA devData{};
    devData.cbSize = sizeof(SP_DEVINFO_DATA);

    for (DWORD idx = 0; SetupDiEnumDeviceInfo(devInfoSet, idx, &devData); ++idx)
    {
        DWORD reqSize = 0;
        SetupDiGetDeviceInstanceIdA(devInfoSet, &devData, nullptr, 0, &reqSize);
        std::string id(reqSize, '\0');
        if (!SetupDiGetDeviceInstanceIdA(devInfoSet, &devData,
                id.data(), reqSize, nullptr))
            continue;
        auto nul = id.find('\0');
        if (nul != std::string::npos) id.erase(nul);

        if (_stricmp(id.c_str(), instanceId.c_str()) != 0)
            continue;

        char svcName[256]{};
        if (SetupDiGetDeviceRegistryPropertyA(devInfoSet, &devData,
                SPDRP_SERVICE, nullptr, reinterpret_cast<PBYTE>(svcName),
                sizeof(svcName), nullptr))
        {
            SetupDiDestroyDeviceInfoList(devInfoSet);
            return ServiceNameToStatus(svcName) == DriverStatus::WinUSB;
        }

        break;
    }

    SetupDiDestroyDeviceInfoList(devInfoSet);
    return false;
}

const char* DriverStatusDescription(DriverStatus status)
{
    switch (status)
    {
        case DriverStatus::WinUSB:      return "WinUSB (compatible)";
        case DriverStatus::OfficialMTP: return "Windows MTP (needs WinUSB)";
        case DriverStatus::Other:       return "Other driver";
        case DriverStatus::NotFound:    return "Device not found";
        default:                        return "Unknown";
    }
}

} // namespace xune

#endif // _WIN32
