#pragma once

#include <memory>
#include <string>
#include <functional>
#include <vector>
#include <mutex>

#include <mtp/ptp/Session.h>
#include <usb/Device.h>
#include <usb/Interface.h>

#include "protocols/http/ZuneHTTPInterceptor.h"
#include "ZuneTypes.h"


// Forward declarations
struct InterceptorConfig;



class NetworkManager {
public:
    using LogCallback = std::function<void(const std::string& message)>;
    using PathResolverCallback = const char* (*)(const char* artist_uuid, const char* endpoint_type, const char* resource_id, void* user_data);
    using CacheStorageCallback = bool (*)(const char* artist_uuid, const char* endpoint_type, const char* resource_id, const void* data, size_t data_length, const char* content_type, void* user_data);

    NetworkManager(std::shared_ptr<mtp::Session> mtp_session, LogCallback log_callback);
    ~NetworkManager();

    // --- Artist Metadata HTTP Interception ---
    bool InitializeHTTPSubsystem();  // Must be called before StartHTTPInterceptor
    void StartHTTPInterceptor(const InterceptorConfig& config);
    void StopHTTPInterceptor();
    bool IsHTTPInterceptorRunning() const;
    InterceptorConfig GetHTTPInterceptorConfig() const;
    void TriggerNetworkMode();  // Send 0x922c(3,3) to initiate PPP/HTTP after track upload
    void EnableNetworkPolling();  // Start 0x922d polling - call AFTER TriggerNetworkMode()
    void SetVerboseNetworkLogging(bool enable);  // Enable/disable verbose TCP/IP packet logging

    // Callback registration for hybrid mode
    void SetPathResolverCallback(PathResolverCallback callback, void* user_data);
    void SetCacheStorageCallback(CacheStorageCallback callback, void* user_data);

    // --- Raw USB Access (for monitoring without MTP session) ---
    // Extracts USB device/interface/endpoints from MTP session for raw monitoring
    // Call this BEFORE Disconnect() - discovers endpoints while interface is still claimed
    // The handles and endpoints remain valid after MTP closes
    USBHandlesWithEndpoints ExtractUSBHandles();

private:
    std::shared_ptr<mtp::Session> mtp_session_;
    LogCallback log_callback_;
    std::unique_ptr<ZuneHTTPInterceptor> http_interceptor_;
    bool verbose_logging_ = true;

    void Log(const std::string& message);
    void VerboseLog(const std::string& message);
};
