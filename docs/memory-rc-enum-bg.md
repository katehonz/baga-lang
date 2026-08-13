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

- Enum в контейнер (`Vec<Opt>`/box) — `rc_box_rel` връща NULL за enum →
  само free (leak-safe).
- Enum като struct поле — `rc_nested_struct_field` е struct-only → такова
  поле не брои struct-а за heap (leak-safe).
- Enum payload в enum payload.
- Match scrutinee temp (`match Err(concat(...))`) — enumът не се release-ва
  след match-а (леак per-match, leak-safe).
- `drop()` builtin върху enum.

## Критерий

- `tests/enum_rc_test.baga` — 8 случая с и без `--rc`, ASan чист.
- Leak repro 500k `Some(concat(...))`: RSS 39.6 MB → 10.9 MB.
- Пълен RC чеклист (`grok-12-08-plan.md`): батерия 152/157 (enum_rc_test
  е новият +1), emit-c без флаг бит-идентичен, bench DURABLE OK.
