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
# Предпоставки: и двата компилатора + lli. Без тях всичко би SKIP-нало
# зелено (bash exit 127 при липсващ ./baga) — фатално, не SKIP.
[ -x ./baga ] || { echo "FAIL: ./baga липсва/не е изпълним — пусни make (или make test-llvm-rc)"; exit 1; }
[ -x ./baga-llvm ] || { echo "FAIL: ./baga-llvm липсва/не е изпълним — пусни make llvm"; exit 1; }
command -v lli-14 > /dev/null || { echo "FAIL: lli-14 не е в PATH"; exit 1; }
PAR_SO=lib/libbaga_par.so
[ -f "$PAR_SO" ] || { mkdir -p lib; gcc -O2 -fPIC -shared -o "$PAR_SO" src/baga_par_rt.c -pthread || exit 1; }
TESTS="tests/rc_test.baga tests/temp_test.baga tests/move_test.baga tests/borrow_test.baga \
tests/cmove_test.baga tests/struct_rc_test.baga tests/enum_rc_test.baga tests/enum_box_rc_test.baga \
tests/nested_assign_rc_test.baga tests/calltemp_rc_test.baga tests/owned_ret_rc_test.baga \
tests/match_temp_rc_test.baga tests/vecvec_rc_test.baga tests/drop_llvm_test.baga \
tests/llvm_rc_vecmap_test.baga tests/llvm_rc_struct_test.baga tests/llvm_rc_struct2_test.baga \
tests/catch_rc_test.baga tests/interp_rc_test.baga tests/raise_rc_test.baga \
tests/generic_struct_rc_test.baga tests/generic_fn_value_test.baga"
for f in $TESTS; do
    [ -f "$f" ] || { echo "SKIP  $f (липсва)"; continue; }
    # SKIP при C грешка е съзнателен дизайн: оракулът е C страната, затова
    # счупен C run („не е LLVM проблем") не е в юрисдикцията на този скрипт.
    # Пазим точния exit code, за да го сравним с LLVM страната (crash след
    # пълен изход или тест без изход не трябва да мине като OK).
    # BAGA_CFLAGS=-w: gcc warning-ите от генерирания C (напр. указателното
    # сравнение при match върху str, `_mv == "lit"`) не са част от семантиката
    # на програмата, но 2>&1 ги слива в изхода и diff-ът би ги видял.
    BAGA_CFLAGS="-w" ./baga --rc -I . -I app-product "$f" > /tmp/rc_c.txt 2>&1; rc_c=$?
    if [ $rc_c -ne 0 ]; then
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
    lli-14 -load "$PAR_SO" /tmp/rc.ll > /tmp/rc_l.txt 2>&1; rc_l=$?
    if [ $rc_c -ne $rc_l ] || ! diff -q /tmp/rc_c.txt /tmp/rc_l.txt > /dev/null; then
        echo "MISMATCH $f (exit C=$rc_c LLVM=$rc_l)"
        diff -u /tmp/rc_c.txt /tmp/rc_l.txt | head -20; FAIL=1
    else
        echo "OK    $f"
    fi
done
[ $FAIL -eq 0 ] && echo "--- rc оракулът е доволен ⚔️"
exit $FAIL
