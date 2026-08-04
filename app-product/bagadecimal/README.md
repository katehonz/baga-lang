# bagaDecimal

**Fixed-precision decimal arithmetic** for Baga — money, rates, and exact
base-10 math without `f64` round-off.

Inspired by [paupino/rust-decimal](https://github.com/paupino/rust-decimal)
(96-bit mantissa + scale + sign, ~28 significant digits).

| | |
|--|--|
| **sandak name** | `bagadecimal` |
| **Layout** | `src/` modules (not a flat package root) |
| **Status** | **P0 shipped** — parse/ops/round/format |
| **Plan** | [PLAN.md](PLAN.md) · gaps: [gaps.md](gaps.md) |

## Quick start

```baga
import "bagadecimal/src/decimal.baga"

let price = dec_parse("25.12")
let rate  = dec_parse("0.085")
let tax   = dec_round_dp(dec_mul(price.value, rate.value).value, 2)
let total = dec_round_dp(dec_add(price.value, tax.value).value, 2)
print(dec_to_string(total.value))   // "27.26"
```

Fallible ops return `DecResult { ok, err, value }`. There is no `dec!(…)`
macro — use `dec_parse` (PLAN D3).

## Layout

```
bagadecimal/
├── sandak.toml
├── README.md  PLAN.md  gaps.md
├── src/
│   ├── decimal.baga     # Decimal + facade imports
│   ├── ops/             # arith, cmp
│   ├── parse/           # from string
│   ├── format/          # to string
│   ├── round/           # round_dp + modes
│   ├── convert/         # i64 / f64 bridges
│   └── math/            # P2: powi, sqrt, …
├── examples/money.baga
├── tests/smoke.baga
└── docs/design-notes.md
```

Build:

```bash
cd app-product/bagadecimal
sandak build          # typechecks entry = src/decimal.baga
```

## Relation to the stack

Pure library — no HTTP/DB. Intended consumers: business logic in `apps/*`,
JSON money as **strings**, later optional Postgres `NUMERIC` helpers.

## License

Same as the monorepo.
