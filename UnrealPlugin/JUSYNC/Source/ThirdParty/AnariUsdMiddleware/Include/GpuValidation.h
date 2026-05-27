#pragma once

#include <vector>
#include <glm/glm.hpp>
#include "GpuKernels.h"

/**
 * GPU Result Validation
 * 
 * Provides validation functions to verify GPU results match CPU results
 * within acceptable tolerances.
 */
namespace anari_usd_middleware {

#ifdef ENABLE_CUDA_ACCELERATION

/**
 * GPU validation utilities
 * 
 * Validates GPU-processed results against CPU reference implementations
 * to ensure correctness before accepting GPU output.
 */
class GpuValidation {
public:
    /**
     * Validate transformed vertices
     * 
     * @param gpuResults GPU-transformed vertices
     * @param inputVertices Original input vertices
     * @param transform Transformation matrix used
     * @param tolerance Acceptable difference tolerance (default: 1e-5f)
     * @return true if GPU results are valid
     */
    static bool validateTransformVertices(
        const std::vector<glm::vec3>& gpuResults,
        const std::vector<glm::vec3>& inputVertices,
        const glm::mat4& transform,
        float tolerance = GpuKernelConfig::VALIDATION_TOLERANCE);

    /**
     * Validate transformed normals
     * 
     * @param gpuResults GPU-transformed normals
     * @param inputNormals Original input normals
     * @param normalMatrix Normal transformation matrix used
     * @param tolerance Acceptable difference tolerance
     * @return true if GPU results are valid
     */
    static bool validateTransformNormals(
        const std::vector<glm::vec3>& gpuResults,
        const std::vector<glm::vec3>& inputNormals,
        const glm::mat3& normalMatrix,
        float tolerance = GpuKernelConfig::VALIDATION_TOLERANCE);

    /**
     * Validate processed UVs
     * 
     * @param gpuResults GPU-processed UVs
     * @param inputUVs Original input UVs
     * @param tolerance Acceptable difference tolerance
     * @return true if GPU results are valid
     */
    static bool validateProcessUVs(
        const std::vector<glm::vec2>& gpuResults,
        const std::vector<glm::vec2>& inputUVs,
        float tolerance = GpuKernelConfig::VALIDATION_TOLERANCE);

    /**
     * Validate all mesh data (vertices, normals, UVs)
     * 
     * @param gpuVertices GPU-transformed vertices
     * @param gpuNormals GPU-transformed normals
     * @param gpuUVs GPU-processed UVs
     * @param inputVertices Original vertices
     * @param inputNormals Original normals
     * @param inputUVs Original UVs
     * @param transform Vertex transformation matrix
     * @param normalMatrix Normal transformation matrix
     * @return true if all GPU results are valid
     */
    static bool validateMeshData(
        const std::vector<glm::vec3>& gpuVertices,
        const std::vector<glm::vec3>& gpuNormals,
        const std::vector<glm::vec2>& gpuUVs,
        const std::vector<glm::vec3>& inputVertices,
        const std::vector<glm::vec3>& inputNormals,
        const std::vector<glm::vec2>& inputUVs,
        const glm::mat4& transform,
        const glm::mat3& normalMatrix);

    /**
     * Get validation statistics
     */
    struct ValidationStats {
        size_t totalComparisons = 0;
        size_t passedComparisons = 0;
        size_t failedComparisons = 0;
        float maxError = 0.0f;
        float averageError = 0.0f;
    };

    /**
     * Get last validation statistics
     */
    static const ValidationStats& getLastValidationStats();

private:
    /**
     * CPU reference implementation for vertex transformation
     */
    static glm::vec3 transformVertexCpu(const glm::vec3& vertex, const glm::mat4& transform);

    /**
     * CPU reference implementation for normal transformation
     */
    static glm::vec3 transformNormalCpu(const glm::vec3& normal, const glm::mat3& normalMatrix);

    /**
     * CPU reference implementation for UV processing
     */
    static glm::vec2 processUvCpu(const glm::vec2& uv);

    /**
     * Compare two vectors with tolerance
     */
    template<typename T>
    static bool vectorsEqual(const T& a, const T& b, float tolerance);

    // Thread-local validation stats (not exported)
    static thread_local ValidationStats lastValidationStats;
};

#else // ENABLE_CUDA_ACCELERATION

// Stub class when CUDA is not available
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
