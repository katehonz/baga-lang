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

# --- T3 R2 регресия: diamond — d се резолвира точно веднъж ---
mkdir -p "$T/dia/d" "$T/dia/b" "$T/dia/c" "$T/dia/root"
printf '[package]\nname = "d"\nversion = "0.1.0"\n' > "$T/dia/d/sandak.toml"
printf '[package]\nname = "b"\nversion = "0.1.0"\n[dependencies]\nd = { path = "../d" }\n' > "$T/dia/b/sandak.toml"
printf '[package]\nname = "c"\nversion = "0.1.0"\n[dependencies]\nd = { path = "../d" }\n' > "$T/dia/c/sandak.toml"
printf '[package]\nname = "root"\nversion = "0.1.0"\n[dependencies]\nb = { path = "../b" }\nc = { path = "../c" }\n' > "$T/dia/root/sandak.toml"
out=$(cd "$T/dia/root" && "$SANDAK" fetch) || fail "diamond fetch exit"
[ "$(echo "$out" | grep -c 'resolved: d ')" = "1" ] || fail "diamond: d не е резолвиран точно веднъж: [$out]"
echo "OK: diamond — споделена зависимост веднъж"

# --- T3 R2 регресия: dep директория без sandak.toml ---
mkdir -p "$T/noman/dep" "$T/noman/root"
printf '[package]\nname = "root"\nversion = "0.1.0"\n[dependencies]\ndep = { path = "../dep" }\n' > "$T/noman/root/sandak.toml"
(cd "$T/noman/root" && "$SANDAK" fetch) 2>"$T/err" && fail "липсващ манифест трябва да гърми"
grep -q "липсва манифест" "$T/err" || fail "липсващ манифест без съобщение: $(cat "$T/err")"
echo "OK: dep без sandak.toml — грешка"

# --- T4: lock файл ---
(cd "$T/ws/myapp" && "$SANDAK" fetch) > /dev/null || fail "fetch за lock"
[ -f "$T/ws/myapp/sandak.lock" ] || fail "sandak.lock не е създаден"
grep -q 'name = "mylib"' "$T/ws/myapp/sandak.lock" || fail "lock без mylib"
grep -q 'source = "path+' "$T/ws/myapp/sandak.lock" || fail "lock без source"
grep -q 'name = "myapp"' "$T/ws/myapp/sandak.lock" || fail "lock без root пакета"
echo "OK: sandak.lock се записва"

(cd "$T/ws/myapp" && "$SANDAK" fetch --locked) > /dev/null || fail "--locked при съвпадение"
echo "OK: --locked при съвпадение"

# разминаване: сменяме версията на mylib след lock
sed -i 's/0.1.0/0.2.0/' "$T/ws/mylib/sandak.toml"
(cd "$T/ws/myapp" && "$SANDAK" fetch --locked) 2>"$T/err" && fail "--locked трябва да гърми"
grep -q "lock" "$T/err" || fail "--locked без съобщение: $(cat "$T/err")"
sed -i 's/0.2.0/0.1.0/' "$T/ws/mylib/sandak.toml"
echo "OK: --locked при разминаване — грешка"

echo "sandak: всички тестове минаха"
