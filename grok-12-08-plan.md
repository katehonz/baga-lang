# План за Grok — паметов модел на Baga (12.08)

## Контекст (къде сме)

Репо: `/home/ziko/z-git/baga`, branch `main`, синхронизиран с origin.
Компилатор: `./baga` (C backend). RC (refcount) паметов моделът е opt-in зад
флага `--rc`. Довършени етапи: RC1–RC4, RC5 v0.1–v0.11, **v1.0a+v1.0b**
(owned-конвенция за struct/enum fn резултати + call-site move). Пълната
история е в `CHANGELOG.md` и т.4–5 по-долу.

Дизайн и честни граници: `docs/memory-rc-bg.md` (RC1, RC2.1, RC4) и
`docs/move-semantics-bg.md` (RC2, RC3), `docs/memory-rc-struct-bg.md`
(RC5 v0.1–v0.5, v0.7–v0.9), `docs/memory-rc-enum-bg.md` (v0.6, v0.10,
v0.11). Чети ги преди всяка задача.

Ключови файлове: `src/codegen_c.c` (всичко е там), `include/baga.h` (Codegen
структурата). RC маркерите в кода са тагнати с коментари `RC1`…`RC5 vX.Y`.

## Контролни точки (задължителни след ВСЯКА промяна)

Пълен чеклист — не commit-вай без всичките:

1. `make` — чист build.
2. RC батерия: `rc_test move_test borrow_test cmove_test temp_test
   struct_rc_test enum_rc_test calltemp_rc_test nested_assign_rc_test
   vecvec_rc_test enum_box_rc_test match_temp_rc_test http_test pg_test
   std/sumtype_test mem_rewind_test` — всички с `--rc` PASS:
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
   Базова линия: **158/163**; 5-те FAIL са pre-existing external peers
   (oauth_pg, orm_boila, registry, https, tls_handshake).
   Никой нов FAIL не е приемлив. (`owned_ret_rc_test` е новият +1 след v1.0a.)
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

### 2. v0.3: temp-ове в условия и for_iter — ГОТОВО
GNU `({ tmp; c = cond; release; c; })` wrap на if/while cond и for-range
hi (lo — hoist веднъж). Release веднага след оценката, преди тялото —
break/continue не пипат cond temp-ове. Leak repro 200k `while
str_find(concat(...))`: 10 MB с --rc vs 141 MB без. Тестове в temp_test.

### 3. go/chan прехвърляне на heap стойности — РЕШЕНО A
Бележка: `docs/memory-rc-chan-bg.md`. Каналът е i64-only; heap минава
през `*_h`. RC1.4 вече retain-ва handle-а (immortal, leak-safe).
**A:** без нова работа. B/C отложени — пипат типове/capture без печалба
за v0.x (leak > корупция вече е спазено).

### 4. Struct полета като собственици — ГОТОВО v0.1 (RC5) + v0.2
Design: `docs/memory-rc-struct-bg.md`. Tag 5 + `retain_S`/`release_S` за
преки heap полета. Регистрира се само свеж литерал или alias на track-нат
struct (`vec_get`/`f()` не — иначе commit underflow). `s = f(s)` не
пуска стария. Тест: `tests/struct_rc_test.baga`. v0.2: drop на
`Vec<S>`/`Map<K,S>` release-ва полетата на box елементите (destructor fn
pointer `elem_rel`/`val_rel` + shim `baga_rc_relf_S`); push/set на свеж
struct литерал е move (RSS 64 MB → 10.9 MB на 500k push+drop). v0.3:
`vec_set`/`map_set` overwrite и `map_del` release-ват стария box
(`*_box_rc`/`baga_map_del_*_rc` с destructor fn pointer; RSS 72 MB →
10.9 MB на 300k overwrite+del). v0.4: release на старото heap поле при
`s.f = x` (alias-safe: retain преди release; RSS 39 MB → 10.9 MB на 500k
field overwrite). v0.5: вложени struct-и — транзитивен `has_heap`,
`retain_S`/`release_S` рекурсират, литералът retain-ва borrowed вложени
полета (RSS 38.5 MB → 10.8 MB на 500k вложени литерала). v0.6: enum
payload-и — tag 6 + `retain_E`/`release_E` по runtime tag, ctor с
owned/borrowed/move payload (`docs/memory-rc-enum-bg.md`,
`tests/enum_rc_test.baga`; RSS 39.6 MB → 10.9 MB). v0.7: call temp-ове в
box push — temp аргумент (str/bytes/Vec) на push/set/map_set е move в
контейнера без retain/release двойка (`tests/calltemp_rc_test.baga`;
struct/enum box temp-ове остават с retain — резултатът може да е borrowed,
leak-safe). v0.8: `s.inner = x` — struct-типизирана цел на field assign:
release_<Inner> на старото поле преди assign, retain на новото при
borrowed/неразличим източник (call резултат — §v0.7 границата), move при
last-use ident, owned при свеж литерал; alias-safe ред (retain преди
release; `tests/nested_assign_rc_test.baga`; RSS 24.9 MB → 10.6 MB на
500k литерален overwrite). v0.9: `Vec<S>` във `Vec` — kind 3 на
`baga_rc_release_vec` приема destructor (`baga_rc_relv_<S>` shim) за
вложения Vec<S>; покрити drop/scope exit/reassign/field overwrite и
`vec_set` overwrite на външния (`*_rc` варианти); `Map<K, Vec<S>>` не
съществува в езика (checker я отхвърля); дълбочина >2 и `Vec<Vec<str>>`
остават leak-safe граница (`tests/vecvec_rc_test.baga`; RSS 48.5 MB →
10.8 MB на 500k push+drop и vec_set overwrite). v0.10: enum в
контейнер/struct поле — `Vec<E>`/`Map<K,E>` drop/overwrite/del през shim
`baga_rc_relf_<E>`, push/set retain-ват не-fresh аргументи, `s.e = x`
release-ва стария payload (alias-safe), `has_heap`/`retain_S`/`release_S`
са транзитивни през enum полета; бонус: borrowed enum в struct литерал
(compile error под --rc) вече е `retain_E` (`tests/enum_box_rc_test.baga`;
RSS 93.5 MB → 10.2 MB на 500k push+drop/overwrite/field цикли).
v0.11: match scrutinee temp — `rc_tmp_collect` слиза в scrutinee-то
(release СЛЕД рамената, binding-ите остават borrowed), enum ctor scrutinee
се release-ва с tag 6, а ctor с untrack-нат ident payload (match binding)
вече retain-ва (иначе rebox от temp scrutinee обесва новия enum);
enum/struct fn резултат scrutinee остава leak-safe граница
(`tests/match_temp_rc_test.baga`; RSS 143.0 MB → 72.1 MB на 500k
ctor+str+fn-result scrutinee, само покритите форми 95.7 MB → 24.9 MB).
Батерията вече е 163 файла, база **158/163**. Задача 5 е готова (по-долу).

### 5. v1.0: owned-конвенция за struct/enum fn резултати — ГОТОВО
Днес struct/enum fn резултатът е неразличим owned/borrowed:
`emit_return_val` не retain-ва struct/enum-типизиран borrowed резултат
Реален borrowed случай: boilaDB `boila_ps_tok`/`boila_dual_ptok` връщат
`Token` чрез `return vec_get(lx.toks, i)`.

**v1.0a:** `emit_return_val` / `rc_emit_match_arm_val` / ламбда params
ползват `rc_heap_tag` + `retain_S`/`retain_E`. Untrack-нат ident в
struct литерал (match binding) също. Тест: `owned_ret_rc_test.baga`.

**v1.0b:** call temp в box push/set е move; `s.inner = f()` / `s.e = f()`
без retain; `match f()` scrutinee се регистрира (`rc_tmp_fresh` +
`rc_heap_tag`). Enum ctor не е temp. Разширени calltemp / nested_assign /
enum_box / match_temp тестове.

Docs: `docs/memory-rc-struct-bg.md`, `docs/memory-rc-enum-bg.md`,
`CHANGELOG.md`.

## Забранено / внимание

- Не пипай нищо извън `if (cg->rc)` клонове — emit-c без флаг трябва да
  остане бит-идентичен.
- Посоката на грешка е винаги leak-safe (leak > корупция). Underflow
  (`rc==0` при release) е чиста грешка и ТРЯБВА да остане фатална.
- LLVM backend-ът не поддържа persist/RC — не го пипай.
- Комит съобщенията са на български/английски микс както досега
  (`feat(runtime): … зад --rc`). Push-вай към `origin main` само след
  пълния чеклист.
