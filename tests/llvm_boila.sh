#!/bin/bash
# LLVM паритет за boilaDB след W4.6/W4.7 (mem_* + nested field assignment).
# Пуска C и LLVM върху един и същ тест и сравнява изход + exit код.
cd "$(dirname "$0")/.."
FAIL=0
LLI=${LLI:-$(for c in lli lli-19 lli-14; do command -v "$c" 2>/dev/null && break; done)}
[ -n "$LLI" ] || { echo "няма lli в PATH"; exit 1; }
if [ ! -f app-product/boilaDB/tools/serve_pg.baga ]; then
    echo "SKIP: boilaDB submodule не е инициализиран"
    exit 0
fi
for so in lib/libbaga_world_rt.so lib/libbaga_par.so; do
    [ -f "$so" ] || { echo "FAIL: $so липсва — пусни make llvm"; exit 1; }
done
FILES="tests/boila_ts_test.baga tests/boila_pool_test.baga tests/boila_p5_test.baga
       tests/boila_http_mux_test.baga tests/boila_pgwire_test.baga"
echo "(lli: $LLI; boilaDB × LLVM)"
for f in $FILES; do
    [ -f "$f" ] || continue
    BOILA_PATH="/tmp/baga_boila_llvm_$$_c" timeout 300 ./baga -I . -I app-product "$f" \
        > /tmp/baga_boila_c.out 2>&1; rc_c=$?
    if ! ./baga-llvm -I app-product --emit-llvm "$f" > /tmp/baga_boila.ll 2>/tmp/baga_boila_err.txt; then
        echo "FAIL  $f (LLVM emit)"; cat /tmp/baga_boila_err.txt; FAIL=1; continue
    fi
    BOILA_PATH="/tmp/baga_boila_llvm_$$_l" timeout 300 "$LLI" \
        -load lib/libbaga_world_rt.so -load lib/libbaga_par.so /tmp/baga_boila.ll \
        > /tmp/baga_boila_l.out 2>&1; rc_l=$?
    if [ $rc_c -ne $rc_l ] || ! diff -q /tmp/baga_boila_c.out /tmp/baga_boila_l.out > /dev/null; then
        echo "MISMATCH $f (exit C=$rc_c LLVM=$rc_l)"; FAIL=1
        diff -u /tmp/baga_boila_c.out /tmp/baga_boila_l.out | head -20
    else
        echo "OK    $f"
    fi
done
[ $FAIL -eq 0 ] && echo "--- boilaDB върви и на LLVM ⚔️"
exit $FAIL
