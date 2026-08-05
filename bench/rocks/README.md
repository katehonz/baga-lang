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
| **batch** | rare sync + `flush_force` at end | async writes + `flush()` at end |

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
