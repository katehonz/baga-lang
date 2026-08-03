# testbaga — language & product gaps

Probe log from apps-roadmap №8 (test runner).

## T1 — no process spawn from Baga

**Symptom.** A pure-Baga multi-file runner cannot `exec` `baga file_test.baga`
or capture exit codes of child processes. Isolation and discovery aggregation
need an outer driver.

**Workaround.** `scripts/baga-test` (bash) runs each file via the baga CLI.

**Severity.** High for a pure-language test runner; low for day-1 DX.

**Verdict.** `process_run(cmd, args) -> {status, stdout, stderr}` in std/os
(or `!IO` extern) when №9/CI force it. Shell driver is honest for P0.

## T2 — no directory listing

**Symptom.** Cannot discover `*_test.baga` without `readdir` / `getdents`.

**Workaround.** Shell `find` in `scripts/baga-test`.

**Severity.** Medium.

**Verdict.** `list_dir(path) -> Vec<str>` in std/os (Linux getdents or
opendir). Natural follow-up to this probe.

## T3 — no function values (L5) → no `test("name", fn)`

**Symptom.** Cannot register tests as values; every case is a statement in
`main`. No filtering by name without codegen.

**Workaround.** Named `assert_*` calls; suite accumulates by hand.

**Severity.** Medium for large suites; fine for current repo scale.

**Verdict.** Closures/function values (L5) — №7 will press harder. Until
then, statement-style tests are the Baga idiom.

## T4 — Suite is by-value field rebuild

**Symptom.** Every `suite_eq_*` returns a new `Suite` (structs are by-value).
Verbose at call sites: `s = suite_eq_i64(s, ...)`.

**Workaround.** Accept the threading (same as ChatState / PgConn).

**Severity.** Low.

**Verdict.** Same as pgbaga G2 — pointer-ish suites or mutable fields later;
not special to testing.

## Closed / fine

- Fail-fast `assert_*` matches today's `check` semantics and exit codes.
- Diff output for `assert_eq_str` / `assert_eq_i64` improves on bare ok/FAIL.
- Package imports + sandak for the lib; shell only for multi-process.
