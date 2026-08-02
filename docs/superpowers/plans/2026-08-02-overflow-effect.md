# Plan: M18 — `!Overflow` as an Effect

Date: 2026-08-02
Spec: `docs/superpowers/specs/2026-08-02-overflow-effect-design.md`
Discipline: test-first; every step ends in a green check.

## Ground truth (verified against code)

- Effects: open-ended string list on `Type` (`include/baga.h:399`), declared on
  return types via `NODE_TYPE_EFFECT`, checked one-way in `check_fn`
  (`checker.c:989`). Call results merge callee effects generically
  (`checker.c:376`); `go`/`pool_map` bubble worker effects (`checker.c:505`).
  ⇒ **`!Overflow` propagation needs no checker change** — it rides the generic
  merge. (Step 5 verifies this empirically rather than assuming it.)
- Verifier gate: `ret_has_non_par_effects` (`verify.c:3160`), used at the
  collect gate (`verify.c:3782`) and the worker gate (`verify.c:1487`).
- Arith verdicts (`naproven`/`natotal`) are computed **only** in `verify_fn`'s
  reporting re-run `ob2` (`verify.c:4082–4123`). `verify_fn_collect`
  (used by `proofs.c`) collects kind-4 obligations during symexec but frees
  them without discharging ⇒ M18 must discharge them there too.
- `make test` asserts via targeted `grep` on `--verify` text, not golden
  files. Negative completeness greps match only
  `^  (ensures|извикване|граница|протокол)` ⇒ a new `  ефект !Overflow:` line
  is invisible to them. **Constraint:** the new line must NOT contain the
  uppercase tokens `ДОКАЗАНО`/`ОБРОЧЕНО`/`НЕ МОГА ДА РЕША` (some tests do
  `grep -c "ДОКАЗАНО"`). Use descriptive lowercase wording.
- `make self` is unaffected by `verify.c`/`proofs.c` edits (C-bootstrap-only);
  keep `self/compiler.baga` untouched and add no new checker error on it.

## Step 0 — Baseline

- `make && make test && make self` → all green before touching anything.
  Record that the 62-example verify corpus passes. (If LLVM present:
  `make test-llvm`.)

## Step 1 — Test fixtures first (red)

Create `examples/verify/` fixtures. Each pairs a spec with a function:

- `ovf_eff_safe.baga` — `inc_bounded(n) requires 0<=n,n<=100 { return n+1 }`,
  no `!Overflow`. Expect: arith 1/1 safe; overflow-effect line says safe/exact.
- `ovf_eff_refuted.baga` — `inc(n) requires n>=0 { return n+1 }`, no
  `!Overflow`. Expect: arith REFUTED at INT64_MAX; overflow-effect line says
  "прелива … недеклариран"; exit code nonzero.
- `ovf_eff_declared.baga` — same body as refuted but `-> i64 !Overflow`.
  Expect: overflow-effect line says declared (honest), no undeclared-violation.
- `ovf_eff_unknown.baga` — a function whose arith is UNKNOWN (e.g. loop-carried
  growth via havoc, or the `(2^62,2^63)` band), no `!Overflow`. Expect:
  overflow-effect line "недоказуема безопасност — декларирай !Overflow"; exit
  code still 0 (UNKNOWN never fails).
- `ovf_eff_redundant.baga` — provably safe body but declares `!Overflow`.
  Expect: note "деклариран, но доказано безопасна".
- `ovf_eff_skip.baga` — a function skipped by the fragment gates (e.g. non-i64
  result or `match`) that also has arith. Expect: NO overflow-effect line
  (skipped ⇒ no claim).
- `ovf_eff_propagate.baga` — caller invokes an `!Overflow` function without
  declaring/catching ⇒ checker error "необработен ефект !Overflow"; a second
  function that declares it passes `--check`.

Run each: `./baga --verify examples/verify/ovf_eff_*.baga` — confirm current
output lacks the new line (red), and `ovf_eff_propagate.baga` currently does
NOT error (red for the checker propagation if a change is needed).

## Step 2 — Admit `Overflow` in the fragment gate (no behavior change yet)

- `verify.c`: generalize `ret_has_non_par_effects` →
  `ret_has_unverifiable_effects(t)`: returns 1 iff some declared effect is
  neither `"Par"` nor `"Overflow"`. Update both call sites (collect gate
  ~3782, worker gate ~1487) and the comment.
- No fixture declares `!Overflow` yet except Step 1's, so the existing 62-file
  corpus is unaffected.
- **Check:** `make && make test` green; `./baga --verify
  examples/verify/ovf_eff_declared.baga` no longer prints
  `ПРОПУСНАТО (ефекти (не-Par))` (it is now analyzed).

## Step 3 — Discharge in `verify_fn_collect` (feeds proofs.c)

- `include/baga.h`: extend `FnVerifyRes` with
  `int ovf_analyzed, ovf_declared, ovf_safe, ovf_res; char *ovf_witness;`.
- `verify.c` `verify_fn_collect`: before freeing `ob`, loop kind-4
  obligations, call `verify_arith_obl`, count `natotal`/`naproven`, capture the
  first REFUTED witness string. Set:
  - `ovf_analyzed = 1` (we passed the gates);
  - `ovf_declared` = return type declares `Overflow` (scan `NODE_TYPE_EFFECT`);
  - `ovf_safe = (natotal==0) || (naproven==natotal)`;
  - `ovf_res` per the spec table (§3.2): PROVEN if safe; REFUTED if
    !safe && refuted && !declared; UNKNOWN if !safe && unknown-only &&
    !declared; PROVEN-with-note if declared (store a flag for the redundant /
    honest-declared cases).
- `fn_verify_res_free`: free `ovf_witness`.
- **Check:** `make` clean (no warnings under `-Wall -Wextra`); unit-print via a
  temporary `--proofs` run on `ovf_eff_refuted.baga` shows the fields are set
  (full proofs formatting lands in Step 6).

## Step 4 — Reporting in `verify_fn` (text + JSON + exit code)

- In the `ob2` arith loop, capture the first REFUTED witness into a local
  buffer (reuse the names/vals already computed).
- After the arith summary line (~4118), for analyzed functions print ONE line
  (wording chosen to avoid the uppercase verdict tokens):
  - safe & !declared: `  ефект !Overflow: безопасна — типът е точен`
  - safe & declared:  `  ефект !Overflow: деклариран, но доказано безопасна`
  - refuted & declared: `  ефект !Overflow: деклариран — прелива при <wit>`
  - refuted & !declared: `  ефект !Overflow: прелива при <wit>, а !Overflow не е деклариран`
    → set `any_refuted = 1`.
  - unknown & declared: `  ефект !Overflow: деклариран — безопасността не е доказуема`
  - unknown & !declared: `  ефект !Overflow: безопасността не е доказуема — декларирай !Overflow`
- JSON: emit `"overflow_effect": {"analyzed","declared","safe","result","witness"}`
  after `"arith"`; skipped branch emits `{"analyzed": false, "result":"skipped"}`.
- **Check:** all six Step-1 fixtures print the expected line; `ovf_eff_refuted`
  exits nonzero, `ovf_eff_unknown` exits zero. `--verify --json` parses
  (`python3 -m json.tool`).

## Step 5 — Checker propagation (verify empirically; likely no change)

- Run `./baga examples/verify/ovf_eff_propagate.baga` (the caller that omits
  `!Overflow`). If the generic call-effect merge already reports
  "необработен ефект !Overflow", **no checker change** — document it. If not,
  add the minimal merge so a call to an `!Overflow` function bubbles
  `"Overflow"` into `cur_effects`.
- **Check:** the omitting caller errors; the declaring caller compiles.
  `make test` still green (no existing example calls an `!Overflow` fn).

## Step 6 — Proofs theorem

- `proofs.c`: after the purity/effects theorem, if `vr.ovf_analyzed`, emit
  `theorem <fn>_overflow_safe` with status from `ovf_res` (ДОКАЗАНО /
  ОБРОЧЕНО + witness / НЕ МОГА ДА РЕША). Skipped ⇒ omit.
- **Check:** `./baga --proofs examples/verify/ovf_eff_safe.baga` shows the
  theorem ДОКАЗАНО; `ovf_eff_refuted.baga` shows ОБРОЧЕНО + witness.

## Step 7 — Makefile regression tests for M18

Append targeted `grep` assertions mirroring the house style (positive +
negative). Assert:
- `ovf_eff_safe`: line "ефект !Overflow: безопасна" present.
- `ovf_eff_refuted`: "прелива при n = 9223372036854775807" + "не е деклариран".
- `ovf_eff_declared`: "деклариран" present, and NO "не е деклариран".
- `ovf_eff_unknown`: "не е доказуема" present; command exit code 0.
- `ovf_eff_skip`: NO "ефект !Overflow" line.
- `ovf_eff_propagate`: `--check` reports "необработен ефект !Overflow".
- **Check:** `make test` green end-to-end.

## Step 8 — Full verification gate

- `make test` (C oracle + all verify greps).
- `make self` (fixed point baga2==baga3) — must stay green; if it breaks,
  the cause is a checker/codegen change leaking into `self/compiler.baga`
  compilation — revert to "no checker change" and re-verify.
- `make test-llvm` if LLVM available (C≡LLVM running oracle).
- baga≡baga2 spot check on `examples/verify/ovf_eff_*` stdout+exit.

## Step 9 — Docs + thesis + commit (separate todos)

Hand off to the writing todos: `thesis-m18`, open-problems chapter, binding
intro/conclusion, bg+en doc parity, CHANGELOG, commit. Commit only on the
user's explicit "commit".

## Risks & mitigations

| Risk | Mitigation |
|---|---|
| `grep -c "ДОКАЗАНО"` tests inflate | overflow-effect line avoids uppercase verdict tokens (Step 4 wording) |
| `make self` breaks | no `self/compiler.baga` change; prefer zero checker edits; run Step 8 |
| double arith work (collect + report) | acceptable: collect discharge is cheap and only for proofs; report re-run stays for per-op printing (matches existing "keep output identical" design) |
| witness capture divergence collect vs report | both use `verify_arith_obl` + first-refuted; assert same string in Step 4 check |
| skipped-function false certificate | `ovf_analyzed=0` ⇒ no line, no theorem (Step 3/4/6) |
