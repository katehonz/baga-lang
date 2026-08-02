# Design: M18 — `!Overflow` as an Effect (the effect system ≡ the verifier)

Date: 2026-08-02
Status: Draft

## 1. Goal

Promote arithmetic safety from a *prose summary* into a *type-level fact*.
Today M15 emits one kind-4 obligation per arithmetic operation and, when all
are proven, prints "N/N операции доказано безопасни" — but nothing carries
that verdict into the type world. M18 makes `!Overflow` a first-class effect
dimension so that:

- a function's **type tells the truth** about overflow (when verified): no
  `!Overflow` in the signature ⇔ the verifier proved every arithmetic
  operation stays inside i64;
- the **verifier becomes the authority** on whether `!Overflow` is required —
  the same one-way discipline as every other effect (body ⊆ declared), except
  the "body performs `!Overflow`" fact is established by the M15 machinery,
  not by a builtin table;
- the effect system and the verifier **become one**: presence/absence of
  `!Overflow` in the type is exactly the verifier's arithmetic-safety verdict.

This is the dissertation's culmination: pillar 2 ("ефекти като измерения на
типа") meets pillar 1 depth (static verification). No new decision procedure —
M18 reuses the M15 kind-4 obligations and the FM core wholesale.

## 2. Current state (from code map)

- Effects are an **open-ended string list on `Type`** (`char **effects;
  int n_effects`, `include/baga.h:399`), declared only on return types via
  `NODE_TYPE_EFFECT` (parser.c:241 `parse_type_with_effects`), checked
  one-way (body ⊆ declared) in `check_fn` (checker.c:989). The compiler today
  only ever *generates* `"IO"` and `"Par"`; any other name is user-declared.
  **Adding `!Overflow` syntactically is free** — no lexer/parser change.
- The verifier already computes the M18 predicate — `naproven == natotal`
  over kind-4 obligations — but only as a text/JSON summary in the reporting
  re-run (`verify.c:4082–4123`), after a throwaway second symexec. Nothing
  stores it on the function or propagates it.
- The only effect↔verifier coupling is the Par gate `ret_has_non_par_effects`
  (`verify.c:3160`), used to skip whole functions (3782) and go-workers
  (1487). A function declaring any non-`Par` effect is skipped honestly.
- Effects are **erased in both codegen backends** (`codegen_c.c:120`,
  `codegen_llvm.c:102`); `NODE_TRY`/`NODE_CATCH` emit their inner expression.
  A verifier/type-level effect needs **no codegen**.
- **Caveat:** `scan_arith_expr` runs only during symexec of *non-skipped*
  functions. Skipped functions produce zero arith obligations, so "no
  `!Overflow`" and "never analyzed" are currently indistinguishable. M18 must
  not read absence as proven-safe for skipped functions.

## 3. The `!Overflow` effect — semantics

`!Overflow` is a new effect name in the existing open-ended string mechanism.

```baga
fn inc(n: i64) -> i64 !Overflow {   // "I may overflow i64"
    return n + 1
}
```

### 3.1 Checker layer (always on, no `--verify` needed)

`!Overflow` propagates exactly like `!IO`/`!Par`:

- A call to an `!Overflow` function merges `"Overflow"` into the caller's
  `cur_effects`; the caller must declare it or catch it (`?` / `catch
  !Overflow => ...`), else the existing "необработен ефект" error fires
  (checker.c:989).
- `go(worker, ...)` bubbles the worker's `Overflow` to the spawn site like its
  other return effects (checker.c:505).
- `catch !Overflow => e` removes the dimension (compile-time only, as today —
  codegen erases it). **M18 does not change runtime overflow semantics**: the
  emitted C still does whatever it does (UB in theory, wrap in practice).
  `!Overflow` is a *static discipline* — visibility in the type + verifier
  discharge. Defining wrap/checked runtime semantics is a separate language
  decision (M15 §6.3).

Without `--verify`, the checker cannot itself prove arithmetic safe, so it
cannot *require* `!Overflow` on an undeclared function — it only enforces
propagation of *declared* effects. Necessity is the verifier's job.

### 3.2 Verifier layer (`--verify`) — the discharge

For every **analyzed** function (not skipped by the fragment gates), after the
M15 arith verdicts are computed, M18 adds one **effect-discharge check**:

Let
- `ovf_declared` = the return type declares `Overflow`;
- `natotal` / `naproven` = M15 kind-4 counts (a function with no arithmetic
  has `natotal == 0` and is trivially safe);
- `ovf_safe` = (`natotal == 0`) || (`naproven == natotal`);
- `ovf_refuted` = some kind-4 obligation is REFUTED (concrete witness).

**Declaration is discharge.** `!Overflow` is not a correctness claim — it is a
permission, exactly like `!IO`. Declaring it does not *prevent* overflow; it
makes overflow *typed*. So a declared `!Overflow` **discharges** the overflow
obligation: the verifier still reports the overflowing operation as evidence,
but it is no longer a contract violation, and it does not fail verification.
An *undeclared* overflow is the violation — the function claimed safety (by
omitting the effect) and the verifier refutes that claim. This is the precise
sense in which the effect system and the verifier become one: the M15 arith
machinery is the *effect inference* for `!Overflow`, and the effect check is
the discharge.

| `ovf_safe` | `ovf_declared` | Verdict | exit flag |
|---|---|---|---|
| true | no | **clean** — type tells the truth; theorem `f_overflow_safe` ДОКАЗАНО | — |
| true | yes | redundant-but-honest note ("деклариран, но доказано безопасна"); allowed, like over-declaring any effect | — |
| false (refuted) | yes | **discharged** — "деклариран — прелива при <witness>; типът е честен". Arith line prints as evidence; the `ensures` verdicts are idealized-ℤ-only (M15 caveat). | not set |
| false (refuted) | no | **VIOLATION** — "прелива при <witness>, а !Overflow не е деклариран". | **set** (refuted) |
| false (unknown only) | yes | discharged — "деклариран — безопасността не е доказуема"; honest | not set |
| false (unknown only) | no | **НЕ МОГА ДА РЕША** — "безопасността не е доказуема — декларирай !Overflow". | not set (UNKNOWN never fails) |

Concretely, the M15 rule "a REFUTED arith obligation sets the exit flag"
becomes conditional: it sets the flag **only when the function does not
declare `!Overflow`**. No existing corpus example declares `!Overflow`, so all
current exit codes are unchanged — the gate is purely additive.

### 3.3 The gate admits `Overflow`

`ret_has_non_par_effects` is generalized to "has an effect that is neither
`Par` nor `Overflow`". Functions whose declared effects are a subset of
`{Par, Overflow}` stay inside the verifiable fragment; `IO` etc. still skip
honestly. This is essential: a function must be *analyzed* for its
`!Overflow` declaration (or its absence) to mean anything. The same whitelist
applies to `worker_sig_supported` (go-workers may now be `!Par !Overflow`).

### 3.4 Skipped functions (honesty policy)

For a function the fragment gates skip (non-i64 result, non-Par/non-Overflow
effects, loops without invariant, `match`/`for`/`try`, …) M18 reports
`ovf_analyzed = 0` and makes **no** overflow-safety claim — its `!Overflow`
declaration (or absence) is unchecked, exactly like its `ensures` today.
"Never analyzed" is never reported as "proven safe".

## 4. The bridge theorem (what M18 makes statable)

**Theorem (overflow soundness, per function).** If `--verify` analyzes a
function `f`, reports `f_overflow_safe` ДОКАЗАНО, and `f` carries no
`!Overflow` in its type, then for every input satisfying `requires`, no
arithmetic operation in `f` overflows i64 (and no division by zero /
`INT64_MIN / -1` occurs). Consequently the idealized-ℤ `ensures` verdicts
hold on the real machine — they are unconditional.

*Sketch.* Direct from M15: `ovf_safe` means every kind-4 obligation is PROVEN,
i.e. each operation's result is provably `|L| ≤ 2^62` (FIT), `|a·b| ≤ 2^63−1`
(MUL), or divisor provably non-zero and not the `INT64_MIN/-1` case (DIVZ).
M18's contribution is not new math but *surfacing this as the type*: the
absence of `!Overflow` is the certificate, the verifier is the checker of that
certificate, and an undeclared function that overflows is a *type error found
by the verifier*.

## 5. Target output

### 5.1 Text (`--verify`)

After the existing `аритметика: N/M операции доказано безопасни` line, M18
prints an effect-discharge line for analyzed functions:

```
inc_bounded:
  ensures #1 (output >= 1): ДОКАЗАНО
  (аритметика: 1/1 операции доказано безопасни)
  ефект !Overflow: доказано безопасна — типът е точен
```

```
inc_unbounded:
  ensures #1 (output >= 1): ДОКАЗАНО (в идеализирания ℤ модел)
  аритметика (n + 1): ОБРОЧЕНО, контрапример: n = 9223372036854775807
  (аритметика: 0/1 операции доказано безопасни — вердиктите за ensures са в идеализирания ℤ модел)
  ефект !Overflow: ОБРОЧЕНО — прелива при n = 9223372036854775807, но !Overflow не е деклариран
```

```
mystery:
  ensures #1 (...): НЕ МОГА ДА РЕША
  (аритметика: 0/1 операции доказано безопасни — ...)
  ефект !Overflow: НЕ МОГА ДА РЕША — декларирай !Overflow, за да е честен типът
```

Skipped functions print no `ефект !Overflow` line (unchanged skip reason).

### 5.2 JSON (`--verify --json`)

New per-function field alongside `"arith"`:

```json
"overflow_effect": {
  "analyzed": true,
  "declared": false,
  "safe": false,
  "result": "refuted",
  "witness": "n = 9223372036854775807"
}
```

`result` ∈ {`proven`, `refuted`, `unknown`, `skipped`} via the existing
`res_word_json`. For skipped functions: `{"analyzed": false, "result":
"skipped"}`.

### 5.3 Proofs (`--proofs`)

A new theorem for functions with a spec (or always, mirroring purity):

```
  theorem inc_bounded_overflow_safe:
    ∀ inputs. no arithmetic in inc_bounded overflows i64
    status: ДОКАЗАНО (M18 — всички аритметични задължения)
```

REFUTED carries the witness; UNKNOWN reports honestly.

## 6. Interface

`include/baga.h` — extend `FnVerifyRes` (no new obligation kind needed; the
check is a post-pass over the already-computed arith verdicts):

```c
typedef struct {
    /* ... existing fields ... */
    int ovf_analyzed;   /* 1 if the function was analyzed (not skipped) */
    int ovf_declared;   /* 1 if return type declares Overflow */
    int ovf_safe;       /* 1 if all kind-4 obligations proven (or none) */
    int ovf_res;        /* 0=PROVEN,1=REFUTED,2=UNKNOWN,3=SKIPPED */
    char *ovf_witness;  /* counterexample string for REFUTED (owned/NULL) */
} FnVerifyRes;
```

`verify.c`:
- `ret_has_non_par_effects` → `ret_has_unverifiable_effects` (admit `Par` and
  `Overflow`); update its two call sites (3782 gate, 1487 worker) and
  `callee_sig_supported`/`worker_sig_supported` accordingly.
- In the reporting re-run, after the arith summary (4118–4123), compute the
  discharge table (§3.2), fill the new `FnVerifyRes` fields, print the text
  line, and emit the JSON field. Set `any_refuted` only for the
  refuted-and-undeclared case.
- `verify_fn_collect` fills the same fields so `proofs.c` can print the
  theorem.

`checker.c`:
- `go`/`pool_map` effect bubbling already merges callee return effects
  generically; confirm `"Overflow"` flows (no special case expected). No
  builtin *generates* `Overflow` — it arises only by declaration and by
  propagation from declared functions. (The verifier, not the checker,
  discovers undeclared overflow.)

`proofs.c`:
- Emit `f_overflow_safe` theorem from the new `FnVerifyRes` fields.

No lexer/parser/codegen changes.

## 7. Implementation order (test-first; detail in the plan)

1. Gate rename + admit `Overflow` (no behavior change on existing corpus —
   no example declares `!Overflow` yet). `make test` green.
2. `FnVerifyRes` fields + reporting text/JSON + proofs theorem, driven by new
   examples `examples/verify/ovf_eff_{safe,refuted,unknown,declared,skip}.baga`.
3. Checker propagation of a *declared* `!Overflow` (call → caller must
   declare/catch), example `ovf_eff_propagate.baga`.
4. `make test` (C≡LLVM oracle), `make self` (fixed point), baga≡baga2 on
   examples. **Risk:** changes to `checker.c` effect propagation may need to
   be mirrored in the self-hosted checker to keep `make self` green — verify
   early; `verify.c`/`proofs.c` are C-bootstrap-only and do not affect baga2.

## 8. Honesty & limitations

- **Static only.** `!Overflow` changes the type and the verifier verdict, not
  the runtime. Overflow still does whatever the emitted C does. `catch
  !Overflow` is compile-time effect removal, not a runtime trap.
- **The (2^62, 2^63) band** stays UNKNOWN (M15 §6.1) → such functions are
  "unknown, declare `!Overflow`", never falsely clean.
- **Skipped functions** make no claim (§3.4). Absence of `!Overflow` on a
  skipped function is *not* a certificate.
- **Over-declaration is allowed** (declaring `!Overflow` on a provably-safe
  function), matching the existing effect discipline — the type is a sound
  over-approximation, and the verifier confirms when it is exact.
- No new false alarms: the only new REFUTED is "refuted arith + undeclared
  effect", which is a genuine soundness statement already backed by an M15
  witness and the M8 conclusiveness gate.

## 9. Regression-safety note

Verify output changes are **additive** (a new text line + JSON field). The
`make test` oracle compares the two backends *running* compiled programs
(C≡LLVM), not golden `--verify` text; `--verify` output is backend-independent
(verify.c is shared). Confirm the test harness has no golden `--verify`
snapshots before relying on this. Exit codes change only where a function is
already REFUTED.
