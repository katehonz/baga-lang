# L5 — function values & closures: design

Date: 2026-08-05. Status: approved by user (chat), implementation in progress.

## Goal

First-class function values in Baga: named references, lambdas with
explicit captures, calls through values, and function tables in
`Vec`/`Map` (the L5 probe sites: jsonrpcbaga R2 method table, tplbaga
filter table, testbaga T3 `test("name", fn)`).

## Syntax

```baga
let f = add                              // named reference
let g: fn(i64, i64) -> i64 = add         // with annotation
fn apply(h: fn(i64) -> i64, x: i64) -> i64 { return h(x) }

let a = 10
let lam = fn [a] (x: i64) -> i64 { return a + x }   // lambda, by-value captures
lam(5)                                   // 15
```

- fn type: `fn(T, ...) -> R` with optional effects `!IO` (parsed inline
  in the type, not only on fn returns).
- Lambda: `fn [captures] (params) -> ret { body }`; captures are named,
  by value (later mutation of the source variable does not propagate;
  `Vec`/`Map` captures share the reference, as everywhere in Baga).
- Calls: `f(x)` for fn-typed locals/params; `vec_get(t, i)(x)` for
  values in containers.

## Runtime model (C backend)

- A function value is an **i64 handle**: `cell2(code_ptr, env_ptr)`
  (the heap pair from the !Par runtime, already emitted everywhere).
- Every user function gets a wrapper emitted next to it:
  `static ret b_f__clo(void *env, params) { return b_f(params); }`.
- A lambda compiles to: a synthetic env struct `b__env_N` (capture
  fields), a synthetic function `b__lam_N(void *env, params)` that
  unpacks the env into locals, and a literal that boxes both.
- Call through a handle (checker knows the static signature):
  `({ i64 h = <expr>; ((ret(*)(void*, params...))cell2_0(h))(cell2_1(h), args); })`.
- Emission order problem is solved with two memstreams: lambda support
  code is emitted before all user-fn bodies (lambdas can't reference
  each other, so their relative order is free).
- Because the value is an i64 handle, `Vec<fn(...)>` / `Map<str, fn(...)>`
  reuse the i64 container paths (checker admits TYPE_FN as element/value
  kind; codegen maps it to the i64 helpers).

## Type checking

- `TYPE_FN` becomes a value type: structural `type_eq` (params + ret;
  container element equality stays kind-level), `type_str` renders the
  signature.
- Bare ident resolving to a registry fn yields its TYPE_FN (exists
  today); using it as a value wraps the wrapper's handle.
- Calls through values: args checked against the TYPE_FN params, result
  is its ret, `ret` effects merge into the caller (same rule as direct
  calls). Wrap-time check: the wrapped fn's/lambda's effects must be a
  subset of the annotation's effects.
- Lambda bodies: captures + params defined in a fresh scope; body
  effects attach to the lambda's TYPE_FN (they do NOT leak into the
  enclosing function — creating a closure is pure; calling it carries
  the effects).
- **Shadowing ban**: a fn-typed local/param may not share a name with a
  registry function (keeps `--verify` sound — the verifier resolves
  callees by name).
- Verifier: calls through values are opaque (honest skip); no other
  interaction.

## Honestly out of v0

- Captures by reference; automatic (free-variable) capture inference.
- LLVM backend (honest `unsupported`, like Map).
- fn values as `go` workers (workers stay named idents, i64 args).
- verify of calls through values.

## Testing

- `tests/std/fnval_test.baga`: named refs, annotations, `apply`-style
  params, lambdas with captures (scalar + Vec), effect-carrying fn
  types, call-through-container (`Map<str, fn(str)->str>` mini method
  table), returns of closures.
- Negative probes in `scripts/run_tests.sh`: arg type mismatch, effect
  violation (wrap `!IO` fn into pure type), shadowing ban, calling a
  non-fn value.
- Adoption: method-table demo in the test itself (the jsonrpcbaga R2 /
  tplbaga / testbaga gaps get "unblocked" notes; rewrites stay optional).
