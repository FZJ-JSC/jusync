# Edit Guide -- Where to Change What

When modifying the codebase, use this guide to find the right file(s).

---

## Adding a New Public API Method

1. Declare in `include/AnariUsdMiddleware.h` (public section)
2. Implement in `src/AnariUsdMiddleware.cpp` (inside `class Impl`)
3. If it also needs a C wrapper, add to `include/AnariUsdMiddleware_C.h` + `src/AnariUsdMiddleware_C.cpp`

## Changing Network Protocol

1. Message types: `include/AnariUsdMessages.h` — `ZmqMessageType` enum + new packed struct
2. Send logic: `src/AnariUsdClient.cpp` — add handler for new request type
3. Receive logic: `src/AnariUsdClient.cpp` — dispatcher demux + new response handler
4. ROUTER receive: `src/ZmqConnector.cpp` — if incoming from broker push

## Adding a New Mesh Data Field

1. Internal struct: `include/UsdProcessor.h` — `MeshData` (glm-based, used during processing)
2. Public struct: `include/AnariUsdMiddleware.h` — `MeshData` (flat float arrays, API surface)
3. Conversion: `src/UsdProcessor.cpp` — `MeshData::toMiddlewareMeshData()` method
4. Validation: Update `MeshData::isValid()` in both headers

## Modifying USD Processing Pipeline

1. Prim walk: `src/UsdProcessor.cpp` — `ProcessPrim()`
2. Mesh extraction: `src/UsdProcessor.cpp` — `ExtractMeshData()`
3. Point cloud: `src/UsdProcessor.cpp` — `ExtractPointCloudData()`
4. Reference resolution: `src/UsdProcessor.cpp` — `resolveReferences()`, `loadReferencedFile()`

## Adding GPU Kernel

1. Declaration: `include/GpuKernels.h` — sync + async variants
2. Implementation: `src/GpuKernels.cu` — `__global__` kernel + host launcher
3. Memory: `include/GpuMemory.h` — `GpuBuffer<T>` if new buffer type needed
4. Validation: `include/GpuValidation.h` + `src/GpuValidation.cpp` — CPU reference comparison
5. CPU fallback: Update `GpuKernels` stub class (below `#else ENABLE_CUDA_ACCELERATION`)

## Modifying Collision Types

1. Enum: `include/CollisionProcessor.h` — `ECollisionComplexity`
2. Generation: `src/CollisionProcessor.cpp` — add new `generate*Collision()` method
3. Validation: Update `CollisionData::isValid()` and `validateFiniteValues()`

## Adding a Callback

1. Typedef: `include/AnariUsdMiddleware.h` — `using NewCallback = std::function<...>`
2. Registration: Add `registerNewCallback(NewCallback)` method
3. Storage: Add to `Impl` class in `src/AnariUsdMiddleware.cpp`
4. Thread safety: Protect with `std::shared_mutex` or `std::mutex`

## Changing Safety Limits

1. Constants: `include/MiddlewareLogging.h` — `anari_usd_middleware::safety` namespace
2. Max recursion: `UsdProcessor::maxRecursionDepth` (runtime configurable)
3. Memory limit: `UsdProcessor::memoryLimitMB` (runtime configurable)

## Modifying ZMQ Configuration

1. ROUTER socket: `src/ZmqConnector.cpp` — `configurePlatformSpecificSocket()`
2. DEALER socket: `src/AnariUsdClient.cpp` — `configureSocket()`
3. Default endpoints: `ZmqConnector::getDefaultEndpoint()`
4. Timeout values: Check method defaults (e.g., `timeoutMs = 5000`)

## Adding a Test Mode

1. Tool: `tests/src/usd_validation_tool.cpp`
2. CMake: `CMakeLists.txt` — add `add_test()` entry
3. Dependencies: `set_tests_properties(... PROPERTIES DEPENDS ...)`

---

## Quick Performance Fixes

| Issue | File | What to Change | Status |
|---|---|---|---|
| glm→flat push_back | `src/AnariUsdMiddleware.cpp` | Fixed: `resize()` + direct index writes | ✅ Fixed |
| ZmqConnector regex | `src/ZmqConnector.cpp` | Fixed: `static const std::regex` | ✅ Fixed |
| CMake duplicate TBB | `CMakeLists.txt` | Fixed: removed duplicate block | ✅ Fixed |
| MemoryMonitor Linux | `src/MemoryMonitor.cpp` | Fixed: reads `/proc/meminfo MemAvailable` | ✅ Fixed |
| MemoryMonitor TOCTOU | `src/MemoryMonitor.cpp` | Fixed: check inside `allocate()` lock | ✅ Fixed |
| VectorMemoryPool dead code | `src/UsdProcessor.cpp` | Fixed: removed | ✅ Fixed |
| ParallelDownloader broken | `include/ParallelDownloader.h` | Fixed: `#error` compile guard | ✅ Fixed |

## Architecture Decisions Needed

- **ROUTER vs DEALER-only**: `receiveFile()` and `receiveAnyMessage()` are stubs. `zmqContext`/`zmqSocket` always null. `startReceiving()`/`stopReceiving()` are no-ops for API compatibility. Either fully remove ROUTER code or re-enable ROUTER socket creation in `initialize()`.
- **Collision simplified impls**: `generateConvexHullCollision` copies first 300 indices (not true convex hull). `generateSimplifiedCollision` uses sampling decimation (not QEM). `generateConvexDecomp` falls back to hull. Implement properly or document as "good-enough".
- **GPU kernels h2d/d2h**: `GpuKernels.cu` allocates/copy/launch/copy every call. Persistent pinned buffers + CUDA streams would cut PCIe overhead.
- **File list JSON parsing**: `AnariUsdClient::requestFileListWithSizes` blocks. Consider async parsing for 16-rank broadcast.
