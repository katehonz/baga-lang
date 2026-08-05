#!/usr/bin/env bash
# test_sketch.sh — Phase 5 io_uring probe smoke (no liburing, no baga).
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
PY="${PYTHON:-python3}"
PROBE="$ROOT/tools/iouring/iouring_probe.py"

echo "=== iouring detect ==="
set +e
"$PY" "$PROBE" detect
rc=$?
set -e
if [[ "$rc" -eq 2 ]]; then
  echo "SKIP: io_uring not available on this host/kernel (exit 2)"
  exit 0
fi
if [[ "$rc" -ne 0 ]]; then
  echo "FAIL: detect rc=$rc"
  exit 1
fi

echo "=== iouring NOP + POLL_ADD ==="
"$PY" "$PROBE" all

echo "=== iouring sketch: all passed ==="
