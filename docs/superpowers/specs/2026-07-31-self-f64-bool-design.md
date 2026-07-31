# Self компилатор: f64 + bool (M1) — Дизайн

> Дата: 2026-07-31. Статус: одобрен (auto mode). Първи инкремент от
> self-hosted catch-up след M0. Подход: **евристики** (по модела на
> съществуващия `expr_is_str`), НЕ пълна checker фаза.
> Проблем (емпирично): self компилаторът е синтаксис-насочен транспилер без
> извеждане на типове. `types.baga` през baga2 дава `3 / 1 / 13 / 1` вместо
> `78.5397 / true / 13.5 / true` — f64 се третира като i64 (f64 литералите
> изобщо не се токенизират), bool се печата като число.

## Идея

Self компилаторът да разпознава f64 и bool достатъчно добре, че `types.baga`
да дава **байт-идентичен** изход с baga (и през двата пътя).

```baga
// types.baga — baga: 78.5398 / true / 13.5 / true (след precision fix)
fn кръг_лице(r: f64) -> f64 { return 3.14159265 * r * r }
let r: f64 = 5.0
let площ = кръг_лице(r)     // f64 — от return типа на fn
let z = x + y               // f64 — y е f64 (numeric promotion)
let по_голямо = x > 5        // bool — сравнение
```

## Предпоставка: precision fix в референтните backend-и

baga днес emit-ва float литералите с `%g` (codegen_c.c:205), което **губи
точност** (`3.14159265` → `3.14159` в генерирания C); LLVM backend-ът нарочно
round-trip-ва през `%g` (codegen_llvm.c:1008-1012), за да съвпада. За да може
self компилаторът да emit-ва **суровия текст** на литерала и пак да съвпада,
референтните backend-и минават на пълна точност:

- `codegen_c.c`: `%g` → `%.17g` (17 значещи цифри round-trip-ват IEEE double).
- `codegen_llvm.c`: маха се `%g` round-trip-а; `LLVMConstReal(double_ty,
  n->float_val)` директно.
- Резултат: baga-C ≡ baga-LLVM (оракълът остава зелен), литералите вече не губят
  точност, и baga2 (emit-ващ суровия текст) съвпада с тях.

## Семантика (self компилатор)

- **Tokenizer:** digit-клонът, след цялата част, ако следва `.` **и** цифра →
  консумира дробната част и emit-ва токен kind **102** (float). Guard-ът
  „`.` следвана от цифра" пази range-овете (`0..10` → `.` следвана от `.` →
  int, не float).
- **AST node kind-ове:** **19 = float lit** (текст = суровият литерал),
  **20 = bool lit** (текст `"1"`/`"0"`). `true`/`false` (токени 14/15) вече
  стават kind 20 (досега — kind 0 „int", затова bool се печаташе като число).
- **emit_expr:** `k==19` → суровият текст (валиден C double литерал);
  `k==20` → текстът (`1`/`0`, C int — bool е `int` по `c_type`).
- **`expr_is_float`** (евристика, по модела на `expr_is_str`):
  - float lit (19) → 1;
  - binary (3) → 1 ако ляв или десен operand е float (numeric promotion);
  - call (4) → 1 ако user fn връща `double` (`fn_ret_c_type == "double"`);
  - ident (2) → проследява до let-инита (като `expr_is_str`) и рекурсира.
- **`expr_is_bool`** (евристика):
  - bool lit (20) → 1;
  - binary (3) с op `== != < > <= >= && ||` → 1;
  - call (4) → 1 за `str_eq` и за user fn с return тип `int` (= bool по
    `c_type`; i64 е `int64_t`, та няма двусмислие);
  - ident (2) → проследява до let-инита и рекурсира.
- **print** (EXPR_STMT): str → `%s`; **float → `%g`**; **bool → `%s` с
  `(arg) ? "true" : "false"`**; иначе → `%lld`. (Точно форматите на baga:
  `baga_print_f64` е `%g`, bool е `%s` с ternary.)
- **LET ctype:** ако `expr_is_float(init)` → `double` (иначе `int64_t` би
  орязал `5.0` до `5`). За bool let-овете ctype остава `int64_t` — print
  класификацията (expr_is_bool по ident-trace) оправя изхода.

## Засегнати компоненти

| Файл | Промяна |
|---|---|
| `src/codegen_c.c` | float литерал `%g` → `%.17g` (precision fix) |
| `src/codegen_llvm.c` | float литерал: маха се `%g` round-trip, директно `LLVMConstReal` |
| `self/compiler.baga` | tokenizer float (102); parse_primary float(19)/bool(20); emit_expr 19/20; `expr_is_float`, `expr_is_bool`; print float/bool; LET ctype double |
| `docs/language-bg.md`, `docs/language-en.md` | бележка: float литералите се emit-ват с пълна точност |

## Извън обхвата

- Vec елементен тип (vec.baga) — следващ инкремент (M2).
- struct / match / effects / `Vec<T>` синтаксис — M3+.
- Пълна checker фаза в self компилатора — M4 (ако изобщо).

## Приемливост

- Оракулът остава 16/16 (precision fix-ът е консистентен C≡LLVM).
- `types.baga`: baga и baga2 дават **идентичен** изход
  (`78.5398 / true / 13.5 / true`), exit 0.
- `make self` → fixed point-ът държи (baga2 == baga3).
- `make test` зелен; другите примери без регресия.
