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

## Phase 2 — Client pipeline hardening (jusync)
- [x] Latest-wins mid-transfer ABORT (supersedes the "epoch in ParallelDownloader" sketch — that class is dead code, `#error`-guarded): `AnariUsdClient::requestFile` takes an `abortFlag`; on fire it stops consuming, `abandonRequestFrames()` purges buffered frames and makes the dispatcher DISCARD the broker's remaining chunks for that request id (5-min expiry). Middleware: abort-token registry + C API `JusyncAbortCreate/Trigger/Release_C` + `RequestFileIntoBufferAbort_C`. UE: `ActiveDownloadAborts` map in the spawner; supersede branch triggers the token; completion path skips the stale parse when `bSuperseded`; restart/cancel call `AbortAllInFlightDownloads()`.
- [ ] Optional `REQ_CANCEL` (type 103) on the wire so the BROKER stops sending (reclaims SSH-tunnel bandwidth; the local abort above still lets the fresh download start immediately). Ship client-side first: done; broker-side is a P5-adjacent follow-up.
- [ ] Live-update priority queue > initial spawn; remove `bInitialSpawnDone` gate for live path (`JUSYNCFileSpawnerActor.cpp:3193`).
- [x] Poll sleeps: production path (`waitForMatchingFrames`) was already condvar-driven; the 10 ms sleeps lived in the dead `ParallelDownloader.cpp` (untouched, guarded). New abort wake path uses `wakeResponseWaiters()` (lock+notify_all), no polling.
- [x] Dispatcher already routes zero-copy `FramePairPtr` shared_ptrs; chunk payload lands straight in the UE buffer via the in-situ callback (no dispatcher memcpy).
- [ ] Notification socket: heartbeat + auto-resubscribe on reconnect (kills silent 20 s backstop lag mode).
- [ ] Knobs: chunk size (4→8/16 MB), SO_RCVBUF, ZMQ HWM; surface in subsystem details/cvars.
- [ ] `CommitCompleteCooldownSeconds`/`LiveUpdatePollInterval` defaults revisited with Phase 0 data.
- Accept: bursty commit stream → lag stays bounded (~1 service time), never grows.

## Phase 3 — RawGeo channel (flagship; mostly jusync, one ANARI PR)
- [ ] Protocol v4 (`AnariUsdProtocol.h`): `RESP_GEO_CHUNK {magic, type, request_id, source_rank, filename[256], prim_path[256], kind(points|tri_mesh), seq, tile_id, count, dtype(fp32|fp16|int16_norm+u8col), stride, total_size, offset, chunk_size}` + REQ_GET_GEO(104).
- [ ] ANARI `UsdBridge`: rawgeo emitter in the TrackStageMemory/export step for GeomPoints/meshes → memcpy of pxr VtArrays → zstd-framed store (`UsdBridgeMemoryStore` reuse) → inherits DiffCapture hashes/notify.
- [ ] Middleware: rawgeo demuxer (no USD involved) → `CPointCloudDataFill.lidar_points` / compact mesh fill — existing API paths, callbacks unchanged.
- [ ] UE plugin: handle rawgeo-backed fills natively (LiDAR path today; GPU path Phase 6).
- [ ] usda/usdc retained for scene structure/materials/hierarchy only.
- Accept: 10M-pt update = ~130 MB binary (35 MB @ stride 4), demux+fill < 0.5 s.

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
