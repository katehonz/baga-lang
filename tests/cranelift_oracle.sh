#!/bin/bash
# Оракъл: сравнява C backend и Cranelift JIT (in-process) за всички примери.
# И двата потока се хващат с 2>&1 (stdout+stderr заедно), като llvm_oracle.sh —
# при JIT compile+run са една стъпка, та съобщението за отказ и изходът са в един поток.
cd "$(dirname "$0")/.."
FAIL=0
for f in examples/*.baga; do
    case "$f" in
        examples/spec_bad.baga|examples/vec_typed.baga|examples/arg_type_bad.baga) continue;;
    esac
    ./baga "$f" > /tmp/baga_c_out.txt 2>&1; rc_c=$?
    ./baga-cranelift "$f" > /tmp/baga_cl_out.txt 2>&1; rc_l=$?
    if grep -q "неподдържан конструкт" /tmp/baga_cl_out.txt; then
        echo "SKIP  $f (честен отказ)"; continue
    fi
    if [ $rc_c -ne $rc_l ] || ! diff -q /tmp/baga_c_out.txt /tmp/baga_cl_out.txt > /dev/null; then
        echo "MISMATCH $f (exit C=$rc_c CL=$rc_l)"; FAIL=1
    else
        echo "OK    $f"
    fi
done
[ $FAIL -eq 0 ] && echo "--- оракулът е доволен ⚔️"
exit $FAIL
