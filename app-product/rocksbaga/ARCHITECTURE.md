# rocksbaga — architecture

**Date:** 2026-08-05  
**Status:** layered package (R10+)  
**Goal:** a maintainable **RocksDB-class** embedded KV path in Baga — not a
flat demo folder.

## Why layers

A single directory of 2.5k+ lines (engine + sstable + page + wal + RESP)
does not scale for:

- SST format evolution (v1…v5, bloom sidecar, block CRC)
- compaction policy (file-count, byte targets, oldest-N pick)
- future: memtable variants, multi-file iterators, tools

Layers mirror a small storage engine, with **one concern per package**.

## Package tree

```
rocksbaga/
├── ARCHITECTURE.md          ← this file
├── README.md
├── sandak.toml
├── engine.baga              ← public re-export → db/engine.baga
├── server.baga              ← public re-export → net/server.baga
├── page.baga / wal.baga / sstable.baga / codec.baga  ← re-exports
│
├── util/
│   └── codec.baga           # LE u32, string sort, shared binary helpers
├── cache/
│   └── page.baga            # clock page cache (S5)
├── wal/
│   └── wal.baga             # append-only WAL + crc32c replay
├── table/
│   ├── bloom.baga           # filter + BAGABLM1 sidecar (no SST types)
│   └── sstable.baga         # BAGASST* format, partial get, uses bloom
├── db/
│   ├── types.baga           # LsmDB struct
│   ├── compact.baga         # pick, merge, promote L0…L3, MANIFEST write
│   └── engine.baga          # open/put/get/flush/recovery (orchestrates)
├── net/
│   └── server.baga          # RESP2 front-end (redis-cli subset)
├── examples/
│   └── demo.baga            # runnable demo
└── docs/
    ├── PLAN.md
    └── gaps.md
```

## Dependency graph (allowed)

```
        net/server
             │
             ▼
          db/engine
           /   |   \
          ▼    ▼    ▼
   db/compact  wal  cache/page
        │        \    /
        ▼         \  /
   table/sstable   \/
        │          /
        ▼         /
   table/bloom   /
        \       /
         ▼     ▼
         util/codec
              │
              ▼
     std/{os,str,crypto,net,bytes}
```

`db/types` is imported by `engine` and `compact` (no cycles).

**Rules**

| Rule | Meaning |
|------|---------|
| No upward imports | `util` must not import `db` / `net` |
| `table` may use `cache` + `util` | SST IO through page cache |
| `db` orchestrates | only layer that owns `LsmDB` lifecycle |
| `net` is optional | library users import `db` only |
| Public re-exports at root | `import "rocksbaga/engine.baga"` stays stable |

## Import styles

```baga
// Preferred (explicit layer) — new code
import "rocksbaga/db/engine.baga"
import "rocksbaga/net/server.baga"
import "rocksbaga/table/sstable.baga"

// Stable short paths (root re-export)
import "rocksbaga/engine.baga"
import "rocksbaga/server.baga"

// Deprecated alias package
import "lsmbaga/engine.baga"   // → rocksbaga/engine.baga shim
```

Internal relative imports (inside package):

```baga
// db/engine.baga
import "../util/codec.baga"
import "../cache/page.baga"
import "../wal/wal.baga"
import "../table/sstable.baga"
```

## Layer responsibilities

### util
Endian helpers, key compare/sort. No IO.

### cache
`PageCache`: fixed page size, clock eviction, `pc_read_at` / invalidate.
**R19:** shared multi-file pool — composite keys `(file_id, page_no)`,
per-file fd registry for writeback, pin counts (`pc_pin` / `pc_get_pin`)
so eviction never drops a page a caller still holds.
Keyed by SST gen (file id).

### wal
Put/Del records, crc32c, replay into mem/tomb maps (called by `db`).

### table
- **bloom** — bitset + R9 sidecar files (key-list API, no `SstRow`).
- **sstable** — magic/version, restart index, embedded bloom in v5 body,
  partial block get, full parse for compact/KEYS.

### db
- **types** — `LsmDB` only.
- **compact** — pick/merge/promote, MANIFEST write on merge.
- **engine** — open/close, put/get/del, flush → L0, recovery; calls compact.

### net
RESP command loop; maps SET/GET/… to `db`. Env: `LSMPATH`, `LSM_*`.

## On-disk layout (unchanged by layers)

```
<prefix>.wal
<prefix>.manifest
<prefix>.sst.<gen>
<prefix>.bloom.<gen>
```

Layers own **code**, not a new disk format.

## Future splits (when files grow again)

| Split | When |
|-------|------|
| ~~`table/bloom.baga`~~ | **done** |
| ~~`db/compact.baga` + `types.baga`~~ | **done** |
| `db/manifest.baga` | version edit log beyond flat MANIFEST |
| `table/block.baga` | shared block builder for SST rebuild |
| `tools/` | offline SST dump / consistency check |

## Tests

| Test | Imports |
|------|---------|
| `tests/lsm_test.baga` | `rocksbaga/engine` + `server` (root re-export) |
| `tests/lsm_recover_test.baga` | `rocksbaga/engine` + `page` |
| Layer smoke | `import "rocksbaga/db/engine.baga"` |

## Non-goals of this layout

- Separate sandak packages per layer (one package, many dirs)
- Breaking rename of `lsm_*` symbols
- Claiming RocksDB module parity (ColumnFamily, VersionSet, …)

See [docs/gaps.md](docs/gaps.md) for engine honesty.
