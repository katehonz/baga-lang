# A Sound Nonlinear Fragment for Static Verification of Integer Programs

**Working title (Master’s-level research note)**  
*Product symbols, path-conditioned nonlinear guards, and a decidable bitwise envelope over Fourier–Motzkin*

**Artifact:** the Baga compiler (`--verify`), milestones M8–M13  
**Kernel decision procedure:** Fourier–Motzkin elimination over ℚ with integer tightening (M7)  
**Status:** implemented and regression-tested (`examples/verify/`)

---

## Abstract

Automated verifiers for imperative code typically either (i) stay inside
linear arithmetic, where Fourier–Motzkin / Simplex / SMT-LRA are complete, or
(ii) hand nonlinear goals to a general SMT solver (NIA, bitvectors) and accept
undecidability, timeouts, and opaque certificates. This note describes a
*third path* taken by Baga: **extend a pure Fourier–Motzkin core with a
finite family of *sound axiom schemas* over fresh symbols that stand for
products, quotients, remainders, and a handful of bitwise identities**.

The resulting fragment is incomplete by design, but every PROVEN verdict is
backed by an unsatisfiability proof in linear arithmetic over ℚ (hence over
ℤ after M7 tightening), and every REFUTED verdict is backed by a concrete
integral witness whose product/div/mod/bit values are *derived* from free
inputs—not guessed as free abstract symbols. Milestone M13 closes two
historically painful gaps: **nonlinear expressions inside branch conditions**
and a **sound special-case theory of bitwise operators** without committing
to full bitvector reasoning.

---

## 1. Problem statement

### 1.1 The classical gap

Let \(P\) be a pure function over signed 64-bit integers with a Hoare triple
\(\{R\}\;P\;\{E\}\). After symbolic execution, each path yields a system of
constraints \(\Gamma\) and a return expression \(r\). Proving \(E\) means
showing
\[
\Gamma \land R \land \lnot E[r/\mathit{output}] \quad\text{is unsatisfiable over }\mathbb{Z}.
\]
If every atom is linear, Fourier–Motzkin decides this over \(\mathbb{Q}\);
integer tightening (M7) restores exactness for strict inequalities on
integer variables. The moment a path condition contains \(n\cdot n \ge 1\),
a pure linear core must either:

1. **Give up** (UNKNOWN) — honest but weak; or  
2. **Abstract** the product as a free variable without relating it to \(n\) —
   which is **unsound for refutation** (spurious counterexamples) and **too
   weak for proof**.

Full nonlinear integer arithmetic (NIA) is undecidable. Bitvector theories
are decidable but exponential and do not compose cleanly with unbounded
integer path conditions.

### 1.2 Research question

> What is the largest *practically useful* extension of linear arithmetic for
> which a zero-dependency C verifier can still issue **sound** PROVEN /
> REFUTED / UNKNOWN verdicts, with **concrete counterexamples** and without
> an external SMT solver?

Milestones M8–M13 are successive answers to that question.

---

## 2. Method: symbolic products and axiom injection

### 2.1 Fresh symbols for nonlinear ops

When the symbolic evaluator meets a non-constant product \(f\cdot g\) of two
linear forms (and a `ReadsList` is threaded), it allocates a fresh name
\(p = \texttt{\_\_p}k\) and records the pair of factors \((f,g)\). Division
and remainder get \(\texttt{\_\_d}k\), \(\texttt{\_\_m}k\); the LSB mask
\(n\,\&\,1\) gets \(\texttt{\_\_b}k\).

Formally the state carries a set \(\Pi\) of *derived symbols*
\[
\Pi \subseteq \{ p \mapsto (f,g),\quad q \mapsto (n,d),\quad r \mapsto (n,d),\quad b \mapsto n \}.
\]

### 2.2 Axiom schemas (sound, incomplete)

After each path mutation that may introduce derived symbols, the verifier
**injects** only universally valid facts, conditioned on what the path already
proves about the factors:

| Schema | Hypothesis | Conclusion |
|--------|------------|------------|
| Square | — | \(v\cdot v \ge 0\), \(v\cdot v \ge v\), \(v\cdot v \ge -v\), \((v\pm 1)^2 \ge 0\) |
| Sign table | sign bounds on \(f,g\) | matching sign / magnitude of \(p=fg\) |
| Mono | \(f\ge 0,g\ge 1\) | \(fg \ge f\) (and symmetric) |
| Const div/mod | \(d\neq 0\), sign of \(n\) | C trunc sign laws; \(0\le n\%d < \|d\|\) when \(n\ge 0\) |
| Floor | \(n\ge 0,d>0\) | \(d\cdot(n/d) \le n\) and remainder bound |
| Identity | both \(n/d\) and \(n\%d\) | \(n = d\cdot q + r\) |
| Var div | \(m\ge 1,n\ge 0\) | \(0\le n/m \le n\), \(0\le n\%m < m\) |
| AM-GM form | squares \(f^2,g^2\) and product \(fg\) | \(f^2+g^2-2fg \ge 0\) |
| Bitwise IDs | — | \(n\|0=n\), \(n\&0=0\), \(n\oplus 0=n\), \(n\oplus n=0\), \(n\&(-1)=n\) |
| LSB | — | \(0 \le (n\&1) \le 1\) (two’s complement) |
| Shift | \(0\le k\le 62\) | \(n\ll k = n\cdot 2^k\); \(n\gg k\) as trunc \(n/2^k\) when \(n\ge 0\) |

No schema invents a fact that fails on \(\mathbb{Z}\) (or, for shifts, on the
documented sub-domain). Completeness is **not** claimed: e.g. full
bitvector arithmetic, general quantifier elimination for NIA, and overflow
semantics of C left-shift on negatives are out of scope.

### 2.3 Witness soundness (no false alarms)

A REFUTED verdict requires an integral assignment to **free** inputs such
that, after *deriving* every product/div/mod/bit value from its factors, the
antecedent holds and the ensures fails. Abstract symbols are never free in
the witness search. A secondary **conclusiveness gate** re-checks that the
pinned free variables make the obligation unsatisfiable for *all* remaining
abstracts—closing the M8 false-alarm class.

---

## 3. Milestone M13 contributions

### 3.1 Products inside Boolean conditions

Before M13, `bool_to_dnf` evaluated comparisons with `se_from_ast(..., NULL)`,
so a guard `n*n >= 1` was nonlinear → the whole branch became UNKNOWN.

**Change:** thread `ReadsList` through `bool_to_dnf` / `cmp_to_formula`. A
product in a condition allocates the same class of symbols as a product in a
return expression. Immediately after decoding an `if`/`while` condition, the
verifier calls `inject_prod_axioms`, so both the then and else forks inherit
e.g. \(n\cdot n \ge 0\).

**Example (proven):**
```baga
spec sq_guard {
    input: n: i64
    output: i64
    ensures: output >= 0
}
fn sq_guard(n: i64) -> i64 {
    if n * n >= 0 { return n * n } else { return 0 - 1 }
}
```

**Example (refuted with witness \(n=0\)):** claiming `output >= 1` for the same
shape.

### 3.2 Bitwise envelope without BV

Full BV theory would require bit-blasting or a dedicated decision procedure.
M13 instead implements **identities that are linear after rewriting**:

- Neutral / annihilator laws for `|`, `&`, `^` reduce to the identity or zero
  linear form.
- `n ^ n` is the constant 0 when both sides are the same linear form.
- `n & 1` is a fresh bit symbol with axiom \(0\le b\le 1\), **not** identified
  with C `n % 2` (which differs on negatives: `(-1)%2 = -1`, `(-1)&1 = 1`).
- `n << k` for constant \(k\in[0,62]\) is exact scaling by \(2^k\) in the
  idealized unbounded integer model used by the verifier.
- `n >> k` is modeled as truncating division by \(2^k\); path axioms for the
  quotient fire under \(n\ge 0\), where C arithmetic right-shift and trunc
  div coincide.

This is a deliberate *theory envelope*: enough to prove realistic contracts
on masking and power-of-two scaling, without claiming a complete BV solver.

---

## 4. Soundness argument (sketch)

**Theorem (PROVEN is sound).**  
If `--verify` reports ДОКАЗАНО for an ensures clause \(E\), then for every
integer input satisfying the requires, every terminating execution of the
function body yields a return value satisfying \(E\).

*Sketch.* Symbolic execution explores a cover of paths. On each path the
constraint store \(\Gamma\) is a conjunction of linear inequalities plus
injected axioms, each of which is true in \(\mathbb{Z}\) under the recorded
factor interpretations. The check shows
\(\Gamma \land R \land \lnot E\) is \(\mathbb{Q}\)-unsat; by M7 tightening,
it is also \(\mathbb{Z}\)-unsat for integer solutions. Anything outside the
fragment marks the path `bad` / UNKNOWN rather than PROVEN.

**Theorem (REFUTED is sound).**  
If `--verify` reports ОБРОЧЕНО with witness \(\vec v\), then executing the
function on \(\vec v\) (in the idealized semantics matching the model of
div/mod/bit used by the injector) violates \(E\).

*Sketch.* The witness search only accepts assignments where derived symbols
evaluate from free inputs; the conclusiveness gate rejects models that rely
on unconstrained abstracts.

**UNKNOWN is always safe:** it never asserts a false contract.

---

## 5. Related work (positioning)

| System | Nonlinear | Bitvectors | Certificates / deps |
|--------|-----------|------------|---------------------|
| Frama-C + WP | via SMT | via SMT | Why3 / external solvers |
| Dafny / Boogie | via Z3 | via Z3 | SMT |
| CBMC | bit-precise | native | bounded model checking |
| **Baga M8–M13** | axiom schemas over FM | identity envelope | **zero deps**, Farkas-style unsat over ℚ |

The scientific contribution is not “another SMT front-end”, but a **self-
contained, auditable fragment** suitable for teaching, bootstrapping, and
embedding in a language whose compiler is itself zero-dependency C.

---

## 6. Evaluation (regression oracle)

All claims below are machine-checked by `make test` (exit 0):

| Suite | Claim |
|-------|--------|
| `square`, `sign_prod`, `fact_full` | product axioms prove factorial / signs |
| `div_const`, `mod_const`, `div_mod_id`, `floor_mul` | const div/mod laws |
| `var_div`, `amgm` | variable divisors + \((x-y)^2\ge 0\) |
| `nonlinear_if` | product guards proven; bad ensures refuted at \(n=0\) |
| `bitwise_laws` | eight identities + LSB bounds proven; `n&1 >= 1` refuted at \(n=0\) |
| `spurious`, `fact_bad`, `bad_*` | soundness gates (no false PROVEN / no false REFUTED) |

---

## 7. Limitations and open problems

1. **Incomplete nonlinear algebra.** Cubes, mixed polynomials beyond recorded
   products, and non-constant exponents remain UNKNOWN.
2. **No full BV.** Arbitrary `n & m` for variable `m` is nonlinear / opaque.
3. **Shift overflow.** The model treats `<<` as exact multiply on \(\mathbb{Z}\);
   C signed overflow is UB — a future mode could track overflow flags.
4. **Negative `>>`.** Arithmetic vs logical shift is not fully modeled;
   axioms activate under \(n\ge 0\).
5. **Quantifiers** beyond element axioms (`v[*]`) and `sorted` are absent.
6. **Concurrency.** `!Par` is runtime-checked, not yet in `--verify`.

These are honest frontiers for further work; M13 is the
Master’s-scale closure of the “nonlinear condition + bitwise envelope”
chapter.

---

## 8. Conclusion

Baga’s verification pipeline shows that a **small set of factor-aware axiom
schemas**, combined with classical Fourier–Motzkin and integer tightening,
lifts a linear verifier into a practically interesting nonlinear and
bitwise fragment **without sacrificing soundness, counterexample quality, or
zero-dependency engineering**. Milestone M13—products in guards and a
documented bitwise envelope—is the piece that turns the pipeline from “cute
demo” into material suitable for a formal methods Master’s thesis:
a clear problem, a precise fragment, machine-checked examples, and an
explicit map of remaining undecidable darkness.

---

## Appendix A — How to reproduce

```bash
make
./baga --verify examples/verify/nonlinear_if.baga
./baga --verify examples/verify/bitwise_laws.baga
make test   # full regression including M0–M13
```

## Appendix B — Mapping to source

| Concept | Location |
|---------|----------|
| Symbolic eval + product/div/mod/bit | `src/verify.c` `se_from_ast` |
| DNF of Boolean AST | `bool_to_dnf`, `cmp_to_formula` |
| Axiom injection | `inject_prod_axioms` |
| if/while fork + inject | `symexec_stmts` / `symexec_while` |
| Witness + conclusiveness | `find_counterexample`, `push_pins_and_derived` |
| FM core | `fm_sat` (same file) |

---

*Baga project — research note (M13). Sound, incomplete; frontiers named.*
