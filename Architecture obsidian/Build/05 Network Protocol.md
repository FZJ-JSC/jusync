# Network Protocol (ZeroMQ Wire Format)

Complete description of the JUSYNC ZeroMQ wire protocol, message types, and protocol flows.

---

## Transport

| Transport | Format | Description |
|---|---|---|
| **TCP** | `tcp://host:port` | Default, cross-machine |
| **IPC** | `ipc://path` | UNIX domain socket |
| **In-Proc** | `inproc://name` | In-process testing |

Default receiver port: **5556**. Auto-fallback to 5557, 5558 on bind failure.

---

## Socket Roles

```
┌──────────────┐      ROUTER/DEALER      ┌──────────────────┐
│   HPC Worker │ ◀─────────────────────  │  Receiver         │
│  DEALER      │    Sends file data     │  ZmqConnector     │
│  socket      │                         │  (bind ROUTER)    │
└──────────────┘                         └──────────────────┘

┌──────────────┐      DEALER/ROUTER      ┌──────────────────┐
│   Receiver   │ ◀─────────────────────  │  Broker           │
│AnariUsdClient│   Requests files        │  Python ROUTER    │
│  (DEALER)    │                         └──────────────────┘
└──────────────┘
```

---

## Binary Wire Format

All struct messages use `#pragma pack(push, 1)` — no padding.

### Every Message Header
```
┌────┬────┬────┐
│ magic │ type │  ... fields ...
│ 0x55534446 │ uint32  │
└────┴────┴────┘
```

### Multipart ZeroMQ Frames
A file transfer is split across multiple ZMQ frames:
```
Frame 0: ZmqFileRequest header (276 bytes packed struct)
Frame 1: Binary file data (N bytes)
```

---

## Message Types (Complete)

### Worker Lifecycle
| Type | ID | Direction | Struct |
|---|---|---|---|
| `WORKER_READY` | 1 | Worker → Broker | `ZmqWorkerReady` |
| `WORKER_HEARTBEAT` | 2 | Worker → Broker | — |
| `BROKER_ACK` | 10 | Broker → Worker | `ZmqBrokerAck` |

### Worker Queries
| Type | ID | Direction | Struct |
|---|---|---|---|
| `REQ_WORKER_STATUS` | 20 | Client → Broker | `ZmqWorkerStatusRequest` |
| `REQ_WORKER_COUNT` | 21 | Client → Broker | `ZmqWorkerStatusRequest` |
| `REQ_WORKER_LIST` | 24 | Client → Broker | `ZmqWorkerStatusRequest` |
| `RESP_WORKER_STATUS` | 22 | Broker → Client | `ZmqWorkerStatusResponse` |
| `RESP_WORKER_COUNT` | 23 | Broker → Client | `ZmqWorkerStatusResponse` |
| `RESP_WORKER_LIST` | 25 | Broker → Client | `ZmqWorkerListResponse` |

### File Operations
| Type | ID | Direction | Struct |
|---|---|---|---|
| `REQ_LIST_FILES` | 100 | Client → Broker | `ZmqFileRequest` |
| `REQ_GET_FILE` | 101 | Client → Broker | `ZmqFileRequest` |
| `REQ_GET_FRAME` | 102 | Client → Broker | `ZmqFileRequest` |
| `REQ_GET_PROPERTY` | 400 | Client → Broker | `ZmqFileRequest` |

### File Responses
| Type | ID | Direction | Struct |
|---|---|---|---|
| `RESP_FILE_LIST` | 200 | Broker → Client | `ZmqFileListResponse` |
| `RESP_FILE_CHUNK` | 201 | Broker → Client | `ZmqFileChunk` + data frame |
| `RESP_FILE_COMPLETE` | 202 | Broker → Client | `ZmqFileComplete` |
| `RESP_NO_FILE` | 203 | Broker → Client | — |
| `RESP_ERROR` | 204 | Broker → Client | `ZmqErrorResponse` |
| `RESP_PROPERTY` | 401 | Broker → Client | `ZmqPropertyResponse` |

### Push Notifications
| Type | ID | Direction | Struct |
|---|---|---|---|
| `NOTIFY_FILE_UPDATE` | 300 | Broker → Client | `ZmqFileNotification` |
| `NOTIFY_COMMIT_COMPLETE` | 301 | Broker → Client | `ZmqFileNotification` |

---

## Protocol Flows

### Request File (Chunked Transfer)
```
  Client                          Broker                          Worker
    │
    │── REQ_GET_FILE (101): "model.usda", rank=0
    │   [ZmqFileRequest: 276 bytes] ─────────────────────────────▶│
    │                                                             │
    │                                                             │── Open file
    │                                                             │
    │◀── RESP_FILE_CHUNK (201): offset=0, 4MB ──────────────────│
    │     [header + 4MB data]                                     │
    │◀── RESP_FILE_CHUNK (201): offset=4MB, 4MB ────────────────│
    │     [header + 4MB data]                                     │
    │◀── RESP_FILE_CHUNK (201): offset=8MB, 512KB ──────────────│
    │     [header + tail data]                                    │
    │◀── RESP_FILE_COMPLETE (202): totalSize=8519680 ────────────│
    │
```
Default chunk size: **4 MB** (`DEFAULT_CHUNK_SIZE = 4 * 1024 * 1024`). Configurable via `ZmqFileRequest.chunk_size`.

### Push Notification
```
  Worker                         Broker                          Client
    │                               │
    │── NOTIFY_FILE_UPDATE (300):   │
    │  "model.usda", size, hash ───▶│── Forward ───────────────▶│
    │                               │                             │
    │                               │                       Triggers
    │                               │                       NotificationCallback
    │                               │                       Consumer decides
    │                               │                       whether to re-download
```

---

## Hash Verification

- **Hash algorithm**: XXH3-128 (128-bit, non-cryptographic, ~15 GB/s)
- **Hash location**: `ZmqFileNotification.hash128[2]` (two `uint64_t`: low + high)
- **Verification**: `HashVerifier::verifyHash128(data, expectedLo, expectedHi)`
- **On mismatch**: Log calculated vs expected, reject file

---

## Error Handling

```
ZmqErrorResponse:
  error_code    int32_t   Application-level error code
  error_message [256]     Human-readable description
```

Timeouts:
- Default file request: **30 seconds**
- Default frame request: **60 seconds**
- Default worker query: **5 seconds**
- Default file list: **10 seconds**

See [[Modules/Message Protocol]] for struct field layouts.
