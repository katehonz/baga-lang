#!/bin/bash
# Оракъл: сравнява C backend и LLVM backend (lli-14) за всички примери.
cd "$(dirname "$0")/.."
FAIL=0
for f in examples/*.baga; do
    [ "$f" = "examples/spec_bad.baga" ] && continue     # очаквана compile грешка
    [ "$f" = "examples/vec_typed.baga" ] && continue    # очаквана compile грешка
    [ "$f" = "examples/arg_type_bad.baga" ] && continue # очаквана compile грешка
    ./baga "$f" > /tmp/baga_c_out.txt 2>&1; rc_c=$?
    if ! ./baga-llvm --emit-llvm "$f" > /tmp/baga.ll 2>/tmp/baga_ll_err.txt; then
        if grep -q "неподдържан конструкт" /tmp/baga_ll_err.txt; then
            echo "SKIP  $f (честен отказ)"
        else
            echo "FAIL  $f (LLVM crash без грешка)"; FAIL=1
        fi
        continue
    fi
    lli-14 /tmp/baga.ll > /tmp/baga_llvm_out.txt 2>&1; rc_l=$?
    if [ $rc_c -ne $rc_l ] || ! diff -q /tmp/baga_c_out.txt /tmp/baga_llvm_out.txt > /dev/null; then
        echo "MISMATCH $f (exit C=$rc_c LLVM=$rc_l)"; FAIL=1
    else
        echo "OK    $f"
    fi
done
[ $FAIL -eq 0 ] && echo "--- оракулът е доволен ⚔️"
exit $FAIL
