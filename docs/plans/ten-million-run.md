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
- [ ] 8. Octree replacement (raw VF point sprites, off-thread double buffer) — THE remaining
      blocker to sub-second: spawn is 70% of the cycle. Then chunk-size 1→8/16 MB for the wire.
- [ ] 9. Commit both repos; update realtime-streaming.md + graphify.

## Notes
- 10M pts x float3 = 120 MB raw per timestep (plus colors/widths).
- Network to JURECA NOT part of this; loopback first (bandwidth ~unbounded) so we isolate software cost.
- Deploy: binary at D:/JSC_Github/ANARI-USD/build-out/anariBigCloudUsd (bind-mounted /opt/sender);
  sender container recreated with JUSYNC_SENDER_BIN=/opt/sender/anariBigCloudUsd.
- Sender lib rebuild: docker run -d --name jusync-dev --entrypoint sleep jusync-sender:build infinity
  + cmake -S /src -B /work/device (flags from Dockerfile L83-91); ninja --target anari_library_usd
  → docker cp libanari_library_usd.so into jusync-sender:/usr/local/lib.
- 100k smoke run already exercised the big-cloud sample (clip updates in place every frame ✓).

