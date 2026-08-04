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

**Verdict.** L4 milestone.

## P4 — read_file: missing vs empty (mdbaga M2, re-hit)

**Symptom.** `demo.baga` cannot tell "no such template file" from "empty
template"; both render to empty output with exit 0.

**Verdict.** Same small std/os fix as M2 (`file_exists` or `read_file_ex`).

## P5 — unannotated vec_new: checker passes, codegen emits i64 elements

**Symptom.** First cut had `let toks = vec_new()` in `tpl_render`, passed
to `tpl_tokenize(src, toks: Vec<str>)`. The checker accepted it, but the C
backend emitted `int64_t` element access (`baga_substr(b_t, …)` where `b_t`
was `int64_t` — gcc `-Wint-conversion`), and the program died in the walk.
Annotating (`let toks: Vec<str> = vec_new()`) fixes it.

**Workaround.** Always annotate `vec_new()` when the element type is only
implied by a later call, not a local push.

**Severity.** High (silent unsound codegen; the annotation rule in the
language docs saves you, but the divergence itself is a compiler bug worth
a test: checker unification must reach codegen element selection).

**Verdict.** Compiler bug candidate — minimal repro: unannotated vec_new
passed to a `Vec<str>` parameter, then `vec_get` on it in the caller.

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
