#include "GpuContext.h"
#include <mutex>
#include <sstream>

#ifdef ENABLE_CUDA_ACCELERATION
#ifdef _WIN32
#include <cuda_runtime.h>
#else
#include <cuda_runtime.h>
#endif
#endif

namespace anari_usd_middleware {

#ifdef ENABLE_CUDA_ACCELERATION

// Static member for singleton
GpuContext& GpuContext::getInstance() {
    static GpuContext instance;
    return instance;
}

bool GpuContext::isAvailable() {
    return getInstance().available.load();
}

const GpuDeviceInfo& GpuContext::getDeviceInfo() {
    return getInstance().deviceInfo;
}

std::string GpuContext::getErrorString(int error) {
    return cudaGetErrorString(static_cast<cudaError_t>(error));
}

bool GpuContext::checkCudaError(int error, const char* operation,
                                const char* file, int line) {
    if (error == cudaSuccess) {
        return true;
    }

    std::string errorMsg = getErrorString(error);
    MIDDLEWARE_LOG_ERROR("CUDA error in %s at %s:%d: %s (error code: %d)",
                        operation, file, line, errorMsg.c_str(), error);
    
    return false;
}

int GpuContext::calculateGridSize(size_t elementCount, int blockSize) {
    return static_cast<int>((elementCount + blockSize - 1) / blockSize);
}

bool GpuContext::meetsMinimumRequirements() const {
    // Minimum compute capability 3.5 (Kepler)
    return (deviceInfo.computeCapabilityMajor > 3 ||
            (deviceInfo.computeCapabilityMajor == 3 && 
             deviceInfo.computeCapabilityMinor >= 5));
}

GpuContext::GpuContext() {
    MIDDLEWARE_LOG_INFO("GpuContext constructor called");
}

GpuContext::~GpuContext() {
    shutdown();
}

bool GpuContext::initialize() {
    if (initialized.load()) {
        MIDDLEWARE_LOG_INFO("GPU context already initialized");
        return available.load();
    }

    MIDDLEWARE_LOG_INFO("Initializing GPU context...");

    // Check CUDA runtime version
    int runtimeVersion = 0;
    cudaError_t err = cudaRuntimeGetVersion(&runtimeVersion);
    if (err != cudaSuccess) {
        MIDDLEWARE_LOG_ERROR("Failed to get CUDA runtime version: %s", getErrorString(err).c_str());
        available.store(false);
        initialized.store(true);
        return false;
    }

    MIDDLEWARE_LOG_INFO("CUDA runtime version: %d.%d", 
                       runtimeVersion / 1000, (runtimeVersion % 100) / 10);

    // Detect available devices
    if (!detectDevices()) {
        MIDDLEWARE_LOG_WARNING("No CUDA devices detected - GPU acceleration disabled");
        available.store(false);
        initialized.store(true);
        return false;
    }

    if (availableDevices.empty()) {
        MIDDLEWARE_LOG_WARNING("No suitable CUDA devices found - GPU acceleration disabled");
        available.store(false);
        initialized.store(true);
        return false;
    }

    // Select best device (default to first available)
    if (!selectDevice(-1)) {
        MIDDLEWARE_LOG_ERROR("Failed to select CUDA device");
        available.store(false);
        initialized.store(true);
        return false;
    }

    // Initialize device properties
    if (!initializeDeviceProperties()) {
        MIDDLEWARE_LOG_ERROR("Failed to initialize device properties");
        available.store(false);
        initialized.store(true);
        return false;
    }

    // Check minimum requirements
    if (!meetsMinimumRequirements()) {
        MIDDLEWARE_LOG_WARNING("GPU does not meet minimum requirements (compute capability >= 3.5)");
        available.store(false);
        initialized.store(true);
        return false;
    }

    initialized.store(true);
    available.store(true);

    MIDDLEWARE_LOG_INFO("GPU context initialized successfully");
    MIDDLEWARE_LOG_INFO("  Device: %s", deviceInfo.deviceName.c_str());
    MIDDLEWARE_LOG_INFO("  Compute Capability: %d.%d", 
                       deviceInfo.computeCapabilityMajor, 
                       deviceInfo.computeCapabilityMinor);
    MIDDLEWARE_LOG_INFO("  Total Memory: %.2f GB", 
                       static_cast<double>(deviceInfo.totalMemory) / (1024.0 * 1024.0 * 1024.0));
    MIDDLEWARE_LOG_INFO("  Multiprocessors: %d", deviceInfo.multiprocessorCount);

    return true;
}

void GpuContext::shutdown() {
    if (!initialized.load()) {
        return;
    }

    MIDDLEWARE_LOG_INFO("Shutting down GPU context");

    // Reset device
    cudaError_t err = cudaDeviceReset();
    if (err != cudaSuccess) {
        MIDDLEWARE_LOG_WARNING("CUDA device reset failed: %s", getErrorString(err).c_str());
    }

    initialized.store(false);
    available.store(false);
    deviceInfo = GpuDeviceInfo();
    availableDevices.clear();

    MIDDLEWARE_LOG_INFO("GPU context shutdown complete");
}

bool GpuContext::detectDevices() {
    int deviceCount = 0;
    cudaError_t err = cudaGetDeviceCount(&deviceCount);
    
    if (err != cudaSuccess) {
        MIDDLEWARE_LOG_ERROR("Failed to get CUDA device count: %s", getErrorString(err).c_str());
        return false;
    }

    if (deviceCount == 0) {
        MIDDLEWARE_LOG_WARNING("No CUDA devices found");
        return false;
    }

    MIDDLEWARE_LOG_INFO("Found %d CUDA device(s)", deviceCount);

    availableDevices.resize(deviceCount);
    for (int i = 0; i < deviceCount; ++i) {
        cudaDeviceProp prop;
        err = cudaGetDeviceProperties(&prop, i);
        if (err != cudaSuccess) {
            MIDDLEWARE_LOG_WARNING("Failed to get properties for device %d: %s", 
                                 i, getErrorString(err).c_str());
            continue;
        }

        GpuDeviceInfo info;
        info.deviceId = i;
        info.deviceName = prop.name;
        info.computeCapabilityMajor = prop.major;
        info.computeCapabilityMinor = prop.minor;
        info.totalMemory = prop.totalGlobalMem;
        info.multiprocessorCount = prop.multiProcessorCount;
        info.isAvailable = true;

        // Get free memory
        err = cudaMemGetInfo(&info.freeMemory, &info.totalMemory);
        if (err != cudaSuccess) {
            info.freeMemory = 0;
        }

        availableDevices[i] = info;

        MIDDLEWARE_LOG_INFO("  Device %d: %s (CC %d.%d, %zu MB)",
                           i, info.deviceName.c_str(),
                           info.computeCapabilityMajor, info.computeCapabilityMinor,
                           info.totalMemory / (1024 * 1024));
    }

    return !availableDevices.empty();
}

bool GpuContext::selectDevice(int deviceId) {
    if (availableDevices.empty()) {
        return false;
    }

    // If specific device requested, use it
    if (deviceId >= 0 && deviceId < static_cast<int>(availableDevices.size())) {
        deviceInfo = availableDevices[deviceId];
    } else {
        // Select device with most memory (simple heuristic)
        size_t maxMemory = 0;
        for (const auto& device : availableDevices) {
            if (device.totalMemory > maxMemory) {
                maxMemory = device.totalMemory;
                deviceInfo = device;
            }
        }
    }

    MIDDLEWARE_LOG_INFO("Selected GPU device %d: %s", 
                       deviceInfo.deviceId, deviceInfo.deviceName.c_str());

    // Set device
    cudaError_t err = cudaSetDevice(deviceInfo.deviceId);
    if (err != cudaSuccess) {
        MIDDLEWARE_LOG_ERROR("Failed to set CUDA device %d: %s",
                           deviceInfo.deviceId, getErrorString(err).c_str());
        return false;
    }

    return true;
}

bool GpuContext::initializeDeviceProperties() {
    // Device is already set, just verify it's working
    int device = 0;
    cudaError_t err = cudaGetDevice(&device);
    if (err != cudaSuccess) {
        MIDDLEWARE_LOG_ERROR("Failed to get current device: %s", getErrorString(err).c_str());
        return false;
    }

    // Verify device matches selected
    if (device != deviceInfo.deviceId) {
        MIDDLEWARE_LOG_WARNING("Device mismatch: expected %d, got %d",
                             deviceInfo.deviceId, device);
    }

    return true;
}

#endif // ENABLE_CUDA_ACCELERATION

} // namespace anari_usd_middleware
