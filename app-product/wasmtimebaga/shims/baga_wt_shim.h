/* baga_wt_shim.h — Baga-friendly handle API over Wasmtime C API.
 *
 * All entry points use only int64_t / const char * so baga `extern fn` can
 * call them. Opaque Wasmtime objects live in an internal handle table.
 *
 * Status codes for call helpers:
 *   0  — ok (result in baga_wt_last_i64 when applicable)
 *   1  — error (message in baga_wt_last_err)
 *   2  — trap  (message in baga_wt_last_err)
 *
 * Handles: positive i64; 0 = invalid / failure.
 *
 * Host kinds (P1 — fixed registry, no Baga function pointers yet):
 *   WT_HOST_LOG_VOID0  (1)  () -> ()   log "hello from host", ++call count
 *   WT_HOST_I32_ID     (2)  (i32) -> i32   identity
 *   WT_HOST_I32_ADD    (3)  (i32,i32) -> i32  a+b
 *
 * P2: linear memory export + WASI preview1 (define_wasi + inherit stdio).
 * P3a: wasmtime_wat2wasm + module-from-buffer (buffer handles).
 * P3b: module serialize/deserialize (buffer + file paths).
 * P3c: extended call shapes — staged args, multi-value, i64/f64.
 */
#ifndef BAGA_WT_SHIM_H
#define BAGA_WT_SHIM_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Host kind constants (mirror in wasm.baga) */
#define BAGA_WT_HOST_LOG_VOID0  1
#define BAGA_WT_HOST_I32_ID     2
#define BAGA_WT_HOST_I32_ADD    3

/* Engine / store / module / instance lifecycle */
int64_t baga_wt_engine_new(void);
int64_t baga_wt_store_new(int64_t engine_h);
int64_t baga_wt_module_from_file(int64_t engine_h, const char *path);
int64_t baga_wt_instance_new(int64_t store_h, int64_t module_h);

/* WAT → wasm buffer (P3a). Buffer handle or 0; free via baga_wt_drop. */
int64_t baga_wt_wat2wasm(const char *wat);
/* Byte length → baga_wt_last_i64; status return */
int64_t baga_wt_buf_len(int64_t buf_h);
/* Byte at index → baga_wt_last_i64; bounds-checked; status return */
int64_t baga_wt_buf_get(int64_t buf_h, int64_t i);
/* Compile wasm bytes from a buffer handle; module handle or 0 */
int64_t baga_wt_module_from_buf(int64_t engine_h, int64_t buf_h);

/* Module serialization (P3b). Serialized artifacts are arbitrary binary
 * (NUL bytes), so the file paths stay entirely in the shim — baga `str`
 * cannot carry them. */
/* Serialize compiled module into a fresh buffer handle; 0 on error */
int64_t baga_wt_module_serialize(int64_t module_h);
/* Serialize straight to a file path; status return (0 ok / 1 err) */
int64_t baga_wt_module_serialize_file(int64_t module_h, const char *path);
/* Deserialize from a buffer handle; module handle or 0.
 * Only safe on output of baga_wt_module_serialize{,_file}. */
int64_t baga_wt_module_deserialize(int64_t engine_h, int64_t buf_h);
/* Deserialize from a file path; module handle or 0 */
int64_t baga_wt_module_deserialize_file(int64_t engine_h, const char *path);

/* Linker (P1) */
int64_t baga_wt_linker_new(int64_t engine_h);
int64_t baga_wt_linker_define_host(int64_t linker_h, const char *module,
                                   const char *name, int64_t host_kind);
int64_t baga_wt_linker_instantiate(int64_t linker_h, int64_t store_h,
                                   int64_t module_h);

/* WASI (P2) — returns 0 ok / 1 err */
int64_t baga_wt_linker_define_wasi(int64_t linker_h);
/* Build wasi_config: inherit stdio, argv=["baga-wasi"], empty env; attach to store. */
int64_t baga_wt_store_set_wasi_inherit(int64_t store_h);

/* Memory export (P2) — memory handle or 0 */
int64_t baga_wt_memory_get(int64_t store_h, int64_t inst_h, const char *name);
/* size in pages / data size in bytes → baga_wt_last_i64; status return */
int64_t baga_wt_memory_size(int64_t store_h, int64_t mem_h);
int64_t baga_wt_memory_data_size(int64_t store_h, int64_t mem_h);
/* load/store; value in last_i64 for loads; status return */
int64_t baga_wt_memory_load8(int64_t store_h, int64_t mem_h, int64_t offset);
int64_t baga_wt_memory_store8(int64_t store_h, int64_t mem_h, int64_t offset,
                              int64_t value);
int64_t baga_wt_memory_load32(int64_t store_h, int64_t mem_h, int64_t offset);
int64_t baga_wt_memory_store32(int64_t store_h, int64_t mem_h, int64_t offset,
                               int64_t value);
/* grow by delta pages; previous page size in last_i64 */
int64_t baga_wt_memory_grow(int64_t store_h, int64_t mem_h, int64_t delta);

/* Call export `name`. Status in return; numeric result via baga_wt_last_i64. */
int64_t baga_wt_call_i32_0(int64_t store_h, int64_t inst_h, const char *name);
int64_t baga_wt_call_i32_1(int64_t store_h, int64_t inst_h, const char *name,
                           int64_t a);
int64_t baga_wt_call_i32_2(int64_t store_h, int64_t inst_h, const char *name,
                           int64_t a, int64_t b);
int64_t baga_wt_call_void_0(int64_t store_h, int64_t inst_h, const char *name);

/* Extended call shapes (P3c) — staged args + generic call + results vector.
 * Stage up to 8 args with baga_wt_args_push_*, then baga_wt_call consumes
 * them (cleared even on failure). Read results via baga_wt_result_*.
 * Value kinds (mirror in wasm.baga): 0=i32 1=i64 2=f32 3=f64 */
void baga_wt_args_clear(void);
int64_t baga_wt_args_push_i32(int64_t v);
int64_t baga_wt_args_push_i64(int64_t v);
int64_t baga_wt_args_push_f64(double v);
/* Status: 0 ok / 1 error / 2 trap (same convention as call helpers). */
int64_t baga_wt_call(int64_t store_h, int64_t inst_h, const char *name,
                     int64_t nresults);
int64_t baga_wt_result_count(void);        /* count → baga_wt_last_i64 */
int64_t baga_wt_result_kind(int64_t i);    /* kind → baga_wt_last_i64 */
int64_t baga_wt_result_i64(int64_t i);     /* i32/i64 → baga_wt_last_i64 */
int64_t baga_wt_result_f64(int64_t i);     /* f32/f64 → baga_wt_last_f64 */
double baga_wt_last_f64(void);

/* Host side effects visible to Baga */
int64_t baga_wt_host_call_count(void);
const char *baga_wt_host_log(void);
void baga_wt_host_reset(void);

/* Drop a handle. Idempotent on 0. */
void baga_wt_drop(int64_t h);

/* Diagnostics */
int64_t baga_wt_last_i64(void);
const char *baga_wt_last_err(void);

int64_t baga_wt_available(void);

#ifdef __cplusplus
}
#endif

#endif /* BAGA_WT_SHIM_H */
