# Baga Minimal Standard Library Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A minimal but serious standard library for Baga — strings, bytes, sorting, JSON, OS/files, time, randomness, buffered IO, TCP networking, and SHA-256/HMAC cryptography — enabled by four small compiler features (bitwise operators, `import`, `extern fn` FFI, arena allocator builtins), proven by two end-to-end validators (`examples/http_echo.baga`, `examples/hash_tool.baga`).

**Architecture:** Four additive compiler changes (Tasks 1–4) unblock everything else. The library itself is pure Baga code under `std/`, one folder per library, built only on: existing builtins (`len`, `char_at`, `substr`, `concat`, `chr`, `ord`, `str_eq`, `vec_*`, `read_file`, `arg*`, `exit`, `eprintln`), the new bitwise operators, `import`, `extern fn` declarations against libc, and arena builtins. All std modules share one global namespace via textual `import` inclusion with an include guard. Tests are Baga programs under `tests/std/` run by `./baga <file>`, exiting non-zero on failure, wired into a new `make test-std` target.

**Tech Stack:** C11 bootstrap compiler (`gcc` + `make`, zero dependencies), optional LLVM backend (`llvm-config-14` + `lli-14`), optional Cranelift JIT (Rust staticlib). Library code: Baga itself.

## Global Constraints

- The compiler keeps **zero dependencies**: `gcc` + `make` only. No new libraries, no new build steps beyond the existing `make` / `make llvm` / `make cranelift`.
- Compiler C code stays `-Wall -Wextra -std=c11` clean (see `Makefile:2`). Every task ends with a green `make` and a green relevant test run.
- **All new code comments, READMEs, and docs in English** (user requirement). Existing Bulgarian identifiers/messages are left untouched; new user-facing error messages follow the existing Bulgarian style where they sit next to Bulgarian messages, but all *comments and documentation* are English.
- **LLVM parity is required for `extern fn`** (Task 10). Before Task 10 lands, the LLVM backend must *honestly refuse* extern programs (via `llvm_unsupported`, which the oracle counts as SKIP) — never silently miscompile.
- **Cranelift may honestly refuse** `extern fn` and arena programs via its existing `cr_unsupported` pattern (`src/codegen_cranelift.c:87-90`); the oracle greps for `неподдържан конструкт` and counts it as SKIP. Bitwise operators, however, must be implemented in Cranelift (Task 1).
- **Do not modify `self/compiler.baga`** (the 2328-line self-hosted compiler). The self-hosting fixed point (`make self`) must stay green; it uses none of the new features.
- Do not break the existing oracle scripts: `tests/llvm_oracle.sh` and `tests/cranelift_oracle.sh` iterate `examples/*.baga`; new examples that cannot run everywhere must be added to their skip lists (shown in the relevant tasks).
- Baga test programs are run via `./baga <file>` (compiles + runs, propagates the program's exit code — `src/main.c:249-252`). Negative tests grep stderr, following the existing `Makefile` `test` target patterns (e.g. `spec_ensures_fail` at `Makefile:94-97`). New std tests follow that style.
- Note: `./baga <file>` does **not** forward argv to the compiled program (`src/main.c:243` runs the binary bare). Validators that need argv compile via `./baga --emit-c` + `gcc` and run the binary directly (Task 11 scripts do exactly this).

## Verified code facts this plan relies on

(Checked against the sources on 2026-08-01; line numbers may drift ±a few lines.)

- `BinOp` **already has** `OP_BIT_AND, OP_BIT_OR, OP_BIT_XOR, OP_LSHIFT, OP_RSHIFT` (`include/baga.h:186-191`). The checker already types them (`src/checker.c:318-320`), `codegen_c` already maps them (`src/codegen_c.c:128-130`), `codegen_llvm` already lowers them (`src/codegen_llvm.c:894-908`). Only the lexer `^` token, the parser wiring, and the Cranelift binop codes are missing. The spec's names `OP_BAND`/`OP_BOR`/… are therefore **not** introduced; the existing `OP_BIT_*`/`OP_LSHIFT`/`OP_RSHIFT` names are used.
- Tokens `TOK_AMP` (`&`), `TOK_PIPE` (`|`), `TOK_LSHIFT` (`<<`), `TOK_RSHIFT` (`>>`) exist (`include/baga.h:91-92,117-118`); there is no `^` token and no `extern`/`import` keyword. Keyword table: `src/lexer.c:220-240`; `kind_names` table: `src/lexer.c:30-96`; single-char tokens: `src/lexer.c:465-487`.
- Parser: `binop_precedence` / `token_to_binop` at `src/parser.c:580-609`; `parse_program` dispatch at `src/parser.c:1002-1030`; `parse_fn` already supports bodyless forward declarations (`src/parser.c:812-820`); `parse_type_with_effects` at `src/parser.c:225-242`.
- Checker: builtin table at `src/checker.c:402-415`; `print`/`println`/`write` are recognized as builtins **before** user functions (`src/checker.c:334-338`) — Task 3 must reorder this so user-declared (extern) functions shadow builtins, because `os.baga` declares `extern fn write`. Function registration pass 1 at `src/checker.c:782-836`; `check_fn` skips bodyless functions already (`src/checker.c:748-751`); effects propagate from a callee's declared return type at call sites (`src/checker.c:448-455`).
- C codegen: builtin name map at `src/codegen_c.c:301-319`; runtime helpers emitted at `src/codegen_c.c:1020-1083`; `emit_forward_decls` at `src/codegen_c.c:893-922`; `emit_fn` at `src/codegen_c.c:709-776` (definitions are only emitted when `item->fn_body` is set, `src/codegen_c.c:1112-1116`); `is_print_call` also treats `write` as print (`src/codegen_c.c:135-142`) — extern calls must be checked *before* it. Name mangling adds a `b_` prefix (`src/codegen_c.c:27-44`); extern calls must bypass it. Generated C includes only `stdio.h`, `stdlib.h`, `stdint.h`, `string.h` (`src/codegen_c.c:1014-1018`) — this constrains which libc symbols can be prototyped without conflicts (see Task 3 design note).
- LLVM codegen: `llvm_unsupported` prints `неподдържан конструкт` (`src/codegen_llvm.c:51-54`) → oracle SKIP. `predeclare_fn_llvm` at `src/codegen_llvm.c:1602-1613` mangles every function name. Call resolution at `src/codegen_llvm.c:1172-1245`; `lg.program` is available for scanning items. `llvm_type` maps `str → ptr`, `i64 → i64`, `f64 → double`, `void → void` and unwraps `NODE_TYPE_EFFECT` (`src/codegen_llvm.c:88-105`). Runtime helpers are built lazily via `baga_rt` (`src/codegen_llvm.c:826-861`) using the `build_baga_vec_*` patterns at `src/codegen_llvm.c:405-540`.
- Cranelift: `cr_unsupported` at `src/codegen_cranelift.c:87-90`; `binop_code` at `src/codegen_cranelift.c:186-201`; unknown builtins already hit `cr_unsupported("вградена функция ...")` at `src/codegen_cranelift.c:383-387` (so arena builtins refuse honestly for free). Function table skips bodyless functions (`src/codegen_cranelift.c:729-731`). Bytecode opcodes shared with Rust: `cranelift/baga_clif_rt.h:35-40` (binop codes 0–21 used) and `cranelift/src/lib.rs:46-67`; `emit_binop` in Rust at `cranelift/src/lib.rs:633-667`.
- `main.c` reads exactly one file (`src/main.c:83-100`) — `import` is implemented there as token-level recursive inclusion (Task 2).
- Structs hold `Vec<T>` fields fine (checker does not type-check struct-literal field values, `src/checker.c:598-617`; `emit_type` maps `Vec → baga_Vec *`, `src/codegen_c.c:67`). Structs are passed/returned by value; functional update style (`p = f(p)`) is used throughout std.
- Effects are compile-time only and erased at codegen (`src/codegen_c.c:432-441`), so `?`/`catch` on extern calls cost nothing at runtime.
- Baga string literals support `\0` escapes and `chr(n)` returns a malloc'd 2-byte string — both are load-bearing for building binary data (sockaddr) via `bcopy` (Tasks 7–8).

## Std API surface (defined in Tasks 5–9, consumed by Tasks 8–11 — names are normative)

```baga
// std/str/str.baga (Task 5) — pure
fn str_starts_with(s: str, prefix: str) -> bool
fn str_ends_with(s: str, suffix: str) -> bool
fn str_find(s: str, needle: str) -> i64            // index or -1
fn str_split(s: str, delim: str) -> Vec<str>       // delim = single character
fn str_join(parts: Vec<str>, sep: str) -> str
fn str_replace(s: str, from: str, to: str) -> str
fn str_trim(s: str) -> str
fn str_repeat(s: str, n: i64) -> str               // used for writable buffers
fn parse_int(s: str) -> i64
fn int_to_str(n: i64) -> str

// std/bytes/bytes.baga (Task 5) — pure; byte = i64 in 0..255 inside Vec<i64>
fn bytes_from_str(s: str) -> Vec<i64>
fn bytes_to_hex(b: Vec<i64>) -> str
fn bytes_from_hex(h: str) -> Vec<i64>
fn base64_encode(b: Vec<i64>) -> str
fn base64_decode(s: str) -> Vec<i64>
fn bytes_eq(a: Vec<i64>, b: Vec<i64>) -> bool

// std/sort/sort.baga (Task 5) — pure
fn sort_i64(v: Vec<i64>)                            // in-place quicksort
fn binary_search_i64(v: Vec<i64>, x: i64) -> i64    // index or -1

// std/json/json.baga (Task 6) — pure; node = i64 index into parallel pools
// tags: -1 error, 0 null, 1 false, 2 true, 3 number, 4 string, 5 array, 6 object
struct JsonDoc { root: i64, tags: Vec<i64>, nums: Vec<f64>, strs: Vec<str>, kids: Vec<i64>, kfrom: Vec<i64>, klen: Vec<i64> }
fn json_parse(s: str) -> JsonDoc                    // on error json_root's tag is -1
fn json_root(d: JsonDoc) -> i64                     // the document's root node
fn json_tag(d: JsonDoc, n: i64) -> i64
fn json_num(d: JsonDoc, n: i64) -> f64
fn json_str(d: JsonDoc, n: i64) -> str
fn json_count(d: JsonDoc, n: i64) -> i64            // array elems / object pairs
fn json_at(d: JsonDoc, n: i64, i: i64) -> i64       // array element node
fn json_key(d: JsonDoc, n: i64, i: i64) -> str      // object key i
fn json_val(d: JsonDoc, n: i64, i: i64) -> i64      // object value node i
fn json_get(d: JsonDoc, n: i64, key: str) -> i64    // value node by key, -1 if absent
fn json_serialize(d: JsonDoc, n: i64) -> str

// std/os/os.baga (Task 7)
extern fn open(path: str, flags: i64, mode: i64) -> i64 !IO
extern fn close(fd: i64) -> i64 !IO
extern fn read(fd: i64, buf: str, count: i64) -> i64 !IO
extern fn write(fd: i64, buf: str, count: i64) -> i64 !IO
extern fn getenv(name: str) -> str !IO
fn env(name: str) -> str !IO                        // "" when unset (NULL → "" coercion)
fn fd_write(fd: i64, data: str) -> i64 !IO          // bytes written, -1 on error
fn fd_read(fd: i64, n: i64) -> str !IO              // up to n bytes, "" at EOF
fn write_file(path: str, data: str) -> i64 !IO      // 0 ok, -1 error
fn mem_i64(s: str, off: i64) -> i64                 // little-endian u64 load from bytes

// std/time/time.baga (Task 7)
extern fn clock_gettime(clk: i64, ts: str) -> i64 !Time
fn time_now_ms() -> i64 !Time                       // CLOCK_REALTIME (0)
fn monotonic_ms() -> i64 !Time                      // CLOCK_MONOTONIC (1)

// std/random/random.baga (Task 7)
extern fn getrandom(buf: str, n: i64, flags: i64) -> i64 !Random
fn random_bytes(n: i64) -> Vec<i64> !Random
fn random_i64() -> i64 !Random

// std/io/io.baga (Task 7)
struct Reader { fd: i64 }
struct Writer { fd: i64, buf: str }
fn reader_new(fd: i64) -> Reader
fn read_line(r: Reader) -> str !IO                  // without trailing \n; "" at EOF
fn read_n(r: Reader, n: i64) -> str !IO
fn writer_new(fd: i64) -> Writer
fn write_str(w: Writer, s: str) -> Writer           // appends to buffer, returns updated
fn flush(w: Writer) -> Writer !IO                   // writes buffer to fd, clears it

// std/net/tcp.baga (Task 8)
extern fn socket(domain: i64, typ: i64, proto: i64) -> i64 !Net
extern fn setsockopt(fd: i64, level: i64, opt: i64, val: i64, len: i64) -> i64 !Net
extern fn bind(fd: i64, addr: i64, addrlen: i64) -> i64 !Net
extern fn listen(fd: i64, backlog: i64) -> i64 !Net
extern fn accept(fd: i64, addr: i64, addrlen: i64) -> i64 !Net
extern fn connect(fd: i64, addr: i64, addrlen: i64) -> i64 !Net
extern fn bzero(dst: i64, n: i64)
extern fn bcopy(src: str, dst: i64, n: i64)
fn tcp_listen(port: i64) -> i64 !Net                // fd or -1 (SO_REUSEADDR set)
fn tcp_accept(listener: i64) -> i64 !Net            // fd or -1
fn tcp_connect(host: str, port: i64) -> i64 !Net    // fd or -1; host = dotted IPv4
fn tcp_read(fd: i64, n: i64) -> str !Net !IO
fn tcp_write(fd: i64, data: str) -> i64 !Net !IO
fn tcp_close(fd: i64) -> i64 !IO

// std/crypto/sha256.baga (Task 9) — pure
fn sha256_bytes(data: Vec<i64>) -> Vec<i64>         // 32-byte digest
fn sha256(msg: str) -> Vec<i64>
fn sha256_hex(msg: str) -> str

// std/crypto/hmac.baga (Task 9) — pure
fn hmac_sha256(key: str, msg: str) -> Vec<i64>
fn hmac_sha256_hex(key: str, msg: str) -> str

// std/crypto/ct.baga (Task 9) — pure
fn ct_eq(a: str, b: str) -> bool
fn ct_eq_bytes(a: Vec<i64>, b: Vec<i64>) -> bool

// Arena builtins (Task 4, compiler-level, not a std module)
fn arena_new() -> i64
fn arena_alloc(a: i64, size: i64) -> i64
fn arena_reset(a: i64)
fn arena_free(a: i64)
```

---

## Task 1: Bitwise operators `& | ^ << >>`

**Files:**
- Modify: `include/baga.h` (token enum at lines 117-120; `BinOp` at 186-191 needs **no** change)
- Modify: `src/lexer.c` (`kind_names` table lines 94-95; single-char switch lines 478-487)
- Modify: `src/parser.c` (`binop_precedence` lines 580-590; `token_to_binop` lines 592-609)
- Modify: `src/codegen_cranelift.c` (`binop_code` lines 186-201)
- Modify: `cranelift/baga_clif_rt.h` (binop codes lines 35-40)
- Modify: `cranelift/src/lib.rs` (binop consts lines 46-67; `emit_binop` lines 643-666)
- Modify: `Makefile` (`test` target, after the `vec_ann` block at line 111)
- Test: `examples/bitwise.baga` (create)
- No changes needed in `src/checker.c`, `src/codegen_c.c`, `src/codegen_llvm.c` (already handle these ops — see verified facts).

**Interfaces:**
- Consumes: existing tokens `TOK_AMP`, `TOK_PIPE`, `TOK_LSHIFT`, `TOK_RSHIFT`; existing `BinOp` variants `OP_BIT_AND`, `OP_BIT_OR`, `OP_BIT_XOR`, `OP_LSHIFT`, `OP_RSHIFT`.
- Produces: new token `TOK_CARET` (`^`); parser precedence `|| < && < | < ^ < & < ==/!= < relational < << />> < additive < multiplicative` (C-compatible); Cranelift binop codes `B_BAND_I=22, B_BOR_I=23, B_BXOR_I=24, B_SHL_I=25, B_SHR_I=26` (header and Rust kept manually in sync, as the header comment demands).

**Steps:**

- [ ] Write the failing test `examples/bitwise.baga`:

```baga
fn main() {
    print(6 & 3)             // 2
    print(6 | 3)             // 7
    print(6 ^ 3)             // 5
    print(1 << 4)            // 16
    print(256 >> 4)          // 16
    print(1 + 2 << 3)        // 24  (shift binds looser than +)
    print(5 & 3 | 8)         // 9   (& tighter than |)
    print(5 ^ 3 & 1)         // 4   (& tighter than ^)
    print(4294967295 >> 8)   // 16777215
}
```

- [ ] Run it to see it fail: `./baga examples/bitwise.baga` → parse error near `&` (`очаквах декларация` / expression error), and `^` is a lexer error (`непознат символ '^'`).
- [ ] Add the token in `include/baga.h`, after `TOK_RSHIFT`:

```c
    TOK_LSHIFT,     /* << */
    TOK_RSHIFT,     /* >> */
    TOK_CARET,      /* ^ */
```

- [ ] Lexer: add to `kind_names` in `src/lexer.c` after the `TOK_RSHIFT` entry (line 95):

```c
    [TOK_LSHIFT]    = "<<",
    [TOK_RSHIFT]    = ">>",
    [TOK_CARET]     = "^",
```

- [ ] Lexer: add the single-char case in the switch at `src/lexer.c:478-487`, after `case '|':`:

```c
        case '|': return make_token(l, TOK_PIPE, start, strdup("|"));
        case '^': return make_token(l, TOK_CARET, start, strdup("^"));
```

- [ ] Parser: renumber `binop_precedence` in `src/parser.c:580-590` (relative order of existing ops is preserved, so no behavior change for existing programs):

```c
static int binop_precedence(TokenKind k) {
    switch (k) {
        case TOK_OR:      return 1;
        case TOK_AND:     return 2;
        case TOK_PIPE:    return 3;    /* |  */
        case TOK_CARET:   return 4;    /* ^  */
        case TOK_AMP:     return 5;    /* &  */
        case TOK_EQ: case TOK_NEQ: return 6;
        case TOK_LT: case TOK_GT: case TOK_LE: case TOK_GE: return 7;
        case TOK_LSHIFT: case TOK_RSHIFT: return 8;
        case TOK_PLUS: case TOK_MINUS: return 9;
        case TOK_STAR: case TOK_SLASH: case TOK_PERCENT: return 10;
        default: return -1;
    }
}
```

- [ ] Parser: extend `token_to_binop` in `src/parser.c:592-609` before the `default:`:

```c
        case TOK_AND:     return OP_AND;
        case TOK_OR:      return OP_OR;
        case TOK_AMP:     return OP_BIT_AND;
        case TOK_PIPE:    return OP_BIT_OR;
        case TOK_CARET:   return OP_BIT_XOR;
        case TOK_LSHIFT:  return OP_LSHIFT;
        case TOK_RSHIFT:  return OP_RSHIFT;
```

(Unary `&x` in `parse_unary` and `&T` in `parse_type` are unaffected: they consume `TOK_AMP` before binary parsing ever sees it. Also extend the debug-only `binop_str` at `src/parser.c:1040-1051` with `case OP_BIT_AND: return "&"; case OP_BIT_OR: return "|"; case OP_BIT_XOR: return "^"; case OP_LSHIFT: return "<<"; case OP_RSHIFT: return ">>";` so `--ast` prints the new operators instead of `?`.)

- [ ] Cranelift: add binop codes in `cranelift/baga_clif_rt.h`:

```c
    B_EQ_F  = 16, B_NEQ_F = 17, B_LT_F = 18, B_GT_F = 19, B_LE_F = 20, B_GE_F = 21,
    B_BAND_I = 22, B_BOR_I = 23, B_BXOR_I = 24, B_SHL_I = 25, B_SHR_I = 26,
```

- [ ] Cranelift: mirror the consts in `cranelift/src/lib.rs` after `const B_GE_F: u8 = 21;`:

```rust
const B_BAND_I: u8 = 22;
const B_BOR_I: u8 = 23;
const B_BXOR_I: u8 = 24;
const B_SHL_I: u8 = 25;
const B_SHR_I: u8 = 26;
```

- [ ] Cranelift: add arms in `emit_binop` in `cranelift/src/lib.rs` before the `_ => panic!` arm:

```rust
        B_BAND_I => (bcx.ins().band(ra, rb), ta),
        B_BOR_I => (bcx.ins().bor(ra, rb), ta),
        B_BXOR_I => (bcx.ins().bxor(ra, rb), ta),
        B_SHL_I => (bcx.ins().ishl(ra, rb), ta),
        B_SHR_I => (bcx.ins().sshr(ra, rb), ta),
```

- [ ] Cranelift C side: extend `binop_code` in `src/codegen_cranelift.c:186-201` before the `default:`:

```c
        case OP_BIT_AND:  if (is_f) cr_unsupported("побитово & върху f64"); return B_BAND_I;
        case OP_BIT_OR:   if (is_f) cr_unsupported("побитово | върху f64"); return B_BOR_I;
        case OP_BIT_XOR:  if (is_f) cr_unsupported("побитово ^ върху f64"); return B_BXOR_I;
        case OP_LSHIFT:   if (is_f) cr_unsupported("<< върху f64"); return B_SHL_I;
        case OP_RSHIFT:   if (is_f) cr_unsupported(">> върху f64"); return B_SHR_I;
```

- [ ] Build everything and run the test:

```bash
make && make llvm && make cranelift
./baga examples/bitwise.baga
```

Expected output: `2 7 5 16 16 24 9 4 16777215`, one per line. `./baga-llvm --emit-llvm examples/bitwise.baga | lli-14 -` prints the same; `./baga-cranelift examples/bitwise.baga` prints the same.

- [ ] Wire into `Makefile` `test` target, after the `vec_ann` block (line 111):

```make
	@echo "=== bitwise ==="
	@./$(BIN) examples/bitwise.baga > /tmp/baga_bitwise_out.txt
	@printf "2\n7\n5\n16\n16\n24\n9\n4\n16777215\n" | diff - /tmp/baga_bitwise_out.txt > /dev/null \
		&& echo "OK: побитови оператори" \
		|| { echo "FAIL: побитови оператори"; exit 1; }
```

- [ ] Run `make test` (this also runs the LLVM and Cranelift oracles, which pick up `examples/bitwise.baga` automatically) — all green.
- [ ] Commit:

```bash
git add include/baga.h src/lexer.c src/parser.c src/codegen_cranelift.c cranelift/baga_clif_rt.h cranelift/src/lib.rs Makefile examples/bitwise.baga
git commit -m "feat: wire bitwise operators &, |, ^, <<, >> through parser and Cranelift"
```

---

## Task 2: `import "path"` — textual inclusion with include guard

**Files:**
- Modify: `include/baga.h` (token enum, keyword block lines 57-75)
- Modify: `src/lexer.c` (`kind_names` lines 38-56; keyword table lines 220-240)
- Modify: `src/main.c` (replace the read+lex block at lines 83-100; add `collect_tokens` above `main`; EOF token handling; the final `free(src)` at line 260 moves into the new helper)
- Modify: `Makefile` (`test` target, after the bitwise block from Task 1)
- Test: `tests/import_lib.baga`, `tests/import_main.baga`, `tests/import_cycle_a.baga`, `tests/import_cycle_b.baga` (create)

**Interfaces:**
- Consumes: existing `read_file` (`src/main.c:6-25`), `lexer_init`/`lexer_next`, `VEC` macros.
- Produces: new token `TOK_IMPORT` (keyword `import`); `static void collect_tokens(const char *path, VEC(Token) *out, VEC(char *) *included, VEC(char *) *stack)` in `main.c`. Semantics: `import "relative/path.baga"` directives are only allowed at the **top of a file**, before any other token; paths resolve relative to the importing file's directory, falling back to the working directory; canonical paths (`realpath`) key the include-once guard; cycles are a hard error. After expansion the parser sees one flat token stream (existing single-namespace behavior; name collisions surface as compile errors exactly as duplicate definitions do today).

**Steps:**

- [ ] Write the failing tests. `tests/import_lib.baga`:

```baga
// import_lib.baga — shared helpers for the import acceptance test.
fn square(x: i64) -> i64 {
    return x * x
}

fn triple(x: i64) -> i64 {
    return x * 3
}
```

`tests/import_main.baga`:

```baga
import "import_lib.baga"
import "import_lib.baga"   // include guard: second import is a no-op

fn main() {
    print(square(7))
    print(triple(7))
}
```

`tests/import_cycle_a.baga`:

```baga
import "import_cycle_b.baga"

fn main() {
    print(1)
}
```

`tests/import_cycle_b.baga`:

```baga
import "import_cycle_a.baga"

fn main() {
    print(2)
}
```

- [ ] Run to see them fail: `./baga tests/import_main.baga` → parse error `очаквах декларация (fn, struct, spec), получих 'import'` (once the keyword exists; before that, `import` lexes as identifier and the error mentions `идентификатор` — either way it fails).
- [ ] Add `TOK_IMPORT` to `include/baga.h` in the keyword block, after `TOK_CONTINUE`:

```c
    TOK_BREAK,
    TOK_CONTINUE,
    TOK_IMPORT,
```

- [ ] Lexer: `kind_names` entry after `[TOK_CONTINUE]` (`src/lexer.c:56`):

```c
    [TOK_CONTINUE] = "continue",
    [TOK_IMPORT]   = "import",
```

- [ ] Lexer: keyword table entry (`src/lexer.c:239`):

```c
        {"continue", TOK_CONTINUE},
        {"import", TOK_IMPORT},
```

- [ ] `main.c`: add the recursive collector above `main` (after the `usage` function). It moves tokens out of each file's local array; `import` is only legal before the first non-import token:

```c
/* import expansion: recursively collect tokens, resolving import "path"
 * directives at the top of each file. The include guard is keyed on the
 * canonical path, so importing the same file twice includes it once.
 * Cycles are an error. */
static void collect_tokens(const char *path, VEC(Token) *out,
                           VEC(char *) *included, VEC(char *) *stack) {
    char *canon = realpath(path, NULL);
    if (!canon) {
        fprintf(stderr, "baga: не мога да отворя '%s': %s\n", path, strerror(errno));
        exit(1);
    }
    for (int i = 0; i < stack->len; i++) {
        if (strcmp(stack->data[i], canon) == 0) {
            fprintf(stderr, "baga: цикличен import: '%s'\n", path);
            exit(1);
        }
    }
    for (int i = 0; i < included->len; i++) {
        if (strcmp(included->data[i], canon) == 0) {
            free(canon);
            return;   /* already included — include guard */
        }
    }
    vec_push(*stack, canon);

    int src_len = 0;
    char *src = read_file(path, &src_len);
    Lexer lexer;
    lexer_init(&lexer, src, src_len, path);
    VEC(Token) ftoks = {0};
    for (;;) {
        Token t = lexer_next(&lexer);
        vec_push(ftoks, t);
        if (t.kind == TOK_EOF) break;
        if (t.kind == TOK_ERROR) {
            baga_error(path, t.pos, "%s", t.text);
            exit(1);
        }
    }

    /* directory of the importing file, for relative resolution */
    char dir[512];
    snprintf(dir, sizeof dir, "%s", path);
    char *slash = strrchr(dir, '/');
    if (slash) *slash = '\0';
    else snprintf(dir, sizeof dir, ".");

    int seen_code = 0;
    for (int i = 0; i < ftoks.len; i++) {
        Token *t = &ftoks.data[i];
        if (t->kind == TOK_EOF) break;
        if (t->kind == TOK_IMPORT) {
            if (seen_code) {
                baga_error(path, t->pos, "import трябва да е в началото на файла");
                exit(1);
            }
            if (i + 1 >= ftoks.len || ftoks.data[i + 1].kind != TOK_STR_LIT) {
                baga_error(path, t->pos, "очаквах низ след import");
                exit(1);
            }
            const char *rel = ftoks.data[i + 1].text;
            char joined[1024];
            char resolved[1024];
            snprintf(joined, sizeof joined, "%s/%s", dir, rel);
            if (realpath(joined, resolved) == NULL &&
                realpath(rel, resolved) == NULL) {
                baga_error(path, t->pos, "не мога да намеря import '%s'", rel);
                exit(1);
            }
            collect_tokens(resolved, out, included, stack);
            i++;   /* consume the path string token */
            continue;
        }
        seen_code = 1;
        vec_push(*out, *t);   /* move the token into the combined stream */
    }

    vec_push(*included, canon);   /* canon stays owned by the guard set */
    stack->len--;
    vec_free(ftoks);              /* token structs' text moved or leaked-by-design */
    free(src);
}
```

- [ ] `main.c`: replace the read+lex block (lines 83-100) with:

```c
    /* read source(s) — import expansion happens here */
    VEC(Token) tokens = {0};
    VEC(char *) included = {0};
    VEC(char *) stack = {0};
    collect_tokens(input_path, &tokens, &included, &stack);

    Token eof;
    memset(&eof, 0, sizeof eof);
    eof.kind = TOK_EOF;
    eof.pos.line = 1;
    eof.pos.col = 1;
    eof.text = strdup("");
    vec_push(tokens, eof);
```

(`VEC(Token) tokens` was previously declared at line 91 — remove that duplicate declaration. The `dump_tokens` block below works unchanged. `realpath` needs no new include — `stdlib.h` comes via `baga.h`; `_POSIX_C_SOURCE 200809L` is already defined at `include/baga.h:4`.)

- [ ] `main.c`: in the cleanup at the end (lines 256-260), remove `free(src);` (source buffers are freed inside `collect_tokens`) and keep the token-text free loop — it now also frees the synthetic EOF's text. Add after `vec_free(tokens);`:

```c
    for (int i = 0; i < included.len; i++) free(included.data[i]);
    vec_free(included);
    vec_free(stack);
```

- [ ] Build and run: `make` && `./baga tests/import_main.baga` → prints `49` then `21`. `./baga tests/import_cycle_a.baga` → exits 1 with `цикличен import` on stderr. Also verify `./baga --emit-c tests/import_main.baga | head` shows both functions once.
- [ ] Wire into `Makefile` `test` target:

```make
	@echo "=== import ==="
	@./$(BIN) tests/import_main.baga > /tmp/baga_import_out.txt
	@printf "49\n21\n" | diff - /tmp/baga_import_out.txt > /dev/null \
		&& echo "OK: import + include guard" \
		|| { echo "FAIL: import"; exit 1; }
	@./$(BIN) tests/import_cycle_a.baga 2>&1 | grep -q "цикличен import" \
		&& echo "OK: import цикълът е хванат" \
		|| { echo "FAIL: import цикълът не е хванат"; exit 1; }
```

- [ ] Run `make test` — all green. (The oracles only scan `examples/*.baga`, so the new `tests/` files do not affect them.)
- [ ] Commit:

```bash
git add include/baga.h src/lexer.c src/main.c Makefile tests/import_lib.baga tests/import_main.baga tests/import_cycle_a.baga tests/import_cycle_b.baga
git commit -m "feat: import \"path\" with include guard and cycle detection"
```

---

## Task 3: `extern fn` declarations (C backend) + write_file via FFI

**Design note (libc symbol choice).** The generated C file includes only `stdio.h`, `stdlib.h`, `stdint.h`, `string.h`. An extern prototype that clashes with a declaration in those headers is a hard C error, so std uses libc symbols **not** declared there: `open`, `read`, `write`, `close` (unistd.h), `socket`/`bind`/`listen`/`accept`/`connect`/`setsockopt` (sys/socket.h), `clock_gettime` (time.h), `getrandom` (sys/random.h), `bzero`/`bcopy` (string**s**.h). The one exception is `getenv` (stdlib.h): it stays compatible because extern prototypes are emitted with C-faithful types — `str` parameter → `const char *`, `str` return → `char *` — which exactly matches the real `char *getenv(const char *)` declaration.

**Files:**
- Modify: `include/baga.h` (token enum keyword block; `NODE_FN` union member at lines 289-295 — add `int is_extern;`)
- Modify: `src/lexer.c` (`kind_names`; keyword table)
- Modify: `src/parser.c` (`parse_program` dispatch at lines 1013-1027)
- Modify: `src/checker.c` (extern type restriction in pass 1, after line 805; user-function-first reorder in `infer_call` at lines 331-338)
- Modify: `src/codegen_c.c` (new helpers `find_extern_fn`, `extern_ret_is_str`, `emit_extern_type`; call-site patch in `emit_expr` `NODE_CALL` at lines 275-349; `emit_forward_decls` at lines 893-922)
- Modify: `src/codegen_llvm.c` (honest refusal in `codegen_llvm`, until Task 10)
- Modify: `src/codegen_cranelift.c` (honest refusal scan in `codegen_cranelift` at lines 723-727)
- Modify: `Makefile` (`test` target)
- Test: `examples/extern_write.baga` (create)

**Interfaces:**
- Consumes: bodyless-function support already in `parse_fn` (`src/parser.c:812-820`) and `check_fn` (`src/checker.c:748`).
- Produces:
  - Syntax: `extern fn socket(domain: i64, typ: i64, proto: i64) -> i64 !Net` — no body; parameter/return types restricted to `i64`, `f64`, `str`, `void`; effects declared and checked as usual.
  - Checker: user-declared functions (including extern) are resolved **before** builtins in `infer_call`, so `extern fn write` shadows the print-style builtin `write`.
  - C backend: extern calls bypass `b_` mangling and the `is_print_call` path; an extern returning `str` is wrapped in a NULL→`""` coercion (GCC statement expression, already used elsewhere in this backend).
  - LLVM/Cranelift: refuse with `llvm_unsupported("extern fn")` / `cr_unsupported("extern fn")` (oracle SKIP).

**Steps:**

- [ ] Write the failing test `examples/extern_write.baga` (flags: `O_WRONLY|O_CREAT|O_TRUNC = 577`, mode `0644 = 420`):

```baga
// extern_write.baga — first real FFI: write a file through libc externs.
extern fn open(path: str, flags: i64, mode: i64) -> i64 !IO
extern fn write(fd: i64, buf: str, count: i64) -> i64 !IO
extern fn close(fd: i64) -> i64 !IO

fn main() -> i64 !IO {
    let fd = open("/tmp/baga_extern_write.txt", 577, 420)?
    if fd < 0 {
        eprintln("open failed")
        exit(1)
    }
    let data = "baga ffi works"
    let w = write(fd, data, len(data))?
    if w != len(data) {
        eprintln("short write")
        exit(1)
    }
    close(fd)
    print("written")
    return 0
}
```

- [ ] Run to see it fail: `./baga examples/extern_write.baga` → parse error (`extern` is not a declaration starter).
- [ ] Add `TOK_EXTERN` to `include/baga.h` after `TOK_IMPORT` (Task 2):

```c
    TOK_IMPORT,
    TOK_EXTERN,
```

- [ ] Add the `is_extern` flag to the `NODE_FN` member of the `Node` union in `include/baga.h` (calloc in `node_alloc` zeroes it):

```c
        /* NODE_FN */
        struct {
            char *fn_name;
            NodeVec params;     /* NODE_PARAM */
            Node *ret_type;     /* NULL → void */
            Node *fn_body;      /* NODE_BLOCK */
            int is_extern;      /* extern fn — no body, links against libc */
        };
```

- [ ] Lexer: `kind_names` entry and keyword table entry:

```c
    [TOK_IMPORT]   = "import",
    [TOK_EXTERN]   = "extern",
```

```c
        {"import", TOK_IMPORT},
        {"extern", TOK_EXTERN},
```

- [ ] Parser: extend the `parse_program` dispatch (`src/parser.c:1013-1027`):

```c
        } else if (check(p, TOK_EXTERN)) {
            advance(p);
            Node *fn = parse_fn(p);
            fn->is_extern = 1;
            if (fn->fn_body)
                parser_error(p, "extern fn '%s' не може да има тяло", fn->fn_name);
            vec_push(prog->items, fn);
        } else if (check(p, TOK_STRUCT)) {
```

- [ ] Checker: reorder `infer_call` so user functions shadow builtins. In `src/checker.c`, inside `if (n->callee->kind == NODE_IDENT)` (line 331), insert before the `print/println/write` block (line 334):

```c
        /* user-defined (incl. extern) functions shadow builtins */
        Type *ft_user = find_fn(ctx, name);
        if (ft_user && ft_user->kind == TYPE_FN) {
            n->callee->type = ft_user;
            if (n->args.len != ft_user->nparams) {
                check_error(ctx, n->pos, "'%s' очаква %d аргумента, получих %d",
                            name, ft_user->nparams, n->args.len);
            }
            int check_n = n->args.len < ft_user->nparams ? n->args.len : ft_user->nparams;
            for (int i = 0; i < check_n; i++) {
                Type *at = n->args.data[i]->type;
                if (!type_assignable(at, ft_user->params[i])) {
                    check_error(ctx, n->pos,
                        "'%s': аргумент #%d е от тип %s, но параметърът е %s",
                        name, i + 1, type_str(at), type_str(ft_user->params[i]));
                }
            }
            Type *ret = ft_user->ret ? ft_user->ret : type_new(TYPE_VOID);
            Type *result = type_new(ret->kind);
            type_merge_effects(result, ret);
            if (ctx->cur_effects)
                type_merge_effects(ctx->cur_effects, ret);
            return result;
        }
```

Then **delete** the now-dead user-function block at lines 428-456 (the `/* user function */` comment through its closing brace, leaving the `check_error(ctx, n->pos, "непозната функция '%s'", name);` fallthrough).

- [ ] Checker: extern type restriction in pass 1 of `check_program`, right after `item->type = ft;` (line 805):

```c
            if (item->is_extern) {
                /* extern fn: params/return restricted to i64, f64, str, void */
                for (int j = 0; j < item->params.len; j++) {
                    Node *pt = item->params.data[j]->param_type;
                    while (pt && pt->kind == NODE_TYPE_EFFECT) pt = pt->inner_type;
                    if (!pt || pt->kind != NODE_TYPE ||
                        (strcmp(pt->type_name, "i64") != 0 &&
                         strcmp(pt->type_name, "f64") != 0 &&
                         strcmp(pt->type_name, "str") != 0 &&
                         strcmp(pt->type_name, "void") != 0))
                        check_error(&ctx, item->pos,
                            "extern fn '%s': неподдържан тип на параметър (само i64, f64, str, void)",
                            item->fn_name);
                }
                Node *rt = item->ret_type;
                while (rt && rt->kind == NODE_TYPE_EFFECT) rt = rt->inner_type;
                if (rt && (rt->kind != NODE_TYPE ||
                    (strcmp(rt->type_name, "i64") != 0 &&
                     strcmp(rt->type_name, "f64") != 0 &&
                     strcmp(rt->type_name, "str") != 0 &&
                     strcmp(rt->type_name, "void") != 0)))
                    check_error(&ctx, item->pos,
                        "extern fn '%s': неподдържан връщан тип (само i64, f64, str, void)",
                        item->fn_name);
            }
```

(`check_fn` needs no change: it already skips NULL bodies, and the empty body-effect accumulator means no "unhandled effect" error for extern declarations.)

- [ ] C codegen: add helpers near the top of `src/codegen_c.c` (after `mangle_name`, line 44):

```c
/* ---- extern fn (FFI) ---- */

/* Find an `extern fn` declaration by baga name, or NULL. */
static Node *find_extern_fn(Codegen *cg, const char *name) {
    if (!cg->program) return NULL;
    for (int i = 0; i < cg->program->items.len; i++) {
        Node *it = cg->program->items.data[i];
        if (it->kind == NODE_FN && it->is_extern &&
            strcmp(it->fn_name, name) == 0)
            return it;
    }
    return NULL;
}

/* Does this extern fn return str? (Effects on the return type are unwrapped.) */
static int extern_ret_is_str(Node *ef) {
    Node *t = ef->ret_type;
    while (t && t->kind == NODE_TYPE_EFFECT) t = t->inner_type;
    return t && t->kind == NODE_TYPE && strcmp(t->type_name, "str") == 0;
}

/* C type for an extern prototype. str params are `const char *`, but a str
 * return is `char *` so the prototype is compatible with libc declarations
 * from the headers we emit (e.g. `char *getenv(const char *)` in stdlib.h). */
static void emit_extern_type(FILE *f, Node *ty, int is_ret) {
    while (ty && ty->kind == NODE_TYPE_EFFECT) ty = ty->inner_type;
    if (!ty) { fprintf(f, "void"); return; }
    if (ty->kind == NODE_TYPE) {
        if (strcmp(ty->type_name, "i64") == 0)       fprintf(f, "int64_t");
        else if (strcmp(ty->type_name, "f64") == 0)  fprintf(f, "double");
        else if (strcmp(ty->type_name, "str") == 0)  fprintf(f, is_ret ? "char *" : "const char *");
        else if (strcmp(ty->type_name, "void") == 0) fprintf(f, "void");
        else                                         fprintf(f, "int64_t");
    } else {
        fprintf(f, "int64_t");
    }
}
```

(`Codegen` is used by `find_extern_fn` before its full definition if placed before `emit_expr` — `Codegen` is defined in `baga.h`, so this is fine anywhere in the file.)

- [ ] C codegen: in `emit_expr`, `NODE_CALL` case (`src/codegen_c.c:275-349`), insert the extern path **before** `is_print_call` (an extern named `write` must not be treated as print). Insert this block immediately after the `case NODE_CALL:` line, before the existing `if (is_print_call(n)) {` — no other change to the case is needed, because the trailing `break;` exits the switch directly from inside the new `if`:

```c
            /* extern fn → direct libc call, no mangling, before builtin dispatch */
            if (n->callee->kind == NODE_IDENT) {
                Node *ef = find_extern_fn(cg, n->callee->name);
                if (ef) {
                    int str_ret = extern_ret_is_str(ef);
                    if (str_ret) fprintf(f, "({ const char *_er = %s(", ef->fn_name);
                    else         fprintf(f, "%s(", ef->fn_name);
                    for (int i = 0; i < n->args.len; i++) {
                        if (i > 0) fprintf(f, ", ");
                        emit_expr(cg, n->args.data[i]);
                    }
                    if (str_ret) fprintf(f, "); _er ? _er : \"\"; })");
                    else         fprintf(f, ")");
                    break;
                }
            }
```

- [ ] C codegen: `emit_forward_decls` (`src/codegen_c.c:893-922`) — emit C-ABI prototypes for extern fns with their raw names, at the top of the loop body:

```c
        if (item->kind != NODE_FN) continue;

        if (item->is_extern) {
            /* extern fn: prototype with the raw C name and C ABI types */
            emit_extern_type(f, item->ret_type, 1);
            fprintf(f, " %s(", item->fn_name);
            if (item->params.len == 0) {
                fprintf(f, "void");
            } else {
                for (int j = 0; j < item->params.len; j++) {
                    if (j > 0) fprintf(f, ", ");
                    emit_extern_type(f, item->params.data[j]->param_type, 0);
                }
            }
            fprintf(f, ");\n");
            continue;
        }
```

(The definition loop at `src/codegen_c.c:1112-1116` already skips bodyless functions, so no C definition is emitted for externs.)

- [ ] LLVM backend: honest refusal until Task 10. In `codegen_llvm` (`src/codegen_llvm.c:1743`), right after `lg.program = program;` (line 1748):

```c
    /* extern fn: отказваме честно до LLVM паритета (вж. llvm_oracle SKIP) */
    for (int i = 0; i < program->items.len; i++) {
        Node *item = program->items.data[i];
        if (item->kind == NODE_FN && item->is_extern)
            llvm_unsupported("extern fn");
    }
```

- [ ] Cranelift backend: honest refusal. In `codegen_cranelift` (`src/codegen_cranelift.c:723`), right after `cg.program = program;` (line 725):

```c
    /* extern fn: FFI е извън Cranelift подмножеството — честен отказ */
    for (int i = 0; i < program->items.len; i++) {
        Node *it = program->items.data[i];
        if (it->kind == NODE_FN && it->is_extern)
            cr_unsupported("extern fn");
    }
```

- [ ] Build and run:

```bash
make && rm -f /tmp/baga_extern_write.txt
./baga examples/extern_write.baga      # prints "written"
cat /tmp/baga_extern_write.txt         # prints "baga ffi works"
./baga --emit-c examples/extern_write.baga | grep -E '^(int64_t (open|write|close)|.*\bopen\()' | head
```

- [ ] Negative test (shell): a bad extern type is rejected —

```bash
printf 'extern fn bad(v: Vec<i64>) -> i64\nfn main() { print(1) }\n' > /tmp/baga_bad_extern.baga
./baga /tmp/baga_bad_extern.baga 2>&1 | grep -q "неподдържан тип на параметър"
```

- [ ] Wire into `Makefile` `test` target:

```make
	@echo "=== extern fn (FFI) ==="
	@rm -f /tmp/baga_extern_write.txt
	@./$(BIN) examples/extern_write.baga | grep -q "written"
	@test "$$(cat /tmp/baga_extern_write.txt)" = "baga ffi works" \
		&& echo "OK: extern fn записва файл" \
		|| { echo "FAIL: extern fn"; exit 1; }
	@printf 'extern fn bad(v: Vec<i64>) -> i64\nfn main() { print(1) }\n' > /tmp/baga_bad_extern.baga
	@./$(BIN) /tmp/baga_bad_extern.baga 2>&1 | grep -q "неподдържан тип на параметър" \
		&& echo "OK: extern fn типовото ограничение е хванато" \
		|| { echo "FAIL: extern fn типовото ограничение"; exit 1; }
```

- [ ] Run `make test` — all green. The LLVM oracle reports `SKIP examples/extern_write.baga (честен отказ)`; the Cranelift oracle likewise.
- [ ] Commit:

```bash
git add include/baga.h src/lexer.c src/parser.c src/checker.c src/codegen_c.c src/codegen_llvm.c src/codegen_cranelift.c Makefile examples/extern_write.baga
git commit -m "feat: extern fn declarations (FFI to libc) in the C backend"
```

---

## Task 4: Arena allocator builtins

**Files:**
- Modify: `src/checker.c` (builtin table at lines 402-415)
- Modify: `src/codegen_c.c` (builtin map at lines 301-319; runtime helpers after `baga_vec_len` at line 1083)
- Modify: `src/codegen_llvm.c` (builtin map at lines 1180-1198; new `build_baga_arena_*` helpers near the vec helpers at lines 405-640; `baga_rt` table at lines 826-861; new `rt_free` near `rt_malloc` at lines 221-224)
- Modify: `Makefile` (`test` target)
- Test: `examples/arena.baga` (create)
- Cranelift: no change — `arena_*` calls already hit `cr_unsupported("вградена функция 'arena_new'")` at `src/codegen_cranelift.c:383-387` (honest refusal, oracle SKIP). Verify this in the test step.

**Interfaces:**
- Consumes: existing builtin registration/emission machinery.
- Produces (baga signatures): `arena_new() -> i64`, `arena_alloc(a: i64, size: i64) -> i64`, `arena_reset(a: i64)`, `arena_free(a: i64)`. The handle and the returned addresses are plain `i64` (pointer-sized, LP64). `arena_alloc` bump-allocates and grows by realloc when full; `arena_reset` rewinds `used` to 0 (all allocations invalidated at once); `arena_free` destroys the arena.

**Steps:**

- [ ] Write the failing test `examples/arena.baga`:

```baga
fn main() {
    let a = arena_new()
    let p1 = arena_alloc(a, 100)
    let p2 = arena_alloc(a, 200)
    print(p2 - p1 >= 100)      // true — bump allocation moves forward
    arena_reset(a)
    let p3 = arena_alloc(a, 50)
    print(p3 == p1)            // true — reset rewinds to the base
    let b = arena_new()
    arena_free(b)
    arena_free(a)
    print("arena ok")
}
```

- [ ] Run to see it fail: `./baga examples/arena.baga` → `непозната функция 'arena_new'`.
- [ ] Checker: add to the generic builtin table in `src/checker.c` (the `builtins[]` array at lines 402-415 — note `vec_*` are handled separately above it, so the arena entries go in this table, after the `eprintln` entry):

```c
            {"eprintln",  TYPE_VOID, 1, 0},
            {"arena_new",   TYPE_I64, 0, 0},
            {"arena_alloc", TYPE_I64, 2, 0},
            {"arena_reset", TYPE_VOID, 1, 0},
            {"arena_free",  TYPE_VOID, 1, 0},
```

- [ ] C codegen: add to `bmap` in `src/codegen_c.c` (after the `vec_len` entry):

```c
                    {"vec_len",     "baga_vec_len"},
                    {"arena_new",   "baga_arena_new"},
                    {"arena_alloc", "baga_arena_alloc"},
                    {"arena_reset", "baga_arena_reset"},
                    {"arena_free",  "baga_arena_free"},
```

- [ ] C codegen: emit the runtime in `codegen_c`, right after the `baga_vec_len` line (line 1083):

```c
    fprintf(out, "\n/* arena allocator: bump allocation, free-all-at-once */\n");
    fprintf(out, "typedef struct { char *base; int64_t used; int64_t cap; } baga_Arena;\n");
    fprintf(out, "static int64_t baga_arena_new(void) {\n");
    fprintf(out, "    baga_Arena *a = malloc(sizeof(baga_Arena));\n");
    fprintf(out, "    a->cap = 65536; a->used = 0; a->base = malloc((size_t)a->cap);\n");
    fprintf(out, "    return (int64_t)(intptr_t)a;\n");
    fprintf(out, "}\n");
    fprintf(out, "static int64_t baga_arena_alloc(int64_t h, int64_t size) {\n");
    fprintf(out, "    baga_Arena *a = (baga_Arena *)(intptr_t)h;\n");
    fprintf(out, "    if (a->used + size > a->cap) {\n");
    fprintf(out, "        int64_t nc = (a->used + size) * 2;\n");
    fprintf(out, "        a->base = realloc(a->base, (size_t)nc); a->cap = nc;\n");
    fprintf(out, "    }\n");
    fprintf(out, "    char *p = a->base + a->used; a->used += size;\n");
    fprintf(out, "    return (int64_t)(intptr_t)p;\n");
    fprintf(out, "}\n");
    fprintf(out, "static void baga_arena_reset(int64_t h) {\n");
    fprintf(out, "    baga_Arena *a = (baga_Arena *)(intptr_t)h; a->used = 0;\n");
    fprintf(out, "}\n");
    fprintf(out, "static void baga_arena_free(int64_t h) {\n");
    fprintf(out, "    baga_Arena *a = (baga_Arena *)(intptr_t)h;\n");
    fprintf(out, "    free(a->base); free(a);\n");
    fprintf(out, "}\n");
```

- [ ] LLVM codegen: add a `baga_Arena` struct type and four builders after the vec helpers (pattern follows `build_baga_vec_grow`/`build_baga_vec_new` at `src/codegen_llvm.c:405-485`), plus `rt_free` next to `rt_malloc` (line 221):

```c
static LLVMValueRef rt_free(void) {
    LLVMTypeRef p[] = { lg.ptr_ty };
    return rt_libc("free", lg.void_ty, p, 1);
}
```

```c
/* ---- arena (mirror на baga_arena_* в codegen_c) ---- */
static LLVMTypeRef baga_arena_ty(void) {
    LLVMTypeRef t = LLVMStructCreateNamed(lg.ctx, "baga_Arena");
    LLVMTypeRef elems[] = { lg.ptr_ty, lg.i64_ty, lg.i64_ty };
    LLVMStructSetBody(t, elems, 3, 0);
    return t;
}
static LLVMTypeRef baga_arena_ptr_ty(void) {
    return LLVMPointerType(baga_arena_ty(), 0);
}
static LLVMValueRef arena_field_ptr(LLVMValueRef a, unsigned idx, const char *nm) {
    return LLVMBuildStructGEP2(lg.builder, baga_arena_ty(), a, idx, nm);
}

/* static int64_t baga_arena_new(void) — handle = pointer към baga_Arena */
static LLVMValueRef build_baga_arena_new(void) {
    LLVMValueRef fn = LLVMAddFunction(lg.mod, "baga_arena_new",
        LLVMFunctionType(lg.i64_ty, NULL, 0, 0));
    h_begin(fn);
    LLVMValueRef sz = LLVMSizeOf(baga_arena_ty());
    LLVMValueRef ma[] = { sz };
    LLVMValueRef raw = h_call(rt_malloc(), ma, 1, "raw");
    LLVMValueRef a = LLVMBuildBitCast(lg.builder, raw, baga_arena_ptr_ty(), "a");
    LLVMBuildStore(lg.builder, LLVMConstInt(lg.i64_ty, 65536, 0),
                   arena_field_ptr(a, 2, "capp"));
    LLVMBuildStore(lg.builder, LLVMConstInt(lg.i64_ty, 0, 0),
                   arena_field_ptr(a, 1, "usedp"));
    LLVMValueRef da[] = { LLVMConstInt(lg.i64_ty, 65536, 0) };
    LLVMValueRef draw = h_call(rt_malloc(), da, 1, "draw");
    LLVMBuildStore(lg.builder, draw, arena_field_ptr(a, 0, "basep"));
    LLVMValueRef h = LLVMBuildPtrToInt(lg.builder, a, lg.i64_ty, "h");
    LLVMBuildRet(lg.builder, h);
    return fn;
}

/* static int64_t baga_arena_alloc(int64_t h, int64_t size) — bump + grow */
static LLVMValueRef build_baga_arena_alloc(void) {
    LLVMTypeRef p[] = { lg.i64_ty, lg.i64_ty };
    LLVMValueRef fn = LLVMAddFunction(lg.mod, "baga_arena_alloc",
        LLVMFunctionType(lg.i64_ty, p, 2, 0));
    h_begin(fn);
    LLVMValueRef a = LLVMBuildIntToPtr(lg.builder, LLVMGetParam(fn, 0),
        baga_arena_ptr_ty(), "a");
    LLVMValueRef size = LLVMGetParam(fn, 1);
    LLVMValueRef usedp = arena_field_ptr(a, 1, "usedp");
    LLVMValueRef capp = arena_field_ptr(a, 2, "capp");
    LLVMValueRef used = LLVMBuildLoad2(lg.builder, lg.i64_ty, usedp, "used");
    LLVMValueRef cap = LLVMBuildLoad2(lg.builder, lg.i64_ty, capp, "cap");
    LLVMValueRef need = LLVMBuildAdd(lg.builder, used, size, "need");
    LLVMValueRef full = LLVMBuildICmp(lg.builder, LLVMIntSGT, need, cap, "full");
    LLVMBasicBlockRef grow_bb = LLVMAppendBasicBlockInContext(lg.ctx, fn, "grow");
    LLVMBasicBlockRef done_bb = LLVMAppendBasicBlockInContext(lg.ctx, fn, "done");
    LLVMBuildCondBr(lg.builder, full, grow_bb, done_bb);
    LLVMPositionBuilderAtEnd(lg.builder, grow_bb);
    LLVMValueRef nc = LLVMBuildMul(lg.builder, need,
        LLVMConstInt(lg.i64_ty, 2, 0), "nc");
    LLVMBuildStore(lg.builder, nc, capp);
    LLVMValueRef base0 = LLVMBuildLoad2(lg.builder, lg.ptr_ty,
        arena_field_ptr(a, 0, "basep"), "base0");
    LLVMValueRef ra[] = { base0, nc };
    LLVMValueRef nd = h_call(rt_realloc(), ra, 2, "nd");
    LLVMBuildStore(lg.builder, nd, arena_field_ptr(a, 0, "basep"));
    LLVMBuildBr(lg.builder, done_bb);
    LLVMPositionBuilderAtEnd(lg.builder, done_bb);
    LLVMValueRef base = LLVMBuildLoad2(lg.builder, lg.ptr_ty,
        arena_field_ptr(a, 0, "basep"), "base");
    LLVMValueRef p = LLVMBuildGEP2(lg.builder, lg.i8_ty, base, &used, 1, "p");
    LLVMBuildStore(lg.builder, need, usedp);
    LLVMValueRef r = LLVMBuildPtrToInt(lg.builder, p, lg.i64_ty, "r");
    LLVMBuildRet(lg.builder, r);
    return fn;
}

/* static void baga_arena_reset(int64_t h) — used = 0 */
static LLVMValueRef build_baga_arena_reset(void) {
    LLVMTypeRef p[] = { lg.i64_ty };
    LLVMValueRef fn = LLVMAddFunction(lg.mod, "baga_arena_reset",
        LLVMFunctionType(lg.void_ty, p, 1, 0));
    h_begin(fn);
    LLVMValueRef a = LLVMBuildIntToPtr(lg.builder, LLVMGetParam(fn, 0),
        baga_arena_ptr_ty(), "a");
    LLVMBuildStore(lg.builder, LLVMConstInt(lg.i64_ty, 0, 0),
                   arena_field_ptr(a, 1, "usedp"));
    LLVMBuildRetVoid(lg.builder);
    return fn;
}

/* static void baga_arena_free(int64_t h) — free(base); free(a) */
static LLVMValueRef build_baga_arena_free(void) {
    LLVMTypeRef p[] = { lg.i64_ty };
    LLVMValueRef fn = LLVMAddFunction(lg.mod, "baga_arena_free",
        LLVMFunctionType(lg.void_ty, p, 1, 0));
    h_begin(fn);
    LLVMValueRef a = LLVMBuildIntToPtr(lg.builder, LLVMGetParam(fn, 0),
        baga_arena_ptr_ty(), "a");
    LLVMValueRef base = LLVMBuildLoad2(lg.builder, lg.ptr_ty,
        arena_field_ptr(a, 0, "basep"), "base");
    LLVMValueRef fa[] = { base };
    h_call(rt_free(), fa, 1, "");
    LLVMValueRef raw = LLVMBuildBitCast(lg.builder, a, lg.ptr_ty, "raw");
    LLVMValueRef fb[] = { raw };
    h_call(rt_free(), fb, 1, "");
    LLVMBuildRetVoid(lg.builder);
    return fn;
}
```

- [ ] LLVM codegen: register in the call-site `bmap` (`src/codegen_llvm.c:1180-1198`, after `vec_len`):

```c
                {"vec_len",     "baga_vec_len"},
                {"arena_new",   "baga_arena_new"},
                {"arena_alloc", "baga_arena_alloc"},
                {"arena_reset", "baga_arena_reset"},
                {"arena_free",  "baga_arena_free"},
```

and in `baga_rt` (`src/codegen_llvm.c:826-861`, after `baga_vec_len`):

```c
    else if (strcmp(name, "baga_vec_len") == 0)     fn = build_baga_vec_len();
    else if (strcmp(name, "baga_arena_new") == 0)   fn = build_baga_arena_new();
    else if (strcmp(name, "baga_arena_alloc") == 0) fn = build_baga_arena_alloc();
    else if (strcmp(name, "baga_arena_reset") == 0) fn = build_baga_arena_reset();
    else if (strcmp(name, "baga_arena_free") == 0)  fn = build_baga_arena_free();
```

- [ ] Build and run all three backends:

```bash
make && make llvm
./baga examples/arena.baga                                   # true, true, arena ok
./baga-llvm --emit-llvm examples/arena.baga > /tmp/arena.ll && lli-14 /tmp/arena.ll
make cranelift && ./baga-cranelift examples/arena.baga       # честен отказ: вградена функция 'arena_new'
```

- [ ] Wire into `Makefile` `test` target:

```make
	@echo "=== arena ==="
	@./$(BIN) examples/arena.baga > /tmp/baga_arena_out.txt
	@printf "true\ntrue\narena ok\n" | diff - /tmp/baga_arena_out.txt > /dev/null \
		&& echo "OK: arena алокатор" \
		|| { echo "FAIL: arena"; exit 1; }
```

- [ ] Run `make test` — all green (Cranelift oracle: `SKIP examples/arena.baga (честен отказ)`).
- [ ] Commit:

```bash
git add src/checker.c src/codegen_c.c src/codegen_llvm.c Makefile examples/arena.baga
git commit -m "feat: arena allocator builtins (arena_new/alloc/reset/free)"
```

---

## Task 5: std `str`, `bytes`, `sort` (pure Baga)

**Files:**
- Create: `std/README.md`, `std/str/str.baga`, `std/str/README.md`, `std/bytes/bytes.baga`, `std/bytes/README.md`, `std/sort/sort.baga`, `std/sort/README.md`
- Test: `tests/std/str_test.baga`, `tests/std/bytes_test.baga`, `tests/std/sort_test.baga` (create)
- No compiler changes.

**Interfaces:**
- Consumes: existing builtins only (`len`, `char_at`, `substr`, `concat`, `chr`, `ord`, `str_eq`, `vec_*`); Task 1 bitwise ops in `bytes`.
- Produces: the `str`/`bytes`/`sort` API surface listed in the normative table above. Conventions: `delim` in `str_split` is a single character; a byte is an `i64` in `0..255` inside a `Vec<i64>`; `str_repeat` builds heap strings that libc externs may later overwrite in place (used as buffers in Tasks 7–8). No effects anywhere (pure modules).

**Steps:**

- [ ] Write the failing tests first. `tests/std/str_test.baga`:

```baga
import "../../std/str/str.baga"

fn check(name: str, ok: bool) {
    if ok {
        print(concat("ok ", name))
    } else {
        eprintln(concat("FAIL ", name))
        exit(1)
    }
}

fn main() {
    check("starts_with", str_starts_with("hello world", "hello"))
    check("starts_with_neg", !str_starts_with("hello", "hello world"))
    check("ends_with", str_ends_with("hello world", "world"))
    check("ends_with_neg", !str_ends_with("hi", "ohi"))
    check("find", str_find("hello world", "world") == 6)
    check("find_missing", str_find("hello", "zzz") == -1)
    check("find_empty", str_find("abc", "") == 0)

    let parts = str_split("a,b,,c", ",")
    check("split_len", vec_len(parts) == 4)
    check("split_0", vec_get(parts, 0) == "a")
    check("split_2", vec_get(parts, 2) == "")
    check("split_3", vec_get(parts, 3) == "c")

    check("join", str_join(parts, "|") == "a|b||c")
    check("replace", str_replace("a-b-c", "-", "+") == "a+b+c")
    check("replace_longer", str_replace("hello", "ll", "LLL") == "heLLLo")
    check("replace_none", str_replace("abc", "z", "q") == "abc")
    check("trim", str_trim("  hi \n\t") == "hi")
    check("trim_all", str_trim("   ") == "")
    check("repeat", str_repeat("ab", 3) == "ababab")
    check("repeat_zero", str_repeat("x", 0) == "")
    check("repeat_len", len(str_repeat(" ", 4096)) == 4096)
    check("parse_int", parse_int("12345") == 12345)
    check("parse_int_neg", parse_int("-42") == -42)
    check("parse_int_trailing", parse_int("12x") == 12)
    check("int_to_str", int_to_str(12345) == "12345")
    check("int_to_str_neg", int_to_str(-42) == "-42")
    check("int_to_str_zero", int_to_str(0) == "0")
    check("roundtrip", parse_int(int_to_str(98765)) == 98765)
    print("str_test: all passed")
}
```

`tests/std/bytes_test.baga`:

```baga
import "../../std/bytes/bytes.baga"

fn check(name: str, ok: bool) {
    if ok {
        print(concat("ok ", name))
    } else {
        eprintln(concat("FAIL ", name))
        exit(1)
    }
}

fn main() {
    let b = bytes_from_str("Hi")
    check("from_str_len", vec_len(b) == 2)
    check("from_str_0", vec_get(b, 0) == 72)
    check("from_str_1", vec_get(b, 1) == 105)

    check("to_hex", bytes_to_hex(b) == "4869")
    check("to_hex_zero", bytes_to_hex(bytes_from_hex("00ff10")) == "00ff10")
    let hf = bytes_from_hex("deadbeef")
    check("from_hex_len", vec_len(hf) == 4)
    check("from_hex_0", vec_get(hf, 0) == 222)
    check("from_hex_3", vec_get(hf, 3) == 239)

    check("b64_empty", base64_encode(vec_new()) == "")
    check("b64_f", base64_encode(bytes_from_str("f")) == "Zg==")
    check("b64_fo", base64_encode(bytes_from_str("fo")) == "Zm8=")
    check("b64_foo", base64_encode(bytes_from_str("foo")) == "Zm9v")
    check("b64_foobar", base64_encode(bytes_from_str("foobar")) == "Zm9vYmFy")
    check("b64_dec_f", bytes_eq(base64_decode("Zg=="), bytes_from_str("f")))
    check("b64_dec_fo", bytes_eq(base64_decode("Zm8="), bytes_from_str("fo")))
    check("b64_dec_foobar", bytes_eq(base64_decode("Zm9vYmFy"), bytes_from_str("foobar")))
    check("b64_roundtrip", bytes_eq(base64_decode(base64_encode(bytes_from_str("baga std"))), bytes_from_str("baga std")))

    check("eq", bytes_eq(bytes_from_str("ab"), bytes_from_str("ab")))
    check("eq_neg", !bytes_eq(bytes_from_str("ab"), bytes_from_str("ac")))
    check("eq_len", !bytes_eq(bytes_from_str("ab"), bytes_from_str("abc")))
    print("bytes_test: all passed")
}
```

`tests/std/sort_test.baga`:

```baga
import "../../std/sort/sort.baga"

fn check(name: str, ok: bool) {
    if ok {
        print(concat("ok ", name))
    } else {
        eprintln(concat("FAIL ", name))
        exit(1)
    }
}

fn main() {
    let v = vec_new()
    vec_push(v, 5)
    vec_push(v, -3)
    vec_push(v, 42)
    vec_push(v, 0)
    vec_push(v, 7)
    vec_push(v, -100)
    sort_i64(v)
    check("sorted", vec_get(v, 0) == -100 && vec_get(v, 1) == -3 && vec_get(v, 2) == 0 && vec_get(v, 3) == 5 && vec_get(v, 4) == 7 && vec_get(v, 5) == 42)
    check("bsearch_hit", binary_search_i64(v, 7) == 4)
    check("bsearch_first", binary_search_i64(v, -100) == 0)
    check("bsearch_last", binary_search_i64(v, 42) == 5)
    check("bsearch_miss", binary_search_i64(v, 6) == -1)

    let e = vec_new()
    sort_i64(e)
    check("empty", vec_len(e) == 0)
    check("bsearch_empty", binary_search_i64(e, 1) == -1)

    let one = vec_new()
    vec_push(one, 9)
    sort_i64(one)
    check("single", vec_get(one, 0) == 9)
    print("sort_test: all passed")
}
```

- [ ] Run to see them fail: `./baga tests/std/str_test.baga` → `не мога да намеря import '../../std/str/str.baga'`.
- [ ] Create `std/str/str.baga` (complete file):

```baga
// str.baga — string utilities. Pure Baga, no effects.
// Built on the len/char_at/substr/concat/chr/ord/str_eq builtins.

fn str_starts_with(s: str, prefix: str) -> bool {
    if len(prefix) > len(s) { return false }
    return substr(s, 0, len(prefix)) == prefix
}

fn str_ends_with(s: str, suffix: str) -> bool {
    if len(suffix) > len(s) { return false }
    return substr(s, len(s) - len(suffix), len(s)) == suffix
}

// Index of the first occurrence of needle, or -1.
fn str_find(s: str, needle: str) -> i64 {
    let n = len(needle)
    if n == 0 { return 0 }
    let mut i: i64 = 0
    while i + n <= len(s) {
        if substr(s, i, i + n) == needle { return i }
        i = i + 1
    }
    return -1
}

// Split on a single-character delimiter. Empty fields are preserved.
fn str_split(s: str, delim: str) -> Vec<str> {
    let out = vec_new()
    let d = char_at(delim, 0)
    let mut start: i64 = 0
    for i in 0..len(s) {
        if char_at(s, i) == d {
            vec_push(out, substr(s, start, i))
            start = i + 1
        }
    }
    vec_push(out, substr(s, start, len(s)))
    return out
}

fn str_join(parts: Vec<str>, sep: str) -> str {
    let mut out = ""
    for i in 0..vec_len(parts) {
        if i > 0 { out = concat(out, sep) }
        out = concat(out, vec_get(parts, i))
    }
    return out
}

// Replace every occurrence of `from` with `to`.
fn str_replace(s: str, from: str, to: str) -> str {
    let n = len(from)
    if n == 0 { return s }
    let mut out = ""
    let mut i: i64 = 0
    while i + n <= len(s) {
        if substr(s, i, i + n) == from {
            out = concat(out, to)
            i = i + n
        } else {
            out = concat(out, substr(s, i, i + 1))
            i = i + 1
        }
    }
    if i < len(s) { out = concat(out, substr(s, i, len(s))) }
    return out
}

// Trim ASCII whitespace (space, tab, LF, CR) from both ends.
fn str_trim(s: str) -> str {
    let mut a: i64 = 0
    let mut b = len(s)
    let mut moving = true
    while a < b && moving {
        let c = char_at(s, a)
        if c == 32 || c == 9 || c == 10 || c == 13 {
            a = a + 1
        } else {
            moving = false
        }
    }
    moving = true
    while b > a && moving {
        let c = char_at(s, b - 1)
        if c == 32 || c == 9 || c == 10 || c == 13 {
            b = b - 1
        } else {
            moving = false
        }
    }
    return substr(s, a, b)
}

// Repeat a string n times (binary doubling — O(log n) concats).
// The result is heap-allocated and may be overwritten in place by libc
// externs that take a `str` buffer (os/net/io rely on this).
fn str_repeat(s: str, n: i64) -> str {
    let mut out = ""
    let mut cur = s
    let mut left = n
    while left > 0 {
        if left % 2 == 1 { out = concat(out, cur) }
        cur = concat(cur, cur)
        left = left / 2
    }
    return out
}

// Parse a base-10 integer; stops at the first non-digit. Leading `-` allowed.
fn parse_int(s: str) -> i64 {
    let mut i: i64 = 0
    let mut sign: i64 = 1
    if len(s) > 0 && char_at(s, 0) == 45 {
        sign = -1
        i = 1
    }
    let mut v: i64 = 0
    while i < len(s) {
        let c = char_at(s, i)
        if c < 48 || c > 57 { return sign * v }
        v = v * 10 + (c - 48)
        i = i + 1
    }
    return sign * v
}

fn int_to_str(n: i64) -> str {
    if n == 0 { return "0" }
    let mut neg = false
    let mut v = n
    if v < 0 {
        neg = true
        v = 0 - v
    }
    let mut out = ""
    while v > 0 {
        out = concat(chr(48 + (v % 10)), out)
        v = v / 10
    }
    if neg { out = concat("-", out) }
    return out
}
```

- [ ] Create `std/bytes/bytes.baga` (complete file):

```baga
// bytes.baga — byte buffers as Vec<i64> holding values 0..255. Pure Baga.
// Hex and Base64 codecs (RFC 4648, standard alphabet, with padding).

fn bytes_from_str(s: str) -> Vec<i64> {
    let out = vec_new()
    for i in 0..len(s) {
        vec_push(out, char_at(s, i))
    }
    return out
}

fn bytes_eq(a: Vec<i64>, b: Vec<i64>) -> bool {
    if vec_len(a) != vec_len(b) { return false }
    for i in 0..vec_len(a) {
        if vec_get(a, i) != vec_get(b, i) { return false }
    }
    return true
}

fn hex_digit(v: i64) -> str {
    if v < 10 { return chr(48 + v) }
    return chr(87 + v)   // 'a' + (v - 10)
}

fn bytes_to_hex(b: Vec<i64>) -> str {
    let mut out = ""
    for i in 0..vec_len(b) {
        let v = vec_get(b, i) & 255
        out = concat(out, hex_digit((v >> 4) & 15))
        out = concat(out, hex_digit(v & 15))
    }
    return out
}

fn hex_val(c: i64) -> i64 {
    if c >= 48 && c <= 57 { return c - 48 }
    if c >= 97 && c <= 102 { return c - 87 }
    if c >= 65 && c <= 70 { return c - 55 }
    return -1
}

// Decodes pairs of hex digits; a trailing half-pair is ignored.
fn bytes_from_hex(h: str) -> Vec<i64> {
    let out = vec_new()
    let mut i: i64 = 0
    while i + 1 < len(h) {
        vec_push(out, hex_val(char_at(h, i)) * 16 + hex_val(char_at(h, i + 1)))
        i = i + 2
    }
    return out
}

fn b64_char(alpha: str, idx: i64) -> str {
    return substr(alpha, idx, idx + 1)
}

fn base64_encode(b: Vec<i64>) -> str {
    let alpha = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/"
    let mut out = ""
    let n = vec_len(b)
    let mut i: i64 = 0
    while i + 3 <= n {
        let v = vec_get(b, i) * 65536 + vec_get(b, i + 1) * 256 + vec_get(b, i + 2)
        out = concat(out, b64_char(alpha, (v >> 18) & 63))
        out = concat(out, b64_char(alpha, (v >> 12) & 63))
        out = concat(out, b64_char(alpha, (v >> 6) & 63))
        out = concat(out, b64_char(alpha, v & 63))
        i = i + 3
    }
    let rem = n - i
    if rem == 1 {
        let v = vec_get(b, i) * 65536
        out = concat(out, b64_char(alpha, (v >> 18) & 63))
        out = concat(out, b64_char(alpha, (v >> 12) & 63))
        out = concat(out, "==")
    }
    if rem == 2 {
        let v = vec_get(b, i) * 65536 + vec_get(b, i + 1) * 256
        out = concat(out, b64_char(alpha, (v >> 18) & 63))
        out = concat(out, b64_char(alpha, (v >> 12) & 63))
        out = concat(out, b64_char(alpha, (v >> 6) & 63))
        out = concat(out, "=")
    }
    return out
}

fn b64_val(c: i64) -> i64 {
    if c >= 65 && c <= 90 { return c - 65 }
    if c >= 97 && c <= 122 { return c - 71 }
    if c >= 48 && c <= 57 { return c + 4 }
    if c == 43 { return 62 }
    if c == 47 { return 63 }
    return 0   // padding and anything else decodes as 0 and is dropped
}

// Input length must be a multiple of 4 (padded output of base64_encode).
fn base64_decode(s: str) -> Vec<i64> {
    let out = vec_new()
    let n = len(s)
    let mut i: i64 = 0
    while i + 4 <= n {
        let c2 = char_at(s, i + 2)
        let c3 = char_at(s, i + 3)
        let v = b64_val(char_at(s, i)) * 262144 + b64_val(char_at(s, i + 1)) * 4096 + b64_val(c2) * 64 + b64_val(c3)
        if c2 == 61 {
            vec_push(out, (v >> 16) & 255)
        } else {
            if c3 == 61 {
                vec_push(out, (v >> 16) & 255)
                vec_push(out, (v >> 8) & 255)
            } else {
                vec_push(out, (v >> 16) & 255)
                vec_push(out, (v >> 8) & 255)
                vec_push(out, v & 255)
            }
        }
        i = i + 4
    }
    return out
}
```

- [ ] Create `std/sort/sort.baga` (complete file):

```baga
// sort.baga — quicksort and binary search for Vec<i64>. Pure Baga.

fn qs_partition(v: Vec<i64>, lo: i64, hi: i64) -> i64 {
    let pivot = vec_get(v, hi)
    let mut i = lo
    for j in lo..hi {
        if vec_get(v, j) <= pivot {
            let tmp = vec_get(v, i)
            vec_set(v, i, vec_get(v, j))
            vec_set(v, j, tmp)
            i = i + 1
        }
    }
    let tmp = vec_get(v, i)
    vec_set(v, i, vec_get(v, hi))
    vec_set(v, hi, tmp)
    return i
}

fn qs_sort(v: Vec<i64>, lo: i64, hi: i64) {
    if lo < hi {
        let p = qs_partition(v, lo, hi)
        qs_sort(v, lo, p - 1)
        qs_sort(v, p + 1, hi)
    }
}

// In-place quicksort, ascending.
fn sort_i64(v: Vec<i64>) {
    if vec_len(v) > 1 {
        qs_sort(v, 0, vec_len(v) - 1)
    }
}

// Index of x in a sorted vector, or -1.
fn binary_search_i64(v: Vec<i64>, x: i64) -> i64 {
    let mut lo: i64 = 0
    let mut hi = vec_len(v) - 1
    while lo <= hi {
        let mid = (lo + hi) / 2
        let m = vec_get(v, mid)
        if m == x { return mid }
        if m < x {
            lo = mid + 1
        } else {
            hi = mid - 1
        }
    }
    return -1
}
```

- [ ] Create `std/README.md`:

```markdown
# std — the Baga standard library

Minimal, zero-dependency standard library. Each library lives in its own
folder and is included with textual imports, e.g.
`import "std/str/str.baga"` (paths resolve relative to the importing file,
then the working directory; every file is included at most once).

| Module  | Contents                                            | Effects      |
|---------|-----------------------------------------------------|--------------|
| str     | split, find, replace, join, trim, repeat, parse_int | pure         |
| bytes   | byte buffers, hex, base64                           | pure         |
| sort    | quicksort, binary search for Vec<i64>               | pure         |
| json    | JSON parser + serializer                            | pure         |
| os      | env, write_file, fd_read/fd_write, mem_i64          | !IO          |
| time    | time_now_ms, monotonic_ms                           | !Time        |
| random  | random_bytes, random_i64                            | !Random      |
| io      | buffered Reader/Writer over fds                     | !IO          |
| net     | tcp_listen/accept/connect/read/write/close          | !Net (+!IO)  |
| crypto  | sha256, hmac_sha256, ct_eq                          | pure         |

## Memory policy

Pure modules follow the language's leak-tolerant default (malloc, never
freed). Programs that need bounded memory use the arena builtins:
`arena_new` / `arena_alloc` / `arena_reset` / `arena_free` — one arena per
request/connection, `arena_reset` (or `arena_free`) at the end of each cycle.
String buffers created by `str_repeat` are heap-allocated and may be
overwritten in place by libc externs that take a `str` buffer (never do this
with string literals).

## Effects policy

Every std function declares its exact effects: `!IO` for file/fd operations,
`!Net` for sockets, `!Random` for randomness, `!Time` for clock reads.
Pure modules have no effects — visible purity in the type.
```

- [ ] Create `std/str/README.md`, `std/bytes/README.md`, `std/sort/README.md` — one short file each: module name, the function signatures from the normative table, one-line semantics per function (copy them from the comments above), "Effects: none (pure). Memory: leak-tolerant."
- [ ] Run the tests:

```bash
./baga tests/std/str_test.baga
./baga tests/std/bytes_test.baga
./baga tests/std/sort_test.baga
```

All print `..._test: all passed` and exit 0. Also sanity-check the LLVM backend on one: `./baga-llvm --emit-llvm tests/std/sort_test.baga | lli-14 -`.
- [ ] Run `make test` — still green (no Makefile change needed yet; these are wired into `make test-std` in Task 11; run them manually in every task until then).
- [ ] Commit:

```bash
git add std/README.md std/str std/bytes std/sort tests/std/str_test.baga tests/std/bytes_test.baga tests/std/sort_test.baga
git commit -m "feat(std): str, bytes and sort libraries with tests"
```

---

## Task 6: std `json` — parser + serializer (pure Baga)

**Files:**
- Create: `std/json/json.baga`, `std/json/README.md`
- Test: `tests/std/json_test.baga` (create)
- No compiler changes.

**Interfaces:**
- Consumes: existing builtins + string `==` (strcmp lowering already exists for `str == str`, `src/codegen_c.c:246-256`).
- Produces: the `JsonDoc` struct, `json_root`, and the `json_*` accessors from the normative table. Representation (the proven parallel-Vec-pools pattern): every node is an `i64` index; `tags`/`nums`/`strs`/`kfrom`/`klen` stay index-aligned; array elements and object key/value pairs live in the flat `kids` pool, sliced by `kfrom[n]`/`klen[n]`. Object keys are string nodes; a pair occupies two consecutive `kids` entries. Numbers keep their **raw text** in `strs[n]` (this is what makes `json_serialize` lossless without an f64→str formatter) and a parsed value in `nums[n]`. Parse errors produce tag `-1`. Parser invariant: after any `jp_value` call, the parsed subtree's root is the **last** node in the pools (`vec_len(p.tags) - 1`) — children are created before their parent, which is why `JsonDoc.root` exists.

**Steps:**

- [ ] Write the failing test `tests/std/json_test.baga`:

```baga
import "../../std/json/json.baga"

fn check(name: str, ok: bool) {
    if ok {
        print(concat("ok ", name))
    } else {
        eprintln(concat("FAIL ", name))
        exit(1)
    }
}

fn main() {
    let d = json_parse("{\"name\":\"baga\",\"version\":1,\"langs\":[\"bg\",\"en\"],\"ok\":true,\"nothing\":null,\"pi\":3.5}")
    let r = json_root(d)
    check("root_obj", json_tag(d, r) == 6)
    check("count", json_count(d, r) == 6)

    let name = json_get(d, r, "name")
    check("name_tag", json_tag(d, name) == 4)
    check("name_val", json_str(d, name) == "baga")

    let ver = json_get(d, r, "version")
    check("version_tag", json_tag(d, ver) == 3)
    check("version_num", json_num(d, ver) == 1.0)

    let pi = json_get(d, r, "pi")
    check("pi_num", json_num(d, pi) == 3.5)

    let langs = json_get(d, r, "langs")
    check("langs_tag", json_tag(d, langs) == 5)
    check("langs_count", json_count(d, langs) == 2)
    check("langs_0", json_str(d, json_at(d, langs, 0)) == "bg")
    check("langs_1", json_str(d, json_at(d, langs, 1)) == "en")

    let ok = json_get(d, r, "ok")
    check("ok_tag", json_tag(d, ok) == 2)
    let nothing = json_get(d, r, "nothing")
    check("null_tag", json_tag(d, nothing) == 0)
    check("missing", json_get(d, r, "nope") == -1)

    // escapes: "a\nb\t\"q\""
    let e = json_parse("{\"s\":\"a\\nb\\t\\\"q\\\"\"}")
    let es = json_get(e, json_root(e), "s")
    check("escape", json_str(e, es) == "a\nb\t\"q\"")

    // \u escape → UTF-8 (U+00E9 = é)
    let u = json_parse("\"\\u00e9\"")
    check("unicode", json_str(u, json_root(u)) == "é")

    // round-trip: parse → serialize → parse → serialize is stable
    let src = "{\"a\":[1,2.5,true,null,\"x\"],\"b\":{\"c\":\"d\"}}"
    let d1 = json_parse(src)
    let s1 = json_serialize(d1, json_root(d1))
    let d2 = json_parse(s1)
    let s2 = json_serialize(d2, json_root(d2))
    check("roundtrip", s1 == s2)
    check("serialize", s1 == src)
    let ea = json_parse("[]")
    check("serialize_empty", json_serialize(ea, json_root(ea)) == "[]")
    let eo = json_parse("{}")
    check("serialize_empty_obj", json_serialize(eo, json_root(eo)) == "{}")

    // error handling: unparseable input → root tag -1
    let bad = json_parse("?")
    check("error_tag", json_tag(bad, json_root(bad)) == -1)
    print("json_test: all passed")
}
```


- [ ] Run to see it fail: `./baga tests/std/json_test.baga` → `не мога да намеря import '../../std/json/json.baga'`.
- [ ] Create `std/json/json.baga` (complete file):

```baga
// json.baga — JSON parser + serializer. Pure Baga, no effects.
//
// Representation: parallel Vec pools (the self/compiler.baga pattern).
// A node is an i64 index; the pools below stay aligned (one push per pool
// per node). Tags: -1 error, 0 null, 1 false, 2 true, 3 number, 4 string,
// 5 array, 6 object.
//   nums[n]          — f64 value for numbers (0.0 otherwise)
//   strs[n]          — raw text for numbers, decoded value for strings
//   kfrom[n]/klen[n] — slice of kids[] for arrays (element nodes) and
//                      objects (key node, value node, ... — keys are strings)
// Parser invariant: after every jp_value call, the parsed subtree's root is
// the LAST node in the pools (children are created before their parent).

struct JsonDoc {
    root: i64,
    tags: Vec<i64>,
    nums: Vec<f64>,
    strs: Vec<str>,
    kids: Vec<i64>,
    kfrom: Vec<i64>,
    klen: Vec<i64>
}

struct JsonParser {
    src: str,
    pos: i64,
    tags: Vec<i64>,
    nums: Vec<f64>,
    strs: Vec<str>,
    kids: Vec<i64>,
    kfrom: Vec<i64>,
    klen: Vec<i64>
}

// ---------- tokenizer / parser ----------

// Return a copy of the parser positioned at `newpos`. Used instead of
// field assignment (p.pos = x), which the LLVM backend does not support.
fn jp_at(p: JsonParser, newpos: i64) -> JsonParser {
    return JsonParser {
        src: p.src, pos: newpos,
        tags: p.tags, nums: p.nums, strs: p.strs,
        kids: p.kids, kfrom: p.kfrom, klen: p.klen
    }
}

// Skip JSON whitespace.
fn jp_ws(p: JsonParser) -> JsonParser {
    let mut running = true
    while running {
        if p.pos >= len(p.src) {
            running = false
        } else {
            let c = char_at(p.src, p.pos)
            if c == 32 || c == 9 || c == 10 || c == 13 {
                p = jp_at(p, p.pos + 1)
            } else {
                running = false
            }
        }
    }
    return p
}

// Push a leaf node (no children).
fn jp_leaf(p: JsonParser, tag: i64, num: f64, txt: str) -> JsonParser {
    vec_push(p.tags, tag)
    vec_push(p.nums, num)
    vec_push(p.strs, txt)
    vec_push(p.kfrom, 0)
    vec_push(p.klen, 0)
    return p
}

// Push a container node whose children were already parsed into `elems`.
fn jp_container(p: JsonParser, tag: i64, elems: Vec<i64>) -> JsonParser {
    let from = vec_len(p.kids)
    for i in 0..vec_len(elems) {
        vec_push(p.kids, vec_get(elems, i))
    }
    vec_push(p.tags, tag)
    vec_push(p.nums, 0.0)
    vec_push(p.strs, "")
    vec_push(p.kfrom, from)
    vec_push(p.klen, vec_len(elems))
    return p
}

// Literal: true / false / null.
fn jp_lit(p: JsonParser, word: str, tag: i64) -> JsonParser {
    if substr(p.src, p.pos, p.pos + len(word)) == word {
        p = jp_at(p, p.pos + len(word))
        return jp_leaf(p, tag, 0.0, "")
    }
    return jp_leaf(p, -1, 0.0, "")
}

fn hex4(s: str) -> i64 {
    let mut v: i64 = 0
    for i in 0..len(s) {
        let c = char_at(s, i)
        let mut d: i64 = 0
        if c >= 48 && c <= 57 { d = c - 48 }
        if c >= 97 && c <= 102 { d = c - 87 }
        if c >= 65 && c <= 70 { d = c - 55 }
        v = v * 16 + d
    }
    return v
}

// Encode a BMP code point as UTF-8 (1-3 bytes).
fn utf8_encode(cp: i64) -> str {
    if cp < 128 { return chr(cp) }
    if cp < 2048 {
        return concat(chr(192 + cp / 64), chr(128 + cp % 64))
    }
    return concat(chr(224 + cp / 4096), concat(chr(128 + (cp / 64) % 64), chr(128 + cp % 64)))
}

// String node. p.pos is at the opening quote. Unterminated strings still
// produce a string node with what was scanned (lenient).
fn jp_string_node(p: JsonParser) -> JsonParser {
    p = jp_at(p, p.pos + 1)
    let mut out = ""
    let mut running = true
    while running {
        if p.pos >= len(p.src) {
            running = false
        } else {
            let c = char_at(p.src, p.pos)
            if c == 34 {
                p = jp_at(p, p.pos + 1)
                running = false
            } else {
                if c == 92 {
                    let e = char_at(p.src, p.pos + 1)
                    if e == 110 { out = concat(out, chr(10)) }
                    if e == 116 { out = concat(out, chr(9)) }
                    if e == 114 { out = concat(out, chr(13)) }
                    if e == 34 { out = concat(out, chr(34)) }
                    if e == 92 { out = concat(out, chr(92)) }
                    if e == 47 { out = concat(out, chr(47)) }
                    if e == 98 { out = concat(out, chr(8)) }
                    if e == 102 { out = concat(out, chr(12)) }
                    if e == 117 {
                        let cp = hex4(substr(p.src, p.pos + 2, p.pos + 6))
                        out = concat(out, utf8_encode(cp))
                        p = jp_at(p, p.pos + 4)
                    }
                    p = jp_at(p, p.pos + 2)
                } else {
                    out = concat(out, substr(p.src, p.pos, p.pos + 1))
                    p = jp_at(p, p.pos + 1)
                }
            }
        }
    }
    return jp_leaf(p, 4, 0.0, out)
}

// Parse an f64: optional '-', integer part, optional fraction, optional
// exponent (e/E with optional sign).
fn json_atof(s: str) -> f64 {
    let mut i: i64 = 0
    let mut sign = 1.0
    if len(s) > 0 && char_at(s, 0) == 45 {
        sign = 0.0 - 1.0
        i = 1
    }
    let mut v = 0.0
    let mut scanning = true
    while i < len(s) && scanning {
        let c = char_at(s, i)
        if c >= 48 && c <= 57 {
            v = v * 10.0 + (c - 48) * 1.0
            i = i + 1
        } else {
            scanning = false
        }
    }
    if i < len(s) && char_at(s, i) == 46 {
        i = i + 1
        let mut div = 10.0
        scanning = true
        while i < len(s) && scanning {
            let c = char_at(s, i)
            if c >= 48 && c <= 57 {
                v = v + (c - 48) * 1.0 / div
                div = div * 10.0
                i = i + 1
            } else {
                scanning = false
            }
        }
    }
    if i < len(s) && (char_at(s, i) == 101 || char_at(s, i) == 69) {
        i = i + 1
        let mut esign: i64 = 1
        if i < len(s) && char_at(s, i) == 45 {
            esign = -1
            i = i + 1
        }
        if i < len(s) && char_at(s, i) == 43 { i = i + 1 }
        let mut exp: i64 = 0
        scanning = true
        while i < len(s) && scanning {
            let c = char_at(s, i)
            if c >= 48 && c <= 57 {
                exp = exp * 10 + (c - 48)
                i = i + 1
            } else {
                scanning = false
            }
        }
        let mut scale = 1.0
        for k in 0..exp {
            scale = scale * 10.0
        }
        if esign < 0 {
            v = v / scale
        } else {
            v = v * scale
        }
    }
    return sign * v
}

// Number node: keeps the raw text (lossless serialization) plus the value.
fn jp_number(p: JsonParser) -> JsonParser {
    let start = p.pos
    let mut running = true
    while running {
        if p.pos >= len(p.src) {
            running = false
        } else {
            let c = char_at(p.src, p.pos)
            if (c >= 48 && c <= 57) || c == 45 || c == 43 || c == 46 || c == 101 || c == 69 {
                p = jp_at(p, p.pos + 1)
            } else {
                running = false
            }
        }
    }
    let raw = substr(p.src, start, p.pos)
    if len(raw) == 0 { return jp_leaf(p, -1, 0.0, "") }
    return jp_leaf(p, 3, json_atof(raw), raw)
}

fn jp_value(p: JsonParser) -> JsonParser {
    p = jp_ws(p)
    if p.pos >= len(p.src) { return jp_leaf(p, -1, 0.0, "") }
    let c = char_at(p.src, p.pos)
    if c == 123 { return jp_object(p) }        // {
    if c == 91 { return jp_array(p) }          // [
    if c == 34 { return jp_string_node(p) }    // "
    if c == 116 { return jp_lit(p, "true", 2) }
    if c == 102 { return jp_lit(p, "false", 1) }
    if c == 110 { return jp_lit(p, "null", 0) }
    return jp_number(p)
}

fn jp_array(p: JsonParser) -> JsonParser {
    p = jp_at(p, p.pos + 1)   // consume [
    let elems = vec_new()
    p = jp_ws(p)
    if p.pos < len(p.src) && char_at(p.src, p.pos) == 93 {   // ]
        p = jp_at(p, p.pos + 1)
        return jp_container(p, 5, elems)
    }
    let mut running = true
    while running {
        p = jp_value(p)
        vec_push(elems, vec_len(p.tags) - 1)
        p = jp_ws(p)
        if p.pos < len(p.src) && char_at(p.src, p.pos) == 44 {   // ,
            p = jp_at(p, p.pos + 1)
        } else {
            running = false
        }
    }
    p = jp_ws(p)
    if p.pos < len(p.src) && char_at(p.src, p.pos) == 93 { p = jp_at(p, p.pos + 1) }
    return jp_container(p, 5, elems)
}

fn jp_object(p: JsonParser) -> JsonParser {
    p = jp_at(p, p.pos + 1)   // consume {
    let elems = vec_new()
    p = jp_ws(p)
    if p.pos < len(p.src) && char_at(p.src, p.pos) == 125 {   // }
        p = jp_at(p, p.pos + 1)
        return jp_container(p, 6, elems)
    }
    let mut running = true
    while running {
        p = jp_ws(p)
        if p.pos < len(p.src) && char_at(p.src, p.pos) == 34 {
            p = jp_string_node(p)                 // key → last node
            let key_node = vec_len(p.tags) - 1
            p = jp_ws(p)
            if p.pos < len(p.src) && char_at(p.src, p.pos) == 58 {   // :
                p = jp_at(p, p.pos + 1)
            }
            p = jp_value(p)                       // value → last node
            vec_push(elems, key_node)
            vec_push(elems, vec_len(p.tags) - 1)
        }
        p = jp_ws(p)
        if p.pos < len(p.src) && char_at(p.src, p.pos) == 44 {
            p = jp_at(p, p.pos + 1)
        } else {
            running = false
        }
    }
    p = jp_ws(p)
    if p.pos < len(p.src) && char_at(p.src, p.pos) == 125 { p = jp_at(p, p.pos + 1) }
    return jp_container(p, 6, elems)
}

// Parse a document. On error, the root's tag is -1.
fn json_parse(s: str) -> JsonDoc {
    let p = JsonParser {
        src: s, pos: 0,
        tags: vec_new(), nums: vec_new(), strs: vec_new(),
        kids: vec_new(), kfrom: vec_new(), klen: vec_new()
    }
    p = jp_ws(p)
    p = jp_value(p)
    let d = JsonDoc {
        root: vec_len(p.tags) - 1,
        tags: p.tags, nums: p.nums, strs: p.strs,
        kids: p.kids, kfrom: p.kfrom, klen: p.klen
    }
    return d
}

// ---------- accessors ----------

fn json_root(d: JsonDoc) -> i64 { return d.root }

fn json_tag(d: JsonDoc, n: i64) -> i64 { return vec_get(d.tags, n) }

fn json_num(d: JsonDoc, n: i64) -> f64 { return vec_get(d.nums, n) }

fn json_str(d: JsonDoc, n: i64) -> str { return vec_get(d.strs, n) }

// Array element count, or object pair count. 0 for leaves.
fn json_count(d: JsonDoc, n: i64) -> i64 {
    let k = vec_get(d.klen, n)
    if vec_get(d.tags, n) == 6 { return k / 2 }
    return k
}

// Array element node at index i.
fn json_at(d: JsonDoc, n: i64, i: i64) -> i64 {
    return vec_get(d.kids, vec_get(d.kfrom, n) + i)
}

// Object key at pair index i.
fn json_key(d: JsonDoc, n: i64, i: i64) -> str {
    let kn = vec_get(d.kids, vec_get(d.kfrom, n) + i * 2)
    return vec_get(d.strs, kn)
}

// Object value node at pair index i.
fn json_val(d: JsonDoc, n: i64, i: i64) -> i64 {
    return vec_get(d.kids, vec_get(d.kfrom, n) + i * 2 + 1)
}

// Object value node for `key`, or -1. Linear scan; first match wins.
fn json_get(d: JsonDoc, n: i64, key: str) -> i64 {
    if n < 0 { return -1 }
    if vec_get(d.tags, n) != 6 { return -1 }
    let cnt = json_count(d, n)
    for i in 0..cnt {
        if json_key(d, n, i) == key { return json_val(d, n, i) }
    }
    return -1
}

// ---------- serializer ----------

fn json_escape(s: str) -> str {
    let mut out = "\""
    for i in 0..len(s) {
        let c = char_at(s, i)
        if c == 34 {
            out = concat(out, "\\\"")
        } else {
            if c == 92 {
                out = concat(out, "\\\\")
            } else {
                if c == 10 {
                    out = concat(out, "\\n")
                } else {
                    if c == 9 {
                        out = concat(out, "\\t")
                    } else {
                        if c == 13 {
                            out = concat(out, "\\r")
                        } else {
                            out = concat(out, substr(s, i, i + 1))
                        }
                    }
                }
            }
        }
    }
    return concat(out, "\"")
}

fn json_serialize(d: JsonDoc, n: i64) -> str {
    let tag = vec_get(d.tags, n)
    if tag == 0 { return "null" }
    if tag == 1 { return "false" }
    if tag == 2 { return "true" }
    if tag == 3 { return vec_get(d.strs, n) }       // raw number text
    if tag == 4 { return json_escape(vec_get(d.strs, n)) }
    if tag == 5 {
        let mut out = "["
        let cnt = vec_get(d.klen, n)
        for i in 0..cnt {
            if i > 0 { out = concat(out, ",") }
            out = concat(out, json_serialize(d, json_at(d, n, i)))
        }
        return concat(out, "]")
    }
    if tag == 6 {
        let mut out = "{"
        let cnt = json_count(d, n)
        for i in 0..cnt {
            if i > 0 { out = concat(out, ",") }
            out = concat(out, json_escape(json_key(d, n, i)))
            out = concat(out, ":")
            out = concat(out, json_serialize(d, json_val(d, n, i)))
        }
        return concat(out, "}")
    }
    return "null"   // error node serializes as null
}
```

- [ ] Create `std/json/README.md`: module name, the `JsonDoc`/`json_root`/`json_*` signatures from the normative table, the tag table (`-1 error, 0 null, 1 false, 2 true, 3 number, 4 string, 5 array, 6 object`), notes: numbers serialize from their raw text; `\u` escapes decode BMP only; the parser is lenient (unterminated strings/containers still yield nodes; a completely unparseable document yields root tag `-1`). "Effects: none (pure). Memory: leak-tolerant."
- [ ] Run: `./baga tests/std/json_test.baga` → prints `json_test: all passed`, exit 0.
- [ ] Run `make test` — still green.
- [ ] Commit:

```bash
git add std/json tests/std/json_test.baga
git commit -m "feat(std): json parser and serializer (pure Baga)"
```

---

## Task 7: std `os`, `time`, `random`, `io` — extern layer + buffered IO

**Files:**
- Create: `std/os/os.baga`, `std/os/README.md`, `std/time/time.baga`, `std/time/README.md`, `std/random/random.baga`, `std/random/README.md`, `std/io/io.baga`, `std/io/README.md`
- Test: `tests/std/os_test.baga`, `tests/std/time_test.baga`, `tests/std/random_test.baga`, `tests/std/io_test.baga` (create)
- No compiler changes.

**Interfaces:**
- Consumes: Task 3 `extern fn`, Task 5 `str_repeat` (writable heap buffers), `parse_int` is **not** needed here.
- Produces: the `os`/`time`/`random`/`io` API from the normative table. Key mechanics:
  - `read(fd, buf, count)` writes into a `str_repeat(" ", n)` heap buffer (never a literal); `substr(buf, 0, got)` trims.
  - `getenv` returning NULL is coerced to `""` by the Task 3 codegen rule, so `env` is total.
  - `mem_i64(s, off)` reconstructs a little-endian u64 from 8 bytes via `char_at` + `<<` + `|` — used to read `struct timespec` fields written by `clock_gettime` into a 16-byte buffer (seconds at offset 0, nanoseconds at offset 8 on LP64 Linux).
  - `fd_write` loops on partial writes.

**Steps:**

- [ ] Write the failing tests. `tests/std/os_test.baga`:

```baga
import "../../std/os/os.baga"

fn check(name: str, ok: bool) {
    if ok {
        print(concat("ok ", name))
    } else {
        eprintln(concat("FAIL ", name))
        exit(1)
    }
}

fn main() -> i64 !IO {
    check("write_file", write_file("/tmp/baga_os_test.txt", "line1\nline2\n") == 0)
    let back = read_file("/tmp/baga_os_test.txt")
    check("read_back", back == "line1\nline2\n")

    // fd layer: O_RDONLY = 0
    let fd = open("/tmp/baga_os_test.txt", 0, 0)?
    check("open_ok", fd >= 0)
    check("fd_read", fd_read(fd, 5) == "line1")
    check("fd_read_rest", fd_read(fd, 100) == "\nline2\n")
    check("fd_read_eof", fd_read(fd, 10) == "")
    close(fd)

    check("fd_write_stdout", fd_write(1, "") == 0)

    // env: PATH virtually always set; a made-up name is not
    check("env_set", len(env("PATH")) > 0)
    check("env_unset", env("BAGA_DEFINITELY_NOT_SET_42") == "")

    // mem_i64: little-endian load
    let b = chr(1)
    let s = concat(b, "aaaaaaa")
    check("mem_i64", mem_i64(s, 0) == 7016996765293437185)
    print("os_test: all passed")
    return 0
}
```

(`mem_i64` check: bytes `01 61 61 61 61 61 61 61` little-endian = 0x6161616161616101 = 7017280452245743361.)

`tests/std/time_test.baga`:

```baga
import "../../std/time/time.baga"

fn check(name: str, ok: bool) {
    if ok {
        print(concat("ok ", name))
    } else {
        eprintln(concat("FAIL ", name))
        exit(1)
    }
}

fn main() -> i64 !Time {
    let now = time_now_ms()?
    check("now_positive", now > 1700000000000)   // after 2023-11-14
    let m1 = monotonic_ms()?
    let mut spin: i64 = 0
    for i in 0..1000000 {
        spin = spin + i
    }
    let m2 = monotonic_ms()?
    check("monotonic_advances", m2 >= m1)
    check("spin", spin >= 0)
    print("time_test: all passed")
    return 0
}
```

`tests/std/random_test.baga`:

```baga
import "../../std/random/random.baga"

fn check(name: str, ok: bool) {
    if ok {
        print(concat("ok ", name))
    } else {
        eprintln(concat("FAIL ", name))
        exit(1)
    }
}

fn main() -> i64 !Random {
    let a = random_bytes(16)?
    let b = random_bytes(16)?
    check("len", vec_len(a) == 16 && vec_len(b) == 16)
    check("range", vec_get(a, 0) >= 0 && vec_get(a, 0) <= 255)
    // 16 random bytes colliding twice in a row is a 2^-128 event
    let mut same: i64 = 0
    for i in 0..16 {
        if vec_get(a, i) == vec_get(b, i) { same = same + 1 }
    }
    check("distinct", same < 16)
    let r = random_i64()?
    check("random_i64", r >= 0 || r < 0)   // just exercise it
    print("random_test: all passed")
    return 0
}
```

`tests/std/io_test.baga`:

```baga
import "../../std/io/io.baga"
import "../../std/os/os.baga"

fn check(name: str, ok: bool) {
    if ok {
        print(concat("ok ", name))
    } else {
        eprintln(concat("FAIL ", name))
        exit(1)
    }
}

fn main() -> i64 !IO {
    // write three lines through a buffered Writer (O_WRONLY|O_CREAT|O_TRUNC = 577)
    let fd = open("/tmp/baga_io_test.txt", 577, 420)?
    let w = writer_new(fd)
    let w2 = write_str(w, "alpha\n")
    let w3 = write_str(w2, "beta\n")
    let w4 = write_str(w3, "gamma\n")
    let w5 = flush(w4)?
    close(w5.fd)

    // read them back line by line
    let rfd = open("/tmp/baga_io_test.txt", 0, 0)?
    let r = reader_new(rfd)
    check("line1", read_line(r)? == "alpha")
    check("line2", read_line(r)? == "beta")
    check("read_n", read_n(r, 3)? == "gam")
    check("read_n_rest", read_n(r, 10)? == "ma\n")
    check("eof", read_line(r)? == "")
    close(rfd)
    print("io_test: all passed")
    return 0
}
```

- [ ] Run to see them fail: `./baga tests/std/os_test.baga` → import not found.
- [ ] Create `std/os/os.baga` (complete file):

```baga
// os.baga — thin wrappers over libc syscalls. Effects: !IO.
//
// Buffer contract: read/getrandom-style externs take a `str` buffer and
// write into it — the buffer must be heap-allocated (built by str_repeat),
// never a string literal.

import "../str/str.baga"

extern fn open(path: str, flags: i64, mode: i64) -> i64 !IO
extern fn close(fd: i64) -> i64 !IO
extern fn read(fd: i64, buf: str, count: i64) -> i64 !IO
extern fn write(fd: i64, buf: str, count: i64) -> i64 !IO
extern fn getenv(name: str) -> str !IO

// Environment variable, or "" when unset (the C backend coerces NULL → "").
fn env(name: str) -> str !IO {
    return getenv(name)?
}

// Write all of `data` to fd, looping on partial writes. Returns 0, or -1.
fn fd_write(fd: i64, data: str) -> i64 !IO {
    let total = len(data)
    let mut off: i64 = 0
    while off < total {
        let w = write(fd, substr(data, off, total), total - off)?
        if w <= 0 {
            close(fd)
            return -1
        }
        off = off + w
    }
    return 0
}

// Read up to n bytes from fd. "" at EOF or on error.
fn fd_read(fd: i64, n: i64) -> str !IO {
    let buf = str_repeat(" ", n)
    let got = read(fd, buf, n)?
    if got <= 0 { return "" }
    return substr(buf, 0, got)
}

// Write data to a file (O_WRONLY|O_CREAT|O_TRUNC = 577, mode 0644 = 420).
// Returns 0 on success, -1 on error.
fn write_file(path: str, data: str) -> i64 !IO {
    let fd = open(path, 577, 420)?
    if fd < 0 { return -1 }
    let rc = fd_write(fd, data)?
    close(fd)
    return rc
}

// Little-endian u64 load from 8 bytes of `s` starting at `off`.
// Used to decode structs filled by libc (e.g. struct timespec).
fn mem_i64(s: str, off: i64) -> i64 {
    let mut v: i64 = 0
    for i in 0..8 {
        v = v | (char_at(s, off + i) << (i * 8))
    }
    return v
}
```

- [ ] Create `std/time/time.baga` (complete file):

```baga
// time.baga — wall clock and monotonic clock via clock_gettime. !Time.
// struct timespec on LP64 Linux: { i64 tv_sec; i64 tv_nsec; } — 16 bytes,
// read back with mem_i64 (os.baga).

import "../os/os.baga"
import "../str/str.baga"

extern fn clock_gettime(clk: i64, ts: str) -> i64 !Time

// clk: 0 = CLOCK_REALTIME, 1 = CLOCK_MONOTONIC. Result in milliseconds.
fn clock_ms(clk: i64) -> i64 !Time {
    let ts = str_repeat(" ", 16)
    let rc = clock_gettime(clk, ts)?
    if rc != 0 { return -1 }
    let sec = mem_i64(ts, 0)
    let nsec = mem_i64(ts, 8)
    return sec * 1000 + nsec / 1000000
}

fn time_now_ms() -> i64 !Time {
    return clock_ms(0)?
}

fn monotonic_ms() -> i64 !Time {
    return clock_ms(1)?
}
```

- [ ] Create `std/random/random.baga` (complete file):

```baga
// random.baga — getrandom(2) wrappers. !Random.

import "../str/str.baga"

extern fn getrandom(buf: str, n: i64, flags: i64) -> i64 !Random

// n random bytes as Vec<i64> (values 0..255).
fn random_bytes(n: i64) -> Vec<i64> !Random {
    let buf = str_repeat(" ", n)
    let got = getrandom(buf, n, 0)?
    let out = vec_new()
    for i in 0..got {
        vec_push(out, char_at(buf, i))
    }
    return out
}

fn random_i64() -> i64 !Random {
    let buf = str_repeat(" ", 8)
    let got = getrandom(buf, 8, 0)?
    if got != 8 { return 0 }
    let mut v: i64 = 0
    for i in 0..8 {
        v = v | (char_at(buf, i) << (i * 8))
    }
    return v
}
```

- [ ] Create `std/io/io.baga` (complete file):

```baga
// io.baga — buffered reader/writer over fd externs (os.baga). !IO.
// Reader buffers one byte at a time (simple, correct); Writer accumulates
// in a string buffer until flush.

import "../os/os.baga"
import "../str/str.baga"

struct Reader {
    fd: i64
}

struct Writer {
    fd: i64,
    buf: str
}

fn reader_new(fd: i64) -> Reader {
    return Reader { fd: fd }
}

// Read one line without the trailing "\n". "" at EOF (also "" for an
// empty line — use read_n when the distinction matters).
fn read_line(r: Reader) -> str !IO {
    let mut line = ""
    let mut running = true
    while running {
        let ch = fd_read(r.fd, 1)?
        if len(ch) == 0 {
            running = false
        } else {
            if ch == "\n" {
                running = false
            } else {
                line = concat(line, ch)
            }
        }
    }
    return line
}

// Read exactly up to n bytes (fewer at EOF).
fn read_n(r: Reader, n: i64) -> str !IO {
    return fd_read(r.fd, n)?
}

fn writer_new(fd: i64) -> Writer {
    return Writer { fd: fd, buf: "" }
}

// Append to the buffer; returns the updated Writer (structs are by value).
fn write_str(w: Writer, s: str) -> Writer {
    return Writer { fd: w.fd, buf: concat(w.buf, s) }
}

// Flush the buffer to the fd and return a cleared Writer.
fn flush(w: Writer) -> Writer !IO {
    fd_write(w.fd, w.buf)?
    return Writer { fd: w.fd, buf: "" }
}
```

- [ ] Create the four READMEs (`std/os/README.md`, `std/time/README.md`, `std/random/README.md`, `std/io/README.md`): signatures from the normative table, one-line semantics, the buffer contract note for `os`, "Effects: !IO / !Time / !Random" respectively, "Memory: buffers are heap strings (leak-tolerant); no arena needed."
- [ ] Run the tests:

```bash
./baga tests/std/os_test.baga
./baga tests/std/time_test.baga
./baga tests/std/random_test.baga
./baga tests/std/io_test.baga
```

All print `..._test: all passed` and exit 0.
- [ ] Run `make test` — still green.
- [ ] Commit:

```bash
git add std/os std/time std/random std/io tests/std/os_test.baga tests/std/time_test.baga tests/std/random_test.baga tests/std/io_test.baga
git commit -m "feat(std): os, time, random and io libraries over extern fn"
```

---

## Task 8: std `net/tcp` — sockets over extern fn

**Files:**
- Create: `std/net/tcp.baga`, `std/net/README.md`
- Test: `tests/std/tcp_test.baga` (create — loopback echo in one process)
- No compiler changes.

**Interfaces:**
- Consumes: Task 3 externs, Task 4 arena (sockaddr staging), `std/os` (`close`, `fd_read`, `fd_write`, `mem_i64` not needed), `std/str` (`str_split`, `parse_int`).
- Produces: the `net/tcp` API from the normative table. Mechanics:
  - `sockaddr_in` (16 bytes) is built in arena memory: `bzero(sa, 16)`, then `bcopy(chr(byte), sa + off, 1)` per byte. `chr(n)` returns a malloc'd 2-byte string whose first byte is `n`, so single-byte `bcopy` always copies exactly one byte — including byte value 0, which baga strings cannot carry through `concat`.
  - IPv4 only. `AF_INET = 2`, `SOCK_STREAM = 1`, `SOL_SOCKET = 1`, `SO_REUSEADDR = 2`.
  - Port and IP octets are stored big-endian (network order): port byte at `sa+2` is `(port >> 8) & 255`, at `sa+3` is `port & 255`.

**Steps:**

- [ ] Write the failing test `tests/std/tcp_test.baga` (single-process loopback: after `listen`, a non-blocking-ish `connect` to 127.0.0.1 completes in the kernel, so `accept` afterwards succeeds — no threads needed):

```baga
import "../../std/net/tcp.baga"
import "../../std/str/str.baga"

fn check(name: str, ok: bool) {
    if ok {
        print(concat("ok ", name))
    } else {
        eprintln(concat("FAIL ", name))
        exit(1)
    }
}

fn main() -> i64 !Net !IO {
    let port = 18431
    let listener = tcp_listen(port)?
    check("listen", listener >= 0)

    let client = tcp_connect("127.0.0.1", port)?
    check("connect", client >= 0)

    let server = tcp_accept(listener)?
    check("accept", server >= 0)

    // echo: client → server → client
    check("write", tcp_write(client, "ping")? == 0)
    let got = tcp_read(server, 64)?
    check("server_got", got == "ping")
    check("reply", tcp_write(server, concat("pong:", got))? == 0)
    check("client_got", tcp_read(client, 64)? == "pong:ping")

    tcp_close(client)?
    tcp_close(server)?
    tcp_close(listener)?
    print("tcp_test: all passed")
    return 0
}
```

- [ ] Run to see it fail: `./baga tests/std/tcp_test.baga` → import not found.
- [ ] Create `std/net/tcp.baga` (complete file):

```baga
// tcp.baga — TCP/IPv4 over libc sockets. !Net (plus !IO for fd operations).
//
// sockaddr_in is 16 bytes staged in an arena: bzero, then one bcopy per
// byte from chr() strings (chr(n) is a malloc'd string whose first byte
// is n — this is how we write byte value 0, which cannot travel through
// baga strings/concat).

import "../os/os.baga"
import "../str/str.baga"

extern fn socket(domain: i64, typ: i64, proto: i64) -> i64 !Net
extern fn setsockopt(fd: i64, level: i64, opt: i64, val: i64, len: i64) -> i64 !Net
extern fn bind(fd: i64, addr: i64, addrlen: i64) -> i64 !Net
extern fn listen(fd: i64, backlog: i64) -> i64 !Net
extern fn accept(fd: i64, addr: i64, addrlen: i64) -> i64 !Net
extern fn connect(fd: i64, addr: i64, addrlen: i64) -> i64 !Net
extern fn bzero(dst: i64, n: i64)
extern fn bcopy(src: str, dst: i64, n: i64)

// Write one byte into raw memory. buf/off are arena addresses (i64).
fn poke8(buf: i64, off: i64, v: i64) {
    bcopy(chr(v & 255), buf + off, 1)
}

// Build a sockaddr_in in `sa` (16 arena bytes): AF_INET, port, 4 ip octets.
fn make_sockaddr(sa: i64, port: i64, o1: i64, o2: i64, o3: i64, o4: i64) {
    bzero(sa, 16)
    poke8(sa, 0, 2)                  // AF_INET (little-endian u16: 02 00)
    poke8(sa, 2, (port >> 8) & 255)  // port, big-endian
    poke8(sa, 3, port & 255)
    poke8(sa, 4, o1)
    poke8(sa, 5, o2)
    poke8(sa, 6, o3)
    poke8(sa, 7, o4)
}

// Set SO_REUSEADDR so quick test restarts can rebind.
fn set_reuseaddr(a: i64, s: i64) -> i64 !Net {
    let opt = arena_alloc(a, 4)
    bzero(opt, 4)
    poke8(opt, 0, 1)
    return setsockopt(s, 1, 2, opt, 4)?
}

// Listen on 0.0.0.0:port. Returns the listener fd, or -1.
fn tcp_listen(port: i64) -> i64 !Net {
    let a = arena_new()
    let sa = arena_alloc(a, 16)
    make_sockaddr(sa, port, 0, 0, 0, 0)
    let s = socket(2, 1, 0)?
    if s < 0 {
        arena_free(a)
        return -1
    }
    set_reuseaddr(a, s)?
    let rb = bind(s, sa, 16)?
    if rb != 0 {
        arena_free(a)
        return -1
    }
    let rl = listen(s, 128)?
    arena_free(a)
    if rl != 0 { return -1 }
    return s
}

// Accept one connection (blocking). Returns the connection fd, or -1.
fn tcp_accept(listener: i64) -> i64 !Net {
    return accept(listener, 0, 0)?
}

// Connect to host ("a.b.c.d" dotted IPv4):port. Returns the fd, or -1.
fn tcp_connect(host: str, port: i64) -> i64 !Net {
    let parts = str_split(host, ".")
    if vec_len(parts) != 4 { return -1 }
    let a = arena_new()
    let sa = arena_alloc(a, 16)
    make_sockaddr(sa, port,
        parse_int(vec_get(parts, 0)) & 255,
        parse_int(vec_get(parts, 1)) & 255,
        parse_int(vec_get(parts, 2)) & 255,
        parse_int(vec_get(parts, 3)) & 255)
    let s = socket(2, 1, 0)?
    if s < 0 {
        arena_free(a)
        return -1
    }
    let rc = connect(s, sa, 16)?
    arena_free(a)
    if rc != 0 { return -1 }
    return s
}

fn tcp_read(fd: i64, n: i64) -> str !Net !IO {
    return fd_read(fd, n)?
}

fn tcp_write(fd: i64, data: str) -> i64 !Net !IO {
    return fd_write(fd, data)?
}

fn tcp_close(fd: i64) -> i64 !IO {
    return close(fd)
}
```

- [ ] Create `std/net/README.md`: signatures from the normative table; notes: IPv4 only, blocking sockets, `sockaddr_in` staging via arena + `poke8`, no TLS (v2). "Effects: !Net (+!IO). Memory: sockaddr staging uses a short-lived arena per call; payload buffers are heap strings."
- [ ] Run: `./baga tests/std/tcp_test.baga` → prints `tcp_test: all passed`, exit 0.
- [ ] Run `make test` — still green.
- [ ] Commit:

```bash
git add std/net tests/std/tcp_test.baga
git commit -m "feat(std): tcp networking over extern fn (loopback echo test)"
```

---

## Task 9: std `crypto` — SHA-256, HMAC-SHA256, constant-time equality (pure Baga)

**Files:**
- Create: `std/crypto/sha256.baga`, `std/crypto/hmac.baga`, `std/crypto/ct.baga`, `std/crypto/README.md`
- Test: `tests/std/sha256_test.baga`, `tests/std/hmac_test.baga` (create; `ct_eq` is exercised inside both)
- No compiler changes. Uses Task 1 bitwise ops (`&`, `|`, `^`, `<<`, `>>`) and Task 5 `bytes`.

**Interfaces:**
- Consumes: `std/bytes/bytes.baga` (`bytes_from_str`, `bytes_to_hex`, `bytes_eq`), `std/str/str.baga` (`str_repeat` for the 1M-byte test).
- Produces: the `crypto` API from the normative table. u32 arithmetic is emulated with i64 + mask `& 4294967295`; bitwise NOT is `(-1) ^ x` (there is no `~` in Baga). Every intermediate sum stays below 2^36, well inside i64.

**Steps:**

- [ ] Write the failing tests. `tests/std/sha256_test.baga` (NIST vectors):

```baga
import "../../std/crypto/sha256.baga"
import "../../std/crypto/ct.baga"
import "../../std/str/str.baga"

fn check(name: str, ok: bool) {
    if ok {
        print(concat("ok ", name))
    } else {
        eprintln(concat("FAIL ", name))
        exit(1)
    }
}

fn main() {
    check("abc", sha256_hex("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad")
    check("empty", sha256_hex("") == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855")
    check("56_bytes", sha256_hex("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq") == "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1")

    // 1,000,000 × 'a' (optional but cheap with str_repeat doubling)
    let big = str_repeat("a", 1000000)
    check("million_a", sha256_hex(big) == "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0")

    // digest shape + ct_eq over raw bytes
    let d = sha256("abc")
    check("digest_len", vec_len(d) == 32)
    check("ct_eq", ct_eq_bytes(d, sha256("abc")))
    check("ct_eq_neg", !ct_eq_bytes(d, sha256("abd")))
    check("ct_eq_str", ct_eq("hello", "hello"))
    check("ct_eq_str_neg", !ct_eq("hello", "hellp"))
    check("ct_eq_str_len", !ct_eq("hello", "hell"))
    print("sha256_test: all passed")
}
```

`tests/std/hmac_test.baga` (RFC 4231 test cases 1 and 2):

```baga
import "../../std/crypto/hmac.baga"
import "../../std/str/str.baga"

fn check(name: str, ok: bool) {
    if ok {
        print(concat("ok ", name))
    } else {
        eprintln(concat("FAIL ", name))
        exit(1)
    }
}

fn main() {
    // RFC 4231 test case 1: key = 0x0b × 20, data = "Hi There"
    let key1 = str_repeat(chr(11), 20)
    check("rfc4231_1", hmac_sha256_hex(key1, "Hi There") == "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7")

    // RFC 4231 test case 2: key = "Jefe", data = "what do ya want for nothing?"
    check("rfc4231_2", hmac_sha256_hex("Jefe", "what do ya want for nothing?") == "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843")

    // sanity: different keys differ
    check("keys_differ", hmac_sha256_hex("k1", "msg") != hmac_sha256_hex("k2", "msg"))
    print("hmac_test: all passed")
}
```

- [ ] Run to see them fail: `./baga tests/std/sha256_test.baga` → import not found.
- [ ] Create `std/crypto/sha256.baga` (complete file, FIPS 180-4):

```baga
// sha256.baga — SHA-256 (FIPS 180-4). Pure Baga, no effects.
// u32 arithmetic is emulated with i64 masked to 32 bits (u32()); bitwise
// NOT is (-1) ^ x. All intermediate sums stay below 2^36 (fits i64).

import "../bytes/bytes.baga"

fn u32(x: i64) -> i64 {
    return x & 4294967295
}

fn rotr(x: i64, n: i64) -> i64 {
    return u32((x >> n) | u32(x << (32 - n)))
}

// Round constants K (first 32 bits of the fractional parts of the cube
// roots of the first 64 primes).
fn sha256_k() -> Vec<i64> {
    let k = vec_new()
    vec_push(k, 1116352408)
    vec_push(k, 1899447441)
    vec_push(k, 3049323471)
    vec_push(k, 3921009573)
    vec_push(k, 961987163)
    vec_push(k, 1508970993)
    vec_push(k, 2453635748)
    vec_push(k, 2870763221)
    vec_push(k, 3624381080)
    vec_push(k, 310598401)
    vec_push(k, 607225278)
    vec_push(k, 1426881987)
    vec_push(k, 1925078388)
    vec_push(k, 2162078206)
    vec_push(k, 2614888103)
    vec_push(k, 3248222580)
    vec_push(k, 3835390401)
    vec_push(k, 4022224774)
    vec_push(k, 264347078)
    vec_push(k, 604807628)
    vec_push(k, 770255983)
    vec_push(k, 1249150122)
    vec_push(k, 1555081692)
    vec_push(k, 1996064986)
    vec_push(k, 2554220882)
    vec_push(k, 2821834349)
    vec_push(k, 2952996808)
    vec_push(k, 3210313671)
    vec_push(k, 3336571891)
    vec_push(k, 3584528711)
    vec_push(k, 113926993)
    vec_push(k, 338241895)
    vec_push(k, 666307205)
    vec_push(k, 773529912)
    vec_push(k, 1294757372)
    vec_push(k, 1396182291)
    vec_push(k, 1695183700)
    vec_push(k, 1986661051)
    vec_push(k, 2177026350)
    vec_push(k, 2456956037)
    vec_push(k, 2730485921)
    vec_push(k, 2820302411)
    vec_push(k, 3259730800)
    vec_push(k, 3345764771)
    vec_push(k, 3516065817)
    vec_push(k, 3600352804)
    vec_push(k, 4094571909)
    vec_push(k, 275423344)
    vec_push(k, 430227734)
    vec_push(k, 506948616)
    vec_push(k, 659060556)
    vec_push(k, 883997877)
    vec_push(k, 958139571)
    vec_push(k, 1322822218)
    vec_push(k, 1537002063)
    vec_push(k, 1747873779)
    vec_push(k, 1955562222)
    vec_push(k, 2024104815)
    vec_push(k, 2227730452)
    vec_push(k, 2361852424)
    vec_push(k, 2428436474)
    vec_push(k, 2756734187)
    vec_push(k, 3204031479)
    vec_push(k, 3329325298)
    return k
}

// Append the 4 big-endian bytes of a u32 word to `out`.
fn push_word(out: Vec<i64>, w: i64) {
    vec_push(out, (w >> 24) & 255)
    vec_push(out, (w >> 16) & 255)
    vec_push(out, (w >> 8) & 255)
    vec_push(out, w & 255)
}

// SHA-256 over a byte vector (values 0..255). Returns the 32-byte digest.
fn sha256_bytes(data: Vec<i64>) -> Vec<i64> {
    // ---- padding: 0x80, zeros to 56 mod 64, 64-bit big-endian length ----
    let msg = vec_new()
    let n = vec_len(data)
    for i in 0..n {
        vec_push(msg, vec_get(data, i) & 255)
    }
    let bitlen = n * 8
    vec_push(msg, 128)
    while (vec_len(msg) % 64) != 56 {
        vec_push(msg, 0)
    }
    for i in 0..8 {
        vec_push(msg, (bitlen >> ((7 - i) * 8)) & 255)
    }

    // ---- initial hash state ----
    let mut h0: i64 = 1779033703
    let mut h1: i64 = 3144134277
    let mut h2: i64 = 1013904242
    let mut h3: i64 = 2773480762
    let mut h4: i64 = 1359893119
    let mut h5: i64 = 2600822924
    let mut h6: i64 = 528734635
    let mut h7: i64 = 1541459225

    let k = sha256_k()
    let total = vec_len(msg)
    let mut block: i64 = 0
    while block < total {
        // ---- message schedule ----
        let w = vec_new()
        for t in 0..16 {
            let o = block + t * 4
            let word = (vec_get(msg, o) << 24) | (vec_get(msg, o + 1) << 16) | (vec_get(msg, o + 2) << 8) | vec_get(msg, o + 3)
            vec_push(w, u32(word))
        }
        for t in 16..64 {
            let x = vec_get(w, t - 15)
            let s0 = u32(rotr(x, 7) ^ rotr(x, 18) ^ (x >> 3))
            let y = vec_get(w, t - 2)
            let s1 = u32(rotr(y, 17) ^ rotr(y, 19) ^ (y >> 10))
            vec_push(w, u32(vec_get(w, t - 16) + s0 + vec_get(w, t - 7) + s1))
        }

        // ---- compression: 64 rounds ----
        let mut a = h0
        let mut b = h1
        let mut c = h2
        let mut dd = h3
        let mut e = h4
        let mut f = h5
        let mut g = h6
        let mut hh = h7
        for t in 0..64 {
            let s1 = u32(rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25))
            let ch = u32((e & f) ^ (u32((-1) ^ e) & g))
            let t1 = u32(hh + s1 + ch + vec_get(k, t) + vec_get(w, t))
            let s0 = u32(rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22))
            let maj = u32((a & b) ^ (a & c) ^ (b & c))
            let t2 = u32(s0 + maj)
            hh = g
            g = f
            f = e
            e = u32(dd + t1)
            dd = c
            c = b
            b = a
            a = u32(t1 + t2)
        }
        h0 = u32(h0 + a)
        h1 = u32(h1 + b)
        h2 = u32(h2 + c)
        h3 = u32(h3 + dd)
        h4 = u32(h4 + e)
        h5 = u32(h5 + f)
        h6 = u32(h6 + g)
        h7 = u32(h7 + hh)
        block = block + 64
    }

    // ---- digest: 8 words, big-endian ----
    let out = vec_new()
    push_word(out, h0)
    push_word(out, h1)
    push_word(out, h2)
    push_word(out, h3)
    push_word(out, h4)
    push_word(out, h5)
    push_word(out, h6)
    push_word(out, h7)
    return out
}

// SHA-256 over a string (its raw bytes). Returns the 32-byte digest.
fn sha256(msg: str) -> Vec<i64> {
    return sha256_bytes(bytes_from_str(msg))
}

// Lowercase hex digest of a string.
fn sha256_hex(msg: str) -> str {
    return bytes_to_hex(sha256(msg))
}
```

- [ ] Create `std/crypto/hmac.baga` (complete file, RFC 2104):

```baga
// hmac.baga — HMAC-SHA256 (RFC 2104) over sha256.baga. Pure Baga.

import "sha256.baga"
import "../bytes/bytes.baga"

// HMAC-SHA256 with a string key and message. Returns the 32-byte MAC.
fn hmac_sha256(key: str, msg: str) -> Vec<i64> {
    let mut k = bytes_from_str(key)
    if vec_len(k) > 64 {
        k = sha256_bytes(k)
    }
    while vec_len(k) < 64 {
        vec_push(k, 0)
    }
    let inner = vec_new()
    let outer = vec_new()
    for i in 0..64 {
        vec_push(inner, vec_get(k, i) ^ 54)   // 0x36 = ipad
        vec_push(outer, vec_get(k, i) ^ 92)   // 0x5c = opad
    }
    let m = bytes_from_str(msg)
    for i in 0..vec_len(m) {
        vec_push(inner, vec_get(m, i))
    }
    let ihash = sha256_bytes(inner)
    for i in 0..32 {
        vec_push(outer, vec_get(ihash, i))
    }
    return sha256_bytes(outer)
}

fn hmac_sha256_hex(key: str, msg: str) -> str {
    return bytes_to_hex(hmac_sha256(key, msg))
}
```

- [ ] Create `std/crypto/ct.baga` (complete file):

```baga
// ct.baga — constant-time equality for MAC/signature comparison.
// Comparison time depends on length only, never on content. Pure Baga.

fn ct_eq(a: str, b: str) -> bool {
    if len(a) != len(b) { return false }
    let mut diff: i64 = 0
    for i in 0..len(a) {
        diff = diff | (char_at(a, i) ^ char_at(b, i))
    }
    return diff == 0
}

fn ct_eq_bytes(a: Vec<i64>, b: Vec<i64>) -> bool {
    if vec_len(a) != vec_len(b) { return false }
    let mut diff: i64 = 0
    for i in 0..vec_len(a) {
        diff = diff | (vec_get(a, i) ^ vec_get(b, i))
    }
    return diff == 0
}
```

- [ ] Create `std/crypto/README.md`: signatures from the normative table; notes: pure Baga (no OpenSSL FFI, deliberate — zero dependencies and proof-of-language); SHA-256 validated against NIST vectors, HMAC against RFC 4231; u32-via-i64 masking; ChaCha20-Poly1305 and TLS are explicitly v2. "Effects: none (pure). Memory: leak-tolerant."
- [ ] Run the tests:

```bash
./baga tests/std/sha256_test.baga
./baga tests/std/hmac_test.baga
```

Both print `..._test: all passed` and exit 0.
- [ ] Run `make test` — still green.
- [ ] Commit:

```bash
git add std/crypto tests/std/sha256_test.baga tests/std/hmac_test.baga
git commit -m "feat(std): sha256, hmac-sha256 and constant-time equality"
```

---

## Task 10: LLVM parity for `extern fn` (plus `str == str`, needed by std)

**Files:**
- Modify: `src/codegen_llvm.c` (remove the Task 3 refusal scan in `codegen_llvm`; `predeclare_fn_llvm` at lines 1602-1613; `NODE_CALL` case at lines 1172-1245; new `extern_ret_is_str_llvm` helper; `NODE_BINARY` string comparison at lines 1123-1127)
- No other files.

**Interfaces:**
- Consumes: Task 3 `is_extern` flag and type restriction; `lg.program`; `fn_type_of` (`src/codegen_llvm.c:1581-1592`), which already handles `NODE_TYPE_EFFECT` returns via `llvm_type`; the existing `baga_str_eq` runtime helper.
- Produces: extern functions declared in the LLVM module under their **raw** C names (no `b_` mangling); calls resolve to them before the print/builtin dispatch; a `str` return is wrapped in `select(isnull, "", result)` — the same NULL→`""` semantics as the C backend. lli-14 resolves the libc symbols dynamically. Additionally, `str == str` / `str != str` lower to `baga_str_eq` (mirroring `codegen_c.c:246-256`) — the std library compares strings pervasively, and without this every std test refuses under LLVM. (Cranelift keeps its honest refusal — string comparison is outside its subset.)

**Steps:**

- [ ] Write the failing check (shell):

```bash
make llvm
./baga-llvm --emit-llvm examples/extern_write.baga > /tmp/baga_extern.ll
```

Fails with `baga: LLVM backend: неподдържан конструкт 'extern fn'` (the Task 3 refusal).

- [ ] Remove the Task 3 refusal scan from `codegen_llvm` (the `for` loop with `llvm_unsupported("extern fn")` right after `lg.program = program;`).
- [ ] Implement `str == str` in `emit_expr_llvm`, `NODE_BINARY` case — replace the `llvm_unsupported("сравнение на низове")` branch (`src/codegen_llvm.c:1123-1127`) with:

```c
            /* str == str / str != str — чрез baga_str_eq (като codegen_c: strcmp) */
            if ((n->bin_op == OP_EQ || n->bin_op == OP_NEQ) &&
                n->left->type && n->right->type &&
                n->left->type->kind == TYPE_STR && n->right->type->kind == TYPE_STR) {
                LLVMValueRef l = emit_expr_llvm(n->left);
                LLVMValueRef r = emit_expr_llvm(n->right);
                if (!l || !r) llvm_unsupported("print в сравнение на низове");
                LLVMValueRef sc = baga_rt("baga_str_eq");
                LLVMValueRef args[] = { l, r };
                char *nm = tmp_name();
                LLVMValueRef eq = h_call(sc, args, 2, nm);
                free(nm);
                /* baga_str_eq връща i64 0/1 → bool i1 */
                char *nm2 = tmp_name();
                LLVMValueRef v = LLVMBuildICmp(lg.builder, LLVMIntNE, eq,
                    LLVMConstInt(lg.i64_ty, 0, 0), nm2);
                free(nm2);
                if (n->bin_op == OP_NEQ) {
                    char *nm3 = tmp_name();
                    v = LLVMBuildNot(lg.builder, v, nm3);
                    free(nm3);
                }
                return v;
            }
```

- [ ] Add the helper near `find_ensures_spec_llvm` (`src/codegen_llvm.c:1526`):

```c
/* Does this extern fn return str? (Effects on the return type are unwrapped.)
 * Mirror на extern_ret_is_str в codegen_c. */
static int extern_ret_is_str_llvm(Node *ef) {
    Node *t = ef->ret_type;
    while (t && t->kind == NODE_TYPE_EFFECT) t = t->inner_type;
    return t && t->kind == NODE_TYPE && strcmp(t->type_name, "str") == 0;
}
```

- [ ] Patch `predeclare_fn_llvm` (`src/codegen_llvm.c:1602-1613`) — insert right after `LLVMTypeRef fn_ty = fn_type_of(fn, NULL, NULL);`:

```c
    if (fn->is_extern) {
        /* extern fn: declare with the raw C name (no b_ mangling) */
        LLVMAddFunction(lg.mod, fn->fn_name, fn_ty);
        return;
    }
```

- [ ] Patch the `NODE_CALL` case in `emit_expr_llvm` (`src/codegen_llvm.c:1172-1245`). Replace its opening:

```c
        case NODE_CALL: {
            /* extern fn → raw libc symbol, before the print/builtin dispatch
             * (an extern named `write` must not become baga_write) */
            Node *ef = NULL;
            if (n->callee->kind == NODE_IDENT) {
                for (int i = 0; lg.program && i < lg.program->items.len; i++) {
                    Node *it = lg.program->items.data[i];
                    if (it->kind == NODE_FN && it->is_extern &&
                        strcmp(it->fn_name, n->callee->name) == 0) {
                        ef = it;
                        break;
                    }
                }
            }
            if (!ef && is_print_call_llvm(n)) {
                emit_print_llvm(n);
                return NULL; /* print е void */
            }
```

Guard the typed-vec dispatch and the bmap loop with `!ef`:

```c
            if (!ef &&
                (strcmp(n->callee->name, "vec_push") == 0 ||
                 strcmp(n->callee->name, "vec_get") == 0 ||
                 strcmp(n->callee->name, "vec_set") == 0)) {
```

```c
            for (int bi = 0; !ef && !fn && bi < (int)(sizeof(bmap) / sizeof(bmap[0])); bi++) {
```

Resolve the extern symbol after `LLVMValueRef fn = NULL;`:

```c
            LLVMValueRef fn = NULL;
            if (ef) fn = LLVMGetNamedFunction(lg.mod, ef->fn_name);
```

And add the NULL→`""` coercion right after `LLVMValueRef result = LLVMBuildCall2(...);` (before `free(name); free(args);`):

```c
            if (ef && extern_ret_is_str_llvm(ef)) {
                /* str return: NULL → "" (като codegen_c — getenv е тотална) */
                LLVMValueRef empty = LLVMBuildGlobalStringPtr(lg.builder, "", "empty");
                LLVMValueRef isnull = LLVMBuildIsNull(lg.builder, result, "isnull");
                result = LLVMBuildSelect(lg.builder, isnull, empty, result, "nn");
            }
```

- [ ] Build and verify parity on the FFI example:

```bash
make llvm
rm -f /tmp/baga_extern_write.txt
./baga-llvm --emit-llvm examples/extern_write.baga > /tmp/baga_extern.ll
lli-14 /tmp/baga_extern.ll                       # prints "written"
test "$(cat /tmp/baga_extern_write.txt)" = "baga ffi works" && echo PARITY-OK
```

- [ ] Verify `str == str` parity (used by every std module):

```bash
printf 'fn main() {\n    print("abc" == "abc")\n    print("abc" == "abd")\n    print("abc" != "abd")\n}\n' > /tmp/baga_streq.baga
./baga /tmp/baga_streq.baga                                        # true false true
./baga-llvm --emit-llvm /tmp/baga_streq.baga | lli-14 -            # true false true
```

- [ ] Verify parity on the std extern modules too:

```bash
./baga-llvm --emit-llvm tests/std/os_test.baga > /tmp/baga_os.ll && lli-14 /tmp/baga_os.ll
./baga-llvm --emit-llvm tests/std/tcp_test.baga > /tmp/baga_tcp.ll && lli-14 /tmp/baga_tcp.ll
./baga-llvm --emit-llvm tests/std/time_test.baga > /tmp/baga_time.ll && lli-14 /tmp/baga_time.ll
./baga-llvm --emit-llvm tests/std/random_test.baga > /tmp/baga_rand.ll && lli-14 /tmp/baga_rand.ll
./baga-llvm --emit-llvm tests/std/io_test.baga > /tmp/baga_io.ll && lli-14 /tmp/baga_io.ll
```

Each prints its `..._test: all passed` line and exits 0.
- [ ] Run `make test` — the LLVM oracle now reports `OK examples/extern_write.baga` (and `OK examples/arena.baga` since Task 4); everything green.
- [ ] Commit:

```bash
git add src/codegen_llvm.c
git commit -m "feat(llvm): extern fn and str == str parity with the C backend"
```

---

## Task 11: Final validators + `make test-std` wiring

**Files:**
- Create: `examples/http_echo.baga`, `examples/hash_tool.baga`, `tests/http_echo_check.sh`, `tests/hash_tool_check.sh`
- Modify: `Makefile` (`.PHONY` line 9; new `test-std` target; hook into `test` before the final echo at line 122)
- Modify: `tests/llvm_oracle.sh` (skip list at lines 6-8), `tests/cranelift_oracle.sh` (skip case at lines 8-10) — `http_echo.baga` is a server loop and must not be run by the oracles.

**Interfaces:**
- Consumes: the complete std surface (`tcp_*`, `str_*`, `int_to_str`, `parse_int`, `sha256_hex`), arena builtins, `arg_count`/`arg`/`read_file` builtins.
- Produces: the two end-game validators. `http_echo.baga` — a minimal HTTP server (plain-text response echoing the request line, fresh arena per request, no framework). `hash_tool.baga` — a CLI that SHA-256-hashes a file. Plus a `test-std` Makefile target that runs every `tests/std/*_test.baga` under the C backend and (when built) the LLVM backend, then both validator check scripts.

**Steps:**

- [ ] Create `examples/hash_tool.baga`:

```baga
// hash_tool.baga — print the SHA-256 hex digest of a file.
// Note: `./baga` does not forward argv to the compiled program, so this is
// driven through --emit-c + gcc by tests/hash_tool_check.sh.
// Limitation: read_file + baga strings are NUL-terminated — text files only.

import "../std/crypto/sha256.baga"

fn main() -> i64 !IO {
    if arg_count() < 1 {
        eprintln("usage: hash_tool <file>")
        exit(1)
    }
    let path = arg(0)
    let data = read_file(path)
    if len(data) == 0 {
        eprintln(concat("cannot read: ", path))
        exit(1)
    }
    print(sha256_hex(data))
    return 0
}
```

- [ ] Create `examples/http_echo.baga`:

```baga
// http_echo.baga — minimal HTTP server: answers every request with a
// plain-text echo of the request line. One fresh arena per request; the
// request bytes are staged into it (bzero/bcopy-style arena usage), then
// the arena is freed after the response is sent — a long-running server
// does not leak.

import "../std/net/tcp.baga"
import "../std/str/str.baga"
import "../std/os/os.baga"

fn main() -> i64 !Net !IO {
    let port = 8080
    if arg_count() > 0 {
        port = parse_int(arg(0))
    }
    let listener = tcp_listen(port)?
    if listener < 0 {
        eprintln("listen failed")
        exit(1)
    }
    print(concat("baga http echo on port ", int_to_str(port)))
    let running = true
    while running {
        let a = arena_new()
        let conn = tcp_accept(listener)?
        if conn >= 0 {
            let req = tcp_read(conn, 65536)?
            // stage the request into the per-request arena
            let staged = arena_alloc(a, len(req))
            bcopy(req, staged, len(req))
            // the request line is everything up to the first CR
            let mut line = req
            let eol = str_find(req, "\r")
            if eol > 0 {
                line = substr(req, 0, eol)
            }
            let body = concat("baga http echo: ", concat(line, "\n"))
            let resp = concat("HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nContent-Length: ", concat(int_to_str(len(body)), concat("\r\nConnection: close\r\n\r\n", body)))
            tcp_write(conn, resp)
            tcp_close(conn)
        }
        arena_free(a)
    }
    return 0
}
```

- [ ] Create `tests/hash_tool_check.sh` (`chmod +x`), comparing against the system `sha256sum`:

```bash
#!/bin/bash
# End-to-end check for examples/hash_tool.baga against coreutils sha256sum.
# Compiles via --emit-c because ./baga does not forward argv.
cd "$(dirname "$0")/.."
set -e
printf 'baga std hash tool\n' > /tmp/baga_ht_input.txt
./baga --emit-c examples/hash_tool.baga > /tmp/baga_ht.c
gcc -O2 -o /tmp/baga_ht /tmp/baga_ht.c -lm
got=$(/tmp/baga_ht /tmp/baga_ht_input.txt)
want=$(sha256sum /tmp/baga_ht_input.txt | cut -d' ' -f1)
if [ "$got" = "$want" ]; then
    echo "OK: hash_tool ($got)"
else
    echo "FAIL: hash_tool ($got != $want)"
    exit 1
fi
```

- [ ] Create `tests/http_echo_check.sh` (`chmod +x`), driving the server over bash's `/dev/tcp`:

```bash
#!/bin/bash
# End-to-end check for examples/http_echo.baga: start the server, make one
# HTTP request, verify the echoed request line, stop the server.
cd "$(dirname "$0")/.."
set -e
./baga --emit-c examples/http_echo.baga > /tmp/baga_http.c
gcc -O2 -o /tmp/baga_http /tmp/baga_http.c -lm
/tmp/baga_http 18377 > /tmp/baga_http_log.txt 2>&1 &
SRV=$!
cleanup() { kill "$SRV" 2>/dev/null || true; wait "$SRV" 2>/dev/null || true; }
trap cleanup EXIT
sleep 1
if ! exec 3<>/dev/tcp/127.0.0.1/18377; then
    echo "FAIL: http_echo (cannot connect)"; exit 1
fi
printf 'GET /hello HTTP/1.0\r\nHost: localhost\r\n\r\n' >&3
resp=$(timeout 5 cat <&3 || true)
exec 3<&- 3>&-
case "$resp" in
    *"200 OK"*"baga http echo: GET /hello HTTP/1.0"*)
        echo "OK: http_echo";;
    *)
        echo "FAIL: http_echo"; echo "$resp"; exit 1;;
esac
```

- [ ] Add `http_echo.baga` to the oracle skip lists. In `tests/llvm_oracle.sh` after line 8:

```bash
    [ "$f" = "examples/http_echo.baga" ] && continue      # сървър (безкраен цикъл)
```

In `tests/cranelift_oracle.sh`, extend the case:

```bash
    case "$f" in
        examples/spec_bad.baga|examples/vec_typed.baga|examples/arg_type_bad.baga|examples/http_echo.baga) continue;;
    esac
```

(`examples/hash_tool.baga` needs no skip entry: with no argv it prints `usage: hash_tool <file>` to stderr and exits 1, deterministically, in every backend. The oracles compare C against LLVM/Cranelift: the LLVM backend (after Task 10) produces the same output and exit code → `OK`; Cranelift hits the `read_file` builtin, which is outside its subset → honest refusal → `SKIP`. Both are passing outcomes.)

- [ ] Wire `test-std` into the `Makefile`. Extend `.PHONY` (line 9):

```make
.PHONY: all clean test test-std test-llvm llvm cranelift test-cranelift self
```

Add the target after the `test-llvm` target (line 57):

```make
test-std: $(BIN)
	@echo "=== std библиотека (C backend) ==="
	@for f in $(wildcard tests/std/*_test.baga); do \
		printf "  %-28s" "$$f"; \
		if ./$(BIN) "$$f" > /tmp/baga_std_out.txt 2>&1; then \
			echo "OK"; \
		else \
			echo "FAIL"; cat /tmp/baga_std_out.txt; exit 1; \
		fi; \
	done
	@if [ -f ./$(LLVM_BIN) ]; then \
		echo "=== std библиотека (LLVM паритет) ==="; \
		for f in $(wildcard tests/std/*_test.baga); do \
			printf "  %-28s" "$$f"; \
			if ./$(LLVM_BIN) --emit-llvm "$$f" > /tmp/baga_std.ll 2>/tmp/baga_std_err.txt \
				&& lli-14 /tmp/baga_std.ll > /tmp/baga_std_llvm_out.txt 2>&1; then \
				echo "OK"; \
			else \
				echo "FAIL"; cat /tmp/baga_std_err.txt /tmp/baga_std_llvm_out.txt; exit 1; \
			fi; \
		done; \
	else \
		echo "(baga-llvm липсва — пропускам std LLVM паритета)"; \
	fi
	@echo "=== финални валидатори ==="
	@./tests/hash_tool_check.sh
	@./tests/http_echo_check.sh
```

Hook it into `test`, right before the final `Всички тестове минаха` echo (line 121-122):

```make
	@echo "=== std библиотека ==="
	@$(MAKE) -s test-std
	@echo ""
	@echo "Всички тестове минаха. ⚔️"
```

- [ ] Full verification from scratch:

```bash
make clean && make && make llvm && make cranelift
make test        # examples + negative tests + oracles + test-std
make self        # self-hosting fixed point must stay green
```

- [ ] Update `docs/language-en.md` and `docs/compiler-en.md` with short sections: bitwise operators table (with the C-compatible precedence), `import "path"` semantics, `extern fn` syntax and type restriction, arena builtins, and a pointer to `std/README.md`. (Docs, English, matching the existing files' structure.)
- [ ] Commit:

```bash
git add examples/http_echo.baga examples/hash_tool.baga tests/http_echo_check.sh tests/hash_tool_check.sh tests/llvm_oracle.sh tests/cranelift_oracle.sh Makefile docs/language-en.md docs/compiler-en.md
git commit -m "feat(std): final validators (http_echo, hash_tool) and test-std target"
```

---

## Post-conditions

- `make test` runs: all pre-existing examples and negative tests, the new `bitwise`/`import`/`extern fn`/`arena` blocks, both oracle scripts, and `test-std` (11 std test programs under C and — when built — LLVM, plus `hash_tool` vs `sha256sum` and a live `http_echo` request).
- `make self` is untouched and green; `self/compiler.baga` was never modified.
- The library is ready for the follow-up HTTP-framework and KV-database specs.
