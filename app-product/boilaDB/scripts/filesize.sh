#!/usr/bin/env bash
# filesize.sh — boilaDB file-size gate (ARCHITECTURE.md §9).
# Hard limit: 400 lines per .baga file. Split preventively at ~350 —
# the barabadb lesson (parser.nim 1957 lines) is why this runs from P0,
# before there is anything to split.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
LIMIT=400
fail=0

while IFS= read -r f; do
  n=$(wc -l < "$f")
  if [[ $n -gt $LIMIT ]]; then
    echo "filesize: FAIL ${f#"$ROOT"/} — $n реда (> $LIMIT)"
    fail=1
  fi
done < <(find "$ROOT" -name '*.baga' -type f | sort)

if [[ $fail -ne 0 ]]; then
  exit 1
fi
echo "filesize: OK — всички .baga файлове в boilaDB са ≤ $LIMIT реда"
