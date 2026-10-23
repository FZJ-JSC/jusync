# Native Unreal Pathlines Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add stable-ID, scalar-colored pathline tubes for live RawGeo sphere point streams in the JUSYNC Unreal plugin, driven by the sender's `particle_id` attribute, with deterministic track selection, filtering, playback display cutoff, and no regression to USD surfaces or RawGeo v1/v2/v3 readers.

**Architecture:** The sender synthesizes a wire-compatible `particle_id` float attribute from `primitive.id` and writes it through the existing RawGeo v2 attribute table. UE preserves every RawGeo point attribute, marks complete RawGeo clouds, and feeds only those clouds to a pathline manager. The manager owns per-element stable-ID tracks, selects a bounded deterministic subset, builds CPU tube meshes on a serialized background worker, and uploads them on the game thread through `RealtimeMeshComponent`. The file spawner mirrors controls, feeds committed samples, and maps playback scrubbing to a display sample ID.

**Tech Stack:** C++17 host code, Unreal Engine 5.7 C++, UE Automation tests, RealtimeMeshComponent, CMake/CTest host tests.

---

## Global Constraints

- Image/version remains `6.0.5`; do not bump a major protocol version.
- Cylinders, cones, and glyphs stay USD surfaces. Only `GEOM_SPHERE` / `IsPointCloud` may use RawGeo point streaming.
- RawGeo v1 and v2 point clouds must remain readable; v3 curves must remain unchanged.
- Stable-ID pathlines only. Do not add Niagara or velocity-field integration.
- Pathlines are disabled by default. Defaults: `4096` tracks, `128` history samples, `524288` total nodes, `4` tube sides, `0.5` radius, ID attribute `particle_id`, scalar attribute `attribute0`, selection seed `1337`.
- Pathline state is independent of sample-cache eviction. Disabling pathlines clears pathline state and actors.
- Pathlines ingest only clouds with `bFromRawGeo == true && bComplete == true`. Never ingest USD point clouds or coarse/intermediate RawGeo subsets.
- The canonical plugin is `D:\JSC_Github\jusync\UnrealPlugin\JUSYNC\Source\JUSYNC`; the sample copy is `D:\JSC_Github\jusync-uesample\Plugins\JUSYNC\Source\JUSYNC`. They must be byte-identical after the change.
- Do not push commits. Do not merge while Critical/Important review findings remain open.
- Use `superpowers:test-driven-development` for each code task: failing test, minimal implementation, passing test.

## Review Focus

- Wire compatibility: the synthetic `particle_id` counts toward the existing 16-attribute and `payload_bytes` limits; `JUSYNC_RAWGEO_STRIDE` must stride IDs exactly like positions.
- Exact ID reconstruction across 16/32/64-bit IDs without float rounding.
- Completeness gating: a partially merged cloud must never add pathline nodes.
- Gap/disappearance semantics: a missing ID creates a break; an omitted element does not age tracks; a dropped backlog creates a break on all tracks.
- Playback: scrubbing changes only the display cutoff, not parsed sample state.
- Threading: worker builds geometry; RealtimeMesh uploads and actor lifecycle stay on the game thread.
- Determinism: selection, tie-breaking, and scalar color must be stable for a fixed seed and input.

---

## File Map

### Sender (`D:\JSC_Github\ANARI-USD`)

- `UsdBridge/RawGeoParticleId.h` (new): ID-width detection and exact 16-bit float packing.
- `UsdBridge/UsdBridgeUsdWriter.cpp`: synthesize `particle_id` in `TryStoreRawGeo` for point clouds.
- `tests/rawgeo_particle_id/CMakeLists.txt` (new): standalone host test.
- `tests/rawgeo_particle_id/test_rawgeo_particle_id.cpp` (new): width, stride, and exactness tests.

### Canonical UE plugin (`D:\JSC_Github\jusync\UnrealPlugin\JUSYNC\Source\JUSYNC`)

- `Public/JUSYNCTypes.h`: point attributes, completeness, RawGeo flag, attribute accessors.
- `Private/Core/JUSYNCSubsystem.cpp`: preserve attributes in whole-frame and merge paths; set flags.
- `Private/Tests/JUSYNCRawGeoAttributeTests.cpp` (new): parser/attribute automation tests.
- `Public/JUSYNCPathlineManager.h` (new): settings, stats, manager API.
- `Private/Pathlines/JUSYNCPathlineManager.cpp` (new): state, selection, ingestion, geometry, worker.
- `Public/JUSYNCPathlineActor.h` (new): RealtimeMesh pathline actor.
- `Private/Pathlines/JUSYNCPathlineActor.cpp` (new): RealtimeMesh upload.
- `Private/Tests/JUSYNCPathlineTests.cpp` (new): ID decode, selection, gaps, filter/color, geometry.
- `Public/JUSYNCFileSpawnerActor.h`: pathline controls, manager pointer, cache rebuild declaration.
- `Private/Spawner/JUSYNCFileSpawnerActor.cpp`: settings sync, ingest, playback cutoff, lifecycle, rebuild.
- `Public/JUSYNCPointCloudSpawner.h`: `GetSpawnLocation` / `GetSpawnScale`.
- `Private/PointCloud/JUSYNCPointCloudSpawner.cpp`: trivial accessors if not inline.
- `Private/Tests/JUSYNCTestHooks.h` (new): parser test hook declaration under automation guard.

### Mirror

- `D:\JSC_Github\jusync-uesample\Plugins\JUSYNC\Source\JUSYNC`: byte-identical copy of the canonical `Source\JUSYNC` tree.

---

## Task 1: Sender `particle_id` packing

**Files:**
- Create: `UsdBridge/RawGeoParticleId.h`
- Create: `tests/rawgeo_particle_id/CMakeLists.txt`
- Create: `tests/rawgeo_particle_id/test_rawgeo_particle_id.cpp`
- Modify: `UsdBridge/UsdBridgeUsdWriter.cpp`

- [ ] **Step 1: Write the failing host test.**

Cover:
- `UINT` IDs `0`, `65535`, `65536`, `0xFFFFFFFF` produce 2 float chunks.
- `ULONG` IDs `0xFFFFFFFFFFFFFFFF`, `0x0123456789ABCDEF` produce 4 exact float chunks.
- `USHORT` produces 1 chunk; `ULONG` values <= 0xFFFF still use 2 chunks (type width wins).
- Source stride `2` selects every second ID.
- Decoding each float with `static_cast<uint32_t>(value)` and shifting by `16 * component` reconstructs the exact integer.

Test entry point:
```cpp
#include "UsdBridge/RawGeoParticleId.h"
#include "UsdBridge/Common/UsdBridgeData.h"
```

- [ ] **Step 2: Run the test and confirm it fails.**

```powershell
cmake -S tests/rawgeo_particle_id -B build-out/tests/rawgeo_particle_id
cmake --build build-out/tests/rawgeo_particle_id --config Release
ctest --test-dir build-out/tests/rawgeo_particle_id -C Release --output-on-failure
```

Expected: configuration/build failure because `RawGeoParticleId.h` does not exist.

- [ ] **Step 3: Implement `RawGeoParticleId.h`.**

Required interface:
```cpp
namespace anari_usd_rawgeo
{
struct ParticleIdEncoding
{
    uint32_t Components = 0;   // 1, 2, or 4
    uint32_t Bytes = 0;        // source integer width
};

ParticleIdEncoding GetParticleIdEncoding(UsdBridgeType Type);

// Packs IDs as exact 16-bit chunks stored in floats. `SourceCount` is the
// number of source instances; `SourceStride` selects every Nth occurrence.
// Appends to `Out` and returns false for an unsupported type.
bool PackParticleIds(const void* Ids, UsdBridgeType Type,
                     uint64_t SourceCount, uint64_t SourceStride,
                     std::vector<float>& Out);
}
```

Implementation rules:
- `UCHAR`/`CHAR` -> 1 component, 1 source byte.
- `USHORT`/`SHORT` -> 1 component, 2 source bytes.
- `UINT`/`INT` -> 2 components, 4 source bytes.
- `ULONG`/`LONG` -> 4 components, 8 source bytes.
- `FLOAT`/`DOUBLE` use `static_cast<uint64_t>(numeric value)` and the same width rules.
- Use `memcpy` to avoid alignment UB.
- Output chunk `c` is `(id >> (16 * c)) & 0xFFFF` cast to `float`; exact for every chunk.

- [ ] **Step 4: Integrate into `TryStoreRawGeo`.**

In the point-cloud overload:
- After collecting real attributes, if `data.InstanceIds` is non-null, `data.InstanceIdsType` is supported, and no real attribute is named `particle_id`, and `numAttrs < 16`:
  - append `particle_id` to `attrDescs`;
  - mark it with sentinel `attrSrcIdx[numAttrs] = UINT32_MAX`;
  - compute components from `GetParticleIdEncoding`.
- Build a full-resolution `std::vector<float> syntheticIds` once before the `packRawGeo` lambda (not inside the lambda, so idle full-res bursts remain valid).
- In the attribute packing loop, select `syntheticIds.data()` for the sentinel and apply the same `effStride` logic.
- Include the synthetic descriptor in `descBytes`, `numAttrs`, `attrBytes`, and `payload_bytes`.
- Keep v2 header version and all subset fields unchanged.

- [ ] **Step 5: Re-run the host test and existing sender tests.**

```powershell
cmake --build build-out/tests/rawgeo_particle_id --config Release
ctest --test-dir build-out/tests/rawgeo_particle_id -C Release --output-on-failure
cmake -S tests/rawgeo_point_gate -B build-out/tests/rawgeo_point_gate
cmake --build build-out/tests/rawgeo_point_gate --config Release
ctest --test-dir build-out/tests/rawgeo_point_gate -C Release --output-on-failure
cmake -S tests/rawgeo_curve_pack -B build-out/tests/rawgeo_curve_pack
cmake --build build-out/tests/rawgeo_curve_pack --config Release
ctest --test-dir build-out/tests/rawgeo_curve_pack -C Release --output-on-failure
```

Expected: all three pass.

- [ ] **Step 6: Commit.**

```powershell
git add UsdBridge/RawGeoParticleId.h UsdBridge/UsdBridgeUsdWriter.cpp tests/rawgeo_particle_id
git commit -m "feat: pack stable particle ids in RawGeo point clouds"
```

---

## Task 2: Preserve RawGeo point attributes in UE

**Files:**
- Modify: `Public/JUSYNCTypes.h`
- Modify: `Private/Core/JUSYNCSubsystem.cpp`
- Create: `Private/Tests/JUSYNCTestHooks.h`
- Create: `Private/Tests/JUSYNCRawGeoAttributeTests.cpp`

- [ ] **Step 1: Add UE point-attribute types and failing tests.**

Add to `FJUSYNCPointCloudData`:
```cpp
USTRUCT(BlueprintType)
struct JUSYNC_API FJUSYNCPointAttribute
{
    GENERATED_BODY()

    UPROPERTY(BlueprintReadOnly, Category = "JUSYNC")
    FString Name;

    UPROPERTY(BlueprintReadOnly, Category = "JUSYNC")
    int32 Components = 0;

    // Point-major: Values[Index * Components + Component].
    TArray<float> Values;
};

UPROPERTY(BlueprintReadOnly, Category = "JUSYNC")
TArray<FJUSYNCPointAttribute> Attributes;

UPROPERTY(BlueprintReadOnly, Category = "JUSYNC")
bool bComplete = true;

UPROPERTY(BlueprintReadOnly, Category = "JUSYNC")
int32 FullPointCount = 0;

UPROPERTY(BlueprintReadOnly, Category = "JUSYNC")
bool bFromRawGeo = false;
```

Add accessors:
```cpp
const FJUSYNCPointAttribute* FindAttribute(const FString& Name) const;
bool GetAttributeScalar(int32 Index, const FString& Name, int32 Component, float& Out) const;
FVector GetPosition(int32 Index) const;
float GetWidth(int32 Index) const;
```
`GetPosition` must read `LidarPoints[i].Location` when `bHasLidarPoints`, else `Positions[i]`, converting `FLidarPointCloudPoint` coordinates the same way as the parser (`x`, `z`, `-y` are already stored, so return the stored location directly).
`GetWidth` returns `Widths[i]` when valid, else `1.0f`.

Write `JUSYNCRawGeoAttributeTests.cpp` using `IMPLEMENT_SIMPLE_AUTOMATION_TEST` and a helper that builds a v2 whole-frame buffer:
- header + one/two attribute descriptors + positions + `particle_id` chunks + `attribute0` values.
- Include the `FJUSYNCFileData`/buffer path through a test hook.

Add `JUSYNCTestHooks.h`:
```cpp
#pragma once
#include "JUSYNCTypes.h"
#if WITH_DEV_AUTOMATION_TESTS
JUSYNC_API bool JUSYNCParseRawGeoForTest(const TArray<uint8>& Buffer, FJUSYNCPointCloudRef& OutCloud);
#endif
```

In `JUSYNCSubsystem.cpp`, implement the hook under `#if WITH_DEV_AUTOMATION_TESTS` by calling the existing static parser and discarding meshes.

Tests:
- Whole-frame v2 with `particle_id` and `attribute0` preserves both names, components, and exact values.
- `bFromRawGeo == true`, `bComplete == true`, `FullPointCount == PointCount`.
- A v1 buffer remains readable and has no attributes.
- Subset buffer with one of two subsets emits `bComplete == false`; after the second subset the emitted cloud is complete and attributes are fully populated at every lattice index.
- `GetAttributeScalar` and `GetPosition` handle `LidarPoints` and `Positions` storage.

- [ ] **Step 2: Run the test and confirm it fails.**

Build the editor target (Task 8 command) or run:
```powershell
& 'C:\Program Files\Epic Games\UE_5.7\Engine\Build\BatchFiles\Build.bat' JusyncSampleProjectEditor Win64 Development -Project='D:\JSC_Github\jusync-uesample\JusyncSampleProject.uproject' -WaitMutex -FromMsBuild
```
Expected: compile failure on missing fields/hook.

- [ ] **Step 3: Implement parser preservation.**

Whole-frame:
- Build `TArray<FJUSYNCPointAttribute>` by walking descriptors and section pointers.
- Copy `N * Components` floats per attribute with `FMemory::Memcpy`.
- Set `bFromRawGeo = true`, `bComplete = true`, `FullPointCount = (int32)N`.

Subset merge:
- Extend `FJUSYNCRawGeoMerge` with `TArray<FJUSYNCPointAttribute> Attributes;` and `bool bHasAttributes;`.
- On merge creation, allocate each attribute to `FullN * Components` zeroed and copy the current subset into its lattice slots.
- Validate attribute signature (count, names, components); a mismatch resets that merge.
- On partial/coarse output, compact attributes alongside points.
- On complete output, move the full attribute arrays into the cloud.
- `JUSYNCBuildCloudFromMergedPoints` accepts attributes and a `bComplete` flag. Pass `false` for coarse/previews, `true` for a complete merge.
- Set `bFromRawGeo = true` for every RawGeo point cloud; leave USD clouds unchanged.

- [ ] **Step 4: Re-run the attribute tests and build.**

Expected: all `JUSYNC.RawGeo.Attributes.*` tests pass and the editor target builds.

- [ ] **Step 5: Commit in canonical and sample.**

Commit canonical first, then copy the changed files to the sample and commit there. Do not push.

---

## Task 3: Pathline core and tests

**Files:**
- Create: `Public/JUSYNCPathlineManager.h`
- Create: `Private/Pathlines/JUSYNCPathlineManager.cpp`
- Create: `Private/Tests/JUSYNCPathlineTests.cpp`
- Modify: `JUSYNC.Build.cs` only if a new module is needed (it should not be).

- [ ] **Step 1: Write failing core tests.**

Test names under `JUSYNC.Pathlines`:
- `DecodeParticleId16/32/64` reconstructs exact IDs from 1/2/4 float chunks.
- `SelectionIsDeterministic` returns the same keys for the same seed and input; a different seed changes the selected subset.
- `SelectionHonorsMaxTracks` selects at most `MaxTracks`, filling vacant slots from new IDs by score.
- `MissingIdBreaksTrack` produces two disconnected runs when an ID is absent for one sample.
- `DuplicateIdUsesFirstOccurrence` counts the duplicate and does not add a second node.
- `OmittedElementDoesNotAge` leaves node count unchanged for an element not fed.
- `FilterHidesNonMatchingAndColorUsesLatestScalar` filters by the latest node scalar at the display cutoff and colors by the same value through the LUT.
- `DisplayCutoffScrubsWithoutChangingState` changes visible node count and leaves total node count unchanged.
- `GeometryBuildsTubes` produces `(segments * Sides * 2)` vertices and `(segments * Sides * 2)` triangles for a simple two-node track, with normals and vertex colors.
- `HistoryAndNodeBudgetClamp` never exceeds `MaxNodes` and keeps at least 2 nodes per active track when possible.

Use a small helper to build `FJUSYNCPointCloudRef` with `LidarPoints`, `particle_id`, and `attribute0`.

- [ ] **Step 2: Run the tests and confirm they fail.**

Build the editor target; expect unresolved symbols/header missing.

- [ ] **Step 3: Implement public settings and stats.**

`FJUSYNCPathlineSettings` fields:
```cpp
bool bEnabled = false;
int32 MaxTracks = 4096;
int32 HistorySamples = 128;
int32 MaxNodes = 524288;
int32 TubeSides = 4;
float Radius = 0.5f;
bool bUsePointWidth = false;
float RadiusScale = 1.0f;
FString IdAttribute = TEXT("particle_id");
FString ScalarAttribute = TEXT("attribute0");
int32 ScalarComponent = 0;
bool bFilterEnabled = false;
float FilterMin = 0.0f;
float FilterMax = 1.0f;
bool bColorByScalar = true;
float ColorMin = 0.0f;
float ColorMax = 1.0f;
FLinearColor FlatColor = FLinearColor::White;
uint64 SelectionSeed = 1337;
int32 TrackExpirySamples = 64;
float PendingTimeoutSeconds = 2.0f;
UMaterialInterface* Material = nullptr;
```

`FJUSYNCPathlineStats`: active tracks, total nodes, visible tracks, dropped samples, duplicate IDs, last ingest ms, last build ms.

- [ ] **Step 4: Implement state and ingestion.**

- Track key is `(ElementName, rank, particleId)` internally; the public element key is the canonical element name plus rank.
- `IngestSampleNow(Element, Cloud, Rank, SampleId, LUT)`:
  - reject `!bFromRawGeo || !bComplete || !IsValid()`;
  - decode IDs; fallback to point index when the attribute is absent or empty;
  - first duplicate wins; increment duplicate stats;
  - for each track, add a node with position, width, latest scalar, sample id;
  - mark `bBreak` when the ID was absent from the element's previous sample or `bPendingBreak` is set;
  - advance `SampleCounter` and expire tracks not seen for `TrackExpirySamples`;
  - select at most `MaxTracks` IDs by deterministic hash score, merging existing tracks and current IDs;
  - enforce per-track history and global node budget by dropping oldest nodes first.
- `SetDisplaySampleId(uint64)` stores the cutoff; `MAX_uint64` means live edge.
- `Reset` clears state and increments a worker generation.
- `SetSettings` detects structural changes (`IdAttribute`, `ScalarAttribute`, `SelectionSeed`, `MaxTracks`, `HistorySamples`, `MaxNodes`, enable state) and clears state, setting `bResampleRequested`.
- Visual changes only set a rebuild flag.

- [ ] **Step 5: Implement selection and geometry.**

- Hash score: mix `SelectionSeed`, element name chars, rank, and particle ID with a stable 64-bit avalanche.
- Sort by `(score desc, particleId asc)` for deterministic ties.
- `BuildGeometry(Element, Cutoff, OutMesh, OutVisibleTracks)`:
  - for each selected track, collect nodes with `SampleId <= Cutoff`;
  - representative scalar is the latest such node; filter applies to it;
  - split at `bBreak`;
  - skip runs shorter than 2 nodes;
  - for each segment, build two rings with `Sides` clamp `[3, 8]`, using a stable frame from the segment direction and a reference axis;
  - radius = `Radius * RadiusScale`; if `bUsePointWidth` and node width > 0, multiply by the mean width;
  - color all vertices with the representative scalar through `LUT` (normalized by `ColorMin/ColorMax`), else flat color;
  - append normals, UVs, colors, and triangles.
- Empty geometry must return a valid empty mesh with `ElementName` set so the actor can clear.

- [ ] **Step 6: Add the serialized worker.**

- `EnqueueSample` pushes a shared work item and starts one `AsyncTask(AnyBackgroundThreadNormalTask)` worker if idle.
- The worker processes items under a state lock and posts built meshes to a game-thread pending map.
- Drop items when the queue exceeds 64; set `bPendingBreak` on every element state.
- `PumpGameThread` drains pending meshes and applies them through the provided callback/actor store.
- `Reset` increments the generation and invalidates queued results.
- Tests use `IngestSampleNow` and `BuildGeometry` directly, not the async queue.

- [ ] **Step 7: Run core tests and commit.**

Expected: all `JUSYNC.Pathlines.*` tests pass. Commit canonical and mirror.

---

## Task 4: Pathline actor and RealtimeMesh rendering

**Files:**
- Create: `Public/JUSYNCPathlineActor.h`
- Create: `Private/Pathlines/JUSYNCPathlineActor.cpp`
- Modify: `Private/Pathlines/JUSYNCPathlineManager.cpp` (actor application in `PumpGameThread`)

- [ ] **Step 1: Add the actor.**

```cpp
UCLASS()
class JUSYNC_API AJUSYNCPathlineActor : public AActor
{
    GENERATED_BODY()
public:
    AJUSYNCPathlineActor();
    void ApplyMesh(const FJUSYNCCompactMeshData& Mesh, UMaterialInterface* Material);
    void ClearMesh();
private:
    UPROPERTY(VisibleAnywhere)
    TObjectPtr<URealtimeMeshComponent> MeshComponent;
};
```

- [ ] **Step 2: Implement upload.**

- Create the component in the constructor, set collision off, cast shadows on, and use `UMaterialInterface*` if provided; otherwise fall back to the subsystem's vertex-color material or engine default.
- Build streams with `JUSYNCBuildRealtimeMeshStreams`; create/update section group `0/"USDGroup"` and polygroup `0`.
- Call `MeshComponent->SetMaterial(0, Material)` and `MarkRenderStateDirty`.
- `ClearMesh` empties the section group or hides the component.

- [ ] **Step 3: Apply in `PumpGameThread`.**

- Accept `UWorld*`, `FJUSYNCPointCloudSpawner*`, and material.
- Spawn one `AJUSYNCPathlineActor` per canonical element key; never per rank.
- Set the actor transform from `Spawner->GetSpawnLocation()` and uniform `Spawner->GetSpawnScale()`.
- Destroy actors for elements that disappear or when disabled/reset.
- Keep actor lifecycle and upload game-thread only.

- [ ] **Step 4: Build and commit.**

Expected: editor target builds; no test renderer required.

---

## Task 5: File-spawner integration and controls

**Files:**
- Modify: `Public/JUSYNCFileSpawnerActor.h`
- Modify: `Private/Spawner/JUSYNCFileSpawnerActor.cpp`
- Modify: `Public/JUSYNCPointCloudSpawner.h`
- Modify: `Private/PointCloud/JUSYNCPointCloudSpawner.cpp`

- [ ] **Step 1: Add controls.**

Add a `JUSYNC|Spawner|Pathlines` section with:
```cpp
bool bEnablePathlines = false;
int32 PathlineMaxTracks = 4096;
int32 PathlineHistorySamples = 128;
int32 PathlineMaxNodes = 524288;
int32 PathlineTubeSides = 4;
float PathlineRadius = 0.5f;
bool bPathlineUsePointWidth = false;
float PathlineRadiusScale = 1.0f;
FString PathlineIdAttribute = TEXT("particle_id");
FString PathlineScalarAttribute = TEXT("attribute0");
int32 PathlineScalarComponent = 0;
bool bPathlineFilterEnabled = false;
float PathlineFilterMin = 0.0f;
float PathlineFilterMax = 1.0f;
bool bPathlineColorByScalar = true;
float PathlineColorMin = 0.0f;
float PathlineColorMax = 1.0f;
FLinearColor PathlineFlatColor = FLinearColor::White;
int32 PathlineSelectionSeed = 1337;
int32 PathlineTrackExpirySamples = 64;
float PathlinePendingTimeoutSeconds = 2.0f;
UMaterialInterface* PathlineMaterial = nullptr;
```
Add read-only stats:
```cpp
int32 PathlineActiveTracks;
int64 PathlineNodeCount;
int32 PathlineVisibleTracks;
int32 PathlineDroppedSamples;
```

- [ ] **Step 2: Add spawner accessors.**

```cpp
FVector GetSpawnLocation() const { return SpawnLocation; }
float GetSpawnScale() const { return SpawnScale; }
```

- [ ] **Step 3: Create and sync the manager.**

- Add `TSharedPtr<FJUSYNCPathlineManager> PathlineManager;` created in `BeginPlay`/constructor and reset in `EndPlay`/`ClearSpawnedActors`.
- Build `FJUSYNCPathlineSettings` from the controls each tick; call `SetSettings`.
- Track structural changes and call `RebuildPathlinesFromSampleCache()` when the manager requests a resample.
- In `Tick`, call `PathlineManager->PumpGameThread(GetWorld(), PCSpawner, PathlineMaterial)` and copy stats back.

- [ ] **Step 4: Ingest committed samples.**

- Change `CacheParsedSample` to return the effective `uint64` sample ID even when the sample cache is disabled.
- In `ApplyParsedFileData`, immediately after `CacheParsedSample`, call `IngestPathlineSample(Result, EffectiveSampleId)`.
- `IngestPathlineSample` loops `Result.CompactPointClouds`, canonicalizes the element name, and enqueues only `bFromRawGeo && bComplete && IsValid()` clouds with `Result.Rank`, the LUT from `Subsystem->GetPointCloudSpawner()`, and the current settings.
- Incomplete/coarse clouds are ignored and do not age tracks.
- If the manager queue dropped samples, keep the pending-break behavior.

- [ ] **Step 5: Hook playback.**

- In `ApplyCachedSample`, after choosing `SelectedSampleId`, call `PathlineManager->SetDisplaySampleId(SelectedSampleId)`.
- In `FollowLatestSample` and live apply paths, call `SetDisplaySampleId(MAX_uint64)`.
- Do not reparse or resample on scrub; only rebuild geometry from retained nodes.

- [ ] **Step 6: Lifecycle.**

- `ClearSpawnedActors`/`EndPlay` call `PathlineManager->Reset()` and destroy pathline actors.
- Disabling pathlines clears state and actors immediately.
- Changing ID/scalar/seed/limits triggers `RebuildPathlinesFromSampleCache()`:
  - reset state;
  - collect `SampleCache` point-cloud entries with `bFromRawGeo && bComplete`;
  - sort by `SampleId`, then rank and element;
  - re-enqueue in order;
  - restore the current display cutoff.

- [ ] **Step 7: Build and run automation tests.**

Expected: both Game and Editor targets build; `JUSYNC.Pathlines` and `JUSYNC.RawGeo.Attributes` tests pass.

- [ ] **Step 8: Commit canonical and mirror.**

---

## Task 6: Mirror and verify synchronization

- [ ] **Step 1: Copy the canonical tree to the sample.**

Use explicit copy of the `Source\JUSYNC` subtree, not a broad repository mirror.

- [ ] **Step 2: Hash-verify.**

```powershell
$a='D:\JSC_Github\jusync\UnrealPlugin\JUSYNC\Source\JUSYNC'
$b='D:\JSC_Github\jusync-uesample\Plugins\JUSYNC\Source\JUSYNC'
$files=Get-ChildItem -LiteralPath $a -Recurse -File | ForEach-Object { $_.FullName.Substring($a.Length) }
$missing=@(); $diff=@()
foreach($f in $files){
  $pb=$b+$f
  if(-not (Test-Path -LiteralPath $pb)){ $missing += $f }
  elseif((Get-FileHash -LiteralPath ($a+$f)).Hash -ne (Get-FileHash -LiteralPath $pb).Hash){ $diff += $f }
}
"MissingInSample=$($missing.Count) Different=$($diff.Count)"
```

Expected: `MissingInSample=0 Different=0`.

- [ ] **Step 3: Commit the sample.**

Do not push.

---

## Task 7: Sender tests and rebuild

- [ ] **Step 1: Configure/build/run all three host tests.**

```powershell
foreach ($t in 'rawgeo_particle_id','rawgeo_point_gate','rawgeo_curve_pack') {
  cmake -S "tests/$t" -B "build-out/tests/$t"
  cmake --build "build-out/tests/$t" --config Release
  ctest --test-dir "build-out/tests/$t" -C Release --output-on-failure
}
```

Expected: 3/3 pass.

- [ ] **Step 2: Rebuild the sender image only if the deployment workflow requires it.**

The feature must remain on image version `6.0.5`. Rebuild `jusync-sender:6.0.5` and `staticxg7/pv-anari-usd:6.0.5` only after host tests pass. Preserve the running container as before.

---

## Task 8: Unreal rebuild and verification

- [ ] **Step 1: Build Editor.**

```powershell
& 'C:\Program Files\Epic Games\UE_5.7\Engine\Build\BatchFiles\Build.bat' JusyncSampleProjectEditor Win64 Development -Project='D:\JSC_Github\jusync-uesample\JusyncSampleProject.uproject' -WaitMutex -FromMsBuild
```

- [ ] **Step 2: Build Game.**

```powershell
& 'C:\Program Files\Epic Games\UE_5.7\Engine\Build\BatchFiles\Build.bat' JusyncSampleProject Win64 Development -Project='D:\JSC_Github\jusync-uesample\JusyncSampleProject.uproject' -WaitMutex -FromMsBuild
```

- [ ] **Step 3: Run UE automation tests.**

```powershell
& 'C:\Program Files\Epic Games\UE_5.7\Engine\Binaries\Win64\UnrealEditor-Cmd.exe' 'D:\JSC_Github\jusync-uesample\JusyncSampleProject.uproject' -ExecCmds='Automation RunTests JUSYNC.Pathlines+JUSYNC.RawGeo.Attributes;Quit' -unattended -nop4 -NullRHI -testexit='Automation Test Queue Empty'
```

Expected: every selected test passes, or report the exact failure if the editor cannot start tests in this environment.

- [ ] **Step 4: Final verification.**

- `git status --short` in all three repos.
- Confirm canonical/sample hash equality.
- Confirm no push occurred.
- Report commit hashes, test results, build results, and any remaining review findings.

---

## Execution Order

1. Task 1 (sender) -> commit `ANARI-USD`.
2. Task 2 (attributes) -> commit canonical + mirror.
3. Tasks 3-4 (pathline core + actor) -> commit canonical + mirror.
4. Task 5 (integration) -> commit canonical + mirror.
5. Task 6 (mirror verification).
6. Task 7 (host tests, optional image rebuild).
7. Task 8 (UE builds/tests).

Do not start Task 2 before Task 1's host test passes. Do not integrate pathlines before the attribute tests pass.
