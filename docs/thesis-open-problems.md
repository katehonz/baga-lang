# Open Problems: A Map of the Remaining Darkness

**Monograph closing chapter**
*Three frontiers — liveness, full bitvectors, rich polynomials — and exactly
which corner of the sound / zero-dependency / complete triangle each one costs*

**Artifact:** the Baga compiler, after milestones M0–M23
**Status:** liveness wait-for acyclicity is M19 (structured fragment);
even powers and the BV identity envelope are M20; consecutive products and
`~n` are M21; signed right shift (floor semantics) is M22; logical right
shift (zero-fill) is M23; full bit-blasting and rich mixed polynomials
remain sketches

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

**Phase 5 sketch (2026-08-05):** `examples/verify/liveness_struct.baga`
proves two *counting* progress lemmas under `--verify` (not temporal
liveness): fixed-N unanimous 2PC ⇒ commit (`tpc_all_yes`); matched fan-in
counts ⇒ balanced (`fanin_matched`).

**M19 / Phase 6 (2026-08-23):** wait-for acyclicity is closed for the
*structured* fragment. `--verify` scans each `go` worker for a recv-first
or send-first first blocking op on the channel argument, counts parent
`send`/`recv`, and emits kind-3 protocol obligations:

- sequential send then recv, join after send, recv after a send-first
  worker → **PROVEN** (acyclic);
- join before send to a recv-first worker, recv with no matching
  send/producer → **REFUTED** (cycle).

Oracle: `examples/verify/waitfor.baga`. Still not temporal liveness (no
fairness/starvation) and not send-blocking on a full buffer; `if`/`while`/
nested `go` stay honest no-claim. The remaining liveness darkness is
fairness and unstructured dynamic scheduling.

**M24 (2026-08-23):** send-blocking on a full bounded buffer is now in the
fragment — and it was a soundness fix, not just coverage: `baga_chan_send`
waits on `not_full`, so a second `send` into a cap-1 channel with no
consumer deadlocks at runtime while M19 called it PROVEN. With a constant
`chan_new(literal)` capacity the verifier keeps a ghost cap and counts:

- parent send: `outstanding - recv_credits >= cap` with no complex worker
  on the channel → **REFUTED**; else "free slot" → **PROVEN** (a credit is
  a recv-first worker already spawned, join handle or `go_bg`);
- join of a send-only worker without competing producers:
  `wf_n_send > cap + parent recvs + other credits` → **REFUTED**, else
  **PROVEN**;
- a closed channel never blocks a send (returns -1) — both checks silent;
  symbolic capacity — honest no-claim.

Oracle: `examples/verify/send_block.baga`; adversarial cases
`lp6_sendblk_full_bad`/`lp6_sendblk_worker_bad` guard the exact false
PROVEN. Remaining darkness: fairness/starvation, `if`/`while` bodies,
recv2/select, >1 recv per worker.

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
- ~~signed vs. logical right shift on negatives~~ — **both closed**: signed is
  M22 with exact floor semantics (`n>>k = floor(n/2^k)`, masked count); logical
  (`>>>`, zero-fill) is M23 with the nonneg envelope `0 ≤ q ≤ 2^(64-k)-1`,
  `q ≥ 2^(63-k)` for `n ≤ -1`, and the same floor envelope as `>>` for
  `n ≥ 0`; arithmetic shift on i32 stays open;
- CRC / hash / checksum code, where the bit pattern *is* the meaning.

### 2.3 Why it is hard — the zero-dependency corner

Full bitvector theory is **decidable** — by bit-blasting to SAT, or by
word-level BV solvers — but exponential in bit-width, and it composes poorly
with the unbounded-ℤ path conditions the rest of the verifier uses. The real
cost is the ethic: a genuine BV procedure means either an external SMT solver
(breaking zero-dependency outright) or an in-house SAT/BV core (still
zero-*external*-dependency, but no longer "a small auditable FM loop"). This
is the frontier where the zero-dependency corner is genuinely threatened, and
this monograph is honest that it may not be closable on the original terms.

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

**M20 (2026-08-23):** option 2 lands. Idempotence (`n&n=n`, `n|n=n`),
`n|-1=-1`, const-fold of `&|^`, low-bit masks `n&(2^k-1) ∈ {0..2^k-1}`
for any sign, and a nonnegative variable envelope (`0 ≤ n&m ≤ n,m`,
`n|m ≥ n,m`). Oracle: `examples/verify/bitwise_mask.baga`.

**M21 (2026-08-23):** `n ^ -1` rewrites to `-n-1` (`~n` on i64 two's
complement, exact at INT64_MIN). Oracle: `examples/verify/bitwise_xor_not.baga`.
Still no bit-blast, no wrap-as-value, no variable XOR / unsign-less mask.

**M22 (2026-08-23):** signed right shift on negatives. The language pins
`>>` as arithmetic shift (floor) with a masked count (`b & 63`) in both
backends — C `>>` on negatives is implementation-defined, so the C backend
emits `baga_ashr_i64` and the LLVM backend masks before `ashr`. The
verifier records `n >> k` with the *count* (not a divisor) and injects
honest floor axioms: for `n ≥ 0` the trunc==floor envelope; for `n ≤ 0`
`q ≤ 0`, `q ≥ n`, `0 ≤ n - 2^k·q ≤ 2^k-1`; unknown sign stays weak;
constant `n` folds exactly. This also fixed a soundness bug: the shift was
previously recorded as C-trunc division, whose `2^k·q ≥ n` axiom for
negative `n` is false under ashr — a potential false PROVEN (now
adversarially covered by `lp6_shr_floor_bad`). Oracle:
`examples/verify/shr_floor.baga`. What remains of this frontier: full
bit-blasting, wrap as a *value*, variable XOR, symbolic masks without sign.

**M23 (2026-08-23):** logical right shift. The language gains `>>>`
(zero-fill) with the same masked count (`b & 63`) in both backends — the C
backend emits `baga_lshr_i64`, the LLVM backend masks before `lshr`; the
self compiler emits the same helper. The verifier records `n >>> k` with the
count and injects honest axioms: unconditionally `0 ≤ q ≤ 2^(64-k)-1` (the
sign bit is cleared); for `n ≥ 0` the same floor envelope as `>>`; for
`n ≤ -1` the lower bound `q ≥ 2^(63-k)`. Honest boundary: the exact relation
`q = floor((n + 2^64)/2^k)` for negative `n` is not linear in the ℤ
variables (`2^64` does not fit an i64), so only the lower bound is kept; and
an upper bound of exactly INT64_MAX (the `k = 1` case) is not provable,
because its negation needs INT64_MAX+1, which overflows the FM core into an
honest "cannot decide". Adversarial coverage: `lp6_lshr_neg_bad` and
`lp6_lshr_floor_bad` keep `>>`'s axioms from being silently reused for `>>>`.
Oracle: `examples/verify/lshr_bounds.baga`. What remains of this frontier:
full bit-blasting, wrap as a *value*, variable XOR, symbolic masks without
sign, arithmetic shift on i32.

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
property the monograph claims; option 2's checker-only variant is
intriguing (finding is hard, checking is cheap — the same asymmetry that makes
`--verify` possible at all) but speculative.

**M20 (2026-08-23):** option 1 lands for *pure even powers*. A recorded
product that is n^k (left-associated `n*n*n*n` is deg 4) injects
`n^{2k} >= 0` into the FM core; when the inner n^{k} is also recorded,
`n^{2k} >= n^k` (square dominance). Cubes were already the M8 sign table.
Oracle: `examples/verify/poly_even.baga`.

**M21 (2026-08-23):** mixed *consecutive* products. Two linear factors
that differ by ±1 satisfy `n(n±1) ≥ 0` and `p ≥` the lesser factor.
`n(n+2)` is refuted at `n = -1`. Oracle: `examples/verify/poly_consec.baga`.
Symbolic exponents, general mixed polynomials, and SOS stay open.

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
right question; the monograph's answer is "not yet, and here is exactly
what it would cost — and here is the one (liveness) that might be had cheaply."

That map of the darkness is itself a contribution. A verifier that knows
precisely where it stops is worth more than one that guesses.

---

*Baga project — open problems. Liveness, full bitvectors, rich polynomials.*
