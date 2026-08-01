# Design: Static Spec Verification (depth pillar I)

Date: 2026-08-01
Status: Draft (awaiting user approval)

## 1. Goal and non-goals

**Goal.** Turn the existing `requires`/`ensures` contracts from *runtime* checks
into **statically verified** obligations for a defined fragment: the compiler
proves an implementation satisfies its spec, or rejects it at compile time with
a concrete counterexample. This is the "depth" turn — making the language's
headline promise ("the compiler is the judge") real rather than demo-deep.

**Constitutional principle — soundness above all.** The verifier is three-valued:
`PROVEN`, `REFUTED (counterexample)`, or `UNKNOWN (cannot decide)`. It must
**never** report PROVEN for a program that can violate its contract.
Incompleteness (UNKNOWN on a correct program) is acceptable and honest;
unsoundness (OK on a wrong program) is a project-ending bug — it is strictly
worse than the runtime check we already have. Every design choice favors
soundness.

**Non-goals (this spec defines the whole staircase but builds only step M0):**
external SMT (zero-dep, own decision procedures); full Presburger/bit-precise
i64 in M0 (we start with linear arithmetic over integers, soundly); recursion,
loops, arrays, structs, effects, `guarantees` prose — later steps; verifying
`main` or impure functions.

## 2. Current state we build on (verified)

- A `spec` is matched to a function **by name** (`find_ensures_spec`).
- `requires`/`ensures` are real AST boolean expressions (`NODE_ENSURE.ensure_expr`),
  already parsed and type-checked; `guarantees` is prose (ignored by verification).
- Codegen emits a runtime wrapper: check `requires` before, call impl, bind the
  return value to `output` (`b_output`), check each `ensures` after.
- So the verifier reuses the **same** `ensure_expr` ASTs — no new surface syntax
  in M0. We add analysis, not language.

## 3. Architecture

A new module `src/verify.c` (+ declarations in `include/baga.h`), invoked by a
new `--verify` CLI mode. It does not touch codegen; it reads the checked AST.

```
        checked AST (fn + its spec)
                 │
   ┌─────────────▼──────────────┐
   │  Symbolic execution (symexec) │  walk the body; state = {var ↦ SymExpr};
   │  straight-line + if/else       │  fork on branches; collect obligations at
   └─────────────┬──────────────┘  each `return`
                 │  obligations: (path_cond, return_symexpr)
   ┌─────────────▼──────────────┐
   │  VC generation                 │  per path:  requires ∧ path_cond
   │                                │     ⇒ ensures[output := return_symexpr]
   └─────────────┬──────────────┘
   ┌─────────────▼──────────────┐
   │  Decision procedure (LIA)      │  negate ⇒ linear constraints;
   │  Fourier–Motzkin + Farkas      │  PROVEN / REFUTED(+model) / UNKNOWN
   └────────────────────────────┘
```

### 3.1 SymExpr (symbolic values)

Canonical **linear form** `c + Σ coeff_i · v_i` over rationals (use `int64_t`
num/den or a small bigint-of-two; M0 keeps coefficients integral and small).
- `+`, `-`, `const*var`, constant folding stay inside linear forms exactly.
- `*` of two non-constants, `/`, `%`, calls → **non-linear/unknown**: the
  symexpr becomes an opaque `UNKNOWN` node. Any obligation that depends on an
  opaque node resolves to `UNKNOWN` (never PROVEN) — this is the soundness
  escape hatch.

### 3.2 Symbolic execution (M0 fragment)

Handles: `let`/`let mut` + assignment (linear RHS), `if/else` (fork state, add
branch predicate / its negation to the path condition), `return` (emit
obligation), implicit final-expression return. **Rejects to UNKNOWN** (honestly,
per-function): loops, recursion (any self-call), non-linear RHS, arrays/structs,
effects. A function is verified only if *all* its paths discharge.

### 3.3 Decision procedure — linear integer arithmetic

Obligation `A ⇒ B` becomes `SAT?(A ∧ ¬B)`:
- Convert to a conjunction of linear atoms `Σ a_i x_i {≤,<,=,≥,>} c`.
- **Fourier–Motzkin elimination** decides satisfiability over the rationals
  (sound + complete for linear rational arithmetic).
- **Soundness direction (PROVEN):** if `A ∧ ¬B` is UNSAT even over Q, it is
  UNSAT over Z ⇒ the obligation holds over integers. **This is the only path to
  PROVEN, and it is sound.** A Farkas certificate (positive linear combination
  yielding `0 ≥ c>0`) is recorded for reporting/proof-extraction.
- **REFUTED:** if SAT and the satisfying assignment is **integral** and really
  violates B (re-checked by substitution), report it as a concrete counterexample.
- **UNKNOWN:** rational-only witness (e.g. `2x = 1`), opaque nodes, or anything
  non-linear → "cannot decide", reported honestly. We do **not** claim PROVEN.

This makes M0 **sound and complete for linear integer obligations whose
relaxation is tight**, and conservatively UNKNOWN elsewhere — exactly the
honesty contract in §1.

### 3.4 i64 overflow

M0 models **mathematical integers**, not wraparound. A proof is about the math
function; if the program can overflow i64, that is a separate concern. M0 notes
this limitation; a bit-precise mode (or an overflow-obligation) is a later step.
The existing `факториел` spec already documents overflow in `requires`
(`n <= 20`), which is the intended idiom.

## 4. M0 acceptance (the deliverable this milestone)

`./baga --verify file.baga` verifies every pure, non-recursive, loop-free,
linear `i64` function that has a spec, and prints, per ensures clause:

```
verify факториел:
  ensures #1 (output > 0): PROVEN
  ensures #2 (n <= 1 || output >= n): PROVEN
```
or
```
verify лош:
  ensures #1 (output >= 0): REFUTED
    counterexample: x = -5  →  output = -5
```
or `UNKNOWN (non-linear / cannot decide)` with the reason. Functions outside the
fragment print `verify f: SKIPPED (reason)` — never silently passed.

**Canonical M0 examples** (committed under `examples/verify/`):
- `abs_val` — `ensures output >= 0`: PROVEN.
- `max2` — `ensures output >= a && output >= b`: PROVEN.
- `bad_abs` (`return x`) against `output >= 0`: REFUTED with counterexample.
- `clamp` (nested if) — bounds PROVEN.
- A non-linear case (`x*x >= 0`): UNKNOWN (honest).

## 5. Testing — the verifier must be tested harder than the code it checks

- **Soundness tests (critical):** a suite of *wrong* programs (each has a real
  counterexample, confirmed by the existing runtime check / `--test-specs`).
  Assert `--verify` never says PROVEN for any of them. A single violation fails
  the build.
- **Completeness tests:** known-correct linear programs all PROVEN.
- **Oracle agreement:** for the M0 fragment, `--verify` PROVEN ⇒ runtime check
  never fires on `--test-specs` random inputs (cross-check the two judges).
- Wired into `make test` as a `verify` block; the LLVM/Cranelift oracles are
  unaffected (`--verify` is a separate mode, no codegen change).

## 6. The staircase (whole shape; only M0 is built now)

| Step | Adds | Decision procedure |
|------|------|--------------------|
| **M0** | straight-line + if/else, linear i64 | Fourier–Motzkin + Farkas |
| M1 | loops via user/`spec` invariants (inductive) | same + invariant checks |
| M2 | arrays: bounds + element invariants | add array theory (read-over-write) |
| M3 | non-linear: intervals + Z3-bitvector-free Gröbner-light / testing | sound under-approx |
| M4 | feed verified invariants into `proofs.c` → real extracted theorems | — |

Each step ends green and keeps the soundness contract. M0's architecture
(symexpr / symexec / DP interface / three-valued result) is designed so M1–M4
extend, not rewrite.

## 7. Honest difficulty assessment

This is the hardest single feature in the project to date — an order of
magnitude beyond `httpdbaga`/`jwtbaga`. The risk is not typing speed but
**soundness correctness**: a subtle bug in Fourier–Motzkin or the integer/rational
boundary makes the verifier a liar. Mitigations: the three-valued discipline
(when unsure → UNKNOWN, never OK), the Farkas certificate checked independently,
and the soundness test suite gated in `make test`. If M0 proves too coarse
(most things UNKNOWN), that is a legitimate, reportable result — not a failure
to be papered over.

## 8. Conventions

- C11, `-Wall -Wextra` clean, zero dependencies, no new build steps.
- `self/compiler.baga` untouched; `make self` stays green (verification is a
  C-bootstrap-only mode; the self compiler need not implement it).
- Comments/docs in English; user-facing verify messages follow the existing
  Bulgarian diagnostic style.
