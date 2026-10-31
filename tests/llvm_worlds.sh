#!/bin/bash
# LLVM оракул за светове: C бекенд срещу LLVM/lli върху СЪЩИЯ runtime.
# LLVM IR-ът вика baga_lw_* от lib/libbaga_world_rt.so (генериран от C
# preamble-а на codegen_api.c — виж Makefile world-rt), затова изходът,
# грешките и exit кодът трябва да съвпадат с ./baga. Всеки файл се пуска
# и под --rc (owned bytes/str от кеша/дампа тогава носят RC header).
#
# C-only проверката за bump арената е отделно в tests/world_bump_test.baga,
# затова пълният worlds_test върви и тук.
cd "$(dirname "$0")/.."
FAIL=0
LLI=${LLI:-$(for c in lli lli-19 lli-14; do command -v "$c" 2>/dev/null && break; done)}
[ -n "$LLI" ] || { echo "няма lli в PATH (apt install llvm-19)"; exit 1; }
for so in lib/libbaga_world_rt.so lib/libbaga_par.so; do
    [ -f "$so" ] || { echo "FAIL: $so липсва — пусни make llvm"; exit 1; }
done
echo "(lli: $LLI; world runtime: lib/libbaga_world_rt.so)"
FILES="tests/worlds_test.baga tests/wvec_test.baga tests/wmap_test.baga
       tests/wrec_test.baga tests/wcache_test.baga tests/world_block_test.baga
       tests/wchan_fanout_test.baga tests/wmet_test.baga"
for mode in "" "--rc"; do
    if [ -z "$mode" ]; then label=" (plain)"; else label=" (rc)"; fi
    for f in $FILES; do
        ./baga $mode -I app-product "$f" > /tmp/baga_worlds_c.out 2>&1; rc_c=$?
        if ! ./baga-llvm -I app-product --emit-llvm $mode "$f" > /tmp/baga_worlds.ll 2>/tmp/baga_worlds_err.txt; then
            echo "FAIL  $f$label (LLVM emit)"; cat /tmp/baga_worlds_err.txt; FAIL=1; continue
        fi
        "$LLI" -load lib/libbaga_world_rt.so -load lib/libbaga_par.so /tmp/baga_worlds.ll \
            > /tmp/baga_worlds_l.out 2>&1; rc_l=$?
        if [ $rc_c -ne $rc_l ] || ! diff -q /tmp/baga_worlds_c.out /tmp/baga_worlds_l.out > /dev/null; then
            echo "MISMATCH $f$label (exit C=$rc_c LLVM=$rc_l)"; FAIL=1
            diff -u /tmp/baga_worlds_c.out /tmp/baga_worlds_l.out | head -20
        else
            echo "OK    $f$label"
        fi
    done
done
[ $FAIL -eq 0 ] && echo "--- LLVM световете са в паритет ⚔️"
exit $FAIL
