# Baga Language Reference

> A complete reference for the Baga programming language: syntax, types,
> semantics, the effect system, the spec system, builtins, and examples.

Baga (Бага) is a compiled, statically typed language built on three ideas:

1. **Spec-first verification** — `spec` is a keyword; the compiler checks
   implementations against their specifications.
2. **Effects as type dimensions** — `str !IO !NotFound` is a *different type*
   from `str`. Errors live in the type system, not in runtime surprises.
3. **Readable proof sketches** — the compiler emits human-readable sketches
   from code and specs; `--verify` adds certificates (or honest UNKNOWN)
   inside its fragment.

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
| `Vec<T>` | vector with annotated element (`i64`, `str`, `f64`, `bytes` or a struct type) | `baga_Vec *` |
| `Map<K,V>` | hash table; key `i64`/`str`, value `i64`/`str`/`f64`/`bytes` | `baga_Map *` |
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
whose body is a bare expression returns that value from the enclosing
function.

When the matched value is a **sum enum** (§11.1), patterns are
`Variant(binding)`, bare `Variant` for payload-less variants, or `_`:

```baga
enum Res { Ok(i64), Err(str) }

fn describe(r: Res) -> str {
    return match r {
        Ok(v) => "успех",
        Err(e) => e,
    }
}
```

The match must be **exhaustive** — the checker errors with the missing
variant's name (`не е пълен — липсва вариант 'Err' (или добави '_')`), and
all arms must agree in type. A pattern that is not a variant of the
scrutinee's enum is rejected. Matches over non-enum values keep the
first-arm-wins rule above with no exhaustiveness check. A bare-expression
arm also works in `-> void` functions, where the value is discarded.

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

### 11.1 Sum types (L3)

An enum may give a variant a single **payload** type, turning it into a sum
type (tagged union):

```baga
enum Res { Ok(i64), Err(str) }
enum Opt { Some(str), None }

fn unwrap_or(r: Res, dflt: i64) -> i64 {
    return match r { Ok(v) => v, _ => dflt }
}

fn main() {
    let r = Ok(42)              // construction: Variant(payload)
    let n: Opt = None           // payload-less variants stay bare
    print(unwrap_or(r, -1))     // 42
}
```

Construction is `Variant(payload)` for payload variants and the bare
`Variant` name for payload-less ones; a payload variant referenced without
its argument is a compile error. Payloads may be any declared type,
including structs (`enum Shape { Dot, Circle(Point) }`).

**A1 — qualified variants.** Several enums may reuse the same variant
names (`Ok` / `Err`). Prefer the qualified form:

```baga
enum PgRes { Ok(i64), Err(str) }
enum RpcRes { Ok(str), Err(i64) }

let a = PgRes::Ok(1)
let b = RpcRes::Ok("{}")
match a {
    PgRes::Ok(v) => v,
    PgRes::Err(e) => 0,
}
// Bare Ok(x) is allowed only when exactly one Ok exists in the program.
// Match patterns are scoped to the scrutinee type, so bare Ok(v) is fine
// when matching a PgRes even if RpcRes also has Ok.
```

Checker rules:

- The type is nominal — `enum Res` is its own type, **not** an `i64`; you
  cannot pass a `Res` where an `i64` is expected, or the reverse.
- Variant names may be shared across enums; use `Enum::Variant`. Bare
  names error when ambiguous (`нееднозначен`). The same name twice
  **inside one enum** is still an error.
- Constructor arity and payload type are checked (`конструкторът 'Ok'
  очаква 1 аргумент`).
- `match` on a sum enum uses variant patterns with bindings and must be
  exhaustive — see §8.

The C backend lowers a sum enum to a tagged struct with a `union` of the
payloads plus a `static inline` constructor per variant. The LLVM backend
rejects sum types with an honest `unsupported` error pointing here.

Honest v1 limits:

- No generics — write a concrete enum per use site; use `Enum::Ok` when
  several Result enums coexist (A1).
- Exactly one payload type per variant; wrap several fields in a struct.
- Nested sum/struct cycles fall back to declaration order; acyclic graphs
  (the common case: `struct Hold { r: Res }`, `enum Box { BoxHas(Wrap) }`)
  are emitted in topological order so **sum enums as struct fields work**.
- **A2:** `Vec<Res>` and `Map<K, Res>` work via the same box path as
  `Vec`/`Map` of structs (push/get copy; missing map key → zeroed tag).

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
type — `Vec<i64>`, `Vec<str>`, `Vec<f64>`, `Vec<bytes>`, or `Vec<SomeStruct>`
— which is fixed by the first `vec_push`/`vec_set`. Mixing element types in
one vector is a compile-time error.

| Signature | Description |
|-----------|-------------|
| `vec_new() -> Vec` | Create an empty vector with an unknown element type. |
| `vec_len(v: Vec) -> i64` | Number of elements. |
| `vec_push(v: Vec, x: i64 \| str \| f64 \| bytes \| struct)` | Append an element; the first push fixes the element type. |
| `vec_get(v: Vec, i: i64) -> i64 \| str \| f64 \| bytes \| struct` | Read the element at index `i` (the vector's element type). |
| `vec_set(v: Vec, i: i64, x: i64 \| str \| f64 \| bytes \| struct)` | Overwrite the element at index `i`. |
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

Supported element types: `i64` (and `i32`, accepted as `i64`), `str`, `f64`,
`bytes` (binary-safe; elements are boxed, like `Map` bytes values), and
**struct types** — `Vec<Line>` etc. Struct elements are boxed copies:
`vec_push`/`vec_set` copy the value in, `vec_get` copies it out (mutating
the returned copy does not touch the vector; reference-typed fields like
`Vec`/`Map` stay shared, as usual for struct assignment). Mixing two
different struct types in one vector is a compile-time error, just like
mixing scalars. Works in both backends (LLVM uses the same box helpers;
oracle: `examples/vec_struct.baga`).
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

### 12.5 Maps (dynamic key–value tables)

A `Map` is a heap-allocated hash table. Keys are `i64`, `str`, or `bytes`
(`bytes` keys are NUL-safe, compared by content); values are
`i64`, `str`, `f64`, `bytes`, or a struct type — fixed by the first
`map_set` (or by a `Map<K, V>` annotation), exactly like `Vec`'s element
type. Maps are pointers: passing one to a function shares it, and mutations
are visible to the caller. `bytes` values are binary-safe (NUL/0xFF
round-trip) — used for residual I/O buffers (chatbaga) and any binary
store. Struct values are boxed copies, like `Vec<struct>`: `map_set` copies
in, `map_get` copies out; reference-typed fields (`Vec`/`Map`) stay shared.

| Signature | Description |
|-----------|-------------|
| `map_new() -> Map` | Empty map; key/value types unknown until first use. |
| `map_set(m, key, val)` | Insert or overwrite; fixes/validates both types. |
| `map_get(m, key) -> val` | Value for the key; `0` / `""` / `0.0` / empty `bytes` / zero struct when absent. |
| `map_has(m, key) -> i64` | `1` when the key exists, else `0`. |
| `map_del(m, key)` | Remove the key (no-op when absent). |
| `map_len(m) -> i64` | Entry count. |
| `map_keys(m) -> Vec` | All keys — `Vec<str>` / `Vec<i64>` / `Vec<bytes>` per the key type. |

```baga
fn tally(m: Map<str, i64>, word: str) {
    map_set(m, word, map_get(m, word) + 1)
}

fn main() {
    let counts: Map<str, i64> = map_new()
    tally(counts, "бага")
    tally(counts, "бага")
    print(map_get(counts, "бага"))   // 2

    let meta = map_new()             // plain Map: types fixed by first set
    map_set(meta, "version", "0.8")
    print(map_get(meta, "version"))  // 0.8
}
```

Mixing key or value types in one map is a compile-time error:

```
map_set: стойност от тип str, но картата е Map<str, i64>
```

Absent-key semantics are zero-values, like Go maps without the `, ok` form —
use `map_has` to distinguish "missing" from "stored zero". For struct values
the zero struct is field-wise safe: `str` fields are `""` (printable), not
NULL; `Vec`/`Map` fields are NULL but `vec_len`/`map_len` tolerate NULL and
return 0. The C backend implements maps natively (chained hash table, grows
at load factor 3/4); the LLVM backend does not support `Map` yet.

### 12.6 Function values and lambdas (L5)

Functions are first-class values. A function value is typed
`fn(T, ...) -> R` (effects allowed: `fn(i64) -> i64 !IO`) and may live in
locals, parameters, return types, and as `Vec`/`Map` elements (method
tables).

```baga
fn add(a: i64, b: i64) -> i64 { return a + b }

fn apply(h: fn(i64) -> i64, x: i64) -> i64 { return h(x) }

fn main() {
    let f = add                       // named reference
    let g: fn(i64, i64) -> i64 = sub  // annotated
    print(f(2, 3))                    // 5

    let n = 10
    let lam = fn [n] (x: i64) -> i64 { return x + n }   // lambda
    print(apply(lam, 5))              // 15

    let ops: Vec<fn(i64) -> i64> = vec_new()            // fn in containers
    vec_push(ops, lam)
    print(vec_get(ops, 0)(7))         // 17
}
```

- **Captures are explicit and by value**: `fn [a, b] (x: i64) -> i64 { ... }`
  copies `a`/`b` into the closure; later mutation of the source variables
  does not propagate. `Vec`/`Map` captures share the reference (as always).
- A lambda's effects ride on its type: wrapping into a pure annotation
  (`let g: fn() -> i64 = do_io`) is a compile error.
- Module-qualified references work: `let q = http_client.http_get` (L6).
- A fn-typed local may not shadow a global function name (keeps `--verify`
  sound); calls through values are opaque to the verifier (honest skip).
- Both backends represent fn values as `(code, env)` handles
  (`cell2` from the par runtime): the C backend emits `__clo` wrappers in
  the preamble, the LLVM backend builds them as lazy IR functions.

### 12.7 `bytes` mutators (S2)

`bytes` buffers are built and mutated with three builtins:

| Signature | Description |
|-----------|-------------|
| `bytes_new(n: i64) -> bytes` | Fresh zeroed buffer of `n` bytes; `n < 0` clamps to 0. |
| `bytes_set(b: bytes, i: i64, v: i64) -> void` | Bounds-checked write — OOB aborts with `baga: bytes_set: индекс N извън границите [0, L)`; `v` is masked to a byte (`& 255`). **Mutates the shared buffer** — aliases see the write (Vec/Map semantics). |
| `bytes_push(b: bytes, v: i64) -> bytes` | Returns a **new** `bytes` of length `len+1` with `v` appended; the source buffer is untouched. O(n) copy per push — fine for frame building, not for hot loops. |

```baga
let buf = bytes_new(4)
bytes_set(buf, 0, 255)
let alias = buf
bytes_set(alias, 1, 7)
print(bytes_at(buf, 1))            // 7 — set is visible through the alias

let f = bytes_push(bytes_push(bytes_new(0), 137), 1)
let g = bytes_push(f, 2)           // f stays len 2; g is a fresh len-3 buffer
```

C backend only; the LLVM backend honestly reports `unsupported` for the
three mutators.

### 12.8 `drop` and memory (MEM-1)

Baga allocates from a bump arena that never reclaims on its own — fine for
short runs, fatal for servers. `drop(x)` is the manual reclamation builtin:
it frees a value's heap blocks **now**, and the checker enforces that you
can never touch the value again.

```baga
fn main() {
    let v = vec_new()
    vec_push(v, 1)
    drop(v)                    // buffer + struct freed
    print(vec_len(v))          // compile error: използване на 'v' след drop
}
```

`drop` accepts only a **let-bound local** of type `Vec`, `Map`, `bytes`,
or `fn`. The free is deep:

- **Vec**: element boxes (for `bytes`/struct elements), the data buffer,
  the Vec struct.
- **Map**: per-value boxes (for boxed struct values), entries, buckets,
  the Map struct.
- **bytes**: the data buffer.
- **fn**: the malloc'd `(code, env)` cell handle. The closure **env box
  stays in the arena** — it may be shared with other fn values, so it is
  not reclaimed (documented, not hidden).

#### Checker rules (all compile errors)

- **Use after drop** — any later read of the variable:
  `използване на 'x' след free` (same wording after `arena_free`).
- **Double drop** — `повторен drop на 'x'`.
- **Drop of a parameter** — params share the caller's buffer for
  Vec/Map, so freeing would dangle the caller:
  `drop на параметър 'x' — параметрите споделят буфера на извикващия`.
- **Drop of a lambda-captured variable** — capturing `x` in a lambda
  marks it (captures of Vec/Map share the reference per §12.6); dropping
  it afterwards — inside or outside the lambda — is rejected:
  `'x' е заснет от ламбда — drop би оставил висящ указател`.
- **Drop inside a loop of a variable declared outside it** — the second
  iteration would be use-after-drop:
  `drop на външна за цикъла променлива 'x' — втората итерация би била use-after-drop`.
  Dropping loop-locals is fine (they are fresh per iteration).
- **Drop of `str` or scalars** — `str` data is arena/interned, scalars
  own no heap: `drop: неподдържан тип ... — drop е за Vec/Map/bytes/fn`.
- **Drop of a non-local expression** — `drop(vec_new())`, `drop(f(x))`:
  `drop очаква локална променлива (let), не израз`.

#### Branches: certainties only

After an `if`, a variable counts as dropped only if it is dropped on
**all** arms. Using it after a join where only one arm dropped it is
allowed — the checker reasons in certainties, not maybes:

```baga
let v = vec_new()
if n > 0 { drop(v) }          // else-arm keeps v live
print(vec_len(v))             // OK — maybe-dropped is not definitely-dropped
```

#### Honest limits (v1)

- **Assignment revival stays an error**: `drop(v); v = vec_new(); use(v)`
  is still rejected — the checker is conservative and keeps `v` dead.
- **Aliasing is the programmer's contract**: the checker tracks
  *variables*, not heap graphs. `let y = x; drop(x); use(y)` is **not**
  diagnosed — exactly like C, dropping `x` while an alias lives is your
  responsibility. The same holds for values stashed inside other
  containers (`vec_push(v2, x)`-style sharing of str/bytes elements).
- **bytes/str inner buffers of freed boxes stay in the arena** — they may
  be shared (a `str` element interned elsewhere), so the deep walker does
  not free them.
- **Blocks > 1024 B are not reclaimed** by the free list (below).
- **Historical garbage stays**: old buffers abandoned by `vec_grow` /
  `map_rehash` are not tracked; `drop` frees the *current* blocks only.
- **Scope-exit leaks are NOT diagnosed** — the compiler has no warning
  severity, so a value that goes out of scope without `drop` is silently
  arena-leaked. Leak hunting is MEM-3 (regions) territory.
- **LLVM backend**: honest `unsupported` for `drop` (C backend only).

#### Runtime: the free list

`baga_alloc` keeps per-size-class free lists for blocks **≤ 1024 B**
(16 B granularity, 64 classes, padded allocations) plus pow2 classes up
to 32 MiB (R18); freed blocks are reused before the bump pointer
advances. Since R52 the arena and both free-list tiers are
**thread-local** (`__thread`) — the old global alloc mutex serialized
every `go`/`go_bg` thread (a de-facto GIL). Proof it works: a
1M-iteration alloc+drop loop peaks at ~6.2 MB maxrss vs ~87.6 MB without
`drop`.

#### MEM-2: verifier obligations (`--verify`)

The static verifier tracks a `HK_DROP` ghost state per source variable
(the same handle-protocol machinery as M14's `spawn → join | detach`):
`let x = vec_new()/map_new()/bytes_new(...)` registers `x` as live,
`drop(x)` transitions it to dropped, and use-after-drop or double drop
on a live path is **ОБРОЧЕНО (REFUTED)** with a witness path
(`examples/verify/mem_drop.baga`). Aliasing and fn-value drop are silent
no-claim paths, and programs outside the supported fragment are skipped
honestly — same gating as M14.

#### MEM-3 lite: arena handle seatbelt

`arena_free(a)` marks the local handle `a` the same way `drop` does:

- **double free** — `повторен arena_free на 'a'`;
- **alloc/reset after free** — `използване на арена 'a' после arena_free`;
- **any use of `a` after free** — `използване на 'a' след free`.

**Region tags (MEM-3):** `let p = arena_alloc(a, n)` associates `p` with
arena handle `a`. After `arena_free(a)`, any use of `p` is
`използване на 'p' след free`. Only direct `arena_alloc` bindings are
tagged (not pointer arithmetic descendants). Runtime: null-handle
guards on `arena_alloc` / `arena_reset`.

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

### 13.5 `!Overflow` — an effect the verifier infers

`!Overflow` is a special effect dimension: unlike `!IO` (which is *generated*
by builtins such as `read_file`), no builtin generates `!Overflow` — it is
**declared** and **propagated** like any effect, and the need for it is
discovered by the static verifier (`--verify`, M15/M18).

```baga
fn inc(n: i64) -> i64 !Overflow {   // "may overflow i64"
    return n + 1
}
```

The semantics (M18): the effect is a *permission*, not a claim.

- A function **without** `!Overflow` claims to be overflow-safe. `--verify`
  checks this: proves it (all arithmetic obligations safe), refutes it with a
  counterexample (overflow with the effect undeclared — nonzero exit), or
  honestly reports НЕ МОГА ДА РЕША (UNKNOWN).
- A function **with** `!Overflow` honestly advertises the risk — the verifier
  still prints the overflow as evidence, but it is no longer a violation
  (exit 0); the `ensures` verdicts are in the idealized ℤ model.
- `!Overflow` propagates through calls: a caller of an `!Overflow` function
  must declare or catch the effect (the same "unhandled effect !Overflow"
  error).

`--proofs` emits a `<fn>_overflow_safe` theorem; `--verify --json` adds an
`overflow_effect` field. For the full theory see
`docs/thesis-m18-overflow-effect.md`.

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
| `vec_push: неподдържан елементен тип A за Vec (поддържат се i64, str, f64, bytes и struct)` | Element type other than `i64`/`str`/`f64`/`bytes` (also `vec_set`). |
| `Vec<T>: неподдържан елементен тип A (поддържат се i64, str, f64, bytes и struct)` | `Vec<A>` annotation with an element other than `i64`/`str`/`f64`/`bytes`. |

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
| `--verify` | Static verification of requires/ensures contracts. |
| `--json` | Machine-readable JSON output (with `--verify`). |
| `--check`, `--lib` | Parse + typecheck only, no main, no codegen — for libraries. |
| `--emit-llvm` | LLVM IR output (requires `make llvm`). |
| `-I <dir>` | Import search directory (repeatable flag). |
| `--version`, `-V` | Compiler version. |
| `--help`, `-h` | Show usage. |

### 18.1 Imports and Packages (sandak)

`import "path/to/file.baga"` at the top of a file textually includes another
file (with an include guard and cycle detection). Lookup order: (1) relative
to the current file, (2) in each `-I` directory in the order given,
(3) relative to the working directory.

**Namespaces.** Every imported file is also a *module* named after its
basename without `.baga` (`std/net/http_client.baga` → module
`http_client`), and its functions can be called qualified:
`http_client.http_get(url)`. Two modules may define the same function name
— this is legal now, and each definition gets a unique internal symbol
(`module.name`). For an **unqualified** call the resolution order is:
(1) the current file's own definition wins, (2) otherwise the single
imported module that defines it, (3) otherwise a compile error listing the
candidate modules (`нееднозначно извикване на 'f' …`). A local variable
whose name equals a module name shadows the module (struct field access
keeps working). Two functions with the same name in the *same* module are
a checker error (`повторна дефиниция`) — forward declarations (`fn f(...)`
without a body) plus one implementation are fine. Struct and enum names
stay global (no qualification yet), as do `go` worker references.

In a packaged project the import carries the package name —
`import "fmrbaga/app.baga"`, `import "std/str/str.baga"` — and the `-I` flags
are computed automatically by the **sandak** package manager from the
`sandak.toml` manifests of the dependencies (`sandak fetch` / `sandak build` /
`sandak run`; see README, "Packages — sandak").

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
| `vec_push` | `(v: Vec, x: i64 \| str \| f64 \| bytes \| struct) -> void` | first push fixes the element type |
| `vec_get` | `(v: Vec, i: i64) -> i64 \| str \| f64 \| bytes \| struct` | returns the element type |
| `vec_set` | `(v: Vec, i: i64, x: i64 \| str \| f64 \| bytes \| struct) -> void` | — |
| `vec_push_str` | `(v: Vec, s: str) -> void` | — |
| `vec_get_str` | `(v: Vec, i: i64) -> str` | — |
| `vec_set_str` | `(v: Vec, i: i64, s: str) -> void` | — |
| `map_new` | `() -> Map` | — |
| `map_set` | `(m: Map, k: i64 \| str, v: i64 \| str \| f64 \| bytes \| struct) -> void` | first set fixes key/value types |
| `map_get` | `(m: Map, k: i64 \| str) -> i64 \| str \| f64 \| bytes \| struct` | zero-value when absent |
| `map_has` | `(m: Map, k: i64 \| str) -> i64` | 1 when the key exists |
| `map_del` | `(m: Map, k: i64 \| str) -> void` | — |
| `map_len` | `(m: Map) -> i64` | — |
| `map_keys` | `(m: Map) -> Vec` | `Vec<str>` / `Vec<i64>` / `Vec<bytes>` per the key type |
| `signal_watch` | `(sig: i64) -> i64` | C1: install handler; 0 ok, -1 error |
| `signal_check` | `() -> i64` | 0 = none, else signal number |
| `signal_clear` | `() -> i64` | return and clear pending |
| `signal_wait` | `(ms: i64) -> i64` | wait up to `ms` (`<0` = forever); signo or 0 |
| `signal_raise` | `(sig: i64) -> i64` | `raise(sig)` to self (tests) |
| `bytes_put` | `(dst: bytes, off: i64, src: bytes) -> void` | R54: in-place memcpy append (bounds-checked no-op) |
| `str_h` | `(s: str) -> i64` | R51: unsafe handle cast (zero-copy chan hop; str is arena-bound) |
| `h_str` | `(h: i64) -> str` | R51: inverse of `str_h` |
| `bytes_h` | `(b: bytes) -> i64` | R66: box `baga_bytes` header for chan hop (data already arena) |
| `h_bytes` | `(h: i64) -> bytes` | R66: inverse of `bytes_h` |
| `map_h` | `(m: Map) -> i64` | R55: unsafe map handle (go_bg ctx packing); C backend |
| `h_map` | `(h: i64) -> Map` | R55: inverse of `map_h`; C backend |

---

*Nothing is new. But nothing is timely. — The Baga philosophy.*
