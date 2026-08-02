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

## Triage summary (end of milestone)

| Gap | Verdict | Next step |
|-----|---------|-----------|
| G1 n-ary concat / interpolation | **closed** | string interpolation `${expr}` shipped |
| G2 `--check`/lib mode without main | **closed** | `./baga --check lib.baga` (alias `--lib`) |
| G3 `read_line` keeps `\r` | **closed** | `read_line` strips trailing `\r` (CRLF == LF) |
| G4 error location on imbalance | YAGNI | fold into a future diagnostics pass |
