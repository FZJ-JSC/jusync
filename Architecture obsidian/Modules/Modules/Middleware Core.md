# Middleware Core

`AnariUsdMiddleware` — The single public entry point to the entire library. Implements **PIMPL** (Pointer-to-Implementation) to hide internal classes and provide a stable ABI.

## Files
- `include/AnariUsdMiddleware.h` — Public class declaration
- `include/AnariUsdMessages.h` — Shared message structs and enums
- `include/MiddlewareLogging.h` — Cross-platform logging macros
- `src/AnariUsdMiddleware.cpp` — PIMPL implementation (1200+ lines)

## Class Design

```cpp
class ANARI_USD_MIDDLEWARE_API AnariUsdMiddleware {
public:
    AnariUsdMiddleware();   // Creates std::make_unique<Impl>
    ~AnariUsdMiddleware();  // Calls shutdown()
    // Non-copyable
    AnariUsdMiddleware(const AnariUsdMiddleware&) = delete;
    AnariUsdMiddleware& operator=(const AnariUsdMiddleware&) = delete;

    bool initialize(const char* endpoint = nullptr);
    void shutdown();
    bool isConnected() const;

private:
    class Impl;
    std::unique_ptr<Impl> pImpl;         // PIMPL

    mutable std::shared_mutex statusMutex;
    std::atomic<bool> initialized{false};
    std::atomic<bool> shutdownRequested{false};
};
```

## Impl Internal Components

```cpp
class Impl {
    ZmqConnector zmqConnector;                    // ⚠️ Dead — no socket created
    shared_ptr<AnariUsdClient> anariUsdClient;     // Active — DEALER client
    unique_ptr<UsdProcessor> usdProcessor;         // Active
    unique_ptr<CollisionProcessor> collisionProcessor; // Active

    // Callbacks
    std::map<int, FileUpdateCallback> updateCallbacks;
    std::map<int, MessageCallback> messageCallbacks;
    std::mutex callbackMutex;

    // ⚠️ Receiver thread — never spawned
    std::thread receiverThread;
    std::atomic<bool> running{false};

    // Duplicate tracking
    unordered_set<string> processedFiles;     // "filename:hash" keys
    mutex processedFilesMutex;
    atomic<size_t> maxTrackedFiles{10000};

    // Gradient/texture cache
    map<string, vector<uint8_t>> cachedTextures;
    mutex textureCacheMutex;

    // Collision config
    atomic<ECollisionComplexity> defaultCollisionComplexity{Complex};

    // Async thread tracking
    vector<thread> asyncThreads;
    mutex asyncThreadsMutex;
    // ...see impl details below...
};
```

## Public API Surface (by category)

### Lifecycle
| Method | Description |
|---|---|
| `initialize(endpoint)` | Creates UsdProcessor, CollisionProcessor, ZmqConnector context. Defaults to `tcp://*:5556`. |
| `shutdown()` | Stops receiver, disconnects ZMQ, disconnects broker client, drops callbacks |
| `isConnected()` | Returns `initialized && !shutdownRequested` |

### DEALER Client (Outbound) ⬅ Active
The middleware acts as a **DEALER client** to connect to a broker/worker:
| Method | Description |
|---|---|
| `connectToBroker(endpoint, timeoutMs)` | Create AnariUsdClient (lazy), `connect()`, start dispatcher thread |
| `disconnectFromBroker()` | Close DEALER connection |
| `isBrokerConnected()` | Client connection status |

### File Requests (Sync)
| Method | Description |
|---|---|
| `requestFileList(targetRank, outFiles)` | JSON file list from broker. Returns `true` with empty list if not connected. |
| `requestFileListWithSizes(targetRank, outFiles)` | List + sizes + XXH3-128 hashes. Returns `false` if not connected. |
| `requestFile(filename, rank, outData)` | Chunked download → assembled into one `vector<uint8_t>` |
| `requestFrame(frameNumber, rank, outFrameFiles)` | All files for a frame |

### File Requests (Async + Callbacks)
| Method | Description |
|---|---|
| `requestWorkerCountAsync(timeout, cb)` | Query worker count |
| `requestTotalWorkerCountAsync(timeout, cb)` | Including rank 0 (legacy string protocol) |
| `requestWorkerStatusAsync(rank, timeout, cb)` | Per-worker tuple |
| `requestFileListAsync(...)` | Async file list |
| `requestFilesParallelAsync(filenames, ranks, spawnCb, completeCb, errorCb)` | Parallel streaming download with RAM awareness via `ParallelDownloadManager` |

### Worker Queries (Sync)
| Method | Returns |
|---|---|
| `requestWorkerCount(outCount, timeout)` | Total connected workers |
| `requestTotalWorkerCount(outCount, timeout)` | Including rank 0 |
| `requestWorkerStatus(rank, outStatus, timeout)` | `(rank, status, hostname, gpuInfo, heartbeat)` |
| `requestWorkerListString(outWorkers, timeout)` | Python-broker-compatible string list |

### Callbacks
| Method | Purpose |
|---|---|
| `registerUpdateCallback(fn)` → `int id` | ⚠️ Unreachable — receiver thread never runs |
| `registerMessageCallback(fn)` | ⚠️ Unreachable — receiver thread never runs |
| `setNotificationCallback(fn)` | Push notifications via `AnariUsdClient` dispatcher |

### USD Loading
| Method | Purpose |
|---|---|
| `LoadUSDBuffer(buffer, fileName, outMeshes)` | → `LoadUSDBufferWithCollision(..., None)` |
| `LoadUSDFromDisk(filePath, outMeshes)` | → `LoadUSDFromDiskWithCollision(..., None)` |
| `LoadUSDBufferWithCollision(buffer, name, complexity, outMeshes)` | UsdProcessor → convertMeshDataWithCollision → CollisionProcessor |
| `LoadUSDFromDiskWithCollision(path, complexity, outMeshes)` | readFileToBuffer → LoadUSDBufferWithCollision |

### Collisions
| Method | Purpose |
|---|---|
| `setDefaultCollisionComplexity(cpx)` | Set default |
| `setCollisionParameters(simplification, hullPrecision, maxHulls)` | Per-instance tuning |
| `getDefaultCollisionComplexity()` | Read current |

### Textures & Gradients
| Method | Purpose |
|---|---|
| `CreateTextureFromBuffer(buffer)` | STB decode to RGBA |
| `WriteGradientLineAsPNG(buffer, outPath)` | PNG file write (stb_image_write) |
| `GetGradientLineAsPNGBuffer(buffer, outPng)` | PNG memory encode |
| `GetCachedGradientTexture(outData, outW, outH)` | Last cached texture (see [[Modules/Point Cloud]]) |

## Internal Pipelines

### USD Load with Collision
```
LoadUSDBufferWithCollision(buffer, name, complexity, outMeshes)
  → usdProcessor->LoadUSDBuffer(buffer, name, processorMeshData, nullptr, progressCb)
  → for each UsdProcessor::MeshData in processorMeshData:
      convertMeshDataWithCollision(processorMesh, complexity, publicMesh)
        → glm→flat conversion: points, normals, uvs, vertex_colors
        → color interpolation detection (vertex/uniform/fallback) — see [[Modules/Color Interpolation]]
        → if complexity != None: collisionProcessor->generateCollision(...)
        → outMeshData.push_back(std::move(publicMesh))
```

### Mesh to Flat Array Conversion (⚡ Performance Issue)
`convertMeshDataWithCollision()` converts `glm::vec3` arrays to flat `float` arrays using per-component `push_back`:

```cpp
// Current: O(N) cache-unfriendly push_back per component
for (const auto& point : processorMeshData.points) {
    publicMeshData.points.push_back(point.x);
    publicMeshData.points.push_back(point.y);
    publicMeshData.points.push_back(point.z);
}
```

`reserve()` is called but each `push_back` is an individual write. A direct index write (`out[3*i+j] = component`) would be ~3x fewer stores due to better register usage. See [[Reference/Known Issues & Dead Code]].

### Duplicate File Tracking
Files tracked by `"filename:hash"` key in `processedFiles` set.
- Cleanup: every hour, if size > maxTrackedFiles/2, clear all entries
- Size limit: 10K entries; if exceeded, remove 10% oldest
- ⚠️ Currently unreachable because receiver thread never runs

### Gradient Texture Cache
When a received file is type `"IMAGE"`, raw data is cached in `cachedTextures[filename]`. Used for point cloud color baking.

See [[Modules/Network Layer]] and [[Modules/ZeroMQ Client]] for ZMQ internals, [[Modules/C Global State]] for C API globals.
