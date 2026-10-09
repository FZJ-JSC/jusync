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
- [ ] ANARI sim/app config: `usd::serialize.outputBinary = true` (device param exists: `UsdDevice.cpp:120`, defaults true in `UsdDevice.h:46` but examples pass 0).
- [ ] Middleware: bench TinyUSDZ crate parse vs ASCII with synthetic 10M-pt `.usd` (generate via anariTutorial_usd binary mode): ms/MB, peak RAM.
- [ ] Verify zstd-magic sniff + hash-on-compressed path works for crate payloads end-to-end.
- [ ] Decision gate: crate parse ≥3× faster than ASCII → adopt for structure files; else rawgeo (Phase 3) is mandatory for geometry, structure stays ASCII.
- Accept: number in hand; toggle documented.

## Phase 2 — Client pipeline hardening (jusync)
- [ ] Latest-wins: per-filename epoch; superseded in-flight → stop assembling, drop arriving chunks, free buffers (AnariUsdClient + ParallelDownloader).
- [ ] Optional `REQ_CANCEL` (type 103) sent on supersede; broker ignores if unknown (ship client-side first).
- [ ] Live-update priority queue > initial spawn; remove `bInitialSpawnDone` gate for live path (`JUSYNCFileSpawnerActor.cpp:3193`).
- [ ] Replace 10 ms poll sleeps with condvar/zmq poll (`ParallelDownloader.cpp:196,281,336`).
- [ ] Dispatcher thread: route frame pointers; move chunk memcpy off dispatcher (`AnariUsdClient` single-recv design).
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
