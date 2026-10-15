#pragma once

#include <vector>
#include <glm/glm.hpp>
#include "GpuKernels.h"

namespace anari_usd_middleware {

#ifdef ENABLE_CUDA_ACCELERATION

class GpuValidation {
public:
    static bool validateTransformVertices(
        const std::vector<glm::vec3>& gpuResults,
        const std::vector<glm::vec3>& inputVertices,
        const glm::mat4& transform,
        float tolerance = GpuKernelConfig::VALIDATION_TOLERANCE);

    static bool validateTransformNormals(
        const std::vector<glm::vec3>& gpuResults,
        const std::vector<glm::vec3>& inputNormals,
        const glm::mat3& normalMatrix,
        float tolerance = GpuKernelConfig::VALIDATION_TOLERANCE);

    static bool validateProcessUVs(
        const std::vector<glm::vec2>& gpuResults,
        const std::vector<glm::vec2>& inputUVs,
        float tolerance = GpuKernelConfig::VALIDATION_TOLERANCE);

    static bool validateMeshData(
        const std::vector<glm::vec3>& gpuVertices,
        const std::vector<glm::vec3>& gpuNormals,
        const std::vector<glm::vec2>& gpuUVs,
        const std::vector<glm::vec3>& inputVertices,
        const std::vector<glm::vec3>& inputNormals,
        const std::vector<glm::vec2>& inputUVs,
        const glm::mat4& transform,
        const glm::mat3& normalMatrix);

    struct ValidationStats {
        size_t totalComparisons = 0;
        size_t passedComparisons = 0;
        size_t failedComparisons = 0;
        float maxError = 0.0f;
        float averageError = 0.0f;
    };

    static const ValidationStats& getLastValidationStats();

private:
    static glm::vec3 transformVertexCpu(const glm::vec3& vertex, const glm::mat4& transform);

    static glm::vec3 transformNormalCpu(const glm::vec3& normal, const glm::mat3& normalMatrix);

    static glm::vec2 processUvCpu(const glm::vec2& uv);

    template<typename T>
    static bool vectorsEqual(const T& a, const T& b, float tolerance);

    // Thread-local validation stats (not exported)

    static thread_local ValidationStats lastValidationStats;
};

#else // ENABLE_CUDA_ACCELERATION

class GpuValidation {
public:
    static bool validateTransformVertices(
        const std::vector<glm::vec3>&,
        const std::vector<glm::vec3>&,
        const glm::mat4&,
        float = 1e-5f) { return false; }

    static bool validateTransformNormals(
        const std::vector<glm::vec3>&,
        const std::vector<glm::vec3>&,
        const glm::mat3&,
        float = 1e-5f) { return false; }

    static bool validateProcessUVs(
        const std::vector<glm::vec2>&,
        const std::vector<glm::vec2>&,
        float = 1e-5f) { return false; }

    static bool validateMeshData(
        const std::vector<glm::vec3>&,
        const std::vector<glm::vec3>&,
        const std::vector<glm::vec2>&,
        const std::vector<glm::vec3>&,
        const std::vector<glm::vec3>&,
        const std::vector<glm::vec2>&,
        const glm::mat4&,
        const glm::mat3&) { return false; }

    struct ValidationStats {
        size_t totalComparisons = 0;
        size_t passedComparisons = 0;
        size_t failedComparisons = 0;
        float maxError = 0.0f;
        float averageError = 0.0f;
    };

    static const ValidationStats& getLastValidationStats() {
        static ValidationStats emptyStats;
        return emptyStats;
    }
};

#endif // ENABLE_CUDA_ACCELERATION

} // namespace anari_usd_middleware
