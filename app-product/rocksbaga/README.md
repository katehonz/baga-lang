# rocksbaga

**Durable LSM-style KV** for Baga — storage flagship on the **RocksDB-class**
path. Layered package (not a single flat folder).

Formerly **`lsmbaga`** (R0–R9). Symbols stay `lsm_*`. **Not** RocksDB
feature parity — see [docs/gaps.md](docs/gaps.md).

**Architecture:** [ARCHITECTURE.md](ARCHITECTURE.md)

## Layout

```
util/           codec (LE, sort)
cache/          page cache
wal/            write-ahead log
table/bloom     filter + BAGABLM1 sidecar
table/sstable   SST format + partial get
db/types        LsmDB struct
db/compact      merge / pick / promote
db/engine       open, put/get, flush, recovery
net/            RESP2 server
examples/       demo
docs/           PLAN, gaps
*.baga          root re-exports (stable import paths)
```

## What works

| Piece | Layer | Notes |
|-------|-------|--------|
| Page cache | `cache/` | 4 KiB pages, clock eviction (default 256); R19 multi-file fd registry + pins |
| WAL | `wal/` | crc32c records, fdatasync |
| Memtable + compact | `db/` | L0…L3, byte targets, oldest-N pick |
| SSTable | `table/` | BAGASST5 + `.bloom.<gen>` sidecar; R20 block scan compact |
| RESP | `net/` | redis-cli subset; **poll multi-conn** (R15) |

**Bench:** `./bench/rocks/run_vs_rocksdb.sh` — pure engine vs RocksDB.  
Scorecard (n=5000): durable PUT **92%** / GET ~parity; batch PUT **137%** —
see [`bench/rocks/results/vs-rocksdb-scorecard.md`](../../bench/rocks/results/vs-rocksdb-scorecard.md).

**Dump:**  
`LSMPATH=/tmp/baga_lsm ./baga -I . -I app-product app-product/rocksbaga/tools/sst_dump.baga`  
(`SST_GEN`, `SST_KEYS=1`, `SST_MAX` optional).

**Parallel engine (R33/R35):**  
`import "rocksbaga/db/workers.baga"` — `lsm_parallel_start(dir, N)` then
`lsm_parallel_set/get`. Jobs are in-memory (cell2 packs on chans). Requires
`LSMPATH`/`LSM_SHARDS` in the environment for worker children.

## Run

```bash
LSMPATH=/tmp/baga_rocks_demo LSMPORT=16579 \
  ./baga -I . -I app-product app-product/rocksbaga/examples/demo.baga

./baga -I . -I app-product tests/lsm_test.baga
./baga -I . -I app-product tests/lsm_recover_test.baga
./baga -I . -I app-product tests/page_cache_test.baga
./baga -I . -I app-product tests/sst_scan_test.baga
```

Env: `LSMPATH`, `LSMPORT`, `LSM_SHARDS` (default 1; key-hash partition),
`LSM_PARALLEL=1` (per-shard workers behind RESP poll; default 0),
`LSM_SERVE_MT=1` (go_bg per connection → same workers; multi-core multi-conn),
`LSM_CF=1` (shared-WAL CF mode: `CF.CREATE/SET/GET/DEL/DROP/LIST`; plain SET = default CF),
`LSM_MAX_DB` (default 16; Redis-style `SELECT` 0..N-1 → `dir` / `dir.db{n}`),
`LSM_FLUSH_AT` (default 256), `LSM_COMPACT_AT` (default 4),
`LSM_CACHE_PAGES` (default 2048 ≈ 8 MiB, split across shards),
`LSM_SYNC_EVERY` (default 1 = fsync each put; raise for bulk),
`LSM_TARGET_BYTES`, `LSM_MERGE_PICK`, `LSM_SERIAL=1` (legacy single-conn).

**RESP serve / bench:**
```bash
LSMPATH=/tmp/baga_lsm LSMPORT=16579 ./baga -I . -I app-product \
  app-product/rocksbaga/tools/serve.baga
./bench/rocks/run_vs_redis.sh          # baga + Redis if installed
```

## API

```baga
// preferred (layered)
import "rocksbaga/db/engine.baga"
import "rocksbaga/net/server.baga"

// stable short paths (root re-export)
import "rocksbaga/engine.baga"
import "rocksbaga/server.baga"

fn lsm_open(dir, flush_at, compact_at) -> LsmDB !IO
fn lsm_put / lsm_put_b / lsm_del / lsm_get / lsm_flush_force / lsm_close
fn lsm_checkpoint(db, dest) -> LsmDB !IO !Time   // R58: point-in-time copy
fn lsm_serve(port) -> i64 !Net !IO !Time !Par
```

## Compat

- `import "rocksbaga/engine.baga"` — root shim → `db/engine.baga`
- `import "lsmbaga/…"` — deprecated package shims → rocksbaga root

## On-disk

```
<dir>.wal | .manifest | .sst.<gen> | .bloom.<gen>
```

## Honest limits

Serial accept by default; `LSM_SERVE_MT=1` (R55 hop-less) reaches Redis
parity on this harness (8 clients: 93–104% PING/SET/GET).
Compaction not RocksDB-scored.
TTL is lazy (expiry on read/flush/compact); no active expire cycle.
Details: [docs/gaps.md](docs/gaps.md), [docs/PLAN.md](docs/PLAN.md).
