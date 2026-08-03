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

# --- T3: резолвър ---
mkdir -p "$T/ws/mylib" "$T/ws/myapp"
cat > "$T/ws/mylib/sandak.toml" <<'EOF'
[package]
name = "mylib"
version = "0.1.0"
entry = "mylib.baga"
EOF
echo 'fn hi() -> i64 { return 7 }' > "$T/ws/mylib/mylib.baga"
cat > "$T/ws/myapp/sandak.toml" <<EOF
[package]
name = "myapp"
version = "0.1.0"
entry = "main.baga"
kind = "bin"

[dependencies]
mylib = { path = "../mylib" }
EOF
out=$(cd "$T/ws/myapp" && "$SANDAK" fetch) || fail "fetch exit"
echo "$out" | grep -q "resolved: mylib 0.1.0" || fail "fetch mylib: [$out]"
echo "$out" | grep -q "resolved: myapp 0.1.0" || fail "fetch myapp: [$out]"
echo "OK: резолвър path deps"

# цикъл: a -> b -> a
mkdir -p "$T/cyc/a" "$T/cyc/b"
printf '[package]\nname = "a"\nversion = "0.1.0"\n[dependencies]\nb = { path = "../b" }\n' > "$T/cyc/a/sandak.toml"
printf '[package]\nname = "b"\nversion = "0.1.0"\n[dependencies]\na = { path = "../a" }\n' > "$T/cyc/b/sandak.toml"
(cd "$T/cyc/a" && "$SANDAK" fetch) 2>"$T/err" && fail "цикълът трябва да гърми"
grep -q "цикъл" "$T/err" || fail "цикъл без съобщение: $(cat "$T/err")"
echo "OK: цикъл в зависимостите — грешка"

# дублирано име: два различни пакета с име dup
mkdir -p "$T/dup/x" "$T/dup/y" "$T/dup/root"
printf '[package]\nname = "dup"\nversion = "0.1.0"\n' > "$T/dup/x/sandak.toml"
printf '[package]\nname = "dup"\nversion = "0.2.0"\n' > "$T/dup/y/sandak.toml"
printf '[package]\nname = "root"\nversion = "0.1.0"\n[dependencies]\none = { path = "../x" }\ntwo = { path = "../y" }\n' > "$T/dup/root/sandak.toml"
(cd "$T/dup/root" && "$SANDAK" fetch) 2>"$T/err" && fail "дупликацията трябва да гърми"
grep -qE "дублирано име|не съвпада" "$T/err" || fail "дупликация без съобщение: $(cat "$T/err")"
echo "OK: дублирано име на пакет — грешка"

echo "sandak: всички тестове минаха"
