# Self компилатор: spec валидация + ensures/requires runtime (M4b) — План

> **For agentic workers:** Steps use checkbox (`- [ ]`) syntax. БЕЗ git мутации.

**Goal:** `spec_bad`, `spec_ensures_fail`, `spec_requires_fail` → baga2 exit 1
като baga. `spec_ensures`/`spec` остават OK. **19/19**.

**Spec:** `docs/superpowers/specs/2026-07-31-self-spec-runtime-design.md`

**Ключови файлове:** `self/compiler.baga` (parse_cmp/parse_expr ~ред 337/354,
top-level loop ~ред 1620, emit_fn_def ~ред 1230, runtime write блок,
check_program ~ред 1580). Целеви C: codegen_c.c wrapper (776-860),
baga_spec_fail (1048).

## Global Constraints

- Адитивно. Грешки/съобщения байт-същите като baga. Оракул 16/16. `make test`
  зелен. `make self` (fixed point) зелен. Без git мутации.
- `||`/`&&` без рекурсивни капани; spec изразите се парсират до `,`/секция/`}`.

---

### Task 1: `||` / `&&` в изрази

**Files:** Modify `self/compiler.baga`

- [ ] **Step 1:** `parse_and` = parse_cmp + loop за `&&` (токен 208) → node 3
  op "&&". `parse_or` = parse_and + loop за `||` (токен 209) → node 3 op "||".
- [ ] **Step 2:** `parse_expr` вика `parse_or` (вместо parse_cmp). Всички
  `parse_expr` обхождат `||`/`&&`.
- [ ] **Step 3:** Проверка — baga2 компилира програма с `a && b || c` (или
  spec_ensures-овите изрази) без грешка.

### Task 2: parse_spec (node 30/31/32)

**Files:** Modify `self/compiler.baga`

- [ ] **Step 1:** `parse_spec`: advance "spec"; име; `{`; loop секции до `}`:
  - секция = ident текст; advance; expect `:`.
  - `output` → parse_type_name → дете node 16 (text=тип).
  - `requires`/`ensures` → loop: parse_expr → дете node 31/32 (text=изходен
    текст за грешка; дете=израз); skip `,`; до следваща секция (ident+`:`,
    lookahead) или `}`.
  - `input`/`guarantees` → skip съдържанието (до следваща секция/`}`).
- [ ] **Step 2:** top-level loop: замени spec skip-а с `parse_spec` +
  `ast_child(prog, spec_node)`.
- [ ] **Step 3:** Проверка — baga2 компилира spec_ensures.baga (spec-ът се
  парси, не чупи останалото).

### Task 3: baga_spec_fail + find_spec + валидация

**Files:** Modify `self/compiler.baga`

- [ ] **Step 1:** runtime: `baga_spec_fail(spec, kind, idx, expr)` — fprintf
  stderr `spec '%s': requires/ensures #N нарушено/а: %s` + exit(1) (байт-също).
- [ ] **Step 2:** `find_spec(prog, fn_name)` → node 30 с text==fn_name (има ли
  дете 31/32).
- [ ] **Step 3:** валидация в check_program: за всеки spec (node 30) с output
  (дете 16) → намери fn (node 11) със същото име; сравни output typekind с fn
  ret typekind; различие → `eprintln("spec '<име>': output ..."); exit(1)`.
- [ ] **Step 4:** Проверка — `spec_bad.baga` → baga2 exit 1.

### Task 4: Runtime codegen (impl + wrapper)

**Files:** Modify `self/compiler.baga`

- [ ] **Step 1:** emit_fn_def: ако fn има spec (find_spec) → emit тялото под
  име `mangle(name) + "_impl"`; после emit wrapper `mangle(name)`:
  - параметри (като fn-а);
  - за всяко requires (дете 31): `if (!(<emit израз>)) baga_spec_fail("<spec>",
    "requires", N, "<text>");`
  - `<ret ctype> b_output = <impl>(<params>);`
  - за всяко ensures (дете 32): `if (!(<emit израз>)) baga_spec_fail("<spec>",
    "ensures", N, "<text>");`
  - `return b_output;`
- [ ] **Step 2:** forward decls: за fn със spec — decl на wrapper-а (и impl).
- [ ] **Step 3:** `output` в ensures → emit_expr мangle-ва "output"→"b_output"
  (съвпада с wrapper temp); параметрите → mangled (съвпадат).
- [ ] **Step 4:** Проверка — `spec_ensures_fail` → exit 1 (ensures #1);
  `spec_requires_fail` → exit 1 (requires #1); `spec_ensures`/`spec` → OK.

### Task 5: Регресия

- [ ] **Step 1:** `make self` → fixed point; `make clean && make && make llvm
  && make test` → зелено.
- [ ] **Step 2:** baga vs baga2 — **19/19 OK** (3-те spec примера exit 1 като
  baga; останалите без регресия).

---

## Self-Review бележки

- Coverage: ||/&& (T1), parse_spec (T2), валидация (T3), codegen (T4),
  регресия (T5).
- Тънки места: (1) spec изрази до `,`/секция/`}` — lookahead за ident+`:` за
  край на секция; (2) `output` → "b_output" чрез mangle (wrapper temp се казва
  `b_output`); (3) impl име `mangle(name)_impl`, wrapper `mangle(name)` (call
  site-овете викат wrapper-а); (4) ret ctype за `b_output` — от fn return тип
  (c_type); void fn с ensures — няма (примерите са i64); (5) baga_spec_fail
  байт-същият формат като C (stderr + exit 1); (6) fixed point: новият код е
  Baga, трябва да е self-consistent (без капани с `||`/`&&` в собствената логика
  — ползвай отделни if-ове където е критично).
