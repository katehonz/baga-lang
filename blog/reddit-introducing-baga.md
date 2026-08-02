# I built a programming language where the compiler statically proves AI-written code against specs — with counterexamples. Errors are type dimensions. It compiles itself.

**Baga** (Бага, Bulgarian for "a warrior who fights alone") is a programming language I've been building that started from a simple observation: *nothing in programming is new, but the right recombination at the right time can be.*

Rust is linear logic (1987) + region-based memory management (1994) + Cyclone (2001). Haskell is category theory (1950s) made practical. Every language is a recombination. So I asked: **what hasn't been glued together yet?**

The answer, I think: a language designed for the workflow where **AI writes the implementation and the compiler is the judge** — with the judge actually able to *prove* things, not just type-check them.

## The Three Pillars

### 1. Specs are first-class citizens — and statically enforced

```baga
spec sum_to {
    input:
        n: i64
    output: i64
    requires:
        n >= 0
    ensures:
        0 <= output
    decreases:
        n
}

fn sum_to(n: i64) -> i64 {
    if n < 1 { return 0 }
    let r = sum_to(n - 1)
    return n + r
}
```

The human writes the spec. AI writes the implementation. The compiler **proves or refutes the implementation against the spec, statically, before anything runs**:

```
$ ./baga --verify term_dec.baga
verify sum_to:
  ensures #1 (0 <= output): ДОКАЗАНО          # PROVEN
  (терминация: доказана чрез decreases — пълна коректност)
  извикване (requires на 'sum_to' при извикване): ДОКАЗАНО
  извикване (терминация: decreases намалява ...): ДОКАЗАНО
```

(The compiler speaks Bulgarian. ДОКАЗАНО = PROVEN, ОБРОЧЕНО = REFUTED, контрапример = counterexample. Language is identity.)

And when the AI gets it wrong, it doesn't get a warning — it gets a **refutation with a concrete counterexample**:

```
verify bad_abs:
  ensures #1 (output >= 0): ОБРОЧЕНО
    контрапример: x = -1
```

This isn't Design by Contract (Eiffel, 1986) with runtime checks. It's not an SMT black box either: the verifier is a small, auditable engine (Fourier–Motzkin elimination over the rationals + symbolic execution + Hoare rules) that is **sound by construction** — the only path to PROVEN is showing the negated obligation is unsatisfiable even over the rationals, which implies unsatisfiable over the integers. Anything outside the fragment is honestly reported UNKNOWN, never falsely proven. Every reported counterexample is re-checked by direct evaluation.

What it covers today: linear arithmetic (integer-exact — strict integer inequalities are tightened, so `n > 0 ⇒ n >= 1` proves), `while` loops with invariants, array bounds safety, element invariants (`v[*] >= 0`), a `sorted(v)` relational axiom, **recursion via assume–guarantee** (partial correctness), **full correctness via `decreases`**, and **products of linear forms** (`x*x >= 0` always; `fa >= 1 ∧ fb >= 1 ⇒ fa*fb >= 1`).

And every refutation passes a **conclusiveness gate**: the reported inputs must violate the contract for *every* value of the verifier's internal abstract variables — otherwise the answer is UNKNOWN, never a false alarm.

The flagship: **factorial, fully proven** — a recursive, non-linear function, with termination, no SMT solver anywhere:

```baga
spec fact {
    input: n: i64
    output: i64
    requires: n >= 0
    ensures: output >= 1
    decreases: n
}

fn fact(n: i64) -> i64 {
    if n <= 0 { return 1 }
    let r = fact(n - 1)   // induction hypothesis: r >= 1
    return n * r          // n >= 1, r >= 1 ⇒ n * r >= 1
}
```

```
verify fact:
  ensures #1 (output >= 1): ДОКАЗАНО
  (терминация: доказана чрез decreases — пълна коректност)
  извикване (requires на 'fact' при извикване): ДОКАЗАНО
  извикване (терминация: decreases намалява ...): ДОКАЗАНО
```

And because the consumer is an AI agent, the judge has a machine API:

```
$ ./baga --verify --json bad_abs.baga
{"functions": [{"name": "bad_abs", "ensures": [{"text": "output >= 0",
  "result": "refuted", "counterexample": [{"name": "x", "value": -1}]}], ...}]}
```

The loop writes itself: agent emits code → `baga --verify --json` → refuted with counterexample → agent fixes → PROVEN. No LLM judging LLM.

### 2. Errors are type dimensions, not exceptions

```baga
fn read_file(path: str) -> str !IO !NotFound {
    let handle = open(path)?      // !IO, !NotFound propagate
    let content = read(handle)?   // !IO, !Permission propagate
    content
}

fn main() {
    let content = read_file("data.txt")
        catch !NotFound => "empty"
        catch !IO => "error"
    print(content)
}
```

`String !IO !NotFound` is a **different type** from `String`. Effects compose automatically when you call functions. Unhandled effects are **compile-time errors**:

```
error: unhandled effect !IO in 'main' — declare it in the return type or catch it
```

This is an effect system (Plotkin & Power, 2003) with explicit effect polymorphism and effect inference. Koka does this academically. Baga does it practically. The `?` operator propagates effects. `catch !E => handler` removes an effect from the set. Effect sets form a join-semilattice under union.

### 3. The compiler extracts proofs from your code

```
proofs for sum_to:
  theorem sum_to_signature:
    ∀ n: i64. sum_to(n) → i64

  theorem sum_to_terminates:
    ∀ n: i64. terminates(sum_to(n))
    evidence: base case with early return, 2 return paths

  theorem sum_to_pure:
    sum_to is pure (no declared effects)

  theorem sum_to_ensures_1:
    requires: n >= 0
    ensures: 0 <= output
    status: ДОКАЗАНО (статично, Fourier–Motzkin)
```

Not Coq. Not Lean. **Readable text** that a human (or AI) can verify — and the contract theorems are backed by the static verifier, not just extracted wishes. This is the **reverse** of Coq's proof extraction. Coq extracts programs from proofs. Baga extracts proof sketches from programs.

## Why now?

| Before | Now |
|---|---|
| Human writes code | AI writes code, human verifies |
| Errors are runtime surprises | Errors must be visible in the type |
| Proofs are for mathematicians | Proofs must be automatic and readable |

Rust answered "how do we prevent segfaults." Good answer. But the question in 2026 isn't "how do we prevent segfaults." The question is **"how do we trust code we didn't write?"**

## The language

```baga
// Yes, Cyrillic identifiers work. Because language is identity.
fn факториел(n: i64) -> i64 {
    if n <= 1 { return 1 }
    n * факториел(n - 1)
}

fn main() {
    for i in 0..15 {
        print(факториел(i))
    }
}
```

Features: `fn`, `let`/`let mut`, `if`/`else`, `while` (with loop invariants), `for..in`, `match`, structs, enums, `str`, `i64`, `f64`, `bool`, `bytes`, `Vec`, string interpolation, effects (`!IO`, `?`, `catch`), specs with `requires`/`ensures`/`decreases`, proof extraction, FFI (`extern fn`), imports. Ships with working HTTP and JWT (HS256) libraries written in Baga.

## It compiles itself

```
C bootstrap → compiler.baga → C → gcc → baga2
baga2       → compiler.baga → C → gcc → baga3
baga2 == baga3 ✓   (fixed point)
```

The self-hosted compiler is **~3200 lines of Baga**. The C bootstrap is ~10k lines of C with **zero dependencies** (just gcc and make). There's also an LLVM IR backend (LLVM 14 C API).

Trust is engineered in, not hoped for:
- **Self-hosting fixed point** — the self-compiler reproduces itself byte-for-byte (`make self`).
- **Backend oracle** — the LLVM backend's output is diffed against the C backend's on every example (`lli-14`, 21/21).
- **Verifier oracle** — `--test-specs` property-tests every contract the static verifier calls PROVEN; they must agree.
- All of the above is one command (`make test`, `make self`) and is wired into CI (`.github/workflows`, picked up by both GitHub Actions and Gitea Actions).

## Current state

This is a **working prototype**, not a production language — but the core claim is real and testable in five minutes: `make && ./baga --verify examples/verify/fact_full.baga`. The verifier's fragment is deliberately small and it says UNKNOWN rather than guessing; general non-linear arithmetic (polynomials of higher degree, division by variables) is the remaining staircase. Effects are compile-time only (erased in codegen).

## Links

- Source: [git.bara-lang.org](https://git.bara-lang.org/baga-lang-ai/baga-lang-ai)
- Docs: theory (type theory, effect systems, proof theory), language reference, and compiler architecture — in English and Bulgarian
- License: MIT

## What's next

- General non-linear contracts (higher-degree polynomials)
- A REPL
- Editor support beyond the VS Code syntax highlighting that ships in the repo

---

*Nothing is new. But nothing is timely. Linear logic is from 1987. It took 30 years to become Rust. Effect systems are from 2003. Maybe now is the time.*

🐆
