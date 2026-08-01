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

The self-hosted compiler is ~960 lines of Baga. It reads its input file from
`arg(0)` and emits C on stdout. `make self` checks the fixed point: the C that
`baga2` generates for `compiler.baga` is byte-identical to what `baga3`
generates — i.e. `baga2` and `baga3` are the same compiler.

## Backends

Three backends, one AST. The C transpiler is the default and needs nothing
beyond `gcc`; the other two are optional and are validated byte-for-byte against
it by oracles in `make test`.

| Backend | Build | Run | Needs | Oracle |
|---|---|---|---|---|
| C transpiler (default) | `make` | `./baga file.baga` | `gcc`, `make` | — (reference) |
| LLVM IR | `make llvm` | `./baga-llvm --emit-llvm file.baga` → `lli-14` | LLVM 14 | 17/17 OK |

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
| `--emit-c` | Generate C code to stdout |
| `--emit-llvm` | Generate LLVM IR to stdout (`baga-llvm`, `make llvm`) |
| `--ast` | Print AST (debug) |
| `--tokens` | Print tokens (debug) |
| `--specs` | Print spec documentation |
| `--proofs` | Extract proof sketches |
| `--test-specs` | Property-based test of spec contracts (random inputs, deterministic seed) |
| `--verify` | Static verification of `requires`/`ensures` (sound; linear i64, loops via invariants, no recursion — see below) |

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
| 7 | **Static** spec verification (`--verify`): M0 linear + M1 loops + M2 array bounds + M3 element invariants | 🟡 in progress |

### Static verification (`--verify`)

`--verify` proves or refutes `requires`/`ensures` **statically** for pure,
non-recursive functions over `i64` with **linear** arithmetic. It is **sound by
construction**: the only path to "ДОКАЗАНО" (proven) is showing the negated
obligation is unsatisfiable even over the rationals (Fourier–Motzkin), which
implies unsatisfiable over the integers. A refuted contract carries a concrete
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

Recursion and non-linear terms are still skipped honestly. `vec_slice` and
`vec_concat` are now language builtins (returning a fresh `Vec`), and the
verifier propagates element invariants through them — a slice inherits the
source's invariants; a concat inherits the invariants **both** operands share.
Remaining staircase: relational invariants (`sorted`), then non-linear
reasoning.

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
