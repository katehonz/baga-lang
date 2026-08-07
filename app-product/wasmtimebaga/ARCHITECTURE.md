# wasmtimebaga — architecture

**Date:** 2026-08-07  
**Model:** [wasmtime-go](https://github.com/bytecodealliance/wasmtime-go) (Bytecode Alliance)  
**Status:** P3b (module serialize/deserialize; WAT→wasm + module-from-buffer; Memory + WASI lite; Linker + host registry)

## Goal

Host embedding of [Wasmtime](https://github.com/bytecodealliance/wasmtime) in Baga — same role as Go/Python/C embeddings in the Wasmtime Language Support list: **run wasm inside a Baga process**, not compile Baga to wasm.

## Layers (mirror of wasmtime-go)

```
┌─────────────────────────────────────────────────────────────┐
│  Baga product code                                          │
│  app-product/wasmtimebaga/{wasm,demo}.baga                  │
│  wt_engine_new / wt_module_from_file / wt_call_i32_2 …      │
└───────────────────────────┬─────────────────────────────────┘
                            │  extern fn (i64 / str only)
┌───────────────────────────▼─────────────────────────────────┐
│  C shim — shims/baga_wt_shim.{c,h}                          │
│  handle table (engine|store|module|instance)                │
│  baga_wt_* entry points                                     │
└───────────────────────────┬─────────────────────────────────┘
                            │  Wasmtime C API
┌───────────────────────────▼─────────────────────────────────┐
│  vendor/  (prebuilt C API, same idea as wasmtime-go/build/) │
│  include/{wasm,wasmtime}.h   lib/libwasmtime.so             │
└─────────────────────────────────────────────────────────────┘
```

| wasmtime-go | wasmtimebaga |
|-------------|--------------|
| `engine.go` + CGO | `wasm.baga` + `baga_wt_engine_*` |
| `shims.c` / `shims.h` | `shims/baga_wt_shim.c` |
| `build/linux-x86_64/libwasmtime.*` | `vendor/lib/libwasmtime.so` |
| `WrapFunc` (host callbacks) | **P1** fixed kinds (`LOG` / `I32_ID` / `I32_ADD`); free closures later |
| `Linker` | **P1** `wt_linker_*` |
| WASI | **P2** `define_wasi` + inherit stdio/argv |
| Memory | **P2** export get + load/store/grow |

## Why a C shim (not raw C API from Baga)

1. Baga `extern fn` only supports `i64` / `str` / `f64` / `void` — not structs, not `uint8_t *` + length cleanly for binary modules.
2. Wasmtime objects are pointers or small structs (`wasmtime_instance_t`) — owned in a **handle table** keyed by positive `i64`.
3. Host callbacks (`WrapFunc`) need C function pointers — shim will register them later.

## Handle model

| Kind | Create | Drop |
|------|--------|------|
| engine | `baga_wt_engine_new` | `baga_wt_drop` |
| store | `baga_wt_store_new(eng)` | `baga_wt_drop` |
| module | `baga_wt_module_from_file(eng, path)`, **P3a** `baga_wt_module_from_buf(eng, buf)`, or **P3b** `baga_wt_module_deserialize{,_file}` | `baga_wt_drop` |
| buf | **P3a** `baga_wt_wat2wasm(wat)` or **P3b** `baga_wt_module_serialize(mod)` | `baga_wt_drop` (frees bytes) |
| instance | `baga_wt_instance_new(store, mod)` | `baga_wt_drop` (no Wasmtime dtor) |

Call path: look up store + instance → `wasmtime_instance_export_get` → `wasmtime_func_call`.  
Status: `0` ok, `1` error, `2` trap; result in `baga_wt_last_i64()`; message in `baga_wt_last_err()`.

## Tooling

| Mechanism | Role |
|-----------|------|
| `sandak.toml` `[native]` | `c_source`, `cflags`, `ldflags` for bin builds |
| `BAGA_CFLAGS` / `BAGA_LDFLAGS` / `BAGA_EXTRA_OBJS` | `baga` CLI link of tests |
| `scripts/fetch-wasmtime-c-api.sh` | download matching prebuilt C API into `vendor/` |
| `run_demo.sh` / `run_test.sh` | build shim + run with `LD_LIBRARY_PATH` |

Default `sandak build` on this package is **lib check only** (`wasm.baga` has no `main`) so monorepo CI does not require `libwasmtime` until you opt into `run_demo.sh`.

## Success criteria

**P0 (GCD):** load `fixtures/gcd.wasm`, instantiate, `gcd(6,27)→3`.

**P1 (Hello + host add):**
1. `fixtures/hello.wasm` imports `""."hello"`; linker defines `WT_HOST_LOG_VOID0`; `run` → host log set.
2. `fixtures/add_host.wasm` imports `env.add`; host `I32_ADD`; `sum(20,22)→42`.

**P2 (Memory + WASI):**
1. `fixtures/mem.wasm` — host load/store/grow; guest `load` sees host writes.
2. `fixtures/wasi_yield.wasm` — WASI `sched_yield` → errno 0 (requires exported `memory`).

**P3a (WAT → wasm):** compile a WAT string in-process (no fixture file) →
magic `\0asm` bytes readable via buffer handle → instantiate from buffer →
`add(20,22)→42`. Bad WAT → handle 0 + `last_err`.

**P3b (serialize/deserialize):** `serialize(gcd.wasm module)` → buffer +
file round trips reproduce `gcd(6,27)→3` / `gcd(48,18)→6`; deserialize
rejects non-artifact bytes (raw wasm source) with an error, never a
fake module.

## Roadmap sketch

| Phase | Scope |
|-------|--------|
| **P0** ✅ | Engine/Store/Module/Instance, file load, call i32×2→i32, demo+test, `[native]` sandak, fetch script |
| **P1** ✅ | Host imports (fixed registry), Linker name-define, Hello + host-add fixtures |
| **P2** ✅ | Memory export (load/store/grow), WASI define + inherit stdio (`sched_yield`) |
| **P3a** ✅ | WAT→wasm in-process + module from buffer (buffer handles) |
| **P3b** ✅ | Module serialize/deserialize — buffer + binary-safe file paths |
| **P3** | Fuel/epoch; preopen dirs; multi-value; component model; free Baga host closures |

## Non-goals

- Pure-Baga wasm interpreter (separate experiment if ever)
- Compile Baga → wasm (producer toolchain)
- Component Model in P0
- Vendoring multi-arch blobs in git (fetch script; `vendor/lib` gitignored)

## Package layout

```
app-product/wasmtimebaga/
  ARCHITECTURE.md   PLAN.md   gaps.md   README.md
  sandak.toml
  wasm.baga         # public API + externs
  demo.baga         # demo (P0–P3b paths)
  shims/            # C bridge
  fixtures/         # gcd.wat / gcd.wasm
  vendor/           # C API (fetched)
  scripts/          # fetch + run
```
