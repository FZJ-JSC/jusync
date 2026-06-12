# Getting Started

## Prerequisites

### Linux
- **GCC** 7+ (GCC 15 needs `-fpermissive` workaround for tinyusdz) or **Clang** 6+
- **CMake** 3.16+
- `libssl-dev` (OpenSSL development headers)
- Optional: `libtbb-dev` (parallel processing acceleration)
- Optional: CUDA toolkit (GPU acceleration)

### Windows
- **Visual Studio** 2019+
- **CMake** 3.16+
- **OpenSSL** – defaults to `C:/Program Files/OpenSSL-Win64`
- Optional: CUDA toolkit

## Clone & Bootstrap

```bash
git clone <repository-url>
cd jusync

# Required: submodules contain external dependencies
git submodule update --init --recursive

# Submodules included:
#   external/zmq        - ZeroMQ (built statically)
#   external/tinyusdz   - USD parsing library
#   external/glm        - Math library (header-only)
#   external/xxhash     - Fast hashing
```

## Build (Quick)

```bash
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
cmake --build . -j$(nproc)
```

## Build with All Features

```bash
cmake .. \
  -DCMAKE_BUILD_TYPE=Release \
  -DENABLE_CUDA=ON \
  -DBUILD_TESTS=ON \
  -DBUILD_JUSYNC_Receiver_GUI=ON

cmake --build . -j$(nproc)
```

## Run Tests

```bash
ctest --output-on-failure           # All tests
ctest -R "usd_basic"               # Filter by name
make test_quick                     # Quick smoke tests
make test_full                      # Full suite, verbose
```

## Directory Layout

```
jusync/
├── include/          # Public headers (17 files)
├── src/              # Implementation (14 .cpp + 1 .cu)
├── external/         # Bundled deps (zmq, tinyusdz, glm, xxhash, stb)
├── tests/            # USD validation tool
├── tools/            # ReceiverUI (Dear ImGui)
├── UnrealPlugin/     # UE5 plugin (JUSYNC/)
├── fonts/            # Embedded font assets
├── CMakeLists.txt
└── CMakePresets.json
```

See [[Build/03 Build System]] for full CMake options.
