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

### Task 8 — H: MPI multi-rank RawGeo streaming (user scope-add 2026-10-10)

Requirement: the whole RawGeo path must work when ANARI-USD runs under
mpirun (N>1), and the jusync UE client must fetch `.geo` from the OWNING
rank. Runs after Task 5 so the MPI test also exercises stride.

Architecture facts (verified in code):
- MPI mode: rank0 = ZmqBroker; EVERY rank runs `InitializeMemoryStore(rank)`
  (UsdDevice.cpp:536) and workers run ZmqWorker + their own
  `FileServingThreadLoop` (UsdDevice.cpp:521-524). Files stored on a worker
  stay in the worker's store; rank0 forwards REQ_GET_FILE with
  `target_rank>=1` (UsdBridgeZmqBroker.cpp:1030-1070) and relays the worker's
  chunks to the client (:1276-1330).
- `TryStoreRawGeo` ends in `g_rankMemoryStore->StoreFile(geoKey, ...)` on the
  LOCAL rank (UsdBridgeUsdWriter.cpp:2070-2071) → on workers it fills the
  worker store. RawGeoEnabled() reads env per-process → stride/RawGeo gates
  propagate naturally under mpirun.

To verify/fix (dig-in list):
1. Worker→rank0 file-update propagation: find how a worker StoreFile reaches
   rank0's client-facing LIST + NOTIFY_FILE_UPDATE_V2 (broker :1375-1440
   "Received from worker" path; likely a FILE_UPDATE message naming keys
   without bytes). Confirm `.geo` keys ride the SAME channel as `.usda/.usd`
   twins (both via StoreFile choke point) — if the update message is emitted
   per-key by the diff-capture/flush flow on workers, we get it free.
2. Rank0 broker list serving reads `g_rankMemoryStore` (rank0's own store,
   :607). If worker keys are absent from the list, client never learns the
   `.geo` — check how worker `.usda` clip keys currently become visible to
   clients (that mechanism must cover `.geo` too; if the client gets a merged
   list with per-file rank, done; if not — mirror the USD twins' treatment).
3. Notification source_rank → client: JUSYNCSubsystem/BlueprintLibrary
   download call must pass the file's rank into AnariUsdClient::getFileData
   `targetRank` (src :820/:986 already parameterized). Verify the `.geo`
   in-situ fetch uses the entry's rank (not hard-wired 0), and that the
   RawGeo sniff + store twin logic tolerates rank>0 files.
4. Client spawn semantics: spawner keys already include rank
   (`%s_r%d`); async builds are per-key so multi-rank = N actors, each
   building independently — confirm drain budget/1-item-per-tick doesn't
   serialize badly at N=2 (fine: async start is ~µs).
5. ZmqWorker::SendFileChunk relay through rank0: relay at :1276-1330 — apply
   single-copy if it re-copies frames (match Task 2 win on the worker path).
6. anariBigCloudUsd under mpirun: SPMD replication or partition? Read the
   example; if replicated, each rank stores the same geoKey in its OWN store
   (client still downloads once, from whichever rank owns/answers). If
   partitioned by prim/clip naming, per-rank keys — verify naming flows into
   `.geo` twins unchanged.
7. Rig: run the sender container with mpirun -n 2 (OpenMPI is in the image;
   check run-sender.sh honors a JUSYNC_MPI_RANKS/NPROCS env; if not, add it
   to the docker dev flow). Recreate `jusync-sender` with NPROCS=2 +
   ANARI_BIG_FPS=2 + JUSYNC_RAWGEO_STRIDE=4, run headless: expect BOTH ranks'
   .geo keys listed, downloads target rank correctly (middleware log shows
   target_rank), two actors spawning/updating in UE, zero USD-text fallback
   (no big .usda commits for the instancer on either rank).
8. If worker-rank RawGeo is broken: prefer fixes on the middleware/broker
   side (routing/listing) over sender-side hacks; NO protocol change if
   avoidable (target_rank already in ZmqFileRequest; source_rank already in
   notifications).

Verification = the mpirun -n2 headless run (numbers + greps in ledger)
AND a regression single-rank run (unchanged behavior). Commits: ANARI-USD
(fixes), jusync (client rank fix if needed), both feature branches.

## Task 10 — ParaView time-series cross-check (future test asset)

Independent visual validation of what the wire actually carries, without UE.

- Sender-side dump (already implemented, opt-in): `JUSYNC_RAWGEO_SNAPSHOT_DIR`
  + `JUSYNC_RAWGEO_SNAPSHOT_EVERY=N` write `cloud_%08llu.ply` (binary,
  float x/y/z [+ float rgb]) built from the EXACT packed JSG1 payload after
  stride sampling → ParaView sees byte-identical geometry to the UE client.
  With the dev sender, `JUSYNC_RAWGEO_SNAPSHOT_DIR=/opt/sender/snaps` lands
  files at `ANARI-USD/build-out/snaps/` on the host.
- Sanity check without ParaView: `docker exec jusync-sender python3 /opt/sender/parse_ply.py`
  (script in build-out) → per-file vertex count (must equal 10M/stride) + byte size.
- ParaView in Docker (future): `docker run -it --rm -p 8080:8080
  -v D:/JSC_Github/ANARI-USD/build-out/snaps:/data
  kitware/paraview:5.13.0-egl glxinfo`-style EGL images expose ParaView; open
  `/data/cloud_*.ply` with reader "File Reader" pattern `%08d` → automatic
  time-series animation of the sampled cloud (verify stride lattice integrity:
  decimated cloud should look like the full one, just sparser).
- Value: catches wire-corruption/ordering/stride bugs no UE log would show,
  and doubles as a second consumer for the Task 8 MPI check (worker-rank keys
  must also snapshot).

## Success bar (user-visible)

Task 11 (closing feature, user-approved 2026-10-10): idle→full-res burst.
While the scene is live the sender streams stride-decimated frames; when the
geometry goes QUIET the sender emits one stride=1 frame so UE always settles
at full density.

- Idle detection (generic, middleware side, UsdBridgeUsdWriter::TryStoreRawGeo):
  cheap change-detect per commit — compare the sampled positions block of this
  frame against the previous frame's (memcmp of the packed payload is bounded;
  on first mismatch or hash env change, reset counter). After
  JUSYNC_RAWGEO_IDLE_FRAMES (default 6) identical commits → next store ignores
  stride (full num_points), log `[UsdBridge] RawGeo idle burst: full-res`, then
  wait for the next real change to resume striding. One burst only per idle
  period.
- Test rig: anariBigCloudUsd dev-env ANARI_BIG_FREEZE_SEC=N — after N seconds
  positions stop updating but commits continue at ANARI_BIG_FPS (models
  "sim paused / user inspecting" while keeping commits alive, which is what the
  writer's idle detector needs).
- Client side: zero changes (header self-describes 10M; async build path +
  latest-wins already handle a big frame: ~0.5 s transfer loopback, ~5 s bg
  build, 0.1 ms swap; no newer frames arrive while idle so the burst sticks).
- Verify (one headless run, FREEZE=60): stride frames for ~60 s, then log
  shows burst, client retrieves 120,000,112 B, one 10M build completes, live
  cloud = full density; then FREEZE ends? no — resume via second env
  ANARI_BIG_FREEZE_UNFREEZE? not needed: burst stickiness is proven by the
  final frame count in spawn log.
- Bandwidth note for JURECA: burst costs one 120 MB transfer (~10-12 s over
  the thin link) — acceptable because it only fires when the scene is static,
  exactly when the user is looking at detail.

## Deferred to milestone 3

- Progressive refinement (#3): stream complementary stride subsets so the
  client merges 2.5M batches into a growing 10M octree incrementally
  (remote-desktop style quality ramp). Real client+middleware work; the
  stride mechanism built in Task 5 is its foundation.

Interactive: 10M-pt cloud at JURECA-scale bandwidth with ≤ ~2 s notify→pixel
at 2 Hz, game thread never stalls ≥ 50 ms, no custom renderer, zero protocol
changes, all wins inside Epic-maintained APIs + our middleware plumbing.
MPI: the same RawGeo fast path works with the sender running under mpirun
(multi-rank), client fetching from the owning rank — JURECA's real launch
mode.
