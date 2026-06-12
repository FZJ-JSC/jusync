# Color Interpolation Modes

USD vertex colors use different interpolation semantics. JUSYNC detects and handles all three.

## Detection Logic

In `AnariUsdMiddleware::Impl::convertMeshDataWithCollision()`:

```cpp
colorCount = processorMeshData.vertex_colors.size()
pointCount = processorMeshData.points.size()
faceCount  = processorMeshData.indices.size() / 3

if colorCount == pointCount  → VERTEX interpolation
if colorCount == faceCount   → UNIFORM (per-face) interpolation
else                          → FALLBACK (pad/truncate)
```

## VERTEX Mode (`colorCount == pointCount`)

Direct 1:1 mapping. Each color corresponds to a vertex.

```cpp
for each glm::vec4 color:
    output.push_back(r, g, b, a);
```

## UNIFORM Mode (`colorCount == faceCount`)

Color is per-face. Must expand to per-vertex by assigning face color to all 3 vertices of each triangle:

```cpp
vertexColors[pointCount] = default white (1.0, 1.0, 1.0, 1.0)

for faceIdx = 0 to faceCount - 1:
    i0 = indices[faceIdx * 3 + 0]
    i1 = indices[faceIdx * 3 + 1]
    i2 = indices[faceIdx * 3 + 2]

    if i0 < pointCount: vertexColors[i0] = faceColor
    if i1 < pointCount: vertexColors[i1] = faceColor
    if i2 < pointCount: vertexColors[i2] = faceColor
```

**Note**: If two faces share a vertex, the last-face's color wins. This is correct for flat shading but may cause seams for smooth-shaded objects.

## FALLBACK Mode (`colorCount` ≠ either)

Treat as vertex-colored with padding:

```cpp
for i = 0 to pointCount - 1:
    if i < colorCount: copy color[i]
    else: default white (1, 1, 1, 1)
```

Warns in log: `"Color count mismatch - using fallback vertex mapping"`

## Output Format

All modes produce the same flat `vector<float>` layout: `[r0, g0, b0, a0, r1, g1, b1, a1, ...]`

Compatible with Unreal Engine vertex color attributes.

See [[Modules/Point Cloud]] for gradient-based point cloud color baking.
