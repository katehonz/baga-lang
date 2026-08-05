# Memory management plan — checker-enforced, systems-grade

Date: 2026-08-05. Status: MEM-1/2 landed 2026-08-05 (drop + seatbelt +
verifier obligations); MEM-3 regions next.
Context: user direction — "the language is a systems language, with
checker-enforced manual memory management". L3 (sum types) is scheduled
next; this plan is the design reference for the memory arc after it.

## 1. Where we are today (inventory)

- **Bump arena global**: `baga_alloc` never reclaims; the whole runtime is
  "leak-tolerant" (`baga_Vec`, `baga_Map`, boxes for `bytes`/`struct`/closure
  envs — everything). Long-running loops OOM — the bn.baga scar (43 s OOM
  before the in-place fix) is the documented proof.
- **Scoped arenas exist but are untracked**: `arena_new() -> i64`,
  `arena_reset`, `arena_free` — handles are plain `i64`; nothing checks
  double-`arena_free`, use-after-`arena_free`, or leaks. The std design doc
  already prescribes the idiom: *one arena per request/connection*.
- **Par runtime** mallocs (`cell2`, channels, go handles) and never frees
  (join/detach consume logically, not physically).
- **`&T` / `*x`** parse and check (`NODE_TYPE_REF`, `UOP_REF/DEREF`) but are
  honest-unsupported in both backends — no raw pointers today.
- **The verifier already tracks resource protocols**: M14's ghost state
  (`spawn → join | detach`, "join after detach is REFUTED") is exactly a
  linear-consumption proof. This is the machinery a memory checker reuses.

## 2. The problem

1. Servers/daemons grow without bound (arena never reclaims mid-request).
2. If we add `free`/`drop` today, nothing prevents use-after-free or
   double-free — the checker has no notion of consumption.
3. Scoped arenas are the intended model but are enforced by convention only.

## 3. Design space

### Option A — Ownership + borrow checking (Rust-lite)
Move semantics on assignment, `&` borrows with lifetimes.
**Against**: fights the language's core semantics — structs are by-value,
`Vec`/`Map` are shared pointers passed freely through functions (every
package relies on this). A borrow checker would rewrite the language. Reject.

### Option B — Linear consumption in the checker (Austral-lite)
New builtin `drop(x)` for heap values (`Vec`, `Map`, `bytes`, closures,
strings). The checker tracks per-variable state `live → dropped` on every
path (the same fix-on-first-use mutation infrastructure + the M14 protocol
ghost state):
- use after `drop` → compile error;
- `drop` twice → compile error;
- scope exit with a live owned value → warning first, error later (opt-in
  strictness per file?).
**For**: small, checker-local, honest; directly reuses M14's
proven/refuted protocol verdicts; `drop` is explicit — manual memory
management with a checker seatbelt, exactly the user's ask.
**Against**: only tracks lexical variables; a value aliased into two
containers can't be tracked (documented: drop of a container-invalidating
value is the programmer's contract, like C — but the common case is checked).

### Option C — Scoped regions formalized (Cyclone-lite)
`let a = arena_new()` becomes a scoped region: values allocated in region
`a` may not escape its lexical scope. The checker tags arena-produced
values with their region and rejects assignments/returns that outlive it.
Idiom: `with_arena { ... }` or per-request arenas in servers.
**For**: matches the existing runtime 1:1; fixes the server growth story
with zero per-value bookkeeping.
**Against**: region-tag propagation through `Vec`/`Map` (containers holding
region values) needs care; escaping detection is approximate.

### Option D — GC / refcounting
Against the systems ethos and the `!Par` performance story; noted and
rejected for the core (nothing stops a future `gc.baga` package).

## 4. Recommendation: B first, then C, both verified by the M14 machinery

- **MEM-1 (B-core)**: `drop(x)` + live/dropped tracking in the checker;
  doubles and uses-after-drop are compile errors. Runtime: `drop` on a
  Vec/Map/bytes walks and frees its heap blocks (the C runtime knows the
  layouts; arena-allocated values are marked free-list-able or the global
  arena gains a free list for boxed sizes).
- **MEM-2 (B-verify)**: new verifier obligation kind reusing M14 ghost
  protocols: `alloc → drop` exactly-once; `--verify` reports
  use-after-drop as REFUTED with a witness path, leaks as UNKNOWN-then-
  proven by the scope-exit check. This is the differentiator: not just a
  linter — the same ДОКАЗАНО/ОБРОЧЕНО verdicts as arithmetic.
- **MEM-3 (C-regions)**: scoped arena idiom made checkable: `arena_free(a)`
  invalidates `a`; values from `a` used after are errors (same live/dropped
  machinery, region-granular). Server loops get bounded memory by
  construction.
- **MEM-4 (interop note)**: closures (L5) allocate env boxes in the arena —
  `drop` of a closure frees the box; `Vec<struct>`/`Map` boxes are covered
  by container drop. No language change needed beyond MEM-1.

## 5. Milestones, files, tests

| Step | Files | Proof |
|------|-------|-------|
| MEM-1 checker tracking | `src/checker.c` (per-var live/dropped table, path join at if/while like loop-havoc), `src/codegen_c.c` (`baga_drop_*` runtime) | `tests/std/drop_test.baga` + negative probes (use-after-drop, double-drop, leak-at-scope-exit) |
| MEM-2 verifier kind | `src/verify.c` (protocol ghost state reuse) | `examples/verify/mem_*.baga` — REFUTED use-after-drop with witness |
| MEM-3 regions | checker region tags + `arena_*` typing | server-loop test with bounded RSS probe |
| MEM-4 docs | `docs/language-{en,bg}.md` new §, CHANGELOG | regression |

Risks: (1) aliasing through containers is the documented honesty boundary —
the checker tracks variables, not heap graphs; (2) `drop` inside closures'
captures is banned (captures are copies/refs — captured values must be
live at every call site); (3) the arena free list is an approximation —
long-run fragmentation is measured, not claimed away.

## 6. Non-goals (v1)

No borrow checker, no lifetimes on references, no GC, no refcounts, no
raw-pointer free of arbitrary `&T`. Strings stay immutable arena values
(dropping individual strings is not tracked in v1 — they're the cheapest
and the most shared).
