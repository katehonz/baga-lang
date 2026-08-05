# rocksbaga

**Durable LSM-style KV** for Baga — storage flagship on the **RocksDB-class**
path (educational language → real ecosystem → engine).

Formerly **`lsmbaga`** (R0–R9). Same on-disk layout, same `lsm_*` API names
(engine symbols kept stable). **Not** RocksDB feature parity — see [gaps.md](gaps.md).

Write path: WAL (crc32c) → memtable → SSTable flush; compaction by file-count
and/or byte targets with optional oldest-N pick. Reads: memtable then newest
SST (binary search / partial blocks), clock page cache, **standalone bloom**
sidecars. Wire protocol **RESP2** ([kvbaga](../kvbaga)) so `redis-cli` works
for the supported command set.

## What works

| Piece | Notes |
|-------|--------|
| Page cache | Fixed-size pages (4 KiB), clock eviction, dirty writeback |
| WAL | Length-prefixed records, crc32c, `fdatasync` every `sync_every` (default 1) |
| Memtable | `Map<str, bytes>` + tombstone map (binary-safe values) |
| SSTable | **`BAGASST5`**: core + per-block crc + bloom + footer; **v1–v4** readable |
| Bloom sidecar | **R9** `<dir>.bloom.<gen>` (`BAGABLM1`) — early miss without SST open |
| Flush | `flush_at` → L0; `SAVE` forces flush |
| Compaction | L0→L1→L2→L3; `compact_at` / `target_bytes` / `merge_pick` (R7–R8) |
| Recovery | MANIFEST + SST gens + WAL replay |
| RESP | `PING` `SET` `GET` `DEL` `EXISTS` `INCR` `KEYS` `DBSIZE` `SAVE` `QUIT` |

## On-disk layout (path **prefix**)

```
<dir>.wal
<dir>.manifest      # next_gen\n then "gen level" lines (0=L0 … 3=L3)
<dir>.sst.<gen>
<dir>.bloom.<gen>   # R9 BAGABLM1
```

## Run

```bash
# demo
LSMPATH=/tmp/baga_rocks_demo LSMPORT=16579 ./baga app-product/rocksbaga/demo.baga

# tests (API still lsm_* symbol names)
./baga -I . -I app-product tests/lsm_test.baga
./baga -I . -I app-product tests/lsm_recover_test.baga
```

Env: `LSMPATH`, `LSMPORT`, `LSM_FLUSH_AT`, `LSM_COMPACT_AT`,
`LSM_TARGET_BYTES`, `LSM_MERGE_PICK`.

## API (engine)

```baga
import "rocksbaga/engine.baga"
fn lsm_open(dir, flush_at, compact_at) -> LsmDB !IO
// db.target_bytes, db.merge_pick
fn lsm_put / lsm_put_b / lsm_del / lsm_get / lsm_keys / lsm_flush_force / lsm_close
fn lsm_serve(port) -> i64 !Net !IO !Time !Par
```

## Compat

Import path **`rocksbaga/…`**. Old name `lsmbaga` is a redirect package
(`app-product/lsmbaga/`) — prefer `rocksbaga`.

## Honest limits

- Serial connections (kvbaga K1).
- Keys `str` (NUL-free); values `bytes`.
- No TTL/EXPIRE; no multi-writer; not RocksDB score-based compaction.
- Compaction / KEYS still full-load some paths.

See [gaps.md](gaps.md).
