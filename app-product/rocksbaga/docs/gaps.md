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

**Still open (later):** pin-count shared page cache; poll multi-conn;
`db/manifest.baga` / `table/block.baga` when needed; RocksDB feature parity
(not claimed).

## L4 — TTL / RESP binary wire / concurrent writers

**Shipped (R3 engine):** memtable `Map<str, bytes>`, WAL/SST values as bytes,
`lsm_put_b` for NUL-safe puts. Compaction merges oldest `compact_at` gens and
**drops pure tombstones**.

**Shipped (R15):** poll multi-conn RESP serve (`std/net/poll`) — many clients
share one `LsmDB` on a single thread. Env `LSM_SERIAL=1` keeps the old
one-conn-at-a-time accept loop. Test: `tcp2_*` in `tests/lsm_test.baga`.

**Still residual:** RESP **command** args are still `Vec<str>` (SET cannot
inject raw NUL over the wire parser); GET uses `resp_bulk_b`. No EXPIRE/TTL;
no multi-writer (shared store is single-threaded event loop).

## Closed by this package

- Binary file IO with embedded NUL — `fd_*_bytes` (std/os).
- `mkdir` / `unlink` / `rename` — std/os (queuebaga Q4 path open).
- Durable put across reopen — WAL + MANIFEST + SST.
