# M3 Element Invariants — Implementation Plan

**Goal:** `--verify` reasons about element invariants `v[*] >= c` (every element
of `v` satisfies a linear predicate) via axiom instantiation at concrete access
indices — proving ensures about read values, and preserving the invariant
through `vec_push`. Sound (drop the axiom when unsure), incomplete (no full
quantifiers). `Vec<i64>` only.

**Spec:** `docs/superpowers/specs/2026-08-01-element-invariants-design.md`

## Architecture (the concrete mechanism)

- `v[*]` parses to a new `NODE_ELEM_REF` (obj = the vec expr). It only appears
  in spec/annotation position; codegen never sees it.
- The checker types `NODE_ELEM_REF` as the element type (i64 for `Vec<i64>`).
- The verifier reads element axioms out of `requires`/loop-`invariant`
  (constraints of the shape `NODE_ELEM_REF <cmp> <linear>`), stores them per
  state as `ElemAxiom { vec, cmp, rhs: Lin }`.
- `vec_get(v, k)` produces a **fresh symbolic** read value `__rN` (a fresh var
  in the env) and, for each axiom on `v` with `k` in range, records an
  **instantiated constraint** `__rN <cmp> rhs` on the state.
- An obligation carries those read-constraints; `verify_ensures` adds them to
  the antecedent, so `ensures output >= 0` with `output = __rN` and a recorded
  `__rN >= 0` discharges to PROVEN.
- `vec_push(v, e)`: for each axiom on `v`, check `P(e)` (substitute `e`'s sym
  for the element); keep the axiom only if proven (range auto-extends via M2
  vlen). `vec_set/slice/concat`: drop axioms for the affected vec (sound).

## Tasks

### Task 1 — AST + parser: `v[*]`
- `include/baga.h`: add `NODE_ELEM_REF` to NodeKind (reuses `obj` field via a
  new `struct { Node *elem_obj; }` union member, or reuse `index_obj`).
- `src/parser.c` `parse_postfix` index branch: if the token after `[` is
  `TOK_STAR`, consume `*` `]` → `NODE_ELEM_REF` with `elem_obj = e`; else parse
  the index expression as today.
- `node_free` + `print_ast` cases for `NODE_ELEM_REF`.
- **Verify:** `./baga --ast` on a spec with `v[*] >= 0` shows the node; no
  parse error.

### Task 2 — Checker: type `NODE_ELEM_REF`
- `src/checker.c` `infer`: `NODE_ELEM_REF` → infer obj, return element type
  (i64 for `Vec<i64>`/`[i64]`; mirror `NODE_INDEX`'s i64 for now).
- **Verify:** a spec with `requires v[*] >= 0` type-checks (no error).

### Task 3 — Verifier: ElemAxiom storage + parse from requires
- `src/verify.c`: `ElemAxiom { char *vec; COp cmp; Lin rhs; }`; add
  `ElemAxiom *ax; int n_ax, cap_ax;` to `State`; clone/free in
  `clone_state_with`/`state_free`.
- A detector `is_elem_constraint(Node *cmp_expr, ...)` that recognizes
  `NODE_ELEM_REF <cmp> <linear>` (and its mirror `<linear> <cmp> NODE_ELEM_REF`)
  and extracts `{vec_name, cmp, rhs Lin}`.
- When building the initial state, scan `spec->spec_requires` for element
  axioms and add them (alongside the existing scalar requires → path).
- **Verify:** unit-style: a function with `requires v[*] >= 0` has the axiom in
  its initial state (temporary debug print).

### Task 4 — Verifier: vec_get instantiation + read constraints
- `scan_vec_expr` `vec_get(v, k)`: create fresh var `__rN`, bind it in the env
  as the call's value; for each axiom on `v`, if `k` is in range (M2), record an
  instantiated constraint `__rN <cmp> rhs` in a new per-state
  `read_cons` list.
- Extend `Obligation` with `ConsList read_cons`; `drain_returns` copies the
  state's `read_cons` into the obligation.
- Make `se_from_ast(NODE_CALL vec_get)` return the fresh `__rN` var (so the
  return value is the symbolic read).
- **Verify:** `vec_get(v, 0)` with axiom `v[*] >= 0` records `__r0 >= 0`.

### Task 5 — Verifier: use read constraints in ensures + push preservation
- `verify_ensures`: add `obl->read_cons` to the antecedent before discharging.
- `scan_vec_expr` `vec_push(v, e)`: for each axiom on `v`, check `P(e)`
  (substitute `e`'s sym into `rhs` comparison) under the current path; drop the
  axiom if not proven. `vec_set/slice/concat`: drop axioms for that vec.
- **Verify:** `elem_param` and `elem_push` prove; `elem_bad` does not.

### Task 6 — Examples + make test + make self
- `examples/verify/elem_param.baga`: `requires v[*] >= 0, vec_len(v) >= 1` ⇒
  `return vec_get(v, 0)` proves `ensures output >= 0`.
- `examples/verify/elem_push.baga`: push non-negative values, read back, prove.
- `examples/verify/elem_bad.baga`: push a possibly-negative value, claim
  `output >= 0` ⇒ NOT proven (soundness).
- Wire all three into `make test` (PROVEN/PROVEN/not-PROVEN).
- **Verify:** `make test` green; `make self` green (annotations don't reach the
  self-compiler's codegen).

## Honesty guardrails
- Every axiom rule is conservative: if `P(e)` or the range can't be proven, the
  axiom is dropped (→ UNKNOWN downstream), never kept falsely.
- `[*]` is annotation-only; if it ever reaches codegen it's a bug (the checker
  allows it only in spec position — verify this doesn't leak into fn bodies).
