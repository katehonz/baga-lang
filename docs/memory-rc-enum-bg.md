# Enum payload-и като собственици — design (RC5 v0.6)

Статус: **имплементиран зад `--rc`** (C backend). Продължение на RC5 серията
(`docs/memory-rc-struct-bg.md` — struct полета). Зависи от RC1–RC5 v0.5.

## Проблемът

Sum enum е `{ int64_t tag; union { T v_Variant; ... } u; }` — payload-ът се
съхранява по стойност, но heap payload (str/bytes/Vec/Map/struct с heap)
не се release-ва никъде: `let o = Some(concat(...))` тече при scope exit.
500k итерации → 39.6 MB RSS.

## Правила (tag 6)

1. **`rc_enum_has_heap`** — enum с поне един variant с heap payload
   (str/bytes/Vec/Map или struct с heap полета). Enum payload в enum не се
   брои (leak-safe граница).
2. **Helper-и** `baga_rc_retain_<E>`/`baga_rc_release_<E>` — `switch (e.tag)`,
   на активния variant се retain/release-ва payload-ът (рекурсивно за struct
   payload чрез v0.5). Forward-declared с останалите RC helper-и.
3. **Локал се регистрира (tag 6)** само при свеж ctor (`Ok(x)` /
   `Res::Ok(x)` — познава се с `rc_is_enum_ctor`) или alias на track-нат
   enum. Fn резултат (`let r = make_opt()`) и bare variant (`None`) НЕ се
   регистрират — leak-safe, както struct правило 2.
4. **Ctor call site** — payload собственост като при struct литерал: fresh
   (concat/call) е owned без retain; borrowed (`vec_get`/поле/match binding)
   се retain-ва; last-use ident е move. RC4 вече третира ctor аргумента
   като прехвърлен (не го release-ва като temp).
5. **`let a = o`** — retain_E. **`o = Other(...)`** — release_E на стария
   (alias-safe ред по общия reassign път). **`return o`** — move.
6. **Match binding** (`Some(s) => ...`) е borrowed копие — не се регистрира;
   enum собственикът изживява arm-а. Arm резултат, който алиасира payload,
   се retain-ва от `rc_emit_match_arm_val` (вече съществуващо).

## Не-цели (v0.6)

- ~~Enum в контейнер (`Vec<Opt>`/box) — `rc_box_rel` връща NULL за enum →
  само free (leak-safe).~~ — **РЕШЕНО в v0.10 (виж по-долу).**
- ~~Enum като struct поле — `rc_nested_struct_field` е struct-only → такова
  поле не брои struct-а за heap (leak-safe).~~ — **РЕШЕНО в v0.10.**
- Enum payload в enum payload.
- ~~Match scrutinee temp (`match Err(concat(...))`) — enumът не се release-ва
  след match-а (леак per-match, leak-safe).~~ — **РЕШЕНО в v0.11 (виж
  по-долу)** за enum ctor scrutinee; fn резултат scrutinee остава граница.
- `drop()` builtin върху enum.

## v0.10: enum в контейнер/struct поле

v0.6 покри enum локали; box елементите (`Vec<E>`/`Map<K,E>`) и enum-
типизираните struct полета оставаха не-цели. v0.10 ги покрива по аналогия
с v0.2/v0.3 (struct в box) и v0.4/v0.8 (struct поле):

- **Box destructor shim-ове** — до `relf`/`retp` на struct-овете (v0.2)
  enum с heap payload получава `baga_rc_relf_<E>(void *p)` →
  `release_E(*p)` и `baga_rc_retp_<E>` → `retain_E(*p)` (с forward
  декларации). `rc_box_rel` резолвира и enum типове, така че ВСИЧКИ
  release сайтове (scope exit, temp release, `drop()`, reassign, `s.f = x`
  с Vec/Map поле, `release_S`/`release_E` за Vec/Map поле/payload) покриват
  `Vec<E>`/`Map<K,E>` без допълнителна работа.
- **Overwrite/del** — `vec_set`/`map_set` вървят през `*_box_rc`
  вариантите от v0.3, `map_del` през `baga_map_del_*_rc`, с `relf_<E>`.
- **Push/set собственост** — общ предикат `rc_box_tracked` (struct с heap
  полета или enum с heap payload). Свеж ctor (`rc_is_enum_ctor`) е move
  (payload-ът е owned от ctor сайта — v0.6 правило 4); last-use ident е
  move (RC2); всичко останало (ident, `vec_get`, call резултат) се
  retain-ва — задължително, щом drop вече release-ва payload-ите (иначе
  underflow при споделен payload). `vec_slice`/`vec_concat` на `Vec<E>`
  вървят през `*_box_rc` с `retp_<E>` (shallow box копия споделят
  payload-а — както struct-овете в v0.2).
- **`s.e = x`** — `rc_field_assign_enum` познава цел `ident.field` с
  track-нат struct ident (tag 5, не param/dead) и enum-типизирано поле с
  heap payload (`rc_nested_enum_field`); `rc_emit_enum_field_release`
  пуска старото поле. Свеж ctor — owned без retain; tracked ident —
  retain/move; всичко останало (call/поле/vec_get/untrack-нат ident) —
  retain (enum fn резултат може да е borrowed — §v0.7 границата,
  leak-safe). Alias-safe ред: retain преди release. Само плоско
  `ident.field` (границата на v0.4/v0.8).
- **Struct с enum поле** — `rc_struct_has_heap` е транзитивен и през enum
  полета (depth-aware взаимна рекурсия `rc_struct_has_heap_d` ↔
  `rc_enum_has_heap_d` срещу циклични декларации); `retain_S`/`release_S`
  викат `retain_E`/`release_E` за тях. Struct САМО с enum поле вече
  получава helper-и (tag 5).
- **Бонус фикс** — borrowed enum (`vec_get`/поле), вграден директно в
  struct литерал, emit-ваше `baga_rc_retain((void *)__rc_sl.<field>)`
  върху enum СТОЙНОСТ — compile error под `--rc` („cannot convert to a
  pointer type"). Tag 6 вече отива в `retain_E` през `__rc_sl.<field>`
  (като tag 5 от v0.5).

Граници (leak-safe, не корупция): enum payload в enum payload не се брои
(както v0.6); `Vec<Vec<E>>` — nested vec shim-ът (v0.9) е struct-only,
enum като най-вътрешен елемент тече както досега; `drop()` builtin върху
enum локал остава не-цел. **v1.0b:** `s.e = f()` е owned без retain.

Измерено: 500k итерации Vec<E> push+drop + map_set overwrite + `s.e = x`
overwrite + struct-с-enum-поле scope exit — RSS 93.5 MB → 10.2 MB (като
leak-free базата). Тест: `tests/enum_box_rc_test.baga` (22 случая).

## v0.11: match scrutinee temp

RC4 не слиза в NODE_MATCH изобщо (консервативно — условни рамена), затова
scrutinee temp-ове (`match f()`, `match Some(concat(...))`) течаха
per-match. Scrutinee-то обаче се оценява безусловно и точно веднъж, ПРЕДИ
рамената — безопасно е да се track-ва:

- `rc_tmp_collect` слиза само в `match_expr` (не в рамената/pattern-ите).
  Release-ът е в края на statement-а — СЛЕД телата на рамената, така че
  borrowed binding-ите (v0.6 пр. 6) са валидни. Покрива str/bytes/Vec/Map
  call temp-ове и вложените в тях temp-ове, във всички позиции (void
  statement, let init, return, if/while cond wrap от RC4 v0.3).
- Enum ctor scrutinee се регистрира като temp с tag 6 (release_E след
  рамената) — enum-ът от ctor сайт притежава payload референциите си
  (v0.6 пр. 4). **v1.0b:** enum/struct fn резултат scrutinee (`match mk()`)
  се регистрира през `rc_tmp_fresh` — резултатът е owned (v1.0a).
- Свързан фикс: ctor с untrack-нат ident payload (match binding или
  enum/struct fn резултат локал) вече retain-ва — v0.6 пр. 4 го
  документира, но `rc_find` не вижда binding-ите (не се регистрират).
  Досега това беше маскирано от leak-а на scrutinee-то; с release-а му
  rebox (`let o2 = match Some(concat(...)) { Some(s) => Some(s), ... }`)
  би обесил новия enum (UAF, хванат от `rebox_outlives` в теста).
  Неразличим owned случай → една излишна референция (leak-safe посока).

Измерено (v0.11): 500k итерации (ctor + str + fn-result scrutinee) — RSS
143.0 MB → 72.1 MB (остатъкът беше `match mk()`); само покритите форми —
95.7 MB → 24.9 MB. **v1.0b затваря `match mk()`.** Тест:
`tests/match_temp_rc_test.baga` (13 случая).

## Критерий

- `tests/enum_rc_test.baga` — 8 случая с и без `--rc`, ASan чист.
- Leak repro 500k `Some(concat(...))`: RSS 39.6 MB → 10.9 MB.
- Пълен RC чеклист (`grok-12-08-plan.md`): батерия 152/157 (enum_rc_test
  е новият +1), emit-c без флаг бит-идентичен, bench DURABLE OK.
