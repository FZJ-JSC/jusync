#pragma once

#include <string>
#include <atomic>
#include <memory>
#include <vector>
#include "MiddlewareLogging.h"

/**
 * GPU Context Management for CUDA Acceleration
 *
 * Provides CUDA device detection, initialization, and error handling
 * with automatic fallback to CPU when GPU is unavailable.
 */
namespace anari_usd_middleware {

/**
 * GPU device information structure
 */
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

/**
 * GPU Context singleton for managing CUDA resources
 *
 * Thread-safe singleton that manages CUDA device initialization,
 * error handling, and provides utilities for GPU operations.
 */
class ANARI_USD_MIDDLEWARE_API GpuContext {
public:
    /**
     * Get singleton instance
     */
    static GpuContext& getInstance();

    /**
     * Check if GPU is available and initialized
     */
    static bool isAvailable();

    /**
     * Get current GPU device info
     */
    static const GpuDeviceInfo& getDeviceInfo();

    /**
     * Get CUDA error string
     * Uses int parameter to avoid cuda_runtime.h dependency in header
     */
    static std::string getErrorString(int error);

    /**
     * Check CUDA error and log if non-zero
     * Uses int parameter to avoid cuda_runtime.h dependency in header
     */
    static bool checkCudaError(int error, const char* operation, const char* file, int line);

    /**
     * Initialize GPU context
     * @return true if initialization successful
     */
    bool initialize();

    /**
     * Shutdown GPU context and release resources
     */
    void shutdown();

    /**
     * Get current device ID
     */
    int getDeviceId() const { return deviceInfo.deviceId; }

    /**
     * Get recommended block size for kernels
     */
    static int getRecommendedBlockSize() { return 256; }

    /**
     * Calculate grid size for given element count
     */
    static int calculateGridSize(size_t elementCount, int blockSize = 256);

    /**
     * Check if GPU meets minimum requirements
     */
    bool meetsMinimumRequirements() const;

    /**
     * Get initialization status
     */
    bool isInitialized() const { return initialized.load(); }

private:
    GpuContext();
    ~GpuContext();

    // Delete copy constructor and assignment
    GpuContext(const GpuContext&) = delete;
    GpuContext& operator=(const GpuContext&) = delete;

    /**
     * Detect available CUDA devices
     */
    bool detectDevices();

    /**
     * Select best available device
     */
    bool selectDevice(int deviceId = -1);

    /**
     * Initialize device properties
     */
    bool initializeDeviceProperties();

private:
    std::atomic<bool> initialized{false};
    std::atomic<bool> available{false};
    GpuDeviceInfo deviceInfo;
    std::vector<GpuDeviceInfo> availableDevices;
};

#else // ENABLE_CUDA_ACCELERATION

// Stub class when CUDA is not available
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

// CUDA error checking macro (only defined when CUDA is available)
#ifdef ENABLE_CUDA_ACCELERATION
#define GPU_CHECK(error) \
    GpuContext::checkCudaError(error, #error, __FILE__, __LINE__)
#else
#define GPU_CHECK(error) true
#endif

} // namespace anari_usd_middleware
