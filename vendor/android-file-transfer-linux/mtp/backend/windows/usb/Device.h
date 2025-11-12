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

#ifndef AFTL_MTP_BACKEND_WINDOWS_USB_DEVICE_H
#define AFTL_MTP_BACKEND_WINDOWS_USB_DEVICE_H

#include <mtp/ptp/IObjectStream.h>
#include <mtp/ByteArray.h>
#include <mtp/Token.h>
#include <mtp/types.h>
#include <mtp/usb/types.h>
#include <windows.h>
#include <winusb.h>

// Undefine Windows macros that conflict with C++ identifiers
#ifdef interface
#undef interface
#endif

namespace mtp { namespace usb
{
	class Context;
	DECLARE_PTR(Context);

	class Endpoint
	{
		u8				_address;
		u16				_maxPacketSize;
		EndpointDirection	_direction;
		EndpointType		_type;

	public:
		Endpoint(u8 address, u16 maxPacketSize, EndpointDirection direction, EndpointType type):
			_address(address), _maxPacketSize(maxPacketSize), _direction(direction), _type(type) { }

		u8 GetAddress() const
		{ return _address; }

		int GetMaxPacketSize() const
		{ return _maxPacketSize; }

		EndpointDirection GetDirection() const
		{ return _direction; }

		EndpointType GetType() const
		{ return _type; }
	};
	DECLARE_PTR(Endpoint);

	class Interface;
	DECLARE_PTR(Interface);
	class InterfaceToken;
	DECLARE_PTR(InterfaceToken);

	class Device : Noncopyable, public std::enable_shared_from_this<Device>
	{
	private:
		ContextPtr			_context;
		HANDLE				_deviceHandle;
		WINUSB_INTERFACE_HANDLE _winusbHandle;
		std::string			_devicePath;

	public:
		Device(ContextPtr ctx, HANDLE deviceHandle, WINUSB_INTERFACE_HANDLE winusbHandle, const std::string& devicePath);
		~Device();

		HANDLE GetHandle() const
		{ return _deviceHandle; }

		WINUSB_INTERFACE_HANDLE GetWinUsbHandle() const
		{ return _winusbHandle; }

		InterfaceTokenPtr ClaimInterface(const InterfacePtr & iface);

		void Reset();
		int GetConfiguration() const;
		void SetConfiguration(int idx);

		void WriteBulk(const EndpointPtr & ep, const IObjectInputStreamPtr &inputStream, int timeout);
		void ReadBulk(const EndpointPtr & ep, const IObjectOutputStreamPtr &outputStream, int timeout);

		void ReadControl(u8 type, u8 req, u16 value, u16 index, ByteArray &data, int timeout);
		void WriteControl(u8 type, u8 req, u16 value, u16 index, const ByteArray &data, int timeout);

		void ClearHalt(const EndpointPtr & ep);

		std::string GetString(int idx) const;
	};
	DECLARE_PTR(Device);
}}

#endif	/* DEVICE_H */
