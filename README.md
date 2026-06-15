# JUSYNC

A high-performance C++ middleware that streams USD geometry, textures, and point cloud data from **HPC simulation** (ANARI-SDK on supercomputers) to **Unreal Engine 5** in real-time. Built on ZeroMQ DEALER/ROUTER with RAM-aware parallel downloads, XXH3-128 change detection, and live diff-aware notifications.

```
HPC Workers (ANARI ranks)  →  ZMQ Broker (rank 0)  →  JUSYNC Middleware (.so)  →  UE5 Plugin (RealtimeMesh)
```

We gratefully acknowledge the Bundesministerium für Forschung, Technologie und Raumfahrt (BMFTR) and Ministerium für Kultur und Wissenschaft des Landes Nordrhein-Westfalen (MWK-NRW) for funding this work in the project InHPC-DE through the Gauss Centre for Supercomputing e.V. (www.gauss-centre.eu).

---

## Table of Contents

- [Pipeline Architecture](#pipeline-architecture)
- [Feature Matrix](#feature-matrix)
- [Dependencies](#dependencies)
- [Building](#building)
- [HPC Pipeline](#hpc-pipeline)
  - [Build ANARI-SDK on Jülich](#build-anari-sdk-on-julich)
  - [DiffCapture Status Table](#diffcapture-status-table)
  - [Run Simulation](#run-simulation)
- [Middleware](#middleware)
  - [Overview](#overview)
  - [Components](#components)
  - [Build](#build-1)
  - [Thread Model](#thread-model)
- [Network Protocol](#network-protocol)
  - [Message Types](#message-types)
  - [Wire Format](#wire-format)
  - [Protocol Flows](#protocol-flows)
- [Unreal Engine 5 Plugin](#unreal-engine-5-plugin)
  - [Setup](#setup)
  - [Blueprint Integration](#blueprint-integration)
  - [Live Updates](#live-updates)
- [API Reference](#api-reference)
- [GUI Testing Application](#gui-testing-application)
- [USD Processing Capabilities](#usd-processing-capabilities)
- [Troubleshooting](#troubleshooting)

---

## Pipeline Architecture

```
┌─────────────────────────────────────────────────────────────────────────────────────┐
│ HPC (Supercomputer - Jülich / Jureca)                                               │
│                                                                                     │
│  Rank 0 (Broker)         Rank 1...N (Workers)                                       │
│  ┌──────────────┐        ┌──────────────────┐                                       │
│  │ ZmqBroker    │ ◄────── │ UsdDevice::      │ ANARI SDK writes USD to               │
│  │ (ROUTER)     │  DEALER │  flushCommit()   │ memory store per commit               │
│  │ - fwd notifs │        │ - TrackStageMem() │                                      │
│  │ - route reqs │        │ - DiffCapture    │ Captures old state before overwrite   │
│  │ - status tbl │        │ - StoreFile()    │ New state + hash128                   │
│  └──────┬───────┘        └──────────────────┘                                       │
│         │ ZMQ (InfiniBand or localhost)                                              │
└─────────┼───────────────────────────────────────────────────────────────────────────┘
          │ NOTIFY_FILE_UPDATE_V2 (hashPrev128, hasOldData)
          │ RESP_FILE_CHUNK (4MB chunks)
┌─────────▼───────────────────────────────────────────────────────────────────────────┐
│ Middleware / Laptop / UE Host                                                       │
│                                                                                     │
│  ┌─────────────────────────────────────────────────────────────────────┐            │
│  │  AnariUsdClient (DEALER)                                           │            │
│  │  ┌──────────────────┐  ┌──────────────────┐  ┌──────────────────┐  │            │
│  │  │ Dispatcher Thread│  │ ParallelDownload │  │ UsdProcessor     │  │            │
│  │  │ (recv all msg)   │→ │ Manager + RAM    │  │ (TinyUSDZ parse) │  │            │
│  │  │ → enqueue        │  │  Monitor         │  │ → mesh / PC      │  │            │
│  │  └──────────────────┘  └──────────────────┘  └──────────────────┘  │            │
│  └─────────────┬────────────────────────────────────┬─────────────────┘            │
│                │ C API callbacks                     │ UE Plugin                     │
│                ▼                                     ▼                               │
│        CMeshData / CPointCloudData        JUSYNCSubsystem                            │
│                                              → AsyncTask(GameThread)                 │
│                                              → Broadcast to Blueprint                │
│                                              → SpawnRealtimeMesh / PointCloud        │
└─────────────────────────────────────────────────────────────────────────────────────┘
```

### Data Flow (Live Update)

```
1. HPC rank updates geometry → TrackStageMemory() exports .usda
2. DiffCapture captures old FileEntry before StoreFile() overwrites
3. SendFileNotificationV2() → broker → middleware
   (includes hash128 of NEW data, hashPrev128 of OLD data)
4. Middleware dispatcher thread recv() → NotificationCallback fires
5. UE JUSYNCSubsystem marshals to game thread → OnNotificationReceived broadcast
6. JUSYNCFileSpawnerActor compares hashPrev vs stored hash:
   - Same hash → SKIP (no download, no destroy)
   - Different hash → download → parse → spawn new → destroy old
```

---

## Feature Matrix

### ✅ Available Features

- [x] **Real-time USD Streaming**: Receive and process USD files (.usd, .usda, .usdc, .usdz) over ZeroMQ
- [x] **Live Update Notifications**: `NOTIFY_FILE_UPDATE`, `NOTIFY_COMMIT_COMPLETE`, `NOTIFY_FILE_UPDATE_V2`
- [x] **Diff-Aware Streaming (V2)**: `hashPrev128` + `hasOldData` enables skip-if-unchanged at UE side
- [x] **Parallel Downloads**: RAM-aware parallel streaming with worker pool and memory budgeting
- [x] **Advanced Mesh Processing**: Vertices, indices, normals, UVs, vertex colors, subdivision scheme
- [x] **Point Cloud / LiDAR**: Extract `GeomPoints`, bake gradient colormap colors, UE LiDAR plugin
- [x] **USD Reference Resolution**: Automatically resolves references, payloads, and clips
- [x] **XXH3-128 Hashing**: Non-cryptographic, ultra-fast hash verification (replaced SHA-256)
- [x] **GPU Acceleration**: CUDA vertex/normal/UV transforms for meshes ≥ 10K vertices
- [x] **Collision Generation**: 5 levels — None, Simple (AABB), ConvexHull, Complex (full), Simplified (decimated)
- [x] **Cross-Platform**: Windows (.dll) and Linux (.so) with dynamic library linking
- [x] **Thread-Safe**: Dispatcher thread owns all ZMQ recv(), callbacks dispatched to consumer threads
- [x] **Unreal Engine 5 Plugin**: Full plugin with Blueprints, async actions, RealtimeMesh, live updates
- [x] **DiffCapture (HPC Broker)**: Terminal status table with per-rank update tracking (10s refresh)
- [x] **Worker Discovery**: Query worker count, status, hostname, GPU info via ZMQ
- [x] **Memory Safety**: RAII patterns, pointer validation macros, bounds checking, atomic stats
- [x] **Duplicate Prevention**: Intelligent duplicate file detection and processing prevention
- [x] **GUI Testing Tool**: Dear ImGui-based application at `tools/ReceiverUI`

### ❌ Not Available

- [ ] **Animation Support**: USD animation and time-varying data is not processed
- [ ] **Material Processing**: Full material extraction (shader graphs, textures from USD) incomplete
- [ ] **Light Processing**: USD light extraction is not implemented
- [ ] **Camera Processing**: USD camera data extraction is not implemented
- [ ] **Volume Rendering**: USD volume data processing is not available
- [ ] **Instancing**: USD instancing and prototypes are not fully supported

---

## Dependencies

| Dependency | Purpose | Notes |
|---|---|---|
| **ZeroMQ** | High-performance messaging | ROUTER/DEALER sockets |
| **TinyUSDZ** | USD parsing | Embedded as submodule |
| **xxhash** | Fast non-cryptographic hashing | XXH3-128, replaces OpenSSL |
| **GLM** | 3D math | Vec3 transforms, matrix ops |
| **STB** | Image processing | PNG encode/decode |
| **CUDA** *(optional)* | GPU acceleration | Vertex/normal/UV transforms |
| **Dear ImGui** *(optional)* | GUI testing tool | `BUILD_JUSYNC_Receiver_GUI=ON` |

> Removed: OpenSSL (no longer needed after XXH3 migration)

---

## Building

### Prerequisites

**Linux (Jülich / Dev Machine):**
- GCC 13+ or Clang 18+
- CMake 3.22+
- ZeroMQ development packages (`libzmq3-dev` or `zeromq-devel`)
- CUDA toolkit *(optional, for GPU acceleration)*

**Windows:**
- Visual Studio 2022+
- CMake 3.22+
- ZeroMQ SDK
- CUDA toolkit *(optional)*

### Build

```bash
git clone https://github.com/FZJ-JSC/jusync.git
cd jusync
git submodule init && git submodule update

mkdir build && cd build

# Configure
cmake .. -DCMAKE_BUILD_TYPE=Release

# Build (shared library)
cmake --build . --config Release

# Build with GUI testing tool
cmake .. -DBUILD_JUSYNC_Receiver_GUI=ON

# Build with GPU acceleration
cmake .. -DBUILD_GPU=ON  # requires CUDA toolkit
```

Output: `libanari_usd_middleware.so` (Linux) / `anari_usd_middleware.dll` (Windows)

---

## HPC Pipeline

### Build ANARI-SDK on Jülich

The ANARI-SDK contains the broker (`ZmqBroker`) and DiffCapture. Build on Jüelah (not dev machine):

```bash
# On Jülich login node:
module load EasyBuild
module load GCC/13.3.0
source /path/to/your/anari-env.sh

git clone https://github.com/staticx-g7/ANARI-USD.git
cd ANARI-USD && git checkout diffcapture

mkdir build && cd build
cmake .. \
  -DCMAKE_BUILD_TYPE=Release \
  -DUSD_ROOT_DIR=$HOME/userinstallation_anari/easybuild/jurecadc/software/OpenUSD/24.11-GCCcore-13.3.0 \
  -DZMQ_INCLUDE_DIR=/path/to/zmq/include \
  -DZMQ_LIBRARY=/path/to/zmq/lib/libzmq.so

make -j$(nproc)
# Output: libanari_library_usd.so  (copy to JUSYNC ThirdParty path)
```

### DiffCapture Status Table

The broker on rank 0 prints a terminal status table every 10 seconds showing per-rank file updates:

```
+================================================================================+
| DIFF-CAPTURE STATUS TABLE                                          interval: 10s |
+================================================================================+
| ssh -N -L 5556:10.0.1.5:5556 -i ~/.ssh/... george2@jureca04.fz-juelich.de   |
| Ranks: 4  |  Active: 3  |  Updates (last 60s): 17                                   |
+--------------------------------------------------------------------------------+
| Rank   Updated         Category   File Updated                                    |
+--------------------------------------------------------------------------------+
| 0      4s ago   [green] geom       scene.usda                                      |
| 1     18s ago [yellow] texture    material_diff.param.usda                        │
| 2      8s ago   [green] geom       clips/prim_r2.usda                              │
| 3       ---           --         (no update yet)                                 |
+================================================================================+
```

**Controls:**
| Env Var | Effect |
|---|---|
| `DIFFCAPTURE_STATUS_INTERVAL=10` | Refresh interval in seconds (default: 10) |
| `DIFFCAPTURE_STATUS_INTERVAL=0` | Disable status table |

### Run Simulation

Start the simulation from Jüelah with the ANARI device, broker auto-starts on rank 0.

```bash
# On your laptop — SSH tunnel to broker:
ssh -N -L 5556:127.0.0.1:5556 -L 5555:127.0.0.1:5555 \
  -i ~/.ssh/ed_25519_universal_openssh george2@jureca04.fz-juelich.de

# On HPC — run your simulation (broker starts automatically on rank 0)
sbatch run_simulation.sh
```

---

## Middleware

### Overview

JUSYNC connects to the ANARI-USD broker as a **DEALER client**. It does NOT run as a server. The architecture:

1. **Connect** to broker at `tcp://host:5556`
2. **Subscribe** to push notifications (live updates)
3. **Query** file lists per rank with sizes and XXH3-128 hashes
4. **Download** files in parallel with RAM budget enforcement
5. **Parse** USD with TinyUSDZ → extract meshes, point clouds, textures
6. **Dispatch** via C callbacks to host application (UE5)

### Components

| Component | Role |
|---|---|
| `AnariUsdClient` | ZMQ DEALER client; dispatcher thread owns all recv() |
| `UsdProcessor` | TinyUSDZ wrapper; extracts meshes, point clouds, UVs |
| `ParallelDownloadManager` | Scheduler + worker pool; RAM-aware parallel streaming |
| `MemoryMonitor` | System RAM awareness; gates download concurrency |
| `CollisionProcessor` | Physics collision generation (5 complexity levels) |
| `HashVerifier` | XXH3-128 hash comparison (replaced OpenSSL SHA-256) |
| `GpuContext` | CUDA device detection; optional GPU-accelerated transforms |
| `AnariUsdMiddleware_C` | C FFI interface for Unreal Engine |

### Thread Model

| Thread | Component | Purpose |
|---|---|---|
| **Dispatcher** | `AnariUsdClient` | Single thread owning ALL ZMQ recv(); enqueues into queue |
| **Scheduler** | `ParallelDownloadManager` | Drains pending downloads, checks RAM budget, starts workers |
| **Workers** | `ParallelDownloadManager` | Pool of threads, request/chunk/accumulate per file |
| **GPU** | `GpuContext` (optional) | CUDA transforms for meshes ≥ 10K vertices |
| **Main** | Host application | Calls API, receives callbacks |

---

## Network Protocol

### Message Types

All messages start with magic `0x55534446` (`"USDF"`). Binary-packed structs over ZMQ multipart frames.

**Worker Registration:**
| Type | ID | Direction |
|---|---|---|
| `WORKER_READY` | 1 | Worker → Broker |
| `WORKER_HEARTBEAT` | 2 | Worker → Broker |
| `BROKER_ACK` | 10 | Broker → Worker |

**Worker Queries:**
| Type | ID | Purpose |
|---|---|---|
| `REQ_WORKER_COUNT` / `RESP_WORKER_COUNT` | 21 / 23 | Total connected workers |
| `REQ_WORKER_STATUS` / `RESP_WORKER_STATUS` | 20 / 22 | Per-worker status, hostname, GPU info |
| `REQ_WORKER_LIST` / `RESP_WORKER_LIST` | 24 / 25 | Full worker list |
| `REQ_GET_PROPERTY` / `RESP_PROPERTY` | 400 / 401 | Generic property queries |

**File Operations:**
| Type | ID | Purpose |
|---|---|---|
| `REQ_LIST_FILES` / `RESP_FILE_LIST` | 100 / 200 | List files per rank (JSON with hashes) |
| `REQ_GET_FILE` / `RESP_FILE_CHUNK` / `RESP_FILE_COMPLETE` | 101 / 201 / 202 | Chunked download (4MB chunks) |
| `RESP_NO_FILE` / `RESP_ERROR` | 203 / 204 | Error responses |

**Push Notifications:**
| Type | ID | Description |
|---|---|---|
| `NOTIFY_FILE_UPDATE` | 300 | File changed (old data not available) |
| `NOTIFY_COMMIT_COMPLETE` | 301 | Scene commit finished |
| `NOTIFY_FILE_UPDATE_V2` | 302 | File changed **with hashPrev128 + hasOldData** |

### Wire Format

**File Request (`REQ_GET_FILE`, 276 bytes):**
```
Offset  Size  Field
0       4     magic (0x55534446)
4       4     message_type (101)
8       4     request_id (monotonic, client-side)
12      4     target_rank (-1 = broadcast all)
16      256   filename (null-terminated UTF-8)
272     4     chunk_size (0 = default 4MB)
```

**Notification (`ZmqFileNotification`, ~384 bytes):**
```
Offset  Size  Field
0       4     magic (0x55534446)
4       4     message_type (300/301/302)
8       4     source_rank
12      256   filename
268     8     file_size
276     8     timestamp (Unix epoch seconds)
284     16    hash128[2] (XXH3-128 of new data)
300     16    hashPrev128[2] (XXH3-128 of old data, 0 if first)
316     1     hasOldData (true if hashPrev128 is valid)
```

### Protocol Flows

**File Download:**
```
Client ──[REQ_GET_FILE]──▶ Broker ──forward──▶ Worker
Client ◀──[RESP_FILE_CHUNK]── Broker ◀──data── Worker  (repeated, 4MB each)
Client ◀──[RESP_FILE_COMPLETE]── Broker
```

**Live Notification (V2):**
```
Worker ──[NOTIFY_FILE_UPDATE_V2]──▶ Broker ──forward──▶ Client
                                                    └──▶ Client (multi-client)
```

---

## Unreal Engine 5 Plugin

### Setup

1. Build middleware → copy `.so` to:
   ```
   jusync-uesample/Plugins/JUSYNC/ThirdParty/AnariUsdMiddleware/Lib/Linux/
   jusync-uesample/Plugins/JUSYNC/Source/ThirdParty/AnariUsdMiddleware/Lib/Linux/
   ```
2. Build ANARI-SDK → copy `.so` to same path
3. Open `jusync-uesample.uproject` in UE5 editor
4. Ensure JUSYNC plugin is enabled in `Edit → Plugins`
5. Connect broker in Blueprint or C++:
   ```cpp
   UJUSYNCSubsystem::ConnectToBroker("tcp://127.0.0.1:5556");
   ```

### Blueprint Integration

**Async Action Nodes:**
| Node | Inputs | Outputs |
|---|---|---|
| `AsyncLoadUSD` | Buffer or file path | MeshData[], PointCloudData[] |
| `AsyncReceiveFiles` | Timer interval | FileData[] (when files arrive) |
| `AsyncCreateTexture` | Buffer data | UTexture2D |
| `AsyncCreateMesh` | MeshData | URealtimeMeshComponent |

**Subsystem Delegates:**
| Delegate | Fires When |
|---|---|
| `OnFileReceived` | Chunked file download complete |
| `OnNotificationReceived` | Broker push notification (V2-aware) |
| `OnMessageReceived` | Text message from broker |

### Live Updates

The `JUSYNCFileSpawnerActor` subscribes to `OnNotificationReceived`:

```
NotificationCallback_Static (C, middleware thread)
  → AsyncTask(GameThread)
  → FJUSYNCNotification (with HashLo, HashHi, HashPrevLo, HashPrevHi, bHasOldData)
  → OnNotificationReceived.Broadcast()
  → AJUSYNCFileSpawnerActor::OnBrokerNotification()
  → if hashPrev != storedHash → download → spawn new → destroy old
  → if hashPrev == storedHash → SKIP (no flicker, no bandwidth)
```

> The hash-gated decision logic compares `Notification.HashPrevLo/Hi` against stored `FileHashLo[Filename]`. Match = skip download completely.

---

## API Reference

### C++ Public API (`AnariUsdMiddleware`)

```cpp
// Lifecycle
middleware->initialize();
middleware->connectToBroker("tcp://host:5556");
middleware->disconnectFromBroker();

// File list (returns FileInfo[] with sizes, hashes)
auto files = middleware->requestFileList(rank, timeoutMs);

// Parallel download (RAM-aware, immediate callbacks)
middleware->requestFilesParallelAsync(filePaths, spawnCallback, errorCallback);

// Worker queries
int count = middleware->requestWorkerCount(timeoutMs);
auto status = middleware->requestWorkerStatus(rank, timeoutMs);

// USD processing
auto meshes = middleware->loadUSDBuffer(data, size);
auto meshes = middleware->loadUSDFromDisk("model.usda");

// Collision generation
middleware->processCollisionMesh(meshData, ECollisionComplexity::Complex);

// Notifications
middleware->setNotificationCallback([&](uint32_t type, int32_t rank,
  const std::string& file, uint64_t size, uint64_t ts,
  uint64_t hashLo, uint64_t hashHi,
  uint64_t hashPrevLo, uint64_t hashPrevHi, bool hasOldData) {
    // Handle live update
});
```

### C FFI (`AnariUsdMiddleware_C.h`)

All functions suffixed `_C` for UE compatibility. Key functions:

```c
// Lifecycle
AnariUsdMiddlewareHandle_t InitializeMiddleware_C(void);
void ConnectToBroker_C(handle, const char* endpoint);

// File operations
FileHandle_t RequestFilesParallelAsync_C(handle, filePaths[], count,
  FileReceivedCallback_C callback, BrokerErrorCallback_C errorCb);

// USD processing
MeshHandle_t LoadUSDBuffer_C(handle, const uint8_t* data, size_t size);
MeshData_t* GetMeshData_C(handle, count);
PointCloudData_t* ProcessPointCloudFromUSD_C(handle, const uint8_t* data, size_t size);

// Collision
void ProcessCollisionMesh_C(handle, CMeshData* mesh, ECollisionComplexity complexity);

// Notifications
void RegisterNotificationCallback_C(NotificationCallback_C callback);

// Cleanup
void FreeMeshData_C(meshData, count);
void FreePointCloudData_C(pcData, count);
void ShutdownMiddleware_C(handle);
```

### C Structs

**`CFileData`**: filename, data[], data_size, hash, file_type
**`CMeshData`**: element_name, type_name, points[], points_count, indices[], indices_count, normals[], uvs[], vertex_colors[], vertex_colors_count, collision[], collision_count, collision_type, collision_complexity, subdivision_scheme
**`CPointCloudData`**: positions[], positions_count, widths[], widths_count, scalar_attributes[], scalar_attributes_count, colors[], colors_count
**`CTextureData`**: data[], data_size, width, height, channels

---

## GUI Testing Application

Dear ImGui-based testing tool at `tools/ReceiverUI`:

```bash
cmake .. -DBUILD_JUSYNC_Receiver_GUI=ON
cmake --build . --config Release
./tools/ReceiverUI/ReceiverUI
```

**Features:**
- Real-time connection status and ZeroMQ state
- File reception monitoring (size, hash, type)
- 3D model viewer for loaded USD
- Mesh data inspector (vertices, normals, UVs, colors)
- Texture preview and gradient extraction
- Performance metrics (processing times, memory)
- Point cloud visualization with colormap

---

## USD Processing Capabilities

- **Geometry Extraction**: Full prim hierarchy walk, mesh points, indices, normals, UVs, vertex colors
- **Reference Resolution**: Automatic resolution of USD references, payloads, and clips
- **Triangulation**: Polygonal faces → triangles
- **Transform Application**: World-space transform matrices applied to points and normals
- **UV Primvar Search**: Multiple primvar name patterns for UV coordinates
- **Coord Space**: USD Z-up right-handed → UE Z-up left-handed (Y-axis flip)
- **Format Support**: .usd, .usda, .usdc, .usdz (via TinyUSDZ composition)
- **Point Cloud**: GeomPoints extraction with width, scalar attributes, gradient colormap color baking
- **GPU Acceleration**: CUDA transforms for meshes ≥ 10K vertices (positions, normals, UVs)
- **Collision**: 5 levels — None / Simple (AABB) / ConvexHull / Complex (full) / Simplified (decimated)
- **Vertex Color Interpolation**: Smooth blending from uniform face colors to vertex colors

---

## Troubleshooting

| Problem | Cause | Fix |
|---|---|---|
| **Broker not reachable** | SSH tunnel not running | `ssh -N -L 5556:localhost:5556 ...` |
| **Hash mismatch on download** | stale .usd on disk / network drop | Re-request file; XXH3 logs both hashes |
| **UE plugin can't find .so** | Middleware not copied to ThirdParty path | Copy `libanari_usd_middleware.so` to `Lib/Linux/` |
| **Status table not printing** | Interval = 0 or env var disabled | Set `DIFFCAPTURE_STATUS_INTERVAL=10` |
| **CUDA fallback to CPU** | No CUDA device or build without GPU | `cmake .. -DBUILD_GPU=ON` + CUDA toolkit |
| **Point cloud empty** | No GeomPoints prims in USD | Check source simulation exports point cloud data |
| **Dead ranks in status** | Worker crashed or network partition | Check HPC job status, restart worker |
| **V2 notifications ignored** | UE handler only checks type 300 | Update `OnBrokerNotification` to handle type 302 |

**Enable verbose logging:**
```bash
# Middleware logging level (0=quiet, 3=verbose)
export MIDDLEWARE_LOG_LEVEL=3

# DiffCapture status table off
export DIFFCAPTURE_STATUS_INTERVAL=0
```
