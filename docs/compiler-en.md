# Baga Compiler Architecture

## Overview

Baga is a statically-typed programming language with Cyrillic identifier support,
compile-time effect checking, and specification verification. The compiler is a
**C transpiler** — it generates C code which is then compiled with gcc to produce
native binaries.

The project has two compiler implementations:

1. **C Bootstrap Compiler** (~3500 lines of C) — the full-featured reference implementation
2. **Self-Hosted Compiler** (~960 lines of Baga) — proves the language can compile itself

---

## Compilation Pipeline

```
┌─────────────────────────────────────────────────────────────────────┐
│                    C BOOTSTRAP COMPILER                              │
│                                                                     │
│  ┌────────┐    ┌────────┐    ┌─────────┐    ┌─────────┐    ┌────┐ │
│  │ Source │───▶│ Lexer  │───▶│ Parser  │───▶│ Checker │───▶│Code│ │
│  │ .baga  │    │        │    │         │    │         │    │gen │ │
│  └────────┘    └────────┘    └─────────┘    └─────────┘    └────┘ │
│                    │              │               │             │    │
│                    ▼              ▼               ▼             ▼    │
│               Tokens        AST          Typed AST        C code   │
│                                                                     │
└─────────────────────────────────────────────────────────────────────┘
                                                              │
                                                              ▼
                                                     ┌──────────────┐
                                                     │  gcc -O2     │
                                                     │  → binary    │
                                                     └──────────────┘
```

```
┌─────────────────────────────────────────────────────────────────────┐
│                    SELF-HOSTING PIPELINE                             │
│                                                                     │
│  Stage 1: C bootstrap compiles compiler.baga → C → gcc → baga2     │
│  Stage 2: baga2 compiles compiler.baga → C → gcc → baga3           │
│  Stage 3: verify baga2 == baga3 (fixed point)                       │
│                                                                     │
│  ┌──────────┐       ┌──────────────┐       ┌──────────┐            │
│  │compiler  │──C──▶ │  gcc         │──bin──▶│  baga2   │            │
│  │.baga     │       │  (bootstrap) │       │          │            │
│  └──────────┘       └──────────────┘       └────┬─────┘            │
│                                                 │                   │
│                                                 ▼                   │
│  ┌──────────┐       ┌──────────────┐       ┌──────────┐            │
│  │compiler  │──C──▶ │  gcc         │──bin──▶│  baga3   │            │
│  │.baga     │       │  (via baga2) │       │          │            │
│  └──────────┘       └──────────────┘       └────┬─────┘            │
│                                                 │                   │
│                                                 ▼                   │
│                                          baga2 == baga3 ✓           │
└─────────────────────────────────────────────────────────────────────┘
```

---

## C Bootstrap Compiler

### Module Layout

| File | Lines | Responsibility |
|------|-------|----------------|
| `src/lexer.c` | ~400 | Tokenization, UTF-8 handling |
| `src/parser.c` | ~900 | Recursive descent parsing → AST |
| `src/checker.c` | ~790 | Type inference, effect checking, spec verification |
| `src/codegen_c.c` | ~840 | C transpilation with name mangling |
| `src/proofs.c` | ~164 | Proof sketch extraction |
| `src/main.c` | ~212 | CLI driver, pipeline orchestration |
| `include/baga.h` | ~460 | Shared data structures and declarations |

### CLI Interface

```
baga [options] <file.baga>

Options:
  --emit-c    Generate C code to stdout (no compilation)
  --ast       Print AST (debug)
  --tokens    Print token stream (debug)
  --specs     Print extracted specifications
  --proofs    Print proof sketches
```

Default behavior: generate C → compile with `gcc -O2` → execute binary → cleanup.

---

### Phase 1: Lexer (`src/lexer.c`)

The lexer converts raw source bytes into a token stream.

**Key features:**
- UTF-8 aware: Cyrillic characters (U+0400–U+04FF) are valid identifier characters
- Two-character operator recognition: `->`, `==`, `!=`, `<=`, `>=`, `&&`, `||`, `..`, `=>`
- String escape processing: `\n`, `\t`, `\\`, `\"`
- Line/column tracking for error reporting
- Single-pass, no backtracking

**Token structure:**
```c
typedef struct {
    TokenKind kind;
    SrcPos    pos;        // {line, col}
    char     *text;       // lexeme (heap-allocated)
    int64_t   int_val;    // for TOK_INT_LIT
    double    float_val;  // for TOK_FLOAT_LIT
} Token;
```

**Algorithm:** Linear scan with a state machine. Each call to `lexer_next()` returns
one token. The lexer peeks one character ahead for two-char operators. Identifiers
consume any byte ≥ 128 (UTF-8 continuation/lead bytes) as part of the name.

---

### Phase 2: Parser (`src/parser.c`)

Recursive descent parser producing a tree of `Node` structs.

**Grammar precedence (low → high):**
1. `||` (logical or)
2. `&&` (logical and)
3. `==`, `!=`, `<`, `>`, `<=`, `>=` (comparison)
4. `+`, `-` (additive)
5. `*`, `/`, `%` (multiplicative)
6. Unary: `-`, `!`, `&`, `*`
7. Primary: literals, identifiers, calls, parenthesized expressions

**AST Node structure:**
```c
struct Node {
    NodeKind kind;
    SrcPos   pos;
    Type    *type;      // inferred type (set by checker)
    union {
        int64_t int_val;                    // NODE_INT_LIT
        char *str_val;                      // NODE_STR_LIT
        char *name;                         // NODE_IDENT
        struct { BinOp bin_op; Node *left; Node *right; };  // NODE_BINARY
        struct { Node *callee; NodeVec args; };             // NODE_CALL
        struct { Node *cond; Node *then_br; Node *else_br; }; // NODE_IF
        struct { NodeVec stmts; };          // NODE_BLOCK
        struct { char *fn_name; NodeVec params; Node *ret_type; Node *fn_body; }; // NODE_FN
        // ... 20+ more variants
    };
};
```

The tagged union approach keeps nodes compact (one allocation per node) while
supporting 30+ node kinds. `NodeVec` is a growable array (`VEC(Node *)` macro).

---

### Phase 3: Checker (`src/checker.c`)

Performs three analyses on the AST:

**1. Type Inference (Hindley-Milner style)**
- Bottom-up type propagation through expressions
- Function return types inferred from `return` statements
- Unification with error recovery (`TYPE_ERROR` sentinel suppresses cascading errors)
- Type equality is structural for primitives, nominal for structs

**2. Effect Checking**
- Effects are annotations on function types: `fn read() -> str !IO`
- Effect propagation: calling an effectful function taints the caller
- `catch !E => handler` removes effect E from the expression's type
- Effects are compile-time only — erased during codegen

**3. Spec Verification**
- `spec` blocks declare input/output types and guarantees
- Checker verifies that the named function's signature matches the spec
- Guarantees are extracted but not formally verified (human/AI audit)

**Type system:**
```c
typedef enum {
    TYPE_VOID, TYPE_BOOL, TYPE_I32, TYPE_I64, TYPE_F64,
    TYPE_STR, TYPE_ARRAY, TYPE_REF, TYPE_STRUCT, TYPE_FN,
    TYPE_VEC, TYPE_ERROR
} TypeKind;

struct Type {
    TypeKind kind;
    Type *elem;         // TYPE_ARRAY element
    Type *pointee;      // TYPE_REF target
    char *name;         // TYPE_STRUCT / TYPE_FN name
    Type *ret;          // TYPE_FN return
    Type **params;      // TYPE_FN parameters
    int nparams;
    char **effects;     // effect annotations
    int n_effects;
};
```

---

### Phase 4: Code Generation (`src/codegen_c.c`)

Transpiles the typed AST into C99 code.

**Name Mangling:**
All user-defined names get a `b_` prefix. Non-ASCII bytes are encoded as `_XX` (hex):
```
факториел → b__d1__84__d0__b0__d0__ba__d1__82__d0__be__d1__80__d0__b8__d0__b5__d0__bb
main      → b_main
```

This ensures Cyrillic identifiers become valid C symbols without collisions.

**Type Mapping:**
| Baga | C |
|------|---|
| `i32` | `int32_t` |
| `i64` | `int64_t` |
| `f64` | `double` |
| `bool` | `int` |
| `str` | `const char *` |
| `Vec` | `baga_Vec *` |
| `void` | `void` |
| `&T` | `T *` |
| `[T]` | `T *` |

**Vec Runtime:**
```c
typedef struct { void **data; int64_t len; int64_t cap; } baga_Vec;
```
A type-erased dynamic array. Separate push/get/set variants for `i64` and `str`
provide type safety at the Baga level while sharing one C implementation.

**Implicit Return:**
If a non-void function's last statement is an expression statement, codegen
converts it to `return expr;`.

---

### Phase 5: Proof Extraction (`src/proofs.c`)

Post-parse pass that walks the AST and emits human-readable proof sketches:
- Function signatures with effects
- Base case detection (if-with-return pattern)
- Recursion detection (heuristic)
- Spec guarantees listed as axioms

Not formal proofs — structured documentation for human or AI verification.

---

## Self-Hosted Compiler (`self/compiler.baga`)

### Architecture

The self-hosted compiler combines lexer + parser + codegen in a single 964-line
Baga file. It deliberately omits the checker (no type inference) and uses a
simpler AST representation.

**Flat AST (linked list):**
Instead of a tree of heap-allocated structs, the self-hosted compiler uses four
parallel vectors:

```
nk: Vec  — node kind (i64)
nt: Vec  — node text (str)
fc: Vec  — first_child index (i64, -1 = none)
ns: Vec  — next_sibling index (i64, -1 = none)
```

Children form a singly-linked list: `first_child` points to the head,
`next_sibling` chains siblings. This avoids recursive data structures and
works within Baga's limited type system (no structs, no enums — just Vec).

**Node Kinds:**
| ID | Kind | Description |
|----|------|-------------|
| 0 | INT | Integer literal |
| 1 | STR | String literal |
| 2 | IDENT | Identifier |
| 3 | BINARY | Binary operation (text = operator) |
| 4 | CALL | Function call |
| 5 | BLOCK | Statement block |
| 6 | LET | Variable binding |
| 7 | RETURN | Return statement |
| 8 | IF | Conditional |
| 9 | WHILE | While loop |
| 10 | EXPR_STMT | Expression statement |
| 11 | FN | Function definition |
| 12 | PARAM | Parameter (text = "name:type") |
| 13 | PROGRAM | Root node |
| 14 | BREAK | Break statement |
| 15 | CONTINUE | Continue statement |
| 16 | TYPE | Return type annotation |
| 17 | ASSIGN | Assignment |
| 18 | FOR | For loop (range-based) |

### Builtins (17 functions)

| Baga name | C function | Signature |
|-----------|-----------|-----------|
| `len` | `baga_len` | `str → i64` |
| `char_at` | `baga_char_at` | `(str, i64) → i64` |
| `substr` | `baga_substr` | `(str, i64, i64) → str` |
| `concat` | `baga_concat` | `(str, str) → str` |
| `str_eq` | `baga_str_eq` | `(str, str) → i64` |
| `chr` | `baga_chr` | `i64 → str` |
| `ord` | `baga_ord` | `str → i64` |
| `read_file` | `baga_read_file` | `str → str` |
| `write` | `baga_write` | `str → void` |
| `vec_new` | `baga_vec_new` | `() → Vec` |
| `vec_push` | `baga_vec_push_i64` | `(Vec, i64) → void` |
| `vec_push_str` | `baga_vec_push_str` | `(Vec, str) → void` |
| `vec_get` | `baga_vec_get_i64` | `(Vec, i64) → i64` |
| `vec_get_str` | `baga_vec_get_str` | `(Vec, i64) → str` |
| `vec_set` | `baga_vec_set_i64` | `(Vec, i64, i64) → void` |
| `vec_set_str` | `baga_vec_set_str` | `(Vec, i64, str) → void` |
| `vec_len` | `baga_vec_len` | `Vec → i64` |

### Type-Aware Codegen

The self-hosted compiler infers C types heuristically:
- String literals → `const char *`
- Calls to `concat`, `substr`, `chr`, `read_file` → `const char *`
- Calls to `vec_new` → `baga_Vec *`
- User function calls → look up return type annotation
- Everything else → `int64_t`

### Forward Declarations

All functions are emitted as C prototypes first, then definitions follow.
This allows mutual recursion without topological sorting.

---

## Design Rationale

### Why C Transpilation (not LLVM/Cranelift)?

1. **Simplicity**: ~840 lines of codegen vs. thousands for a proper backend
2. **Portability**: gcc/clang available everywhere
3. **Debuggability**: generated C is human-readable
4. **Optimization**: gcc -O2 provides production-quality optimization for free
5. **Bootstrapping**: C compiler can compile itself with just gcc

### Why Flat AST in Self-Hosted?

Baga (Phase 1) lacks:
- Structs with typed fields (in the self-hosted subset)
- Enums
- Recursive data types
- Generics

The linked-list-in-vectors approach works within these constraints while
remaining efficient (O(1) node creation, O(children) traversal).

### Why Effects Are Compile-Time Only

Effects (`!IO`, `!Error`) serve as documentation and static guarantees.
They are erased during codegen because:
- No runtime effect handlers (yet)
- C has no concept of effects
- The type system enforces effect discipline at compile time

### Why Decimal Encoding in Self-Hosted Mangling

The C bootstrap uses hex (`_%02x`), but the self-hosted compiler uses decimal
(`int_to_str(c)`) because Baga lacks a hex formatting primitive. Both produce
unique, collision-free encodings — they just don't produce identical C symbols.
This is acceptable because the self-hosted compiler is a separate binary.

---

## Bug Postmortems (Self-Hosting)

### Bug 1: Escape Sequences Not Converted

**Symptom:** Strings containing `\n` printed literal backslash-n instead of newline.

**Root cause:** The tokenizer stored raw source characters without processing
escape sequences. `\n` remained as two characters (92, 110) instead of one (10).

**Fix:** Added escape processing in the string literal branch of `tokenize()`:
```
if sc == 92 {
    pos = pos + 1
    let ec = char_at(src, pos)
    if ec == 110 { sval = concat(sval, chr(10)) }  // \n
    if ec == 116 { sval = concat(sval, chr(9)) }   // \t
    if ec == 92  { sval = concat(sval, chr(92)) }  // \\
    if ec == 34  { sval = concat(sval, chr(34)) }  // \"
}
```

### Bug 2: Missing Implicit Return

**Symptom:** Functions whose last statement was an expression returned garbage.

**Root cause:** Codegen emitted the last expression as a statement (`expr;`)
instead of `return expr;`. C functions fall through to undefined behavior.

**Fix:** In `emit_fn_def`, detect when the last statement in a non-void function
is an EXPR_STMT (kind 10) and emit `return <expr>;` instead.

### Bug 3: Forward Declarations Not Handled

**Symptom:** Parser crashed when encountering `fn foo() -> i64` without a body.

**Root cause:** The parser unconditionally expected `{` after the return type.
Forward declarations (prototype-only, no body) are needed for mutual recursion.

**Fix:** After parsing the return type, check if the next token is `{`. If not,
return the FN node without a BLOCK child. Codegen skips bodyless functions in
the definition pass (they were already emitted as prototypes).

### Bug 4: Void Return Type Defaulted to int64_t

**Symptom:** Void functions generated as `int64_t f(void)` — gcc warnings about
missing return statements.

**Root cause:** `fn_ret_c_type()` returned `"int64_t"` as default when no TYPE
child was found. Functions without explicit `-> type` should be `void`.

**Fix:** Changed default return in `emit_fn`/`emit_fn_def` to `"void"` when no
TYPE node (kind 16) is present among the function's children.

---

## Performance Characteristics

### Compilation Speed

| Phase | Complexity | Bottleneck |
|-------|-----------|------------|
| Lexer | O(n) | Single pass, no allocation per char |
| Parser | O(n) | Recursive descent, one node per token |
| Checker | O(n · d) | d = max nesting depth (type propagation) |
| Codegen | O(n) | Linear AST walk, fprintf to file |
| gcc | O(n²) worst | Dominates total compile time |

For typical programs (< 10K lines), the Baga phases complete in < 50ms.
gcc compilation takes 200–500ms depending on optimization level.

### Memory

- C bootstrap: arena-style allocation (one `malloc` per node, freed at exit)
- Self-hosted: Vec growth (doubling strategy), no deallocation during compilation
- Peak memory: ~2× source size for AST + token stream

### Generated Code Quality

Since output goes through `gcc -O2`:
- Tail calls optimized where applicable
- Loop unrolling, vectorization inherited from gcc
- String operations are naive (malloc per concat) — no interning or rope
- Vec uses `void**` — no specialization, pointer indirection on every access

### Self-Hosted Compiler Performance

The self-hosted compiler is ~5–10× slower than the C bootstrap due to:
- String concatenation for code emission (O(n²) in output size)
- No hash tables (linear scan for builtin lookup, function name resolution)
- Vec-based AST has worse cache locality than struct-based

This is acceptable: the self-hosted compiler's purpose is correctness proof,
not production use.

---

## File Dependencies

```
include/baga.h ◀── src/lexer.c
               ◀── src/parser.c
               ◀── src/checker.c
               ◀── src/codegen_c.c
               ◀── src/proofs.c
               ◀── src/main.c

self/compiler.baga  (standalone, no imports — emits its own C runtime)
```

---

## Build System

```makefile
CC      ?= gcc
CFLAGS  := -O2 -Wall -Wextra -std=c11 -Iinclude
LDFLAGS := -lm
SRCS    := src/main.c src/lexer.c src/parser.c src/checker.c src/codegen_c.c src/proofs.c
```

`make` builds the bootstrap compiler. `make test` runs all example programs.

---

## Future Directions

- Incremental compilation (skip unchanged modules)
- Proper backend (LLVM/Cranelift) for native codegen without C intermediary
- Runtime effect handlers (algebraic effects)
- Formal proof verification (integrate with Lean/Coq)
- Generic types and trait system
- Standard library beyond the 17 builtins
