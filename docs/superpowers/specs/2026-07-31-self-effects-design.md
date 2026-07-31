# Self компилатор: effects/catch (M3e) — Дизайн

> Дата: 2026-07-31. Статус: одобрен (auto mode). Пети (последен) под-инкремент
> на M3. Цел: `effects.baga` → baga≡baga2. Завършва M3 (синтактичен паритет).
> Проблем (емпирично): effect анотации в return тип (`-> str !IO !NotFound`)
> чупят parse_fn (gcc: `b_IO = 0LL`); `?` propagate и верижните `catch` не се
> боравят.

## Идея

Ефектите са **compile-time only** (както в C backend-а — NODE_CATCH/NODE_TRY
emit-ват само израза). Self компилаторът ги **прескача**:

- **return тип:** след `parse_type_name` за return-а, skip на `!Име` токени
  (двойки `!` + ident).
- **`?` propagate:** postfix `?` след израз (в parse_primary ident-клона) се
  прескача.
- **`catch !E => handler`:** съществуващият skip става **while** (вложени/
  верижни catch-ове), и в LET клона, и в expr-stmt fallback-а.

## Семантика

- parse_fn: `while is_single("!") { advance; advance }` след return type node.
- parse_primary (ident клон): `while is_single("?") { advance }` след
  field-access loop-а.
- catch skip: `while vec_get(tk,pos)==16 { catch; !; E; =; >; parse_expr }`
  (token 16 = `catch`; `=>` = `=` + `>`).

## Засегнати компоненти

| Файл | Промяна |
|---|---|
| `self/compiler.baga` | parse_fn return effect skip; parse_primary postfix `?`; catch skip → while (LET + expr-stmt) |

## Извън обхвата

- Истинска ефектова проверка / spec runtime в self — M4.
- `?` върху не-ident изрази (примерите ползват `call(?)`).

## Приемливост

- `effects.baga`: baga ≡ baga2 (`съдържание`), exit 0.
- `make self` → fixed point държи. `make test` зелен.
- **M3 завършен:** всички конструкт-примери са OK (14/19); останалите 5 са
  checker/spec-runtime (M4): arg_type_bad, spec_bad, vec_typed,
  spec_ensures_fail, spec_requires_fail.
