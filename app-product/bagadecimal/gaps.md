# bagadecimal — language & product gaps

Probe log for bagaDecimal (rust-decimal-style decimal package).

## Anticipated (from PLAN)

### D1 — no `u32` / `u128`

**Symptom.** rust-decimal uses three `u32` limbs + flags. Baga only has
signed `i64` (and `f64`).

**Workaround.** Store limbs in `i64`, mask to 32 bits; wide mul via
careful i64 products or `std/crypto/bn`.

**Severity.** Medium.

**Verdict.** Convention; document limb invariants in `docs/design-notes.md`.

### D2 — no operator overloading

**Symptom.** Cannot write `a + b` for `Decimal`.

**Workaround.** `dec_add` / `dec_mul` / …

**Severity.** Low (API noise only).

**Verdict.** Accept for P0; consistent with rest of baga.

### D3 — no macros

**Symptom.** No `dec!(1.23)` compile-time literal.

**Workaround.** `dec_parse("1.23")` at runtime; optional later codegen.

**Severity.** Medium for DX.

**Verdict.** Language gap; not blocking correctness.

### D4 — no `Result` / sum types (L3)

**Symptom.** Overflow and parse errors need a side channel.

**Workaround.** `DecResult { ok, err, value }` (same stand-in as pg/jsonrpc).

**Severity.** Medium.

**Verdict.** Migrate when L3 lands.

### D5 — only `f64` among floats

**Symptom.** Lossy `Decimal ↔ f64` is the only IEEE bridge.

**Workaround.** Document loss; prefer string/i64 bridges for money.

**Severity.** Low.

**Verdict.** By design.

### D6 — 96-bit mul/div cost

**Symptom.** Schoolbook 96×96 may be slow or awkward in pure i64.

**Workaround.** Internal use of `bn.baga` if needed; keep public API fixed.

**Severity.** Medium for P1 maths.

**Verdict.** **Measured 2026-08-04, closed.** 200 `dec_div` calls with a
multi-limb divisor (the bit-by-bit path, worst case) run in ~1.4 s —
≈7 ms/division. Fine for money workloads (tens of divisions per
document). Knuth-style divmod only if a real bulk workload shows up.

## Observed in P0

### D1 — confirmed, and it bit hard (signed-overflow class)

Three `i64` limbs + `4294967295` masks work, **but** the limb library
originally assumed 64-bit unsigned headroom that signed `i64` does not
have. Two verified bugs, both fixed 2026-08-04:

- **mul crash** — `dec_limb_mul` accumulated `out + ai*b_j + carry` up to
  ~2^64; the sum wrapped negative, the arithmetic-shift carry went
  negative, and the drain loop walked out of bounds.
  `dec_mul(3100000000, 3100000000)` aborted at runtime.
- **div_small silent corruption** — `rem * 2^32 + limb` overflowed for
  single-limb divisors > 2^31; `dec_div_scale(1, 3000000000, 28)` printed
  confidently wrong digits.

Fix: 16-bit half-limb arithmetic in the wide paths (`dec_limb_mul`,
`dec_limb_mul_small`, `dec_limb_div_small`) — every intermediate stays
below 2^48. Regression tests in `tests/decimal_test.baga`
(`mul_big_limbs`, `mul_max32`, `mul_rescale_big`, `div_big_single_limb`,
`div_max32`). The limb invariants in `docs/design-notes.md` must state
the 2^48 ceiling explicitly.

## Observed in P0.6

### D8 — mul scale rescue (rust-decimal parity)

`dec_mul` used to error whenever the exact product exceeded 96 bits at
scale ≤ 28. It now rescues rust-decimal-style: fractional digits are
dropped (rounding on the most significant dropped digit) until the
mantissa fits; an error is returned only when the integer part alone
overflows. Regression: `mul_rescale_big` (scale 48 → 19),
`mul_true_overflow` (still loud).

### D9 — percent_of intermediate rounding (accounting-visible)

`dec_percent_of` pre-rounded the rate to scale 8 before multiplying —
33.3333333% of 999999999.99 posted 333333330.00 instead of 333333333.00.
Now: exact `dec_mul`, one `dec_div_scale` by 100, one rounding at the
posting. `dec_with_percent` taxes the same rounded base it posts.
Regression: `percent_precise`.

### D2 — confirmed

All ops are `dec_add` / `dec_mul` / … — acceptable.

### D4 — `DecResult` / `DecI64` stand-ins

Parse, arith, and round return `DecResult`. Integer extract uses `DecI64`.
Same L3 family as the rest of the stack.

### D7 — no `Vec<Decimal>` (L4)

**Symptom.** Cannot `vec_push` of `Decimal` for invoice lines.

**Workaround.** `dec_sum2` / `dec_sum3` or loop variables; parallel
`Vec<str>` of `dec_to_pg` strings when batching.

**Severity.** Medium for multi-line documents.

**Verdict.** Language L4; app-level loops are fine for P0.5.

## Closed

- P0 money path (parse, +−×÷, round_dp, format, money example).
- P0.5 Postgres NUMERIC text bridge + accounting helpers (`decimal_pg_test`).
