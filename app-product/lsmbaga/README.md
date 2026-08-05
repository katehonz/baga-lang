# lsmbaga

**Durable LSM-style KV** for Baga — storage flagship and the first step on the
**RocksDB-class endgame** (educational language → real ecosystem → engine).
Write path is WAL (crc32c) → memtable; flush builds sorted SSTables;
compaction-lite merges tables when there are too many. Reads walk memtable
then newest SST first (binary search per table), through a **clock page
cache**. The wire protocol is **RESP2** (same framing as [kvbaga](../kvbaga)),
so `redis-cli` works for the supported command set.

Foundation used: `bytes` mutators (S2), `pread`/`pwrite`/`fsync` (S3),
`crc32c` (S4), `drop`/arena free list (MEM-1/2), plus `mkdir`/`unlink`/`rename`
and binary `fd_*_bytes` in `std/os`.

## What works

| Piece | Notes |
|-------|--------|
| Page cache (S5) | Fixed-size pages (4 KiB), clock eviction, dirty writeback, invalidate-on-unlink |
| WAL | Length-prefixed records, crc32c, `fdatasync` every `sync_every` (default 1) |
| Memtable | `Map<str,str>` + tombstone map |
| SSTable | Magic **`BAGASST2`** (writes): sorted rows + **restart index** (every 16) + crc; get = index bsearch + block scan. **`BAGASST1`** still readable (R1 path) |
| Flush | Threshold `flush_at` (default 32); `SAVE` forces flush over RESP |
| Compaction-lite | When `gens >= compact_at` (default 3) → one merged SST |
| Recovery | MANIFEST + SST gens + WAL replay |
| RESP | `PING` `SET` `GET` `DEL` `EXISTS` `INCR` `KEYS` `DBSIZE` `SAVE` `QUIT` |

## On-disk layout (path **prefix**)

```
<dir>.wal
<dir>.manifest      # next_gen\n then gens oldest-first
<dir>.sst.<gen>
```

Example: `LSMPATH=/tmp/baga_lsm` → `/tmp/baga_lsm.wal`, `/tmp/baga_lsm.sst.1`, …

## Run

```bash
# demo (worker on :16579, LSMPATH default /tmp/baga_lsm)
LSMPATH=/tmp/baga_lsm_demo LSMPORT=16579 ./baga app-product/lsmbaga/demo.baga

# tests
./baga tests/lsm_test.baga
```

With redis-cli (server must be running):

```bash
LSMPATH=/tmp/baga_lsm LSMPORT=6379 ./baga -c '…'   # or go_bg serve in demo
redis-cli -p 6379 SET foo bar
redis-cli -p 6379 GET foo
redis-cli -p 6379 SAVE
```

Env: `LSMPATH`, `LSMPORT` / port arg, `LSM_FLUSH_AT`, `LSM_COMPACT_AT`.

## API (engine)

```baga
fn lsm_open(dir, flush_at, compact_at) -> LsmDB !IO
fn lsm_put / lsm_del / lsm_get / lsm_keys / lsm_flush_force / lsm_close
fn lsm_serve(port) -> i64 !Net !IO !Time !Par
```

## Honest limits

- Serial connections (same `go`/store constraint as kvbaga K1).
- NUL-free `str` keys/values (kvbaga K2 product residual).
- No TTL/EXPIRE; SET options rejected with ERR.
- Get still loads the full SST file (index avoids full *parse*, not full IO); no bloom / page-sized blocks yet.
- Compaction is “merge everything”, not leveled.
- Single process; no multi-writer.

See [gaps.md](gaps.md).
