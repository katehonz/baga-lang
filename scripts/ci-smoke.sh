#!/usr/bin/env bash
# ci-smoke.sh — GitHub Actions subset. Full suite is `make test` (Postgres, boila, all packages).
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
BIN="${BAGA:-$ROOT/baga}"
export BAGA="$BIN"
IFLAGS=(-I . -I app-product)

if [[ ! -x "$BIN" ]]; then
	echo "ci-smoke: missing $BIN (run make)" >&2
	exit 127
fi

run() { "$BIN" "${IFLAGS[@]}" "$@"; }

need() {
	if [[ ! -f "$1" ]]; then
		echo "ci-smoke: missing $1 (submodule not checked out)" >&2
		exit 1
	fi
}

need app-product/httpdbaga/http.baga
need app-product/jwtbaga/jwt.baga
need app-product/otpbaga/otp.baga

echo "=== examples ==="
run examples/zdravei.baga
run examples/faktorial.baga >/dev/null
run examples/bitwise.baga >/dev/null
echo "OK: examples"

echo "=== --check / --lib ==="
run --check app-product/httpdbaga/http.baga | grep -q "ok:"
run --lib app-product/jwtbaga/jwt.baga | grep -q "ok:"
echo "OK: --check httpdbaga, --lib jwtbaga"

echo "=== hmac / sha1 / jwt / otp ==="
run tests/std/hmac_test.baga
run tests/std/sha1_test.baga
run tests/jwt_test.baga
run tests/otp_test.baga

echo "=== filesize-global ==="
bash "$ROOT/scripts/filesize-global.sh"

echo "ci-smoke: OK"
