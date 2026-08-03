#!/bin/bash
# sandak тестове — всяка проверка печати "OK: <име>" или излиза с FAIL
set -u
cd "$(dirname "$0")/../.."
SANDAK=$(realpath "${SANDAK:-./sandak}")
export BAGA=${BAGA:-$(pwd)/baga}
T=$(mktemp -d); trap 'rm -rf "$T"' EXIT
fail() { echo "FAIL: $1"; exit 1; }

# --- T2: манифест парсер ---
out=$(cd tests/sandak/fixtures/basic && "$SANDAK" manifest) \
  || fail "manifest exit"
exp='name=greeter
version=0.1.0
entry=greeter.baga
kind=lib
dep=std path=../std'
[ "$out" = "$exp" ] || fail "manifest изход: [$out]"
echo "OK: манифест парсер"

(cd tests/sandak/fixtures/bad_toml && "$SANDAK" manifest) 2>"$T/err" \
  && fail "bad_toml трябва да гърми"
grep -q "sandak:" "$T/err" || fail "bad_toml без съобщение"
echo "OK: счупен манифест — грешка"

echo "sandak: всички тестове минаха"
