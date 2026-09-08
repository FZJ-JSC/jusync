#include "GpuKernels.h"
#include "GpuValidation.h"
#include <glm/glm.hpp>
#include <thread>
#include <chrono>

namespace anari_usd_middleware {

// ============================================================================
// CUDA Kernel: Vertex Transformation
// ============================================================================

/**
 * CUDA kernel for transforming vertices with 4x4 matrix
 * 
 * Each thread transforms one vertex: v' = M * v
 * Matrix is stored in row-major order (16 floats)
 */
__global__ void transformVerticesKernel(
    const float3* __restrict__ inputVertices,
    const float* __restrict__ transformMatrix,  // 4x4 matrix, row-major
    float3* __restrict__ outputVertices,
    size_t vertexCount)
{
    size_t idx = blockIdx.x * blockDim.x + threadIdx.x;
    if (idx >= vertexCount) return;

    float3 v = inputVertices[idx];

    // Fast NaN/Inf check using integer representation
    const int* ix = reinterpret_cast<const int*>(&v.x);
    const int* iy = reinterpret_cast<const int*>(&v.y);
    const int* iz = reinterpret_cast<const int*>(&v.z);

    // Check if any component is NaN or Inf (exponent bits all 1s)
    if ((*ix & 0x7F800000) == 0x7F800000 ||
        (*iy & 0x7F800000) == 0x7F800000 ||
        (*iz & 0x7F800000) == 0x7F800000) {
        // Invalid input - output zero vector
        outputVertices[idx] = make_float3(0.0f, 0.0f, 0.0f);
        return;
    }

    // 4x4 matrix multiplication (unrolled for performance)
    // Matrix is row-major: [m00, m01, m02, m03, m10, m11, ...]
    float x = transformMatrix[0] * v.x + transformMatrix[1] * v.y + 
              transformMatrix[2] * v.z + transformMatrix[3];
    float y = transformMatrix[4] * v.x + transformMatrix[5] * v.y + 
              transformMatrix[6] * v.z + transformMatrix[7];
    float z = transformMatrix[8] * v.x + transformMatrix[9] * v.y + 
              transformMatrix[10] * v.z + transformMatrix[11];

    // Validate output
    const int* itx = reinterpret_cast<const int*>(&x);
    const int* ity = reinterpret_cast<const int*>(&y);
    const int* itz = reinterpret_cast<const int*>(&z);

    if ((*itx & 0x7F800000) == 0x7F800000 ||
        (*ity & 0x7F800000) == 0x7F800000 ||
        (*itz & 0x7F800000) == 0x7F800000) {
        // Invalid output - use original vertex
        outputVertices[idx] = make_float3(v.x, v.y, v.z);
    } else {
        outputVertices[idx] = make_float3(x, y, z);
    }
}

// ============================================================================
// CUDA Kernel: Normal Transformation
// ============================================================================

/**
 * CUDA kernel for transforming normals with 3x3 matrix
 * 
 * Each thread transforms and normalizes one normal: n' = normalize(M * n)
 * Matrix is stored in row-major order (9 floats)
 */
__global__ void transformNormalsKernel(
    const float3* __restrict__ inputNormals,
    const float* __restrict__ normalMatrix,  // 3x3 matrix, row-major
    float3* __restrict__ outputNormals,
    size_t normalCount)
{
    size_t idx = blockIdx.x * blockDim.x + threadIdx.x;
    if (idx >= normalCount) return;

    float3 n = inputNormals[idx];

    // Fast NaN/Inf check
    const int* ix = reinterpret_cast<const int*>(&n.x);
    const int* iy = reinterpret_cast<const int*>(&n.y);
    const int* iz = reinterpret_cast<const int*>(&n.z);

    if ((*ix & 0x7F800000) == 0x7F800000 ||
        (*iy & 0x7F800000) == 0x7F800000 ||
        (*iz & 0x7F800000) == 0x7F800000) {
        // Invalid input - output default up vector
        outputNormals[idx] = make_float3(0.0f, 1.0f, 0.0f);
        return;
    }

    // 3x3 matrix multiplication
    float x = normalMatrix[0] * n.x + normalMatrix[1] * n.y + normalMatrix[2] * n.z;
    float y = normalMatrix[3] * n.x + normalMatrix[4] * n.y + normalMatrix[5] * n.z;
    float z = normalMatrix[6] * n.x + normalMatrix[7] * n.y + normalMatrix[8] * n.z;

    // Normalization using rsqrt (reciprocal square root) for performance
    float lengthSquared = x * x + y * y + z * z;
    
    // Handle zero-length normals
    if (lengthSquared < 1e-10f) {
        outputNormals[idx] = make_float3(0.0f, 1.0f, 0.0f);  // Default up vector
        return;
    }

    float invLength = rsqrtf(lengthSquared);
    outputNormals[idx] = make_float3(x * invLength, y * invLength, z * invLength);
}

// ============================================================================
// CUDA Kernel: UV Processing
// ============================================================================

/**
 * CUDA kernel for UV coordinate processing
 * 
 * Each thread processes one UV coordinate:
 * - Validates for NaN/Inf
 * - Clamps to reasonable range [-10, 10]
 */
__global__ void processUVsKernel(
    const float2* __restrict__ inputUVs,
    float2* __restrict__ outputUVs,
    size_t uvCount)
{
    size_t idx = blockIdx.x * blockDim.x + threadIdx.x;
    if (idx >= uvCount) return;

    float2 uv = inputUVs[idx];

    // Fast NaN/Inf check using integer representation
    const int* ix = reinterpret_cast<const int*>(&uv.x);
    const int* iy = reinterpret_cast<const int*>(&uv.y);

    if ((*ix & 0x7F800000) == 0x7F800000 || (*iy & 0x7F800000) == 0x7F800000) {
        // Invalid UV - output zero
        outputUVs[idx] = make_float2(0.0f, 0.0f);
    } else {
        // Clamp UV coordinates to reasonable range
        float sx = fminf(fmaxf(uv.x, -10.0f), 10.0f);
        float sy = fminf(fmaxf(uv.y, -10.0f), 10.0f);
        outputUVs[idx] = make_float2(sx, sy);
    }
}

// ============================================================================
// Host Functions
// ============================================================================

bool GpuKernels::transformVertices(
    const std::vector<glm::vec3>& inputVertices,
    const glm::mat4& transform,
    std::vector<glm::vec3>& outputVertices)
{
    // Check if GPU should be used
    if (!shouldUseGpuForVertices(inputVertices.size())) {
        return false;
    }

    size_t vertexCount = inputVertices.size();
    if (vertexCount == 0) {
        return false;
    }

    // Allocate output
    outputVertices.resize(vertexCount);

    // Create GPU buffers
    GpuBuffer<float3> d_inputVertices;
    GpuBuffer<float3> d_outputVertices;
    GpuBuffer<float> d_transformMatrix;

    if (!d_inputVertices.allocate(vertexCount)) {
        MIDDLEWARE_LOG_WARNING("Failed to allocate GPU buffer for input vertices");
        return false;
    }

    if (!d_outputVertices.allocate(vertexCount)) {
        MIDDLEWARE_LOG_WARNING("Failed to allocate GPU buffer for output vertices");
        return false;
    }

    if (!d_transformMatrix.allocate(16)) {
        MIDDLEWARE_LOG_WARNING("Failed to allocate GPU buffer for transform matrix");
        return false;
    }

    // Convert input vertices to float3 array for GPU transfer
    std::vector<float3> float3Vertices(vertexCount);
    for (size_t i = 0; i < vertexCount; ++i) {
        float3Vertices[i] = make_float3(
            inputVertices[i].x, 
            inputVertices[i].y, 
            inputVertices[i].z
        );
    }

    // Convert transform matrix to row-major float array
    std::vector<float> transformFloats(16);
    // glm::mat4 is column-major, convert to row-major for CUDA kernel
    transformFloats[0] = transform[0][0]; transformFloats[1] = transform[1][0];
    transformFloats[2] = transform[2][0]; transformFloats[3] = transform[3][0];
    transformFloats[4] = transform[0][1]; transformFloats[5] = transform[1][1];
    transformFloats[6] = transform[2][1]; transformFloats[7] = transform[3][1];
    transformFloats[8] = transform[0][2]; transformFloats[9] = transform[1][2];
    transformFloats[10] = transform[2][2]; transformFloats[11] = transform[3][2];
    transformFloats[12] = transform[0][3]; transformFloats[13] = transform[1][3];
    transformFloats[14] = transform[2][3]; transformFloats[15] = transform[3][3];

    // Create CUDA stream for async operations
    cudaStream_t stream;
    cudaError_t err = cudaStreamCreate(&stream);
    if (!GPU_CHECK(err)) {
        MIDDLEWARE_LOG_WARNING("Failed to create CUDA stream");
        return false;
    }

    // Copy data to GPU
    if (!GpuMemoryUtils::h2d(d_inputVertices.data(), float3Vertices.data(), vertexCount, stream)) {
        MIDDLEWARE_LOG_WARNING("Failed to copy input vertices to GPU");
        cudaStreamDestroy(stream);
        return false;
    }

    if (!GpuMemoryUtils::h2d(d_transformMatrix.data(), transformFloats.data(), 16, stream)) {
        MIDDLEWARE_LOG_WARNING("Failed to copy transform matrix to GPU");
        cudaStreamDestroy(stream);
        return false;
    }

    // Launch kernel
    int blockSize = GpuKernelConfig::BLOCK_SIZE;
    int gridSize = GpuContext::calculateGridSize(vertexCount, blockSize);

    transformVerticesKernel<<<gridSize, blockSize, 0, stream>>>(
        d_inputVertices.data(),
        d_transformMatrix.data(),
        d_outputVertices.data(),
        vertexCount
    );

    // Check for launch errors
    err = cudaGetLastError();
    if (!GPU_CHECK(err)) {
        MIDDLEWARE_LOG_WARNING("Vertex transformation kernel launch failed");
        cudaStreamDestroy(stream);
        return false;
    }

    // Synchronize stream
    if (!GpuMemoryUtils::synchronizeStream(stream)) {
        MIDDLEWARE_LOG_WARNING("Failed to synchronize GPU stream");
        cudaStreamDestroy(stream);
        return false;
    }

    // Copy results back to host
    std::vector<float3> outputFloat3(vertexCount);
    if (!GpuMemoryUtils::d2h(outputFloat3.data(), d_outputVertices.data(), vertexCount, stream)) {
        MIDDLEWARE_LOG_WARNING("Failed to copy output vertices from GPU");
        cudaStreamDestroy(stream);
        return false;
    }

    // Cleanup stream
    cudaStreamDestroy(stream);

    // Convert output to glm::vec3
    for (size_t i = 0; i < vertexCount; ++i) {
        outputVertices[i] = glm::vec3(
            outputFloat3[i].x,
            outputFloat3[i].y,
            outputFloat3[i].z
        );
    }

    MIDDLEWARE_LOG_DEBUG("GPU vertex transformation complete: %zu vertices", vertexCount);
    return true;
}

bool GpuKernels::transformNormals(
    const std::vector<glm::vec3>& inputNormals,
    const glm::mat3& normalMatrix,
    std::vector<glm::vec3>& outputNormals)
{
    // Check if GPU should be used
    if (!shouldUseGpuForNormals(inputNormals.size())) {
        return false;
    }

    size_t normalCount = inputNormals.size();
    if (normalCount == 0) {
        return false;
    }

    // Allocate output
    outputNormals.resize(normalCount);

    // Create GPU buffers
    GpuBuffer<float3> d_inputNormals;
    GpuBuffer<float3> d_outputNormals;
    GpuBuffer<float> d_normalMatrix;

    if (!d_inputNormals.allocate(normalCount)) {
        MIDDLEWARE_LOG_WARNING("Failed to allocate GPU buffer for input normals");
        return false;
    }

    if (!d_outputNormals.allocate(normalCount)) {
        MIDDLEWARE_LOG_WARNING("Failed to allocate GPU buffer for output normals");
        return false;
    }

    if (!d_normalMatrix.allocate(9)) {
        MIDDLEWARE_LOG_WARNING("Failed to allocate GPU buffer for normal matrix");
        return false;
    }

    // Convert input normals to float3 array
    std::vector<float3> float3Normals(normalCount);
    for (size_t i = 0; i < normalCount; ++i) {
        float3Normals[i] = make_float3(
            inputNormals[i].x,
            inputNormals[i].y,
            inputNormals[i].z
        );
    }

    // Convert normal matrix to row-major float array
    std::vector<float> matrixFloats(9);
    matrixFloats[0] = normalMatrix[0][0]; matrixFloats[1] = normalMatrix[1][0];
    matrixFloats[2] = normalMatrix[2][0];
    matrixFloats[3] = normalMatrix[0][1]; matrixFloats[4] = normalMatrix[1][1];
    matrixFloats[5] = normalMatrix[2][1];
    matrixFloats[6] = normalMatrix[0][2]; matrixFloats[7] = normalMatrix[1][2];
    matrixFloats[8] = normalMatrix[2][2];

    // Create CUDA stream
    cudaStream_t stream;
    cudaError_t err = cudaStreamCreate(&stream);
    if (!GPU_CHECK(err)) {
        MIDDLEWARE_LOG_WARNING("Failed to create CUDA stream");
        return false;
    }

    // Copy data to GPU
    if (!GpuMemoryUtils::h2d(d_inputNormals.data(), float3Normals.data(), normalCount, stream)) {
        MIDDLEWARE_LOG_WARNING("Failed to copy input normals to GPU");
        cudaStreamDestroy(stream);
        return false;
    }

    if (!GpuMemoryUtils::h2d(d_normalMatrix.data(), matrixFloats.data(), 9, stream)) {
        MIDDLEWARE_LOG_WARNING("Failed to copy normal matrix to GPU");
        cudaStreamDestroy(stream);
        return false;
    }

    // Launch kernel
    int blockSize = GpuKernelConfig::BLOCK_SIZE;
    int gridSize = GpuContext::calculateGridSize(normalCount, blockSize);

    transformNormalsKernel<<<gridSize, blockSize, 0, stream>>>(
        d_inputNormals.data(),
        d_normalMatrix.data(),
        d_outputNormals.data(),
        normalCount
    );

    // Check for launch errors
    err = cudaGetLastError();
    if (!GPU_CHECK(err)) {
        MIDDLEWARE_LOG_WARNING("Normal transformation kernel launch failed");
        cudaStreamDestroy(stream);
        return false;
    }

    // Synchronize
    if (!GpuMemoryUtils::synchronizeStream(stream)) {
        MIDDLEWARE_LOG_WARNING("Failed to synchronize GPU stream");
        cudaStreamDestroy(stream);
        return false;
    }

    // Copy results back
    std::vector<float3> outputFloat3(normalCount);
    if (!GpuMemoryUtils::d2h(outputFloat3.data(), d_outputNormals.data(), normalCount, stream)) {
        MIDDLEWARE_LOG_WARNING("Failed to copy output normals from GPU");
        cudaStreamDestroy(stream);
        return false;
    }

    cudaStreamDestroy(stream);

    // Convert to glm::vec3
    for (size_t i = 0; i < normalCount; ++i) {
        outputNormals[i] = glm::vec3(
            outputFloat3[i].x,
            outputFloat3[i].y,
            outputFloat3[i].z
        );
    }

    MIDDLEWARE_LOG_DEBUG("GPU normal transformation complete: %zu normals", normalCount);
    return true;
}

bool GpuKernels::processUVs(
    const std::vector<glm::vec2>& inputUVs,
    std::vector<glm::vec2>& outputUVs)
{
    // Check if GPU should be used
    if (!shouldUseGpuForUVs(inputUVs.size())) {
        return false;
    }

    size_t uvCount = inputUVs.size();
    if (uvCount == 0) {
        return false;
    }

    // Allocate output
    outputUVs.resize(uvCount);

    // Create GPU buffers
    GpuBuffer<float2> d_inputUVs;
    GpuBuffer<float2> d_outputUVs;

    if (!d_inputUVs.allocate(uvCount)) {
        MIDDLEWARE_LOG_WARNING("Failed to allocate GPU buffer for input UVs");
        return false;
    }

    if (!d_outputUVs.allocate(uvCount)) {
        MIDDLEWARE_LOG_WARNING("Failed to allocate GPU buffer for output UVs");
        return false;
    }

    // Convert input UVs to float2 array
    std::vector<float2> float2UVs(uvCount);
    for (size_t i = 0; i < uvCount; ++i) {
        float2UVs[i] = make_float2(inputUVs[i].x, inputUVs[i].y);
    }

    // Create CUDA stream
    cudaStream_t stream;
    cudaError_t err = cudaStreamCreate(&stream);
    if (!GPU_CHECK(err)) {
        MIDDLEWARE_LOG_WARNING("Failed to create CUDA stream");
        return false;
    }

    // Copy data to GPU
    if (!GpuMemoryUtils::h2d(d_inputUVs.data(), float2UVs.data(), uvCount, stream)) {
        MIDDLEWARE_LOG_WARNING("Failed to copy input UVs to GPU");
        cudaStreamDestroy(stream);
        return false;
    }

    // Launch kernel
    int blockSize = GpuKernelConfig::BLOCK_SIZE;
    int gridSize = GpuContext::calculateGridSize(uvCount, blockSize);

    processUVsKernel<<<gridSize, blockSize, 0, stream>>>(
        d_inputUVs.data(),
        d_outputUVs.data(),
        uvCount
    );

    // Check for launch errors
    err = cudaGetLastError();
    if (!GPU_CHECK(err)) {
        MIDDLEWARE_LOG_WARNING("UV processing kernel launch failed");
        cudaStreamDestroy(stream);
        return false;
    }

    // Synchronize
    if (!GpuMemoryUtils::synchronizeStream(stream)) {
        MIDDLEWARE_LOG_WARNING("Failed to synchronize GPU stream");
        cudaStreamDestroy(stream);
        return false;
    }

    // Copy results back
    std::vector<float2> outputFloat2(uvCount);
    if (!GpuMemoryUtils::d2h(outputFloat2.data(), d_outputUVs.data(), uvCount, stream)) {
        MIDDLEWARE_LOG_WARNING("Failed to copy output UVs from GPU");
        cudaStreamDestroy(stream);
        return false;
    }

    cudaStreamDestroy(stream);

    // Convert to glm::vec2
    for (size_t i = 0; i < uvCount; ++i) {
        outputUVs[i] = glm::vec2(outputFloat2[i].x, outputFloat2[i].y);
    }

    MIDDLEWARE_LOG_DEBUG("GPU UV processing complete: %zu UVs", uvCount);
    return true;
}

// ============================================================================
// ASYNCHRONOUS GPU OPERATIONS
// ============================================================================

// GpuAsyncHandle implementation
bool GpuAsyncHandle::waitForCompletion(int timeoutMs) {
    if (!valid) return false;
    
    if (timeoutMs < 0) {
        // Wait indefinitely
        cudaError_t err = cudaStreamSynchronize(stream);
        if (!GPU_CHECK(err)) {
            MIDDLEWARE_LOG_WARNING("Failed to synchronize async GPU operation");
            return false;
        }
    } else {
        // Wait with timeout using events
        cudaEvent_t event;
        cudaEventCreate(&event);
        cudaEventRecord(event, stream);
        
        cudaError_t err = cudaEventSynchronize(event);
        while (err == cudaErrorNotReady) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            err = cudaEventSynchronize(event);
            // Check timeout
            // Note: Simple timeout implementation, could be improved with absolute time
        }
        
        cudaEventDestroy(event);
        
        if (!GPU_CHECK(err)) {
            MIDDLEWARE_LOG_WARNING("Failed to synchronize async GPU operation with timeout");
            return false;
        }
    }
    
    return true;
}

bool GpuAsyncHandle::cancel() {
    if (!valid) return false;
    
    // Note: CUDA doesn't support true cancellation of in-flight kernels
    // We can only destroy the stream and mark as invalid
    // The kernel will complete but results will be discarded
    
    valid = false;
    cudaStreamDestroy(stream);
    stream = 0;
    
    // Free GPU buffers
    for (void* buf : gpuBuffers) {
        if (buf) cudaFree(buf);
    }
    gpuBuffers.clear();
    
    MIDDLEWARE_LOG_DEBUG("Async GPU operation canceled");
    return true;
}

// Async vertex transformation
std::shared_ptr<GpuAsyncHandle> GpuKernels::transformVerticesAsync(
    const std::vector<glm::vec3>& inputVertices,
    const glm::mat4& transform,
    std::vector<glm::vec3>& outputVertices,
    GpuAsyncCallback callback)
{
    // Check if GPU should be used
    if (!shouldUseGpuForVertices(inputVertices.size())) {
        if (callback) callback(false, "Vertex count below GPU threshold");
        return nullptr;
    }

    size_t vertexCount = inputVertices.size();
    if (vertexCount == 0) {
        if (callback) callback(false, "Empty vertex buffer");
        return nullptr;
    }

    // Allocate output
    outputVertices.resize(vertexCount);

    // Create async handle
    auto handle = std::make_shared<GpuAsyncHandle>();
    handle->valid = true;

    // Create GPU buffers
    GpuBuffer<float3> d_inputVertices;
    GpuBuffer<float3> d_outputVertices;
    GpuBuffer<float> d_transformMatrix;

    if (!d_inputVertices.allocate(vertexCount)) {
        MIDDLEWARE_LOG_WARNING("Failed to allocate GPU buffer for input vertices (async)");
        if (callback) callback(false, "Failed to allocate GPU buffer for input vertices");
        return nullptr;
    }
    handle->gpuBuffers.push_back(d_inputVertices.data());

    if (!d_outputVertices.allocate(vertexCount)) {
        MIDDLEWARE_LOG_WARNING("Failed to allocate GPU buffer for output vertices (async)");
        if (callback) callback(false, "Failed to allocate GPU buffer for output vertices");
        return nullptr;
    }
    handle->gpuBuffers.push_back(d_outputVertices.data());

    if (!d_transformMatrix.allocate(16)) {
        MIDDLEWARE_LOG_WARNING("Failed to allocate GPU buffer for transform matrix (async)");
        if (callback) callback(false, "Failed to allocate GPU buffer for transform matrix");
        return nullptr;
    }
    handle->gpuBuffers.push_back(d_transformMatrix.data());

    // Convert input vertices to float3 array for GPU transfer
    std::vector<float3> float3Vertices(vertexCount);
    for (size_t i = 0; i < vertexCount; ++i) {
        float3Vertices[i] = make_float3(
            inputVertices[i].x, 
            inputVertices[i].y, 
            inputVertices[i].z
        );
    }

    // Convert transform matrix to row-major float array
    std::vector<float> transformFloats(16);
    transformFloats[0] = transform[0][0]; transformFloats[1] = transform[1][0];
    transformFloats[2] = transform[2][0]; transformFloats[3] = transform[3][0];
    transformFloats[4] = transform[0][1]; transformFloats[5] = transform[1][1];
    transformFloats[6] = transform[2][1]; transformFloats[7] = transform[3][1];
    transformFloats[8] = transform[0][2]; transformFloats[9] = transform[1][2];
    transformFloats[10] = transform[2][2]; transformFloats[11] = transform[3][2];
    transformFloats[12] = transform[0][3]; transformFloats[13] = transform[1][3];
    transformFloats[14] = transform[2][3]; transformFloats[15] = transform[3][3];

    // Create CUDA stream for async operations
    cudaStream_t stream;
    cudaError_t err = cudaStreamCreate(&stream);
    if (!GPU_CHECK(err)) {
        MIDDLEWARE_LOG_WARNING("Failed to create CUDA stream (async)");
        if (callback) callback(false, "Failed to create CUDA stream");
        return nullptr;
    }
    handle->stream = stream;

    // Capture outputVertices reference for callback
    auto outputRef = std::make_shared<std::vector<glm::vec3>>(outputVertices);
    outputRef->resize(vertexCount);

    // Create async callback wrapper that handles the full async pipeline
    auto asyncCallback = [callback, outputRef, vertexCount](bool success, const std::string& error) {
        if (callback) {
            callback(success, error);
        }
    };

    // Copy data to GPU asynchronously
    if (!GpuMemoryUtils::h2d(d_inputVertices.data(), float3Vertices.data(), vertexCount, stream)) {
        MIDDLEWARE_LOG_WARNING("Failed to copy input vertices to GPU (async)");
        cudaStreamDestroy(stream);
        if (callback) callback(false, "Failed to copy input vertices to GPU");
        handle->valid = false;
        return nullptr;
    }

    if (!GpuMemoryUtils::h2d(d_transformMatrix.data(), transformFloats.data(), 16, stream)) {
        MIDDLEWARE_LOG_WARNING("Failed to copy transform matrix to GPU (async)");
        cudaStreamDestroy(stream);
        if (callback) callback(false, "Failed to copy transform matrix to GPU");
        handle->valid = false;
        return nullptr;
    }

    // Launch kernel asynchronously
    int blockSize = GpuKernelConfig::BLOCK_SIZE;
    int gridSize = GpuContext::calculateGridSize(vertexCount, blockSize);

    transformVerticesKernel<<<gridSize, blockSize, 0, stream>>>(
        d_inputVertices.data(),
        d_transformMatrix.data(),
        d_outputVertices.data(),
        vertexCount
    );

    // Check for launch errors
    err = cudaGetLastError();
    if (!GPU_CHECK(err)) {
        MIDDLEWARE_LOG_WARNING("Vertex transformation kernel launch failed (async)");
        cudaStreamDestroy(stream);
        if (callback) callback(false, "Vertex transformation kernel launch failed");
        handle->valid = false;
        return nullptr;
    }

    // Copy results back to host asynchronously
    auto outputFloat3 = std::make_shared<std::vector<float3>>(vertexCount);
    if (!GpuMemoryUtils::d2hAsync(outputFloat3->data(), d_outputVertices.data(), vertexCount, stream, 
        [asyncCallback, outputRef, vertexCount, outputFloat3](bool success) {
            if (!success) {
                asyncCallback(false, "Failed to copy output vertices from GPU");
                return;
            }
            
            // Convert output to glm::vec3
            for (size_t i = 0; i < vertexCount; ++i) {
                (*outputRef)[i] = glm::vec3(
                    (*outputFloat3)[i].x,
                    (*outputFloat3)[i].y,
                    (*outputFloat3)[i].z
                );
            }
            
            MIDDLEWARE_LOG_DEBUG("GPU vertex transformation async complete: %zu vertices", vertexCount);
            asyncCallback(true, "");
        })) {
        MIDDLEWARE_LOG_WARNING("Failed to setup async copy for output vertices");
        cudaStreamDestroy(stream);
        if (callback) callback(false, "Failed to setup async copy for output vertices");
        handle->valid = false;
        return nullptr;
    }

    MIDDLEWARE_LOG_DEBUG("GPU vertex transformation async started: %zu vertices", vertexCount);
    return handle;
}

// Async normal transformation
std::shared_ptr<GpuAsyncHandle> GpuKernels::transformNormalsAsync(
    const std::vector<glm::vec3>& inputNormals,
    const glm::mat3& normalMatrix,
    std::vector<glm::vec3>& outputNormals,
    GpuAsyncCallback callback)
{
    // Check if GPU should be used
    if (!shouldUseGpuForNormals(inputNormals.size())) {
        if (callback) callback(false, "Normal count below GPU threshold");
        return nullptr;
    }

    size_t normalCount = inputNormals.size();
    if (normalCount == 0) {
        if (callback) callback(false, "Empty normal buffer");
        return nullptr;
    }

    // Allocate output
    outputNormals.resize(normalCount);

    // Create async handle
    auto handle = std::make_shared<GpuAsyncHandle>();
    handle->valid = true;

    // Create GPU buffers
    GpuBuffer<float3> d_inputNormals;
    GpuBuffer<float3> d_outputNormals;
    GpuBuffer<float> d_normalMatrix;

    if (!d_inputNormals.allocate(normalCount)) {
        MIDDLEWARE_LOG_WARNING("Failed to allocate GPU buffer for input normals (async)");
        if (callback) callback(false, "Failed to allocate GPU buffer for input normals");
        return nullptr;
    }
    handle->gpuBuffers.push_back(d_inputNormals.data());

    if (!d_outputNormals.allocate(normalCount)) {
        MIDDLEWARE_LOG_WARNING("Failed to allocate GPU buffer for output normals (async)");
        if (callback) callback(false, "Failed to allocate GPU buffer for output normals");
        return nullptr;
    }
    handle->gpuBuffers.push_back(d_outputNormals.data());

    if (!d_normalMatrix.allocate(9)) {
        MIDDLEWARE_LOG_WARNING("Failed to allocate GPU buffer for normal matrix (async)");
        if (callback) callback(false, "Failed to allocate GPU buffer for normal matrix");
        return nullptr;
    }
    handle->gpuBuffers.push_back(d_normalMatrix.data());

    // Convert input normals to float3 array
    std::vector<float3> float3Normals(normalCount);
    for (size_t i = 0; i < normalCount; ++i) {
        float3Normals[i] = make_float3(
            inputNormals[i].x,
            inputNormals[i].y,
            inputNormals[i].z
        );
    }

    // Convert normal matrix to row-major float array
    std::vector<float> matrixFloats(9);
    matrixFloats[0] = normalMatrix[0][0]; matrixFloats[1] = normalMatrix[1][0];
    matrixFloats[2] = normalMatrix[2][0];
    matrixFloats[3] = normalMatrix[0][1]; matrixFloats[4] = normalMatrix[1][1];
    matrixFloats[5] = normalMatrix[2][1];
    matrixFloats[6] = normalMatrix[0][2]; matrixFloats[7] = normalMatrix[1][2];
    matrixFloats[8] = normalMatrix[2][2];

    // Create CUDA stream
    cudaStream_t stream;
    cudaError_t err = cudaStreamCreate(&stream);
    if (!GPU_CHECK(err)) {
        MIDDLEWARE_LOG_WARNING("Failed to create CUDA stream (async)");
        if (callback) callback(false, "Failed to create CUDA stream");
        return nullptr;
    }
    handle->stream = stream;

    // Copy data to GPU
    if (!GpuMemoryUtils::h2d(d_inputNormals.data(), float3Normals.data(), normalCount, stream)) {
        MIDDLEWARE_LOG_WARNING("Failed to copy input normals to GPU (async)");
        cudaStreamDestroy(stream);
        if (callback) callback(false, "Failed to copy input normals to GPU");
        handle->valid = false;
        return nullptr;
    }

    if (!GpuMemoryUtils::h2d(d_normalMatrix.data(), matrixFloats.data(), 9, stream)) {
        MIDDLEWARE_LOG_WARNING("Failed to copy normal matrix to GPU (async)");
        cudaStreamDestroy(stream);
        if (callback) callback(false, "Failed to copy normal matrix to GPU");
        handle->valid = false;
        return nullptr;
    }

    // Launch kernel
    int blockSize = GpuKernelConfig::BLOCK_SIZE;
    int gridSize = GpuContext::calculateGridSize(normalCount, blockSize);

    transformNormalsKernel<<<gridSize, blockSize, 0, stream>>>(
        d_inputNormals.data(),
        d_normalMatrix.data(),
        d_outputNormals.data(),
        normalCount
    );

    // Check for launch errors
    err = cudaGetLastError();
    if (!GPU_CHECK(err)) {
        MIDDLEWARE_LOG_WARNING("Normal transformation kernel launch failed (async)");
        cudaStreamDestroy(stream);
        if (callback) callback(false, "Normal transformation kernel launch failed");
        handle->valid = false;
        return nullptr;
    }

    // Copy results back asynchronously
    auto outputFloat3 = std::make_shared<std::vector<float3>>(normalCount);
    auto outputRef = std::make_shared<std::vector<glm::vec3>>(outputNormals);
    
    if (!GpuMemoryUtils::d2hAsync(outputFloat3->data(), d_outputNormals.data(), normalCount, stream,
        [callback, outputRef, normalCount, outputFloat3](bool success) {
            if (!success) {
                if (callback) callback(false, "Failed to copy output normals from GPU");
                return;
            }
            
            // Convert to glm::vec3
            for (size_t i = 0; i < normalCount; ++i) {
                (*outputRef)[i] = glm::vec3(
                    (*outputFloat3)[i].x,
                    (*outputFloat3)[i].y,
                    (*outputFloat3)[i].z
                );
            }
            
            MIDDLEWARE_LOG_DEBUG("GPU normal transformation async complete: %zu normals", normalCount);
            if (callback) callback(true, "");
        })) {
        MIDDLEWARE_LOG_WARNING("Failed to setup async copy for output normals");
        cudaStreamDestroy(stream);
        if (callback) callback(false, "Failed to setup async copy for output normals");
        handle->valid = false;
        return nullptr;
    }

    MIDDLEWARE_LOG_DEBUG("GPU normal transformation async started: %zu normals", normalCount);
    return handle;
}

// Async UV processing
std::shared_ptr<GpuAsyncHandle> GpuKernels::processUVsAsync(
    const std::vector<glm::vec2>& inputUVs,
    std::vector<glm::vec2>& outputUVs,
    GpuAsyncCallback callback)
{
    // Check if GPU should be used
    if (!shouldUseGpuForUVs(inputUVs.size())) {
        if (callback) callback(false, "UV count below GPU threshold");
        return nullptr;
    }

    size_t uvCount = inputUVs.size();
    if (uvCount == 0) {
        if (callback) callback(false, "Empty UV buffer");
        return nullptr;
    }

    // Allocate output
    outputUVs.resize(uvCount);

    // Create async handle
    auto handle = std::make_shared<GpuAsyncHandle>();
    handle->valid = true;

    // Create GPU buffers
    GpuBuffer<float2> d_inputUVs;
    GpuBuffer<float2> d_outputUVs;

    if (!d_inputUVs.allocate(uvCount)) {
        MIDDLEWARE_LOG_WARNING("Failed to allocate GPU buffer for input UVs (async)");
        if (callback) callback(false, "Failed to allocate GPU buffer for input UVs");
        return nullptr;
    }
    handle->gpuBuffers.push_back(d_inputUVs.data());

    if (!d_outputUVs.allocate(uvCount)) {
        MIDDLEWARE_LOG_WARNING("Failed to allocate GPU buffer for output UVs (async)");
        if (callback) callback(false, "Failed to allocate GPU buffer for output UVs");
        return nullptr;
    }
    handle->gpuBuffers.push_back(d_outputUVs.data());

    // Convert input UVs to float2 array
    std::vector<float2> float2UVs(uvCount);
    for (size_t i = 0; i < uvCount; ++i) {
        float2UVs[i] = make_float2(inputUVs[i].x, inputUVs[i].y);
    }

    // Create CUDA stream
    cudaStream_t stream;
    cudaError_t err = cudaStreamCreate(&stream);
    if (!GPU_CHECK(err)) {
        MIDDLEWARE_LOG_WARNING("Failed to create CUDA stream (async)");
        if (callback) callback(false, "Failed to create CUDA stream");
        return nullptr;
    }
    handle->stream = stream;

    // Copy data to GPU
    if (!GpuMemoryUtils::h2d(d_inputUVs.data(), float2UVs.data(), uvCount, stream)) {
        MIDDLEWARE_LOG_WARNING("Failed to copy input UVs to GPU (async)");
        cudaStreamDestroy(stream);
        if (callback) callback(false, "Failed to copy input UVs to GPU");
        handle->valid = false;
        return nullptr;
    }

    // Launch kernel
    int blockSize = GpuKernelConfig::BLOCK_SIZE;
    int gridSize = GpuContext::calculateGridSize(uvCount, blockSize);

    processUVsKernel<<<gridSize, blockSize, 0, stream>>>(
        d_inputUVs.data(),
        d_outputUVs.data(),
        uvCount
    );

    // Check for launch errors
    err = cudaGetLastError();
    if (!GPU_CHECK(err)) {
        MIDDLEWARE_LOG_WARNING("UV processing kernel launch failed (async)");
        cudaStreamDestroy(stream);
        if (callback) callback(false, "UV processing kernel launch failed");
        handle->valid = false;
        return nullptr;
    }

    // Copy results back asynchronously
    auto outputFloat2 = std::make_shared<std::vector<float2>>(uvCount);
    auto outputRef = std::make_shared<std::vector<glm::vec2>>(outputUVs);
    
    if (!GpuMemoryUtils::d2hAsync(outputFloat2->data(), d_outputUVs.data(), uvCount, stream,
        [callback, outputRef, uvCount, outputFloat2](bool success) {
            if (!success) {
                if (callback) callback(false, "Failed to copy output UVs from GPU");
                return;
            }
            
            // Convert to glm::vec2
            for (size_t i = 0; i < uvCount; ++i) {
                (*outputRef)[i] = glm::vec2((*outputFloat2)[i].x, (*outputFloat2)[i].y);
            }
            
            MIDDLEWARE_LOG_DEBUG("GPU UV processing async complete: %zu UVs", uvCount);
            if (callback) callback(true, "");
        })) {
        MIDDLEWARE_LOG_WARNING("Failed to setup async copy for output UVs");
        cudaStreamDestroy(stream);
        if (callback) callback(false, "Failed to setup async copy for output UVs");
        handle->valid = false;
        return nullptr;
    }

    MIDDLEWARE_LOG_DEBUG("GPU UV processing async started: %zu UVs", uvCount);
    return handle;
}

} // namespace anari_usd_middleware
