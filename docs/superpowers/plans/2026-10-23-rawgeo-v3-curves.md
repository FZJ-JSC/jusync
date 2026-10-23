# RawGeo v3 Curves Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Stream linear curve geometry from ANARI-USD to Unreal without USD parsing, with curve-granular progressive subsets, on single-rank and multi-rank MPI alike.

**Architecture:** A third append-only RawGeo header version adds `topology`, `curve_count`, `full_curve_count`, `width_components`, and `flags`; the sender packs whole curves into rank-scoped `_s<k>` files; the UE client merges subsets by filename base and expands polylines into four-sided tubes that reuse the existing compact-mesh cache, materials, and morph path.

**Tech Stack:** C++17 (middleware + protocol), Unreal Engine 5.7 (UE module, automation tests), Python 3 (fixture generator), CMake + CTest, Docker for the sender image.

**Spec:** `docs/superpowers/specs/2026-10-23-rawgeo-v3-curves-design.md`

## Global Constraints

- Protocol versions 1 and 2 stay byte-identical and readable; v3 is append-only.
- Header sizes: `JusyncRawGeoHeaderV1` 112, `JusyncRawGeoHeaderV2` 136, `JusyncRawGeoHeader` (v3) 160, `JusyncRawGeoAttrDesc` 40, under `#pragma pack(push,1)`.
- `JUSYNC_RAWGEO_VERSION = 3`; the sender's **point** path keeps emitting **v2** so existing point clients are unaffected. Only curves emit v3.
- Section order: positions, colors, attributes, curve_vertex_counts, widths. No padding.
- Subset `k` of `S` carries whole curves with ordinal `k + i·S`; `curve_count == ceil((C-k)/S)`; empty subsets are legal.
- `S == 1` never appends `_s0`. `S > 1` emits all `S` payloads.
- Merge key is the filename with `_s<k>` stripped; never `prim_name` alone.
- Element identity is `MakeRankedElementKey(prim_name, rank)`.
- `JUSYNC_RAWGEO_CURVES` defaults to `0`; `JUSYNC_RAWGEO_CURVE_SUBSETS` defaults to `4`, clamped `[1,8]`.
- Client converts coordinates `(x,y,z) -> (x,z,-y)`; sender emits raw ANARI/USD coordinates.
- All UE edits go to `D:\JSC_Github\jusync\UnrealPlugin\JUSYNC` (source of truth), then deploy with `tools\deploy-ue-plugin.ps1`.

## Review Focus

- **Empty subsets / `full_curve_count == 0`** — a rank with no curves must produce silence, and an empty subset must still count toward completion; otherwise merges hang forever. Tested in Tasks 2, 4, 6.
- **Two ranks sharing one `prim_name`** — a merge keyed on `prim_name + time` mixes rank geometry. Tested in Task 4.
- **Degenerate curves (2 nodes, repeated points, zero widths)** — tube frames can produce NaNs. Tested in Task 3/5.
- **Malformed curve lengths (a length < 2, or a sum != `num_points`)** — must be rejected, not read past the buffer. Tested in Tasks 1, 6, 7.
- **`S` drift / values above 8** — a payload with a different `S` than its siblings leaves a merge stuck. Tested in Task 4.

---

## File Structure

**`jusync` (protocol + tests + canonical UE plugin)**

| Path | Responsibility |
|---|---|
| `include/AnariUsdProtocol.h` | Frozen v1/v2, current v3, `header_bytes`, `valid()` |
| `tests/rawgeo_v3/test_rawgeo_v3.cpp` | Header layout + `valid()` + round-trip unit test |
| `tests/scripts/make_rawgeo_v3.py` | Deterministic `.geo` fixtures + verifier |
| `UnrealPlugin/JUSYNC/Source/JUSYNC/Private/JUSYNCRawGeoCurves.h/.cpp` | Curve payload parse, subset merge, tube build |
| `UnrealPlugin/JUSYNC/Source/JUSYNC/Private/Tests/JUSYNCRawGeoCurveTests.cpp` | UE automation tests |
| `UnrealPlugin/JUSYNC/Source/JUSYNC/Private/JUSYNCSubsystem.cpp` | RawGeo dispatch + mesh finalize |
| `UnrealPlugin/JUSYNC/Source/JUSYNC/Public/JUSYNCElementNames.h` | `ParseFileRank` |
| `UnrealPlugin/JUSYNC/Source/JUSYNC/Private/JUSYNCFileSpawnerActor.cpp` | Rank fallback, transient-cache skip |

**`ANARI-USD` (sender)**

| Path | Responsibility |
|---|---|
| `UsdBridge/RawGeoCurvePack.h` | Pure packer + subset naming, no USD dependency |
| `tests/rawgeo_curve_pack/` | Host-compiled packer test |
| `UsdBridge/UsdBridgeUsdWriter.h/.cpp` | Curve overload, env gates, cap, idle, store |
| `AnariUsdProtocol.h` | v3 header copy |

**`jusync-uesample`** — receives everything via `deploy-ue-plugin.ps1`; commit the deployed sources on the branch.

---

### Task 0: Sync the sample plugin's newer sources into the canonical plugin

**Files:**
- Modify (copy over): `UnrealPlugin/JUSYNC/Source/JUSYNC/JUSYNC.Build.cs`, `Private/JUSYNCAnimationController.cpp`, `Private/JUSYNCFileChangeTracker.cpp`, `Private/JUSYNCFileSpawnerActor.cpp`, `Private/JUSYNCMeshMorpher.cpp`, `Private/JUSYNCModule.cpp`, `Private/JUSYNCPointCloudSpawner.cpp`, `Private/JUSYNCSubsystem.cpp`, `Private/JUSYNCUSDLoader.cpp`, `Public/JUSYNCElementNames.h`, `Public/JUSYNCFileChangeTracker.h`, `Public/JUSYNCFileSpawnerActor.h`, `Public/JUSYNCModule.h`, `Public/JUSYNCPointCloudSpawner.h`, `Public/JUSYNCTypes.h`, `Public/JUSYNCUSDLoader.h`
- Create (copied): `UnrealPlugin/JUSYNC/Source/JUSYNC/Private/JUSYNCFileSpawnerCustomization.cpp`, `.h`

**Interfaces:**
- Consumes: nothing.
- Produces: canonical and sample `Source/JUSYNC` trees are identical, so later tasks edit one place.

- [ ] **Step 1: Copy the sample's newer sources over the canonical tree**

```powershell
$src = 'D:\JSC_Github\jusync-uesample\Plugins\JUSYNC\Source\JUSYNC'
$dst = 'D:\JSC_Github\jusync\UnrealPlugin\JUSYNC\Source\JUSYNC'
robocopy $src $dst /E /NJH /NDL /NFL /XD Binaries Intermediate Saved /XF *.dll *.lib *.pdb *.exp
if ($LASTEXITCODE -ge 8) { throw "robocopy failed: $LASTEXITCODE" }
```

- [ ] **Step 2: Confirm only expected files changed**

Run: `cd D:\JSC_Github\jusync; git status --porcelain UnrealPlugin/JUSYNC/Source/JUSYNC`
Expected: the 16 modified files and 2 added customization files listed above, nothing else.

- [ ] **Step 3: Confirm the deploy script is now a source no-op**

Run: `cd D:\JSC_Github\jusync; .\tools\deploy-ue-plugin.ps1 -WhatIf`
Expected: log at `$env:TEMP\jusync-deploy-robocopy.log`; `Select-String -Path $env:TEMP\jusync-deploy-robocopy.log -Pattern 'JUSYNCSubsystem.cpp'` returns nothing (no source would be copied).

- [ ] **Step 4: Compile the sample editor target**

Run:
```powershell
& 'C:\Program Files\Epic Games\UE_5.7\Engine\Build\BatchFiles\Build.bat' JusyncSampleProjectEditor Win64 Development -Project='D:\JSC_Github\jusync-uesample\JusyncSampleProject.uproject' -WaitMutex
```
Expected: `Build succeeded` / exit code 0.

- [ ] **Step 5: Commit**

```bash
cd D:\JSC_Github\jusync
git add UnrealPlugin/JUSYNC/Source/JUSYNC
git commit -m "Sync sample plugin sources into the canonical plugin"
```

---

### Task 1: Protocol v3 header and `valid()`

**Files:**
- Modify: `jusync/include/AnariUsdProtocol.h:372-451`
- Create: `jusync/tests/rawgeo_v3/test_rawgeo_v3.cpp`
- Modify: `jusync/CMakeLists.txt` (inside `if(BUILD_TESTS)`)
- Modify: `ANARI-USD/AnariUsdProtocol.h`, `jusync/UnrealPlugin/JUSYNC/Source/ThirdParty/AnariUsdMiddleware/Include/AnariUsdProtocol.h`, `jusync-uesample/Plugins/JUSYNC/Source/ThirdParty/AnariUsdMiddleware/Include/AnariUsdProtocol.h`

**Interfaces:**
- Consumes: nothing.
- Produces:
  ```cpp
  struct JusyncRawGeoHeaderV1;   // 112 bytes, frozen
  struct JusyncRawGeoHeaderV2;   // 136 bytes, frozen (v1 + subset_index/subset_count/full_num_points)
  struct JusyncRawGeoHeader;     // 160 bytes, v3 (v2 + topology/curve_count/full_curve_count/width_components/flags)
  static size_t JusyncRawGeoHeader::header_bytes(uint32_t ver); // 1->112, 2->136, default->160
  bool JusyncRawGeoHeader::valid(size_t totalBytes) const;
  inline constexpr uint32_t JUSYNC_RAWGEO_VERSION = 3;
  inline constexpr uint32_t JUSYNC_RAWGEO_TOPOLOGY_POINTS = 0;
  inline constexpr uint32_t JUSYNC_RAWGEO_TOPOLOGY_LINEAR_CURVES = 1;
  ```

- [ ] **Step 1: Write the failing test**

`jusync/tests/rawgeo_v3/test_rawgeo_v3.cpp`:

```cpp
#include "AnariUsdProtocol.h"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <vector>

using namespace anari_usd_protocol;
static int g_fails = 0;
#define CHECK(cond, ...) do { if (!(cond)) { ++g_fails; printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); } } while (0)

// Layout exactly as the spec documents it (no descs, colors=3, one width).
static std::vector<uint8_t> BuildV3(uint32_t topology, uint64_t numPoints,
                                    uint64_t curveCount, uint64_t fullCurveCount,
                                    uint64_t subsetIndex, uint64_t subsetCount,
                                    uint32_t widthComponents,
                                    const std::vector<uint32_t>& curveCounts)
{
    JusyncRawGeoHeader h{};
    h.magic = JUSYNC_RAWGEO_MAGIC;
    h.version = JUSYNC_RAWGEO_VERSION;
    std::strncpy(h.prim_name, "Streamlines", sizeof(h.prim_name) - 1);
    h.num_points = numPoints;
    h.time = 1.5;
    h.position_components = 3;
    h.color_components = 3;
    h.num_attributes = 0;
    h.attr_uniform_count = (uint32_t)numPoints;
    h.subset_index = subsetIndex;
    h.subset_count = subsetCount;
    h.full_num_points = numPoints;
    h.topology = topology;
    h.curve_count = (uint32_t)curveCount;
    h.full_curve_count = fullCurveCount;
    h.width_components = widthComponents;
    h.flags = 0;
    h.payload_bytes = numPoints * 3 * 4 + numPoints * 3 * 4
                    + curveCounts.size() * 4 + numPoints * widthComponents * 4;
    std::vector<uint8_t> b(sizeof(h) + (size_t)h.payload_bytes, 0);
    std::memcpy(b.data(), &h, sizeof(h));
    uint8_t* p = b.data() + sizeof(h);
    for (uint64_t i = 0; i < numPoints * 3; ++i) { float v = (float)i; std::memcpy(p, &v, 4); p += 4; }
    p += numPoints * 3 * 4; // colors stay zero
    std::memcpy(p, curveCounts.data(), curveCounts.size() * 4); p += curveCounts.size() * 4;
    for (uint64_t i = 0; i < numPoints * widthComponents; ++i) { float v = 1.0f; std::memcpy(p, &v, 4); p += 4; }
    return b;
}

static JusyncRawGeoHeader ReadHeader(const std::vector<uint8_t>& b)
{
    JusyncRawGeoHeader h{};
    std::memcpy(&h, b.data(), std::min(b.size(), sizeof(h)));
    return h;
}

int main()
{
    CHECK(sizeof(JusyncRawGeoHeaderV1) == 112, "v1 size");
    CHECK(sizeof(JusyncRawGeoHeaderV2) == 136, "v2 size");
    CHECK(sizeof(JusyncRawGeoHeader) == 160, "v3 size");
    CHECK(sizeof(JusyncRawGeoAttrDesc) == 40, "attr size");
    CHECK(JusyncRawGeoHeader::header_bytes(1) == 112, "header_bytes(1)");
    CHECK(JusyncRawGeoHeader::header_bytes(2) == 136, "header_bytes(2)");
    CHECK(JusyncRawGeoHeader::header_bytes(3) == 160, "header_bytes(3)");

    // v1 and v2 payloads stay valid byte-for-byte.
    {
        JusyncRawGeoHeaderV1 h{};
        h.magic = JUSYNC_RAWGEO_MAGIC; h.version = 1;
        std::strncpy(h.prim_name, "p", sizeof(h.prim_name) - 1);
        h.num_points = 2; h.position_components = 3;
        h.attr_uniform_count = 2; h.payload_bytes = 24;
        std::vector<uint8_t> b(sizeof(h) + 24, 0);
        std::memcpy(b.data(), &h, sizeof(h));
        JusyncRawGeoHeader v3{};
        std::memcpy(&v3, b.data(), sizeof(h));
        CHECK(v3.valid(b.size()), "v1 payload valid");
    }
    {
        JusyncRawGeoHeaderV2 h{};
        h.magic = JUSYNC_RAWGEO_MAGIC; h.version = 2;
        h.num_points = 2; h.position_components = 3; h.attr_uniform_count = 2;
        h.payload_bytes = 24; h.subset_index = 1; h.subset_count = 4; h.full_num_points = 8;
        std::vector<uint8_t> b(sizeof(h) + 24, 0);
        std::memcpy(b.data(), &h, sizeof(h));
        JusyncRawGeoHeader v3{};
        std::memcpy(&v3, b.data(), sizeof(h));
        CHECK(v3.valid(b.size()), "v2 payload valid");
        v3.subset_index = 4;
        CHECK(!v3.valid(b.size()), "v2 subset_index bound");
    }

    // v3 flat points
    { auto b = BuildV3(0, 4, 0, 0, 0, 1, 0, {}); CHECK(ReadHeader(b).valid(b.size()), "v3 points"); }
    // v3 curves, 2 of 10, subset 2 of 4, widths on
    { auto b = BuildV3(1, 4, 2, 10, 2, 4, 1, {2, 2}); CHECK(ReadHeader(b).valid(b.size()), "v3 curves"); }
    // empty subset
    { auto b = BuildV3(1, 0, 0, 10, 3, 4, 0, {}); CHECK(ReadHeader(b).valid(b.size()), "empty subset"); }

    auto reject = [](std::vector<uint8_t> b) { return !ReadHeader(b).valid(b.size()); };
    { auto b = BuildV3(0, 4, 0, 0, 0, 1, 0, {}); reinterpret_cast<JusyncRawGeoHeader*>(b.data())->magic = 0; CHECK(reject(b), "bad magic"); }
    { auto b = BuildV3(0, 4, 0, 0, 0, 1, 0, {}); reinterpret_cast<JusyncRawGeoHeader*>(b.data())->version = 4; CHECK(reject(b), "bad version"); }
    { auto b = BuildV3(0, 4, 0, 0, 0, 1, 0, {}); reinterpret_cast<JusyncRawGeoHeader*>(b.data())->flags = 1; CHECK(reject(b), "bad flags"); }
    { auto b = BuildV3(0, 4, 0, 0, 0, 1, 0, {}); reinterpret_cast<JusyncRawGeoHeader*>(b.data())->topology = 2; CHECK(reject(b), "bad topology"); }
    { auto b = BuildV3(0, 4, 0, 0, 0, 1, 0, {}); reinterpret_cast<JusyncRawGeoHeader*>(b.data())->curve_count = 1; CHECK(reject(b), "points with curves"); }
    { auto b = BuildV3(1, 4, 2, 1, 0, 1, 0, {2, 2}); CHECK(reject(b), "curve_count > full_curve_count"); }
    { auto b = BuildV3(1, 2, 2, 10, 0, 4, 0, {1, 1}); CHECK(reject(b), "num_points < 2*curve_count"); }
    { auto b = BuildV3(1, 4, 2, 10, 0, 4, 0, {2, 2}); reinterpret_cast<JusyncRawGeoHeader*>(b.data())->payload_bytes += 4; CHECK(reject(b), "payload_bytes mismatch"); }
    { auto b = BuildV3(1, 4, 2, 10, 0, 4, 0, {2, 2}); b.resize(b.size() - 4); CHECK(reject(b), "truncated"); }
    { auto b = BuildV3(1, 4, 2, 10, 0, 4, 0, {2, 2}); reinterpret_cast<JusyncRawGeoHeader*>(b.data())->subset_index = 4; CHECK(reject(b), "subset_index >= subset_count"); }
    { auto b = BuildV3(1, 4, 2, 10, 0, 4, 0, {2, 2}); reinterpret_cast<JusyncRawGeoHeader*>(b.data())->attr_uniform_count = 3; CHECK(reject(b), "attr_uniform_count != num_points"); }

    printf(g_fails ? "FAILED %d\n" : "OK rawgeo_v3\n", g_fails);
    return g_fails ? 1 : 0;
}
```

- [ ] **Step 2: Run it and confirm it fails to compile**

Run:
```powershell
cmake -S D:\JSC_Github\jusync -B D:\JSC_Github\jusync\build -DBUILD_TESTS=ON
cmake --build D:\JSC_Github\jusync\build --config Release --target test_rawgeo_v3
```
Expected: compile error, `JusyncRawGeoHeaderV2` is not a member / `topology` is not a member.

- [ ] **Step 3: Implement the header**

In `include/AnariUsdProtocol.h`:

```cpp
inline constexpr uint32_t JUSYNC_RAWGEO_VERSION = 3;
inline constexpr uint32_t JUSYNC_RAWGEO_TOPOLOGY_POINTS = 0;
inline constexpr uint32_t JUSYNC_RAWGEO_TOPOLOGY_LINEAR_CURVES = 1;

// Rename today's struct to JusyncRawGeoHeaderV2 (no field changes) and add:
struct JusyncRawGeoHeader
{
    // ... every V2 field in the same order ...
    uint32_t topology;
    uint32_t curve_count;
    uint64_t full_curve_count;
    uint32_t width_components;
    uint32_t flags;

    static size_t header_bytes(uint32_t ver)
    {
        switch (ver) {
            case 1:  return sizeof(JusyncRawGeoHeaderV1);
            case 2:  return sizeof(JusyncRawGeoHeaderV2);
            default: return sizeof(JusyncRawGeoHeader);
        }
    }
    bool valid(size_t totalBytes) const;
};
```

`valid()` implements exactly spec §4.3: magic, version 1/2/3, `position_components == 3`, colors in {0,3,4}, `attr_uniform_count == num_points`, v2+ subset bounds, v3 topology/flags/width rules, `num_points >= 2*curve_count` for curves, and the exact section-byte sum. Keep `totalBytes >= table + payload_bytes`; require `payload_bytes` to equal the computed sum. Add:

```cpp
static_assert(offsetof(JusyncRawGeoHeaderV2, subset_index) == sizeof(JusyncRawGeoHeaderV1));
static_assert(offsetof(JusyncRawGeoHeader, topology) == sizeof(JusyncRawGeoHeaderV2));
static_assert(sizeof(JusyncRawGeoHeaderV1) == 112, "v1 wire size");
static_assert(sizeof(JusyncRawGeoHeaderV2) == 136, "v2 wire size");
static_assert(sizeof(JusyncRawGeoHeader) == 160, "v3 wire size");
```

- [ ] **Step 4: Register the test**

In the root `CMakeLists.txt`, inside `if(BUILD_TESTS)`:

```cmake
add_executable(test_rawgeo_v3 tests/rawgeo_v3/test_rawgeo_v3.cpp)
target_include_directories(test_rawgeo_v3 PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_features(test_rawgeo_v3 PRIVATE cxx_std_17)
add_test(NAME rawgeo_v3 COMMAND test_rawgeo_v3)
set_tests_properties(rawgeo_v3 PROPERTIES LABELS "protocol")
```

- [ ] **Step 5: Run the test**

Run: `ctest --test-dir D:\JSC_Github\jusync\build -C Release -R rawgeo_v3 --output-on-failure`
Expected: `100% tests passed`, `OK rawgeo_v3`.

- [ ] **Step 6: Propagate the header**

```powershell
Copy-Item D:\JSC_Github\jusync\include\AnariUsdProtocol.h D:\JSC_Github\ANARI-USD\AnariUsdProtocol.h -Force
Copy-Item D:\JSC_Github\jusync\include\AnariUsdProtocol.h D:\JSC_Github\jusync\UnrealPlugin\JUSYNC\Source\ThirdParty\AnariUsdMiddleware\Include\AnariUsdProtocol.h -Force
Copy-Item D:\JSC_Github\jusync\include\AnariUsdProtocol.h D:\JSC_Github\jusync-uesample\Plugins\JUSYNC\Source\ThirdParty\AnariUsdMiddleware\Include\AnariUsdProtocol.h -Force
```
Verify all four hashes match: `Get-FileHash <path>` on each.

- [ ] **Step 7: Commit in all three repos**

```bash
cd D:\JSC_Github\jusync
git add include/AnariUsdProtocol.h tests/rawgeo_v3/test_rawgeo_v3.cpp CMakeLists.txt UnrealPlugin/JUSYNC/Source/ThirdParty/AnariUsdMiddleware/Include/AnariUsdProtocol.h
git commit -m "Add RawGeo v3 curve header and validity rules"

cd D:\JSC_Github\ANARI-USD
git add AnariUsdProtocol.h
git commit -m "Add RawGeo v3 curve header and validity rules"

cd D:\JSC_Github\jusync-uesample
git add Plugins/JUSYNC/Source/ThirdParty/AnariUsdMiddleware/Include/AnariUsdProtocol.h
git commit -m "Add RawGeo v3 curve header and validity rules"
```

---

### Task 2: Deterministic v3 fixture generator

**Files:**
- Create: `jusync/tests/scripts/make_rawgeo_v3.py`

**Interfaces:**
- Consumes: the v3 layout from Task 1.
- Produces: `.geo` files under `<out>/clips/` with names `Wave_Geom__<time>[_r<rank>][_s<k>].geo`; `--verify <dir>` exits 0 and prints `OK <n> files` or exits 1 with the failing file.

- [ ] **Step 1: Write the generator**

CLI:
```
python make_rawgeo_v3.py --out DIR --curves 6 --points-per-curve 5 --subsets 4 [--rank 2] [--time 0]
python make_rawgeo_v3.py --verify DIR
```
Rules:
- `--subsets 1` writes a single file with no `_s` suffix; `S > 1` writes `k = 0..S-1`.
- Rank `>= 0` inserts `_r<rank>` before the time: `Wave_Geom__r2_0_s1.geo`.
- Curve `i` is a polyline of `points_per_curve` nodes: `x = i`, `y = 0.1*i + 0.01*n`, `z = 0.05*n`; colors alternate; widths = `0.02 + 0.01*i`.
- Pack with `struct.pack('<I I 64s Q d I I I I Q Q Q Q I I Q I I')` and assert `calcsize == 160`.
- `--verify` re-reads each file, checks magic/version/size math/`Σ curve_vertex_counts == num_points`, and checks that the union of subset ordinals is exactly `0..C-1`.

- [ ] **Step 2: Generate and verify**

Run:
```powershell
python D:\JSC_Github\jusync\tests\scripts\make_rawgeo_v3.py --out "$env:TEMP\rawgeo_v3" --curves 6 --points-per-curve 5 --subsets 4
python D:\JSC_Github\jusync\tests\scripts\make_rawgeo_v3.py --verify "$env:TEMP\rawgeo_v3"
```
Expected: `wrote 4 files` then `OK 4 files`; subset curve counts are `2,2,1,1`.

- [ ] **Step 3: Generate the MPI and single-shot variants**

```powershell
python D:\JSC_Github\jusync\tests\scripts\make_rawgeo_v3.py --out "$env:TEMP\rawgeo_v3_mpi" --curves 4 --points-per-curve 3 --subsets 2 --rank 0
python D:\JSC_Github\jusync\tests\scripts\make_rawgeo_v3.py --out "$env:TEMP\rawgeo_v3_mpi" --curves 2 --points-per-curve 3 --subsets 2 --rank 1
python D:\JSC_Github\jusync\tests\scripts\make_rawgeo_v3.py --out "$env:TEMP\rawgeo_v3_solo" --curves 3 --points-per-curve 4 --subsets 1
python D:\JSC_Github\jusync\tests\scripts\make_rawgeo_v3.py --verify "$env:TEMP\rawgeo_v3_mpi"
python D:\JSC_Github\jusync\tests\scripts\make_rawgeo_v3.py --verify "$env:TEMP\rawgeo_v3_solo"
```
Expected: `OK 4 files`, `OK 1 files`.

- [ ] **Step 4: Commit**

```bash
cd D:\JSC_Github\jusync
git add tests/scripts/make_rawgeo_v3.py
git commit -m "Add deterministic RawGeo v3 curve fixture generator"
```

---

### Task 3: Client v3 dispatch and single-payload curve tubes

**Files:**
- Create: `jusync/UnrealPlugin/JUSYNC/Source/JUSYNC/Private/JUSYNCRawGeoCurves.h`
- Create: `jusync/UnrealPlugin/JUSYNC/Source/JUSYNC/Private/JUSYNCRawGeoCurves.cpp`
- Create: `jusync/UnrealPlugin/JUSYNC/Source/JUSYNC/Private/Tests/JUSYNCRawGeoCurveTests.cpp`
- Modify: `jusync/UnrealPlugin/JUSYNC/Source/JUSYNC/Private/JUSYNCSubsystem.cpp:1647-1676, 2036-2063`

**Interfaces:**
- Consumes: `anari_usd_protocol::JusyncRawGeoHeader` (Task 1); `FJUSYNCCompactMeshData` (`Public/JUSYNCTypes.h`).
- Produces:
  ```cpp
  struct FJUSYNCRawGeoCurvePayload
  {
      FString ElementName;
      double Time = -1.0;
      int64 SubsetIndex = 0;
      int64 SubsetCount = 1;
      int64 FullCurveCount = 0;
      bool bHasColors = false;
      bool bHasWidths = false;
      TArray<int32> CurveCounts;   // per curve in this payload
      TArray<FVector3f> Points;    // UE space (x,z,-y)
      TArray<FColor> Colors;       // empty or Points.Num()
      TArray<float> Widths;        // empty or Points.Num()
  };
  namespace JUSYNCRawGeoCurves
  {
      bool ParsePayload(const uint8* Data, SIZE_T Size,
                        const anari_usd_protocol::JusyncRawGeoHeader& Hdr,
                        FJUSYNCRawGeoCurvePayload& Out);
      // Fills OutMesh (may be null) and returns true when the payload was well-formed.
      bool IngestAndBuild(const TArray<uint8>& Buffer, const FString& Filename,
                          const anari_usd_protocol::JusyncRawGeoHeader& Hdr,
                          FJUSYNCCompactMeshRef& OutMesh);
      void FlushMerges();
  }
  ```

- [ ] **Step 1: Write the failing automation test**

`Private/Tests/JUSYNCRawGeoCurveTests.cpp`, guarded by `#if WITH_DEV_AUTOMATION_TESTS`:

```cpp
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FJUSYNCCurveSingleTest, "JUSYNC.RawGeo.CurveSingle",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
```

The test builds an S=1 v3 payload with 3 curves of lengths `{3,2,4}` (9 nodes). It calls
`JUSYNCRawGeoCurves::FlushMerges()` first, then
`JUSYNCRawGeoCurves::IngestAndBuild(Buffer, TEXT("clips/Wave_Geom__0.geo"), Hdr, Mesh)`, then asserts:
- returns true and `Mesh.IsValid()`;
- `Mesh->ElementName == TEXT("Streamlines")`, `Mesh->TypeName == TEXT("BasisCurves")`;
- `Mesh->Points.Num() == 9 * 4` and `Mesh->Triangles.Num() == (2 + 1 + 3) * 4 * 2`;
- `Mesh->Points[0] == FVector3f(px, pz, -py)` for the first source node;
- `Mesh->HasNormals()` and `Mesh->HasVertexColors()`;
- every normal is unit length and, for the first ring, `dot(N, P - curveCenter) > 0.9`.

Add a second test `JUSYNC.RawGeo.CurveDegenerate` with a 2-node curve whose two points are
identical: asserts `IngestAndBuild` returns true and the resulting mesh has 0 triangles (no NaN).

- [ ] **Step 2: Run the test to confirm it fails**

```powershell
cd D:\JSC_Github\jusync
.\tools\deploy-ue-plugin.ps1
& 'C:\Program Files\Epic Games\UE_5.7\Engine\Build\BatchFiles\Build.bat' JusyncSampleProjectEditor Win64 Development -Project='D:\JSC_Github\jusync-uesample\JusyncSampleProject.uproject' -WaitMutex
& 'C:\Program Files\Epic Games\UE_5.7\Engine\Binaries\Win64\UnrealEditor-Cmd.exe' 'D:\JSC_Github\jusync-uesample\JusyncSampleProject.uproject' -ExecCmds='Automation RunTests JUSYNC.RawGeo.CurveSingle; Quit' -unattended -nopause -nullrhi -nosplash -testexit='Automation Test Queue Empty' -log
```
Expected: build error `JUSYNCRawGeoCurves.h: No such file`.

- [ ] **Step 3: Implement parse + tubes**

`ParsePayload`: validate `topology == 1`, `curve_count ≤ full_curve_count`, `num_attributes ≤ 16`,
every `CurveCounts[i] ≥ 2`, and `Σ CurveCounts == num_points`. Compute section pointers with
`header_bytes + num_attributes*40`; read curve counts before widths. Map every position to
`FVector3f(x, z, -y)`; colors to `FColor(FMath::Clamp(FMath::RoundToInt(c*255),0,255), ...)`
(alpha from a 4th component, else 255).

`BuildTubes` (static in the .cpp):
- Declare the CVars in this file: `jusync.Curve.Sides` (default `4`), `jusync.Curve.DefaultRadius`
  (`0.5`), `jusync.Curve.RadiusScale` (`1.0`), `jusync.Curve.MaxNodes` (`4000000`).
- `Sides = FMath::Clamp(CVarJusyncCurveSides.GetValueOnAnyThread(), 3, 8)`.
- `radius = (Widths.Num() ? Widths[i] : DefaultRadius) * RadiusScale`.
- Per node: `T = normalize(P[i+1]-P[i-1])` (endpoints use the adjacent segment);
  `Ref = |T.Z| < 0.9 ? (0,0,1) : (0,1,0)`; `Side = normalize(T × Ref)`; `Up = Side × T`.
- Ring vertex `s`: `dir = Side*cos(2πs/Sides) + Up*sin(2πs/Sides)`; `V = P[i] + dir*radius`;
  normal `dir`; color from the node.
- Segment `i→i+1`, side `s`, `s1=(s+1)%Sides`: triangles
  `(base_i+s, base_i1+s, base_i+s1)` and `(base_i+s1, base_i1+s, base_i1+s1)`.
- Skip a segment when `|P[i+1]-P[i]| < 1e-6`.
- Drop curves with `< 2` nodes; if no geometry remains, `Out.Triangles` stays empty.
- Set `ElementName = Payload.ElementName`, `TypeName = "BasisCurves"`,
  `bHasBakedColors = bHasColors`.

`IngestAndBuild` for `SubsetCount == 1`: parse, build tubes, return true.

- [ ] **Step 4: Wire the dispatch**

In `JUSYNCSubsystem.cpp`:
- Change the signature to
  `static bool JUSYNCParseRawGeoBuffer(const TArray<uint8>& Buffer, const FString& Filename, FJUSYNCPointCloudRef& OutCloud, TArray<FJUSYNCCompactMeshRef>& OutMeshes)`.
- Accept versions 1, 2, and 3 (`if (v < 1 || v > 3) return false;`). Fill v1 implied fields as today; leave v2 topology at 0.
- Allow `num_points == 0` only when `version == 3 && topology == 1`; keep rejecting 0 for points.
- When `topology == 1`, call `JUSYNCRawGeoCurves::IngestAndBuild(Buffer, Filename, Hdr, Mesh)` and, if `Mesh.IsValid()`, add it to `OutMeshes`; return true.
- At the call site (currently line ~2042), pass `Filename` and `OutMeshes`.

Add a `JUSYNCFinalizeCompactMesh(FJUSYNCCompactMeshData&)` helper immediately below the
`CVarJusyncMeshFlipWinding` declaration that applies the flip knob, normalizes normals, and sets
`bHasBakedColors`. Call it for each RawGeo curve mesh before returning (the RawGeo branch returns
before the USD finalize loop, so it would otherwise skip the knob).

- [ ] **Step 5: Run the tests**

Repeat the deploy/build/run commands from Step 2.
Expected: `Automation Test Queue Empty`, `JUSYNC.RawGeo.CurveSingle` and `.CurveDegenerate` pass.

- [ ] **Step 6: Commit (canonical + deployed sample)**

```bash
cd D:\JSC_Github\jusync
git add UnrealPlugin/JUSYNC/Source/JUSYNC/Private/JUSYNCRawGeoCurves.h UnrealPlugin/JUSYNC/Source/JUSYNC/Private/JUSYNCRawGeoCurves.cpp UnrealPlugin/JUSYNC/Source/JUSYNC/Private/Tests/JUSYNCRawGeoCurveTests.cpp UnrealPlugin/JUSYNC/Source/JUSYNC/Private/JUSYNCSubsystem.cpp
git commit -m "Render RawGeo v3 curves as tubes in the UE client"

cd D:\JSC_Github\jusync-uesample
git add Plugins/JUSYNC/Source/JUSYNC
git commit -m "Sync RawGeo v3 curve client"
```

---

### Task 4: Curve subsets, filename keys, and rank fallback

**Files:**
- Modify: `jusync/UnrealPlugin/JUSYNC/Source/JUSYNC/Private/JUSYNCRawGeoCurves.cpp`
- Modify: `jusync/UnrealPlugin/JUSYNC/Source/JUSYNC/Public/JUSYNCElementNames.h`
- Modify: `jusync/UnrealPlugin/JUSYNC/Source/JUSYNC/Private/JUSYNCFileSpawnerActor.cpp:5527-5558`
- Modify: `jusync/UnrealPlugin/JUSYNC/Source/JUSYNC/Private/Tests/JUSYNCRawGeoCurveTests.cpp`

**Interfaces:**
- Consumes: `ParsePayload`/`BuildTubes` from Task 3.
- Produces:
  ```cpp
  // JUSYNCElementNames.h
  inline bool ParseFileRank(const FString& Filename, int32& OutRank); // "_r<digits>_" after the last "__"
  // JUSYNCRawGeoCurves
  FString StripSubsetSuffix(const FString& Filename); // drops "_s<k>" before ".geo"
  ```

- [ ] **Step 1: Write the failing tests**

Add `JUSYNC.RawGeo.CurveMerge`, `JUSYNC.RawGeo.CurveRank`, `JUSYNC.RawGeo.RankParse`:
- **Merge:** C=4, S=2; ingest `Wave_Geom__0_s1.geo` first → `Mesh` null; ingest `_s0.geo` → completion returns a mesh with 4 curves in ordinal order (the first point belongs to curve 0 from `_s0`; the last point to curve 3 from `_s1`). Ingesting `_s0` again starts a fresh merge and returns null.
- **Empty subset:** C=1, S=4; ingest k=3,2,1 (empty) then k=0 → complete mesh with 1 curve; no hang.
- **Two ranks:** same `prim_name`, files `Wave_Geom__r0_0_s0.geo`/`_s1` and `Wave_Geom__r1_0_s0.geo`/`_s1`; after all four, two separate completed meshes exist (assert via returned meshes, not `ElementName`, which is identical).
- **S drift:** ingest `_s0` with `subset_count=2`, then `_s1` with `subset_count=4`; the merge resets and does not complete.
- **Rank parse:** `Wave_Geom__r3_7_s2.geo` → 3; `Wave_Geom__7_s2.geo` → false; `Foo_r2_Geom__0.geo` → false (must be after the last `__`).

- [ ] **Step 2: Run to confirm failure**

Run the UE automation command from Task 3 Step 2 with `JUSYNC.RawGeo.CurveMerge`. Expected: build error / test not found.

- [ ] **Step 3: Implement**

- `StripSubsetSuffix`: strip `.geo`, find `_s` before the end, require all digits after it, return the remaining base. If no match, return the input unchanged.
- Global `TMap<FString, FJUSYNCCurveMerge> GRawGeoCurveMerges` + `FCriticalSection`, mirroring the point merge. Key = `StripSubsetSuffix(Filename)`.
- `FJUSYNCCurveMerge` holds `SubsetCount`, `FullCurveCount`, `Time`, `PrimName`, `FilledSlots`, `Filled`, `LastUseSeconds`, and per-ordinal `TArray<FVector3f>/TArray<FColor>/TArray<float>`.
- On a mismatch (`S` or `C`), drop the old entry and start fresh; log a warning once per key.
- Scatter: for `i < CurveCounts.Num()`, ordinal `k + i*S`; slot data is the payload's slice.
- Completion: `FilledSlots` has all `S` bits set. Concatenate slots in ordinal order into a single `FJUSYNCRawGeoCurvePayload` and build tubes.
- Policy `jusync.RawGeo.CurveProgressive`: 0 = emit subset 0 only, drop others; 1 = emit after every subset; 2 (default) = emit subset 0 + completion.
- Evict entries older than 30 s or beyond `jusync.RawGeo.RefineBudgetKPoints` (count `full_num_points * 40` bytes).
- `FlushMerges()` empties the map (called from `JUSYNCFlushRawGeoMerges`).
- Add `ParseFileRank` to `JUSYNCElementNames.h` and call it at the top of `ResolveFileRank` after the `KnownRank >= 0` check.

- [ ] **Step 4: Run the tests**

Deploy, build, run `JUSYNC.RawGeo.CurveMerge;JUSYNC.RawGeo.CurveRank;JUSYNC.RawGeo.RankParse`.
Expected: all pass; log shows one `FULL` per rank-scoped key.

- [ ] **Step 5: Commit**

```bash
cd D:\JSC_Github\jusync
git add UnrealPlugin/JUSYNC/Source/JUSYNC/Private/JUSYNCRawGeoCurves.cpp UnrealPlugin/JUSYNC/Source/JUSYNC/Public/JUSYNCElementNames.h UnrealPlugin/JUSYNC/Source/JUSYNC/Private/JUSYNCFileSpawnerActor.cpp UnrealPlugin/JUSYNC/Source/JUSYNC/Private/Tests/JUSYNCRawGeoCurveTests.cpp
git commit -m "Merge RawGeo v3 curve subsets by rank-scoped filename"

cd D:\JSC_Github\jusync-uesample
git add Plugins/JUSYNC/Source/JUSYNC
git commit -m "Sync RawGeo v3 curve subset merge"
```

---

### Task 5: Curve rendering budget, CVars, and cache hygiene

**Files:**
- Modify: `jusync/UnrealPlugin/JUSYNC/Source/JUSYNC/Private/JUSYNCRawGeoCurves.cpp`
- Modify: `jusync/UnrealPlugin/JUSYNC/Source/JUSYNC/Public/JUSYNCTypes.h:222-233`
- Modify: `jusync/UnrealPlugin/JUSYNC/Source/JUSYNC/Private/JUSYNCFileSpawnerActor.cpp:1859-1864`
- Modify: `jusync/UnrealPlugin/JUSYNC/Source/JUSYNC/Private/Tests/JUSYNCRawGeoCurveTests.cpp`

**Interfaces:**
- Consumes: everything in Task 4.
- Produces: `FJUSYNCCompactMeshData::bTransient`; CVars `jusync.Curve.Sides` (4), `jusync.Curve.DefaultRadius` (0.5), `jusync.Curve.RadiusScale` (1.0), `jusync.Curve.MaxNodes` (4000000), `jusync.RawGeo.CurveProgressive` (2).

- [ ] **Step 1: Write the failing tests**

Add to the automation suite:
- `JUSYNC.RawGeo.CurveDecimate`: a 100-node curve with `jusync.Curve.MaxNodes=10` yields at most 10 rings, keeps the first and last node, and logs one reduction line.
- `JUSYNC.RawGeo.CurveTransient`: with `CurveProgressive=2`, the subset-0 mesh has `bTransient == true`; the completed mesh has `bTransient == false`.
- `JUSYNC.RawGeo.CurveSides`: `Sides=3` → `Points.Num() == nodes*3`; `Sides=9` clamps to 8.

- [ ] **Step 2: Run to confirm failure**

Expected: `bTransient` is not a member / CVar missing.

- [ ] **Step 3: Implement**

- Add `bool bTransient = false;` to `FJUSYNCCompactMeshData` (plain member, no `UPROPERTY`).
- In `IngestAndBuild`, set `OutMesh->bTransient = (SubsetCount > 1 && !bComplete)`.
- In the spawner cache store condition, skip `MeshCache->Store` when
  `Result.CompactMeshes.ContainsByPredicate([](const FJUSYNCCompactMeshRef& M){ return M.IsValid() && M->bTransient; })`.
- Decimation: `Stride = max(1, ceil(totalNodes / MaxNodes))`; keep indices `0, Stride, 2*Stride, ...` plus the last index; require `>= 2` nodes per curve.
- CVars with the names/defaults above, read with `GetValueOnAnyThread()`.
- Log once per threshold crossing: `UE_LOG(LogJUSYNC, Warning, TEXT("RawGeo curves: %d nodes exceed budget, stride %d"), ...)`.

- [ ] **Step 4: Run the tests**

Expected: all `JUSYNC.RawGeo.*` tests pass.

- [ ] **Step 5: Commit**

```bash
cd D:\JSC_Github\jusync
git add UnrealPlugin/JUSYNC/Source/JUSYNC
git commit -m "Budget RawGeo curve rendering and skip caching coarse frames"

cd D:\JSC_Github\jusync-uesample
git add Plugins/JUSYNC/Source/JUSYNC
git commit -m "Sync RawGeo curve rendering budget"
```

---

### Task 6: Sender-side curve packer (pure, host-testable)

**Files:**
- Create: `ANARI-USD/UsdBridge/RawGeoCurvePack.h`
- Create: `ANARI-USD/tests/rawgeo_curve_pack/CMakeLists.txt`
- Create: `ANARI-USD/tests/rawgeo_curve_pack/test_rawgeo_curve_pack.cpp`

**Interfaces:**
- Consumes: `ANARI-USD/AnariUsdProtocol.h` (Task 1).
- Produces:
  ```cpp
  namespace jusync_rawgeo
  {
  struct CurveAttribute { const char* Name; uint32_t Components; const float* Data; };
  struct CurvePackInput
  {
      const char* PrimName; double Time;
      const float* Points; uint64_t NumPoints;
      const float* Colors; uint32_t ColorComponents;   // 0|3|4
      const float* Widths;                              // NumPoints or null
      const int32_t* CurveLengths; uint64_t NumCurves;
      const CurveAttribute* Attributes; uint32_t NumAttributes; // <= 16
      uint64_t SubsetIndex, SubsetCount, FullCurveCount; // FullCurveCount 0 => NumCurves
  };
  bool PackCurvePayload(const CurvePackInput& In, std::vector<uint8_t>& Out);
  std::string AddSubsetSuffix(const std::string& GeoKey, uint64_t K, uint64_t S);
  }
  ```

- [ ] **Step 1: Write the failing test**

`test_rawgeo_curve_pack.cpp` (`CHECK` macro + `main`, mirrors `tests/binary_parse/test_binary_parse.cpp`):
- `S=1`, lengths `{3,2,4}`: header `num_points==9`, `curve_count==3`, `full_curve_count==3`,
  `width_components==1`, `payload_bytes == 9*12 + 9*3*4 + 9*4 + 3*4`; `valid()` on the buffer.
- `S=2`: subset 0 has 7 points / 2 curves with counts `{3,4}`; subset 1 has 2 points / 1 curve with `{2}`; ordinals round-trip.
- `S=4, C=1`: subsets 1–3 have `num_points==0`, `curve_count==0`, `payload_bytes==0` and still pass `valid()`.
- Reject: a length `< 2`; `Σ lengths != NumPoints`; `SubsetCount == 0`; `SubsetIndex >= SubsetCount`; `ColorComponents == 2`; `NumAttributes == 17`.
- `AddSubsetSuffix("a.geo", 3, 4) == "a_s3.geo"`; `AddSubsetSuffix("a.geo", 0, 1) == "a.geo"`.

- [ ] **Step 2: Build it and confirm failure**

`tests/rawgeo_curve_pack/CMakeLists.txt`:
```cmake
cmake_minimum_required(VERSION 3.16)
project(rawgeo_curve_pack_test CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
add_executable(test_rawgeo_curve_pack test_rawgeo_curve_pack.cpp)
target_include_directories(test_rawgeo_curve_pack PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../..)
```

Run:
```powershell
cmake -S D:\JSC_Github\ANARI-USD\tests\rawgeo_curve_pack -B D:\JSC_Github\ANARI-USD\build-out\rawgeo_curve_pack
cmake --build D:\JSC_Github\ANARI-USD\build-out\rawgeo_curve_pack --config Release
```
Expected: `RawGeoCurvePack.h: No such file`.

- [ ] **Step 3: Implement the packer**

Algorithm (header-only, `inline`):
1. Validate inputs (nulls, counts, components, `NumAttributes ≤ 16`, `SubsetCount ≥ 1`,
   `SubsetIndex < SubsetCount`, all lengths `≥ 2`, `Σ lengths == NumPoints`).
2. `C = FullCurveCount ? FullCurveCount : NumCurves`; require `NumCurves ≤ C`.
   For `S > 1`, require `NumCurves == ceil((C - K) / S)`; for `S == 1`, require `C == NumCurves`.
3. Offset table: `Start[i] = Σ_{j<i} lengths[j]`.
4. Selected ordinals: `c = K; c < C; c += S` (iterate `i` and derive `c`).
5. `selPoints = Σ lengths[c]`; build the v3 header with `topology=1`,
   `curve_count=selected`, `full_curve_count=C`, `width_components = Widths ? 1 : 0`,
   `attr_uniform_count = selPoints`, `flags = 0`.
6. `payload_bytes = selPoints*12 + selPoints*colorComps*4 + selPoints*1*4(if widths)
   + selected*4 + selPoints*Σ attrComponents*4`.
7. Write header, desc table, then sections in order: points (per selected curve, contiguous),
   colors, attributes, curve counts, widths.
8. For an empty selection, emit only the header with `payload_bytes = 0`.

- [ ] **Step 4: Run the test**

```powershell
cmake --build D:\JSC_Github\ANARI-USD\build-out\rawgeo_curve_pack --config Release
D:\JSC_Github\ANARI-USD\build-out\rawgeo_curve_pack\Release\test_rawgeo_curve_pack.exe
```
Expected: `OK rawgeo_curve_pack`, exit 0.

- [ ] **Step 5: Commit**

```bash
cd D:\JSC_Github\ANARI-USD
git add UsdBridge/RawGeoCurvePack.h tests/rawgeo_curve_pack
git commit -m "Add host-testable RawGeo v3 curve packer"
```

---

### Task 7: Sender writer integration (gates, cap, idle, store)

**Files:**
- Modify: `ANARI-USD/UsdBridge/UsdBridgeUsdWriter.h:189-193`
- Modify: `ANARI-USD/UsdBridge/UsdBridgeUsdWriter.cpp:1837-2077`
- Modify: `ANARI-USD/tests/rawgeo_curve_pack/test_rawgeo_curve_pack.cpp` (only if a helper moves)

**Interfaces:**
- Consumes: `jusync_rawgeo::PackCurvePayload`, `AddSubsetSuffix` (Task 6).
- Produces:
  ```cpp
  bool UsdBridgeUsdWriter::TryStoreRawGeo(UsdBridgePrimCache* cacheEntry,
                                          const UsdBridgeCurveData& data, double timeStep);
  ```

- [ ] **Step 1: Extract shared plumbing (no behavior change)**

Add static helpers used by both overloads:
- `static bool RawGeoCurveEnabled()` — `JUSYNC_RAWGEO_CURVES=1`.
- `static uint64_t RawGeoCurveSubsets()` — `JUSYNC_RAWGEO_CURVE_SUBSETS`, default 4, clamp 1..8.
- `static bool RawGeoGeoKey(UsdBridgePrimCache*, double timeStep, std::string& Out)` — the existing clip-stage/extension logic.
- `static uint64_t RawGeoHash(const void* data, size_t bytes, uint64_t seed)` — FNV-1a.

The call site `UsdBridge.cpp:991` already dispatches `BRIDGE_USDWRITER.TryStoreRawGeo(cache, geomData, timeStep)`
inside a template, so a new non-template `UsdBridgeCurveData` overload is selected automatically and
returning `true` skips `UpdateUsdGeometry`. Include `RawGeoCurvePack.h` in the writer `.cpp`.

Also change the **point** path's local pack type to `JusyncRawGeoHeaderV2`, in both the
`packRawGeo` lambda parameter and the `anari_usd_protocol::JusyncRawGeoHeader hdr;` declaration,
and set `outHdr.version = 2`, so point payloads stay v2.

- [ ] **Step 2: Implement the curve overload**

- Reject (return false) unless `RawGeoEnabled() && RawGeoCurveEnabled() && g_rankMemoryStore &&
  cacheEntry && data.Points && data.NumPoints > 0 && data.CurveLengths && data.NumCurveLengths > 0
  && RawGeoFloatComponents(data.PointsType) == 3`.
- `colors`: per point when `data.Colors && !data.PerPrimColors`; components 3/4 else 0.
- `widths`: `data.Scales` only when `data.ScalesType == FLOAT`; otherwise omit.
  Never expand `data.UniformScale`.
- Attributes: copy `data.Attributes[a]` where `Data && !PerPrimData`, up to 16,
  components `1..4`.
- `S = RawGeoCurveSubsets()`; `baseKey = geoKey`; pack all `k = 0..S-1` into
  `std::vector<std::vector<uint8_t>>` **before** storing any.
- Reject the whole frame when any payload exceeds `1536 MB` (same constant as points).
- Idle: hash `(NumPoints, lengths, points, colors, widths)`; skip all subsets when the hash
  equals the last one for `baseKey` for `JUSYNC_RAWGEO_IDLE_FRAMES` consecutive frames.
  Use a separate `std::unordered_map<std::string, uint64_t>` + counter.
- For each `k`: `key = AddSubsetSuffix(baseKey, k, S)`; `GetDiffCapture().CapturePreStore(key)`;
  `g_rankMemoryStore->StoreFile(key, buf.data(), buf.size(), "application/x-jusync-geo")`.
- Never emit the PLY snapshot for curves; never apply stride.

- [ ] **Step 3: Compile the sender**

The `jusync-sender` image compiles ANARI SDK, OpenUSD, and this repo during `docker build`;
a successful image build is the compile check (30–90 min on the first run):

```powershell
cd D:\JSC_Github\ANARI-USD
.\docker\jusync-sender\build.ps1 -Tag rawgeo-v3
```
Expected: `OK: jusync-sender:rawgeo-v3`, no compiler errors in `UsdBridgeUsdWriter.cpp`.
Record the image tag in the commit message.

- [ ] **Step 4: Commit**

```bash
cd D:\JSC_Github\ANARI-USD
git add UsdBridge/UsdBridgeUsdWriter.h UsdBridge/UsdBridgeUsdWriter.cpp
git commit -m "Emit RawGeo v3 curve subsets behind an env gate"
```

---

### Task 8: End-to-end matrix, regression, and rollout docs

**Files:**
- Modify: `jusync/docs/superpowers/specs/2026-10-23-rawgeo-v3-curves-design.md` (status + `header_bytes` note)
- Modify: `jusync/UnrealPlugin/JUSYNC/Source/JUSYNC/Private/Tests/JUSYNCRawGeoCurveTests.cpp` (fixture-driven dispatch test)
- Possibly modify: `jusync/tools/deploy-ue-plugin.ps1` (only if the header needs an explicit copy entry)

**Interfaces:**
- Consumes: all prior tasks.
- Produces: a recorded pass/fail table for the spec's 10-row matrix.

- [ ] **Step 1: Run the full C++ suite**

```powershell
ctest --test-dir D:\JSC_Github\jusync\build -C Release --output-on-failure
```
Expected: all tests pass, including `rawgeo_v3` and `binary_static_check`.

- [ ] **Step 2: Run the UE automation suite**

```powershell
cd D:\JSC_Github\jusync; .\tools\deploy-ue-plugin.ps1
& 'C:\Program Files\Epic Games\UE_5.7\Engine\Build\BatchFiles\Build.bat' JusyncSampleProjectEditor Win64 Development -Project='D:\JSC_Github\jusync-uesample\JusyncSampleProject.uproject' -WaitMutex
& 'C:\Program Files\Epic Games\UE_5.7\Engine\Binaries\Win64\UnrealEditor-Cmd.exe' 'D:\JSC_Github\jusync-uesample\JusyncSampleProject.uproject' -ExecCmds='Automation RunTests JUSYNC.RawGeo; Quit' -unattended -nopause -nullrhi -nosplash -testexit='Automation Test Queue Empty' -log
```
Expected: all `JUSYNC.RawGeo.*` tests pass.

- [ ] **Step 3: Run the non-MPI and single-rank fixture matrix**

```powershell
python D:\JSC_Github\jusync\tests\scripts\make_rawgeo_v3.py --out "$env:TEMP\rawgeo_v3" --curves 6 --points-per-curve 5 --subsets 4
python D:\JSC_Github\jusync\tests\scripts\make_rawgeo_v3.py --out "$env:TEMP\rawgeo_v3_solo" --curves 3 --points-per-curve 4 --subsets 1
python D:\JSC_Github\jusync\tests\scripts\make_rawgeo_v3.py --verify "$env:TEMP\rawgeo_v3"
python D:\JSC_Github\jusync\tests\scripts\make_rawgeo_v3.py --verify "$env:TEMP\rawgeo_v3_solo"
```
Expected: `OK 4 files`, `OK 1 files`. Add the fixture directory to the spawner's watch path,
confirm the log shows one completed curve mesh per directory with the expected curve count.

- [ ] **Step 4: Run the MPI fixture matrix**

```powershell
python D:\JSC_Github\jusync\tests\scripts\make_rawgeo_v3.py --out "$env:TEMP\rawgeo_v3_mpi" --curves 4 --points-per-curve 3 --subsets 2 --rank 0
python D:\JSC_Github\jusync\tests\scripts\make_rawgeo_v3.py --out "$env:TEMP\rawgeo_v3_mpi" --curves 2 --points-per-curve 3 --subsets 2 --rank 1
python D:\JSC_Github\jusync\tests\scripts\make_rawgeo_v3.py --verify "$env:TEMP\rawgeo_v3_mpi"
```
Expected: one actor per rank (two actors, same `ElementName`, different ranks); no cross-rank
completion. If a real 2-rank sender is available, repeat with
`JUSYNC_RAWGEO=1 JUSYNC_RAWGEO_CURVES=1 JUSYNC_RAWGEO_CURVE_SUBSETS=2 mpiexec -n 2 ...`
and confirm the same.

- [ ] **Step 5: Regression check the point path**

Run the existing milestone-2 10M-point RawGeo case with `JUSYNC_RAWGEO=1` (curves off).
Expected: identical point counts/hashes to the pre-branch run; payload version is 2.

- [ ] **Step 6: Update the spec and commit**

Set the spec status to `Implemented (branch feat/rawgeo-v3-curves)`, note the point path stays on
v2, and record the matrix results.

```bash
cd D:\JSC_Github\jusync
git add docs/superpowers/specs/2026-10-23-rawgeo-v3-curves-design.md UnrealPlugin/JUSYNC/Source/JUSYNC/Private/Tests/JUSYNCRawGeoCurveTests.cpp
git commit -m "Verify RawGeo v3 curves across the non-MPI and MPI matrix"

cd D:\JSC_Github\jusync-uesample
git add Plugins/JUSYNC/Source/JUSYNC
git commit -m "Verify RawGeo v3 curve client"
```

---

## Self-Review Notes

- **Spec coverage:** header (§4) → Task 1; subset semantics (§4.4) → Tasks 2/4/6; MPI contract (§5) →
  Tasks 4 and 8; sender (§6) → Tasks 6/7; client (§7) → Tasks 3–5; compatibility (§8) → Tasks 1/7/8;
  tests (§9) → Tasks 1–4, 6, 8.
- **Deliberate deviation from the spec text:** the sender's point path emits v2, not v3, to keep
  existing point clients working during rollout. The client accepts 1, 2, and 3.
- **Rank resolution:** `ResolveFileRank` gains a filename fallback; the existing file-list lookup and
  rank-0 fallback remain unchanged.
- **Winding:** tubes are built in UE space with an outward ring order; `jusync.Mesh.FlipWinding`
  remains the override knob and is applied by `JUSYNCFinalizeCompactMesh`.
