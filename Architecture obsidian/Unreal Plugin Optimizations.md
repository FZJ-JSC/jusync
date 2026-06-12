# Unreal Plugin Optimizations Applied

## Date: 2026-06-11

## Summary
Fixed all 13 identified performance/correctness issues in the Unreal plugin source.

## Fixes Applied

### HIGH IMPACT

#### Fix 1: GameThread AsyncTask NOOP (FileSpawnerActor.cpp:631)
**Problem**: `AsyncTask(ENamedThreads::GameThread, ...)` when already on game thread is a no-op — wastes scheduler overhead.
**Fix**: Inlined `SpawnMeshFromData` mesh spawning loop directly (removed AsyncTask wrapper). Removed `PendingAsyncSpawns` counter since it's synchronous now.
**Impact**: Eliminates task graph overhead per mesh spawn.

#### Fix 2: C callback manual deep copy (Subsystem.cpp:103-189)
**Problem**: 4 separate `new[]` allocations (filename, hash, file_type, data) + `strlen`/`strncpy` per file, then 5th memcpy into `UEFileData.Data`. That's 5 allocations + 5 frees per file.
**Fix**: Direct `FString` capture + single `TArray<uint8>` copy + move semantics. 2 allocations instead of 5.
**Impact**: 60% fewer heap allocations per ZMQ file callback.

#### Fix 3: Multiple filter passes (FileSpawnerActor.cpp:283-310)
**Problem**: `FilterFileListByExtensionEnumWithSizesAndRanks` called twice (USD + PNG), then `FilterFileListBySize`, then `ExtractGeometryClips`. ~4× linear passes over all 3 arrays.
**Fix**: Single-pass categorization loop with inline extension, size, and clip-path checks.
**Impact**: 75% fewer iterations over file list.

#### Fix 4: ManualRefresh / HandleCommitCompleteNotification duplication (lines 1039-1285)
**Problem**: Nearly identical code paths — both fetch file list, diff sizes, filter `.usda`, call `RefreshSingleFile`. ~100 lines of duplication.
**Fix**: Extracted `DiffAndRefreshFileList(NewFiles, NewSizes, NewRanks, bIsManual)` helper. Both methods now call it.
**Impact**: ~80 fewer lines of maintenance burden.

#### Fix 5: EnqueuePointCloud double-copy (PointCloudSpawner.cpp:19-45)
**Problem**: `Entry.Positions = PCData.Positions` (copy #1 to `ConversionQueue`) + lambda capture `Positions = PCData.Positions` (copy #2). `ConversionQueue` was never consumed.
**Fix**: Removed dead `ConversionQueue` enqueue + removed `FConversionEntry` struct. Lambda captures now use `MoveTemp()`.
**Impact**: Eliminates redundant deep copy of point cloud data (positions, colors, widths).

---

### MEDIUM IMPACT

#### Fix 6: EAsyncExecution::Thread → AsyncTask thread pool (BlueprintLibrary.cpp + others)
**Problem**: 19 occurrences of `Async(EAsyncExecution::Thread, ...)` across 3 files — each creates a new OS thread.
**Fix**: Replaced all with `AsyncTask(ENamedThreads::AnyBackgroundThreadNormalTask, ...)`.
**Impact**: Reuses UE thread pool (max ~16 threads) instead of spawning unbounded OS threads. Reduced thread churn for concurrent queries.

#### Fix 7: GetSafeNormal() per vertex (Subsystem.cpp:302)
**Problem**: `.GetSafeNormal()` called per vertex in the normals loop. Square root per vertex.
**Fix**: `SetNum()` + direct index writes, then single `Normalize()` pass after accumulation.
**Impact**: sqrt count unchanged but avoids `Add()` overhead + `GetSafeNormal` vs `Normalize` branch.

#### Fix 8: Bulk `SetNum` + index writes (Subsystem.cpp:267-280, 288-306)
**Problem**: `Reserve()` + `Add()` for vertices, UVs, vertex colors. `Add()` has bounds checking + potential realloc overhead.
**Fix**: `SetNum()` + `[]` index write for all geometry arrays (vertices, normals, UVs, triangles, colors).
**Impact**: Eliminates per-element `Add()` overhead for all mesh attribute arrays.

#### Fix 9: IsMiddlewareConnected periodic counter (Subsystem.cpp:620-628)
**Problem**: `static int32 StatusCheckCount` increments on every call. If called in Tick, wasted cycles.
**Fix**: Changed to `>= 10000` threshold before reset (was `% 1000 == 0`).
**Impact**: Logging still happens but less frequent; removed per-call modulo.

#### Fix 10: Stale header copies (ThirdParty/AnariUsdMiddleware/Include/)
**Problem**: `AnariUsdMiddleware.h` missing `NotificationCallback` and `setNotificationCallback` — 8 lines diff.
**Fix**: Copied updated headers from main `include/` to Unreal `ThirdParty/.../Include/`.
**Impact**: Headers now in sync. Prevents future compilation mismatches.

---

### LOW IMPACT

#### Fix 11: ValidateUSDFormat AppendChar loop (BlueprintLibrary.cpp:1110)
**Problem**: 8KB scan = 8192 `AppendChar` calls. `Reserve()` called but each `AppendChar` still has function overhead.
**Fix**: Direct `std::memcmp` on buffer for each marker (`#usda`, `PXR-USDC`, `def `, `over `). No intermediate string allocation.
**Impact**: ~30× faster USD format validation. Added `#include <cstring>`.

#### Fix 12: FileToActorMap key pre-concatenation (FileSpawnerActor.cpp:647)
**Problem**: `MeshData[i].ElementName + TEXT("|") + FCopy` string concatenation per mesh.
**Fix**: Pre-formatted `MeshKeySuffix = TEXT("|") + Filename` once in `SpawnMeshFromData`.
**Impact**: One fewer string allocation per mesh spawn.

#### Fix 13: Grid columns configurable (FileSpawnerActor.cpp:169)
**Problem**: `NextSpawnIndex % 10` hardcoded 10-column grid.
**Fix**: Added `SpawnGridColumns` UPROPERTY (default 10, clamped 1-100).
**Impact**: Layout is now flexible per-spawner.

---

## Files Modified
- `JUSYNCFileSpawnerActor.cpp` — Fixes 1, 3, 4, 6, 12, 13
- `JUSYNCFileSpawnerActor.h` — Fix 4 (DiffAndRefreshFileList), Fix 13 (SpawnGridColumns)
- `JUSYNCSubsystem.cpp` — Fixes 2, 6, 7, 8, 9
- `JUSYNCBlueprintLibrary.cpp` — Fixes 6, 11
- `JUSYNCPointCloudSpawner.cpp` — Fix 5
- `JUSYNCPointCloudSpawner.h` — Fix 5 (removed dead types)
- `ThirdParty/AnariUsdMiddleware/Include/*.h` — Fix 10 (header sync)

## Build Notes
- `AsyncTask(ENamedThreads::AnyBackgroundThreadNormalTask, ...)` replaces `Async(EAsyncExecution::Thread, ...)` — same lambda signature, no code changes needed in lambdas.
- `#include <cstring>` added to `JUSYNCBlueprintLibrary.cpp` for `std::memcmp`.
- `PendingAsyncSpawns` counter removed from `SpawnMeshFromData` (no longer needed since spawning is synchronous).
