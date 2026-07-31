# Self компилатор: --test-specs (property-based) (M5) — Дизайн

> Дата: 2026-07-31. Статус: одобрен (auto mode). Продължение на M4b (spec
> runtime). Цел: `baga2 --test-specs <файл>` да дава **байт-идентичен** изход с
> `baga --test-specs <файл>` (детерминирани бройки от xorshift PRNG).
> Проблем: self компилаторът няма режим `--test-specs` (property-based тестване
> на spec договорите) — само нормална компилация.

## Идея

`--test-specs` генерира случаен вход (PRNG), филтрира по requires, извиква
функцията (wrapper-ът от M4b проверява ensures) и брои минали/пропуснати:

```
факториел: 100/100 теста минаха (10065 пропуснати от requires)
Всички spec тестове минаха. ⚔️
```

При нарушена ensures → `baga_spec_fail` (контрапример + exit 1).

## Семантика (по модела на codegen_c.c:925-1000)

- **Флаг:** `baga2 --test-specs файл` → `arg(0)=="--test-specs"` → test режим,
  файл = `arg(1)`. Иначе файл = `arg(0)`, нормален режим.
- **parse_spec:** пази и **input**-ите (node 12 деца `"име:тип"`, като fn
  параметри) — досега се skip-ваха. (output 16, requires 31, ensures 32 — вече.)
- **spec_inputs_testable(spec):** всички input типове са `i64`/`bool` → 1.
- **emit_requires_predicate(spec):** `static int b__req_<име>(params) { return
  (r1) && (r2); }` (requires изразите, node 31).
- **test driver main** (в test режим, вместо нормалния main):
  - `baga_seed`/`baga_rand_i64` (xorshift, **същият** seed `0x243F6A8885A308D3`
    → детерминирани бройки); `baga_cur_args`/`baga_cur_nargs`.
  - за всеки тестируем spec: 100 теста (`BAGA_TEST_COUNT`), retry до 1000000
    (`BAGA_TEST_TRIES`) за вход, удовлетворяващ requires; bool → `rand(0,1)`,
    i64 → `rand(-1000,1000)`; вика fn (wrapper); брои passed/skipped;
    `printf("<име>: %d/100 теста минаха (%d пропуснати от requires)\n")`.
  - накрая `printf("Всички spec тестове минаха. ⚔️\n")`.
  - нетестируем spec → `printf("<име>: пропусната (неподдържан тип за
    --test-specs)\n")`.
- **baga_spec_fail** (self runtime): добавя "  вход: ..." (baga_cur_args) като C
  (за контрапримера).

## Засегнати компоненти

| Файл | Промяна |
|---|---|
| `self/compiler.baga` | parse_spec пази input (node 12); флаг `--test-specs` в main; `spec_inputs_testable`; `emit_requires_predicate`; `emit_test_driver`; runtime `baga_seed`/`baga_rand_i64`/`baga_cur_args`/`baga_cur_nargs` + counterexample в `baga_spec_fail` |

## Извън обхвата

- `--test-specs` за LLVM backend-а на self (self emit-ва само C).
- Нестандартни входни типове (само i64/bool, като C).

## Приемливост

- `baga2 --test-specs examples/spec_ensures.baga` ≡ `baga --test-specs ...`
  (`факториел: 100/100 теста минаха (10065 пропуснати от requires)` + финал).
- `baga2 --test-specs examples/spec_ensures_fail.baga` → exit 1, `ensures #1
  нарушена` (като baga).
- Нормалният режим (19/19) без регресия; `make self` fixed point държи;
  `make test` зелен.
