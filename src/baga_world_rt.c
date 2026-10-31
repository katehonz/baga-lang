/* baga_world_rt.c — runtime за световете в LLVM бекенда.
 *
 * LLVM IR-ът не носи C preamble-а на C бекенда, затова `make llvm` изважда
 * СЪЩИЯ генериран runtime (codegen_api.c, до маркера BAGA_RUNTIME_END) в
 * lib/baga_world_rt_gen.inc и го компилира в lib/libbaga_world_rt.so.
 * Тук има само тънки експортни обвивки `baga_lw_*`: LLVM кодът вика тях през
 * `lli -load lib/libbaga_world_rt.so`, значи поведението (вкл. съобщенията за
 * грешки) е идентично с C бекенда, без втори ръчно поддържан port.
 *
 * Видимостта е hidden; само обвивките са default, за да не изтичат вътрешните
 * static функции/глобали на runtime-а в процеса на lli. */

#include "../lib/baga_world_rt_gen.inc"

#define LW_EXPORT __attribute__((visibility("default")))

/* world_dump/world_dump_compact/wcache_get_bytes връщат bytes, алокирани в
 * thread-local bump арената на C runtime-а. LLVM `drop(bytes)` вика libc
 * free, затова копираме в malloc буфер и връщаме арената обратно. */
static baga_bytes lw_bytes_dup(baga_bytes b, int release_src) {
    baga_bytes r;
    r.len = b.len > 0 ? b.len : 0;
    r.data = NULL;
    if (b.len > 0 && b.data) {
        unsigned char *d = (unsigned char *)malloc((size_t)b.len);
        if (!d) { fprintf(stderr, "baga: world runtime: oom\n"); exit(1); }
        memcpy(d, b.data, (size_t)b.len);
        r.data = d;
    }
    if (release_src && b.data) baga_free(b.data, b.len > 0 ? b.len : 1);
    return r;
}

LW_EXPORT int64_t baga_lw_world_new(void) { return baga_world_new(); }
LW_EXPORT int64_t baga_lw_world_alloc(int64_t w, int64_t n) { return baga_world_alloc(w, n); }
LW_EXPORT int64_t baga_lw_world_freeze(int64_t w) { return baga_world_freeze(w); }
LW_EXPORT int64_t baga_lw_world_state(int64_t w) { return baga_world_state(w); }
LW_EXPORT void baga_lw_world_free(int64_t w) { baga_world_free(w); }
LW_EXPORT int64_t baga_lw_world_used(int64_t w) { return baga_world_used(w); }
LW_EXPORT baga_bytes baga_lw_world_dump(int64_t w) {
    return lw_bytes_dup(baga_world_dump(w), 1);
}
LW_EXPORT baga_bytes baga_lw_world_dump_compact(int64_t w) {
    return lw_bytes_dup(baga_world_dump_compact(w), 1);
}
LW_EXPORT int64_t baga_lw_world_load(baga_bytes b) { return baga_world_load(b); }

LW_EXPORT void baga_lw_world_link(int64_t o, int64_t t) { baga_world_link(o, t); }
LW_EXPORT void baga_lw_world_unlink(int64_t o, int64_t t) { baga_world_unlink(o, t); }
LW_EXPORT int64_t baga_lw_world_links(int64_t o) { return baga_world_links(o); }
LW_EXPORT int64_t baga_lw_world_linked(int64_t o, int64_t i) { return baga_world_linked(o, i); }

LW_EXPORT int64_t baga_lw_wvec_new(int64_t w) { return baga_wvec_new(w); }
LW_EXPORT void baga_lw_wvec_push(int64_t w, int64_t v, int64_t x) { baga_wvec_push(w, v, x); }
LW_EXPORT int64_t baga_lw_wvec_get(int64_t w, int64_t v, int64_t i) { return baga_wvec_get(w, v, i); }
LW_EXPORT int64_t baga_lw_wvec_len(int64_t w, int64_t v) { return baga_wvec_len(w, v); }
LW_EXPORT void baga_lw_wvec_push_str(int64_t w, int64_t v, const char *s) {
    baga_wvec_push_str(w, v, s);
}
LW_EXPORT const char *baga_lw_wvec_get_str(int64_t w, int64_t v, int64_t i) {
    return baga_wvec_get_str(w, v, i);
}
LW_EXPORT void baga_lw_wvec_push_bytes(int64_t w, int64_t v, baga_bytes b) {
    baga_wvec_push_bytes(w, v, b);
}
LW_EXPORT baga_bytes baga_lw_wvec_get_bytes(int64_t w, int64_t v, int64_t i) {
    return baga_wvec_get_bytes(w, v, i);
}

LW_EXPORT int64_t baga_lw_wmap_new(int64_t w) { return baga_wmap_new(w); }
LW_EXPORT void baga_lw_wmap_set(int64_t w, int64_t m, int64_t k, int64_t v) {
    baga_wmap_set(w, m, k, v);
}
LW_EXPORT int64_t baga_lw_wmap_get(int64_t w, int64_t m, int64_t k) {
    return baga_wmap_get(w, m, k);
}
LW_EXPORT int64_t baga_lw_wmap_has(int64_t w, int64_t m, int64_t k) {
    return baga_wmap_has(w, m, k);
}
LW_EXPORT void baga_lw_wmap_del(int64_t w, int64_t m, int64_t k) {
    baga_wmap_del(w, m, k);
}
LW_EXPORT int64_t baga_lw_wmap_len(int64_t w, int64_t m) { return baga_wmap_len(w, m); }
LW_EXPORT void baga_lw_wmap_set_str(int64_t w, int64_t m, int64_t k, const char *s) {
    baga_wmap_set_str(w, m, k, s);
}
LW_EXPORT const char *baga_lw_wmap_get_str(int64_t w, int64_t m, int64_t k) {
    return baga_wmap_get_str(w, m, k);
}

LW_EXPORT int64_t baga_lw_wrec_new(int64_t w, int64_t n) { return baga_wrec_new(w, n); }
LW_EXPORT int64_t baga_lw_wrec_len(int64_t w, int64_t r) { return baga_wrec_len(w, r); }
LW_EXPORT int64_t baga_lw_wrec_has(int64_t w, int64_t r, int64_t i) {
    return baga_wrec_has(w, r, i);
}
LW_EXPORT void baga_lw_wrec_set(int64_t w, int64_t r, int64_t i, int64_t v) {
    baga_wrec_set(w, r, i, v);
}
LW_EXPORT void baga_lw_wrec_set_str(int64_t w, int64_t r, int64_t i, const char *s) {
    baga_wrec_set_str(w, r, i, s);
}
LW_EXPORT void baga_lw_wrec_set_bytes(int64_t w, int64_t r, int64_t i, baga_bytes b) {
    baga_wrec_set_bytes(w, r, i, b);
}
LW_EXPORT int64_t baga_lw_wrec_get(int64_t w, int64_t r, int64_t i) {
    return baga_wrec_get(w, r, i);
}
LW_EXPORT const char *baga_lw_wrec_get_str(int64_t w, int64_t r, int64_t i) {
    return baga_wrec_get_str(w, r, i);
}
LW_EXPORT baga_bytes baga_lw_wrec_get_bytes(int64_t w, int64_t r, int64_t i) {
    return baga_wrec_get_bytes(w, r, i);
}

LW_EXPORT void baga_lw_wcache_set(int64_t w, int64_t k, int64_t v) {
    baga_wcache_set(w, k, v);
}
LW_EXPORT int64_t baga_lw_wcache_get(int64_t w, int64_t k) { return baga_wcache_get(w, k); }
LW_EXPORT int64_t baga_lw_wcache_has(int64_t w, int64_t k) { return baga_wcache_has(w, k); }
LW_EXPORT void baga_lw_wcache_del(int64_t w, int64_t k) { baga_wcache_del(w, k); }
LW_EXPORT void baga_lw_wcache_clear(int64_t w) { baga_wcache_clear(w); }
LW_EXPORT int64_t baga_lw_wcache_len(int64_t w) { return baga_wcache_len(w); }
LW_EXPORT void baga_lw_wcache_set_str(int64_t w, int64_t k, const char *s) {
    baga_wcache_set_str(w, k, s);
}
LW_EXPORT const char *baga_lw_wcache_get_str(int64_t w, int64_t k) {
    return baga_wcache_get_str(w, k);
}
LW_EXPORT void baga_lw_wcache_set_bytes(int64_t w, int64_t k, baga_bytes b) {
    baga_wcache_set_bytes(w, k, b);
}
LW_EXPORT baga_bytes baga_lw_wcache_get_bytes(int64_t w, int64_t k) {
    return lw_bytes_dup(baga_wcache_get_bytes(w, k), 1);
}

LW_EXPORT void baga_lw_world_compact(int64_t w) { baga_world_compact(w); }
