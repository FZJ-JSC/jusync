# API Reference

Complete public API surface of the `AnariUsdMiddleware` class.

## Files
- `include/AnariUsdMiddleware.h` (240 lines)
- `include/AnariUsdMessages.h` (482 lines)

---

## Construction & Lifecycle

```cpp
AnariUsdMiddleware* mw = new AnariUsdMiddleware();

bool ok = mw->initialize("tcp://*:5556");
// Optional: endpoint param, defaults to tcp://*:5556

mw->shutdown();
delete mw;
```

## Callback Registration

### File Update Callback
Fired when a file is received over the ROUTER socket.

```cpp
int id = mw->registerUpdateCallback(
    [](const FileData& fd) {
        // fd.filename, fd.data, fd.hash, fd.fileType
    });
mw->unregisterUpdateCallback(id);
```

### Message Callback
Fired on text/JSON messages.

```cpp
int id = mw->registerMessageCallback(
    [](const string& msg) { /* ... */ });
```

### Notification Callback
Fired on push notifications from broker.

```cpp
mw->setNotificationCallback(
    [](uint32_t msgType, int32_t rank, const string& filename,
       uint64_t fileSize, uint64_t timestamp) {
        if (msgType == 300) // NOTIFY_FILE_UPDATE
        if (msgType == 301) // NOTIFY_COMMIT_COMPLETE
    });
```

---

## Receiver Thread

```cpp
mw->startReceiving();   // Start background loop on ROUTER socket
mw->stopReceiving();    // Graceful stop
```

---

## Client (Outbound DEALER)

```cpp
mw->connectToBroker("tcp://broker-host:5555", 5000);
mw->disconnectFromBroker();
bool connected = mw->isBrokerConnected();
```

---

## Synchronous File Requests

```cpp
// List files
vector<string> files;
mw->requestFileList(0, files, 10000);

// List files + sizes
vector<FileInfo> fi;
mw->requestFileListWithSizes(0, fi, 10000);

// Download file
vector<uint8_t> data;
mw->requestFile("model.usda", 0, data, 30000);

// All files for a frame
vector<pair<string, vector<uint8_t>>> frame;
mw->requestFrame(42, 0, frame, 60000);
```

---

## Asynchronous Requests

```cpp
// Worker count
mw->requestTotalWorkerCountAsync(5000,
    [](uint32_t count) { /* ... */ },
    [](const string& err) { /* ... */ });

// File list
mw->requestFileListAsync(0, 10000,
    [](const vector<string>& files) { /* ... */ });

// Worker status
mw->requestWorkerStatusAsync(0, 10000,
    [](const vector<tuple<int32_t, uint32_t, string, string, uint64_t>>& ws) {
        for (auto& [rank, status, host, gpu, heartbeat] : ws) { }
    });
```

---

## Parallel Streaming Download

```cpp
vector<string> files = {"a.usda", "b.usdc", "c.usd"};
vector<int32_t> ranks = {0, 0, 1};

mw->requestFilesParallelAsync(files, ranks, 30000,

    // Per-file spawn (fires immediately on each completion)
    [](const string& fn, const vector<uint8_t>& data) {
        mw->LoadUSDBuffer(data, fn, meshes);
    },

    // All done
    []() { LOG("all files downloaded"); },

    // Per-file error
    [](const string& fn, const string& err) { LOG(err); }
);
```

---

## Worker Queries (Sync)

```cpp
uint32_t wc;
mw->requestWorkerCount(wc, 5000);
mw->requestTotalWorkerCount(wc, 5000);

vector<tuple<int32_t, uint32_t, string, string, uint64_t>> status;
mw->requestWorkerStatus(0, status, 10000);

vector<tuple<int32_t, string, string>> workers;
mw->requestWorkerListString(workers, 5000);
```

---

## USD Loading

```cpp
vector<MeshData> meshes;

// From buffer
mw->LoadUSDBuffer(data, "model.usda", meshes);

// From disk
mw->LoadUSDFromDisk("/path/to/model.usda", meshes);

// With collision
mw->LoadUSDBufferWithCollision(data, "model.usda",
    ECollisionComplexity::Simplified, meshes);
```

---

## Collision Configuration

```cpp
mw->setDefaultCollisionComplexity(ECollisionComplexity::ConvexHull);
mw->setCollisionParameters(0.25f, 0.001f, 32);
auto cpx = mw->getDefaultCollisionComplexity();
```

---

## Textures

```cpp
// Decode raw image
TextureData tex = mw->CreateTextureFromBuffer(imageData);

// Gradient as PNG
mw->WriteGradientLineAsPNG(gradientData, "/tmp/grad.png");
vector<uint8_t> pngBuf;
mw->GetGradientLineAsPNGBuffer(gradientData, pngBuf);

// Cached gradient
vector<uint8_t> data; int w, h;
mw->GetCachedGradientTexture(data, w, h);
```

---

## MeshData Structure

```cpp
struct MeshData {
    string elementName;           // USD prim name
    string typeName;              // "Xform", "Mesh", etc.
    vector<float>   points;       // Flat xyz...
    vector<uint32_t> indices;     // Tri indices
    vector<float>   normals;      // Flat xyz...
    vector<float>   uvs;          // Flat uv...
    vector<float>   vertex_colors;// Flat rgba...
    CollisionData   collision;

    // USD features
    string subdivisionScheme;     // "catmull-clark", "bilinear", "none"
    bool   doubleSided;
    vector<uint32_t> faceVertexCounts;
    vector<vector<float>> uvSets;
    vector<string> uvSetNames;

    // Helpers
    bool isValid() const;
    size_t getVertexCount();
    size_t getTriangleCount();
    size_t getUVSetCount();
    bool hasSubdivision();
    void clear();
};
```

---

## FileInfo Structure

```cpp
struct FileInfo {
    string   name;
    uint64_t size;
    int32_t  source_rank;
    uint64_t hash128[2];   // XXH3-128
};
```
