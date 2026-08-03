#!/bin/bash
# Docker smoke: локално "приложение" в temp git repo -> образ -> бинарникът работи.
# Toolchain-ът също идва от локалното repo (file://) — тества точно clone пътя.
set -eu
cd "$(dirname "$0")/../.."
command -v docker >/dev/null 2>&1 || { echo "SKIP: няма docker"; exit 0; }

T=$(mktemp -d); trap 'rm -rf "$T"' EXIT

# приложение: hello бинарник, kind = bin, без deps
mkdir -p "$T/helloapp"
cat > "$T/helloapp/sandak.toml" <<'EOF'
[package]
name = "helloapp"
version = "0.1.0"
entry = "main.baga"
kind = "bin"
EOF
cat > "$T/helloapp/main.baga" <<'EOF'
fn main() {
    print("docker-smoke-ok")
}
EOF
# lock за --locked: генерираме го с локалния sandak
SANDAK_BIN=$(pwd)/sandak
(cd "$T/helloapp" && "$SANDAK_BIN" fetch >/dev/null) \
  || { echo "FAIL: lock генериране"; exit 1; }
git -C "$T/helloapp" init -q -b main
git -C "$T/helloapp" add -A
git -C "$T/helloapp" -c user.email=t@t -c user.name=t commit -qm init

# Контейнерът не вижда host fs (file:// не работи), затова сервираме двата repo-та
# през git daemon (git://) — пак се тества точно clone пътя. Bare клонинг значи
# само КОМИТНАТО състояние — toolchain ref = текущия branch.
BAGA_REF=$(git rev-parse --abbrev-ref HEAD)
mkdir -p "$T/base"
git clone -q --bare "file://$PWD" "$T/base/baga.git"
git clone -q --bare "file://$T/helloapp" "$T/base/helloapp.git"
git daemon --export-all --reuseaddr --base-path="$T/base" --listen=0.0.0.0 --port=9418 &
DPID=$!
trap 'kill $DPID 2>/dev/null; rm -rf "$T"' EXIT

docker build -q \
  --add-host=host.docker.internal:host-gateway \
  --build-arg "BAGA_REPO=git://host.docker.internal/baga.git" \
  --build-arg "BAGA_REF=$BAGA_REF" \
  --build-arg "APP_REPO=git://host.docker.internal/helloapp.git" \
  --build-arg APP_REF=main \
  -t baga-smoke . > /dev/null
out=$(docker run --rm baga-smoke)
[ "$out" = "docker-smoke-ok" ] || { echo "FAIL: изход [$out]"; exit 1; }
echo "OK: docker smoke (git URL -> образ -> бинарник)"
