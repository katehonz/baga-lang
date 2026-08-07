#!/usr/bin/env bash
# self_parity.sh — LP7: self-hosting паритет (guard).
#
# Инвариант 1 (soundness): НИКОЙ пример не работи поведенчески различно
#   под self компилатора (baga2) спрямо C bootstrap-а (baga).
# Инвариант 2 (монотонно покритие): примерите от списъка PARITY остават
#   PARITY — покритието на self компилатора не се свива.
#
# Примери извън списъка могат честно да гърмят в self пътя (loud gcc
# грешки за L3–L6/!Par синтаксис извън покритието — виж
# idea/lang-probes.md LP7); ако един ден минат на PARITY, скриптът не се
# чупи — само инвариант 1 важи за тях.
set -u

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
BIN="${BAGA:-./baga}"
BAGAIFLAGS="${BAGAIFLAGS:--I . -I app-product}"

PARITY_LIST="arena.baga argv.baga bitwise.baga bytes.baga cvet.baga effects.baga extern_write.baga faktorial.baga fib.baga interp.baga match.baga spec.baga spec_ensures.baga spec_ensures_fail.baga spec_requires_fail.baga strings.baga tochka.baga types.baga vec_ann.baga vec.baga vec_f64.baga zdravei.baga"

# baga2 (self компилаторът като еймитър на C) — построй го, ако липсва.
if [ ! -x /tmp/baga2 ]; then
	"$BIN" $BAGAIFLAGS --emit-c self/compiler.baga > /tmp/lp7_baga2.c || exit 1
	gcc -O2 -o /tmp/baga2 /tmp/lp7_baga2.c -lm -pthread || exit 1
fi

FAIL=0
declare -A state
for name in $PARITY_LIST; do state[$name]=0; done

for f in examples/*.baga; do
	b=$(basename "$f")
	case "$b" in spec_bad.baga|vec_typed.baga|arg_type_bad.baga) continue;; esac
	"$BIN" "$f" > /tmp/lp7_c.txt 2>&1; rc_c=$?
	if ! /tmp/baga2 "$f" > /tmp/lp7_s.c 2>/dev/null; then
		if [ -n "${state[$b]:-}" ] && [ "${state[$b]}" != "2" ]; then
			echo "FAIL: $b — беше PARITY, вече гърми в self компилатора"; FAIL=1
		fi
		continue
	fi
	if ! gcc -O2 -o /tmp/lp7_s_bin /tmp/lp7_s.c -lm -pthread 2>/dev/null; then
		if [ -n "${state[$b]:-}" ] && [ "${state[$b]}" != "2" ]; then
			echo "FAIL: $b — беше PARITY, вече гърми при gcc на self изхода"; FAIL=1
		fi
		continue
	fi
	/tmp/lp7_s_bin > /tmp/lp7_s.txt 2>&1; rc_s=$?
	if [ $rc_c -eq $rc_s ] && diff -q /tmp/lp7_c.txt /tmp/lp7_s.txt > /dev/null; then
		if [ -n "${state[$b]:-}" ]; then state[$b]=2; fi
	else
		echo "FAIL: $b — поведенческа дивергенция C срещу self (soundness!)"
		diff /tmp/lp7_c.txt /tmp/lp7_s.txt | head -10
		FAIL=1
	fi
done

for name in $PARITY_LIST; do
	if [ "${state[$name]}" != "2" ]; then
		echo "FAIL: $name — вече не е PARITY"
		FAIL=1
	fi
done

# LP7: чисто language-ниво отказване на неподдържания синтаксис
# (сум enum-и с payload-и, fn стойности/ламбди, !Par concurrency)
for name in sum_enum.baga closures.baga par.baga; do
	if /tmp/baga2 "examples/$name" 2>&1 >/dev/null | grep -q "неподдържан конструкт в self компилатора"; then
		echo "OK: self отказва чисто $name (не счупен C)"
	else
		echo "FAIL: $name — очаквах чисто language-ниво отказване"
		FAIL=1
	fi
done

if [ $FAIL -eq 0 ]; then
	echo "OK: self-hosting паритет — 22 PARITY, без поведенческа дивергенция (LP7)"
else
	exit 1
fi
