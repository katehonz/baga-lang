#!/usr/bin/env bash
# Live wasmtimebaga checks (requires vendor C API).
set -euo pipefail

PKG="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$PKG/../.." && pwd)"

"$PKG/run_demo.sh" > /tmp/wasmtimebaga_demo.out
grep -q "gcd(6, 27) = 3" /tmp/wasmtimebaga_demo.out
grep -q "gcd(48, 18) = 6" /tmp/wasmtimebaga_demo.out
grep -q "wat add(20, 22) = 42" /tmp/wasmtimebaga_demo.out
grep -q "wasmtimebaga demo: ok" /tmp/wasmtimebaga_demo.out

# package-local smoke (not under tests/ so make test does not require vendor)
if [[ -f "$PKG/tests/smoke.baga" ]]; then
  export BAGA="${BAGA:-$ROOT/baga}"
  export BAGA_CFLAGS="-I$PKG/vendor/include"
  export BAGA_EXTRA_OBJS="$PKG/target/baga_wt_shim.o"
  export BAGA_LDFLAGS="-L$PKG/vendor/lib -lwasmtime -ldl -Wl,-rpath,$PKG/vendor/lib"
  export LD_LIBRARY_PATH="$PKG/vendor/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
  mkdir -p "$PKG/target"
  gcc -O2 -std=c11 -c -o "$PKG/target/baga_wt_shim.o" "$PKG/shims/baga_wt_shim.c" \
    -I"$PKG/vendor/include" -I"$PKG/shims"
  "$BAGA" -I "$ROOT" -I "$ROOT/app-product" "$PKG/tests/smoke.baga"
fi

echo "wasmtimebaga run_test: all passed"
