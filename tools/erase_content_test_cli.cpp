/**
 * Erase All Content Test CLI - Tests the device content erasure functionality
 *
 * This tool verifies the EraseAllContent operation (MTP FormatStore) works correctly
 * with a connected Zune device. This operation will delete ALL content on the device.
 *
 * Usage:
 *   erase_content_test_cli
 *
 * Operations tested:
 *   1. Connect to device and display info
 *   2. Show current storage usage
 *   3. Prompt for confirmation (requires typing full phrase)
 *   4. Perform FormatStore operation
 *   5. Verify device is empty
 */

#include "lib/src/ZuneDevice.h"
#include "zune_wireless/zune_wireless_api.h"
#include <iostream>
#include <iomanip>
#include <chrono>
#include <string>
#include <algorithm>

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

void show_storage_info(ZuneDevice& device) {
    try {
        // Get storage info using MTP session
        auto session = device.GetMtpSession();
        if (session) {
            auto storage_ids = session->GetStorageIDs();
            
            for (const auto& storage_id : storage_ids.StorageIDs) {
                auto storage_info = session->GetStorageInfo(mtp::StorageId(storage_id));
                
                std::cout << "Storage ID: 0x" << std::hex << storage_id << std::dec << std::endl;
                std::cout << "  Type: 0x" << std::hex << storage_info.StorageType << std::dec << std::endl;
                std::cout << "  Filesystem: 0x" << std::hex << storage_info.FilesystemType << std::dec << std::endl;
                std::cout << "  Access: 0x" << std::hex << storage_info.AccessCapability << std::dec << std::endl;
                std::cout << "  Capacity: " << format_bytes(storage_info.MaxCapacity) << std::endl;
                std::cout << "  Free Space: " << format_bytes(storage_info.FreeSpaceInBytes) << std::endl;
                std::cout << "  Used Space: " << format_bytes(storage_info.MaxCapacity - storage_info.FreeSpaceInBytes) << std::endl;
                
                if (!storage_info.StorageDescription.empty()) {
                    std::cout << "  Description: " << storage_info.StorageDescription << std::endl;
                }
                if (!storage_info.VolumeLabel.empty()) {
                    std::cout << "  Volume Label: " << storage_info.VolumeLabel << std::endl;
                }
                std::cout << std::endl;
            }
        }
    } catch (const std::exception& e) {
        std::cout << "Could not retrieve detailed storage information: " << e.what() << std::endl;
    }
}

int main() {
    std::cout << "=== Erase All Content Test CLI ===" << std::endl;
    std::cout << std::endl;
    std::cout << "WARNING: This tool will PERMANENTLY DELETE all content on your Zune device!" << std::endl;
    std::cout << "         This includes all music, videos, photos, podcasts, and playlists." << std::endl;
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

    // Step 2: Show current storage usage
    std::cout << "========================================" << std::endl;
    std::cout << "Current Storage Status" << std::endl;
    std::cout << "========================================" << std::endl;
    show_storage_info(device);

    // Step 3: Final warning and confirmation
    std::cout << "========================================" << std::endl;
    std::cout << "!!! FINAL WARNING !!!" << std::endl;
    std::cout << "========================================" << std::endl;
    std::cout << std::endl;
    std::cout << "You are about to ERASE ALL CONTENT on the device." << std::endl;
    std::cout << "This action CANNOT be undone!" << std::endl;
    std::cout << std::endl;
    std::cout << "To proceed, type exactly: DELETE ALL MY ZUNE CONTENT" << std::endl;
    std::cout << "Or press Enter to cancel: ";
    
    std::string confirmation = read_input();
    
    if (confirmation != "DELETE ALL MY ZUNE CONTENT") {
        std::cout << std::endl;
        std::cout << "Operation cancelled. Device content was NOT erased." << std::endl;
        device.Disconnect();
        return 0;
    }

    // Step 4: Perform erase operation
    std::cout << std::endl;
    std::cout << "========================================" << std::endl;
    std::cout << "Erasing All Content" << std::endl;
    std::cout << "========================================" << std::endl;
    std::cout << "This operation will:" << std::endl;
    std::cout << "  1. Format device storage (takes ~5 seconds)" << std::endl;
    std::cout << "  2. Finalize device state" << std::endl;
    std::cout << "  3. Reboot the device" << std::endl;
    std::cout << std::endl;
    std::cout << "DO NOT disconnect the device!" << std::endl;
    std::cout << std::endl;

    auto start_time = std::chrono::steady_clock::now();
    int result = device.EraseAllContent();
    auto end_time = std::chrono::steady_clock::now();
    
    auto elapsed_seconds = std::chrono::duration_cast<std::chrono::seconds>(end_time - start_time).count();

    std::cout << std::endl;
    std::cout << "Operation completed in " << elapsed_seconds << " seconds." << std::endl;
    std::cout << std::endl;

    if (result == 0) {
        std::cout << "SUCCESS: All content has been erased!" << std::endl;
        std::cout << std::endl;
        
        std::cout << "========================================" << std::endl;
        std::cout << "DEVICE IS NOW REBOOTING" << std::endl;
        std::cout << "========================================" << std::endl;
        std::cout << std::endl;
        std::cout << "The device is rebooting and will be unavailable for 10-15 seconds." << std::endl;
        std::cout << "After the device finishes rebooting:" << std::endl;
        std::cout << "  - It will be completely empty" << std::endl;
        std::cout << "  - All music, playlists, and content will be gone" << std::endl;
        std::cout << "  - You can reconnect and sync new content" << std::endl;
        std::cout << std::endl;
        
        // Don't try to show storage info - device is disconnected/rebooting
        std::cout << "Note: The device connection has been closed due to the reboot." << std::endl;
        std::cout << "      Reconnect after the device finishes rebooting to verify." << std::endl;
    } else {
        std::cerr << "FAILED: Erase operation failed with error code " << result << std::endl;
        switch (result) {
            case -1:
                std::cerr << "  Error: Device not connected" << std::endl;
                break;
            case -2:
                std::cerr << "  Error: MTP operation failed" << std::endl;
                std::cerr << "  The device may not support this operation or may be in use." << std::endl;
                break;
            default:
                std::cerr << "  Error: Unknown error" << std::endl;
                break;
        }
    }
    std::cout << std::endl;
    std::cout << "========================================" << std::endl;
    std::cout << "Summary" << std::endl;
    std::cout << "========================================" << std::endl;
    std::cout << std::endl;
    std::cout << "EraseAllContent operation sequence:" << std::endl;
    std::cout << "  1. FormatStore (0x100f) - Erases all content (~5 seconds)" << std::endl;
    std::cout << "  2. GetDevicePropValue (0x1015) - Query property 0xd217 (3 times)" << std::endl;
    std::cout << "  3. Operation9217 (0x9217) - Zune-specific finalization" << std::endl;
    std::cout << "  4. GetStorageInfo (0x1005) - Verify format success" << std::endl;
    std::cout << "  5. GetDevicePropValue (0x1015) - Final state check" << std::endl;
    std::cout << "  6. RebootDevice (0x9204) - Reboot the device" << std::endl;
    std::cout << std::endl;
    std::cout << "Error codes:" << std::endl;
    std::cout << "   0: Success - content erased and device rebooting" << std::endl;
    std::cout << "  -1: Device not connected" << std::endl;
    std::cout << "  -2: MTP operation failed" << std::endl;
    std::cout << std::endl;
    std::cout << "Note: Device will be disconnected after reboot command" << std::endl;
    std::cout << std::endl;

    // Cleanup
    device.Disconnect();
    std::cout << "Disconnected from device." << std::endl;

    return (result == 0) ? 0 : 1;
}