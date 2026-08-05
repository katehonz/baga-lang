# Design: light optional borrow checker (C′ / Phase 5)

Date: 2026-08-05  
Status: **direction only** — not scheduled for implementation  
Parent: `docs/superpowers/plans/2026-08-05-memory-management.md` §7  
Track: advanced plan **C′**

## Intent

A **light, opt-in** checker pass that catches a few high-value mistakes
without becoming Rust (no lifetime params, no move-by-default, no rewrite of
`Vec`/`Map` sharing).

## Non-goals

- Full affine ownership / move semantics on every assignment  
- Lifetime generics or region variables in the type system  
- Breaking existing monorepo packages when the flag is off  
- Replacing MEM-1/2/3 drop / arena seatbelts  

## Candidate shapes (any one is enough for a first PR)

1. **Exclusive local borrow** — after an explicit `borrow mut x` (or attribute),
   reject aliasing use of `x` until the borrow ends. Default code unchanged.
2. **Drop-adjacent ban** — while a value is `drop`-owned, forbid a second live
   name that still aliases it (use-after-drop via second binding).
3. **`--borrow-lite` WARN pass** — separate from typecheck; WARN by default,
   hard error only under an extra flag.

## Default semantics (unchanged)

| Construct | Today | With C′ off |
|-----------|--------|-------------|
| Struct by value | copy fields | same |
| `Vec` / `Map` | shared heap | same |
| `drop` | frees current block | same |

## When to implement

Only if product pain shows (e.g. use-after-drop through a second name the
checker could have seen). Prefer MEM-3 payload regions if both compete.

## Exit criteria (if ever started)

- Design note (this file) + one opt-in probe under `examples/` or `tests/`  
- Full monorepo builds without flags still green  
- Language-en/bg one paragraph under memory / MEM  

## Relation to Phase 5

Listed as stretch. **Does not block** product B tracks or RocksDB path.
