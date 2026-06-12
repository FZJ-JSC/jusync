# Modules

Index of all modules in the JUSYNC middleware library.

---

## Core
- [[Modules/Middleware Core]] — `AnariUsdMiddleware` PIMPL class, public API entry point
- [[Modules/Logging System]] — Cross-platform logging macros, safety utilities

## Network
- [[Modules/Network Layer]] — `ZmqConnector` (ROUTER — ⚠️ dead code, DEALER-only mode)
- [[Modules/ZeroMQ Client]] — `AnariUsdClient` (DEALER socket, outbound — active path)
- [[Modules/Message Protocol]] — Binary wire format, `ZmqMessageType` enum, packed structs

## USD Processing
- [[Modules/USD Processor]] — `UsdProcessor` with TinyUSDZ backend, mesh/texture/point cloud extraction
- [[Modules/Point Cloud]] — GeomPoints extraction, gradient color baking
- [[Modules/Color Interpolation]] — Vertex color modes: vertex/uniform/fallback

## Hashing
- [[Modules/Hash Verifier]] — `HashVerifier` with XXH3-128 (replaced SHA-256)

## Collisions
- [[Modules/Collision Processor]] — `CollisionProcessor`, bounding boxes, convex hulls, decimation

## GPU
- [[Modules/CUDA GPU]] — `GpuContext`, `GpuKernels`, `GpuMemory`, `GpuValidation`

## Parallelism
- [[Modules/Parallel Downloader]] — `ParallelDownloader` (high-level wrapper) + `ParallelDownloadManager` (RAM-aware streaming)
- [[Modules/Memory Monitor]] — `MemoryMonitor` (RAM awareness for downloads)

## C Interface
- [[Modules/C Interface]] — `AnariUsdMiddleware_C.h` FFI wrapper
- [[Modules/C Global State]] — Single-instance-per-process model, global atomics

## Dependencies
- [[Reference/External Dependencies]] — Bundled libs in `external/` (zmq, tinyusdz, glm, xxhash, stb, nlohmann, imgui)
