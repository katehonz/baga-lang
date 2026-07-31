# Self компилатор: enum (M3c) — Дизайн

> Дата: 2026-07-31. Статус: одобрен (auto mode). Трети под-инкремент на M3.
> Цел: `cvet.baga` → baga≡baga2. cvet ползва enum + match (match вече работи
> от M3b); трябва enum декларация + референции към варианти.

## Идея

По модела на C backend-а (codegen_c.c:217-236, 1076-1088):

- **Декларация** `enum Цвят { Червено, Зелено, Синьо }` →
  `typedef enum { b_Цвят_b_Червено = 0, b_Цвят_b_Зелено = 1, ... } b_Цвят;`
  (вариант = `mangle(enum)_mangle(вариант)`, стойност = индекс).
- **Референция** `Зелено` (ident) → `b_Цвят_b_Зелено` (enum константата).

## AST

- **28 = enum decl** (text = име; деца = варианти).
- **29 = variant** (text = име на вариант).

## Семантика

- **Tokenizer:** `enum` НЕ е ключова дума (ident "enum") — разпознава се на
  топ ниво по текст.
- **Top-level loop:** при ident "enum" → `parse_enum`: име, `{`, варианти
  (идентификатори, `,` се прескача) до `}` → node 28 с деца 29.
- **emit enum decl** (преди struct decl-ите, както C прави enums first):
  `typedef enum { mangle(enum)_mangle(var) = i, ... } mangle(enum);`.
- **`enum_variant_cname(prog, name)`**: сканира enum decl-ове (28) за вариант
  (29) с това име → `mangle(enum)_mangle(name)`, иначе `""`.
- **emit_expr ident (k==2):** ако `enum_variant_cname` ≠ `""` → това; иначе
  `mangle(t)`.

## Засегнати компоненти

| Файл | Промяна |
|---|---|
| `self/compiler.baga` | `parse_enum` (28/29); top-level "enum"; emit enum typedef; `enum_variant_cname`; emit_expr ident → enum константа |

## Извън обхвата

- `Vec<T>`/spec (M3d), effects/catch (M3e).
- Enum като тип на параметър (cvet ползва `ц: i64`); методи/assoc. стойности.

## Приемливост

- `cvet.baga`: baga ≡ baga2 (`1` и `зелено`), exit 0.
- `make self` → fixed point държи. `make test` зелен. OK (11) без регресия.
