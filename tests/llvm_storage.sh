#!/bin/bash
# LLVM паритет за rocksbaga + queuebaga (mem_* / nested field assignment).
cd "$(dirname "$0")/.."
FAIL=0
LLI=${LLI:-$(for c in lli lli-19 lli-14; do command -v "$c" 2>/dev/null && break; done)}
[ -n "$LLI" ] || { echo "няма lli в PATH"; exit 1; }
if [ ! -f app-product/rocksbaga/db/engine.baga ] || [ ! -f app-product/queuebaga/queue.baga ]; then
    echo "SKIP: rocksbaga/queuebaga submodule не е инициализиран"
    exit 0
fi
for so in lib/libbaga_world_rt.so lib/libbaga_par.so; do
    [ -f "$so" ] || { echo "FAIL: $so липсва — пусни make llvm"; exit 1; }
done
FILES="tests/lsm_test.baga tests/lsm_engine_test.baga tests/workers_test.baga
       tests/lsm_recover_test.baga tests/lsm_filter_test.baga tests/queue_test.baga"
echo "(lli: $LLI; rocksbaga/queuebaga × LLVM)"
for f in $FILES; do
    [ -f "$f" ] || continue
    timeout 300 ./baga -I . -I app-product "$f" > /tmp/baga_store_c.out 2>&1; rc_c=$?
    if ! ./baga-llvm -I app-product --emit-llvm "$f" > /tmp/baga_store.ll 2>/tmp/baga_store_err.txt; then
        echo "FAIL  $f (LLVM emit)"; cat /tmp/baga_store_err.txt; FAIL=1; continue
    fi
    timeout 300 "$LLI" -load lib/libbaga_world_rt.so -load lib/libbaga_par.so /tmp/baga_store.ll \
        > /tmp/baga_store_l.out 2>&1; rc_l=$?
    if [ $rc_c -ne $rc_l ] || ! diff -q /tmp/baga_store_c.out /tmp/baga_store_l.out > /dev/null; then
        echo "MISMATCH $f (exit C=$rc_c LLVM=$rc_l)"; FAIL=1
        diff -u /tmp/baga_store_c.out /tmp/baga_store_l.out | head -20
    else
        echo "OK    $f"
    fi
done
[ $FAIL -eq 0 ] && echo "--- storage/queue стекът върви и на LLVM ⚔️"
exit $FAIL
