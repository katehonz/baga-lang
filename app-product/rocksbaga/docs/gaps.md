# rocksbaga — language & storage gaps (was lsmbaga)

Probe log from building the Track S flagship on S2–S4 + MEM-1/2.

## L1 — struct scalar fields do not mutate through by-value params

**Symptom.** `fn f(pc: PageCache) { pc.hand = 1 }` does not update the
caller; Maps inside the struct do (shared). Vec field *reassignment*
(`pc.keys = nk`) also does not stick; `vec_push` on the shared vec does.

**Workaround.** Return-updated struct (`fn f(pc) -> PageCache` / `PcGet`
wrappers). Documented in page.baga.

**Severity.** Medium ergonomics tax for every engine mutator.

## L2 — no `rmdir`; cleanup leaves empty dirs

**Symptom.** Tests can `mkdir` but not remove the directory after
`unlink` of children (only `unlink` for files).

**Workaround.** Prefer path **prefix** files (no directory), like queuebaga.

## L3 — SST full-file load on every get (partial)

**Symptom.** `sst_get` still **reads the whole SST file** through the page
cache (full body IO). Fine at probe scale; true block-level page IO is later.

**Shipped (R1):** after full parse, lookup was **binary search** + min/max.

**Shipped (R2):** new writes use **BAGASST2** with a **restart index** every
16 records. Get does CRC + restart **bsearch** + **one-block scan** (no full
row materialize). Compaction/`KEYS` still full-parse. **BAGASST1** still
readable (parse + bsearch).

**Shipped (R4):** bloom + restart (BAGASST3).

**Shipped (R5):** **BAGASST4** footer + partial get; **L0/L1**.

**Shipped (R6):** **BAGASST5** per-restart-block **crc32c** (verified on partial
get block load). **L2** level: L1 ≥ `compact_at` → merge to L2; collapse ≥2
L2 files. File-count size-tier lite. v1–v4 readable.

**Shipped (R7):** **byte-size level targets** via `LsmDB.target_bytes` (L0 = T,
L1 = 4T, L2 = 16T, L3 = 64T) + **L3** promote from L2. File-count `compact_at`
still works; `target_bytes=0` (default) keeps R5/R6 pair-collapse behaviour.
Env `LSM_TARGET_BYTES` on serve. Test: `r7_*` in `tests/lsm_test.baga`.

**Shipped (B4.1):** recovery story + test (`tests/lsm_recover_test.baga`):
WAL+SST reopen (tomb/wal/sst), multi-reopen after compact, page-cache stress
with `cap=2` (eviction) still correct. v5 get remains partial-file for hits.

**Shipped (R8):** **oldest-N merge pick** via `LsmDB.merge_pick` (0 = merge all
over files; >0 = oldest max(2,N), with byte targets growing the pick until
coverage). Env `LSM_MERGE_PICK`. Test: `r8_*` in `tests/lsm_test.baga`.

**Shipped (R9):** **standalone bloom sidecar** `<dir>.bloom.<gen>` (`BAGABLM1`
+ m + bits + crc). Written with every new SST; `sst_get` skips SST open on
definite miss. Embedded bloom remains in BAGASST5 for compatibility.
Unlinked with SST on compact. Test: `r9_*` in `tests/lsm_test.baga`.

**Shipped (R10):** package renamed **`rocksbaga`** (was `lsmbaga`);
`app-product/lsmbaga/` is a deprecated import shim. API symbols still `lsm_*`.

**Shipped (layout):** multi-directory architecture — `util/`, `cache/`, `wal/`,
`table/{bloom,sstable}`, `db/{types,compact,engine}`, `net/`, `examples/`,
`docs/` + root re-exports. See [ARCHITECTURE.md](../ARCHITECTURE.md).

**Shipped (R11 get path):**
- **`pc_read_at`** preallocates (was O(n²) `bytes_push` per byte).
- **In-memory bloom cache** (`BloomCache` on `LsmDB`) — load sidecar once
  per gen; invalidate on compact.
- **Open SST fd cache** (`sst_fds` / `sst_sizes`) — no open/close per get.
- Skip embedded bloom when sidecar already said maybe.
- Skip per-block CRC on partial get (full-parse / compact still body-CRC).
- Default page cache **256 × 4 KiB**.
- Bench (`bench/rocks`, n=1000 durable): GET ~0.5k → ~80k ops/s (~150×);
  still well below RocksDB (~0.5M ops/s). See `results/vs-rocksdb-latest.txt`.

**Shipped (R12):** `SstMeta` cache on `LsmDB` — footer, restart offsets,
first/last key loaded once per gen. Partial get skips re-reading index and
last block. Bench n=1000 durable: GET ~80k → ~167k ops/s (~31% of RocksDB).

**Shipped (R13):** `SstMeta.rkeys` — all restart keys loaded once; get does
in-memory bsearch + **one** block `pc_read_at`. Bench n=1000 durable:
GET_SEQ ~200k ops/s (~37% RocksDB), GET_RND ~250k (~48%).

**Shipped (R14 put path):**
- `push_u32_le` / `push_u8` via one concat (not 4× `bytes_push`).
- Preallocated `wal_record` / bloom sidecar / SST file buffer.
- `sst_build` returns bloom once (sidecar reuses it).
- **WAL write buffer** (`wal_buf`, default 64 KiB) flushed on sync / full /
  memtable flush / close.
- Bench n=1000 durable PUT ~695 ops/s (~82% RocksDB). Batch PUT still ≪
  RocksDB (interpreter + flush tax).

**Shipped (R15):** poll multi-conn RESP serve — see L4 below.

**Shipped (R16 TTL):** optional `BAGATTL1` value envelope (`util/ttl.baga`),
lazy expiry on get/flush/compact; RESP `SET EX`/`SETEX`/`EXPIRE`/`TTL`/
`PERSIST`. Tests: `ttl_*` in `tests/lsm_test.baga`.

**Shipped (R17 `sst_build` prealloc):** the R14 concat chain in `sst_build`
was O(n²) in time **and** in unreachable garbage — Baga has no GC, so every
`bytes_concat` intermediate lives until exit. Merges past ~4.5k rows (n=5000
bench, L3 collapse) allocated ~7 GB per call and got OOM-killed (froze the
host). Rewritten as exact prealloc + offset writes (two passes, zero concat).
Bench n=10000 durable now completes: PUT ~549 ops/s, GET_SEQ ~175k,
GET_RND ~123k. **Language lesson:** in a no-GC language, growable-buffer
concat chains are memory bombs even when they are "fast enough" — prealloc
like R14 did for WAL, or pay quadratic RSS.

**Shipped (R18 server memory arc — MEM-1 machinery applied):** soak test
(RESP server, 100k cmd) drove RSS 1 MB → 6.8 GB (~34 KB/cmd). Root causes
and fixes:
- `tcp_read_bytes` built a Vec<i64> per byte (~100 KB garbage per 4 KB read)
  → prealloc `bytes_set` shape; new `tcp_read_into` / `fd_pread_into` take a
  caller-owned scratch str (allocated once), so a read costs only the
  returned (droppable) bytes.
- serve loops: zero-copy chunk adopt + `drop` of consumed chunk / args
  spine / reply per command.
- WAL buffer: offset append into a preallocated 64 KiB buffer
  (`wal_buf_len`) — the concat append leaked the whole window per record.
- `sst_read_raw`: exact prealloc via `fd_size` + page copies — the
  per-page `push_bytes` chain leaked O(pages²) (~45 MB per 600 KB merge).
- `drop` on: loaded SST bodies after parse, merge row vecs / acc maps,
  written SST body/file/bloom, evicted/invalidated page-cache pages.
- **Runtime: free list extended past 1 KiB** — pow2 classes 2 KiB..32 MiB
  (`baga_fl_big`, full-class-size bump like the MEM-1 small-class fix).
  Without this, `drop` of any block >1 KiB was a no-op and the whole
  storage-engine churn (4 KiB pages, SST bodies) could never recycle.
  C backend only; LLVM backend has no drop/free-list yet (parity TODO).

Soak after: PING 1.3 KB/cmd, GET ~2 KB/cmd, SET ~9 KB/cmd (was 34/37);
engine in-process 200k puts complete (was OOM ~150k). Bench n=10000
durable: PUT ~599 ops/s, GET_SEQ ~204k, GET_RND ~164k — faster than R17.
**Residual (by design, v1):** `str` is not droppable (plan §6) — RESP arg
strings and small temporaries (~1 KB/cmd) stay arena-bound forever. That
is the MEM-3-full / str-reclamation roadmap item, not a bug.

**Shipped (R19 shared page cache + pin counts):**
- **`PageCache.fds`** — `file_id → fd` registry. Eviction writeback resolves
  the correct fd per dirty page (no more single-fallback-fd bug when the
  cache holds pages from multiple SSTs). `pc_register_file` /
  `pc_unregister_file`; wired from `sst_fd_get` / `sst_fd_drop` /
  `sst_read_raw` / compact invalidate / `lsm_close`.
- **`PageCache.pins`** — pin count per cache key. Clock eviction never
  drops `pin > 0`; if every resident page is pinned, `pc_ensure_cap`
  returns `rc = -2` (buffer pool full).
- **`pc_pin` / `pc_unpin` / `pc_get_pin`** — explicit hold for callers that
  keep page bytes across further cache mutations. `pc_read_at` pins each
  source page while copying so a tiny cap cannot free the page mid-span.
- Tests: `tests/page_cache_test.baga` (pin survival, multi-file writeback,
  all-pinned fail); existing `lsm_test` / `lsm_recover_test` green.

**Shipped (R20 block-level scan + `table/block.baga`):**
- **`table/block.baga`** — record layout (`block_rec_at` / `block_rec_put` /
  `block_rec_size` / `block_last_key`); shared by `sst_build` and parse.
- **`sst_scan_begin` / `sst_scan_next`** — iterate one SST by restart block
  through the page cache (one block resident at a time; drop previous).
- **`sst_fold_into`** — compact merge path streams rows into acc/tomb maps
  without materializing the whole SST file. v4/v5 only; older formats fall
  back to `sst_load`.
- Tests: `tests/sst_scan_test.baga`; `lsm_test` / `lsm_recover_test` green.

**Shipped (R21 flush ROWS + batch flush_at):**
- Profile (`bench/rocks/run_profile.sh flush`): single `FLUSH_FORCE` n=2000
  was **~56% ROWS** (map_keys + sort + SstRow). Root cause: `sort_strs` was
  full-range **insertion sort O(n²)**.
- `sort_strs` → quicksort + insertion for slices under 16 (`util/codec.baga`).
- `lsm_flush` / `lsm_merge_indices`: sort `map_keys` in place (drop mem-key
  copy); pure-tomb test via `map_has(mem)==0`.
- `flush_at` sweep (n=2000, no per-put fsync): 64→6.6k, 256→23k, 512→37k,
  2000→67k ops/s. Batch head-to-head now uses `flush_at=N` (one memtable
  window + `flush_force`), not 31×64-key flushes.
- Probe: `./bench/rocks/run_profile.sh flush`.

**Shipped (R22 WAL/CRC put path):**
- MEM put profile (n=5000): ~90% of time was `wal_record`/`crc32c_b`.
  `crc32c_table()` rebuilt every call — for 120 B WAL records, soft CRC
  is ~4× faster; table path kept for ≥4 KiB (SST bodies).
- `wal_record`: one buffer (payload at off=4 + CRC), no dual alloc.
- `lsm_put_b`: skip `map_del` when tomb is empty.
- Batch head-to-head: n=1000 ~50%→~58%; n=5000 PUT **above** RocksDB in
  this harness (memtable window + soft CRC). Durable still fsync-bound ~93%.

**Shipped (R23 GET path):**
- Profile: `LOOKUP_META` ~90% of GET; within it `pc_read_at` copy +
  `block_rec_at`/`str_of_bytes` per record.
- `block_find`: compare keys in-place (no miss alloc); ~4× faster scan.
- `LsmDB.hot_*`: last restart-block span for get only (not scan/compact —
  avoids double-free with R18 drops). Sequential GET reuses the block.
- Defer `time_now_ms` until resolve; TTL magic byte-compare.
- n=2000: GET ~213k → ~681k seq / ~373k rnd. vs RocksDB n=5000 batch:
  GET_SEQ ~parity, GET_RND ~76%.

**Shipped (R24 random GET):**
- `pc_read_at` cost scaled with span size (byte-loop). Single-page spans now
  use `bytes_slice` (runtime memcpy).
- Restart interval 16→8 (~944 B blocks vs ~1888) for new SSTs; halves copy
  work on cold random hits. LsmDB hot span still covers sequential.
- GET_RND n=2000 ~373k→~552k; vs RocksDB roughly parity on this harness.

**Shipped (R25 page cache scale):**
- Default buffer pool **8 MiB** (2048×4 KiB). At n=20k random GET the old
  1 MiB pool thrashed (ratio ~27% RocksDB); with 8 MiB ~parity+.
- Empty mem/tomb short-circuit on get; serve `LSM_FLUSH_AT` default 256;
  `LSM_CACHE_PAGES` override.

**Shipped (R26 manifest + KEYS + dump):**
- `db/manifest.baga` owns flat MANIFEST parse/write; engine/compact call it.
- `lsm_keys` / RESP KEYS use `sst_fold_into` (block scan), not full SST load.
- Offline tool: `app-product/rocksbaga/tools/sst_dump.baga`
  (`LSMPATH`, optional `SST_GEN`, `SST_KEYS=1`, `SST_MAX`).

**Shipped (R27 block CRC on scan path):**
- Meta loads `bcrcs` (tag=5). Scan verifies each restart block before use;
  mismatch → scan closed. Fold/compact never full-loads after v4/v5 CRC fail.
- Get path intentionally skips per-block CRC (throughput); full body CRC still
  on legacy full-load.

**Shipped (R28–R30 RESP serve):**
- Harness: `./bench/rocks/run_vs_redis.sh` (`BENCH_PIPE`, default 64).
- R29 parse/dispatch; R30 NODELAY + 64 KiB reads + client pipeline.
- vs **Redis 8.x** (AOF off), n=5000, `LSM_SYNC_EVERY=10000`:

  | mode | PING baga/Redis | SET baga/Redis | GET baga/Redis |
  |------|----------------:|--------------:|--------------:|
  | pipe=1 | 26k / 44k (58%) | 25k / 41k (61%) | 26k / 40k (64%) |
  | pipe=64 | 157k / 574k (27%) | 108k / 326k (33%) | 114k / 315k (36%) |

- Pipeline multiplies absolute ops (~5×); Redis gains more (ratio falls).
  Artifact: `bench/rocks/results/vs-redis-*.txt`.

**Shipped (R31 scored pick + DBSIZE):**
- Merge pick with `merge_pick>0` selects k largest SSTs (older wins ties),
  then folds oldest-first. Reduces rewriting tiny files when sizes diverge.
- `DBSIZE` uses `lsm_dbsize` (live fold, no sort) vs `KEYS` (sorted list).

**Shipped (R32–R33 multi-shard + workers):**
- R32 `LsmCluster` + RESP (single-thread poll, all shards).
- R33 `db/workers.baga`: one exclusive writer thread per shard; jobs via
  chan(i64). API for parallel SET/GET/DEL.
- Env for workers test/serve: `LSMPATH`, `LSM_SHARDS`, `LSM_SYNC_EVERY`, …

**Shipped (R34 RESP → workers):**
- `LSM_PARALLEL=1`: network thread parses RESP, submits jobs, drains `done`
  with ordered per-fd replies. Supported: PING, SET, GET, DEL, SETEX, SAVE.

**Shipped (R35 in-memory mailbox):**
- Job req/rep packed as cell2 trees on channels (no `dir.jobs.*` files).
- Poll timeout 0 while jobs outstanding → single-client ~13k ops/s vs
  ~20k single-thread (was ~1k with disk hop).

**Shipped (R36 multi-conn soak):**
- `BENCH_CLIENTS` / `--clients` aggregate throughput. At 8 clients pipe=1,
  `LSM_PARALLEL=1` reaches ~86–90% of single-thread SET (not faster).

**Shipped (R37 cheaper hop):**
- Reply kind codes; short-string pack; lock-free job ids; skip unused
  val/extra unpack. ~0–10% on P1 soaks; ceiling still single-thread poll.

**Shipped (R38 reply coalesce):**
- Concat ready RESP replies → one `tcp_write_bytes` (writev-like without
  kernel writev). P0 pipeline ~2× (c4×pipe16 SET ~104k → ~207k).
- P1 gains less (hop still dominates).

**Shipped (R39 atomic MANIFEST):**
- Publish via `.manifest.tmp` + fsync + rename. Crash during rewrite no
  longer truncates the live MANIFEST in place.

**Shipped (R40 version edit log):**
- `.manifest.log` records A/D/N; open replays onto snapshot. Flush/compact
  append instead of full rewrite; compact@32 edits or close.

**Shipped (R41 multi-DB / SELECT):**
- Redis-like logical DBs via `SELECT` (0..15). Each index is a separate
  on-disk `LsmCluster` (`dir` / `dir.db{n}`). Not RocksDB CF (no shared WAL).
- Parallel workers: SELECT unsupported (single LSMPATH).

**Shipped (R42 FLUSHDB / FLUSHALL):**
- Wipe durable files + reopen empty store. FLUSHDB = selected DB; FLUSHALL
  walks all logical DB indices.

**Shipped (R43 SCAN):**
- Cursor paging over sorted live keys + MATCH/COUNT. Still full live-set fold
  per call (honest; not a streaming iterator yet).

**Shipped (R44 shared-WAL CF):**
- One WAL, many CF memtables/SST trees. Engine API + recover.
  No per-CF block-cache policy.

**Shipped (R45 MT serve):**
- `LSM_SERVE_MT=1`: OS thread per connection, shared exclusive shard workers.
  Real multi-core for multi-conn write/read. Not a free lunch on single-conn
  (same hop tax as PARALLEL).

**Shipped (R46 durable CF names + RESP):**
- `.cfs` name map (atomic). `LSM_CF=1` serve: `CF.CREATE/SET/GET/DEL/LIST`.

**Shipped (R47 MT reply routing + soak):**
- **Bug:** all MT conn threads shared one `done` chan; `lsm_mt_wait` stashed
  other threads' replies in *thread-local* maps — a stolen reply was lost
  and the robbed thread blocked forever. 8-client soak hung in SET phase.
- **Fix:** job carries its own reply chan (`lsm_mb_job` rc field); each conn
  thread waits on a private chan. Worker ctx done chan now unused legacy.
- **Soak after fix** (8 clients, n=5000/client, `LSM_SERVE_MT=1 LSM_SHARDS=4`
  vs p0 poll vs p1 workers, artifacts `vs-redis-20260806T11*.txt`):

  | pipe | mode | PING | SET | GET |
  |------|------|-----:|----:|----:|
  | 1  | p0 | 43.0k | 28.7k | 36.7k |
  | 1  | p1 | 42.7k | 29.0k | 29.2k |
  | 1  | mt | 41.8k | 30.5k | 32.1k |
  | 16 | p0 | 134k | 60k | 55k |
  | 16 | p1 | 136k | 64k | 77k |
  | 16 | mt | **322k** | **77k** | 67k |

  Read: pipe=1 is RTT-bound (all equal); pipe=16 MT clearly wins PING
  (2.4×) and SET (1.3×). GET varies run to run (55–86k p0) — noisy.

**Shipped (R48 CF compaction + WAL rotation):**
- `lsm_cf_flush` now runs the L0..L3 compaction chain per family (SSTs no
  longer pile up per CF forever).
- Shared WAL rotates (close/unlink/reopen, reset offsets) once every family
  is clean. Before: WAL grew forever; every reopen replayed already-flushed
  records back into memtables (unbounded recovery + duplicate flushes).

**Shipped (R49 CF.DROP):**
- `lsm_cf_drop(db, name)` + RESP `CF.DROP name` → `:1`/`:0` (default CF
  refused). Discards memtable, closes SST fd caches, unlinks
  `.sst.*`/`.bloom.*`/`.manifest*` for live gens, persists `.cfs`.
- WAL replay skips records whose CF no longer holds a name
  (`lsm_cf_id_known`) — dropped families stay dropped after crash/reopen.
- Tests: `r48_*` / `r49_*` in `tests/cf_test.baga`.

**Shipped (R50 MT batch hop):**
- MT conn thread parses + submits the whole pipelined batch, then waits for
  replies in request order (kinds/refs/imm vecs). No throughput change —
  the per-command wait was not the wall.

**Shipped (R51 zero-copy hop):**
- New builtins `str_h(str) -> i64` / `h_str(i64) -> str` (C backend; LLVM
  parity TODO). Safe because `str` is arena-bound (never freed) — a handle
  passed cross-thread stays valid for process life.
- Job/reply payloads in `db/workers.baga` are raw handles now; the R35/R37
  byte-loop pack/unpack is legacy (kept, unused). SET 84k→96k — useful but
  not the wall either.

**Shipped (R52 thread-local arena — the actual GIL):**
- Micro-bench decomposition (n=20000, 100 B vals): pure engine 540k ops/s
  (1.9µs), single-submitter hop 77.5k (12.9µs). Aggregate MT never passed
  ~96k regardless of thread count → serialization, not latency.
- Root cause: ONE global `baga_alloc_mu` around every `baga_alloc` /
  `baga_free` — every str/vec/bytes op in every thread serialized.
- Fix: arena + both free-list tiers are `__thread`. Safe: str is
  arena-bound; free-list blocks are interchangeable raw memory (a
  cross-thread free just lands in the freeing thread's list).
- MT soak vs Redis 8.x (8 clients, n=5000/client, pipe=16, shards=4):

  | metric | before R52 | after R52 | Redis | ratio |
  |--------|-----------:|----------:|------:|------:|
  | PING | 348k | 348k | 349–351k | **97–99%** |
  | SET | 84k | **158–175k** | 249–251k | **62–70%** |
  | GET | 68k | **189k** | 222k | **84–85%** |

  Artifacts: `vs-redis-20260806T113411Z.txt`, `…113426Z.txt`.

**Bench harness fix (same day):** orphaned `/tmp/baga_*` server zombies
(driver killed, child survived) held random bench ports; one "failed" run
had actually benched a stale server. `run_vs_redis.sh` now starts the
server with `setsid` + kills the process group, and re-rolls the port if
it already answers.

**Shipped (R53 worker drain):** `lsm_worker_one` + `chan_try_recv` drain
(≤64 jobs per wakeup). Neutral — R50 batching already kept queues full.

**Shipped (R54 reply assembly, no O(n²) concat):**
- GET collapsed at pipe=64 (114k vs Redis 329k): replies were appended with
  `bytes_concat` per command → O(batch²) copies (~230 KB per 64-batch).
- New builtin `bytes_put(dst, off, src)` (in-place memcpy); MT conn keeps a
  persistent per-conn scratch buffer, doubling on overflow (old buffer
  abandoned — checker forbids drop of loop-outer locals).
- After: pipe=64 GET **289k (87%)**, PING 615k (99%), SET 223k (57%);
  pipe=16 shards=8 **PING 99% / SET 85% / GET 86%**.
  Artifacts: `vs-redis-20260806T114833Z.txt`, `…114851Z.txt`.

**Shipped (R55 hop-less MT — parity with Redis):**
- The remaining ~15% was structural: every command hopped conn thread →
  chan → worker → chan → conn. Removed the hop entirely.
- New builtins `map_h(map) -> i64` / `h_map(i64) -> map` (C backend; LLVM
  parity TODO) — a shared `Map<i64, LsmDB>` passes through the go_bg ctx.
- Design: shard dbs are boxed map entries (`baga_map_set_box` memcpys into
  a stable per-entry box). All keys inserted before the first `go_bg`
  (no rehash afterwards); each shard's entry is read/copied-back only
  under its own mutex. Conn thread: `mutex_lock` → copy db out →
  `lsm_put/get/del/put_ex` inline → store back → `mutex_unlock`.
- No workers, no chans, no job packing on the MT command path; the R50
  kinds/refs/imm machinery collapsed (replies are immediate again).
- Safety note: concurrent `map_get_box` on *different* entries is pure
  reads of stable buckets; same-entry access is mutex-serialized.
- vs Redis 8.x (8 clients, n=5000, shards=8):

  | pipe | PING | SET | GET |
  |------|-----:|----:|----:|
  | 16 | 350k (**104%**) | 234k (**93%**) | 220k (**98%**) |
  | 64 | 613k (98%) | 363k (94%) | 326k (**99%**) |

  Artifacts: `vs-redis-20260806T120051Z.txt`, `…120104Z.txt`.
- Smoke: SET/GET/DEL/SETEX + kill/restart recovery — values survive.

**Shipped (R56 MT command parity):**
- `lsm_mt_exec`: MGET/MSET, INCR/DECR, APPEND, TYPE, EXISTS,
  EXPIRE/TTL/PERSIST, DBSIZE, SAVE/BGSAVE — inline under shard mutexes
  (multi-key commands lock one shard at a time). Wire-tested with
  redis-cli. Perf unchanged: 97/94/98% (pipe=16, shards=8).

**Shipped (R57 active expire, MT mode):**
- Redis-style active cycle: shared expires index `"sid:key" -> deadline`
  (own mutex `emu`) + a 100 ms sweeper thread (`lsm_mt_sweeper`).
- Update points: SETEX/EXPIRE record; SET/MSET/INCR/DECR/APPEND/PERSIST/
  DEL clear (plain SET clears TTL — Redis semantics).
- Sweeper claims due entries, then tombstones only keys whose value is
  still expired (re-SET without TTL in the meantime is skipped).
- Verified: dbsize 3 → 1 three seconds after SETEX 1 with no reads.
- Lazy expiry (R16) stays the correctness net; the sweeper frees space
  and keeps DBSIZE honest without reads.

**Shipped (R58 checkpoint):**
- `lsm_checkpoint(db, dest)` — flush memtable, copy every live SST +
  bloom sidecar (1 MiB binary chunks), write a fresh MANIFEST at `dest`.
  RocksDB Checkpoint analogue: the copy opens with plain `lsm_open(dest)`.
  Source store stays online. Tests: `r58_*` in `tests/lsm_test.baga`
  (copy matches source incl. deletes; source usable after).

**Shipped (R59 poll command parity):**
- Cluster exec (poll path) gained MGET/MSET/DECR/APPEND/TYPE — same
  surface as MT mode now. Wire-tested with redis-cli.

**Still open (later):** richer RocksDB CF options; CHECKPOINT over RESP; per-CF block-cache policy; LLVM backend parity for R51/R52/R54/R55 (handle builtins, thread-local arena, bytes_put).

## L4 — TTL / RESP binary wire / concurrent writers

**Shipped (R3 engine):** memtable `Map<str, bytes>`, WAL/SST values as bytes,
`lsm_put_b` for NUL-safe puts. Compaction merges oldest `compact_at` gens and
**drops pure tombstones**.

**Shipped (R15):** poll multi-conn RESP serve (`std/net/poll`) — many clients
share one `LsmDB` on a single thread. Env `LSM_SERIAL=1` keeps the old
one-conn-at-a-time accept loop. Test: `tcp2_*` in `tests/lsm_test.baga`.

**Still residual:** RESP **command** args are still `Vec<str>` (SET cannot
inject raw NUL over the wire parser); GET uses `resp_bulk_b`. TTL via
`SETEX`/`EXPIRE` is R16. No multi-writer (shared store is single-threaded
event loop).

## Closed by this package

- Binary file IO with embedded NUL — `fd_*_bytes` (std/os).
- `mkdir` / `unlink` / `rename` — std/os (queuebaga Q4 path open).
- Durable put across reopen — WAL + MANIFEST + SST.
