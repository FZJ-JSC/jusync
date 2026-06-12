# Architecture Overview

JUSYNC bridges **USD content creation tools** (Maya, Houdini, HPC renderers) with **ANARI-based real-time rendering** and **Unreal Engine**. It runs as a shared library (`libanari_usd_middleware.so`) embedded in the consuming application.

## System Diagram

```
┌──────────────┐   DEALER/ROUTER    ┌──────────────────────────────────────┐
│   HPC Worker │ ◀─── ZeroMQ ─────▶ │             Receiver                 │
│  (Rank N)    │                    │  ┌────────────────────────────────┐  │
│              │                    │  │       ZmqConnector             │  │
│ Sends USD /  │                    │  │  (ROUTER socket, bind)         │  │
│ Files over   │                    │  └──────────┬─────────────────────┘  │
│ ZeroMQ       │                    │             │                         │
└──────────────┘                    │             ▼                         │┐
                                    │     ┌─────────────────────┐          │  │
┌──────────────┐                    │     │   HashVerifier       │          │  │
│  Python      │                    │     │  (XXH3-128 hash)    │          │  │
│   Broker     │                    │     └──────────┬──────────┘          │  │
│  (ROUTER)    │                    │               │ validated            │  │
└──────┬───────┘                    │               ▼                       │  │
       │                            │     ┌─────────────────────┐          │  │
       │ Also receives from         │     │   UsdProcessor       │          │  │
       │ AnariUsdClient (DEALER)    │     │  (TinyUSDZ backend) │          │  │
       │                            │     └──────────┬──────────┘          │  │
┌────────────────────────────────────────────────────────────────────────────┘  │
│                              Lib Inside Host Process                           │
│                                                                                │
│  ┌─────────────────────────────────────────────────────────────────────────┐   │
│  │                      AnariUsdMiddleware (PIMPL)                          │   │
│  │  ┌──────────────┐  ┌───────────────┐  ┌─────────────────────────────┐  │   │
│  │  │ ZmqConnector │  │ AnariUsdClient│  │ ParallelDownloadManager     │  │   │
│  │  │ (ROUTER)     │  │ (DEALER)      │  │ + MemoryMonitor             │  │   │
│  │  └──────────────┘  └───────────────┘  └─────────────────────────────┘  │   │
│  │  ┌──────────────┐  ┌───────────────┐  ┌─────────────────────────────┐  │   │
│  │  │ UsdProcessor │  │CollisionProc  │  │     GPU (optional)            │  │   │
│  │  │ (TinyUSDZ)   │  │               │  │  GpuContext + GpuKernels     │  │   │
│  │  └──────────────┘  └───────────────┘  └─────────────────────────────┘  │   │
│  └─────────────────────────────────────────────────────────────────────────┘   │
│                                         │                                      │
│                                         ▼                                      │
│                              Callback dispatch to host app                      │
│                              (Unreal RMC / ANARI renderer)                      │
└────────────────────────────────────────────────────────────────────────────────┘
```

## Thread Model

| Thread | Component | Purpose |
|---|---|---|
| **Receiver Thread** | `ZmqConnector` | Blocking loop on ROUTER `recv()`, deserializes multipart frames, dispatches callbacks |
| **Dispatcher Thread** | `AnariUsdClient` | Single thread that owns ALL DEALER socket `recv()` calls. Enqueues responses into `responseQueue`. Request threads dequeue — never call `recv()` directly. |
| **Download Workers** | `ParallelDownloadManager` | Pool of worker threads that send file requests, accumulate chunks, and spawn completed files immediately |
| **Scheduler Thread** | `ParallelDownloadManager` | Queues pending downloads, respects RAM limits, throttles concurrency |
| **Main Thread** | Application | Calls public API, receives callbacks |

## Core Design Decisions

1. **PIMPL pattern** — `AnariUsdMiddleware` hides implementation in `class Impl`. Reduces ABI surface, allows compilation unit isolation.
2. **Static ZeroMQ** — `external/zmq` is built and linked statically. No ZMQ DLL/so dependency for end users.
3. **XXH3-128 replaced SHA-256** — Hash verification switched from OpenSSL SHA-256 to non-cryptographic XXH3-128 for speed. No mutex needed (stateless).
4. **Dispatcher thread pattern** — ZeroMQ's DEALER socket is not safe for concurrent `recv()`. A dedicated dispatcher serializes all receives into a lock-free queue. Other threads only dequeue.
5. **CPU fallback** — GPU kernels (CUDA) auto-fallback to CPU when `ENABLE_CUDA_ACCELERATION` is false, GPU is absent, or mesh is below `MIN_VERTICES_FOR_GPU` (10,000).

## Data Flow (File Reception)

1. Client or broker sends multipart ZeroMQ frame: `[ZmqFileRequest header] → [file binary data]`
2. `ZmqConnector::receiveFile()` deserializes frames on the receiver thread
3. `HashVerifier::verifyHash128()` validates XXH3-128 hash
4. `FileUpdateCallback` fires with `FileData { filename, data, hash, fileType }`
5. Consumer calls `LoadUSDBuffer()` → `UsdProcessor` extracts mesh arrays
6. `MeshData` returned with vertices, normals, UVs, indices, collision, subdivision scheme

## Data Flow (Parallel Download)

1. App calls `requestFilesParallelAsync()` with filenames + target ranks
2. `ParallelDownloadManager` schedules downloads respecting RAM budget (`MemoryMonitor`)
3. Workers send `REQ_GET_FILE` over DEALER socket; dispatcher thread receives chunks
4. Each completed file fires `spawn_callback(filename, data)` immediately — memory freed right after
5. When all finish, `completionCallback()` fires

See [[Build/05 Network Protocol]] for wire format details.
