# rocksbaga vs RocksDB

Honest **engine microbench**: same keys, values, op counts, single writer.

| Side | Implementation |
|------|----------------|
| rocksbaga | pure Baga `lsm_put` / `lsm_get` (`bench/rocks/engine_bench.baga`) |
| RocksDB | Python `rocksdict` (official RocksDB) |

No RESP/TCP — compares the storage engines, not redis protocol stacks.

## Modes

| Mode | rocksbaga | RocksDB |
|------|-----------|---------|
| **durable** (default) | `sync_every=1` (fdatasync each put) | `WriteOptions.sync=true` |
| **batch** | `flush_at=N`, rare sync + `flush_force` at end | async writes + `flush()` at end |

Optional: `BENCH_FLUSH_AT=k` overrides the memtable flush threshold.

`durable` is the strict fairness case. `batch` is closer to bulk load.

## Run

```bash
# default: N=10000, durable
./bench/rocks/run_vs_rocksdb.sh

BENCH_N=20000 BENCH_MODE=batch ./bench/rocks/run_vs_rocksdb.sh
BENCH_N=5000 BENCH_VLEN=100 BENCH_MODE=durable ./bench/rocks/run_vs_rocksdb.sh
```

First run creates a venv at `/tmp/rocksbench-venv` and installs `rocksdict`.

## Output

Machine lines from both engines, then a ratio table (rocksbaga as % of RocksDB ops/s).

Artifacts: `bench/rocks/results/vs-rocksdb-*.txt` and `vs-rocksdb-latest.txt`.

## How to read results

- **ratio ≥ 100%** — rocksbaga matches or beats RocksDB on that metric
- **ratio 50–100%** — in the same league (close)
- **ratio ≪ 50%** — clear gap; see gaps (interpreter/C backend, page cache, fsync path)

This is not a claim of RocksDB feature parity — only throughput under this harness.

## Phase profile (differential)

Where does put/get time go? Isolates language vs fsync vs flush without
invasive engine counters:

```bash
./bench/rocks/run_profile.sh              # put/get phases
./bench/rocks/run_profile.sh flush        # flush / sst / compact
./bench/rocks/run_profile.sh all
BENCH_N=5000 ./bench/rocks/run_profile.sh flush
```

### Put/get (`run_profile.sh` / `engine_profile.baga`)

| Phase | What it measures |
|-------|------------------|
| `KEYS` | `pad_key` only |
| `VAL_BYTES` | `bytes_of_str` of fixed payload |
| `MEM` | WAL encode + buf + memtable (**no** fsync, **no** flush) |
| `SYNC` | MEM + fsync every put |
| `FLUSH` | MEM + `flush_at=64` (rare fsync) |
| `DURABLE` | head-to-head default (`sync_every=1`, flush 64) |
| `BATCH` | rare sync + `flush_force` at end |
| `GET_MEM` / `GET_SEQ` / `GET_RND` | get before reopen / SST seq / SST random |

Derived: `SHARE_OF_DURABLE mem%=… fsync%=…`.

### Flush (`run_profile.sh flush` / `flush_profile.baga`)

| Phase | What it measures |
|-------|------------------|
| `ROWS` | map_keys + sort + `SstRow` build |
| `BLOOM` | `bloom_build_bits` alone |
| `SST_BUILD` | in-memory SST body (incl. bloom) |
| `SST_DISK` | write + fsync + bloom sidecar |
| `WAL_SYNC` / `MANIFEST` / `WAL_ROTATE` | durable bookkeeping |
| `FLUSH_FORCE` | full `lsm_flush_force` (no compact) |
| `MULTI_FLUSH` | `flush_at=64`, `compact_at=4` (bench-like) |
| `COMPACT` | compact only, after L0-only fill |

Derived: `SHARE_OF_FLUSH rows%/bloom%/build_core%/disk%/wal%`.

Artifacts: `results/profile-*.txt`, `results/flush-profile-*.txt`.

## RESP vs Redis (optional)

Compares the **full RESP stack** (not pure engine):

```bash
./bench/rocks/run_vs_redis.sh                          # default pipe=64
BENCH_PIPE=1 ./bench/rocks/run_vs_redis.sh             # one RTT / command
BENCH_N=10000 BENCH_PIPE=256 ./bench/rocks/run_vs_redis.sh
LSM_SYNC_EVERY=1 ./bench/rocks/run_vs_redis.sh         # durable SET
LSM_PARALLEL=1 LSM_SHARDS=4 ./bench/rocks/run_vs_redis.sh  # multi-core workers
BENCH_CLIENTS=8 BENCH_PIPE=1 LSM_PARALLEL=1 LSM_SHARDS=4 \
  ./bench/rocks/run_vs_redis.sh                        # multi-conn soak (R36)
```

Uses `tools/serve.baga` + `resp_client.py` (`--clients` / `BENCH_CLIENTS`).
Redis side only if `redis-server` is on `PATH` (`--appendonly no` for fair bulk SET).

Multi-conn artifact: `results/multi-par-soak-latest.txt`.
