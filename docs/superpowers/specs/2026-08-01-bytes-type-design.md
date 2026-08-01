# Design: G5 — A Binary-Safe `bytes` Type

Date: 2026-08-01
Status: Draft (awaiting user approval)

## 1. Goal and non-goals

**Goal.** Add a first-class, binary-safe `bytes` type so binary data (hashes,
HMACs, signatures, protocol buffers) no longer has to be smuggled through
`Vec<i64>`. Closes gap G5 from the jwtbaga probe, where `chr(0) == ""` (strings
are null-terminated C strings) made signatures unrepresentable as `str`.

**Non-goals (M1).** Rewriting `std/crypto`/`std/bytes` to use `bytes` (a
follow-up); a `bytes` builder/arena; UTF-8 validation; `bytes` in the verifier
(M2 bounds tracking is `Vec`-only for now). M1 is the **core type + builtins +
hex**, self-contained and tested.

## 2. Representation

`bytes` is a fat pointer, mirroring `baga_Vec`:
```c
typedef struct { unsigned char *data; int64_t len; } baga_bytes;
```
Passed/returned **by value** (the struct), like `baga_Vec *` is by pointer — but
bytes is a value type (cheap: pointer + length). Data is heap-allocated and
owned; M1 uses the same leak-tolerant model as the rest of the runtime (no
free).

## 3. Surface

**Type name:** `bytes` (resolves to `TYPE_BYTES`).

**Hex literal:** `x"deadbeef"` → a `bytes` value. Even-length hex; the lexer
reads `x"..."` as a bytes-literal token (distinct from the identifier `x`).

**Builtins:**
| Builtin | Signature | Notes |
|---|---|---|
| `bytes_len(b)` | `bytes -> i64` | |
| `bytes_at(b, i)` | `(bytes, i64) -> i64` | byte 0–255 |
| `bytes_slice(b, a, c)` | `(bytes, i64, i64) -> bytes` | `[a, c)` |
| `bytes_concat(a, b)` | `(bytes, bytes) -> bytes` | |
| `bytes_of_str(s)` | `str -> bytes` | copies the string bytes |
| `str_of_bytes(b)` | `bytes -> str` | copies; binary-safe (may embed NUL) |
| `hex_encode(b)` | `bytes -> str` | lowercase hex |
| `hex_decode(s)` | `str -> bytes` | skips non-hex chars |

> **Naming note:** the conversions are `bytes_of_str`/`str_of_bytes` (not
> `bytes_from_str`/`bytes_to_str`) because `std/bytes` and `app-product/jwtbaga`
> already define user functions named `bytes_from_str`/`bytes_to_str` over
> `Vec<i64>`; builtins shadow user functions, so distinct names avoid the
> collision.

## 4. Implementation surface

- **`include/baga.h`**: `TYPE_BYTES` kind; `NODE_BYTES_LIT` node (+ `TOK_BYTES_LIT`).
- **Lexer (`src/lexer.c`)**: `x"..."` → `TOK_BYTES_LIT` (raw hex content).
- **Parser (`src/parser.c`)**: `bytes` type name; `NODE_BYTES_LIT` from the token.
- **Checker (`src/checker.c`)**: resolve `bytes`; type the 8 builtins; type
  `NODE_BYTES_LIT` as `bytes`.
- **C codegen (`src/codegen_c.c`)**: emit the `baga_bytes` struct + 8 runtime
  helpers + a `baga_bytes_from_hex` for literals; map the builtins; emit
  `NODE_BYTES_LIT`.
- **LLVM codegen (`src/codegen_llvm.c`)**: **not implemented in M1** — a
  pre-scan (`program_uses_bytes`) refuses any program using bytes with
  "неподдържан конструкт", so the oracle SKIPs it honestly. Full LLVM parity is
  a follow-up.
- **Self-compiler (`self/compiler.baga`)**: mirrored and working. Its inlined
  tokenizer reads `x"..."` (kind 103), the parser builds a BYTES_LIT node
  (kind 34), `emit_expr` emits `baga_bytes_from_hex(...)`, the LET heuristic
  detects `baga_bytes` (literal or bytes-returning builtin), `expr_is_str`
  recognizes `hex_encode`/`str_of_bytes`, and the runtime gains the `baga_bytes`
  helpers. `make self` stays green and `baga2` produces output identical to the
  bootstrap on `examples/bytes.baga`.

## 5. Testing

- `examples/bytes.baga`: build bytes from hex literals, check
  `bytes_len`/`bytes_at`/`bytes_slice`/`bytes_concat`, and the
  `bytes_of_str`/`str_of_bytes`/`hex_encode`/`hex_decode` conversions (uses
  `${}` interpolation, no std import, so the self-compiler handles it too).
  Exact-output diff in `make test`.
- `make self` green; LLVM/Cranelift oracles SKIP `bytes.baga` (honest refusal).

## 6. Honesty / risk

The LLVM backend lacks bytes (documented refusal, oracle SKIPs) — full parity is
a follow-up. The self-compiler's bytes typing is **heuristic** (LET infers
`baga_bytes` from a bytes literal or a known bytes-returning builtin; an exotic
bytes expression could be mistyped as `i64`). `str_of_bytes` produces a string
that may contain NUL — downstream `strlen`-based C ops would truncate it; that
is inherent (use `bytes` ops for binary). The builtin names avoid collisions
with the existing `Vec`-based user functions of the same intent (see §3 note).
