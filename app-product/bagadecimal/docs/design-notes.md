# bagaDecimal — design notes

## Mapping from rust-decimal

| rust-decimal | bagaDecimal |
|--------------|-------------|
| `Decimal { flags, lo, mid, hi }` | `Decimal { lo, mid, hi, flags }` |
| `Decimal::new(num, scale)` | `dec_from_i64_scale(num, scale)` |
| `Decimal::from_parts(lo, mid, hi, neg, scale)` | `dec_from_parts(...)` |
| `from_str` | `dec_parse` / `dec_parse_exact` |
| `to_string` | `dec_to_string` |
| `round_dp` | `dec_round_dp` |
| `checked_*` | `dec_checked_*` (P1) |
| `MathematicalOps` | `src/math/` (P2) |

## Scale rules (P0)

- Scale is an integer `0..28` inclusive (rust-decimal max scale 28).
- Addition/subtraction: align to the **larger** scale, then operate on
  mantissas; result scale = max(scale_a, scale_b) before optional normalize.
- Multiplication: mantissa product, scale = scale_a + scale_b; if scale > 28
  the product is rescaled to 28 (half-away on the most significant dropped
  digit). If the result still exceeds 96 bits, the scale is **rescued**
  rust-decimal-style: fractional digits are dropped (same rounding rule)
  until the mantissa fits; an error is returned only when the integer part
  alone overflows 96 bits.
- Division: `dec_div_scale(a, b, rs)` gives the result at scale `rs`,
  rounded half-away via a guard digit; `dec_div` picks
  `clamp(max(scale_a, scale_b), 6, 28)`.

## Rounding modes (P1, shipped 2026-08-04)

One `dec_round_impl(d, places, mode)` behind five public entries:
`dec_round_dp` (half-away, default), `dec_round_bankers_dp` (half-even),
`dec_trunc_dp` (toward zero), `dec_floor_dp` (toward −inf),
`dec_ceil_dp` (toward +inf). Dropped digits are tracked as
most-significant-dropped + sticky bit; zero results normalize to +0.

## Scientific notation (P1, shipped 2026-08-04)

`dec_parse` accepts `[eE][+-]?digits` after the mantissa (`1.23e4`,
`1E-2`). Exact when representable; a resulting scale above 28 is rounded
half-away (same rule as the mul rescale), an overflowing mantissa is an
error. Exponent digits saturate at ±1000 — larger exponents can only
overflow (error) or round to zero anyway.

## Overflow

P1 rule (already in force): `DecResult` everywhere and never silent wrap —
fail loud over wrong money. The limb library keeps a stricter invariant:
**no intermediate may reach 2^63**. Wide paths therefore run on 16-bit
half-limbs (products < 2^32, column sums < 2^34, division steps < 2^48);
the 2026-08-04 mul/div signed-overflow bugs (see gaps.md D1) are what this
invariant prevents.

## Why not arbitrary precision?

rust-decimal deliberately caps precision for speed and a fixed struct size.
Baga’s probe goal is the same: a **shippable money type**, not a CAS.
Arbitrary precision would be a different package (bigdecimal-class).

## Internal wide arithmetic

When 96-bit × 96-bit needs a 192-bit product:

1. Prefer limb schoolbook on six 32-bit limbs in `i64` registers, or
2. Temporarily lift to `bn_from_bytes` / `bn_mul` / re-encode.

Public types never expose `bn` — only `Decimal`.
