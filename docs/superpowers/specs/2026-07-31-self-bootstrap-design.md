# Възпроизводим self-hosting bootstrap (M0) — Дизайн

> Дата: 2026-07-31. Статус: одобрен (auto mode). Първи milestone от
> self-hosted catch-up.
> Проблем: self-hosting-ът (`baga2 == baga3`) е деклариран в README/blog като
> стълб, но (1) self компилаторът чете **твърдо кодиран** файл
> (`examples/zdravei.baga`, self/compiler.baga:866), не argv; (2) езикът
> **няма достъп до програмни аргументи** — генерираният C `main` е
> `int main(void)` (codegen_c.c:1104), няма builtin `arg()`; (3) няма скрипт
> в build-а, който да възпроизвежда bootstrap-а. Без argv self компилаторът
> не може да чете произволен източник, т.е. не може да компилира сам себе си
> по възпроизводим начин.

## Идея

Две свързани, чисто адитивни промени:

**M0a — програмни аргументи като езикова фича.** Нови builtins:

```baga
fn main() {
    print(arg_count())   // брой аргументи БЕЗ името на програмата → i64
    print(arg(0))        // първият аргумент (0-based) → str; "" ако няма
}
```

**M0b — bootstrap в build-а.** self компилаторът чете източника от `arg(0)`;
неговият emit-нат C runtime също носи `baga_arg`/`baga_arg_count` (за да може
`baga2` да компилира програми, които ползват argv — вкл. самия compiler.baga
за `baga3`). Нов `make self` target възпроизвежда:

```
baga  → self/compiler.baga → s2.c → gcc → baga2     (C bootstrap)
baga2 → self/compiler.baga → s3.c → gcc → baga3     (self компилира себе си)
baga3 → self/compiler.baga → s4.c                    (self компилира себе си)
diff s3.c s4.c  → празен ⇒ инвариантът държи
```

**Какъв е инвариантът.** `baga` (C bootstrap) и `baga2` (self) са два различни
компилатора с различен codegen (различни банери, форматиране, helper-и) —
`s2.c` и `s3.c` естественно се различават и това НЕ е грешка. Смисленият
self-hosting инвариант е **fixed point**: self компилаторът възпроизвежда себе
си, т.е. `baga2(compiler.baga) == baga3(compiler.baga)` (`s3.c == s4.c`). Това
е точният смисъл на „baga2 == baga3" — като *компилатори* (дават идентичен
изход за един и същ вход), не като байтове на междинния C спрямо C bootstrap-а.

## Семантика

### M0a: `arg_count` / `arg`
- **Конвенция:** `arg_count()` връща броя аргументи **без** името на програмата
  (`argc - 1`, но не по-малко от 0). `arg(i)` е 0-based върху потребителските
  аргументи: `arg(0)` е първият след името на програмата. Извън границите →
  `""` (без crash). Четенето на argv е чисто (без `!IO` ефект).
- **Checker:** `arg_count` → ret `i64`, 0 параметъра; `arg` → ret `str`,
  1 параметър (вградената builtins таблица, checker.c ~ред 390). Без ефект.
- **C backend:**
  - runtime: глобални `static int baga_argc = 0; static char **baga_argv = 0;`
    + `baga_arg_count()` (връща `baga_argc > 0 ? baga_argc - 1 : 0`)
    + `baga_arg(int64_t i)` (връща `baga_argv[i+1]`, ако `i+1 < baga_argc`,
    иначе `""`).
  - main wrapper: `int main(void)` → `int main(int argc, char **argv)`,
    задава `baga_argc = argc; baga_argv = argv;` преди `b_main()`. (И в
    нормалния път codegen_c.c:1104, и в test-driver-а codegen_c.c:944 —
    глобалите са zero-init, така че `--test-specs` пътят е безопасен.)
  - builtin map (codegen_c.c ~ред 298): `{"arg_count","baga_arg_count"}`,
    `{"arg","baga_arg"}`.
- **LLVM backend:** симетрично — `main` става `(i32, [i32, ptr])`, запазва ги в
  IR глобални; `baga_arg_count`/`baga_arg` като lazy IR функции (по модела на
  `build_baga_vec_push_i64`). Нужно е, за да е фичата честна и в двата backend-а
  (оракулът ги сравнява).
- **Грешки:** няма нови — `arg`/`arg_count` са builtins; грешен брой аргументи
  се хваща от съществуващата проверка.

### M0b: self compiler + bootstrap
- **self/compiler.baga main():** `read_file("examples/zdravei.baga")` →
  `read_file(arg(0))`. (Пази `catch !IO => ""`.)
- **self emit runtime:** към блока с `write(...)` runtime helper-и се добавят
  `baga_argc`/`baga_argv` глобални + `baga_arg_count`/`baga_arg`, и emit-натият
  `main` става `int main(int argc, char **argv)` със задаване на глобалите.
- **self emit builtins:** `arg_count`/`arg` се map-ват към `baga_arg_count`/
  `baga_arg` (където self компилаторът map-ва builtins — emit_expr).
- **Makefile `self` target:**
  1. `./baga --emit-c self/compiler.baga > /tmp/baga_self2.c && gcc … -o baga2`
  2. `./baga2 self/compiler.baga > /tmp/baga_self3.c && gcc … -o baga3`
  3. `./baga3 self/compiler.baga > /tmp/baga_self4.c`
  4. `diff /tmp/baga_self3.c /tmp/baga_self4.c` → ако е празен, fixed point-ът
     държи (baga2 и baga3 са идентични компилатори).
  - Забележка: `baga` (C bootstrap) приема `--emit-c`; `baga2`/`baga3` (self)
    не — те винаги emit-ват C на stdout и четат файла от `arg(0)`. Скриптът
    отчита тази разлика (само стъпка 1 ползва `--emit-c`).

## Засегнати компоненти

| Файл | Промяна |
|---|---|
| `src/checker.c` | `arg_count`/`arg` в builtins таблицата |
| `src/codegen_c.c` | runtime globals + `baga_arg_count`/`baga_arg`; `main(argc,argv)`; builtin map |
| `src/codegen_llvm.c` | IR globals + lazy `baga_arg_count`/`baga_arg`; `main(argc,argv)` wrapper; builtin map |
| `self/compiler.baga` | main чете `arg(0)`; emit-ва runtime `baga_arg*` + `main(argc,argv)`; map-ва `arg`/`arg_count` |
| `examples/argv.baga` | нов: print `arg_count()` + `arg(0)` (в оракула; без аргументи → `0` и ``) |
| `Makefile` | `self` target (baga2/baga3 + diff на генерирания C) |
| `docs/language-bg.md`, `docs/language-en.md` | §19 (вградени функции): `arg_count`/`arg`; бележка за self-hosting |

## Извън обхвата

- Синтактичен паритет на self парсера (`Vec<T>`, `spec` блокове, struct,
  match) — следващи milestone-и (M1+).
- Checker фаза в self компилатора — M3.
- `ensures`/`requires` codegen в self — M4.
- CLI флагове (`--emit-c` и пр.) в self компилатора — не са нужни за
  bootstrap-а (baga2 чете файл, пише C на stdout).

## Приемливост

- `examples/argv.baga` → `0` и празен ред (без аргументи), идентично през C и
  LLVM; ръчно през gcc `/tmp/a x y` → `2` и `x` (baga не препредава argv;
  lli-14 също: `lli-14 /tmp/a.ll x y` → `2` и `x`).
- Оракулът става 16/16 (+ argv.baga).
- `make self` → fixed point: `diff baga_self3.c baga_self4.c` празен
  (baga2 и baga3 са идентични компилатори), exit 0.
- `make test` зелен; `self/*.baga` се компилират от `baga`.
