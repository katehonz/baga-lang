# Static Verification (M0) Implementation Plan

**Goal:** `./baga --verify file.baga` statically proves/refutes `requires`/
`ensures` for pure, non-recursive, loop-free, **linear** `i64` functions —
soundly (never PROVEN unless it truly holds), with concrete counterexamples on
REFUTED, honest UNKNOWN otherwise. New `src/verify.c`; no codegen change.

**Spec:** `docs/superpowers/specs/2026-08-01-static-verification-design.md`

## Global Constraints

- **Soundness is the acceptance bar.** Every task keeps the three-valued
  discipline: when unsure → UNKNOWN, never PROVEN. The soundness test suite
  (Task 8) gates `make test`.
- C11, `-Wall -Wextra` clean, zero deps. `self/compiler.baga` untouched;
  `make self` stays green (`--verify` is C-bootstrap-only).
- Reuse the existing `ensure_expr` ASTs; **no new surface syntax**.
- Rationals are exact (`int64` num/den, gcd-normalized, checked for overflow →
  overflow ⇒ UNKNOWN, never a wrong PROVEN).

## Verified facts relied on

- Spec ↔ fn matched by name (`find_ensures_spec`, `codegen_c.c:711`).
- `spec_requires`/`spec_ensures` are `NODE_ENSURE` with `.ensure_expr` (a
  checked boolean AST) and `.ensure_text`.
- `output` is the special identifier bound to the return value in ensures.
- AST: `NODE_BINARY{bin_op,left,right}` (OP_ADD/SUB/MUL/DIV/MOD, OP_LT/GT/LE/GE/
  EQ/NEQ, OP_AND/OR), `NODE_UNARY{UOP_NEG,UOP_NOT}`, `NODE_INT_LIT{int_val}`,
  `NODE_IDENT{name}`, `NODE_IF{cond,then_br,else_br}`, `NODE_BLOCK{stmts}`,
  `NODE_LET{let_name,is_mut,let_init}`, `NODE_ASSIGN{assign_target,assign_val}`,
  `NODE_RETURN{ret_val}`, `NODE_EXPR_STMT{expr}`, `NODE_CALL{callee,args}`.
- Implicit return: last `NODE_EXPR_STMT` of a non-void fn body is the return
  (`codegen_c.c:795-800`).

---

## Task 1 — Scaffold `src/verify.c` + `--verify` mode

- [ ] `include/baga.h`: add `int verify;` to options; declare `int verify_program(Node *prog)`.
- [ ] `src/main.c`: parse `--verify` (help text BG, like `--test-specs`); when
      set, run `verify_program` after the checker and return its exit code
      (0 = all PROVEN/skipped, 1 = any REFUTED). No codegen.
- [ ] `src/verify.c`: skeleton with the result enum
      `V_PROVEN, V_REFUTED, V_UNKNOWN, V_SKIP` and a per-function reporter that
      prints `verify <name>:` then one line per ensures clause.
- [ ] Add `src/verify.o` to `Makefile` `SRCS`/`OBJS`.
- **Verify:** `make` builds; `./baga --verify examples/spec_ensures.baga` runs
      and (still stubbed) prints `verify факториел: SKIPPED (not implemented)`.

## Task 2 — Rationals + linear forms (`SymExpr`)

- [ ] `Rat { int64 num, den }` with gcd-normalization, `rat_add/sub/mul/neg`,
      `rat_cmp0`, overflow-checked (on overflow set a `*overflowed` flag →
      caller returns UNKNOWN).
- [ ] `Lin { Rat c; vec<(char *var, Rat coeff)> }` — canonical linear form
      `c + Σ coeff·var`; `lin_const`, `lin_var`, `lin_add`, `lin_sub`,
      `lin_scale(Rat)`, `lin_neg`, `lin_is_const`.
- [ ] `SymExpr` = `Lin` + a `nonlinear` flag. `se_from_ast(Node, env) -> SymExpr`:
      INT_LIT→const; IDENT→var (or env lookup); ADD/SUB→lin add/sub; unary NEG;
      MUL/DIV/MOD where **both** sides const → fold, else `nonlinear=true`.
- [ ] Unit-style smoke in a scratch test (not committed): `(x+1)+(x-1) == 2x`.
- **Verify:** compiles; a tiny `--verify` on a hand-built case folds correctly
      (temporary print).

## Task 3 — Constraints from boolean AST (De Morgan + case split)

- [ ] `Constraint { Lin lhs; enum {LT,LE} op; Rat rhs }` (normalize `>`/`>=` by
      negating to `<`/`<=`; `EQ` → two LEs; strict vs non-strict tracked).
- [ ] `constraints_of(Node *bool_ast, env, int negated) -> ConsResult`:
      - comparison atom → 1 constraint (negated flips op);
      - `OP_AND` → union; under negation (De Morgan) becomes a **case split**;
      - `OP_OR` → case split; under negation becomes union;
      - `UOP_NOT` → recurse with flipped `negated`;
      - anything else → `CONS_UNKNOWN`.
      A `ConsResult` is a **DNF**: list of branches, each branch a conjunction of
      constraints. Case split = concatenate branches; union = cartesian-product
      branches. (Keeps `A || B` in ensures and `&&` in path conditions uniform.)
- **Verify:** `n <= 1 || output >= n` yields 2 branches; its negation yields 1
      branch with 2 constraints (checked via temporary print).

## Task 4 — Fourier–Motzkin satisfiability + Farkas certificate

- [ ] `fm_sat(vec<Constraint>) -> {UNSAT, SAT}` over rationals: eliminate vars
      one at a time; for the eliminated var, combine every lower bound with every
      upper bound; detect contradiction (`0 <= c` with `c < 0`, or `0 < c` with
      `c <= 0`).
- [ ] Record a **Farkas certificate** for UNSAT (the positive linear combination
      of original constraints that yields the contradiction); independently
      re-check the certificate by rational evaluation before trusting PROVEN.
- [ ] PROVEN rule: obligation `ANTE ⇒ ENS` per ensures branch =
      `fm_sat(ANTE_constraints ∪ ¬ENS_branch)` is UNSAT for **every** ENS branch.
- **Verify:** `{n<=1, 1<=0·n... }`-style UNSAT detected; certificate re-checks.

## Task 5 — Counterexample search (REFUTED, sound)

- [ ] When an obligation's constraint system is SAT, search for an **integral**
      witness: enumerate small candidates per variable (0, ±1, bounds ±{0,1},
      then a bounded grid), evaluate the *original* AST (requires, path, ensures)
      by direct interpretation; a witness that satisfies ante and violates ensures
      ⇒ REFUTED with the assignment printed.
- [ ] No integral witness found ⇒ UNKNOWN (never REFUTED without a real witness,
      never PROVEN).
- [ ] A small AST interpreter `eval_bool(Node, env)` / `eval_i64` for witnesses
      (also reused by Task 8 oracle).
- **Verify:** `bad_abs` (`return x`, ensures `output>=0`) → REFUTED `x = -1`.

## Task 6 — Symbolic execution + VC generation

- [ ] `State { env: name↦SymExpr; path: vec<Constraint> (DNF branches); }`.
- [ ] `symexec` over: LET/ASSIGN (linear RHS → bind; nonlinear → mark fn SKIP),
      IF (fork: then adds `cond` constraints, else adds `¬cond`; if cond not
      convertible → SKIP fn), RETURN (emit obligation `(path, return_symexpr)`),
      implicit final-expr return. WHILE/FOR/MATCH/CALL-to-other-fn/effects →
      SKIP fn with reason.
- [ ] Per obligation, per ensures clause: substitute `output := return_symexpr`
      into the ensures AST (`subst_output`), then Task 3/4/5.
- [ ] A function verifies only if **all** obligations × ensures clauses are
      PROVEN; any REFUTED ⇒ REFUTED; else UNKNOWN.
- **Verify:** `abs_val` both paths PROVEN; `max2` PROVEN.

## Task 7 — Spec integration + reporting

- [ ] For each `NODE_FN` with a body, find its spec by name; gather
      `requires` (added to every obligation's antecedent) and `ensures`.
- [ ] Purity/fragment gate: skip (with reason) functions with effects, recursion
      (self-call), or unsupported statements — printed as `SKIPPED (reason)`,
      never silently passed.
- [ ] Final report format per spec §4 (BG diagnostics: `ДОКАЗАНО` /
      `ОБРОЧЕНО` + counterexample / `НЕ МОГА ДА РЕША` + reason / `ПРОПУСНАТО`).
- **Verify:** `examples/spec_ensures.baga` (`факториел`) — recursion ⇒ SKIPPED
      (honest); linear examples report correctly.

## Task 8 — Examples + soundness/completeness/oracle tests

- [ ] `examples/verify/`: `abs_val.baga`, `max2.baga`, `clamp.baga` (PROVEN);
      `bad_abs.baga`, `bad_max.baga` (REFUTED); `nonlinear.baga` (UNKNOWN);
      `recursive.baga` (SKIPPED).
- [ ] **Soundness suite (gates make test):** every REFUTED/UNKNOWN example must
      *actually* have a counterexample confirmed by `--test-specs`/runtime;
      assert `--verify` output never contains `ДОКАЗАНО` for the bad ones.
- [ ] **Completeness:** PROVEN examples all report `ДОКАЗАНО`.
- [ ] **Oracle agreement:** PROVEN ⇒ `--test-specs` finds no violation.
- [ ] Wire a `=== verify (static) ===` block into `make test`.
- **Verify:** `make test` green end-to-end; `make self` green.

## Task 9 — Final validation + commit

- [ ] `make` clean (`-Wall -Wextra` no new warnings); `make test`; `make self`.
- [ ] Propose commit; commit + push only on explicit user instruction.
