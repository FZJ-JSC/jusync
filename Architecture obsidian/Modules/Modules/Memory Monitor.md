# Memory Monitor

`MemoryMonitor` — Tracks system RAM usage and guards against memory exhaustion during parallel downloads.

## Files
- `include/MemoryMonitor.h` (74 lines)
- `src/MemoryMonitor.cpp`

## Purpose
Used by `ParallelDownloadManager` to prevent RAM exhaustion when downloading many large USD files concurrently.

## API

```cpp
explicit MemoryMonitor(size_t system_reserve_bytes = 1_GB);

// Queries (static)
static size_t getTotalSystemMemory();

// Queries
size_t getAvailableMemory() const;    // Current free RAM
bool canAllocate(size_t) const;       // Would allocation fit?
size_t getAllocatedMemory() const;    // Currently tracked by this monitor
float getUsagePercentage() const;     // 0..100

// Tracking
bool allocate(size_t);   // Reserve (pre-allocation gate)
void release(size_t);    // Free (post-spawn cleanup)
```

## How It Works

1. On construction, reads total system RAM (`getTotalSystemMemory`)
2. Reserves `system_reserve_bytes` (default 1 GB) for OS / host app
3. `canAllocate(N)` returns true only if `available - reserved >= N + allocated`
4. `allocate(N)` atomically increments `allocated_memory` if within budget
5. `release(N)` atomically decrements after consumer is done with data

## Platform-Specific Memory Query
- **Linux**: Reads `/proc/meminfo MemAvailable` (includes reclaimable caches). Falls back to `sysinfo.freeram` if MemAvailable not found.
- **Windows**: Uses `GlobalMemoryStatusEx().ullAvailPhys`
- Fallback: returns total system memory (conservative)

## Integration
Called from `ParallelDownloadManager::downloadSchedulerThread()`:
```cpp
if (memory_monitor.canAllocate(downloads.estimated_size)) {
    memory_monitor.allocate(downloads.estimated_size);
    // start worker thread
}
// Later, after spawn callback:
memory_monitor.release(downloads.accumulated_data.size());
```
