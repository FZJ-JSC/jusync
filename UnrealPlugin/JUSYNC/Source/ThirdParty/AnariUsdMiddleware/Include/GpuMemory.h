#pragma once

#include <vector>
#include <memory>
#include "GpuContext.h"

#ifdef ENABLE_CUDA_ACCELERATION
#include <cuda_runtime.h>

/**
 * GPU Memory Management Utilities
 * 
 * Provides safe GPU memory allocation, deallocation, and data transfer
 * with automatic error handling and cleanup.
 */
namespace anari_usd_middleware {

/**
 * Smart GPU memory buffer with automatic cleanup
 * 
 * Template class for managing GPU device memory with RAII semantics.
 * Automatically frees memory on destruction and provides safe access.
 */
template<typename T>
class GpuBuffer {
public:
    /**
     * Constructor
     */
    GpuBuffer() : devicePtr(nullptr), size(0), ownMemory(true) {}

    /**
     * Create GPU buffer with given size
     * @param count Number of elements
     * @return true if allocation successful
     */
    bool allocate(size_t count) {
        if (devicePtr) {
            free();
        }

        size = count;
        cudaError_t err = cudaMalloc(&devicePtr, size * sizeof(T));
        if (err != cudaSuccess) {
            MIDDLEWARE_LOG_ERROR("GPU memory allocation failed: %s (%zu elements)",
                               GpuContext::getErrorString(err).c_str(), size);
            devicePtr = nullptr;
            size = 0;
            return false;
        }

        MIDDLEWARE_LOG_DEBUG("GPU buffer allocated: %zu elements (%zu bytes)",
                           size, size * sizeof(T));
        return true;
    }

    /**
     * Wrap existing device pointer
     * @param ptr Existing device pointer
     * @param count Number of elements
     * @param owned Whether this buffer owns the memory
     */
    void wrap(T* ptr, size_t count, bool owned = true) {
        devicePtr = ptr;
        size = count;
        ownMemory = owned;
    }

    /**
     * Free GPU memory
     */
    void free() {
        if (devicePtr && ownMemory) {
            cudaError_t err = cudaFree(devicePtr);
            if (err != cudaSuccess) {
                MIDDLEWARE_LOG_WARNING("GPU memory free failed: %s",
                                     GpuContext::getErrorString(err).c_str());
            }
            devicePtr = nullptr;
            size = 0;
        }
    }

    /**
     * Destructor
     */
    ~GpuBuffer() {
        free();
    }

    /**
     * Get device pointer
     */
    T* data() const { return devicePtr; }

    /**
     * Get buffer size
     */
    size_t getSize() const { return size; }

    /**
     * Check if buffer is valid
     */
    bool isValid() const { return devicePtr != nullptr && size > 0; }

    /**
     * Reset buffer
     */
    void reset() {
        free();
    }

private:
    T* devicePtr;
    size_t size;
    bool ownMemory;
};

/**
 * GPU memory utilities
 */
class GpuMemoryUtils {
public:
    /**
     * Copy data from host to device
     * @param dst Device pointer
     * @param src Host pointer
     * @param count Number of elements
     * @param stream CUDA stream (optional)
     * @return true if copy successful
     */
    template<typename T>
    static bool h2d(T* dst, const T* src, size_t count, cudaStream_t stream = 0) {
        if (!dst || !src || count == 0) {
            return false;
        }

        cudaError_t err = cudaMemcpyAsync(dst, src, count * sizeof(T),
                                         cudaMemcpyHostToDevice, stream);
        return GPU_CHECK(err);
    }

    /**
     * Copy data from device to host
     * @param dst Host pointer
     * @param src Device pointer
     * @param count Number of elements
     * @param stream CUDA stream (optional)
     * @return true if copy successful
     */
    template<typename T>
    static bool d2h(T* dst, const T* src, size_t count, cudaStream_t stream = 0) {
        if (!dst || !src || count == 0) {
            return false;
        }

        cudaError_t err = cudaMemcpyAsync(dst, src, count * sizeof(T),
                                         cudaMemcpyDeviceToHost, stream);
        return GPU_CHECK(err);
    }

    /**
     * Copy data from host vector to GPU buffer
     */
    template<typename T>
    static bool h2d(GpuBuffer<T>& gpuBuffer, const std::vector<T>& hostData,
                   cudaStream_t stream = 0) {
        if (!gpuBuffer.isValid() || hostData.empty()) {
            return false;
        }

        if (gpuBuffer.getSize() < hostData.size()) {
            if (!gpuBuffer.allocate(hostData.size())) {
                return false;
            }
        }

        return h2d(gpuBuffer.data(), hostData.data(), hostData.size(), stream);
    }

    /**
     * Copy data from GPU buffer to host vector
     */
    template<typename T>
    static bool d2h(std::vector<T>& hostData, const GpuBuffer<T>& gpuBuffer,
                   cudaStream_t stream = 0) {
        if (!gpuBuffer.isValid()) {
            return false;
        }

        hostData.resize(gpuBuffer.getSize());
        return d2h(hostData.data(), gpuBuffer.data(), gpuBuffer.getSize(), stream);
    }

    /**
     * Set memory to zero
     * @param ptr Device pointer
     * @param count Number of elements
     * @return true if successful
     */
    template<typename T>
    static bool setZero(T* ptr, size_t count) {
        if (!ptr || count == 0) {
            return false;
        }

        cudaError_t err = cudaMemsetAsync(ptr, 0, count * sizeof(T));
        return GPU_CHECK(err);
    }

    /**
     * Synchronize all streams
     * @return true if successful
     */
    static bool synchronize() {
        cudaError_t err = cudaDeviceSynchronize();
        return GPU_CHECK(err);
    }

    /**
     * Synchronize specific stream
     * @param stream CUDA stream
     * @return true if successful
     */
    static bool synchronizeStream(cudaStream_t stream) {
        cudaError_t err = cudaStreamSynchronize(stream);
        return GPU_CHECK(err);
    }

    /**
     * Get free GPU memory
     * @return Free memory in bytes
     */
    static size_t getFreeMemory() {
        size_t freeMemory = 0, totalMemory = 0;
        cudaError_t err = cudaMemGetInfo(&freeMemory, &totalMemory);
        if (err != cudaSuccess) {
            MIDDLEWARE_LOG_WARNING("Failed to get GPU memory info: %s",
                                 GpuContext::getErrorString(err).c_str());
            return 0;
        }
        return freeMemory;
    }

    /**
     * Get total GPU memory
     * @return Total memory in bytes
     */
    static size_t getTotalMemory() {
        size_t freeMemory = 0, totalMemory = 0;
        cudaError_t err = cudaMemGetInfo(&freeMemory, &totalMemory);
        if (err != cudaSuccess) {
            MIDDLEWARE_LOG_WARNING("Failed to get GPU memory info: %s",
                                 GpuContext::getErrorString(err).c_str());
            return 0;
        }
        return totalMemory;
    }

    /**
     * Check if there's enough free memory for allocation
     * @param requiredBytes Required memory in bytes
     * @return true if enough memory available
     */
    static bool hasEnoughMemory(size_t requiredBytes) {
        size_t freeMemory = getFreeMemory();
        return freeMemory >= requiredBytes;
    }

    /**
     * Async copy from device to host with callback
     * @param dst Host pointer
     * @param src Device pointer
     * @param count Number of elements
     * @param stream CUDA stream
     * @param callback Callback invoked when copy completes
     * @return true if async copy started successfully
     */
    template<typename T, typename Callback>
    static bool d2hAsync(T* dst, const T* src, size_t count, cudaStream_t stream, Callback callback) {
        if (!dst || !src || count == 0) {
            callback(false);
            return false;
        }

        cudaError_t err = cudaMemcpyAsync(dst, src, count * sizeof(T),
                                         cudaMemcpyDeviceToHost, stream);
        if (!GPU_CHECK(err)) {
            callback(false);
            return false;
        }

        // Record event after the copy
        cudaEvent_t event;
        err = cudaEventCreate(&event);
        if (!GPU_CHECK(err)) {
            callback(false);
            return false;
        }

        err = cudaEventRecord(event, stream);
        if (!GPU_CHECK(err)) {
            cudaEventDestroy(event);
            callback(false);
            return false;
        }

        // Create a completion task using cudaStreamAddCallback
        auto context = std::make_shared<std::pair<cudaEvent_t, Callback>>(event, std::move(callback));
        
        cudaStreamCallback_t streamCallback = [](cudaStream_t, cudaError_t status, void* ptr) {
            auto* ctx = static_cast<std::shared_ptr<std::pair<cudaEvent_t, Callback>>*>(ptr);
            auto context = *ctx;
            delete ctx;
            
            bool success = (status == cudaSuccess);
            if (success) {
                // Also check the event
                cudaError_t err = cudaEventSynchronize(context->first);
                success = (err == cudaSuccess);
            }
            
            context->second(success);
            cudaEventDestroy(context->first);
            context.reset();
        };

        err = cudaStreamAddCallback(stream, streamCallback, &context, 0);
        if (!GPU_CHECK(err)) {
            cudaEventDestroy(event);
            delete &context;
            callback(false);
            return false;
        }

        return true;
    }
};

} // namespace anari_usd_middleware

#endif // ENABLE_CUDA_ACCELERATION
