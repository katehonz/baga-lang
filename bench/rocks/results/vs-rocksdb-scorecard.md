# rocksbaga vs RocksDB — scorecard

**Date:** 2026-08-06  
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
| **PUT** | 765 ops/s | 824 ops/s | **92%** |
| **GET_SEQ** | 556k ops/s | 534k ops/s | **103%** |
| **GET_RND** | 500k ops/s | 505k ops/s | **98%** |

Artifact: `vs-rocksdb-20260806T095807Z.txt`

**Read:** durable PUT is fsync-bound (~same wall time as RocksDB).  
GET sequential slightly **above** RocksDB; random ~parity.

---

## Batch (flush at end)

| metric | rocksbaga | RocksDB | ratio |
|--------|----------:|--------:|------:|
| **PUT** | 192k ops/s | 140k ops/s | **137%** |
| **GET_SEQ** | 625k ops/s | 522k ops/s | **119%** |
| **GET_RND** | 500k ops/s | 498k ops/s | **100%** |

Artifact: `vs-rocksdb-20260806T095822Z.txt` (also `vs-rocksdb-latest.txt`)

**Read:** bulk load path **beats** RocksDB on this harness.  
GET at or above RocksDB.

---

## One-line summary

> Pure Baga LSM: **~92% of RocksDB durable PUT**, **GET parity**,  
> **batch PUT 137%** of RocksDB — measured head-to-head, same keys/values/N.

RocksDB has years of production depth. This scorecard is the storage
engine *speed* bar, not the full product bar. Still: worth owning.
