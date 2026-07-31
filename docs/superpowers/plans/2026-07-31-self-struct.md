# Self компилатор: struct (M3a) — План

> **For agentic workers:** Steps use checkbox (`- [ ]`) syntax. БЕЗ git мутации.

**Goal:** `tochka.baga` през baga2 — идентичен изход с baga (`25`). Struct
декларация, литерал, достъп на поле, типова позиция.

**Spec:** `docs/superpowers/specs/2026-07-31-self-struct-design.md`

**Ключови файлове:** `self/compiler.baga` (top-level loop в main ~ред 940+,
parse_primary ~ред 189-225, emit_expr ~ред 519+, c_type ~ред 830, emit_fn/
forward decls ~ред 990+, LET ctype ~ред 820). Целеви C (codegen_c.c): decl
`typedef struct {...} b_Име;`, лит `(b_Име){ .b_f = v }`, поле `obj.b_f`.

## Global Constraints

- Адитивно. Оракул 16/16. `make test` зелен. `make self` (fixed point) зелен.
  mangle() се ползва консистентно (имена да съвпадат). Без git мутации.

---

### Task 1: Парсер — struct decl + лит + поле

**Files:** Modify `self/compiler.baga`

- [ ] **Step 1: `parse_struct`** (нова fn)

advance "struct"; име; `{`; цикъл полета `име : тип` (node 25, text
`"име:тип"`, child = type node не е нужен — типът е в текста) до `}` (`,` се
прескача). Връща node 21 (text = име) с деца 25.

- [ ] **Step 2: top-level loop**

В main, в while-а: `else if vec_get(tk,pos)==0 && str_eq(vec_get_str(tt,pos),
"struct")==1 { let s = parse_struct(...); ast_child(prog, s) }`.

- [ ] **Step 3: struct литерал + поле в parse_primary (ident клон)**

Преструктурирай ident клона: `let mut base = ast_kind(2, t)`; ако `(` → call
(base = call); после ако `{` → struct лит (base = node 22 с деца node 24);
после postfix цикъл `.` → node 23 (base = field). Връща base.

### Task 2: Codegen — emit + c_type + let

**Files:** Modify `self/compiler.baga`

- [ ] **Step 1: emit_expr 22/23**

- 22 (struct лит): `(mangle(t)){ ` + за всяко дете 24: `.mangle(поле) =
  emit(стойност)` (разделени с `, `) + ` }`.
- 23 (поле): `emit(обект) + "." + mangle(t)`.

- [ ] **Step 2: emit struct decl**

В main emit-а, ПРЕДИ forward decl-ите: за всеки child node 21 →
`typedef struct {\n` + за всяко дете 25 (parse `"име:тип"`): `    c_type(тип)
mangle(име);\n` + `} mangle(име);\n\n`.

- [ ] **Step 3: c_type**

След примитивите: `if str_eq(t, "") == 1 { return "int64_t" }`; иначе
`mangle(t)` (struct ref) вместо default `int64_t`.

- [ ] **Step 4: LET ctype**

В LET: `if node_kind(nk, init) == 22 { ctype = mangle(node_text(nt, init)) }`.

### Task 3: Проверка + регресия

- [ ] **Step 1:** `make && ./baga --emit-c self/compiler.baga > /tmp/s2.c &&
  gcc … -o /tmp/baga2` → компилира се.
- [ ] **Step 2:** `/tmp/baga2 examples/tochka.baga > /tmp/t.c && gcc … && run`
  → `25` (идентично с `./baga examples/tochka.baga`).
- [ ] **Step 3:** `make self` → fixed point; `make clean && make && make llvm
  && make test` → зелено; baga vs baga2 — tochka минава в OK, 9-те OK без
  регресия.

---

## Self-Review бележки

- Coverage: парсер (T1), codegen+c_type+let (T2), регресия (T3).
- Тънки места: (1) mangle консистентно навсякъде (decl/лит/поле/тип), иначе
  имената се разминават; (2) c_type default → mangle(t) засяга САМО непознати
  имена (compiler.baga няма такива → fixed point безопасен); празно → int64_t;
  (3) postfix `.` цикълът трябва да е СЛЕД call check-а (за `f(a).x`);
  (4) struct лит `{` vs block — в parse_primary ident-клонът, `{` след име е
  литерал (не блок); (5) parse_struct: `,` между полетата се прескача;
  (6) emit struct decl ПРЕДИ forward decl-ите (typedef-ите трябва да са видими).
