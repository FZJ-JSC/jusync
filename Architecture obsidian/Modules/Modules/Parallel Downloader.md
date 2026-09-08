# Parallel Downloader

Two related classes handle parallel file downloads:
- `ParallelDownloader` — ⚠️ **Broken** (calls non-existent `requestFileAsync`). Compile guard added. `.cpp` not in CMake build.
- `ParallelDownloadManager` — ✅ Active. Streaming pipeline with RAM awareness and immediate spawning.

## Files
| File | Class | Purpose | Build? |
|---|---|---|---|
| `include/ParallelDownloader.h` | `ParallelDownloader` | Simple parallel download API | Header only (#error guard) |
| `include/ParallelDownloadManager.h` | `ParallelDownloadManager` | RAM-aware streaming with spawn | ✅ Yes |
| `src/ParallelDownloader.cpp` | `ParallelDownloader` impl | Not compiled (never added to CMake) | ❌ No |
| `src/ParallelDownloadManager.cpp` | Manager | Implementation | ✅ Yes |

> Use `AnariUsdMiddleware::requestFilesParallelAsync()` which internally uses `ParallelDownloadManager`. Do NOT use `ParallelDownloader`.

## ParallelDownloader (High-Level)

### API
```cpp
// Sync (returns results)
vector<DownloadResult> downloadFiles(
    filenames, targetRank, maxParallel, timeoutMs);

// Async (callbacks)
void downloadFilesAsync(
    filenames, targetRank,
    ProgressCallback,   // (filename, downloaded, total, activeTransfers)
    CompletionCallback, // (DownloadResult)
    maxParallel, timeoutMs);

// Streaming (RAM-aware)
void downloadWithStreaming(
    filenames, targetRank,
    maxMemoryBytes, maxParallel, timeoutMs,
    onFileReady,   // Called when each file is ready
    onComplete);   // Called when all done

// Control
void cancelAll();
bool waitForCompletion(timeoutMs);
size_t getActiveDownloadCount();
double getAverageSpeed();
```

### DownloadResult
```cpp
struct DownloadResult {
    string filename;
    vector<uint8_t> data;
    bool success;
    string error;
    uint64_t size;
    chrono::milliseconds duration;
};
```

### Internal Structure
- `DownloadTask` per file: `promise<DownloadResult>`, `future`, tracking state
- `pollThread_` — monitors completion, calls callbacks
- `tasksMutex_` / `tasksCV_` — thread-safe task queue

## ParallelDownloadManager (Streaming, RAM-Aware)

Designed for HPC pipelines where many large USD files must be downloaded without blowing RAM.

### Key Design Decisions
1. **Immediate spawn** — File callback fires as soon as download completes, not after all files are done
2. **RAM awareness** — `MemoryMonitor` gates new download starts based on available system RAM
3. **Memory release** — Memory freed immediately after spawn callback returns
4. **Single socket** — Uses one DEALER socket with request-ID multiplexing

### API
```cpp
ParallelDownloadManager(shared_ptr<AnariUsdClient> client, size_t maxParallel = 0);

void downloadFilesStreaming(
    filenames, targetRanks,
    spawnCallback,      // (filename, data) — IMMEDIATE on each completion
    completionCallback, // () — when ALL done
    errorCallback,      // (filename, error)
    timeoutMs);

void stop();                    // Graceful shutdown
bool isRunning();
Stats getStats();
```

### Stats
```cpp
struct Stats {
    size_t total_files_requested;
    size_t files_downloaded;
    size_t files_spawned;
    size_t download_errors;
    size_t total_bytes_downloaded;
    float memory_usage_percentage;
};
```

### Architecture

```
┌──────────────────────────────────────────────────────────────┐
│                ParallelDownloadManager                        │
│                                                               │
│  downloadFilesStreaming() ──▶ pending_downloads queue        │
│                                    │                          │
│                  ┌─────────────────┼─────────────────┐       │
│                  │                 │                 │       │
│            ┌─────▼─────┐   ┌──────▼──────┐  ┌──────▼──────┐│
│            │ Scheduler  │   │ Worker  1   │  │ Worker  N   ││
│            │ Thread     │   │ Thread      │  │ Thread      ││
│            │            │   │             │  │             ││
│            │ RAM check  │   │ Send REQ   │  │ Send REQ   ││
│            │ Schedule   │   │ on ZMQ    │  │ on ZMQ    ││
│            └─────┬──────┘   └──────┬─────┘  └──────┬──────┘│
│                  │                 │                │       ││
│                  └─────────────────┼────────────────┘       │
│                                    │                        │
│                   ┌────────────────▼────────────────┐       │
│                   │      Dispatcher Thread           │       │
│                   │  (owns ZMQ recv, demux by       │       │
│                   │   request_id → DownloadContext)  │       │
│                   └────────────────┬────────────────┘       │
│                                    │                        │
│                   ┌────────────────▼────────────────┐       │
│                   │  active_downloads map            │       │
│                   │  {request_id → shared_ptr<      │       │
│                   │    DownloadContext>}             │       │
│                   │  + MemoryMonitor                 │       │
│                   └────────────────┬────────────────┘       │
│                                    │                        │
│                   ┌────────────────▼────────────────┐       │
│                   │  spawnFileImmediately()          │       │
│                   │  → Callback (filename, data)     │       │
│                   │  → cleanupCompletedDownload()    │       │
│                   │  → Memory::release(size)         │       │
│                   └─────────────────────────────────┘       │
└──────────────────────────────────────────────────────────────┘
```

### Threads
| Thread | Purpose |
|---|---|
| **Dispatcher** | Receives ZMQ responses, demultiplexes by request ID, accumulates chunks |
| **Scheduler** | Drains `pending_downloads` queue, checks RAM via `MemoryMonitor`, starts new workers |
| **Workers** | Send file requests, handle responses, accumulate data |

### DownloadContext
```cpp
struct DownloadContext {
    uint32_t request_id;
    string filename;
    int32_t target_rank;
    size_t estimated_size;
    time_point start_time;
    vector<uint8_t> accumulated_data;  // Chunk accumulator
    uint64_t expected_file_size, received_bytes;
    bool completed, failed;
    string error_message;
    FileSpawnCallback spawn_callback;
    ErrorCallback error_callback;
};
```
