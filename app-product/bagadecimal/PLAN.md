# bagaDecimal — plan

> Decimal arithmetic package for Baga, modelled on
> [paupino/rust-decimal](https://github.com/paupino/rust-decimal).
> Financial / fixed-precision math without `f64` round-off.

**Package name (sandak):** `bagadecimal`  
**Product name:** bagaDecimal  
**Status:** **P0 + accounting/PG path** (2026-08-04) · next: more rounding modes,
checked overflow, JSON string money  
**Roadmap role:** apps probe **№11** — money type + Postgres `NUMERIC` for
accounting apps (like rust-decimal + db-postgres).

---

## Why this package

| Need | Why not `f64` / `i64` |
|------|----------------------|
| Money, tax, rates | `0.1 + 0.2 ≠ 0.3` in binary float |
| Fixed scale (2 dp, 4 dp, …) | `i64` cents works until mul/div of rates |
| Exact parse/format of decimal strings | No silent binary rounding |

**rust-decimal** solves this with a **96-bit mantissa + scale + sign** (same
family as .NET `System.Decimal` / OLE Automation Currency). We follow that
shape so:

1. Algorithms and test vectors port cleanly from rust-decimal / .NET docs.
2. Max precision (~28–29 significant decimal digits) is enough for money and
   most financial intermediate products.
3. We can reuse `std/crypto/bn.baga` for heavy ops when 96-bit schoolbook is
   awkward, without exposing arbitrary-precision API in v1.

---

## Representation (locked for P0)

```
struct Decimal {
    lo: i64,      // bits 0..31   (stored in low 32 of i64; upper cleared)
    mid: i64,     // bits 32..63
    hi: i64,      // bits 64..95
    flags: i64    // bit 31 = negative; bits 16..23 = scale 0..28
}
```

- **Mantissa** = unsigned 96-bit integer `hi:mid:lo` (little-endian 32-bit
  limbs in spirit of rust-decimal `from_parts`).
- **Scale** = number of digits after the decimal point (`value = ±m / 10^scale`).
- **Sign** in flags (not two’s-complement mantissa).
- Trailing zeros in the fractional part are **preserved** until `dec_normalize`.

Baga has no `u32`/`u128`; we keep limbs in `i64` and mask with `0xffffffff`.
All intermediate products that need more than 96 bits go through a short
working buffer (`Vec<i64>` 32-bit limbs or `bn_*`) and then re-pack with scale
adjustment / overflow check.

---

## Scope map vs rust-decimal

| rust-decimal area | bagaDecimal module | P0 | P1 | P2 |
|-------------------|--------------------|----|----|-----|
| `Decimal` core + parts | `src/decimal.baga` | ✅ | | |
| `+ − * / %`, cmp, abs, neg | `src/ops/` | ✅ | | |
| `from_str` / parse | `src/parse/` | ✅ | | |
| `to_string` / format | `src/format/` | ✅ | | |
| `round_dp`, strategies | `src/round/` | MidpointAwayFromZero, TowardZero | banker's, floor/ceil | |
| `from_i64` / `to_i64` / f64 lossy | `src/convert/` | i64 exact | f64 lossy | |
| `pow`, `sqrt`, `ln`, `exp` | `src/math/` | | √ subset | fuller maths |
| serde / JSON string | later / std json helpers | | string codec | |
| Postgres `NUMERIC` | `src/pg` text bind/cell | ✅ text | | binary OID |
| money helpers | `src/money` | ✅ | | |
| `dec!` macro | **gap** (no macros) | parse at runtime | | |

Out of scope v1: SIMD, const eval, arbitrary precision (`bigdecimal`-class),
serde features, diesel/postgres drivers (use package API from orm later).

---

## Implementation phases

### P0 — core money path (ship bar) ✅

1. `Decimal` + constructors: zero, one, `from_parts`, `from_i64_scale`,
   `from_str` (plain decimal, optional leading `+`/`-`).
2. Format: `to_string` (preserve scale / trailing zeros like rust-decimal).
3. Ops: `add`, `sub`, `mul`, `div` / `div_scale`, `cmp`,
   `is_zero`, `is_negative`, `abs`, `neg`.
4. Round: `round_dp(d, n)` MidpointAwayFromZero (common for money).
5. Tests: `tests/decimal_test.baga` (30+ checks) + package smoke.
6. `examples/money.baga`: 25.12 × 8.5% → **27.26**.

**Exit:** `sandak build` + `tests/decimal_test.baga` green ✅

### P0.5 — accounting + Postgres NUMERIC ✅

- `src/money/`: `dec_money`, `dec_as_money`, `dec_normalize`, `dec_percent_of`,
  `dec_with_percent`, `dec_sum2/3` (no `Vec<Decimal>` — L4).
- `src/pg/`: `dec_to_pg` / `dec_from_pg`, `pg_param_decimal`, `pg_cell_decimal`,
  `pg_col_is_numeric` (OID 1700) — text protocol, same idea as rust-decimal’s
  postgres feature without binary format.
- Live: `tests/decimal_pg_test.baga` (CREATE / INSERT $N::numeric / SELECT / SUM).

### P1 — robustness

- `trunc`, `floor`, `ceil`, more rounding modes (banker's).
- `from_str` scientific notation (`1.2e-3`).
- Checked ops naming consistency; more edge vectors.
- JSON: always serialize money as **string**, never f64.

### P2 — maths + binary NUMERIC (optional)

- `src/math/`: `powi`, `sqrt`.
- Binary NUMERIC wire (if pgbaga gains binary binds) — not required for
  accounting if text `$1::numeric` stays the default.

---

## Language probes (expected gaps)

| ID | Symptom | Severity | Path |
|----|---------|----------|------|
| D1 | No `u32`/`u128` — 96-bit via three `i64` + masks | Medium | Convention + tests |
| D2 | No operator overloading — `dec_add(a,b)` not `a+b` | Low | API naming |
| D3 | No macros — no `dec!(1.23)` | Medium | `dec_parse("1.23")` |
| D4 | No `Result`/enums — ok/err structs | Medium | L3 when ready |
| D5 | `f64` only other float — lossy convert documented | Low | convert module |
| D6 | Mul/div of 96-bit may want bn | Medium | Use `std/crypto/bn` internally if needed |

Record hits in `gaps.md` as implementation proceeds.

---

## Architecture (directories)

```
app-product/bagadecimal/
├── sandak.toml              # name = bagadecimal, entry = src/decimal.baga
├── README.md
├── PLAN.md                  # this file
├── gaps.md
├── src/                     # library surface (not a flat dump)
│   ├── decimal.baga         # Decimal struct, constructors, re-exports / facade
│   ├── ops/
│   │   ├── arith.baga       # add sub mul div rem
│   │   └── cmp.baga         # cmp eq lt abs neg is_*
│   ├── parse/
│   │   └── parse.baga       # from_str, from_str_exact
│   ├── format/
│   │   └── format.baga      # to_string, to_string_normalized
│   ├── round/
│   │   └── round.baga       # round_dp + modes
│   ├── convert/
│   │   └── convert.baga     # i64 / (lossy) f64 bridges
│   └── math/                # P2
│       └── math.baga        # powi sqrt … (stubs until P2)
├── examples/
│   └── money.baga           # CLI demo (bin-style entry optional)
├── tests/                   # package-local; monorepo also has tests/decimal_*
│   └── smoke.baga
└── docs/
    └── design-notes.md      # scale rules, overflow, rust-decimal mapping
```

**Import convention (monorepo):**

```baga
import "bagadecimal/src/decimal.baga"
// or, once facade is stable:
import "bagadecimal/src/ops/arith.baga"
```

`sandak.toml` `entry` points at the facade `src/decimal.baga` so
`sandak build` typechecks the whole graph via its imports.

**Why `src/`:** rust-decimal and most serious crates keep implementation under
`src/`; existing *baga packages are small and flat. bagaDecimal is large enough
that flat root would become unreadable (ops + parse + format + math + round).

---

## Test strategy

| Layer | Where | What |
|-------|--------|------|
| Unit | `tests/decimal_test.baga` (repo `tests/`) | ops, parse, format, round goldens |
| Smoke | `app-product/bagadecimal/tests/smoke.baga` | quick import + one mul |
| Demo | `examples/money.baga` | human-facing tax calculation |
| Oracle | offline python `decimal` / rust-decimal | generate hex/`from_parts` vectors |

Wire `decimal_test` into `scripts/baga-test` discovery automatically once named
`*_test.baga` under repo `tests/`.

---

## Dependencies

```toml
[package]
name = "bagadecimal"
version = "0.1.0"
entry = "src/decimal.baga"

[dependencies]
std = { path = "../../std" }
# optional later: use bn for wide mul
# (no hard dep until ops need it)
```

Pure library — no http/pg. That keeps the probe focused on arithmetic and
language gaps, not the web stack.

---

## Success criteria (P0)

1. `cd app-product/bagadecimal && sandak build` OK.
2. `dec_parse("10.50")`, mul by rate, `round_dp(2)`, format → exact money string.
3. At least 30 automated checks (parse/ops/round/overflow).
4. `gaps.md` lists D1–D6 with verdicts.
5. PLAN phases P0 marked done in this file.

---

## Non-goals (explicit)

- Replacing `f64` for graphics / ML.
- Exact IEEE decimal64/128 interchange (we track rust-decimal / .NET shape).
- Compile-time decimal literals (needs language macros).
- Dropping a second “money” type — one `Decimal` is enough.
