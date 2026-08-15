# Baga ⚔️

**Version [0.9.0](VERSION)** · [Changelog](CHANGELOG.md)

> *"The question is not 'what is new'. The question is 'what has not been glued together yet'."*

## What is Baga

**Baga (Бага)** is a **programming language** for systems and product code in the age of AI: you write normal programs; the compiler keeps effects, specs, and proof sketches visible so a human (or an agent) can trust the result.

| Pillar | Meaning |
|--------|---------|
| **Spec-first verification** | Specs are first-class; the compiler checks implementations against them (`--verify` on a stated fragment). |
| **Effects as type dimensions** | `str !IO !Net` is a different type from `str` — side effects show up in the type. |
| **Readable proof sketches** | Human-readable sketches extracted from code and specs — not Coq/Lean proof objects. |

The **toolchain** is a small C bootstrap (`baga`, optional LLVM backend, package manager **sandak**). **Product code** is written in Baga itself under `std/`, `app-product/`, and `apps/`.

## Pure-Baga cryptography (no OpenSSL at runtime)

Cryptography is **implemented in Baga**, not linked against OpenSSL/libcrypto for runtime. OpenSSL is only a **test peer** (e.g. TLS handshake / `https` mock).

| Layer | In-tree modules | Use |
|-------|-----------------|-----|
| Primitives | `std/crypto` — SHA-1/256, HMAC, AES, GCM, HKDF, **bn**, **X25519**, **P-256/ECDSA**, **RSA** (PKCS#1 / PSS), DER, X.509 | hashes, MAC, AEAD, signatures |
| TLS 1.3 client | `std/net/tls.baga` | record layer, handshake, cert + CertificateVerify, app traffic |
| HTTPS | `std/net/http_client.baga` | `http://` and **`https://`** over pure TLS |
| JWT | `app-product/jwtbaga` | **HS256** sign/verify; **RS256** / **ES256** verify (OIDC-ready) |

Stack highlights: live TLS against `openssl s_server`; JWT goldens cross-checked with Python; constant-time compares for MACs/tags where it matters.

## Application ecosystem (`app-product/` + `apps/`)

**Ecosystem packages, not demos** — each is a shippable building block that
stresses the language (effects, IO, concurrency, memory). Build with **sandak**
(`sandak.toml` per package). The stack exists to **prove Baga can host real
systems work**; the long horizon is a **RocksDB-like engine** (`rocksbaga`).

| Package | Role |
|---------|------|
| **httpdbaga** | HTTP/1.1 + HTTP/2 + HPACK |
| **jwtbaga** | JWT HS256 / RS256 / ES256 |
| **pgbaga** | PostgreSQL wire client (SCRAM, `$1` params) |
| **boilaDB** | Multimodal SQL database (BoilaSQL + PG wire `:6575` + HTTP) — [docs](app-product/boilaDB/docs/README.md) · [repo](https://github.com/bagalang/boilaDB) (submodule) |
| **boilabaga** | Client adapter to boilaDB over PG wire (`:6575`) |
| **ormbaga** | ActiveRecord-style ORM + goose migrations (Postgres or boila) |
| **fmrbaga** | Web framework (router, JSON, workers) — Lucky-inspired |
| **kvbaga** | RESP KV server (`Map` probe) |
| **rocksbaga** | Durable LSM KV (WAL → memtable → SST + page cache + bloom sidecar) on RESP — **storage flagship** (was `lsmbaga`) · [repo](https://github.com/bagalang/rocksbaga) (submodule) |
| **raftbaga** | 3-node Raft (election + log replication) over channels — Track S consensus exam |
| **metbaga** / **logbaga** / **cloudbaga** | Prometheus metrics, JSON logs, 12-factor demo (Track C1–C4) |
| **pbbaga** | Protobuf wire codec + gRPC message framing (Track C5) |
| **relbaga** / **flagbaga** | Retry/breaker/bulkhead + typed CLI flags (Track C6–C7) |
| **txnbaga** | 2PC coordinator + MVCC store (Track S8) |
| **otelbaga** | W3C traceparent lite (Track C8 subset) |
| **pathbaga** / **globbaga** / **uuidbaga** / **bufbaga** / **querybaga** | Universal path, glob, UUID v4, string builder, URL query |
| **statusbaga** / **mdtbaga** / **ctxbaga** | gRPC codes/Status, metadata MD, context deadline/cancel (Go-shaped) |
| **wsbaga** / **chatbaga** | WebSocket + multi-room chat (`poll`) |
| **oauthbaga** | OAuth proxy (integration exam) |
| **bagadecimal** | Fixed-precision decimal + Postgres `NUMERIC` (accounting) · [repo](https://github.com/bagalang/bagadecimal) (submodule) |
| **mdbaga**, **tplbaga**, **queuebaga**, **jsonrpcbaga**, **grebaga**, **testbaga** | Markdown, templates, jobs, RPC, grep CLI, asserts |
| **imgbaga** | Raster images — PNG/JPEG/GIF/QOI/ICO/TIFF/WebP (VP8+VP8L)/BMP/PNM (`image` crate) |
| **apps/api**, **apps/registry** | Sample product + sandak package registry |

Canonical stack: `apps/*` → **fmrbaga** → httpdbaga / jwtbaga / ormbaga → **pgbaga** → Postgres  
(or ormbaga → **boilabaga** → pgbaga → **boilaDB** PG wire). See [`app-product/BASE.md`](app-product/BASE.md).

## Quick Start

```bash
git submodule update --init --recursive
make
./baga examples/zdravei.baga
# Здравей, багатуре. Боят започва.
```

Or try it in the browser — `python3 playground/serve.py` → http://localhost:8080
(see [playground/README.md](playground/README.md)).

Zero dependencies for the core compiler — only `gcc` and `make`. The LLVM
backend is optional (see [Backends](#backends)).

### Two build levels

- The **toolchain** — the `baga` compiler (C bootstrap), `sandak` itself,
  the optional LLVM backend, the `!Par` runtime — is C and builds through
  the **Makefile**: `make`, `make sandak`, `make llvm`. The Makefile does
  not list packages or product tests.
- **Baga code** (`std`, `app-product/*`, `apps/*`) builds through
  **sandak** (`sandak.toml` per package) on top of the compiled `baga`
  binary.
- **Regression** — `make test` builds the toolchain, then runs
  `scripts/run_tests.sh`: sandak discovery for packages, `scripts/baga-test`
  for `tests/**/*_test.baga`, and `scripts/run_verify.sh` for `--verify`.

As in Rust: cargo builds the packages, not rustc. The Makefile is rustc's
bootstrap, not the crate graph.

## Packages — sandak

`sandak` is the package manager (the chest that holds the crates). Each package
has a `sandak.toml`:

```toml
[package]
name = "api"
version = "0.1.0"
entry = "start.baga"
kind = "bin"            # default: lib

[dependencies]
fmrbaga = { path = "../../app-product/fmrbaga" }
jwtbaga = { git = "https://github.com/user/jwtbaga", rev = "a1b2c3", subdir = "." }
```

```bash
make sandak
cd apps/api
sandak fetch    # resolve deps, write sandak.lock
sandak build    # -> target/api   (libs: typecheck via baga --lib)
sandak run      # build + run
```

Imports name the package: `import "fmrbaga/app.baga"`. The compiler resolves
them through `-I <dir>` search paths, which sandak computes from the
dependency graph. Docker: edit `APP_REPO`/`APP_REF` in `docker-compose.yml`
and `docker compose up --build` — the image clones the toolchain and the app
from git, fetches the dependencies, and builds (`sandak fetch && sandak build
--locked`; the fetch comes first because path-dep locks are not portable).

Notes: `sandak build` and `sandak run` fetch automatically when there is no
lock (plain `fetch` is for explicit control); `sandak manifest` prints the
parsed manifest for debugging. Known limitations: one version of a package
per graph, no registry; `branch`/`tag` refs float — pin with `rev = "<sha>"`
for reproducible builds; the git cache is clone-once (no auto-update).

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

Not formal proof objects. Readable sketches; contracts that pass `--verify` are backed by linear-arithmetic certificates (or honest UNKNOWN).

## Relation to Spec-Driven Development

The industry has converged on the same diagnosis Baga starts from. Microsoft
now promotes **Spec-Driven Development (SDD)** as the foundation of
AI-native engineering — specs as the *shared source of truth* for humans and
AI, *align first* instead of *prompt first and fix later* — and ships
**GitHub Spec Kit**, an open-source workflow around it
(Constitution → Specify → Clarify → Plan → Tasks → Implement → Validate).
See [Spec-Driven Development: the foundation of AI-native engineering](https://developer.microsoft.com/blog/spec-driven-development-ai-native-engineering/)
(Apoorv Gupta, Principal Software Engineer at Microsoft).

Baga shares the philosophy and pushes it one level deeper — from **process**
into the **language**:

| | Microsoft SDD / Spec Kit | Baga |
|---|---|---|
| Spec as source of truth | Yes — documents and tooling | Yes — `spec` is a language construct |
| AI generates code from spec | Core focus | Possible, but not the only path |
| Conformance check | Validate step (tests, review) | **Compile-time** static verification |
| Effects / side-effects | Not modeled | First-class in types (`!IO`, `!Overflow`, …) |
| Soundness | AI + human review | Sound fragment with linear-arithmetic certificates |

SDD fixes the workflow around AI agents; Baga makes the spec something the
*compiler* checks and extracts readable proofs from. In SDD terms, Baga is
the **spec-as-source, machine-checked** end of the spectrum.

> Защо го правим и какъв е смисълът, в крайна сметка —
> виж [docs/meaning-bg.md](docs/meaning-bg.md).

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
├── src/                    # C bootstrap compiler + sandak
├── self/                   # Self-hosted compiler (Baga)
├── std/                    # Standard library (crypto, net/tls, json, …)
├── app-product/            # Ecosystem packages (http, jwt, pg, img, decimal, …)
├── apps/                   # Sample products (api, registry)
├── examples/               # Language examples
├── tests/                  # Regression suite
├── scripts/run_tests.sh    # make test → here
├── docs/                   # Language / compiler / theory (EN + BG)
├── Makefile                # C toolchain only
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
| `--proofs` | Extract proof sketches (static verification status for specs, termination verdicts, loop-invariant lemmas) |
| `--test-specs` | Property-based test of spec contracts (random inputs, deterministic seed) |
| `--verify` | Static verification of `requires`/`ensures` (sound fragment: linear-ish i64, loops via invariants, recursion via assume–guarantee + optional `decreases` — see below) |
| `--json` | Machine-readable JSON output for `--verify` (verdicts + counterexamples; for AI agents and CI) |

## Documentation

- **[Research monograph (binding document)](docs/thesis.md)** — M0–M18 as one arc: front matter, chapter map, conclusion
- [Theory & Mathematics (EN)](docs/theory-en.md) — Type theory, effects, Hoare, **Fourier–Motzkin / Farkas / nonlinear envelopes (M0–M18)**
- [Теория и Математика (BG)](docs/theory-bg.md) — Типове, ефекти, Хоар, **FM / Farkas / нелинейни обвивки (M0–M18)**
- [Research note M13](docs/thesis-m13-nonlinear-fragment.md) — nonlinear + bitwise fragment
- [Research note M14](docs/thesis-m14-par-fragment.md) — fork–join determinism + handle protocols (concurrency in `--verify`)
- [Research note M15](docs/thesis-m15-arith-safety.md) — the ℤ-vs-i64 bridge (arithmetic safety) + the loop-havoc soundness fix
- [Research note M16](docs/thesis-m16-channel-invariants.md) — channel content invariants, cross-thread rely–guarantee
- [Research note M17](docs/thesis-m17-pairs.md) — pair abstraction: cell2 rewrites + channel pair APIs
- [Evaluation](docs/evaluation.md) — Baga `--verify` vs bit-precise model checking (methodology + results, `bench/`)
- [Research note M18](docs/thesis-m18-overflow-effect.md) — **culmination: `!Overflow` as an effect (effect system ≡ verifier judgement)**
- [Open problems](docs/thesis-open-problems.md) — honest frontier: liveness, full bitvectors, rich polynomials
- [Language Reference (EN)](docs/language-en.md) — Syntax, types, semantics
- [Езикова Справка (BG)](docs/language-bg.md) — Синтаксис, типове, семантика
- [Compiler Architecture (EN)](docs/compiler-en.md) — Pipeline, AST, codegen
- [Архитектура на Компилатора (BG)](docs/compiler-bg.md) — Конвейер, AST, кодогенерация
- [boilaDB](app-product/boilaDB/docs/README.md) — multimodal SQL database (BoilaSQL, PG wire, HTTP)

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
| 8 | General non-linear reasoning | ✅ M9–M13 (var div/mod, AM-GM, products in if-guards, bitwise laws) |
| 9 | Concurrency (`!Par`, `go`/`go_bg`/`join`/`detach`, channels, mutex) — cloud accept loops | ✅ M1 |
| 10 | LLVM backend `!Par` parity (`libbaga_par.so` + lli `-load`) | ✅ |
| 11 | `!Par` in `--verify`: fork–join determinism + handle protocols (M14) | ✅ |
| 12 | Arithmetic safety: ℤ-vs-i64 bridge + loop-havoc soundness fix (M15) | ✅ |
| 13 | Channel content invariants + cross-thread discharge (M16) | ✅ |
| 14 | Pair abstraction: cell2 + channel pair APIs in `--verify` (M17) | ✅ |
| 15 | **`!Overflow` as an effect — the effect system ≡ the verifier (M18)** | ✅ |
| 16 | Language arc M19–M24: missing-return checker, effect payloads (`!E(T)`, `raise`/`catch`), generics (fn + struct monomorphization), traits/impl, statically verified `guarantees` | ✅ |
| 17 | LLVM parity for M20–M24 (payload runtime, generic fns, traits, generic structs) | ✅ |
| 18 | RC memory model v1.0 (`--rc`, opt-in): RC1–RC5 — ownership, containers, struct/enum fields, owned fn results | ✅ |

### Static verification (`--verify`)

`--verify` proves or refutes `requires`/`ensures` **statically** for pure
functions over `i64` with **linear** arithmetic (recursion since M5 — partial
correctness, see below; `!Par` functions since M14 — fork–join + handle
protocols, see below). It is **sound by
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
  `div_mod_id.baga`.

- **M11** — **floor multiplication and complete square**. For `q = n/k` with
  `n >= 0`, `k > 0`: inject `k*q <= n` and remainder bound `n - k*q <= k-1`
  (so `k*(n/k) <= n` and `2*(n/2) <= n`). For square `s = v*v`: also
  `s - 2v + 1 >= 0` and `s + 2v + 1 >= 0` (i.e. `(v±1)^2`). Examples:
  `floor_mul.baga`, `complete_sq.baga`.

- **M12** — **variable divisors + AM-GM**. When `m >= 1` and `n >= 0`:
  `n/m >= 0`, `n/m <= n`, `0 <= n%m < m`, and `n = (n/m)*m + (n%m)` via the
  product `q*m`. Special cases: `n/n = 1`, `n%n = 0` (n≠0), `n/1 = n`,
  `0/m = 0`. Also `(x-y)² = x²+y²-2xy ≥ 0` when those products appear.
  Examples: `var_div.baga`, `amgm.baga`. Without `m >= 1`, non-negativity of
  `n/m` is correctly **refuted** (e.g. m = -1).

- **M13** — **products in conditionals + sound bitwise fragment**.
  `bool_to_dnf` threads `ReadsList`, so a guard like `n * n >= 1` becomes a
  path constraint over a fresh product var (with the same axioms as M8–M12),
  not an honest UNKNOWN. After every `if`/`while` fork the product axioms are
  re-injected so both branches inherit them. Bitwise ops stay **outside full
  bitvector theory** but admit proven identities:
  `n|0 = n`, `n&0 = 0`, `n^0 = n`, `n^n = 0`, `n&-1 = n`;
  `n&1 ∈ {0,1}` over two's complement (explicitly **not** C `% 2` on negatives);
  `n<<k = n·2^k` and `n>>k` as trunc-div by `2^k` under `n >= 0`.
  Examples: `nonlinear_if.baga`, `bitwise_laws.baga`.
  Scientific note: `docs/thesis-m13-nonlinear-fragment.md`.

- **M14** — **concurrency (`!Par`) in the verifier**. No shared mutable state
  exists in the language (no globals, no closures — `go(f, x)` passes a named
  function and an `i64` by value), so the linear core suffices. Functions
  whose only effect is `Par` are verifiable. **Fork–join determinism:** for a
  pure verifiable worker, `join(go(f, x)) ≡ f(x)` — the worker's contract
  applies via M5 assume–guarantee (requires discharged at spawn, ensures
  assumed for the join result). **Handle protocols:** join handles and
  channels carry a ghost state (`spawn → join | detach`; channel open/closed);
  a second consume (fatal at runtime) is REFUTED statically with a
  counterexample; `send` on a known-closed channel is provably `-1`.
  Statement-level par calls only; pair-returning builtins, mutexes,
  `pool_map`, and effectful workers stay honestly outside. Examples:
  `par_join.baga`, `par_join_bad.baga`, `par_detach_bad.baga`,
  `par_chan.baga`. Scientific note: `docs/thesis-m14-par-fragment.md`.

- **M15** — **arithmetic safety: the ℤ-vs-i64 bridge**. The verifier reasons
  in idealized ℤ but the runtime is i64; M15 emits one obligation per
  arithmetic operation (`+ - * -x / % <<`) and proves it cannot overflow on
  its path (exact FM bound search; products via tightest provable |factor|
  bounds, compared in `__int128`), or refutes it with a concrete
  large-magnitude witness — `abs(INT64_MIN)`, `n + 1` at `n = INT64_MAX`,
  `n / m` at `m = 0`. When every arith obligation of a function is PROVEN,
  the idealized model and the runtime coincide and the ensures verdicts are
  unconditional; otherwise the output says so explicitly. Same milestone
  shipped two soundness fixes found by the new analysis: **loop havoc**
  (variables assigned in a `while` body are havoced before the invariant is
  assumed — before, stale pre-loop values made invariants vacuous and could
  falsely PROVE; see `loop_havoc.baga`) and an INT64_MIN-unsafe rational
  core (`rat_*` now use `__int128`; `fm_sat` bails out conservatively on
  overflow). Examples: `ovf_add.baga`, `ovf_mul.baga`, `div_zero.baga`,
  `loop_havoc.baga`. Scientific note: `docs/thesis-m15-arith-safety.md`.

- **M16** — **channel content invariants (rely–guarantee)**. A new
  statement-level annotation `invariant c[*] >= 1` declares "every payload
  sent on `c` satisfies the predicate" (anchored on the resolved symbolic
  channel var, so aliases work). `chan_send` discharges it (else the axiom
  drops, M3 rule); `chan_recv` instantiates it. Cross-thread: a worker's
  `requires c[*] ...` is discharged against the caller's axioms at `go`
  spawn, and a worker without matching requires drops them — rely–guarantee
  in the small. `go` workers may now declare `Par` effects. The scalar form
  `invariant e` doubles as an `assume`. Examples: `chan_inv.baga`,
  `chan_inv_bad.baga`, `chan_inv_par.baga`, `chan_inv_escape.baga`.
  Scientific note: `docs/thesis-m16-channel-invariants.md`.

- **M17** — **pair abstraction**. `cell2(a,b)` / `cell2_0(p)` / `cell2_1(p)`
  are exact rewrites (`cell2_0(cell2(a,b)) = a`), allowed anywhere including
  conditions. The pair-returning channel APIs enter the fragment:
  `chan_recv2` (ok ∈ [0,1]), `chan_try_recv`/`chan_recv_timeout`
  (status ∈ [0,2]), `chan_select2*` (which ∈ [0,3]) — the value component
  carries M16 content axioms (for select2*, only what both channels share).
  `go(worker, cell2(a, b))` packs arguments; a worker's
  `requires cell2_1(p) >= 1` is discharged at spawn where the components are
  visible. Examples: `pair_recv2.baga`, `pair_select.baga`, `pair_go.baga`.
  Scientific note: `docs/thesis-m17-pairs.md`.

- **M18** — **`!Overflow` as an effect (the culmination)**. Arithmetic safety
  becomes a *type-level effect*: the M15 obligations are the effect inference
  for `!Overflow`, and the effect check is the discharge. A function that
  omits `!Overflow` claims safety — proven (`ефект !Overflow: безопасна —
  типът е точен`), refuted with a witness (overflow + undeclared ⇒ nonzero
  exit), or honestly UNKNOWN. Declaring `!Overflow` discharges it (the overflow
  is printed as evidence, verification does not fail) — a permission, like
  `!IO`, not a claim. `--proofs` emits `theorem f_overflow_safe`; `--verify
  --json` adds an `overflow_effect` field. The effect system and the verifier
  become one judgement. Examples: `ovf_eff_{safe,refuted,declared,unknown,
  redundant,skip,propagate}.baga`. Scientific note:
  `docs/thesis-m18-overflow-effect.md`.

`vec_slice` / `vec_concat` propagate element invariants; `sorted(v)` is
supported. `--proofs` surfaces the verifier's established facts: real
termination verdicts (`decreases` → full correctness vs partial), while-loop
invariants as lemmas with their Hoare status, and the M18 `f_overflow_safe`
theorem. Remaining (the honest frontier — `docs/thesis-open-problems.md`):
liveness for channels, full bitvectors, rich polynomials.

> **Language note (M2):** `[T]` is now sugar for `Vec<T>` (the growable
> `baga_Vec`), not a raw C pointer — so vector parameters can be written
> `v: [i64]` and used with the `vec_*` builtins uniformly.

## Design principles

- **Zero dependencies** for the core compiler: C + `gcc` + `make`. Optional backends (LLVM) are additive.
- **Soundness over completeness**: PROVEN requires a certificate; outside the fragment the verdict is UNKNOWN.
- **Auditability**: Fourier–Motzkin core, readable witnesses, no external solver.
- **Self-hosting**: `make self` checks the fixed point (`baga2` reproduces `baga3`).
- **Identity**: Cyrillic identifiers are first-class (the name *Бага* is Bulgarian).

> *Nothing is new. The question is what has not been glued together yet — and whether the glue is sound.*

## License

MIT.
