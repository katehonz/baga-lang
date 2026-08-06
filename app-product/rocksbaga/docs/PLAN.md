# rocksbaga — plan (was lsmbaga)

Date: 2026-08-05
Status: **P0 MVP done** (S5+S6)
Goal: Track S flagship — durable KV on RESP.

## P0 ✅

1. std/os: mkdir/unlink/rename + binary fd_*_bytes
2. Page cache (clock)
3. WAL + crc32c records
4. Memtable + SST flush + MANIFEST
5. Compaction-lite
6. Recovery reopen
7. RESP subset + SAVE flush
8. `lsm_test` + demo

## P1

- Sparse index / restart keys for SST — **done (R2, BAGASST2)**
- `Map<str, bytes>` values — **done (R3)**; RESP command path still str
- Oldest-N compaction + drop pure tombs — **done (R3)**
- poll-based multi-conn serve (kvbaga K1) — **done (R15)**
- Optional TTL column

## P2

- Bloom filter in SST — **done (R4)**
- **BAGASST4** footer + partial get — **done (R5)**
- L0/L1 levels — **done (R5)**
- **BAGASST5** per-block CRC + **L2** — **done (R6)**
- Byte-size level targets + **L3** — **done (R7)** (`target_bytes`)
- Oldest-N merge pick — **done (R8)** (`merge_pick`)
- Standalone bloom filter file — **done (R9)** (`BAGABLM1` sidecar)
- Package rename **rocksbaga** — **done (R10)**; `lsmbaga` shim kept
- Layered package tree (`util`/`cache`/`wal`/`table`/`db`/`net`) — **done**
- Split `table/bloom` + `db/{types,compact}` — **done**
- **R11 get path** — bloom cache, SST fd cache, O(n) `pc_read_at`,
  skip embedded bloom when sidecar hit, skip block CRC on get,
  default page cache 256 pages — **done**
- **R12 SST meta cache** — footer + restart offs + min/max keys per gen — **done**
- **R13 restart keys in meta** — in-memory bsearch, one block read per get — **done**
- **R14 put path** — prealloc WAL/bloom/SST encode, `push_u32_le` 1-concat,
  single bloom build, 64 KiB WAL write buffer — **done**
- **R15 poll multi-conn** — `std/net/poll` event loop for RESP serve — **done**
- **R16 TTL** — `BAGATTL1` value envelope, lazy expiry, RESP EXPIRE/TTL/SETEX/
  PERSIST — **done** (P1 "Optional TTL column" closed)
- **R17 sst_build prealloc** — killed the O(n²) concat chain (OOM at n≥5000
  bench) — **done**
- **R18 server memory** — scratch-buffer reads, drop discipline, offset WAL
  append, free list >1 KiB (runtime); soak 34→1.3 KB/cmd (PING) — **done**
- **R19 shared page cache + pins** — per-file fd registry for multi-file
  writeback; pin counts block eviction; `pc_get_pin` / `pc_read_at` pin
  mid-span; `sst_fd_get` registers on open — **done**
- **R20 block scan + `table/block.baga`** — record encode/decode module;
  restart-block iterator (`sst_scan_*`); compact fold streams via page
  cache (no full-file copy for v4/v5); legacy full-load fallback — **done**
- **R21 flush ROWS** — `sort_strs` quicksort (was O(n²) insertion); flush/
  compact build rows from `map_keys` in place (no mem-key copy); batch bench
  `flush_at=N` + end `flush_force` — **done**
- **R22 WAL/CRC put path** — soft crc32c under 4 KiB; single-buffer
  `wal_record`; skip empty-tomb `map_del` — **done**
- **R23 GET path** — `block_find` no-alloc key scan; LsmDB hot restart-block
  span; defer `time_now` / cheaper TTL magic — **done**
- **R24 random GET** — single-page `pc_read_at` memcpy path; restart every 8
  — **done**
- **R25 page cache scale** — default 8 MiB cache; empty mem/tomb skip; serve
  flush_at=256; `LSM_CACHE_PAGES` — **done**
- **R26 manifest + KEYS scan + sst_dump** — `db/manifest.baga`; KEYS via
  `sst_fold_into`; `tools/sst_dump.baga` — **done**
- **R27 block CRC** — verify BAGASST5 per-block crc on scan/fold; get path
  skips (perf); no full-load fallback after CRC fail — **done**
- **R28 RESP bench** — `tools/serve.baga`, `LSM_SYNC_EVERY`, RESP harness
  `bench/rocks/run_vs_redis.sh` — **done**
- **R29 RESP parse** — digit framing parse; `lsm_eq_ci` dispatch; buffer
  drop on full consume — **done**
- **R30 RESP pipeline** — NODELAY + 64 KiB reads; `resp_bulk_b` prealloc;
  `BENCH_PIPE` client pipeline — **done**
- **R31 scored compact + DBSIZE** — size-score merge pick; `lsm_dbsize`
  without key sort — **done**
- **R32 multi-shard** — key-hash `LsmCluster` (`LSM_SHARDS`); RESP on cluster;
  N=1 path-compat — **done**
- **R33 per-shard workers** — exclusive shard workers + job channels
  (`db/workers.baga`) — **done**
- **R34 RESP → workers** — `LSM_PARALLEL=1` poll hop + ordered replies —
  **done**
- **R35 in-memory job mailbox** — cell2-packed payloads on chans; poll
  spin when jobs outstanding — **done**
- **R36 multi-conn soak** — `BENCH_CLIENTS` harness; P1→90% of P0 at 8
  clients (honest: not yet faster) — **done**
- **R37 cheaper hop** — reply codes, short pack, lock-free id — **done**
  (modest; P1 still ≤ P0)
- **R38 reply coalesce** — one write per read batch / dirty fd — **done**
  (~2× P0 pipeline SET)
- **R39 atomic MANIFEST** — tmp + fsync + rename publish — **done**
- **R40 version edit log** — `manifest.log` A/D/N; compact@32 + close — **done**
- **R41 multi-DB SELECT** — `dir.db{n}` namespaces; RESP SELECT — **done**
- **R42 FLUSHDB/FLUSHALL** — wipe path + reopen; multi-DB aware — **done**
- **R43 SCAN** — cursor + MATCH/COUNT; cluster page — **done**
- **R44 shared-WAL CF** — `db/cf.baga`, WAL op 17/18 — **done**
- **R45 MT serve** — `LSM_SERVE_MT=1` go_bg conn → shard workers — **done**
- **R46 durable CF names + RESP CF.*** — `.cfs` map; CF.SET/GET/DEL — **done**
- (next) MT multi-conn soak; CF polish
