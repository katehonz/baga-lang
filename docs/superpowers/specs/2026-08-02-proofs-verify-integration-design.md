# Design: M4 — Proof Extraction + Static Verification Integration

Date: 2026-08-02
Status: Draft

## 1. Goal

Connect the static verifier (`--verify`, verify.c) with proof extraction
(`--proofs`, proofs.c) so that verified contracts appear as **theorems with
proof status** in the extracted proof document — not just "RUNTIME-CHECKED".

The three pillars close: specs are written (pillar 1), verified statically
(pillar 1 depth), and the result is an extracted readable theorem (pillar 3).

## 2. Current state

- `--proofs` prints: signature theorem, termination theorem, purity/effects
  theorem, and spec guarantees with `status: RUNTIME-CHECKED`.
- `--verify` prints per-ensures: ДОКАЗАНО / ОБРОЧЕНО (+ counterexample) /
  НЕ МОГА ДА РЕША / ПРОПУСНАТО (reason).
- The two never talk to each other.

## 3. Target output

```
proofs for first:
  theorem first_signature:
    ∀ v: [i64]. first(v) → i64

  theorem first_terminates:
    ∀ v: [i64]. terminates(first(v))
    evidence: 1 return path(s)

  theorem first_pure:
    first is pure (no declared effects)

  theorem first_ensures_1:
    requires: v[*] >= 0, vec_len(v) >= 1
    ensures: output >= 0
    status: ДОКАЗАНО (статично, Fourier–Motzkin)
```

For REFUTED:
```
  theorem bad_abs_ensures_1:
    ensures: output >= 0
    status: ОБРОЧЕНО
    контрапример: x = -5 → output = -5
```

For UNKNOWN/SKIPPED:
```
  theorem nonlinear_ensures_1:
    ensures: output >= 0
    status: НЕ МОГА ДА РЕША (нелинейна аритметика)
```

Functions without a spec keep the current output (signature/termination/purity
only).

## 4. Interface

New in `include/baga.h`:
```c
typedef struct {
    int res;            /* 0=PROVEN, 1=REFUTED, 2=UNKNOWN, 3=SKIPPED */
    char *ens_text;     /* the ensures clause text (borrowed) */
    char *skip_reason;  /* if SKIPPED (borrowed or NULL) */
    char **wit_names;   /* counterexample variable names (owned, may be NULL) */
    long long *wit_vals;/* counterexample values */
    int wn;             /* number of witness bindings */
} EnsVerifyRes;

typedef struct {
    EnsVerifyRes *ens;  /* per-ensures results */
    int n_ens;
    int skipped;        /* 1 if the whole function was skipped */
    char *skip_reason;  /* why skipped (borrowed) */
} FnVerifyRes;

/* Collect verification results without printing. Returns 0 on success. */
int verify_fn_collect(Node *prog, Node *fn, FnVerifyRes *out);
void fn_verify_res_free(FnVerifyRes *r);
```

## 5. Implementation

- **verify.c**: refactor `verify_fn` → extract the computation into
  `verify_fn_collect` (fills the struct); `verify_fn` becomes a thin
  print-over-results wrapper. No logic change.
- **proofs.c**: in the spec section, call `verify_fn_collect`; format each
  ensures as a theorem with the verification status. Replace the old
  "RUNTIME-CHECKED" block.
- **No new CLI flags** — `--proofs` now automatically includes verification
  results when a spec is present.

## 6. Testing

- `./baga --proofs examples/verify/abs_val.baga` shows ДОКАЗАНО for ensures #1.
- `./baga --proofs examples/verify/bad_abs.baga` shows ОБРОЧЕНО + counterexample.
- `./baga --proofs examples/verify/nonlinear.baga` shows НЕ МОГА ДА РЕША.
- `make test` green (existing tests unaffected).
- `make self` green (proofs.c is C-bootstrap-only).

## 7. Honesty

The proof extraction remains readable text, not formal proof objects. The
status string is the verifier's verdict — no inflation. Functions outside the
fragment honestly report SKIPPED/UNKNOWN.
