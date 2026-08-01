# Design: `std` — Minimal Standard Library for Baga

Date: 2026-08-01
Status: Approved by user (scope, layout, English docs)

## 1. Goal and non-goals

**Goal:** a minimal but serious standard library that makes real applications
writable in Baga — cryptography, networking, files, time, JSON. It is the
proof-of-language artifact, not a teaching demo. The end-game validators (an
HTTP backend framework and a KV database) are **separate, later specs** — this
design makes them possible but does not include them.

**Non-goals (YAGNI):** TLS, async/coroutines, user-defined generics, closures,
impl blocks/methods, a package manager, Unicode-aware strings. The compiler
itself keeps zero dependencies (`gcc` + `make` only).

## 2. Layer 0 — enabling compiler changes

Small, additive changes. No new language concepts.

### 2.1 Bitwise operators

The lexer already produces `&`, `|`, `<<`, `>>` tokens but the parser never
uses them as binary operators; `^` (XOR) does not exist at all.

- Wire `&`, `|`, `<<`, `>>` into `binop_precedence` / `token_to_binop`
  (`src/parser.c:580-609`), with C-compatible precedence: `<<`/`>>` below
  additive, then `&`, then `^`, then `|`, all above `&&`/`||`.
- Add `^` token to the lexer (`src/lexer.c`), parser, checker (`src/checker.c`)
  and the three codegens. C backend maps 1:1 to C operators; LLVM maps to
  `and`/`or`/`xor`/`shl`/`ashr`; Cranelift to its integer ops.
- New BinOp variants: `OP_BAND`, `OP_BOR`, `OP_BXOR`, `OP_SHL`, `OP_SHR`.

### 2.2 `extern fn` declarations (FFI to libc)

Syntax:

```baga
extern fn socket(domain: i64, typ: i64, proto: i64) -> i64 !Net
```

- No body. Parameter/return types restricted to `i64`, `f64`, `str`
  (`str` ↔ `char*`), `void`. Effects are declared and checked as usual.
- Parser: new top-level item form keyed on an `extern` keyword (new token).
- Checker: registers the signature in the function table; body-checking is
  skipped; effect checking treats calls like builtin calls.
- C backend: emits `#include`-free prototype + direct call — libc symbols
  resolve at link time, no new files in the build.
- LLVM backend: emits `declare` + `call`.
- Cranelift backend: honestly refuses programs containing `extern fn`
  (same policy as struct/Vec/str builtins today).

### 2.3 `import "path"`

- `import "std/str/str.baga"` at the top of a file. Textual inclusion with an
  include guard keyed on the canonical resolved path: importing the same file
  twice includes it once. Cycles are an error.
- No packages, no namespacing: all imported top-level names share one
  namespace; collisions are compile errors (current behavior for duplicates).
- Paths resolve relative to the importing file's directory, falling back to
  the working directory.

### 2.4 Arena allocator builtins

Added to the emitted C runtime (~40 lines) alongside the existing `baga_*`
helpers, and mirrored in the LLVM backend:

- `arena_new() -> i64` — returns an arena handle.
- `arena_alloc(a: i64, size: i64) -> i64` — bump-allocates, returns an address
  (usable as a raw byte pointer).
- `arena_reset(a: i64)` — frees all allocations of the arena at once.
- `arena_free(a: i64)` — destroys the arena.

Memory model: one arena per request/connection; `arena_reset` at the end of
each cycle — a long-running server does not leak. Short-lived programs keep
the current "leak by design" behavior. Cranelift refuses arenas (consistent
with its subset policy).

## 3. Library layout

Each library lives in its own folder so it can grow (submodules, per-library
README, tests):

```
std/
├── README.md               # overview, memory model, effects policy
├── str/
│   ├── str.baga            # split, find, replace, join, trim, parse_int, starts_with
│   └── README.md
├── bytes/
│   ├── bytes.baga          # Vec<i64> as byte buffer (0-255), hex, base64
│   └── README.md
├── sort/
│   ├── sort.baga           # quicksort + binary search for Vec<i64>
│   └── README.md
├── json/
│   ├── json.baga           # parser + serializer (pure Baga)
│   └── README.md
├── os/
│   ├── os.baga             # env, write_file, read/write fd (extern: getenv, open, write)
│   └── README.md
├── time/
│   ├── time.baga           # time_now_ms, monotonic_ms (extern: clock_gettime)
│   └── README.md
├── random/
│   ├── random.baga         # random_bytes (extern: getrandom)
│   └── README.md
├── io/
│   ├── io.baga             # buffered reader/writer over fd externs
│   └── README.md
├── net/
│   ├── tcp.baga            # listen, accept, connect, read, write (extern: sockets)
│   └── README.md
└── crypto/
    ├── sha256.baga         # pure Baga, NIST-validated
    ├── hmac.baga           # HMAC-SHA256 over sha256.baga
    ├── ct.baga             # constant-time equality
    └── README.md

tests/std/
├── str_test.baga
├── bytes_test.baga
├── sort_test.baga
├── json_test.baga
├── os_test.baga
├── time_test.baga
├── random_test.baga
├── io_test.baga
├── tcp_test.baga           # loopback echo
├── sha256_test.baga        # NIST vectors
└── hmac_test.baga          # RFC 4231 vectors
```

## 4. Module contents

### str (pure)
`str_split`, `str_find`, `str_replace`, `str_join`, `str_trim`,
`str_starts_with`, `str_ends_with`, `parse_int`, `int_to_str` — built on the
existing `len`/`char_at`/`substr`/`concat` builtins.

### bytes (pure)
Byte buffers as `Vec<i64>` holding values 0-255: `bytes_from_str`,
`bytes_to_hex`, `bytes_from_hex`, `base64_encode`, `base64_decode`, `bytes_eq`.

### sort (pure)
`sort_i64`, `binary_search_i64`.

### json (pure)
`json_parse(str) -> JsonValue`-style API within current type-system limits:
a tagged representation using parallel Vec pools (the proven
`self/compiler.baga` pattern — enum tag + index into typed pools), plus
`json_serialize`. Objects/arrays/strings/numbers/bools/null.

### os / time / random (extern)
Thin wrappers: `env(name)`, `write_file(path, data)`, `fd_read`, `fd_write`,
`time_now_ms`, `monotonic_ms`, `random_bytes(n)`.

### io
`BufferedReader`/`BufferedWriter` as structs over an fd + arena buffer:
`read_line`, `read_n`, `write_str`, `flush`.

### net
`tcp_listen(port) -> i64 !Net`, `tcp_accept(listener) -> i64 !Net`,
`tcp_connect(host, port) -> i64 !Net !IO`, `tcp_read`, `tcp_write`,
`tcp_close`. sockaddr built byte-by-byte in Baga over the extern syscalls.

### crypto (pure, deliberately not OpenSSL FFI)
SHA-256 (u32 arithmetic via i64 + mask `& 0xFFFFFFFF`), HMAC-SHA256,
constant-time `ct_eq`. Pure Baga proves the language and keeps zero deps.
ChaCha20-Poly1305 and any pure-Baga TLS are explicitly v2.

## 5. Effects policy

Every std function declares its exact effects — the library is the first real
consumer of the effect system:

- `!IO` — file/fd operations
- `!Net` — sockets
- `!Random` — random_bytes
- `!Time` — clock reads
- pure — str, bytes, sort, json, crypto (visible purity in the type)

## 6. Memory policy

- Pure modules: current leak-tolerant behavior.
- io/net: caller supplies an arena for large buffers.
- Server loop idiom: `while running { let a = arena_new(); handle(a); arena_free(a); }`
  (or `arena_reset` on a reused arena).
- Documented in `std/README.md`.

## 7. Testing — dogfooding

- Every module has a `tests/std/<module>_test.baga` using Baga's own spec
  system (`requires`/`ensures` + `--test-specs` property testing) where
  applicable.
- SHA-256 validated against NIST vectors; HMAC against RFC 4231 vectors.
- net validated with a loopback echo test (server + client in one process via
  two sockets, or self-connect).
- `make test` extended with a `test-std` target; the LLVM oracle keeps
  applying (extern fn works there too); Cranelift skips std tests, as it does
  for Vec/struct today.

## 8. Phases (each ends with a green `make test`)

1. **Bitwise operators + `^`** — parser/checker/three codegens + tests.
2. **`import`** — split one example across two files as the acceptance test.
3. **`extern fn` (C backend)** — first real FFI: reimplement `write_file`
  through it.
4. **Arena builtins.**
5. **str, bytes, sort** — pure Baga, spec-tested.
6. **json** — parser + serializer round-trip tests.
7. **os, time, random, io** — extern layer + buffered IO.
8. **net/tcp** — loopback echo test.
9. **crypto** — sha256 + hmac + ct_eq against published vectors.
10. **LLVM parity for extern fn.**
11. **Final validators:** `examples/http_echo.baga` — a minimal HTTP server
    (plain-text response, arena per request, no framework) and
    `examples/hash_tool.baga` — a CLI that SHA-256-hashes a file. If both
    work, the library is ready for the framework spec.

## 9. Conventions

- All code comments, READMEs, and docs in English (user requirement).
- Library code follows the existing Baga style of `self/compiler.baga`.
- Compiler changes follow existing C style (`-Wall -Wextra` clean).
- Each std folder's README documents the API surface, effects, and memory
  contract of that library.
