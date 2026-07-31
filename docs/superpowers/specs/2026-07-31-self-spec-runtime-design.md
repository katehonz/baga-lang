# Self компилатор: spec валидация + ensures/requires runtime (M4b) — Дизайн

> Дата: 2026-07-31. Статус: одобрен (auto mode). Втори инкремент на M4
> (последният от catch-up). Цел: `spec_bad`, `spec_ensures_fail`,
> `spec_requires_fail` → baga≡baga2 (exit 1 при нарушение). Завършва 19/19.
> Проблем: self компилаторът skip-ва spec блокове (M3d) — няма валидация нито
> runtime проверки. baga: spec_bad → compile грешка; ensures/requires fail →
> runtime `baga_spec_fail` → exit 1.

## Идея

1. **`||`/`&&` в изрази** — self парсерът ги токенизира, но не ги парси.
   Добавят се `parse_or`/`parse_and` слоеве над `parse_cmp` (node 3 с op
   `||`/`&&`).
2. **Парсване на spec блокове** (вместо skip) → AST node **30 (spec)**:
   - text = име на spec;
   - дете **16** (output тип, text = тип) — за валидация;
   - деца **31 (requires)** / **32 (ensures)**: text = изходния текст (за
     грешка), дете = парснатият израз.
3. **`baga_spec_fail` runtime** в self (същият като C: fprintf stderr + exit 1).
4. **Spec валидация** (spec_bad): output тип на spec-а ≠ return тип на fn →
   `eprintln(...); exit(1)`.
5. **Runtime codegen** (wrapper подход като C): fn със spec се emit-ва като
   `b_NAME_impl` (тялото) + wrapper `b_NAME`, който:
   - проверява requires преди повикването (`if (!(R)) baga_spec_fail(...)`);
   - вика impl-а, записва резултата в `b_output`;
   - проверява ensures (`output` → `b_output`, параметрите → mangled имена);
   - връща `b_output`.
   `output`/параметрите в ensures/requires изразите се emit-ват чрез обичайния
   `emit_expr` (mangle-ва ident-и → `b_output`/`b_x` съвпадат с wrapper-а).

## Семантика

- **parse_or/parse_and**: `parse_expr = parse_or`; `parse_or` → `parse_and`
  (`||`); `parse_and` → `parse_cmp` (`&&`). node 3 с op текста.
- **parse_spec**: `spec` име `{` → секции `input:`/`output:`/`requires:`/
  `ensures:`/`guarantees:` (разпознават се като ident + `:`). requires/ensures:
  изрази, разделени с `,`, до следваща секция или `}`. output: един тип (node 16).
- **find_spec(prog, fn_name)**: node 30 с text == fn_name (с ensures/requires).
- **emit за fn със spec**: forward decl на wrapper-а; def: impl (име +
  `_impl`) с тялото; wrapper с requires/ensures проверки.
- **baga_spec_fail** (self runtime): `spec '%s': requires/ensures #N нарушено/а:
  <expr>` на stderr + `exit(1)` (байт-също като baga).

## Засегнати компоненти

| Файл | Промяна |
|---|---|
| `self/compiler.baga` | parse_or/parse_and (`||`/`&&`); parse_spec (node 30/31/32); top-level "spec" → parse_spec; `baga_spec_fail` runtime; find_spec; spec валидация (output тип); emit impl+wrapper с requires/ensures; check_program валидира spec output |

## Открита предпоставка: унарен минус

`spec_requires_fail` ползва `-1`/`-5`; self парсерът **не** боравеше унарен
минус (`-1` → `0` + `1`). Добавен е в `parse_primary`: водещ `-` → binary
`(0 - operand)` (emit `(0LL - x)`). Покрива се от fixed point-а.

## Извън обхвата

- `--test-specs` (property-based) в self — само runtime проверки при изпълнение.
- guarantees (текстови) — не се проверяват (както в C).

## Приемливост

- `spec_bad.baga`: baga2 exit 1 (spec output f64 ≠ fn i64).
- `spec_ensures_fail.baga`: baga2 exit 1 (`ensures #1 нарушена`).
- `spec_requires_fail.baga`: baga2 exit 1 (`requires #1 нарушено`).
- `spec_ensures.baga`, `spec.baga` остават OK (валидни spec-ове, проверките
  минават). `make self` → fixed point. `make test` зелен. **19/19**.
