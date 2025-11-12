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

#include <mtp/ptp/Session.h>
#include <mtp/ptp/Messages.h>
#include <mtp/ptp/Container.h>
#include <mtp/ptp/OperationRequest.h>
#include <mtp/ptp/ByteArrayObjectStream.h>
#include <mtp/ptp/JoinedObjectStream.h>
#include <mtp/log.h>
#include <usb/Device.h>
#include <limits>
#include <array>
#include <iostream>
#include <iomanip>
#include <algorithm>

namespace mtp
{

	const StorageId Session::AnyStorage(0);
	const StorageId Session::AllStorages(0xffffffffu);
	const ObjectId Session::Device(0);
	const ObjectId Session::Root(0xffffffffu);


#define CHECK_RESPONSE(RCODE) do { \
	if ((RCODE) != ResponseType::OK && (RCODE) != ResponseType::SessionAlreadyOpen) \
		throw InvalidResponseException(__func__, (RCODE)); \
} while(false)

	class Session::Transaction
	{
		Session *	_session;
	public:
		u32			Id;

		Transaction(Session *session): _session(session)
		{ session->SetCurrentTransaction(this); }
		~Transaction()
		{ _session->SetCurrentTransaction(0); }
	};

	Session::Session(const PipePacketer & packeter, u32 sessionId):
		_packeter(packeter), _sessionId(sessionId), _nextTransactionId(1), _transaction(),
		_getObjectModificationTimeBuggy(false), _separateBulkWrites(false),
		_defaultTimeout(DefaultTimeout)
	{
		{
			Transaction tr(this);
			_deviceInfo = GetDeviceInfo(_packeter, tr.Id, _defaultTimeout);
		}
		if (_deviceInfo.Manufacturer == "Microsoft")
		{
			_separateBulkWrites = true;

			// Add Zune-specific operation codes to supported operations
			// These are vendor extensions that may not be reported in GetDeviceInfo
			_deviceInfo.OperationsSupported.push_back((OperationCode)0x9215);
			_deviceInfo.OperationsSupported.push_back((OperationCode)0x9224);
			_deviceInfo.OperationsSupported.push_back((OperationCode)0x9225);
			_deviceInfo.OperationsSupported.push_back((OperationCode)0x9226);
			_deviceInfo.OperationsSupported.push_back((OperationCode)0x9227);
			_deviceInfo.OperationsSupported.push_back((OperationCode)0x9228);
			_deviceInfo.OperationsSupported.push_back((OperationCode)0x922b);
			_deviceInfo.OperationsSupported.push_back((OperationCode)0x922c);
			_deviceInfo.OperationsSupported.push_back((OperationCode)0x922d);
			_deviceInfo.OperationsSupported.push_back((OperationCode)0x922f);
			_deviceInfo.OperationsSupported.push_back((OperationCode)0x9230);
		_deviceInfo.OperationsSupported.push_back((OperationCode)0x9231);
		// Phase 1 operations
		_deviceInfo.OperationsSupported.push_back((OperationCode)0x9212);
		_deviceInfo.OperationsSupported.push_back((OperationCode)0x9213);
		_deviceInfo.OperationsSupported.push_back((OperationCode)0x9216);
		_deviceInfo.OperationsSupported.push_back((OperationCode)0x9217);
		_deviceInfo.OperationsSupported.push_back((OperationCode)0x9218);
		// Phase 2 operations
		_deviceInfo.OperationsSupported.push_back((OperationCode)0x9214);
		_deviceInfo.OperationsSupported.push_back((OperationCode)0x9219);

			_deviceInfo.OperationsSupported.push_back(OperationCode::SetDevicePropValue);  // Needed for QueryDevicePropertyViaSet
			_deviceInfo.OperationsSupported.push_back(OperationCode::ResetDevicePropValue);
		}

		_getPartialObject64Supported = _deviceInfo.Supports(OperationCode::GetPartialObject64);
		_getObjectPropertyListSupported = _deviceInfo.Supports(OperationCode::GetObjectPropList);
		_getObjectPropValueSupported = _deviceInfo.Supports(OperationCode::GetObjectPropValue);
		_editObjectSupported = _deviceInfo.Supports(OperationCode::BeginEditObject) &&
			_deviceInfo.Supports(OperationCode::EndEditObject) &&
			_deviceInfo.Supports(OperationCode::TruncateObject) &&
			_deviceInfo.Supports(OperationCode::SendPartialObject);
	}

	Session::~Session()
	{ try { Close(); } catch(const std::exception &ex) { } }

	void Session::SetCurrentTransaction(Transaction *transaction)
	{
		scoped_mutex_lock l(_transactionMutex);
		_transaction = transaction;
		if (_transaction)
			_transaction->Id = _nextTransactionId++;
	}

	void Session::Send(PipePacketer &packeter, const OperationRequest &req, int timeout)
	{
		if (timeout <= 0)
			timeout = DefaultTimeout;
		Container container(req);

		// Extract opcode and transaction ID from container.Data
		// MTP packet structure: [0-3]=length, [4-5]=type, [6-7]=opcode, [8-11]=transID
		u16 opcode = 0;
		u32 transId = 0;
		if (container.Data.size() >= 12) {
			opcode = (u16)container.Data[6] | ((u16)container.Data[7] << 8);
			transId = (u32)container.Data[8] | ((u32)container.Data[9] << 8) |
			          ((u32)container.Data[10] << 16) | ((u32)container.Data[11] << 24);
		}

		// // Log outgoing command packet
		// std::cout << "[USB TX] Command 0x" << std::hex << std::setw(4) << std::setfill('0')
		//           << opcode << std::dec << " TransID=" << transId << " ";
		// for (size_t i = 0; i < std::min(container.Data.size(), size_t(40)); i++) {
		// 	printf("%02x", container.Data[i]);
		// }
		// if (container.Data.size() > 40) std::cout << "...";
		// std::cout << " (" << container.Data.size() << " bytes)" << std::endl;

		packeter.Write(container.Data, timeout);
	}


	void Session::Send(const OperationRequest &req, int timeout)
	{
		if (timeout <= 0)
			timeout = _defaultTimeout;
		Container container(req);
		Send(_packeter, req, timeout);
	}

	void Session::Close()
	{
		scoped_mutex_lock l(_mutex);
		Transaction transaction(this);
		Send(OperationRequest(OperationCode::CloseSession, transaction.Id));
		ByteArray data, response;
		ResponseType responseCode;
		_packeter.Read(0, data, responseCode, response, _defaultTimeout);
		//HexDump("payload", data);
	}

	ByteArray Session::Get(PipePacketer &packeter, u32 transaction, ByteArray & response, int timeout)
	{
		if (timeout <= 0)
			timeout = DefaultTimeout;

		ByteArray data;
		ResponseType responseCode;
		packeter.Read(transaction, data, responseCode, response, timeout);

		// Log received response packet
		// std::cout << "[USB RX] Response 0x" << std::hex << std::setw(4) << std::setfill('0')
		//           << (int)responseCode << std::dec << " TransID=" << transaction;
		// if (response.size() > 0) {
		// 	std::cout << " Response: ";
		// 	for (size_t i = 0; i < std::min(response.size(), size_t(40)); i++) {
		// 		printf("%02x", response[i]);
		// 	}
		// 	if (response.size() > 40) std::cout << "...";
		// 	std::cout << " (" << response.size() << " bytes)";
		// }
		// if (data.size() > 0) {
		// 	std::cout << " Data: ";
		// 	for (size_t i = 0; i < std::min(data.size(), size_t(40)); i++) {
		// 		printf("%02x", data[i]);
		// 	}
		// 	if (data.size() > 40) std::cout << "...";
		// 	std::cout << " (" << data.size() << " bytes)";
		// }
		// std::cout << std::endl;

		CHECK_RESPONSE(responseCode);
		return data;
	}

	ByteArray Session::Get(u32 transaction, ByteArray &response, int timeout)
	{
		if (timeout <= 0)
			timeout = _defaultTimeout;
		return Get(_packeter, transaction, response, timeout);
	}

	template<typename ... Args>
	ByteArray Session::RunTransactionWithDataRequest(int timeout, OperationCode code, ByteArray & response, const IObjectInputStreamPtr & inputStream, Args && ... args)
	{
#if 0
		try
		{ _packeter.PollEvent(_defaultTimeout); }
		catch(const std::exception &ex)
		{ error("exception in interrupt: ", ex.what()); }
#endif

		scoped_mutex_lock l(_mutex);
		if (!_deviceInfo.Supports(code))
			throw std::runtime_error("Operation code " + ToString(code) + " not supported.");
		Transaction transaction(this);
		Send(OperationRequest(code, transaction.Id, std::forward<Args>(args) ... ), timeout);
		if (inputStream)
		{
			DataRequest req(code, transaction.Id);
			Container container(req, inputStream);

			// // Log data being sent
			// std::cout << "[USB TX] Data for 0x" << std::hex << std::setw(4) << std::setfill('0')
			//           << (int)code << std::dec << " TransID=" << transaction.Id
			//           << " (" << inputStream->GetSize() << " bytes)" << std::endl;

			if (_separateBulkWrites)
			{
				_packeter.Write(std::make_shared<ByteArrayObjectInputStream>(container.Data), timeout);
				_packeter.Write(inputStream, timeout);
			} else
				_packeter.Write(std::make_shared<JoinedObjectInputStream>(std::make_shared<ByteArrayObjectInputStream>(container.Data), inputStream), timeout);
		}
		return Get(transaction.Id, response);
	}

	msg::DeviceInfo Session::GetDeviceInfo(PipePacketer& packeter, u32 transactionId, int timeout)
	{
		ByteArray response;
		Send(packeter, OperationRequest(OperationCode::GetDeviceInfo, transactionId), timeout);
		auto info = Get(packeter, 0, response, timeout);
		return ParseResponse<msg::DeviceInfo>(info);
	}


	msg::ObjectHandles Session::GetObjectHandles(StorageId storageId, ObjectFormat objectFormat, ObjectId parent, int timeout)
	{ return ParseResponse<msg::ObjectHandles>(RunTransaction(timeout, OperationCode::GetObjectHandles, storageId.Id, static_cast<u32>(objectFormat), parent.Id)); }

	msg::StorageIDs Session::GetStorageIDs()
	{ return ParseResponse<msg::StorageIDs>(RunTransaction(_defaultTimeout, OperationCode::GetStorageIDs)); }

	msg::StorageInfo Session::GetStorageInfo(StorageId storageId)
	{ return ParseResponse<msg::StorageInfo>(RunTransaction(_defaultTimeout, OperationCode::GetStorageInfo, storageId.Id)); }

	msg::SendObjectPropListResponse Session::SendObjectPropList(StorageId storageId, ObjectId parentId, ObjectFormat format, u64 objectSize, const ByteArray & propList)
	{
		ByteArray responseData;
		IObjectInputStreamPtr inputStream = std::make_shared<ByteArrayObjectInputStream>(propList);
		RunTransactionWithDataRequest(_defaultTimeout, OperationCode::SendObjectPropList, responseData, inputStream, storageId.Id, parentId.Id, static_cast<u32>(format), static_cast<u32>(objectSize >> 32), static_cast<u32>(objectSize));
		return ParseResponse<msg::SendObjectPropListResponse>(responseData);
	}

	msg::NewObjectInfo Session::CreateDirectory(const std::string &name, ObjectId parentId, StorageId storageId, AssociationType type)
	{
		if (_deviceInfo.Supports(OperationCode::SendObjectPropList))
		{
			//modern way of creating objects
			ByteArray propList;
			{
				OutputStream os(propList);
				os.Write32(1); //number of props
				os.Write32(0); //object handle
				os.Write16(static_cast<u16>(ObjectProperty::ObjectFilename));
				os.Write16(static_cast<u16>(DataTypeCode::String));
				os.WriteString(name);
			}
			auto response = SendObjectPropList(storageId, parentId, ObjectFormat::Association, 0, propList);

			msg::NewObjectInfo noi;
			noi.StorageId = response.StorageId;
			noi.ParentObjectId = response.ParentObjectId;
			noi.ObjectId = response.ObjectId;
			return noi;
		}
		else
		{
			mtp::msg::ObjectInfo oi;
			oi.Filename = name;
			oi.ParentObject = parentId;
			oi.StorageId = storageId;
			oi.ObjectFormat = mtp::ObjectFormat::Association;
			oi.AssociationType = type;
			return SendObjectInfo(oi, storageId, parentId);
		}
	}

	msg::ObjectInfo Session::GetObjectInfo(ObjectId objectId)
	{ return ParseResponse<msg::ObjectInfo>(RunTransaction(_defaultTimeout, OperationCode::GetObjectInfo, objectId.Id)); }

	msg::ObjectPropertiesSupported Session::GetObjectPropertiesSupported(ObjectFormat format)
	{ return ParseResponse<msg::ObjectPropertiesSupported>(RunTransaction(_defaultTimeout, OperationCode::GetObjectPropsSupported, static_cast<u32>(format))); }

	ByteArray Session::GetObjectPropertyDesc(ObjectProperty code, ObjectFormat format)
	{ return RunTransaction(_defaultTimeout, OperationCode::GetObjectPropDesc, static_cast<u32>(code), static_cast<u32>(format)); }

	void Session::GetObject(ObjectId objectId, const IObjectOutputStreamPtr &outputStream)
	{
		scoped_mutex_lock l(_mutex);
		Transaction transaction(this);
		Send(OperationRequest(OperationCode::GetObject, transaction.Id, objectId.Id));
		ByteArray response;
		ResponseType responseCode;
		_packeter.Read(transaction.Id, outputStream, responseCode, response, _defaultTimeout);
		CHECK_RESPONSE(responseCode);
	}

	void Session::GetThumb(ObjectId objectId, const IObjectOutputStreamPtr &outputStream)
	{
		scoped_mutex_lock l(_mutex);
		Transaction transaction(this);
		Send(OperationRequest(OperationCode::GetThumb, transaction.Id, objectId.Id));
		ByteArray response;
		ResponseType responseCode;
		_packeter.Read(transaction.Id, outputStream, responseCode, response, _defaultTimeout);
		CHECK_RESPONSE(responseCode);
	}

	ByteArray Session::GetPartialObject(ObjectId objectId, u64 offset, u32 size)
	{
		if (_getPartialObject64Supported)
			return RunTransaction(_defaultTimeout, OperationCode::GetPartialObject64, objectId.Id, offset, offset >> 32, size);
		else
		{
			if (offset + size > std::numeric_limits<u32>::max())
				throw std::runtime_error("32 bit overflow for GetPartialObject");
			return RunTransaction(_defaultTimeout, OperationCode::GetPartialObject, objectId.Id, offset, size);
		}
	}


	msg::NewObjectInfo Session::SendObjectInfo(const msg::ObjectInfo &objectInfo, StorageId storageId, ObjectId parentObject)
	{
		if (objectInfo.Filename.empty())
			throw std::runtime_error("object filename must not be empty");

		if (_deviceInfo.Supports(OperationCode::SendObjectPropList))
		{
			//modern way of creating objects
			ByteArray propList;
			{
				OutputStream os(propList);
				os.Write32(1); //number of props
				os.Write32(0); //object handle
				os.Write16(static_cast<u16>(ObjectProperty::ObjectFilename));
				os.Write16(static_cast<u16>(DataTypeCode::String));
				os.WriteString(objectInfo.Filename);
			}

			auto response = SendObjectPropList(storageId, parentObject, objectInfo.ObjectFormat, objectInfo.ObjectCompressedSize, propList);

			msg::NewObjectInfo noi;
			noi.StorageId = response.StorageId;
			noi.ParentObjectId = response.ParentObjectId;
			noi.ObjectId = response.ObjectId;
			return noi;
		}

		scoped_mutex_lock l(_mutex);
		Transaction transaction(this);
		Send(OperationRequest(OperationCode::SendObjectInfo, transaction.Id, storageId.Id, parentObject.Id));
		{
			DataRequest req(OperationCode::SendObjectInfo, transaction.Id);
			if (_separateBulkWrites)
			{
				ByteArray data;
				OutputStream os(data);
				objectInfo.Write(os);
				auto is = std::make_shared<ByteArrayObjectInputStream>(data);

				Container container(req, is);
				_packeter.Write(container.Data, _defaultTimeout);

				_packeter.Write(is, _defaultTimeout);
			}
			else
			{
				OutputStream stream(req.Data);
				objectInfo.Write(stream);
				Container container(req);
				_packeter.Write(container.Data, _defaultTimeout);
			}
		}
		ByteArray data, response;
		ResponseType responseCode;
		_packeter.Read(transaction.Id, data, responseCode, response, _defaultTimeout);
		//HexDump("response", response);
		CHECK_RESPONSE(responseCode);
		return ParseResponse<msg::NewObjectInfo>(response);
	}

	void Session::SendObject(const IObjectInputStreamPtr &inputStream, int timeout)
	{
		scoped_mutex_lock l(_mutex);
		Transaction transaction(this);
		Send(OperationRequest(OperationCode::SendObject, transaction.Id));
		{
			DataRequest req(OperationCode::SendObject, transaction.Id);
			Container container(req, inputStream);
			if (_separateBulkWrites)
			{
				_packeter.Write(container.Data, timeout);
				_packeter.Write(inputStream, timeout);
			} else
				_packeter.Write(std::make_shared<JoinedObjectInputStream>(std::make_shared<ByteArrayObjectInputStream>(container.Data), inputStream), timeout);
		}
		ByteArray response;
		Get(transaction.Id, response);
	}

	void Session::BeginEditObject(ObjectId objectId)
	{ RunTransaction(_defaultTimeout, OperationCode::BeginEditObject, objectId.Id); }

	void Session::SendPartialObject(ObjectId objectId, u64 offset, const ByteArray &data)
	{
		IObjectInputStreamPtr inputStream = std::make_shared<ByteArrayObjectInputStream>(data);
		ByteArray response;
		RunTransactionWithDataRequest(_defaultTimeout, OperationCode::SendPartialObject, response, inputStream, objectId.Id, offset, offset >> 32, data.size());
	}

	void Session::TruncateObject(ObjectId objectId, u64 size)
	{ RunTransaction(_defaultTimeout, OperationCode::TruncateObject, objectId.Id, size, size >> 32); }

	void Session::EndEditObject(ObjectId objectId)
	{ RunTransaction(_defaultTimeout, OperationCode::EndEditObject, objectId.Id); }

	void Session::SetObjectProperty(ObjectId objectId, ObjectProperty property, const ByteArray &value)
	{
		IObjectInputStreamPtr inputStream = std::make_shared<ByteArrayObjectInputStream>(value);
		ByteArray response;
		RunTransactionWithDataRequest(_defaultTimeout, OperationCode::SetObjectPropValue, response, inputStream, objectId.Id, (u16)property);
	}

	StorageId Session::GetObjectStorage(mtp::ObjectId id)
	{
		StorageId storageId(GetObjectIntegerProperty(id, ObjectProperty::StorageId));
		if (storageId == AnyStorage || storageId == AllStorages)
			throw std::runtime_error("returned wildcard storage id as storage for object");
		return storageId;
	}

	ObjectId Session::GetObjectParent(mtp::ObjectId id)
	{ return ObjectId(GetObjectIntegerProperty(id, ObjectProperty::ParentObject)); }

	void Session::SetObjectProperty(ObjectId objectId, ObjectProperty property, const std::string &value)
	{
		ByteArray data;
		data.reserve(value.size() * 2 + 1);
		OutputStream stream(data);
		stream << value;
		SetObjectProperty(objectId, property, data);
	}

	ByteArray Session::GetObjectProperty(ObjectId objectId, ObjectProperty property)
	{ return RunTransaction(_defaultTimeout, OperationCode::GetObjectPropValue, objectId.Id, (u16)property); }

	u64 Session::GetObjectIntegerProperty(ObjectId objectId, ObjectProperty property)
	{
		if (!_getObjectPropValueSupported) {
			auto info = GetObjectInfo(objectId);
			switch(property)
			{
				case ObjectProperty::StorageId:
					return info.StorageId.Id;
				case ObjectProperty::ObjectFormat:
					return static_cast<u32>(info.ObjectFormat);
				case ObjectProperty::ProtectionStatus:
					return info.ProtectionStatus;
				case ObjectProperty::ObjectSize:
					return info.ObjectCompressedSize;
				case ObjectProperty::RepresentativeSampleFormat:
					return info.ThumbFormat;
				case ObjectProperty::RepresentativeSampleSize:
					return info.ThumbCompressedSize;
				case ObjectProperty::RepresentativeSampleWidth:
					return info.ThumbPixWidth;
				case ObjectProperty::RepresentativeSampleHeight:
					return info.ThumbPixHeight;
				case ObjectProperty::Width:
					return info.ImagePixWidth;
				case ObjectProperty::Height:
					return info.ImagePixHeight;
				case ObjectProperty::ImageBitDepth:
					return info.ImageBitDepth;
				case ObjectProperty::ParentObject:
					return info.ParentObject.Id;
				case ObjectProperty::AssociationType:
					return static_cast<u32>(info.AssociationType);
				case ObjectProperty::AssociationDesc:
					return info.AssociationDesc;
				default:
					throw std::runtime_error("Device does not support object properties and no ObjectInfo fallback for " + ToString(property) + ".");
			}
		}
		return ReadSingleInteger(GetObjectProperty(objectId, property));
	}

	void Session::SetObjectProperty(ObjectId objectId, ObjectProperty property, u64 value)
	{
		std::array<u8, sizeof(value)> data;
		std::fill(data.begin(), data.end(), 0);
		size_t i;
		for(i = 0; i < data.size() && value != 0; ++i, value >>= 8)
		{
			data[i] = value;
		}
		if (i <= 4)
			i = 4;
		else
			i = 8;

		SetObjectProperty(objectId, property, ByteArray(data.begin(), data.begin() + i));
	}

	void Session::SetObjectPropertyAsArray(ObjectId objectId, ObjectProperty property, const ByteArray &value)
	{
		auto n = value.size();

		ByteArray array;
		OutputStream out(array);
		array.reserve(n + 4);
		out.WriteArray(value);

		SetObjectProperty(objectId, property, array);
	}

	std::string Session::GetObjectStringProperty(ObjectId objectId, ObjectProperty property)
	{
		if (!_getObjectPropValueSupported) {
			auto info = GetObjectInfo(objectId);
			switch(property)
			{
				case ObjectProperty::ObjectFilename:
					return info.Filename;
				case ObjectProperty::DateCreated:
				case ObjectProperty::DateAuthored:
				case ObjectProperty::DateAdded:
					return info.CaptureDate;
				case ObjectProperty::DateModified:
					return info.ModificationDate;
				default:
					throw std::runtime_error("Device does not support object properties and no ObjectInfo fallback for " + ToString(property) + ".");
			}
		}
		return ReadSingleString(GetObjectProperty(objectId, property));
	}

	time_t Session::GetObjectModificationTime(ObjectId id)
	{
		if (!_getObjectModificationTimeBuggy)
		{
			try
			{
				auto mtimeStr = GetObjectStringProperty(id, mtp::ObjectProperty::DateModified);
				auto mtime = mtp::ConvertDateTime(mtimeStr);
				if (mtime != 0) //long standing Android bug
					return mtime;
			}
			catch(const std::exception &ex)
			{
				debug("exception while getting mtime: ", ex.what());
			}
			_getObjectModificationTimeBuggy = true;
		}
		auto oi = GetObjectInfo(id);
		return mtp::ConvertDateTime(oi.ModificationDate);
	}

	u64 Session::GetDeviceIntegerProperty(DeviceProperty property)
	{ return ReadSingleInteger(GetDeviceProperty(property)); }

	std::string Session::GetDeviceStringProperty(DeviceProperty property)
	{ return ReadSingleString(GetDeviceProperty(property)); }

	void Session::SetDeviceProperty(DeviceProperty property, const ByteArray &value)
	{
		IObjectInputStreamPtr inputStream = std::make_shared<ByteArrayObjectInputStream>(value);
		ByteArray response;
		RunTransactionWithDataRequest(_defaultTimeout, OperationCode::SetDevicePropValue, response, inputStream, (u16)property);
	}

	void Session::SetDeviceProperty(DeviceProperty property, const std::string &value)
	{
		ByteArray data;
		data.reserve(value.size() * 2 + 1);
		OutputStream stream(data);
		stream << value;
		SetDeviceProperty(property, data);
	}

	ByteArray Session::ResetDeviceProperty(DeviceProperty property)
	{ return RunTransaction(_defaultTimeout, OperationCode::ResetDevicePropValue, static_cast<u32>(property)); }

	void Session::SendVendorControlRequest(u8 type, u8 request, u16 value, u16 index, const ByteArray &data, int timeout)
	{
		usb::DevicePtr device = _packeter.GetPipe()->GetDevice();
		if (!device) {
			throw std::runtime_error("USB device not available for control transfer");
		}
		device->WriteControl(type, request, value, index, data, timeout);
	}

	ByteArray Session::ReceiveVendorControlData(u8 type, u8 request, u16 value, u16 index, size_t length, int timeout)
	{
		usb::DevicePtr device = _packeter.GetPipe()->GetDevice();
		if (!device) {
			throw std::runtime_error("USB device not available for control transfer");
		}
		ByteArray data(length);
		device->ReadControl(type, request, value, index, data, timeout);
		return data;
	}

	usb::BulkPipePtr Session::GetBulkPipe() const
	{
		return _packeter.GetPipe();
	}

	void Session::PollEvent(int timeout)
	{
		_packeter.PollEvent(timeout);
	}

	ByteArray Session::GetObjectPropertyList(ObjectId objectId, ObjectFormat format, ObjectProperty property, u32 groupCode, u32 depth, int timeout)
	{ return RunTransaction(timeout, OperationCode::GetObjectPropList, objectId.Id, (u32)format, property != ObjectProperty::All? (u32)property: 0xffffffffu, groupCode, depth); }

	void Session::DeleteObject(ObjectId objectId, int timeout)
	{ RunTransaction(timeout, OperationCode::DeleteObject, objectId.Id, 0); }

	msg::DevicePropertyDesc Session::GetDevicePropertyDesc(DeviceProperty property)
	{ return ParseResponse<msg::DevicePropertyDesc>(RunTransaction(_defaultTimeout, OperationCode::GetDevicePropDesc, static_cast<u32>(property))); }

	ByteArray Session::GetDeviceProperty(DeviceProperty property)
	{ return RunTransaction(_defaultTimeout, OperationCode::GetDevicePropValue, static_cast<u32>(property)); }

	ByteArray Session::GenericOperation(OperationCode code)
	{ return RunTransaction(_defaultTimeout, code); }

	ByteArray Session::GenericOperation(OperationCode code, const ByteArray & payload)
	{
		IObjectInputStreamPtr inputStream = std::make_shared<ByteArrayObjectInputStream>(payload);
		ByteArray response;
		return RunTransactionWithDataRequest(_defaultTimeout, code, response, inputStream);
	}

	void Session::EnableSecureFileOperations(u32 cmac[4])
	{ RunTransaction(_defaultTimeout, OperationCode::EnableTrustedFilesOperations, cmac[0], cmac[1], cmac[2], cmac[3]); }

	void Session::RebootDevice()
	{ RunTransaction(_defaultTimeout, OperationCode::RebootDevice); }

	void Session::EnableWirelessSync()
	{ RunTransaction(_defaultTimeout, (OperationCode)0x9230, 1); }

	void Session::DisableWirelessSync()
	{ RunTransaction(_defaultTimeout, (OperationCode)0x9230, 2); }  // Use parameter 2 (not 0) - verified from Windows capture

	void Session::ClearWirelessPairing()
	{
		// Complete sequence from Windows capture when disabling wireless sync:
		// 1. 0x922f() - prepare/reset
		// 2. 0x9230(2) - clear pairing
		// 3. 0x922b(3,2) - post-clear operation
		// 4. 0x922d(3,3) - cleanup
		// 5. 0x9215() - unknown operation
		// 6. 0x9230(1) - re-enable wireless
		// 7. 0x922b(3,1) - post-enable operation

		RunTransaction(_defaultTimeout, (OperationCode)0x922f);
		RunTransaction(_defaultTimeout, (OperationCode)0x9230, 2);
		RunTransaction(_defaultTimeout, (OperationCode)0x922b, 3, 2);
		RunTransaction(_defaultTimeout, (OperationCode)0x922d, 3, 3);
		RunTransaction(_defaultTimeout, (OperationCode)0x9215);
		RunTransaction(_defaultTimeout, (OperationCode)0x9230, 1);
		RunTransaction(_defaultTimeout, (OperationCode)0x922b, 3, 1);
	}

	// ========================================================================
	// PHASE 1 OPERATIONS (Sync Pairing)
	// ========================================================================

	void Session::Operation9212()
	{ (void)RunTransaction(_defaultTimeout, (OperationCode)0x9212); }

	void Session::Operation9213()
	{ (void)RunTransaction(_defaultTimeout, (OperationCode)0x9213); }

	void Session::Operation9216()
	{ RunTransaction(_defaultTimeout, (OperationCode)0x9216); }

	// HTTP initialization operations
	void Session::Operation1002(u32 param)
	{ RunTransaction(_defaultTimeout, (OperationCode)0x1002, param); }

	void Session::Operation1014(u32 param)
	{ (void)RunTransaction(_defaultTimeout, (OperationCode)0x1014, param); }

	void Session::Operation9801(u32 param)
	{ (void)RunTransaction(_defaultTimeout, (OperationCode)0x9801, param); }

	void Session::Operation9802(u32 param1, u32 param2)
	{ (void)RunTransaction(_defaultTimeout, (OperationCode)0x9802, param1, param2); }
	void Session::Operation9808(u32 storageId, u32 formatCode, u32 parentObject, u32 reserved1, u32 reserved2)
	{ (void)RunTransaction(_defaultTimeout, (OperationCode)0x9808, storageId, formatCode, parentObject, reserved1, reserved2); }

	void Session::Operation9808(u32 storageId, u32 formatCode, u32 parentObject, u32 reserved1, u32 reserved2, const ByteArray& data)
	{
		// Send Operation 0x9808 with data payload (e.g., folder name in UTF-16LE)
		IObjectInputStreamPtr inputStream = std::make_shared<ByteArrayObjectInputStream>(data);
		ByteArray response;
		RunTransactionWithDataRequest(_defaultTimeout, (OperationCode)0x9808, response, inputStream,
			storageId, formatCode, parentObject, reserved1, reserved2);
	}

	void Session::Operation9217(u32 param)
	{ RunTransaction(_defaultTimeout, (OperationCode)0x9217, param); }

	void Session::Operation9218(u32 param1, u32 param2, u32 param3)
	{ RunTransaction(_defaultTimeout, (OperationCode)0x9218, param1, param2, param3); }

	ByteArray Session::Operation9227_Init()
	{
		// Phase 1 usage: SENDS 4 bytes of zeros TO device, device responds with error 0x2002
		// Frame 7038: Command 0x9227
		// Frame 7040: Data packet with 4 bytes: 00000000
		// Frame 7045: Device responds with GeneralError (0x2002) - this is EXPECTED behavior
		ByteArray data(4, 0);  // 4 bytes of zeros
		IObjectInputStreamPtr inputStream = std::make_shared<ByteArrayObjectInputStream>(data);
		ByteArray response;
		RunTransactionWithDataRequest(_defaultTimeout, (OperationCode)0x9227, response, inputStream);
		return response;
	}

	// ========================================================================
	// PHASE 2 OPERATIONS (Wireless Setup)
	// ========================================================================

	ByteArray Session::Operation9224()
	{ return RunTransaction(_defaultTimeout, (OperationCode)0x9224); }

	void Session::Operation9226()
	{ RunTransaction(_defaultTimeout, (OperationCode)0x9226); }

	void Session::Operation9228(u32 param)
	{ RunTransaction(_defaultTimeout, (OperationCode)0x9228, param); }

	void Session::Operation9219(u32 param1, u32 param2, u32 param3)
	{
		// Operation 0x9219 with 3 parameters
		// From capture frame 449: params = (0, 0, 0x1388=5000)
		RunTransaction(_defaultTimeout, (OperationCode)0x9219, param1, param2, param3);
	}
	ByteArray Session::Operation922f(const ByteArray &data)
	{
		// Operation 0x922f RECEIVES data from the device, not sends it
		// The 'data' parameter is ignored - this operation gets data FROM device
		return RunTransaction(_defaultTimeout, (OperationCode)0x922f);
	}

	void Session::Operation9230(u32 mode)
	{ RunTransaction(_defaultTimeout, (OperationCode)0x9230, mode); }

	void Session::Operation9231()
	{
		// Operation 0x9231 sends 258 bytes of zeros (WiFi initialization)
		// From capture frame 284-286: sends 270 total bytes (12 byte header + 258 data)
		ByteArray data(258, 0);  // 258 bytes of zeros
		IObjectInputStreamPtr inputStream = std::make_shared<ByteArrayObjectInputStream>(data);
		ByteArray response;
		RunTransactionWithDataRequest(_defaultTimeout, (OperationCode)0x9231, response, inputStream);
	}

	ByteArray Session::Operation922b(u32 param1, u32 param2, u32 param3)
	{
		// 0x922b sends command with parameters AND data payload TO device
		// From capture: frame 1229 = command(3,1,0), frame 1231 = StartData(272 bytes), frame 1233 = data(260 bytes)
		// Data structure: first 4 bytes = 0x0000c252 (little-endian), rest = zeros
		ByteArray data(260, 0);  // 260 bytes of zeros
		data[0] = 0x02;
		data[1] = 0x52;
		data[2] = 0xc0;
		data[3] = 0x00;

		ByteArray response;
		auto stream = std::make_shared<ByteArrayObjectInputStream>(data);
		return RunTransactionWithDataRequest(_defaultTimeout, (OperationCode)0x922b, response, stream, param1, param2, param3);
	}

	void Session::Operation922c(const ByteArray &data, u32 param1, u32 param2)
	{
		// Phase 2 Section 1: Op_0x922c sends data to device
		// Data is loaded from hex file and passed in
		// First call: phase2_section1_data/op_0x922c_first.hex (83 bytes)
		// Second call: phase2_section1_data/op_0x922c_second.hex (83 bytes)
		IObjectInputStreamPtr inputStream = std::make_shared<ByteArrayObjectInputStream>(data);
		ByteArray response;
		RunTransactionWithDataRequest(_defaultTimeout, (OperationCode)0x922c, response, inputStream, param1, param2);
	}

	ByteArray Session::Operation922d(u32 param1, u32 param2)
	{
		// Phase 2 Section 1: Op_0x922d sometimes returns DATA
		// Usually returns just OK response, but after Op_0x922f or Op_0x922c it returns DATA + OK
		return RunTransaction(_defaultTimeout, (OperationCode)0x922d, param1, param2);
	}

	void Session::Operation922a(const std::string &album_name)
	{
		// Operation 0x922A: Register album context for metadata retrieval
		// Called after artist object deletion, before metadata requests
		// Sends 530 bytes of data to device with album name
		// Data structure from capture frame 5956:
		//   Bytes 0-7:   uint64 = 0
		//   Bytes 8-11:  uint32 = 100
		//   Bytes 12-15: uint32 = 0
		//   Bytes 16-19: uint32 = 1
		//   Bytes 20+:   UTF-16LE album name (null-terminated)
		//   Padding:     zeros to 530 bytes total

		ByteArray data(530, 0);  // 530 bytes, initialized to zeros

		// Write uint64 = 0 (bytes 0-7 already zero)

		// Write uint32 = 100 at bytes 8-11 (little-endian)
		data[8] = 0x64;  // 100 decimal
		data[9] = 0x00;
		data[10] = 0x00;
		data[11] = 0x00;

		// Bytes 12-15 already zero (uint32 = 0)

		// Write uint32 = 1 at bytes 16-19 (little-endian)
		data[16] = 0x01;
		data[17] = 0x00;
		data[18] = 0x00;
		data[19] = 0x00;

		// Write UTF-16LE album name starting at byte 20
		size_t offset = 20;
		for (char c : album_name) {
			if (offset + 1 < 530) {
				data[offset++] = static_cast<u8>(c);  // Low byte (ASCII char)
				data[offset++] = 0x00;  // High byte (for basic ASCII)
			}
		}
		// Add null terminator (UTF-16LE null = 0x0000)
		if (offset + 1 < 530) {
			data[offset++] = 0x00;
			data[offset++] = 0x00;
		}
		// Remaining bytes already zero (padding)

		ByteArray response;
		auto stream = std::make_shared<ByteArrayObjectInputStream>(data);
		RunTransactionWithDataRequest(_defaultTimeout, (OperationCode)0x922a, response, stream);
	}

	void Session::Operation9215()
	{ RunTransaction(_defaultTimeout, (OperationCode)0x9215); }

	ByteArray Session::GetWiFiNetworkList()
	{ return RunTransaction(_defaultTimeout, (OperationCode)0x9225); }

	void Session::SetWiFiConfiguration(const ByteArray &configData)
	{
		IObjectInputStreamPtr inputStream = std::make_shared<ByteArrayObjectInputStream>(configData);
		ByteArray response;
		RunTransactionWithDataRequest(_defaultTimeout, (OperationCode)0x9227, response, inputStream);
	}


	Session::ObjectEditSession::ObjectEditSession(const SessionPtr & session, ObjectId objectId): _session(session), _objectId(objectId)
	{ session->BeginEditObject(objectId); }

	Session::ObjectEditSession::~ObjectEditSession()
	{ _session->EndEditObject(_objectId); }

	void Session::ObjectEditSession::Truncate(u64 size)
	{ _session->TruncateObject(_objectId, size); }

	void Session::ObjectEditSession::Send(u64 offset, const ByteArray &data)
	{ _session->SendPartialObject(_objectId, offset, data); }

	void Session::AbortCurrentTransaction(int timeout)
	{
		u32 transactionId;
		{
			scoped_mutex_lock l(_transactionMutex);
			if (!_transaction)
				throw std::runtime_error("no transaction in progress");
			transactionId = _transaction->Id;
		}
		_packeter.Abort(transactionId, timeout);
	}

	void Session::SetObjectReferences(ObjectId objectId, const msg::ObjectHandles & objects)
	{
		ByteArray data;
		OutputStream out(data);
		objects.Write(out);

		IObjectInputStreamPtr inputStream = std::make_shared<ByteArrayObjectInputStream>(data);
		ByteArray response;
		RunTransactionWithDataRequest(_defaultTimeout, OperationCode::SetObjectReferences, response, inputStream, objectId.Id);
	}

	msg::ObjectHandles Session::GetObjectReferences(ObjectId objectId)
	{ return ParseResponse<msg::ObjectHandles>(RunTransaction(_defaultTimeout, OperationCode::GetObjectReferences, objectId.Id)); }

}
