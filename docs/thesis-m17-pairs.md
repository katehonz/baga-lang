# A Pair Abstraction for a Linear Verifier

**Working title (PhD-scale research note, slice 4)**
*Exact rewrites for `cell2`, status ranges for channel pair APIs,
and contracts over packed worker arguments*

**Artifact:** the Baga compiler (`--verify`), milestone M17
**Prior art in the same pipeline:** M0–M13, M14 (fork–join), M15 (arithmetic
safety), M16 (channel content invariants)
**Status:** implemented and regression-tested (`examples/verify/pair_*.baga`)

---

## Abstract

Baga's channels report composite results — `(ok, value)`, `(status, value)`,
`(which, value)` — packed into a single `i64` by the pure `cell2` builtins.
M14 deferred them: a pair is opaque to a linear verifier. This note closes
the deferral with almost no machinery: pairs ride the same symbolic-entry
infrastructure as products (M8). `cell2_0(cell2(a,b)) = a` is an exact
rewrite; the pair-returning channel APIs produce fresh status components
with tight range constraints and value components carrying the M16 content
axioms; and `go(worker, cell2(a, b))` makes contracts about packed arguments
dischargeable at the spawn site, where the components are visible.

---

## 1. The observation

A pair is not a value to reason *about* — it is two values with a name.
Once the verifier keeps, for each fresh pair symbol, its two component
linear forms (the same `ReadEntry` record M8 uses for product factors),
every projection is a lookup, not a theory:

- `cell2(a, b)` allocates `__qK` with components `(a, b)`;
- `cell2_0(p)` / `cell2_1(p)` resolve through the table when `p` is a bare
  symbolic var, else stay honestly opaque.

Because the table threads through `se_from_ast` and `bool_to_dnf`,
projections work *inside conditions* — the idiomatic
`if cell2_0(r) == 1 { ... }` becomes an ordinary path fork.

## 2. The channel pair APIs

Each pair-returning builtin yields `(status, value)`:

| API | status fact | value fact |
|---|---|---|
| `chan_recv2(c)` | `0 <= ok <= 1` | M16 content axioms of `c` |
| `chan_try_recv(c)` | `0 <= s <= 2` | M16 content axioms of `c` |
| `chan_recv_timeout(c, ms)` | `0 <= s <= 2` | M16 content axioms of `c` |
| `chan_select2(c0, c1)` | `0 <= w <= 3` | only axioms **both** channels share |
| `chan_select2_wait` | `[0,3]` over-approx of `{0,1,3}` | same |
| `chan_select2_timeout` | `0 <= w <= 3` (exact) | same |

The select rule is the honest one: the value may come from either channel,
so only a predicate present on both (same direction, structurally equal
bound) is a consequence. `select2_wait`'s `which ∈ {0,1,3}` is modeled as
the interval `[0,3]`; the status is an abstract `__c` var, so the M8
conclusiveness gate refuses any refutation that would need the impossible
`w = 2`.

**The ok-flag pattern, made expressible.** M16 documented that `chan_recv`
returns `0` on closed+empty, colliding with payload `0`. With pairs the
discipline is code, not convention:

```baga
let r = chan_recv2(c)
if cell2_0(r) == 1 {
    return cell2_1(r)   // a real payload: >= 1 by the channel invariant
}
return 1                // closed+empty fallback
```

— proven (`examples/verify/pair_recv2.baga`).

## 3. Contracts over packed arguments

`go` takes one `i64` argument; `cell2` is the runtime's packing idiom. With
the pair table, a worker spec may speak about the components —

```baga
spec worker {
    input: p: i64
    output: i64
    requires: cell2_1(p) >= 1
    ensures: output >= 1
}
```

— and at `go(worker, cell2(c, n))` the spawn site's pair entry makes
`cell2_1(p)` evaluate to `n`, so the requires discharges as an ordinary
obligation (`n >= 1`, ДОКАЗАНО). Inside the worker the packed parameter
stays honestly opaque (the worker's own verdicts reflect that; the spawn's
discharge is the checked boundary). `examples/verify/pair_go.baga`.

## 4. Evaluation

| Example | Claim |
|---|---|
| `pair_recv2.baga` | ok-flag branch + content invariant ⇒ ensures ДОКАЗАНО |
| `pair_select.baga` | `which ∈ [0,3]` proven; `which <= 2` honestly UNKNOWN (abstract status, M8 gate) |
| `pair_go.baga` | `requires cell2_1(p) >= 1` discharged at spawn; join fact ДОКАЗАНО |

All machine-checked by `make test`.

## 5. Limitations

- Pair components that are themselves nonlinear stay opaque (no nested
  pairs-of-pairs reasoning).
- Packed channels in `requires c[*]` form remain out (a channel per argument
  works; `cell2_0(p)[*]` does not).
- The closed+empty caveat of M16 stands for the value component; the ok-flag
  pattern is the sanctioned way around it.

## 6. Positioning

Product types in verifiers usually mean either full algebraic datatypes
(heavy) or uninterpreted projections (weak). The middle path here — named
pairs with remembered components, plugged into an existing symbolic-entry
table — gives exact projections, tight status ranges, and spawn-site
contract checking for the price of one struct flag. It also completes the
deferred lists of both concurrency milestones: M14's pair builtins and
M16's ok-flag discipline are now part of the fragment.

---

## Appendix A — How to reproduce

```bash
make
./baga --verify examples/verify/pair_recv2.baga
./baga --verify examples/verify/pair_select.baga
./baga --verify examples/verify/pair_go.baga
make test
```

## Appendix B — Mapping to source

| Concept | Location |
|---------|----------|
| Pair entries | `ReadEntry.is_pair`, `reads_push_pair`, `reads_find_pair` |
| Rewrites | `se_from_ast` (`cell2`, `cell2_0/1`) |
| Fragment gate | `is_pair_builtin_call`, `is_par_builtin_call`, `par_call_gate` |
| Pair API semantics | `make_status_pair`, `eval_par_call` |

---

*Galactic University draft — Baga project. Slice 4: the deferred list is now
empty.*
