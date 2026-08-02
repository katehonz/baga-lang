# Evaluation — Baga `--verify` vs bit-precise model checking

**Status:** Baga column machine-generated (`bench/run_eval.sh`);
CBMC column reproduces with `sudo apt install cbmc && bench/run_eval.sh`.

## Methodology

15 tasks spanning the verifier's fragment: overflow/arithmetic, division,
loops with invariants, recursion with termination, nonlinear products,
concurrency (fork–join, handle protocols), and channel content invariants.
The Baga side runs the repository's own fixtures (`examples/verify/*.baga`)
— no benchmark-specific tuning; the same files gate `make test`. The CBMC
side has hand-written C twins of the arithmetic/loop subset
(`bench/cbmc/*.c`) using `__CPROVER_assume`/`__CPROVER_assert` and
`--signed-overflow-check` / `--div-by-zero-check`, which are the direct
analogues of Baga's M15 arithmetic-safety obligations.

Three verdict columns are reported separately, because Baga's soundness
model distinguishes them: **ensures** (spec contracts, idealized-ℤ),
**arith** (M15/M18 overflow safety — the ℤ-vs-i64 bridge), **protocol**
(M14 handle lifecycle). A task is only *fully* proven when all applicable
columns are proven — the M18 `!Overflow` effect exists precisely to make
the arith column a type-level decision rather than a silent assumption.

## Results

See [`bench/RESULTS.md`](../bench/RESULTS.md) (regenerated on every run).
Summary of the current run:

- **All 15 tasks produce the expected verdicts** — including the hard
  honesty cases: `loop_havoc` (no false proof through stale loop values),
  `pair_select`'s abstract-status UNKNOWN, `par_detach_bad`'s protocol
  refutation.
- **The M18 signature case**: `abs_val` — ensures proven *and*
  `abs(INT64_MIN)` overflow refuted with the exact witness, in the same run.
- **Timings: 1–8 ms per task** (one outlier: `sum` at ~180 ms, the
  FM bound search over loop invariants). The whole suite verifies faster
  than CBMC takes to start up.

## Where Baga wins

1. **Speed & transparency.** Millisecond verdicts with *readable*
   counterexamples (`n = 9223372036854775807`), not solver dumps. The
   witness discipline (M8 conclusiveness) means a printed counterexample
   is always real — never an artifact of abstraction.
2. **Zero dependencies.** The entire analysis is one C file over a
   Fourier–Motzkin core. No SMT solver, no license questions, no timeouts.
3. **Concurrency fragment.** Fork–join determinism, handle protocols,
   channel content invariants — CBMC has no comparable high-level view of
   Baga's `go`/channel programs (the CBMC column is "—" there; those tasks
   would need hand-modeled pthread harnesses).

## Where Baga loses (honestly)

1. **Coverage.** CBMC is bit-precise over all of C: floats, pointers,
   heap, structs, full bitvectors. Baga's fragment is pure i64 functions
   with linear-ish arithmetic, Vec bounds, and the concurrency subset.
   Anything outside reports UNKNOWN — by design, never a false proof.
2. **Bit-precise nonlinear arithmetic.** Baga's nonlinear reasoning is
   axiom-scheme-based (sound, incomplete); CBMC bit-blasts and decides
   (slowly, but decides) nonlinear goals in its fragment.
3. **The (2^62, 2^63) band** reports UNKNOWN where a 128-bit rational core
   or bit-blasting would decide (documented M15 limitation).
4. **Loops need invariants.** CBMC unrolls; Baga inducts. Unbounded-growth
   arithmetic in loops (`sum`, `fact_full`'s `n*r`) is honestly UNKNOWN
   without user-supplied bounds.

## Reproduce

```bash
make                        # build baga
bench/run_eval.sh           # fills bench/RESULTS.md (Baga column)
sudo apt install cbmc       # optional, then re-run for the CBMC column
```

## Verdict

Within its deliberately sound fragment, Baga matches what a bit-precise
model checker reports — in milliseconds, with readable witnesses, and with
a concurrency fragment CBMC does not reach. Outside the fragment it says
UNKNOWN instead of guessing. That trade — *coverage for trust and speed* —
is the technical claim.
