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
| Page cache | `cache/` | 4 KiB pages, clock eviction (default 256 pages) |
| WAL | `wal/` | crc32c records, fdatasync |
| Memtable + compact | `db/` | L0…L3, byte targets, oldest-N pick |
| SSTable | `table/` | BAGASST5 + `.bloom.<gen>` sidecar; R11 bloom/fd cache |
| RESP | `net/` | redis-cli subset; **poll multi-conn** (R15) |

**Bench:** `./bench/rocks/run_vs_rocksdb.sh` — pure engine vs RocksDB.

## Run

```bash
LSMPATH=/tmp/baga_rocks_demo LSMPORT=16579 \
  ./baga -I . -I app-product app-product/rocksbaga/examples/demo.baga

./baga -I . -I app-product tests/lsm_test.baga
./baga -I . -I app-product tests/lsm_recover_test.baga
```

Env: `LSMPATH`, `LSMPORT`, `LSM_FLUSH_AT`, `LSM_COMPACT_AT`,
`LSM_TARGET_BYTES`, `LSM_MERGE_PICK`, `LSM_SERIAL=1` (legacy single-conn).

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

Serial accept, no multi-writer, compaction not RocksDB-scored.
TTL is lazy (expiry on read/flush/compact); no active expire cycle.
Details: [docs/gaps.md](docs/gaps.md), [docs/PLAN.md](docs/PLAN.md).
