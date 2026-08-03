# testbaga — baga test runner (plan)

Date: 2026-08-04
Status: P0 done (assert lib + suite + shell discovery)
Goal: apps-roadmap **№8** — language eats its own food.

## Phases

### P0 — assertions + discovery driver ✅

1. `assert.baga` — fail-fast `assert_*` + `Suite` accumulate/finish.
2. `demo.baga` self-check; `tests/testbaga_test.baga` dogfood.
3. `scripts/baga-test` — find `*_test.baga`, run each via baga CLI.
4. Migrate one std test (`sort_test`) onto testbaga.

### P1 — more dogfood + ergonomics

- Migrate remaining std/app tests off local `check`.
- Optional: `assert_eq_f64` with epsilon; `assert_contains`.
- `suite_finish` JSON summary for CI.

### P2 — pure-Baga runner (needs language)

- `list_dir` / readdir in std/os.
- `process_run` / exec to isolate tests (or embed compile API).
- Function values (L5) for `test("name", fn)`.

## Success criteria (P0) — met

1. `sandak build` testbaga OK.
2. `testbaga_test` + `demo` pass; suite counts soft failures correctly.
3. `./scripts/baga-test` runs multiple files and aggregates exit status.
