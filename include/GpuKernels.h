#pragma once

#include <vector>
#include <glm/glm.hpp>
#include <functional>
#include <memory>
#include "GpuContext.h"

#ifdef ENABLE_CUDA_ACCELERATION
#include "GpuMemory.h"
#endif

namespace anari_usd_middleware {

struct GpuKernelConfig {
    static constexpr size_t MIN_VERTICES_FOR_GPU = 10000;

    static constexpr size_t MIN_UVS_FOR_GPU = 10000;

    static constexpr size_t MIN_NORMALS_FOR_GPU = 10000;

    static constexpr int BLOCK_SIZE = 256;

    static constexpr float VALIDATION_TOLERANCE = 1e-5f;
};

#ifdef ENABLE_CUDA_ACCELERATION

using GpuAsyncCallback = std::function<void(bool success, const std::string& errorMessage)>;

class GpuAsyncHandle {
public:
    GpuAsyncHandle() : valid(false), stream(nullptr) {}

    bool isValid() const { return valid; }

    bool waitForCompletion(int timeoutMs = -1);

    bool cancel();

private:
    friend class GpuKernels;
    bool valid;
    cudaStream_t stream;
    std::vector<void*> gpuBuffers;  // Track allocated buffers for cleanup
};

class ANARI_USD_MIDDLEWARE_API GpuKernels {
public:

    static bool transformVertices(
        const std::vector<glm::vec3>& inputVertices,
        const glm::mat4& transform,
        std::vector<glm::vec3>& outputVertices);

    static bool transformNormals(
        const std::vector<glm::vec3>& inputNormals,
        const glm::mat3& normalMatrix,
        std::vector<glm::vec3>& outputNormals);

    static bool processUVs(
        const std::vector<glm::vec2>& inputUVs,
        std::vector<glm::vec2>& outputUVs);


    static std::shared_ptr<GpuAsyncHandle> transformVerticesAsync(
        const std::vector<glm::vec3>& inputVertices,
        const glm::mat4& transform,
        std::vector<glm::vec3>& outputVertices,
        GpuAsyncCallback callback);

    static std::shared_ptr<GpuAsyncHandle> transformNormalsAsync(
        const std::vector<glm::vec3>& inputNormals,
        const glm::mat3& normalMatrix,
        std::vector<glm::vec3>& outputNormals,
        GpuAsyncCallback callback);

    static std::shared_ptr<GpuAsyncHandle> processUVsAsync(
        const std::vector<glm::vec2>& inputUVs,
        std::vector<glm::vec2>& outputUVs,
        GpuAsyncCallback callback);

    static bool shouldUseGpuForVertices(size_t vertexCount) {
        return GpuContext::isAvailable() &&
               vertexCount >= GpuKernelConfig::MIN_VERTICES_FOR_GPU;
    }

    static bool shouldUseGpuForUVs(size_t uvCount) {
        return GpuContext::isAvailable() &&
               uvCount >= GpuKernelConfig::MIN_UVS_FOR_GPU;
    }

    static bool shouldUseGpuForNormals(size_t normalCount) {
        return GpuContext::isAvailable() &&
               normalCount >= GpuKernelConfig::MIN_NORMALS_FOR_GPU;
    }

private:
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

using GpuAsyncCallback = std::function<void(bool success, const std::string& errorMessage)>;

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
