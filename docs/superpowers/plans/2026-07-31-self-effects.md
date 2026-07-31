# Self компилатор: effects/catch (M3e) — План

> **For agentic workers:** Steps use checkbox (`- [ ]`) syntax. БЕЗ git мутации.

**Goal:** `effects.baga` през baga2 — идентичен изход с baga (`съдържание`).
Завършва M3 (синтактичен паритет).

**Spec:** `docs/superpowers/specs/2026-07-31-self-effects-design.md`

**Ключови файлове:** `self/compiler.baga` (parse_fn return ~ред 577,
parse_primary ident клон ~ред 285, LET catch skip ~ред 388, expr-stmt catch
skip ~ред 494).

## Global Constraints

- Адитивно; ефектите са compile-time only (прескачат се). Оракул 16/16.
  `make test` зелен. `make self` (fixed point) зелен. Без git мутации.

---

### Task 1: Прескачане на ефектовите конструкти

**Files:** Modify `self/compiler.baga`

- [ ] **Step 1: return effect skip** — в parse_fn, след return type node:
  `while is_single("!") { advance; advance }` (`!Име` двойки).

- [ ] **Step 2: postfix `?`** — в parse_primary ident-клона, след field-access
  loop-а: `while is_single("?") { advance }`.

- [ ] **Step 3: catch skip → while** — LET клон и expr-stmt fallback:
  `if vec_get==16` → `while vec_get==16` (catch; !; E; `=`; `>`; parse_expr).

### Task 2: Проверка + регресия

- [ ] **Step 1:** baga2 се компилира; `/tmp/baga2 examples/effects.baga` → C →
  gcc → `съдържание` (идентично с baga).
- [ ] **Step 2:** `make self` → fixed point; `make test` зелен; baga vs baga2 —
  effects минава в OK; **всички конструкт-примери OK (14/19)**; останалите 5 са
  checker/spec (M4).

---

## Self-Review бележки

- Coverage: return effect skip, postfix `?`, catch while (T1); регресия (T2).
- Тънки места: (1) `!Име` = два токена (`!` 300 + ident); (2) `=>` = `=` + `>`;
  (3) catch skip while за верижни catch-ове (effects.baga има 2); (4) `?` само
  в ident-клона (достатъчно за примерите); (5) M3 приключва — self компилаторът
  парси/кодогенерира целия синтаксис; остава M4 (checker + spec runtime).
