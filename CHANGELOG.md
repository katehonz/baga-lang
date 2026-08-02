# Changelog

## [Unreleased]

### Static verification — M16: channel content invariants (rely–guarantee)
- New statement-level annotation `invariant <expr>` (contextual keyword):
  - `invariant c[*] >= 1` — "every payload sent on channel `c` satisfies the
    predicate", anchored on the channel's resolved symbolic var (aliases work).
  - scalar form (no `[*]`) acts as `assume` — the path gains the constraint.
  - `chan_send` discharges the predicate (else the axiom is dropped, M3
    rule); `chan_recv` instantiates it on the result.
- Cross-thread: a worker's `requires c[*] ...` is discharged against the
  caller's axioms at `go` spawn (kind-2 obligation, provable); a worker
  without matching requires drops them at spawn — honest, never unsound.
  The same discharge/drop rules apply at plain M5 calls.
- `go` workers may now declare `Par` effects (channel-using workers were
  previously outside the fragment; non-`Par` effects still skip).
- Examples: `examples/verify/chan_inv{,_bad,_par,_escape}.baga`.
- Note: `docs/thesis-m16-channel-invariants.md`.

### Static verification — M15: arithmetic safety (the ℤ-vs-i64 bridge)
- New kind-4 obligations: every `+ - * -x / % <<` in verified code gets a
  verdict — ДОКАЗАНО (cannot overflow on this path), ОБРОЧЕНО with a concrete
  large-magnitude witness (e.g. `abs(INT64_MIN)`, `n + 1` at `n = INT64_MAX`,
  `n / m` at `m = 0`), or honestly НЕ МОГА ДА РЕША.
- Exact bound search over the FM core (binary search on feasibility);
  products use tightest provable |factor| bounds, compared in `__int128`.
- When all arith obligations of a function are proven, the idealized-ℤ model
  and the i64 runtime coincide — the output says so; otherwise it marks the
  ensures verdicts as idealized-model-only. JSON: `"arith": [...]`.
- The extreme window (2^62, 2^63) reports UNKNOWN, never a false proof.

### Soundness fixes (found by M15)
- **M1 loop havoc**: variables assigned/let-bound in a `while` body are now
  havoced before the invariant is assumed (head + post-loop states). Before,
  the post-loop state kept stale pre-loop values, making invariants vacuous —
  a loop returning `-n` was falsely ДОКАЗАНО for `output >= 0`. Now honestly
  UNKNOWN unless the invariant really covers the variable
  (`examples/verify/loop_havoc.baga`).
- **Rational core**: `rat_add/rat_mul/rat_mk/v_gcd/rat_neg` are now
  INT64_MIN-safe (`__int128` intermediates); `fm_sat` bails out conservatively
  (SAT = "cannot decide") on overflowed constraints.

### Static verification — M14: `!Par` enters `--verify`
- Functions whose only effect is `Par` are now verifiable (other effects
  still skip honestly).
- **Fork–join determinism:** for a pure verifiable worker `f`,
  `join(go(f, x)) ≡ f(x)` — the worker spec applies via M5 assume–guarantee
  (requires discharged at spawn, ensures assumed for the join result).
- **Handle protocols:** ghost state per symbolic handle —
  `spawn → join | detach`; join/detach after consume is REFUTED with a
  counterexample (join-after-detach is fatal at runtime). Channels track
  open/closed; `send` on a known-closed channel is provably `-1`.
- New JSON field `"protocol"` for kind-3 obligations.
- Boundary (honest skips): pair-returning builtins (`chan_recv2`,
  `chan_try_recv`, `chan_select2*`), mutexes, `pool_map`, effectful workers.
- Examples: `examples/verify/par_{join,join_bad,detach_bad,chan}.baga`.
- Note: `docs/thesis-m14-par-fragment.md`.

### Proof extraction
- `--proofs` now prints the verifier's established facts, not just heuristics:
  - `_terminates` uses the real verdict — recursion with a proven `decreases`
    measure is reported as full correctness; otherwise honestly partial.
  - while-loop invariants appear as `lemma <fn>_invariant_<k>` with their
    Hoare status (init + preservation proven, or honestly unproven → UNKNOWN).

## [0.2.0] — 2026-08-02

First tagged release after the static-verification arc and theory write-up.

### Static verification (`--verify`)
- **M0–M7** — linear i64 paths, while invariants, bounds, element axioms,
  assume–guarantee recursion, `decreases` termination, integer tightening
- **M8–M12** — product symbols, sign table, const/var div–mod, floor mul,
  complete square, AM-GM identity, conclusiveness gate (no false alarms)
- **M13** — products inside `if`/`while` guards; sound bitwise envelope
  (`| & ^` neutrals, `n&1∈{0,1}`, `<<`/`>>` special cases)

### Concurrency & backends
- `!Par`: `go` / `join` / channels / select wait–timeout
- LLVM `!Par` parity via `libbaga_par.so`

### Docs
- `docs/theory-{en,bg}.md` — Fourier–Motzkin, Farkas, ℤ-tightening, M0–M13
- `docs/thesis-m13-nonlinear-fragment.md` — research note

### CLI
- `baga --version` / `-V` prints `baga 0.2.0`

## [0.1.0] — unreleased baseline

Bootstrap compiler, self-hosting, effects, specs runtime, std library, playground.
