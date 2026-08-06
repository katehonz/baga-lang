#!/usr/bin/env bash
# run_vs_redis.sh — RESP head-to-head: rocksbaga server vs Redis (if available).
#
# Usage (repo root):
#   ./bench/rocks/run_vs_redis.sh
#   BENCH_N=10000 ./bench/rocks/run_vs_redis.sh
#   SKIP_REDIS=1 ./bench/rocks/run_vs_redis.sh   # baga only
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
BAGA="${BAGA:-$ROOT/baga}"
N="${BENCH_N:-5000}"
VLEN="${BENCH_VLEN:-100}"
# Pipeline width: 1 = one RTT/cmd; 64 = Redis-style bulk (R30).
PIPE="${BENCH_PIPE:-64}"
# Concurrent RESP clients (R36 multi-conn soak). n is per-client.
CLIENTS="${BENCH_CLIENTS:-1}"
# Ephemeral ports avoid TIME_WAIT / leftover listeners between consecutive soaks.
BAGA_PORT="${BAGA_PORT:-$((16500 + RANDOM % 500))}"
REDIS_PORT="${REDIS_PORT:-$((17000 + RANDOM % 500))}"
OUTDIR="${BENCH_OUT:-$ROOT/bench/rocks/results}"
mkdir -p "$OUTDIR"
STAMP="$(date -u +%Y%m%dT%H%M%SZ)"
LOG="$OUTDIR/vs-redis-$STAMP.txt"
SKIP_REDIS="${SKIP_REDIS:-0}"

if [[ ! -x "$BAGA" ]]; then
  echo "missing baga binary at $BAGA" >&2
  exit 127
fi

export BENCH_N="$N" BENCH_VLEN="$VLEN" BENCH_PIPE="$PIPE" BENCH_CLIENTS="$CLIENTS"

cleanup() {
  if [[ -n "${BAGA_PID:-}" ]] && kill -0 "$BAGA_PID" 2>/dev/null; then
    kill "$BAGA_PID" 2>/dev/null || true
    wait "$BAGA_PID" 2>/dev/null || true
  fi
  if [[ -n "${REDIS_PID:-}" ]] && kill -0 "$REDIS_PID" 2>/dev/null; then
    kill "$REDIS_PID" 2>/dev/null || true
    wait "$REDIS_PID" 2>/dev/null || true
  fi
  rm -f /tmp/baga_resp_bench.* 2>/dev/null || true
}
trap cleanup EXIT

echo "=== RESP head-to-head: rocksbaga vs Redis ===" | tee "$LOG"
echo "n=$N vlen=$VLEN pipe=$PIPE clients=$CLIENTS parallel=${LSM_PARALLEL:-0} shards=${LSM_SHARDS:-1} host=$(uname -n) $(uname -m)" | tee -a "$LOG"
echo "" | tee -a "$LOG"

# --- rocksbaga ---
# Unique path per run so leftover SSTs cannot poison the next soak.
BENCH_PATH="/tmp/baga_resp_bench_$$"
rm -f "${BENCH_PATH}".wal "${BENCH_PATH}".manifest
rm -f "${BENCH_PATH}".sst.* "${BENCH_PATH}".bloom.* 2>/dev/null || true
export LSMPATH="$BENCH_PATH" LSMPORT="$BAGA_PORT"
export LSM_FLUSH_AT="${LSM_FLUSH_AT:-10000}"
export LSM_CACHE_PAGES="${LSM_CACHE_PAGES:-2048}"
# Fair vs Redis --appendonly no: do not fsync every SET (still WAL-buffered).
# Override with LSM_SYNC_EVERY=1 for durable RESP.
export LSM_SYNC_EVERY="${LSM_SYNC_EVERY:-10000}"
# R34: set LSM_PARALLEL=1 LSM_SHARDS=N for multi-core workers behind RESP.
export LSM_PARALLEL="${LSM_PARALLEL:-0}"
export LSM_SHARDS="${LSM_SHARDS:-1}"

"$BAGA" -I "$ROOT" -I "$ROOT/app-product" \
  "$ROOT/app-product/rocksbaga/tools/serve.baga" \
  >/tmp/baga_resp_server.log 2>&1 &
BAGA_PID=$!

# wait for accept
for i in $(seq 1 50); do
  if (echo >/dev/tcp/127.0.0.1/"$BAGA_PORT") 2>/dev/null; then
    break
  fi
  # bash /dev/tcp may not work; use python
  if python3 -c "import socket; s=socket.create_connection(('127.0.0.1',$BAGA_PORT),0.2); s.close()" 2>/dev/null; then
    break
  fi
  sleep 0.1
done
if ! python3 -c "import socket; s=socket.create_connection(('127.0.0.1',$BAGA_PORT),1); s.close()" 2>/dev/null; then
  echo "FAIL: rocksbaga did not listen on $BAGA_PORT" | tee -a "$LOG"
  cat /tmp/baga_resp_server.log | tee -a "$LOG" || true
  exit 1
fi

echo "--- rocksbaga RESP (port $BAGA_PORT) ---" | tee -a "$LOG"
set +e
BENCH_HOST=127.0.0.1 BENCH_PORT="$BAGA_PORT" BENCH_PIPE="$PIPE" BENCH_CLIENTS="$CLIENTS" \
  python3 "$ROOT/bench/rocks/resp_client.py" 2>&1 | tee -a "$LOG" | tee /tmp/baga_resp_out.txt
baga_rc=${PIPESTATUS[0]}
set -e

# --- redis (optional) ---
rdb_rc=0
if [[ "$SKIP_REDIS" != "1" ]] && command -v redis-server >/dev/null 2>&1; then
  echo "" | tee -a "$LOG"
  echo "--- Redis RESP (port $REDIS_PORT) ---" | tee -a "$LOG"
  redis-server --port "$REDIS_PORT" --save "" --appendonly no \
    --dir /tmp --dbfilename baga_redis_bench.rdb \
    >/tmp/redis_resp_server.log 2>&1 &
  REDIS_PID=$!
  for i in $(seq 1 50); do
    if python3 -c "import socket; s=socket.create_connection(('127.0.0.1',$REDIS_PORT),0.2); s.close()" 2>/dev/null; then
      break
    fi
    sleep 0.1
  done
  set +e
  BENCH_HOST=127.0.0.1 BENCH_PORT="$REDIS_PORT" BENCH_PIPE="$PIPE" BENCH_CLIENTS="$CLIENTS" \
    python3 "$ROOT/bench/rocks/resp_client.py" 2>&1 | tee -a "$LOG" | tee /tmp/rdb_resp_out.txt
  rdb_rc=${PIPESTATUS[0]}
  set -e

  parse() {
    local f="$1" k="$2"
    rg -o "${k}=[0-9]+" "$f" | head -1 | cut -d= -f2
  }
  echo "" | tee -a "$LOG"
  echo "=== comparison (ops/s; higher is better) ===" | tee -a "$LOG"
  printf '%-12s %12s %12s %10s\n' "metric" "rocksbaga" "Redis" "ratio" | tee -a "$LOG"
  compare_row() {
    local label="$1" bk="$2" rk="$3"
    local b r
    b=$(parse /tmp/baga_resp_out.txt "$bk")
    r=$(parse /tmp/rdb_resp_out.txt "$rk")
    if [[ -z "$b" || -z "$r" || "$r" == "0" ]]; then
      printf '%-12s %12s %12s %10s\n' "$label" "${b:-?}" "${r:-?}" "n/a" | tee -a "$LOG"
      return
    fi
    local pct=$(( b * 100 / r ))
    printf '%-12s %12s %12s %9s%%\n' "$label" "$b" "$r" "$pct" | tee -a "$LOG"
  }
  compare_row "PING" "PING_OPS" "PING_OPS"
  compare_row "SET" "SET_OPS" "SET_OPS"
  compare_row "GET" "GET_OPS" "GET_OPS"
else
  echo "" | tee -a "$LOG"
  echo "(Redis skipped: redis-server not found or SKIP_REDIS=1)" | tee -a "$LOG"
  echo "Install redis-server for head-to-head, or read rocksbaga absolute ops above." | tee -a "$LOG"
fi

echo "" | tee -a "$LOG"
if [[ "$baga_rc" -ne 0 ]]; then
  echo "FAIL baga_rc=$baga_rc" | tee -a "$LOG"
  exit 1
fi
cp "$LOG" "$OUTDIR/vs-redis-latest.txt"
echo "log: $LOG" | tee -a "$LOG"
echo "OK" | tee -a "$LOG"
