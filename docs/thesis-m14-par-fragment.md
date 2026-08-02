# Verifying Structured Concurrency Without a Memory Model

**Working title (PhD-scale research note, slice 1)**
*Fork–join determinism and handle protocols over a Fourier–Motzkin core*

**Artifact:** the Baga compiler (`--verify`), milestone M14
**Prior art in the same pipeline:** M0–M13 (see `thesis-m13-nonlinear-fragment.md`)
**Status:** implemented and regression-tested (`examples/verify/par_*.baga`)

---

## Abstract

Static verification of concurrent programs usually starts where the pain
starts: weak memory models, interference, thread-modular reasoning. This note
describes a different entry point, enabled by a language-design decision:
**Baga has no shared mutable state**. There are no globals and no closures;
`go(f, x)` spawns a named function on a single `i64` passed by value. Data
races are therefore impossible *by construction*, and a large, useful class of
concurrent programs — structured fork–join with CSP channels over `i64` —
becomes verifiable with the same linear-arithmetic core (Fourier–Motzkin +
integer tightening) that powers the sequential fragment. Two mechanisms
suffice: a **functional model of fork–join** (`join(go(f, x)) ≡ f(x)` for pure
workers, via the existing assume–guarantee machinery) and **ghost-state handle
protocols** (the runtime's join/detach state machine and channel open/closed
flags, checked statically with counterexamples).

---

## 1. The observation

Concurrent verification is hard because of *interference*: threads mutate
shared state, so assertions must be stable under arbitrary environment steps
(Owicki–Gries, rely–guarantee, separation logic). Remove shared mutable state
and interference vanishes:

| Traditional obstacle | Baga |
|---|---|
| Shared variables | none exist (no globals, no closures) |
| Captured environments | impossible — worker gets one `i64` by value |
| Aliasing of handles | only through explicit `let h2 = h` (tracked symbolically) |
| Data races | impossible by construction |
| Channels | thread-safe monitor (mutex + condvars in the runtime) |

What remains to verify is **value flow** (what does `join` return?) and
**protocol adherence** (is every handle consumed at most once?). Both are
*per-path, per-handle* properties — exactly what symbolic execution over a
linear core already does.

## 2. Fork–join determinism

**Lemma.** Let `f` be a pure, terminating `fn(i64) -> i64` whose contract is
verified. Then for every `x`, `join(go(f, x))` and `f(x)` denote the same
value, and the spawn site satisfies the Hoare triple of the call site.

*Argument.* Purity means the worker's result depends only on `x`; the runtime
join returns the worker's return value (a `pthread_join` + stored result).
Scheduling affects *when*, not *what*. Hence the worker's `requires` must hold
at spawn (discharged as call-site obligations) and its `ensures` may be
assumed for the join result — precisely the M5 assume–guarantee rule. The
verifier literally synthesizes the call `f(x)` at the `go` site and reuses
the sequential machinery. No new proof rules.

**Consequence.** `ensures` of a parallel fan-out function is provable with
zero concurrency-specific reasoning:

```baga
fn par_double(n: i64) -> i64 !Par {
    let h1 = go(worker, n)     // worker: requires x >= 0, ensures output >= 1
    let h2 = go(worker, n)
    return join(h1) + join(h2) // ensures output >= 2 — ДОКАЗАНО
}
```

A claim contradicting the worker's contract through the join is REFUTED with
a concrete counterexample (`par_join_bad.baga`: `output <= 0` refuted at
`n = 0`), subject to the same M8 conclusiveness gate as sequential calls —
no false alarms through abstract call values.

## 3. Handle protocols as ghost state

The runtime defines a finite protocol per join handle
(`src/baga_par_rt.c`): `open → joined | detached`; `join` after `detach` is
*fatal* (`exit(1)`). M14 lifts this to a static analysis:

- Each `go` allocates a fresh symbolic handle `__hN` with ghost state `open`
  and the worker's symbolic result attached.
- `join(h)` / `detach(h)` on a known-open handle transition the state and
  yield the stored result / `0`.
- A second consume on the same handle emits a **protocol obligation**: an
  unsatisfiable positive constraint (`1 <= 0`) on the current path, which the
  standard discharge machinery REFUTES with a concrete witness whenever the
  path is live — unconditionally ("при всеки вход") when the path is free of
  input constraints (`par_detach_bad.baga`).
- Unknown handles (e.g. an `i64` parameter) yield no claims — sound by
  silence.

Because symbolic states fork per branch and never merge, the ghost state is
path-precise: `join` in both branches of an `if` is fine; `join` in one
branch and `detach` in the other is fine; either order on a single path is
caught.

Channels get the analogous two-state protocol (`open/closed`), which already
proves a real fact: `send` on a known-closed channel is definitely `-1`
(`par_chan.baga`). Payload claims about `recv` are honestly UNKNOWN — the
interval is exact.

## 4. Soundness and honesty

- **PROVEN** verdicts rest on the same ℚ-unsat certificates as M0–M13; the
  concurrency mechanisms only *add path constraints* that are valid under the
  runtime semantics (fork–join determinism, send-return interval, ghost
  transitions).
- **REFUTED** verdicts reuse the witness search with the M8 conclusiveness
  gate; protocol refutations re-check that the violating path is live.
- **UNKNOWN** covers everything outside the fragment: pair-returning builtins
  (`chan_recv2`, `chan_try_recv`, `chan_select2*`), mutexes, `pool_map`,
  effectful workers, and any function declaring a non-`Par` effect.

## 5. Limitations and the road to M15

1. **No cross-thread content invariants.** "Every value on channel `c` is
   positive" requires relating send and recv sites across threads —
   rely–guarantee over channel contents. This is the real PhD-scale problem
   and is deliberately out of slice 1.
2. **No liveness.** Deadlock freedom, lock ordering, and termination of
   blocking `recv` are untouched (partial-correctness reading throughout).
3. **No pair protocols.** The `cell2`-returning channel APIs need a pair
   abstraction in the symbolic domain before their status ranges become
   provable.
4. **Pure workers only.** Effectful workers (the cloud accept-loop pattern)
   are gated out; verifying them needs the effect system to meet the
   verifier, not just the runtime.

## 6. Positioning

| System | Concurrency model | Needs |
|---|---|---|
| VST / Iris (Coq) | full separation logic | interactive proofs |
| Verifast / Viper | permission accounting | annotations, SMT |
| CBMC | bounded interleaving exploration | bounds, SAT |
| **Baga M14** | structured fork–join + channels, no shared state | **nothing** — same FM core |

The contribution is the observation that a *language restriction* (no shared
mutable state) collapses a large fragment of concurrent verification onto
sequential machinery — and that the remaining runtime protocol (join/detach,
open/closed) is a small finite-state analysis with counterexamples, free of
SMT solvers and free of new proof theory.

---

## Appendix A — How to reproduce

```bash
make
./baga --verify examples/verify/par_join.baga        # fork–join determinism
./baga --verify examples/verify/par_detach_bad.baga  # protocol refuted
./baga --verify --json examples/verify/par_chan.baga # machine-readable
make test                                            # full regression incl. M14
```

## Appendix B — Mapping to source

| Concept | Location |
|---------|----------|
| Fragment gates (Par-only effects, builtin whitelist) | `verify_fn_collect`, `has_unsupported_rec`, `par_call_gate` |
| Ghost handle state | `HandleList` in `State` (`src/verify.c`) |
| Fork–join functional model | `eval_par_call` (synthesizes the worker call, reuses `eval_user_call`) |
| Protocol obligations (kind 3) | `push_protocol_violation`, `verify_call_req` |
| Text/JSON reporting | `verify_fn` (`"protocol"` field) |

---

*Galactic University draft — Baga project. Slice 1 of the concurrency chapter:
the part where the absence of a feature (shared state) is the feature.*
