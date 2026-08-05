# L3 — sum types (payload enums) & full match: design

Date: 2026-08-05. Status: approved by user (chat), implementation in progress.

## Goal

Sum types in Baga: enum variants carrying one typed payload, value
construction, and `match` with bindings + exhaustiveness checking. This
closes the last open language level (L1 Map, L2 HTTP client, L4
containers-of-structs, L5 closures, L6 namespaces are done) and retires
the ok/err struct stand-ins waiting across the app stack (jsonrpcbaga
R1 `RpcResult`, tplbaga P2 `TplOut`, bagadecimal D4, oauthbaga).

## Syntax

```baga
enum Res { Ok(i64), Err(str) }        // sum enum: variants with payload
enum Opt { Some(str), None }          // mixed payload + plain variants

let r = Ok(42)                        // construction: call syntax
let o = None                          // plain variant: bare (unchanged)

match r {
    Ok(v) => v,                       // binding pattern: v : i64 in arm scope
    Err(e) => { print(e); 0 },
}

fn unwrap_or(r: Res, dflt: i64) -> i64 {   // enum types in annotations
    return match r { Ok(v) => v, _ => dflt }
}
```

- A variant carries **exactly one** payload type; a payload may be any
  first-class type (`i64/f64/bool/str/bytes/struct/fn/Vec/Map`) — multi-field
  variants are written with a struct payload (`Move(Point)`), not new syntax.
- An enum with ≥1 payload variant is a **sum enum**. Plain enums
  (no payloads) keep today's semantics untouched: `i64`, bare variants,
  usable wherever an `i64` is expected.

## Type checking

- New `TypeKind`: `TYPE_ENUM`, nominal (`type_eq` by enum name, like
  structs). Sum enums are **not** `i64`-compatible — that is the point
  (no accidental arithmetic on a `Res`).
- Registration (checker pass 1): variants of a sum enum get a payload
  `Type *` in the variant table; plain variants of a sum enum register
  with `payload = NULL` but still belong to the TYPE_ENUM type.
- **Variant names of a sum enum are globally unique** across all enums
  and all functions in the program — checker error
  `повторена дефиниция на вариант 'Ok'` / `името 'Ok' е едновременно
  функция и вариант`. Needed for unambiguous construction. Plain enums
  keep today's behavior (first-match resolution) — no breakage.
- Construction `Ok(expr)`: resolves through the variant table; exactly
  one argument, `type_assignable` against the payload type; result type
  is the enum's TYPE_ENUM. Bare reference to a payload variant without
  call → error `конструкторът 'Ok' изисква 1 аргумент`.
- Bare reference to a plain variant of a sum enum (`None`) yields the
  TYPE_ENUM value (tag only).
- Match on a TYPE_ENUM scrutinee:
  - patterns: `Variant(binding)`, `Variant` (plain), `_`;
  - the binding is a fresh local of the payload type, scoped to the arm;
  - **exhaustiveness**: every variant covered or a wildcard present,
    else compile error `match върху 'Res' не е пълен — липсва вариант
    'Err' (или добави '_')`;
  - all arm bodies must agree in type with the first arm (`type_eq`,
    ERROR-tolerant) — for sum-enum matches only; non-enum matches keep
    today's first-arm-wins behavior (documented gap).
- Enum types are legal in parameter/return/let annotations (`Res`
  resolves like a struct name in `parse_type`).
- **Containers: honest rejection for v1** — `Vec<Res>` / `Map<str,Res>`
  are a checker error (`sum enum-ите още не са елементи на Vec/Map`),
  same honesty pattern as `Vec<struct>` had before L4 closed it.
- Effects: construction and matching are pure. Verifier: a sum-enum
  value is opaque to `--verify` (honest skip, like fn values).

## Runtime model (C backend)

```c
typedef struct { int tag; union { int64_t v_Ok; const char *v_Err; } u; } Res;
static inline Res Res__Ok(int64_t a0) { Res r; r.tag = 0; r.u.v_Ok = a0; return r; }
static inline Res Res__Err(const char *a0) { Res r; r.tag = 1; r.u.v_Err = a0; return r; }
```

- One C struct per sum enum: `int tag` (declaration order, same numbers
  as today) + a union with one member per payload variant. Plain
  variants get no union member. Values are by-value everywhere, exactly
  like structs (no boxing, arena only holds what payloads point to).
- `Ok(42)` emits `Res__Ok(42)`; bare `None` emits the compound literal
  `(Res){ .tag = 1 }`. Constructor/enum C names are mangled through the
  existing `mangle()` so a user fn named `Res__Ok` cannot collide
  (mangled user code gets the `b_` prefix).
- Match on a sum enum: same GCC statement-expression shape as today's
  match — compare `_mv.tag == k`, and at the arm head emit
  `payload_t binding = _mv.u.v_Ok;` before the body. Wildcard = final
  `else`.
- Result-type mapping of the match statement-expression (`emit_ctype`)
  gains TYPE_ENUM → mangled enum name and TYPE_STRUCT → mangled struct
  name (today a struct result silently falls to `int64_t` — fixed here
  because sum matches routinely produce structs/enums).
- `emit_zero_struct` is not needed (sum enums can't be Map values yet).

## Honestly out of v1

- Generic `Result<T,E>` / user generics of any kind (write a concrete
  enum per use-site — exactly what the stand-in sites need).
- `Vec<sum enum>` / `Map<K, sum enum>` elements (checker error; the
  box-path from L4 is the known migration when needed).
- Multi-payload variants without a struct wrapper; pattern guards;
  nested/or patterns; `if is Ok` tests outside match.
- Explicit discriminants (`A = 5`) — still indices in declaration order.
- LLVM backend: honest `unsupported` (same mechanism/message shape as
  L5 fn values, pointing at the docs section).
- Exhaustiveness / arm-type consistency for non-enum matches
  (integers/strings): unchanged, documented.
- Self-hosting: `self/compiler.baga` does not use sum types, so no
  parity work; `make self` must simply stay green.

## Testing

- `tests/std/sumtype_test.baga` (testbaga asserts): construction +
  match bindings; mixed enums (`None` bare); str/struct/bytes payloads;
  enum as param and return type; match as an expression in `let`;
  wildcard arm; nested match; the canonical `Res`-shaped Result
  round-trip (the jsonrpcbaga/tplbaga stand-in shape).
- Negative probes in `scripts/run_tests.sh`: non-exhaustive match
  (missing variant named in the error), wrong payload type, constructor
  with 0/2 args, bare reference to a payload variant, duplicate variant
  name across two enums, `Vec<Res>` rejection.
- Gates: `make test` green (incl. LLVM oracle untouched — sum tests are
  C-backend only), `make self` green.
- Adoption notes in the affected gaps.md files (jsonrpcbaga R1, tplbaga
  P2, bagadecimal D4, oauthbaga): "unblocked" + optional migration, same
  convention as L5.
