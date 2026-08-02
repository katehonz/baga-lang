# Closing the ℤ-vs-i64 Gap: Arithmetic Safety for an Idealized Verifier

**Working title (PhD-scale research note, slice 2)**
*Per-operation overflow obligations, exact bound search over Fourier–Motzkin,
and two soundness bugs found along the way*

**Artifact:** the Baga compiler (`--verify`), milestone M15
**Prior art in the same pipeline:** M0–M13 (`thesis-m13-nonlinear-fragment.md`),
M14 (`thesis-m14-par-fragment.md`)
**Status:** implemented and regression-tested (`examples/verify/ovf_*.baga`,
`div_zero.baga`, `loop_havoc.baga`)

---

## Abstract

A verifier that reasons over mathematical integers ℤ while the machine
computes in two's-complement i64 has a hole in its soundness story: every
PROVEN verdict is conditional on the absence of overflow. This note describes
how Baga closes that hole *without leaving its zero-dependency
Fourier–Motzkin core*: each arithmetic operation in verified code emits a
separate **arithmetic-safety obligation** that is proven (no overflow
possible on this path), refuted with a concrete large-magnitude witness, or
honestly reported UNKNOWN. When all obligations of a function are proven, the
idealized model and the runtime provably coincide — the ensures verdicts
become unconditional. Building the analysis exposed **two real soundness
bugs** in the existing pipeline — a missing havoc in the Hoare while-rule and
an INT64_MIN-unsafe rational arithmetic core — both fixed, with regressions.

---

## 1. The gap

M0–M14 establish: PROVEN means "the negated obligation is unsatisfiable over
ℚ (hence ℤ after tightening)". But the generated C computes in `int64_t`.
The function

```baga
fn inc(n: i64) -> i64 { return n + 1 }   // ensures output > n
```

is PROVEN in the idealized model — and wrong at `n = INT64_MAX` (formally UB
in the emitted C; a wrap in practice). The contract language of M0–M14 is
thus a logic of an idealized machine. M15 makes the gap *visible and
checkable* instead of implicit.

## 2. Method: per-operation safety obligations

For every `+`, `-`, `*`, unary `-`, `/`, `%`, `<<` occurring in verified code
(statements, assignments, returns, branch and loop guards), the verifier
emits a kind-4 obligation carrying the operation's linear forms and the path
condition. Three shapes:

| Shape | Check | Verdict machinery |
|---|---|---|
| FIT (`a+b`, `a-b`, `-x`, `n<<k`) | `\|L\| ≤ 2^62` provable? | exact bound search (§3) |
| MUL (`a*b`, product symbol) | tightest provable `\|fa\|·\|fb\| ≤ 2^63−1` | bound search per factor, `__int128` product |
| DIVZ (`n/d`, `n%d`) | `d = 0` infeasible ∧ `d = −1 ∧ n = INT64_MIN` infeasible | two FM feasibility queries |

A PROVEN obligation means: on every execution reaching this point along this
path, the operation cannot overflow. A REFUTED obligation carries an input
witness — found by a dedicated search whose candidate grid includes
large magnitudes (±2^31, 2^32, ⌊√2^63⌋+1, ±2^62, INT64_MIN/MAX), because
overflow witnesses live near the representable edge. Witnesses obey the M8
conclusiveness discipline: the violation must depend only on pinned inputs
and derived product values, never on unrealizable abstract symbols —
otherwise the verdict degrades to UNKNOWN.

**The bridge theorem (informal).** If every arithmetic obligation of a
function is PROVEN, then every concrete execution stays inside i64 exactly
where the symbolic model computed in ℤ, so the two semantics agree on the
return value — and the M0–M14 ensures verdicts are unconditional. The
verifier states this explicitly: it prints the count of proven-safe
operations, and marks the function as idealized-model-only otherwise.

**Deliberate incompleteness.** The provable-fit window is `|L| ≤ 2^62`, not
`2^63 − 1`: exact rational arithmetic near `2^63` overflows its own int64
constants, and the core now *bails out conservatively* there (§4). The
top-of-range band reports UNKNOWN — never a false proof.

## 3. Exact bound search over FM

`fm_maxabs(path, L)`: the tightest K in [0, 2^62] with `path ⊢ −K ≤ L ≤ K`,
computed by binary search where each step is one FM feasibility query
(`path ∧ L ≥ K+1` / `path ∧ L ≤ −K−1`). 64 iterations suffice; the queries
are the same `fm_sat` the whole verifier uses. Products then need only
`A·B ≤ 2^63−1` in `__int128`. This is a poor-man's interval domain —
*exact*, because FM decides each bound query exactly, and *free*, because no
new theory is added.

## 4. Two soundness bugs found by M15

A new analysis is a new auditor. Both findings are fixed and regressed.

### 4.1 The missing havoc (M1 while-rule)

The while-rule assumed the invariant over the *current* symbolic
environment. But variables assigned in the loop body kept their **stale
pre-loop values** in the head and post-loop states, making invariants about
them vacuous. This function was *proven*:

```baga
fn sneaky(n: i64) -> i64 {       // ensures output >= 0 — false!
    let mut i: i64 = 0
    let mut s: i64 = 0
    while i < n invariant i >= 0 { s = s - 1; i = i + 1 }
    return s                     // runtime: -n; the runtime contract fires
}
```

The fix is the textbook Hoare rule: collect the body's assigned/let-bound
variables, and **havoc** them (fresh abstract symbols) before assuming the
invariant — in the preservation head state and in the post-loop
continuation. Loop-produced havoc symbols join the M8 abstract class, so the
witness machinery can never pin them to unrealizable values. `sneaky` is now
honestly UNKNOWN; strengthening the invariant to cover `s` would fail
preservation (it is false) — also honest. Regression:
`examples/verify/loop_havoc.baga`.

### 4.2 INT64_MIN in the rational core

`rat_add`/`rat_mul`/`v_gcd` computed `a.num < 0 ? -a.num : a.num` — undefined
for `INT64_MIN`, and in practice the overflow guard misfired, so exact
rational arithmetic silently degraded exactly where overflow witnesses live.
All rational ops now use `__int128` intermediates, and `fm_sat` treats any
overflowed constraint as undecidable (answers SAT — the conservative
direction for every verdict: fewer proofs, never a false one).

## 5. Evaluation

| Example | Claim |
|---|---|
| `ovf_add.baga` | `n+1` under `0 ≤ n ≤ 100` proven safe; under `n ≥ 0` refuted at `n = INT64_MAX` |
| `ovf_mul.baga` | `\|a\|,\|b\| ≤ 10^9` proven safe via FM bounds; unbounded refuted at `a = 2^62, b = 2` |
| `div_zero.baga` | `m ≥ 1` proves division safety; without it, refuted at `m = 0` |
| `abs_val.baga` | ensures still proven, **and** `abs(INT64_MIN)` overflow refuted with the exact witness |
| `loop_havoc.baga` | no false PROVEN through stale pre-loop values |

All claims are machine-checked by `make test`.

## 6. Limitations and next steps

1. **The (2^62, 2^63) band is UNKNOWN** — a wider radix or 128-bit rational
   core would close it; engineering, not theory.
2. **Loop-carried overflow** is provable only through invariants that bound
   the variable (havoc makes unbounded growth honestly UNKNOWN).
3. **Runtime semantics** of overflow in Baga are still "whatever the emitted
   C does" (UB in theory, wrap in practice). Defining wrap (`-fwrapv`) or
   checked semantics is a language decision — and the input to the M16
   vision: `!Overflow` as an effect dimension, where potentially-overflowing
   arithmetic is *visible in the type* and the verifier discharges it.

## 7. Positioning

SMT-based verifiers get overflow reasoning "for free" from bitvector theory
and pay with timeouts and opacity. Baga's answer is structural: keep the
linear core, emit per-operation obligations, search bounds exactly, and say
UNKNOWN when the fragment ends. The result is an auditable, zero-dependency
arithmetic-safety analysis whose false-alarm rate is *zero by construction* —
the same design ethic as M8's conclusiveness gate, now applied to the
machine-integers themselves.

---

## Appendix A — How to reproduce

```bash
make
./baga --verify examples/verify/ovf_add.baga     # bounded proven / unbounded refuted
./baga --verify examples/verify/abs_val.baga     # abs(INT64_MIN) caught
./baga --verify examples/verify/loop_havoc.baga  # the soundness regression
make test                                        # full regression incl. M15
```

## Appendix B — Mapping to source

| Concept | Location |
|---------|----------|
| Scanner + obligation shapes | `scan_arith_expr`, `push_arith_obl` |
| Bound search | `fm_maxabs`, `feas_above`/`feas_below` |
| Verdicts | `verify_arith_obl` (AK_FIT / AK_MUL / AK_DIVZ) |
| Witness search (large candidates) | `find_arith_witness`, `build_eff_env` |
| Loop havoc | `collect_assigned_rec`, `havoc_vars` in `symexec_while` |
| Rational core hardening | `rat_*`, `v_gcd`, `fm_sat` overflow bail-out |
| Reporting | `verify_fn` (`аритметика` / JSON `"arith"`) |

---

*Galactic University draft — Baga project. The milestone that audits the
auditor: two soundness bugs found, zero false alarms shipped.*
