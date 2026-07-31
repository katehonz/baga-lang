# Поправка на LLVM backend-а — Имплементационен план

> **For agentic workers:** Steps use checkbox (`- [ ]`) syntax.
> **ВАЖНО:** БЕЗ `git commit`/`git add` — git мутации само с одобрение.

**Goal:** LLVM backend-ът генерира коректен IR за числовото ядро на Бага + control flow + match + contracts, и ЧЕСТНО отказва всичко останало с compile-time грешка. Критерий: оракъл скрипт сравнява C vs LLVM изход за всички примери — байт за байт, включително exit кодове.

**Architecture:** Symbol table име→alloca със scope стек; предекларация на всички функции; f64 промоция по правилата на C backend-а; contract wrapper (impl+wrapper) като в codegen_c.c; нула тихи default-и.

**Spec:** `docs/superpowers/specs/2026-07-31-llvm-backend-fix-design.md` (съдържа root cause анализа — прочети го!)

**Референции, които ТРЯБВА да прочетеш преди код:**
- `src/codegen_llvm.c` (433 реда, текущото счупено състояние)
- `src/codegen_c.c` — моделът за: mangle, implicit return, присвоявания, for, match, print по типове, contract wrapper (`find_ensures_spec`, baga_spec_fail съобщенията — трябва да са БАЙТОВО същите)
- `src/main.c` — къде се вика `codegen_llvm` спрямо `check_program`
- `include/baga.h` — AST възлите

## Global Constraints

- LLVM 14 C API (`llvm-c/Core.h` и пр.); `make llvm` трябва да минава без нови warnings (има един вече съществуващ warning в codegen_llvm.c ред ~7 — ако е тривиален, оправи го).
- Съобщенията за грешки на компилатора — на български. Съобщенията от `baga_spec_fail` в IR — БАЙТОВО същите като C backend-а (oracle-ът diff-ва изхода на spec_ensures_fail!).
- НИКАКВИ тихи стойности: всеки неподдържан възел →
  `baga: LLVM backend: неподдържан конструкт '<описание>'` на stderr + `exit(1)`.
- Без git мутации. Build: `make llvm`. Регресия: `make test`.

---

### Task 1: Ядро — symbol table, изрази, присвояване, предекларации

**Files:**
- Modify: `src/codegen_llvm.c` (пренаписване на ядрото)
- Modify: `src/main.c` (подредба check_program → codegen_llvm)

**Interfaces:**
- Produces: коректен IR за: литерали (i64/f64/bool/str), ident (load), binary (int+f64, сравнения, &&/||), unary (-, !), call, let, assign (=, +=, -=, *=, /=), return, if/else, while, for+break+continue, expr stmt; предекларирани функции; грешка за всичко друго.

- [ ] **Step 1: Падащ тест (оракулът)**

Създай `tests/llvm_oracle.sh` (chmod +x):

```bash
#!/bin/bash
# Оракъл: сравнява C backend и LLVM backend (lli-14) за всички примери.
cd "$(dirname "$0")/.."
FAIL=0
for f in examples/*.baga; do
    [ "$f" = "examples/spec_bad.baga" ] && continue   # очаквана compile грешка
    ./baga "$f" > /tmp/baga_c_out.txt 2>&1; rc_c=$?
    if ! ./baga-llvm --emit-llvm "$f" > /tmp/baga.ll 2>/tmp/baga_ll_err.txt; then
        if grep -q "неподдържан конструкт" /tmp/baga_ll_err.txt; then
            echo "SKIP  $f (честен отказ)"
        else
            echo "FAIL  $f (LLVM crash без грешка)"; FAIL=1
        fi
        continue
    fi
    lli-14 /tmp/baga.ll > /tmp/baga_llvm_out.txt 2>&1; rc_l=$?
    if [ $rc_c -ne $rc_l ] || ! diff -q /tmp/baga_c_out.txt /tmp/baga_llvm_out.txt > /dev/null; then
        echo "MISMATCH $f (exit C=$rc_c LLVM=$rc_l)"; FAIL=1
    else
        echo "OK    $f"
    fi
done
[ $FAIL -eq 0 ] && echo "--- оракулът е доволен ⚔️"
exit $FAIL
```

Run: `./tests/llvm_oracle.sh`
Expected: MISMATCH-ове навсякъде (това е падащият тест).

- [ ] **Step 2: main.c — подредба**

Прочети main.c. Ако `codegen_llvm` се вика преди `check_program` — премести го след (типовете на възлите трябва да са инферирани). `--emit-llvm` пътят трябва да печати грешките на checker-а и да НЕ генерира IR при грешки (както C пътят).

- [ ] **Step 3: Symbol table + грешки**

В codegen_llvm.c добави:

```c
/* ---- Symbol table: име → alloca ---- */
#define LLVM_MAX_VARS 256
typedef struct { char *name; LLVMValueRef alloca; } LLVMVar;
typedef struct {
    LLVMVar vars[LLVM_MAX_VARS];
    int count;
    int scope_marks[64];
    int depth;
} LLVMSymtab;
static LLVMSymtab lg_st;

static void st_push(void) { lg_st.scope_marks[lg_st.depth++] = lg_st.count; }
static void st_pop(void)  { lg_st.count = lg_st.scope_marks[--lg_st.depth]; }
static void st_define(const char *name, LLVMValueRef alloca) {
    lg_st.vars[lg_st.count].name = (char *)name;
    lg_st.vars[lg_st.count].alloca = alloca;
    lg_st.count++;
}
static LLVMValueRef st_lookup(const char *name) {
    for (int i = lg_st.count - 1; i >= 0; i--)
        if (strcmp(lg_st.vars[i].name, name) == 0) return lg_st.vars[i].alloca;
    return NULL;
}

/* ---- честен отказ ---- */
static void llvm_unsupported(const char *what) {
    fprintf(stderr, "baga: LLVM backend: неподдържан конструкт '%s'\n", what);
    exit(1);
}
```

Имената в таблицата са Бага имената (неманглирани); mangling се ползва само за LLVM value names.

- [ ] **Step 4: Изрази**

Пренапиши `emit_expr_llvm`:
- `NODE_IDENT`: `st_lookup` → `LLVMBuildLoad2` от типа на alloca-та; липса → enum вариант? (Task 2) → `llvm_unsupported("недефинирана променлива в LLVM backend")`.
- `NODE_BINARY`: определи дали е f64 операция по `LLVMTypeOf` на операндите (double → FAdd/FSub/FMul/FDiv/FRem + FCmp O-predicates; иначе int вариантите). Промоция: ако единият е double, а другият i64/i32/i1 → `LLVMBuildSIToFP`. Сравненията връщат i1. `&&`/`||` — bitwise And/Or върху i1 е ОК (bool е i1; C backend-ът също не прави short-circuit — свери с codegen_c.c и направи СЪЩОТО).
- `NODE_UNARY`: `UOP_NEG` → Neg/FNeg по тип; `UOP_NOT` → Not върху i1; `&`/`*` → llvm_unsupported.
- `NODE_CALL`: по име през `LLVMGetNamedFunction`; `print`/`println`/`write` — Task 3; други builtins → llvm_unsupported с името.
- default: `llvm_unsupported(...)` с името на възела (брой като int в съобщението е ОК).

- [ ] **Step 5: Оператори**

- `NODE_LET`: alloca (тип от let_type или от init->type през `llvm_type_resolved`), store на init; st_define; st_push/st_pop в `emit_block_llvm`.
- `NODE_ASSIGN`: target ident → store на стойността; за `+=` и пр. — прочети как parser-ът/codegen_c ги представя (вероятно desugared до обикновен assign с binary вдясно; ако не — огледало на codegen_c). Не-ident target → llvm_unsupported.
- `NODE_IF/WHILE`: както са, но с push/pop scope по блоковете.
- `NODE_FOR`: range `lo..hi` — alloca на променливата (i64), loop cond `i < hi`, инкремент; `continue` скача към инкремента — подай cond/increment basic block през параметър (разшир `emit_stmt_llvm` с два block refs: `cont_bb` за break и `incr_bb` за continue — while подава cond_bb, for подава increment block-а).
- `NODE_EXPR_STMT`: emit_expr_llvm.

- [ ] **Step 6: Предекларации + implicit return**

В `codegen_llvm`: първи проход `LLVMAddFunction` за всички NODE_FN (точните типове), после телата. В `emit_fn_llvm`: ако тялото завършва с `NODE_EXPR_STMT` при не-void функция → `ret` на стойността (огледало на codegen_c.c:671-675). Параметрите: alloca в entry + st_define.

- [ ] **Step 7: Проверка**

Run: `make llvm && ./tests/llvm_oracle.sh`
Expected: OK за zdravei, faktorial, fib; вероятно още MISMATCH-ове при types/cvet/match (f64/enum/match — Task 2/3) и spec_* (contracts — Task 4).

---

### Task 2: f64, enum варианти, match

**Files:**
- Modify: `src/codegen_llvm.c`

- [ ] **Step 1: f64 print и литерали**

`print(x)` при f64 аргумент → `printf("%g\n", x)` — свери C backend-а (`baga_print_f64` ползва `%g`) — същият формат! bool → каквото C backend-ът прави (прочети codegen_c.c print логиката; вероятно `%s` с "true"/"false" или `%lld` — ОГЛЕДАЛО 1:1).

- [ ] **Step 2: Enum варианти**

При `NODE_IDENT`, който не е в symtab: потърси в `program->items` enum с този вариант → `LLVMConstInt(i64, index)`. (Съхранявай `Node *program` в `lg`.)

- [ ] **Step 3: match**

`NODE_MATCH` върху i64: за всеки клон — `icmp eq` за литерален pattern, wildcard `_` → else блок; веригата от condbr-ове; тяло на клон. Ако клонът е израз, връщащ стойност от функцията (огледало на codegen_c — прочети как той третира match arms с гол израз: „връща тази стойност от обграждащата функция" — вероятно emit-ва return). Не-i64 match → llvm_unsupported.

- [ ] **Step 4: Проверка**

Run: `make llvm && ./tests/llvm_oracle.sh`
Expected: OK и за types, cvet, match.

---

### Task 3: Contracts (ensures/requires) в LLVM

**Files:**
- Modify: `src/codegen_llvm.c`

- [ ] **Step 1: fprintf/exit декларации**

```c
/* в codegen_llvm, до printf: */
LLVMTypeRef fprintf_ty = LLVMFunctionType(lg.i32_ty, (LLVMTypeRef[]){lg.ptr_ty, lg.ptr_ty}, 2, 1);
lg.fprintf_fn = LLVMAddFunction(lg.mod, "fprintf", fprintf_ty);
LLVMTypeRef exit_ty = LLVMFunctionType(lg.void_ty, (LLVMTypeRef[]){lg.i32_ty}, 1, 0);
lg.exit_fn = LLVMAddFunction(lg.mod, "exit", exit_ty);
lg.stderr_ptr = LLVMAddGlobal(lg.mod, lg.ptr_ty, "stderr"); /* external global */
```

(За glibc `stderr` е global — `LLVMAddGlobal` без initializer го декларира като external. Load преди всяко fprintf повикване.)

- [ ] **Step 2: spec_fail helper в IR**

Генерирай веднъж `baga_spec_fail(ptr spec, ptr kind, i64 idx, ptr expr)` като LLVM функция: двата формат низа като globals (БАЙТОВО като C: `spec '%s': requires #%lld нарушено: %s\n` / `spec '%s': ensures #%lld нарушена: %s\n`), `strcmp` декларация за избор, fprintf + exit(1).

- [ ] **Step 3: Wrapper емисия**

Огледало на codegen_c: за функция със spec (requires/ensures) — тялото става `b___impl_<mangled>` (внимание: llvm_mangle дава decimal байтове — `b__209_...`; impl името трябва да е уникално: mangling на `"__impl_" + име`), публичното име става wrapper: requires проверки (condbr → fail block: call baga_spec_fail с global strings за spec име и ensure текст; екраниране не е нужно — `LLVMBuildGlobalStringPtr` поема байтовете), после call impl, store в `b_output` alloca, ensures проверки, load + ret. Void функции с requires — както при C.

Важно: `find_ensures_spec` логиката трябва и тук — копирай я (тя е малка) или я изнеси… не — copy-paste в codegen_llvm.c е честният минимален ход (двата backend-а са независими).

- [ ] **Step 4: Проверка**

Run: `make llvm && ./tests/llvm_oracle.sh`
Expected: **всички** редове OK освен SKIP за effects, strings, tochka, vec. Ключовото: `spec_ensures_fail` и `spec_requires_fail` дават БАЙТОВО същия изход и exit=1 през `lli-14`.

---

### Task 4: Makefile + документация

**Files:**
- Modify: `Makefile`, `docs/compiler-bg.md`, `docs/compiler-en.md`

- [ ] **Step 1: Makefile**

```make
test-llvm: baga baga-llvm
	@./tests/llvm_oracle.sh
```

и в `test` target-а, накрая:

```make
	@if [ -f ./baga-llvm ]; then $(MAKE) -s test-llvm; else echo "(baga-llvm липсва — пропускам LLVM оракула)"; fi
```

- [ ] **Step 2: Документация**

В `docs/compiler-bg.md` LLVM секцията: поддържаното (списъка от дизайна), честният отказ с примерно съобщение, оракулът `tests/llvm_oracle.sh`. Същото в `docs/compiler-en.md`.

- [ ] **Step 3: Финална регресия**

Run: `make clean && make && make llvm && make test`
Expected: всичко зелено, включително оракула.

---

## Self-Review бележки

- Spec coverage: symbol table/изрази/assign/предекларации (T1), f64/enum/match (T2), contracts (T3), оракъл/Makefile/docs (T1 S1, T4). main.c подредба (T1 S2).
- Тънки места: (1) bool print форматът трябва да е огледало на C backend-а — oracle-ът ще хване разлика; (2) `&&`/`||` short-circuit поведение — огледало на C; (3) match с гол израз = return от функцията — огледало на codegen_c; (4) stderr global за fprintf — glibc-специфично, но това е и C target-ът; (5) llvm_mangle impl име — `"__impl_"` префиксът минава през mangle без промяна (само букви/долни черти).
