# Collision Processor

`CollisionProcessor` — Generates simplified collision geometry from visual mesh data. Designed for Unreal Engine physics integration (UBodySetup, PhysicsHandle, etc.).

## Files
- `include/CollisionProcessor.h` (414 lines)
- `src/CollisionProcessor.cpp`

## Collision Complexity Levels

```cpp
enum class ECollisionComplexity : uint8_t {
    None         = 0,   // No collision generation
    Simple       = 1,   // Axis-aligned bounding box
    ConvexHull   = 2,   // Convex hull around mesh
    Complex      = 3,   // Full mesh triangle soup (default)
    Simplified   = 4,   // Decimated mesh for performance
    ConvexDecomp = 5,   // V-HACD convex decomposition
};
```

| Level | Algorithm | Use Case | Performance | Status |
|---|---|---|---|---|
| `None` | — | Decorative/static no-physics | N/A | ✅ Working |
| `Simple` | Min/max bounding box + sphere | Static, distant objects | Fastest | ✅ Working |
| `ConvexHull` | Copies first 300 indices (simplified) | Moving objects, characters | Fast | ⚠️ Simplified — not true convex hull |
| `Complex` | Direct triangle soup | Static accurate collision | Slow, high memory | ✅ Working |
| `Simplified` | Sampling decimation (not QEM) | Moving, complex shapes | Medium | ⚠️ Simplified — sampling, not QEM |
| `ConvexDecomp` | Falls back to ConvexHull | Concave moving objects | Slow | ⚠️ Simplified — fallback |

## Data Structures

### `CollisionData`
```cpp
struct ANARI_USD_MIDDLEWARE_API CollisionData {
    ECollisionComplexity collisionType;

    // General collision mesh
    std::vector<float>    vertices;     // Flat x,y,z...
    std::vector<uint32_t> indices;      // Triangle indices

    // Simple collision
    glm::vec3 boundingBoxMin, boundingBoxMax;
    glm::vec3 sphereCenter;
    float     sphereRadius;

    // Convex hull
    std::vector<float>    convexVertices;
    std::vector<uint32_t> convexIndices;

    bool isValid() const;     // Bounds + finite check
    void clear();
    size_t getVertexCount() const;
    size_t getTriangleCount() const;
    bool hasConvexHull() const;
    size_t getMemoryUsage() const;

private:
    bool validateFiniteValues() const;  // All coords are std::isfinite()
};
```

### `MeshDataWithCollision`
```cpp
struct ANARI_USD_MIDDLEWARE_API MeshDataWithCollision {
    // Visual mesh
    std::string elementName, typeName;
    std::vector<float>    points, normals, uvs, vertex_colors;
    std::vector<uint32_t> indices;
    // Collision
    CollisionData collisionData;
};
```

## Public API

### Generation
```cpp
bool generateCollision(
    const vector<float>&  vertices,       // Input visual mesh
    const vector<uint32_t>& indices,      // Input visual mesh
    ECollisionComplexity complexity,
    CollisionData& outCollisionData       // Output
);

bool generateCollisionForMeshes(
    vector<MeshDataWithCollision>& meshes,
    ECollisionComplexity complexity
);  // Batch processing, multi-threaded

bool generateCollisionForMesh(
    MeshDataWithCollision& mesh,
    ECollisionComplexity complexity
);  // Single mesh convenience
```

### Configuration
```cpp
void setSimplificationRatio(float);       // Target % of original tris (default 0.25)
void setConvexHullPrecision(float);      // Hull approximation error (default 0.001)
void setMaxConvexHulls(int);             // Max hulls for V-HACD (default 32)
void setMaxProcessingTime(float);        // Timeout in seconds (default 30)
void setQualityVsPerformance(float);     // 0.0=perf, 1.0=quality (default 0.5)
void setMultiThreadingEnabled(bool);     // Enable TBB parallel batch (default true)
setProgressCallback(fn);                 // 0.0..1.0 progress
```

### Metadata (Static Helpers — Useful for Unreal Blueprints)
```cpp
static bool isValidComplexity(ECollisionComplexity);
static string getComplexityName(ECollisionComplexity);
static string getComplexityDescription(ECollisionComplexity);
static string getComplexityRecommendation(ECollisionComplexity);
```

## Internal Algorithms

| Method | Algorithm |
|---|---|
| `generateBoundingBoxCollision()` | Min/max sweep over vertices + bounding sphere |
| `generateConvexHullCollision()` | Convex hull from input vertices |
| `generateComplexCollision()` | Copy mesh with index optimization |
| `generateSimplifiedCollision()` | `decimateMesh()` → reduce triangle count |
| `generateConvexDecomposition()` | V-HACD into convex parts |

## Unreal Integration

```cpp
namespace CollisionUtils {
    ANARI_USD_MIDDLEWARE_API
    bool ConvertToUnrealFormat(const CollisionData&, void* unrealBodySetup);

    ANARI_USD_MIDDLEWARE_API
    double EstimateProcessingTime(vertexCount, triangleCount, complexity);

    ANARI_USD_MIDDLEWARE_API
    ECollisionComplexity GetRecommendedComplexity(vertexCount, triangleCount, isStatic);

    ANARI_USD_MIDDLEWARE_API
    bool ValidateMeshForCollision(vertices, indices, string& errorMessage);
}
```

## Statistics

```cpp
struct ProcessingStats {
    uint64_t collisionsGenerated;
    uint64_t generationErrors;
    uint64_t totalVerticesProcessed;
    uint64_t totalTrianglesProcessed;
    double   averageProcessingTime;
    double   totalProcessingTime;
};
ProcessingStats getProcessingStats() const;
void resetProcessingStats();
```

## Safety
- All vertex coordinates validated with `std::isfinite()` 
- Vertex/indices bounded by `safety::MAX_MESH_VERTICES * 3` and `safety::MAX_MESH_INDICES`
- Processing time capped at `maxProcessingTime_` (default 30s)
