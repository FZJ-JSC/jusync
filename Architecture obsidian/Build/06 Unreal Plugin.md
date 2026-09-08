# Unreal Plugin

JUSYNC ships as an Unreal Engine 5 plugin for real-time USD streaming and RealtimeMesh integration.

## Location
```
UnrealPlugin/JUSYNC/
```

## Purpose
Exposes `AnariUsdMiddleware` as a UE5 plugin so artists and HPC pipelines can stream live USD geometry into Unreal via the RealtimeMesh system (RMC).

## Integration Points

### Middleware Logging Bridge
`MiddlewareLogging.h` auto-detects Unreal compilation:
```cpp
#if defined(_MSC_VER) && defined(__UNREAL__)
    #define MIDDLEWARE_LOG_INFO(format, ...) UE_LOG(LogTemp, Display, TEXT(format), ##__VA_ARGS__)
    #define MIDDLEWARE_VALIDATE_POINTER FPlatformMemory::IsValidPointer(ptr)
#else
    // Standard C++ macros
#endif
```

This means all middleware logs appear in the **Unreal Output Log** and **Windows Event Viewer** (via `OutputDebugStringA`).

### RealtimeMesh Compatibility
- `safety::MAX_MESH_VERTICES` and `MAX_MESH_INDICES` are set to essentially unlimited (`MAX_INT64 / 2`) to accommodate RMC's ability to handle very large geometry
- MeshData includes `subdivisionScheme`, `doubleSided`, `faceVertexCounts`, `uvSets` — all data that RealtimeMesh expects
- Collision data (`CollisionData`) is structured for direct conversion to `UBodySetup`

### Memory Safety for UE
- `MIDDLEWARE_VALIDATE_POINTER` uses `FPlatformMemory::IsValidPointer(ptr)` — UE's pointer validation
- `MIDDLEWARE_SAFE_DELETE` null-safe delete + nullptr assignment
- `ANARI_USD_MIDDLEWARE_API` export macros handle DLL export correctly for UE plugin loading

### Callbacks to Game Thread
Middleware callbacks fire on background threads. The plugin bridges them to the UE game thread using:
- `AsyncTask(ENamedThreads::GameThread, ...)` — for RMC spawning
- `FGraphEventAsync` — fire-and-forget operations

---

## Using the Plugin

1. Copy `UnrealPlugin/JUSYNC/` into your project's `Plugins/` folder
2. Enable plugin in UE Editor
3. Link `libanari_usd_middleware.so` (Linux) or `anari_usd_middleware.dll` (Windows) to your project
4. Use Blueprint or C++ to:
   - Initialize middleware
   - Connect to broker
   - Register callbacks
   - Spawn RealtimeMeshes from received MeshData

## Build Integration

The middleware library is built separately (via CMake) and the plugin links against it. RPATH is set to `$ORIGIN` on Linux so UE can find `.so` at plugin load time.

See [[Modules/Logging System]] for UE-specific logging macros.
