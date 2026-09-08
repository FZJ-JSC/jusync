# Message Protocol

The JUSYNC network protocol uses binary-structured messages over ZeroMQ multipart frames. All messages begin with a `uint32_t` magic number `0x55534446` (`"USDF"`).

## Files
- `include/AnariUsdMessages.h` (482 lines)

## Magic Number
```cpp
constexpr uint32_t ANARI_USD_MAGIC = 0x55534446; // "USDF"
```
Every message header starts with this value. `MessageUtils::isValidMagic()` validates incoming frames.

## Message Types

```cpp
enum class ZmqMessageType : uint32_t {
    // ── Worker Registration ──
    WORKER_READY        = 1,
    WORKER_HEARTBEAT    = 2,
    BROKER_ACK          = 10,

    // ── Worker Queries ──
    REQ_WORKER_STATUS   = 20,
    REQ_WORKER_COUNT    = 21,
    REQ_WORKER_LIST     = 24,
    RESP_WORKER_STATUS  = 22,
    RESP_WORKER_COUNT   = 23,
    RESP_WORKER_LIST    = 25,

    // ── File Operations ──
    REQ_LIST_FILES      = 100,    // List available files
    REQ_GET_FILE        = 101,    // Request specific file
    REQ_GET_FRAME       = 102,    // Request all files for a frame
    REQ_GET_PROPERTY    = 400,    // Query broker property

    // ── File Responses ──
    RESP_FILE_LIST      = 200,
    RESP_FILE_CHUNK     = 201,    // Chunked file data
    RESP_FILE_COMPLETE  = 202,    // File transfer complete
    RESP_NO_FILE        = 203,    // File not found
    RESP_ERROR          = 204,
    RESP_PROPERTY       = 401,

    // ── Push Notifications ──
    NOTIFY_FILE_UPDATE      = 300,  // File changed on worker
    NOTIFY_COMMIT_COMPLETE  = 301,  // Scene commit finished
};
```

## Message Structures (packed, `#pragma pack(push, 1)`)

### `ZmqFileRequest` — Request a file
```
Offset  Size  Field
0       4     magic (0x55534446)
4       4     message_type (101)
8       4     request_id
12      4     target_rank (-1 = all)
16      256   filename (null-terminated)
272     4     chunk_size (0 = default 4MB)
```
Total: 276 bytes header + binary payload in separate frame.

### `ZmqFileChunk` — Received file chunk
```
0       4     magic
4       4     message_type (201)
8       4     request_id
12      4     source_rank
16      256   filename
272     8     file_size
280     8     chunk_offset
288     4     chunk_size
```
Followed by `chunk_size` bytes of data.

### `ZmqFileListResponse` — File listing
```
0       4     magic
4       4     message_type (200)
8       4     request_id
12      4     source_rank
16      4     file_count
```
Followed by `file_count × 256` bytes of filenames.

### `ZmqFileComplete` — Transfer done
```
0       4     magic
4       4     message_type (202)
8       4     request_id
12      4     source_rank
16      256   filename
272     8     total_size (bytes transferred)
```

### `ZmqWorkerStatusRequest`
```
0       4     magic
4       4     message_type (20 or 21)
8       4     request_id
12      4     target_rank (-1 = all workers)
```

### `ZmqWorkerStatusResponse`
```
0       4     magic
4       4     message_type (22 or 23)
8       4     request_id
12      4     source_rank
16      4     worker_count
20      4     worker_status (0=offline, 1=idle, 2=busy, 3=error)
24      8     last_heartbeat (Unix timestamp)
32      64    hostname
96      128   gpu_info
```

### `ZmqWorkerInfo` (inside WorkerListResponse)
```
0       4     rank
4       64    hostname
68      64    ip_address
```

### `ZmqErrorResponse`
```
0       4     magic
4       4     message_type (204)
8       4     request_id
12      4     error_code
16      256   error_message
```

### `ZmqPropertyResponse`
```
0       4     magic
4       4     message_type (401)
8       4     request_id
12      4     property_type (0=int32, 1=string, -1=error)
16      4     int_value
20      256   string_value
```

### `ZmqFileNotification` (push notification)
```
0       4     magic
4       4     message_type (300 or 301)
8       4     source_rank
12      256   filename
268     8     file_size
276     8     timestamp
284     16    hash128[2] (XXH3-128, two uint64)
```

## Helper Structs

### `FileInfo`
```cpp
struct FileInfo {
    std::string name;
    uint64_t    size;
    int32_t     source_rank;
    uint64_t    hash128[2];  // XXH3-128
};
```

### `MessageUtils`
```cpp
isValidMagic(uint32_t)        // == 0x55534446
isValidMessageType(uint32_t)  // 1..401
isNotificationType(uint32_t)  // 300 or 301
getMessageTypeName(uint32_t)  // Enum → string
```

## Protocol Flow Examples

### Request File
```
Client                          Broker                        Worker
  │                               │                             │
  │── ZmqFileRequest (101) ──────▶│                             │
  │   { file: "model.usda" }      │── Forward ─────────────────▶│
  │                               │                             │
  │                               │◀── File data (chunked) ─────│
  │◀── ZmqFileChunk (201) ───────│                             │
  │     { offset, data... }       │                             │
  │     ... (more chunks)         │                             │
  │◀── ZmqFileComplete (202) ────│                             │
  │     { totalSize }             │                             │
```

### Notification (Push)
```
Worker                         Broker                          Client
  │                               │                             │
  │── NOTIFY_FILE_UPDATE (300) ──▶│                             │
  │                               │── Forward (300) ───────────▶│
  │                               │                             │
```
