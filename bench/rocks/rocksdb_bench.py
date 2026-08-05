#!/usr/bin/env python3
"""rocksdb_bench.py — RocksDB side of head-to-head vs rocksbaga.

Same workload as engine_bench.baga:
  N puts, N sequential gets, N LCG-random gets; value length VLEN.

Env:
  BENCH_N, BENCH_VLEN, BENCH_MODE=durable|batch
  ROCKS_DIR  (default /tmp/baga_rocksdb_bench)

Output: same META/PUT_*/GET_*_*/OK lines as engine_bench.baga.
"""
from __future__ import annotations

import os
import shutil
import sys
import time


def pad_key(i: int) -> str:
    return f"k{i:08d}"


def ops_per_s(n: int, ms: float) -> int:
    if ms <= 0:
        return n * 1000
    return int((n * 1000) / ms)


def main() -> int:
    try:
        from rocksdict import Options, Rdict, WriteOptions
    except ImportError:
        print("FAIL: rocksdict not installed (use venv from run_vs_rocksdb.sh)", file=sys.stderr)
        return 2

    n = int(os.environ.get("BENCH_N", "10000"))
    if n < 100:
        n = 100
    vlen = int(os.environ.get("BENCH_VLEN", "100"))
    vlen = max(1, min(vlen, 4096))
    mode = os.environ.get("BENCH_MODE", "durable")
    if mode not in ("durable", "batch"):
        mode = "durable"

    path = os.environ.get("ROCKS_DIR", "/tmp/baga_rocksdb_bench")
    if os.path.isdir(path):
        shutil.rmtree(path)
    os.makedirs(path, exist_ok=True)

    opts = Options()
    # Keep comparison single-threaded / simple — match rocksbaga one-writer model.
    opts.create_if_missing(True)
    opts.increase_parallelism(1)
    opts.set_max_background_jobs(1)

    db = Rdict(path, options=opts)
    wopts = WriteOptions()
    if mode == "durable":
        wopts.sync = True  # fsync each put — fair vs rocksbaga sync_every=1
    else:
        wopts.sync = False

    print(f"META n={n} vlen={vlen} mode={mode}")
    val = b"x" * vlen  # fixed payload — same as rocksbaga bench

    # PUT
    t0 = time.perf_counter()
    for i in range(n):
        db.put(pad_key(i).encode(), val, write_opt=wopts)
    if mode == "batch":
        # flush memtable + durability point at end (bulk-load style)
        db.flush()
    t1 = time.perf_counter()
    put_ms = (t1 - t0) * 1000.0
    print(f"PUT_MS={put_ms:.0f} PUT_OPS={ops_per_s(n, put_ms)}")

    # Reopen for GET (drop process-side cache; fairer cold-ish read path)
    db.close()
    db = Rdict(path, options=opts)

    # sequential GET
    t2 = time.perf_counter()
    hits = 0
    for i in range(n):
        v = db.get(pad_key(i).encode())
        if v is not None:
            hits += 1
    t3 = time.perf_counter()
    gseq_ms = max((t3 - t2) * 1000.0, 0.001)
    print(f"GET_SEQ_MS={gseq_ms:.0f} GET_SEQ_OPS={ops_per_s(n, gseq_ms)}")
    if hits != n:
        print(f"FAIL hits={hits} want={n}", file=sys.stderr)
        db.close()
        return 1

    # random GET
    t4 = time.perf_counter()
    x = 1
    hits = 0
    for _ in range(n):
        x = (x * 1103515245 + 12345) & 0x7FFFFFFF
        idx = x % n
        v = db.get(pad_key(idx).encode())
        if v is not None:
            hits += 1
    t5 = time.perf_counter()
    grnd_ms = max((t5 - t4) * 1000.0, 0.001)
    print(f"GET_RND_MS={grnd_ms:.0f} GET_RND_OPS={ops_per_s(n, grnd_ms)}")
    if hits != n:
        print(f"FAIL rnd hits={hits}", file=sys.stderr)
        db.close()
        return 1

    db.close()
    print("OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
