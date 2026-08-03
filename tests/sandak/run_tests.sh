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

# --- T5: git зависимост (локално repo, offline) ---
mkdir -p "$T/extlib"
cat > "$T/extlib/sandak.toml" <<'EOF'
[package]
name = "extlib"
version = "0.3.0"
entry = "extlib.baga"
EOF
echo 'fn ext_hi() -> i64 { return 9 }' > "$T/extlib/extlib.baga"
git -C "$T/extlib" init -q -b master
git -C "$T/extlib" add -A
git -C "$T/extlib" -c user.email=t@t -c user.name=t commit -qm init
git -C "$T/extlib" tag v1

mkdir -p "$T/gitapp"
cat > "$T/gitapp/sandak.toml" <<EOF
[package]
name = "gitapp"
version = "0.1.0"
entry = "main.baga"
kind = "bin"

[dependencies]
extlib = { git = "file://$T/extlib", branch = "master" }
EOF
echo 'import "extlib/extlib.baga"
fn main() { print(ext_hi()) }' > "$T/gitapp/main.baga"

out=$(cd "$T/gitapp" && "$SANDAK" fetch) || fail "git fetch exit"
echo "$out" | grep -q "resolved: extlib 0.3.0" || fail "git fetch extlib: [$out]"
[ -d "$T/gitapp/.sandak/cache" ] || fail "няма .sandak/cache"
grep -q 'source = "git+file://' "$T/gitapp/sandak.lock" || fail "lock без git source"
grep -q 'rev = "branch:master"' "$T/gitapp/sandak.lock" || fail "lock без git rev"
echo "OK: git зависимост се клонира и резолвира"

# второ fetch ползва кеша (без мрежа) — трябва да мине
out=$(cd "$T/gitapp" && "$SANDAK" fetch) || fail "git fetch от кеш"
echo "OK: git кеш се ползва повторно"

# --locked: сменен ref в манифеста срещу lock → грешка source/rev
sed -i 's/branch = "master"/tag = "v1"/' "$T/gitapp/sandak.toml"
(cd "$T/gitapp" && "$SANDAK" fetch --locked) 2>"$T/err" \
  && fail "--locked със сменен rev трябва да гърми"
grep -q "source/rev" "$T/err" || fail "--locked без source/rev съобщение: $(cat "$T/err")"
sed -i 's/tag = "v1"/branch = "master"/' "$T/gitapp/sandak.toml"
echo "OK: --locked при сменен git rev — грешка"

# --- T5: git зависимост със subdir ---
mkdir -p "$T/monorepo/subpkg"
cat > "$T/monorepo/subpkg/sandak.toml" <<'EOF'
[package]
name = "subpkg"
version = "1.0.0"
entry = "subpkg.baga"
EOF
echo 'fn sub_hi() -> i64 { return 5 }' > "$T/monorepo/subpkg/subpkg.baga"
git -C "$T/monorepo" init -q -b master
git -C "$T/monorepo" add -A
git -C "$T/monorepo" -c user.email=t@t -c user.name=t commit -qm init

mkdir -p "$T/subapp"
cat > "$T/subapp/sandak.toml" <<EOF
[package]
name = "subapp"
version = "0.1.0"
entry = "main.baga"
kind = "bin"

[dependencies]
subpkg = { git = "file://$T/monorepo", branch = "master", subdir = "subpkg" }
EOF
echo 'import "subpkg/subpkg.baga"
fn main() { print(sub_hi()) }' > "$T/subapp/main.baga"

out=$(cd "$T/subapp" && "$SANDAK" fetch) || fail "git subdir fetch exit"
echo "$out" | grep -q "resolved: subpkg 1.0.0" || fail "git subdir fetch: [$out]"
grep -q 'source = "git+file://' "$T/subapp/sandak.lock" || fail "subdir lock без git source"
echo "OK: git зависимост със subdir"

# --- T5 R1: git зависимост по rev (commit SHA) ---
mkdir -p "$T/extlib2"
cat > "$T/extlib2/sandak.toml" <<'EOF'
[package]
name = "extlib2"
version = "0.4.0"
entry = "extlib2.baga"
EOF
echo 'fn ext2_hi() -> i64 { return 11 }' > "$T/extlib2/extlib2.baga"
git -C "$T/extlib2" init -q -b master
git -C "$T/extlib2" add -A
git -C "$T/extlib2" -c user.email=t@t -c user.name=t commit -qm init
SHA=$(git -C "$T/extlib2" rev-parse HEAD)

mkdir -p "$T/revapp"
cat > "$T/revapp/sandak.toml" <<EOF
[package]
name = "revapp"
version = "0.1.0"
entry = "main.baga"
kind = "bin"

[dependencies]
extlib2 = { git = "file://$T/extlib2", rev = "$SHA" }
EOF
echo 'import "extlib2/extlib2.baga"
fn main() { print(ext2_hi()) }' > "$T/revapp/main.baga"

out=$(cd "$T/revapp" && "$SANDAK" fetch) || fail "git rev fetch exit"
echo "$out" | grep -q "resolved: extlib2 0.4.0" || fail "git rev fetch: [$out]"
[ -d "$T/revapp/.sandak/cache/extlib2-$SHA" ] || fail "няма cache dir за rev"
grep -q "rev = \"rev:$SHA\"" "$T/revapp/sandak.lock" || fail "lock без rev:<sha>"
echo "OK: git зависимост по rev (commit SHA)"

# несъществуващ rev: fetch гърми и НЕ оставя отровен cache dir
sed -i "s/rev = \"$SHA\"/rev = \"0000000000000000000000000000000000000000\"/" "$T/revapp/sandak.toml"
(cd "$T/revapp" && "$SANDAK" fetch) 2>"$T/err" && fail "лош rev трябва да гърми"
[ ! -d "$T/revapp/.sandak/cache/extlib2-0000000000000000000000000000000000000000" ] \
  || fail "отровен cache dir след провален rev fetch"
sed -i "s/rev = \"00*\"/rev = \"$SHA\"/" "$T/revapp/sandak.toml"
echo "OK: провален rev fetch не трови кеша"

# --- T5 R1: --locked при git→path смяна (source mismatch) ---
sed -i 's|extlib = .*|extlib = { path = "../extlib" }|' "$T/gitapp/sandak.toml"
(cd "$T/gitapp" && "$SANDAK" fetch --locked) 2>"$T/err" \
  && fail "--locked при git→path смяна трябва да гърми"
grep -q "source" "$T/err" || fail "--locked без source съобщение: $(cat "$T/err")"
sed -i "s|extlib = .*|extlib = { git = \"file://$T/extlib\", branch = \"master\" }|" "$T/gitapp/sandak.toml"
echo "OK: --locked при git→path смяна — грешка source"

# --- T6: build + run ---
# използваме $T/ws от T3 (myapp -> mylib); myapp/main.baga липсва още
cat > "$T/ws/myapp/main.baga" <<'EOF'
import "mylib/mylib.baga"

fn main() {
    print(hi())
}
EOF
out=$(cd "$T/ws/myapp" && "$SANDAK" build) || fail "build exit"
[ -x "$T/ws/myapp/target/myapp" ] || fail "няма target/myapp"
echo "OK: sandak build прави бинарник"

out=$(cd "$T/ws/myapp" && "$SANDAK" run) || fail "run exit"
[ "$out" = "7" ] || fail "run изход: [$out]"
echo "OK: sandak run"

# lib пакет: build = --lib проверка
out=$(cd "$T/ws/mylib" && "$SANDAK" build) || fail "lib build exit"
echo "$out" | grep -q "ok:" || fail "lib build изход: [$out]"
echo "OK: sandak build на библиотека (--lib)"

# gitapp от T5: build + run през git dep
out=$(cd "$T/gitapp" && "$SANDAK" run) || fail "gitapp run exit"
[ "$out" = "9" ] || fail "gitapp run изход: [$out]"
echo "OK: build/run през git зависимост"

echo "sandak: всички тестове минаха"
