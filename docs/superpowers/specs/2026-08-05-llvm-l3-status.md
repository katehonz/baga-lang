# A4 — LLVM L3 status (Phase 5)

Date: 2026-08-05  
Status: **deferred — C backend remains ship path**

## Today

`src/codegen_llvm.c` refuses sum types honestly:

- `TYPE_ENUM` → `llvm_unsupported("sum types (L3) — само C бекенда…")`
- Payload enum names detected via `is_sum_enum_name` → same error  

Plain `i64` tag enums (no payload) may still lower as constants where used;
**payload sums / full match on L3** are C-only.

## Why deferred

1. Production packages and `scripts/run_tests.sh` use the **C backend**.  
2. L3 lowering needs tagged unions + match + topo typedef order (already in
   C codegen) — a large LLVM port without product pressure.  
3. Map and other containers already force C for many product paths.

## Scope if pursued

| Piece | Work |
|-------|------|
| Tagged union | `{ i64 tag; payload }` or opaque box like C |
| Match | switch on tag; extract payload |
| Typedef order | same topo as C for mutual/sum nests |
| Tests | subset of `tests/std/sumtype_test.baga` under LLVM |

## Non-goals for a first A4 PR

- Full monorepo on LLVM  
- Map + L3 together in one PR  
- Par/LLVM interactions with sum payloads  

## Acceptance (future)

`baga --emit-llvm` (or the LLVM driver) compiles and runs a small L3
example (`enum Res { Ok(i64), Err(str) }` + match) without falling back to C.
