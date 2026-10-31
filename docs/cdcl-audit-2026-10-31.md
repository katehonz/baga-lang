# CDCL Soundness Audit — 2026-10-31 (M32)

**Scope:** `src/verify_bv_sat.c` (DPLL+CDCL solver), `src/verify_bv.c` (caller), `src/verify_impl.h` (BvSat struct).
**Result:** CDCL enabled (`sat.cdcl = 1`), 138/138 verify gates pass. Root cause found and fixed.

## Root cause of false UNSAT (false ДОКАЗАНО)

**Location:** `bv_sat_solve()`, CDCL conflict-clause search (old lines 316–325).

```c
/* WRONG: "no true literal" ≠ "conflict" */
for (int ci = 0; ci < s->n_cl; ci++) {
    int sat = 0;
    for (int i = 0; i < c->n; i++)
        if (lit_value(s, c->lits[i]) > 0) { sat = 1; break; }
    if (!sat) { conf_cl = ci; break; }   /* BUG */
}
```

`lit_value()` returns **0 for unassigned**, not just for false. A clause with some
unassigned and some false literals is **unit or unassigned, not a conflict** — only a
clause with **all literals false (< 0)** is a conflict. The old scan picked the first
clause with no *true* literal, which includes unit clauses mid-propagation. Feeding a
non-conflict clause into `cdcl_analyze` produced learned clauses that were not logical
consequences of the input — pruning valid branches → false UNSAT → false ДОКАЗАНО.

**Symptom observed:** `lshr_var_bad` (`x>>>k >= 0`, false at k&63=0) and
`distrib_honest` became ДОКАЗАНО under CDCL.

## Fix

1. `sat_propagate` now records the actual conflict clause index in `s->conflict_cl`
   at the exact point of conflict (`unassigned == 0` — all literals false):
   ```c
   if (unassigned == 0) { s->conflict_cl = ci; return 1; }
   ```
2. The CDCL branch in `bv_sat_solve` uses `s->conflict_cl` directly instead of scanning.
3. Tautology guard in `cdcl_analyze` (learned clause must not contain both `v` and `-v`).

## Audit of `cdcl_analyze` (MiniSat-style 1-UIP) — verified correct

| Check | Verdict |
|-------|---------|
| seen[] per-variable, cleared after analysis | correct |
| Current-level literals counted, not added to learn_buf | correct (`level[v] >= cur_level → counter++`) |
| Lower-level literals added to learn_buf, backjump = max their level | correct |
| Trail walk backward picks last-assigned seen current-level var | correct |
| `q == p` skip in reason clause (resolvent eliminates the pivot) | correct |
| `seen[pv] = 0; counter--` before processing reason | correct |
| `learn_buf[0] = -p` (asserting literal = ¬UIP) | correct |
| Unit learned clause (n_learn==1) → backjump level 0 | correct |
| UIP pushed via `trail_push_reason` at backjump level | correct |
| `cl` pointer stability during analysis loop (no realloc inside loop) | correct |
| Level-0 assignments (level[v]==0) excluded from analysis | correct |

## Soundness argument (sketch)

Each learned clause is produced by resolution of the conflict clause with reason
clauses along the trail. Resolution is sound: if C1 ∨ ... ∨ Cn are all implied by the
original formula, any resolvent is also implied. By induction on the number of
resolution steps, the learned clause is a logical consequence of the input CNF.
Therefore adding it never eliminates satisfying assignments → UNSAT verdicts remain
sound. SAT verdicts are witnessed by a full model (unchanged path).

## Tripwires in run_verify.sh

- `xor_nonneg_bad`, `lshr_var_bad`, `var_mask_bad` must stay **ОБРОЧЕНО** (SAT witnesses
  found — the negation is satisfiable). A false ДОКАЗАНО on any *_bad case fails the suite.
- `distrib_honest` (`(x&y)|(x&~y) == x`) is a **true** identity — under CDCL it is now
  correctly ДОКАЗАНО (the old gate expected UNKNOWN because DPLL exhausted its budget;
  that was incompleteness, not unsoundness).

## Residual risks

- Clause deletion is not implemented (learned clauses accumulate). Budget caps total
  conflicts, so memory is bounded by `budget × avg_clause_size`. For the current
  bit-blasted formulas (≤ a few thousand vars), this is fine.
- The `reason` array stores clause indices (+1); clause indices are stable because
  `s->cl` only grows (never shrinks/deletes), so realloc doesn't invalidate indices.
  Pointers into `s->cl` are only held within single function scopes (no cross-call
  dangling).
