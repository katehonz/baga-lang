#!/usr/bin/env bash
# Download Wasmtime C API prebuilt into vendor/ (wasmtime-go style).
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
VERSION="${WASMTIME_VERSION:-v47.0.2}"
# strip leading v for path segment if present
VER_NUM="${VERSION#v}"
ARCH="$(uname -m)"
case "$ARCH" in
  x86_64|amd64) TARGET="x86_64-linux" ;;
  aarch64|arm64) TARGET="aarch64-linux" ;;
  *)
    echo "fetch-wasmtime-c-api: unsupported arch $ARCH" >&2
    exit 1
    ;;
esac

URL="https://github.com/bytecodealliance/wasmtime/releases/download/v${VER_NUM}/wasmtime-v${VER_NUM}-${TARGET}-c-api.tar.xz"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

echo "fetch-wasmtime-c-api: $URL"
curl -fsSL "$URL" -o "$TMP/capi.tar.xz"
tar -xJf "$TMP/capi.tar.xz" -C "$TMP"
SRC="$(echo "$TMP"/wasmtime-v${VER_NUM}-*-c-api)"
if [[ ! -d "$SRC" ]]; then
  echo "fetch-wasmtime-c-api: unexpected archive layout" >&2
  ls -la "$TMP" >&2
  exit 1
fi

mkdir -p "$ROOT/vendor/lib"
rm -rf "$ROOT/vendor/include"
cp -a "$SRC/include" "$ROOT/vendor/"
# shared lib is enough for runtime; skip huge .a
cp -a "$SRC/lib/libwasmtime.so" "$ROOT/vendor/lib/"
echo "v${VER_NUM}" > "$ROOT/vendor/WASMTIME_VERSION"
echo "fetch-wasmtime-c-api: installed to $ROOT/vendor (Wasmtime v${VER_NUM})"
