#!/usr/bin/env bash
# run_profile.sh — differential phase probes for rocksbaga (no RocksDB).
#
# Usage (repo root):
#   ./bench/rocks/run_profile.sh              # put/get phases
#   ./bench/rocks/run_profile.sh flush        # flush/sst/compact breakdown
#   ./bench/rocks/run_profile.sh all          # both
#   BENCH_N=5000 BENCH_VLEN=100 ./bench/rocks/run_profile.sh flush
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
BAGA="${BAGA:-$ROOT/baga}"
N="${BENCH_N:-2000}"
VLEN="${BENCH_VLEN:-100}"
CN="${BENCH_COMPACT_N:-$N}"
OUTDIR="${BENCH_OUT:-$ROOT/bench/rocks/results}"
mkdir -p "$OUTDIR"
STAMP="$(date -u +%Y%m%dT%H%M%SZ)"
WHICH="${1:-put}"

if [[ ! -x "$BAGA" ]]; then
  echo "missing baga binary at $BAGA" >&2
  exit 127
fi

export BENCH_N="$N" BENCH_VLEN="$VLEN" BENCH_COMPACT_N="$CN"

run_put() {
  local LOG="$OUTDIR/profile-$STAMP.txt"
  echo "=== rocksbaga engine profile (put/get differential) ===" | tee "$LOG"
  echo "n=$N vlen=$VLEN host=$(uname -n) $(uname -m)" | tee -a "$LOG"
  echo "" | tee -a "$LOG"
  "$BAGA" -I "$ROOT" -I "$ROOT/app-product" \
    "$ROOT/bench/rocks/engine_profile.baga" 2>&1 | tee -a "$LOG"
  cp "$LOG" "$OUTDIR/profile-latest.txt"
  echo "log: $LOG" | tee -a "$LOG"
}

run_flush() {
  local LOG="$OUTDIR/flush-profile-$STAMP.txt"
  echo "=== rocksbaga flush/sst/compact profile ===" | tee "$LOG"
  echo "n=$N vlen=$VLEN compact_n=$CN host=$(uname -n) $(uname -m)" | tee -a "$LOG"
  echo "" | tee -a "$LOG"
  "$BAGA" -I "$ROOT" -I "$ROOT/app-product" \
    "$ROOT/bench/rocks/flush_profile.baga" 2>&1 | tee -a "$LOG"
  cp "$LOG" "$OUTDIR/flush-profile-latest.txt"
  echo "log: $LOG" | tee -a "$LOG"
}

case "$WHICH" in
  put|engine) run_put ;;
  flush|sst)  run_flush ;;
  all)
    run_put
    echo ""
    STAMP="$(date -u +%Y%m%dT%H%M%SZ)"
    run_flush
    ;;
  *)
    echo "usage: $0 [put|flush|all]" >&2
    exit 2
    ;;
esac
echo "OK"
