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

# --- T2 R1 регресия: няколко зависимости + редове след тях (strtok_r) ---
mkdir "$T/multi"
cat > "$T/multi/sandak.toml" <<'EOF'
[package]
name = "multi"
version = "0.1.0"

[dependencies]
aaa = { path = "../aaa" }
bbb = { git = "https://example.com/bbb.git", tag = "v1.0" }
ccc = { git = "https://example.com/ccc.git", subdir = "lib" }
# редове след зависимостите — не трябва да се губят
[dependencies]
ddd = { path = "../ddd" }
EOF
out=$(cd "$T/multi" && "$SANDAK" manifest) || fail "multi manifest exit"
exp='name=multi
version=0.1.0
kind=lib
dep=aaa path=../aaa
dep=bbb git=https://example.com/bbb.git
dep=ccc git=https://example.com/ccc.git
dep=ddd path=../ddd'
[ "$out" = "$exp" ] || fail "multi-dep manifest изход: [$out]"
echo "OK: много зависимости — нищо не се губи (strtok_r)"

echo "sandak: всички тестове минаха"
