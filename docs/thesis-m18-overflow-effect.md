# The Effect System and the Verifier Become One

**Research note (M18 culmination of the M0–M18 arc)**
*`!Overflow` as an effect dimension: arithmetic-safety analysis as effect
inference, declaration as discharge*

**Artifact:** the Baga compiler (`--verify` / `--proofs`), milestone M18
**Prior art in the same pipeline:** M0–M13 (nonlinear fragment),
M14 (fork–join), M15 (arithmetic safety), M16 (channel invariants),
M17 (pairs)
**Status:** implemented and regression-tested (`examples/verify/ovf_eff_*.baga`)

---

## Abstract

M15 closed the ℤ-vs-i64 gap by emitting one *arithmetic-safety obligation*
per operation and reporting, as prose, when all of them are proven: "N/N
операции доказано безопасни". The verdict was real but *extra-typological* —
it lived in the verifier's output, not in the program's types. This note
describes M18, which promotes that verdict into the type system by making
`!Overflow` a first-class **effect**. The move is small in code and large in
meaning: the M15 obligation machinery is reinterpreted as an **effect
inference** for `!Overflow`, and the existing one-way effect check (body ⊆
declared) becomes its **discharge**. A function that omits `!Overflow`
*claims* arithmetic safety and the verifier refutes the claim with a concrete
witness when it is false; a function that declares `!Overflow` *advertises*
the risk and is discharged — exactly as `!IO` is a permission, not a failure.
The result is the arc's culmination: the effect system (pillar 2,
"ефекти като измерения на типа") and the verifier (pillar 1, depth) are no
longer two features that happen to share a compiler — they are the same
judgement seen from two sides.

---

## 1. The gap after M15

M15 made overflow *visible*: every `+ - * -x / % <<` carries a kind-4
obligation, proven (cannot overflow on this path), refuted (a concrete
large-magnitude witness), or honestly UNKNOWN. When every obligation of a
function is proven, the verifier prints that the idealized-ℤ model and the
i64 runtime coincide. But three things remained unsatisfying:

1. **The verdict was prose, not a type.** Whether a function is overflow-safe
   was something you read off the report, not something you could see in its
   signature or propagate to its callers.
2. **No composition.** If `f` overflows and `g` calls `f`, the type of `g`
   said nothing about it. The effect system — which already tracks `!IO` and
   `!Par` through composition — was silent about the one dimension the
   verifier had actually reasoned about.
3. **The bridge theorem was informal.** M15 stated "if all obligations are
   proven, the two semantics agree" as a comment. Nothing in the language let
   you *name* that theorem, let alone have the compiler check that you had
   earned it.

M18 fixes all three with one design decision.

## 2. The observation: an effect is a permission, not a claim

The whole milestone rests on a re-reading of what an effect *is*. Consider
`!IO`. A function declared `-> str !IO` is not *violating* anything by doing
IO; the effect is a **permission** that the type system tracks and that
callers must handle. The compiler does not prove that an `!IO` function is
"IO-safe" — the declaration *is* the contract.

Overflow fits the same mould. A function that might overflow i64 is not
broken; it is *effectful* in the overflow dimension. So:

> **`!Overflow` is the permission to overflow. The verifier's arithmetic-
> safety analysis is the inference that discovers where the permission is
> needed; the effect check is the discharge that confirms it is declared.**

This is the precise sense in which the effect system and the verifier become
one. The M15 kind-4 obligations do not change at all — they are simply
reinterpreted as the *evidence* for an effect judgement, and the familiar
rule "every effect performed must be declared or caught" does the rest.

## 3. The mechanism

### 3.1 Syntax and propagation (free)

Effects in Baga are an open-ended list of names on the return type
(`include/baga.h`); `!Overflow` needs no lexer or parser change. Propagation
is the generic effect merge already used for `!IO`/`!Par`: a call to an
`!Overflow` function bubbles the effect into the caller, which must declare
it or catch it, on pain of the standard "необработен ефект !Overflow" error
(`src/checker.c`). No builtin *generates* `!Overflow` — it arises by
declaration and by propagation; the verifier, not the checker, discovers
*undeclared* overflow.

### 3.2 The gate admits `Overflow`

The verifier's fragment gate previously admitted only `Par`-effect functions
(M14). M18 generalizes it: a function whose declared effects are a subset of
`{Par, Overflow}` is analyzed; any other effect still skips honestly
(`ret_has_unverifiable_effects`, `src/verify.c`). This is essential — a
function must be *analyzed* for its `!Overflow` declaration, or its absence,
to mean anything.

### 3.3 Declaration is discharge

For an analyzed function the verifier computes, from the M15 obligations:

- `ovf_safe` — every kind-4 obligation is PROVEN (a function with no
  arithmetic is trivially safe);
- `ovf_declared` — the return type carries `!Overflow`;
- whether any obligation was REFUTED with a concrete witness, versus merely
  UNKNOWN.

The discharge table:

| safety | declared | verdict | fails? |
|---|---|---|---|
| safe | no | **clean** — the type is exact; `f_overflow_safe` is a theorem | no |
| safe | yes | redundant-but-honest note (over-declaration is always allowed) | no |
| overflows | yes | **discharged** — the overflow is reported as *evidence*, the type is honest, `ensures` are idealized-ℤ-only | **no** |
| overflows | no | **VIOLATION** — "прелива при ⟨witness⟩, а !Overflow не е деклариран" | **yes** |
| unknown | yes | discharged — honestly advertised | no |
| unknown | no | "безопасността не е доказуема — декларирай !Overflow" | no (UNKNOWN never fails) |

The single load-bearing line of the implementation is the gate on the exit
flag: a REFUTED arithmetic obligation fails verification **only when the
function does not declare `!Overflow`**. Declaring the effect discharges the
obligation — the permission has been asked for and granted. No existing
corpus example declares `!Overflow`, so every prior exit code is unchanged;
the rule is purely additive.

### 3.4 Skipped functions stay honest

A function the fragment gates skip (non-i64 result, an effect outside
`{Par, Overflow}`, loops without invariants, `match`/`for`/`try`) is never
analyzed and produces **no** overflow claim. "Never analyzed" is never
reported as "proven safe" — there is simply no `ефект !Overflow` line and no
`f_overflow_safe` theorem. Absence of `!Overflow` on a skipped function is
not a certificate.

## 4. The bridge theorem, now a type theorem

M15's informal bridge becomes a statement *about the type*:

**Theorem (overflow soundness).** Let `f` be a function `--verify` analyzes,
and suppose `f` carries no `!Overflow` in its type and verification does not
refute it. Then for every input satisfying `f`'s `requires`, no arithmetic
operation in `f` overflows i64 (and no division by zero or `INT64_MIN / -1`
occurs); consequently every idealized-ℤ `ensures` verdict holds on the real
machine, unconditionally.

*Sketch.* Absence of `!Overflow` plus no refutation means the discharge table
landed in the "clean" row, i.e. `ovf_safe`: every kind-4 obligation is PROVEN.
By M15 each operation is then provably within range (FIT `|L| ≤ 2^62`, MUL
`|a·b| ≤ 2^63−1`, DIVZ divisor non-zero and not the `MIN/-1` case). The new
content of M18 is not the arithmetic but the *certificate's location*: the
absence of an effect in the signature is the certificate, and the verifier is
what checks it. `--proofs` emits exactly this as `theorem f_overflow_safe`.

The contrapositive is the educational payoff: a function that overflows and
omits `!Overflow` is a **type error found by the verifier** — refuted with the
same concrete witness M15 already computed (`n = 9223372036854775807` for
`n + 1` under `n ≥ 0`).

## 5. Soundness and honesty

- **No new false proofs.** The only new REFUTED verdict is "refuted arithmetic
  + undeclared effect", backed by an M15 witness that already passed the M8
  conclusiveness gate. M18 adds no way to prove a false contract.
- **No new false alarms.** Discharging a declared overflow is sound because
  `!Overflow` asserts nothing — it advertises. The verifier still prints the
  overflowing operation as evidence; it simply stops treating an *advertised*
  risk as a *violation*.
- **UNKNOWN is preserved.** The `(2^62, 2^63)` band and havoced loop variables
  stay UNKNOWN; such functions ask for `!Overflow` and never fail verification
  whether or not it is given.
- **The runtime is untouched.** Effects are erased in both codegen backends
  (`NODE_TYPE_EFFECT` is transparently unwrapped). `!Overflow` changes the
  type and the verdict, not what the emitted C does on overflow (UB in theory,
  wrap in practice). `catch !Overflow` is compile-time effect removal, not a
  runtime trap. M18 is a *static discipline of visibility*; defining wrap or
  checked runtime semantics remains a separate language decision (M15 §6.3).

## 6. Evaluation

All claims machine-checked by `make test` (and the C≡LLVM oracle
`make test-llvm`; the self-hosting fixed point `make self` is unaffected —
`verify.c`/`proofs.c` are C-bootstrap-only):

| Example | Claim |
|---|---|
| `ovf_eff_safe` | `0≤n≤100 ⇒ n+1` safe, no `!Overflow` ⇒ "ефект !Overflow: безопасна — типът е точен" |
| `ovf_eff_refuted` | `n≥0`, `n+1`, no `!Overflow` ⇒ violation at `n = INT64_MAX`, **exit 1** |
| `ovf_eff_declared` | same body with `!Overflow` ⇒ discharged, evidence printed, **exit 0** |
| `ovf_eff_unknown` | havoced loop growth, no `!Overflow` ⇒ "декларирай !Overflow", exit 0 |
| `ovf_eff_redundant` | safe body with `!Overflow` ⇒ "деклариран, но … доказано безопасна" |
| `ovf_eff_skip` | `!IO` function ⇒ skipped, **no** overflow line |
| `ovf_eff_propagate` | caller omits `!Overflow` ⇒ checker error "необработен ефект !Overflow" |
| `ovf_eff_propagate_ok` | caller declares `!Overflow` ⇒ `--check` passes |
| `--verify --json` | `"overflow_effect": {analyzed, declared, safe, result, witness}` |
| `--proofs` | `theorem f_overflow_safe` with the discharge status |

## 7. Limitations

1. **Static only.** The runtime semantics of overflow is unchanged (§5).
   `!Overflow` makes the risk visible and checked, not handled at runtime.
2. **The extreme band stays UNKNOWN** (M15 §6.1): functions whose arithmetic
   lives in `(2^62, 2^63)` are "unknown — declare `!Overflow`", never falsely
   clean.
3. **Skipped functions make no claim** (§3.4); the certificate exists only
   inside the verifiable fragment.
4. **No effect polymorphism over overflow.** A higher-order combinator cannot
   yet abstract over "whatever overflow my argument does"; the effect is
   monomorphic, like the rest of Baga's effect system today.
5. **Overflow is one effect dimension.** The same discharge idea should apply
   to other machine-integers concerns (the shift-overflow sub-domain M13
   idealized, `f64` NaN/∞ if floats enter the fragment) — future work.

## 8. Positioning

| System | Overflow reasoning | Effect system | Coupling |
|---|---|---|---|
| Dafny / Boogie | bitvector via Z3 | none (modifies clause) | none |
| Frama-C + WP | via SMT | none | none |
| CBMC | bit-precise BMC | none | none |
| Koka / Eff | n/a | algebraic effects | no verifier |
| **Baga M18** | FM bound search (M15) | open-ended effect rows | **the verifier infers the effect; the effect check discharges it** |

SMT-based verifiers get overflow "for free" from bitvector theory and pay in
opacity; effect systems get composition and pay in having nothing to say about
arithmetic. Baga's contribution is the *identification*: the same linear core
that proves `ensures` also infers an effect, and the same effect discipline
that tracks `!IO` discharges it. The two pillars of the language were always
the same judgement; M18 makes the compiler agree.

## 9. Conclusion

M18 is the smallest milestone with the largest claim. Almost nothing new is
computed — the M15 obligations, the FM core, the effect merge, the
conclusiveness gate are all reused. What changes is the *typing judgement*
those results inhabit: overflow is no longer a verdict you read but an effect
you declare, propagate, and are checked for. A verified function without
`!Overflow` now carries a theorem in its type; a function that overflows
without declaring it is a type error with a counterexample. The effect system
and the verifier become one — which was, from the concept note's second
pillar, the point all along.

---

## Appendix A — How to reproduce

```bash
make
./baga --verify examples/verify/ovf_eff_safe.baga      # clean: типът е точен
./baga --verify examples/verify/ovf_eff_refuted.baga   # violation, exit 1
./baga --verify examples/verify/ovf_eff_declared.baga  # discharged, exit 0
./baga --verify --json examples/verify/ovf_eff_refuted.baga
./baga --proofs examples/verify/ovf_eff_safe.baga      # f_overflow_safe
make test && make self && make test-llvm
```

## Appendix B — Mapping to source

| Concept | Location |
|---------|----------|
| `!Overflow` propagation (generic effect merge) | `src/checker.c` (call-effect merge; no M18-specific change) |
| Fragment gate admits `{Par, Overflow}` | `ret_has_unverifiable_effects` (`src/verify.c`) |
| Effect-name test on return type | `ret_has_effect_named` |
| Discharge (effect inference from kind-4) | `verify_fn_collect` (kind-4 loop → `FnVerifyRes.ovf_*`) |
| Declaration discharges the exit flag | `verify_fn` reporting re-run (`r == R_REFUTED && !ovf_declared`) |
| Witness rendering | `witness_str` |
| `FnVerifyRes` fields | `include/baga.h` (`ovf_analyzed/declared/safe/res/witness`) |
| `f_overflow_safe` theorem | `src/proofs.c` (after the purity/effects theorem) |
| JSON field | `"overflow_effect"` in `verify_fn` |

---

*Baga project — research note (M18). Effect system and verifier as one judgement.*
