# rocksbaga vs RocksDB — scorecard

**Date:** 2026-08-07
**Host:** deb1 x86_64
**Harness:** `./bench/rocks/run_vs_rocksdb.sh`
**N=5000**, vlen=100, pure engine (no RESP/TCP)

RocksDB side: Python `rocksdict` (official RocksDB).
rocksbaga: Baga `lsm_put` / `lsm_get` (C backend).

This is a **microbench**, not a claim of RocksDB feature parity.
It *is* something to be proud of: a young language stack, same-league numbers.

---

## Durable (fsync each put)

| metric | rocksbaga | RocksDB | ratio |
|--------|----------:|--------:|------:|
| **PUT** | 763 ops/s | 810 ops/s | **94%** |
| **GET_SEQ** | 625k ops/s | 518k ops/s | **120%** |
| **GET_RND** | 625k ops/s | 490k ops/s | **127%** |

Artifact: `vs-rocksdb-20260807T201644Z.txt`

**Read:** durable PUT is fsync-bound (~same wall time as RocksDB).
GET sequential **20% above** RocksDB; random **27% above**.

---

## Batch (flush at end)

| metric | rocksbaga | RocksDB | ratio |
|--------|----------:|--------:|------:|
| **PUT** | 192k ops/s | 133k ops/s | **144%** |
| **GET_SEQ** | 714k ops/s | 461k ops/s | **154%** |
| **GET_RND** | 714k ops/s | 330k ops/s | **216%** |

Artifact: `vs-rocksdb-20260807T201702Z.txt` (also `vs-rocksdb-latest.txt`)

**Read:** bulk load path **beats** RocksDB on this harness.
GET 1.5–2.2× RocksDB today (the RocksDB side ran colder than the
2026-08-06 card — ratios swing with RAM pressure on this host).

---

## One-line summary

> Pure Baga LSM: **~94% of RocksDB durable PUT**, **GET 120–127%**,
> **batch PUT 144%**, **batch GET up to 216%** of RocksDB — measured
> head-to-head, same keys/values/N.

RocksDB has years of production depth. This scorecard is the storage
engine *speed* bar, not the full product bar. Still: worth owning.

Previous card (2026-08-06): durable PUT 92%, GET 98–103%; batch PUT 137%,
GET 100–119%.

---

## 2026-11-01 card (real disk — btrfs on NVMe)

**Host:** deb1 x86_64, bench dir `/var/tmp/rocksbench` (btrfs, **not tmpfs** —
`BENCH_DIR` set so fdatasync is real). N=5000, vlen=100, pure engine.

| mode | metric | rocksbaga | RocksDB | ratio |
|------|--------|----------:|--------:|------:|
| durable | PUT | 762 ops/s | 612 ops/s | **124%** |
| durable | GET_SEQ | 555k ops/s | 437k ops/s | **127%** |
| durable | GET_RND | 555k ops/s | 486k ops/s | **114%** |
| batch | PUT | 116k ops/s | 140k ops/s | **82%** |
| batch | GET_SEQ | 714k ops/s | 501k ops/s | **142%** |
| batch | GET_RND | 714k ops/s | 483k ops/s | **148%** |

Artifacts: `vs-rocksdb-20261101T093046Z.txt` (durable),
`vs-rocksdb-20261101T093103Z.txt` (batch).

**Read:** on real disk the durable PUT is fsync-bound and now **beats**
RocksDB on this harness (124%); reads are 114–127%. Batch (bulk) PUT is
82% this run — noisier than the 2026-08-07 card (144%); batch reads 142–148%.

**Caveat (important):** the default bench dir is under `/tmp`, which is
**tmpfs** on many hosts — there `fdatasync` is a no-op and "durable" PUT
numbers (e.g. 40k+ ops/s) are not comparable to a real disk. Always run
with `BENCH_DIR=<real-disk dir>` for durable claims; the runner now prints
the fs type and warns on tmpfs.
