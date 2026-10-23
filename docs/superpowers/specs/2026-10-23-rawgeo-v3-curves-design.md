# RawGeo v3 — Curve Topology for Streamlines

- **Status:** Implemented on `feat/rawgeo-v3-curves`
- **Date:** 2026-10-23
- **Branches:** `feat/rawgeo-v3-curves` in `ANARI-USD`, `jusync`, `jusync-uesample`
- **Related:** `docs/plans/realtime-streaming.md`, `docs/plans/milestone-2-lidar-squeeze.md`

---

## 1. Goal

Let the ANARI-USD sender stream **linear curve geometry** (polylines) outside the USD
path, so JUSYNC can render streamlines / pathlines / fiber strands without paying
TinyUSDZ parsing, zstd decompression, or text-USD bandwidth.

RawGeo currently bypasses USD for **points only** (the `UsdBridgeInstancerData`
overload). Curves have `points + curveVertexCounts + widths`, so the v3 payload
carries one extra topology section.

Success means:

1. A sender-side streamline dataset arrives in UE as tubes with no USD parse.
2. `v1` and `v2` payloads still parse byte-for-byte as before.
3. Correct behavior with **1 rank, single-process MPI, and multi-rank MPI**, including
   per-rank subset merges with no cross-rank contamination.

## 2. Non-goals

- Triangle-mesh RawGeo (meshes keep using the USD path).
- Stable per-point / per-curve ids for cross-timestep particle tracking.
- Cubic / Bézier curve bases. The sender forces `type = linear`
  (`UsdBridgeUsdWriter_Geometry.cpp:368`).
- Host-side vector-field integration. Streamlines are produced on the HPC side.
- Changing `UsdBridgeCurveData`, the USD writer, or the middleware's transport.

## 3. Current state

| Fact | Location |
|---|---|
| `JUSYNC_RAWGEO_MAGIC = "JSG1"`, versions 1 and 2 | `jusync/include/AnariUsdProtocol.h:374-375` |
| `JusyncRawGeoHeaderV1` frozen; current header = v2 (lattice fields appended) | `:387-451` |
| `header_bytes(ver)` returns `sizeof(V1)` for 1, **`sizeof(current)` otherwise** | `:425-428` |
| ANARI-USD ships **v1 only** and never sets the lattice fields | `ANARI-USD/AnariUsdProtocol.h:373-413` |
| Sender RawGeo is implemented for instancer points only | `UsdBridgeUsdWriter.h:189-193`, `.cpp:1846` |
| Sender decimates by stride; it does **not** emit `_s<k>` subsets | `UsdBridgeUsdWriter.cpp:1868-1987` |
| Client merges v2 point subsets into one cloud | `JUSYNCSubsystem.cpp:1522-1992` |
| Client strips `_s<k>` before time bucketing | `JUSYNCAnimationController.cpp:179-191` |
| Client detects `_s<k>` subsets by filename | `JUSYNCFileSpawnerActor.cpp:125-159` |
| UE element identity is `prim_name + rank` | `JUSYNCElementNames.h:169-173` |
| UE has no line renderer; RealtimeMesh is triangles only | `RealtimeMeshVertexFactory.h:68` |

Two consequences drive the design:

1. `header_bytes()` must gain an explicit v2 case, or v2 payloads will be misread
   as soon as v3 fields are appended.
2. The sender must learn the subset lattice, because only the client has it today.

## 4. Wire format

### 4.1 Versioning

Three frozen layouts, append-only:

```cpp
inline constexpr uint32_t JUSYNC_RAWGEO_MAGIC   = 0x3147534A; // "JSG1"
inline constexpr uint32_t JUSYNC_RAWGEO_VERSION = 3;

struct JusyncRawGeoAttrDesc {          // unchanged, 40 bytes
    char     name[32];
    uint32_t components;               // 1..4 floats per element
    uint32_t pad;
};

struct JusyncRawGeoHeaderV1 {          // FROZEN, version 1
    uint32_t magic;
    uint32_t version;
    char     prim_name[64];
    uint64_t num_points;
    double   time;
    uint32_t position_components;      // == 3
    uint32_t color_components;         // 0 | 3 | 4
    uint32_t num_attributes;
    uint32_t attr_uniform_count;
    uint64_t payload_bytes;
};

struct JusyncRawGeoHeaderV2 {          // FROZEN, version 2
    // ... all V1 fields, same order ...
    uint64_t subset_index;
    uint64_t subset_count;
    uint64_t full_num_points;
};

struct JusyncRawGeoHeader {            // CURRENT, version 3
    // ... all V2 fields, same order ...
    uint32_t topology;                 // 0 = points, 1 = linear curves
    uint32_t curve_count;              // curves in THIS payload
    uint64_t full_curve_count;         // curves in the source frame
    uint32_t width_components;         // 0 = absent, 1 = one float per point
    uint32_t flags;                    // must be 0

    static size_t header_bytes(uint32_t ver)
    {
        switch (ver) {
            case 1:  return sizeof(JusyncRawGeoHeaderV1);
            case 2:  return sizeof(JusyncRawGeoHeaderV2);
            default: return sizeof(JusyncRawGeoHeader);
        }
    }
    bool valid(const void* data, size_t totalBytes) const;
};
```

The header is inside `#pragma pack(push, 1)` (`AnariUsdProtocol.h:83-568`), so the
layouts are exact. Required compile-time assertions:

```cpp
static_assert(offsetof(JusyncRawGeoHeaderV2, subset_index) == sizeof(JusyncRawGeoHeaderV1));
static_assert(offsetof(JusyncRawGeoHeader,    topology)    == sizeof(JusyncRawGeoHeaderV2));
static_assert(sizeof(JusyncRawGeoHeaderV1) == 112, "v1 wire size");
static_assert(sizeof(JusyncRawGeoHeaderV2) == 136, "v2 wire size");
static_assert(sizeof(JusyncRawGeoHeader)   == 160, "v3 wire size");
static_assert(sizeof(JusyncRawGeoAttrDesc) == 40,  "attr descriptor size");
```

### 4.2 Section layout

After the header and the `num_attributes × JusyncRawGeoAttrDesc` table, sections
appear in this fixed order with no padding:

| # | Section | Size | Present when |
|---|---|---|---|
| 1 | `positions` | `num_points × 3 × f32` | always |
| 2 | `colors` | `num_points × color_components × f32` | `color_components != 0` |
| 3 | `attributes` | `num_points × Σ attr[i].components × f32` | `num_attributes != 0` |
| 4 | `curve_vertex_counts` | `curve_count × u32` | `topology == 1` |
| 5 | `widths` | `num_points × width_components × f32` | `width_components != 0` |

`payload_bytes` is the sum of every present section. `attr_uniform_count` **must
equal** `num_points` for all versions (per-point attributes only).

### 4.3 `valid()`

Reject unless all hold:

- `magic` matches and `version` is 1, 2, or 3.
- `position_components == 3`.
- `color_components ∈ {0, 3, 4}`.
- `attr_uniform_count == num_points`.
- Version ≥ 2: `subset_count ≥ 1`, `subset_index < subset_count`,
  `full_num_points ≥ num_points`.
- Version 3:
  - `topology ∈ {0, 1}` and `flags == 0` and `width_components ∈ {0, 1}`.
  - `topology == 0` ⇒ `curve_count == 0`, `full_curve_count == 0`,
    `width_components == 0` (pure v2 behavior).
  - `topology == 1` ⇒ `curve_count ≤ full_curve_count`; and when
    `full_curve_count > 0`, `curve_count ≥ 0` (empty subsets are legal).
  - `topology == 1` ⇒ `num_points ≥ 2 × curve_count` (cheap lower bound; the exact
    `Σ curve_vertex_counts == num_points` check belongs to the data parser).
- Exact byte math:

```text
table    = header_bytes(version) + num_attributes * 40
pos      = num_points * 3 * 4
col      = num_points * color_components * 4
attr     = num_points * Σ descs[i].components * 4
curvesec = (version == 3 && topology == 1) ? curve_count * 4 : 0
widthsec = (version == 3) ? num_points * width_components * 4 : 0
totalBytes >= table + pos + col + attr + curvesec + widthsec
payload_bytes == pos + col + attr + curvesec + widthsec
```

Version 1 implicitly means `subset_index = 0`, `subset_count = 1`,
`full_num_points = num_points`, `topology = 0`, `width_components = 0`.

**Empty subsets are valid:** `num_points == 0`, `curve_count == 0`,
`payload_bytes == 0` is a legal v3 curve payload.

### 4.4 Subset semantics (curve-granular)

For `topology == 1` and `subset_count = S`:

- Subset `k` carries **whole curves** whose source ordinal is `k + i·S`,
  for `i = 0 … curve_count-1`.
- The number of curves in subset `k` is `ceil((C - k) / S)` where
  `C = full_curve_count`. `curve_count` must equal that value.
- No ordinal array is transmitted: ordinals are derivable from `k`, `S`, and `i`.
- The coarse preview is subset `0`: *fewer streamlines, each at full shape*. It is
  always a valid polyline set, never a chord approximation.
- Reassembly: allocate `C` curve slots, scatter each payload's curves into
  `k + i·S`, concatenate slots in ordinal order once all `S` payloads are present.
  The result is point-for-point identical to a single `S = 1` payload.
- `S = 1` **must not** append `_s0` to the filename (see 4.5).

Point-subset semantics (v2, `topology == 0`) are unchanged: subset `k` carries
source points `i` with `(i % S) == k`.

### 4.5 Filenames

The sender derives the RawGeo key from the clip stage name, replacing the USD
extension with `.geo` (`UsdBridgeUsdWriter.cpp:1859-1864`). Clip names are built as:

```
clips/<Name>_Geom_ + ("_r<rank>" if mpiSize > 1) + "_<time>" + .usda|.usd
```

Therefore:

| Case | Filename |
|---|---|
| non-MPI or `mpisize == 1`, `S == 1` | `clips/Foo_Geom__7.geo` |
| non-MPI or `mpisize == 1`, `S > 1` | `clips/Foo_Geom__7_s<k>.geo` |
| MPI `mpisize > 1`, `S == 1` | `clips/Foo_Geom__r3_7.geo` |
| MPI `mpisize > 1`, `S > 1` | `clips/Foo_Geom__r3_7_s<k>.geo` |

`_Geom_` + `_<time>` is the `"__"` that `JUSYNCIsSubsetStreamFile` looks for; the
`_s<k>` tail is stripped by `DetectTimeStepOrder` before time parsing. No client
naming changes are required.

## 5. MPI and non-MPI contract

The sender writes to a **rank-local** `g_rankMemoryStore`; the broker aggregates
per rank. Rank identity therefore lives in the filename and in the file-list /
notification metadata, never in `prim_name` (which is identical across ranks).

Required invariants:

1. **Merge key is the filename base with `_s<k>` removed**, e.g.
   `Foo_Geom__r3_7`. Never `prim_name`, never `prim_name + time`.
2. **Element key is `MakeRankedElementKey(prim_name, rank)`**, as today.
   `JUSYNCElementNames::Parse` strips the `_s<k>` tail before rank/time parsing, and
   `JUSYNCBuildLogicalActorKey` strips it from the time mark too, so the coarse and
   completed frames resolve to the same UE element/actor.
3. **Rank resolution order** for v3 curves:
   a. rank parsed from `_r<digits>_` in the filename (authoritative, present exactly
      when `mpisize > 1`);
   b. `ResolveFileRank(Filename, KnownRank)`;
   c. `0`.
   This prevents an early subset from rank 3 being filed under rank 0 before the
   file list is available.
4. **No curves on a rank** (`full_curve_count == 0`): emit nothing, no files, no
   notifications.
5. **`0 < C < S`**: emit all `S` payloads; empty ones carry `curve_count = 0`.
   Completeness must be computable without heuristics.
6. **`subset_count` is per rank.** Rank-scoped merge keys make env drift harmless.
   Log a warning if two payloads of one base disagree on `S`.
7. **No arrival-order assumptions.** Any `k` may arrive first or last.
8. **Eviction.** Incomplete merges expire via the existing budget/LRU
   (`jusync.RawGeo.RefineBudgetKPoints`) plus a max-age sweep (default 30 s).
9. **Single-rank MPI** (`MpiSize == 1`) produces no `_r` suffix and behaves exactly
   like non-MPI. This must be an explicit test case.

## 6. Sender design (ANARI-USD)

### 6.1 New overload

```cpp
bool TryStoreRawGeo(UsdBridgePrimCache* cacheEntry,
                    const UsdBridgeCurveData& data, double timeStep);
```

The generic template continues to return `false`, so meshes are unaffected.

### 6.2 Gates and env

| Variable | Default | Meaning |
|---|---|---|
| `JUSYNC_RAWGEO` | `0` | existing master switch |
| `JUSYNC_RAWGEO_CURVES` | `0` | **new**: enable v3 curve emission |
| `JUSYNC_RAWGEO_CURVE_SUBSETS` | `4` | **new**: `S`, clamped `[1, 8]` |
| `JUSYNC_RAWGEO_IDLE_FRAMES` | `6` | existing: skip identical frames |
| `JUSYNC_RAWGEO_STRIDE` | `1` | points only; ignored for curves |

All ranks must run with the same values; see invariant 6 for the failure mode.

### 6.3 Packing

- Reject when `!RawGeoEnabled() || !getenv curves || !g_rankMemoryStore ||
  !cacheEntry || !data.Points || data.NumPoints == 0 || data.CurveLengths == nullptr
  || data.NumCurveLengths == 0` → fall back to USD.
- Validate every `CurveLengths[i] ≥ 2` and `Σ CurveLengths == NumPoints`; otherwise
  log and fall back (do not emit a malformed frame).
- `topology = 1`, `curve_count = NumCurveLengths`, `full_curve_count = NumCurveLengths`.
- `width_components = 1` iff `data.Scales != nullptr` and `data.ScalesType == FLOAT`;
  widths are copied per point. If only `data.UniformScale` is set, omit widths and let
  the client use `jusync.Curve.DefaultRadius`. Do **not** expand a uniform radius.
- `colors` copied per point when `data.Colors != nullptr` (components from
  `RawGeoFloatComponents(ColorsType)`, accepted 3 or 4).
- Up to 16 float attributes, per point, same rules as the point path
  (`data.Attributes`, `PerPrimData` skipped).
- Coordinate space is untouched: raw ANARI/USD right-handed Y-up. The client applies
  `(x, z, -y)`.

### 6.4 Subset emission

- `S == 1`: one payload, no `_s` suffix.
- `S > 1`: emit exactly `S` payloads. Subset `k` copies curves
  `k, k+S, k+2S, …` with their point runs, widths, colors, and attributes.
- Check the 1.5 GB per-payload cap **before** storing anything; on overflow, store
  nothing and return `false` (USD path handles the frame).
- Idle detection keys on the frame base (rank + prim + time), not the subset file:
  if the packed full-frame digest matches the previous frame, skip all `S` files.
- No stride decimation, no PLY snapshot, no idle "burst" upgrade for curves.
- Empty subsets still advertise the frame-level `width_components` (the packer may
  receive a non-null dummy pointer with zero points), so merge flags do not depend
  on which subset arrives first.
- On prim removal/rename, `RemoveStoreEntriesForFile` expands the base stage key to
  its `.usd`/`.usda` twins, the base `.geo`, and `_s0.geo` ... `_s7.geo`. A retired
  curve element cannot leave stale subset aliases in the broker.
- Each subset file goes through `GetDiffCapture().CapturePreStore(geoKey)` and
  `g_rankMemoryStore->StoreFile(geoKey, …, "application/x-jusync-geo")`.

### 6.5 Refactor discipline

Extract the shared plumbing (geo key derivation, rank suffix, cap check, idle state,
DiffCapture/store) into helpers used by both overloads. The point path's observable
behavior must not change; its existing tests are the guard.

## 7. Client design (jusync-uesample)

### 7.1 Parsing

`JUSYNCParseRawGeoBuffer` gains the filename:

```cpp
static bool JUSYNCParseRawGeoBuffer(const TArray<uint8>& Buffer,
                                    const FString& Filename,
                                    FJUSYNCPointCloudRef& OutCloud,
                                    FJUSYNCCompactMeshRef& OutMesh);
```

Dispatch:

- v1, or v2/v3 with `topology == 0`: existing point-cloud path.
- v3 with `topology == 1`: curve path in `JUSYNCCurveBuilder`.
- Unknown version: reject with a warning (no crash).

Validation mirrored from 4.3, including the exact byte math and
`Σ curve_vertex_counts == num_points`.

Before allocating merge slots, the parser also bounds wire metadata:
`subset_count <= 8`, `full_curve_count <= 4,194,304`,
`full_curve_count <= full_num_points / 2`, `full_num_points >= num_points`, and
`curve_count == ceil((full_curve_count - subset_index) / subset_count)`. This rejects
an unbounded curve-slot allocation and a truncated subset that would otherwise fill
its slot and complete a frame with missing curves.

### 7.2 Merge

`FJUSYNCCurveMerge` keyed by `StripSubsetSuffix(Filename)`:

```cpp
struct FJUSYNCCurveMerge {
    uint64 SubsetCount = 1;
    uint64 CurvesFull = 0;
    uint64 CurvesFilled = 0;
    double Time = 0.0;
    double LastUseSeconds = 0.0;
    FString PrimName;
    FString FileBase;
    TArray<TArray<FVector3f>> Curves;      // ordinal slots
    TArray<TArray<FColor>>  CurveColors;
    TArray<TArray<float>>   CurveWidths;
    TBitArray<> FilledSlots;               // per subset k, not per curve
};
```

- `FilledSlots` tracks which `k` arrived (not which curves), so empty subsets still
  complete the frame.
- Coarse emission copies subset 0 curves in ordinal order.
- Full emission concatenates all slots once `FilledSlots` is all set.
- Budget: `full_num_points × ~40 bytes` accounted against
  `jusync.RawGeo.RefineBudgetKPoints`, evicting oldest.
- Age sweep: entries older than 30 s are dropped with a warning. The de-duplicated
  metadata-warning key set is bounded so a long session cannot grow it without limit.

### 7.3 Tubes

`FJUSYNCCurveBuilder::Build(const FJUSYNCCurveSet& Set, FJUSYNCCompactMeshData& Out)`
expands polylines into tubes:

- `Sides = clamp(jusync.Curve.Sides, 3, 8)`, default `4`.
- Per node `i`: `T = normalize(P[i+1] - P[i-1])` (endpoints use the adjacent
  segment). `Ref = |T.Z| < 0.9 ? (0,0,1) : (0,1,0)`; `Side = normalize(cross(T, Ref))`;
  `Up = cross(Side, T)`.
- Ring vertex `s`: `dir = Side·cos(2πs/Sides) + Up·sin(2πs/Sides)`,
  `V = P[i] + dir·radius`, `N = dir`.
- Between rings `i` and `i+1`: two triangles per side; winding fixed, then the
  existing `jusync.Mesh.FlipWinding` knob still applies downstream.
- `radius = (widths ? widths[i] : jusync.Curve.DefaultRadius) ×
  jusync.Curve.RadiusScale`.
- Degenerate segments (zero length) are skipped; curves with `< 2` nodes are dropped.
- `VertexColors` = per-node color (clamped, ×255) or `FColor::White` with
  `bHasBakedColors = false` when the payload has no colors.
- Positions and normals are mapped `(x, z, -y)`.
- Budget: if total nodes exceed `jusync.Curve.MaxNodes` (default 4,000,000),
  decimate each curve with a global stride, always keeping the first and last node
  (minimum 2 nodes), and log the reduction. This is an advisory target, not a hard
  cap: endpoints are retained, so many very short curves can still exceed it.

Vertex/triangle cost: `nodes × Sides` vertices, `segments × Sides × 2` triangles.

### 7.4 Emission, cache, morph

- The result is an `FJUSYNCCompactMeshRef` with `ElementName = Hdr.prim_name`,
  `TypeName = "BasisCurves"`, entering the normal mesh update path. Cache, morph,
  materials, LUT/vertex colors, and time-step animation then apply unchanged.
- Do **not** cache intermediate coarse frames (`subset_count > 1` and not final);
  cache only the completed frame. Cache keys already include the filename, which
  differs per subset.
- Curve order can change between time steps; the mesh morpher falls back to
  instant-apply when matched vertices fall below `jusync.Morph.MeshMatchMinPct`.
  Acceptable for v1.

### 7.5 CVars

| CVar | Default | Meaning |
|---|---|---|
| `jusync.RawGeo.CurveProgressive` | `2` | 2 = coarse (k=0) now + full on completion; 1 = also emit after every subset; 0 = coarse only, drop the rest |
| `jusync.Curve.Sides` | `4` | tube sides, clamped 3–8 |
| `jusync.Curve.DefaultRadius` | `0.5` | radius when the payload has no widths |
| `jusync.Curve.RadiusScale` | `1.0` | multiplier applied to widths |
| `jusync.Curve.MaxNodes` | `4000000` | decimation threshold |
| `jusync.RawGeo.RefineBudgetKPoints` | `16384` | existing; shared merge budget (thousands of points) |

## 8. Compatibility and rollout

- **Old clients** reading a v3 payload see `version != 1 && version != their max` and
  reject safely — but the sender wrote `.geo` instead of USD, so the result is
  silence. Therefore `JUSYNC_RAWGEO_CURVES` defaults to **off** and is only enabled
  after the UE side ships.
- **New clients** read v1, v2, and v3.
- **Points** are unchanged: same packing, same subset semantics, same idle/burst.
- All three `AnariUsdProtocol.h` copies must match before the sender is enabled:
  `jusync/include/`, `jusync/UnrealPlugin/JUSYNC/Source/ThirdParty/...`,
  `jusync-uesample/Plugins/JUSYNC/Source/ThirdParty/...`.
- Phases:
  1. header + `valid()` + tests (no runtime behavior change);
  2. client parser + merge + tubes + synthetic data;
  3. sender curve writer behind the env gate;
  4. default-on once the UE path is verified against MPI and non-MPI.

## 9. Test plan

### 9.1 Unit (C++, `jusync`)

New `tests/rawgeo_v3/test_rawgeo_v3.cpp`, registered in the root `CMakeLists.txt`
next to `stream_perf` and via `add_test(NAME rawgeo_v3 ... LABELS protocol)`:

- header sizes and prefix offsets (112 / 136 / 160; attr desc 40);
- `valid()` accept/reject table for v1, v2, v3, including truncated buffers,
  bad `flags`, bad `topology`, bad byte math, `attr_uniform_count != num_points`;
- v3 round-trip pack/unpack of a frame with 3 curves / 8 points, widths, colors,
  2 attributes;
- subset math: `C = 10`, `S = 4` → counts `3, 3, 2, 2`; ordinals for each payload;
- empty subset (`num_points == 0`, `curve_count == 0`) accepted.

### 9.2 Synthetic generator

`jusync/tests/scripts/make_rawgeo_v3.py` writes `.geo` files for every matrix row
below, plus matching `usda` twins where useful. Deterministic, no HPC needed.

### 9.3 Client matrix

| # | Scenario | Must prove |
|---|---|---|
| 1 | non-MPI, `S = 1` | baseline render |
| 2 | non-MPI, `S = 4` | merge + naming |
| 3 | `mpisize = 1` | no `_r` suffix, identical to non-MPI |
| 4 | MPI 2 ranks, same prim, `S = 1` | two distinct elements |
| 5 | MPI 2 ranks, same prim, `S = 4` | rank-scoped merges |
| 6 | MPI, one rank with `C = 0` | silence from that rank |
| 7 | MPI, `C < S` | empty subsets complete the frame |
| 8 | out-of-order subsets | coarse/full policy correct |
| 9 | rank dies mid-frame | eviction, no leak |
| 10 | old client + v3 file | clean rejection, no crash |

Rows 1, 2, 3, 7, 8, 10 run headless from the generator. MPI rows 4–6, 9 run against
the demo container with 2+ ranks when available; otherwise simulated by
hand-placed files with `_r` suffixes.

### 9.4 Regression

- `ctest --test-dir build --output-on-failure` (all labels) in `jusync`.
- UE auto-quit smoke run with the sample project (existing harness) before/after.
- Point-cloud RawGeo path: the milestone-2 10M-point case was **not rerun on this
  branch**. The point path is only changed by the header version bump (v1 -> v2
  defaults), and the existing CTest/UE suites cover the v2 layout; the 10M
  performance/byte-hash claim remains prior-branch evidence.

### 9.5 Results (branch `feat/rawgeo-v3-curves`)

Fixtures generated with the UE-bundled Python are read twice: the generator's own
`--verify` and an independent host program that memcpys the C++ `JusyncRawGeoHeader`
and calls `valid()`. Every generated file passes both.

| # | Scenario | Evidence on this branch |
|---|---|---|
| 1 | non-MPI, `S = 1` | `rawgeo_v3_solo` (1 file) passes Python + C++ `valid()`; `JUSYNC.RawGeo.CurveSingle` |
| 2 | non-MPI, `S = 4` | `rawgeo_v3` (4 files) passes both; `CurveMerge`, `CurveEmptySubset` |
| 3 | `mpisize = 1` | generator without `--rank` emits no `_r` suffix and matches row 1; `RankParse` fallback |
| 4 | MPI 2 ranks, `S = 1` | `rawgeo_v3_mpi_s1` (2 files: `_r0`, `_r1`) passes both; `CurveRank` |
| 5 | MPI 2 ranks, `S = 4` | `rawgeo_v3_mpi_s4` (8 files) passes both; `CurveRank` rank-scoped merges |
| 6 | MPI, one rank with `C = 0` | sender rejects `NumCurveLengths == 0` and falls back to USD (gate in `TryStoreRawGeo`, so that rank emits no `.geo`); `CurveEmptySubset` covers empty payloads |
| 7 | MPI, `C < S` | rank 1 of `rawgeo_v3_mpi_s4` has `C = 2`, `S = 4` (2 empty subsets) and verifies; `CurveEmptySubset` |
| 8 | out-of-order subsets | `CurveMerge` ingests `s1` before `s0`; colors/widths upgrade for late arrival (`c21f03a`, `0d4924c`) |
| 9 | rank dies mid-frame | 30 s sweep plus budget eviction in `JUSYNCFlushRawGeoMerges`; covered by code review, not a clock-driven test |
| 10 | old client + v3 file | client accepts versions 1/2/3 and rejects unknown ones; v3 `valid()`/version unit tests; a v2-only client rejects version 3 before reading the payload |

Other regression evidence:

- `ctest --test-dir build -C Release`: 13/13 pass, including `rawgeo_v3` and
  `binary_static_check`. On Windows the static check searches `lib<name>.dll` while
  CMake produces `anari_usd_middleware.dll`; copying the DLL to the searched name in
  the build tree makes the two environment-only tests pass (no source change).
- UE automation: 15/15 `JUSYNC.RawGeo.*` tests pass after the review fixes. New
  coverage includes `CurveRejectCount` (unbounded/truncated subset metadata),
  `CurveLateUpgrade` (colors/widths from a later subset, default radius backfill),
  `CurveSubsetKey` (`_s<k>` does not change the element/group key), and
  `CurveDispatch` (middleware buffer -> finalized `BasisCurves` mesh).
- Host packer: `test_rawgeo_curve_pack` passes, including two-attribute round-trip,
  `C = 10, S = 4 -> 3, 3, 2, 2`, wrong-subset rejection, and the
  `StoreKeysForFile` `_s0.._s7` expansion/dedup cases.
- Sender-to-client E2E (rerun after the review fixes): a temporary ANARI `curve`
  sender (image `jusync-curve-sender:test`) emitted four v3 `.geo` subsets through a
  live ZMQ broker on host port 5557. `JUSYNC.RawGeo.BrokerCurveE2E` used the
  middleware C API to list and download all four files, merged them in the client,
  and asserted a completed `CurveStream` / `BasisCurves` mesh with 120 vertices,
  192 triangles, and baked colors. The test is opt-in through `JUSYNC_E2E_ENDPOINT`;
  without it the test reports a skip and does not touch the network. Set
  `JUSYNC_E2E_REQUIRED=1` to turn a missing endpoint into a failure in CI.
- Point-cloud RawGeo now uses the v2 header (136 bytes) with the single-subset lattice
  fields (`subset_index = 0`, `subset_count = 1`, `full_num_points = n`). Before this
  branch the sender stamped v1 and the client synthesized those same defaults in its
  v1 branch, so rendering behavior is unchanged. `JUSYNC_RAWGEO_VERSION` (= 3) is only
  used for curve payloads.
- Sender: `jusync-sender:rawgeo-v3` compiles `UsdBridgeUsdWriter.cpp` against OpenUSD
  25.11 in the Docker image (the host has no USD toolchain).
- The 10M-point point-path regression was not rerun on this branch; the point
  payload layout is unchanged except for the explicit v2 defaults. See 9.4.

## 10. File map

### ANARI-USD (branch `feat/rawgeo-v3-curves`)

| File | Change |
|---|---|
| `AnariUsdProtocol.h` | V1 frozen, V2 frozen, V3 current, `header_bytes`, `valid`, asserts |
| `UsdBridge/UsdBridgeUsdWriter.h` | curve overload declaration |
| `UsdBridge/UsdBridgeUsdWriter.cpp` | shared helpers, curve packing, subsets, gates |

### jusync (branch `feat/rawgeo-v3-curves`)

| File | Change |
|---|---|
| `include/AnariUsdProtocol.h` | v3 |
| `UnrealPlugin/JUSYNC/Source/ThirdParty/AnariUsdMiddleware/Include/AnariUsdProtocol.h` | v3 |
| `tests/rawgeo_v3/test_rawgeo_v3.cpp` | new unit test |
| `tests/scripts/make_rawgeo_v3.py` | new generator |
| `CMakeLists.txt` | test target + CTest entry |
| `UnrealPlugin/JUSYNC/...` | v3 client (see below), after step 0 sync |

### jusync-uesample (branch `feat/rawgeo-v3-curves`)

| File | Change |
|---|---|
| `Plugins/JUSYNC/Source/ThirdParty/.../AnariUsdProtocol.h` | v3 |
| `Plugins/JUSYNC/Source/JUSYNC/Private/JUSYNCRawGeoCurves.{h,cpp}` | new: merge + tubes |
| `Plugins/JUSYNC/Source/JUSYNC/Private/JUSYNCSubsystem.cpp` | parse dispatch, filename arg |
| `Plugins/JUSYNC/Source/JUSYNC/Private/JUSYNCFileSpawnerActor.cpp` | filename rank fallback, coarse-frame cache skip |
| `Plugins/JUSYNC/Source/JUSYNC/Public/JUSYNCTypes.h` | optional curve type if needed |

### Prerequisite step 0 (separate commit)

`jusync/UnrealPlugin/JUSYNC` is declared the source of truth
(`tools/deploy-ue-plugin.ps1:1-3`) but the sample is ~520 lines ahead
(`JUSYNCSubsystem.cpp` alone +521/−70, plus `JUSYNCFileSpawnerCustomization.*`).
Before v3 lands, commit a mechanical sample → canonical source sync so the deploy
script cannot later revert the sample's newer work. Then implement v3 in the
canonical and deploy back.

## 11. Decisions and rejected alternatives

| Decision | Rationale |
|---|---|
| **Curves only** in v3 | Meshes already stream via USD; a generalized topology block doubles surface area for no current consumer. |
| **Curve-granular subsets**, not point-decimated | Coarse previews show fewer but correctly shaped streamlines. Point decimation produces chorded curves that snap on completion and badly misrepresents thin strands. |
| **Widths as their own section** | Stops overloading `attribute0` (the current point-cloud hack) and matches ANARI `radius`/USD `widths`. |
| **No stable ids** | Not needed for rendering; costs 4–8 bytes/point; can be added as a v4 section when pathlines need tracking. |
| **Tubes, not Niagara ribbons** | Tubes are camera-independent triangles and reuse RealtimeMesh, the cache, the morph material, and the existing materials. Camera-facing ribbons would need a new shader contract; Niagara would need a new pipeline. |
| **Filename-base merge key** | Rank identity is already in the filename; `prim_name` is rank-ambiguous. Avoids plumbing a rank parameter through the middleware parser. |
| **Empty subsets emitted** | Makes frame completeness deterministic without timeouts. |
| **`S = 1` omits `_s0`** | Keeps the filename identical to the existing whole-frame convention. |
| **Sender gate defaults off** | A v3 file that an old client cannot parse is silently missing geometry. |

## 12. Risks

| Risk | Mitigation |
|---|---|
| `CurveLengths` semantics differ from assumed contiguous runs | Unit test with a synthetic ANARI curve commit; verify `Σ lengths == NumPoints` in the writer and fall back to USD on mismatch. |
| Subset file/notification amplification (`S × prims × ranks`) | Clamp `S ≤ 8`; default `4`; document broker load; coarse-only mode (`CurveProgressive 0`) available. |
| Tube vertex explosion (`nodes × sides`) | `jusync.Curve.MaxNodes` decimation; `Sides = 4` default; log reductions. |
| MPC/rank suffix drift between clip and `.geo` names | Derive both from the same `mpiRank/mpiSize` values; assert the `.geo` key starts with the clip key in debug builds. |
| Rank unknown before the file list arrives | Parse `_r<digits>_` from the filename first. |
| v2 header misread after appending v3 | Explicit `header_bytes` cases + size asserts + unit test. |
| Three protocol-header copies diverge | Single source in `jusync/include`; deploy script copies it; CI check compares hashes. |
| Coarse-frame cache churn | Skip cache store for intermediate subsets. |

## 13. Open questions

None. Every decision above is resolved; anything discovered during implementation
that changes a wire-format choice reopens this spec before code is written.
