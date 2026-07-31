# Възпроизводим self-hosting bootstrap (M0) — План

> **For agentic workers:** Steps use checkbox (`- [ ]`) syntax. БЕЗ git мутации.

**Goal:** (M0a) езикът получава `arg_count()`/`arg(i)` през checker + C + LLVM;
(M0b) self компилаторът чете входа от `arg(0)` и `make self` възпроизвежда
`baga2 == baga3` в build-а.

**Spec:** `docs/superpowers/specs/2026-07-31-self-bootstrap-design.md`

**Ключови файлове:** `src/checker.c` (builtins таблица ~ред 390),
`src/codegen_c.c` (runtime ~ред 1010-1065, builtin map ~ред 298, main ~ред 944
и 1104), `src/codegen_llvm.c` (main wrapper ~ред 1673, lazy helpers ~ред 499/708,
builtin map в NODE_CALL), `self/compiler.baga` (main ~ред 865, runtime write
блок ~ред 895-925, emit builtins в emit_expr ~ред 500-560).

## Global Constraints

- Адитивно: нови builtins, без промяна на съществуващо поведение. Грешки на
  български. Оракулът зелен (става 16/16 с argv.baga). `make test` зелен.
  `self/*.baga` се компилират от `baga`. Без git мутации.
- `arg(i)` извън границите → `""`, без crash. `arg_count()` ≥ 0.
- LLVM пътят трябва да е симетричен на C (оракулът ги сравнява).

---

## M0a — програмни аргументи (езикова фича)

### Task A1: Checker + тестове (първо)

**Files:** Modify `src/checker.c`; New `/tmp/argv_*.baga`, `examples/argv.baga`

- [ ] **Step 1: Тестови файлове**

`/tmp/argv_pos.baga`:
```baga
fn main() {
    print(arg_count())
    print(arg(0))
}
```
(стартира се без аргументи → `0` и празен ред; с `a b` → `2` и `a`.)

`examples/argv.baga` — същото (в оракула; детерминиран изход `0` + ``).

- [ ] **Step 2: builtins таблица**

В `checker.c` builtins масива (`{"len", TYPE_I64, 1, 0}, ...`) добави:
```c
            {"arg_count", TYPE_I64, 0, 0},
            {"arg",       TYPE_STR, 1, 0},
```
(без `has_io` — четенето на argv е чисто.)

- [ ] **Step 3: Проверка (checker)**

`make && ./baga /tmp/argv_pos.baga` → все още НЕ тръбва да гърми в checker-а
(може да fail-не в codegen/link, защото helper-ите още ги няма — това е
очаквано; важното е да няма „непозната функция").

### Task A2: C backend

**Files:** Modify `src/codegen_c.c`

- [ ] **Step 1: runtime globals + helpers**

В runtime блока (до `baga_cur_args`, ~ред 1034) добави:
```c
    fprintf(out, "static int baga_argc = 0;\n");
    fprintf(out, "static char **baga_argv = 0;\n");
    fprintf(out, "static int64_t baga_arg_count(void) { return baga_argc > 0 ? baga_argc - 1 : 0; }\n");
    fprintf(out, "static const char *baga_arg(int64_t i) { return (i + 1 < baga_argc) ? baga_argv[i + 1] : \"\"; }\n");
```

- [ ] **Step 2: main wrapper-и**

Нормалният main (~ред 1104):
```c
        fprintf(out, "int main(int argc, char **argv) {\n");
        fprintf(out, "    baga_argc = argc; baga_argv = argv;\n");
        fprintf(out, "    b_main();\n");
        fprintf(out, "    return 0;\n");
        fprintf(out, "}\n");
```
Test-driver main (~ред 944): също `int main(int argc, char **argv)` + задаване
на глобалите (или поне остави глобалите zero-init — безопасни са). Прегледай
emit_test_driver и направи main-а `(int argc, char **argv)` за консистентност.

- [ ] **Step 3: builtin map**

В таблицата ~ред 298 добави `{"arg_count","baga_arg_count"}, {"arg","baga_arg"}`.

- [ ] **Step 4: Проверка (C)**

`make && ./baga /tmp/argv_pos.baga` → `0` + празен ред.
`./baga /tmp/argv_pos.baga a b` → `2` + `a` (ако baga пуска бинарника с
аргументи; ако `baga` не препредава аргументи на детето, тествай през
`--emit-c` + gcc + ръчно пускане: `./baga --emit-c /tmp/argv_pos.baga > /tmp/a.c && gcc -O2 -Iinclude /tmp/a.c -o /tmp/a -lm && /tmp/a a b`).
ВАЖНО: провери как `baga` стартира генерирания бинарник (main.c) — дали
препредава argv. Ако не, оракулът пак минава (без аргументи → `0`/``), а
ръчният тест с аргументи е през gcc.

### Task A3: LLVM backend

**Files:** Modify `src/codegen_llvm.c`

- [ ] **Step 1: Прочети** main wrapper-а (~ред 1673-1690) и един lazy helper
(`build_baga_vec_push_i64`, ~ред 499-520) + dispatch-а (~ред 708). Виж как се
правят IR глобални (търси `LLVMAddGlobal`/`LLVMSetInitializer`).

- [ ] **Step 2: IR globals + main**

`main` wrapper: тип `(i32, [i32, ptr]) → i32`; в entry-то запази argc/argv в
IR глобални `@baga_argc` (i32) / `@baga_argv` (ptr), после извикай b_main.

- [ ] **Step 3: lazy helpers**

`build_baga_arg_count` (i64, чете `@baga_argc`, `argc>0 ? argc-1 : 0`) и
`build_baga_arg` (i64→ptr, bounds check, връща `@baga_argv[i+1]` или `""`
global string). Регистрирай ги в dispatch-а (~ред 708) и в builtin map-а на
NODE_CALL (`arg_count`→`baga_arg_count`, `arg`→`baga_arg`).

- [ ] **Step 4: Проверка (LLVM)**

`make llvm && ./baga-llvm --emit-llvm /tmp/argv_pos.baga > /tmp/a.ll && lli-14 /tmp/a.ll`
→ `0` + празен ред (идентично с C).
`./tests/llvm_oracle.sh` → 16/16 (argv.baga влиза автоматично).

---

## M0b — self compiler + bootstrap

### Task B1: self compiler чете arg(0) + emit-ва argv runtime

**Files:** Modify `self/compiler.baga`

- [ ] **Step 1: main чете arg(0)**

`let src = read_file("examples/zdravei.baga") catch !IO => ""` →
`let src = read_file(arg(0)) catch !IO => ""`.

- [ ] **Step 2: emit runtime baga_arg***

В write-блока с runtime helper-и (~ред 895-925) добави (като `write(...)`
редове) глобални `baga_argc`/`baga_argv` + `baga_arg_count`/`baga_arg` —
същите като в codegen_c.c.

- [ ] **Step 3: emit main(argc,argv)**

Намери къде self компилаторът emit-ва `int main(void)` за генерираната
програма (търси `"int main"` в compiler.baga) → `int main(int argc, char **argv)`
+ `baga_argc = argc; baga_argv = argv;` преди извикването на b_main.

- [ ] **Step 4: emit builtins arg/arg_count**

В emit_expr builtin handling-а (~ред 500-560) добави map `arg_count`→
`baga_arg_count()`, `arg`→`baga_arg(...)` (по модела на съществуващите
builtins там — виж как emit-ва `len`/`chr`/`read_file`).

- [ ] **Step 5: Проверка (self)**

`make && ./baga --emit-c self/compiler.baga > /tmp/s.c && gcc -O2 -Iinclude /tmp/s.c -o /tmp/baga2 -lm`
→ компилира се (baga2).
`/tmp/baga2 examples/zdravei.baga > /tmp/z.c && gcc -O2 -Iinclude /tmp/z.c -o /tmp/z -lm && /tmp/z`
→ `Здравей, свят!` (baga2 компилира здравей през arg(0)).

### Task B2: make self (fixed point: baga2 == baga3)

**Files:** Modify `Makefile`

- [ ] **Step 1: self target**

```make
self: $(BIN)
	@echo "=== self-hosting bootstrap ==="
	@./$(BIN) --emit-c self/compiler.baga > /tmp/baga_self2.c
	@gcc $(CFLAGS) -o /tmp/baga2 /tmp/baga_self2.c $(LDFLAGS)
	@/tmp/baga2 self/compiler.baga > /tmp/baga_self3.c
	@gcc $(CFLAGS) -o /tmp/baga3 /tmp/baga_self3.c $(LDFLAGS)
	@/tmp/baga3 self/compiler.baga > /tmp/baga_self4.c
	@if diff -q /tmp/baga_self3.c /tmp/baga_self4.c > /dev/null; then \
		echo "OK: baga2 == baga3 (fixed point — self компилаторът се възпроизвежда) ⚔️"; \
	else \
		echo "FAIL: baga2 != baga3"; diff /tmp/baga_self3.c /tmp/baga_self4.c | head -20; exit 1; \
	fi
```
(Истинският инвариант е **fixed point**: `baga` (C bootstrap) и `baga2` (self)
са различни компилатора с различен codegen, затова `baga_self2.c` и
`baga_self3.c` се различават — това НЕ е грешка. Сравняваме `baga_self3.c`
(изход на baga2) с `baga_self4.c` (изход на baga3): ако съвпадат, self
компилаторът е стабилен — `baga2 == baga3` като компилатори.)

- [ ] **Step 2: Проверка**

`make self` → `OK: baga2 == baga3 (fixed point ...)`, exit 0.

### Task B3: документация

**Files:** Modify `docs/language-bg.md`, `docs/language-en.md`, `README.md`

- [ ] **Step 1:** §19 (вградени функции): `arg_count() -> i64`, `arg(i) -> str`
— семантика (0-based, без името на програмата, `""` извън границите). BG + EN.

- [ ] **Step 2:** README Self-Hosting секция: споменава `make self`
(възпроизводимият bootstrap).

- [ ] **Step 3:** Финална регресия: `make clean && make && make llvm && make test && make self`
→ всичко зелено (оракъл 16/16, baga2==baga3).

---

## Self-Review бележки

- Coverage: checker (A1), C (A2), LLVM (A3), self compiler (B1), bootstrap
  target (B2), docs (B3).
- Тънки места: (1) LLVM IR globals + lazy helpers — най-рисковата част, чети
  съществуващите `build_baga_vec_*` и `LLVMAddGlobal` преди да пишеш;
  (2) `baga` (C CLI) може да НЕ препредава argv на генерирания бинарник —
  оракулът няма аргументи, затова е зелен; ръчният тест с аргументи е през
  gcc (main.c препредава ли argv? — провери); (3) self компилаторът emit-ва
  `int main(void)` на конкретно място — намери го точно, не предполагай;
  (4) `make self` разчита, че baga2 пише C на stdout и чете arg(0) — без
  `--emit-c`; (5) diff на генерирания C (не бинарници) — детерминирано.
- Ред: M0a трябва да е ЗЕЛЕН (оракъл 16/16) преди M0b — self компилаторът
  ползва `arg`, който baga трябва вече да поддържа.
