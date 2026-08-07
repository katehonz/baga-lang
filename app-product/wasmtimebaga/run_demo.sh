#!/usr/bin/env bash
# Build demo.baga with native shim + libwasmtime and run it.
set -euo pipefail

PKG="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$PKG/../.." && pwd)"
BAGA="${BAGA:-$ROOT/baga}"
SANDAK="${SANDAK:-$ROOT/sandak}"

if [[ ! -x "$BAGA" ]]; then
  echo "run_demo: missing $BAGA (run make)" >&2
  exit 127
fi
if [[ ! -f "$PKG/vendor/lib/libwasmtime.so" ]]; then
  echo "run_demo: missing vendor/lib/libwasmtime.so — run ./scripts/fetch-wasmtime-c-api.sh" >&2
  exit 1
fi

cd "$PKG"
mkdir -p target

# compile shim
gcc -O2 -std=c11 -c -o target/baga_wt_shim.o shims/baga_wt_shim.c \
  -Ivendor/include -Ishims

# emit + link demo
export BAGA_CFLAGS="-Ivendor/include"
export BAGA_EXTRA_OBJS="target/baga_wt_shim.o"
export BAGA_LDFLAGS="-Lvendor/lib -lwasmtime -ldl -Wl,-rpath,$PKG/vendor/lib"
export LD_LIBRARY_PATH="$PKG/vendor/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"

"$BAGA" -I "$ROOT" -I "$ROOT/app-product" demo.baga "$@"
