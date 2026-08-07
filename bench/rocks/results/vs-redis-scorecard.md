# rocksbaga vs Redis — RESP scorecard

**Date:** 2026-08-07
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
| **PING** | 339k ops/s | 340k ops/s | **99%** |
| **SET** | 233k ops/s | 241k ops/s | **96%** |
| **GET** | 214k ops/s | 218k ops/s | **98%** |

Artifact: `vs-redis-20260807T200902Z.txt` (also `vs-redis-latest.txt`).

**Read:** after R52 (thread-local arena — the real GIL), R54 (reply
assembly without O(n²) concat) and R55 (hop-less MT), the Baga RESP
server sits at **96–99% of Redis** aggregate throughput on this harness.
R69 (2026-08-07, INCRBY/DECRBY/GETSET/STRLEN/GETDEL/SETNX/UNLINK +
multi-key DEL in all four exec cores) re-verified the numbers — command
parity work costs nothing measurable on the hot SET/GET/PING path.
Previous scorecard (2026-08-06, R68): PING 97% / SET 92% / GET 98%.

---

## One-line summary

> Pure Baga LSM + RESP server: **96–99% of Redis 8.x** on pipelined
> multi-client soak, with durable WAL/SST underneath and binary-safe
> keys and values.

See also `vs-rocksdb-scorecard.md` for the pure-engine numbers vs
RocksDB (batch PUT 137%, GET parity).
