#!/bin/bash
# LLVM parity за pure app-product пакетите (codec/std-ish stack).
# Пуска C и LLVM върху един и същ тест и сравнява изход + exit код.
# Ако app-product submodule-ите липсват (CI llvm job), просто skip-ва.
cd "$(dirname "$0")/.."
FAIL=0
LLI=${LLI:-$(for c in lli lli-19 lli-14; do command -v "$c" 2>/dev/null && break; done)}
[ -n "$LLI" ] || { echo "няма lli в PATH (apt install llvm-19)"; exit 1; }
if [ ! -f app-product/metbaga/met.baga ]; then
    echo "SKIP: app-product submodules не са инициализирани"
    exit 0
fi
for so in lib/libbaga_world_rt.so lib/libbaga_par.so; do
    [ -f "$so" ] || { echo "FAIL: $so липсва — пусни make llvm"; exit 1; }
done

FILES="buf chrono csv flag glob img jwt office otp path pb pdf smtp tpl uuid xml zip met wmet jsonrpc"
echo "(lli: $LLI; pure app-product stack)"
for t in $FILES; do
    f="tests/${t}_test.baga"
    [ -f "$f" ] || continue
    ./baga -I app-product "$f" > /tmp/baga_pure_c.out 2>&1; rc_c=$?
    if grep -q "не мога да намеря import" /tmp/baga_pure_c.out; then
        echo "SKIP  $f (липсва submodule)"; continue
    fi
    if ! ./baga-llvm -I app-product --emit-llvm "$f" > /tmp/baga_pure.ll 2>/tmp/baga_pure_err.txt; then
        echo "FAIL  $f (LLVM emit)"; cat /tmp/baga_pure_err.txt; FAIL=1; continue
    fi
    timeout 240 "$LLI" -load lib/libbaga_world_rt.so -load lib/libbaga_par.so /tmp/baga_pure.ll \
        > /tmp/baga_pure_l.out 2>&1; rc_l=$?
    if [ $rc_c -ne $rc_l ] || ! diff -q /tmp/baga_pure_c.out /tmp/baga_pure_l.out > /dev/null; then
        echo "MISMATCH $f (exit C=$rc_c LLVM=$rc_l)"; FAIL=1
        diff -u /tmp/baga_pure_c.out /tmp/baga_pure_l.out | head -20
    else
        echo "OK    $f"
    fi
done
[ $FAIL -eq 0 ] && echo "--- pure app-product стекът е в LLVM паритет ⚔️"
exit $FAIL
