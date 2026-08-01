# Cranelift JIT backend (през Rust FFI) — Дизайн

> Дата: 2026-08-01. Статус: одобрен за планиране (auto mode).
> Фаза 3 от пътната карта: LLVM backend-ът е готов (17/17 в оракула, 0 SKIP);
> този milestone добавя Cranelift — in-process JIT, основата за REPL.

## Емпирично проучване (преди решението)

Пробата `/tmp/clifprobe` (Rust staticlib + C caller) доказа:

1. **Работи:** Rust `staticlib` с `cranelift-jit/codegen/frontend/native/module`
   (v0.134) се билдва; C го линква с `gcc ... -lpthread -ldl -lm`; C вика JIT-а
   през `extern "C"` FFI; Cranelift JIT-ва функция **in-process**, която вика
   libc `printf` с низ от data сегмент + i32 аргумент; изходът и exit кодът са
   коректни. Ключови настройки: `is_pic=false`, `use_colocated_libcalls=false`.
   `FunctionBuilder` е в `cranelift-frontend` (не в `cranelift-codegen`).

2. **Не работи (отхвърлен път „C генерира `.clif` текст → Rust парсва → JIT"):**
   `cranelift_reader::parse_functions` връща `Vec<Function>`, но външните имена
   (`%printf`) се превръщат в `UserExternalNameRef` към таблица
   (`predeclared_external_names`), която парсърът **изхвърля** при връщане
   (потвърдено в `cranelift-reader-0.134.3/src/parser.rs`). Имената не могат да
   се възстановят → JIT-ът не може да резолвне libc символите на парснати
   функции. Освен това ръчно генерираният `.clif` е придирчив (calling
   conventions, preamble `fnN = %name(...)` референции — пробата няколко пъти
   се счупи на синтаксис). `clif-util run` ползва интерпретатор, не JIT —
   текст→JIT не е утъпкан път.

## Архитектурно решение

**C генерира сериализиран per-function IR (компактен bytecode); Rust staticlib
го интерпретира в Cranelift IR и го JIT-ва in-process.** (Architecture B.)

- Codegen-ът (AST → bytecode) остава в C, огледален на `codegen_llvm.c`.
- `FunctionBuilder` живее изцяло в Rust (няма cross-FFI lifetime проблеми —
  цяла функция се строи наведнъж в един Rust call).
- JIT-ът резолва имена, които Rust декларира директно (libc + runtime helpers)
  — няма name-loss.
- Това е утъпканият път (всички cranelift-jit примери строят програмно) и е
  основата за REPL (JIT модулът персистира между дефинициите).

## Принципи

1. **Никакви тихи стойности.** Всеки AST възел извън обхвата → compile-time
   грешка `baga: Cranelift backend: неподдържан конструкт '<какво>'` + exit 1.
2. **Байт за байт.** Наблюдаемото поведение (stdout, stderr, exit code) съвпада
   с C backend-а. Оракулът diff-ва.
3. **Огледало на C backend-а.** Формати на print (`%lld`, `%g`, `true`/`false`,
   `%s`), contract съобщения (`spec '%s': requires #%lld нарушено: %s\n` /
   `... ensures #%lld нарушена: %s\n`), f64 промоция — всичко 1:1 с `codegen_c.c`.

## Обхват (поддържано след този milestone)

- Типове: `i64`, `i32`, `f64`, `bool`, `str` (само литерали + print), `void`.
- Изрази: литерали, ident, binary (int + f64 с промоция i64→f64 като в C
  backend-а), unary (`-`, `!`), `&&`/`||` (без short-circuit — огледало на C),
  повиквания.
- Оператори: let, присвояване (`=`, `+=` …), return, if/else, while,
  for (`0..n`) с break/continue, match върху i64 (литерали + `_`), expr stmt.
- Enum варианти → i64 константи.
- `print`/`println`/`write` за i64/f64/bool/str.
- Програмни аргументи: `arg(i)`, `arg_count()` (като C backend-а).
- **Contracts:** wrapper pattern — `b___impl_<fn>` + публична функция с requires
  преди и ensures след повикването; нарушение → същото съобщение на stderr +
  exit(1). (Редът `  вход:` е само за C/`--test-specs`, тук не се печата.)

Очаквано зелени в оракула: zdravei, faktorial, fib, types, cvet, match, spec,
spec_ensures, spec_ensures_fail, spec_requires_fail, argv.

## Извън обхвата (честен отказ с грешка)

struct/field, Vec (`vec_*`), масиви `[T]`, str builtins (len/concat/substr/…),
`read_file`, референции `&`/`*`, match върху не-i64. Примерите effects, strings,
tochka, vec, vec_ann, vec_f64 остават „SKIP (неподдържан)" в оракула — с грешка,
не с грешен изход. Те са обхват на следващия milestone „Cranelift пълно
покритие" (огледален на `2026-07-31-llvm-full-coverage`). REPL-ът (`--repl`)
е отделен бъдещ milestone; този JIT е основата му.

## Сериализиран IR (bytecode)

C emit-ва поток от инструкциите в байтов буфер; Rust го чете и строи Cranelift.
**Стойностен модел:** стекова машина — всеки израз оставя стойност (и тип) върху
стека; Rust пази паралелен стек от `(cranelift Value, Type)`. Инвариант: в
началото на всеки блок (LABEL) стекът е празен (Baga е statement-ориентиран;
изрази не пресичат блокови граници).

Инструкции (opcode u8 + операнди, little-endian):

| Opcode | Операнди | Семантика |
|---|---|---|
| `ICONST` | i64 | push i64 константа |
| `FCONST` | f64 (8 байта) | push f64 константа |
| `BCONST` | u8 (0/1) | push bool |
| `SCONST` | u32 str_id | push ptr към интерниран низ |
| `LOAD` | u16 slot | push стойността на локал |
| `STORE` | u16 slot | pop → запис в локал |
| `ALLOCA` | u16 slot, u8 ty | задели локал (stack slot) |
| `BINOP` | u8 op | pop 2, push резултат (op кодира вид+тип: add/sub/mul/div/rem × i/f, cmp eq/ne/lt/le/gt/ge × i/f → bool) |
| `AND`/`OR` | — | pop 2 bool, push bool (bitwise, без short-circuit) |
| `NOT` | — | pop bool, push !bool |
| `NEG` | u8 ty | pop, push -x (i/f) |
| `PROMOTE` | — | pop i64, push f64 (sitofp) |
| `CALL` | u32 fn_id, u16 nargs | pop nargs, push резултат (void → нищо) |
| `RET` | — | pop 1 → return |
| `RET_VOID` | — | return void |
| `BR` | u32 label | безусловен скок |
| `BR_FALSE` | u32 label | pop bool → скок ако 0 |
| `LABEL` | u32 label | граница на блок (Rust създава/влиза в block) |

`fn_id` индексира функцията в таблицата от функции (потребителска или runtime
helper). `label` е локален за функцията номератор. `str_id` индексира глобалната
таблица от низове (data сегменти в Rust).

## FFI интерфейс (C ↔ Rust)

Rust staticlib `cranelift/` → `libbaga_cranelift.a`. `extern "C"`:

```c
void *baga_jit_new(void);                       /* JIT модул + native ISA */
void  baga_jit_free(void *jit);
int   baga_jit_intern_str(void *jit, const char *bytes, size_t len); /* → str_id */
/* дефинира функция от bytecode; ret_ty/param_tys са Cranelift-типови кодове */
int   baga_jit_define(void *jit, const char *name,
                      int ret_ty, const int *param_tys, int nparams,
                      const unsigned char *code, size_t code_len);
int   baga_jit_run_main(void *jit);             /* finalize + call main → exit */
```

Runtime helpers (printf wrappers, `baga_arg`/`baga_arg_count`, `baga_spec_fail`,
`fprintf`/`exit`) Rust декларира/дефинира сам с фиксирани имена и сигнатури
(огледало на C preamble-а в `codegen_c.c`); C ги реферира по `fn_id` през
фиксирана споделена таблица (header `cranelift/baga_clif_rt.h` с enum-и).

## Засегнати компоненти

| Файл | Промяна |
|---|---|
| `cranelift/Cargo.toml`, `cranelift/src/lib.rs` | нов Rust staticlib: JIT модул, интерпретатор bytecode→Cranelift, runtime helpers |
| `cranelift/baga_clif_rt.h` | нови enum-и: opcodes, типови кодове, fn_id на runtime helpers (споделени C↔Rust) |
| `src/codegen_cranelift.c` | нов: AST → bytecode (емитър), symbol table име→slot, извиква FFI |
| `include/baga.h` | `#ifdef BAGA_CRANELIFT void codegen_cranelift(Node *program, int emit_only);` |
| `src/main.c` | dispatch: default-run през Cranelift JIT; `--emit-cranelift` → disassembly на bytecode (debug) |
| `tests/cranelift_oracle.sh` | нов: C (`./baga`) vs Cranelift JIT (`./baga-cranelift`) за всички примери |
| `Makefile` | `cranelift` target (cargo build + gcc link), `test-cranelift`, условно в `test` |
| `docs/compiler-bg.md`, `docs/compiler-en.md` | Cranelift секция: поддържано, честен отказ, оракул |

## Оракъл (tests/cranelift_oracle.sh)

За всеки `examples/*.baga` (без очакваните compile грешки spec_bad/vec_typed/
arg_type_bad):
1. `./baga $f` → out_c + exit_c (референция).
2. `./baga-cranelift $f` → out_cl + exit_cl (in-process JIT; ако завърши с
   „неподдържан конструкт" → `SKIP`).
3. `diff out_c out_cl` и `exit_c == exit_cl`, иначе `MISMATCH` + exit 1.

Приемливост: **12 OK** за ядрото (вкл. `effects` — `?`/`catch` са pass-through,
а примерът не вика `read_file`) + 5 SKIP (strings, tochka, vec, vec_ann,
vec_f64). `make test` зелен; `make self` непроменен (self-ът е C codegen, не
пипаме).

## Build бележки

- `cargo build --release` в `cranelift/` дава `target/release/libbaga_cranelift.a`.
- `baga-cranelift` = C обекти (без codegen_c.o не може — main ползва codegen_c за
  `--emit-c`/default C път; всъщност C обектите са същите като за `baga`) +
  `src/codegen_cranelift.c` + Rust staticlib, линкван с `-lpthread -ldl -lm`.
- `#ifdef BAGA_CRANELIFT` пази кода; без cargo/rust → `make cranelift` пропуска
  (като `make llvm` без LLVM). `make test` вика оракула само ако `baga-cranelift`
  съществува.
