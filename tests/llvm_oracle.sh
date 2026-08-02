#!/bin/bash
# Оракъл: сравнява C backend и LLVM backend (lli-14) за всички примери.
# !Par helpers live in lib/libbaga_par.so (src/baga_par_rt.c) and are loaded
# into lli so go/join/chan match the C backend.
cd "$(dirname "$0")/.."
FAIL=0
PAR_SO=lib/libbaga_par.so
if [ ! -f "$PAR_SO" ]; then
    mkdir -p lib
    gcc -O2 -fPIC -shared -o "$PAR_SO" src/baga_par_rt.c -pthread || exit 1
fi
for f in examples/*.baga; do
    [ "$f" = "examples/spec_bad.baga" ] && continue     # очаквана compile грешка
    [ "$f" = "examples/vec_typed.baga" ] && continue    # очаквана compile грешка
    [ "$f" = "examples/arg_type_bad.baga" ] && continue # очаквана compile грешка
    ./baga "$f" > /tmp/baga_c_out.txt 2>&1; rc_c=$?
    if ! ./baga-llvm --emit-llvm "$f" > /tmp/baga.ll 2>/tmp/baga_ll_err.txt; then
        if grep -q "неподдържан конструкт" /tmp/baga_ll_err.txt; then
            echo "SKIP  $f (честен отказ)"
        else
            echo "FAIL  $f (LLVM crash без грешка)"; cat /tmp/baga_ll_err.txt; FAIL=1
        fi
        continue
    fi
    # Load par runtime for !Par symbols; harmless for pure programs.
    lli-14 -load "$PAR_SO" /tmp/baga.ll > /tmp/baga_llvm_out.txt 2>&1; rc_l=$?
    if [ $rc_c -ne $rc_l ] || ! diff -q /tmp/baga_c_out.txt /tmp/baga_llvm_out.txt > /dev/null; then
        echo "MISMATCH $f (exit C=$rc_c LLVM=$rc_l)"; FAIL=1
        diff -u /tmp/baga_c_out.txt /tmp/baga_llvm_out.txt | head -20
    else
        echo "OK    $f"
    fi
done
[ $FAIL -eq 0 ] && echo "--- оракулът е доволен ⚔️"
exit $FAIL
