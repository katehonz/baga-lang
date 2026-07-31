# Self компилатор: match (M3b) — Дизайн

> Дата: 2026-07-31. Статус: одобрен (auto mode). Втори под-инкремент на M3.
> Цел: `match.baga` → baga≡baga2. (`match` се токенизира като ident → днес
> emit-ва `b_match;`.)

## Идея

`match expr { pat => body, ... _ => body }` като **израз**, по модела на C
backend-а — GCC statement expression:

```c
({ int64_t _mrN = 0; int64_t _mvN = expr;
   if (_mvN == 0) { _mrN = 100; }
   else if (_mvN == 1) { _mrN = 200; }
   else { _mrN = 999; }
   _mrN; })
```

`N` = node idx (уникален per match възел — безопасен при вложени match-ове).

## AST

- **26 = match**: child[0] = scrutinee; child[1..] = arm-ове.
- **27 = arm**: text = `"_"` за wildcard, иначе `""`; деца: `[pattern, body]`
  (нормален) или `[body]` (wildcard). `body` е израз (parse_expr).

## Семантика

- **parse_primary (ident клон):** ако text е `"match"` → parse match:
  scrutinee = parse_expr; `{`; цикъл arm-ове: pattern = parse_primary (int лит/
  ident; ident `"_"` → wildcard), `"=>"` (токени `=` + `>`), body = parse_expr;
  arm node 27; `}`. Връща node 26.
- **emit_expr (26):** result тип от първия arm body — `expr_is_str` → `const
  char *`, `expr_is_float` → `double`, `expr_is_bool` → `int`, иначе `int64_t`.
  If-chain: нормален arm → `if (_mvN == pat) { _mrN = body; }` (след първия —
  `else if`); wildcard → `else { _mrN = body; }`.
- **implicit return:** match като последен expr_stmt в non-void fn вече се
  emit-ва като `return ({...});` (съществуващата implicit-return логика).

## Засегнати компоненти

| Файл | Промяна |
|---|---|
| `self/compiler.baga` | parse_primary: match (26/27); emit_expr 26 (GCC stmt expr, if-chain, result тип) |

## Извън обхвата

- enum (M3c — cvet ползва enum + match; match-ът вече ще работи, трябва enum).
- Block-тяло на arm (`pat => { ... }`) — примерите ползват expr arm-ове.
- `Vec<T>`/spec (M3d), effects (M3e).

## Приемливост

- `match.baga`: baga ≡ baga2 (`100 200 300 999 999` за i=0..4), exit 0.
- `make self` → fixed point държи. `make test` зелен. OK (10) без регресия.
