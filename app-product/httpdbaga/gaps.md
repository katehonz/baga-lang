# httpdbaga — language gaps found while building

Building this HTTP library is a probe of the language. Each entry records a
friction point with the evidence that exposed it. Verdicts are assigned at
milestone end: **roadmap** (language change worth specifying), **YAGNI**, or
**app-specific**.

Entry shape: symptom → repro → workaround → severity → verdict.

---

## G1 — `concat` is strictly 2-argument (no variadics, no interpolation)

**Symptom.** Building a JSON string of N fragments needs either N-1 nested
`concat(...)` calls or N-1 sequential `let`/reassign statements. The nested
form is hostile to hand-writing.

**Evidence.** While writing the test echo body I produced a 7-level nested
`concat` that failed to compile (`очаквах израз, получих ','`). Bisection
proved the parser is *correct*: a programmatically-balanced 10-paren version
compiles and runs. My hand-written version had 9 opens vs 10 closes — the deep
nesting made a paren miscount trivially easy to make and hard to spot. The
error pointed at a `,` far from the actual imbalance.

**Workaround.** Sequential statements (what `serve_one` in
`tests/http_test.baga` and `http_respond` in `http.baga` use):
```baga
let mut body = "{\"method\":\""
body = concat(body, http_method(req))
body = concat(body, "\",\"path\":\"")
...
```

**Severity.** Medium. Every string-building site in real code hits this; the
verbose form is correct but noisy, and the compact form is a foot-gun.

**Verdict.** Roadmap candidate. Two options to spec: (a) variadic `concat`
(n-ary, C backend already has an n-ary `baga_concat` it could grow), or
(b) string interpolation (`"{method}"`-style). Interpolation is the bigger win
for JSON/templating; variadic concat is the smaller change.

## G2 — a library file without `main` cannot be compile-checked

**Symptom.** `./baga --emit-c app-product/httpdbaga/http.baga` fails with
`липсва функция 'main'`. A pure library module cannot be type-checked or
codegen-checked on its own; you must compile a consumer that imports it.

**Evidence.** Direct run during Task 1 (see plan). The only way to validate
`http.baga` was to write `tests/http_test.baga` (which has a `main`) and
compile that.

**Workaround.** Always compile via a consumer/test.

**Severity.** Low–medium. Workable, but it means a library author gets no
fast "does this module even typecheck" loop, and errors surface attributed to
the consumer file, not the module.

**Verdict.** **Closed.** `./baga --check lib.baga` (alias `--lib`) runs parse +
typecheck without requiring `main` and prints `ok: <path>`.

## G3 — `read_line` keeps the trailing `\r`

**Symptom.** std/io `read_line` strips `\n` but not `\r`. HTTP (and any CRLF
protocol) leaves every line ending in byte 13; the blank header-terminator
line arrives as `"\r"` (len 1), not `""`.

**Evidence.** Loopback probe: sending `GET /x HTTP/1.1\r\nHost: demo\r\n\r\n`,
`read_line` returned `"GET /x HTTP/1.1\r"` (last byte 13) and the "empty" line
had len 1.

**Workaround.** `str_trim` every line (strips CR), and test the header
terminator as `line == "" || line == "\r"`.

**Severity.** Low. `str_trim` handles it; but it is a per-protocol tax and a
trap (an `== ""` check alone silently never terminates the header loop).

**Verdict.** **Closed.** `read_line` strips a trailing `\r` before the `\n`, so
CRLF and LF yield the same content. HTTP header loops can now use `line == ""`
for the blank terminator.

## G4 — error messages localize far from the root cause on unbalanced parens

**Symptom.** A paren imbalance in a nested expression reports `очаквах израз,
получих ','` at a comma, not "unbalanced parentheses" near the actual site.

**Evidence.** Same session as G1 — the misleading location slowed the
diagnosis until I counted parens programmatically.

**Severity.** Low. Cosmetic; only bites on malformed input.

**Verdict.** YAGNI for now. Note for a future parser-error-quality pass: a
depth/imbalance hint would help.

---

## G8 — `chr()` is a UTF-8 codepoint encoder; no raw-byte source for 128..255

**Symptom.** `chr(n)` encodes codepoint `n` as UTF-8: `chr(255)` is the
2-byte string `0xC3 0xBF`, not the single byte `0xFF`. Any "stage one byte"
helper built on `chr` silently corrupts values ≥ 128.

**Evidence.** `tests/probe_binary_io.baga` (2026-08-03):
`check("chr255_is_utf8_len2", len(chr(255)) == 2)` passes. This makes
`tcp.baga`'s `poke8(mfd, off, v)` — `pwrite(mfd, chr(v & 255), 1, off)` —
write `0xC3` for any `v >= 128`: a latent bug in `std/net`, harmless today
only because the sole caller stages `127.0.0.1`/`0.0.0.0` octets.

**Workaround.** The HTTP/2 milestone avoids `poke8` entirely: binary writes go
through `bytes` → `str_of_bytes` → `write(fd, s, count)`; binary reads land
in a `str_repeat` buffer and come out via `byte_at`.

**Severity.** High (silent data corruption in a `std/` helper, latent).

**Verdict.** Roadmap. A raw byte constructor (`byte(n)` returning a 1-byte
string/bytes regardless of value) and a fix of `poke8` on top of it.

## G9 — no mutator for `bytes` (no push/set/new)

**Symptom.** Constructing binary data dynamically requires building a
`Vec<i64>` with per-byte `vec_push` and converting via `bytes_from_vec`, or
assembling hex literals. There is no `bytes_push`/`bytes_set`/`bytes_new(n)`.

**Evidence.** Every binary builder in this milestone (`h2.baga`'s `h2_one`/
`h2_be16`/`h2_be32`, `hpack.baga`'s `hpack_one_byte`, the read path of
`tcp_read_bytes`) is a `vec_new` + `vec_push` loop around `bytes_from_vec`.

**Workaround.** The Vec round-trip above. Correct, but 3–4 lines per byte
buffer and one extra copy.

**Severity.** Medium. A per-byte tax on every binary protocol.

**Verdict.** Roadmap. `bytes` mutators (push/set, or a bytes builder) —
pairs naturally with G10.

## G10 — `extern fn` cannot take `bytes` (binary FFI needs a workaround)

**Symptom.** The checker restricts `extern fn` parameters to `i64`/`f64`/
`str` ("неподдържан тип на параметър"), so a binary buffer cannot cross the
FFI boundary as `bytes`.

**Evidence.** Design probe for the HTTP/2 milestone (2026-08-03): declaring
`extern fn write(fd: i64, b: bytes, n: i64)` is rejected; the C codegen maps
non-{i64,f64,str} extern types to `int64_t` anyway (a struct-by-value
mismatch).

**Workaround.** `str_of_bytes` (explicit-length memcpy) + an extern taking
`str`, passing the count explicitly; never call `len()`/`substr()` on the
resulting string. Probed binary-safe in `tests/probe_binary_io.baga` and
`tests/std/tcp_bytes_test.baga`.

**Severity.** Medium. Works, but the binary-safety contract is invisible in
types — one stray `len()` away from corruption.

**Verdict.** Roadmap. `bytes` parameters on `extern fn` (codegen passes
`b.data`).

## G11 — global arena allocator was not thread-safe (fixed this milestone)

**Symptom.** All string/vec/bytes allocations go through the global
block-based arena `baga_alloc`. Until 2026-08-03 it had no lock: two threads
allocating concurrently corrupted the block list (same bytes handed out
twice) — silent data loss or a segfault.

**Evidence.** `tests/probe_alloc_race.baga`: 8 threads × 2000 concats,
expected total length 128032; observed **114370 / 107856 / 120159 across
three runs** (pre-fix). The h2 loopback test segfaulted on the same race.
Post-fix (a `pthread_mutex_t` around the arena bump in the emitted runtime,
`src/codegen_c.c`): 128032 on every run; `tests/h2_test.baga` and a
50-request mixed parallel curl burst pass clean.

**Workaround (pre-fix).** None in-language — every `go`/`go_bg` program that
allocates in workers was exposed.

**Severity.** Critical for any multi-threaded program (the language ships
`go`/channels as a core feature).

**Verdict.** **Closed** (fixed 2026-08-03 in the C backend runtime). The
self-hosted compiler emits its own malloc-based runtime and was unaffected.
Note: the mutex serializes allocation — fine at demo scale, a sharded/per-thread
arena is the future performance answer.

## G12 — bitwise ops bind LOOSER than comparisons (C precedence trap)

**Symptom.** `x & 128 == 128` parses as `x & (128 == 128)` = `x & 1`, not
`(x & 128) == 128` — the same trap as C. Used in a condition it errors
("очаквах bool в условие, получих i64"); assigned to a `let` it silently
produces a wrong `i64`.

**Evidence.** `print(6 & 4 == 4)` prints `0` while `print((6 & 4) == 4)`
prints `true` (2026-08-03 probe). The HPACK decoder shipped with five such
sites; all needed explicit parens.

**Workaround.** Parenthesize every bitwise-vs-comparison mix.

**Severity.** Medium-high. Silent wrong values on assignment; the condition
case at least fails loudly.

**Verdict.** Roadmap. Either raise `&`/`|`/`^` above comparisons (modern
languages mostly do), or reject unparenthesized mixes with a diagnostic.

## G13 — type errors in imported code are attributed to the wrong file/line

**Symptom.** Compiling a consumer of a multi-file import chain reports
checker errors with line numbers from the expanded token stream, pointing at
the entry file: `tests/hpack_test.baga:225:14` for a 140-line test file (the
real site was inside `hpack.baga`).

**Evidence.** The five G12 condition errors above all reported as
`tests/hpack_test.baga:{92,180,257,271,309}` — three of those lines do not
exist in the file.

**Workaround.** Guess the real file from the message content, grep.

**Severity.** Low-medium. Slows every multi-file library iteration.

**Verdict.** Roadmap (diagnostics pass) — tokens carry source positions;
attribution just needs to follow them through import expansion.

## G14 — no associative container (maps) — stream tables are parallel Vecs

**Symptom.** HTTP/2 needs stream-id → state lookup; HPACK needs name →
index. With no map type these become parallel `Vec` pools + linear scans
(`h2_find`, `hpack_static_exact`), and per-stream payload storage needs
flat pools with offset/length bookkeeping (struct elements stay impossible
— element types are i64/str/f64/bytes only).

**Evidence.** `h2.baga`'s stream pool (`s_ids`/`s_kseg`/`s_vseg`/
`s_body_off`/`s_body_len`/`s_state`), the header-segment `\n`-join trick in
`h2_finish_headers`, and the append-only `body_pool` — all shaped by the
missing map.

**Workaround.** The above; fine at demo scale, O(n) per lookup.

**Severity.** Medium. Every stateful protocol/data structure hits it.

**Verdict.** Roadmap (long-term language feature, not this milestone).

---

## Triage summary (end of milestone)

| Gap | Verdict | Next step |
|-----|---------|-----------|
| G1 n-ary concat / interpolation | **closed** | string interpolation `${expr}` shipped |
| G2 `--check`/lib mode without main | **closed** | `./baga --check lib.baga` (alias `--lib`) |
| G3 `read_line` keeps `\r` | **closed** | `read_line` strips trailing `\r` (CRLF == LF) |
| G4 error location on imbalance | YAGNI | fold into a future diagnostics pass |
| G8 `chr()` UTF-8 only (poke8 ≥ 128 broken) | roadmap | raw byte constructor + poke8 fix |
| G9 no `bytes` mutators | roadmap | bytes push/set/builder |
| G10 extern can't take `bytes` | roadmap | `bytes` FFI parameters |
| G11 arena not thread-safe | **closed** | mutex in the emitted runtime (fixed 2026-08-03) |
| G12 bitwise-vs-compare precedence | roadmap | precedence change or diagnostic |
| G13 error attribution across imports | roadmap | diagnostics pass (positions through imports) |
| G14 no map type | roadmap | long-term feature; stream tables stay parallel Vecs |
