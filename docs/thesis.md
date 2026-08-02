# A Self-Contained, Sound, Auditable Verifier by Recombination

**Research monograph — binding document (front matter, chapter map, conclusion)**
*The Baga project, milestones M0–M18. Claims are tied to artifacts:
`make test`, `--verify` certificates, and concrete witnesses.*

> The chapters below are the four research notes
> `thesis-m13-nonlinear-fragment.md`, `thesis-m14-par-fragment.md` +
> `thesis-m16-channel-invariants.md` + `thesis-m17-pairs.md`,
> `thesis-m15-arith-safety.md`, and `thesis-m18-overflow-effect.md`, closed by
> `thesis-open-problems.md`. This document is the spine that binds them into
> one argument.

---

## Abstract

Can a useful static verifier be built with **no external solver**, **no
unsound abstraction**, and **no new proof theory** — by recombining classical
pieces (Fourier–Motzkin elimination, Farkas' lemma, integer tightening,
symbolic execution, effect rows, rely–guarantee) in the right way? This
monograph answers *yes, up to an explicit and honest frontier*, through
eighteen milestones of the Baga compiler. The verifier reasons over
mathematical integers with a pure linear-arithmetic core, then extends its
reach by *sound, incomplete* envelopes — recorded products and bitwise
identities (Chapter 1), structured concurrency over a language with no shared
state (Chapter 2), and a bridge from idealized integers to the machine's i64
(Chapter 3). The arc culminates in Chapter 4, where that bridge is promoted
into the type system: `!Overflow` becomes an effect, the verifier's
arithmetic analysis becomes effect inference, and the effect system and the
verifier are shown to be the same judgement. Every PROVEN verdict is backed by
a linear-arithmetic unsatisfiability certificate; every REFUTED verdict by a
concrete witness; everything else is honestly UNKNOWN. The closing chapter
maps the three frontiers that remain — liveness, full bitvectors, rich
polynomials — and states precisely which property each one costs.

---

## 1. Introduction

### 1.1 The question

The 2026 question, per the project's concept note, is not "how do we avoid
segfaults" — Rust answered that. It is **"how do we trust code we did not
write"**, in an era when an AI writes the code and a human verifies it. Three
pillars follow: specifications as first-class citizens; effects as dimensions
of the type; and readable *proof sketches*, with static certificates where `--verify` decides. The verifier is
the engine that makes the first two pillars mean something.

### 1.2 The thesis

> A practically useful verifier does not need a general SMT solver. It needs a
> **small, auditable decision core** (linear arithmetic over ℚ, tightened to
> ℤ), a **discipline of sound extensions** (axiom schemas that never invent a
> false fact), and the **honesty to say UNKNOWN** at the boundary. With those
> three, a surprising amount of nonlinear, concurrent, and machine-level
> reasoning falls out — and the result can live in a compiler with zero
> dependencies, suitable for teaching, bootstrapping, and trust.

The contribution is therefore not a new algorithm but a **recombination** and
a **design ethic**: soundness first, auditability second, completeness only as
far as those allow.

### 1.3 The unifying thread

One core recurs in every chapter: **Fourier–Motzkin elimination with a
conclusiveness gate**. It decides the linear path conditions (M0–M7); it is
wrapped by product/bitwise axiom schemas (M8–M13); it discharges fork–join and
channel obligations by *adding path constraints* rather than new theory
(M14, M16, M17); it searches exact arithmetic bounds (M15); and its verdicts
become an effect (M18). The verifier grows by *accreting sound facts onto the
same core*, never by replacing it. That continuity is the monograph's
structural claim, and M18 is its demonstration: the newest feature reuses the
oldest machinery unchanged.

---

## 2. Chapter map

### Chapter 1 — A sound nonlinear fragment (M8–M13)
`thesis-m13-nonlinear-fragment.md`

The classical gap: a linear core must give up or unsoundly abstract the moment
a path condition contains `n·n ≥ 1`. The answer: **fresh symbols for products,
quotients, remainders, and a bitwise LSB mask**, plus a finite family of
*sound axiom schemas* (square, sign table, monotonicity, div/mod laws, AM-GM,
bitwise identities, shifts). Incomplete by design, but every PROVEN verdict is
a ℚ-unsat certificate and every REFUTED verdict a derived witness. M13 closes
two gaps — nonlinear guards inside `if`/`while`, and a bitwise envelope that
is *linear after rewriting* — without committing to full bitvector reasoning.

### Chapter 2 — Structured concurrency without a memory model (M14, M16, M17)
`thesis-m14-par-fragment.md`, `thesis-m16-channel-invariants.md`,
`thesis-m17-pairs.md`

The observation: Baga has **no shared mutable state** (no globals, no
closures; `go(f, x)` passes one `i64` by value), so data races are impossible
*by construction* and interference vanishes. What remains — value flow and
protocol adherence — is per-path, per-handle, exactly what the sequential core
already does. Fork–join is a *functional* model (`join(go(f,x)) ≡ f(x)`);
handle and channel protocols are a small finite-state ghost analysis; channel
*content* invariants are rely–guarantee discharged at spawn, reusing the
element-axiom machinery; and the pair abstraction (`cell2`) makes composite
channel results and packed worker contracts provable. The chapter's punchline:
the *absence* of a feature (shared state) is the feature.

### Chapter 3 — Machine integers: closing the ℤ-vs-i64 gap (M15)
`thesis-m15-arith-safety.md`

A verifier over ℤ that compiles to i64 has a hole: every PROVEN verdict is
conditional on no overflow. M15 emits one **arithmetic-safety obligation** per
operation (FIT / MUL / DIVZ), proves it by *exact bound search* over the same
FM core (binary search on feasibility), refutes it with a large-magnitude
witness, or reports UNKNOWN. When all obligations prove, the idealized model
and the runtime provably coincide. Building the analysis audited the auditor:
it exposed and fixed two real soundness bugs (a missing loop havoc, and
INT64_MIN-unsafe rational arithmetic).

### Chapter 4 — The culmination: `!Overflow` as an effect (M18)
`thesis-m18-overflow-effect.md`

M15's verdict was prose; M18 makes it a **type**. `!Overflow` is an effect —
a *permission*, like `!IO`, not a claim. The M15 obligations are
reinterpreted as the **effect inference** for `!Overflow`; the one-way effect
check (body ⊆ declared) is the **discharge**. A function that omits
`!Overflow` claims safety and is refuted with a witness when false; a function
that declares it advertises the risk and is discharged. The effect system
(pillar 2) and the verifier (pillar 1, depth) become one judgement seen from
two sides. Almost nothing new is computed — the milestone is a change of
*typing judgement*, which is exactly why it is the culmination.

### Coda — Open problems
`thesis-open-problems.md`

The honest frontier, stated as a map of the sound / zero-dependency /
complete triangle: **liveness for channels** (the limit of the safety lens;
admits a structural, ethic-preserving attack), **full bitvectors** (decidable
but threatens zero-dependency), and **rich polynomials** (undecidable in
general, so forever a fragment — but one growable by recorded monomials
without ever shipping a false proof).

---

## 3. What holds the argument together

Three commitments are never broken across all eighteen milestones, and the
argument is weaker wherever any of them is even tempted:

1. **Soundness is non-negotiable.** No milestone ships a false PROVEN or a
   false REFUTED. When the fragment ends, the verdict is UNKNOWN — and M15
   showed the payoff of that discipline, when the new analysis *found* two
   soundness bugs in the existing pipeline rather than adding one.
2. **Zero dependencies.** The whole verifier is Fourier–Motzkin + symbolic
   execution in C, in a compiler whose bootstrap is `gcc`. This is what makes
   it auditable, teachable, and bootstrappable — and it is the property the
   open-problems chapter is most careful not to sacrifice.
3. **Counterexamples are first-class.** A REFUTED verdict is not "failed to
   prove"; it is a concrete input that breaks the contract, derived from free
   variables (never from unrealizable abstract symbols, by the M8
   conclusiveness gate). The verifier is a *judge*, and its refutations are
   evidence.

The recurring move — *extend reach by accreting sound facts onto a fixed
core, and report the boundary honestly* — is the method this monograph
demonstrates and the method it recommends.

---

## 4. Conclusion

The work set out to show that a verifier can be **useful, sound,
auditable, and dependency-free** at once, by recombination rather than
invention. The four chapters are the evidence:

- a nonlinear and bitwise fragment that stays inside linear arithmetic
  (Chapter 1);
- a concurrency story that collapses onto the sequential core because the
  language forbids shared state (Chapter 2);
- a machine-integer bridge that turns "no overflow" from an assumption into a
  checked obligation — and audits the auditor along the way (Chapter 3);
- and the identification of that obligation with an effect, so that the type
  system and the verifier speak with one voice (Chapter 4).

The culmination (M18) is deliberately small in code and large in meaning: it
proves that the pillars were never separate. The effect that tracks `!IO` and
the judgement that proves `ensures` are the same mechanism; overflow was
always an effect waiting to be named.

What remains is honest and named (the coda): liveness, full bitvectors, rich
polynomials. The verifier does not reach them yet — and says so, with a
counterexample-shaped hole where each one would go. A verifier that knows
precisely where it stops is the kind of tool worth trusting with code you did
not write. That, finally, was the question.

---

## Appendix — Reproducing the whole arc

```bash
make && make test && make self && make test-llvm   # the regression oracle

# Chapter 1
./baga --verify examples/verify/nonlinear_if.baga
./baga --verify examples/verify/bitwise_laws.baga
# Chapter 2
./baga --verify examples/verify/par_join.baga
./baga --verify examples/verify/chan_inv_par.baga
./baga --verify examples/verify/pair_go.baga
# Chapter 3
./baga --verify examples/verify/ovf_add.baga
./baga --verify examples/verify/loop_havoc.baga
# Chapter 4
./baga --verify examples/verify/ovf_eff_declared.baga   # discharged, exit 0
./baga --verify examples/verify/ovf_eff_refuted.baga    # violation, exit 1
./baga --proofs examples/verify/ovf_eff_safe.baga       # f_overflow_safe
```

---

*Baga project — research monograph. One decision core, one soundness ethic,
one typing-and-verification judgement.*
