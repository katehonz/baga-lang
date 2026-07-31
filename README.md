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

Zero dependencies. Only `gcc` and `make`.

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

Baga compiles itself:

```
C bootstrap → compiler.baga → C → gcc → baga2
baga2       → compiler.baga → C → gcc → baga3
baga2 == baga3 ✓
```

The self-hosted compiler is ~960 lines of Baga.

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
| `--ast` | Print AST (debug) |
| `--tokens` | Print tokens (debug) |
| `--specs` | Print spec documentation |
| `--proofs` | Extract proof sketches |

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
| 4 | Effect system (!IO, ?, catch) | ✅ |
| 5 | Spec verification | ✅ |
| 6 | Proof extraction | ✅ |
| 3 | Cranelift / LLVM backends | ⏳ |

## Philosophy

Baga is a *bagatur* — a Bulgarian warrior. It fights alone. It depends on no one.

- Compiler in C. Zero dependencies. `gcc` has been on every machine since 1987.
- Self-hosting as a rite of passage.
- Cyrillic identifiers. Because language is identity.

> *Nothing is new. But nothing is timely. Linear logic is from 1987. It took 30 years to become Rust. Effect systems are from 2003. Maybe now is the time.*

## License

MIT.
