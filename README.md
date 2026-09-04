# JUSYNC

A high-performance C++ middleware that streams USD geometry, textures, and point-cloud data from **HPC simulation** (ANARI-SDK on supercomputers) to **Unreal Engine 5** in real time.

JUSYNC is built around a ZeroMQ DEALER/ROUTER pipeline, RAM-aware bounded parallel downloads, XXH3-128 change detection, zstd-compressed USD payloads, zero-copy / in-situ USD parsing, compact UE-facing fill paths, and live diff-aware notifications.

```
HPC Workers (ANARI ranks)  →  ZMQ Broker (rank 0)  →  JUSYNC Middleware (.so)  →  UE5 Plugin (RealtimeMesh / LiDAR)
```

We gratefully acknowledge the Bundesministerium für Forschung, Technologie und Raumfahrt (BMFTR) and Ministerium für Kultur und Wissenschaft des Landes Nordrhein-Westfalen (MWK-NRW) for funding this work in the project InHPC-DE through the Gauss Centre for Supercomputing e.V. (www.gauss-centre.eu).

---

## Table of Contents

- [Pipeline Architecture](#pipeline-architecture)
- [Feature Matrix](#feature-matrix)
- [Repository Layout](#repository-layout)
- [Dependencies](#dependencies)
- [Building](#building)
- [CI/CD and Testing](#cicd-and-testing)
- [HPC Pipeline](#hpc-pipeline)
  - [Build ANARI-SDK on Jülich](#build-anari-sdk-on-julich)
  - [DiffCapture Status Table](#diffcapture-status-table)
  - [Run Simulation](#run-simulation)
- [Middleware](#middleware)
  - [Overview](#overview)
  - [Components](#components)
  - [Thread Model](#thread-model)
- [Network Protocol](#network-protocol)
  - [Protocol Version](#protocol-version)
  - [Message Types](#message-types)
  - [Wire Format](#wire-format)
  - [Protocol Flows](#protocol-flows)
- [Unreal Engine 5 Plugin](#unreal-engine-5-plugin)
  - [Setup](#setup)
  - [Spawner Actor Features](#spawner-actor-features)
  - [Materials and Colors](#materials-and-colors)
  - [Blueprint Integration](#blueprint-integration)
  - [Live Updates](#live-updates)
- [Performance / Memory Model](#performance--memory-model)
- [API Reference](#api-reference)
  - [C++ Public API](#c-public-api-anariusdmiddleware)
  - [C FFI API](#c-ffi-anariusdmiddleware_ch)
  - [In-Situ / Zero-Copy Parse API](#in-situ--zero-copy-parse-api)
  - [C Structs](#c-structs)
- [GUI Testing Application](#gui-testing-application)
- [USD Processing Capabilities](#usd-processing-capabilities)
- [Troubleshooting](#troubleshooting)

---

## Pipeline Architecture

```
┌─────────────────────────────────────────────────────────────────────────────────────┐
│ HPC (Supercomputer - Jülich / JURECA)                                               │
│                                                                                     │
│  Rank 0 (Broker)         Rank 1...N (Workers)                                       │
│  ┌──────────────┐        ┌──────────────────┐                                       │
│  │ ZmqBroker    │ ◄────── │ UsdDevice::      │ ANARI SDK writes USD to               │
│  │ (ROUTER)     │  DEALER │  flushCommit()   │ memory store per commit               │
│  │ - fwd notifs │        │ - TrackStageMem() │                                      │
│  │ - route reqs │        │ - DiffCapture     │ Captures old state before overwrite   │
│  │ - status tbl │        │ - StoreFile()     │ New state + hash128                   │
│  └──────┬───────┘        └──────────────────┘                                       │
│         │ ZMQ (InfiniBand or localhost)                                              │
└─────────┼───────────────────────────────────────────────────────────────────────────┘
          │ NOTIFY_FILE_UPDATE_V2 (hashPrev128, hasOldData)
          │ NOTIFY_SCENE_UPDATE / NOTIFY_PROPERTY_UPDATE
          │ RESP_FILE_CHUNK (4 MB chunks, optionally zstd-compressed USD payload)
┌─────────▼───────────────────────────────────────────────────────────────────────────┐
│ Middleware / Laptop / UE Host                                                       │
│                                                                                     │
│  ┌─────────────────────────────────────────────────────────────────────┐            │
│  │  AnariUsdClient (DEALER)                                           │            │
│  │  ┌──────────────────┐  ┌──────────────────┐  ┌──────────────────┐  │            │
│  │  │ Dispatcher Thread│  │ ParallelDownload │  │ UsdProcessor     │  │            │
│  │  │ (single recv)    │→ │ Manager + RAM    │  │ (TinyUSDZ parse) │  │            │
│  │  │ → per-id queue   │  │  Monitor         │  │ zstd / zero-copy │  │            │
│  │  └──────────────────┘  └──────────────────┘  │ → mesh / PC /    │  │            │
│  │                                               │   compact fill   │  │            │
│  │                                               └──────────────────┘  │            │
│  └─────────────┬────────────────────────────────────┬─────────────────┘            │
│                │ C API callbacks                     │ UE Plugin                     │
│                ▼                                     ▼                               │
│        CMeshData / CPointCloudData         JUSYNCSubsystem                          │
│        / compact in-situ fill buffers        → AsyncTask(GameThread)                 │
│                                              → Broadcast to Blueprint                │
│                                              → SpawnRealtimeMesh / PointCloud        │
└─────────────────────────────────────────────────────────────────────────────────────┘
```

### Data Flow (Live Update)

```
1. HPC rank updates geometry → TrackStageMemory() exports .usda
2. DiffCapture captures old FileEntry before StoreFile() overwrites it
3. SendFileNotificationV2() → broker → middleware
   (includes hash128 of NEW data, hashPrev128 of OLD data, hasOldData)
4. Middleware dispatcher thread receives message and routes by request/notification type
5. UE JUSYNCSubsystem marshals to game thread → OnNotificationReceived broadcast
6. JUSYNCFileSpawnerActor compares hashPrev vs stored hash:
   - Same hash → SKIP (no download, no destroy)
   - Different hash → download → parse → spawn/update new → destroy old
```

---

## Feature Matrix

### Available Features

- [x] **Real-time USD streaming**: Receive and process USD files (`.usd`, `.usda`, `.usdc`, `.usdz`) over ZeroMQ
- [x] **Live update notifications**: `NOTIFY_FILE_UPDATE`, `NOTIFY_COMMIT_COMPLETE`, `NOTIFY_FILE_UPDATE_V2`
- [x] **Scene / property update notifications**: `NOTIFY_SCENE_UPDATE` and `NOTIFY_PROPERTY_UPDATE`
- [x] **Diff-aware streaming (V2)**: `hashPrev128` + `hasOldData` enables skip-if-unchanged logic in UE
- [x] **XXH3-128 hashing**: Fast non-cryptographic change detection and file-list hashing
- [x] **zstd-compressed USD payloads**: Broker-compressed payloads are detected by magic bytes and decompressed automatically
- [x] **Zero-copy / in-place USD parsing**: Clean `.usda` payloads can be parsed from the caller buffer without an intermediate copy
- [x] **In-situ two-phase parse API**: Query layout first, then fill caller-owned UE-friendly buffers
- [x] **Compact mesh fill path**: 32-bit float positions/normals/UVs for UE compact meshes
- [x] **Direct LiDAR point fill**: Optional packed `CPointCloudLidarPoint_v1` fill path matching UE `FLidarPointCloudPoint`
- [x] **Bounded parallel downloads**: RAM-aware worker pool with pipeline-depth and bandwidth controls
- [x] **Single-recv dispatcher**: One dispatcher thread owns ZMQ `recv()` and routes responses by request ID
- [x] **Advanced mesh processing**: Vertices, indices, normals, UVs, vertex colors, subdivision scheme, double-sided flag, multiple UV sets
- [x] **Point cloud / LiDAR**: Extract `GeomPoints`, widths, scalar attributes, colors, normals, and bounding boxes
- [x] **USD reference resolution**: Automatically resolves references, payloads, and clips
- [x] **GPU acceleration**: Optional CUDA vertex/normal/UV transforms for large meshes
- [x] **Collision generation**: None, Simple (AABB), ConvexHull, Complex (full), Simplified (decimated), ConvexDecomp
- [x] **Cross-platform**: Windows (`.dll`) and Linux (`.so`)
- [x] **Thread-safe client**: Dispatcher thread owns all ZMQ `recv()`; callbacks are dispatched to consumer threads
- [x] **Unreal Engine 5 plugin**: Full plugin with Blueprints, async actions, RealtimeMesh, LiDAR point clouds, live updates, mesh cache, and time-step animation
- [x] **DiffCapture (HPC broker)**: Terminal status table with per-rank update tracking
- [x] **Worker discovery**: Query worker count, status, hostname, GPU info via ZMQ
- [x] **Memory safety**: RAII patterns, pointer validation, bounds checking, atomic stats
- [x] **Duplicate prevention**: Filename+rank duplicate detection and processing prevention
- [x] **GUI testing tool**: Dear ImGui-based application at `tools/ReceiverUI`
- [x] **CTest + GitHub Actions CI**: Separate CMake/CTest checks for build, USD, collision, error, performance, integration, and per-file tests

### Partial / Host-Side Features

- [x] **UE material path**: The UE plugin can apply assigned materials, vertex colors, LUT textures, and sender textures
- [ ] **Full USD material extraction**: Shader graphs, material networks, and USD-authored texture bindings are not fully extracted by the middleware
- [ ] **USD light extraction**: Not implemented
- [ ] **USD camera extraction**: Not implemented
- [ ] **USD volume rendering**: Not implemented
- [ ] **USD instancing / prototypes**: Not fully supported
- [ ] **USD time sampling**: Not processed inside a single USD file. The UE plugin supports filename-based time-step playback separately

---

## Repository Layout

```
jusync/
├── CMakeLists.txt
├── README.md
├── include/                  Public C/C++ middleware API
├── src/                      Middleware implementation
├── tests/
│   ├── src/
│   │   └── usd_validation_tool.cpp
│   ├── data/
│   │   └── usd_samples/
│   └── scripts/
├── tools/
│   └── ReceiverUI/           Optional Dear ImGui testing app
├── external/                 Git submodules and bundled dependencies
├── UnrealPlugin/
│   └── JUSYNC/               Plugin metadata / sync helper location
└── .github/
    ├── actions/
    │   └── run-ctest/        Reusable CTest job action
    └── workflows/
        └── test.yml          GitHub Actions CI
```

The full Unreal Engine sample project lives in the companion repository:

```
jusync-uesample/
```

The middleware repository produces the shared library and headers consumed by the UE plugin.

---

## Dependencies

| Dependency | Purpose | Notes |
|---|---|---|
| **ZeroMQ** | High-performance messaging | Bundled and built statically from `external/zmq` |
| **cppzmq** | C++ ZMQ bindings | Bundled submodule |
| **OpenSSL** | Required by ZMQ / secure transport support | Install `libssl-dev` on Linux or set `OPENSSL_ROOT_DIR` |
| **TinyUSDZ** | USD parsing | Embedded submodule |
| **GLM** | 3D math | Embedded submodule |
| **STB** | Image processing | Embedded submodule |
| **xxhash** | XXH3-128 hashing | Bundled source |
| **zstd** | Decompress broker-compressed USD payloads | Optional at runtime, enabled by `ENABLE_ZSTD=ON` |
| **TBB** | Optional parallel processing acceleration | Used if found by CMake; sequential fallback if absent |
| **CUDA** | Optional GPU acceleration | Vertex/normal/UV transforms for large meshes |
| **Dear ImGui** | Optional GUI testing tool | `BUILD_JUSYNC_Receiver_GUI=ON` |

> Note: OpenSSL is still required by the build. XXH3 replaced SHA-256 for fast change detection, but it did not remove the OpenSSL dependency.

---

## Building

### Prerequisites

**Linux:**

- GCC 11+ (GCC 13/14 tested; GCC 14+ tinyusdz warnings are handled automatically)
- CMake 3.16+ (3.22+ recommended)
- Ninja *(recommended)*
- `pkg-config`
- `libssl-dev`
- `libzstd-dev` *(recommended for broker-compressed payloads)*
- CUDA toolkit *(optional, for GPU acceleration)*

**Windows:**

- Visual Studio 2022 or newer
- CMake 3.16+
- OpenSSL / vcpkg / prebuilt OpenSSL path
- CUDA toolkit *(optional)*

### Linux Build

```bash
git clone https://github.com/FZJ-JSC/jusync.git
cd jusync
git submodule update --init --recursive

cmake -S . -B build -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_TESTS=ON \
  -DENABLE_CUDA=OFF \
  -DENABLE_ZSTD=ON \
  -DCMAKE_EXPORT_COMPILE_COMMANDS=ON

cmake --build build --parallel

ctest --test-dir build --output-on-failure
```

Output:

```
build/libanari_usd_middleware.so
build/usd_validation_tool
```

### Linux Build with CUDA

```bash
cmake -S . -B build -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DENABLE_CUDA=ON \
  -DCUDA_ARCHITECTURES=89 \
  -DBUILD_TESTS=ON

cmake --build build --parallel
```

> CUDA 13 dropped older architectures such as `sm_60`, `sm_61`, and `sm_70`. Pass an architecture list valid for your GPU, e.g. `-DCUDA_ARCHITECTURES=89`.

### Windows Build

```bat
git clone https://github.com/FZJ-JSC/jusync.git
cd jusync
git submodule update --init --recursive

cmake -S . -B build -G "Visual Studio 17 2022" -A x64 ^
  -DCMAKE_BUILD_TYPE=Release ^
  -DBUILD_TESTS=ON ^
  -DENABLE_CUDA=OFF

cmake --build build --config Release
```

### CMake Options

| Option | Default | Description |
|---|---:|---|
| `CMAKE_BUILD_TYPE` | `Release` | Build type for single-config generators |
| `ENABLE_CUDA` | `OFF` | Enable CUDA GPU acceleration |
| `CUDA_ARCHITECTURES` | `60;61;70;75;80;86` | CUDA architectures. Override for CUDA 13 / newer GPUs |
| `ENABLE_ZSTD` | `ON` | Enable zstd decompression of broker USD payloads |
| `BUILD_TESTS` | `ON` | Build `usd_validation_tool` and register CTest tests |
| `BUILD_JUSYNC_Receiver_GUI` | `OFF` | Build optional Dear ImGui receiver GUI |
| `OPENSSL_ROOT_DIR` | auto | Explicit OpenSSL root if `find_package(OpenSSL)` fails |
| `CMAKE_EXPORT_COMPILE_COMMANDS` | `OFF` | Export `compile_commands.json` for clang-tidy / clangd |

---

## CI/CD and Testing

GitHub Actions is defined in:

```
.github/workflows/test.yml
```

It uses a native CMake/CTest pipeline. It does **not** build or upload a Docker image artifact.

### CI Jobs

| Job | Purpose |
|---|---|
| `Build & Smoke Tests` | Configure, build, run `build_verification` + `binary_static_check`, upload test payload |
| `Static Analysis` | Run clang-tidy against `compile_commands.json`; currently a non-blocking report |
| `Test USD Basic` | Run `usd_basic_processing` |
| `Test Collision Simple` | Run `collision_simple` |
| `Test Collision Complex` | Run `collision_complex` |
| `Test Collision Convex` | Run `collision_convex` |
| `Test Error Handling` | Run `error_handling_tests` |
| `Test Performance` | Run `performance_benchmarks` |
| `Test Integration` | Run `integration_suite` |
| `Test USD Files` | Run per-file `usd_file_*` tests |

The workflow triggers on:

```yaml
on:
  push:
  pull_request:
  workflow_dispatch:
```

So it runs on every push, every pull request, and manually from the Actions tab.

### CTest Labels

The CMake build registers labeled tests:

| Label | Tests |
|---|---|
| `build` | `build_verification`, `binary_static_check` |
| `basic` | `usd_basic_processing` |
| `collision` | `collision_simple`, `collision_complex`, `collision_convex` |
| `error` | `error_handling_tests` |
| `performance` | `performance_benchmarks` |
| `integration` | `integration_suite` |
| `file` | `usd_file_*` per-USD-sample tests |

Run a group locally:

```bash
ctest --test-dir build -L collision --output-on-failure
ctest --test-dir build -L file --output-on-failure
ctest --test-dir build --output-on-failure
```

### Validation Tool

The test executable is:

```
build/usd_validation_tool
```

Useful modes:

```bash
./build/usd_validation_tool --mode=static-check
./build/usd_validation_tool --mode=usd-basic
./build/usd_validation_tool --mode=collision --type=simple
./build/usd_validation_tool --mode=collision --type=complex
./build/usd_validation_tool --mode=collision --type=convex
./build/usd_validation_tool --mode=error-handling
./build/usd_validation_tool --mode=performance
./build/usd_validation_tool --mode=integration
```

Single-file mode:

```bash
./build/usd_validation_tool --mode=usd-basic tests/data/usd_samples/simpleCubeVertex.usda
```

### CI Failure Feedback

If a CI check fails, GitHub shows the failing job separately. Each CTest job uploads a log artifact such as:

```
ctest-usd-basic
ctest-collision-simple
ctest-usd-files
clang-tidy
```

The CTest output uses `--output-on-failure`, so the failing test command and output appear in the job log.

---

## HPC Pipeline

### Build ANARI-SDK on Jülich

The ANARI-SDK contains the broker (`ZmqBroker`) and DiffCapture. Build on Jülich (not the dev machine):

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
# Output: libanari_library_usd.so  (copy to JUSYNC ThirdParty path if required)
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
| 1     18s ago [yellow] texture    material_diff.param.usda                         |
| 2      8s ago   [green] geom       clips/prim_r2.usda                              |
| 3       ---           --         (no update yet)                                   |
+================================================================================+
```

**Controls:**

| Env Var | Effect |
|---|---|
| `DIFFCAPTURE_STATUS_INTERVAL=10` | Refresh interval in seconds (default: 10) |
| `DIFFCAPTURE_STATUS_INTERVAL=0` | Disable status table |

### Run Simulation

Start the simulation from Jülich with the ANARI device. The broker auto-starts on rank 0.

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

JUSYNC connects to the ANARI-USD broker as a **DEALER client**. It does not run as a server.

The main path is:

1. **Connect** to broker at `tcp://host:5556`
2. **Subscribe** to push notifications for live updates
3. **Query** file lists per rank with sizes, ranks, and XXH3-128 hashes
4. **Download** files in parallel with RAM and pipeline-depth budgeting
5. **Decode** zstd-compressed USD payloads when present
6. **Parse** USD with TinyUSDZ, preferably zero-copy / in-place
7. **Fill** caller-owned buffers via legacy, UE-double, compact-float, or direct LiDAR paths
8. **Dispatch** results through C/C++ callbacks to the host application (UE5)

### Components

| Component | Role |
|---|---|
| `AnariUsdClient` | ZMQ DEALER client; single dispatcher thread owns all `recv()` |
| `UsdProcessor` | TinyUSDZ wrapper; zstd detection, zero-copy parse, mesh/point-cloud extraction |
| `ParallelDownloadManager` | Scheduler + worker pool; bounded, RAM-aware parallel streaming |
| `MemoryMonitor` | System RAM awareness; gates download concurrency |
| `CollisionProcessor` | Physics collision generation with multiple complexity levels |
| `HashVerifier` | XXH3-128 hash verification |
| `GpuContext` | Optional CUDA device detection and GPU-accelerated transforms |
| `AnariUsdMiddleware_C` | C FFI interface for Unreal Engine and other hosts |

### Thread Model

| Thread | Component | Purpose |
|---|---|---|
| **Dispatcher** | `AnariUsdClient` | Single thread owning all ZMQ `recv()`; routes by request ID / notification type |
| **Scheduler** | `ParallelDownloadManager` | Drains pending downloads, checks RAM/pipeline budget, starts workers |
| **Workers** | `ParallelDownloadManager` | Request/chunk/accumulate per file |
| **Parse** | `UsdProcessor` / host | USD parsing can occur on worker or host thread depending on integration |
| **GPU** | `GpuContext` *(optional)* | CUDA transforms for large meshes |
| **Main** | Host application | Calls API, receives callbacks, updates UI / UE |

---

## Network Protocol

### Protocol Version

The protocol is defined in:

```
include/AnariUsdProtocol.h
```

Current values:

```
protocol version = 3
min supported protocol version = 1
default chunk size = 4 MB
magic = 0x55534446 ("USDF")
```

Unknown future message types should be ignored safely by consumers.

### Message Types

All messages use packed binary structs over ZMQ multipart frames.

**Worker registration:**

| Type | ID | Direction |
|---|---:|---|
| `WORKER_READY` | 1 | Worker → Broker |
| `WORKER_HEARTBEAT` | 2 | Worker → Broker |
| `BROKER_ACK` | 10 | Broker → Worker |

**Worker queries:**

| Type | ID | Purpose |
|---|---:|---|
| `REQ_WORKER_STATUS` / `RESP_WORKER_STATUS` | 20 / 22 | Per-worker status, hostname, GPU info |
| `REQ_WORKER_COUNT` / `RESP_WORKER_COUNT` | 21 / 23 | Connected worker count |
| `REQ_WORKER_LIST` / `RESP_WORKER_LIST` | 24 / 25 | Full worker list |
| `REQ_GET_PROPERTY` / `RESP_PROPERTY` | 400 / 401 | Generic property query |

**File operations:**

| Type | ID | Purpose |
|---|---:|---|
| `REQ_LIST_FILES` / `RESP_FILE_LIST` | 100 / 200 | List files per rank (JSON with sizes/hashes/ranks) |
| `REQ_GET_FILE` / `RESP_FILE_CHUNK` / `RESP_FILE_COMPLETE` | 101 / 201 / 202 | Chunked download |
| `REQ_GET_FRAME` | 102 | Frame-based file request |
| `RESP_NO_FILE` / `RESP_ERROR` | 203 / 204 | Error responses |

**Push notifications:**

| Type | ID | Description |
|---|---:|---|
| `NOTIFY_FILE_UPDATE` | 300 | File changed (legacy, old data not available) |
| `NOTIFY_COMMIT_COMPLETE` | 301 | Scene commit finished |
| `NOTIFY_FILE_UPDATE_V2` | 302 | File changed with `hashPrev128` + `hasOldData` |
| `NOTIFY_SCENE_UPDATE` | 303 | Typed scene update |
| `NOTIFY_PROPERTY_UPDATE` | 304 | Typed property update |

**Scene snapshot:**

| Type | ID | Purpose |
|---|---:|---|
| `REQ_SCENE_SNAPSHOT` / `RESP_SCENE_SNAPSHOT` | 500 / 501 | Reserved / scene snapshot transport |

### Wire Format

**File Request (`REQ_GET_FILE`, 276 bytes):**

```
Offset  Size  Field
0       4     magic (0x55534446)
4       4     message_type (101)
8       4     request_id (monotonic, client-side)
12      4     target_rank (-1 = broadcast all)
16      256   filename (null-terminated UTF-8)
272     4     chunk_size (0 = default 4 MB)
```

**File Chunk (`RESP_FILE_CHUNK`, 292 bytes):**

```
Offset  Size  Field
0       4     magic (0x55534446)
4       4     message_type (201)
8       4     request_id
12      4     source_rank
16      256   filename (null-terminated UTF-8)
272     8     file_size
280     8     chunk_offset
288     4     chunk_size
```

**File Notification (`ZmqFileNotification`, 317 bytes):**

```
Offset  Size  Field
0       4     magic (0x55534446)
4       4     message_type (300 / 301 / 302)
8       4     source_rank
12      256   filename (null-terminated UTF-8)
268     8     file_size
276     8     timestamp (Unix epoch seconds)
284     16    hash128[2] (XXH3-128 of new data)
300     16    hashPrev128[2] (XXH3-128 of old data, 0 if first)
316     1     hasOldData (true if hashPrev128 is valid)
```

**Scene Update (`ZmqSceneUpdate`, 528 bytes):**

Used by `NOTIFY_SCENE_UPDATE` and `NOTIFY_PROPERTY_UPDATE`:

```
magic
message_type
source_rank
timestamp
commit_id
revision
prim_path[256]
property_name[64]
change_type
value_type
int_value
float_value
vec4[4]
string_value[128]
payload_size
reserved
```

### Protocol Flows

**File download:**

```
Client ──[REQ_GET_FILE]──▶ Broker ──forward──▶ Worker
Client ◀──[RESP_FILE_CHUNK]── Broker ◀──data── Worker  (repeated, 4 MB each)
Client ◀──[RESP_FILE_COMPLETE]── Broker
```

**Live notification (V2):**

```
Worker ──[NOTIFY_FILE_UPDATE_V2]──▶ Broker ──forward──▶ Client
                                                     └──▶ Client (multi-client)
```

**Scene / property update:**

```
Worker ──[NOTIFY_SCENE_UPDATE / NOTIFY_PROPERTY_UPDATE]──▶ Broker ──forward──▶ Client
```

---

## Unreal Engine 5 Plugin

The UE5 plugin consumes the middleware shared library and headers. The sample project is in:

```
jusync-uesample
```

### Setup

1. Build the middleware:

   ```bash
   cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTS=ON
   cmake --build build --parallel
   ```

2. Copy the Linux shared library:

   ```bash
   cp build/libanari_usd_middleware.so \
     ../jusync-uesample/Plugins/JUSYNC/Source/ThirdParty/AnariUsdMiddleware/Lib/Linux/
   ```

3. Copy public headers:

   ```bash
   cp include/AnariUsdMiddleware.h \
      include/AnariUsdMiddleware_C.h \
      include/AnariUsdClient.h \
      include/AnariUsdMessages.h \
      include/AnariUsdProtocol.h \
      include/UsdProcessor.h \
      include/ParallelDownloadManager.h \
      include/ParallelDownloader.h \
      include/MemoryMonitor.h \
      include/CollisionProcessor.h \
      include/HashVerifier.h \
      include/GpuContext.h \
      include/GpuKernels.h \
      include/GpuMemory.h \
      include/GpuValidation.h \
      include/MiddlewareLogging.h \
      include/ZmqConnector.h \
      ../jusync-uesample/Plugins/JUSYNC/Source/ThirdParty/AnariUsdMiddleware/Include/
   ```

4. Open the sample project in Unreal Engine 5.

5. Ensure the JUSYNC plugin is enabled in `Edit → Plugins`.

6. Connect to the broker from Blueprint or C++:

   ```cpp
   UJUSYNCSubsystem* Subsystem = MyGameInstance->GetSubsystem<UJUSYNCSubsystem>();
   Subsystem->InitializeMiddleware(TEXT("tcp://127.0.0.1:5556"));
   ```

For Windows, place the DLL/import library under:

```
jusync-uesample/Plugins/JUSYNC/Source/ThirdParty/AnariUsdMiddleware/Lib/Win64/
```

and copy the required runtime DLLs (OpenSSL/ZMQ) alongside it if the middleware was built against them.

### Spawner Actor Features

The main UE actor is:

```
AJUSYNCFileSpawnerActor
```

It handles:

- Broker connection and file-list fetching
- USD/clip filtering
- Parallel download pipeline
- Mesh and point-cloud spawning
- Live updates
- Mesh cache
- Point-cloud actor pooling
- Material assignment
- Time-step animation
- Spawn placement grid / target actor
- Performance diagnostics

Key categories in the Details panel:

| Category | Examples |
|---|---|
| Connection | `BrokerEndpoint`, `RequestTimeoutMs`, `BandwidthBytesPerSecond` |
| Filters | `MinimumFileSizeBytes`, `bFilterUSDOnly`, `bClipsOnly` |
| Placement | `SpawnTargetActor`, `BaseSpawnLocation`, `SpawnSpacing`, `SpawnGridColumns`, `SpawnScale` |
| Material | `SpawnMaterial`, `TextureSampleParameterName`, `bUseGradientColors` |
| Live Update | `bEnableLiveUpdates`, `LiveUpdatePollInterval`, `bAutoRefreshMeshes`, `CommitCompleteCooldownSeconds` |
| Pipeline | `PipelineDepth`, `MaxSpawnsPerFrame`, `bEnablePerfLogging` |
| Point Cloud | `PointCloudSize`, `PointShape`, `PointOrientation`, `PointScaling`, `PointSizeBias`, `GapFillingStrength`, `PointCloudPoolSize`, normals options |
| Cache | `bEnableMeshCache`, `MeshCacheMaxEntries`, `MeshCacheMaxMB`, `bCachePointClouds` |
| Animation | `bEnableTimeStepAnimation`, `TimeStepPlaybackFPS`, `bLoopTimeStepAnimation` |

### Materials and Colors

The middleware extracts geometry and optional vertex colors / point-cloud colors. Material interpretation is mostly host-side.

The UE plugin supports:

- **Actor-assigned spawn material wins**
  - If `SpawnMaterial` contains a vertex-color expression, the plugin uses the vertex-color path and keeps `SpawnMaterial` assigned.
  - If `SpawnMaterial` contains a 2D color texture parameter, the plugin creates a material instance dynamic and sets the LUT/gradient/sender texture on that parameter.
  - If no `SpawnMaterial` is assigned, the plugin falls back to a vertex-color material such as `M_VertexColor` when vertex colors are present.
- **Vertex-color meshes**
  - Source or baked vertex colors are displayed with a vertex-color material.
- **Gradient / LUT textures**
  - Point-cloud or mesh scalar data can be mapped through a 1D LUT texture.
- **Sender texture path**
  - Full mesh textures can be applied through the actor texture material path.
- **Point-cloud colors**
  - LiDAR point colors are carried in the point payload. When width/scalar data is present, the plugin can map scalar values through a gradient LUT.

> The sample project may contain specific materials such as `Base_Material` and `M_VertexColor` under `Content/`. Those are sample-project assets, not middleware requirements.

### Blueprint Integration

**Subsystem delegates:**

| Delegate | Fires When |
|---|---|
| `OnFileReceived` | File download complete |
| `OnMessageReceived` | Text message received from broker |
| `OnProcessingProgress` | Processing progress update |
| `OnError` | Middleware / pipeline error |
| `OnPointCloudReceived` | Point cloud parsed / available |
| `OnNotificationReceived` | Broker push notification |
| `OnSceneUpdateReceived` | Typed scene / property update |
| `OnProtocolDiagnosticsReceived` | Protocol diagnostics / health event |

**Spawner delegates:**

| Delegate | Fires When |
|---|---|
| `OnFileProgress` | Per-file spawn progress |
| `OnFileComplete` | File spawned / updated |
| `OnAllComplete` | Initial spawn pipeline complete |
| `OnError` | Spawner error |

### Live Updates

The `JUSYNCFileSpawnerActor` subscribes to `OnNotificationReceived`:

```
NotificationCallback_Static (C, middleware thread)
  → AsyncTask(GameThread)
  → FJUSYNCNotification (HashLo, HashHi, HashPrevLo, HashPrevHi, bHasOldData)
  → OnNotificationReceived.Broadcast()
  → AJUSYNCFileSpawnerActor::OnBrokerNotification()
  → if hashPrev != storedHash → download → parse → spawn/update new → destroy old
  → if hashPrev == storedHash → SKIP (no flicker, no bandwidth)
```

The hash-gated decision logic compares `Notification.HashPrevLo/Hi` against stored `FileHashLo[Filename]` / `FileHashHi[Filename]`. A match means the local actor already represents the previous state, so the update can be skipped.

A slow polling backstop can also be enabled to catch missed notifications.

---

## Performance / Memory Model

JUSYNC is optimized for lower host memory, lower UE game-thread hitch, and lower refresh bandwidth.

### Middleware

- **Single-recv dispatcher**: One dispatcher thread owns ZMQ `recv()`, reducing socket contention and making response routing deterministic.
- **Per-request response queue**: Chunk responses are routed by `request_id` instead of relying on global locks.
- **Bounded parallel downloads**: RAM-aware scheduling limits in-flight downloads and respects pipeline-depth controls.
- **zstd payload support**: Compressed broker payloads reduce network transfer before parse.
- **Zero-copy parse path**: Clean `.usda` payloads can be parsed directly from the received buffer.
- **In-situ layout query/fill**: Hosts can query element counts first and fill caller-owned buffers without intermediate C-side allocation.
- **Compact fill path**: 32-bit float positions/normals/UVs avoid double-precision intermediate geometry when the host can consume compact buffers.
- **Direct LiDAR fill**: Packed point layout writes directly into UE-compatible point-cloud storage.
- **Hash-gated notifications**: `hashPrev128` lets the host skip unchanged files without downloading or spawning.

### UE Plugin

- **Buffer move path**: Parsed file buffers are moved across thread boundaries where possible instead of copied.
- **Compact refs**: Mesh data is stored in contiguous float/int/byte arrays suitable for RealtimeMesh streams.
- **Mesh cache**: Parsed meshes can be cached by file hash to avoid re-parsing unchanged files.
- **In-place RealtimeMesh updates**: Existing USD section groups can be updated instead of destroying and recreating actors.
- **Prebuilt RealtimeMesh streams**: Stream sets can be prepared ahead of component update to reduce repeated construction work.
- **Point-cloud actor pool**: LiDAR point-cloud actors are pooled to reduce spawn/destroy churn.
- **Frame budget controls**: `PipelineDepth` and `MaxSpawnsPerFrame` spread large updates across multiple frames.
- **Push-first live updates**: Broker notifications are the primary refresh path; polling is only an optional backstop.

---

## API Reference

### C++ Public API (`AnariUsdMiddleware`)

```cpp
#include "AnariUsdMiddleware.h"

using namespace anari_usd_middleware;

AnariUsdMiddleware middleware;

middleware.initialize();
middleware.connectToBroker("tcp://127.0.0.1:5556");
middleware.startReceiving();

// File list with sizes, ranks, and XXH3-128 hashes
std::vector<FileInfo> files;
middleware.requestFileListWithSizes(rank, files, 10000);

// Parallel download with immediate per-file callback
middleware.requestFilesParallelAsync(
    filePaths,
    targetRanks,
    30000,
    [](const std::string& filename, const std::vector<uint8_t>& data) {
        // Parse / forward file
    },
    []() {
        // All requested files completed
    },
    [](const std::string& filename, const std::string& error) {
        // Per-file error
    }
);

// USD processing
std::vector<MeshData> meshes;
middleware.LoadUSDBufferWithCollision(
    buffer,
    "model.usda",
    ECollisionComplexity::Complex,
    meshes
);

// Worker queries
uint32_t workerCount = 0;
middleware.requestWorkerCount(workerCount, 5000);

// Notifications
middleware.setNotificationCallback(
    [](uint32_t type,
       int32_t rank,
       const std::string& file,
       uint64_t size,
       uint64_t ts,
       uint64_t hashLo,
       uint64_t hashHi,
       uint64_t hashPrevLo,
       uint64_t hashPrevHi,
       bool hasOldData) {
        // Handle live update
    }
);

middleware.stopReceiving();
middleware.shutdown();
```

### C FFI (`AnariUsdMiddleware_C.h`)

All functions are suffixed with `_C` for UE compatibility.

Key functions:

```c
// Lifecycle
int  InitializeMiddleware_C(const char* endpoint);
void ShutdownMiddleware_C(void);
int  IsConnected_C(void);
int  StartReceiving_C(void);
void StopReceiving_C(void);

// Broker / file operations
int  ConnectToBroker_C(const char* endpoint, int timeout_ms);
void DisconnectFromBroker_C(void);
int  IsBrokerConnected_C(void);

int  RequestFileListWithSizes_C(/* ... */);
int  RequestFile_C(/* ... */);
int64_t RequestFileIntoBuffer_C(/* ... */);
int  AsyncRequestFile_C(/* ... */);
int  RequestFrame_C(/* ... */);
void RequestFilesParallelAsync_C(/* ... */);
int  RequestFilesParallelDirect_C(/* ... */);

// USD processing
int  LoadUSDBuffer_C(/* ... */);
int  LoadUSDFromDisk_C(/* ... */);
int  LoadUSDFull_C(/* ... */);
int  LoadUSDFullFromPointer_C(/* ... */);
int  LoadUSDBufferWithCollision_C(/* ... */);
int  LoadUSDFromDiskWithCollision_C(/* ... */);

// In-situ / compact parse
int  QueryUSDFullLayout_C(/* ... */);
int  FillUSDFull_C(/* ... */);
int  FillUSDFullCompact_C(/* ... */);
void FreeParsedUSD_C(void* handle);
void FreeUSDFullLayouts_C(/* ... */);

// Point clouds
int  ProcessPointCloudFromUSD_C(/* ... */);
int  GetCachedGradientTexture_C(/* ... */);
void FreePointCloudData_C(CPointCloudData* clouds, size_t count);
void FreeCachedGradientTexture_C(unsigned char* gradient_rgba);

// Textures
CTextureData CreateTextureFromBuffer_C(/* ... */);
int  WriteGradientLineAsPNG_C(/* ... */);
int  GetGradientLineAsPNGBuffer_C(/* ... */);
int  GetImageRowAsPNGBuffer_C(/* ... */);
int  GetPNGDimensions_C(/* ... */);

// Callbacks
void RegisterUpdateCallback_C(FileReceivedCallback_C callback);
void RegisterUpdateCallbackSpan_C(FileReceivedSpanCallback_C callback);
void RegisterMessageCallback_C(MessageReceivedCallback_C callback);
void RegisterNotificationCallback_C(NotificationCallback_C callback);
void RegisterSceneUpdateCallback_C(SceneUpdateCallback_C callback);
void RegisterProtocolDiagnosticsCallback_C(ProtocolDiagnosticsCallback_C callback);

// Cleanup
void FreeMeshData_C(CMeshData* meshes, size_t count);
void FreeTextureData_C(CTextureData* texture);
void FreeBuffer_C(unsigned char* buffer);
void FreeFileData_C(CFileData* file_data);
void FreeFileList_C(/* ... */);
void FreeFileListWithSizes_C(/* ... */);
void FreeFileListWithSizesAndRanks_C(/* ... */);
```

### In-Situ / Zero-Copy Parse API

The in-situ API avoids intermediate C-side allocation.

Typical flow:

```c
void* handle = nullptr;
CMeshLayout* meshLayouts = nullptr;
CPointCloudLayout* cloudLayouts = nullptr;
size_t meshCount = 0;
size_t cloudCount = 0;

int ok = QueryUSDFullLayout_C(
    buffer,
    bufferSize,
    "model.usda",
    &handle,
    &meshLayouts,
    &meshCount,
    &cloudLayouts,
    &cloudCount
);

if (!ok) {
    // Handle error
}

// Allocate caller-owned buffers using meshLayouts / cloudLayouts.
// For UE compact meshes, use CMeshDataFillCompact.
// For direct LiDAR, set CPointCloudDataFill.lidar_points.

ok = FillUSDFullCompact_C(
    handle,
    meshFills,
    meshCount,
    cloudFills,
    cloudCount
);

FreeUSDFullLayouts_C(
    meshLayouts,
    meshCount,
    cloudLayouts,
    cloudCount
);

FreeParsedUSD_C(handle);
```

Available fill styles:

| Style | Struct | Use Case |
|---|---|---|
| Legacy allocation | `CMeshData`, `CPointCloudData` | Simple hosts, tests |
| UE double precision | `CMeshDataFill`, `CPointCloudDataFill` | Match UE `FVector` / `FVector2D` / `FColor` layouts |
| Compact float | `CMeshDataFillCompact` | Lower-memory UE compact mesh path |
| Direct LiDAR | `CPointCloudDataFill.lidar_points` | Write directly into packed `FLidarPointCloudPoint`-compatible storage |

### C Structs

**`CFileData`**

```c
typedef struct {
    char filename[256];
    unsigned char* data;
    size_t data_size;
    char hash[64];
    char file_type[32];
} CFileData;
```

**`CMeshData`**

```c
typedef struct {
    char element_name[256];
    char type_name[128];

    float* points;
    size_t points_count;

    unsigned int* indices;
    size_t indices_count;

    float* normals;
    size_t normals_count;

    float* uvs;
    size_t uvs_count;

    float* vertex_colors;
    size_t vertex_colors_count;

    int collision_type;
    float* collision_vertices;
    size_t collision_vertices_count;
    unsigned int* collision_indices;
    size_t collision_indices_count;

    float bounding_box_min[3];
    float bounding_box_max[3];
    float sphere_center[3];
    float sphere_radius;

    const char* subdivision_scheme;
    int double_sided;

    unsigned int* face_vertex_counts;
    size_t face_vertex_counts_size;

    float** uv_sets;
    const char** uv_set_names;
    size_t uv_sets_count;

    unsigned char* vertex_colors8;
} CMeshData;
```

**`CPointCloudData`**

```c
typedef struct {
    char element_name[256];
    char type_name[128];

    float* positions;
    size_t points_count;

    float* normals;
    float* colors;
    float* widths;

    int has_normals;
    int has_colors;
    int has_widths;

    float bounding_box_min[3];
    float bounding_box_max[3];

    unsigned char* colors8;
} CPointCloudData;
```

**Layout query structs**

```c
typedef struct {
    char element_name[256];
    char type_name[128];
    size_t points_count;
    size_t indices_count;
    size_t normals_count;
    size_t uvs_count;
    size_t vertex_colors_count;
} CMeshLayout;

typedef struct {
    char element_name[256];
    size_t point_count;
    int has_colors;
    int has_normals;
    int has_widths;
} CPointCloudLayout;
```

**Fill structs**

```c
typedef struct {
    double* points;
    int32_t* indices;
    double* normals;
    double* uvs;
    unsigned char* vertex_colors8;
} CMeshDataFill;

typedef struct {
    float* points;
    int32_t* indices;
    float* normals;
    float* uvs;
    unsigned char* vertex_colors8;
} CMeshDataFillCompact;

typedef struct {
    double* positions;
    unsigned char* colors8;
    float* widths;

    float bounding_box_min[3];
    float bounding_box_max[3];

    void* lidar_points;
    int32_t lidar_point_stride;
    uint32_t lidar_point_version;
} CPointCloudDataFill;
```

**Direct LiDAR point**

```c
#pragma pack(push, 1)
typedef struct {
    float location[3];
    unsigned char color[4];
    unsigned char normal[3];
    unsigned char flags;
} CPointCloudLidarPoint_v1;
#pragma pack(pop)
```

---

## GUI Testing Application

An optional Dear ImGui-based testing tool lives in:

```
tools/ReceiverUI
```

Build it with:

```bash
cmake -S . -B build -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_JUSYNC_Receiver_GUI=ON

cmake --build build --parallel
```

Run:

```bash
./build/tools/ReceiverUI/ReceiverUI
```

Features:

- Real-time connection status and ZeroMQ state
- File reception monitoring
- 3D model viewer for loaded USD
- Mesh data inspector
- Texture preview and gradient extraction
- Performance metrics
- Point-cloud visualization

---

## USD Processing Capabilities

- **Geometry extraction**: Mesh points, indices, normals, UVs, vertex colors
- **Reference resolution**: Automatic resolution of USD references, payloads, and clips
- **Triangulation**: Polygonal faces converted to triangles
- **Transform application**: World-space transforms applied to points and normals
- **Coordinate conversion**: USD right-handed coordinate data converted for UE left-handed usage
- **UV primvar search**: Multiple primvar name patterns for UV coordinates
- **Multiple UV sets**: Optional extraction of additional UV sets
- **Subdivision metadata**: Subdivision scheme and double-sided flags preserved
- **Point clouds**: `GeomPoints` extraction with widths, scalar attributes, colors, normals, and bounding boxes
- **zstd payload support**: Broker-compressed USD payloads are detected and decompressed
- **Zero-copy parse path**: Clean `.usda` payloads can be parsed from the incoming buffer
- **Preprocessing fallback**: Known problematic USD patterns (`0: None`, `asset:images/`, `texCoord2f`) are handled with a controlled fallback
- **In-situ fill**: Caller-owned buffers can be filled directly after layout query
- **Compact fill**: 32-bit float geometry path for UE compact meshes
- **Direct LiDAR fill**: Packed point layout compatible with UE LiDAR point cloud storage
- **Collision generation**: None / Simple / ConvexHull / Complex / Simplified / ConvexDecomp
- **GPU acceleration**: Optional CUDA transforms for large meshes

---

## Troubleshooting

| Problem | Likely Cause | Fix |
|---|---|---|
| Broker not reachable | SSH tunnel not running | `ssh -N -L 5556:localhost:5556 ...` |
| Hash mismatch on download | Stale USD data, network drop, or broker race | Re-request file; inspect XXH3 logs |
| UE plugin cannot find middleware `.so` | Library not copied to plugin ThirdParty path | Copy to `Plugins/JUSYNC/Source/ThirdParty/AnariUsdMiddleware/Lib/Linux/` |
| CMake cannot find OpenSSL | Missing system OpenSSL dev package | Install `libssl-dev` or set `-DOPENSSL_ROOT_DIR=...` |
| zstd-compressed payloads fail | Built without zstd or `ENABLE_ZSTD=OFF` | Install `libzstd-dev` and configure with `-DENABLE_ZSTD=ON` |
| CUDA fallback to CPU | No CUDA device or built without CUDA | Use `-DENABLE_CUDA=ON` and valid `-DCUDA_ARCHITECTURES=...` |
| Point cloud empty | No `GeomPoints` prims in USD | Check simulation output for point-cloud geometry |
| Dead ranks in status table | Worker crashed or network partition | Check HPC job status and restart worker |
| V2 notifications ignored | Host only handling type 300 | Handle `NOTIFY_FILE_UPDATE_V2` (302) in the notification callback |
| CI static analysis noisy | clang-tidy report is intentionally non-blocking | Review `clang-tidy` artifact; tighten checks later if needed |
| CI test job fails but build passes | Specific CTest group failed | Open the specific job log and the uploaded `ctest-*` artifact |

**Enable verbose logging:**

```bash
# Middleware logging level (0=quiet, 3=verbose)
export MIDDLEWARE_LOG_LEVEL=3

# Disable DiffCapture status table
export DIFFCAPTURE_STATUS_INTERVAL=0
```
