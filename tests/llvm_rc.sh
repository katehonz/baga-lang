#!/bin/bash
# RC батерия под LLVM бекенда. Оракул: изходът на ./baga --rc (C) трябва
# да съвпада с ./baga-llvm --emit-llvm --rc + lli-14.
#
# Тестовете, които LLVM още не компилира (предсъществуващи дупки като
# byte_at, field-assign), се SKIP-ват само при честен отказ
# („неподдържан конструкт"); всяко друго LLVM compile фиаско е FAIL.
# Когато дупка се затвори, съответният тест автоматично „оживява" и
# започва да се diff-ва — скриптът не изисква ръчна промяна.
cd "$(dirname "$0")/.."
FAIL=0
PAR_SO=lib/libbaga_par.so
[ -f "$PAR_SO" ] || { mkdir -p lib; gcc -O2 -fPIC -shared -o "$PAR_SO" src/baga_par_rt.c -pthread || exit 1; }
TESTS="tests/rc_test.baga tests/temp_test.baga tests/move_test.baga tests/borrow_test.baga \
tests/cmove_test.baga tests/struct_rc_test.baga tests/enum_rc_test.baga tests/enum_box_rc_test.baga \
tests/nested_assign_rc_test.baga tests/calltemp_rc_test.baga tests/owned_ret_rc_test.baga \
tests/match_temp_rc_test.baga tests/vecvec_rc_test.baga tests/drop_llvm_test.baga \
tests/llvm_rc_vecmap_test.baga tests/llvm_rc_struct_test.baga tests/llvm_rc_struct2_test.baga"
for f in $TESTS; do
    [ -f "$f" ] || { echo "SKIP  $f (липсва)"; continue; }
    if ! ./baga --rc -I . -I app-product "$f" > /tmp/rc_c.txt 2>&1; then
        echo "SKIP  $f (C --rc не минава — не е LLVM проблем)"; continue
    fi
    if ! ./baga-llvm --emit-llvm --rc -I . -I app-product "$f" > /tmp/rc.ll 2>/tmp/rc_err.txt; then
        if grep -q "неподдържан конструкт" /tmp/rc_err.txt; then
            echo "SKIP  $f (предсъществуващо LLVM ограничение)"
        else
            echo "FAIL  $f (LLVM compile)"; cat /tmp/rc_err.txt; FAIL=1
        fi
        continue
    fi
    lli-14 -load "$PAR_SO" /tmp/rc.ll > /tmp/rc_l.txt 2>&1
    if diff -q /tmp/rc_c.txt /tmp/rc_l.txt > /dev/null; then echo "OK    $f"; else
        echo "MISMATCH $f"; diff -u /tmp/rc_c.txt /tmp/rc_l.txt | head -20; FAIL=1
    fi
done
[ $FAIL -eq 0 ] && echo "--- rc оракулът е доволен ⚔️"
exit $FAIL
