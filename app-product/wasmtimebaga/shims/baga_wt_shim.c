/* baga_wt_shim.c — handle table + thin Wasmtime C API wrappers for Baga. */
#include "baga_wt_shim.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <wasm.h>
#include <wasmtime.h>

enum {
    H_NONE = 0,
    H_ENGINE = 1,
    H_STORE = 2,
    H_MODULE = 3,
    H_INSTANCE = 4,
    H_LINKER = 5,
    H_MEMORY = 6,
    H_BUF = 7
};

typedef struct {
    int kind;
    union {
        wasm_engine_t *engine;
        struct {
            wasmtime_store_t *store;
            wasm_engine_t *engine;
        } store;
        struct {
            wasmtime_module_t *module;
            wasm_engine_t *engine;
        } module;
        struct {
            wasmtime_instance_t instance;
            int64_t store_h;
        } instance;
        struct {
            wasmtime_linker_t *linker;
            wasm_engine_t *engine;
        } linker;
        struct {
            wasmtime_memory_t memory;
            int64_t store_h;
        } memory;
        struct {
            uint8_t *data;
            size_t len;
        } buf;
    } u;
} Slot;

#define MAX_SLOTS 256
static Slot g_slots[MAX_SLOTS];
static char g_err[1024];
static int64_t g_last_i64;

static int64_t g_host_calls;
static char g_host_log[512];

static void set_err(const char *msg) {
    if (!msg) msg = "unknown error";
    snprintf(g_err, sizeof g_err, "%s", msg);
}

static void set_err_wasmtime(const char *prefix, wasmtime_error_t *error,
                             wasm_trap_t *trap) {
    char buf[896];
    buf[0] = '\0';
    if (error) {
        wasm_name_t m;
        wasmtime_error_message(error, &m);
        size_t n = m.size < sizeof(buf) - 1 ? m.size : sizeof(buf) - 1;
        memcpy(buf, m.data, n);
        buf[n] = '\0';
        wasm_byte_vec_delete(&m);
        wasmtime_error_delete(error);
    } else if (trap) {
        wasm_message_t m;
        wasm_trap_message(trap, &m);
        size_t n = m.size < sizeof(buf) - 1 ? m.size : sizeof(buf) - 1;
        memcpy(buf, m.data, n);
        buf[n] = '\0';
        wasm_byte_vec_delete(&m);
        wasm_trap_delete(trap);
    } else {
        snprintf(buf, sizeof buf, "failed");
    }
    snprintf(g_err, sizeof g_err, "%s: %s", prefix, buf);
}

static int64_t alloc_slot(void) {
    for (int i = 1; i < MAX_SLOTS; i++) {
        if (g_slots[i].kind == H_NONE)
            return (int64_t)i;
    }
    set_err("handle table full");
    return 0;
}

static Slot *get_slot(int64_t h, int kind) {
    if (h <= 0 || h >= MAX_SLOTS) {
        set_err("invalid handle");
        return NULL;
    }
    Slot *s = &g_slots[h];
    if (s->kind != kind) {
        set_err("wrong handle kind");
        return NULL;
    }
    return s;
}

int64_t baga_wt_available(void) { return 1; }
int64_t baga_wt_last_i64(void) { return g_last_i64; }
const char *baga_wt_last_err(void) { return g_err[0] ? g_err : ""; }
int64_t baga_wt_host_call_count(void) { return g_host_calls; }
const char *baga_wt_host_log(void) { return g_host_log[0] ? g_host_log : ""; }

void baga_wt_host_reset(void) {
    g_host_calls = 0;
    g_host_log[0] = '\0';
}

/* ── host callbacks ──────────────────────────────────────────────────── */

static wasm_trap_t *host_dispatch(void *env, wasmtime_caller_t *caller,
                                  const wasmtime_val_t *args, size_t nargs,
                                  wasmtime_val_t *results, size_t nresults) {
    (void)caller;
    int64_t kind = (int64_t)(intptr_t)env;
    g_host_calls++;

    switch (kind) {
    case BAGA_WT_HOST_LOG_VOID0:
        snprintf(g_host_log, sizeof g_host_log, "hello from host");
        return NULL;
    case BAGA_WT_HOST_I32_ID:
        if (nargs < 1 || nresults < 1)
            return wasmtime_trap_new("host i32_id arity", 16);
        results[0].kind = WASMTIME_I32;
        results[0].of.i32 = args[0].of.i32;
        return NULL;
    case BAGA_WT_HOST_I32_ADD:
        if (nargs < 2 || nresults < 1)
            return wasmtime_trap_new("host i32_add arity", 17);
        results[0].kind = WASMTIME_I32;
        results[0].of.i32 = args[0].of.i32 + args[1].of.i32;
        return NULL;
    default:
        return wasmtime_trap_new("unknown host kind", 17);
    }
}

static wasm_functype_t *functype_for_host(int64_t kind) {
    switch (kind) {
    case BAGA_WT_HOST_LOG_VOID0:
        return wasm_functype_new_0_0();
    case BAGA_WT_HOST_I32_ID:
        return wasm_functype_new_1_1(wasm_valtype_new_i32(),
                                     wasm_valtype_new_i32());
    case BAGA_WT_HOST_I32_ADD:
        return wasm_functype_new_2_1(wasm_valtype_new_i32(),
                                     wasm_valtype_new_i32(),
                                     wasm_valtype_new_i32());
    default:
        return NULL;
    }
}

/* ── lifecycle ───────────────────────────────────────────────────────── */

int64_t baga_wt_engine_new(void) {
    g_err[0] = '\0';
    wasm_engine_t *eng = wasm_engine_new();
    if (!eng) {
        set_err("wasm_engine_new failed");
        return 0;
    }
    int64_t h = alloc_slot();
    if (!h) {
        wasm_engine_delete(eng);
        return 0;
    }
    g_slots[h].kind = H_ENGINE;
    g_slots[h].u.engine = eng;
    return h;
}

int64_t baga_wt_store_new(int64_t engine_h) {
    g_err[0] = '\0';
    Slot *es = get_slot(engine_h, H_ENGINE);
    if (!es) return 0;
    wasmtime_store_t *store = wasmtime_store_new(es->u.engine, NULL, NULL);
    if (!store) {
        set_err("wasmtime_store_new failed");
        return 0;
    }
    int64_t h = alloc_slot();
    if (!h) {
        wasmtime_store_delete(store);
        return 0;
    }
    g_slots[h].kind = H_STORE;
    g_slots[h].u.store.store = store;
    g_slots[h].u.store.engine = es->u.engine;
    return h;
}

int64_t baga_wt_module_from_file(int64_t engine_h, const char *path) {
    g_err[0] = '\0';
    Slot *es = get_slot(engine_h, H_ENGINE);
    if (!es) return 0;
    if (!path || !path[0]) {
        set_err("empty module path");
        return 0;
    }
    FILE *f = fopen(path, "rb");
    if (!f) {
        set_err("open module file failed");
        return 0;
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        set_err("fseek module failed");
        return 0;
    }
    long n = ftell(f);
    if (n < 0) {
        fclose(f);
        set_err("ftell module failed");
        return 0;
    }
    if (fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        set_err("fseek module failed");
        return 0;
    }
    uint8_t *wasm = (uint8_t *)malloc((size_t)n);
    if (!wasm) {
        fclose(f);
        set_err("oom reading module");
        return 0;
    }
    size_t rd = fread(wasm, 1, (size_t)n, f);
    fclose(f);
    if (rd != (size_t)n) {
        free(wasm);
        set_err("short read module");
        return 0;
    }

    wasmtime_module_t *module = NULL;
    wasmtime_error_t *error =
        wasmtime_module_new(es->u.engine, wasm, (size_t)n, &module);
    free(wasm);
    if (error || !module) {
        set_err_wasmtime("module_new", error, NULL);
        return 0;
    }

    int64_t h = alloc_slot();
    if (!h) {
        wasmtime_module_delete(module);
        return 0;
    }
    g_slots[h].kind = H_MODULE;
    g_slots[h].u.module.module = module;
    g_slots[h].u.module.engine = es->u.engine;
    return h;
}

int64_t baga_wt_wat2wasm(const char *wat) {
    g_err[0] = '\0';
    if (!wat || !wat[0]) {
        set_err("empty wat source");
        return 0;
    }
    wasm_byte_vec_t out;
    memset(&out, 0, sizeof out);
    wasmtime_error_t *error = wasmtime_wat2wasm(wat, strlen(wat), &out);
    if (error) {
        set_err_wasmtime("wat2wasm", error, NULL);
        return 0;
    }
    uint8_t *data = (uint8_t *)malloc(out.size ? out.size : 1);
    if (!data) {
        wasm_byte_vec_delete(&out);
        set_err("oom copying wasm bytes");
        return 0;
    }
    memcpy(data, out.data, out.size);
    size_t len = out.size;
    wasm_byte_vec_delete(&out);

    int64_t h = alloc_slot();
    if (!h) {
        free(data);
        return 0;
    }
    g_slots[h].kind = H_BUF;
    g_slots[h].u.buf.data = data;
    g_slots[h].u.buf.len = len;
    return h;
}

int64_t baga_wt_buf_len(int64_t buf_h) {
    g_err[0] = '\0';
    g_last_i64 = 0;
    Slot *bs = get_slot(buf_h, H_BUF);
    if (!bs) return 1;
    g_last_i64 = (int64_t)bs->u.buf.len;
    return 0;
}

int64_t baga_wt_buf_get(int64_t buf_h, int64_t i) {
    g_err[0] = '\0';
    g_last_i64 = 0;
    Slot *bs = get_slot(buf_h, H_BUF);
    if (!bs) return 1;
    if (i < 0 || (size_t)i >= bs->u.buf.len) {
        set_err("buf index out of bounds");
        return 1;
    }
    g_last_i64 = (int64_t)bs->u.buf.data[(size_t)i];
    return 0;
}

int64_t baga_wt_module_from_buf(int64_t engine_h, int64_t buf_h) {
    g_err[0] = '\0';
    Slot *es = get_slot(engine_h, H_ENGINE);
    if (!es) return 0;
    Slot *bs = get_slot(buf_h, H_BUF);
    if (!bs) return 0;

    wasmtime_module_t *module = NULL;
    wasmtime_error_t *error = wasmtime_module_new(
        es->u.engine, bs->u.buf.data, bs->u.buf.len, &module);
    if (error || !module) {
        set_err_wasmtime("module_new", error, NULL);
        return 0;
    }

    int64_t h = alloc_slot();
    if (!h) {
        wasmtime_module_delete(module);
        return 0;
    }
    g_slots[h].kind = H_MODULE;
    g_slots[h].u.module.module = module;
    g_slots[h].u.module.engine = es->u.engine;
    return h;
}

static int64_t module_slot(wasm_engine_t *engine, wasmtime_module_t *module) {
    int64_t h = alloc_slot();
    if (!h) {
        wasmtime_module_delete(module);
        return 0;
    }
    g_slots[h].kind = H_MODULE;
    g_slots[h].u.module.module = module;
    g_slots[h].u.module.engine = engine;
    return h;
}

int64_t baga_wt_module_serialize(int64_t module_h) {
    g_err[0] = '\0';
    Slot *ms = get_slot(module_h, H_MODULE);
    if (!ms) return 0;

    wasm_byte_vec_t out;
    memset(&out, 0, sizeof out);
    wasmtime_error_t *error =
        wasmtime_module_serialize(ms->u.module.module, &out);
    if (error) {
        set_err_wasmtime("module_serialize", error, NULL);
        return 0;
    }
    uint8_t *data = (uint8_t *)malloc(out.size ? out.size : 1);
    if (!data) {
        wasm_byte_vec_delete(&out);
        set_err("oom copying serialized module");
        return 0;
    }
    memcpy(data, out.data, out.size);
    size_t len = out.size;
    wasm_byte_vec_delete(&out);

    int64_t h = alloc_slot();
    if (!h) {
        free(data);
        return 0;
    }
    g_slots[h].kind = H_BUF;
    g_slots[h].u.buf.data = data;
    g_slots[h].u.buf.len = len;
    return h;
}

int64_t baga_wt_module_serialize_file(int64_t module_h, const char *path) {
    g_err[0] = '\0';
    Slot *ms = get_slot(module_h, H_MODULE);
    if (!ms) return 1;
    if (!path || !path[0]) {
        set_err("empty serialize path");
        return 1;
    }

    wasm_byte_vec_t out;
    memset(&out, 0, sizeof out);
    wasmtime_error_t *error =
        wasmtime_module_serialize(ms->u.module.module, &out);
    if (error) {
        set_err_wasmtime("module_serialize", error, NULL);
        return 1;
    }
    FILE *f = fopen(path, "wb");
    if (!f) {
        wasm_byte_vec_delete(&out);
        set_err("open serialize path failed");
        return 1;
    }
    size_t total = out.size;
    size_t wr = total ? fwrite(out.data, 1, total, f) : 0;
    int rc = fclose(f);
    wasm_byte_vec_delete(&out);
    if (wr != total || rc != 0) {
        set_err("write serialized module failed");
        return 1;
    }
    return 0;
}

int64_t baga_wt_module_deserialize(int64_t engine_h, int64_t buf_h) {
    g_err[0] = '\0';
    Slot *es = get_slot(engine_h, H_ENGINE);
    if (!es) return 0;
    Slot *bs = get_slot(buf_h, H_BUF);
    if (!bs) return 0;

    wasmtime_module_t *module = NULL;
    wasmtime_error_t *error = wasmtime_module_deserialize(
        es->u.engine, bs->u.buf.data, bs->u.buf.len, &module);
    if (error || !module) {
        set_err_wasmtime("module_deserialize", error, NULL);
        return 0;
    }
    return module_slot(es->u.engine, module);
}

int64_t baga_wt_module_deserialize_file(int64_t engine_h, const char *path) {
    g_err[0] = '\0';
    Slot *es = get_slot(engine_h, H_ENGINE);
    if (!es) return 0;
    if (!path || !path[0]) {
        set_err("empty deserialize path");
        return 0;
    }

    wasmtime_module_t *module = NULL;
    wasmtime_error_t *error =
        wasmtime_module_deserialize_file(es->u.engine, path, &module);
    if (error || !module) {
        set_err_wasmtime("module_deserialize_file", error, NULL);
        return 0;
    }
    return module_slot(es->u.engine, module);
}

int64_t baga_wt_instance_new(int64_t store_h, int64_t module_h) {
    g_err[0] = '\0';
    Slot *ss = get_slot(store_h, H_STORE);
    if (!ss) return 0;
    Slot *ms = get_slot(module_h, H_MODULE);
    if (!ms) return 0;

    wasmtime_context_t *ctx = wasmtime_store_context(ss->u.store.store);
    wasmtime_instance_t instance;
    memset(&instance, 0, sizeof instance);
    wasm_trap_t *trap = NULL;
    wasmtime_error_t *error = wasmtime_instance_new(
        ctx, ms->u.module.module, NULL, 0, &instance, &trap);
    if (error || trap) {
        set_err_wasmtime("instance_new", error, trap);
        return 0;
    }

    int64_t h = alloc_slot();
    if (!h) return 0;
    g_slots[h].kind = H_INSTANCE;
    g_slots[h].u.instance.instance = instance;
    g_slots[h].u.instance.store_h = store_h;
    return h;
}

/* ── linker ──────────────────────────────────────────────────────────── */

int64_t baga_wt_linker_new(int64_t engine_h) {
    g_err[0] = '\0';
    Slot *es = get_slot(engine_h, H_ENGINE);
    if (!es) return 0;
    wasmtime_linker_t *linker = wasmtime_linker_new(es->u.engine);
    if (!linker) {
        set_err("wasmtime_linker_new failed");
        return 0;
    }
    int64_t h = alloc_slot();
    if (!h) {
        wasmtime_linker_delete(linker);
        return 0;
    }
    g_slots[h].kind = H_LINKER;
    g_slots[h].u.linker.linker = linker;
    g_slots[h].u.linker.engine = es->u.engine;
    return h;
}

int64_t baga_wt_linker_define_host(int64_t linker_h, const char *module,
                                   const char *name, int64_t host_kind) {
    g_err[0] = '\0';
    Slot *ls = get_slot(linker_h, H_LINKER);
    if (!ls) return 1;
    if (!name || !name[0]) {
        set_err("empty host export name");
        return 1;
    }
    if (!module) module = "";

    wasm_functype_t *ty = functype_for_host(host_kind);
    if (!ty) {
        set_err("unknown host kind");
        return 1;
    }

    wasmtime_error_t *error = wasmtime_linker_define_func(
        ls->u.linker.linker, module, strlen(module), name, strlen(name), ty,
        host_dispatch, (void *)(intptr_t)host_kind, NULL);
    wasm_functype_delete(ty);
    if (error) {
        set_err_wasmtime("linker_define_func", error, NULL);
        return 1;
    }
    return 0;
}

int64_t baga_wt_linker_define_wasi(int64_t linker_h) {
    g_err[0] = '\0';
    Slot *ls = get_slot(linker_h, H_LINKER);
    if (!ls) return 1;
    wasmtime_error_t *error = wasmtime_linker_define_wasi(ls->u.linker.linker);
    if (error) {
        set_err_wasmtime("linker_define_wasi", error, NULL);
        return 1;
    }
    return 0;
}

int64_t baga_wt_store_set_wasi_inherit(int64_t store_h) {
    g_err[0] = '\0';
    Slot *ss = get_slot(store_h, H_STORE);
    if (!ss) return 1;

    wasi_config_t *wasi = wasi_config_new();
    if (!wasi) {
        set_err("wasi_config_new failed");
        return 1;
    }
    const char *argv0 = "baga-wasi";
    const char *argv[] = {argv0};
    if (!wasi_config_set_argv(wasi, 1, argv)) {
        wasi_config_delete(wasi);
        set_err("wasi_config_set_argv failed");
        return 1;
    }
    wasi_config_inherit_stdout(wasi);
    wasi_config_inherit_stderr(wasi);
    wasi_config_inherit_stdin(wasi);

    wasmtime_context_t *ctx = wasmtime_store_context(ss->u.store.store);
    wasmtime_error_t *error = wasmtime_context_set_wasi(ctx, wasi);
    /* set_wasi takes ownership of wasi on success; on error we must delete? */
    if (error) {
        set_err_wasmtime("context_set_wasi", error, NULL);
        return 1;
    }
    return 0;
}

int64_t baga_wt_linker_instantiate(int64_t linker_h, int64_t store_h,
                                   int64_t module_h) {
    g_err[0] = '\0';
    Slot *ls = get_slot(linker_h, H_LINKER);
    if (!ls) return 0;
    Slot *ss = get_slot(store_h, H_STORE);
    if (!ss) return 0;
    Slot *ms = get_slot(module_h, H_MODULE);
    if (!ms) return 0;

    wasmtime_context_t *ctx = wasmtime_store_context(ss->u.store.store);
    wasmtime_instance_t instance;
    memset(&instance, 0, sizeof instance);
    wasm_trap_t *trap = NULL;
    wasmtime_error_t *error =
        wasmtime_linker_instantiate(ls->u.linker.linker, ctx,
                                    ms->u.module.module, &instance, &trap);
    if (error || trap) {
        set_err_wasmtime("linker_instantiate", error, trap);
        return 0;
    }

    int64_t h = alloc_slot();
    if (!h) return 0;
    g_slots[h].kind = H_INSTANCE;
    g_slots[h].u.instance.instance = instance;
    g_slots[h].u.instance.store_h = store_h;
    return h;
}

/* ── memory ──────────────────────────────────────────────────────────── */

int64_t baga_wt_memory_get(int64_t store_h, int64_t inst_h, const char *name) {
    g_err[0] = '\0';
    Slot *ss = get_slot(store_h, H_STORE);
    if (!ss) return 0;
    Slot *is = get_slot(inst_h, H_INSTANCE);
    if (!is) return 0;
    if (!name || !name[0]) {
        set_err("empty memory export name");
        return 0;
    }
    if (is->u.instance.store_h != store_h) {
        set_err("instance/store mismatch");
        return 0;
    }

    wasmtime_context_t *ctx = wasmtime_store_context(ss->u.store.store);
    wasmtime_extern_t item;
    memset(&item, 0, sizeof item);
    bool ok = wasmtime_instance_export_get(ctx, &is->u.instance.instance, name,
                                           strlen(name), &item);
    if (!ok || item.kind != WASMTIME_EXTERN_MEMORY) {
        set_err("export not found or not a memory");
        return 0;
    }

    int64_t h = alloc_slot();
    if (!h) return 0;
    g_slots[h].kind = H_MEMORY;
    g_slots[h].u.memory.memory = item.of.memory;
    g_slots[h].u.memory.store_h = store_h;
    return h;
}

static int mem_ctx(int64_t store_h, int64_t mem_h, wasmtime_context_t **ctx_out,
                   wasmtime_memory_t **mem_out) {
    Slot *ss = get_slot(store_h, H_STORE);
    if (!ss) return 1;
    Slot *ms = get_slot(mem_h, H_MEMORY);
    if (!ms) return 1;
    if (ms->u.memory.store_h != store_h) {
        set_err("memory/store mismatch");
        return 1;
    }
    *ctx_out = wasmtime_store_context(ss->u.store.store);
    *mem_out = &ms->u.memory.memory;
    return 0;
}

int64_t baga_wt_memory_size(int64_t store_h, int64_t mem_h) {
    g_err[0] = '\0';
    g_last_i64 = 0;
    wasmtime_context_t *ctx;
    wasmtime_memory_t *mem;
    if (mem_ctx(store_h, mem_h, &ctx, &mem)) return 1;
    g_last_i64 = (int64_t)wasmtime_memory_size(ctx, mem);
    return 0;
}

int64_t baga_wt_memory_data_size(int64_t store_h, int64_t mem_h) {
    g_err[0] = '\0';
    g_last_i64 = 0;
    wasmtime_context_t *ctx;
    wasmtime_memory_t *mem;
    if (mem_ctx(store_h, mem_h, &ctx, &mem)) return 1;
    g_last_i64 = (int64_t)wasmtime_memory_data_size(ctx, mem);
    return 0;
}

int64_t baga_wt_memory_load8(int64_t store_h, int64_t mem_h, int64_t offset) {
    g_err[0] = '\0';
    g_last_i64 = 0;
    wasmtime_context_t *ctx;
    wasmtime_memory_t *mem;
    if (mem_ctx(store_h, mem_h, &ctx, &mem)) return 1;
    size_t len = wasmtime_memory_data_size(ctx, mem);
    if (offset < 0 || (size_t)offset >= len) {
        set_err("memory load8 out of bounds");
        return 1;
    }
    uint8_t *data = wasmtime_memory_data(ctx, mem);
    g_last_i64 = (int64_t)data[(size_t)offset];
    return 0;
}

int64_t baga_wt_memory_store8(int64_t store_h, int64_t mem_h, int64_t offset,
                              int64_t value) {
    g_err[0] = '\0';
    wasmtime_context_t *ctx;
    wasmtime_memory_t *mem;
    if (mem_ctx(store_h, mem_h, &ctx, &mem)) return 1;
    size_t len = wasmtime_memory_data_size(ctx, mem);
    if (offset < 0 || (size_t)offset >= len) {
        set_err("memory store8 out of bounds");
        return 1;
    }
    uint8_t *data = wasmtime_memory_data(ctx, mem);
    data[(size_t)offset] = (uint8_t)(value & 0xff);
    return 0;
}

int64_t baga_wt_memory_load32(int64_t store_h, int64_t mem_h, int64_t offset) {
    g_err[0] = '\0';
    g_last_i64 = 0;
    wasmtime_context_t *ctx;
    wasmtime_memory_t *mem;
    if (mem_ctx(store_h, mem_h, &ctx, &mem)) return 1;
    size_t len = wasmtime_memory_data_size(ctx, mem);
    if (offset < 0 || (size_t)offset + 4 > len) {
        set_err("memory load32 out of bounds");
        return 1;
    }
    uint8_t *data = wasmtime_memory_data(ctx, mem);
    uint32_t v;
    memcpy(&v, data + (size_t)offset, 4);
    g_last_i64 = (int64_t)(int32_t)v;
    return 0;
}

int64_t baga_wt_memory_store32(int64_t store_h, int64_t mem_h, int64_t offset,
                               int64_t value) {
    g_err[0] = '\0';
    wasmtime_context_t *ctx;
    wasmtime_memory_t *mem;
    if (mem_ctx(store_h, mem_h, &ctx, &mem)) return 1;
    size_t len = wasmtime_memory_data_size(ctx, mem);
    if (offset < 0 || (size_t)offset + 4 > len) {
        set_err("memory store32 out of bounds");
        return 1;
    }
    uint8_t *data = wasmtime_memory_data(ctx, mem);
    uint32_t v = (uint32_t)value;
    memcpy(data + (size_t)offset, &v, 4);
    return 0;
}

int64_t baga_wt_memory_grow(int64_t store_h, int64_t mem_h, int64_t delta) {
    g_err[0] = '\0';
    g_last_i64 = 0;
    if (delta < 0) {
        set_err("memory grow negative delta");
        return 1;
    }
    wasmtime_context_t *ctx;
    wasmtime_memory_t *mem;
    if (mem_ctx(store_h, mem_h, &ctx, &mem)) return 1;
    uint64_t prev = 0;
    wasmtime_error_t *error =
        wasmtime_memory_grow(ctx, mem, (uint64_t)delta, &prev);
    if (error) {
        set_err_wasmtime("memory_grow", error, NULL);
        return 1;
    }
    g_last_i64 = (int64_t)prev;
    return 0;
}

/* ── calls ───────────────────────────────────────────────────────────── */

static int64_t call_export(int64_t store_h, int64_t inst_h, const char *name,
                           const int64_t *args, size_t nargs, size_t nresults) {
    g_err[0] = '\0';
    g_last_i64 = 0;
    Slot *ss = get_slot(store_h, H_STORE);
    if (!ss) return 1;
    Slot *is = get_slot(inst_h, H_INSTANCE);
    if (!is) return 1;
    if (!name || !name[0]) {
        set_err("empty export name");
        return 1;
    }
    if (is->u.instance.store_h != store_h) {
        set_err("instance/store mismatch");
        return 1;
    }

    wasmtime_context_t *ctx = wasmtime_store_context(ss->u.store.store);
    wasmtime_extern_t item;
    memset(&item, 0, sizeof item);
    bool ok = wasmtime_instance_export_get(ctx, &is->u.instance.instance, name,
                                           strlen(name), &item);
    if (!ok || item.kind != WASMTIME_EXTERN_FUNC) {
        set_err("export not found or not a function");
        return 1;
    }

    wasmtime_val_t params[2];
    for (size_t i = 0; i < nargs && i < 2; i++) {
        params[i].kind = WASMTIME_I32;
        params[i].of.i32 = (int32_t)args[i];
    }
    wasmtime_val_t results[1];
    memset(results, 0, sizeof results);
    wasm_trap_t *trap = NULL;
    wasmtime_error_t *error = wasmtime_func_call(
        ctx, &item.of.func, nargs ? params : NULL, nargs,
        nresults ? results : NULL, nresults, &trap);
    if (error) {
        set_err_wasmtime("func_call", error, NULL);
        return 1;
    }
    if (trap) {
        set_err_wasmtime("func_call", NULL, trap);
        return 2;
    }
    if (nresults > 0) {
        if (results[0].kind == WASMTIME_I32) {
            g_last_i64 = (int64_t)results[0].of.i32;
        } else if (results[0].kind == WASMTIME_I64) {
            g_last_i64 = results[0].of.i64;
        } else {
            g_last_i64 = 0;
        }
    }
    return 0;
}

int64_t baga_wt_call_i32_0(int64_t store_h, int64_t inst_h, const char *name) {
    return call_export(store_h, inst_h, name, NULL, 0, 1);
}

int64_t baga_wt_call_i32_1(int64_t store_h, int64_t inst_h, const char *name,
                           int64_t a) {
    int64_t args[1] = {a};
    return call_export(store_h, inst_h, name, args, 1, 1);
}

int64_t baga_wt_call_i32_2(int64_t store_h, int64_t inst_h, const char *name,
                           int64_t a, int64_t b) {
    int64_t args[2] = {a, b};
    return call_export(store_h, inst_h, name, args, 2, 1);
}

int64_t baga_wt_call_void_0(int64_t store_h, int64_t inst_h, const char *name) {
    return call_export(store_h, inst_h, name, NULL, 0, 0);
}

void baga_wt_drop(int64_t h) {
    if (h <= 0 || h >= MAX_SLOTS) return;
    Slot *s = &g_slots[h];
    switch (s->kind) {
    case H_ENGINE:
        if (s->u.engine) wasm_engine_delete(s->u.engine);
        break;
    case H_STORE:
        if (s->u.store.store) wasmtime_store_delete(s->u.store.store);
        break;
    case H_MODULE:
        if (s->u.module.module) wasmtime_module_delete(s->u.module.module);
        break;
    case H_INSTANCE:
    case H_MEMORY:
        break;
    case H_LINKER:
        if (s->u.linker.linker) wasmtime_linker_delete(s->u.linker.linker);
        break;
    case H_BUF:
        if (s->u.buf.data) free(s->u.buf.data);
        break;
    default:
        break;
    }
    memset(s, 0, sizeof *s);
}
