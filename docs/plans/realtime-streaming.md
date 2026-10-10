# JUSYNC Real-Time Streaming Plan — point clouds/meshes (10M+ pts, GB-scale updates) over SSH-tunneled ZMQ → UE 5.7

**Repos & branches**
- `D:\JSC_Github\jusync` → new branch `feat/realtime-streaming` (middleware + UE plugin under `UnrealPlugin/JUSYNC`)
- `D:\JSC_Github\ANARI-USD` → new branch `feat/rawgeo-streaming` (sender: ANARI USD device + broker)
- Companion UE project (worked in, not branched unless requested): `D:\JSC_Github\jusync-uesample` (UE 5.7)

**Constraints (hard)**
- No disk writes anywhere; all payloads stay in RAM (already true: MemoryStore/Middleware are in-memory).
- Network = SSH-tunneled TCP only (JURECA compute nodes: no UDP, no open ports).
- Rendering stays inside UE + plugin (no external DCC/tools).
- Protocol must stay forward-compatible ("unknown message types safely ignored" — already a design rule, protocol v3 → v4).

**Latency budget target**
- Live update visible < 3 s typical for 10M-pt cloud with regional changes; per-stage table must show why if not.

---

## Phase 0 — Observability (jusync) — DO FIRST
- [ ] `src/AnariUsdClient.cpp`: record t0=notify-received, per-request stage stamps (dl_start, dl_done, decompress_done, parse_done, fill_done, callback_done) in `DownloadResult`/diagnostics struct.
- [ ] `src/ParallelDownloadManager.cpp` / `ParallelDownloader.cpp`: propagate stamps (already times downloads; add parse/fill).
- [ ] `include/AnariUsdProtocol.h` (+ `_C` + `MiddlewareLogging`): expose diagnostics string/struct; extend `ProtocolDiagnosticsCallback` payload.
- [ ] UE `JUSYNCSubsystem.cpp` / `JUSYNCFileSpawnerActor.cpp` / `JUSYNCFileChangeTracker.*`: log state transitions (Queued→Downloading→Parsing→Spawning→done) + per-actor "visible lag".
- [ ] Add console command / stat HUD `JUSYNC.Streaming` showing per-file stage table (last N files).
- [ ] Run against real sim (or Docker rig, Phase D) → **write measured baseline to `docs/baseline-latency.md`**.
- Accept: stage-breakdown table proves dominant stage(s).

## Phase 1 — Binary `.usdc` validation (jusync test-heavy; ANARI 1 line)
- [x] ANARI sim/app config: `usd::serialize.outputBinary = true` — examples flipped 0→1 (ANARI commit `64cd835`); device default already true. JURECA app must stop passing 0.
- [x] Client parse routing verified: `tests/binary_parse/test_binary_parse.cpp` feeds real crates (tinyusdz `cube.usdc`, `suzanne.usdc`) through the production `QueryUSDFullLayout_C` → `FillUSDFullCompact_C` path — PXR-USDC magic detection (post-zstd) routes binary past ASCII-only transforms; fills finite geometry; PASS. Crate parse vs ASCII bench on 10M-pt data: deferred to rig/E2E (fixtures differ in size).
- [x] Verified wire design: store keys **keep `.usda` in both modes** (sender `TrackStageMemory` comment), clients sniff magic — so binary streaming needs no key/protocol changes. UE filters made extension-agnostic anyway (disk-mode names `.usd`).
- [x] Known gap (pre-existing, not a binary regression): `extractClipsFromString` regex in `resolveReferences` only sees text; live flow enumerates by broker notification names, so unaffected. Revisit only if initial-load fallback paths break in E2E.
- [x] Decision gate: binary crate = adopt for clips (size win alone ~3–4× vs ASCII; parse ≥ parity). If E2E shows crate parse slower, rawgeo (Phase 3) remains the mandatory geometry path.
- [x] Validation rig: `ANARI-USD/docker/jusync-sender/` — builds ANARI-SDK + OpenUSD 25.11 + device, runs ZMQ broker on :5556 + noVNC desktop + ParaView; UE connects `tcp://localhost:5556` as JURECA stand-in (`run.ps1`, GPU w/ CPU fallback).
- Accept: number in hand; toggle documented. → **Done locally; E2E throughput/parse numbers pending the rig (or real JURECA run) + Phase 0 `jusync.diag` table.**
- [x] **P1 crate regression caught & fixed (2026-10-09, byte-level):** the earlier "all stages stored as model/vnd.usd-crate" confirmation read the store's *metadata* — actual bytes were still `#usda` ASCII. pxr 25.11 has **no in-memory crate API**: `SdfLayer::ExportToString` and `SdfFileFormat::WriteToString` (including the usdc-format override) emit TEXT while returning `ok=true` — proven with a probe compiled against the sender container's own USD install. `ExportLayerToString` rewritten: `Export()` → `/dev/shm` tmpfs round-trip (bytes never touch a block device; unique name per export; unlinked immediately) + **PXR-USDC magic validation** before labeling crate + ASCII fallback; `JUSYNC_CRATE_TMP` env overrides the dir (empty = force ASCII; Windows default empty). Verified via stream_perf: crate=35/ascii=0; FullScene 12375→7632 B (1.6× at KB scale — ratio grows with geometry volume).

## Phase 2 — Client pipeline hardening (jusync)
- [x] Latest-wins mid-transfer ABORT (supersedes the "epoch in ParallelDownloader" sketch — that class is dead code, `#error`-guarded): `AnariUsdClient::requestFile` takes an `abortFlag`; on fire it stops consuming, `abandonRequestFrames()` purges buffered frames and makes the dispatcher DISCARD the broker's remaining chunks for that request id (5-min expiry). Middleware: abort-token registry + C API `JusyncAbortCreate/Trigger/Release_C` + `RequestFileIntoBufferAbort_C`. UE: `ActiveDownloadAborts` map in the spawner; supersede branch triggers the token; completion path skips the stale parse when `bSuperseded`; restart/cancel call `AbortAllInFlightDownloads()`.
- [ ] Optional `REQ_CANCEL` (type 103) on the wire so the BROKER stops sending (reclaims SSH-tunnel bandwidth; the local abort above still lets the fresh download start immediately). Ship client-side first: done; broker-side is a P5-adjacent follow-up.
- [ ] Live-update priority queue > initial spawn; remove `bInitialSpawnDone` gate for live path (`JUSYNCFileSpawnerActor.cpp:3193`).
- [x] Poll sleeps: production path (`waitForMatchingFrames`) was already condvar-driven; the 10 ms sleeps lived in the dead `ParallelDownloader.cpp` (untouched, guarded). New abort wake path uses `wakeResponseWaiters()` (lock+notify_all), no polling.
- [x] Dispatcher already routes zero-copy `FramePairPtr` shared_ptrs; chunk payload lands straight in the UE buffer via the in-situ callback (no dispatcher memcpy).
- [x] Notification keepalive (supersedes "heartbeat + auto-resubscribe": the broker has no SUBSCRIBE - it is a ROUTER pushing to recently-HEARD identities, which is exactly why a silently reconnected tunnel drops notifications until the next 20 s poll). Dispatcher's 100 ms idle tick now: sends a fire-and-forget `REQ_GET_PROPERTY totalWorkerCount` ping after 5 s of silence (reply discarded via the abandoned-request set; keeps the SSH tunnel warm and resets the dead-link timer), and after 20 s with zero inbound marks the link dead (diagnostics event + fails waiters immediately) and runs teardown+reconnect on one detached thread (3 attempts, 5 s apart; guarded by `explicitDisconnect` so a deliberate close is never resurrected; thread owns a `shared_from_this()` reference so no use-after-free). NOTE (2026-10-09 rig findings, below): the ping alone did NOT keep NOTIFY_* flowing — two gaps: the ANARI-USD broker only registered push identities in its list/file branches, and libzmq regenerates an anonymous socket's identity on every TCP reconnect.
- [ ] Knobs: chunk size (4→8/16 MB), SO_RCVBUF, ZMQ HWM; surface in subsystem details/cvars.
- [ ] `CommitCompleteCooldownSeconds`/`LiveUpdatePollInterval` defaults revisited with Phase 0 data.
- [x] **UE-side P0–P2 code build-verified (2026-10-09):** `UBT JusyncSampleProjectEditor Win64 Development` compiles + links clean against the abort-export import lib. Drift fixed, repo is now true source of truth: `JUSYNCBenchmarkActor.cpp/.h` + `ThirdParty/xxhash` ported in from the sample; `FJUSYNCBenchmarkResult` regained the stage-timing/counter fields its actor writes; spawner's duplicate `extern "C"` block gated `#if !WITH_ANARI_USD_MIDDLEWARE` (was C4273 vs the real dllimport header); `ECVF_CodeOnly` removed in UE 5.7 → `ECVF_Default` for `jusync.diag` (also makes it user-typable); Build.cs gained the `Json` module dep (cluster-report parsing) and regained `bUseUnity=false`; deploy script now syncs `Lib\Win64` lib/exp/dll — the import lib UBT actually links, which was 3 months stale.
- [x] **Broker keep-warm gaps found & fixed (2026-10-09, rig-measured):** the idle heartbeat alone did NOT keep NOTIFY_* flowing: (1) `UsdBridgeZmqBroker.cpp` registered push targets (`client_ids_`) only in its list/file request branches — property pings answered but never (re-)registered, so idle clients went dark after each broker restart (measured: 0 pushes in a 10 s window); (2) libzmq regenerates an anonymous DEALER's identity on every TCP reconnect, so pushes target stale ids exactly after tunnel blips. Fix: broker registers the identity on REQ_GET_PROPERTY too; client pins a stable `routing_id` per client instance (`AnariUsdClient::configureSocket`). Verified: 310 pushes / 12 s idle (file-update=297, commit-complete=13), broker-forwarded == client-received 1:1, zero NULL-callback warnings.
- [x] **stream_perf middleware harness (`tests/stream_perf/`, lives under BUILD_TESTS in the root CMakeLists):** drives the same C API order UE uses (Initialize→ConnectToBroker→register→StartReceiving); reports connect/list/download ms, crate-vs-ASCII magic sniff, abort-token responsiveness, notification counts by type, diag journal. Local rig (loopback, KB-scale tutorial clips): connect 7–11 ms, list 7.9 ms for 53 files, 0.4–0.7 ms per download, aborts: pre-fired rc=-1 @0.0 ms / in-flight abort @0.1 ms / post-completion triggers harmless. **Loopback upper bound ≈ 3–4 MB/s on tiny files (per-request overhead dominated) — NOT a JURECA tunnel estimate.**
- Accept: bursty commit stream → lag stays bounded (~1 service time), never grows.

## Phase 3 — RawGeo channel (SHIPPED 2026-10-09 — store-key side channel, no wire change)
Implemented as a **file-store side channel**, not a new protocol: the sender mirrors instancer float
arrays into the in-memory store under `clips/<name>__<time>.geo` with a self-describing
`JusyncRawGeoHeader` ('JSG1' magic, prim name, counts, color/attr descriptors) + raw float sections
(pack-1, AnariUsdProtocol.h). Broker/notify/diff-capture/file-list are key-generic → **zero wire or
middleware-DLL changes**.
- ANARI-USD (`feat/rawgeo-streaming`): `JUSYNC_RAWGEO=1` → `UsdBridgeUsdWriter::TryStoreRawGeo`
  memcpy-blobs positions(+colors+per-vertex float attrs); `SetGeometryDataTemplate` returns before
  `UpdateUsdGeometry`/`TrackStageMemory`, removing the per-frame pxr crate export (~1 s at 10M).
  USD clip twins stay registered but empty (~923 B) → auto-ignored by the client's
  `MinimumFileSizeBytes=1000` gate. `.geo` twin purged in `RemoveStoreEntriesForFile`.
  Example lesson: the SDK swallows no-op re-commits → anariBigCloudUsd must re-`anariSetParameter`
  the position array each frame (objects retained → prim names stay stable).
- jusync (UE plugin only): `JUSYNCIsLiveUpdateGeometryFile` + FilterFileList accept `clips/*.geo`;
  `LoadUSDFullCompactFromBuffer` sniffs JSG1 magic BEFORE any USD call → `JUSYNCParseRawGeoBuffer`
  fills `FJUSYNCPointCloudData.LidarPoints` directly (same (x,z,-y) swap, colors 0..1,
  attribute0.x→Widths, origin-inclusive bbox quirk as the middleware direct fill).
- Measured 10M/1 Hz loopback: sender commit→flush 1014→**226 ms**; parse+fill 770→**154 ms**;
  transfer ~440 ms (~272 MB/s); spawn ~3.5 s (octree → milestone 2 raw VF renderer); end cycle
  7.9→~5 s. At 2 Hz the 120 MB download is latest-wins-aborted nearly always (queue 770–920 ms)
  → run 10M at 1 fps until transfer + spawn shrink (chunk-size knob P2 + point renderer).
- Accept: ✅ met — 10M update = 120 MB wire, demux+fill 154 ms < 0.5 s.
- Original sketch (superseded, kept for context): REQ_GET_GEO(104)/RESP_GEO_CHUNK protocol v4 with
  dtype/stride/LOD fields — tiling (Phase 4) can still layer onto the `.geo` key scheme unchanged.

## Phase 4 — Tiling + LOD (ANARI device side, zero middleware change)
- [ ] Rawgeo emitter: voxel-hash point indices into K³ tiles (default 4³=64) → independent MemoryStore files+hashes.
- [ ] Device params: `usd::rawgeo::stride` (live LOD), optional `usd::rawgeo::quant` (fp16/int16+bbox) later.
- [ ] Middleware `RequestTileLOD(filename, lod)` API; policy in spawner: full-res for center/idle tiles only.
- Accept: typical regional commit = 1–10 MB wire; lag < 3 s on ~50 Mbps tunnel.

## Phase 5 — Throughput plumbing (jusync + ops docs)
- [ ] `InitializeMiddleware_C` accepts endpoint list; connection pool over 4 forwarded ports `-L 5556..5559`; per-file stream assignment.
- [ ] Verify broker send-loop pacing (`UsdBridgeZmqBroker.cpp` chunk-per-tick loop, no sleep-starvation); fix if found.
- [ ] Ops doc: SSH flags (`-o ServerAliveInterval=15`, no `-C`), iperf/dd measurement steps.
- Accept: multi-stream aggregate ≥2× single-stream; measured & logged.

## Phase 6 — UE plugin: GPU-resident point renderer (sample project/plugin)
- [ ] `UJUSYNCStreamedPointCloudComponent`: double-buffered StructuredBuffer upload (ENQUEUE_RENDER_COMMAND), pointer-swap per update.
- [ ] Compute pass: screen-space-size decimation (replaces r.LidarPointBudget octree logic); gradient-LUT in shader; optional gap-fill blur.
- [ ] Custom scene proxy instanced point-sprite draw (fallback toggle to ULidarPointCloud for static tiles).
- [ ] Per-update CPU cost target: octree rebuild gone; hitch < 2 ms on game thread.
- [ ] (Bridge to fluffy-volumes arc: compute voxel-splat pass stub.)
- Accept: live 10M-pt updates, zero respawn churn; VRAM ~200 MB steady; fps stable.

## Phase 7 — Optional frame deltas (both repos)
- [ ] Rawgeo: delta-vs-previous (seq-keyed), shuffle+zstd; keyframe every N + on-resync-request.
- [ ] Middleware base-cache per tile; apply-delta; resync policy.
- Accept: only if 0–6 leave gap; target further 5–10× wire reduction.

## Phase D — Docker dev rig (run alongside Phase 0)
- [ ] `D:\JSC_Github\jusync\dev\rig\Dockerfile`: `nvidia/cuda:12.x-ubuntu22.04` + Xvfb/x11vnc + noVNC + conda OpenUSD + ANARI-SDK + `feat/rawgeo-streaming` build + ParaView; `--gpus all -e NVIDIA_DRIVER_CAPABILITIES=all -p 6080:6080`.
- [ ] `docker-compose.dev.yml` + README: local broker on :5556 (localhost — no SSH), synthetic 10M-pt source (anariTutorial_usd / ParaView ANARI).
- [ ] Windows middleware DLL build + UE 5.7 sample connects to localhost broker → full loop offline.
- Accept: reproducible E2E test bench in <5 min from clean checkout.

---

## Execution order & effort
`0 (0.5d) → D (0.5–1d, parallel) → 1 (1d) → 2 (2–3d) → 3 (4–6d) → 4 (3–5d) → 5 (1–2d) → 6 (5–8d) → [7 if needed]`

## Risks
- TinyUSDZ crate parsing weak on huge VtArrays → mitigated by rawgeo (geometry never touches USD).
- Broker worker→rank0 hop for rawgeo big buffers → chunk via existing 4–16 MB path; verify no copies explode RAM (MemoryStore immutable-shared entries already used).
- ULidarPointCloud octree rebuild stays until Phase 6 → interim: cap live-res full-res point count via LOD.
- UE 5.7 plugin API drift for custom scene proxy → Phase 6 last, behind toggle.

## First actions upon Build mode
1. `git -C D:\JSC_Github\jusync checkout -b feat/realtime-streaming`
2. `git -C D:\JSC_Github\ANARI-USD checkout -b feat/rawgeo-streaming`
3. Commit this plan doc into jusync `docs/plans/realtime-streaming.md`.
4. Start Phase 0 instrumentation.

---

## Status notes - 2026-10-09 (headless harness + streaming fixes)

### Headless perf testing (answers "can we test without opening UE?")
- `tools/run-headless-perf.ps1`: runs the sample project via `UnrealEditor-Cmd.exe -game -nullrhi` with cmdline harness flags implemented in `FJUSYNCModule::MaybeStartHeadlessHarness` (JUSYNCModule.cpp):
  - `-JusyncAutoSpawner` spawn AJUSYNCFileSpawnerActor + StartSpawning() once the world begins play,
  - `-JusyncAutoDiag=SEC[,COUNT]` + `-JusyncAutoDiagRows=N` dump the jusync.diag table on a timer,
  - `-JusyncAutoQuit=SEC` force-exit (RequestExitWithStatus) so runs are scriptable.
- Fully automated: exit 0, log parsed by the script (harness timeline, last diag table, top errors).

### Bugs found & fixed (all verified headless)
1. **Empty viewport root cause** (0c8dd2b): hard-coded 2048-byte "tiny file" guards swallowed the 1303/1416 B tutorial clips on live-update paths; now honor MinimumFileSizeBytes (default 1000) like the initial load.
2. **PointInstancer never extracted** (UsdProcessor.cpp): ANARI-USD writes point clouds as `def PointInstancer` with `point3f[] positions.timeSamples`; ProcessPrim only handled GeomPoints. Added ExtractPointInstancerAsCloud (static or time-sampled positions, primvars:color incl. timeSamples via GeomPrimvar, attribute0 scalars for gradient bake). Points files went `pc=0` -> `pc=1`.
3. **Gradient deadlock**: sampler PNGs are 256x256 textures whose rows are a horizontal ramp, but the LUT loader rejected H>2 ("not a gradient") -> every gradient-colored PC parked in PendingNoColorClouds forever, PendingAsyncPCS pinned, initial spawn never completed, live updates never started. Fix: sample top scanline (BGRA8 GetRaw). Safety net: expired no-color clouds now spawn WITHOUT the gradient (bAllowNoGradient) instead of ping-ponging on the 2 s requeue.
4. **Stale middleware DLL shadowing**: deploy-ue-plugin.ps1 now also syncs `<sample>`Binaries`Win64 (UBT stages a copy there; if stale it wins the loader search - cost a debugging cycle).
5. `[COMPLETION] ENTRY` logged at Display per notification (2300 lines/75 s) - demoted to Verbose.

### Measured (tutorial rig, loopback broker; numbers = after last NOTIFY, trace-restart semantics)
- notify->spawn on live updates: **3.2 ms** (parse 0.3, fill 0.0, spawn 2.9) per 1.4 KB crate clip.
- Live updates flow end-to-end: Refreshing-file x35 in 75 s, PC direct-LiDAR spawns x36 (4 pts each, correct), zero parse failures, zero delaying-PC, pipeline complete + live-update gating released.

### Still open
- Broker restarts between loop passes make the initial list sometimes tiny (3 files) - harness-side retry or sender keep-warm across passes.
- `stream_perf` gained dev-only `[dump-substring out-path]` argv to pull broker bytes for inspection.
- JURECA tunnel throughput test (dd) still pending - gates P3 vs P5.


### Resolved in milestone 2 (2026-10-10, see docs/plans/milestone-2-lidar-squeeze.md + ten-million-run.md ledger)
- 2 Hz starvation → fixed: async off-thread builds + latest-wins stash (no aborted
  downloads) + sender-side stride LOD: 2.5M pts @2 Hz shows ≈1.13 fps at ~1.07 s
  notify→pixel; game thread max swap 0.21 ms.
- Chunk-size knob → superseded: the win was broker SINGLE-COPY chunk send instead
  (226→193 ms commit→flush; A0 ceiling probes showed copy-bound, not MTU-bound).
- Idle scenes → now auto-burst one full-resolution (10M) frame after 6 quiet commits,
  then stream zero bytes (JUSYNC_RAWGEO_IDLE_FRAMES).
- Multi-rank MPI senders stream per-rank `.geo` keys end-to-end (client fetches from
  owning rank; verified mpirun -n 2).
- Broker restart / tiny initial list + JURECA dd remain open (harness + field tasks).

