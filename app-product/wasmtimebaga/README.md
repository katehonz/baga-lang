# wasmtimebaga

**Wasmtime host embedding for Baga** — community-style language support, modeled on [wasmtime-go](https://github.com/bytecodealliance/wasmtime-go).

Run WebAssembly modules inside a Baga process via the official **Wasmtime C API** and a thin C shim (handles + `extern fn`).

## Status

**P3a** — WAT→wasm + module from in-memory buffer; Memory + WASI lite; Linker + host registry; gcd / hello / add / mem / wasi / wat demos.

See [ARCHITECTURE.md](ARCHITECTURE.md), [PLAN.md](PLAN.md), [gaps.md](gaps.md).

## Quick start

```bash
# from repo root
make && make sandak

cd app-product/wasmtimebaga
./scripts/fetch-wasmtime-c-api.sh   # once — downloads C API into vendor/
./run_demo.sh
# gcd(6, 27) = 3
# gcd(48, 18) = 6
# wat add(20, 22) = 42
# wasmtimebaga demo: ok
```

Typecheck only (no `libwasmtime` required):

```bash
cd app-product/wasmtimebaga
../../sandak build
```

## API

### P0 — no imports

```baga
import "wasmtimebaga/wasm.baga"

let eng = wt_engine_new()?
let store = wt_store_new(eng)?
let mod = wt_module_from_file(eng, "fixtures/gcd.wasm")?
let inst = wt_instance_new(store, mod)?
let st = wt_call_i32_2_status(store, inst, "gcd", 6, 27)?
// st == 0 → result in wt_last_i64()
```

Convenience: `wt_run_i32_2(path, "gcd", 6, 27)`.

### P1 — Linker + host (wasmtime-go Hello)

```baga
// import "" "hello" → host LOG; export "run"
wt_run_hello("fixtures/hello.wasm", "", "hello", "run")?
// wt_host_log() == "hello from host"

// or manual:
let linker = wt_linker_new(eng)?
wt_linker_define_host(linker, "env", "add", WT_HOST_I32_ADD())?
let inst = wt_linker_instantiate(linker, store, mod)?
```

Host kinds (fixed registry — not free Baga closures yet):

| Kind | Signature | Effect |
|------|-----------|--------|
| `WT_HOST_LOG_VOID0` | `() → ()` | log `"hello from host"` |
| `WT_HOST_I32_ID` | `(i32) → i32` | identity |
| `WT_HOST_I32_ADD` | `(i32,i32) → i32` | `a+b` |

### P2 — Memory + WASI

```baga
let mem = wt_memory_get(store, inst, "mem")?
wt_memory_store8(store, mem, 100, 42)?
let v = wt_memory_load8(store, mem, 100)?   // 42
wt_memory_grow(store, mem, 1)?              // prev pages

// WASI preview1
wt_store_set_wasi_inherit(store)?
wt_linker_define_wasi(linker)?
let inst = wt_linker_instantiate(linker, store, mod)?
```

### P3a — WAT → wasm (no fixture file)

```baga
// Compile WAT text at runtime and run it — no .wasm on disk.
let wat = "(module (func (export \"add\") (param i32 i32) (result i32) local.get 0 local.get 1 i32.add))"
let r = wt_run_wat_i32_2(wat, "add", 20, 22)?   // 42

// Or manually (inspect the produced bytes):
let buf = wt_wat2wasm(wat)?          // buffer handle, 0 on error
let n = wt_buf_len(buf)?             // 41
wt_buf_get(buf, 0)?                  // 0 — magic "\0asm"
let mod = wt_module_from_wasm(eng, buf)?
wt_drop(buf)
```

## Layout

| Path | Role |
|------|------|
| `wasm.baga` | public API + FFI declarations |
| `demo.baga` | demo binary (P0–P3a paths) |
| `shims/` | C handle bridge → libwasmtime |
| `fixtures/gcd.wasm` | sample module |
| `vendor/` | prebuilt C API (fetched) |
| `[native]` in sandak.toml | cflags / ldflags for bin link |

## Design notes

- **Not** a pure-Baga interpreter — production path is Wasmtime (Cranelift), same as Go/C embeddings.
- Handles are positive `i64`; `0` means failure — see `wt_last_err()`.
- Host imports (`WrapFunc`) and WASI are **P1/P2** (gap W3).

## Wasmtime version

Pinned by fetch script (default **v47.0.2**, matching local `wasmtime` CLI). Override:

```bash
WASMTIME_VERSION=v47.0.2 ./scripts/fetch-wasmtime-c-api.sh
```
