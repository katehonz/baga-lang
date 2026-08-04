# bagaDecimal

**Fixed-precision decimal** for Baga — счетоводство, цени, ДДС, курсове — без
`f64` грешки. Модел: [paupino/rust-decimal](https://github.com/paupino/rust-decimal)
(96-bit mantissa + scale + sign).

| | |
|--|--|
| **sandak** | `bagadecimal` **0.3.0** |
| **Layout** | `src/` modules |
| **Postgres** | `NUMERIC` text bridge (`src/pg`) — като rust-decimal `db-postgres` |
| **Plan** | [PLAN.md](PLAN.md) · [gaps.md](gaps.md) |

## Счетоводен пример

```baga
import "bagadecimal/src/decimal.baga"

let price = dec_money("25.12")           // scale 2
let line  = dec_with_percent(price.value, dec_parse("8.5").value)
// line.value == 27.26  (ДДС 8.5%)
print(dec_to_string(line.value))
```

## PostgreSQL `NUMERIC` (text protocol / pgbaga)

Същият подход като rust-decimal върху text format: сумата е decimal string
на wire-а, без binary float.

```baga
import "bagadecimal/src/decimal.baga"   // includes pg helpers
// or: import "bagadecimal/src/pg/pg.baga"

// INSERT
let vals = vec_new()
let nulls = vec_new()
pg_param_str(vals, nulls, "фактура-1")
pg_param_decimal(vals, nulls, dec_money("1500.00").value)
let r = pg_query_params(c,
    "INSERT INTO lines(label, amount) VALUES ($1, $2::numeric)",
    vals, nulls)?

// SELECT
let s = pg_query(c, "SELECT amount FROM lines")?
let amt = pg_cell_decimal(s, 0, 0)       // DecResult
// pg_col_is_numeric(s, 0) == 1          // OID 1700
// pg_cell_decimal_or_zero(s, 0, 0)      // NULL → 0
```

Live proof: `tests/decimal_pg_test.baga` (същият Postgres като `pg_test`).

| Helper | Role |
|--------|------|
| `dec_to_pg` / `dec_from_pg` | pure text codec |
| `pg_param_decimal` | bind `$N::numeric` |
| `pg_cell_decimal` | read cell → `Decimal` |
| `pg_col_is_numeric` | OID 1700 |
| `pg_cell_decimal_or_zero` | NULL-safe (display only — виж предупреждението) |

**Не ползвай** `pg_cell_f64` за пари. **Не поствай** суми, сметнати с
`pg_cell_decimal_or_zero` — грешка в клетката е неразличима от реално
0.00; за постнати суми ползвай `pg_cell_decimal` (стриктен `DecResult`).

## Layout

```
src/
  types.baga limb.baga decimal.baga   # core + entry
  ops/   parse/  format/  round/  convert/
  money/   # dec_money, percent, normalize, sum2/3
  pg/      # NUMERIC bridge (depends on pgbaga)
  math/    # P2 stubs
examples/money.baga
tests/smoke.baga
```

```bash
cd app-product/bagadecimal && sandak build
./baga -I . -I app-product tests/decimal_test.baga
./baga -I . -I app-product tests/decimal_pg_test.baga   # needs Postgres
```

## API snapshot (P0 + accounting + P1 rounding/parse)

| Area | Functions |
|------|-----------|
| Construct | `dec_zero`, `dec_one`, `dec_from_parts`, `dec_from_i64(_scale)`, `dec_parse` (с експонента: `1.23e4`) |
| Format | `dec_to_string`, `dec_to_string_normalized` |
| Ops | `dec_add/sub/mul/div/div_scale`, `dec_cmp/eq/lt`, `dec_abs/neg` |
| Round | `dec_round_dp` (half-away), `dec_round_bankers_dp` (half-even), `dec_trunc_dp`, `dec_floor_dp`, `dec_ceil_dp` |
| Money | `dec_money`, `dec_as_money`, `dec_is_money`, `dec_normalize`, `dec_percent_of`, `dec_with_percent`, `dec_sum2/3`, `dec_sum_vec(Vec<Decimal>)` |
| PG | see table above |

Fallible paths return `DecResult { ok, err, value }`. `dec_mul` сваля
scale със закръгляне, докато мантисата се побере в 96 бита (rescue) —
грешка само когато цялата част прелива.

## Gaps (език)

- ~~няма `Vec<Decimal>` (L4)~~ — от 2026-08-04 `Vec<struct>` е в езика → `dec_sum_vec`
- няма `dec!` macro → `dec_parse` / `dec_money`
- няма operator overloading → `dec_add(a,b)`
