# Self компилатор: --test-specs (M5) — План

> **For agentic workers:** Steps use checkbox (`- [ ]`) syntax. БЕЗ git мутации.

**Goal:** `baga2 --test-specs <файл>` ≡ `baga --test-specs <файл>` (байт-идентичен,
детерминирани бройки). Нормалният режим 19/19 без регресия.

**Spec:** `docs/superpowers/specs/2026-07-31-self-test-specs-design.md`

**Ключови файлове:** `self/compiler.baga` (parse_spec ~ред 600, main ~ред 1730,
runtime write блок ~ред 1800, emit_fn_def ~ред 1360). Целеви C: codegen_c.c
`emit_test_driver` (925-1000), `emit_requires_predicate` (671-705),
`spec_inputs_testable` (660), runtime `baga_rand_i64`/`baga_cur_args` (932-1056).

## Global Constraints

- Адитивно (нов режим). PRNG seed **същият** (`0x243F6A8885A308D3`) → същите
  бройки. Съобщения на български, байт-същите. `make test` зелен. `make self`
  (fixed point) зелен. Без git мутации.

---

### Task 1: parse_spec пази input-ите

**Files:** Modify `self/compiler.baga`

- [ ] **Step 1:** В parse_spec, секция `input:` → вместо skip, парси `име : тип`
  записи → дете node 12 (text `"име:тип"`) на spec node-а (30). (Като fn
  параметри; разграничава се от 31/32 по kind.)

### Task 2: Флаг --test-specs в main

**Files:** Modify `self/compiler.baga`

- [ ] **Step 1:** main: `let mut test_specs = 0; let mut file = arg(0)`; ако
  `str_eq(arg(0), "--test-specs")` → `test_specs = 1; file = arg(1)`.
  `let src = read_file(file) ...`.
- [ ] **Step 2:** main: подава `test_specs` към codegen — ако 1 →
  `emit_test_driver(...)`; иначе нормалният main emit.

### Task 3: Runtime (PRNG + counterexample)

**Files:** Modify `self/compiler.baga`

- [ ] **Step 1:** runtime write блок: `baga_seed` (`0x243F6A8885A308D3ULL`),
  `baga_rand_i64(lo,hi)` (xorshift <<13 >>7 <<17), `baga_cur_args[16]`,
  `baga_cur_nargs`.
- [ ] **Step 2:** `baga_spec_fail` — добавя "  вход: ..." цикъл (като C) преди
  exit(1).

### Task 4: spec_inputs_testable + emit_requires_predicate

**Files:** Modify `self/compiler.baga`

- [ ] **Step 1:** `spec_inputs_testable(spec)`: всички node-12 деца са i64/bool.
- [ ] **Step 2:** `emit_requires_predicate(spec)`: `static int
  b__req_<mangle без b_>(params) { return (r1) && (r2); }` (requires node 31
  изрази). Име: `b__req_` + mangle(spec_name) без водещото `b_` (като C: `m+2`).

### Task 5: emit_test_driver

**Files:** Modify `self/compiler.baga`

- [ ] **Step 1:** `emit_test_driver(prog)`: emit-ва requires предикатите за
  тестируемите spec-ове; после main с: за всеки spec — ако нетестируем →
  `пропусната (...)`; иначе 100 теста, retry 1000000, bool→rand(0,1)/i64→
  rand(-1000,1000), `ok = b__req_<име>(args)`, `if(!ok) skipped++`, после
  `baga_cur_nargs`/`baga_cur_args`, `(void)<impl wrapper>(args)`, `passed++`;
  `printf("<име>: %d/100 теста минаха (%d пропуснати от requires)\n")`; накрая
  `Всички spec тестове минаха. ⚔️`. (Константи 100 / 1000000.)
- [ ] **Step 2:** извиква се от main при `test_specs==1` (вместо нормалния main).
  fn impl+wrapper (M4b) се emit-ват и в test режим (driver-ът вика wrapper-а).

### Task 6: Проверка + регресия

- [ ] **Step 1:** `baga2 --test-specs examples/spec_ensures.baga` ≡ baga (същите
  бройки 100/100, 10065).
- [ ] **Step 2:** `baga2 --test-specs examples/spec_ensures_fail.baga` → exit 1,
  `ensures #1 нарушена`.
- [ ] **Step 3:** `make self` → fixed point; `make clean && make && make llvm &&
  make test` → зелено; baga vs baga2 (нормален режим) → 19/19.

---

## Self-Review бележки

- Coverage: parse input (T1), флаг (T2), runtime (T3), predicate+testable (T4),
  driver (T5), регресия (T6).
- Тънки места: (1) PRNG seed/алгоритъм **идентични** с C → същите бройки;
  (2) `b__req_<име>` = `b__req_` + mangle(spec_name) без `b_` (C ползва `m+2`);
  (3) driver-ът вика **wrapper-а** (mangle(spec_name)), не impl-а — wrapper-ът
  проверява ensures; (4) `baga_cur_args` за контрапримера при ensures fail;
  (5) test режим НЕ променя нормалния codegen (additive); (6) `uint64_t` за
  seed — self runtime-ът го emit-ва като C typedef.
