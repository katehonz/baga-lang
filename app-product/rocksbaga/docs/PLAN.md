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
- Shared page cache across open files with pin counts
