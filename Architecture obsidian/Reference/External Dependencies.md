# External Dependencies

Bundled third-party libraries in `external/`.

## Directory Layout

```
external/
├── zmq/              # ZeroMQ 4.x — static build
├── ZeroMQ/           # ⚠️ Duplicate? CMakeLists uses external/zmq only
├── cppzmq/           # C++ bindings for ZeroMQ
├── tinyusdz/         # TinyUSDZ — USD parsing, static build
├── glm/              # GLM — math library, header-only
├── stb/              # STB — image decode (stb_image.h), header-only
├── xxhash/           # xxHash — fast hashing, compiled into target
├── nlohmann/         # JSON for Modern C++ (single-include, header-only)
└── imgui/            # Dear ImGui (for ReceiverUI tool)
```

## Build Integration

| Library | CMake | Link Mode | Usage |
|---|---|---|---|
| **ZeroMQ** (`zmq/`) | `add_subdirectory(external/zmq)` | Static (`libzmq-static`) | Networking |
| **cppzmq** (`cppzmq/`) | `#include` from path | Header-only | C++ ZMQ wrapper |
| **TinyUSDZ** (`tinyusdz/`) | `add_subdirectory(external/tinyusdz)` | Static (`tinyusdz_static`) | USD parsing |
| **GLM** (`glm/`) | `add_subdirectory(external/glm)` | Header-only | Math (vec3, mat4) |
| **xxHash** (`xxhash/`) | `target_sources(... xxhash/xxhash.c)` | Compiled in | XXH3-128 hashing |
| **STB** (`stb/`) | `#include` from path | Header-only (impl defined) | Image decode (stb_image) + encode (stb_image_write) |
| **nlohmann/json** (`nlohmann/`) | `#include` from path | Header-only (single-include) | JSON parsing of file list responses |
| **Dear ImGui** (`imgui/`) | `add_subdirectory(tools/ReceiverUI)` | Compiled into ReceiverUI tool only | GUI testing app |

## Notes

### ZeroMQ vs ZeroMQ (Duplicate)
`external/zmq/` is used by `CMakeLists.txt`. `external/ZeroMQ/` appears to be a duplicate submodule — confirm and potentially remove.

### nlohmann/json
Used in `AnariUsdClient.cpp` to parse file-list JSON responses from the broker:
```cpp
#include "../../external/nlohmann/single_include/nlohmann/json.hpp"
using json = nlohmann::json;
// ... json::parse(jsonStr)
```

### TinyUSDZ GCC 15 Workaround
```cmake
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU" AND CMAKE_CXX_COMPILER_VERSION VERSION_GREATER_EQUAL 15)
    target_compile_options(tinyusdz_static PRIVATE -fpermissive)
endif()
```

### TBB (System, Not Bundled)
Not bundled — found via `find_package(TBB QUIET)`. Used for `std::execution::par` parallel algorithms when available (UV normalization, collision batch processing). Falls back to sequential when absent.

See [[Build/03 Build System]] for CMake configuration.
