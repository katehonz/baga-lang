#!/usr/bin/env bash
# deps.sh — boilaDB layer-discipline gate (ARCHITECTURE.md §3).
#
# Layer ranks (one-way dependencies, no upward imports):
#   core(0) < storage(1) < txn(2) < catalog/index(3)
#   < fts/vector/ts/graph(4) < sql(5) < server(6) < api(7) < tools(8)
#
# server/ owns BoilaServer + multi-db registry + boila_server_exec
# (kimi-deps D2 — was wrongly in storage/, pulling txn/sql upward).
#
# An `import "../X/..."` is legal iff rank(X) < rank(self), OR both are
# same-layer siblings (catalog↔index — ARCHITECTURE §3 draws them co-level).
# Upward edges are violations unless listed in GRANDFATHER below.
# Grandfather must stay empty after kimi-deps D1–D5.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"

rank() {
  case "$1" in
    core) echo 0 ;;
    storage) echo 1 ;;
    txn) echo 2 ;;
    catalog | index) echo 3 ;;
    fts | vector | ts | graph) echo 4 ;;
    sql) echo 5 ;;
    server) echo 6 ;;
    api) echo 7 ;;
    tools) echo 8 ;;
    *) echo -1 ;;
  esac
}

# catalog and index are one layer (siblings may import each other).
siblings() {
  [[ "$1" == "catalog" || "$1" == "index" ]] && [[ "$2" == "catalog" || "$2" == "index" ]]
}

GRANDFATHER=(
)

declare -A grand=()
for e in "${GRANDFATHER[@]}"; do
  grand[$e]=1
done
declare -A seen=()
fail=0

# Emit "src tgt count" per cross-module dir pair over all .baga files.
pairs() {
  (cd "$ROOT" && grep -rn '^import "\.\./' --include='*.baga' . || true) |
    sed 's|^\./||' |
    awk -F: '{
      path = $1
      line = $0
      sub(/^[^:]*:[0-9]+:import "\.\.\//, "", line)
      sub(/\/.*$/, "", line)
      m = split(path, p, "/")
      if (m >= 2) print p[1] " " line
    }' | sort | uniq -c | awk '{print $2 " " $3 " " $1}'
}

while read -r src tgt cnt; do
  rs=$(rank "$src")
  rt=$(rank "$tgt")
  if [[ $rs -lt 0 || $rt -lt 0 ]]; then
    echo "deps: FAIL unknown layer dir in edge $src -> $tgt — assign it a rank in deps.sh"
    fail=1
    continue
  fi
  if [[ $rt -lt $rs ]]; then
    continue
  fi
  if [[ $rt -eq $rs ]] && siblings "$src" "$tgt"; then
    continue
  fi
  key="$src:$tgt"
  seen[$key]=$cnt
  if [[ -z "${grand[$key]:-}" ]]; then
    echo "deps: FAIL $src -> $tgt ($cnt imports) — upward/same-rank edge not in the grandfather list (ARCHITECTURE.md §3)"
    fail=1
  fi
done < <(pairs)

for e in "${GRANDFATHER[@]}"; do
  if [[ -z "${seen[$e]:-}" ]]; then
    echo "deps: FAIL stale grandfather entry $e — no live import matches; remove it from deps.sh"
    fail=1
  fi
done

if [[ $fail -ne 0 ]]; then
  exit 1
fi
echo "deps: OK — all cross-module imports go down the §3 layers; ${#GRANDFATHER[@]} grandfathered exceptions (target: 0)"
