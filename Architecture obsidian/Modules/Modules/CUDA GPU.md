# CUDA GPU

Optional GPU acceleration for heavy mesh processing operations (vertex transforms, normal transforms, UV processing).

## Files
| File | Purpose |
|---|---|
| `include/GpuContext.h` | CUDA device detection, singleton context |
| `include/GpuKernels.h` | Kernel declarations (sync + async API) |
| `include/GpuMemory.h` | GPU memory allocation, h2d/d2h transfers |
| `include/GpuValidation.h` | CPU-vs-GPU result validation |
| `src/GpuContext.cpp` | Device init, error handling |
| `src/GpuValidation.cpp` | Validation logic |
| `src/GpuKernels.cu` | CUDA kernel implementations |

## Compile-Time Gate

All GPU code is behind `#ifdef ENABLE_CUDA_ACCELERATION`. When CUDA is not available, stub classes compile with no-op implementations returning `false` (triggers CPU fallback).

```bash
cmake -DENABLE_CUDA=ON ..
```

## GPU Kernel Configuration

```cpp
struct GpuKernelConfig {
    static constexpr size_t MIN_VERTICES_FOR_GPU = 10000;
    static constexpr size_t MIN_UVS_FOR_GPU = 10000;
    static constexpr size_t MIN_NORMALS_FOR_GPU = 10000;
    static constexpr int BLOCK_SIZE = 256;
    static constexpr float VALIDATION_TOLERANCE = 1e-5f;
};
```

Thresholds prevent GPU overhead on small meshes. Below 10K elements, CPU is faster.

## GpuContext (Singleton)

```cpp
class GpuContext {
public:
    static GpuContext& getInstance();
    static bool isAvailable();
    static const GpuDeviceInfo& getDeviceInfo();

    bool initialize();          // Detect devices, select best, init
    void shutdown();

    bool meetsMinimumRequirements() const;
    bool isInitialized() const;
    int getDeviceId() const;

    static int getRecommendedBlockSize();  // 256
    static int calculateGridSize(elementCount, blockSize);
    static std::string getErrorString(int error);
    static bool checkCudaError(error, operation, file, line);
};
```

### Error Checking Macro
```cpp
#define GPU_CHECK(error) \
    GpuContext::checkCudaError(error, #error, __FILE__, __LINE__)
```

### Device Info
```cpp
struct GpuDeviceInfo {
    int deviceId;
    string deviceName;
    int computeCapabilityMajor, computeCapabilityMinor;
    size_t totalMemory, freeMemory;
    int multiprocessorCount;
    bool isAvailable;
};
```

## GpuKernels API

### Synchronous (Blocking)
```cpp
static bool transformVertices(inputVec3, mat4 transform, outputVec3);
static bool transformNormals(inputVec3, mat3 normalMatrix, outputVec3);
static bool processUVs(inputVec2, outputVec2);
```

### Asynchronous (Callback-based)
```cpp
static shared_ptr<GpuAsyncHandle> transformVerticesAsync(
    inputVec3, mat4, outputVec3, GpuAsyncCallback callback);
// Same for transformNormalsAsync, processUVsAsync
```

### Decision Helpers
```cpp
static bool shouldUseGpuForVertices(vertexCount);
static bool shouldUseGpuForUVs(uvCount);
static bool shouldUseGpuForNormals(normalCount);
```
Returns true only if GPU available AND count >= 10,000.

### GpuAsyncHandle
```cpp
class GpuAsyncHandle {
    bool waitForCompletion(int timeoutMs = -1);
    bool cancel();
    bool isValid() const;
};
```

## GpuMemory -- Safe GPU Allocations

### GpuBuffer<T> (RAII Smart Pointer)
```cpp
GpuBuffer<glm::vec3> buf;
buf.allocate(10000);           // cudaMalloc
buf.free();                    // cudaFree on destruct
buf.data();                    // T* device pointer
buf.isValid();                 // ptr != nullptr && size > 0
```

### GpuMemoryUtils
```cpp
template<typename T> static bool h2d(devicePtr, hostPtr, count, stream);
template<typename T> static bool d2h(hostPtr, devicePtr, count, stream);
template<typename T> static bool h2d(GpuBuffer<T>&, vector<T>&, stream);
template<typename T> static bool d2h(vector<T>&, GpuBuffer<T>&, stream);
template<typename T> static bool setZero(T*, count);
static bool synchronize();
static bool synchronizeStream(stream);
static size_t getFreeMemory();
static size_t getTotalMemory();
static bool hasEnoughMemory(requiredBytes);
static bool d2hAsync(dst, src, count, stream, callback);  // Stream callback
```

## GpuValidation -- CPU Reference Checking

Validates GPU results against CPU reference within tolerance (1e-5):

```cpp
static bool validateTransformVertices(gpuResults, inputVerts, transform, tolerance);
static bool validateTransformNormals(gpuResults, inputNormals, normalMatrix, tolerance);
static bool validateProcessUVs(gpuResults, inputUVs, tolerance);
static bool validateMeshData(gpuVerts, gpuNormals, gpuUVs, inputVerts, inputNormals, inputUVs, transform, normalMatrix);
```

### Validation Stats
```cpp
struct ValidationStats {
    size_t totalComparisons, passedComparisons, failedComparisons;
    float maxError, averageError;
};
static const ValidationStats& getLastValidationStats();
```

## Fallback Path

When GPU is unavailable, stubs return `false`, and the caller falls back to CPU:
```cpp
// In UsdProcessor:
if (!GpuKernels::transformVertices(verts, transform, outVerts)) {
    // CPU fallback: glm::transform per vertex
}
```
