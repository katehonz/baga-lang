#!/usr/bin/env bash
# filesize-global.sh — глобален file-size gate (монорепото, извън boilaDB).
# Hard limit: 600 реда на .baga файл. boilaDB има свой по-строг gate
# (400, ARCHITECTURE.md §9) — scripts/filesize.sh в пакета, изключен тук.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
LIMIT=600
EXCLUDE="$ROOT/app-product/boilaDB"
fail=0

while IFS= read -r f; do
  case "$f" in
    "$EXCLUDE"/*) continue ;;
  esac
  n=$(wc -l < "$f")
  if [[ $n -gt $LIMIT ]]; then
    echo "filesize-global: FAIL ${f#"$ROOT"/} — $n реда (> $LIMIT)"
    fail=1
  fi
done < <(find "$ROOT" -name '*.baga' -type f -not -path '*/target/*' | sort)

if [[ $fail -ne 0 ]]; then
  exit 1
fi
echo "filesize-global: OK — всички .baga файлове (без boilaDB) са ≤ $LIMIT реда"
