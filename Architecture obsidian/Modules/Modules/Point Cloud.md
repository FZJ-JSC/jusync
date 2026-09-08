# Point Cloud & Gradient Baking

JUSYNC extracts two geometry types from USD stages: **meshes** (GeomMesh) and **point clouds** (GeomPoints). Point clouds are used for particle visualizations with baked-in gradient colors.

## Files
- `src/UsdProcessor.cpp` — `ExtractPointCloudData()`, `BakeColorsFromGradient()`
- `include/UsdProcessor.h` — `PointCloudData` struct

## Point Cloud Extraction

`ExtractPointCloudData(geomPoints, outData, worldTransform)`:

1. Reads `positions` (`point3f[]`) — transforms by world matrix (CPU or GPU)
2. Reads `normals` (optional, `normal3f[]`)
3. Reads `widths` (optional, `float[]` — per-point radius)
4. Reads `attrib0` (optional, scalar values → mapped to gradient colors)

### Gradient Color Baking

When a point cloud has scalar attributes (`attrib0`) and a gradient texture is available:

```
attrib0[i] ∈ [0.0, 1.0]  →  sample gradient texture at position
gradient pixel at (attrib0[i] * (texWidth - 1), 0) → RGBA color
```

`BakeColorsFromGradient(pointCloud, gradientRGBA, texWidth)`:
- `attrib0` scalar maps to horizontal position in 1-pixel-high gradient texture
- Result: `vertex_colors` — per-point baked RGBA for direct rendering

## PointCloudData Structure

```cpp
struct PointCloudData {
    string elementName;          // Prim name
    string typeName;             // Always "GeomPoints"
    vector<glm::vec3> positions;
    vector<glm::vec4> vertex_colors;   // Baked from gradient
    vector<glm::vec3> normals;
    vector<float> widths;        // Per-point radii
    vector<glm::vec2> scalarAttributes; // attribute0: [scalar, 0]
    vector<string> uvSetNames;

    size_t getPointCount() const;
    bool hasColors() const;
    bool hasNormals() const;
    bool isValid() const; // name non-empty + positions non-empty
};
```

## Gradient Texture Cache

Middleware caches gradient/colormap textures received over ZMQ:

```cpp
// In AnariUsdMiddleware::Impl
std::map<string, vector<uint8_t>> cachedTextures;  // filename → raw PNG data
std::mutex textureCacheMutex;
```

When a file with type `"IMAGE"` is received, it's cached. `GetCachedGradientTexture()` returns the most recently cached texture.

### API
```cpp
bool WriteGradientLineAsPNG(buffer, outPath);         // Decode + write to disk
bool GetGradientLineAsPNGBuffer(buffer, outPng);      // Decode + encode to PNG bytes
bool GetCachedGradientTexture(outData, outW, outH);   // Get last cached gradient
```

Height-2 images (gradient stripes) are detected and the top row is extracted (height → 1).

See [[Modules/Color Interpolation]] for mesh vertex color handling.
