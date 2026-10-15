#pragma once

#include <string>
#include <atomic>
#include <memory>
#include <vector>
#include "MiddlewareLogging.h"

namespace anari_usd_middleware {

struct GpuDeviceInfo {
    int deviceId = -1;
    std::string deviceName;
    int computeCapabilityMajor = 0;
    int computeCapabilityMinor = 0;
    size_t totalMemory = 0;
    size_t freeMemory = 0;
    int multiprocessorCount = 0;
    bool isAvailable = false;
};

#ifdef ENABLE_CUDA_ACCELERATION

class ANARI_USD_MIDDLEWARE_API GpuContext {
public:
    static GpuContext& getInstance();

    static bool isAvailable();

    static const GpuDeviceInfo& getDeviceInfo();

    static std::string getErrorString(int error);

    static bool checkCudaError(int error, const char* operation, const char* file, int line);

    bool initialize();

    void shutdown();

    int getDeviceId() const { return deviceInfo.deviceId; }

    static int getRecommendedBlockSize() { return 256; }

    static int calculateGridSize(size_t elementCount, int blockSize = 256);

    bool meetsMinimumRequirements() const;

    bool isInitialized() const { return initialized.load(); }

private:
    GpuContext();
    ~GpuContext();

    GpuContext(const GpuContext&) = delete;
    GpuContext& operator=(const GpuContext&) = delete;

    bool detectDevices();

    bool selectDevice(int deviceId = -1);

    bool initializeDeviceProperties();

private:
    std::atomic<bool> initialized{false};
    std::atomic<bool> available{false};
    GpuDeviceInfo deviceInfo;
    std::vector<GpuDeviceInfo> availableDevices;
};

#else // ENABLE_CUDA_ACCELERATION

class ANARI_USD_MIDDLEWARE_API GpuContext {
public:
    static GpuContext& getInstance() {
        static GpuContext instance;
        return instance;
    }

    static bool isAvailable() { return false; }

    static const GpuDeviceInfo& getDeviceInfo() {
        static GpuDeviceInfo info;
        return info;
    }

    bool initialize() { return false; }
    void shutdown() {}
    int getDeviceId() const { return -1; }
    static int getRecommendedBlockSize() { return 256; }
    static int calculateGridSize(size_t, int = 256) { return 0; }
    bool meetsMinimumRequirements() const { return false; }
    bool isInitialized() const { return false; }
};

#endif // ENABLE_CUDA_ACCELERATION

#ifdef ENABLE_CUDA_ACCELERATION
#define GPU_CHECK(error) \
    GpuContext::checkCudaError(error, #error, __FILE__, __LINE__)
#else
#define GPU_CHECK(error) true
#endif

} // namespace anari_usd_middleware
