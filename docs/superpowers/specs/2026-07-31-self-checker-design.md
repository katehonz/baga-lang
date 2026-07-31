# Self компилатор: checker — аргументи + vec (M4a) — Дизайн

> Дата: 2026-07-31. Статус: одобрен (auto mode). Първи инкремент на M4
> (семантичен паритет).
> Цел: self компилаторът да **отхвърля** (exit 1) `arg_type_bad.baga` и
> `vec_typed.baga` — както baga. (`spec_bad` → M4b, изисква парсване на spec.)
> Проблем: self компилаторът няма checker — компилира невалидни програми
> (arg_type_bad/vec_typed → exit 0 / segfault вместо exit 1).

## Идея

Минимален checker пас в self компилатора (преди codegen), който:
1. събира **сигнатури** на потребителските функции (име → типове на параметрите);
2. на **call site** проверява брой + типове на аргументите срещу параметрите;
3. проверява **vec елементно смесване** (i64 и str в един vec).

При грешка: `eprintln(съобщение); exit(1)` — без да emit-ва C.

## Предпоставка: `exit` / `eprintln` builtins

Няма ги в езика. Добавят се:
- `exit(code: i64) -> void` — C: `exit((int)code)`; LLVM: наличният `exit_fn`
  (i64→i32 truncate).
- `eprintln(msg: str) -> void` — C: `fprintf(stderr, "%s\n", msg)`; LLVM:
  fprintf към stderr (вж. spec-fail емисията).
- self runtime: `baga_exit`, `baga_eprintln` + map в emit_expr.

## Семантика (checker в self)

- **type kind код** (за вътрешно ползване): `i64`=0, `str`=1, `f64`=2,
  `bool`=3, `vec`=4, `unknown`=-1.
- **`expr_typekind(...idx)`** (евристика): str лит (node 1)→1; int лит (0)→0;
  float лит (19)→2; bool лит (20)→3; call (4)→ return тип на fn/builtin; ident
  (2)→ enum вариант→0, иначе **unknown**. **Без let-trace** (идент-и по име без
  scope даваха фалшиви позитиви върху compiler.baga и чупеха fixed point-а) —
  checker-ът е консервативен: проверява само аргументи-литерали (достатъчно за
  примерите; `f("текст")` се хваща).
- **fn сигнатури**: scan prog за node 11; параметри от node 12 (`име:тип` →
  c_type-базиран kind); return от node 16.
- **arg check**: за call към user fn — брой аргументи == брой параметри; за всеки
  аргумент с известен kind, сравнява с параметъра (i64/i32 съвместими). Грешка:
  `'<име>': аргумент #N е от тип X, но параметърът е Y` (като baga).
- **vec mixing**: за всяко vec име — ако има И i64-push, И str-push → грешка
  `vec_push: елемент от тип str, но векторът е Vec<i64>` (като baga).

## Засегнати компоненти

| Файл | Промяна |
|---|---|
| `src/checker.c` | `exit`, `eprintln` в builtins |
| `src/codegen_c.c` | runtime `baga_exit`/`baga_eprintln` + builtin map |
| `src/codegen_llvm.c` | `exit`/`eprintln` lazy helpers + map |
| `self/compiler.baga` | runtime `baga_exit`/`baga_eprintln` + map; checker пас (fn сигнатури, expr_typekind, arg check, vec mixing); викане в main преди codegen |

## Извън обхвата

- spec валидация (spec_bad) + ensures/requires runtime — M4b.
- Пълно типово извеждане (struct, generics) — проверяват се само литерали/прости
  изрази (достатъчно за примерите).

## Приемливост

- `arg_type_bad.baga`, `vec_typed.baga`: baga2 exit 1 (като baga), грешка на
  stderr; baga≡baga2 (празен stdout, exit 1).
- `make self` → fixed point държи (checker-ът е self-consistent Baga код).
- `make test` зелен; 14-те OK без регресия.
