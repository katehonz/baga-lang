# mdbaga — language & product gaps

Probe log from apps-roadmap №4 (Markdown → HTML). Same shape as kv/ws/chat.

## M1 — nested `concat` still dominates large builders

**Symptom.** Even with `${expr}` interpolation (already in the language),
long HTML assembly is still chains of `out = concat(out, …)`. A 400-line
parser spends much of its surface area on string glue.

**Workaround.** Interpolation for short locals; `concat` loops for builders.

**Severity.** Medium — readable enough, tedious to write/review.

**Verdict.** Variadic `concat` or a small `str_buf` builder would help №4/№7;
interpolation alone is not enough for append-heavy loops. Logged (extends
httpdbaga G1).

## M2 — no `file_exists` / error channel for `read_file`

**Symptom.** `read_file(path)` returns `""` for both missing files and empty
files. CLI cannot exit non-zero on "not found" without a separate probe.

**Workaround.** Document; treat empty as valid MD (empty fragment).

**Severity.** Low for the probe; medium for a real CLI UX.

**Verdict.** `Result`/`!IO` structured errors (L3) or `file_stat` in std/os —
when №6/№9 force it. App-level for now.

## M3 — single-char `str_split` only

**Symptom.** `str_split` takes a one-character delimiter. CRLF needs a
`str_replace` pass first. Multi-char delimiters (e.g. `\r\n` as unit) missing.

**Workaround.** Normalize `\r\n` → `\n` before split (done in `md_to_html`).

**Severity.** Low.

**Verdict.** YAGNI until a protocol needs multi-char split; replace is fine.

## M4 — no nested lists / tables / images (product)

**Symptom.** CommonMark/GFM features omitted in P0.

**Severity.** Medium for "real docs"; fine for README-class MD.

**Verdict.** Product P1 — no language change required.

## Closed / fine

- **Interpolation `${}`** already landed before this probe — used lightly;
  loop `concat` remains the bottleneck (M1).
- Line walk + `Vec<str>` of lines is adequate; no `Vec<struct>` forced yet
  (AST of blocks would want L4 later).
- Package imports + sandak: clean (`std` only).
