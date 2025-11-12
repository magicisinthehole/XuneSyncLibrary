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

#include <usb/Device.h>
#include <usb/Context.h>
#include <usb/Interface.h>
#include <mtp/ByteArray.h>
#include <mtp/log.h>
#include <stdexcept>

namespace mtp { namespace usb
{

	Device::Device(ContextPtr context, HANDLE deviceHandle, WINUSB_INTERFACE_HANDLE winusbHandle, const std::string& devicePath)
		: _context(context), _deviceHandle(deviceHandle), _winusbHandle(winusbHandle), _devicePath(devicePath)
	{
		debug("Windows USB Device: Created for ", devicePath);
	}

	Device::~Device()
	{
		if (_winusbHandle != INVALID_HANDLE_VALUE && _winusbHandle != NULL)
		{
			WinUsb_Free(_winusbHandle);
			_winusbHandle = NULL;
		}

		if (_deviceHandle != INVALID_HANDLE_VALUE)
		{
			CloseHandle(_deviceHandle);
			_deviceHandle = INVALID_HANDLE_VALUE;
		}

		debug("Windows USB Device: Destroyed");
	}

	void Device::Reset()
	{
		if (_winusbHandle == NULL || _winusbHandle == INVALID_HANDLE_VALUE)
		{
			throw std::runtime_error("WinUsb_ResetPipe failed: Invalid handle");
		}

		// WinUSB doesn't have a direct reset device equivalent
		// We reset individual pipes instead
		debug("Windows USB Device: Reset requested (resetting pipes)");
	}

	int Device::GetConfiguration() const
	{
		// WinUSB doesn't expose configuration index directly
		// Typically devices have only one configuration (index 1)
		return 1;
	}

	void Device::SetConfiguration(int idx)
	{
		// WinUSB doesn't allow changing configuration
		// Configuration is set when the device is opened
		debug("Windows USB Device: SetConfiguration(", idx, ") - not supported by WinUSB");
	}

	InterfaceToken::InterfaceToken(DevicePtr device, int index)
		: _device(device), _index(index)
	{
		// Interface is already claimed when WinUSB handle is created
		debug("Windows USB Interface: Claimed interface ", index);
	}

	InterfaceToken::~InterfaceToken()
	{
		// Interface is released when WinUSB handle is freed
		debug("Windows USB Interface: Released interface ", _index);
	}

	InterfaceTokenPtr Device::ClaimInterface(const InterfacePtr & iface)
	{
		return std::make_shared<InterfaceToken>(shared_from_this(), iface->GetIndex());
	}

	void Device::WriteBulk(const EndpointPtr & ep, const IObjectInputStreamPtr &inputStream, int timeout)
	{
		if (_winusbHandle == NULL || _winusbHandle == INVALID_HANDLE_VALUE)
		{
			throw std::runtime_error("WriteBulk failed: Invalid WinUSB handle");
		}

		// Read data from input stream
		size_t size = inputStream->GetSize();
		ByteArray data(size);
		inputStream->Read(data.data(), size);

		// Set timeout policy
		if (timeout > 0)
		{
			ULONG timeoutMs = static_cast<ULONG>(timeout);
			WinUsb_SetPipePolicy(_winusbHandle, ep->GetAddress(), PIPE_TRANSFER_TIMEOUT, sizeof(ULONG), &timeoutMs);
		}

		// Perform bulk write
		ULONG bytesWritten = 0;
		BOOL result = WinUsb_WritePipe(
			_winusbHandle,
			ep->GetAddress(),
			const_cast<u8*>(data.data()),
			static_cast<ULONG>(data.size()),
			&bytesWritten,
			NULL
		);

		if (!result)
		{
			DWORD lastError = GetLastError();
			throw std::runtime_error("WinUsb_WritePipe failed with error: " + std::to_string(lastError));
		}

		if (bytesWritten != data.size())
		{
			throw std::runtime_error("Short write: expected " + std::to_string(data.size()) +
			                        " bytes, wrote " + std::to_string(bytesWritten));
		}
	}

	void Device::ReadBulk(const EndpointPtr & ep, const IObjectOutputStreamPtr &outputStream, int timeout)
	{
		if (_winusbHandle == NULL || _winusbHandle == INVALID_HANDLE_VALUE)
		{
			throw std::runtime_error("ReadBulk failed: Invalid WinUSB handle");
		}

		// Set timeout policy
		if (timeout > 0)
		{
			ULONG timeoutMs = static_cast<ULONG>(timeout);
			WinUsb_SetPipePolicy(_winusbHandle, ep->GetAddress(), PIPE_TRANSFER_TIMEOUT, sizeof(ULONG), &timeoutMs);
		}

		// Read in chunks until we get a short packet
		ByteArray buffer(ep->GetMaxPacketSize() * 1024);
		ULONG bytesRead = 0;

		do
		{
			BOOL result = WinUsb_ReadPipe(
				_winusbHandle,
				ep->GetAddress(),
				buffer.data(),
				static_cast<ULONG>(buffer.size()),
				&bytesRead,
				NULL
			);

			if (!result)
			{
				DWORD lastError = GetLastError();
				if (lastError == ERROR_SEM_TIMEOUT)
				{
					throw std::runtime_error("ReadBulk timeout");
				}
				throw std::runtime_error("WinUsb_ReadPipe failed with error: " + std::to_string(lastError));
			}

			if (bytesRead > 0)
			{
				outputStream->Write(buffer.data(), bytesRead);
			}
		}
		while (bytesRead == buffer.size()); // Continue while we receive full packets
	}

	void Device::ReadControl(u8 type, u8 req, u16 value, u16 index, ByteArray &data, int timeout)
	{
		if (_winusbHandle == NULL || _winusbHandle == INVALID_HANDLE_VALUE)
		{
			throw std::runtime_error("ReadControl failed: Invalid WinUSB handle");
		}

		WINUSB_SETUP_PACKET setupPacket;
		setupPacket.RequestType = type;
		setupPacket.Request = req;
		setupPacket.Value = value;
		setupPacket.Index = index;
		setupPacket.Length = static_cast<USHORT>(data.size());

		ULONG bytesRead = 0;
		BOOL result = WinUsb_ControlTransfer(
			_winusbHandle,
			setupPacket,
			data.data(),
			static_cast<ULONG>(data.size()),
			&bytesRead,
			NULL
		);

		if (!result)
		{
			DWORD lastError = GetLastError();
			throw std::runtime_error("WinUsb_ControlTransfer (read) failed with error: " + std::to_string(lastError));
		}
	}

	void Device::WriteControl(u8 type, u8 req, u16 value, u16 index, const ByteArray &data, int timeout)
	{
		if (_winusbHandle == NULL || _winusbHandle == INVALID_HANDLE_VALUE)
		{
			throw std::runtime_error("WriteControl failed: Invalid WinUSB handle");
		}

		WINUSB_SETUP_PACKET setupPacket;
		setupPacket.RequestType = type;
		setupPacket.Request = req;
		setupPacket.Value = value;
		setupPacket.Index = index;
		setupPacket.Length = static_cast<USHORT>(data.size());

		ULONG bytesWritten = 0;
		BOOL result = WinUsb_ControlTransfer(
			_winusbHandle,
			setupPacket,
			const_cast<u8*>(data.data()),
			static_cast<ULONG>(data.size()),
			&bytesWritten,
			NULL
		);

		if (!result)
		{
			DWORD lastError = GetLastError();
			throw std::runtime_error("WinUsb_ControlTransfer (write) failed with error: " + std::to_string(lastError));
		}
	}

	void Device::ClearHalt(const EndpointPtr & ep)
	{
		if (_winusbHandle == NULL || _winusbHandle == INVALID_HANDLE_VALUE)
		{
			throw std::runtime_error("ClearHalt failed: Invalid WinUSB handle");
		}

		BOOL result = WinUsb_ResetPipe(_winusbHandle, ep->GetAddress());
		if (!result)
		{
			DWORD lastError = GetLastError();
			mtp::error("WinUsb_ResetPipe failed with error: ", lastError);
		}
	}

	std::string Device::GetString(int idx) const
	{
		if (_winusbHandle == NULL || _winusbHandle == INVALID_HANDLE_VALUE)
		{
			return std::string();
		}

		if (idx == 0)
		{
			return std::string();
		}

		// Get string descriptor
		UCHAR buffer[256];
		ULONG bytesRead = 0;

		BOOL result = WinUsb_GetDescriptor(
			_winusbHandle,
			USB_STRING_DESCRIPTOR_TYPE,
			static_cast<UCHAR>(idx),
			0,  // Language ID (0 = default)
			buffer,
			sizeof(buffer),
			&bytesRead
		);

		if (!result || bytesRead < 2)
		{
			return std::string();
		}

		// USB string descriptors are UTF-16LE
		// First byte is length, second is descriptor type
		UCHAR length = buffer[0];
		if (length > bytesRead)
		{
			length = static_cast<UCHAR>(bytesRead);
		}

		// Convert UTF-16LE to ASCII (simplified conversion)
		std::string str;
		for (UCHAR i = 2; i < length; i += 2)
		{
			if (buffer[i] >= 32 && buffer[i] < 127)
			{
				str += static_cast<char>(buffer[i]);
			}
		}

		return str;
	}

}}
