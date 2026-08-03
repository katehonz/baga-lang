# grebaga — language & product gaps

Probe log from apps-roadmap №9 (grep-like CLI).

## G1 — `read_line` collapses empty line and EOF

**Symptom.** `std/io/read_line` returns `""` for both an empty line and EOF
(documented). A grep loop cannot tell "blank line to test" from "stop".

**Workaround.** `gre_next_line`: chunked `fd_read` + residual buffer; `ok=0`
only when EOF and buffer empty.

**Severity.** Medium for any line protocol.

**Verdict.** Optional `read_line_ex -> {ok, line}` in std/io — small, useful
for №5/№9-class tools. App-level scanner is fine for P0.

## G2 — recursive `*` match can be slow / deep

**Symptom.** `gre_match_here` backtracks on `*` and recurses per byte. Fine
for short lines; pathological patterns × long lines hurt.

**Workaround.** Accept for P0; lines are typically short.

**Severity.** Low for CLI README use; high for adversarial input.

**Verdict.** Iterative NFA or restrict `*` later. Not a language gap.

## G3 — no argv flag library

**Symptom.** Every CLI reimplements `-n`/`-i`/`--` scanning with `arg(i)`.

**Workaround.** Inline in `demo.baga`.

**Severity.** Low until many CLIs share flags.

**Verdict.** Tiny `std/os/args` helper or package when №8/№9/№4 demos pile up.

## Closed / fine

- Streaming without loading whole file: chunked read works.
- `write_file` + scan round-trip in tests.
- Package imports + sandak; exit codes via `main -> i64`.
