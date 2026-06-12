# Known Issues & Dead Code

Audit of code paths that are unreachable, duplicated, or incomplete as found in a full source review.

---

## ✅ Fixed (2025-06-11)

The following issues have been patched in source:

1. **glm→flat array conversion** — Changed from `push_back` per component to `resize` + direct index writes. ~4x faster for large meshes.
2. **MemoryMonitor TOCTOU race** — `canAllocate()` check moved inside `allocate()` lock. Public `canAllocate()` now reads system memory + local budget.
3. **MemoryMonitor Linux MemAvailable** — Now reads `/proc/meminfo MemAvailable` (includes reclaimable caches). Windows (`GlobalMemoryStatusEx`) unchanged.
4. **ZmqConnector dead context** — `initialize()` no longer creates a ZMQ context. DEALER-only stub mode documented.
5. **ROUTER receiver dead code** — `receiveFile()` and `receiveAnyMessage()` replaced with one-line stubs that return false + log warning.
6. **VectorMemoryPool** — Removed all 50 lines of dead code from `UsdProcessor.cpp`.
7. **CMake duplicate TBB block** — Removed duplicate at lines 186-199.
8. **ZmqConnector regex** — Changed `std::regex` to `static const std::regex` in `validateTcpEndpoint()`.
9. **ParallelDownloader** — Header now has compile-time `#error` guard pointing to `ParallelDownloadManager`. `.cpp` is not in CMake build.
10. **validateGeometry double-pass** — Merged bounds + validation into single pass per attribute type.

### Memory: Copy Elimination (Applied)
The USD parsing chain was audited and optimized for unnecessary copies:
- **Geometry path `LoadUSDBuffer`**: `buffer → string → vector` (2 copies) → 1 copy. "0: None" fix applied on a single string intermediary.
- **Small file `preprocessUsdContent`** (< 10MB): Line split + rebuild (N allocations) → in-place `string::replace` + newline-scanning line-33 patch. 3 allocs → 1.
- **Clip extraction**: `ExtractClipsFromRawContent` gets `string` overload. Geometry path passes `fixedContent` through `resolveReferences`, skipping re-copy from vector.
- **`validateUSDFormat`**: Uses `string_view` instead of `std::string` — no copy for format check.
- **Mesh strings**: `elementName`/`typeName` use `std::move` in `convertMeshDataWithCollision`.

---

## Dead Code (Unreachable)

### ROUTER Receiver Thread
**Files**: `ZmqConnector.cpp:78`, `AnariUsdMiddleware.cpp:310-343`

`ZmqConnector::initialize()` no longer creates a ZMQ context. `zmqContext` and `zmqSocket` remain declared in the header for compile compatibility but are always null. `receiveFile()` and `receiveAnyMessage()` return false immediately.

`AnariUsdMiddleware::Impl::startReceiving()` sets `running = true` but never spawns a thread. This is intentional — the public API expects the method to exist (C API, tools call it).

Consequently:
- `sendReply()`, `receiveMessagePart()`, `drainRemainingParts()` — private helper functions, unreachable
- `configurePlatformSpecificSocket()` — unreachable, `zmqSocket` is always null
- `processedFiles` duplicate tracking — unreachable from receiver path

**Design decision needed**: DEALER-only (remove ROUTER code) or dual-mode (re-enable ROUTER).

### VectorMemoryPool — REMOVED
Previously at `UsdProcessor.cpp:600-652`. Dead code deleted.

---

## Incomplete Implementations

### CollisionProcessor Stubs
**File**: `CollisionProcessor.cpp`

| Method | Status |
|---|---|
| `generateBoundingBoxCollision()` | Implemented |
| `generateComplexCollision()` | Implemented |
| `generateConvexHullCollision()` | Simplified — copies first 300 indices (not true convex hull) |
| `generateSimplifiedCollision()` | Implemented via `decimateMesh` — sampling decimation, not QEM |
| `generateConvexDecomposition()` | Falls back to convex hull |

The header declares these methods in the `ECollisionComplexity` enum. ConvexHull/Simplified have simplified implementations. ConvexDecomp falls back to ConvexHull.

---

## Minor Inconsistencies

- `external/ZeroMQ/` vs `external/zmq/` — potentially duplicate submodules
- `AnariUsdMiddleware.cpp` line 1162-1173: `requestFileListWithSizes` has excessive debug logging leftover from debugging
- `test_middleware.cpp` references `startReceiving()` which is now a no-op (test still works)

See [[Reference/Edit Guide]] for where to fix each remaining item.
