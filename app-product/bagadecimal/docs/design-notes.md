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
  or mantissa exceeds 96 bits → overflow / rescale policy (document per op).
- Division: choose result scale (default: max(scale_a, scale_b) or a
  fixed “money” scale parameter) — exact policy fixed when implementing
  `dec_div`.

## Overflow

P0 may use “best effort + document”; P1 introduces `DecResult` and never
silent wrap. Prefer failing loud over wrong money.

## Why not arbitrary precision?

rust-decimal deliberately caps precision for speed and a fixed struct size.
Baga’s probe goal is the same: a **shippable money type**, not a CAS.
Arbitrary precision would be a different package (bigdecimal-class).

## Internal wide arithmetic

When 96-bit × 96-bit needs a 192-bit product:

1. Prefer limb schoolbook on six 32-bit limbs in `i64` registers, or
2. Temporarily lift to `bn_from_bytes` / `bn_mul` / re-encode.

Public types never expose `bn` — only `Decimal`.
