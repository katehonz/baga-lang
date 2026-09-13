#!/usr/bin/env bash
# neg_oracle.sh — LP11: negative-test оракул (двустранен parity).
#
# Проблемът, който решава: self_parity.sh сравнява само ПРИЕТИ програми
# (поведенческа дивергенция). Ако bootstrap-ът отхвърли програма, а self
# я приеме, оракулът не вижда нищо — а това е също толкова лошо: една и
# съща програма се компилира под единия компилатор и не под другия.
#
# Инвариант: всяка програма в tests/neg/ трябва да бъде ОТХВЪРЛЕНА
# и от bootstrap-а (./baga), И от self компилатора (/tmp/baga2).
# Ако някой от двамата я приеме → FAIL (тиха дивергенция на отказите).
set -u

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
BIN="${BAGA:-./baga}"
BAGAIFLAGS="${BAGAIFLAGS:--I . -I app-product}"

# baga2 (self компилаторът) — построй го, ако липсва.
if [ ! -x /tmp/baga2 ]; then
	"$BIN" $BAGAIFLAGS --emit-c self/compiler.baga > /tmp/lp7_baga2.c || exit 1
	gcc -O2 -o /tmp/baga2 /tmp/lp7_baga2.c -lm -pthread || exit 1
fi

FAIL=0
TOTAL=0
for f in tests/neg/*.baga; do
	[ -e "$f" ] || continue
	b=$(basename "$f")
	TOTAL=$((TOTAL + 1))
	ok=1

	# bootstrap трябва да отхвърли
	if "$BIN" $BAGAIFLAGS "$f" > /tmp/neg_b.txt 2>&1; then
		echo "FAIL: $b — bootstrap-ът я ПРИЕ (трябва да отхвърли)"
		ok=0
	fi

	# self трябва да отхвърли
	if /tmp/baga2 "$f" > /tmp/neg_s.c 2>&1; then
		echo "FAIL: $b — self компилаторът я ПРИЕ (трябва да отхвърли)"
		ok=0
	fi

	[ $ok -eq 1 ] || FAIL=1
done

if [ "$TOTAL" -eq 0 ]; then
	echo "FAIL: няма negative примери в tests/neg/"
	exit 1
fi

if [ $FAIL -eq 0 ]; then
	echo "OK: negative оракул — $TOTAL примера, отхвърлени И от двата компилатора (LP11)"
else
	exit 1
fi
