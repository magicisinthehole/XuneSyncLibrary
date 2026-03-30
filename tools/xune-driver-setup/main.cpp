// Elevated helper for WinUSB driver install/restore.
// Runs with requireAdministrator UAC manifest — launched by Xune via ShellExecute.
//
// Usage:
//   xune-driver-setup.exe install <instance-id> [--backup]
//   xune-driver-setup.exe restore <instance-id>
//   xune-driver-setup.exe status  <instance-id>
//
// Exit codes: 0=success, 1=device not found, 2=driver op failed,
//             3=libwdi error, 4=bad arguments

#include <windows.h>
#include <setupapi.h>
#include <cfgmgr32.h>
#include <shlobj.h>
#include <initguid.h>
#include <usbiodef.h>

#include <libwdi.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <fstream>
#include <algorithm>

#pragma comment(lib, "setupapi.lib")
#pragma comment(lib, "cfgmgr32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "newdev.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "advapi32.lib")

// ── Backup path ──────────────────────────────────────────────────────────

static std::string GetBackupDir()
{
    char appData[MAX_PATH]{};
    if (!SHGetSpecialFolderPathA(nullptr, appData, CSIDL_LOCAL_APPDATA, TRUE))
        return "";
    std::string dir = std::string(appData) + "\\Xune\\driver-backup\\";
    CreateDirectoryA((std::string(appData) + "\\Xune").c_str(), nullptr);
    CreateDirectoryA(dir.c_str(), nullptr);
    return dir;
}

static std::string BackupFilePath(const std::string& instanceId)
{
    std::string filename = instanceId;
    std::replace(filename.begin(), filename.end(), '\\', '_');
    return GetBackupDir() + filename + ".backup";
}

// ── Backup / Restore helpers ─────────────────────────────────────────────

static bool SaveBackup(const std::string& instanceId,
                       const std::string& driverName,
                       const std::string& mfgName)
{
    auto path = BackupFilePath(instanceId);
    std::ofstream f(path);
    if (!f) return false;
    f << "DRIVER_NAME=" << driverName << "\n";
    f << "MFG_NAME=" << mfgName << "\n";
    f << "INSTANCE_ID=" << instanceId << "\n";
    return true;
}

struct BackupInfo { std::string driverName, mfgName, instanceId; };

static bool LoadBackup(const std::string& instanceId, BackupInfo& out)
{
    std::ifstream f(BackupFilePath(instanceId));
    if (!f) return false;
    std::string line;
    while (std::getline(f, line))
    {
        auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        auto key = line.substr(0, eq);
        auto val = line.substr(eq + 1);
        if (key == "DRIVER_NAME") out.driverName = val;
        else if (key == "MFG_NAME") out.mfgName = val;
        else if (key == "INSTANCE_ID") out.instanceId = val;
    }
    return !out.driverName.empty();
}

// ── SetupAPI: find device and get current driver info ────────────────────

static bool FindDevice(const std::string& instanceId,
                       std::string& outDriverName, std::string& outMfg)
{
    HDEVINFO devs = SetupDiGetClassDevsA(
        nullptr, "USB", nullptr, DIGCF_ALLCLASSES | DIGCF_PRESENT);
    if (devs == INVALID_HANDLE_VALUE) return false;

    SP_DEVINFO_DATA dd{}; dd.cbSize = sizeof(dd);
    for (DWORD i = 0; SetupDiEnumDeviceInfo(devs, i, &dd); ++i)
    {
        char id[MAX_PATH]{};
        if (!SetupDiGetDeviceInstanceIdA(devs, &dd, id, MAX_PATH, nullptr))
            continue;
        if (_stricmp(id, instanceId.c_str()) != 0)
            continue;

        char desc[256]{};
        SetupDiGetDeviceRegistryPropertyA(devs, &dd, SPDRP_DEVICEDESC,
            nullptr, reinterpret_cast<PBYTE>(desc), sizeof(desc), nullptr);
        outDriverName = desc;

        char mfg[256]{};
        SetupDiGetDeviceRegistryPropertyA(devs, &dd, SPDRP_MFG,
            nullptr, reinterpret_cast<PBYTE>(mfg), sizeof(mfg), nullptr);
        outMfg = mfg;

        SetupDiDestroyDeviceInfoList(devs);
        return true;
    }
    SetupDiDestroyDeviceInfoList(devs);
    return false;
}

// ── Install WinUSB via libwdi ────────────────────────────────────────────

static int DoInstall(const std::string& instanceId, bool backup)
{
    // Backup current driver if requested
    if (backup)
    {
        std::string drvName, mfg;
        if (FindDevice(instanceId, drvName, mfg))
        {
            if (SaveBackup(instanceId, drvName, mfg))
                fprintf(stderr, "Backed up original driver: %s\n", drvName.c_str());
            else
                fprintf(stderr, "Warning: could not save driver backup\n");
        }
    }

    // Enumerate via libwdi
    struct wdi_device_info *list = nullptr;
    struct wdi_options_create_list cl{};
    cl.list_all = TRUE;
    cl.list_hubs = FALSE;
    cl.trim_whitespaces = TRUE;

    int r = wdi_create_list(&list, &cl);
    if (r != WDI_SUCCESS)
    {
        fprintf(stderr, "wdi_create_list failed: %s\n", wdi_strerror(r));
        return 3;
    }

    // Find Zune by VID:PID
    struct wdi_device_info* dev = nullptr;
    for (auto* cur = list; cur; cur = cur->next)
    {
        if (cur->vid == 0x045E && (cur->pid == 0x0710 || cur->pid == 0x063E))
        {
            dev = cur;
            break;
        }
    }
    if (!dev)
    {
        wdi_destroy_list(list);
        fprintf(stderr, "Zune device not found via libwdi\n");
        return 1;
    }

    fprintf(stderr, "Found: %s (VID_%04X&PID_%04X)\n",
            dev->desc ? dev->desc : "Zune", dev->vid, dev->pid);

    // Prepare driver files in temp
    char driverPath[MAX_PATH]{};
    GetTempPathA(MAX_PATH, driverPath);
    strcat_s(driverPath, "xune-winusb-driver");

    struct wdi_options_prepare_driver pd{};
    pd.driver_type = WDI_WINUSB;
    pd.vendor_name = const_cast<char*>("Xune");
    pd.disable_cat = FALSE;
    pd.disable_signing = FALSE;

    r = wdi_prepare_driver(dev, driverPath, "zune_winusb.inf", &pd);
    if (r != WDI_SUCCESS)
    {
        wdi_destroy_list(list);
        fprintf(stderr, "wdi_prepare_driver failed: %s\n", wdi_strerror(r));
        return 3;
    }

    // Install
    struct wdi_options_install_driver id{};
    id.hWnd = nullptr;
    id.install_filter_driver = FALSE;
    id.pending_install_timeout = 60000;

    fprintf(stderr, "Installing WinUSB driver...\n");
    r = wdi_install_driver(dev, driverPath, "zune_winusb.inf", &id);
    wdi_destroy_list(list);

    if (r != WDI_SUCCESS)
    {
        fprintf(stderr, "wdi_install_driver failed: %s\n", wdi_strerror(r));
        return 2;
    }

    fprintf(stderr, "WinUSB driver installed successfully\n");
    return 0;
}

// ── Restore original driver ──────────────────────────────────────────────

static int DoRestore(const std::string& instanceId)
{
    BackupInfo bk;
    if (!LoadBackup(instanceId, bk))
    {
        fprintf(stderr, "No backup found for %s\n", instanceId.c_str());
        return 1;
    }

    HDEVINFO devs = SetupDiGetClassDevsA(
        nullptr, "USB", nullptr, DIGCF_ALLCLASSES | DIGCF_PRESENT);
    if (devs == INVALID_HANDLE_VALUE)
    {
        fprintf(stderr, "SetupDiGetClassDevs failed\n");
        return 2;
    }

    SP_DEVINFO_DATA dd{}; dd.cbSize = sizeof(dd);
    bool found = false;
    for (DWORD i = 0; SetupDiEnumDeviceInfo(devs, i, &dd); ++i)
    {
        char id[MAX_PATH]{};
        if (SetupDiGetDeviceInstanceIdA(devs, &dd, id, MAX_PATH, nullptr) &&
            _stricmp(id, instanceId.c_str()) == 0)
        {
            found = true;
            break;
        }
    }
    if (!found)
    {
        SetupDiDestroyDeviceInfoList(devs);
        fprintf(stderr, "Device not found: %s\n", instanceId.c_str());
        return 1;
    }

    // Build compatible driver list and search for the backed-up driver
    if (!SetupDiBuildDriverInfoList(devs, &dd, SPDIT_COMPATDRIVER))
    {
        SetupDiDestroyDeviceInfoList(devs);
        fprintf(stderr, "SetupDiBuildDriverInfoList failed\n");
        return 2;
    }

    SP_DRVINFO_DATA_A drvInfo{}; drvInfo.cbSize = sizeof(drvInfo);
    bool drvFound = false;
    for (DWORD i = 0; SetupDiEnumDriverInfoA(devs, &dd, SPDIT_COMPATDRIVER, i, &drvInfo); ++i)
    {
        std::string desc(drvInfo.Description);
        std::string mfg(drvInfo.MfgName);
        if (bk.driverName.find(desc) != std::string::npos ||
            bk.driverName.find(mfg) != std::string::npos)
        {
            drvFound = true;
            break;
        }
    }

    if (!drvFound)
    {
        fprintf(stderr, "Original driver not in system database: %s\n", bk.driverName.c_str());
        SetupDiDestroyDriverInfoList(devs, &dd, SPDIT_COMPATDRIVER);
        SetupDiDestroyDeviceInfoList(devs);
        return 2;
    }

    if (!SetupDiSetSelectedDriverA(devs, &dd, &drvInfo) ||
        !SetupDiCallClassInstaller(DIF_INSTALLDEVICE, devs, &dd))
    {
        fprintf(stderr, "Driver restore failed (error %lu)\n", GetLastError());
        SetupDiDestroyDriverInfoList(devs, &dd, SPDIT_COMPATDRIVER);
        SetupDiDestroyDeviceInfoList(devs);
        return 2;
    }

    SetupDiDestroyDriverInfoList(devs, &dd, SPDIT_COMPATDRIVER);
    SetupDiDestroyDeviceInfoList(devs);

    // Delete backup file
    DeleteFileA(BackupFilePath(instanceId).c_str());

    fprintf(stderr, "Original driver restored successfully\n");
    return 0;
}

// ── Status check ─────────────────────────────────────────────────────────

static int DoStatus(const std::string& instanceId)
{
    HDEVINFO devs = SetupDiGetClassDevsA(
        nullptr, "USB", nullptr, DIGCF_ALLCLASSES | DIGCF_PRESENT);
    if (devs == INVALID_HANDLE_VALUE) return 1;

    SP_DEVINFO_DATA dd{}; dd.cbSize = sizeof(dd);
    for (DWORD i = 0; SetupDiEnumDeviceInfo(devs, i, &dd); ++i)
    {
        char id[MAX_PATH]{};
        if (!SetupDiGetDeviceInstanceIdA(devs, &dd, id, MAX_PATH, nullptr))
            continue;
        if (_stricmp(id, instanceId.c_str()) != 0)
            continue;

        char svc[256]{};
        SetupDiGetDeviceRegistryPropertyA(devs, &dd, SPDRP_SERVICE,
            nullptr, reinterpret_cast<PBYTE>(svc), sizeof(svc), nullptr);

        std::string s(svc);
        std::transform(s.begin(), s.end(), s.begin(), ::tolower);

        SetupDiDestroyDeviceInfoList(devs);
        if (s == "winusb") { printf("WinUSB\n"); return 0; }
        if (s == "wudfrd") { printf("OfficialMTP\n"); return 0; }
        printf("Other:%s\n", svc);
        return 0;
    }

    SetupDiDestroyDeviceInfoList(devs);
    printf("NotFound\n");
    return 1;
}

// ── Entry point ──────────────────────────────────────────────────────────

int main(int argc, char* argv[])
{
    if (argc < 3)
    {
        fprintf(stderr,
            "Usage:\n"
            "  xune-driver-setup install <instance-id> [--backup]\n"
            "  xune-driver-setup restore <instance-id>\n"
            "  xune-driver-setup status  <instance-id>\n");
        return 4;
    }

    std::string cmd = argv[1];
    std::string instanceId = argv[2];

    if (cmd == "install")
    {
        bool backup = false;
        for (int i = 3; i < argc; ++i)
            if (std::string(argv[i]) == "--backup") backup = true;
        return DoInstall(instanceId, backup);
    }
    if (cmd == "restore")
        return DoRestore(instanceId);
    if (cmd == "status")
        return DoStatus(instanceId);

    fprintf(stderr, "Unknown command: %s\n", cmd.c_str());
    return 4;
}
