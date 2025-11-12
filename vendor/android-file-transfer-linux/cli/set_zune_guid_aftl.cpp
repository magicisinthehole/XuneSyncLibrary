/*
 * Set Zune PC GUID using Android File Transfer library with MTPZ auth
 */

#include <mtp/ptp/Device.h>
#include <mtp/ptp/Session.h>
#include <mtp/ptp/DeviceProperty.h>
#include <mtp/mtpz/TrustedApp.h>
#include <usb/Context.h>
#include <iostream>
#include <fstream>
#include <iomanip>

std::string GetMtpzDataPath() {
	char * home = getenv("HOME");
	if (home)
		return std::string(home) + "/.mtpz-data";
	else
		return ".mtpz-data";
}

std::string ReadGuidFromFile(const std::string& path) {
	std::ifstream file(path);
	std::string guid;
	if (file) {
		std::getline(file, guid);
	}
	return guid;
}

std::string FormatGuidWithBraces(const std::string& guid) {
	// Convert UUID to Windows format with braces
	std::string upper = guid;
	for (char& c : upper) {
		c = toupper(c);
	}
	return "{" + upper + "}";
}

mtp::ByteArray EncodeGuidAsUtf16(const std::string& guid_with_braces) {
	// PTP String format: uint8 char count + UTF-16LE data + null terminator
	mtp::ByteArray result;

	// Char count (including null terminator)
	result.push_back(guid_with_braces.size() + 1);

	// UTF-16LE encoding
	for (char c : guid_with_braces) {
		result.push_back(c);
		result.push_back(0);
	}

	// Null terminator
	result.push_back(0);
	result.push_back(0);

	return result;
}

int main() {
	std::cout << "==================================================================" << std::endl;
	std::cout << "  Zune PC GUID Setter (using AFTL with MTPZ authentication)" << std::endl;
	std::cout << "==================================================================" << std::endl;
	std::cout << std::endl;

	try {
		// Read Mac GUID
		std::string mac_guid = ReadGuidFromFile(".mac-zune-guid");
		if (mac_guid.empty()) {
			std::cerr << "Error: Could not read .mac-zune-guid file" << std::endl;
			return 1;
		}

		std::string guid_str = FormatGuidWithBraces(mac_guid);
		std::cout << "Mac GUID: " << guid_str << std::endl;
		std::cout << std::endl;

		// Create USB context and find device
		std::cout << "Connecting to USB device..." << std::endl;
		mtp::usb::ContextPtr ctx(new mtp::usb::Context);
		mtp::DevicePtr device = mtp::Device::FindFirst(ctx, nullptr, true, false);
		if (!device) {
			std::cerr << "Error: No MTP device found" << std::endl;
			return 1;
		}

		// Open session
		std::cout << "Opening MTP session..." << std::endl;
		mtp::SessionPtr session = device->OpenSession(1);

		// MTPZ Authentication
		std::cout << "Starting MTPZ authentication..." << std::endl;
		mtp::TrustedAppPtr trustedApp = mtp::TrustedApp::Create(session, GetMtpzDataPath());
		if (trustedApp) {
			trustedApp->Authenticate();
			std::cout << "✅ MTPZ authentication successful!" << std::endl;
		} else {
			std::cerr << "Warning: Could not create TrustedApp" << std::endl;
		}
		std::cout << std::endl;

		// Encode GUID
		mtp::ByteArray guid_data = EncodeGuidAsUtf16(guid_str);

		// Set property 0xd401 (SynchronizationPartner)
		std::cout << "Setting property 0xd401 (Synchronization Partner)..." << std::endl;
		try {
			session->SetDeviceProperty((mtp::DeviceProperty)0xd401, guid_data);
			std::cout << "✅ Successfully set property 0xd401!" << std::endl;
		} catch (const std::exception& e) {
			std::cerr << "❌ Failed to set 0xd401: " << e.what() << std::endl;
		}
		std::cout << std::endl;

		// Set property 0xd220 (PC GUID - custom Zune property)
		std::cout << "Setting property 0xd220 (PC GUID)..." << std::endl;
		try {
			session->SetDeviceProperty((mtp::DeviceProperty)0xd220, guid_data);
			std::cout << "✅ Successfully set property 0xd220!" << std::endl;
		} catch (const std::exception& e) {
			std::cerr << "❌ Failed to set 0xd220: " << e.what() << std::endl;
		}
		std::cout << std::endl;

		// Verify properties
		std::cout << "Verifying properties..." << std::endl;
		try {
			mtp::ByteArray value = session->GetDeviceProperty((mtp::DeviceProperty)0xd220);
			std::cout << "Property 0xd220 value (" << value.size() << " bytes)" << std::endl;
		} catch (const std::exception& e) {
			std::cerr << "Could not read 0xd220: " << e.what() << std::endl;
		}

		std::cout << std::endl;
		std::cout << "Done!" << std::endl;

	} catch (const std::exception& e) {
		std::cerr << "Fatal error: " << e.what() << std::endl;
		return 1;
	}

	return 0;
}
