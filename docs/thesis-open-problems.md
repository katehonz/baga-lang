# Open Problems: A Map of the Remaining Darkness

**Dissertation closing chapter**
*Three frontiers — liveness, full bitvectors, rich polynomials — and exactly
which corner of the sound / zero-dependency / complete triangle each one costs*

**Artifact:** the Baga compiler, after milestones M0–M18
**Status:** positioning and feasibility sketches; not implemented

---

## 0. Why these three

The preceding chapters built a verifier by *never* trading away two things:
**soundness** (no false PROVEN, no false REFUTED — UNKNOWN instead) and
**zero dependencies** (a Fourier–Motzkin core in C, no external solver). The
price is **incompleteness**, and the price is honest: every chapter ended with
a "Limitations" section naming what it cannot do. This chapter collects the
three largest such frontiers — the ones an audience will ask about first — and
states each one *precisely*: what it would let us prove, why it is hard, and
whether it can be had without breaking the ethic that makes the verifier
auditable. They are not a wishlist. They are a map of where the triangle
bites.

| Frontier | Closed chapters that point at it | Corner it threatens |
|---|---|---|
| Liveness for channels | M14, M16 (partial correctness only) | completeness (of *progress*), not soundness |
| Full bitvector theory | M13 (identity envelope), M15 (idealized shifts) | zero-dependency |
| Rich polynomials | M8–M13 (recorded products only) | completeness (undecidable in general) |

---

## 1. Liveness for channels

### 1.1 What is proven today

M14–M17 prove **partial correctness** of structured concurrency: fork–join
determinism (`join(go(f,x)) ≡ f(x)`), handle and channel protocols
(join-after-detach is refuted, send-on-closed is `-1`), channel *content*
invariants (rely–guarantee over payloads), and pair-status ranges. Every one
of these is a safety property — "nothing bad happens on any terminating
execution" — and every one rides the sequential FM core because Baga has no
shared mutable state.

### 1.2 What is missing

**Liveness** — "something good eventually happens". Concretely:

- *Deadlock freedom:* this fan-in/fan-out never reaches a state where every
  thread is blocked on a `chan_recv` that no thread will ever satisfy.
- *Termination of blocking operations:* `chan_recv(c)` returns under the
  assumption that some producer sends.
- *Progress under scheduling:* no fair schedule starves a worker.

### 1.3 Why it is hard — and why "no shared state" does not help here

The absence of shared mutable state collapsed *safety* onto sequential
reasoning because interference vanished. Deadlock is not about data; it is
about the **control structure of composition** — who is waiting on whom. That
structure survives the removal of shared state intact. Liveness needs
reasoning about interleaving and the absence of stuck global states, which is
exactly what separation-logic and model-checking tools do, at the usual price
(SMT, annotation weight, or bounded exploration).

### 1.4 Feasibility sketch (the Baga-native path)

The most promising attack is **structural, not exploratory**. The M16
discipline already requires *known-N fan-in* (exactly N sends, N recvs, then
close) for its content predicates to be clean. That is the seed of a
deadlock-freedom proof:

1. Count sends and recvs per channel *symbolically* (the verifier already
   tracks channel ghost state, M14).
2. Prove the counts match and the channel **dependency graph is acyclic**
   (no cycle of "thread A waits on a channel that thread B holds while B
   waits on A").
3. Matched counts + acyclic wait-for graph ⇒ deadlock freedom, as a graph
   theorem, not an interleaving exploration.

This keeps the zero-SMT ethic: the proof is a counting argument plus a
cycle check, both decidable in the existing symbolic machinery. The honest
limit: it covers *structured* deadlock (acyclic, count-matched), not arbitrary
dynamic scheduling, and it says nothing about fairness/starvation. Of the
three frontiers this is the one most native to Baga's design, and the one
most likely to yield a clean next chapter.

---

## 2. Full bitvector theory

### 2.1 What is proven today

M13 deliberately avoided bitvector theory. It implements a **bitwise identity
envelope**: neutral/annihilator laws (`n|0=n`, `n&0=0`, `n^0=n`, `n^n=0`),
the LSB bound (`0 ≤ n&1 ≤ 1`), and shifts as exact scaling (`n<<k = n·2^k`)
or truncating division (`n>>k ≈ n/2^k` for `n≥0`). These are *linear after
rewriting*, so they stay inside the FM core. M15 then reasons about overflow
in an *idealized* ℤ model and discharges it (M18) as an effect — but it does
not model two's-complement wrap at the bit level.

### 2.2 What is missing

Exact reasoning about:

- masking with a **variable** mask (`n & m` for symbolic `m`) — today opaque;
- two's-complement wrap as a *value* (not just an effect to be declared);
- signed vs. logical right shift on negatives;
- CRC / hash / checksum code, where the bit pattern *is* the meaning.

### 2.3 Why it is hard — the zero-dependency corner

Full bitvector theory is **decidable** — by bit-blasting to SAT, or by
word-level BV solvers — but exponential in bit-width, and it composes poorly
with the unbounded-ℤ path conditions the rest of the verifier uses. The real
cost is the ethic: a genuine BV procedure means either an external SMT solver
(breaking zero-dependency outright) or an in-house SAT/BV core (still
zero-*external*-dependency, but no longer "a small auditable FM loop"). This
is the frontier where the zero-dependency corner is genuinely threatened, and
the dissertation is honest that it may not be closable on the original terms.

### 2.4 Feasibility sketch

Two graded options, in the spirit of "sound envelope, never a false proof":

1. **Bounded-width BV on demand.** For goals that are *purely* bitwise over a
   small width (u8/u16, or the low k bits), lazily bit-blast just that goal to
   a tiny embedded SAT check, and fall back to the identity envelope
   otherwise. Zero external dependencies are preserved if the SAT core is
   in-house; auditability is preserved because the blast is bounded and
   isolated from the ℤ core.
2. **Reflected identities.** Keep ℤ as the only theory and *add* sound
   bitvector identities to the M13 envelope on demand (more rewrite rules,
   each independently checkable on ℤ). Incomplete forever, but never unsound
   and never a new dependency.

The honest verdict: option 2 is free and likely permanent; option 1 buys real
power at the cost of a small in-house solver — a defensible but real
concession.

---

## 3. Rich polynomials

### 3.1 What is proven today

M8–M13 handle nonlinearity by **recording products as fresh symbols** and
injecting sound axiom schemas: squares (`v²≥0`, `(v±1)²≥0`), a sign table,
monotonicity (`f≥0,g≥1 ⇒ fg≥f`), and the AM-GM form (`f²+g²−2fg≥0`). This
proves factorial, sign laws, and a surprising amount of realistic nonlinear
code — but only what reduces to *recorded binary products* and their linear
consequences.

### 3.2 What is missing

- **Cubes and higher powers** (`n³`, `n^k` for non-constant k);
- **mixed polynomials** beyond the products the evaluator happened to record;
- **polynomial loop invariants** of degree > 2.

### 3.3 Why it is hard — the completeness corner

Full nonlinear integer arithmetic (NIA) is **undecidable** (Hilbert's tenth
problem). There is no complete procedure to extend toward; the game is to
carve larger *sound* fragments. This frontier threatens completeness in the
strongest sense: not "we have not built it yet" but "no verifier can finish
it".

### 3.4 Feasibility sketch

Again two graded options:

1. **Iterated product symbols (incremental, sound, incomplete).** A cube is a
   product of a product. Extend the M8 symbol table to *recorded monomials* of
   bounded degree, reusing the sign/magnitude schemas one degree up. This is
   the cheapest extension: it stays entirely inside the FM core, adds no
   dependency, and is sound by the same argument as M8 — it simply proves more
   of the polynomial cases while leaving the rest honestly UNKNOWN.
2. **Sum-of-squares certificates (powerful, dependency-laden).** For the
   important special case "polynomial ≥ 0", SOS/semidefinite programming is
   decidable over ℚ and would discharge a large class of nonlinear goals with
   checkable certificates. But an SDP solver is a heavy dependency — the
   opposite corner from the ethic — unless one settles for rational SOS with a
   small in-house certificate *checker* (verify a supplied SOS decomposition
   over ℤ, which is just arithmetic, rather than *finding* one, which is the
   hard part).

The honest verdict: option 1 is the natural next step and preserves every
property the dissertation claims; option 2's checker-only variant is
intriguing (finding is hard, checking is cheap — the same asymmetry that makes
`--verify` possible at all) but speculative.

---

## 4. The triangle, stated plainly

The three frontiers are independent, and each illuminates a different edge of
the design space:

- **Liveness** shows the limit of the *safety* lens — and, encouragingly,
  admits a structural attack that keeps the ethic intact.
- **Full bitvectors** shows the limit of *zero-dependency* — decidable, but
  only at the price of a solver.
- **Rich polynomials** shows the limit of *completeness* — undecidable in
  general, so forever a fragment, but a fragment that can be grown one
  recorded monomial at a time without ever shipping a false proof.

None of them is a reason to distrust the finished work. They are the honest
boundary of a verifier that chose, at every milestone, to be **sound first,
auditable second, and complete only as far as those two allow**. The audience
that asks "but can it do liveness / bitvectors / polynomials?" is asking the
right question; the dissertation's answer is "not yet, and here is exactly
what it would cost — and here is the one (liveness) that might be had cheaply."

That map of the darkness is itself a contribution. A verifier that knows
precisely where it stops is worth more than one that guesses.

---

*Galactic University draft — Baga project. Keep the jokes; keep the theorems
tighter; and never pretend the darkness is smaller than it is. ⚔️*
