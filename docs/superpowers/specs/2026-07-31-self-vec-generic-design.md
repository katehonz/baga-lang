# Self компилатор: Vec<T> синтаксис + spec skip (M3d) — Дизайн

> Дата: 2026-07-31. Статус: одобрен (auto mode). Четвърти под-инкремент на M3.
> Цел: `vec_ann.baga` → baga≡baga2.
> Проблем (емпирично): `Vec<i64>`/`Vec<str>` в параметри чупи парсването —
> типът се чете като един токен (`Vec`), `<i64>` остава и десинхронизира
  параметрите (gcc: conflicting types, spec-токени като параметри).

## Идея

- **`Vec<T>` в типова позиция** (параметри, return, let анотация): ново
  helper `parse_type_name` — чете името на типа и **консумира** незадължителния
  `<...>` (балансирани `<`/`>`). Връща базовото име (`Vec`) — codegen-ът
  третира `Vec` и `Vec<T>` еднакво (`baga_Vec *`), елементният тип се взима от
  `vec_is_str` (M2) по push-овете.
- **spec блок** (`spec име { ... }`): прескача се (self компилаторът няма spec
  checking/codegen). Експлицитен skip на балансирания `{...}` (по-robust от
  token-by-token).

## Семантика

- **`parse_type_name(tk, tt, pos) -> str`**: име = текущ токен, advance; ако
  следва `<` → консумира до балансиран `>` (depth брояч). Връща името.
- **parse_fn:** param type и return type → `parse_type_name` (вместо един токен).
- **parse_stmt LET:** анотацията `: тип` → `parse_type_name` (вместо един
  advance).
- **top-level:** при ident `"spec"` → skip: advance име, после балансиран
  `{...}`.

## Засегнати компоненти

| Файл | Промяна |
|---|---|
| `self/compiler.baga` | `parse_type_name` (нова); parse_fn param/return; parse_stmt LET анотация; top-level spec skip |

## Извън обхвата

- Spec checking/codegen в self (M4). Елементният тип на Vec идва от `vec_is_str`
  (M2), не от анотацията (достатъчно за примерите).
- effects/catch (M3e).

## Приемливост

- `vec_ann.baga`: baga ≡ baga2 (`30` и `здравей`), exit 0.
- `make self` → fixed point държи. `make test` зелен. OK (12) без регресия.
