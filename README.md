
# JUSYNC

A high-performance C++ middleware library that provides seamless communication between ANARI (ANAlytic Rendering Interface) applications and USD (Universal Scene Description) data streams via ZeroMQ messaging. This middleware enables real-time streaming of USD files, textures, and mesh data over network connections, making it ideal for collaborative workflows, live preview systems, and distributed rendering pipelines.

## Overview

The JUSYNC Middleware acts as a bridge between USD content creation tools and ANARI-based rendering applications. It implements a robust ZeroMQ ROUTER/DEALER communication pattern to handle file transfers with hash verification, automatic USD format conversion, and comprehensive mesh data extraction capabilities.

## Recent Updates (v1.0.1)

### 🔧 Critical Bug Fixes
- **Fixed duplicate file processing** - Implemented `isDuplicateFile()` and `markFileAsProcessed()` methods to prevent the same file from being processed multiple times
- **Resolved C interface memory access violations** - Eliminated premature memory deallocation in C interface that was causing application crashes
- **Fixed mesh data conversion** - Corrected conversion between internal `glm::vec3` format and public API flat float arrays for RealtimeMesh compatibility

### 🛡️ Enhanced Safety & Validation
- **Comprehensive input validation** - Added extensive bounds checking throughout USD processing pipeline
- **Memory safety improvements** - Enhanced memory allocation checks with proper exception handling and size validation
- **Thread safety enhancements** - Fixed atomic operations in statistics classes and improved mutex usage
- **Pointer validation** - Added `MIDDLEWARE_VALIDATE_POINTER` macros throughout the codebase

### 🎮 RealtimeMesh Integration
- **Intelligent geometry preservation** - Added detection for large geometry arrays to preserve original USD data for Unreal's RealtimeMesh system
- **Enhanced mesh validation** - Comprehensive mesh data validation with bounds checking and geometry integrity validation
- **Optimized preprocessing** - Smart preprocessing that detects and preserves large geometry for better Unreal Engine compatibility

### 🔄 USD Processing Improvements
- **Enhanced reference resolution** - Improved reference and payload resolution system with better error handling
- **Transform validation** - Added determinant checking to prevent singular matrix transformations
- **Better error context** - Enhanced error messages with more specific context about failed operations

### 🌐 Cross-Platform Enhancements
- **Safe string handling** - Added platform-specific safe string copying methods (`strncpy_s` on Windows, standard `strncpy` elsewhere)
- **Improved conditional compilation** - Enhanced `MiddlewareLogging.h` with better Unreal Engine detection
- **Fixed API macro definitions** - Resolved duplicate API macro definitions in C interface

### ⚠️ Breaking Changes
- **ProcessingStats and MessageStats classes** are now non-copyable (use snapshot methods instead)
- **Enhanced validation** may reject previously accepted invalid data

## Feature Matrix

### ✅ Available Features

- [x] **Real-time USD Streaming**: Receive and process USD files (.usd, .usda, .usdc, .usdz) in real-time over ZeroMQ connections
- [x] **Advanced Mesh Processing**: Extract complete geometry data including vertices, indices, normals, and UV coordinates
- [x] **USD Reference Resolution**: Automatically resolves USD references, payloads, and clips to load complete scenes
- [x] **Hash Verification**: Built-in SHA-256 hash verification ensures data integrity during transmission
- [x] **Cross-platform Support**: Compatible with Windows and Linux environments with dynamic library linking
- [x] **Thread-safe Operations**: Multi-threaded architecture with callback-based event handling
- [x] **GUI Testing Tool**: Dear ImGui-based GUI application at `../tools/ReceiverUI`
- [x] **Texture Processing**: Handle image data with gradient extraction and PNG encoding/decoding
- [x] **Load from Disk**: Direct USD file loading from filesystem with `LoadUSDFromDisk()`
- [x] **Load from Buffer**: Process USD data from memory buffers with `LoadUSDBuffer()`
- [x] **USD Format Detection**: Automatic detection of USD file formats and types
- [x] **Triangulation**: Converts polygonal faces to triangles for real-time rendering
- [x] **Coordinate Transformation**: Transforms vertices and normals using world transformation matrices
- [x] **UV Coordinate Handling**: Searches multiple primvar names for texture coordinates
- [x] **Comprehensive Logging**: Environment-specific logging (Unreal Engine vs Standard C++)
- [x] **Error Handling**: Detailed error reporting with fallback mechanisms
- [x] **Memory Management**: RAII patterns with automatic cleanup
- [x] **Duplicate Prevention**: Intelligent duplicate file detection and processing prevention
- [x] **RealtimeMesh Compatibility**: Optimized for Unreal Engine RealtimeMesh workflows

### ❌ Not Available Features

- [ ] **USD to USDC Conversion**: `ConvertUSDtoUSDC()` method exists but requires external `tusdcat` tool
- [ ] **Material Processing**: Material extraction from USD files is not fully implemented
- [ ] **Animation Support**: USD animation and time-varying data is not processed
- [ ] **Light Processing**: USD light extraction is not implemented
- [ ] **Camera Processing**: USD camera data extraction is not implemented
- [ ] **Subdivision Surfaces**: Advanced USD subdivision surfaces are not supported
- [ ] **Volume Rendering**: USD volume data processing is not available
- [ ] **Instancing**: USD instancing and prototypes are not fully supported

### 🚧 Work in Progress

- [x] Unreal plugin

## Architecture

The middleware consists of several key components:

- **AnariUsdMiddleware**: Main interface class providing the public API with PIMPL pattern
- **ZmqConnector**: Handles ZeroMQ ROUTER socket communication for receiving data from DEALER clients
- **UsdProcessor**: Processes USD files using TinyUSDZ library for mesh and texture extraction
- **HashVerifier**: Provides SHA-256 hash calculation and verification utilities using OpenSSL

## Dependencies

- **ZeroMQ**: High-performance messaging library for network communication
- **OpenSSL**: Cryptographic library for hash verification
- **TinyUSDZ**: Lightweight USD file processing library with composition support
- **GLM**: Mathematics library for 3D transformations
- **STB**: Single-header libraries for image processing
- **Dear ImGui**: For the optional GUI testing application

## Building

### Prerequisites

**Windows**:
- Visual Studio 2019 or later
- CMake 3.16+
- ZeroMQ SDK (configurable path, defaults to `D:/SDK/ZeroMQ`)
- OpenSSL (configurable path, defaults to `C:/Program Files/FireDaemon OpenSSL 3`)

**Linux**:
- GCC 7+ or Clang 6+
- CMake 3.16+
- ZeroMQ development packages (`libzmq3-dev`)
- OpenSSL development packages (`libssl-dev`)

### Build Instructions

```


# Clone the repository

git clone <repository-url>
cd jusync_usd_middleware

# Create build directory

mkdir build
cd build

# Configure with CMake (uses dynamic paths)

cmake .. -DCMAKE_BUILD_TYPE=Release

# Override default paths if needed

cmake .. -DZMQ_ROOT=/custom/zmq/path -DOPENSSL_ROOT_DIR=/custom/openssl/path

# Build the library

cmake --build . --config Release

# Build with GUI testing tool

cmake .. -DBUILD_JUSYNC_Receiver_GUI=ON

# Disable tests if not needed

cmake .. -DBUILD_TESTS=OFF

```

## GUI Testing Application

The middleware includes a Dear ImGui-based GUI application located at `../tools/ReceiverUI` for testing and visualizing model loading:

```


# Build with GUI enabled

cmake .. -DBUILD_JUSYNC_Receiver_GUI=ON
cmake --build . --config Release

# Run the GUI application

cd tools/ReceiverUI
./ReceiverUI  \# or ReceiverUI.exe on Windows

```

The GUI application provides:
- **Real-time Connection Status**: Visual indicators for ZeroMQ connection state
- **File Reception Monitoring**: Live display of incoming files with size and hash information
- **USD Model Visualization**: Interactive 3D viewer for loaded USD models
- **Mesh Data Inspector**: Detailed view of extracted vertices, normals, and UV coordinates
- **Texture Preview**: Display of processed textures and gradient lines
- **Performance Metrics**: Real-time statistics for processing times and memory usage

## API Flow Architecture

```

graph TD
A[Client Application] --> B[AnariUsdMiddleware::initialize]
B --> C[ZmqConnector::initialize]
C --> D[Bind to ZeroMQ Endpoint]

    A --> E[registerUpdateCallback]
    A --> F[registerMessageCallback]
    
    A --> G[startReceiving]
    G --> H[Background Thread Loop]
    
    I[ZeroMQ Client] --> J[Send File/Message]
    J --> K[ZmqConnector::receiveFile]
    K --> L[HashVerifier::verifyHash]
    L --> M{Hash Valid?}
    
    M -->|Yes| N[Trigger FileUpdateCallback]
    M -->|No| O[Log Error & Reject]
    
    N --> P[UsdProcessor::LoadUSDBuffer]
    P --> Q[Extract Mesh Data]
    Q --> R[Return MeshData Array]
    
    S[Image Buffer] --> T[UsdProcessor::CreateTextureFromBuffer]
    T --> U[STB Image Processing]
    U --> V[Return TextureData]
    
    W[USD File Path] --> X[LoadUSDFromDisk]
    X --> Y[Read File to Buffer]
    Y --> P
    ```

## Public API Reference

### Core Classes

#### AnariUsdMiddleware

```


## Network Protocol

### ZeroMQ ROUTER/DEALER Pattern

**File Transfer Message Format**:
1. Client Identity (automatic)
2. Filename
3. File Content (binary)
4. SHA-256 Hash

**Simple Message Format**:
1. Client Identity (automatic)
2. Message Content (JSON/text)

### Client Example (Python)

```

import zmq
import hashlib

context = zmq.Context()
socket = context.socket(zmq.DEALER)
socket.connect("tcp://localhost:5556")

# Send USD file

with open("model.usd", "rb") as f:
data = f.read()

file_hash = hashlib.sha256(data).hexdigest()
socket.send_multipart([
b"model.usd",
data,
file_hash.encode()
])

```

## USD Processing Capabilities

The middleware includes comprehensive USD processing functionality:

- **USD Parsing**: Extracts geometry, materials, UVs, and transformations from USD files using TinyUSDZ
- **Reference Resolution**: Automatically resolves USD references, payloads, and clips to load complete scenes
- **Triangulation**: Converts polygonal faces to triangles for real-time rendering
- **Coordinate Transformation**: Transforms vertices and normals using proper world transformation matrices
- **UV Coordinate Handling**: Searches for UV coordinates across multiple possible primvar names
- **Texture Processing**: Creates textures from raw buffer data with gradient extraction capabilities
- **Format Detection**: Supports multiple USD formats (.usd, .usda, .usdc, .usdz)
- **Content Preprocessing**: Fixes common USD content issues for better compatibility

## Error Handling

The middleware provides comprehensive error handling with detailed logging:

- **Connection Errors**: Automatic endpoint fallback and retry mechanisms
- **Hash Verification**: Detailed mismatch reporting with calculated vs expected hashes
- **USD Processing**: TinyUSDZ error reporting with reference resolution fallbacks
- **File System**: Proper error handling for disk operations and file access
- **Memory Management**: RAII patterns with automatic cleanup

## Troubleshooting

**Common Issues**:

- **Port Binding**: Library tries alternative endpoints automatically
- **USD References**: Searches multiple patterns for referenced geometry files
- **Hash Failures**: Logs both expected and calculated hashes for debugging
- **Missing Dependencies**: Clear error messages with installation guidance
- **File Access**: Proper error reporting for file system operations

Enable verbose logging for detailed processing information including USD prim hierarchies, ZeroMQ message flow, and file system operations.

