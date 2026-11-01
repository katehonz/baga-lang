#!/usr/bin/env bash
# run_vs_rocksdb.sh — head-to-head rocksbaga (engine) vs RocksDB (rocksdict).
#
# Fairness (honest):
#   durable — both fsync each put (rocksbaga sync_every=1, RocksDB WriteOptions.sync)
#   batch   — bulk-style (rocksbaga rare fsync + flush_force; RocksDB async + flush)
# Single writer, same N / value size / key shape.
#
# Usage (repo root):
#   ./bench/rocks/run_vs_rocksdb.sh
#   BENCH_N=5000 BENCH_MODE=batch ./bench/rocks/run_vs_rocksdb.sh
#   BENCH_N=20000 BENCH_MODE=durable ./bench/rocks/run_vs_rocksdb.sh
#   BENCH_DIR=/var/tmp/rocksbench ./bench/rocks/run_vs_rocksdb.sh
#     ^ real-disk dir: honest fdatasync cost. Default /tmp is tmpfs on many
#       hosts, where fsync is a no-op and "durable" PUT is not comparable.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
BAGA="${BAGA:-$ROOT/baga}"
VENV="${ROCKS_VENV:-/tmp/rocksbench-venv}"
MODE="${BENCH_MODE:-durable}"
N="${BENCH_N:-10000}"
VLEN="${BENCH_VLEN:-100}"
BENCH_ROOT="${BENCH_DIR:-/tmp/rocksbench}"
OUTDIR="${BENCH_OUT:-$ROOT/bench/rocks/results}"
mkdir -p "$OUTDIR" "$BENCH_ROOT"
STAMP="$(date -u +%Y%m%dT%H%M%SZ)"
LOG="$OUTDIR/vs-rocksdb-$STAMP.txt"
BAGA_TMP="$(mktemp /tmp/baga_rb_out.XXXXXX)"
RDB_TMP="$(mktemp /tmp/rdb_out.XXXXXX)"
trap 'rm -f "$BAGA_TMP" "$RDB_TMP"' EXIT

if [[ ! -x "$BAGA" ]]; then
  echo "missing baga binary at $BAGA" >&2
  exit 127
fi

if [[ ! -x "$VENV/bin/python" ]]; then
  echo "=== creating venv + rocksdict at $VENV ==="
  python3 -m venv "$VENV"
  "$VENV/bin/pip" install -q rocksdict
fi

export BENCH_N="$N" BENCH_VLEN="$VLEN" BENCH_MODE="$MODE"
export BENCH_PREFIX="$BENCH_ROOT/baga_rocks"
export BENCH_DIR="$BENCH_ROOT/rocksdb"

echo "=== head-to-head: rocksbaga vs RocksDB ===" | tee "$LOG"
echo "n=$N vlen=$VLEN mode=$MODE host=$(uname -n) $(uname -m)" | tee -a "$LOG"
echo "bench_dir=$BENCH_ROOT fs=$(df -T "$BENCH_ROOT" | tail -1 | awk '{print $2" on "$NF}')" | tee -a "$LOG"
if df -T "$BENCH_ROOT" | tail -1 | grep -q tmpfs; then
  echo "WARN: bench dir is on tmpfs — fdatasync is a no-op; durable PUT is NOT fsync-bound here" | tee -a "$LOG"
fi
echo "" | tee -a "$LOG"

echo "--- rocksbaga (pure engine) ---" | tee -a "$LOG"
set +e
"$BAGA" -I "$ROOT" -I "$ROOT/app-product" "$ROOT/bench/rocks/engine_bench.baga" 2>&1 | tee -a "$LOG" | tee "$BAGA_TMP"
baga_rc=${PIPESTATUS[0]}
set -e

echo "" | tee -a "$LOG"
echo "--- RocksDB (rocksdict) ---" | tee -a "$LOG"
set +e
"$VENV/bin/python" "$ROOT/bench/rocks/rocksdb_bench.py" 2>&1 | tee -a "$LOG" | tee "$RDB_TMP"
rdb_rc=${PIPESTATUS[0]}
set -e

parse() {
  # $1 file $2 KEY
  local f="$1" k="$2"
  rg -o "${k}=[0-9]+" "$f" | head -1 | cut -d= -f2
}

echo "" | tee -a "$LOG"
echo "=== comparison (ops/s; higher is better) ===" | tee -a "$LOG"
printf '%-12s %12s %12s %10s\n' "metric" "rocksbaga" "RocksDB" "ratio" | tee -a "$LOG"

compare_row() {
  local label="$1" bk="$2" rk="$3"
  local b r
  b=$(parse "$BAGA_TMP" "$bk")
  r=$(parse "$RDB_TMP" "$rk")
  if [[ -z "$b" || -z "$r" || "$r" == "0" ]]; then
    printf '%-12s %12s %12s %10s\n' "$label" "${b:-?}" "${r:-?}" "n/a" | tee -a "$LOG"
    return
  fi
  # ratio = baga/rocks * 100 as integer percent of RocksDB
  local pct=$(( b * 100 / r ))
  printf '%-12s %12s %12s %9s%%\n' "$label" "$b" "$r" "$pct" | tee -a "$LOG"
}

compare_row "PUT" "PUT_OPS" "PUT_OPS"
compare_row "GET_SEQ" "GET_SEQ_OPS" "GET_SEQ_OPS"
compare_row "GET_RND" "GET_RND_OPS" "GET_RND_OPS"

echo "" | tee -a "$LOG"
if [[ "$baga_rc" -ne 0 || "$rdb_rc" -ne 0 ]]; then
  echo "FAIL baga_rc=$baga_rc rocks_rc=$rdb_rc" | tee -a "$LOG"
  exit 1
fi

# Also write latest pointer
cp "$LOG" "$OUTDIR/vs-rocksdb-latest.txt"
echo "log: $LOG" | tee -a "$LOG"
echo "OK" | tee -a "$LOG"
