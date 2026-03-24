#include "GpuValidation.h"
#include <cmath>
#include <algorithm>
#include <numeric>

namespace anari_usd_middleware {

#ifdef ENABLE_CUDA_ACCELERATION

// Thread-local validation stats
thread_local GpuValidation::ValidationStats GpuValidation::lastValidationStats;

bool GpuValidation::validateTransformVertices(
    const std::vector<glm::vec3>& gpuResults,
    const std::vector<glm::vec3>& inputVertices,
    const glm::mat4& transform,
    float tolerance)
{
    lastValidationStats = ValidationStats();
    
    if (gpuResults.empty() || inputVertices.empty()) {
        return true;  // Empty data is valid
    }

    if (gpuResults.size() != inputVertices.size()) {
        MIDDLEWARE_LOG_ERROR("Vertex count mismatch: GPU=%zu, CPU=%zu",
                           gpuResults.size(), inputVertices.size());
        return false;
    }

    size_t count = gpuResults.size();
    lastValidationStats.totalComparisons = count;
    float totalError = 0.0f;
    size_t failedCount = 0;

    // Sample-based validation for large datasets (check every 100th vertex)
    size_t sampleStep = (count > 10000) ? 100 : 1;

    for (size_t i = 0; i < count; i += sampleStep) {
        lastValidationStats.totalComparisons++;
        
        // Calculate CPU reference
        glm::vec3 cpuResult = transformVertexCpu(inputVertices[i], transform);
        glm::vec3 gpuResult = gpuResults[i];

        // Calculate error
        float error = glm::distance(cpuResult, gpuResult);
        totalError += error;
        lastValidationStats.maxError = std::max(lastValidationStats.maxError, error);

        if (error > tolerance) {
            failedCount++;
            if (failedCount <= 5) {  // Log first 5 failures
                MIDDLEWARE_LOG_WARNING("Vertex validation failed at index %zu: CPU=(%f,%f,%f), GPU=(%f,%f,%f), error=%f",
                                     i,
                                     cpuResult.x, cpuResult.y, cpuResult.z,
                                     gpuResult.x, gpuResult.y, gpuResult.z,
                                     error);
            }
        }
    }

    lastValidationStats.failedComparisons = failedCount;
    lastValidationStats.passedComparisons = lastValidationStats.totalComparisons - failedCount;
    lastValidationStats.averageError = static_cast<float>(totalError) / static_cast<float>(lastValidationStats.totalComparisons);

    // Fail if more than 1% of samples failed
    bool passed = (failedCount * 100 < lastValidationStats.totalComparisons);
    
    if (!passed) {
        MIDDLEWARE_LOG_ERROR("Vertex validation failed: %zu/%zu samples exceeded tolerance (%f)",
                           failedCount, lastValidationStats.totalComparisons, tolerance);
    } else {
        MIDDLEWARE_LOG_DEBUG("Vertex validation passed: %zu/%zu samples within tolerance (max error: %f, avg: %f)",
                           lastValidationStats.passedComparisons, lastValidationStats.totalComparisons,
                           lastValidationStats.maxError, lastValidationStats.averageError);
    }

    return passed;
}

bool GpuValidation::validateTransformNormals(
    const std::vector<glm::vec3>& gpuResults,
    const std::vector<glm::vec3>& inputNormals,
    const glm::mat3& normalMatrix,
    float tolerance)
{
    lastValidationStats = ValidationStats();
    
    if (gpuResults.empty() || inputNormals.empty()) {
        return true;
    }

    if (gpuResults.size() != inputNormals.size()) {
        MIDDLEWARE_LOG_ERROR("Normal count mismatch: GPU=%zu, CPU=%zu",
                           gpuResults.size(), inputNormals.size());
        return false;
    }

    size_t count = gpuResults.size();
    lastValidationStats.totalComparisons = count;
    float totalError = 0.0f;
    size_t failedCount = 0;

    // Sample-based validation
    size_t sampleStep = (count > 10000) ? 100 : 1;

    for (size_t i = 0; i < count; i += sampleStep) {
        lastValidationStats.totalComparisons++;
        
        glm::vec3 cpuResult = transformNormalCpu(inputNormals[i], normalMatrix);
        glm::vec3 gpuResult = gpuResults[i];

        // For normals, use angular distance
        float dot = glm::dot(cpuResult, gpuResult);
        dot = std::clamp(dot, -1.0f, 1.0f);
        float angleError = std::acos(dot);  // Radians
        float error = angleError * 57.2958f;  // Convert to degrees

        totalError += error;
        lastValidationStats.maxError = std::max(lastValidationStats.maxError, error);

        if (error > tolerance * 100.0f) {  // Convert tolerance to degrees
            failedCount++;
        }
    }

    lastValidationStats.failedComparisons = failedCount;
    lastValidationStats.passedComparisons = lastValidationStats.totalComparisons - failedCount;
    lastValidationStats.averageError = totalError / static_cast<float>(lastValidationStats.totalComparisons);

    bool passed = (failedCount * 100 < lastValidationStats.totalComparisons);
    
    if (!passed) {
        MIDDLEWARE_LOG_ERROR("Normal validation failed: %zu/%zu samples exceeded tolerance",
                           failedCount, lastValidationStats.totalComparisons);
    }

    return passed;
}

bool GpuValidation::validateProcessUVs(
    const std::vector<glm::vec2>& gpuResults,
    const std::vector<glm::vec2>& inputUVs,
    float tolerance)
{
    lastValidationStats = ValidationStats();
    
    if (gpuResults.empty() || inputUVs.empty()) {
        return true;
    }

    if (gpuResults.size() != inputUVs.size()) {
        MIDDLEWARE_LOG_ERROR("UV count mismatch: GPU=%zu, CPU=%zu",
                           gpuResults.size(), inputUVs.size());
        return false;
    }

    size_t count = gpuResults.size();
    lastValidationStats.totalComparisons = count;
    float totalError = 0.0f;
    size_t failedCount = 0;

    // Sample-based validation
    size_t sampleStep = (count > 10000) ? 100 : 1;

    for (size_t i = 0; i < count; i += sampleStep) {
        lastValidationStats.totalComparisons++;
        
        glm::vec2 cpuResult = processUvCpu(inputUVs[i]);
        glm::vec2 gpuResult = gpuResults[i];

        float error = glm::distance(cpuResult, gpuResult);
        totalError += error;
        lastValidationStats.maxError = std::max(lastValidationStats.maxError, error);

        if (error > tolerance) {
            failedCount++;
        }
    }

    lastValidationStats.failedComparisons = failedCount;
    lastValidationStats.passedComparisons = lastValidationStats.totalComparisons - failedCount;
    lastValidationStats.averageError = totalError / static_cast<float>(lastValidationStats.totalComparisons);

    bool passed = (failedCount * 100 < lastValidationStats.totalComparisons);
    
    if (!passed) {
        MIDDLEWARE_LOG_ERROR("UV validation failed: %zu/%zu samples exceeded tolerance",
                           failedCount, lastValidationStats.totalComparisons);
    }

    return passed;
}

bool GpuValidation::validateMeshData(
    const std::vector<glm::vec3>& gpuVertices,
    const std::vector<glm::vec3>& gpuNormals,
    const std::vector<glm::vec2>& gpuUVs,
    const std::vector<glm::vec3>& inputVertices,
    const std::vector<glm::vec3>& inputNormals,
    const std::vector<glm::vec2>& inputUVs,
    const glm::mat4& transform,
    const glm::mat3& normalMatrix)
{
    bool verticesValid = true;
    bool normalsValid = true;
    bool uvsValid = true;

    // Validate vertices if data exists
    if (!inputVertices.empty() && !gpuVertices.empty()) {
        verticesValid = validateTransformVertices(gpuVertices, inputVertices, transform);
    }

    // Validate normals if data exists
    if (!inputNormals.empty() && !gpuNormals.empty()) {
        normalsValid = validateTransformNormals(gpuNormals, inputNormals, normalMatrix);
    }

    // Validate UVs if data exists
    if (!inputUVs.empty() && !gpuUVs.empty()) {
        uvsValid = validateProcessUVs(gpuUVs, inputUVs);
    }

    return verticesValid && normalsValid && uvsValid;
}

const GpuValidation::ValidationStats& GpuValidation::getLastValidationStats() {
    return lastValidationStats;
}

// ============================================================================
// CPU Reference Implementations
// ============================================================================

glm::vec3 GpuValidation::transformVertexCpu(const glm::vec3& vertex, const glm::mat4& transform) {
    // Fast NaN/Inf check
    const int* ix = reinterpret_cast<const int*>(&vertex.x);
    const int* iy = reinterpret_cast<const int*>(&vertex.y);
    const int* iz = reinterpret_cast<const int*>(&vertex.z);

    if ((*ix & 0x7F800000) == 0x7F800000 ||
        (*iy & 0x7F800000) == 0x7F800000 ||
        (*iz & 0x7F800000) == 0x7F800000) {
        return glm::vec3(0.0f, 0.0f, 0.0f);
    }

    // Matrix multiplication (matches CUDA kernel)
    float x = transform[0][0] * vertex.x + transform[1][0] * vertex.y + 
              transform[2][0] * vertex.z + transform[3][0];
    float y = transform[0][1] * vertex.x + transform[1][1] * vertex.y + 
              transform[2][1] * vertex.z + transform[3][1];
    float z = transform[0][2] * vertex.x + transform[1][2] * vertex.y + 
              transform[2][2] * vertex.z + transform[3][2];

    // Validate output
    const int* itx = reinterpret_cast<const int*>(&x);
    const int* ity = reinterpret_cast<const int*>(&y);
    const int* itz = reinterpret_cast<const int*>(&z);

    if ((*itx & 0x7F800000) == 0x7F800000 ||
        (*ity & 0x7F800000) == 0x7F800000 ||
        (*itz & 0x7F800000) == 0x7F800000) {
        return vertex;  // Return original on invalid output
    }

    return glm::vec3(x, y, z);
}

glm::vec3 GpuValidation::transformNormalCpu(const glm::vec3& normal, const glm::mat3& normalMatrix) {
    // Fast NaN/Inf check
    const int* ix = reinterpret_cast<const int*>(&normal.x);
    const int* iy = reinterpret_cast<const int*>(&normal.y);
    const int* iz = reinterpret_cast<const int*>(&normal.z);

    if ((*ix & 0x7F800000) == 0x7F800000 ||
        (*iy & 0x7F800000) == 0x7F800000 ||
        (*iz & 0x7F800000) == 0x7F800000) {
        return glm::vec3(0.0f, 1.0f, 0.0f);  // Default up
    }

    // Matrix multiplication
    glm::vec3 transformed = normalMatrix * normal;

    // Normalize
    float lengthSquared = glm::dot(transformed, transformed);
    if (lengthSquared < 1e-10f) {
        return glm::vec3(0.0f, 1.0f, 0.0f);  // Default up
    }

    return transformed / std::sqrt(lengthSquared);
}

glm::vec2 GpuValidation::processUvCpu(const glm::vec2& uv) {
    // Fast NaN/Inf check
    const int* ix = reinterpret_cast<const int*>(&uv.x);
    const int* iy = reinterpret_cast<const int*>(&uv.y);

    if ((*ix & 0x7F800000) == 0x7F800000 || (*iy & 0x7F800000) == 0x7F800000) {
        return glm::vec2(0.0f, 0.0f);
    }

    // Clamp to reasonable range
    return glm::vec2(
        std::clamp(uv.x, -10.0f, 10.0f),
        std::clamp(uv.y, -10.0f, 10.0f)
    );
}

template<typename T>
bool GpuValidation::vectorsEqual(const T& a, const T& b, float tolerance) {
    return glm::distance(a, b) <= tolerance;
}

#endif // ENABLE_CUDA_ACCELERATION

} // namespace anari_usd_middleware
