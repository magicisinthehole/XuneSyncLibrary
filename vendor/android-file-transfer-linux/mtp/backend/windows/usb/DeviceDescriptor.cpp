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

#include <usb/DeviceDescriptor.h>
#include <usb/Context.h>
#include <usb/Interface.h>
#include <mtp/log.h>
#include <stdexcept>
#include <regex>

namespace mtp { namespace usb
{

	Configuration::Configuration(const USB_CONFIGURATION_DESCRIPTOR& config)
		: _config(config)
	{
	}

	Configuration::~Configuration()
	{
	}

	InterfacePtr Configuration::GetInterface(DevicePtr device, ConfigurationPtr config, int idx, int settings) const
	{
		if (idx < 0 || idx >= static_cast<int>(_interfaces.size()))
		{
			throw std::out_of_range("Interface index out of range");
		}
		return _interfaces[idx];
	}

	DeviceDescriptor::DeviceDescriptor(const std::string& devicePath, const std::string& instanceId)
		: _devicePath(devicePath), _deviceInstanceId(instanceId), _vendorId(0), _productId(0)
	{
		memset(&_descriptor, 0, sizeof(_descriptor));

		// Extract VID and PID from device instance ID
		// Format: USB\VID_045E&PID_063E\...
		std::regex vidPidRegex("VID_([0-9A-Fa-f]{4})&PID_([0-9A-Fa-f]{4})", std::regex::icase);
		std::smatch matches;

		if (std::regex_search(instanceId, matches, vidPidRegex))
		{
			if (matches.size() >= 3)
			{
				_vendorId = static_cast<u16>(std::stoul(matches[1].str(), nullptr, 16));
				_productId = static_cast<u16>(std::stoul(matches[2].str(), nullptr, 16));
				debug("Parsed VID:PID = ", std::hex, _vendorId, ":", _productId, std::dec);
			}
		}

		// Try to query full descriptor when device is opened
		// For now, we have basic VID/PID from the instance ID
	}

	DeviceDescriptor::~DeviceDescriptor()
	{
	}

	DevicePtr DeviceDescriptor::Open(ContextPtr context)
	{
		// Open device handle using CreateFile
		std::wstring wDevicePath(_devicePath.begin(), _devicePath.end());

		HANDLE deviceHandle = CreateFileW(
			wDevicePath.c_str(),
			GENERIC_WRITE | GENERIC_READ,
			FILE_SHARE_WRITE | FILE_SHARE_READ,
			NULL,
			OPEN_EXISTING,
			FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED,
			NULL
		);

		if (deviceHandle == INVALID_HANDLE_VALUE)
		{
			DWORD error = GetLastError();
			throw std::runtime_error("Failed to open device " + _devicePath +
			                        " (error " + std::to_string(error) + ")");
		}

		// Initialize WinUSB
		WINUSB_INTERFACE_HANDLE winusbHandle;
		BOOL result = WinUsb_Initialize(deviceHandle, &winusbHandle);

		if (!result)
		{
			DWORD error = GetLastError();
			CloseHandle(deviceHandle);
			throw std::runtime_error("WinUsb_Initialize failed for device " + _devicePath +
			                        " (error " + std::to_string(error) + ")");
		}

		// Query device descriptor
		USB_DEVICE_DESCRIPTOR deviceDescriptor;
		ULONG bytesReturned = 0;
		result = WinUsb_GetDescriptor(
			winusbHandle,
			USB_DEVICE_DESCRIPTOR_TYPE,
			0,
			0,
			reinterpret_cast<PUCHAR>(&deviceDescriptor),
			sizeof(deviceDescriptor),
			&bytesReturned
		);

		if (result && bytesReturned >= sizeof(deviceDescriptor))
		{
			_descriptor = deviceDescriptor;
			_vendorId = deviceDescriptor.idVendor;
			_productId = deviceDescriptor.idProduct;
		}

		debug("Opened device: VID=", std::hex, _vendorId, " PID=", _productId, std::dec);

		return std::make_shared<Device>(context, deviceHandle, winusbHandle, _devicePath);
	}

	DevicePtr DeviceDescriptor::TryOpen(ContextPtr context)
	{
		try
		{
			return Open(context);
		}
		catch (const std::exception& ex)
		{
			debug("Failed to open device ", _devicePath, ": ", ex.what());
			return nullptr;
		}
	}

	ConfigurationPtr DeviceDescriptor::GetConfiguration(int conf, DevicePtr openDevice)
	{
		// Use already-open device handle if provided
		HANDLE deviceHandle = INVALID_HANDLE_VALUE;
		WINUSB_INTERFACE_HANDLE winusbHandle = NULL;
		bool needsCleanup = false;
		BOOL result;

		if (openDevice)
		{
			// Use the handles from the already-open device
			deviceHandle = openDevice->GetHandle();
			winusbHandle = openDevice->GetWinUsbHandle();
		}
		else
		{
			// Open device temporarily to query configuration
			std::wstring wDevicePath(_devicePath.begin(), _devicePath.end());

			deviceHandle = CreateFileW(
				wDevicePath.c_str(),
				GENERIC_WRITE | GENERIC_READ,
				FILE_SHARE_WRITE | FILE_SHARE_READ,
				NULL,
				OPEN_EXISTING,
				FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED,
				NULL
			);

			if (deviceHandle == INVALID_HANDLE_VALUE)
			{
				throw std::runtime_error("Failed to open device for configuration query");
			}

			BOOL result = WinUsb_Initialize(deviceHandle, &winusbHandle);

			if (!result)
			{
				CloseHandle(deviceHandle);
				throw std::runtime_error("WinUsb_Initialize failed for configuration query");
			}
			needsCleanup = true;
		}

		// Create configuration object
		USB_CONFIGURATION_DESCRIPTOR configDesc;
		memset(&configDesc, 0, sizeof(configDesc));
		configDesc.bLength = sizeof(USB_CONFIGURATION_DESCRIPTOR);
		configDesc.bDescriptorType = USB_CONFIGURATION_DESCRIPTOR_TYPE;
		configDesc.bConfigurationValue = conf;

		auto configPtr = std::make_shared<Configuration>(configDesc);

		// Query interface descriptor
		USB_INTERFACE_DESCRIPTOR interfaceDesc;
		ULONG bytesReturned = 0;
		result = WinUsb_QueryInterfaceSettings(
			winusbHandle,
			0,  // Interface index
			&interfaceDesc
		);

		if (result)
		{
			debug("Interface: class=", (int)interfaceDesc.bInterfaceClass,
			      " subclass=", (int)interfaceDesc.bInterfaceSubClass,
			      " protocol=", (int)interfaceDesc.bInterfaceProtocol);

			// Query interface name directly using WinUsb_GetDescriptor
			std::string interfaceName;
			if (interfaceDesc.iInterface != 0)
			{
				UCHAR buffer[256];
				ULONG bytesRead = 0;
				BOOL stringResult = WinUsb_GetDescriptor(
					winusbHandle,
					USB_STRING_DESCRIPTOR_TYPE,
					interfaceDesc.iInterface,
					0,
					buffer,
					sizeof(buffer),
					&bytesRead
				);

				if (stringResult && bytesRead >= 2)
				{
					UCHAR length = buffer[0];
					if (length > bytesRead) length = static_cast<UCHAR>(bytesRead);
					for (UCHAR i = 2; i < length; i += 2)
					{
						if (buffer[i] >= 32 && buffer[i] < 127)
							interfaceName += static_cast<char>(buffer[i]);
					}
				}
			}

			auto interface = std::make_shared<Interface>(
				nullptr,  // Don't need device ptr for interface
				configPtr,
				interfaceDesc,
				interfaceName
			);

			// Query endpoints
			for (UCHAR i = 0; i < interfaceDesc.bNumEndpoints; i++)
			{
				WINUSB_PIPE_INFORMATION pipeInfo;
				result = WinUsb_QueryPipe(
					winusbHandle,
					0,  // Interface index
					i,  // Pipe index
					&pipeInfo
				);

				if (result)
				{
					EndpointDirection direction = EndpointDirection::Out;
					if (pipeInfo.PipeId & 0x80)
					{
						direction = EndpointDirection::In;
					}

					EndpointType type = EndpointType::Bulk;
					switch (pipeInfo.PipeType)
					{
						case UsbdPipeTypeControl:
							type = EndpointType::Control;
							break;
						case UsbdPipeTypeIsochronous:
							type = EndpointType::Isochronous;
							break;
						case UsbdPipeTypeBulk:
							type = EndpointType::Bulk;
							break;
						case UsbdPipeTypeInterrupt:
							type = EndpointType::Interrupt;
							break;
					}

					auto endpoint = std::make_shared<Endpoint>(
						pipeInfo.PipeId,
						pipeInfo.MaximumPacketSize,
						direction,
						type
					);

					interface->AddEndpoint(endpoint);

					debug("  Endpoint ", (int)i, ": address=0x", std::hex, (int)pipeInfo.PipeId, std::dec,
					      " maxPacket=", pipeInfo.MaximumPacketSize);
				}
			}

				configPtr->AddInterface(interface);
			configDesc.bNumInterfaces = 1;
		}

		// Only clean up if we opened the device ourselves
		if (needsCleanup)
		{
			WinUsb_Free(winusbHandle);
			CloseHandle(deviceHandle);
		}

		return configPtr;
	}

	ConfigurationPtr DeviceDescriptor::GetConfiguration(int conf)
	{
		// Call the overload with nullptr to open device temporarily
		return GetConfiguration(conf, nullptr);
	}

	ByteArray DeviceDescriptor::GetDescriptor() const
	{
		ByteArray data(sizeof(_descriptor));
		memcpy(data.data(), &_descriptor, sizeof(_descriptor));
		return data;
	}

	bool DeviceDescriptor::QueryDeviceDescriptor()
	{
		// This is called internally when device is opened
		return _vendorId != 0 || _productId != 0;
	}

}}
