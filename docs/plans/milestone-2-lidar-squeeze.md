# Milestone 2 — Lidar-squeeze: make the maintained LiDAR plugin path realtime

Approved 2026-10-09 (chat): NO custom vertex factory. Squeeze Epic's
LidarPointCloud plugin + our existing pipeline instead. Execute A→G with a
measured ledger after each step.

## Baselines (loopback, 10M pts @ 1 Hz, RawGeo on; from Phase-3 ledger)

| Metric | Baseline |
|---|---|
| sender commit→flush | 226 ms |
| transfer (120,000,112 B) | ~440 ms (~272 MB/s) |
| client parse+fill | 154 ms |
| spawn `SetData` (octree, game thread) | ~3 500 ms |
| queue wait @2 Hz | 770–920 ms (downloads latest-wins-aborted) |
| end-to-end cycle @1 Hz | ~5 s |

## Findings that re-scope the original A/B/C plan

- Chunk size is NOT 1 MB: client requests `DEFAULT_CHUNK_SIZE = 32 MB`
  (AnariUsdClient.h:398) and broker honors up to 128 MB cap
  (UsdBridgeZmqBroker.cpp:983/1461), poll tick tightens to 1 ms while
  transfers are in flight (line 538). → Task A becomes: find where the 440 ms
  actually goes (Docker Desktop NAT ceiling? broker copies?) and fix that.
- Broker builds each chunk as `std::vector<uint8_t> msgData` + memcpy + a
  SECOND copy inside `zmq::message_t(data, size)` ctor (lines 1573-1580).
  Prime target for copy elimination (120 MB × 2 extra copies per update).
- Spawner conversion already runs on a background task; ONLY `SetData` +
  recolor loops remain on GT paths (DrainReadyQueueLocked, line ~686).
- ULidarPointCloud ships Epic async APIs: `CreateFromData(Points,
  FLidarPointCloudAsyncParameters{bUseAsync, Progress, Completion})`
  (LidarPointCloud.h:33-53,669-677), `InsertPoints(..., bCanceled,
  ProgressCallback)` (:536-541), `bAsyncCancelled` (:141). Use these.

## Constraints

- Keep user WIP uncommitted/untouched: jusync `CMakeLists.txt`,
  `src/UsdProcessor.cpp`; untracked `graphify-out/`, ANARI-USD `build-out/`.
- Never push. Commits: jusync → `feat/realtime-streaming`, ANARI-USD →
  `feat/rawgeo-streaming`.
- Work happens in-place in D:\JSC_Github\* trees (container bind-mounts and
  the UE-sample robocopy key on these absolute paths; branches are already
  isolated from main). Ledgered as a ruling instead of a new worktree.
- Sender lib dev flow: `ninja -C /work/device anari_library_usd` in
  `jusync-dev` → `docker cp` to `build-out/` → bind-mounted over
  `/usr/local/lib/libanari_library_usd.so` in `jusync-sender`; env changes
  REQUIRE container recreate (run with same -v/-p flags + new -e).
- UE plugin edit flow: `tools/deploy-ue-plugin.ps1` (robocopy) THEN
  `Build.bat JusyncSampleProjectEditor Win64 Development -Project=
  D:\JSC_Github\jusync-uesample\JusyncSampleProject.uproject` (UE editor
  must be closed). Measure via `tools/run-headless-perf.ps1`.
- JSG1 protocol headers exist in 3 synced copies — only touch them if a task
  says so (none do; stride is sender-internal, num_points shrinks naturally).

---

### Task 1 — A0: quantify the transfer ceiling + refresh baseline

Prove where the 440 ms lives before optimizing anything.
- `docker port jusync-sender` to see published ports; recreate the sender
  (same env: ANARI_BIG_POINTS=10000000 ANARI_BIG_FPS=1 JUSYNC_RAWGEO=1
  JUSYNC_LOOP=1) with one extra diagnostic publish `-p 5599:5599`.
- In-container `python3 -m http.server 5599 --directory /tmp` serving a
  400 MB /dev/zero file; on host: `Invoke-WebRequest`/`curl.exe` it to NUL,
  record MB/s. Repeat 3×.
- Also grab a fresh end-to-end baseline: one `run-headless-perf.ps1
  -DiagEvery 10 -DiagCount 6 -QuitAfter 75` run; mine transfer/parse/SetData
  timings from the log the usual way (notify→Retrieved file = transfer,
  Retrieved→RawGeo payload = parse, PERF PC SetData line = spawn).
- Ledger the numbers. Ruling for Task 2 input: NAT-bound (ceiling ≤ ~350 MB/s)
  vs copy-bound (headroom ≥ 2×).
- No commits (probe-only); plan-file commit is this task's BASE.

### Task 2 — A1: kill broker copies (if headroom) or document ceiling

- Edit `UsdBridgeZmqBroker.cpp` chunk send: build one `zmq::message_t
  hdr+payload` in a REUSED scratch buffer (member `std::vector<uint8_t>
  chunkScratch`, resize-on-demand) with a single memcpy, send via
  `zmq::const_buffer(msg.data(), msg.size())` overload that does not copy
  again (cppzmq `message_t(buffer_t&&)` or construct message from
  zmq_msg_init_size + direct write). Exactly one copy per chunk.
- Rebuild via `jusync-dev`, redeploy, re-run 1 Hz perf. Record transfer ms +
  sender commit→flush. If NAT-bound per Task-1 ruling, still keep the change
  (helps production CPU + tunnel pacing) but expect transfer parity; ledger it.
- Commit ANARI-USD `feat/rawgeo-streaming`.

### Task 3 — B: async octree build + latest-wins swap (the 3.5 s → ~0 hitch)

Rework `JUSYNCPointCloudSpawner` direct-LiDAR path:
- `DrainReadyQueueLocked` no longer calls `LidarCloud->SetData(Entry.Points)`
  on GT. Instead: start (or supersede) ONE in-flight build per actor key:
  new `ULidarPointCloud` via `CreateFromData(TArray64<FLidarPointCloudPoint>&,
  FLidarPointCloudAsyncParameters{true, nullptr, completion})` — points moved
  in (keep Epic's "don't invalidate data" contract alive by the cloud owning
  its copy once built).
- Completion (worker thread) → `FFunctionGraphTask` to GT → if this build is
  still the latest generation for the key: `Comp->SetPointCloud(NewCloud)`
  (+ re-apply actor bounds/transform, `MarkRenderStateDirty`), pool the OLD
  cloud (keep at most 2 assets per key: visible + being-built; GC the
  third). If superseded: drop the stale asset.
- If a build is in flight when the next ReadyQueue entry for the same key
  arrives: stash it as `PendingReplacement` (latest-wins, do NOT stack); kick
  its build on completion. Update `ten-million-run.md` queue semantics: GT
  budget no longer blocks on octree.
- Normals: ensure `MaybeCalculateNormals` is a no-op for streaming clouds
  (skip/bypass) — measure.
- Verification: 1 Hz run → `PERF PC SetData` GT line replaced by `PERF PC
  async build ... (bg)` + GT-swap ms ≤ 10 ms logged; viewport loop keeps
  ticking (no multi-second GT stall in diag timestamps). 2 Hz run → queue
  wait gone (ReadyQueue ≤ 1 deep, no .geo download aborts from GT stall).
- Commit jusync `feat/realtime-streaming`.

### Task 4 — C: recolor loops parallel + parse-thread audit

- `ParallelFor` the LUT-recolor loop in the background conversion (spawner
  ~:188) and any equivalent in the legacy path.
- Audit: where does `JUSYNCParseRawGeoBuffer` run (which thread calls
  `LoadUSDFullCompactFromBuffer`)? If GT, move the whole parse+fill to the
  existing AsyncTask stage (or middleware worker) so GT only receives the
  finished arrays.
- Verification: `RawGeo payload` fill ms (expect 154→~60 at 10M with
  ParallelFor) + thread-name log once; no functional change (points match:
  spot-check hash in diag table).
- Commit jusync.

### Task 5 — D: sender `JUSYNC_RAWGEO_STRIDE` (live LOD, zero protocol change)

- `UsdBridgeUsdWriter::TryStoreRawGeo`: read env `JUSYNC_RAWGEO_STRIDE`
  (default 1) once; when >1, sample every Nth point (positions, colors,
  per-vertex attrs) → `num_points = ceil(N_src/stride)`. bbox/origin fields
  computed from SAMPLED points (client semantics unchanged). USD path
  untouched; client untouched (JSG1 self-describes count).
- Rebuild lib, redeploy, recreate sender container at **2 Hz with
  stride 4** (2.5M pts, ~30 MB/frame) with diag `-JusyncAutoDiag=10`.
- Verification: payload size in logs == expected 2.5M×12+hdr; sender
  commit→flush and transfer drop ~4×; parse+fill ~4×; async build ~3.5/4 s
  (from Task 3) → end-cycle < 2 s at 2 Hz; points visibly update at 2 Hz
  (frame counter in sender log advances, client `time` field changes).
- Commit ANARI-USD.

### Task 6 — E+F: render budget + memory ledger

- Set streaming component `RenderingBudget` explicitly (check
  ULidarPointCloudComponent property; pick sane default e.g. 100 with
  r.LidarPointCloud LOD path documented) so idle 10M view stays smooth;
  expose as spawner config.
- Add cheap memory trace to the perf path: log per completed cycle
  `FPlatformMemory::GetStats().UsedPhysical` + count of live
  `ULidarPointCloud` assets (`GetTransientPackage` objcount or static
  counter in spawner). Run 5-min soak at 2 Hz/stride 4.
- Verification: RSS steady ±5% over soak (no per-cycle growth); asset count
  ≤ 2×keys. Fix pool leak if not. Commit jusync.

### Task 7 — G: re-measure full ledger, docs, review

- Runs: 1 Hz/stride 1 (10M, "idle high-quality") and 2 Hz/stride 4
  ("interactive"). Mine every metric from Task 0 baselines.
- Produce the A→G table (ms per stage before/after + % of cycle).
- Update `realtime-streaming.md` (Phase 6 replaced by this milestone's
  outcome; P2 line closed with the NAT finding) + tick `ten-million-run.md`.
- graphify update. Final whole-branch review (fresh subagent, both repos).
- Commits to jusync.

## Success bar (user-visible)

Interactive: 10M-pt cloud at JURECA-scale bandwidth with ≤ ~2 s notify→pixel
at 2 Hz, game thread never stalls ≥ 50 ms, no custom renderer, zero protocol
changes, all wins inside Epic-maintained APIs + our middleware plumbing.
