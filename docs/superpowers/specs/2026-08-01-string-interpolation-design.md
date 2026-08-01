# Design: G1 — String Interpolation

Date: 2026-08-01
Status: Draft (awaiting user approval)

## 1. Goal and non-goals

**Goal.** Ergonomic string building: `"sum: {s}, done: {flag}"` instead of
nested `concat("sum: ", concat(int_to_str(s), concat(", done: ", ...)))`. This
was the #1 friction in the httpdbaga/jwtbaga probes (every JSON body was 8+
lines of `concat`). Closes gap G1 from `app-product/*/gaps.md`.

**Non-goals (M1).** f64 interpolation (no clean `f64_to_str` builtin yet —
deferred); format specs (`{x:04d}`); interpolation expressions containing `{`,
`}`, or string literals (M1 restricts to brace-free, quote-free expressions:
idents, calls, field/index access, arithmetic).

## 2. Syntax

- `${expr}` inside a string literal interpolates `expr` (converted to `str`).
- `$$` is a literal `$`. A lone `{` or `}` is **ordinary text** — this matters:
  `self/compiler.baga` is full of C-code strings containing braces, so a bare
  `{expr}` syntax would have collided with them. `$` appears nowhere in the
  existing sources, so `${...}` is conflict-free.
- An unclosed `${` is a **parse error** (fail loud, not silent).
- Supported `expr` types: `str` (as-is), `i64` (`baga_i64_to_str`), `bool`
  (`? "true":"false"`). Other types → compile error "неподдържан тип за
  интерполация".

## 3. Desugaring (the fixed-point-safe core)

Interpolation is **parser-level syntactic sugar**: `"a${x}b"` parses to the
exact AST as `concat("a", concat(«x», "b"))`, where `«x»` is a new
`NODE_TO_STR` wrapper node holding `x`. Because it desugars to existing
constructs (`concat` calls) plus one tiny node, the change is small and the
`baga2 == baga3` fixed point is preserved (compiler.baga itself uses no
interpolation — it has no `$` in any string).

`NODE_TO_STR` conversion is type-directed in codegen (the checker records the
inner type): `str`→identity, `i64`→`baga_i64_to_str`, `bool`→`? "true":"false"`.

## 4. Implementation surface

- **C lexer (`src/lexer.c`)**: unchanged (`$`/`{`/`}` are ordinary content
  bytes inside a string).
- **C parser (`src/parser.c`)**: when building a string literal, scan `str_val`
  for `${...}` (brace-depth); if found, build the `concat`/`NODE_TO_STR` chain
  via a sub-lex+sub-parse of each expression; else plain `NODE_STR_LIT`.
  `$$` → literal `$`. New `NODE_TO_STR` node kind (+ free/print/check/codegen).
- **Checker (`src/checker.c`)**: `NODE_TO_STR` types its inner expr; result is
  `str`; error on unsupported inner type; effects propagate.
- **C codegen (`src/codegen_c.c`)**: `NODE_TO_STR` emits the type-directed
  conversion; new runtime helper `baga_i64_to_str` (no import needed).
- **LLVM codegen (`src/codegen_llvm.c`)**: `NODE_TO_STR` likewise, with a
  hand-built IR `baga_i64_to_str` (decimal, sign-aware) for C/LLVM parity.
- **Self-compiler (`self/compiler.baga`)**: **not yet mirrored** (follow-up).
  The fixed point holds because compiler.baga uses no interpolation; but a
  program compiled by the self-compiled compiler does not get interpolation
  until the mirror lands. Documented as a known limitation, not a soundness
  issue.

## 5. Testing

- `examples/interp.baga`: interpolate str/i64/bool, a call `${double(n)}`,
  `$$` literal, lone braces as text; exact-output diff in `make test`.
- `make self` stays green (fixed point) — the acceptance gate.

## 6. Honesty / risk

The self-compiler mirror is deferred: it needs the same `${...}` desugaring in
`self/compiler.baga` (parse-time, to keep baga2==baga3 byte-identical). Until
then interpolation is a bootstrap/LLVM feature. This is a completeness gap, not
a correctness one.
