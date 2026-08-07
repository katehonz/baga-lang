# wasmtimebaga — plan

Date: 2026-08-07  
Status: **P2 done** (Memory + WASI lite live)  
Goal: Wasmtime host embedding for Baga, modeled on **wasmtime-go**.

## Why

Language Support in Wasmtime is about **host embeddings**. Go is an official BA binding over the C API (CGO + shims + prebuilt lib). Baga gets the same layering: idiomatic `.baga` API + C shim + `libwasmtime`.

## Phases

### P0 — GCD path (this iteration) ✅

1. Architecture doc + package scaffold under `app-product/wasmtimebaga`.
2. Toolchain: sandak `[native]` (`c_source` / `cflags` / `ldflags`); baga env `BAGA_CFLAGS` / `BAGA_LDFLAGS` / `BAGA_EXTRA_OBJS`.
3. C shim handle API: engine, store, module-from-file, instance (0 imports), call i32.
4. Baga wrappers in `wasm.baga`; `demo.baga` prints `gcd(6,27)=3`.
5. Fixture `fixtures/gcd.{wat,wasm}`; fetch script for C API v47.
6. `gaps.md` for W1–W6; `run_demo.sh` / `run_test.sh` + `tests/smoke.baga`.

### P1 — host imports + Linker ✅

1. `baga_wt_linker_new` / `define_host` / `instantiate`.
2. Fixed host registry: `LOG_VOID0`, `I32_ID`, `I32_ADD` (gap W3 residual).
3. `fixtures/hello.wasm` + `add_host.wasm`; `wt_run_hello`; `call_void_0`.
4. Demo + smoke cover Hello and host-add paths.

### P2 — Memory + WASI lite ✅

1. Memory export handle: size/data_size, load8/store8, load32/store32, grow.
2. WASI: `linker_define_wasi` + `store_set_wasi_inherit` (argv=`baga-wasi`, inherit stdio).
3. Fixtures: `mem.wasm`, `wasi_yield.wasm` (`sched_yield` + `memory` export).
4. Demo + smoke cover both paths. Fuel/epoch deferred to P3.

### P3 — polish

- WAT→wasm via shim (`wasmtime_wat2wasm`) optional.
- Multi-value, multi-memory, serialize module.
- Optional component model.

## Non-goals (P0)

Host callbacks, WASI, component model, multi-arch vendor in git, pure interpreter.

## Files

| File | Role |
|------|------|
| `ARCHITECTURE.md` | layer diagram + decisions |
| `wasm.baga` | public API + externs |
| `demo.baga` | gcd smoke binary source |
| `shims/baga_wt_shim.*` | handle bridge |
| `fixtures/gcd.*` | wasmtime-go GCD module |
| `scripts/fetch-wasmtime-c-api.sh` | vendor C API |
| `run_demo.sh` / `run_test.sh` | link + run |
| `gaps.md` | language / packaging gaps |

## Success criteria (P0) — met

1. `./run_demo.sh` prints `gcd(6, 27) = 3` and `wasmtimebaga demo: ok`.
2. `sandak build` in package dir typechecks the lib without requiring vendor.
3. Gaps logged honestly (host callbacks, link story, optional CI).
