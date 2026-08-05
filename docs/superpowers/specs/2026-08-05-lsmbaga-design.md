# lsmbaga MVP (S5+S6): page cache + WAL → memtable → SSTable + compaction-lite

Date: 2026-08-05. Status: **shipped** (MVP).
Parent: `docs/superpowers/plans/2026-08-05-cloud-storage-direction.md`
(Track S flagship; foundation S2–S4 + MEM-1/2 already shipped).

## Goal

Ship a **durable KV engine probe** in pure Baga:

```
RESP2 client (redis-cli) → lsmbaga server
                              │
                         put/del → WAL (crc32c) + memtable
                         get     → memtable → SST pages (page cache)
                         flush   → sorted SSTable + MANIFEST + WAL rotate
                         compact → merge-lite when ≥ N tables
```

Credibility probe for Track S: not a toy Map in RAM, but a real
write-ahead + flush + read-through path using the storage foundation.

## S5 — page cache (clock)

```baga
struct PageCache {
    page_size: i64,          // 4096
    cap: i64,                // slot count
    hand: i64,               // clock hand
    // parallel maps keyed by composite: file_id * 1_000_000_000 + page_no
    data: Map<i64, bytes>,   // page payload (exactly page_size, zero-padded)
    dirty: Map<i64, i64>,    // 1 if dirty
    refbit: Map<i64, i64>,   // clock reference bit
    keys: Vec<i64>           // resident keys for clock walk
}

fn pc_new(page_size, cap) -> PageCache
fn pc_get(pc, fd, file_id, page_no) -> bytes !IO   // load on miss; clock evict
fn pc_mark_dirty(pc, key)
fn pc_flush_file(pc, fd, file_id) -> i64 !IO       // write dirty pages for file
fn pc_invalidate_file(pc, file_id)                 // drop slots for a file
```

- Miss: `fd_pread_bytes` of `page_size` at `page_no * page_size`; short read
  zero-pads to full page (EOF / hole).
- Eviction (clock): walk `keys` from `hand`; if `refbit==1` clear and continue;
  if dirty, write back then drop; remove from maps/`keys`.
- Hit: set refbit=1, return shared bytes (aliases see later `bytes_set` —
  pages are treated read-mostly; writers rebuild the page bytes).

Honest v1: single-process, no pin counts, no multi-fd concurrent writers.

## S6 — LSM engine

### On-disk layout (flat prefix, no nested dirs required)

```
<dir>.wal          append-only WAL
<dir>.manifest     text: next_gen\n then one gen per line (oldest first)
<dir>.sst.<gen>    sorted SSTable files
```

`mkdir` is added to std/os for callers that want a real directory; the
engine itself still works with a path **prefix** (queuebaga idiom) so
`/tmp/baga_lsm` works without a directory existing.

### WAL record (binary-safe, crc32c)

```
[u32 le payload_len][payload][u32 le crc32c(payload)]
payload = [u8 op][u32 le klen][key…][u32 le vlen][val…]
op: 1 = Put, 2 = Del  (Del has vlen=0)
```

Append via `fd_write_bytes`; durability via `fdatasync` every
`sync_every` records (default 1 for correctness demos; tests may raise).

### SSTable

**v3 (current writes) — `BAGASST3`:**

```
magic 8 = "BAGASST3"
body:
  u32 le count
  records sorted by key:
    u8 op | u32 klen | key | u32 vlen | val
  restart index: N × u32 le offsets into body, then u32 N
    (restart every 16 records; first offset = 4)
  bloom filter bytes (m/8), then u32 le m  (m ≈ 10 bits/key, min 64)
u32 le crc32c(body)
```

Lookup: newest table first; bloom may-contain → restart bsearch → one-block
scan. `BAGASST2` (index, no bloom) and `BAGASST1` (full parse + bsearch)
still readable. Tombstone (op=Del) shadows older Puts.

### Memtable

`Map<str, bytes>` values + `Map<str, i64>` tombstones (1 = deleted). Keys
remain `str` (NUL-free); values are binary-safe. `mem_n` counts live put/del
ops since last flush; when `mem_n >= flush_at` (default 32), flush.

### Flush

1. Collect keys from mem + tomb, sort, write new SST gen.
2. `fsync` SST; append gen to MANIFEST; `fsync` MANIFEST.
3. Rotate WAL (`unlink` + reopen `O_CREAT|O_TRUNC` or rewrite empty).
4. Clear mem/tomb; `mem_n = 0`.

### Compaction (oldest-N)

When `vec_len(gens) >= compact_at` (default 3): merge the **oldest**
`compact_at` tables newest-wins into one new SST, drop pure tombstones
(nothing older under the merged prefix), keep younger gens, `unlink`
merged files, rewrite MANIFEST. Page-cache slots for old file_ids
invalidated.

### Recovery

Open: read MANIFEST → gens; open WAL; replay records into mem/tomb;
leave SST gens as-is. No WAL of unflushed state is lost if fdatasync
honored.

## RESP surface (kvbaga-compatible subset)

Reuse `kvbaga/resp.baga`. Commands:

| Command | Durable? | Notes |
|---------|----------|--------|
| PING / QUIT | — | |
| SET k v | yes | no EX in MVP (returns ERR if EX given) |
| GET / DEL / EXISTS | yes | |
| DBSIZE / KEYS | yes | full merge view (mem + tables; expensive) |
| INCR | yes | read-modify-write via get+set |
| EXPIRE / TTL / SET EX | no | honest ERR — TTL needs a deadline column |

Serial accept loop (same K1 constraint as kvbaga).

## std/os additions (engine needs)

```baga
extern mkdir / unlink / rename
fn fd_write_bytes / fd_pwrite_bytes / fd_pread_bytes   // binary-safe (tcp idiom)
```

Document the NUL contract: length from `bytes_len`, never `strlen` on the
payload path.

## Out of scope (honest)

- Bloom filters, sparse indexes, leveled compaction, multi-threaded
  compaction, binary values with embedded NUL in str keys/values,
  crash-atomic multi-file rename without fsync discipline beyond
  MANIFEST last, Raft (S7).

## Tests

- `tests/std/os_fs_test.baga` — mkdir/unlink/rename + binary pwrite/pread
  round-trip including a mid-buffer `0x00`.
- `tests/lsm_test.baga` — open → SET/GET → flush trigger → reopen recovery;
  DEL tombstone; compaction path; RESP loopback (go_bg server).
- `app-product/lsmbaga/demo.baga` — manual smoke.

## Success criteria

1. `sandak build` for lsmbaga green.
2. `lsm_test` green under `baga-test`.
3. redis-cli style RESP client can SET/GET against the server.
4. Kill-and-reopen (two open() calls in one process simulating restart)
   restores flushed + WAL-replayed keys.
5. CHANGELOG + package README + gaps.md + parent plan status note.
