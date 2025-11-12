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

#ifndef AFTL_MTP_BACKEND_WINDOWS_USB_DEVICEDESCRIPTOR_H
#define AFTL_MTP_BACKEND_WINDOWS_USB_DEVICEDESCRIPTOR_H

#include <mtp/types.h>
#include <usb/Device.h>
#include <usb/Interface.h>
#include <windows.h>
#include <winusb.h>
#include <string>

// Undefine Windows macros that conflict with C++ identifiers
#ifdef interface
#undef interface
#endif

namespace mtp { namespace usb
{
	class Configuration : Noncopyable
	{
		USB_CONFIGURATION_DESCRIPTOR	_config;
		std::vector<InterfacePtr>		_interfaces;

	public:
		Configuration(const USB_CONFIGURATION_DESCRIPTOR& config);
		~Configuration();

		int GetIndex() const
		{ return _config.bConfigurationValue; }

		int GetInterfaceCount() const
		{ return _interfaces.size(); }

		int GetInterfaceAltSettingsCount(int idx) const
		{ return 1; } // Windows doesn't easily expose alt settings count

		InterfacePtr GetInterface(DevicePtr device, ConfigurationPtr config, int idx, int settings) const;

		void AddInterface(InterfacePtr interface)
		{ _interfaces.push_back(interface); }
	};
	DECLARE_PTR(Configuration);

	class DeviceDescriptor
	{
	private:
		std::string						_devicePath;
		std::string						_deviceInstanceId;
		USB_DEVICE_DESCRIPTOR			_descriptor;
		u16								_vendorId;
		u16								_productId;

	public:
		DeviceDescriptor(const std::string& devicePath, const std::string& instanceId);
		~DeviceDescriptor();

		u16 GetVendorId() const
		{ return _vendorId; }

		u16 GetProductId() const
		{ return _productId; }

		const std::string& GetDevicePath() const
		{ return _devicePath; }

		const std::string& GetDeviceInstanceId() const
		{ return _deviceInstanceId; }

		DevicePtr Open(ContextPtr context);
		DevicePtr TryOpen(ContextPtr context);

		int GetConfigurationsCount() const
		{ return _descriptor.bNumConfigurations; }

		ConfigurationPtr GetConfiguration(int conf);
		ConfigurationPtr GetConfiguration(int conf, DevicePtr openDevice);

		ByteArray GetDescriptor() const;

	private:
		bool QueryDeviceDescriptor();
	};
	DECLARE_PTR(DeviceDescriptor);

}}

#endif
