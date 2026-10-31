# AGENTS.md — baga-lang

Baga is a systems programming language (Bulgarian keywords) with a C bootstrap
compiler, optional LLVM backend, and `sandak` package manager. This repo is a
monorepo: compiler + `std/` + `examples/` + `tests/` + in-tree `apps/` live here;
every `app-product/*` directory is a **git submodule** (own repo under
`github.com/bagalang/<name>`).

## Build (two levels — do not confuse them)

```bash
make              # C toolchain only: ./baga (C backend)
make sandak       # package manager: ./sandak
make llvm         # optional LLVM backend: ./baga-llvm + lib/*.so (needs llvm-config)
make test         # full regression → scripts/run_tests.sh (needs make + make sandak first)
make self         # self-hosting fixed-point check (baga2 == baga3)
make test-llvm    # LLVM oracle: C vs lli output parity (needs make llvm)
```

Baga packages (`std/`, `app-product/*`, `apps/*`) build via **sandak**, not make:

```bash
cd apps/api && ../../sandak build    # → target/api
../../sandak run                      # build + run
```

## Run / check a single file

```bash
./baga -I . -I app-product examples/zdravei.baga      # compile + run
./baga -I . -I app-product --check file.baga          # typecheck only (libs)
./baga -I . -I app-product --verify examples/verify/abs_val.baga   # static verification
./baga --emit-c file.baga                             # emit C to stdout
```

`-I . -I app-product` is required for imports to resolve (`std/...`, `fmrbaga/...`).

## Run a single test

```bash
./baga -I . -I app-product tests/jwt_test.baga
scripts/baga-test tests/jwt_test.baga                 # via the test harness
```

## Layout rules (verified in BASE.md)

- `app-product/*` packages stay **universal** — no product domain inside them.
- `apps/*` are independent products — each owns routes, models, migrations.
- Each package has `sandak.toml` with `name == directory name`.
- Imports use package names (`import "fmrbaga/app.baga"`), never `../../` paths.
- Each package may carry `gaps.md` documenting known limitations — read it before editing.

## Test prerequisites (external services)

Some tests need services running — they skip/fail gracefully without them:

```bash
# boilaDB PG wire (for orm_boila_test):
./baga -I . -I app-product app-product/boilaDB/tools/serve_pg.baga   # listens :6575

# Postgres for apps/api runbook (docs/runbooks/product-path.md):
export PGHOST=127.0.0.1 PGPORT=5432 PGUSER=bagatest PGPASSWORD='...' PGDATABASE=baga_orm

# openssl s_server for TLS handshake tests (run_tests.sh starts these on :18443/:18444)
```

## CI

`.github/workflows/ci.yml` fetches only 3 submodules for smoke
(httpdbaga, jwtbaga, otpbaga), rewrites SSH→HTTPS, builds with `make && make sandak`,
runs examples + `make self` + `make test-llvm`.

## Never commit

`.env`, `**/password.txt`, `*.o`, `baga`, `baga-llvm`, `sandak`, `lib/`, `target/` — all in `.gitignore`.
