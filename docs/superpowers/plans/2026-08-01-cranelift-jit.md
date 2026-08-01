# Cranelift JIT backend — Имплементационен план

> **For agentic workers:** Steps use checkbox (`- [ ]`) syntax.
> **ВАЖНО:** БЕЗ `git commit`/`git add` — git мутации само с одобрение.

**Goal:** Cranelift JIT backend през Rust FFI: C генерира сериализиран bytecode,
Rust staticlib го JIT-ва in-process. Критерий: `tests/cranelift_oracle.sh`
сравнява C (`./baga`) vs Cranelift JIT (`./baga-cranelift`) за всички примери —
байт за байт + exit кодове. Ядрото (11 примера) OK; struct/Vec/str/effects —
честен отказ (SKIP).

**Architecture:** Стеков bytecode (opcode u8 + операнди); Rust пази стек от
`(Value, Type)`; цяла функция се строи наведнъж в един Rust call (FunctionBuilder
не пресича FFI). Runtime helpers (print, spec_fail, arg) Rust декларира сам;
C ги реферира по фиксиран `fn_id`. JIT: `is_pic=false`, `use_colocated_libcalls=false`.

**Spec:** `docs/superpowers/specs/2026-08-01-cranelift-jit-design.md` (прочети го —
съдържа емпиричното решение и таблицата с opcode-ове!)

**Референции, които ТРЯБВА да прочетеш преди код:**
- `src/codegen_llvm.c` — МОДЕЛЪТ: symbol table, emit_expr/emit_stmt структура,
  f64 промоция, match, contracts wrapper, print по типове. Cranelift emitter-ът
  следва същата рекурсивна структура, но emit-ва bytecode вместо LLVM IR.
- `src/codegen_c.c` — форматите (print `%lld`/`%g`/`true`/`false`/`%s`),
  `baga_spec_fail` съобщенията (БАЙТОВО същите!), mangle, implicit return, for.
- `include/baga.h` — AST възлите (NODE_*), Type.
- `src/main.c` — dispatch, къде `check_program` е преди codegen.
- Работещ Rust JIT seed (от пробата `/tmp/clifprobe/src/lib.rs` — възпроизведен
  долу в Task 1, стъпка 2; ако `/tmp` е изчистен, ползвай inline кода).

## Global Constraints

- Rust 1.97 + cargo (налични). Cranelift 0.134 (`cranelift-codegen/frontend/jit/
  module/native`). `FunctionBuilder`/`FunctionBuilderContext` са в
  `cranelift-frontend`. `settings::Builder::set` изисква `use ...::Configurable`.
  `get_finalized_function` връща `*const u8` (не Result).
- Линк от C: `gcc ... libbaga_cranelift.a -lpthread -ldl -lm`.
- Съобщения за грешки — на български. `baga_spec_fail` изход — БАЙТОВО като C.
- НИКАКВИ тихи стойности: неподдържан възел →
  `baga: Cranelift backend: неподдържан конструкт '<описание>'` на stderr + exit 1.
- Без git мутации. Build: `make cranelift`. Регресия: `make test`, `make self`.

---

### Task 1: Build glue — Rust staticlib + Makefile + падащ оракъл

**Files:**
- Create: `cranelift/Cargo.toml`, `cranelift/src/lib.rs`, `cranelift/baga_clif_rt.h`
- Create: `tests/cranelift_oracle.sh` (chmod +x)
- Modify: `Makefile`

**Interfaces:**
- Produces: `make cranelift` билдва `baga-cranelift`; оракълът стартира и дава
  FAIL/SKIP (червено) — все още няма реален codegen.

- [ ] **Step 1: Падащ тест (оракулът)**

`tests/cranelift_oracle.sh` (chmod +x) — огледален на `tests/llvm_oracle.sh`, но
без `lli` (JIT-ът е in-process):

```bash
#!/bin/bash
# Оракъл: сравнява C backend и Cranelift JIT (in-process) за всички примери.
cd "$(dirname "$0")/.."
FAIL=0
for f in examples/*.baga; do
    case "$f" in
        examples/spec_bad.baga|examples/vec_typed.baga|examples/arg_type_bad.baga) continue;;
    esac
    ./baga "$f" > /tmp/baga_c_out.txt 2>&1; rc_c=$?
    ./baga-cranelift "$f" > /tmp/baga_cl_out.txt 2>/tmp/baga_cl_err.txt; rc_l=$?
    if grep -q "неподдържан конструкт" /tmp/baga_cl_err.txt; then
        echo "SKIP  $f (честен отказ)"; continue
    fi
    if [ $rc_c -ne $rc_l ] || ! diff -q /tmp/baga_c_out.txt /tmp/baga_cl_out.txt > /dev/null; then
        echo "MISMATCH $f (exit C=$rc_c CL=$rc_l)"; FAIL=1
    else
        echo "OK    $f"
    fi
done
[ $FAIL -eq 0 ] && echo "--- оракулът е доволен ⚔️"
exit $FAIL
```

- [ ] **Step 2: Rust staticlib — работещ JIT seed**

`cranelift/Cargo.toml`:

```toml
[package]
name = "baga_cranelift"
version = "0.1.0"
edition = "2021"

[lib]
crate-type = ["staticlib"]

[dependencies]
cranelift-codegen = "0.134.3"
cranelift-frontend = "0.134.2"
cranelift-jit = "0.134.3"
cranelift-module = "0.134.3"
cranelift-native = "0.134.3"

[profile.release]
opt-level = 2
```

`cranelift/src/lib.rs` — seed (ДОКАЗАН работещ от пробата; само `baga_jit_new`/
`free` + hardcoded hello за сега; интерпретаторът идва в Task 2):

```rust
use std::os::raw::{c_char, c_int};
use cranelift_codegen::ir::{types, AbiParam, InstBuilder, UserFuncName};
use cranelift_codegen::settings::{self, Configurable};
use cranelift_frontend::{FunctionBuilder, FunctionBuilderContext};
use cranelift_jit::{JITBuilder, JITModule};
use cranelift_module::{DataDescription, Linkage, Module};

struct Jit { module: JITModule }

fn build_module() -> JITModule {
    let mut fb = settings::builder();
    fb.set("use_colocated_libcalls", "false").unwrap();
    fb.set("is_pic", "false").unwrap();
    let isa = cranelift_native::builder().expect("host ISA")
        .finish(settings::Flags::new(fb)).unwrap();
    JITModule::new(JITBuilder::with_isa(isa, cranelift_module::default_libcall_names()))
}

#[no_mangle] pub extern "C" fn baga_jit_new() -> *mut Jit {
    Box::into_raw(Box::new(Jit { module: build_module() }))
}
#[no_mangle] pub extern "C" fn baga_jit_free(p: *mut Jit) {
    if !p.is_null() { unsafe { drop(Box::from_raw(p)); } }
}
```

- [ ] **Step 3: baga_clif_rt.h — споделени enum-и**

`cranelift/baga_clif_rt.h` — opcode-ове (от spec таблицата), типови кодове
(`TY_VOID=0, TY_I64=1, TY_I32=2, TY_F64=3, TY_BOOL=4, TY_PTR=5`), BINOP кодове,
и `fn_id` enum за runtime helpers (`RT_PRINT_I64, RT_PRINT_F64, RT_PRINT_BOOL,
RT_PRINT_STR, RT_PRINTLN_I64, ..., RT_ARG, RT_ARG_COUNT, RT_SPEC_FAIL, ...`).
Този header се include-ва от `codegen_cranelift.c`; Rust страната дефинира
същите стойности като `const` (ръчно синхронизирани — коментирай „keep in sync").

- [ ] **Step 4: Makefile**

Добави (по модела на `llvm` target-а):

```make
CRANELIFT_DIR := cranelift
CRANELIFT_LIB := $(CRANELIFT_DIR)/target/release/libbaga_cranelift.a
CRANELIFT_BIN := baga-cranelift

cranelift: $(CRANELIFT_BIN)

$(CRANELIFT_LIB):
	cargo build --release --manifest-path $(CRANELIFT_DIR)/Cargo.toml

$(CRANELIFT_BIN): $(OBJS) src/codegen_cranelift.o $(CRANELIFT_LIB)
	$(CC) $(CFLAGS) -DBAGA_CRANELIFT -I$(CRANELIFT_DIR) -o $@ \
	    src/main.c src/lexer.c src/parser.c src/checker.c src/codegen_c.c \
	    src/proofs.c src/codegen_cranelift.c $(CRANELIFT_LIB) -lpthread -ldl -lm

test-cranelift: $(BIN) $(CRANELIFT_BIN)
	@./tests/cranelift_oracle.sh
```

и в `test`, до LLVM оракула:

```make
	@if [ -f ./$(CRANELIFT_BIN) ]; then $(MAKE) -s test-cranelift; else echo "(baga-cranelift липсва — пропускам Cranelift оракула)"; fi
```

Забележка: `codegen_cranelift.c` още не съществува — Task 3. За да се билдва
`baga-cranelift` в Task 1/2, създай stub `src/codegen_cranelift.c` с
`void codegen_cranelift(Node *p, int e){ (void)p;(void)e; fprintf(stderr,"baga: Cranelift backend: неподдържан конструкт 'stub'\n"); exit(1); }`
(под `#ifdef BAGA_CRANELIFT`).

- [ ] **Step 5: Проверка**

Run: `make cranelift && ./baga-cranelift examples/zdravei.baga; ./tests/cranelift_oracle.sh`
Expected: билдва се; stub-ът дава „неподдържан конструкт 'stub'" → оракулът
 Skip-ва всичко или FAIL-ва (червено — очаквано).

---

### Task 2: Rust — bytecode интерпретатор + runtime helpers

**Files:**
- Modify: `cranelift/src/lib.rs`

**Interfaces:**
- `baga_jit_intern_str`, `baga_jit_define`, `baga_jit_run_main` (от spec).
- Runtime helpers като Cranelift функции: `baga_print_i64/f64/bool/str`,
  `baga_println_*` (с `\n`), `baga_write_str` (без `\n`), `baga_arg`,
  `baga_arg_count`, `baga_spec_fail`. Форматите — 1:1 с codegen_c.c.
- Produces: `baga_jit_define` приема bytecode и го JIT-ва; unit тест с ръчно
  написан bytecode минава.

- [ ] **Step 1: Интерниране на низове (data сегменти)**

`baga_jit_intern_str(jit, bytes, len) -> c_int`: `declare_data` +
`DataDescription::define(bytes)`; върни `DataId` като i32 (пази `Vec<DataId>` в
`Jit`). В bytecode `SCONST str_id` → `declare_data_in_func` + `symbol_value`.

- [ ] **Step 2: Runtime helpers**

Декларирай libc (`printf`, `fprintf`, `exit`, `malloc`…) като Import с точни
сигнатури. Дефинирай `baga_print_i64` = `printf("%lld\0", x)` и т.н. — форматите
КОПИРАЙ от codegen_c.c (`baga_print_*`). `baga_spec_fail(spec, kind, idx, expr)`
— двата формат низа БАЙТОВО като C; `strcmp` за избор requires/ensures; fprintf
към stderr + exit(1). Пази `argc/argv` в global-и (като LLVM backend-а) —
`baga_arg_count`/`baga_arg` четат от тях.

- [ ] **Step 3: Интерпретатор bytecode → Cranelift**

`baga_jit_define(jit, name, ret_ty, param_tys, nparams, code, code_len)`:
1. `declare_function(name, Local, sig)` (sig от ret_ty/param_tys).
2. `make_context`; `FunctionBuilder::new`.
3. **Два прохода:** (a) сканирай bytecode за `LABEL` → създай blocks
   (`create_block`), пази `label → Block` map; (b) emit: върви по инструкциите,
   поддържай стек `Vec<(Value, Type)>`. `LABEL` → `switch_to_block` +
   `seal_block` (всички блокове се seal-ват при влизане, защото скоковете са
   напред/назад към вече-или-предстоящ блок; ако forward branch към несъздаден
   блок — създай го при `BR*` и seal-вай при `LABEL`).
4. Стекова логика: `ICONST`→`iconst I64`; `FCONST`→`f64const`; `BINOP`→pop 2,
   `iadd/fadd/…` или `icmp/fcmp` (→ I8 bool); `PROMOTE`→`fcvt_from_sint`;
   `LOAD/STORE`→`stack_load/stack_store` (slot → `create_stack_slot` веднъж);
   `CALL fn_id nargs`→pop nargs, `call` (func ref от declare_func_in_func за
   runtime helper или потребителска функция); `RET`→`return_(&[v])`;
   `BR_FALSE`→`brif cond, then_bb, [], else_bb, []` (else = следващият блок).
5. `finalize(target_config)`; `module.define_function`.

Сигнатури на потребителски функции: C подава ret_ty/param_tys; за рекурсия
всички функции се `declare`-ват ПРЕДИ да се `define`-нат (C вика `baga_jit_define`
два прохода: declare-all после define-all — или Rust пази pending decls; по-просто:
C прави declare pass през отделен FFI `baga_jit_declare` — добави го ако трябва).

- [ ] **Step 4: Unit тест (cargo test)**

В `lib.rs` `#[cfg(test)]`: ръчно сглоби bytecode за `fn main(){ print(40+2); return 0 }`
(`[ALLOCA?, ICONST 40, ICONST 2, BINOP add_i, CALL RT_PRINT_I64 1, ICONST 0, RET]`),
`baga_jit_define` + `baga_jit_run_main`; assert exit 0. (По-добре: integration
тест в `cranelift/tests/`, който печата и проверява stdout — но за пръв проход
assert-ни exit кода.)

Run: `cargo test --manifest-path cranelift/Cargo.toml --release`
Expected: зелено.

---

### Task 3: C emitter — AST → bytecode (`src/codegen_cranelift.c`)

**Files:**
- Modify: `src/codegen_cranelift.c` (замени stub-а)
- Modify: `include/baga.h` (декларация под `#ifdef BAGA_CRANELIFT`)

**Interfaces:**
- `void codegen_cranelift(Node *program, int emit_only)`:
  - `emit_only=1` (`--emit-cranelift`) → печата human-readable disassembly на
    bytecode-а (debug; не вика Rust).
  - `emit_only=0` → интернира низове, declare+define всички функции през FFI,
    `baga_jit_run_main` → exit code → `exit()`.
- Symbol table име→u16 slot (scope push/pop), огледална на codegen_llvm.c.
- Emit-ва bytecode в динамичен буфер (`unsigned char *`, realloc).

- [ ] **Step 1: Buffer + symbol table + disassembler**

`Buf { unsigned char *d; size_t n, cap; }` с `emit_u8/u16/u32/i64/f64`.
Symbol table: име→slot (i64/f64/bool/ptr slot-ове; типът се пази отделно за
LOAD). `cr_unsupported(what)` → грешка + exit 1 (съобщението от spec).
Disassembler: switch по opcode → печата четим ред (за `--emit-cranelift`).

- [ ] **Step 2: Изрази (`cr_expr`)**

Огледало на `emit_expr_llvm`:
- `NODE_IDENT`: slot → `LOAD`; ако не е локал → enum вариант (програма items) →
  `ICONST index`; иначе `cr_unsupported`.
- `NODE_BINARY`: emit ляво, дясно; определи f64 по типовете (промоция `PROMOTE`
  ако единият е f64); `BINOP` с конкретен код. `&&`/`||`→`AND`/`OR` (без
  short-circuit — огледало на C). Сравнения → `BINOP cmp_*` (→ bool).
- `NODE_UNARY`: `-`→`NEG ty`; `!`→`NOT`; `&`/`*`→`cr_unsupported`.
- `NODE_CALL`: `print`/`println`/`write` → emit аргументи + `CALL RT_PRINT_* 1`
  (по тип на аргумента); `arg`/`arg_count` → `CALL RT_ARG/RT_ARG_COUNT`;
  потребителска функция → `CALL fn_id nargs`; друг builtin → `cr_unsupported`.
- Литерали: `ICONST`/`FCONST`/`BCONST`/`SCONST` (intern низа).

- [ ] **Step 3: Оператори (`cr_stmt`)**

- `NODE_LET`: `ALLOCA slot ty`; emit init; `STORE slot`; define в symtab.
- `NODE_ASSIGN`: emit стойност (за `+=` — emit LOAD + BINOP + …, огледало на C);
  `STORE`. Не-ident target → `cr_unsupported`.
- `NODE_IF`: emit cond; `BR_FALSE else`; then; `BR end`; `LABEL else`; else;
  `LABEL end`. (Label-и = локален брояч.)
- `NODE_WHILE`: `LABEL cond`; cond; `BR_FALSE end`; тяло; `BR cond`; `LABEL end`.
  break→`BR end_label`, continue→`BR cond_label` (подай ги рекурсивно).
- `NODE_FOR`: alloca i; `ICONST lo; STORE i`; `LABEL cond`; `LOAD i; ICONST hi;
  BINOP lt_i; BR_FALSE end`; тяло; `LABEL incr`; `LOAD i; ICONST 1; BINOP add_i;
  STORE i; BR cond`; `LABEL end`. continue→incr, break→end.
- `NODE_MATCH` (i64): за всеки клон `LOAD scrut; ICONST pat; BINOP eq_i;
  BR_FALSE next`; тяло; `BR end`; `LABEL next`; wildcard→тяло. Не-i64 →
  `cr_unsupported`.
- `NODE_RETURN`: emit стойност → `RET`; void → `RET_VOID`.
- `NODE_EXPR_STMT`: `cr_expr` + (ако оставя стойност) pop чрез dummy `STORE` в
  scratch slot или `DROP` opcode (добави `DROP` ако трябва — по-просто: изрази
  като statements са само call-ове, които не връщат нищо ползваемо; ако връщат —
  добави `DROP`).

- [ ] **Step 4: Функции + contracts + main wrapper**

- Първи проход: `baga_jit_declare` за всички NODE_FN (точни типове) — за рекурсия.
- Втори проход: `cr_fn` → bytecode буфер; `baga_jit_define`. Implicit return
  (последен expr stmt при не-void → RET) — огледало codegen_c.
- Contracts: за функция със spec — тялото в `b___impl_<mangle>`, публичната е
  wrapper: requires проверки (`BR_FALSE ok` → `CALL RT_SPEC_FAIL ...` който
  exit-ва), call impl, ensures проверки. Намери spec-а (`find_ensures_spec` —
  копирай малката логика от codegen_llvm.c). `RT_SPEC_FAIL` аргументи: spec име
  (str_id), kind (str_id "requires"/"ensures"), idx (i64), expr текст (str_id).

- [ ] **Step 5: Проверка (disassembly)**

Run: `make cranelift && ./baga-cranelift --emit-cranelift examples/faktorial.baga`
Expected: четим bytecode disassembly, без грешки. (Все още не се изпълнява.)

---

### Task 4: main.c wiring + изпълнение

**Files:**
- Modify: `src/main.c`

**Interfaces:**
- `baga-cranelift файл.baga` → `codegen_cranelift(program, 0)` (JIT + run).
- `baga-cranelift --emit-cranelift файл.baga` → `codegen_cranelift(program, 1)`.
- `check_program` е ПРЕДИ codegen (както за LLVM).

- [ ] **Step 1: Dispatch**

Добави флаг `emit_cranelift`. Под `#ifdef BAGA_CRANELIFT`: ако `emit_cranelift`
→ `codegen_cranelift(program, 1); return 0;`. Default път (не emit_c/emit_llvm):
ако binary-то е `baga-cranelift` (т.е. `BAGA_CRANELIFT` дефиниран) →
`codegen_cranelift(program, 0)` вместо gcc-компилация. (Внимание: `baga` и
`baga-cranelift` се билдват от един main.c — `BAGA_CRANELIFT` macro-то ги
различава; default C път остава за `baga`.)

- [ ] **Step 2: Проверка**

Run: `make cranelift && ./baga-cranelift examples/zdravei.baga && ./baga-cranelift examples/faktorial.baga`
Expected: `Здравей, багатуре. Боят започва.` и факториелите — байт като `./baga`.

---

### Task 5: Оракъл зелено + документация

**Files:**
- Modify: `docs/compiler-bg.md`, `docs/compiler-en.md`

- [ ] **Step 1: Оракъл**

Run: `./tests/cranelift_oracle.sh`
Expected: OK за zdravei, faktorial, fib, types, cvet, match, spec, spec_ensures,
spec_ensures_fail, spec_requires_fail, argv (11); SKIP за effects, strings,
tochka, vec, vec_ann, vec_f64 (6). `spec_ensures_fail`/`spec_requires_fail` —
БАЙТОВ exit=1 + същото съобщение.

- [ ] **Step 2: Документация**

Cranelift секция в `docs/compiler-bg.md` (и EN): поддържаното (списък), честният
отказ + примерно съобщение, оракулът, архитектурата (C→bytecode→Rust JIT),
build (`make cranelift`, изисква cargo). Отбележи: REPL е бъдещ milestone.

- [ ] **Step 3: Финална регресия**

Run: `make clean && make && make llvm && make cranelift && make test && make self`
Expected: всичко зелено — C тестове, LLVM оракул (17/17), Cranelift оракул
(11 OK + 6 SKIP), self fixed point.

---

## Self-Review бележки

- Spec coverage: build glue (T1), Rust интерпретатор+helpers (T2), C emitter (T3),
  main wiring (T4), оракъл/docs (T5). Contracts са в T3 Step 4.
- Тънки места: (1) **forward branches** — `BR` към label преди `LABEL`-а; реши се
  с two-pass (първо създай всички blocks от LABEL-ите, после emit) — така
  `switch_to_block`/`seal_block` са чисти. (2) **bool представяне** — Cranelift
  `icmp` дава `I8` (b1); print(bool) и `BR_FALSE` трябва да са консистентни
  (ползвай I8 навсякъде, `brif` работи с i8). (3) **f64 промоция** — `PROMOTE`
  само когато единият операнд е f64; огледало на codegen_llvm `coerce`.
  (4) **рекурсия** — declare pass ПРЕДИ define pass (T3 Step 4). (5) **spec_fail
  форматите** — копирай БАЙТОВО от codegen_c.c; оракулът diff-ва
  spec_ensures_fail. (6) **DROP** — ако expr-stmt оставя стойност, добави `DROP`
  opcode (pop без използване); спецификацията не го включва, добави при нужда и
  обнови spec таблицата. (7) cargo мрежа — налична (probe-ът теглеше crates).
