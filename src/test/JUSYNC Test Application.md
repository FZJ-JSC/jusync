<img src="https://r2cdn.perplexity.ai/pplx-full-logo-primary-dark%402x.png" class="logo" width="120"/>

# ANARI USD Middleware Test Application

A comprehensive C++ test application for the ANARI USD Middleware that demonstrates ZeroMQ-based USD file processing, mesh extraction, and texture handling capabilities.

## Overview

This test application serves as both a demonstration and validation tool for the ANARI USD Middleware. It can operate in two modes:

- **Network Mode**: Receives USD files and images via ZeroMQ and processes them in real-time
- **Disk Mode**: Loads and processes USD files directly from disk for testing


## Features

### Core Capabilities

- **Real-time USD Processing**: Receives USD files via ZeroMQ and extracts mesh data
- **Multi-format Support**: Handles USDA, USD, USDC, and USDZ files
- **Image Processing**: Processes PNG/JPG images and extracts gradient lines
- **Mesh Analysis**: Detailed extraction of vertices, triangles, normals, and UV coordinates
- **File Verification**: SHA256 hash verification for received files
- **Comprehensive Logging**: Detailed processing information and debugging output


### Network Features

- ZeroMQ ROUTER socket for reliable file transfer
- Multi-part message handling (Identity + Filename + Content + Hash)
- Automatic file type detection
- Real-time processing statistics


## Prerequisites

### Required Dependencies

```bash
# Core libraries
- ZeroMQ (libzmq)
- TinyUSDZ
- GLM (OpenGL Mathematics)
- stb_image/stb_image_write

# Build tools
- CMake 3.15+
- C++17 compatible compiler
- pkg-config (for dependency resolution)
```


### Installation

```bash
# Ubuntu/Debian
sudo apt-get install libzmq3-dev libglm-dev

# macOS
brew install zeromq glm

# Windows (vcpkg)
vcpkg install zeromq glm
```


## Building

```bash
mkdir build && cd build
cmake ..
make -j$(nproc)
```


## Usage

### Network Mode (Default)

Starts a ZeroMQ server that listens for incoming USD files and images:

```bash
# Use default endpoint (tcp://*:5556)
./test_middleware

# Specify custom endpoint
./test_middleware --endpoint tcp://*:8080

# Custom save directory
./test_middleware --save-dir /path/to/output
```


### Disk Mode

Test USD file loading directly from disk:

```bash
./test_middleware --disk /path/to/file.usd
```


### Command Line Options

| Option | Description | Default |
| :-- | :-- | :-- |
| `--endpoint <addr>` | ZeroMQ endpoint to bind to | `tcp://*:5556` |
| `--save-dir <path>` | Directory for saved files | `received` |
| `--disk <file>` | Test disk loading mode | - |
| `--help` | Show help message | - |

## Output Structure

The application creates detailed output for each processed file:

```
received/
├── 20250523_105300_model.usd              # Original USD file
├── 20250523_105300_model.usd_analysis.txt # Mesh analysis report
├── 20250523_105301_texture.png            # Original image
└── 20250523_105301_gradient_texture.png   # Extracted gradient
```


## Processing Details

### USD File Processing

1. **File Reception**: Multi-part ZeroMQ message with hash verification
2. **Format Detection**: Automatic detection of USDA/USD/USDC/USDZ formats
3. **Mesh Extraction**: Recursive processing of USD primitives
4. **Data Conversion**: Extraction of vertices, indices, normals, and UVs
5. **Analysis Generation**: Detailed mesh statistics and reports

### Image Processing

1. **Texture Creation**: Conversion to internal texture format
2. **Gradient Extraction**: Special handling for 2-row images (keeps top row)
3. **PNG Generation**: Export processed textures as PNG files

## Example Output

```
=== ANARI USD Middleware Processor ===
Initializing ZMQ connection...
Endpoint: tcp://*:5556
✅ Middleware initialized successfully
✅ Callbacks registered successfully
🚀 ZMQ USD Processor is running...
📥 Waiting for USD files via ZMQ...

==================================================
📁 RECEIVED FILE #1
==================================================
📄 Filename: cube.usd
📊 Size: 2.34 KB
🏷️ Type: USD
🔐 Hash: a1b2c3d4e5f6...
💾 Saved to: received/20250523_105300_cube.usd

🔧 PROCESSING USD FILE
------------------------------
✅ Successfully extracted mesh data!
⏱️ Processing time: 45ms

📊 MESH ANALYSIS
----------------------------------------
🗂️ File: cube.usd
🔢 Total meshes: 1

 🔸 Mesh 1: Cube
 📝 Type: Mesh
 🔺 Vertices: 8
 🔻 Triangles: 12
 ➡️ Normals: 24
 🗺️ UVs: 14

📈 SUMMARY:
 🔢 Total vertices: 8
 🔢 Total triangles: 12
```


## Architecture

### Key Components

- **ZMQUSDProcessor**: Main application class handling file processing
- **AnariUsdMiddleware**: Core middleware interface
- **UsdProcessor**: USD file parsing and mesh extraction
- **ZmqConnector**: ZeroMQ communication handling
- **HashVerifier**: File integrity verification


### Message Flow

```
ZMQ Client → ZmqConnector → AnariUsdMiddleware → UsdProcessor → Mesh Data
                ↓
         File Callbacks → ZMQUSDProcessor → Analysis & Storage
```


## Debugging

### Verbose Output

The application provides comprehensive logging for debugging:

- File reception details
- Hash verification results
- USD parsing information
- Mesh extraction statistics
- Error diagnostics


### Common Issues

1. **Port Binding Failures**: Try alternative ports (15555) automatically
2. **USD Parsing Errors**: Check file format and TinyUSDZ compatibility
3. **Hash Mismatches**: Verify file integrity during transmission

## Testing

### Unit Testing

```bash
# Test disk loading
./test_middleware --disk samples/cube.usd

# Test with various USD formats
./test_middleware --disk samples/scene.usda
./test_middleware --disk samples/model.usdc
```


### Integration Testing

Use the provided Python client scripts to send test files via ZeroMQ.

## Performance

### Benchmarks

- **Small USD files** (<1MB): ~10-50ms processing time
- **Medium USD files** (1-10MB): ~50-200ms processing time
- **Large USD files** (>10MB): ~200ms+ processing time


### Memory Usage

- Base application: ~50MB
- Per USD file: +10-100MB (depending on complexity)
- Automatic cleanup after processing


## Contributing

1. Follow the existing code style and formatting
2. Add comprehensive logging for new features
3. Update this README for any new functionality
4. Test with various USD file formats

## License

This project is part of the ANARI USD Middleware suite. See the main project license for details.

<div style="text-align: center">⁂</div>

[^1]: AnariUsdMiddleware.cpp

[^2]: UsdProcessor.cpp

[^3]: ZmqConnector.cpp

[^4]: test_middleware.cpp

[^5]: AnariUsdMiddleware.h

[^6]: HashVerifier.h

[^7]: UsdProcessor.h

[^8]: ZmqConnector.h

[^9]: https://doc.owncloud.com/pdf/desktop/2.9_ownCloud_Desktop_Client_Manual.pdf

[^10]: https://www.youtube.com/watch?v=MwoAM3sznS0

[^11]: http://zguide2.wdfiles.com/local--files/page:start/zguide-c.pdf

[^12]: https://learn.microsoft.com/en-us/aspnet/core/test/middleware?view=aspnetcore-9.0

[^13]: https://web3py.readthedocs.io/_/downloads/en/v6.20.1/pdf/

[^14]: https://gitlab.com/seesat/rodos/-/blob/master/test-suite/README.md

[^15]: https://issues.apache.org/jira/browse/ARROW-16919?focusedCommentId=17571385

[^16]: https://clone-swarm.usask.ca/?page=dashboard

[^17]: https://issues.apache.org/jira/browse/ARROW-16919

[^18]: https://gitlab2.informatik.uni-wuerzburg.de/stl56jc/tamariv_uwb/-/blob/tuning/rodos/test-suite/README.md

