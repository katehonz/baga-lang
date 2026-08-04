# tplbaga — language & product gaps

Probe log from apps-roadmap **№7** (template engine).

## P1 — no function values (L5) → filter name switch

**Symptom.** Filters are hard-coded in `tpl_filter_apply`; no
`tpl_filter_register("upper", upper_fn)`. Template helpers/filters are the
canonical closure use case — handlers as values, registered per render.

**Workaround.** Name switch + `name:arg` spec convention.

**Severity.** High for a template *framework*; fine for a fixed probe.

**Verdict.** L5 — same lineage as fmrbaga G1, queuebaga Q5, jsonrpcbaga R2.
№7 confirms handler-style APIs are the loudest L5 customer.

## P2 — no Result / sum types (L3)

**Symptom.** `TplOut { ok, html, err }` — same stand-in as pgbaga G1 /
jsonrpcbaga R1.

**Verdict.** Migrate target when `Result` lands: `Render(html)` /
`Err(TplError)` with position in the template.

## P3 — no Vec<struct> (L4) → prefix-encoded tokens

**Symptom.** The token stream is `Vec<str>` with a one-letter prefix
("T"/"V"/"R"/"I"/"E"/"X") instead of `Vec<Token>`; block pairing is a
`Map<i64,i64>` jump table instead of node pointers.

**Workaround.** Prefix convention; works, but every parser re-pays it.

**Severity.** Medium; shared with queuebaga (job structs serialized to disk).

**Verdict.** **Closed 2026-08-04** — `Vec<struct>` shipped (boxed copies,
C backend). The prefix encoding stays (working, tested); a rewrite to
`Vec<Token>` is optional cleanup, not a blocker.

## P4 — read_file: missing vs empty (mdbaga M2, re-hit)

**Symptom.** `demo.baga` cannot tell "no such template file" from "empty
template"; both render to empty output with exit 0.

**Verdict.** Same small std/os fix as M2 (`file_exists` or `read_file_ex`).

## P5 — unannotated vec_new: checker passed, codegen emitted i64 (FIXED)

**Symptom.** First cut had `let toks = vec_new()` in `tpl_render`, passed
to `tpl_tokenize(src, toks: Vec<str>)`. The checker accepted it, but the C
backend emitted `int64_t` element access (`baga_substr(b_t, …)` where `b_t`
was `int64_t` — gcc `-Wint-conversion`), and the program died in the walk.
Annotating (`let toks: Vec<str> = vec_new()`) worked around it.

**Root cause.** Call arguments were only checked assignable; an unannotated
Vec/Map argument never got its element kind from the callee parameter, so a
later `vec_get` fell into the historical i64 default — the checker and the
codegen agreed on the *wrong* type. Silent garbage reads, no diagnostic.

**Fix (2026-08-04).** The checker now fixes an unknown element kind from
the parameter at the call site (same fix-on-first-use mutation as
`vec_push`); the reverse order (`vec_get` first) is a compile error. Two
regressions in `make test` (vec + map call-site inference).

**Verdict.** Closed — the probe did its job: an app found a soundness bug,
and the fix landed with tests.

## P6 — keywords are not reservable identifiers (`spec`)

**Symptom.** A filter parameter named `spec` fails to parse ("очаквах
'идентификатор'") — `spec` is a declaration keyword. Fine and expected, but
there is no diagnostic pointing at the keyword clash; the parse errors look
like noise.

**Verdict.** Tiny DX: "ключовата дума 'spec' не може да е идентификатор"
would save a stare. Not language design, just error messages.

## Closed / fine

- `Map<str,str>` context: lookup + truthiness natural, zero friction.
- Jump-table walk: nested if/else without recursion or pair returns.
- Escaping + raw triple braces match mustache expectations (46 checks).
- `${}` language interpolation stays the compile-time story; tplbaga is the
  runtime-data sibling — no overlap, both useful.
