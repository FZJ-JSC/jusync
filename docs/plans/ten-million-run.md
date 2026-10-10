# 10M-point realtime push (session plan)

- [x] 1. Scout sender container: example sources, cmake cache, build tools present?
      → runtime image has NO toolchain; added Dockerfile.build (jusync-sender:build = dev image
        + build-essential/cmake/ninja) + persistent jusync-dev container for incremental builds.
- [x] 2. Big-cloud sender example: examples/anariBigCloudUsd.c (env knobs ANARI_BIG_POINTS/FPS/
      SECONDS/COLORS); scene built ONCE (retained handles) so prim name stays stable (no _u1
      churn); fixed usd::time=0 so each clip holds ONE replaced timeSample; no renderer attached
      (save+notify still runs in UsdDevice::renderFrame).
- [x] 3. Rig at 10M verified: clips/bigCloud_Geom__0.000000.usd(.usda twins) = 120,001,149 B/frame.
- [x] 4. Headless UE run diag @ 10M, 2Hz:
      parse 561.9 ms | fill 208.3 ms | spawn 7105.5 ms | total ~7876 ms notify→visible.
      Sender commit→flush ≈ 1014 ms (pxr crate export per frame ×2: SetGeometryData Track +
      RecalculateAllMemoryUsage re-export).
- [x] 5. Diagnosis: USD is the bottleneck on BOTH ends (crate serialize ~1s sender; tinyusdz
      0.56s client) + ULidarPointCloud octree rebuild 7s on game thread. Latest-wins (P2)
      keeps pipeline unclogged at 2Hz; steady state ≈ 0.13 Hz.
- [x] 6. Realtime fix (RawGeo channel — SHIPPED, measured below):
      a. Sender (ANARI-USD, feat/rawgeo-streaming): JUSYNC_RAWGEO=1 → UsdBridge mirrors raw
         float3 (+colors+attrs) into store key clips/<name>__<t>.geo (JSG1 header, protocol
         shared in AnariUsdProtocol.h) and SKIPS UpdateUsdGeometry/crate export for that geom.
         Files: UsdBridge.cpp (SetGeometryDataTemplate branch), UsdBridgeUsdWriter.*
         (TryStoreRawGeo, .geo twin purge), examples/CMakeLists.txt.
      b. Client (jusync): gate accepts clips/*.geo; UJUSYNCSubsystem sniffs JSG1 magic before
         any USD path → JUSYNCParseRawGeoBuffer fills FJUSYNCPointCloudData.LidarPoints directly
         (same axis-swap/color/widths semantics as middleware direct fill; empty USD twins <
         MinimumFileSizeBytes=1000 are auto-ignored).
      c. Sender animation bug found+fixed: SDK drops no-op re-commits → example re-sets the
         position ARRAY per frame (hash now changes every frame; verified in client notifications).
- [x] 7. Re-measured RawGeo run (10M):
      sender commit→flush 226 ms | transfer ~440 ms (~272 MB/s loopback, 120 MB) |
      raw parse+fill 154 ms (was 770 USD parse+fill) | spawn ~3.5 s octree | cycle ~5 s @1Hz.
      2 Hz starves: latest-wins aborts the in-flight 120 MB download at every next notify
      (~95 aborts vs 1 completion in 75 s) → keep 1 fps until chunk-size knob + point renderer.
- [x] 8. Octree replacement (raw VF point sprites [SUPERSEDED in milestone 2: kept Epic's ULidarPointCloud with off-thread async builds + sender-side stride LOD; custom VF path stayed parked], off-thread double buffer) — THE remaining
      blocker to sub-second: spawn is 70% of the cycle. Then chunk-size 1→8/16 MB for the wire.
- [x] 9. Commit both repos; update realtime-streaming.md + graphify.

## Notes
- 10M pts x float3 = 120 MB raw per timestep (plus colors/widths).
- Network to JURECA NOT part of this; loopback first (bandwidth ~unbounded) so we isolate software cost.
- Deploy: binary at D:/JSC_Github/ANARI-USD/build-out/anariBigCloudUsd (bind-mounted /opt/sender);
  sender container recreated with JUSYNC_SENDER_BIN=/opt/sender/anariBigCloudUsd.
- Sender lib rebuild: docker run -d --name jusync-dev --entrypoint sleep jusync-sender:build infinity
  + cmake -S /src -B /work/device (flags from Dockerfile L83-91); ninja --target anari_library_usd
  → docker cp libanari_library_usd.so into jusync-sender:/usr/local/lib.
- 100k smoke run already exercised the big-cloud sample (clip updates in place every frame ✓).



## Milestone 2 (lidar squeeze) — A→G ledger, 2026-10-10

Full plan: docs/plans/milestone-2-lidar-squeeze.md. All numbers measured headless on the
docker rig (loopback), 10M-point bigCloud, Epic LidarPointCloud plugin only.

| Step | Change | Before → After |
|---|---|---|
| A0 | baseline (RawGeo era start, 1 Hz/10M) | commit→flush 226 ms · transfer ~440 ms · parse+fill 154 ms · spawn **blocks game thread 3.5 s** · cycle ~5 s · 2 Hz starves (~95 aborted downloads / 75 s ≈ 0.013 fps effective) |
| A1 | broker single-copy chunk send (ANARI-USD e49143b) | commit→flush 226 → **193 ms** |
| B | async octree build off-thread + latest-wins stash (jusync 31c7930) | GT stall 3500 ms → **0.12 ms swap**; builds 3.9–4.8 s @10M invisible |
| C | ParallelFor parse/fill (jusync 596ff59) | parse+fill 154 → **13–20 ms** (10M), **3–5 ms** (2.5M) |
| D | sender JUSYNC_RAWGEO_STRIDE (ANARI-USD f2a33c6) | payload 120.0 → **30.0 MB** (÷4, zero protocol change) |
| E | Epic FLidarPointCloudLODManager verdict | no code: r.LidarTargetFPS=59 adaptive render decimation already on |
| F | GC: MarkAsGarbage on swap + throttled CollectGarbage (jusync 64371f6) | liveClouds 64 (5-min soak, unbounded) → **steady 2**; sweeps 13–24 ms/16 swaps |
| H | MPI multi-rank: lazy MPI_Init + bind-IP split (ANARI-USD 20a6c0b) | mpirun = instant abort → **N ranks stream**, client fetches each key from its owning rank |
| 11 | idle→full-res burst (ANARI-USD 05fec09) | permanently-decimated view → **6 quiet commits → one 10M frame, then 0 bytes** |

Steady-state at "G" (final, measured 2026-10-10):

- **2 Hz / stride 4 (2.5M pts)** — the interactive mode: sender commit→flush 84–92 ms,
  notify→got 131–149 ms, parse+fill 3–5 ms, build 918–957 ms (off-thread), swap ≤0.21 ms,
  62 updates shown in 55 s (**≈1.13 fps effective, ~1.07 s notify→pixel**), liveClouds 2,
  GT never stalls ≥50 ms. Target ≤2 s MET (and at 2 Hz where A0 showed ~0 fps).
- **1 Hz / stride 1 (full 10M)** — commit→flush 197–220 ms, transfer 534–684 ms,
  parse+fill 13–19 ms, 10M build 5.5–6.0 s off-thread under sustained 1 Hz backpressure,
  swap ≤0.21 ms, 14 full-density updates in 79 s, liveClouds 2. A0's cycle was ~5 s but
  with the game thread frozen 3.5 s every frame.
- **MPI mpirun -n 2, 2 Hz / stride 4** — both ranks store+notify their own
  `clips/*_r{0,1}*.geo`, rank0 relays, client downloads 91-from-r0/90-from-r1,
  44 swaps on BOTH actors, steady notify→got 152–164 ms.
- **Idle burst (FREEZE=45 s)** — burst `full-res 10000000 points (114.4 MB)` both ranks
  ~2.3 s after motion stops, 10M builds 4.3–5.8 s, then zero .geo traffic for the rest
  of the run while commits keep flowing.

Headline A→G ratios: game-thread stall **3.5 s → 0.21 ms** (≈16,000×); interactive
latency (2 Hz) **not achievable → 1.07 s**; effective shown-fps @2 Hz **0.013 → 1.13
(~87×)**; parse+fill **31–51×**; sender flush **2.3–2.7×**; wire bytes per frame **4×**
(→ 8–16× at higher strides for the thin JURECA link); paused-scene bandwidth
**2 Hz×30 MB → 0**, at **full 10M density**.

JURECA note: 2 Hz × 120 MB is impossible over the SSH tunnel — run stride 4–16 live and
let the idle burst deliver 10M when the sim pauses (one 120 MB transfer ≈ 10–12 s at
~10 MB/s). Tunnel dd measurement still pending.

