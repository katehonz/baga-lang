# Baga ⚔️

**A programming language for the age of AI.** Spec-first verification. Effects as type dimensions. Automatically extracted proofs.

> *"The question is not 'what is new'. The question is 'what has not been glued together yet'."*

Baga (Бага) is a programming language built on three pillars:

1. **Spec-first verification** — specifications are first-class citizens. The compiler checks implementations against specs.
2. **Effects as type dimensions** — `String !IO !NotFound` is a different type from `String`. Errors are visible in the type system.
3. **Automatic proof extraction** — the compiler extracts human-readable theorems from code. Not Coq. Not Lean. Readable text.

## Quick Start

```bash
make
./baga examples/zdravei.baga
# Здравей, багатуре. Боят започва.
```

Or try it in the browser — `python3 playground/serve.py` → http://localhost:8080
(see [playground/README.md](playground/README.md)).

Zero dependencies for the core compiler — only `gcc` and `make`. The LLVM
backend is optional (see [Backends](#backends)).

## The Three Pillars

### 1. Spec-First Verification

```baga
spec sort {
    input:
        arr: [i64]
    output: [i64]
    guarantees:
        - output is sorted
        - output has same elements as input
}

fn sort(arr: [i64]) -> [i64] {
    // AI writes this. The compiler checks it against the spec.
    // If the implementation violates a guarantee — rejection, not warning.
}
```

The human is the architect. AI is the consumer. The compiler is the judge.

### 2. Effects as Type Dimensions

```baga
fn read_file(path: str) -> str !IO !NotFound {
    // This function has effects: IO and NotFound
}

fn main() {
    // Every effect dimension must be handled
    let content = read_file("data.txt")
        catch !NotFound => "empty"
        catch !IO => "error"
    print(content)
}
```

Effects compose automatically. Unhandled effects are compile-time errors:

```
error: unhandled effect !IO in 'main' — declare it in the return type or catch it
```

### 3. Proof Extraction

```bash
$ ./baga --proofs examples/faktorial.baga
```

```
proofs for факториел:
  theorem факториел_signature:
    ∀ n: i64. факториел(n) → i64

  theorem факториел_terminates:
    ∀ n: i64. terminates(факториел(n))
    evidence: base case with early return, 2 return paths

  theorem факториел_pure:
    факториел is pure (no declared effects)
```

Not formal proofs. Readable specifications that humans (or AI) can verify.

## Examples

```baga
// Fibonacci with while loop
fn фибоначи(n: i64) -> i64 {
    let mut a: i64 = 0
    let mut b: i64 = 1
    let mut i: i64 = 0
    while i < n {
        let temp = b
        b = a + b
        a = temp
        i = i + 1
    }
    return a
}

fn main() {
    for i in 0..15 {
        print(фибоначи(i))
    }
}
```

```baga
// Structs and match
struct Точка { x: f64, y: f64 }

fn разстояние(a: Точка, b: Точка) -> f64 {
    let dx = a.x - b.x
    let dy = a.y - b.y
    dx * dx + dy * dy
}
```

```baga
// Enums
enum Цвят { Червено, Зелено, Синьо }

fn име(ц: i64) -> str {
    match ц {
        0 => "червено",
        1 => "зелено",
        2 => "синьо",
        _ => "непознато",
    }
}
```

## Self-Hosting

Baga compiles itself — reproducibly, via `make self`:

```
C bootstrap → compiler.baga → C → gcc → baga2
baga2       → compiler.baga → C → gcc → baga3
baga3       → compiler.baga → C
baga2 == baga3 ✓   (fixed point: the self compiler reproduces itself)
```

The self-hosted compiler is ~2660 lines of Baga. It reads its input file from
`arg(0)` and emits C on stdout. `make self` checks the fixed point: the C that
`baga2` generates for `compiler.baga` is byte-identical to what `baga3`
generates — i.e. `baga2` and `baga3` are the same compiler. The self compiler
covers the language used by `examples/` — structs, enums, match, effects
(`catch`/`?`), `Vec<T>`, bytes, string interpolation, specs with runtime
contracts, bitwise/shift operators, the arena allocator, and `extern fn` FFI —
and its output is behavior-identical to the C bootstrap on every example.

## Backends

Three backends, one AST. The C transpiler is the default and needs nothing
beyond `gcc`; the other two are optional and are validated byte-for-byte against
it by oracles in `make test`.

| Backend | Build | Run | Needs | Oracle |
|---|---|---|---|---|
| C transpiler (default) | `make` | `./baga file.baga` | `gcc`, `make` | — (reference) |
| LLVM IR | `make llvm` | `./baga-llvm --emit-llvm file.baga` → `lli-14 -load lib/libbaga_par.so` | LLVM 14 | oracle (incl. `!Par`) |

- **C transpiler** — emits C, compiles with `gcc`, runs. Full language coverage.
- **LLVM** — emits LLVM IR directly from the AST (`src/codegen_llvm.c`). Full
  coverage; `tests/llvm_oracle.sh` diffs it against the C backend via `lli-14`.

## Project Structure

```
baga/
├── include/baga.h          # AST, tokens, types
├── src/
│   ├── main.c              # CLI
│   ├── lexer.c             # Tokenizer
│   ├── parser.c            # Recursive descent parser
│   ├── checker.c           # Type checking + effect checking
│   ├── codegen_c.c         # C code generator
│   ├── codegen_llvm.c      # LLVM IR backend (optional, `make llvm`)
│   ├── verify.c            # Static spec verification (`--verify`)
│   └── proofs.c            # Proof extraction
├── self/
│   ├── lexer.baga          # Self-hosted lexer
│   ├── parser.baga         # Self-hosted parser
│   └── compiler.baga       # Self-hosted compiler (~960 lines)
├── examples/               # Example programs
├── docs/                   # Documentation (EN + BG)
├── Makefile
└── README.md
```

## CLI Flags

| Flag | Description |
|---|---|
| (none) | Compile and run |
| `--check` / `--lib` | Parse + typecheck only (no `main` required) — for library modules |
| `--emit-c` | Generate C to stdout (libraries without `main` OK; no C `main` wrapper) |
| `--emit-llvm` | Generate LLVM IR to stdout (`baga-llvm`, `make llvm`) |
| `--ast` | Print AST (debug) |
| `--tokens` | Print tokens (debug) |
| `--specs` | Print spec documentation |
| `--proofs` | Extract proof sketches (includes static verification status for specs) |
| `--test-specs` | Property-based test of spec contracts (random inputs, deterministic seed) |
| `--verify` | Static verification of `requires`/`ensures` (sound; linear i64, loops via invariants, no recursion — see below) |
| `--json` | Machine-readable JSON output for `--verify` (verdicts + counterexamples; for AI agents and CI) |

## Documentation

- [Theory & Mathematics (EN)](docs/theory-en.md) — Type theory, effect systems, proof theory
- [Теория и Математика (BG)](docs/theory-bg.md) — Теория на типовете, ефектови системи, теория на доказателствата
- [Language Reference (EN)](docs/language-en.md) — Syntax, types, semantics
- [Езикова Справка (BG)](docs/language-bg.md) — Синтаксис, типове, семантика
- [Compiler Architecture (EN)](docs/compiler-en.md) — Pipeline, AST, codegen
- [Архитектура на Компилатора (BG)](docs/compiler-bg.md) — Конвейер, AST, кодогенерация

## Roadmap

| Phase | What | Status |
|---|---|---|
| 1 | C bootstrap compiler | ✅ |
| 2 | Self-hosting (baga2 == baga3) | ✅ |
| 3 | LLVM backend | ✅ |
| 4 | Effect system (!IO, ?, catch) | ✅ |
| 5 | Spec verification — runtime contracts (`requires`/`ensures`) | ✅ |
| 6 | Proof extraction | ✅ |
| 7 | **Static** spec verification (`--verify`): M0–M8 + **M9** product sign table + const division | ✅ |
| 8 | General non-linear reasoning | ✅ M9–M10 (sign/div/mod, square dom, mono, n=qk+r) |
| 9 | Concurrency (`!Par`, `go`/`go_bg`/`join`/`detach`, channels, mutex) — cloud accept loops | ✅ M1 |
| 10 | LLVM backend `!Par` parity (`libbaga_par.so` + lli `-load`) | ✅ |

### Static verification (`--verify`)

`--verify` proves or refutes `requires`/`ensures` **statically** for pure
functions over `i64` with **linear** arithmetic (recursion since M5 — partial
correctness, see below). It is **sound by
construction**: the only path to "ДОКАЗАНО" (proven) is showing the negated
obligation is unsatisfiable over the rationals (Fourier–Motzkin, with integer
tightening so strict integer inequalities are exact — M7), which implies
unsatisfiable over the integers. A refuted contract carries a concrete
counterexample; anything undecidable in the fragment is reported "НЕ МОГА ДА
РЕША" (unknown) — never falsely proven.

- **M0** — straight-line code and `if`/`else`.
- **M1** — `while` loops with user-supplied invariants, verified by the Hoare
  rule (init + preservation). The post-loop invariant is trusted **only** if
  both checks are proven (a soundness gate — an unproven invariant yields
  UNKNOWN downstream, never a false proof):
  ```baga
  while i < m invariant s >= 0, i >= 0 {
      s = s + k
      i = i + 1
  }
  ```
- **M2** — **array bounds safety**: every `vec_get`/`vec_set` index is checked
  against the symbolically tracked vector length (`vec_new` → 0, `vec_push`
  → +1, `Vec` parameters → a symbolic length constrained by `requires
  vec_len(v) ...`). Proves accesses in range, or refutes with a counterexample.
  Lengths flow through loops via the invariant mechanism.
- **M3** — **element invariants**: a `v[*] >= c` annotation (in `requires` or a
  loop `invariant`) means "every element of `v` satisfies the predicate". It is
  stored as a quantified axiom and **instantiated** at each concrete `vec_get`
  index, so an `ensures` about a read element follows. `vec_push(v, e)` and
  `vec_set(v, k, e)` preserve the axiom when `e` provably satisfies the
  predicate (otherwise the axiom is dropped — sound). Example:
  ```baga
  requires: v[*] >= 0, vec_len(v) >= 1
  ensures:  output >= 0
  fn first(v: [i64]) -> i64 { return vec_get(v, 0) }   // proven
  ```

- **M5** — **recursion and modular calls (assume–guarantee)**: a call to a
  user function with a spec, in statement position (`let x = f(args)`,
  `return f(args)`, or a bare call), is verified by contract: the callee's
  `requires` are **discharged** against the caller path (a violation is
  refuted with a counterexample, e.g. `f(n - 1)` where the callee needs
  `m >= 1`), and only if they are all proven — and the callee's own body is
  verifiable — the callee's conjunctive `ensures` are **assumed** for the
  result. For a recursive callee this is exactly the induction hypothesis of
  the Hoare rule for recursion, so the verdict is **partial correctness**:
  the output marks it honestly (`рекурсия: частична коректност — терминацията
  не се доказва`; JSON `"partial_correctness": true`). Calls nested inside
  expressions (`n * fact(n-1)`) stay honestly skipped.
  ```baga
  spec sum_to {
      input: n: i64
      output: i64
      requires: n >= 0
      ensures: 0 <= output
  }
  fn sum_to(n: i64) -> i64 {
      if n <= 0 { return 0 }
      let r = sum_to(n - 1)   // induction hypothesis: r >= 0
      return n + r
  }
  // ensures ДОКАЗАНО; requires при извикване ДОКАЗАНО (partial correctness)
  ```

- **M6** — **termination via `decreases`**: a `decreases: <expr>` clause in the
  spec (i64 over the inputs) upgrades recursion from partial to **full
  correctness**: the verifier discharges `D >= 0` at entry (from `requires`)
  and `D' >= 0 ∧ D' < D` at every self-recursive call. A measure that does
  not decrease is refuted with a counterexample (and the function honestly
  falls back to partial correctness). Output: `терминация: доказана чрез
  decreases`; JSON `"termination": "proven"`.
- **M7** — **integer-exact reasoning**: over ℤ, `lhs < rhs ⟺ lhs <= rhs - 1`
  when all coefficients are integers, so every integer strict inequality is
  tightened before Fourier–Motzkin runs. This closes the classic rational gap
  (`n > 0 ⇒ n >= 1` is now PROVEN) without any branching. Soundness is
  untouched: the tightened system has exactly the same integer solutions, so
  nothing false is ever proven, and anything ℤ-satisfiable stays
  ℚ-satisfiable (counterexamples survive).

- **M8** — **products of linear forms + no false alarms**. A non-constant
  product `x * y` becomes a fresh symbolic variable with the factors
  remembered; the verifier injects only provable facts: `x*x >= 0` always,
  `fa >= 1 ∧ fb >= 1 ⇒ fa*fb >= 1`, `fa >= 0 ∧ fb >= 0 ⇒ fa*fb >= 0`. That is
  enough to prove **factorial, fully** (`fact_full.baga`: `output >= 1` via
  `n >= 1, r >= 1 ⇒ n*r >= 1`, plus `decreases` termination). In the witness
  search product values are **derived concretely from their factors**, so a
  refutation like `x*y >= 0` at `x = -1, y = 1` is real, not abstract.
  Equally important: a **conclusiveness gate** on every refutation — the
  pinned inputs (and reads/lengths) must make the claim unsatisfiable for
  *every* value of the abstract call/product vars; otherwise the verdict is
  UNKNOWN. Before M8, `caller(n) = g(n)` could be "refuted" for a true
  contract via an unrealizable abstract value — a false alarm. Never again.

- **M9** — **full product sign table + division by constant** (Phase 8 first
  slice). Products also get: both non-positive ⇒ `p >= 0`; mixed signs ⇒
  `p <= 0`; both `<= -1` ⇒ `p >= 1`; mixed `>=1`/`<=-1` ⇒ `p <= -1`. Integer
  division `n / k` for non-zero integer constant `k` becomes a fresh `__d`
  with C trunc-toward-zero sign axioms (`n>=0,k>0 ⇒ q>=0`, etc.). Witnesses
  derive `q = n/k` concretely. Examples: `sign_prod.baga`, `div_const.baga`,
  `sum_sq.baga`. **M9b** adds `n % k` for nonzero constant `k` with bounds
  `0 <= n%k < k` when `n >= 0` (`mod_const.baga`).

- **M10** — **square dominance, product monotonicity, div–mod identity**.
  For `s = v*v` over ℤ: `s >= 0`, `s >= v`, `s >= -v` (via consecutive
  products). For `p = fa*fb`: if `fa>=0, fb>=1` then `p >= fa`; if
  `fa>=1, fb>=0` then `p >= fb`. When both `n/k` and `n%k` appear for the
  same constant `k`, inject `n = k*q + r`. Also fixed `¬(a==b)` conversion
  so `ensures output == n` is provable. Examples: `poly_depth.baga`,
  `div_mod_id.baga`. Still honest-UNKNOWN for division/mod by a variable.

`vec_slice` / `vec_concat` propagate element invariants; `sorted(v)` is
supported. Remaining: richer polynomials, variable divisors, feeding verified
invariants into `--proofs`.

> **Language note (M2):** `[T]` is now sugar for `Vec<T>` (the growable
> `baga_Vec`), not a raw C pointer — so vector parameters can be written
> `v: [i64]` and used with the `vec_*` builtins uniformly.

## Philosophy

Baga is a *bagatur* — a Bulgarian warrior. It fights alone. It depends on no one.

- Compiler in C. Zero dependencies. `gcc` has been on every machine since 1987.
- Optional backends (LLVM) are additive — the core stays dependency-free.
- Self-hosting as a rite of passage.
- Cyrillic identifiers. Because language is identity.

> *Nothing is new. But nothing is timely. Linear logic is from 1987. It took 30 years to become Rust. Effect systems are from 2003. Maybe now is the time.*

## License

MIT.
