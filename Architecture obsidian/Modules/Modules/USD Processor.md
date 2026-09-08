# USD Processor

`UsdProcessor` — Parses USD files and extracts mesh geometry, textures, and point clouds. Wraps **TinyUSDZ** for lightweight, no-dependency USD parsing.

## Files
- `include/UsdProcessor.h` (573 lines)
- `src/UsdProcessor.cpp`

## Architecture

```
USD Buffer (.usd/.usda/.usdc/.usdz)
        │
        ▼
┌─────────────────────────────────┐
│       UsdProcessor (PIMPL)      │
│                                 │
│  ┌─────────────────────────┐   │
│  │  TinyUSDZ Stage/Prims   │   │
│  │  ProcessPrim() walk     │   │
│  │  ┌───────┐ ┌────────┐  │   │
│  │  │Mesh   │ │Points  │  │   │
│  │  │Extract │ │Extract │  │   │
│  │  └───────┘ └────────┘  │   │
│  └─────────────────────────┘   │
│                                 │
│  Transform pipeline:           │
│  ┌──────┐ ┌──────┐ ┌──────┐   │
│  │Verts │ │Nrm   │ │UV    │   │
│  │GPU   │ │GPU   │ │GPU   │   │
│  └──────┘ └──────┘ └──────┘   │
│  (fallback: CPU)              │
└─────────────────────────────────┘
        │
        ▼
   MeshData[]  PointCloudData[]
```

## Public API

### Core Loading
| Method | Return | Description |
|---|---|---|
| `LoadUSDBuffer(buffer, fileName, outMeshes, outCloud, progressCb)` | `bool` | Parse USD from memory buffer. Optional point cloud extraction. |
| `LoadUSDFromDisk(filePath, outMeshes, progressCb)` | `bool` | Parse USD from filesystem. |

### Texture
| Method | Return | Description |
|---|---|---|
| `CreateTextureFromBuffer(buffer, format)` | `TextureData` | STB-image decode. Supports PNG, JPEG. |

### Configuration
| Method | Description |
|---|---|
| `setMaxRecursionDepth(int)` | USD hierarchy depth limit (default 1000) |
| `setMemoryLimit(MB)` | Memory budget in MB |
| `setReferenceResolutionEnabled(bool)` | Auto-resolve USD references/payloads (default true) |

### Stats & Validation
| Method | Return | Description |
|---|---|---|
| `getProcessingStats()` | `ProcessingStats::Snapshot` | Thread-safe atomic snapshot |
| `resetProcessingStats()` | `void` | Reset counters |
| `validateUSDFormat(buffer, fileName)` | `bool` | Validate magic bytes / structure |
| `getSupportedExtensions()` | `vector<string>` | `.usd .usda .usdc .usdz` |

## Internal Processing Pipeline

### 1. Preprocess
`preprocessUsdContent(buffer)` — Fixes common USD issues before parsing.

### 2. Stage Load
TinyUSDZ opens the staged buffer and returns a `tinyusdz::Stage` root prim.

### 3. Prim Walk
`ProcessPrim(prim, outMeshes, parentTransform, depth)` recursively walks the prim hierarchy:
- Validates recursion depth (`maxRecursionDepth`, default 1000)
- For each prim, computes local transform via `GetLocalTransform()`
- Multiplies with parent transform for world-space matrix
- Validates determinant to reject singular matrices
- Dispatches to `ExtractMeshData()` for `GeomMesh`, or `ExtractPointCloudData()` for `GeomPoints`

### 4. Mesh Extraction
`ExtractMeshData(mesh, outMeshData, worldTransform)`:
1. Reads vertex positions (`points`)
2. Reads face vertex counts → triangulates to `indices` (groups of 3)
3. Reads normals → transforms by world matrix
4. Reads UV coordinates → `extractUVCoordinates()` searches multiple primvar names
5. Reads vertex colors → `extractVertexColors()`
6. Reads subdivision scheme (`catmull-clark`, `bilinear`, `none`)
7. Reads doubleSided flag
8. Normalizes UVs: `normalizeUVCoordinatesParallel()` (TBB-parallel when available)
9. Validates indices: `validateMeshIndices()`

### 5. Point Cloud Extraction
`ExtractPointCloudData(geomPoints, outData, worldTransform)`:
- Reads positions, widths, scalar attributes
- Bakes colors from gradient texture: `BakeColorsFromGradient()` maps `attribute0` scalars → RGBA via colormap

### 6. Reference Resolution
If enabled, `resolveReferences()` resolves:
- USD `References` — embedded `.usda`/`.usdc` payloads
- USD `Payloads` — lazy-loaded external files
- USD `Clips` — animation clip references (pattern-matched from raw content)

### 7. GPU Acceleration (Optional)
When `ENABLE_CUDA_ACCELERATION` is defined and GPU available:
- Vertices ≥ 10,000 → `GpuKernels::transformVertices()`
- Normals ≥ 10,000 → `GpuKernels::transformNormals()`
- UVs ≥ 10,000 → `GpuKernels::processUVs()`
- Results validated by `GpuValidation` against CPU reference with tolerance 1e-5

## Data Structures

### `MeshData` (internal, glm-based)
```cpp
struct MeshData {
    std::string elementName, typeName;
    std::vector<glm::vec3>  points;
    std::vector<uint32_t>   indices;
    std::vector<glm::vec3>  normals;
    std::vector<glm::vec2>  uvs;
    std::vector<glm::vec4>  vertex_colors;
    // USD features
    std::string  subdivisionScheme;
    bool         doubleSided;
    std::vector<uint32_t>   faceVertexCounts;
    std::vector<std::vector<glm::vec2>> uvSets;
    std::vector<std::string>   uvSetNames;

    // Helpers
    bool isValid();           // Bounds, size, triangle count % 3
    bool validateGeometry();  // Cross-check indices vs vertex count
    glm::vec3 getBounds();    // AABB min/max
    MeshData toMiddlewareMeshData(); // Convert to flat float arrays
};
```

### `PointCloudData`
```cpp
struct PointCloudData {
    std::string elementName, typeName;
    std::vector<glm::vec3>  positions;
    std::vector<glm::vec4>  vertex_colors;  // Baked from gradient
    std::vector<glm::vec3>  normals;
    std::vector<float>      widths;
    std::vector<glm::vec2>  scalarAttributes;  // attribute0 values
    std::vector<std::string> uvSetNames;
};
```

### `ProcessingStats` (atomic, non-copyable)
```cpp
struct ProcessingStats {
    std::atomic<uint64_t> filesProcessed;
    std::atomic<uint64_t> meshesExtracted;
    std::atomic<uint64_t> texturesProcessed;
    std::atomic<uint64_t> referencesResolved;
    std::atomic<uint64_t> processingErrors;
    std::atomic<uint64_t> totalBytesProcessed;
    // Delete copy. Move via store(). Snapshot for reading.
    Snapshot getSnapshot() const;
};
```

## Safety

- `MIDDLEWARE_VALIDATE_POINTER(ptr, "UsdProcessor")` macros guard null pointers
- `validateTransform()` rejects NaN/Inf matrix values
- `validateFilePath()` blocks path traversal
- `checkMemoryLimit()` enforces budget before allocation
- Mesh bounds: `MAX_MESH_VERTICES`, `MAX_MESH_INDICES` from `safety` namespace

## Critical "0: None" Fix

TinyUSDZ fails to parse USD files containing `0: None` (empty timeSampled arrays emitted by USD ArrayWriter). A mandatory string replacement is applied **before any parsing**:

```cpp
// Applied to ALL files, always
while (content.find("0: None") != npos)
    replace("0: None" → "0: []");
```

Without this fix, large geometry files fail with `MeshCount=0, CloudCount=0`. The fix is applied early in `LoadUSDBuffer()` before checking for geometry vs non-geometry paths.

## Dead Code

~~`VectorMemoryPool` (UsdProcessorImpl, ~50 lines)~~ — **Removed**. Was a pool of 10 pre-allocated `PooledVector` structs for recycled mesh data. Never called by any code. See [[Reference/Known Issues & Dead Code]].

## Memory Optimizations (Applied)

The USD parsing chain was profiled for unnecessary copies. Fixes applied:

- **Geometry path**: `LoadUSDBuffer` applies "0: None" fix on a single `std::string` intermediary, then converts to `vector<uint8_t>` once. Previously: `buffer → string → vector` (2 copies). Now: 1 copy.
- **Small file path** (`preprocessUsdContent`, < 10MB): Line splitting + rebuild (N allocations) replaced with in-place `string::replace` + manual line-33 patch via newline scanning. 3 allocations → 1.
- **Clip extraction**: `ExtractClipsFromRawContent` now has `extractClipsFromString(const string&)` overload. Geometry path passes the `fixedContent` string through `resolveReferences`, avoiding re-copy from `vector<uint8_t>`.
- **validateUSDFormat**: Uses `string_view` instead of `std::string` — no copy for format check.
- **Mesh string members**: `elementName`/`typeName` use `std::move` in `convertMeshDataWithCollision`.

## Parallel Algorithm Policy

Vertex/normal/UV transformation uses `std::transform` with parallel execution policy when available:

```cpp
#if defined(__clang__)
    #define PAR_POLICY          // libc++ has no parallel execution policies
#else
    #define PAR_POLICY std::execution::par,  // GCC: parallel
#endif
```
