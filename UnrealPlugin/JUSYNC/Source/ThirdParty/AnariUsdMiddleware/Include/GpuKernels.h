#pragma once

#include <vector>
#include <glm/glm.hpp>
#include <functional>
#include <memory>
#include "GpuContext.h"

#ifdef ENABLE_CUDA_ACCELERATION
#include "GpuMemory.h"
#endif

/**
 * GPU Kernel Declarations for USD Processing
 * 
 * Provides CUDA kernel interfaces for vertex transformation,
 * normal transformation, and UV coordinate processing.
 * 
 * Supports both synchronous and asynchronous execution modes.
 */
namespace anari_usd_middleware {

/**
 * GPU kernel configuration
 */
struct GpuKernelConfig {
    // Minimum vertex count to use GPU (threshold)
    static constexpr size_t MIN_VERTICES_FOR_GPU = 10000;
    
    // Minimum UV count to use GPU
    static constexpr size_t MIN_UVS_FOR_GPU = 10000;
    
    // Minimum normal count to use GPU
    static constexpr size_t MIN_NORMALS_FOR_GPU = 10000;
    
    // Block size for kernels
    static constexpr int BLOCK_SIZE = 256;
    
    // Tolerance for floating-point comparison in validation
    static constexpr float VALIDATION_TOLERANCE = 1e-5f;
};

#ifdef ENABLE_CUDA_ACCELERATION

/**
 * Callback type for async GPU operations
 * @param success true if operation completed successfully
 * @param errorMessage error message if failed (empty if success)
 */
using GpuAsyncCallback = std::function<void(bool success, const std::string& errorMessage)>;

/**
 * Handle for async GPU operations
 */
class GpuAsyncHandle {
public:
    GpuAsyncHandle() : valid(false), stream(nullptr) {}
    
    bool isValid() const { return valid; }
    
    // Wait for async operation to complete
    bool waitForCompletion(int timeoutMs = -1);
    
    // Cancel async operation
    bool cancel();
    
private:
    friend class GpuKernels;
    bool valid;
    cudaStream_t stream;
    std::vector<void*> gpuBuffers;  // Track allocated buffers for cleanup
};

/**
 * GPU Kernels for mesh processing
 * 
 * Provides GPU-accelerated operations for:
 * - Vertex transformation (4x4 matrix multiplication)
 * - Normal transformation (3x3 matrix multiplication)
 * - UV coordinate processing
 * 
 * Supports both synchronous and asynchronous execution modes.
 */
class ANARI_USD_MIDDLEWARE_API GpuKernels {
public:
    // ========================================================================
    // SYNCHRONOUS API (blocks until complete)
    // ========================================================================
    
    /**
     * Transform vertices using GPU (synchronous)
     * 
     * @param inputVertices Input vertex positions (vec3)
     * @param transform 4x4 transformation matrix
     * @param outputVertices Output transformed vertices
     * @return true if GPU processing successful, false for fallback
     */
    static bool transformVertices(
        const std::vector<glm::vec3>& inputVertices,
        const glm::mat4& transform,
        std::vector<glm::vec3>& outputVertices);

    /**
     * Transform normals using GPU (synchronous)
     * 
     * @param inputNormals Input normal vectors (vec3)
     * @param normalMatrix 3x3 normal transformation matrix
     * @param outputNormals Output transformed and normalized normals
     * @return true if GPU processing successful, false for fallback
     */
    static bool transformNormals(
        const std::vector<glm::vec3>& inputNormals,
        const glm::mat3& normalMatrix,
        std::vector<glm::vec3>& outputNormals);

    /**
     * Process UV coordinates using GPU (synchronous)
     * 
     * @param inputUVs Input UV coordinates (vec2)
     * @param outputUVs Output processed UVs (normalized and validated)
     * @return true if GPU processing successful, false for fallback
     */
    static bool processUVs(
        const std::vector<glm::vec2>& inputUVs,
        std::vector<glm::vec2>& outputUVs);

    // ========================================================================
    // ASYNCHRONOUS API (returns immediately, callback on completion)
    // ========================================================================
    
    /**
     * Transform vertices using GPU (asynchronous)
     * 
     * @param inputVertices Input vertex positions (vec3)
     * @param transform 4x4 transformation matrix
     * @param outputVertices Output transformed vertices (filled on completion)
     * @param callback Callback invoked when operation completes
     * @return GpuAsyncHandle for waiting/canceling, nullptr if failed to start
     */
    static std::shared_ptr<GpuAsyncHandle> transformVerticesAsync(
        const std::vector<glm::vec3>& inputVertices,
        const glm::mat4& transform,
        std::vector<glm::vec3>& outputVertices,
        GpuAsyncCallback callback);

    /**
     * Transform normals using GPU (asynchronous)
     * 
     * @param inputNormals Input normal vectors (vec3)
     * @param normalMatrix 3x3 normal transformation matrix
     * @param outputNormals Output transformed and normalized normals (filled on completion)
     * @param callback Callback invoked when operation completes
     * @return GpuAsyncHandle for waiting/canceling, nullptr if failed to start
     */
    static std::shared_ptr<GpuAsyncHandle> transformNormalsAsync(
        const std::vector<glm::vec3>& inputNormals,
        const glm::mat3& normalMatrix,
        std::vector<glm::vec3>& outputNormals,
        GpuAsyncCallback callback);

    /**
     * Process UV coordinates using GPU (asynchronous)
     * 
     * @param inputUVs Input UV coordinates (vec2)
     * @param outputUVs Output processed UVs (normalized and validated, filled on completion)
     * @param callback Callback invoked when operation completes
     * @return GpuAsyncHandle for waiting/canceling, nullptr if failed to start
     */
    static std::shared_ptr<GpuAsyncHandle> processUVsAsync(
        const std::vector<glm::vec2>& inputUVs,
        std::vector<glm::vec2>& outputUVs,
        GpuAsyncCallback callback);

    /**
     * Check if GPU should be used for given vertex count
     */
    static bool shouldUseGpuForVertices(size_t vertexCount) {
        return GpuContext::isAvailable() && 
               vertexCount >= GpuKernelConfig::MIN_VERTICES_FOR_GPU;
    }

    /**
     * Check if GPU should be used for given UV count
     */
    static bool shouldUseGpuForUVs(size_t uvCount) {
        return GpuContext::isAvailable() && 
               uvCount >= GpuKernelConfig::MIN_UVS_FOR_GPU;
    }

    /**
     * Check if GPU should be used for given normal count
     */
    static bool shouldUseGpuForNormals(size_t normalCount) {
        return GpuContext::isAvailable() && 
               normalCount >= GpuKernelConfig::MIN_NORMALS_FOR_GPU;
    }

private:
    // Internal implementations
    static bool transformVerticesInternal(
        const glm::vec3* inputVertices,
        const float* transformMatrix,
        glm::vec3* outputVertices,
        size_t vertexCount,
        cudaStream_t stream);

    static bool transformNormalsInternal(
        const glm::vec3* inputNormals,
        const float* normalMatrix,
        glm::vec3* outputNormals,
        size_t normalCount,
        cudaStream_t stream);

    static bool processUVsInternal(
        const glm::vec2* inputUVs,
        glm::vec2* outputUVs,
        size_t uvCount,
        cudaStream_t stream);
};

#else // ENABLE_CUDA_ACCELERATION

// Callback type for async GPU operations (stub)
using GpuAsyncCallback = std::function<void(bool success, const std::string& errorMessage)>;

// Stub class when CUDA is not available
class GpuAsyncHandle {
public:
    GpuAsyncHandle() : valid(false) {}
    bool isValid() const { return valid; }
    bool waitForCompletion(int) { return false; }
    bool cancel() { return false; }
private:
    bool valid;
};

class GpuKernels {
public:
    static bool transformVertices(
        const std::vector<glm::vec3>&,
        const glm::mat4&,
        std::vector<glm::vec3>&) { return false; }

    static bool transformNormals(
        const std::vector<glm::vec3>&,
        const glm::mat3&,
        std::vector<glm::vec3>&) { return false; }

    static bool processUVs(
        const std::vector<glm::vec2>&,
        std::vector<glm::vec2>&) { return false; }

    // Async stubs (not available without CUDA)
    static std::shared_ptr<GpuAsyncHandle> transformVerticesAsync(
        const std::vector<glm::vec3>&,
        const glm::mat4&,
        std::vector<glm::vec3>&,
        GpuAsyncCallback) { return nullptr; }

    static std::shared_ptr<GpuAsyncHandle> transformNormalsAsync(
        const std::vector<glm::vec3>&,
        const glm::mat3&,
        std::vector<glm::vec3>&,
        GpuAsyncCallback) { return nullptr; }

    static std::shared_ptr<GpuAsyncHandle> processUVsAsync(
        const std::vector<glm::vec2>&,
        std::vector<glm::vec2>&,
        GpuAsyncCallback) { return nullptr; }

    static bool shouldUseGpuForVertices(size_t) { return false; }
    static bool shouldUseGpuForUVs(size_t) { return false; }
    static bool shouldUseGpuForNormals(size_t) { return false; }
};

#endif // ENABLE_CUDA_ACCELERATION

} // namespace anari_usd_middleware
