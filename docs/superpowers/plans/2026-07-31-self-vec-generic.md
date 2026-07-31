# Self компилатор: Vec<T> синтаксис + spec skip (M3d) — План

> **For agentic workers:** Steps use checkbox (`- [ ]`) syntax. БЕЗ git мутации.

**Goal:** `vec_ann.baga` през baga2 — идентичен изход с baga (`30` и `здравей`).

**Spec:** `docs/superpowers/specs/2026-07-31-self-vec-generic-design.md`

**Ключови файлове:** `self/compiler.baga` (parse_fn params/return ~ред 528-545,
parse_stmt LET анотация, top-level loop ~ред 1262).

## Global Constraints

- Адитивно. Оракул 16/16. `make test` зелен. `make self` (fixed point) зелен.
  Без git мутации.

---

### Task 1: `parse_type_name` + употреба

**Files:** Modify `self/compiler.baga`

- [ ] **Step 1: `parse_type_name`** (нова fn, преди parse_fn)

име = текущ токен + advance; ако следва `<` → консумира балансирано (depth) до
`>`; връща името.

- [ ] **Step 2: parse_fn**

param: замени `let ptype = vec_get_str(...); advance` с `let ptype =
parse_type_name(tk, tt, pos)`. return: замени `let rettype = vec_get_str(...);
advance` с `let rettype = parse_type_name(tk, tt, pos)`.

- [ ] **Step 3: parse_stmt LET анотация**

`: тип` — замени единичния advance на тип-токена с `parse_type_name(tk, tt,
pos)` (консумира `Vec<T>` цялото).

### Task 2: spec skip (top-level)

**Files:** Modify `self/compiler.baga`

- [ ] **Step 1:** В top-level loop, до struct/enum: при ident `"spec"` →
  advance име; ако `{` → балансиран skip (depth) до `}`.

### Task 3: Проверка + регресия

- [ ] **Step 1:** baga2 се компилира; `/tmp/baga2 examples/vec_ann.baga` → C →
  gcc → `30` и `здравей` (идентично с baga).
- [ ] **Step 2:** `make self` → fixed point; `make test` зелен; baga vs baga2 —
  vec_ann минава в OK, 12-те OK без регресия.

---

## Self-Review бележки

- Coverage: parse_type_name + употреба (T1), spec skip (T2), регресия (T3).
- Тънки места: (1) `<`/`>` в типова позиция са generic (не сравнения) —
  parse_type_name се вика само след `:`/`->`; (2) depth брояч за вложени
  `Vec<Vec<i64>>` (примерите са single-level); (3) връща се базовото име `Vec`
  — c_type("Vec") = baga_Vec *, елементът е от vec_is_str (M2); (4) spec skip —
  балансиран `{...}`, spec-ът няма checker/codegen в self; (5) token-by-token
  skip би проработил и без експлицитен spec skip, но експлицитният е robust.
