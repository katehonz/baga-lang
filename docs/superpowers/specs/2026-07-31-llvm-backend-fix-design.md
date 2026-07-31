# Поправка на LLVM backend-а — Дизайн

> Дата: 2026-07-31. Статус: одобрен за планиране (auto mode).
> Systematic debugging, Phase 1 (root cause): `NODE_IDENT` търси име само в
> globals, а локалите са alloca-та → винаги константа 0. Навсякъде default
> клонове emit-ват тихи нули вместо грешка. Функциите не се предекларират.

## Принципи на поправката

1. **Никакви тихи стойности.** Всеки AST възел, който backend-ът не поддържа,
   е compile-time грешка: `baga: LLVM backend: неподдържан конструкт '<какво>'`
   + exit 1. По-добре честен отказ от мълчаливо грешен код.
2. **Symbol table.** Линейна таблица име → alloca (`LLVMValueRef`), със
   scope push/pop по блоковете. `NODE_IDENT` → load от alloca;
   `NODE_LET`/`NODE_ASSIGN` → alloca/store. Параметрите се alloca-ват в entry.
3. **Предекларация на всички функции** преди телата (като в C backend-а).
4. **Оракъл:** скрипт, който сравнява изхода на C backend и LLVM backend
   (`lli-14`) за всички поддържани примери, вкл. exit кодовете. Договорите
   (ensures/requires) са готов оракъл за коректност върху рекурсия.

## Обхват (поддържано след поправката)

- Типове: `i64`, `i32`, `f64`, `bool`, `str` (само литерали + print), `void`.
- Изрази: литерали, ident, binary (int + f64 с промоция i64→f64 като в C
  backend-а: `f64` ако някой операнд е `f64`), unary (`-`, `!`), call.
- Оператори: let, присвояване (`=`, `+=` … — каквото parser-ът произвежда,
  огледало на `codegen_c.c`), return, if/else, while, for (`0..n` range),
  break, continue, match (i64 литерали + `_`), expr stmt.
- Enum варианти → i64 константи (от program items).
- `print`/`println` за i64/f64/bool/str — изходът трябва да съвпада БАЙТ ЗА
  БАЙТ с C backend-а (oracle-ът diff-ва).
- **Contracts:** wrapper pattern като C backend-а — `b___impl_<fn>` + публична
  функция с requires проверки преди и ensures след повикването; нарушение →
  `fprintf(stderr, ...)` (същите съобщения) + `exit(1)`. Редът `вход:` е
  само за C/`--test-specs`, тук не се печата.

## Извън обхвата (честен отказ с грешка)

struct/field, Vec, масиви, str builtins (len/concat/...), read_file, ефекти
(`?`/`catch`), референции `&`/`*`. Примерите effects/strings/tochka/vec остават
„SKIP (неподдържан)" в оракула — но вече с грешка, не с грешен изход.

## Засегнати компоненти

| Файл | Промяна |
|---|---|
| `src/codegen_llvm.c` | пренаписване на ядрото (~433 → ~700 реда): symbol table, предекларации, пълни изрази/оператори, match, print по типове, contract wrapper, fprintf/exit decls, грешки вместо тихи нули |
| `src/main.c` | увери се, че `check_program` върви ПРЕДИ `codegen_llvm` (типовете на възлите трябва да са попълнени) |
| `tests/llvm_oracle.sh` | нов: сравнява C vs LLVM изход за всички примери |
| `Makefile` | `test-llvm` target, викан от `test` (условно — само ако `baga-llvm` съществува) |
| `docs/compiler-bg.md`, `docs/compiler-en.md` | LLVM секцията: какво се поддържа, какво отказва |

## Оракъл (tests/llvm_oracle.sh)

За всеки `examples/*.baga`:
1. `./baga $f` → out_c + exit_c (референция; за `spec_bad.baga` — очаквана compile грешка, пропуска се).
2. `./baga-llvm --emit-llvm $f` → .ll; ако завърши с „неподдържан конструкт" → `SKIP`.
3. `lli-14` върху .ll → out_llvm + exit_llvm.
4. `diff out_c out_llvm` и `exit_c == exit_llvm`, иначе `MISMATCH` + exit 1.

Очаквано зелени: zdravei, faktorial, fib, match, cvet, types, spec,
spec_ensures, spec_ensures_fail (exit 1 + същото съобщение!), spec_requires_fail.
Очаквано SKIP: effects, strings, tochka, vec.
