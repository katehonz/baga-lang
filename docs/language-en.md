# Baga Language Reference

> A complete reference for the Baga programming language: syntax, types,
> semantics, the effect system, the spec system, builtins, and examples.

Baga (Бага) is a compiled, statically typed language built on three ideas:

1. **Spec-first verification** — `spec` is a keyword; the compiler checks
   implementations against their specifications.
2. **Effects as type dimensions** — `str !IO !NotFound` is a *different type*
   from `str`. Errors live in the type system, not in runtime surprises.
3. **Automatic proof extraction** — the compiler emits human-readable proof
   sketches from your code.

Baga transpiles to C and compiles with `gcc`. It has zero runtime
dependencies. Identifiers may be written in Latin or Cyrillic — language is
identity.

---

## 1. Getting Started

```baga
fn main() {
    print("Здравей, багатуре. Боят започва.")
}
```

```bash
$ baga examples/zdravei.baga
Здравей, багатуре. Боят започва.
```

Every program must define a `main` function with no parameters. Execution
starts there. A missing `main` is a compile-time error.

---

## 2. Lexical Structure

### 2.1 Comments

```baga
// A line comment runs to the end of the line.

/* A block comment.
   Block comments NEST, so you can comment out code
   /* that itself contains comments */ safely. */
```

### 2.2 Identifiers

An identifier starts with a letter (`a`–`z`, `A`–`Z`), an underscore `_`, or
any UTF-8 multibyte character (this is what enables Cyrillic). Subsequent
characters may also be digits `0`–`9`.

All of these are valid identifiers:

```baga
let x = 1
let резултат = 2
let _private = 3
let име_на_потребител = "бага"
```

Keywords are reserved and may not be used as identifiers (see §2.4).

### 2.3 Literals

**Integers.** Decimal by default. Hexadecimal (`0x`), binary (`0b`), and
octal (`0o`) prefixes are supported. Underscores may separate digits for
readability. Integer literals have type `i64`.

```baga
let a = 42          // decimal
let b = 0xFF        // hexadecimal = 255
let c = 0b1010      // binary = 10
let d = 0o17        // octal = 15
let e = 1_000_000   // underscores ignored
```

**Floats.** A decimal point and/or an exponent make a literal a float of type
`f64`.

```baga
let pi = 3.14159
let avogadro = 6.022e23
let tiny = 1.0e-9
```

**Strings.** Double-quoted, type `str`. Escape sequences:

| Escape | Meaning        |
|--------|----------------|
| `\n`   | newline        |
| `\t`   | tab            |
| `\r`   | carriage return|
| `\\`   | backslash      |
| `\"`   | double quote   |
| `\0`   | null byte      |

```baga
let greeting = "Здравей, \"свят\"!\n"
```

**Characters.** Single-quoted, e.g. `'A'`. A character literal is an integer
(the byte value of the character), so it has type `i64`. Supported escapes:
`\n \t \r \\ \' \0`.

```baga
let code = 'A'   // 65
```

**Booleans.** `true` and `false`, type `bool`.

### 2.4 Keywords

```
fn  let  mut  if  else  while  for  in  return  match
struct  impl  spec  enum  true  false  catch  break  continue
```

---

## 3. Grammar (EBNF)

The grammar below is descriptive; Baga is parsed by a recursive-descent parser
with precedence climbing for binary operators. Semicolons are optional
statement terminators.

```ebnf
program        = { top_level } ;
top_level      = fn_decl | struct_decl | enum_decl | spec_decl ;

(* Declarations *)
fn_decl        = "fn", ident, "(", [ param, { ",", param } ], ")",
                 [ "->", type, { "!", ident } ], ( block | /* empty: forward decl */ ) ;
param          = ident, ":", type ;
struct_decl    = "struct", ident, "{", [ field_decl, { ",", field_decl } ], "}" ;
field_decl     = ident, ":", type ;
enum_decl      = "enum", ident, "{", [ ident, { ",", ident } ], "}" ;
spec_decl      = "spec", ident, "{",
                   "input:", [ param, { param } ],
                   "output:", type,
                   "guarantees:", { "-", free_text },
                 "}" ;

(* Types *)
type           = "&", type                       (* reference *)
               | "[", type, "]"                  (* array *)
               | "Vec", [ "<", type, ">" ]       (* vector; element only i64/str *)
               | ident ;                         (* named: i32 i64 f64 bool str ... *)

(* Statements *)
block          = "{", { statement }, "}" ;
statement      = let_stmt | return_stmt | break_stmt | continue_stmt
               | while_stmt | for_stmt | if_expr | expr_stmt ;
let_stmt       = "let", [ "mut" ], ident, [ ":", type ], "=", expression, [ ";" ] ;
return_stmt    = "return", [ expression ], [ ";" ] ;
break_stmt     = "break", [ ";" ] ;
continue_stmt  = "continue", [ ";" ] ;
while_stmt     = "while", expression, block ;
for_stmt       = "for", ident, "in", expression, block ;
expr_stmt      = expression, [ ";" ] ;

(* Expressions, lowest to highest precedence *)
expression     = assignment ;
assignment     = logical_or, [ assign_op, expression ] ;   (* right-assoc *)
assign_op      = "=" | "+=" | "-=" | "*=" | "/=" ;
logical_or     = logical_and, { "||", logical_and } ;
logical_and    = equality, { "&&", equality } ;
equality       = comparison, { ( "==" | "!=" ), comparison } ;
comparison     = additive, { ( "<" | ">" | "<=" | ">=" ), additive } ;
additive       = multiplicative, { ( "+" | "-" ), multiplicative } ;
multiplicative = unary, { ( "*" | "/" | "%" ), unary } ;
unary          = ( "-" | "!" | "&" | "*" ), unary | postfix ;
postfix        = primary, { call | index | range | field | try | catch } ;
call           = "(", [ expression, { ",", expression } ], ")" ;
index          = "[", expression, "]" ;
range          = "..", unary ;
field          = ".", ident ;
try            = "?" ;
catch          = "catch", "!", ident, "=>", unary ;

primary        = int_lit | float_lit | str_lit | char_lit
               | "true" | "false"
               | ident                           (* variable *)
               | struct_lit                      (* Name { field: expr, ... } *)
               | "(", expression, ")"
               | if_expr | match_expr | block ;

if_expr        = "if", expression, block, [ "else", ( block | if_expr ) ] ;
match_expr     = "match", expression, "{", { match_arm }, "}" ;
match_arm      = ( "_" | unary ), "=>", ( block | expression ), [ "," ] ;
struct_lit     = ident, "{", field_init, { ",", field_init }, "}" ;
field_init     = ident, ":", expression ;
```

---

## 4. Types

Baga is statically typed with local type inference. You may annotate types
explicitly or let the compiler infer them from initializers.

| Type    | Meaning                                  | C representation   |
|---------|------------------------------------------|--------------------|
| `i32`   | 32-bit signed integer                    | `int32_t`          |
| `i64`   | 64-bit signed integer (default for ints) | `int64_t`          |
| `f64`   | 64-bit floating point                    | `double`           |
| `bool`  | boolean (`true` / `false`)               | `int`              |
| `str`   | string (null-terminated, immutable)      | `const char *`     |
| `Vec`   | dynamic array (heap-allocated)           | `baga_Vec *`       |
| `Vec<T>` | vector with annotated element (`i64` or `str`) | `baga_Vec *` |
| `[T]`   | array of `T`                             | pointer            |
| `&T`    | reference to `T`                         | `T *`              |
| `void`  | no value (procedures)                    | `void`             |
| struct  | user-defined record                      | `struct`           |

### 4.1 Inference and numeric promotion

- An integer literal infers to `i64`.
- A float literal infers to `f64`. Literals are emitted at full precision
  (`%.17g`, an IEEE 754 double round-trip) — no lost digits.
- A string literal infers to `str`.
- `true` / `false` infer to `bool`.

When two numeric types meet in a binary operation, the result is promoted:

```
f64  if either operand is f64
i64  otherwise, if either operand is i64
i32  otherwise
```

```baga
let x = 10        // i64
let y = 3.5       // f64
let z = x + y     // f64 (i64 promoted to f64)
```

### 4.2 Effect-qualified types

Any type may carry effect tags. `str !IO` means "a string, but obtaining it
performs IO". See §13.

---

## 5. Variables and `let` Bindings

```baga
let x = 5                 // immutable, type inferred (i64)
let mut y: i64 = 10       // mutable, explicit type
let name: str = "бага"    // immutable with annotation

y = y + 1                 // OK: y is mut
// x = 6                  // (assignment to immutable — avoid)
```

- `let` introduces an immutable binding.
- `let mut` introduces a mutable binding that may be reassigned.
- The type annotation `: type` is optional; when omitted it is inferred from
  the initializer.
- A `let` binding is scoped to the enclosing block. Re-declaring a name in the
  same scope is an error (`повторно дефиниране`).

### 5.1 Assignment operators

```baga
x = expr      // plain assignment
x += expr     // x = x + expr
x -= expr     // x = x - expr
x *= expr     // x = x * expr
x /= expr     // x = x / expr
```

The compound forms desugar to a binary operation on the target and the value.

---

## 6. Functions

```baga
fn add(a: i64, b: i64) -> i64 {
    return a + b
}
```

- Parameters always carry an explicit type: `name: type`.
- At call sites the checker matches argument count **and types** against the
  parameters; a mismatch is a compile-time error (`i32`/`i64` are compatible).
- The return type follows `->`. Omit it for procedures that return nothing
  (`void`).
- Effects, if any, follow the return type: `-> str !IO !NotFound`.
- A function with no body (`fn f(x: i64) -> i64`) is a forward declaration.

### 6.1 Return values

Use `return expr` to return early. The value of the last expression in a block
is also used as an implicit return in expression positions (for example, in
`match` arms and `if` branches).

```baga
fn max(a: i64, b: i64) -> i64 {
    if a > b {
        return a
    }
    return b
}
```

### 6.2 Recursion

Functions may call themselves. Forward declarations let mutually recursive
functions refer to each other regardless of order.

```baga
fn факториел(n: i64) -> i64 {
    if n <= 1 {
        return 1
    }
    return n * факториел(n - 1)
}
```

---

## 7. Control Flow

### 7.1 `if` / `else`

The condition must be `bool`. `else if` chains are supported.

```baga
fn sign(n: i64) -> str {
    if n > 0 {
        return "положително"
    } else if n < 0 {
        return "отрицателно"
    } else {
        return "нула"
    }
}
```

`if` is also an expression and can appear on the right-hand side of a `let`
or `return`.

### 7.2 `while`

```baga
let mut i: i64 = 0
while i < 10 {
    print(i)
    i = i + 1
}
```

### 7.3 `for`

`for` iterates over a range `lo..hi`. The range is half-open: it includes `lo`
and excludes `hi`. The loop variable is a fresh `i64` binding scoped to the
body.

```baga
for i in 0..15 {
    print(i)        // 0, 1, 2, ... 14
}
```

### 7.4 `break` and `continue`

```baga
let mut i: i64 = 0
while true {
    i = i + 1
    if i == 3 { continue }   // skip to next iteration
    if i > 5 { break }       // exit the loop
    print(i)
}
```

---

## 8. `match`

`match` compares a value against patterns and evaluates the first matching arm.
Patterns may be literals, identifiers, or the wildcard `_`. Each arm body is
either an expression or a block.

```baga
fn описание(n: i64) -> i64 {
    let резултат = match n {
        0 => 100,
        1 => 200,
        2 => 300,
        _ => 999,
    }
    return резултат
}
```

The `_` wildcard matches anything and is conventionally placed last. An arm
whose body is a bare expression returns that value from the enclosing function.

---

## 9. Operators and Precedence

From loosest to tightest binding:

| Precedence | Operators                         | Associativity |
|------------|-----------------------------------|---------------|
| 0 (loosest)| `=` `+=` `-=` `*=` `/=`           | right         |
| 1          | `\|\|`                            | left          |
| 2          | `&&`                              | left          |
| 3          | `==` `!=`                         | left          |
| 4          | `<` `>` `<=` `>=`                 | left          |
| 5          | `+` `-`                           | left          |
| 6          | `*` `/` `%`                       | left          |
| 7          | unary `-` `!` `&` `*`             | right         |
| 8 (tightest)| postfix `()` `[]` `..` `.` `?` `catch` | left    |

### 9.1 Operator summary

| Category    | Operators                                        |
|-------------|--------------------------------------------------|
| Arithmetic  | `+` `-` `*` `/` `%`                              |
| Comparison  | `==` `!=` `<` `>` `<=` `>=`                      |
| Logical     | `&&` `\|\|` `!`                                  |
| Assignment  | `=` `+=` `-=` `*=` `/=`                          |
| Reference   | `&` (address-of) `*` (dereference)               |
| Range       | `..`                                             |
| Effect      | `?` (propagate) `catch !E => handler`            |

Two-character operators recognized by the lexer:

```
->  =>  ..  ==  !=  <=  >=  &&  ||  <<  >>  +=  -=  *=  /=
```

Arithmetic requires numeric operands; applying `+` to a string and an integer,
for example, is a compile-time error.

---

## 10. Structs

A `struct` is a named record of typed fields.

```baga
struct Точка {
    x: f64,
    y: f64,
}
```

Construct values with a struct literal and access fields with `.`:

```baga
fn разстояние(a: Точка, b: Точка) -> f64 {
    let dx = a.x - b.x
    let dy = a.y - b.y
    return dx * dx + dy * dy
}

fn main() {
    let a = Точка { x: 0.0, y: 0.0 }
    let b = Точка { x: 3.0, y: 4.0 }
    print(разстояние(a, b))   // 25
}
```

Accessing a field that does not exist on the struct is an error
(`непознато поле`). Constructing an undeclared struct is an error
(`непознат struct`).

---

## 11. Enums

An `enum` declares a set of named variants. Variants are integer-valued,
numbered from `0` in declaration order, and may be used wherever an `i64` is
expected.

```baga
enum Цвят {
    Червено,   // 0
    Зелено,    // 1
    Синьо,     // 2
}

fn име(ц: i64) -> str {
    match ц {
        0 => "червено",
        1 => "зелено",
        2 => "синьо",
        _ => "непознато",
    }
}

fn main() {
    let c = Зелено       // 1
    print(име(c))        // зелено
}
```

---

## 12. Strings and Vectors (Builtins)

Baga ships a small standard library of builtins. They are always in scope; no
import is needed.

### 12.1 Output

| Signature | Description |
|-----------|-------------|
| `print(x, ...)` | Print each argument followed by a newline. Dispatches on type. |
| `println(x, ...)` | Alias of `print`. |
| `write(s)` | Print a string with **no** trailing newline. |

`print` accepts integers, floats, strings, and booleans, printing each on its
own line.

### 12.2 String operations

| Signature | Description |
|-----------|-------------|
| `len(s: str) -> i64` | Byte length of `s`. |
| `char_at(s: str, i: i64) -> i64` | Byte value of the character at index `i`. |
| `substr(s: str, a: i64, b: i64) -> str` | Substring from index `a` (inclusive) to `b` (exclusive). |
| `concat(a: str, b: str) -> str` | Concatenate two strings into a new string. |
| `str_eq(a: str, b: str) -> bool` | True if the strings are byte-equal. |
| `chr(c: i64) -> str` | One-character string from a byte value. |
| `ord(s: str) -> i64` | Byte value of the first character of `s` (0 if empty). |

### 12.3 File IO

| Signature | Description |
|-----------|-------------|
| `read_file(path: str) -> str !IO` | Read an entire file. Carries the `!IO` effect. Returns `""` if the file cannot be opened. |

Because `read_file` has the `!IO` effect, calling it forces the caller to
either declare `!IO` in its own return type or handle it with `catch` (see
§13).

### 12.4 Vectors (dynamic arrays)

A `Vec` is a heap-allocated, growable array. Each vector carries an element
type — `Vec<i64>`, `Vec<str>`, or `Vec<f64>` — which is fixed by the first
`vec_push`/`vec_set`. Mixing element types in one vector is a compile-time
error.

| Signature | Description |
|-----------|-------------|
| `vec_new() -> Vec` | Create an empty vector with an unknown element type. |
| `vec_len(v: Vec) -> i64` | Number of elements. |
| `vec_push(v: Vec, x: i64 \| str \| f64)` | Append an element; the first push fixes the element type. |
| `vec_get(v: Vec, i: i64) -> i64 \| str \| f64` | Read the element at index `i` (the vector's element type). |
| `vec_set(v: Vec, i: i64, x: i64 \| str \| f64)` | Overwrite the element at index `i`. |
| `vec_push_str(v: Vec, s: str)` | Alias of `vec_push` for strings (legacy code). |
| `vec_get_str(v: Vec, i: i64) -> str` | Alias of `vec_get` for strings (legacy code). |
| `vec_set_str(v: Vec, i: i64, s: str)` | Alias of `vec_set` for strings (legacy code). |

```baga
fn main() {
    let v = vec_new()        // Vec<?>
    vec_push(v, 10)          // Vec<i64> — type is fixed
    vec_push(v, 20)
    print(vec_len(v))        // 2
    print(vec_get(v, 0))     // 10

    let sv = vec_new()
    vec_push(sv, "здравей")  // Vec<str>
    print(vec_get(sv, 0))    // здравей

    let fv = vec_new()
    vec_push(fv, 1.5)        // Vec<f64>
    print(vec_get(fv, 0))    // 1.5
}
```

Mixing types is caught by the checker:

```baga
let v = vec_new()
vec_push(v, 10)
vec_push(v, "грешка")   // ERROR: vec_push: елемент от тип str, но векторът е Vec<i64>
```

Supported element types: `i64` (and `i32`, accepted as `i64`), `str`, and `f64`.
The element type is a property of the type at the binding site.

`Vec<T>` is a valid type anywhere a type can be written — parameters,
`let` annotations, return types, and spec `input:`/`output:`. The annotation
carries the element type across the function boundary, so `vec_get` returns
`T` and a `vec_push` of another type is a compile-time error:

```baga
fn сума(v: Vec<i64>) -> i64 {
    let mut s: i64 = 0
    for i in 0..vec_len(v) {
        s += vec_get(v, i)     // i64 — from the annotation, not from push
    }
    return s
}
```

Plain `Vec` without `<T>` keeps its old behavior: unknown element, with
`vec_get` falling back to the historical default `i64`. An annotation with
an element other than `i64`/`str` (`Vec<f64>`) is a compile-time error.

---

## 13. The Effect System

Effects are *dimensions of a type*. A function that performs IO does not merely
return `str`; it returns `str !IO`. The compiler tracks these tags and refuses
to compile code that ignores them.

### 13.1 Declaring effects

Effects are listed after the return type, each prefixed with `!`:

```baga
fn прочети(път: str) -> str !IO !NotFound {
    return read_file(път)
}
```

Effect names are ordinary identifiers (`IO`, `NotFound`, `Permission`, ...).
You define the vocabulary; the compiler enforces the bookkeeping.

### 13.2 Propagating with `?`

The postfix `?` operator runs an effectful expression and *propagates* its
effects to the enclosing function. The enclosing function must then declare
those effects (or catch them).

```baga
fn обработи(път: str) -> str !IO !NotFound {
    let данни = прочети(път)?   // effects flow up into 'обработи'
    return данни
}
```

### 13.3 Handling with `catch`

`catch` removes a single effect dimension by providing a fallback value:

```baga
expr catch !Effect => fallback
```

Chain multiple `catch` clauses to handle several effects:

```baga
fn main() {
    let съдържание = прочети("данни.txt")
        catch !NotFound => "празно"
        catch !IO => "грешка"
    print(съдържание)
}
```

After both effects are caught, the expression is a plain `str` with no
remaining effects, so `main` (which returns `void`, no effects) type-checks.

### 13.4 The unhandled-effect error

If an effect reaches a function that neither declares it nor catches it, the
compiler reports:

```
file.baga: 4:1: необработен ефект !IO във 'main' — декларирай го в return типа или го хвани с catch
```

("unhandled effect !IO in 'main' — declare it in the return type or catch it").

This is the heart of the system: effects cannot be silently dropped.

---

## 14. The Spec System

A `spec` describes *what* a function must do; the function body describes
*how*. The compiler verifies that a spec and its function agree on shape, and
records the spec's guarantees for proof extraction.

```baga
spec сортирай {
    input:
        arr: i64
    output: i64
    guarantees:
        - output is sorted
        - output has same elements as input
}

fn сортирай(arr: i64) -> i64 {
    return arr
}
```

### 14.1 Structure of a spec

```
spec <name> {
    input:
        <param>: <type>
        ...
    output: <type>
    guarantees:
        - <free-text guarantee>
        - <free-text guarantee>
}
```

- The spec's `<name>` must match an existing function name.
- `input` lists parameters; their count and types must match the function.
- `output` is the return type; it must match the function's return type.
- `guarantees` are free-text lines, each beginning with `-`. They document the
  contract and are surfaced by `--proofs` and `--specs`.

### 14.2 What the compiler checks

The compiler rejects a program when a spec disagrees with its function:

| Mismatch | Error |
|----------|-------|
| Spec names a nonexistent function | `spec '<name>' описва функция, която не съществува` |
| Input arity differs | `spec '<name>': input има N параметъра, но функцията има M` |
| Input type differs | `spec '<name>': параметър '<p>' е A в spec-а, но B във функцията` |
| Output type differs | `spec '<name>': output е A, но функцията връща B` |

Guarantees themselves are not yet formally proven; `--proofs` reports them with
the status `UNVERIFIED — requires formal proof or testing`.

### 14.3 Executable guarantees (`ensures:`)

The `ensures:` section holds boolean Baga expressions, separated by commas.
The names of the input parameters and `output` — the returned value — are
visible inside them. The compiler type-checks each expression (it must be
`bool`) and compiles it into a runtime check that runs after every call of the
function — including recursive ones. On violation the program stops:

```
spec 'удвой': ensures #1 нарушена: output == 2 * x
```

```baga
spec факториел {
    input:
        n: i64
    output: i64
    ensures:
        output > 0,
        n <= 1 || output >= n
}
```

`guarantees:` remains free-text documentation (status UNVERIFIED in
`--proofs`); `ensures:` is executed (status RUNTIME-CHECKED). `ensures` on a
function without a return type is a compile-time error. The LLVM backend also
executes `ensures` (and `requires`) — the same wrapper pattern as the C backend.

### 14.4 Preconditions (`requires:`)

The `requires:` section holds boolean expressions over the input parameters,
separated by commas. They are type-checked at compile time and executed
**before** the function body on every call. On violation the program stops:

```
spec 'корен': requires #1 нарушено: x >= 0
```

```baga
spec корен {
    input:
        x: i64
    output: i64
    requires:
        x >= 0
    ensures:
        output >= 0
}
```

`requires` is also allowed on functions without a return type (unlike
`ensures`, which requires `output`). `output` is not visible inside requires
expressions.

### 14.5 Property-based testing (`--test-specs`)

`baga --test-specs file.baga` does not run `main`; it generates a test driver
that calls every function with `ensures`/`requires` 100 times with
deterministic random inputs (fixed seed — reproducible runs). `requires`
filters invalid inputs (the driver keeps generating until it finds a valid
one); an `ensures` violation is a counterexample and stops the program with
the input:

```
spec 'удвой': ensures #1 нарушена: output == 2 * x
  вход: -347
```

Only functions whose inputs are all of type `i64` and/or `bool` are supported;
the rest are skipped with a message. The contract's domain is part of the
contract — for example `факториел` restricts `n <= 20`, because `i64`
overflows beyond that.

---

## 15. Proof Extraction

`baga --proofs <file>` prints readable proof sketches derived from the AST:

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

For each function the compiler emits a signature theorem, a termination
sketch (with evidence such as base cases and return-path counts), and a
purity/effect theorem. If a spec exists, its guarantees are listed too.

---

## 16. Example Programs

### 16.1 Hello, world

```baga
fn main() {
    print("Здравей, багатуре. Боят започва.")
}
```

### 16.2 Factorial (recursion)

```baga
fn факториел(n: i64) -> i64 {
    if n <= 1 {
        return 1
    }
    return n * факториел(n - 1)
}

fn main() {
    print(факториел(10))   // 3628800
}
```

### 16.3 Fibonacci (while loop)

```baga
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

### 16.4 Summation with a for loop

```baga
fn main() {
    let mut сума: i64 = 0
    for i in 1..101 {
        сума += i
    }
    print(сума)   // 5050
}
```

### 16.5 Type inference and promotion

```baga
fn кръг_лице(r: f64) -> f64 {
    return 3.14159265 * r * r
}

fn main() {
    let r: f64 = 5.0
    print(кръг_лице(r))

    let x = 10        // i64
    let y = 3.5       // f64
    print(x + y)      // f64

    let по_голямо = x > 5
    print(по_голямо)  // true
}
```

### 16.6 Structs and field access

```baga
struct Точка { x: f64, y: f64 }

fn разстояние(a: Точка, b: Точка) -> f64 {
    let dx = a.x - b.x
    let dy = a.y - b.y
    return dx * dx + dy * dy
}

fn main() {
    let a = Точка { x: 0.0, y: 0.0 }
    let b = Точка { x: 3.0, y: 4.0 }
    print(разстояние(a, b))   // 25
}
```

### 16.7 Enums and match

```baga
enum Цвят { Червено, Зелено, Синьо }

fn име(ц: i64) -> str {
    match ц {
        0 => "червено",
        1 => "зелено",
        2 => "синьо",
        _ => "непознато",
    }
}

fn main() {
    let c = Зелено
    print(име(c))   // зелено
}
```

### 16.8 Strings

```baga
fn main() {
    let s = "Здравей, свят!"
    print(len(s))             // byte length
    print(char_at(s, 0))      // first byte
    print(substr(s, 0, 7))    // "Здравей"
    print(concat(s, " 🐆"))
    print(ord("А"))
    print(chr(65))            // "A"
}
```

### 16.9 Vectors

```baga
fn main() {
    let v = vec_new()
    vec_push(v, 10)
    vec_push(v, 20)
    vec_push(v, 30)
    print(vec_len(v))      // 3
    print(vec_get(v, 1))   // 20

    let sv = vec_new()
    vec_push_str(sv, "здравей")
    vec_push_str(sv, "свят")
    print(vec_get_str(sv, 0))   // здравей
}
```

### 16.10 Effects: propagate and handle

```baga
fn прочети_файл(път: str) -> str !IO !NotFound {
    return read_file(път)
}

fn обработи(път: str) -> str !IO !NotFound {
    let данни = прочети_файл(път)?
    return данни
}

fn main() {
    let резултат = прочети_файл("данни.txt")
        catch !NotFound => "празно"
        catch !IO => "грешка"
    print(резултат)
}
```

### 16.11 Specs

```baga
spec сортирай {
    input:
        arr: i64
    output: i64
    guarantees:
        - output is sorted
        - output has same elements as input
}

fn сортирай(arr: i64) -> i64 {
    return arr
}

fn main() {
    print(сортирай(42))
}
```

### 16.12 GCD (Euclid, while + modulo)

```baga
fn gcd(a: i64, b: i64) -> i64 {
    let mut x = a
    let mut y = b
    while y != 0 {
        let t = x % y
        x = y
        y = t
    }
    return x
}

fn main() {
    print(gcd(48, 18))   // 6
}
```

---

## 17. Error Messages

Errors are printed as `file: line:col: message`. Messages are emitted in
Bulgarian; English glosses follow.

### 17.1 Lexical errors

| Message | Meaning |
|---------|---------|
| `незатворен низ` | Unterminated string literal. |
| `незатворен символ` | Unterminated character literal. |
| `непозната escape последователност` | Unknown escape sequence in a string/char. |
| `непознат символ '<c>' (0xNN)` | A character the lexer does not recognize. |

### 17.2 Parse errors

| Message | Meaning |
|---------|---------|
| `очаквах '<X>', получих '<Y>'` | Expected token X but found Y. |
| `очаквах израз, получих '<Y>'` | Expected an expression but found Y. |
| `очаквах декларация (fn, struct, spec), получих '<Y>'` | Top level allows only declarations. |

### 17.3 Type and semantic errors

| Message | Meaning |
|---------|---------|
| `повторно дефиниране на '<name>'` | Name redeclared in the same scope. |
| `недефинирана променлива '<name>'` | Use of an undefined variable. |
| `непозната функция '<name>'` | Call to an unknown function. |
| `'<name>' очаква N аргумента, получих M` | Wrong argument count. |
| `'<name>': аргумент #N е от тип A, но параметърът е B` | Argument of incompatible type passed to a user function (`i32`/`i64` are compatible). |
| `аритметична операция върху не-числови типове (A, B)` | `+ - * / %` applied to non-numeric operands. |
| `очаквах bool в условие, получих A` | `if` condition is not a boolean. |
| `очаквах bool в условие на while, получих A` | `while` condition is not a boolean. |
| `непознато поле '.<name>'` | Field does not exist on the struct. |
| `непознат struct '<name>'` | Struct literal for an undeclared struct. |
| `връщам A, но функцията очаква B` | Returned type does not match the declared return type. |
| `vec_push: елемент от тип A, но векторът е Vec<B>` | Mixing element types in one `Vec` (also `vec_set`). |
| `vec_push: неподдържан елементен тип A за Vec (поддържат се i64, str и f64)` | Element type other than `i64`/`str`/`f64` (also `vec_set`). |
| `Vec<T>: неподдържан елементен тип A (поддържат се i64, str и f64)` | `Vec<A>` annotation with an element other than `i64`/`str`/`f64`. |

### 17.4 Effect errors

| Message | Meaning |
|---------|---------|
| `необработен ефект !<E> във '<fn>' — декларирай го в return типа или го хвани с catch` | An effect reached a function that neither declares nor catches it. |

### 17.5 Spec errors

| Message | Meaning |
|---------|---------|
| `spec '<name>' описва функция, която не съществува` | No function matches the spec name. |
| `spec '<name>': input има N параметъра, но функцията има M` | Spec/function arity mismatch. |
| `spec '<name>': параметър '<p>' е A в spec-а, но B във функцията` | Spec/function parameter type mismatch. |
| `spec '<name>': output е A, но функцията връща B` | Spec/function return type mismatch. |
| `spec '<name>': ensures изисква функция с върнат тип` | `ensures` on a void function. |
| `spec '<name>': ensures #N е A, очаквах bool` | The ensures expression is not boolean. |
| `spec '<name>': requires #N е A, очаквах bool` | The requires expression is not boolean. |

### 17.6 Program structure

| Message | Meaning |
|---------|---------|
| `липсва функция 'main'` | The program has no `main` function. |

---

## 18. Compiler Command Line

```
baga [options] <file.baga>
```

With no options, Baga generates C, compiles it with `gcc -O2`, runs the
resulting binary, and cleans up the temporary files.

| Flag | Description |
|------|-------------|
| *(none)* | Compile and run. |
| `--emit-c` | Print the generated C code to stdout; do not compile. |
| `--ast` | Print the parsed AST (debug). |
| `--tokens` | Print the token stream (debug). |
| `--specs` | Print spec documentation extracted from the source. |
| `--proofs` | Print extracted proof sketches. |
| `--test-specs` | Property-based test of spec contracts (random inputs, deterministic seed). |
| `--help`, `-h` | Show usage. |

---

## 19. Summary of Builtins

| Builtin | Signature | Effect |
|---------|-----------|--------|
| `print` | `(...) -> void` | — |
| `println` | `(...) -> void` | — |
| `write` | `(s: str) -> void` | — |
| `len` | `(s: str) -> i64` | — |
| `char_at` | `(s: str, i: i64) -> i64` | — |
| `substr` | `(s: str, a: i64, b: i64) -> str` | — |
| `concat` | `(a: str, b: str) -> str` | — |
| `str_eq` | `(a: str, b: str) -> bool` | — |
| `chr` | `(c: i64) -> str` | — |
| `ord` | `(s: str) -> i64` | — |
| `read_file` | `(path: str) -> str` | `!IO` |
| `arg_count` | `() -> i64` | number of program arguments (excluding the program name) |
| `arg` | `(i: i64) -> str` | the i-th argument (0-based, excluding the name); `""` out of bounds |
| `vec_new` | `() -> Vec` | — |
| `vec_len` | `(v: Vec) -> i64` | — |
| `vec_push` | `(v: Vec, x: i64 \| str \| f64) -> void` | first push fixes the element type |
| `vec_get` | `(v: Vec, i: i64) -> i64 \| str \| f64` | returns the element type |
| `vec_set` | `(v: Vec, i: i64, x: i64 \| str \| f64) -> void` | — |
| `vec_push_str` | `(v: Vec, s: str) -> void` | — |
| `vec_get_str` | `(v: Vec, i: i64) -> str` | — |
| `vec_set_str` | `(v: Vec, i: i64, s: str) -> void` | — |

---

*Nothing is new. But nothing is timely. — The Baga philosophy.*
