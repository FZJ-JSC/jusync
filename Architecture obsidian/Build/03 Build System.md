# Build System

CMake-based build with configurable features, static linking, and cross-platform support.

## Files
- `CMakeLists.txt` (579 lines)
- `CMakePresets.json`
- `Dockerfile`, `docker-compose.yml`

## Project Declaration

```cmake
cmake_minimum_required(VERSION 3.16)
project(anari_usd_middleware VERSION 1.0.0 LANGUAGES CXX)
# Or with CUDA: LANGUAGES CXX CUDA
```

C++ standard: **17** (enforced via `CMAKE_CXX_STANDARD 17`).

## Configuration Options

| Option | Default | Description |
|---|---|---|
| `ENABLE_CUDA` | `OFF` | Enable GPU acceleration (`src/GpuKernels.cu`) |
| `BUILD_TESTS` | `ON` | Build `usd_validation_tool` + CTest |
| `BUILD_JUSYNC_Receiver_GUI` | `OFF` | Dear ImGui testing app (`tools/ReceiverUI`) |

### CUDA Architectures
```cmake
set(CUDA_ARCHITECTURES "60;61;70;75;80;86" CACHE STRING ...)
```

## Dependencies

| Library | Source | Link Mode | Notes |
|---|---|---|---|
| **ZeroMQ** | `external/zmq` | **Static** (`libzmq-static`) | No runtime dependency |
| **OpenSSL** | System | Dynamic (`libssl`, `libcrypto`) | `find_package(OpenSSL REQUIRED)` |
| **TinyUSDZ** | `external/tinyusdz` | **Static** (`tinyusdz_static`) | `add_subdirectory` |
| **GLM** | `external/glm` | Header-only | `add_subdirectory` |
| **xxHash** | `external/xxhash` | Compiled into target | `xxhash.c` in `target_sources` |
| **STB** | `external/stb` | Header-only | `#include "external/stb"` |
| **TBB** | System (optional) | Dynamic (`TBB::tbb`) | `find_package(TBB QUIET)` |

### Override Dependency Paths
```bash
cmake -DOPENSSL_ROOT_DIR=/path/to/openssl ..
# Or env var: export OPENSSL_ROOT_DIR=/path
```

## ⚠️ Known CMake Issues
- Lines 143-156 and 176-189: `find_package(TBB QUIET)` appears twice identically. Second is redundant.

## Output

### Main Target
```cmake
add_library(${PROJECT_NAME} SHARED ...)
```
- Linux: `libanari_usd_middleware.so`
- Windows: `anari_usd_middleware.dll`

### Sources (14 .cpp + 1 .c + optional 1 .cu)
```
src/AnariUsdMiddleware.cpp
src/ZmqConnector.cpp
src/AnariUsdClient.cpp
src/HashVerifier.cpp
src/UsdProcessor.cpp
src/AnariUsdMiddleware_C.cpp
src/CollisionProcessor.cpp
src/ParallelDownloadManager.cpp
src/MemoryMonitor.cpp
src/GpuContext.cpp
src/GpuValidation.cpp
external/xxhash/xxhash.c
# Optional:
src/GpuKernels.cu    # When ENABLE_CUDA=ON
```

## Platform-Specific Details

### Linux
```cmake
# GLIBC 2.35 compatibility
-_GLIBCXX_USE_CXX11_ABI=1, _GNU_SOURCE, _FORTIFY_SOURCE=2
-O3 -pipe -fPIC
# Static C++ runtime
-static-libstdc++, -static-libgcc
# RPATH for UE
INSTALL_RPATH "$ORIGIN"
# System libs for static ZMQ
pthread, dl, rt, m
```

### Windows
```cmake
# DLL export
-DANARI_USD_MIDDLEWARE_API=__declspec(dllexport)
# Dynamic runtime
/MD (Release), /MDd (Debug)
# Link
ws2_32, iphlpapi
```

### GCC 15 Workaround
```cmake
# tinyusdz macro: template-dependent lookup in is_array
target_compile_options(tinyusdz_static PRIVATE -fpermissive)
```

## Verification Target (Linux)
```bash
make verify_glibc_compatibility
```
Checks: GLIBC symbols, `ldd` dependencies, static ZMQ linkage, static libstdc++ linkage.

## Build Commands

```bash
# Configure
cmake -B build -DCMAKE_BUILD_TYPE=Release

# Build
cmake --build build -j$(nproc) --config Release

# With all features
cmake -B build \
  -DCMAKE_BUILD_TYPE=Release \
  -DENABLE_CUDA=ON \
  -DBUILD_TESTS=ON \
  -DBUILD_JUSYNC_Receiver_GUI=ON

# Install
cmake --install build
```

## Installation
```cmake
# Library → lib/
# Headers → include/
# usd_validation_tool → bin/ (if BUILD_TESTS)
# OpenSSL DLLs → bin/ (Windows only)
```

## Docker
```bash
docker compose up --build
```
