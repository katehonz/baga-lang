# rocksbaga vs Redis — RESP scorecard

**Date:** 2026-08-06
**Host:** deb1 x86_64
**Harness:** `./bench/rocks/run_vs_redis.sh` (`BENCH_CLIENTS=8 BENCH_PIPE=16
LSM_SERVE_MT=1 LSM_SHARDS=8`)
**N=5000/client**, vlen=100, full RESP stack over TCP (pipelined).

Redis side: Redis 8.x (`--appendonly no`).
rocksbaga: `tools/serve.baga` MT mode (hop-less, per-shard mutexes, R55).

This is a **server bench**, not a claim of Redis feature parity (no
replication, no Lua, no eviction policies — see `app-product/rocksbaga`).

---

## MT serve (R55+), 8 clients, pipe=16, shards=8

| metric | rocksbaga | Redis | ratio |
|--------|----------:|------:|------:|
| **PING** | 339k ops/s | 349k ops/s | **97%** |
| **SET** | 230k ops/s | 248k ops/s | **92%** |
| **GET** | 215k ops/s | 219k ops/s | **98%** |

Artifact: `vs-redis-20260806T152425Z.txt` (also `vs-redis-latest.txt`).

**Read:** after R52 (thread-local arena — the real GIL), R54 (reply
assembly without O(n²) concat) and R55 (hop-less MT), the Baga RESP
server sits at **92–98% of Redis** aggregate throughput on this harness.
R67/R68 (binary-safe keys end-to-end) re-verified the same numbers —
the bytes-key path costs nothing measurable.

---

## One-line summary

> Pure Baga LSM + RESP server: **92–98% of Redis 8.x** on pipelined
> multi-client soak, with durable WAL/SST underneath and binary-safe
> keys and values.

See also `vs-rocksdb-scorecard.md` for the pure-engine numbers vs
RocksDB (batch PUT 137%, GET parity).
