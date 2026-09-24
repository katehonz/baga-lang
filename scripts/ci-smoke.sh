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
need app-product/smtpbaga/smtp.baga
need app-product/7x7office/secp/idm/mail_text.baga
need app-product/7x7office/secp/idm/ws_roles.baga

echo "=== examples ==="
run examples/zdravei.baga
run examples/faktorial.baga >/dev/null
run examples/bitwise.baga >/dev/null
echo "OK: examples"

echo "=== --check / --lib ==="
run --check app-product/httpdbaga/http.baga | grep -q "ok:"
run --lib app-product/jwtbaga/jwt.baga | grep -q "ok:"
# -I flags must precede --check/--lib (otherwise the next token is the file).
echo "OK: --check httpdbaga, --lib jwtbaga"

echo "=== hmac / sha1 / jwt / otp / smtp ==="
run tests/std/hmac_test.baga
run tests/std/sha1_test.baga
run tests/jwt_test.baga
run tests/otp_test.baga
run tests/smtp_test.baga

echo "=== mail (Фаза 6.3: reset токени, payload, текстове) ==="
run -I app-product/7x7office/secp tests/mail_test.baga

echo "=== workspaces (Фаза 2: роли, нива, slug) ==="
run -I app-product/7x7office/secp tests/ws_roles_test.baga

# Интеграционният тест за файлове по пространство иска жив Postgres и
# построен secp — в CI средата ги няма (виж scripts/run_tests.sh, където се
# пуска при наличен target/secp). Тук само проверяваме, че не е изчезнал.
need app-product/7x7office/secp/tools/ws_files_smoke.sh

echo "=== filesize-global ==="
bash "$ROOT/scripts/filesize-global.sh"

echo "ci-smoke: OK"
