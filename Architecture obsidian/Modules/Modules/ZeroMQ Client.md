# ZeroMQ Client (AnariUsdClient)

`AnariUsdClient` — DEALER socket client that connects to an ANARI USD broker to request files from HPC workers.

## Files
- `include/AnariUsdClient.h` (304 lines)
- `src/AnariUsdClient.cpp`

## Role
Serves as the **outbound** network interface. The middleware can act as both ROUTER server (`ZmqConnector`) and DEALER client (`AnariUsdClient`) simultaneously.

## Socket Type
- **DEALER** — Connects to a broker's ROUTER socket
- Sends structured binary messages (`ZmqFileRequest`, `ZmqFileListResponse`, etc.)
- Uses request IDs for correlation

## Dispatcher Thread -- The Key Design

ZeroMQ DEALER sockets are **NOT safe for concurrent `recv()` on the same socket**. The solution is a dispatcher pattern:

```
┌─────────────────────────────────────────────────────┐
│                   AnariUsdClient                     │
│                                                      │
│  ┌──────────────┐       ┌────────────────────────┐  │
│  │ Request      │       │  Dispatch Thread        │  │
│  │ Threads (N)  │       │                        │  │
│  │              │       │  while(dispatcherActive)│  │
│  │  ┌────────┐  │       │    zmqSocket.recv()    │  │
│  │  │Send REQ│  │       │    enqueue to queue    │  │
│  │  └────┬───┘  │       │                        │  │
│  │       │ wait │       └──────────┬─────────────┘  │
│  │       ▼ for    │                    │ enqueue      │
│  │  ┌────────┐   │         responseQueue (thread-safe)│
│  │  │Dequeue │   │                    │ dequeue      │
│  │  └────────┘   │       └────────────┴─────────────┘  │
└─────────────────────────────────────────────────────┘
```

- `dispatchThread` runs `dispatcherThread()` — continuously polls `recv()` and enqueues frame pairs into `responseQueue`
- Request threads call `waitForMatchingFrames(requestId, timeout)` — they **never** touch `recv()`
- `recvMutex` serializes all receive operations as an extra safety layer

## Connection Management

```cpp
bool connect(const char* brokerEndpoint, int timeoutMs = 5000);
void disconnect(int gracefulTimeoutMs = 1000);
bool isConnected() const;
ConnectionStatus getConnectionStatus() const;
```

## File Request API

### Async (Callback-based)
```cpp
bool requestFileList(rank, FileListCallback cb, timeoutMs);
bool requestFileListWithSizes(rank, FileListWithSizesCallback cb, timeoutMs);
bool requestFile(filename, rank, chunkCb, completeCb, errorCb, timeoutMs);
bool requestFrame(frameNumber, rank, chunkCb, completeCb, errorCb, timeoutMs);
```

### Sync (Blocking)
```cpp
bool getFileSync(filename, rank, outData, timeoutMs);
bool getFileListSync(rank, outFiles, timeoutMs);
bool getFileListWithSizesSync(rank, outFiles, timeoutMs);
```

### Parallel
```cpp
bool requestFilesParallel(
    filenames, targetRanks,
    spawnCallback,    // Called per completed file
    completionCallback,
    errorCallback,
    timeoutMs);
```

## Worker Status Queries

### Async
```cpp
requestWorkerStatus(rank, WorkerStatusCallback, timeout)
requestWorkerCount(WorkerCountCallback, timeout)
```

### Sync
```cpp
getWorkerStatusSync(rank, outWorkers, timeout)
// Returns vector of (rank, status:0-3, hostname, gpuInfo, heartbeat)

getWorkerCountSync(outCount, timeout)
getTotalWorkerCountSync(outCount, timeout)  // Legacy string protocol, includes rank 0
requestWorkerListString(outWorkers, timeout) // Python-broker compatible
```

## Notifications

```cpp
void setNotificationCallback(NotificationCallback cb);
```
Receives push notifications from broker:
- `NOTIFY_FILE_UPDATE` (type 300) — A file was updated on a worker
- `NOTIFY_COMMIT_COMPLETE` (type 301) — Scene commit finished

## Request ID System

```cpp
std::atomic<uint32_t> nextRequestId{1};
std::map<uint32_t, bool> pendingRequests;
uint32_t generateRequestId();
```
Each outbound request gets a monotonic ID. The dispatcher matches inbound frames by this ID.

## Callback Types

```cpp
FileChunkCallback:     (filename, chunkData, offset, totalSize)
FileCompleteCallback:  (filename, totalSize)
FileListCallback:      (filenames)
FileListWithSizesCallback: (FileInfo[])
ErrorCallback:         (errorMessage)
NotificationCallback:  (msgType, sourceRank, filename, fileSize, timestamp)
WorkerStatusCallback:  (rank, status, hostname, gpuInfo, heartbeat)
WorkerCountCallback:   (totalWorkers)
```

## Connection Statistics

```cpp
struct ConnectionStats {
    std::atomic<uint64_t> totalRequestsSent;
    std::atomic<uint64_t> totalResponsesReceived;
    std::atomic<uint64_t> totalBytesReceived;
    std::atomic<uint64_t> failedRequests;

    Snapshot getSnapshot() const;
    void reset();
};
```

## Response Queue

```cpp
std::queue<std::shared_ptr<std::pair<
    vector<uint8_t>,     // delimiter frame
    vector<uint8_t>      // data frame
>>> responseQueue;

std::mutex responseQueueMutex;
std::condition_variable responseQueueCv;
```

Methods: `enqueueResponseFrame()`, `waitForMatchingFrames()`, `tryDequeueMatching()`

See [[Modules/Network Layer]] for ROUTER side (`ZmqConnector`), [[Modules/Message Protocol]] for wire format.
