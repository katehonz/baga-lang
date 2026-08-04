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

**Verdict.** Measure in ops; optimize only if tests force it.

## Observed in P0

### D1 — confirmed

Three `i64` limbs + `4294967295` masks work; wide mul uses `Vec` of 32-bit
limbs (`src/limb.baga`). No language change needed.

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
