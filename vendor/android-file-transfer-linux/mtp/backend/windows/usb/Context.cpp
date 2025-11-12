/*
    This file is part of Android File Transfer For Linux.
    Copyright (C) 2015-2020  Vladimir Menshakov

    This library is free software; you can redistribute it and/or modify it
    under the terms of the GNU Lesser General Public License as published by
    the Free Software Foundation; either version 2.1 of the License,
    or (at your option) any later version.

    This library is distributed in the hope that it will be useful, but
    WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
    Lesser General Public License for more details.

    You should have received a copy of the GNU Lesser General Public License
    along with this library; if not, write to the Free Software Foundation,
    Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA
*/

#include <usb/Context.h>
#include <usb/DeviceDescriptor.h>
#include <mtp/log.h>
#include <windows.h>
#include <setupapi.h>
#include <devguid.h>
#include <regstr.h>
#include <cfgmgr32.h>
#include <usbiodef.h>
#include <string>
#include <vector>

#pragma comment(lib, "setupapi.lib")
#pragma comment(lib, "winusb.lib")

namespace mtp { namespace usb
{

	Context::Context(int debugLevel)
	{
		debug("Windows USB Context: Initializing");
		EnumerateDevices();
		debug("Windows USB Context: Found ", _devices.size(), " devices");
	}

	Context::~Context()
	{
		debug("Windows USB Context: Cleanup");
	}

	void Context::Wait()
	{
		// Windows doesn't have an equivalent to libusb_handle_events
		// Event handling is done per-device via overlapped I/O
	}

	void Context::EnumerateDevices()
	{
		// GUID for USB devices - we'll enumerate all USB devices
		// and filter for MTP/PTP devices in the upper layer
		GUID usbDeviceGuid = GUID_DEVINTERFACE_USB_DEVICE;

		// Get device information set for all USB devices
		HDEVINFO deviceInfoSet = SetupDiGetClassDevs(
			&usbDeviceGuid,
			NULL,
			NULL,
			DIGCF_PRESENT | DIGCF_DEVICEINTERFACE
		);

		if (deviceInfoSet == INVALID_HANDLE_VALUE)
		{
			error("SetupDiGetClassDevs failed: ", GetLastError());
			return;
		}

		SP_DEVINFO_DATA deviceInfoData;
		deviceInfoData.cbSize = sizeof(SP_DEVINFO_DATA);

		// Enumerate all devices in the set
		for (DWORD i = 0; SetupDiEnumDeviceInfo(deviceInfoSet, i, &deviceInfoData); i++)
		{
			// Get device interface data
			SP_DEVICE_INTERFACE_DATA deviceInterfaceData;
			deviceInterfaceData.cbSize = sizeof(SP_DEVICE_INTERFACE_DATA);

			if (!SetupDiEnumDeviceInterfaces(
				deviceInfoSet,
				&deviceInfoData,
				&usbDeviceGuid,
				0,
				&deviceInterfaceData))
			{
				continue;
			}

			// Get the required size for device interface detail
			DWORD requiredSize = 0;
			SetupDiGetDeviceInterfaceDetail(
				deviceInfoSet,
				&deviceInterfaceData,
				NULL,
				0,
				&requiredSize,
				NULL
			);

			if (requiredSize == 0)
			{
				continue;
			}

			// Allocate buffer for device interface detail
			std::vector<BYTE> buffer(requiredSize);
			PSP_DEVICE_INTERFACE_DETAIL_DATA deviceInterfaceDetailData =
				reinterpret_cast<PSP_DEVICE_INTERFACE_DETAIL_DATA>(&buffer[0]);
			deviceInterfaceDetailData->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA);

			// Get device interface detail (device path)
			if (!SetupDiGetDeviceInterfaceDetail(
				deviceInfoSet,
				&deviceInterfaceData,
				deviceInterfaceDetailData,
				requiredSize,
				NULL,
				&deviceInfoData))
			{
				continue;
			}

			// Get device path as string
			std::string devicePath = deviceInterfaceDetailData->DevicePath;

			// Get device instance ID
			WCHAR instanceId[MAX_DEVICE_ID_LEN];
			if (CM_Get_Device_IDW(deviceInfoData.DevInst, instanceId, MAX_DEVICE_ID_LEN, 0) != CR_SUCCESS)
			{
				continue;
			}

			std::wstring wInstanceId = instanceId;
			std::string deviceInstanceId(wInstanceId.begin(), wInstanceId.end());

			debug("Found USB device: ", devicePath);
			debug("  Instance ID: ", deviceInstanceId);

			try
			{
				// Create device descriptor
				auto descriptor = std::make_shared<DeviceDescriptor>(devicePath, deviceInstanceId);
				_devices.push_back(descriptor);
			}
			catch (const std::exception& ex)
			{
				debug("Failed to create descriptor for device ", devicePath, ": ", ex.what());
			}
		}

		SetupDiDestroyDeviceInfoList(deviceInfoSet);
	}

}}
