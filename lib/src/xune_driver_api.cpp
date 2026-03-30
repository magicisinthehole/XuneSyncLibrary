#include <xune_sync/xune_sync_api.h>

#ifdef _WIN32

#include "driver_manager.h"
#include <vector>

static std::vector<xune::DeviceDriverInfo> g_cachedDevices;

int zune_driver_get_device_count(void)
{
    g_cachedDevices = xune::GetZuneDeviceDriverInfo();
    return static_cast<int>(g_cachedDevices.size());
}

zune_driver_status_t zune_driver_get_status(int index)
{
    if (index < 0 || index >= static_cast<int>(g_cachedDevices.size()))
        return 0;
    return static_cast<int>(g_cachedDevices[index].status);
}

const char* zune_driver_get_status_description(int index)
{
    if (index < 0 || index >= static_cast<int>(g_cachedDevices.size()))
        return "Unknown";
    return xune::DriverStatusDescription(g_cachedDevices[index].status);
}

const char* zune_driver_get_device_instance_id(int index)
{
    if (index < 0 || index >= static_cast<int>(g_cachedDevices.size()))
        return "";
    return g_cachedDevices[index].instanceId.c_str();
}

const char* zune_driver_get_device_description(int index)
{
    if (index < 0 || index >= static_cast<int>(g_cachedDevices.size()))
        return "";
    return g_cachedDevices[index].description.c_str();
}

bool zune_driver_is_winusb_installed(const char* instance_id)
{
    if (!instance_id) return false;
    return xune::IsWinUSBInstalled(instance_id);
}

#else // non-Windows stubs

int zune_driver_get_device_count(void)                      { return 0; }
zune_driver_status_t zune_driver_get_status(int)            { return 0; }
const char* zune_driver_get_status_description(int)         { return "Not supported"; }
const char* zune_driver_get_device_instance_id(int)         { return ""; }
const char* zune_driver_get_device_description(int)         { return ""; }
bool zune_driver_is_winusb_installed(const char*)           { return false; }

#endif
