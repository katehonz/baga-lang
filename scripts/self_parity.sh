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

PARITY_LIST="arena.baga argv.baga bitwise.baga bytes.baga bytes_edges.baga bytes_handle.baga clo_capture.baga closures.baga cvet.baga drop.baga effects.baga effects_payload.baga effects_probe.baga effects_raise_diverge.baga extern_write.baga f64_surface.baga faktorial.baga fib.baga generic_nested.baga generic_structs.baga generics.baga handles.baga interp.baga map.baga map_enum.baga match.baga mem.baga nested_enum.baga par.baga par_chan.baga par_pool.baga par_select.baga runtime_edges.baga signal.baga spec.baga spec_ensures.baga spec_ensures_fail.baga spec_requires_fail.baga strings.baga struct_enum_field.baga sum_enum.baga tail_return.baga tochka.baga traits.baga types.baga vec_ann.baga vec.baga vec_f64.baga vec_nested.baga vec_struct.baga zdravei.baga"

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

# LP8-E: par* (go/join/chan/select/pool) вече са в PARITY_LIST; чисти
# language-ниво откази остават само за конструкти извън всяко покритие
# (виж idea/lang-probes.md LP8). Секцията за очаквани откази е празна.

if [ $FAIL -eq 0 ]; then
	echo "OK: self-hosting паритет — 51 PARITY, без поведенческа дивергенция (LP7+LP8)"
else
	exit 1
fi
