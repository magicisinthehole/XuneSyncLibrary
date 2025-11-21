#pragma once

#include <string>
#include <cstdint>
#include <usb/Device.h>
#include <usb/Interface.h>

// USB handles structure for raw monitoring (includes pre-discovered endpoints)
struct USBHandlesWithEndpoints {
    mtp::usb::DevicePtr device;
    mtp::usb::InterfacePtr interface;
    mtp::usb::EndpointPtr endpoint_in;
    mtp::usb::EndpointPtr endpoint_out;
};

struct ZuneObjectInfoInternal {
    uint32_t handle;
    std::string filename;
    uint64_t size;
    bool is_folder;
};
