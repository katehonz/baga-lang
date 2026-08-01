# Design: M3 — Element Invariants for the Verifier

Date: 2026-08-01
Status: Implemented (M3)

## 1. Goal and non-goals

**Goal.** Let the static verifier (`--verify`) reason about **properties of all
elements** of a `Vec`/`bytes` — e.g. "every element is ≥ 0" — not just lengths
and bounds (M2). This is the heart of "spec-first": proving a function preserves
an element invariant, then using it at access sites.

**Non-goals (M3).** Full first-order quantifier elimination (the linear solver
stays quantifier-free); element invariants through `vec_set`/`vec_slice`/
`vec_concat` (only `vec_push` preservation in M3); nested/relational invariants
(`sorted`, `v[i] <= v[i+1]`); invariants over `bytes` elements (M3 is `Vec<i64>`
only). Anything outside the fragment → honest UNKNOWN, never a false PROVEN.

## 2. The reasoning problem (and the tractable core)

"∀i∈[0,len(v)): v[i] ≥ 0" is a quantified fact. The Fourier–Motzkin core is
quantifier-free, so M3 uses **axiom instantiation**: a quantified invariant is
stored as a *template* and instantiated at the **concrete** index of each
access, yielding a quantifier-free obligation the existing solver discharges.

Two proof directions:
- **Use (instantiation):** at `vec_get(v, k)` with known `0 ≤ k < len(v)`,
  instantiate `∀i: P(v[i])` at `k` ⇒ `P(v[k])`. Discharges an ensures about the
  read value.
- **Preservation (push):** `vec_push(v, e)` preserves `∀i: P(v[i])` if `P(e)`
  holds (new element satisfies P) and the old axiom held (old elements
  unchanged). The axiom's length bound extends to `len(v)+1`.

This covers the dominant pattern: *build a Vec by pushing values that satisfy P,
then read elements back and rely on P.*

## 3. Syntax

Element-wise invariant in `ensures` / `requires` / loop `invariant`, using `[*]`
to mean "every element":

```baga
spec nonneg_first {
    input:
        v: [i64]
    output: i64
    requires:
        vec_len(v) >= 1,
        v[*] >= 0          // every element of v is >= 0
    ensures:
        output >= 0
}

fn nonneg_first(v: [i64]) -> i64 {
    return vec_get(v, 0)   // instantiates v[*] >= 0 at index 0
}
```

`v[*] <cmp> <linear>` is the M3 form (one quantified var, linear predicate over
the element value). `[*]` is a new parser construct (index with `*`), desugared
to a quantified-constraint AST node the verifier interprets; other code ignores
it (it only appears in spec/annotation position).

## 4. Representation in the symbolic state

A new per-state list of **element axioms**:
```
ElemAxiom { vec: char* (name);  cmp: CmpOp;  rhs: Lin }   // ∀i∈[0,len(vec)): vec[i] <cmp> rhs
```
(`rhs` is a linear form over scalars in scope, e.g. `0`, or `k+1`.)

The symbolic state already tracks `vlen` (M2). The axiom's valid range is
`[0, vlen(vec))` implicitly.

## 5. Rules

- **`vec_new()`**: no axiom (empty; any `∀` over it is vacuous).
- **`requires v[*] >= c`** on a Vec param: add `ElemAxiom{v, GE, c}` to the
  initial state.
- **`vec_push(v, e)`**: for each axiom on `v`, check `P(e)` (substitute `e` for
  the element value, discharge with the solver under the current path). If
  proven, keep the axiom (its range auto-extends because `vlen(v)` grew via M2).
  If not proven, **drop** the axiom for `v` (sound: we no longer know it holds).
- **`vec_get(v, k)`**: the read value is a fresh symbolic `r`. For each axiom on
  `v`, if `k` is known in-range (M2 bounds), instantiate: assert `r <cmp> rhs`
  into the state (so a later `ensures r >= 0` follows). Record `r`'s constraint.
- **`vec_set`/`vec_slice`/`vec_concat`**: drop affected axioms (M3 does not
  model their element semantics) — sound over-approximation.

## 6. Discharging an ensures with instantiation

`ensures output >= 0` where `output = vec_get(v, 0)`:
1. `output` is the fresh `r` from the `vec_get`; the axiom instantiation already
   asserted `r >= 0` into the state.
2. The ensures obligation `path ⇒ output >= 0` becomes `path ∧ (r >= 0) ⇒ r >= 0`
   — trivially UNSAT-to-negate ⇒ PROVEN.

For a param access `vec_get(v, k)` with `requires v[*] >= 0` and `requires
vec_len(v) > k`: instantiate at `k` (range proven from requires) ⇒ `r >= 0`.

## 7. Implementation surface

- **Parser (`src/parser.c`)**: parse `v[*]` in spec/annotation expressions → a
  quantified-constraint node (e.g. reuse `NODE_INDEX` with a `*` marker, or a
  small new node). Only valid in `requires`/`ensures`/`invariant`.
- **Checker (`src/checker.c`)**: accept `v[*] <cmp> <expr>` in annotations;
  type-check the element predicate (element is i64 for `Vec<i64>`).
- **Verifier (`src/verify.c`)**:
  - `ElemAxiom` list in `State` (+ clone/free).
  - Parse element axioms from `requires` (initial state) and loop `invariant`.
  - `scan_vec_expr`: apply the push/get/set rules above.
  - Instantiation feeds the read value's constraint into the obligation's
    antecedent so the existing solver discharges the ensures.
- **No codegen change** (annotations are verifier-only; `[*]` never reaches
  codegen because spec bodies aren't emitted).

## 8. Testing

- `examples/verify/elem_param.baga`: `requires v[*] >= 0, vec_len(v) >= 1` ⇒
  `vec_get(v, 0)` proves `ensures output >= 0` (instantiation on a param).
- `examples/verify/elem_push.baga`: build a Vec by pushing non-negative values,
  read one back, prove `>= 0` (push preservation + instantiation).
- `examples/verify/elem_bad.baga`: push a possibly-negative value, then claim
  `output >= 0` ⇒ **not PROVEN** (UNKNOWN or REFUTED) — soundness.
- Wired into `make test`; `make self` unaffected (annotations don't reach the
  self-compiler's codegen).

## 9. Honesty / risk

M3 is **quantifier-free reasoning about quantified facts** via instantiation at
concrete indices — sound but incomplete. Invariants through `vec_set`/`slice`/
`concat`, relational invariants (`sorted`), and bytes elements are deferred
(M4+). The risk is the parser/checker plumbing for `[*]` in annotations and the
axiom bookkeeping in the symbolic state; each rule is conservative (drop the
axiom when unsure) so soundness is preserved even where completeness is not.
