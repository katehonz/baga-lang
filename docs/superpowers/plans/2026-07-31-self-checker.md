# Self компилатор: checker — аргументи + vec (M4a) — План

> **For agentic workers:** Steps use checkbox (`- [ ]`) syntax. БЕЗ git мутации.

**Goal:** baga2 отхвърля `arg_type_bad.baga` и `vec_typed.baga` (exit 1, грешка
на stderr) — като baga. `spec_bad` → M4b.

**Spec:** `docs/superpowers/specs/2026-07-31-self-checker-design.md`

**Ключови файлове:** `src/checker.c` (builtins ~ред 400), `src/codegen_c.c`
(runtime ~ред 1010, builtin map ~ред 298), `src/codegen_llvm.c` (lazy helpers,
`exit_fn` ~ред 1626, builtin map ~ред 1120), `self/compiler.baga` (runtime write
блок, emit_expr builtin map, main ~ред 1180).

## Global Constraints

- Адитивно. Грешките на български (като baga). Оракул 16/16. `make test` зелен.
  `make self` (fixed point) зелен — checker-ът е Baga код, който baga2 компилира.
  Без git мутации.

---

### Task 0: `exit` / `eprintln` builtins (baga C + LLVM)

**Files:** Modify `src/checker.c`, `src/codegen_c.c`, `src/codegen_llvm.c`

- [ ] **Step 1: checker.c** — builtins: `{"exit", TYPE_VOID, 1, 0}`,
  `{"eprintln", TYPE_STR, 1, 0}`→ всъщност `eprintln` връща void:
  `{"eprintln", TYPE_VOID, 1, 0}`.

- [ ] **Step 2: codegen_c.c** — runtime:
  `static void baga_exit(int64_t c) { exit((int)c); }`,
  `static void baga_eprintln(const char *s) { fprintf(stderr, "%s\n", s); }`;
  builtin map: `{"exit","baga_exit"}, {"eprintln","baga_eprintln"}`.

- [ ] **Step 3: codegen_llvm.c** — lazy `build_baga_exit` (call `exit_fn` с
  i32 truncate), `build_baga_eprintln` (fprintf stderr); dispatch + builtin map.

- [ ] **Step 4: Проверка** — `make && make llvm && ./tests/llvm_oracle.sh` →
  16/16. Ръчно: `echo 'fn main(){ eprintln("x")\n exit(3) }'` → exit 3, "x" на
  stderr.

### Task 1: self runtime `exit`/`eprintln`

**Files:** Modify `self/compiler.baga`

- [ ] **Step 1:** runtime write блок: `baga_exit`, `baga_eprintln` (като C).
- [ ] **Step 2:** emit_expr builtin map: `exit`→`baga_exit`,
  `eprintln`→`baga_eprintln`.

### Task 2: Checker пас в self компилатора

**Files:** Modify `self/compiler.baga`

- [ ] **Step 1: помощни** — `typekind_of_typename(name)` (i64→0, str→1, f64→2,
  bool→3, Vec→4, иначе 0); `expr_typekind(...idx)` (литерали по node kind; call
  → fn ret kind; ident → let-init trace); fn-сигнатури (scan node 11 → име,
  param kinds от node 12, ret kind от node 16) — паралелни vec-ове.

- [ ] **Step 2: arg check** — `check_calls(...)`: обхожда AST за node 4 към user
  fn; брой аргументи; за аргумент с известен kind ≠ параметъра (i64/i32
  съвместими) → `eprintln("'<име>': аргумент #N е от тип X, но параметърът е Y");
  exit(1)`.

- [ ] **Step 3: vec mixing** — `check_vecs(...)`: за всяко vec име, ако има И
  i64-push (vec_push/vec_set с не-str елемент), И str-push (vec_push_str/
  vec_set_str или vec_push със str елемент) → `eprintln("vec_push: елемент от
  тип str, но векторът е Vec<i64>"); exit(1)`.

- [ ] **Step 4:** в main, СЛЕД parse и ПРЕДИ codegen: `check_calls(...);
  check_vecs(...)`.

### Task 3: Проверка + регресия

- [ ] **Step 1:** baga2 се компилира; `/tmp/baga2 examples/arg_type_bad.baga` →
  exit 1 (грешка на stderr); `/tmp/baga2 examples/vec_typed.baga` → exit 1.
- [ ] **Step 2:** `make self` → fixed point; `make clean && make && make llvm &&
  make test` → зелено; baga vs baga2 — arg_type_bad/vec_typed минават в OK
  (празен stdout, exit 1); 14-те OK без регресия.

---

## Self-Review бележки

- Coverage: builtins (T0), self runtime (T1), checker (T2), регресия (T3).
- Тънки места: (1) `exit`/`eprintln` трябва да са в baga ПРЕДИ self компилаторът
  да ги ползва (baga2 се компилира от baga); (2) LLVM `exit_fn` е i32 — truncate
  на i64; (3) checker-ът е Baga код — fixed point-ът изисква да е
  self-consistent (без `||`/`&&` в изрази, ако парсерът не ги поддържа — ползвай
  отделни if-ове); (4) arg check: само аргументи с известен kind (unknown се
  пропуска — без фалшиви грешки); (5) vec mixing по име (като vec_is_str);
  (6) spec_bad НЕ се покрива (M4b — нужно е парсване на spec блока).
