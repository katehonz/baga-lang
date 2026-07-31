# Self компилатор: struct (M3a) — Дизайн

> Дата: 2026-07-31. Статус: одобрен (auto mode). Първи под-инкремент на M3
> (синтактичен паритет). Цел: `tochka.baga` → baga≡baga2.
> Проблем (емпирично): self компилаторът не борави struct — декларацията се
> прескача, литерал `Точка { x: 0.0 }` и достъп `a.x` се парсират грешно
> (gcc: `'b_x' undeclared`).

## Идея

Пълна struct поддръжка в self компилатора, по модела на C backend-а:

- **Декларация** `struct Точка { x: f64, y: f64 }` →
  `typedef struct { double b_x; double b_y; } b_Точка;` (mangled име + полета).
- **Литерал** `Точка { x: 0.0, y: 0.0 }` → `(b_Точка){ .b_x = 0.0, .b_y = 0.0 }`.
- **Достъп на поле** `a.x` → `b_a.b_x`.
- **Типова позиция** (параметър/return `a: Точка`) → `c_type("Точка")` =
  mangled име.

## AST node kind-ове (свободни от M1/M2: 19, 20 заети)

- **21 = struct decl** (text = име; деца = field decl-ове).
- **25 = field decl** (text = `"име:тип"`, като param kind 12).
- **22 = struct lit** (text = име; деца = field init-ове).
- **24 = field init** (text = име на поле; дете = стойност).
- **23 = field access** (text = име на поле; дете = обект).

## Семантика

- **Tokenizer:** `struct` НЕ е ключова дума (ident, kind 0, текст "struct") —
  разпознава се по текст на топ ниво и в parse_primary.
- **Top-level loop:** освен `fn` (токен 1), при ident "struct" → `parse_struct`:
  име, `{`, полета `име : тип ,` до `}` → node 21 с деца 25.
- **parse_primary (ident клон):** след ident (+ опционално call), postfix цикъл
  за `.поле` → node 23 (обект = досегашният base). Покрива `a.x`, `a.x.y`,
  `f(a).x`.
- **parse_primary (struct лит):** ако ident е последван от `{` (не `(`) →
  struct лит: `{ поле : expr , ... }` → node 22 (име) с деца node 24 (поле →
  стойност).
- **emit_expr:**
  - 22 → `(mangle(име)){ .mangle(поле) = стойност, ... }`.
  - 23 → `emit(обект) + "." + mangle(поле)`.
- **emit struct decl** (преди forward decl-ите): за всеки node 21 →
  `typedef struct { c_type(тип) mangle(поле); ... } mangle(име);`.
- **c_type:** непознато (не-примитивно) име → `mangle(име)` (struct ref);
  празно → `int64_t` (fallback). Примитивите (i64/str/Vec/f64/bool) непроменени.
- **LET ctype:** init node 22 (struct лит) → ctype = `mangle(име)`.

## Засегнати компоненти

| Файл | Промяна |
|---|---|
| `self/compiler.baga` | top-level `parse_struct` (21/25); parse_primary: struct лит (22/24) + postfix поле (23); emit_expr 22/23; emit struct decl; `c_type` непознато→mangle; LET ctype за 22 |

## Извън обхвата

- match (M3b), enum (M3c), `Vec<T>` + spec (M3d), effects/catch (M3e).
- Методи върху struct, generics.

## Приемливост

- `tochka.baga`: baga ≡ baga2 (`25` — 3²+4²), exit 0.
- `make self` → fixed point държи (compiler.baga няма struct-ове; c_type
  промяната засяга само непознати имена, каквито там няма).
- `make test` зелен; OK примерите (9) без регресия.
