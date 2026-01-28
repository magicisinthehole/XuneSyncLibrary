/**
 * Sync Partnership Test CLI - Tests sync partner GUID and device name operations
 *
 * This tool verifies the new GetSyncPartnerGuid and SetDeviceName MTP operations
 * work correctly with a connected Zune device.
 *
 * Usage:
 *   sync_partnership_test_cli
 *
 * Operations tested:
 *   1. Read sync partner GUID (MTP property 0xd401)
 *   2. Read device name (MTP property 0xd402)
 *   3. Optionally set new device name
 *   4. Verify changes
 */

#include "lib/src/ZuneDevice.h"
#include "zune_wireless/zune_wireless_api.h"
#include <iostream>
#include <iomanip>
#include <chrono>
#include <string>

void log_callback(const std::string& message) {
    auto now = std::chrono::system_clock::now();
    auto time = std::chrono::system_clock::to_time_t(now);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()) % 1000;

    std::cout << "[" << std::put_time(std::localtime(&time), "%H:%M:%S")
              << "." << std::setfill('0') << std::setw(3) << ms.count() << "] "
              << message << std::endl;
}

std::string trim(const std::string& str) {
    size_t first = str.find_first_not_of(" \t\n\r\f\v");
    if (first == std::string::npos) return "";
    size_t last = str.find_last_not_of(" \t\n\r\f\v");
    return str.substr(first, last - first + 1);
}

std::string read_input(const std::string& prompt = "") {
    if (!prompt.empty()) {
        std::cout << prompt;
    }
    std::string input;
    std::getline(std::cin, input);
    return trim(input);
}

bool read_yes_no(const std::string& prompt) {
    while (true) {
        std::string input = read_input(prompt + " (y/n): ");
        if (input == "y" || input == "Y") return true;
        if (input == "n" || input == "N") return false;
        std::cout << "Invalid input. Please enter 'y' or 'n'." << std::endl;
    }
}

int main() {
    std::cout << "=== Sync Partnership Test CLI ===" << std::endl;
    std::cout << std::endl;
    std::cout << "This tool tests the GetSyncPartnerGuid and SetDeviceName operations." << std::endl;
    std::cout << std::endl;

    // Step 1: Connect to device
    std::cout << "Connecting to device via USB..." << std::endl;
    ZuneDevice device;
    device.SetLogCallback(log_callback);

    if (!device.ConnectUSB()) {
        std::cerr << "ERROR: Failed to connect to device via USB" << std::endl;
        std::cerr << "Make sure a Zune device is connected." << std::endl;
        return 1;
    }

    std::cout << std::endl;
    std::cout << "========================================" << std::endl;
    std::cout << "Device Info" << std::endl;
    std::cout << "========================================" << std::endl;
    std::cout << "Name:   " << device.GetName() << std::endl;
    std::cout << "Model:  " << device.GetModel() << std::endl;
    std::cout << "Serial: " << device.GetSerialNumber() << std::endl;
    std::cout << std::endl;

    // ===== TEST 1: Read Sync Partner GUID =====
    std::cout << "========================================" << std::endl;
    std::cout << "TEST 1: Read Sync Partner GUID" << std::endl;
    std::cout << "========================================" << std::endl;

    std::string sync_guid = device.GetSyncPartnerGuid();

    if (sync_guid.empty()) {
        std::cout << "Result: EMPTY (error reading property or not connected)" << std::endl;
    } else {
        std::cout << "Sync Partner GUID: " << sync_guid << std::endl;

        // Check if it's the null GUID (no partnership established)
        if (sync_guid == "{00000000-0000-0000-0000-000000000000}") {
            std::cout << "Status: NO SYNC PARTNERSHIP (null GUID)" << std::endl;
            std::cout << "        Device is in 'guest' mode or has never been paired." << std::endl;
        } else {
            std::cout << "Status: SYNC PARTNERSHIP ESTABLISHED" << std::endl;
            std::cout << "        Device has been paired with a host computer." << std::endl;
        }
    }
    std::cout << std::endl;

    // ===== TEST 2: Read Current Device Name =====
    std::cout << "========================================" << std::endl;
    std::cout << "TEST 2: Read Device Name" << std::endl;
    std::cout << "========================================" << std::endl;

    std::string original_name = device.GetName();
    std::cout << "Current device name: \"" << original_name << "\"" << std::endl;
    std::cout << std::endl;

    // ===== TEST 3: Set New Device Name (Optional) =====
    std::cout << "========================================" << std::endl;
    std::cout << "TEST 3: Set Device Name (Optional)" << std::endl;
    std::cout << "========================================" << std::endl;

    if (!read_yes_no("Do you want to test changing the device name?")) {
        std::cout << "Skipping SetDeviceName test." << std::endl;
    } else {
        std::string new_name = read_input("Enter new device name (or press Enter for default test name): ");

        if (new_name.empty()) {
            new_name = "Test Zune " + std::to_string(std::time(nullptr) % 10000);
        }

        std::cout << std::endl;
        std::cout << "Changing device name from \"" << original_name << "\" to \"" << new_name << "\"..." << std::endl;

        int result = device.SetDeviceName(new_name);

        if (result == 0) {
            std::cout << "SUCCESS: SetDeviceName returned 0" << std::endl;

            // Verify the change by re-reading
            std::string verified_name = device.GetName();
            std::cout << "Verification: Device name is now \"" << verified_name << "\"" << std::endl;

            if (verified_name == new_name) {
                std::cout << "VERIFIED: Name change confirmed!" << std::endl;
            } else {
                std::cout << "WARNING: Name may not have updated yet (cached?)" << std::endl;
            }

            // Offer to restore original name
            std::cout << std::endl;
            if (read_yes_no("Restore original device name (\"" + original_name + "\")?")) {
                int restore_result = device.SetDeviceName(original_name);
                if (restore_result == 0) {
                    std::cout << "SUCCESS: Original name restored" << std::endl;
                } else {
                    std::cerr << "FAILED: Could not restore original name (error " << restore_result << ")" << std::endl;
                }
            }
        } else {
            std::cerr << "FAILED: SetDeviceName returned error code " << result << std::endl;
            switch (result) {
                case -1:
                    std::cerr << "  Error: Invalid input (empty name)" << std::endl;
                    break;
                case -2:
                    std::cerr << "  Error: Device not connected" << std::endl;
                    break;
                case -3:
                    std::cerr << "  Error: MTP operation failed" << std::endl;
                    break;
                default:
                    std::cerr << "  Error: Unknown error" << std::endl;
                    break;
            }
        }
    }
    std::cout << std::endl;

    // ===== TEST 4: Establish New Sync Partnership (Optional) =====
    std::cout << "========================================" << std::endl;
    std::cout << "TEST 4: Establish Sync Partnership (Optional)" << std::endl;
    std::cout << "========================================" << std::endl;

    std::cout << "WARNING: This will generate a NEW sync partner GUID for this device." << std::endl;
    std::cout << "         The old GUID will be overwritten." << std::endl;
    std::cout << std::endl;

    std::string original_guid = sync_guid;

    if (!read_yes_no("Do you want to test establishing a NEW sync partnership?")) {
        std::cout << "Skipping EstablishSyncPairing test." << std::endl;
    } else {
        std::string pairing_name = read_input("Enter device name for pairing (or press Enter to keep current): ");
        if (pairing_name.empty()) {
            pairing_name = original_name;
        }

        std::cout << std::endl;
        std::cout << "Establishing sync partnership with name \"" << pairing_name << "\"..." << std::endl;

        int result = device.EstablishSyncPairing(pairing_name);

        if (result == 0) {
            std::cout << "SUCCESS: EstablishSyncPairing returned 0" << std::endl;

            // Verify by re-reading the GUID
            std::string new_guid = device.GetSyncPartnerGuid();
            std::cout << "New Sync Partner GUID: " << new_guid << std::endl;

            if (new_guid != original_guid && new_guid != "{00000000-0000-0000-0000-000000000000}") {
                std::cout << "VERIFIED: New partnership GUID established!" << std::endl;
            } else if (new_guid == original_guid) {
                std::cout << "NOTE: GUID unchanged (may have been re-established with same value)" << std::endl;
            } else {
                std::cout << "WARNING: GUID is null - partnership may not have been established" << std::endl;
            }
        } else {
            std::cerr << "FAILED: EstablishSyncPairing returned error code " << result << std::endl;
        }
    }
    std::cout << std::endl;

    // ===== TEST 5: Test C API Functions =====
    std::cout << "========================================" << std::endl;
    std::cout << "TEST 5: Verify C API Functions" << std::endl;
    std::cout << "========================================" << std::endl;

    // Get device handle for C API testing
    // Note: We'll use zune_device_create/connect for this
    zune_device_handle_t c_handle = zune_device_create();
    if (c_handle && zune_device_connect_usb(c_handle)) {
        std::cout << "C API connection: SUCCESS" << std::endl;

        // Test C API GetSyncPartnerGuid
        const char* c_guid = zune_device_get_sync_partner_guid(c_handle);
        if (c_guid) {
            std::cout << "C API zune_device_get_sync_partner_guid: \"" << c_guid << "\"" << std::endl;
        } else {
            std::cout << "C API zune_device_get_sync_partner_guid: NULL (error)" << std::endl;
        }

        // Test C API GetName
        const char* c_name = zune_device_get_name(c_handle);
        if (c_name) {
            std::cout << "C API zune_device_get_name: \"" << c_name << "\"" << std::endl;
        } else {
            std::cout << "C API zune_device_get_name: NULL (error)" << std::endl;
        }

        zune_device_disconnect(c_handle);
    } else {
        std::cout << "C API connection: FAILED (this is expected if device is already in use)" << std::endl;
    }

    if (c_handle) {
        zune_device_destroy(c_handle);
    }
    std::cout << std::endl;

    // ===== Summary =====
    std::cout << "========================================" << std::endl;
    std::cout << "Test Summary" << std::endl;
    std::cout << "========================================" << std::endl;
    std::cout << std::endl;
    std::cout << "Sync Partner GUID interpretation:" << std::endl;
    std::cout << "  - Empty string:      Error reading property" << std::endl;
    std::cout << "  - {00000000-...}:    No sync partnership (guest mode)" << std::endl;
    std::cout << "  - Real GUID:         Sync partnership established" << std::endl;
    std::cout << std::endl;
    std::cout << "SetDeviceName error codes:" << std::endl;
    std::cout << "  -  0: Success" << std::endl;
    std::cout << "  - -1: Invalid input (empty name)" << std::endl;
    std::cout << "  - -2: Device not connected" << std::endl;
    std::cout << "  - -3: MTP operation failed" << std::endl;
    std::cout << std::endl;

    // Cleanup
    device.Disconnect();
    std::cout << "Disconnected from device." << std::endl;

    return 0;
}
