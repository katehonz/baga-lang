#!/usr/bin/env bash
# Chunked seed + query for ts / graph / vec (Q2 arena: one chunk per process).
# Usage:
#   bash bench/boila/run_modality_benches.sh [ts|graph|vec|all]
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$ROOT"
BAGA="${BAGA:-./baga}"
I=("-I" "." "-I" "app-product")

run_chunked() {
  local name="$1" src="$2" prefix="$3" total="$4" nchunks="$5"
  local extra_env="${6:-}"
  echo "=== $name seed total=$total nchunks=$nchunks ==="
  local c=0
  while [ "$c" -lt "$nchunks" ]; do
    # shellcheck disable=SC2086
    env ${prefix}_PHASE=write ${prefix}_TOTAL="$total" ${prefix}_NCHUNKS="$nchunks" \
      ${prefix}_CHUNK="$c" $extra_env \
      "$BAGA" "${I[@]}" "$src" || return 1
    c=$((c + 1))
  done
  echo "=== $name query ==="
  # shellcheck disable=SC2086
  env ${prefix}_PHASE=query $extra_env \
    "$BAGA" "${I[@]}" "$src" || return 1
}

which="${1:-all}"

if [ "$which" = "ts" ] || [ "$which" = "all" ]; then
  # S5 target 1M; default 100k for wall-time (override TS_BENCH_TOTAL)
  run_chunked "ts" "bench/boila/ts_bench.baga" "TS_BENCH" \
    "${TS_BENCH_TOTAL:-100000}" "${TS_BENCH_NCHUNKS:-20}"
fi

if [ "$which" = "graph" ] || [ "$which" = "all" ]; then
  # G1 target 1M edges; default 100k chain (+100k skip)
  run_chunked "graph" "bench/boila/graph_bench.baga" "GR_BENCH" \
    "${GR_BENCH_TOTAL:-100000}" "${GR_BENCH_NCHUNKS:-20}"
fi

if [ "$which" = "vec" ] || [ "$which" = "all" ]; then
  # V2 target 100k×128d; default 10k×16d (SQL literal size)
  run_chunked "vec" "bench/boila/vec_bench.baga" "VEC_BENCH" \
    "${VEC_BENCH_TOTAL:-10000}" "${VEC_BENCH_NCHUNKS:-20}" \
    "VEC_BENCH_DIM=${VEC_BENCH_DIM:-16}"
fi

echo "=== done ==="
