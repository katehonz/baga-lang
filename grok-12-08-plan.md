# План за Grok — паметов модел на Baga (12.08)

## Контекст (къде сме)

Репо: `/home/ziko/z-git/baga`, branch `main`, синхронизиран с origin.
Компилатор: `./baga` (C backend). RC (refcount) паметов моделът е opt-in зад
флага `--rc`. Довършени етапи (последните два commit-а):

- `07a51dc` — RC3: container move (push/set без retain при last-use аргумент)
- `ae04302` — RC4: per-statement temporaries tracking + RC1.1/1.2/1.3 фиксове

Дизайн и честни граници: `docs/memory-rc-bg.md` (RC1, RC2.1, RC4) и
`docs/move-semantics-bg.md` (RC2, RC3). Чети ги преди всяка задача.

Ключови файлове: `src/codegen_c.c` (всичко е там), `include/baga.h` (Codegen
структурата). RC маркерите в кода са тагнати с коментари `RC1`…`RC4`.

## Контролни точки (задължителни след ВСЯКА промяна)

Пълен чеклист — не commit-вай без всичките:

1. `make` — чист build.
2. RC батерия: `rc_test move_test borrow_test cmove_test temp_test http_test
   pg_test std/sumtype_test mem_rewind_test` — всички с `--rc` PASS:
   `./baga --rc -I . -I app-product tests/<t>.baga`
3. ASan+UBSan върху същите: `--emit-c` → `gcc -g -O1
   -fsanitize=address,undefined -std=c11 -Iinclude -o /tmp/x out.c
   src/baga_par_rt.c -lm -pthread` → изпълни.
4. Бит-идентичен emit-c БЕЗ флаг срещу HEAD build (направи
   `git worktree add /tmp/baga_base <base-commit> && make -C /tmp/baga_base`
   и сравни `--emit-c` изхода на 5-6 файла с `cmp`).
5. Пълен пакет без флаг: `bash scripts/run_tests.sh` — единственият
   допустим FAIL е boilaDB filesize гейтът (pre-existing на HEAD).
6. Пълна tests/ батерия с --rc (~40 мин):
   `printf '#!/bin/sh\nexec '$PWD'/baga --rc "$@"\n' > /tmp/baga_rc &&
    chmod +x /tmp/baga_rc && BAGA=/tmp/baga_rc bash scripts/baga-test tests`
   Базова линия: **150/155**; 5-те FAIL са pre-existing external peers
   (oauth_pg, orm_boila, registry, https, tls_handshake).
   Никой нов FAIL не е приемлив. (`boila_ts_test` е зелен след RC1.4.)
7. Bench (само ако пипаш retain/release пътеките): boilaDB 100k insert —
   `BOILA_PHASE=write BOILA_CHUNKS=1 BOILA_ROWS=100000
   BOILA_BENCH_ROOT=/tmp/boila_x ./baga --rc -I . -I app-product
   bench/boila/insert_write.baga` (изтрий root-а преди това), после
   `BOILA_PHASE=verify` — трябва `DURABLE OK`. База: 415 µs/ред, RSS 1.76 GB.
   Внимание: `/usr/bin/time` го няма — мери с python wrapper
   (`resource.getrusage`) или вътрешната ns/ред метрика на bench-а.

## Задачи (по приоритет)

### 1. boila_ts_test FPE под --rc — ГОТОВО (RC1.4)
Не беше деление в time_bucket. `boila_open_mt` прави `map_h(mus)` /
`map_h(dbs)` и връща само i64 handle в struct-а; под --rc scope exit
release-ваше картата → `h_map` върху труп с `nb=0` → SIGFPE в
`baga_map_slot` при `boila_shard_lock`. Фикс: `map_h`/`str_h`/`bytes_h`
retain-ват (immortal escape, leak-safe). Тест: `map_h_survives` в
`rc_test`. Пълна --rc батерия: **150/155** (само 5-те external peers).

### 2. v0.3: temp-ове в условия и for_iter (RC4 довършване)
RC4 не покрива temp-ове в `while`/`for`/`if` условия и `for x in <expr>`
(консервативно изключени — hoisting над цикъл би оценил веднъж вместо на
итерация). Това е оставащият leak в cond-тежки цикли
(`while str_find(...) >= 0` с temp в аргументите). Подход: temp-овете в
условие на цикъл се release-ват В КРАЯ НА ВСЯКА итерация (не преди цикъла) —
демек декларациите отиват вътре в тялото/края на итерацията, не преди
while-а. За `if` условия — след оценката, преди клоновете. Внимавай с
`break`/`continue` (release преди скока, както rc_release_to_loop).
Критерий: leak repro с temp в while-условие дава равен RSS; чеклистът.
Дизайнът се дописва в `docs/memory-rc-bg.md` §v0.2 (в момента пише, че е
изключено — обнови текста).

### 3. go/chan прехвърляне на heap стойности
`go`/chan на heap стойности е изрично непокрито (docs/memory-rc-bg.md
§Ограничения). Проучи `baga_chan_send/recv` пътя в codegen_c.c (~линия
4560+ в runtime emission-а) и направи retain при send / release при recv
след консумация. По-малко ясна задача — първо напиши design бележка в
docs/ и я обсъди преди имплементация.

### 4. Struct полета като собственици (голяма, само ако 1-3 са готови)
Struct полетата не се track-ват (shared-pointer семантика) — това е
фундаменталната оставаща граница. Изисква drop-ове за struct-ове по
стойност при scope exit + retain дисциплина при копиране. Не започвай без
отделен design документ и без да са зелени 1-3.

## Забранено / внимание

- Не пипай нищо извън `if (cg->rc)` клонове — emit-c без флаг трябва да
  остане бит-идентичен.
- Посоката на грешка е винаги leak-safe (leak > корупция). Underflow
  (`rc==0` при release) е чиста грешка и ТРЯБВА да остане фатална.
- LLVM backend-ът не поддържа persist/RC — не го пипай.
- Комит съобщенията са на български/английски микс както досега
  (`feat(runtime): … зад --rc`). Push-вай към `origin main` само след
  пълния чеклист.
