# LLM Architecture Guide

> This Obsidian vault is designed as a **source of truth for AI/LLM agents** to understand the JUSYNC codebase. When you need to edit, debug, or extend the code, consult the notes below to find the right source files before touching them.

---

## How to Use This Vault

1. **Find a component** → Go to [[Modules/Modules]] for the index
2. **Read the module note** → It lists exact file paths, public API, internal methods, data structures, and thread-safety rules
3. **Jump to source** → Every note links `header.h` and `implementation.cpp` paths
4. **Trace data flow** → Read [[02 Architecture Overview]] for the thread model and pipeline
5. **Understand protocol** → Read [[Modules/Message Protocol]] for wire format and `AnariUsdMessages.h` struct layouts

---

## Quick File Map (AI Agent Cheatsheet)

If you need to edit a specific concern, start at these files:

| Concern | Start Here | Also Read |
|---|---|---|
| **Public API surface** | `include/AnariUsdMiddleware.h` | `src/AnariUsdMiddleware.cpp` |
| **Inbound ZeroMQ** | `include/ZmqConnector.h` | `src/ZmqConnector.cpp` |
| **Outbound ZeroMQ (DEALER)** | `include/AnariUsdClient.h` | `src/AnariUsdClient.cpp` |
| **Binary wire format** | `include/AnariUsdMessages.h` | — |
| **USD mesh extraction** | `include/UsdProcessor.h` | `src/UsdProcessor.cpp` |
| **Hash verification** | `include/HashVerifier.h` | `src/HashVerifier.cpp` |
| **Collision generation** | `include/CollisionProcessor.h` | `src/CollisionProcessor.cpp` |
| **Parallel downloads** | `include/ParallelDownloadManager.h` | `src/ParallelDownloadManager.cpp` |
| **RAM guard** | `include/MemoryMonitor.h` | `src/MemoryMonitor.cpp` |
| **GPU kernels** | `include/GpuKernels.h` | `src/GpuKernels.cu` |
| **GPU context** | `include/GpuContext.h` | `src/GpuContext.cpp` |
| **GPU memory** | `include/GpuMemory.h` | (header-only template) |
| **CPU-GPU validation** | `include/GpuValidation.h` | `src/GpuValidation.cpp` |
| **C FFI wrapper** | `include/AnariUsdMiddleware_C.h` | `src/AnariUsdMiddleware_C.cpp` |
| **Logging/safety macros** | `include/MiddlewareLogging.h` | — |
| **Build config** | `CMakeLists.txt` | `CMakePresets.json` |

---

## Key Design Rules (Edit Safely)

### Thread Model
- **ZmqConnector** (ROUTER): ⚠️ ROUTER socket is dead code. `zmqSocket` is null. No receiver thread exists.
- **AnariUsdClient** (DEALER): owns a **dispatcher thread** that serializes ALL `recv()`. Other threads only dequeue from `responseQueue`. **Never call `recv()` directly from a request thread.**
- **ParallelDownloadManager**: has dispatcher + scheduler + N worker threads. Workers send requests; dispatcher receives; scheduler gates on RAM.

### Memory Safety
- All safety constants are in `MiddlewareLogging.h` → `anari_usd_middleware::safety` namespace
- Use `MIDDLEWARE_VALIDATE_POINTER(ptr, "context")` for null checks
- Use `MIDDLEWARE_SAFE_ARRAY_ACCESS(arr, idx, "context")` for bounds
- Use `MIDDLEWARE_VALIDATE_FINITE(value, "context")` for NaN/Inf

### GPU Fallback
- Everything behind `#ifdef ENABLE_CUDA_ACCELERATION`
- Stub classes exist for non-CUDA builds — they return `false` to trigger CPU fallback
- Threshold: 10,000 elements minimum before GPU is used (`GpuKernelConfig::MIN_*`)

### Hash Algorithm
- **XXH3-128** (NOT SHA-256). Hash is `{uint64_t lo, uint64_t hi}`.
- `HashVerifier` is stateless — no mutex, no instance needed.

### PIMPL
- `AnariUsdMiddleware` uses `std::unique_ptr<Impl> pImpl`
- `UsdProcessor` uses `std::unique_ptr<UsdProcessorImpl> pImpl`
- If adding members, add to `Impl`, not the public class.

### Critical Fixes in Code
- **"0: None" → "0: []"** — TinyUSDZ fails on empty timeSampled arrays. Applied to ALL files before any parsing. Without this, large geometry files fail with `MeshCount=0`.
- **DEALER-only mode** — ROUTER functionality is dead code. `ZmqConnector` creates no socket. `startReceiving()` is a no-op.

### Known Dead Code
- **ROUTER receiver thread** — `zmqSocket` and `zmqContext` always null. `receiveFile()`/`receiveAnyMessage()` are stubs. `startReceiving()`/`stopReceiving()` are no-ops for API compatibility.
- **ParallelDownloader** — Compile guard added. `.cpp` not in CMake build. Use `ParallelDownloadManager`.
- **Collision simplified impls** — `ConvexHull` copies first 300 indices. `Simplified` uses sampling. `ConvexDecomp` falls back to hull.

### Known Bugs — FIXED
- ~~MemoryMonitor Linux~~ → Now reads `/proc/meminfo MemAvailable`.
- ~~MemoryMonitor TOCTOU~~ → Check moved inside `allocate()` lock.
- ~~ParallelDownloader broken~~ → Compile guard + removed from build.
- ~~ZmqConnector regex~~ → `static const`.
- ~~glm→flat conversion~~ → `resize()` + direct index writes.
- ~~VectorMemoryPool~~ → Removed.
- ~~CMake duplicate TBB~~ → Removed.

### Remaining issues (not fixed)
- **GPU kernels h2d/d2h** — Allocates GpuBuffer + copies on every kernel call. Persistent pinned buffers would help. Designated as out-of-scope.
- **File list JSON blocking** — `requestFileListWithSizes` blocks for JSON parsing. Consider async for 16-rank scenarios.

---

## External Dependencies

| External | Path | Type |
|---|---|---|
| ZeroMQ | `external/zmq/` | Static build |
| ZeroMQ (dup?) | `external/ZeroMQ/` | ⚠️ Possibly duplicate |
| TinyUSDZ | `external/tinyusdz/` | Static build |
| GLM | `external/glm/` | Header-only |
| xxHash | `external/xxhash/` | Compiled into target |
| STB | `external/stb/` | Header-only |
| nlohmann/json | `external/nlohmann/` | Header-only, JSON parsing |
| Dear ImGui | `external/imgui/` | ReceiverUI tool only |
| cppzmq | `external/cppzmq/` | Header-only, ZMQ C++ bindings |
