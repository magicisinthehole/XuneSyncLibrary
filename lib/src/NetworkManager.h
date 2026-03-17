#pragma once

#include <memory>
#include <string>
#include <functional>
#include <vector>
#include <mutex>
#include <atomic>
#include <condition_variable>

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
    void EnableNetworkPolling();  // Enable polling flag - call AFTER TriggerNetworkMode()
    int PollNetworkData(int timeout_ms);  // Single poll cycle - called from C# in a loop
    void RequestShutdown();  // Signal shutdown and wait for in-flight operations to complete
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
    std::shared_ptr<ZuneHTTPInterceptor> http_interceptor_;
    mutable std::mutex interceptor_mutex_;
    bool verbose_logging_ = true;

    // Deferred callbacks — stored until interceptor is created
    PathResolverCallback pending_path_resolver_ = nullptr;
    void* pending_path_resolver_user_data_ = nullptr;
    CacheStorageCallback pending_cache_storage_ = nullptr;
    void* pending_cache_storage_user_data_ = nullptr;

    std::atomic<bool> shutdown_requested_{false};
    std::atomic<int> active_operations_{0};
    std::mutex shutdown_mutex_;
    std::condition_variable shutdown_cv_;

    void Log(const std::string& message);
    void VerboseLog(const std::string& message);
};
