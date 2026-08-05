# A1 — Qualified sum variants: design

Date: 2026-08-05. Status: **approved direction** (advanced plan Phase 1).  
Parent: `docs/superpowers/plans/2026-08-05-advanced-go-rust.md` Track A1-a.

## Goal

Allow multiple sum enums in one program (including across imports) to reuse
variant names such as `Ok` / `Err`, with unambiguous construction and match
via **qualified forms**. Bare forms stay when the variant name is unique.

This unblocks library-scale `Result`-shaped APIs (pg / orm / jsonrpc / pbbaga)
without the current `CallOk` / `DecOk` name tax.

## Syntax

```baga
enum PgRes { Ok(PgRows), Err(str) }
enum RpcRes { Ok(str), Err(RpcErrBody) }

let a = PgRes::Ok(rows)          // qualified construction
let b = RpcRes::Ok("{}")
let c = Ok(42)                   // bare: only if exactly one Ok in program

match a {
    PgRes::Ok(r) => …,
    PgRes::Err(e) => …,
}

// bare match still OK when unique:
match only_res {
    Ok(v) => v,
    Err(e) => 0,
}
```

- Qualifier is the **enum type name** + `::` + variant (Rust-like).
- No spaces around `::` required; lexer treats `::` as one token or two `:` (pick one in impl — prefer single `TOK_COLONCOLON`).

## Resolution rules

### Construction `Name(args)` (bare call)

1. If `Name` is a function → call (unchanged).  
2. Else if exactly one sum-enum variant is named `Name` → construct that.  
3. Else if multiple variants share `Name` → error:  
   `вариантът 'Ok' е нееднозначен — ползвай PgRes::Ok(...) или RpcRes::Ok(...)`.  
4. Else → existing unknown-fn error.

### Construction `Enum::Variant(args)`

1. `Enum` must name a sum enum (or plain enum — plain keeps today’s `Enum_Variant` C tags; optional later).  
2. `Variant` must belong to that enum.  
3. Payload arity/type checks as today.

### Bare ident `None` (payload-less)

Same uniqueness rule as bare construction: unique → ok; ambiguous → require `Opt::None`.

### Match patterns

- `Variant(binding)` / `Variant` — same uniqueness as bare.  
- `Enum::Variant(binding)` / `Enum::Variant` — always resolve to that enum.  
- Exhaustiveness unchanged (per scrutinee type).

## Checker changes (global uniqueness)

**Today:** sum-variant names are globally unique (`повторена дефиниция на вариант`).

**After A1:**  
- Drop global uniqueness for sum variants.  
- Keep: variant cannot collide with a **function** name still? Prefer: functions win on bare call only when no unique variant — document. Simplest: allow `fn Ok` and variant `Ok` — bare `Ok(x)` prefers variant if unique, else fn if unique, else error.  
- Duplicate **same enum** variant names still error.

## Codegen

No runtime change. Qualified and bare lower to the same `Enum__Variant(...)` constructors. Match still compares `.tag`.

## LLVM

Still honest `unsupported` for L3 until A4.

## Tests

| Case | Expect |
|------|--------|
| Two enums both `Ok`/`Err`, only qualified use | compile + run |
| Bare `Ok` with two enums | compile error, names both enums |
| Bare `Ok` with one enum | still works (compat) |
| Match mixed qualified + bare unique | ok |
| `sumtype_test` existing suite | green unchanged |

Negative probes in `scripts/run_tests.sh` or baga-test.

## Docs

- `docs/language-en.md` / `language-bg.md` §11.1  
- CHANGELOG  
- Advanced plan Phase 1 exit criterion #1  

## Non-goals

- Generics `Result<T,E>`  
- `use PgRes::Ok` imports  
- Nested paths `pkg.Enum::Ok` (module-qualified types stay separate L6 story)

## Implementation order

1. Lexer/parser: `::` + path in expr and match pattern  
2. Checker: remove global unique; add ambiguity errors; resolve qualified  
3. Codegen: path → same constructors  
4. Tests + docs  

## Estimate

1–2 focused days on C toolchain; no product migration in the same PR.
