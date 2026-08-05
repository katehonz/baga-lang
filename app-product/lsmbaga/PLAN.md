# lsmbaga — plan

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
- poll-based multi-conn serve (kvbaga K1)
- Optional TTL column

## P2

- Bloom filter in SST — **done (R4)**
- **BAGASST4** footer + partial get — **done (R5)**
- L0/L1 levels — **done (R5)**
- **BAGASST5** per-block CRC + **L2** — **done (R6)**
- Byte-size level targets / L3+
- Shared page cache across open files with pin counts
- Optional skip whole-body CRC on get-only path
