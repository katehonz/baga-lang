# Self компилатор: vec елементен тип (M2) — План

> **For agentic workers:** Steps use checkbox (`- [ ]`) syntax. БЕЗ git мутации.

**Goal:** `vec.baga` през baga2 — идентичен изход с baga. `vec_push`/`vec_set`/
`vec_get` избират `_i64` vs `_str` helper по елементния тип (евристики).

**Spec:** `docs/superpowers/specs/2026-07-31-self-vec-elem-design.md`

**Ключови файлове:** `self/compiler.baga` (`emit_expr` def ~ред 516 + 16 call
site-а, CALL клон ~ред 537-560, `expr_is_str` ~ред 580).

## Global Constraints

- Адитивно. Оракул 16/16. `make test` зелен. `make self` (fixed point) зелен —
  compiler.baga ползва vec интензивно, i64/str vec-овете трябва да останат
  правилни. Без git мутации.

---

### Task 1: `prog` в `emit_expr`

**Files:** Modify `self/compiler.baga`

- [ ] **Step 1: дефиниция** — `fn emit_expr(nk: Vec, nt: Vec, fc: Vec, ns: Vec,
  idx: i64)` → `fn emit_expr(nk: Vec, nt: Vec, fc: Vec, ns: Vec, prog: i64,
  idx: i64)` (prog преди idx).

- [ ] **Step 2: call site-ове** — механично: всяко `emit_expr(nk, nt, fc, ns, X)`
  → `emit_expr(nk, nt, fc, ns, prog, X)` (16 места + рекурсиите). Еднакво
  литерално заместване на префикса `emit_expr(nk, nt, fc, ns, ` →
  `emit_expr(nk, nt, fc, ns, prog, ` (def-ът има типове `nk: Vec`, не се засяга).

- [ ] **Step 3: Проверка** — `make && ./baga --emit-c self/compiler.baga
  > /tmp/s2.c && gcc … -o /tmp/baga2` → компилира се (baga проверява бройката
  аргументи — пропуснат site гърми тук).

### Task 2: `vec_is_str` + динамичен helper + expr_is_str

**Files:** Modify `self/compiler.baga`

- [ ] **Step 1: `vec_is_str`** (нова fn, навигация по first_child/next_sibling)

Ако vnode е ident (kind 2): име = node_text; scan на програмата за
EXPR_STMT(10)→CALL(4) с callee ident `vec_push`/`vec_set`/`vec_push_str`/
`vec_set_str` и първи аргумент ident със същото име: `_str` алиас → 1;
`vec_push(v, elem)` → `expr_is_str(elem)`; `vec_set(v, i, elem)` →
`expr_is_str(elem)` (3-ти арг). Иначе 0.

- [ ] **Step 2: динамичен helper в CALL**

В callee-is-ident блока, изчисли `a1 = next_sibling(ns, callee)`. Замени
статичните:
- `vec_push`: `elem = next_sibling(ns, a1)`; `cname = expr_is_str(elem) ?
  baga_vec_push_str : baga_vec_push_i64`.
- `vec_set`: `elem = next_sibling(ns, next_sibling(ns, a1))` (3-ти); същото.
- `vec_get`: `cname = vec_is_str(a1) ? baga_vec_get_str : baga_vec_get_i64`.
(`_str` алиасите и vec_new/vec_len остават статични.)

- [ ] **Step 3: `expr_is_str` разпознава `vec_get`**

В call клона на `expr_is_str`: ако callee е `vec_get` и `vec_is_str(first arg)`
→ 1 (резултатът е str → print `%s`).

- [ ] **Step 4: Проверка**

`/tmp/baga2 examples/vec.baga > /tmp/v.c && gcc … -o /tmp/v && /tmp/v`
→ `3 / 10 / 20 / 30 / здравей / свят` (идентично с `./baga examples/vec.baga`).

### Task 3: Регресия

- [ ] **Step 1:** `make self` → fixed point държи.
- [ ] **Step 2:** `make clean && make && make llvm && make test` → зелено.
- [ ] **Step 3:** baga vs baga2 за всички примери — vec.baga минава в OK;
  без НОВА регресия (OK: argv, faktorial, fib, spec, spec_ensures, strings,
  zdravei, types, vec).

---

## Self-Review бележки

- Coverage: prog threading (T1), vec_is_str + helper + expr_is_str (T2),
  регресия (T3).
- Тънки места: (1) префикс-заместването на emit_expr НЕ засяга def-а (типове
  `nk: Vec`); (2) baga проверява бройката аргументи → пропуснат site гърми при
  компилация, не тихо; (3) vec_is_str е по име — compiler.baga няма конфликт
  (nk/fc/ns/pos = i64, nt/tt = str, консистентни навсякъде); (4) vec_push/set
  са локални (елементът е аргумент), само vec_get вика vec_is_str; (5) fixed
  point: i64 vec-овете на compiler.baga остават _i64 (vec_push(nk, kind) —
  kind е i64 → expr_is_str=0).
