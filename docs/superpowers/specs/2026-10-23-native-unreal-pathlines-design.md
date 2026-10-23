# Native Unreal Pathlines — Stable-ID Particle Trails

- **Status:** Approved design, implementation pending
- **Date:** 2026-10-23
- **Branches:** `feat/rawgeo-v3-curves` in `ANARI-USD`, `jusync`, `jusync-uesample`
- **Image version:** `6.0.5` (unchanged; no tag bump for this feature)
- **Related:** `docs/superpowers/specs/2026-10-23-rawgeo-v3-curves-design.md`,
  `docs/plans/realtime-streaming.md`, `docs/plans/milestone-2-lidar-squeeze.md`

---

## 1. Goal

Add native Unreal pathlines for live ANARI particle streams.

A pathline is the trail of one stable particle identity over time. It is not a
velocity-field streamline. The first release connects successive positions of a
stable particle ID, with breaks when the ID disappears and returns.

The feature must:

1. Update incrementally as committed live samples arrive.
2. Let the user cap the number of tracks, the history window, and total nodes.
3. Let the user hide tracks outside a numeric scalar range.
4. Color every visible track by a numeric scalar attribute through the existing
   JUSYNC gradient LUT.
5. Survive additions, disappearances, gaps, duplicate IDs, and reused IDs.
6. Preserve existing RawGeo behavior: v1/v2 point payloads still parse, curves
   stay on v3, and point clouds stay on v2.
7. Keep cylinders, cones, and glyphs as USD surfaces. Only sphere point clouds
   may use the RawGeo point stream.

Success means a live sphere point cloud with a `particle_id` attribute produces
smooth, stable tubes in Unreal without parsing USD per sample, and rotating or
FitToVolume-transforming the point cloud moves the pathlines with it.

---

## 2. Scope

### In scope

- Stable-ID pathlines from RawGeo point clouds.
- CPU-built tube geometry rendered through `RealtimeMeshComponent`.
- Per-point attribute preservation in the UE RawGeo parser.
- A sender-side synthetic `particle_id` attribute derived from `primitive.id`.
- Live and cached-sample playback behavior.
- Details-panel controls on `AJUSYNCFileSpawnerActor`.
- Unit and integration tests for ID encoding, parser preservation, track state,
  filtering, coloring, geometry, and playback.

### Out of scope

- Velocity-field streamlines or integration of `primitive.velocity`.
- Niagara ribbons, GPU simulation, or GPU particle systems.
- Pathlines for cylinders, cones, glyphs, or USD point clouds without RawGeo
  attributes.
- Time-varying pathline width as a separate animation channel.
- A dedicated UMG pathline panel. Details-panel properties and Blueprint-callable
  resample/clear functions are sufficient for v1.
- Changing the RawGeo wire version or the `6.0.5` image tag.

---

## 3. Terminology

- **Sample:** one committed sender frame. All files from the same sender render
  frame share one `SampleId`.
- **Logical element:** the canonicalized actor identity after folding
  `<name>_u<k>` rebuild variants, for example `bigCloud`.
- **Track key:** `(source rank, particle ID)`.
- **Node:** one committed position of one track at one sample.
- **Run:** a consecutive set of nodes with no segment break. A run of `n >= 2`
  nodes becomes a tube.
- **Display cutoff:** the sample ID up to which nodes are rendered. Live mode
  uses the latest ingested sample; playback uses the selected cached sample.
- **Representative scalar:** the scalar value of a track's newest node at or
  before the display cutoff. This drives both filtering and coloring.

---

## 4. Sender Contract

### 4.1 Wire version stays unchanged

Point clouds continue to use RawGeo **v2**. Curves continue to use **v3**. This
feature adds an ordinary per-point float attribute; it does not add a header
field, change a section layout, or bump `JUSYNC_RAWGEO_VERSION`.

The v1 and v2 parsers already tolerate unknown attributes, so older clients see
the new attribute and ignore it.

### 4.2 Synthetic `particle_id`

`primitive.id` is a UInt/Long ANARI array, not a float attribute. RawGeo v2 only
carries float attributes. The sender therefore preserves stable identity as a
synthetic float attribute named `particle_id`.

In `UsdBridgeUsdWriter::TryStoreRawGeo(UsdBridgePrimCache*,
const UsdBridgeInstancerData&, double)`:

1. If `data.Attributes` already contains a non-per-prim attribute named
   `particle_id` with 1, 2, or 4 components, leave it alone.
2. Otherwise, if `data.InstanceIds` is non-null and fewer than 16 float
   attributes are emitted, append a synthetic `particle_id` attribute.
3. Otherwise, emit no ID attribute. UE falls back to point-index identity.

The synthetic attribute is appended after the real attributes. It counts toward
the 16-attribute limit, the descriptor table, and `payload_bytes`.

### 4.3 ID encoding

IDs are packed as 16-bit chunks into float32 values. Every chunk is exactly
representable in float32.

| Source ID element size | Components | Value layout |
| --- | ---: | --- |
| 1 or 2 bytes | 1 | `id & 0xFFFF` |
| 4 bytes | 2 | `[id & 0xFFFF, (id >> 16) & 0xFFFF]` |
| 8 bytes | 4 | four 16-bit chunks, least significant first |

The source bytes are read as unsigned bit patterns. Signed IDs keep their bit
pattern; pathline identity uses the unsigned 64-bit reconstruction. The decoder
reassembles:

```text
id = chunk[0] | (chunk[1] << 16) | (chunk[2] << 32) | (chunk[3] << 48)
```

Only component counts of 1, 2, or 4 are valid. A real `particle_id` attribute
with 3 components, or one with non-finite values, is treated as malformed. UE
warns once per element and falls back to point index.

Packing honors `JUSYNC_RAWGEO_STRIDE`: the synthetic ID is generated for the
source points that survive the stride, exactly like positions and attributes.
The existing payload cap check includes the synthetic attribute; if the cap is
exceeded, the existing USD fallback still applies.

### 4.4 No sender-side state

The sender does not track particle lifetimes, sort IDs, or carry previous
positions. Pathline state is entirely client-side.

---

## 5. UE Parser Changes

### 5.1 Point attribute payload type

Add a point attribute container to `JUSYNCTypes.h`:

```cpp
USTRUCT(BlueprintType)
struct JUSYNC_API FJUSYNCPointAttribute
{
    GENERATED_BODY()

    UPROPERTY(BlueprintReadOnly, Category = "JUSYNC")
    FString Name;

    // 1..4 floats per point.
    UPROPERTY(BlueprintReadOnly, Category = "JUSYNC")
    int32 Components = 1;

    // Point-major: point i starts at Values[i * Components].
    UPROPERTY(BlueprintReadOnly, Category = "JUSYNC")
    TArray<float> Values;

    int32 GetPointCount() const
    {
        return Components > 0 ? Values.Num() / Components : 0;
    }
};
```

Extend `FJUSYNCPointCloudData`:

```cpp
UPROPERTY(BlueprintReadOnly, Category = "JUSYNC")
TArray<FJUSYNCPointAttribute> Attributes;

// True only when this payload is the whole frame. Coarse and intermediate
// progressive subsets set this false.
UPROPERTY(BlueprintReadOnly, Category = "JUSYNC")
bool bComplete = true;

// Source point count for a progressive payload. -1 when unknown.
UPROPERTY(BlueprintReadOnly, Category = "JUSYNC")
int64 FullPointCount = -1;

// True only for a payload produced by the RawGeo point parser. USD point
// clouds have no per-point attributes and must not enter pathline state.
UPROPERTY(BlueprintReadOnly, Category = "JUSYNC")
bool bFromRawGeo = false;
```

`Attributes` is populated for both the `LidarPoints` and legacy `Positions`
paths. `Widths` and the existing `attribute0` width behavior do not change; a
named `attribute0` may exist both in `Attributes` and in `Widths`.

The RawGeo whole-frame path and `JUSYNCBuildCloudFromMergedPoints` set
`bFromRawGeo = true`. The USD point-cloud loading paths leave it `false`.

### 5.2 Whole-frame parser

In `JUSYNCParseRawGeoBuffer`, after validating descriptors and computing the
attribute section base:

1. Walk the descriptor table once and record the byte offset of every attribute
   section.
2. For each descriptor, allocate a `FJUSYNCPointAttribute` with `Components` and
   `N * Components` float values.
3. Fill the values with a parallel copy/scatter. Attribute data is already in
   wire float form; no coordinate conversion is applied to attributes.
4. Set `bComplete = true` and `FullPointCount = N`.

The copy must be bounded by `Hdr.attr_uniform_count` and `N`, and must validate
`Components` in `[1, 4]` as the current parser already does.

### 5.3 Progressive subset merge

Extend `FJUSYNCRawGeoMerge` with:

```cpp
TArray<FJUSYNCPointAttribute> Attributes;
bool bHasAttributes = false;
```

On the first subset for a merge key, allocate each attribute at `FullN *
Components` and zero-fill it. On every subset, scatter attribute values from
subset point `j` to lattice slot `K + j*S`:

```text
dst[Idx * Components + c] = src[j * Components + c]
```

The merge signature must include the attribute name/component layout. If a later
subset carries a different layout, discard the merge and start fresh, matching
the existing `Stride`/`FullN` mismatch behavior.

Emission rules:

- **Complete merge:** `bComplete = true`, `FullPointCount = FullN`, attributes
  are the fully scattered arrays.
- **Coarse or partial merge:** `bComplete = false`, `FullPointCount = FullN`,
  attributes contain only filled slots or are omitted. Pathlines ignore these
  payloads.
- **Policy 0 (coarse only):** `bComplete = false`.
- **Whole-frame v1 or single-shot v2:** `bComplete = true`.

`JUSYNCBuildCloudFromMergedPoints` gains an attributes parameter and sets the
new fields. Its existing bounding-box and point-count behavior stays intact.

### 5.4 ID decoding helper

Add a pathline-side helper that, for a cloud, returns a stable ID for point `i`:

1. Find an attribute matching `PathlineIdAttribute` case-insensitively.
2. If found and `Components` is 1, 2, or 4, decode the 16-bit chunks.
3. If found but malformed, warn once and fall back to `i`.
4. If not found, use `i`.

The helper does not mutate the cloud. It exposes a small `TryGetId`/`DecodeId`
API so unit tests can exercise it without a manager.

When `JUSYNC_RAWGEO_STRIDE > 1`, point-index fallback is not stable across
samples. The sender should emit `particle_id`; the client logs a one-time warning
when it uses index fallback on a subsampled stream if it can detect that case.

### 5.5 Cloud access helpers

`FJUSYNCPointCloudData` can carry either `LidarPoints` or legacy `Positions`.
Pathline code must not assume `Positions` is populated. Add read-only helpers:

- `GetPosition(i)`: `LidarPoints[i].Location` when `HasLidarPoints()`, otherwise
  `Positions[i]`, otherwise zero.
- `GetWidth(i)`: `Widths[i]` when valid, otherwise zero.
- `GetAttributeScalar(i, Attribute, Component)`: find the attribute by
  case-insensitive name, validate `i` and `Component`, and return
  `Values[i * Components + Component]`.

The helpers are used by both the ID decoder and the scalar extractor, and are
unit-testable without a live manager.

---

## 6. Pathline Data Model

### 6.1 Ownership

`AJUSYNCFileSpawnerActor` owns one `TSharedPtr<FJUSYNCPathlineManager>`. The
manager is created on the game thread when pathlines are first enabled or when a
sample is first ingested, and is cleared by `ClearSpawnedActors`, `StartSpawning`,
and `EndPlay`.

The manager owns pathline actors and track state. It does not own point-cloud
actors. It reads the point-cloud spawner's current spawn location and uniform
scale so pathlines follow the same transform, including FitToVolume.

### 6.2 Logical elements

There is one pathline state and one pathline actor per canonical logical element.
The file spawner canonicalizes names through
`JUSYNCCanonicalPointCloudElement` / `JUSYNCElementNames::CanonicalActorIdentity`
before ingesting. Different ranks of the same element feed the same state.

Track keys include rank, so two ranks that reuse the same particle ID remain
distinct. Point-cloud actors for all ranks share one spawn transform, so a single
pathline actor per element is correct.

### 6.3 Track key and node

```cpp
struct FJUSYNCPathlineTrackKey
{
    int32 Rank = 0;
    uint64 ParticleId = 0;

    bool operator==(const FJUSYNCPathlineTrackKey& Other) const
    {
        return Rank == Other.Rank && ParticleId == Other.ParticleId;
    }
};

struct FJUSYNCPathlineNode
{
    FVector3f Position = FVector3f::ZeroVector;
    float Width = 0.0f;
    float Scalar = 0.0f;
    bool bScalarValid = false;
    uint64 SampleId = 0;
    int64 Ordinal = 0;
    bool bBreakBefore = false;
};

struct FJUSYNCPathlineTrack
{
    FJUSYNCPathlineTrackKey Key;
    uint64 SelectionScore = 0;
    TArray<FJUSYNCPathlineNode> Nodes;
    int64 LastSeenOrdinal = -1;
    int32 AbsentSamples = 0;
    bool bPendingBreak = false;
    bool bVisible = true;
    int32 Slot = INDEX_NONE;
};
```

The manager keeps a fixed-capacity track array plus:

```cpp
TArray<FJUSYNCPathlineTrack> Tracks;
TArray<int32> FreeSlots;
TMap<FJUSYNCPathlineTrackKey, int32> KeyToTrack;
int32 ActiveTrackCount = 0;
int64 NextOrdinal = 0;
uint64 LastIngestedSampleId = 0;
```

Provide `GetTypeHash(const FJUSYNCPathlineTrackKey&)` combining rank and particle
ID so the map can hash keys. A reused slot resets all track fields. The key map is
the only place where a selected key is looked up.

### 6.4 Global ordinal vs. sample ID

Each ingested sample increments a manager-local monotonic `Ordinal`. Nodes store
both `Ordinal` and the sender/cache `SampleId`.

- `Ordinal` drives the history window (`HistorySamples`).
- `SampleId` drives playback cutoff and same-sample replacement.

This separation keeps the window correct even when sender timestamps jump or
manual refreshes insert synthetic IDs.

---

## 7. Selection

### 7.1 Deterministic hash-ranked subset

When a state first sees a complete frame, it selects at most
`PathlineMaxTracks` keys. The selector is deterministic for a given seed and
does not depend on point order.

```text
score(key) = HashCombine(PathlineSelectionSeed, HashCombine(rank, particleId))
```

Keep the highest scores. Use `Algo::NthElement` on the candidate array or a
bounded heap so the first frame does not require a full sort of millions of
keys.

The default seed is `1337`. Changing `PathlineSelectionSeed` clears the state and
resamples from the next complete frame. This is the explicit "show me a different
subset" control.

Selection is not attribute-ranked. Filtering is a display concern and must not
silently change which particles are tracked.

### 7.2 Slot filling

After selection, a slot stays bound to its key until the track expires or the
user changes the capacity/seed.

When a track expires:

1. Remove it from `KeyToTrack`.
2. Append its slot to `FreeSlots`.
3. Destroy or shrink its geometry on the next rebuild.

When `FreeSlots` is non-empty at the end of a sample, collect unselected keys
from that sample, score them, and fill the highest-scoring candidates up to the
vacant count. Newly selected tracks start with one node at that sample; they draw
after their second node.

### 7.3 Expiry

A selected track that is absent from its element's complete cloud gets
`bPendingBreak = true` and `++AbsentSamples`. It is not removed immediately, so a
short gap keeps the surrounding history.

After `PathlineTrackExpirySamples` consecutive element samples with the ID
missing, remove the track and free its slot. A later ID with the same value is a
new track and never connects to the old trail.

Default `PathlineTrackExpirySamples = 64`.

### 7.4 Capacity changes

- **Increase `PathlineMaxTracks`:** free slots become available; fill them from
  the next complete sample. No historical nodes are invented for new keys.
- **Decrease `PathlineMaxTracks`:** release tracks with the highest
  `SelectionScore` first, because lower scores are the higher-priority subset.
  Trim and rebuild immediately.
- **Change `PathlineSelectionSeed`:** clear state and resample.

---

## 8. Ingestion Semantics

### 8.1 Complete frames only

Pathline ingests only payloads with `bFromRawGeo == true` and
`bComplete == true`. USD point clouds have no reliable per-point attributes, and
coarse/intermediate progressive subsets would create a different particle set
and break track continuity.

The file spawner tracks sample IDs that have complete RawGeo point-cloud cache
entries:

- `CacheParsedSample` adds `EffectiveSampleId` to a pending pathline sample list
  when it caches a complete point cloud.
- `Tick` ingests pending sample IDs in ascending order when
  `bInitialSpawnDone`, `!bSceneDiffInFlight`, and
  `!ChangeTracker->HasActiveWork()`.
- If any cached point cloud for a sample is incomplete, defer that sample for a
  short grace period (`PathlinePendingTimeoutSeconds = 2.0`). After the grace
  period, ingest the complete entries and skip the incomplete payloads without
  aging their tracks.
- If the sample cache evicts a pending ID before ingestion, drop it and log once.
  The next complete sample appends normally, leaving a gap.

The manager ignores sample IDs less than or equal to `LastIngestedSampleId` and
processes in ascending order.

### 8.2 Per-sample update

For each complete payload `(ElementName, Rank, SampleId)`:

1. Decode IDs for all points. For duplicate keys inside the sample, keep the
   first occurrence and warn once per sample.
2. For an existing selected track:
   - If the last node has the same `SampleId`, replace that node's position,
     width, and scalar. This handles re-delivery.
   - Otherwise append a new node. If `bPendingBreak`, set
     `node.bBreakBefore = true`, then clear the flag.
   - Reset `AbsentSamples` to zero.
3. For a selected key that is absent from this element's cloud, set
   `bPendingBreak = true` and increment `AbsentSamples`.
4. If `FreeSlots` is non-empty, fill them from unselected keys in this sample as
   described in §7.2.
5. Expire tracks that exceed `PathlineTrackExpirySamples`.
6. Trim history and enforce the node budget.

Multiple point clouds for the same element and rank in one sample are processed
in a deterministic order (rank ascending, then cache serial order). The
same-key replacement rule applies if a key appears in more than one payload.

**Element omission is not particle disappearance.** The sender can omit an
unchanged point cloud, or emit a sample where an element has no payload. That
must not increment `AbsentSamples` or break trails. A track only ages while its
own element has an ingested complete cloud and the track's ID is missing from
that cloud. Element removal is driven by the file spawner's element lifecycle,
not by per-sample absence.

### 8.3 Scalar extraction

The scalar attribute is `PathlineScalarAttribute`. Defaults:

- If the property is empty, prefer `attribute0` when present.
- Otherwise use the first non-ID attribute with at least one component.
- Otherwise use the point width.
- Otherwise the scalar is invalid.

For a multi-component attribute, read component `PathlineScalarComponent`
(clamped to the component count). The scalar is stored per node, so filtering and
color at a playback cutoff use the value from that cutoff, not the live value.

If the attribute is missing or non-finite, `bScalarValid = false`. Such a track
is colored white and is hidden when filtering is enabled.

Changing `PathlineScalarAttribute` or `PathlineScalarComponent` clears node
arrays but keeps the selected key set, then rebuilds asynchronously from the
retained sample cache in ascending sample order, up to
`PathlineHistorySamples`. If the cache does not cover the full window, history
starts at the oldest cached sample and the log records the truncation. This
avoids storing every attribute in every node.

### 8.4 History and node budget

Per-track trimming:

```text
EffectiveHistory = clamp(
    PathlineMaxNodes / max(1, ActiveTrackCount),
    2,
    PathlineHistorySamples)
```

Remove nodes from the front of each track while
`Nodes.Num() > EffectiveHistory`. The value is recomputed when tracks are
selected, expire, or the capacity changes.

This makes `PathlineMaxNodes` a hard node cap while allowing longer histories
when only a few tracks are active. If `PathlineMaxNodes < 2`, clamp it to 2. If
`PathlineMaxNodes / 2 < PathlineMaxTracks`, cap the effective active track count
to `max(1, PathlineMaxNodes / 2)` and log once.

A track with a break that survives trimming starts a new run at its retained
first node; the break flag on that node is harmless.

### 8.5 Missing IDs and identity fallback

- Missing `particle_id` attribute: use point index as the particle ID. This is
  stable only if the sender preserves point order.
- Malformed `particle_id`: warn once per element and use point index.
- Duplicate ID in one sample: first occurrence wins.
- Same sample delivered twice: replace the last node.
- Returning ID after a gap: append a break, never connect across the gap.
- Reused ID after expiry: new track, new trail.

---

## 9. Filtering and Coloring

### 9.1 Representative scalar

For a track at display cutoff `S`:

```text
rep(track, S) = scalar of the newest node with node.SampleId <= S
```

If no such node exists, the track is not drawn at that cutoff.

### 9.2 Filter

`bPathlineFilterEnabled` defaults to `false`. When enabled, a track is visible
only when:

```text
rep >= PathlineFilterMin && rep <= PathlineFilterMax
```

NaN or invalid scalars are hidden when filtering is enabled. The filter hides the
whole track, including all of its disjoint runs. It never deletes nodes or frees
slots; clearing the filter restores the trail immediately.

Default range: `PathlineFilterMin = 0.0`, `PathlineFilterMax = 1.0`.

### 9.3 Color

A visible track's color is constant across all of its vertices for a given
rebuild:

```text
t = clamp((rep - PathlineColorMin) / (PathlineColorMax - PathlineColorMin), 0, 1)
index = round(t * (LUT.Num() - 1))
color = LUT[index]
```

If `PathlineColorMax <= PathlineColorMin`, use the middle LUT entry. If the LUT
is empty or `bPathlineColorByScalar` is false, use `PathlineFlatColor` (default
white). Invalid scalars use `PathlineFlatColor`.

Defaults: `PathlineColorMin = 0.0`, `PathlineColorMax = 1.0`,
`bPathlineColorByScalar = true`.

The LUT is obtained from `FJUSYNCPointCloudSpawner::GetGradientLUT()`. A LUT
version change triggers a geometry rebuild that recomputes vertex colors without
re-ingesting samples.

Changing filter range, color range, flat color, or LUT only rebuilds geometry.
It does not clear track state.

---

## 10. Rendering

### 10.1 Pathline actor

Add `AJUSYNCPathlineActor`:

- `URealtimeMeshComponent` as the root.
- `URealtimeMeshSimple` initialized lazily.
- One section group, key `FRealtimeMeshSectionGroupKey::Create(0, TEXT("PathlineGroup"))`.
- Material slot 0 named `PathlineMaterial`.
- Collision disabled by default.
- Shadow casting controlled by `bPathlineCastShadow` (default `false`).
- Hidden when there is no valid geometry.

The manager keeps `TMap<FString, AJUSYNCPathlineActor*>` keyed by canonical
element. Actors are spawned lazily on the game thread and destroyed on clear or
when the element has no active tracks.

### 10.2 Transform

Every point-cloud rank shares the spawner's `SpawnLocation` and uniform
`SpawnScale`. The pathline actor copies those values:

```text
ActorLocation = PointCloudSpawner->GetSpawnLocation()
ActorRotation = FRotator::ZeroRotator
ActorScale    = FVector(PointCloudSpawner->GetSpawnScale())
```

Add read-only accessors to `FJUSYNCPointCloudSpawner` for the two values, or an
equivalent transform accessor. Sync the pathline actor on each manager tick and
after a rebuild, so `FitToVolume` and manual resizing stay aligned.

The parsed point positions are already in UE space through the existing
`(x, z, -y)` conversion. Pathline geometry uses the same local coordinates as
the point cloud. No extra coordinate conversion is applied.

### 10.3 Tube builder

For each visible track:

1. Take nodes with `SampleId <= DisplaySampleId`.
2. Split into runs at `bBreakBefore`.
3. Skip runs with fewer than two nodes.
4. For each node, compute a tangent from its neighbors in the run:
   - first node: next minus current;
   - last node: current minus previous;
   - interior: next minus previous;
   - zero/degenerate tangent: fallback `(1,0,0)`.
5. Build an orthonormal ring frame using the same reference-axis logic as
   `JUSYNCRawGeoCurves::BuildTubesInternal`.
6. Emit `PathlineTubeSides` vertices per node and two triangles per side per
   connected node pair.

Radius:

- If `bPathlineUsePointWidth` is true and the node width is valid:
  `radius = max(0, node.Width) * PathlineRadiusScale`.
- Otherwise: `radius = PathlineRadius`.
- Clamp negative/NaN radius to zero.

Defaults: `PathlineTubeSides = 4` (clamp 3..8), `PathlineRadius = 0.5`,
`bPathlineUsePointWidth = false`, `PathlineRadiusScale = 1.0`.

Normals are radial. Vertex colors come from §9.3. UVs are optional; if emitted,
use `U = side / Sides` and `V = accumulated run length / max(PathlineRadius, 0.01)`.
The default vertex-color material does not require meaningful UVs.

The tube builder lives on a worker thread and produces a
`FJUSYNCCompactMeshData`. It never touches `URealtimeMeshComponent` directly.

### 10.4 Upload

On the game thread:

1. Call `JUSYNCBuildRealtimeMeshStreams(MeshData, Streams)`.
2. Resolve `URealtimeMeshSimple` from the component.
3. `CreateSectionGroup` on first use, otherwise `UpdateSectionGroup`.
4. Set the section visible and the material slot.
5. Apply the actor transform.

If the geometry has zero vertices/triangles, hide the section and component
instead of uploading an empty group. Do not call `UpdateSectionGroup` with an
invalid `FJUSYNCCompactMeshData`.

Material resolution order:

1. `PathlineMaterial`, if set.
2. `/Game/Materials/M_VertexColor`.
3. Engine default material with a dynamic instance setting
   `UseVertexColor = 1.0`.

This mirrors `UJUSYNCSubsystem::UpdateRealtimeMeshFromJUSYNC`.

---

## 11. Threading and Performance

### 11.1 Serialized worker

Pathline ingestion and tube construction run on a background worker so the game
thread does not scan million-point clouds or build millions of vertices.

The manager owns a serialized work queue:

- `EnqueueSample(elementName, ref, rank, sampleId, settingsSnapshot)` pushes a
  work item.
- At most one worker drains the queue at a time.
- Work items are processed in sample order.
- Finished meshes are posted to the game thread with
  `FFunctionGraphTask::CreateAndDispatchWhenReady`.
- A generation counter invalidates results after `Clear()` or a configuration
  reset.
- If the queue falls behind, drop intermediate unprocessed samples but keep the
  newest. Dropped samples set `bPendingBreak` on every track before the next
  appended node, so a long gap never becomes a connecting segment. Log at most
  once per second.

The game thread only applies finished meshes, updates transforms, and handles
UI-triggered rebuilds. It never scans point clouds.

### 11.2 Ingestion cost

- One scan per complete sample, `O(N)` in point count.
- Track lookup is a hash map keyed by `(rank, id)`.
- Only selected tracks allocate nodes.
- Attributes are not copied wholesale into nodes; only the configured scalar and
  width are stored per node.
- First-frame selection is `O(N)` plus top-K selection; use `Algo::NthElement`
  or a bounded heap, not a full sort.

### 11.3 Geometry budget

Default maximum:

- 4096 tracks
- 128 history samples
- 524,288 nodes
- 4 sides

At full budget that is about 2.1M vertices and 4.2M triangles. `int32` indices
are sufficient. Users who need more should raise the node budget or lower the
side count; the manager logs a one-time warning when a request is clamped.

`PathlineUpdateIntervalSeconds` defaults to `0.0` (ingest every committed
sample). A positive value throttles ingestion and coalesces queued samples.

---

## 12. Playback and Lifecycle

### 12.1 Display cutoff

The manager keeps `DisplaySampleId`.

- Live mode: `DisplaySampleId = LastIngestedSampleId`.
- Playback mode: `DisplaySampleId = SelectedCachedSampleId`.
- `ApplyCachedSample` is the only place that sets the playback cutoff.

Changing the cutoff does not reparse point clouds and does not mutate track
state. It only filters nodes and rebuilds geometry. Backward and forward
scrubbing within the retained window is therefore cheap and deterministic.

If the selected sample is older than the retained node window, the oldest
retained nodes are shown. This is a documented limitation, not an error.

### 12.2 Live data during playback

While the user scrubs, live point clouds are cached but may be held in
`PendingLiveSamples` and not applied. Pathlines continue to ingest samples that
are genuinely present in the sample cache and complete. When playback returns to
live, `FlushPendingLiveSamples` restores the newest pending payload, and the next
drain ingests it. The display cutoff returns to the latest ingested sample.

### 12.3 Reset and disable

- `ClearSpawnedActors`, `StartSpawning`, and `EndPlay` call
  `PathlineManager->Clear()`: cancel pending work generation, destroy actors,
  clear tracks, and reset sample bookkeeping.
- Setting `bEnablePathlines = false` hides and clears pathline actors and state so
  no CPU or GPU cost remains. Re-enabling starts a fresh selection.
- `ClearSampleCache` does not clear pathline nodes; the pathline window is
  independent. `ClearSpawnedActors` does clear both.
- When an element has no active tracks after expiry, destroy its pathline actor.
- The file spawner's element lifecycle is authoritative. When
  `DestroyUnreplacedPCActors` retires a point-cloud element, it calls
  `PathlineManager->RemoveElement(CanonicalElement)` so the trail and actor are
  destroyed. Playback hiding calls `SetElementVisible(CanonicalElement, bool)`
  so a scrub does not permanently delete live history.
- An element that is still in the file list but omitted from a particular
  sample stays alive and keeps its last trail; omission is not expiry.

### 12.4 Changing configuration

| Change | Effect |
| --- | --- |
| `bEnablePathlines` | Toggle; disabling clears state. |
| `PathlineMaxTracks` up | Fill free slots from the next complete sample. |
| `PathlineMaxTracks` down | Release highest-score tracks, trim, rebuild. |
| `PathlineHistorySamples` down | Trim nodes immediately, rebuild. |
| `PathlineHistorySamples` up | Keep accumulating; old nodes are not recovered. |
| `PathlineMaxNodes` down | Recompute effective history, trim, rebuild. |
| `PathlineSelectionSeed` | Clear state, resample. |
| `PathlineScalarAttribute` / component | Clear nodes, keep selected keys, rebuild from cached complete samples. |
| Filter/color ranges, flat color, LUT | Rebuild geometry only. |
| Tube sides/radius/width mode | Rebuild geometry only. |
| `PathlineMaterial` | Reassign material and rebuild. |

Configuration reads are snapshotted on the game thread before enqueueing worker
work. Workers never read live `UPROPERTY` values.

---

## 13. Controls and Defaults

Add these to `AJUSYNCFileSpawnerActor` under a `Pathlines` category:

| Property | Default | Range / notes |
| --- | --- | --- |
| `bEnablePathlines` | `false` | Master toggle |
| `PathlineMaxTracks` | `4096` | `1..65536` |
| `PathlineHistorySamples` | `128` | `2..4096` |
| `PathlineMaxNodes` | `524288` | `1024..8000000` |
| `PathlineTubeSides` | `4` | `3..8` |
| `PathlineRadius` | `0.5` | fallback radius |
| `bPathlineUsePointWidth` | `false` | use point width |
| `PathlineRadiusScale` | `1.0` | width multiplier |
| `PathlineIdAttribute` | `particle_id` | case-insensitive |
| `PathlineScalarAttribute` | `attribute0` | empty = auto |
| `PathlineScalarComponent` | `0` | clamped to component count |
| `bPathlineFilterEnabled` | `false` | hide outside range |
| `PathlineFilterMin` | `0.0` | inclusive |
| `PathlineFilterMax` | `1.0` | inclusive |
| `bPathlineColorByScalar` | `true` | use LUT |
| `PathlineColorMin` | `0.0` | color normalization |
| `PathlineColorMax` | `1.0` | color normalization |
| `PathlineFlatColor` | `FLinearColor::White` | fallback color |
| `PathlineSelectionSeed` | `1337` | resample by changing |
| `PathlineTrackExpirySamples` | `64` | element samples with ID missing before free |
| `PathlineUpdateIntervalSeconds` | `0.0` | 0 = every sample |
| `PathlinePendingTimeoutSeconds` | `2.0` | incomplete-sample grace |
| `PathlineMaterial` | `nullptr` | optional override |
| `bPathlineCastShadow` | `false` | shadow cost |

Blueprint-callable functions:

- `ResamplePathlines()` — clear state and select a new subset from the next
  complete sample.
- `ClearPathlines()` — destroy actors and clear state.
- `GetPathlineStats(FJUSYNCPathlineStats&)` — active tracks, nodes, visible
  tracks, dropped samples, last ingest/build milliseconds. Useful for the
  Details panel and tests.

The default disabled state means no worker, no actors, and no per-sample scan
until the user turns pathlines on.

---

## 14. Tests

### 14.1 Sender tests (`ANARI-USD`)

Add `tests/rawgeo_particle_id/test_rawgeo_particle_id.cpp`:

1. 32-bit `InstanceIds` pack into two exact float chunks, little-endian.
2. 64-bit `InstanceIds` pack into four exact float chunks.
3. Signed bit patterns round-trip as unsigned IDs.
4. An existing `particle_id` float attribute is not duplicated.
5. A full 16-attribute payload does not append a 17th descriptor; the payload
   remains valid.
6. `JUSYNC_RAWGEO_STRIDE` subsamples IDs in lockstep with positions.
7. Sphere point clouds still emit v2 `.geo`; cylinders, cones, and glyphs do not
   take the RawGeo point path.
8. Header size and v1/v2 prefix assertions remain unchanged.

Run:

```text
ctest --test-dir build -R rawgeo_particle_id
ctest --test-dir build -R rawgeo_point_gate
ctest --test-dir build -R rawgeo_curve_pack
```

### 14.2 UE parser tests

Add `Private/Tests/JUSYNCPathlineTests.cpp` using the existing UE automation
style in `JUSYNCRawGeoCurveTests.cpp`.

1. Whole-frame v1/v2 attributes preserve name, components, and point-major
   values.
2. `attribute0` still fills `Widths` exactly as before.
3. v2 progressive merge scatters attributes into the correct lattice slots.
4. A complete merge's attributes match a stride-1 reference payload.
5. Coarse/partial payloads set `bComplete = false`; complete payloads set true
   and `FullPointCount` correctly.
6. RawGeo payloads set `bFromRawGeo = true`; USD point clouds leave it false.
7. Malformed component counts are rejected without crashing.
8. ID decoding round-trips 1-, 2-, and 4-component encodings.
9. Missing/malformed IDs fall back to point index with one warning.

### 14.3 Pathline state tests

Use a small manager test harness that calls the synchronous ingest path directly.

1. Selection is deterministic for a seed and independent of point order.
2. `PathlineMaxTracks` is respected.
3. Two nodes across two samples create one drawable run.
4. A missing ID in an emitted element sample creates a break; a returning ID
   does not connect.
5. Duplicate ID in one sample keeps the first occurrence.
6. Re-delivering the same sample replaces the last node.
7. Expiry frees a slot; a new ID fills the vacant slot by score.
8. Reused ID after expiry starts a new track.
9. `PathlineHistorySamples` trims the oldest nodes.
10. `PathlineMaxNodes` clamps effective history and total nodes.
11. Increasing/decreasing `PathlineMaxTracks` behaves as specified.
12. Changing the selection seed clears state and resamples.
13. Omitting an element from a global sample does not break or age its tracks.
14. A dropped worker-backlog sample sets a pending break, not a long segment.
15. `RemoveElement` destroys state; `SetElementVisible` only hides it.

### 14.4 Filter/color tests

1. Representative scalar uses the last node at or before the cutoff.
2. Filter excludes tracks below/above the inclusive range.
3. Filter never deletes nodes or frees slots.
4. Color maps the representative scalar through the LUT at the expected index.
5. `ColorMax <= ColorMin` uses the middle LUT entry.
6. Missing/invalid scalar uses the flat color and hides only when filtering is
   enabled.
7. LUT version changes rebuild colors without changing track nodes.

### 14.5 Geometry tests

1. A 2-node run with 4 sides emits 8 vertices and 8 triangles.
2. A 3-node run emits 12 vertices and 16 triangles.
3. A break between runs does not create connecting triangles.
4. One-node runs emit no geometry.
5. Radius uses `PathlineRadius` or width × scale as configured.
6. Vertex colors are constant per track.
7. Normals are unit length and radial.
8. The actor transform matches the point-cloud spawner location and uniform
   scale.
9. Empty geometry hides the component instead of uploading an empty section.

### 14.6 Playback tests

1. A cutoff between samples shows the prefix only.
2. Backward then forward scrubbing does not re-ingest point clouds.
3. A cutoff older than the retained window clips to the oldest retained nodes.
4. Returning to live uses the latest ingested sample.
5. Live samples cached during playback are ingested after the pending flush.

### 14.7 Integration and regression

1. Live sender with `particle_id` produces visible pathlines and stable track
   counts across frames.
2. Spheres render as points and pathlines; cylinders/cones/glyphs still render
   as USD surfaces.
3. `FitToVolume` moves/scales pathlines with the point cloud.
4. Disabling pathlines removes actors and worker activity.
5. Both `JusyncSampleProject` and `JusyncSampleProjectEditor` build with the
   canonical and sample plugin sources synchronized.
6. Existing `rawgeo_point_gate` and `rawgeo_curve_pack` tests still pass.

---

## 15. File Map

### Canonical plugin (`D:\JSC_Github\jusync\UnrealPlugin\JUSYNC`)

- `Source/JUSYNC/Public/JUSYNCTypes.h`
  - `FJUSYNCPointAttribute`
  - `FJUSYNCPointCloudData::Attributes`, `bComplete`, `FullPointCount`,
    `bFromRawGeo`
  - pathline stats type if it is shared
- `Source/JUSYNC/Private/Core/JUSYNCSubsystem.cpp`
  - whole-frame attribute copy
  - `FJUSYNCRawGeoMerge` attribute scatter
  - completeness flags
  - ID decode helper if kept private
- `Source/JUSYNC/Public/JUSYNCFileSpawnerActor.h`
  - pathline controls and stats
  - manager pointer and pending sample bookkeeping
- `Source/JUSYNC/Private/Spawner/JUSYNCFileSpawnerActor.cpp`
  - cache hook, pending drain, tick pump, playback cutoff, lifecycle
- `Source/JUSYNC/Public/JUSYNCPathlineManager.h`
- `Source/JUSYNC/Private/Pathline/JUSYNCPathlineManager.cpp`
  - selection, state, worker queue, filtering, color, geometry rebuild
- `Source/JUSYNC/Public/JUSYNCPathlineActor.h`
- `Source/JUSYNC/Private/Pathline/JUSYNCPathlineActor.cpp`
  - RealtimeMesh actor, material, transform, upload
- `Source/JUSYNC/Private/Pathline/JUSYNCPathlineGeometry.cpp`
  - optional split for the tube builder if it improves testability
- `Source/JUSYNC/Private/Tests/JUSYNCPathlineTests.cpp`

### Sample mirror (`D:\JSC_Github\jusync-uesample\Plugins\JUSYNC`)

Every file above is mirrored byte-for-byte or kept functionally identical,
following the repository's existing synchronization pattern. Build verification
must compile both the Game and Editor targets from the sample project.

### Sender (`D:\JSC_Github\ANARI-USD`)

- `UsdBridge/UsdBridgeUsdWriter.cpp`
  - synthetic `particle_id` packing in the RawGeo point writer
- `tests/rawgeo_particle_id/test_rawgeo_particle_id.cpp`
- `tests/CMakeLists.txt` or the existing test registration file
- no protocol header change, no image tag change

### Documentation

- `docs/superpowers/specs/2026-10-23-native-unreal-pathlines-design.md`
- `docs/superpowers/plans/2026-10-23-native-unreal-pathlines.md`
  (created after this spec is approved)

---

## 16. Decisions

1. **Stable-ID pathlines only in v1.** No velocity integration and no Niagara.
2. **CPU tubes through RealtimeMeshComponent.** The RawGeo v3 curve renderer is
   for static/rebuilt curve meshes; pathlines need incremental updates and a
   separate state model.
3. **One logical element per manager; track key `(rank, id)`.** Ranks share one
   transform, so one pathline actor per element is correct.
4. **One actor per element, not per rank.** The point-cloud spawner places all
   ranks at the same location and uniform scale.
5. **Synthetic `particle_id` as a float attribute.** This preserves wire v2 and
   avoids a protocol bump.
6. **16-bit chunk encoding.** Exact in float32 and works for 32- and 64-bit IDs.
7. **Complete frames only.** Progressive coarse/partial payloads never update
   pathline state.
8. **Deterministic hash-ranked selection with seed `1337`.** Reproducible and
   independent of point order; changing the seed resamples.
9. **Vacant slots fill from new IDs by score.** No historical nodes are invented
   for newly selected tracks.
10. **Representative scalar at the display cutoff.** This makes filtering and
    coloring correct during playback.
11. **One scalar attribute drives both filter and color.** Separate ranges keep
    the UI simple; changing the attribute rebuilds from the sample cache.
12. **Per-node scalar/width only.** Full per-node attribute storage is rejected
    for memory reasons; attribute changes rebuild from cache.
13. **History is sample-count based with a global node cap.** Effective history
    is `MaxNodes / ActiveTracks`, clamped to `HistorySamples`.
14. **Background worker with a serialized queue.** The game thread never scans
    point clouds or builds tubes.
15. **Playback uses a display cutoff, not reparse.** Scrubbing is cheap and does
    not disturb track state.
16. **Disabling pathlines clears state.** Avoids stale trails and background
    cost.
17. **Image version remains `6.0.5`.** No tag or protocol major-version change.
18. **Canonical and sample JUSYNC sources stay synchronized.** No merge with
    open Critical/Important review findings, and no push unless requested.

---

## 17. Risks and Limitations

- **RawGeo-only.** USD point clouds and non-sphere instancers do not have the
  attributes needed for stable IDs. They will not show pathlines.
- **Index fallback is fragile.** If the sender reorders points or subsamples with
  `JUSYNC_RAWGEO_STRIDE > 1` without `particle_id`, tracks can jump. The client
  warns once.
- **No velocity streamlines.** These are trails of positions, not lines tangent
  to a vector field.
- **Attribute changes can truncate history.** Rebuilding from the sample cache
  only covers the retained cache window, which may be shorter than
  `PathlineHistorySamples`.
- **Playback beyond the retained node window clips.** The pathline window is
  independent of the sample cache; old nodes are not recovered.
- **Selection changes lose history for new tracks.** Newly filled slots start at
  the sample where they were selected.
- **Full-budget GPU cost.** 524,288 nodes at 4 sides is about 4.2M triangles.
  Users should lower the node budget or sides for dense streams.
- **Duplicate IDs are ambiguous.** First occurrence wins; a sender that emits
  duplicate IDs in one frame will produce one trail.
- **Attribute memory in the sample cache.** Preserving attributes increases the
  cached point-cloud footprint. The existing cache byte accounting should include
  attribute values so `SampleCacheMaxMB` still trims correctly.
- **Transform synchronization is a copy.** Pathlines follow the point-cloud
  spawner transform on the next manager tick, not in the same frame as a
  FitToVolume change.
- **Thread shutdown.** The worker must observe the generation/shutdown flag and
  discard results after `EndPlay`; otherwise it could post to a destroyed actor.

---

## 18. Acceptance Criteria

The feature is complete when:

1. A live RawGeo sphere stream with a 32-bit or 64-bit `particle_id` shows stable
   tubes with no USD parsing per sample.
2. Cylinders, cones, and glyphs still render as USD surfaces and do not emit
   RawGeo point streams.
3. v1/v2 RawGeo payloads parse exactly as before, and curves remain on v3.
4. Pathlines update incrementally and survive gaps, disappearances, duplicate
   IDs, and ID reuse.
5. The user can cap tracks, history, and nodes without exceeding the configured
   node budget.
6. Filtering hides non-matching tracks; every visible track is colored by the
   configured scalar attribute.
7. Scrubbing changes the displayed trail prefix without reparsing point clouds.
8. `FitToVolume` and point-cloud transforms move pathlines consistently.
9. Disabling pathlines removes all pathline actors and worker activity.
10. Unit tests, UE automation tests, and both UE target builds pass.
11. Canonical and sample JUSYNC sources are synchronized.
12. Image version remains `6.0.5`, and no commits are pushed or merged while
    Critical/Important review findings remain open.
