# Self компилатор: match (M3b) — План

> **For agentic workers:** Steps use checkbox (`- [ ]`) syntax. БЕЗ git мутации.

**Goal:** `match.baga` през baga2 — идентичен изход с baga (`100/200/300/999/
999`). match като израз (GCC statement expression).

**Spec:** `docs/superpowers/specs/2026-07-31-self-match-design.md`

**Ключови файлове:** `self/compiler.baga` (parse_primary ident клон ~ред 205,
emit_expr ~ред 578). Целеви C (codegen_c.c NODE_MATCH): `({ T _mrN=0; int64_t
_mvN=expr; if(_mvN==pat){_mrN=body;} else {...} _mrN; })`.

## Global Constraints

- Адитивно. Оракул 16/16. `make test` зелен. `make self` (fixed point) зелен.
  `=>` е два токена (`=` + `>`). Без git мутации.

---

### Task 1: Парсер — match (26) + arm (27)

**Files:** Modify `self/compiler.baga`

- [ ] **Step 1:** В parse_primary ident клон (k==0), СЛЕД advance и ПРЕДИ
  `let mut base = ast_kind(2, t)`: `if str_eq(t, "match") == 1 { ... return
  match node ... }`.

- [ ] **Step 2:** match parsing:
  - scrutinee = parse_expr; mnode = ast_kind(26, ""); ast_child(mnode, scrutinee).
  - expect `{`; цикъл до `}` (или EOF):
    - pattern = parse_primary; wildcard ако pattern е ident (kind 2) с текст "_".
    - expect `=` (is_single) и `>` (is_single).
    - body = parse_expr.
    - arm = ast_kind(27, wildcard ? "_" : ""); ако НЕ wildcard → ast_child(arm,
      pattern); ast_child(arm, body); ast_child(mnode, arm).
    - skip `,`.
  - expect `}`; return mnode.

### Task 2: Codegen — emit_expr 26

**Files:** Modify `self/compiler.baga`

- [ ] **Step 1:** emit_expr, до другите kind-ове:
  - mexpr = first_child; arm1 = next_sibling(mexpr).
  - result ctype от body-то на arm1 (последно дете): expr_is_str → `const char
    *`; expr_is_float → `double`; expr_is_bool → `int`; иначе `int64_t`.
  - suf = int_to_str(idx).
  - init: `({ <ctype> _mr<suf> = 0; int64_t _mv<suf> = <emit mexpr>; `.
  - за всеки arm: body = последно дете; ако text=="_" → `else { _mr<suf> =
    <body>; }`; иначе pattern = first_child → `[else ]if (_mv<suf> == <pat>) {
    _mr<suf> = <body>; }` (първият без `else`).
  - край: `_mr<suf>; })`.

### Task 3: Проверка + регресия

- [ ] **Step 1:** `make && ./baga --emit-c self/compiler.baga > /tmp/s2.c &&
  gcc … -o /tmp/baga2` → компилира се.
- [ ] **Step 2:** `/tmp/baga2 examples/match.baga > /tmp/m.c && gcc … && run`
  → `100/200/300/999/999` (идентично с baga).
- [ ] **Step 3:** `make self` → fixed point; `make test` зелен; baga vs baga2 —
  match минава в OK, 10-те OK без регресия.

---

## Self-Review бележки

- Coverage: парсер (T1), codegen (T2), регресия (T3).
- Тънки места: (1) `=>` = два токена `=` + `>`; (2) wildcard = ident "_";
  (3) result тип от първия arm body (match.baga i64, cvet str); (4) уникален
  suf = node idx (без конфликт при вложени match); (5) arm body е израз
  (примерите нямат block arm-ове); (6) match като expr_stmt + implicit return
  покрива cvet-овия `fn име { match ... }`.
