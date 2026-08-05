#!/usr/bin/env bash
# run_latency.sh — cloud-profile latency microbench (p50/p99).
# Usage (from repo root):
#   ./bench/run_latency.sh
#   BENCH_N=5000 ./bench/run_latency.sh
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BAGA="${BAGA:-$ROOT/baga}"
if [[ ! -x "$BAGA" ]]; then
  echo "run_latency.sh: missing $BAGA (run make)" >&2
  exit 127
fi
echo "=== baga latency microbench ==="
"$BAGA" -I "$ROOT" -I "$ROOT/app-product" "$ROOT/bench/latency.baga"
