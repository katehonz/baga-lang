# Self компилатор: vec елементен тип (M2) — Дизайн

> Дата: 2026-07-31. Статус: одобрен (auto mode). Продължение на M1 (евристики).
> Проблем (емпирично): `vec.baga` през baga2 дава боклук за str-вектор
> (`здравей` → `94450294038538`). Self компилаторът map-ва `vec_get`/`vec_push`/
> `vec_set` **статично** към `_i64` helper-ите; без знание за елементния тип.

## Идея

Изборът на helper (`_i64` vs `_str`) да става по елементния тип, с евристики:

- `vec_push(v, x)` / `vec_set(v, i, x)` — **локално**: елементът е аргумент;
  `expr_is_str(x)` решава (`x` е str → `_str`, иначе → `_i64`).
- `vec_get(v, i)` — няма елементен аргумент; трябва да се знае типът на `v`.
  Нова `vec_is_str(v)`: проследява vec променливата (по име) до `vec_push`/
  `vec_set`/`vec_push_str`/`vec_set_str` повиквания върху нея; ако някой push-ва
  str (или ползва `_str` алиас) → str vec.
- Резултатът на `vec_get` върху str vec е `str` — `expr_is_str` го разпознава
  (за print `%s` и let ctype).

## Нужда от `prog` в `emit_expr`

`expr_is_str`/`vec_is_str` изискват `prog` (ident-trace / глобален scan), а
`emit_expr` днес няма този параметър. Добавя се `prog: i64` към `emit_expr`
(16 call site-а + рекурсиите). Безопасно: baga проверява бройката аргументи,
та пропуснат site гърми веднага при компилация на compiler.baga.

## Семантика

- `vec_is_str(nk, nt, fc, ns, prog, vnode)`: ако `vnode` е ident (kind 2),
  взима името и сканира програмата за EXPR_STMT→CALL с callee `vec_push`/
  `vec_set`/`vec_push_str`/`vec_set_str` и първи аргумент същото име:
  `_str` алиас → 1; `vec_push(v, elem)`/`vec_set(v, i, elem)` с
  `expr_is_str(elem)` → 1. Иначе 0. (Евристика по име — документно ограничение
  при едноименни vec-ове с различен тип в различни функции; не се среща в
  примерите нито в compiler.baga.)
- `emit_expr` CALL: `vec_push`/`vec_set` → helper по `expr_is_str(elem)`;
  `vec_get` → helper по `vec_is_str(v)`. `_str` алиасите остават статични.
- `expr_is_str` (call клон): добавя `vec_get` с `vec_is_str(first arg)` → 1.

## Засегнати компоненти

| Файл | Промяна |
|---|---|
| `self/compiler.baga` | `emit_expr` + `prog` параметър (16 site-а); `vec_is_str` (нова); динамичен vec helper избор в CALL; `expr_is_str` разпознава `vec_get` върху str vec |

## Извън обхвата

- `Vec<T>` синтаксис, struct, match, effects — M3.
- Checker фаза / spec runtime — M4.
- Vec елементен тип при aliasing през друга променлива (евристиката е по име).

## Приемливост

- `vec.baga`: baga ≡ baga2 (`3 / 10 / 20 / 30 / здравей / свят`), exit 0.
- `make self` → fixed point държи (compiler.baga ползва vec интензивно —
  i64 vec-овете nk/fc/ns/pos остават `_i64`, str vec-овете nt/tt — `_str`).
- `make test` зелен; другите примери без регресия.
