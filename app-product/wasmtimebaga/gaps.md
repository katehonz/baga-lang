# wasmtimebaga — language & packaging gaps

Probe log for Wasmtime host embedding (wasmtime-go model).

## W1 — external library linking

**Symptom.** `baga` / `sandak` historically linked only `-lm -pthread`. Cannot resolve `libwasmtime`.

**Mitigation (P0).**  
- sandak `[native]` section: `c_source`, `cflags`, `ldflags` for **bin** builds.  
- `BAGA_CFLAGS` / `BAGA_LDFLAGS` / `BAGA_EXTRA_OBJS` for ad-hoc `baga` CLI runs.  
- Default package entry stays **lib** so monorepo `sandak build` discovery does not require vendor.

**Residual.** No first-class multi-source native, no rpath `$ORIGIN` (sandak `check_safe` bans `$`). Run scripts set `LD_LIBRARY_PATH`.

## W2 — opaque C types / binary buffers

**Symptom.** Wasmtime needs pointers and `uint8_t *` + length. Baga extern only maps `i64`/`str`/`f64`/`void`.

**Mitigation.** Handle table in C shim; module load via **file path**
(`wt_module_from_file`) or **in-memory buffer** (P3a: `wt_wat2wasm` →
buffer handle → `wt_module_from_wasm`). Wasm bytes are readable from Baga
per index (`wt_buf_len` / `wt_buf_get`).

**Next.** Pass baga `bytes`/`Vec` directly to C as data pointer + len
(bulk copy both directions) once codegen supports extern of `bytes` or a
pair of i64s (`ptr`, `len` from a builtin).

## W3 — host callbacks (`WrapFunc`) — partially closed (P1)

**Symptom.** wasmtime-go `WrapFunc` turns a host language function into a wasm import. Baga cannot export a function pointer to C.

**Mitigation (P1).** Fixed host registry in the shim:
- `WT_HOST_LOG_VOID0` (1) — `() -> ()`, sets host log string
- `WT_HOST_I32_ID` (2) — `(i32) -> i32`
- `WT_HOST_I32_ADD` (3) — `(i32,i32) -> i32`

`wt_linker_define_host(linker, module, name, kind)` + Hello/add_host fixtures live.

**Residual.** Arbitrary Baga closures as host funcs still blocked (needs L5 export or codegen glue). Extend registry kinds as product needs appear.

## W4 — vendor / multi-arch prebuilts

**Symptom.** wasmtime-go checks in platform libs on tags. 29MB+ `.so` is heavy for this monorepo.

**Mitigation.** `scripts/fetch-wasmtime-c-api.sh` → `vendor/`; `vendor/lib/` gitignored.

**Residual.** Optional CI job; document version pin (`WASMTIME_VERSION`).

## W5 — multi-value / f32 / f64 call shapes

**Symptom.** Call helpers are i32×0..2 → i32 / void0.

**Verdict.** Extend call helpers as needed; baga `f64` extern exists for some paths.

## W7 — memory as baga `bytes` (partial)

**Symptom.** Host can load/store i8/i32 via offsets; no bulk copy into baga `bytes` yet (extern can't take `bytes`).

**Mitigation (P2).** Offset load/store + grow on memory export handle.

**Next.** Builtin or shim bulk copy once bytes-to-ptr is available.

## W8 — WASI surface

**Mitigation (P2).** `define_wasi` + inherit stdio + fixed argv. Guest needs exported `memory` for some WASI paths.

**Residual.** preopen dirs, custom stdout file, full wasi-http — product-driven.

## W6 — optional test in default suite

**Symptom.** Full `make test` must not fail machines without `libwasmtime`.

**Mitigation.** Lib typecheck only in discovery; live link via `run_test.sh` / optional step.

## Closed / fine

- Wasmtime C API GCD path verified standalone before shim.
- `extern fn` + handle i64 is enough for P0 host→wasm calls.
