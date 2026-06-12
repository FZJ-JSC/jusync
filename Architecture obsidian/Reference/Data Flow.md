# Data Flow -- End-to-End

This note traces a single USD file from network ingress to consumer callback, thread by thread.

---

## Path A: ROUTER Reception (File Pushed by Broker)

```
Broker (Python)                    ZmqConnector           AnariUsdMiddleware         Consumer
     │                                  │                        │                      │
     │── ZMQ multipart:                │                        │                      │
     │  Frame 0: identity              │                        │                      │
     │  Frame 1: "model.usda"          │                        │                      │
     │  Frame 2: <binary data>         │                        │                      │
     │  Frame 3: <hash string>         │                        │                      │
     │────────────────────────────────▶│  [receiver thread]     │                      │
     │                                  │                        │                      │
     │                                  │ receiveFile()          │                      │
     │                                  │ deserialize frames     │                      │
     │                                  │                        │                      │
     │                                  │ HashVerifier::verify   │                      │
     │                                  │    (XXH3-128)          │                      │
     │                                  │                        │                      │
     │                                  │ FileUpdateCallback     │                      │
     │                                  │ ─────────────────────━│─▶ notify consumer    │
     │                                  │                        │                      │
     │                                  │                        │ Consumer calls:      │
     │                                  │                        │ LoadUSDBuffer() ─────│─▶
     │                                  │                        │                      │
     │                                  │                        │ [main/UI thread]     │
     │                                  │                        │ UsdProcessor:        │
     │                                  │                        │  preprocessUsd()     │
     │                                  │                        │  TinyUSDZ stage      │
     │                                  │                        │  ProcessPrim walk    │
     │                                  │                        │  ExtractMeshData     │
     │                                  │                        │  GpuKernels (if >=10K)│
     │                                  │                        │  CollisionProcessor  │
     │                                  │                        │                      │
     │                                  │                        │ vector<MeshData> ───────▶
     │                                  │                        │ returned to consumer  │
```

---

## Path B: DEALER Client (Request File from HPC Worker)

```
Consumer                    AnariUsdClient              Broker              Worker
   │                              │                           │                    │
   │ requestFile()                │                           │                    │
   │─────────────────────────────▶│                           │                    │
   │                              │  dispatcher thread:       │                    │
   │                              │  owns recv()              │                    │
   │                              │                           │                    │
   │                              │ REQ_GET_FILE (101)        │                    │
   │                              │──────────────────────────▶│──────────────────▶ │
   │                              │                           │                    │
   │                              │◀── RESP_FILE_CHUNK (201)  │                    │
   │                              │   dispatcher enqueues     │                    │
   │                              │   to responseQueue        │                    │
   │                              │                           │                    │
   │                              │◀── RESP_FILE_CHUNK (201)  │                    │
   │                              │                           │                    │
   │                              │◀── RESP_FILE_COMPLETE     │                    │
   │                              │                           │                    │
   │                              │ FileCompleteCallback      │                    │
   │                              │───────────────────────────│─▶ notify consumer  │
```

---

## Path C: Parallel Streaming Download

```
Consumer                  ParallelDownloadManager                  Broker
   │        dispatcher + scheduler + workers (threads)               │
   │              │   │   │                                          │
   │ requestFiles │   │   │                                          │
   │ ParallelAsync│   │   │  REQ 1, REQ 2, REQ 3 (parallel)         │
   │─────────────━│───│───│─────────────────────────────────────────▶│
   │   spawnCb    │   │   │                                          │
   │ ◀────────────│───│───│── RESP CHUNK 1a ── RESP CHUNK 2a ──────│
   │              │   │   │  Worker accumulates chunks               │
   │ ◀────────────│───│───│── RESP CHUNK 1b ── COMPLETE 1 ──────────│
   │   spawnCb    │   │   │  File 1 spawned IMMEDIATELY              │
   │ ◀────────────│───│───│── RESP CHUNK 2b                          │
   │              │   │   │                                          │
   │ ◀────────────│───│───│── COMPLETE 2 ── COMPLETE 3 ─────────────│
   │   completeCb │   │   │  Files 2 & 3 spawned                    │
   │◀─────────────│───│───│                                          │
```

### RAM Guard
`MemoryMonitor` sits between scheduler and workers:
```
scheduler: pending download (estimated 50MB)
   → MemoryMonitor.canAllocate(50MB)?
       YES → MemoryMonitor.allocate(50MB) → start worker
       NO  → wait, re-check when scheduler loops
After spawn: MemoryMonitor.release(actualSize)
```

---

## Thread Affinity Summary

| Operation | Thread | Blocking? |
|---|---|---|
| ROUTER `recv()` | ZmqConnector receiver thread | Yes |
| DEALER `recv()` | AnariUsdClient dispatcher | Yes |
| DEALER `send()` | Any request thread | Yes (short timeout) |
| `responseQueue` dequeue | Any request thread | Yes (with CV wait) |
| `LoadUSDBuffer()` | Caller's thread | Yes (CPU-bound) |
| GpuKernels transform | Caller's thread (CUDA kernel launches async) | Sync or async |
| Collision generation | Caller's thread or TBB workers | Yes |
| Callback dispatch | Receiver/dispatcher thread | — |
