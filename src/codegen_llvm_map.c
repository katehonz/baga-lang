/* codegen_llvm_map.c — част от codegen_llvm.c (виж codegen_llvm_impl.h). */
#include "baga.h"
#include "codegen_llvm_impl.h"

/* ---- Map: baga_Map = { baga_MapEntry **b, i64 nb, i64 len } ----

 *
 * Огледало на C preamble-а (codegen_c): вериги от baga_MapEntry,
 * FNV-1a хеш за str/bytes ключове и splitmix-подобен микс за i64;
 * rehash с удвояване при load > 3/4. ktag: 0=i64, 1=str, 2=bytes.
 * Стойности: iv/fv/sv/bv inline; pv е box (malloc копие) за struct/enum.
 * baga_MapEntry = { i64 ik, i8* sk, baga_bytes bk, i64 ktag,
 *                   i64 iv, double fv, i8* sv, baga_bytes bv, i8* pv,
 *                   baga_MapEntry *next } — полета 0..9. */

#ifdef BAGA_LLVM

static LLVMTypeRef baga_map_entry_ty(void) {
    LLVMTypeRef t = LLVMGetTypeByName(lg.mod, "baga_MapEntry");
    if (t) return t;
    t = LLVMStructCreateNamed(lg.ctx, "baga_MapEntry");
    LLVMTypeRef elems[] = {
        lg.i64_ty, lg.ptr_ty, llv_baga_bytes_ty(), lg.i64_ty,
        lg.i64_ty, lg.double_ty, lg.ptr_ty, llv_baga_bytes_ty(), lg.ptr_ty,
        LLVMPointerType(t, 0),
    };
    LLVMStructSetBody(t, elems, 10, 0);
    return t;
}
LLVMTypeRef llv_baga_map_entry_ptr_ty(void) {
    return LLVMPointerType(baga_map_entry_ty(), 0);
}
static LLVMTypeRef baga_map_entry_slot_ty(void) {
    return LLVMPointerType(llv_baga_map_entry_ptr_ty(), 0);
}
static LLVMTypeRef baga_map_ty(void) {
    LLVMTypeRef t = LLVMGetTypeByName(lg.mod, "baga_Map");
    if (t) return t;
    t = LLVMStructCreateNamed(lg.ctx, "baga_Map");
    LLVMTypeRef elems[] = {
        baga_map_entry_slot_ty(), lg.i64_ty, lg.i64_ty,
    };
    LLVMStructSetBody(t, elems, 3, 0);
    return t;
}
LLVMTypeRef llv_baga_map_ptr_ty(void) {
    return LLVMPointerType(baga_map_ty(), 0);
}

static LLVMValueRef map_fld(LLVMValueRef m, unsigned idx, const char *nm) {
    return LLVMBuildStructGEP2(lg.builder, baga_map_ty(), m, idx, nm);
}
LLVMValueRef llv_ent_fld(LLVMValueRef e, unsigned idx, const char *nm) {
    return LLVMBuildStructGEP2(lg.builder, baga_map_entry_ty(), e, idx, nm);
}
LLVMValueRef llv_map_load_nb(LLVMValueRef m) {
    return LLVMBuildLoad2(lg.builder, lg.i64_ty, map_fld(m, 1, "nbp"), "nb");
}
LLVMValueRef llv_map_load_b(LLVMValueRef m) {
    return LLVMBuildLoad2(lg.builder, baga_map_entry_slot_ty(),
                          map_fld(m, 0, "bp"), "b");
}

static LLVMValueRef rt_memcmp(void) {
    LLVMTypeRef p[] = { lg.ptr_ty, lg.ptr_ty, lg.i64_ty };
    return llv_rt_libc("memcmp", lg.i32_ty, p, 3);
}

/* static uint64_t baga_map_hash_str(const char *s) — FNV-1a до NUL */
LLVMValueRef llv_build_baga_map_hash_str(void) {
    LLVMTypeRef p[] = { lg.ptr_ty };
    LLVMValueRef fn = LLVMAddFunction(lg.mod, "baga_map_hash_str",
        LLVMFunctionType(lg.i64_ty, p, 1, 0));
    llv_h_begin(fn);
    LLVMValueRef h = llv_entry_alloca(lg.i64_ty, "h");
    LLVMBuildStore(lg.builder,
        LLVMConstInt(lg.i64_ty, 1469598103934665603ULL, 0), h);
    LLVMValueRef sp = llv_entry_alloca(lg.ptr_ty, "sp");
    LLVMBuildStore(lg.builder, LLVMGetParam(fn, 0), sp);
    LLVMBasicBlockRef cond = LLVMAppendBasicBlockInContext(lg.ctx, fn, "cond");
    LLVMBasicBlockRef body = LLVMAppendBasicBlockInContext(lg.ctx, fn, "body");
    LLVMBasicBlockRef done = LLVMAppendBasicBlockInContext(lg.ctx, fn, "done");
    LLVMBuildBr(lg.builder, cond);
    LLVMPositionBuilderAtEnd(lg.builder, cond);
    LLVMValueRef sv = LLVMBuildLoad2(lg.builder, lg.ptr_ty, sp, "sv");
    LLVMValueRef c = LLVMBuildLoad2(lg.builder, lg.i8_ty, sv, "c");
    LLVMValueRef is0 = LLVMBuildICmp(lg.builder, LLVMIntEQ, c,
        LLVMConstInt(lg.i8_ty, 0, 0), "is0");
    LLVMBuildCondBr(lg.builder, is0, done, body);
    LLVMPositionBuilderAtEnd(lg.builder, body);
    LLVMValueRef hv = LLVMBuildLoad2(lg.builder, lg.i64_ty, h, "hv");
    LLVMValueRef x = LLVMBuildXor(lg.builder, hv,
        LLVMBuildZExt(lg.builder, c, lg.i64_ty, "cz"), "x");
    LLVMBuildStore(lg.builder, LLVMBuildMul(lg.builder, x,
        LLVMConstInt(lg.i64_ty, 1099511628211ULL, 0), "hm"), h);
    LLVMValueRef one = LLVMConstInt(lg.i64_ty, 1, 0);
    LLVMBuildStore(lg.builder,
        LLVMBuildGEP2(lg.builder, lg.i8_ty, sv, &one, 1, "sn"), sp);
    LLVMBuildBr(lg.builder, cond);
    LLVMPositionBuilderAtEnd(lg.builder, done);
    LLVMBuildRet(lg.builder, LLVMBuildLoad2(lg.builder, lg.i64_ty, h, "r"));
    return fn;
}

/* static uint64_t baga_map_hash_i64(int64_t k) — splitmix-подобен микс */
LLVMValueRef llv_build_baga_map_hash_i64(void) {
    LLVMTypeRef p[] = { lg.i64_ty };
    LLVMValueRef fn = LLVMAddFunction(lg.mod, "baga_map_hash_i64",
        LLVMFunctionType(lg.i64_ty, p, 1, 0));
    llv_h_begin(fn);
    LLVMValueRef c33 = LLVMConstInt(lg.i64_ty, 33, 0);
    LLVMValueRef x = LLVMGetParam(fn, 0);
    x = LLVMBuildXor(lg.builder, x, LLVMBuildLShr(lg.builder, x, c33, "s1"), "x1");
    x = LLVMBuildMul(lg.builder, x,
        LLVMConstInt(lg.i64_ty, 0xff51afd7ed558ccdULL, 0), "m1");
    x = LLVMBuildXor(lg.builder, x, LLVMBuildLShr(lg.builder, x, c33, "s2"), "x2");
    x = LLVMBuildMul(lg.builder, x,
        LLVMConstInt(lg.i64_ty, 0xc4ceb9fe1a85ec53ULL, 0), "m2");
    x = LLVMBuildXor(lg.builder, x, LLVMBuildLShr(lg.builder, x, c33, "s3"), "x3");
    LLVMBuildRet(lg.builder, x);
    return fn;
}

/* static uint64_t baga_map_hash_bytes(baga_bytes k)
 * — FNV-1a върху data+len (R67, NUL-safe) */
LLVMValueRef llv_build_baga_map_hash_bytes(void) {
    LLVMTypeRef p[] = { llv_baga_bytes_ty() };
    LLVMValueRef fn = LLVMAddFunction(lg.mod, "baga_map_hash_bytes",
        LLVMFunctionType(lg.i64_ty, p, 1, 0));
    llv_h_begin(fn);
    LLVMValueRef a = llv_bytes_param_alloca(LLVMGetParam(fn, 0));
    LLVMValueRef data = llv_bytes_load_data(a);
    LLVMValueRef len = llv_bytes_load_len(a);
    LLVMValueRef h = llv_entry_alloca(lg.i64_ty, "h");
    LLVMBuildStore(lg.builder,
        LLVMConstInt(lg.i64_ty, 1469598103934665603ULL, 0), h);
    LLVMValueRef iv = llv_entry_alloca(lg.i64_ty, "i");
    LLVMBuildStore(lg.builder, LLVMConstInt(lg.i64_ty, 0, 0), iv);
    LLVMBasicBlockRef cond = LLVMAppendBasicBlockInContext(lg.ctx, fn, "cond");
    LLVMBasicBlockRef body = LLVMAppendBasicBlockInContext(lg.ctx, fn, "body");
    LLVMBasicBlockRef done = LLVMAppendBasicBlockInContext(lg.ctx, fn, "done");
    LLVMBuildBr(lg.builder, cond);
    LLVMPositionBuilderAtEnd(lg.builder, cond);
    LLVMValueRef i = LLVMBuildLoad2(lg.builder, lg.i64_ty, iv, "i");
    LLVMValueRef cc = LLVMBuildICmp(lg.builder, LLVMIntSLT, i, len, "cc");
    LLVMBuildCondBr(lg.builder, cc, body, done);
    LLVMPositionBuilderAtEnd(lg.builder, body);
    LLVMValueRef cb = LLVMBuildLoad2(lg.builder, lg.i8_ty,
        LLVMBuildGEP2(lg.builder, lg.i8_ty, data, &i, 1, "dp"), "cb");
    LLVMValueRef hv = LLVMBuildLoad2(lg.builder, lg.i64_ty, h, "hv");
    LLVMValueRef x = LLVMBuildXor(lg.builder, hv,
        LLVMBuildZExt(lg.builder, cb, lg.i64_ty, "cz"), "x");
    LLVMBuildStore(lg.builder, LLVMBuildMul(lg.builder, x,
        LLVMConstInt(lg.i64_ty, 1099511628211ULL, 0), "hm"), h);
    LLVMBuildStore(lg.builder, LLVMBuildAdd(lg.builder, i,
        LLVMConstInt(lg.i64_ty, 1, 0), "in"), iv);
    LLVMBuildBr(lg.builder, cond);
    LLVMPositionBuilderAtEnd(lg.builder, done);
    LLVMBuildRet(lg.builder, LLVMBuildLoad2(lg.builder, lg.i64_ty, h, "r"));
    return fn;
}

/* занулява кофите b[0..nb) */
static void map_null_buckets(LLVMValueRef fn, LLVMValueRef b, LLVMValueRef nb) {
    LLVMValueRef iv = llv_entry_alloca(lg.i64_ty, "i");
    LLVMBuildStore(lg.builder, LLVMConstInt(lg.i64_ty, 0, 0), iv);
    LLVMBasicBlockRef cond = LLVMAppendBasicBlockInContext(lg.ctx, fn, "cond");
    LLVMBasicBlockRef body = LLVMAppendBasicBlockInContext(lg.ctx, fn, "body");
    LLVMBasicBlockRef done = LLVMAppendBasicBlockInContext(lg.ctx, fn, "done");
    LLVMBuildBr(lg.builder, cond);
    LLVMPositionBuilderAtEnd(lg.builder, cond);
    LLVMValueRef i = LLVMBuildLoad2(lg.builder, lg.i64_ty, iv, "i");
    LLVMValueRef cc = LLVMBuildICmp(lg.builder, LLVMIntSLT, i, nb, "cc");
    LLVMBuildCondBr(lg.builder, cc, body, done);
    LLVMPositionBuilderAtEnd(lg.builder, body);
    LLVMBuildStore(lg.builder, LLVMConstNull(llv_baga_map_entry_ptr_ty()),
        LLVMBuildGEP2(lg.builder, llv_baga_map_entry_ptr_ty(), b, &i, 1, "slot"));
    LLVMBuildStore(lg.builder, LLVMBuildAdd(lg.builder, i,
        LLVMConstInt(lg.i64_ty, 1, 0), "in"), iv);
    LLVMBuildBr(lg.builder, cond);
    LLVMPositionBuilderAtEnd(lg.builder, done);
}

/* static baga_Map *baga_map_new(void) — 16 празни кофи.
 * Под --rc struct-ът носи header (rc живее върху него); bucket масивът
 * остава plain malloc — притежаван 1:1, умира с контейнера (като в C:
 * release_map го free-ва директно, никой не го retain-ва). */
LLVMValueRef llv_build_baga_map_new(void) {
    LLVMValueRef fn = LLVMAddFunction(lg.mod, "baga_map_new",
        LLVMFunctionType(llv_baga_map_ptr_ty(), NULL, 0, 0));
    llv_h_begin(fn);
    LLVMValueRef m = LLVMBuildBitCast(lg.builder,
        llv_rc_alloc_call(LLVMSizeOf(baga_map_ty()), "raw"), llv_baga_map_ptr_ty(), "m");
    LLVMBuildStore(lg.builder, LLVMConstInt(lg.i64_ty, 16, 0),
                   map_fld(m, 1, "nbp"));
    LLVMBuildStore(lg.builder, LLVMConstInt(lg.i64_ty, 0, 0),
                   map_fld(m, 2, "lenp"));
    LLVMValueRef bsz[] = { LLVMConstInt(lg.i64_ty, 16 * 8, 0) };
    LLVMValueRef b = LLVMBuildBitCast(lg.builder,
        llv_h_call(llv_rt_malloc(), bsz, 1, "braw"), baga_map_entry_slot_ty(), "b");
    LLVMBuildStore(lg.builder, b, map_fld(m, 0, "bp"));
    map_null_buckets(fn, b, LLVMConstInt(lg.i64_ty, 16, 0));
    LLVMBuildRet(lg.builder, m);
    return fn;
}

/* static baga_MapEntry **baga_map_slot(baga_Map *m, int64_t ik,
 *     const char *sk, uint64_t h)
 * Връща слота на записа или NULL връзката (insert point). */
LLVMValueRef llv_build_baga_map_slot(void) {
    LLVMTypeRef p[] = { llv_baga_map_ptr_ty(), lg.i64_ty, lg.ptr_ty, lg.i64_ty };
    LLVMValueRef fn = LLVMAddFunction(lg.mod, "baga_map_slot",
        LLVMFunctionType(baga_map_entry_slot_ty(), p, 4, 0));
    llv_h_begin(fn);
    LLVMValueRef m = LLVMGetParam(fn, 0);
    LLVMValueRef ik = LLVMGetParam(fn, 1);
    LLVMValueRef sk = LLVMGetParam(fn, 2);
    LLVMValueRef h = LLVMGetParam(fn, 3);
    LLVMValueRef idx = LLVMBuildURem(lg.builder, h, llv_map_load_nb(m), "idx");
    LLVMValueRef sp = llv_entry_alloca(baga_map_entry_slot_ty(), "sp");
    LLVMBuildStore(lg.builder, LLVMBuildGEP2(lg.builder,
        llv_baga_map_entry_ptr_ty(), llv_map_load_b(m), &idx, 1, "s0"), sp);
    LLVMBasicBlockRef cond = LLVMAppendBasicBlockInContext(lg.ctx, fn, "cond");
    LLVMBasicBlockRef chk  = LLVMAppendBasicBlockInContext(lg.ctx, fn, "chk");
    LLVMBasicBlockRef chks = LLVMAppendBasicBlockInContext(lg.ctx, fn, "chks");
    LLVMBasicBlockRef chki = LLVMAppendBasicBlockInContext(lg.ctx, fn, "chki");
    LLVMBasicBlockRef cmps = LLVMAppendBasicBlockInContext(lg.ctx, fn, "cmps");
    LLVMBasicBlockRef cmpi = LLVMAppendBasicBlockInContext(lg.ctx, fn, "cmpi");
    LLVMBasicBlockRef nextb = LLVMAppendBasicBlockInContext(lg.ctx, fn, "next");
    LLVMBasicBlockRef retb = LLVMAppendBasicBlockInContext(lg.ctx, fn, "ret");
    LLVMBuildBr(lg.builder, cond);
    LLVMPositionBuilderAtEnd(lg.builder, cond);
    LLVMValueRef s = LLVMBuildLoad2(lg.builder, baga_map_entry_slot_ty(), sp, "s");
    LLVMValueRef e = LLVMBuildLoad2(lg.builder, llv_baga_map_entry_ptr_ty(), s, "e");
    LLVMBuildCondBr(lg.builder, LLVMBuildIsNull(lg.builder, e, "isn"), retb, chk);
    LLVMPositionBuilderAtEnd(lg.builder, chk);
    LLVMValueRef esk = LLVMBuildLoad2(lg.builder, lg.ptr_ty,
        llv_ent_fld(e, 1, "skp"), "esk");
    LLVMBuildCondBr(lg.builder, LLVMBuildIsNull(lg.builder, sk, "skn"),
                    chki, chks);
    /* str ключ: (*e)->sk && strcmp((*e)->sk, sk) == 0 */
    LLVMPositionBuilderAtEnd(lg.builder, chks);
    LLVMBuildCondBr(lg.builder, LLVMBuildIsNull(lg.builder, esk, "esn"),
                    nextb, cmps);
    LLVMPositionBuilderAtEnd(lg.builder, cmps);
    LLVMValueRef ca[] = { esk, sk };
    LLVMValueRef cmp = llv_h_call(llv_rt_strcmp(), ca, 2, "cmp");
    LLVMValueRef eq = LLVMBuildICmp(lg.builder, LLVMIntEQ, cmp,
        LLVMConstInt(lg.i32_ty, 0, 0), "eq");
    LLVMBuildCondBr(lg.builder, eq, retb, nextb);
    /* i64 ключ: !(*e)->sk && (*e)->ik == ik */
    LLVMPositionBuilderAtEnd(lg.builder, chki);
    LLVMBuildCondBr(lg.builder, LLVMBuildIsNull(lg.builder, esk, "esn2"),
                    cmpi, nextb);
    LLVMPositionBuilderAtEnd(lg.builder, cmpi);
    LLVMValueRef eik = LLVMBuildLoad2(lg.builder, lg.i64_ty,
        llv_ent_fld(e, 0, "ikp"), "eik");
    LLVMValueRef eqi = LLVMBuildICmp(lg.builder, LLVMIntEQ, eik, ik, "eqi");
    LLVMBuildCondBr(lg.builder, eqi, retb, nextb);
    LLVMPositionBuilderAtEnd(lg.builder, nextb);
    LLVMBuildStore(lg.builder, llv_ent_fld(e, 9, "nxp"), sp);
    LLVMBuildBr(lg.builder, cond);
    LLVMPositionBuilderAtEnd(lg.builder, retb);
    LLVMBuildRet(lg.builder,
        LLVMBuildLoad2(lg.builder, baga_map_entry_slot_ty(), sp, "r"));
    return fn;
}

/* static baga_MapEntry **baga_map_slot_b(baga_Map *m, baga_bytes k,
 *     uint64_t h) — R67: memcmp сравнение, само записи с ktag == 2 */
LLVMValueRef llv_build_baga_map_slot_b(void) {
    LLVMTypeRef p[] = { llv_baga_map_ptr_ty(), llv_baga_bytes_ty(), lg.i64_ty };
    LLVMValueRef fn = LLVMAddFunction(lg.mod, "baga_map_slot_b",
        LLVMFunctionType(baga_map_entry_slot_ty(), p, 3, 0));
    llv_h_begin(fn);
    LLVMValueRef m = LLVMGetParam(fn, 0);
    LLVMValueRef ka = llv_bytes_param_alloca(LLVMGetParam(fn, 1));
    LLVMValueRef kdata = llv_bytes_load_data(ka);
    LLVMValueRef klen = llv_bytes_load_len(ka);
    LLVMValueRef h = LLVMGetParam(fn, 2);
    LLVMValueRef idx = LLVMBuildURem(lg.builder, h, llv_map_load_nb(m), "idx");
    LLVMValueRef sp = llv_entry_alloca(baga_map_entry_slot_ty(), "sp");
    LLVMBuildStore(lg.builder, LLVMBuildGEP2(lg.builder,
        llv_baga_map_entry_ptr_ty(), llv_map_load_b(m), &idx, 1, "s0"), sp);
    LLVMBasicBlockRef cond = LLVMAppendBasicBlockInContext(lg.ctx, fn, "cond");
    LLVMBasicBlockRef chk  = LLVMAppendBasicBlockInContext(lg.ctx, fn, "chk");
    LLVMBasicBlockRef chkl = LLVMAppendBasicBlockInContext(lg.ctx, fn, "chkl");
    LLVMBasicBlockRef chkm = LLVMAppendBasicBlockInContext(lg.ctx, fn, "chkm");
    LLVMBasicBlockRef cmpm = LLVMAppendBasicBlockInContext(lg.ctx, fn, "cmpm");
    LLVMBasicBlockRef nextb = LLVMAppendBasicBlockInContext(lg.ctx, fn, "next");
    LLVMBasicBlockRef retb = LLVMAppendBasicBlockInContext(lg.ctx, fn, "ret");
    LLVMBuildBr(lg.builder, cond);
    LLVMPositionBuilderAtEnd(lg.builder, cond);
    LLVMValueRef s = LLVMBuildLoad2(lg.builder, baga_map_entry_slot_ty(), sp, "s");
    LLVMValueRef e = LLVMBuildLoad2(lg.builder, llv_baga_map_entry_ptr_ty(), s, "e");
    LLVMBuildCondBr(lg.builder, LLVMBuildIsNull(lg.builder, e, "isn"), retb, chk);
    LLVMPositionBuilderAtEnd(lg.builder, chk);
    LLVMValueRef ktag = LLVMBuildLoad2(lg.builder, lg.i64_ty,
        llv_ent_fld(e, 3, "ktp"), "ktag");
    LLVMValueRef is2 = LLVMBuildICmp(lg.builder, LLVMIntEQ, ktag,
        LLVMConstInt(lg.i64_ty, 2, 0), "is2");
    LLVMBuildCondBr(lg.builder, is2, chkl, nextb);
    LLVMPositionBuilderAtEnd(lg.builder, chkl);
    LLVMValueRef bkp = llv_ent_fld(e, 2, "bkp");
    LLVMValueRef ebl = LLVMBuildLoad2(lg.builder, lg.i64_ty,
        LLVMBuildStructGEP2(lg.builder, llv_baga_bytes_ty(), bkp, 1, "blp"), "ebl");
    LLVMValueRef eql = LLVMBuildICmp(lg.builder, LLVMIntEQ, ebl, klen, "eql");
    LLVMBuildCondBr(lg.builder, eql, chkm, nextb);
    /* k.len == 0 || memcmp((*e)->bk.data, k.data, k.len) == 0 */
    LLVMPositionBuilderAtEnd(lg.builder, chkm);
    LLVMValueRef isz = LLVMBuildICmp(lg.builder, LLVMIntEQ, klen,
        LLVMConstInt(lg.i64_ty, 0, 0), "isz");
    LLVMBuildCondBr(lg.builder, isz, retb, cmpm);
    LLVMPositionBuilderAtEnd(lg.builder, cmpm);
    LLVMValueRef ebd = LLVMBuildLoad2(lg.builder, lg.ptr_ty,
        LLVMBuildStructGEP2(lg.builder, llv_baga_bytes_ty(), bkp, 0, "bdp"), "ebd");
    LLVMValueRef ma[] = { ebd, kdata, klen };
    LLVMValueRef cmp = llv_h_call(rt_memcmp(), ma, 3, "cmp");
    LLVMValueRef eq = LLVMBuildICmp(lg.builder, LLVMIntEQ, cmp,
        LLVMConstInt(lg.i32_ty, 0, 0), "eq");
    LLVMBuildCondBr(lg.builder, eq, retb, nextb);
    LLVMPositionBuilderAtEnd(lg.builder, nextb);
    LLVMBuildStore(lg.builder, llv_ent_fld(e, 9, "nxp"), sp);
    LLVMBuildBr(lg.builder, cond);
    LLVMPositionBuilderAtEnd(lg.builder, retb);
    LLVMBuildRet(lg.builder,
        LLVMBuildLoad2(lg.builder, baga_map_entry_slot_ty(), sp, "r"));
    return fn;
}

/* static void baga_map_rehash(baga_Map *m) — удвояване; хешът се смята
 * наново за всеки запис по ktag (0=i64, 1=str, 2=bytes) */
LLVMValueRef llv_build_baga_map_rehash(void) {
    LLVMTypeRef p[] = { llv_baga_map_ptr_ty() };
    LLVMValueRef fn = LLVMAddFunction(lg.mod, "baga_map_rehash",
        LLVMFunctionType(lg.void_ty, p, 1, 0));
    llv_h_begin(fn);
    LLVMValueRef m = LLVMGetParam(fn, 0);
    LLVMValueRef onb = llv_map_load_nb(m);
    LLVMValueRef ob = llv_map_load_b(m);
    LLVMValueRef nb2 = LLVMBuildMul(lg.builder, onb,
        LLVMConstInt(lg.i64_ty, 2, 0), "nb2");
    LLVMBuildStore(lg.builder, nb2, map_fld(m, 1, "nbp"));
    LLVMValueRef bsz[] = { LLVMBuildMul(lg.builder, nb2,
        LLVMConstInt(lg.i64_ty, 8, 0), "bsz") };
    LLVMValueRef nb = LLVMBuildBitCast(lg.builder,
        llv_h_call(llv_rt_malloc(), bsz, 1, "braw"), baga_map_entry_slot_ty(), "nb");
    LLVMBuildStore(lg.builder, nb, map_fld(m, 0, "bp"));
    map_null_buckets(fn, nb, nb2);
    /* for j in [0, onb): пренареждане на веригата ob[j] */
    LLVMValueRef jv = llv_entry_alloca(lg.i64_ty, "j");
    LLVMBuildStore(lg.builder, LLVMConstInt(lg.i64_ty, 0, 0), jv);
    LLVMValueRef ev = llv_entry_alloca(llv_baga_map_entry_ptr_ty(), "e");
    LLVMValueRef hv = llv_entry_alloca(lg.i64_ty, "hv");
    LLVMBasicBlockRef cond2 = LLVMAppendBasicBlockInContext(lg.ctx, fn, "ocond");
    LLVMBasicBlockRef body2 = LLVMAppendBasicBlockInContext(lg.ctx, fn, "obody");
    LLVMBasicBlockRef done2 = LLVMAppendBasicBlockInContext(lg.ctx, fn, "odone");
    LLVMBasicBlockRef wcond = LLVMAppendBasicBlockInContext(lg.ctx, fn, "wcond");
    LLVMBasicBlockRef wbody = LLVMAppendBasicBlockInContext(lg.ctx, fn, "wbody");
    LLVMBasicBlockRef wdone = LLVMAppendBasicBlockInContext(lg.ctx, fn, "wdone");
    LLVMBasicBlockRef hb  = LLVMAppendBasicBlockInContext(lg.ctx, fn, "hb");
    LLVMBasicBlockRef hs  = LLVMAppendBasicBlockInContext(lg.ctx, fn, "hs");
    LLVMBasicBlockRef hst = LLVMAppendBasicBlockInContext(lg.ctx, fn, "hst");
    LLVMBasicBlockRef hi  = LLVMAppendBasicBlockInContext(lg.ctx, fn, "hi");
    LLVMBasicBlockRef hj  = LLVMAppendBasicBlockInContext(lg.ctx, fn, "hj");
    LLVMBuildBr(lg.builder, cond2);
    LLVMPositionBuilderAtEnd(lg.builder, cond2);
    LLVMValueRef j = LLVMBuildLoad2(lg.builder, lg.i64_ty, jv, "j");
    LLVMValueRef cc = LLVMBuildICmp(lg.builder, LLVMIntSLT, j, onb, "cc");
    LLVMBuildCondBr(lg.builder, cc, body2, done2);
    LLVMPositionBuilderAtEnd(lg.builder, body2);
    LLVMBuildStore(lg.builder, LLVMBuildLoad2(lg.builder,
        llv_baga_map_entry_ptr_ty(),
        LLVMBuildGEP2(lg.builder, llv_baga_map_entry_ptr_ty(), ob, &j, 1, "slot"),
        "e0"), ev);
    LLVMBuildBr(lg.builder, wcond);
    LLVMPositionBuilderAtEnd(lg.builder, wcond);
    LLVMValueRef e = LLVMBuildLoad2(lg.builder, llv_baga_map_entry_ptr_ty(), ev, "e");
    LLVMBuildCondBr(lg.builder, LLVMBuildIsNull(lg.builder, e, "isn"),
                    wdone, wbody);
    LLVMPositionBuilderAtEnd(lg.builder, wbody);
    LLVMValueRef nx = LLVMBuildLoad2(lg.builder, llv_baga_map_entry_ptr_ty(),
        llv_ent_fld(e, 9, "nxp"), "nx");
    LLVMValueRef ktag = LLVMBuildLoad2(lg.builder, lg.i64_ty,
        llv_ent_fld(e, 3, "ktp"), "ktag");
    LLVMValueRef is2 = LLVMBuildICmp(lg.builder, LLVMIntEQ, ktag,
        LLVMConstInt(lg.i64_ty, 2, 0), "is2");
    LLVMBuildCondBr(lg.builder, is2, hb, hs);
    /* ktag == 2 → hash_bytes(e->bk) */
    LLVMPositionBuilderAtEnd(lg.builder, hb);
    LLVMValueRef bkv = LLVMBuildLoad2(lg.builder, llv_baga_bytes_ty(),
        llv_ent_fld(e, 2, "bkp"), "bkv");
    LLVMValueRef ba[] = { bkv };
    LLVMBuildStore(lg.builder,
        llv_h_call(llv_baga_rt("baga_map_hash_bytes"), ba, 1, "h"), hv);
    LLVMBuildBr(lg.builder, hj);
    /* ktag != 2 → e->sk ? hash_str(e->sk) : hash_i64(e->ik) */
    LLVMPositionBuilderAtEnd(lg.builder, hs);
    LLVMValueRef esk = LLVMBuildLoad2(lg.builder, lg.ptr_ty,
        llv_ent_fld(e, 1, "skp"), "esk");
    LLVMBuildCondBr(lg.builder, LLVMBuildIsNull(lg.builder, esk, "esn"),
                    hi, hst);
    LLVMPositionBuilderAtEnd(lg.builder, hst);
    LLVMValueRef sa[] = { esk };
    LLVMBuildStore(lg.builder,
        llv_h_call(llv_baga_rt("baga_map_hash_str"), sa, 1, "h"), hv);
    LLVMBuildBr(lg.builder, hj);
    LLVMPositionBuilderAtEnd(lg.builder, hi);
    LLVMValueRef eik = LLVMBuildLoad2(lg.builder, lg.i64_ty,
        llv_ent_fld(e, 0, "ikp"), "eik");
    LLVMValueRef ia[] = { eik };
    LLVMBuildStore(lg.builder,
        llv_h_call(llv_baga_rt("baga_map_hash_i64"), ia, 1, "h"), hv);
    LLVMBuildBr(lg.builder, hj);
    /* вмъкване в началото на новата кофа */
    LLVMPositionBuilderAtEnd(lg.builder, hj);
    LLVMValueRef h2 = LLVMBuildLoad2(lg.builder, lg.i64_ty, hv, "h2");
    LLVMValueRef idx = LLVMBuildURem(lg.builder, h2, nb2, "idx");
    LLVMValueRef slotp = LLVMBuildGEP2(lg.builder, llv_baga_map_entry_ptr_ty(),
        nb, &idx, 1, "slotp");
    LLVMValueRef old = LLVMBuildLoad2(lg.builder, llv_baga_map_entry_ptr_ty(),
        slotp, "old");
    LLVMBuildStore(lg.builder, old, llv_ent_fld(e, 9, "nxp"));
    LLVMBuildStore(lg.builder, e, slotp);
    LLVMBuildStore(lg.builder, nx, ev);
    LLVMBuildBr(lg.builder, wcond);
    LLVMPositionBuilderAtEnd(lg.builder, wdone);
    LLVMBuildStore(lg.builder, LLVMBuildAdd(lg.builder, j,
        LLVMConstInt(lg.i64_ty, 1, 0), "jn"), jv);
    LLVMBuildBr(lg.builder, cond2);
    LLVMPositionBuilderAtEnd(lg.builder, done2);
    LLVMBuildRetVoid(lg.builder);
    return fn;
}

/* споделено тяло на put след намиране на слота: създава запис, връзва го,
 * len++, rehash при load > 3/4. ik/sk/bk/ktag са вече изчислени.
 * rc_key (само под --rc): 1 = str ключ, 2 = bytes ключ — retain-ът от put
 * се ПУСКА, ако entry-то вече съществува (ключът не се пъха втори път —
 * codegen_c.c:5933/6062). */
static LLVMValueRef map_put_finish(LLVMValueRef fn, LLVMValueRef m,
                                   LLVMValueRef slot, LLVMValueRef ik,
                                   LLVMValueRef sk, LLVMValueRef bk,
                                   LLVMValueRef ktag, int rc_key) {
    LLVMValueRef found = LLVMBuildLoad2(lg.builder, llv_baga_map_entry_ptr_ty(),
        slot, "found");
    LLVMBasicBlockRef have = LLVMAppendBasicBlockInContext(lg.ctx, fn, "have");
    LLVMBasicBlockRef ins  = LLVMAppendBasicBlockInContext(lg.ctx, fn, "ins");
    LLVMBuildCondBr(lg.builder, LLVMBuildIsNull(lg.builder, found, "isn"),
                    ins, have);
    LLVMPositionBuilderAtEnd(lg.builder, have);
    if (lg.rc && rc_key == 1) {
        llv_h_call(llv_baga_rt("baga_rc_release_str"), &sk, 1, "");
    } else if (lg.rc && rc_key == 2) {
        LLVMValueRef kd = LLVMBuildExtractValue(lg.builder, bk, 0, "kd");
        llv_h_call(llv_baga_rt("baga_rc_release_bytes"), &kd, 1, "");
    }
    LLVMBuildRet(lg.builder, found);
    LLVMPositionBuilderAtEnd(lg.builder, ins);
    LLVMValueRef esz[] = { LLVMSizeOf(baga_map_entry_ty()) };
    LLVMValueRef e = LLVMBuildBitCast(lg.builder,
        llv_h_call(llv_rt_malloc(), esz, 1, "eraw"), llv_baga_map_entry_ptr_ty(), "e");
    LLVMBuildStore(lg.builder, ik, llv_ent_fld(e, 0, "ikp"));
    LLVMBuildStore(lg.builder, sk, llv_ent_fld(e, 1, "skp"));
    LLVMBuildStore(lg.builder, bk, llv_ent_fld(e, 2, "bkp"));
    LLVMBuildStore(lg.builder, ktag, llv_ent_fld(e, 3, "ktp"));
    LLVMBuildStore(lg.builder, LLVMConstInt(lg.i64_ty, 0, 0), llv_ent_fld(e, 4, "ivp"));
    LLVMBuildStore(lg.builder, LLVMConstNull(lg.double_ty), llv_ent_fld(e, 5, "fvp"));
    LLVMBuildStore(lg.builder, LLVMConstNull(lg.ptr_ty), llv_ent_fld(e, 6, "svp"));
    LLVMBuildStore(lg.builder, LLVMConstNull(llv_baga_bytes_ty()), llv_ent_fld(e, 7, "bvp"));
    LLVMBuildStore(lg.builder, LLVMConstNull(lg.ptr_ty), llv_ent_fld(e, 8, "pvp"));
    LLVMBuildStore(lg.builder, LLVMConstNull(llv_baga_map_entry_ptr_ty()),
                   llv_ent_fld(e, 9, "nxp"));
    LLVMBuildStore(lg.builder, e, slot);
    LLVMValueRef lenp = map_fld(m, 2, "lenp");
    LLVMValueRef nl = LLVMBuildAdd(lg.builder,
        LLVMBuildLoad2(lg.builder, lg.i64_ty, lenp, "len"),
        LLVMConstInt(lg.i64_ty, 1, 0), "nl");
    LLVMBuildStore(lg.builder, nl, lenp);
    /* if (m->len * 4 > m->nb * 3) baga_map_rehash(m); */
    LLVMValueRef l4 = LLVMBuildMul(lg.builder, nl,
        LLVMConstInt(lg.i64_ty, 4, 0), "l4");
    LLVMValueRef n3 = LLVMBuildMul(lg.builder, llv_map_load_nb(m),
        LLVMConstInt(lg.i64_ty, 3, 0), "n3");
    LLVMValueRef over = LLVMBuildICmp(lg.builder, LLVMIntSGT, l4, n3, "over");
    LLVMBasicBlockRef rb   = LLVMAppendBasicBlockInContext(lg.ctx, fn, "rb");
    LLVMBasicBlockRef done = LLVMAppendBasicBlockInContext(lg.ctx, fn, "done");
    LLVMBuildCondBr(lg.builder, over, rb, done);
    LLVMPositionBuilderAtEnd(lg.builder, rb);
    LLVMValueRef ra[] = { m };
    llv_h_call(llv_baga_rt("baga_map_rehash"), ra, 1, "");
    LLVMBuildBr(lg.builder, done);
    LLVMPositionBuilderAtEnd(lg.builder, done);
    LLVMBuildRet(lg.builder, e);
    return NULL; /* caller-ът не ползва връзката */
}

/* static baga_MapEntry *baga_map_put(baga_Map *m, int64_t ik,
 *     const char *sk, uint64_t h) — ktag = sk ? 1 : 0
 * Под --rc: retain(sk) преди slot (претенция за съхранение); ако entry-то
 * съществува, map_put_finish пуска retain-а (codegen_c.c:5930-5933).
 * retain/release на NULL (i64 ключ) са no-op по magic guard-а. */
LLVMValueRef llv_build_baga_map_put(void) {
    LLVMTypeRef p[] = { llv_baga_map_ptr_ty(), lg.i64_ty, lg.ptr_ty, lg.i64_ty };
    LLVMValueRef fn = LLVMAddFunction(lg.mod, "baga_map_put",
        LLVMFunctionType(llv_baga_map_entry_ptr_ty(), p, 4, 0));
    llv_h_begin(fn);
    LLVMValueRef m = LLVMGetParam(fn, 0);
    LLVMValueRef ik = LLVMGetParam(fn, 1);
    LLVMValueRef sk = LLVMGetParam(fn, 2);
    if (lg.rc)
        llv_h_call(llv_baga_rt("baga_rc_retain"), &sk, 1, "rcr");
    LLVMValueRef sa[] = { m, ik, sk, LLVMGetParam(fn, 3) };
    LLVMValueRef slot = llv_h_call(llv_baga_rt("baga_map_slot"), sa, 4, "slot");
    LLVMValueRef ktag = LLVMBuildSelect(lg.builder,
        LLVMBuildIsNull(lg.builder, sk, "skn"),
        LLVMConstInt(lg.i64_ty, 0, 0), LLVMConstInt(lg.i64_ty, 1, 0), "ktag");
    map_put_finish(fn, m, slot, ik, sk,
                   LLVMConstNull(llv_baga_bytes_ty()), ktag, 1);
    return fn;
}

/* static baga_MapEntry *baga_map_put_b(baga_Map *m, baga_bytes k,
 *     uint64_t h) — R67 bytes ключ, ktag = 2
 * Под --rc: retain(k.data) преди slot; пуска се при съществуващо entry
 * (codegen_c.c:6059-6062). */
LLVMValueRef llv_build_baga_map_put_b(void) {
    LLVMTypeRef p[] = { llv_baga_map_ptr_ty(), llv_baga_bytes_ty(), lg.i64_ty };
    LLVMValueRef fn = LLVMAddFunction(lg.mod, "baga_map_put_b",
        LLVMFunctionType(llv_baga_map_entry_ptr_ty(), p, 3, 0));
    llv_h_begin(fn);
    LLVMValueRef m = LLVMGetParam(fn, 0);
    LLVMValueRef ka = llv_bytes_param_alloca(LLVMGetParam(fn, 1));
    LLVMValueRef kv = LLVMBuildLoad2(lg.builder, llv_baga_bytes_ty(), ka, "kv");
    if (lg.rc) {
        LLVMValueRef kd = LLVMBuildExtractValue(lg.builder, kv, 0, "kd");
        llv_h_call(llv_baga_rt("baga_rc_retain"), &kd, 1, "rcr");
    }
    LLVMValueRef sa[] = { m, kv, LLVMGetParam(fn, 2) };
    LLVMValueRef slot = llv_h_call(llv_baga_rt("baga_map_slot_b"), sa, 3, "slot");
    map_put_finish(fn, m, slot, LLVMConstInt(lg.i64_ty, 0, 0),
                   LLVMConstNull(lg.ptr_ty), kv,
                   LLVMConstInt(lg.i64_ty, 2, 0), 2);
    return fn;
}

/* static int64_t baga_map_len(baga_Map *m) { return m ? m->len : 0; } */
LLVMValueRef llv_build_baga_map_len(void) {
    LLVMTypeRef p[] = { llv_baga_map_ptr_ty() };
    LLVMValueRef fn = LLVMAddFunction(lg.mod, "baga_map_len",
        LLVMFunctionType(lg.i64_ty, p, 1, 0));
    llv_h_begin(fn);
    LLVMValueRef m = LLVMGetParam(fn, 0);
    LLVMBasicBlockRef nullb = LLVMAppendBasicBlockInContext(lg.ctx, fn, "null");
    LLVMBasicBlockRef haveb = LLVMAppendBasicBlockInContext(lg.ctx, fn, "have");
    LLVMBuildCondBr(lg.builder, LLVMBuildIsNull(lg.builder, m, "isn"),
                    nullb, haveb);
    LLVMPositionBuilderAtEnd(lg.builder, nullb);
    LLVMBuildRet(lg.builder, LLVMConstInt(lg.i64_ty, 0, 0));
    LLVMPositionBuilderAtEnd(lg.builder, haveb);
    LLVMBuildRet(lg.builder,
        LLVMBuildLoad2(lg.builder, lg.i64_ty, map_fld(m, 2, "lenp"), "len"));
    return fn;
}

/* R55: map <-> i64 handle casts (като baga_str_h/baga_h_str) */
LLVMValueRef llv_build_baga_map_h(void) {
    LLVMTypeRef p[] = { llv_baga_map_ptr_ty() };
    LLVMValueRef fn = LLVMAddFunction(lg.mod, "baga_map_h",
        LLVMFunctionType(lg.i64_ty, p, 1, 0));
    llv_h_begin(fn);
    LLVMBuildRet(lg.builder,
        LLVMBuildPtrToInt(lg.builder, LLVMGetParam(fn, 0), lg.i64_ty, "h"));
    return fn;
}
LLVMValueRef llv_build_baga_h_map(void) {
    LLVMTypeRef p[] = { lg.i64_ty };
    LLVMValueRef fn = LLVMAddFunction(lg.mod, "baga_h_map",
        LLVMFunctionType(llv_baga_map_ptr_ty(), p, 1, 0));
    llv_h_begin(fn);
    LLVMBuildRet(lg.builder, LLVMBuildIntToPtr(lg.builder,
        LLVMGetParam(fn, 0), llv_baga_map_ptr_ty(), "m"));
    return fn;
}

/* ---- typed map ops: baga_map_{set,get}_<key>_<val>,
 * baga_map_{has,del,keys}_<key>; key: 0=str, 1=i64, 2=bytes;
 * val: 0=i64, 1=str, 2=f64, 3=bytes, 4=box ---- */

/* "str"→0, "i64"→1, "bytes"→2 (пише дължината в len); -1 при непознат */
int llv_map_key_kind(const char *s, int *len) {
    if (strncmp(s, "str", 3) == 0)   { *len = 3; return 0; }
    if (strncmp(s, "i64", 3) == 0)   { *len = 3; return 1; }
    if (strncmp(s, "bytes", 5) == 0) { *len = 5; return 2; }
    return -1;
}
int llv_map_val_kind(const char *s) {
    if (strcmp(s, "i64") == 0)   return 0;
    if (strcmp(s, "str") == 0)   return 1;
    if (strcmp(s, "f64") == 0)   return 2;
    if (strcmp(s, "bytes") == 0) return 3;
    if (strcmp(s, "box") == 0)   return 4;
    return -1;
}
static LLVMTypeRef map_key_ty(int key) {
    return key == 0 ? lg.ptr_ty : key == 1 ? lg.i64_ty : llv_baga_bytes_ty();
}
static LLVMTypeRef map_val_ty(int val) {
    switch (val) {
        case 0: return lg.i64_ty;
        case 1: return lg.ptr_ty;
        case 2: return lg.double_ty;
        case 3: return llv_baga_bytes_ty();
        default: return lg.ptr_ty;   /* box */
    }
}

/* e = baga_map_put*(m, k, hash(k)) според вида на ключа */
static LLVMValueRef map_put_call(LLVMValueRef m, int key, LLVMValueRef k) {
    if (key == 2) {
        LLVMValueRef ka = llv_bytes_param_alloca(k);
        LLVMValueRef kv = LLVMBuildLoad2(lg.builder, llv_baga_bytes_ty(), ka, "kv");
        LLVMValueRef ha[] = { kv };
        LLVMValueRef hh = llv_h_call(llv_baga_rt("baga_map_hash_bytes"), ha, 1, "h");
        LLVMValueRef pa[] = { m, kv, hh };
        return llv_h_call(llv_baga_rt("baga_map_put_b"), pa, 3, "e");
    }
    LLVMValueRef ha[] = { k };
    LLVMValueRef hh = llv_h_call(llv_baga_rt(key == 0 ? "baga_map_hash_str"
                                              : "baga_map_hash_i64"), ha, 1, "h");
    LLVMValueRef ik = key == 1 ? k : LLVMConstInt(lg.i64_ty, 0, 0);
    LLVMValueRef sk = key == 0 ? k : LLVMConstNull(lg.ptr_ty);
    LLVMValueRef pa[] = { m, ik, sk, hh };
    return llv_h_call(llv_baga_rt("baga_map_put"), pa, 4, "e");
}

/* slot = baga_map_slot*(m, k, hash(k)) — insert point или слот на записа */
static LLVMValueRef map_slot_call(LLVMValueRef m, int key, LLVMValueRef k) {
    if (key == 2) {
        LLVMValueRef ka = llv_bytes_param_alloca(k);
        LLVMValueRef kv = LLVMBuildLoad2(lg.builder, llv_baga_bytes_ty(), ka, "kv");
        LLVMValueRef ha[] = { kv };
        LLVMValueRef hh = llv_h_call(llv_baga_rt("baga_map_hash_bytes"), ha, 1, "h");
        LLVMValueRef pa[] = { m, kv, hh };
        return llv_h_call(llv_baga_rt("baga_map_slot_b"), pa, 3, "slot");
    }
    LLVMValueRef ha[] = { k };
    LLVMValueRef hh = llv_h_call(llv_baga_rt(key == 0 ? "baga_map_hash_str"
                                              : "baga_map_hash_i64"), ha, 1, "h");
    LLVMValueRef ik = key == 1 ? k : LLVMConstInt(lg.i64_ty, 0, 0);
    LLVMValueRef sk = key == 0 ? k : LLVMConstNull(lg.ptr_ty);
    LLVMValueRef pa[] = { m, ik, sk, hh };
    return llv_h_call(llv_baga_rt("baga_map_slot"), pa, 4, "slot");
}

/* static void baga_map_set_<key>_<val>(m, k, v[, size]):
 * e = put(...); e->fld = v; box: e->pv ??= malloc(size); memcpy */
LLVMValueRef llv_build_baga_map_set(const char *name, int key, int val) {
    LLVMTypeRef p[4];
    int np = 0;
    p[np++] = llv_baga_map_ptr_ty();
    p[np++] = map_key_ty(key);
    p[np++] = map_val_ty(val);
    if (val == 4) p[np++] = lg.i64_ty;
    LLVMValueRef fn = LLVMAddFunction(lg.mod, name,
        LLVMFunctionType(lg.void_ty, p, (unsigned)np, 0));
    llv_h_begin(fn);
    LLVMValueRef e = map_put_call(LLVMGetParam(fn, 0), key, LLVMGetParam(fn, 1));
    /* Под --rc: str/bytes стойността се retain-ва; при overwrite — release
     * на старата ПРЕДИ store (codegen_c.c:5961-5962/5982-5983). Старите
     * слотове на нов запис са NULL → release-ът е no-op по magic guard-а.
     * box (struct/enum) стойностите се retain-ват/release-ват от call
     * site-а (той знае типа — baga_map_set_<k>_box е generic). */
    if (lg.rc && val == 1) {
        LLVMValueRef sv = LLVMGetParam(fn, 2);
        llv_h_call(llv_baga_rt("baga_rc_retain"), &sv, 1, "rcr");
        LLVMValueRef old = LLVMBuildLoad2(lg.builder, lg.ptr_ty,
            llv_ent_fld(e, 6, "svp"), "old");
        llv_h_call(llv_baga_rt("baga_rc_release_str"), &old, 1, "");
    } else if (lg.rc && val == 3) {
        LLVMValueRef vd = LLVMBuildExtractValue(lg.builder,
            LLVMGetParam(fn, 2), 0, "vd");
        llv_h_call(llv_baga_rt("baga_rc_retain"), &vd, 1, "rcr");
        LLVMValueRef ob = LLVMBuildLoad2(lg.builder, lg.ptr_ty,
            LLVMBuildStructGEP2(lg.builder, llv_baga_bytes_ty(),
                llv_ent_fld(e, 7, "bvp"), 0, "obdp"), "obd");
        llv_h_call(llv_baga_rt("baga_rc_release_bytes"), &ob, 1, "");
    }
    if (val != 4) {
        unsigned fld = val == 0 ? 4 : val == 1 ? 6 : val == 2 ? 5 : 7;
        LLVMBuildStore(lg.builder, LLVMGetParam(fn, 2), llv_ent_fld(e, fld, "vp"));
        LLVMBuildRetVoid(lg.builder);
        return fn;
    }
    /* box: if (!e->pv) e->pv = malloc(size); memcpy(e->pv, src, size)
     * Под --rc box-ът носи header (llv_rc_alloc_call) — release_map го free-ва
     * през базата след destructor-а на полетата. */
    LLVMValueRef size = LLVMGetParam(fn, 3);
    LLVMValueRef pvp = llv_ent_fld(e, 8, "pvp");
    LLVMValueRef pv = LLVMBuildLoad2(lg.builder, lg.ptr_ty, pvp, "pv");
    LLVMBasicBlockRef alb = LLVMAppendBasicBlockInContext(lg.ctx, fn, "alb");
    LLVMBasicBlockRef cpb = LLVMAppendBasicBlockInContext(lg.ctx, fn, "cpb");
    LLVMBuildCondBr(lg.builder, LLVMBuildIsNull(lg.builder, pv, "isn"), alb, cpb);
    LLVMPositionBuilderAtEnd(lg.builder, alb);
    LLVMBuildStore(lg.builder, llv_rc_alloc_call(size, "box"), pvp);
    LLVMBuildBr(lg.builder, cpb);
    LLVMPositionBuilderAtEnd(lg.builder, cpb);
    LLVMValueRef pv2 = LLVMBuildLoad2(lg.builder, lg.ptr_ty, pvp, "pv2");
    LLVMValueRef mc[] = { pv2, LLVMGetParam(fn, 2), size };
    llv_h_call(llv_rt_memcpy(), mc, 3, "");
    LLVMBuildRetVoid(lg.builder);
    return fn;
}

/* static <val> baga_map_get_<key>_<val>(m, k): e = *slot(...);
 * липсващ ключ → 0 / "" / 0.0 / {NULL,0} / NULL (като codegen_c) */
LLVMValueRef llv_build_baga_map_get(const char *name, int key, int val) {
    LLVMTypeRef rty = map_val_ty(val);
    LLVMTypeRef p[] = { llv_baga_map_ptr_ty(), map_key_ty(key) };
    LLVMValueRef fn = LLVMAddFunction(lg.mod, name,
        LLVMFunctionType(rty, p, 2, 0));
    llv_h_begin(fn);
    LLVMValueRef slot = map_slot_call(LLVMGetParam(fn, 0), key,
                                      LLVMGetParam(fn, 1));
    LLVMValueRef e = LLVMBuildLoad2(lg.builder, llv_baga_map_entry_ptr_ty(),
                                    slot, "e");
    LLVMValueRef res = llv_entry_alloca(rty, "res");
    LLVMBuildStore(lg.builder, val == 1
        ? LLVMBuildGlobalStringPtr(lg.builder, "", "empty")
        : LLVMConstNull(rty), res);
    LLVMBasicBlockRef have = LLVMAppendBasicBlockInContext(lg.ctx, fn, "have");
    LLVMBasicBlockRef done = LLVMAppendBasicBlockInContext(lg.ctx, fn, "done");
    LLVMBuildCondBr(lg.builder, LLVMBuildIsNull(lg.builder, e, "isn"),
                    done, have);
    LLVMPositionBuilderAtEnd(lg.builder, have);
    unsigned fld = val == 0 ? 4 : val == 1 ? 6 : val == 2 ? 5
                 : val == 3 ? 7 : 8;
    LLVMValueRef fv = LLVMBuildLoad2(lg.builder, rty, llv_ent_fld(e, fld, "fp"), "fv");
    if (val == 1) {
        /* str: (e && e->sv) ? e->sv : "" */
        LLVMBasicBlockRef setb = LLVMAppendBasicBlockInContext(lg.ctx, fn, "set");
        LLVMBuildCondBr(lg.builder, LLVMBuildIsNull(lg.builder, fv, "svn"),
                        done, setb);
        LLVMPositionBuilderAtEnd(lg.builder, setb);
        LLVMBuildStore(lg.builder, fv, res);
        LLVMBuildBr(lg.builder, done);
    } else {
        LLVMBuildStore(lg.builder, fv, res);
        LLVMBuildBr(lg.builder, done);
    }
    LLVMPositionBuilderAtEnd(lg.builder, done);
    LLVMBuildRet(lg.builder, LLVMBuildLoad2(lg.builder, rty, res, "r"));
    return fn;
}

/* static int64_t baga_map_has_<key>(m, k) { return *slot ? 1 : 0; } */
LLVMValueRef llv_build_baga_map_has(const char *name, int key) {
    LLVMTypeRef p[] = { llv_baga_map_ptr_ty(), map_key_ty(key) };
    LLVMValueRef fn = LLVMAddFunction(lg.mod, name,
        LLVMFunctionType(lg.i64_ty, p, 2, 0));
    llv_h_begin(fn);
    LLVMValueRef slot = map_slot_call(LLVMGetParam(fn, 0), key,
                                      LLVMGetParam(fn, 1));
    LLVMValueRef e = LLVMBuildLoad2(lg.builder, llv_baga_map_entry_ptr_ty(),
                                    slot, "e");
    LLVMBuildRet(lg.builder, LLVMBuildZExt(lg.builder,
        LLVMBuildIsNotNull(lg.builder, e, "has"), lg.i64_ty, "r"));
    return fn;
}

/* static void baga_map_del_<key>(m, k) — извършва записа от веригата.
 * Под --rc: release на ключа (по ktag) и на str/bytes стойността + free на
 * entry shell-а (codegen_c.c:6021-6028). pv (box) не се пипа — както в C
 * (del не знае val_size; release_map/drop_map я чисти). */
LLVMValueRef llv_build_baga_map_del(const char *name, int key) {
    LLVMTypeRef p[] = { llv_baga_map_ptr_ty(), map_key_ty(key) };
    LLVMValueRef fn = LLVMAddFunction(lg.mod, name,
        LLVMFunctionType(lg.void_ty, p, 2, 0));
    llv_h_begin(fn);
    LLVMValueRef m = LLVMGetParam(fn, 0);
    LLVMValueRef slot = map_slot_call(m, key, LLVMGetParam(fn, 1));
    LLVMValueRef e = LLVMBuildLoad2(lg.builder, llv_baga_map_entry_ptr_ty(),
                                    slot, "e");
    LLVMBasicBlockRef delb = LLVMAppendBasicBlockInContext(lg.ctx, fn, "del");
    LLVMBasicBlockRef done = LLVMAppendBasicBlockInContext(lg.ctx, fn, "done");
    LLVMBuildCondBr(lg.builder, LLVMBuildIsNull(lg.builder, e, "isn"),
                    done, delb);
    LLVMPositionBuilderAtEnd(lg.builder, delb);
    LLVMValueRef nx = LLVMBuildLoad2(lg.builder, llv_baga_map_entry_ptr_ty(),
        llv_ent_fld(e, 9, "nxp"), "nx");
    LLVMBuildStore(lg.builder, nx, slot);
    LLVMValueRef lenp = map_fld(m, 2, "lenp");
    LLVMBuildStore(lg.builder, LLVMBuildSub(lg.builder,
        LLVMBuildLoad2(lg.builder, lg.i64_ty, lenp, "len"),
        LLVMConstInt(lg.i64_ty, 1, 0), "nl"), lenp);
    if (lg.rc) {
        /* ключ: ktag == 2 → bytes, иначе str (NULL при i64 → no-op) */
        LLVMValueRef ktag = LLVMBuildLoad2(lg.builder, lg.i64_ty,
            llv_ent_fld(e, 3, "ktp"), "ktag");
        LLVMValueRef isb = LLVMBuildICmp(lg.builder, LLVMIntEQ, ktag,
            LLVMConstInt(lg.i64_ty, 2, 0), "isb");
        LLVMBasicBlockRef kb = LLVMAppendBasicBlockInContext(lg.ctx, fn, "kb");
        LLVMBasicBlockRef ks = LLVMAppendBasicBlockInContext(lg.ctx, fn, "ks");
        LLVMBasicBlockRef kd = LLVMAppendBasicBlockInContext(lg.ctx, fn, "kd");
        LLVMBuildCondBr(lg.builder, isb, kb, ks);
        LLVMPositionBuilderAtEnd(lg.builder, kb);
        LLVMValueRef bkd = LLVMBuildLoad2(lg.builder, lg.ptr_ty,
            LLVMBuildStructGEP2(lg.builder, llv_baga_bytes_ty(),
                llv_ent_fld(e, 2, "bkp"), 0, "bkdp"), "bkd");
        llv_h_call(llv_baga_rt("baga_rc_release_bytes"), &bkd, 1, "");
        LLVMBuildBr(lg.builder, kd);
        LLVMPositionBuilderAtEnd(lg.builder, ks);
        LLVMValueRef sk = LLVMBuildLoad2(lg.builder, lg.ptr_ty,
            llv_ent_fld(e, 1, "skp"), "sk");
        llv_h_call(llv_baga_rt("baga_rc_release_str"), &sk, 1, "");
        LLVMBuildBr(lg.builder, kd);
        LLVMPositionBuilderAtEnd(lg.builder, kd);
        /* стойност: sv/bv се release-ват безусловно — неизползваните слотове
         * са NULL и release-ът е no-op (като codegen_c) */
        LLVMValueRef sv = LLVMBuildLoad2(lg.builder, lg.ptr_ty,
            llv_ent_fld(e, 6, "svp"), "sv");
        llv_h_call(llv_baga_rt("baga_rc_release_str"), &sv, 1, "");
        LLVMValueRef bvd = LLVMBuildLoad2(lg.builder, lg.ptr_ty,
            LLVMBuildStructGEP2(lg.builder, llv_baga_bytes_ty(),
                llv_ent_fld(e, 7, "bvp"), 0, "bvdp"), "bvd");
        llv_h_call(llv_baga_rt("baga_rc_release_bytes"), &bvd, 1, "");
        LLVMValueRef er = LLVMBuildBitCast(lg.builder, e, lg.ptr_ty, "er");
        llv_h_call(llv_rt_free(), &er, 1, "");
    }
    LLVMBuildBr(lg.builder, done);
    LLVMPositionBuilderAtEnd(lg.builder, done);
    LLVMBuildRetVoid(lg.builder);
    return fn;
}

/* static baga_Vec *baga_map_keys_<key>(m) — ключовете в bucket ред
 * (като C: str → sk ? sk : "", bytes → box-нато baga_bytes) */
LLVMValueRef llv_build_baga_map_keys(const char *name, int key) {
    LLVMTypeRef p[] = { llv_baga_map_ptr_ty() };
    LLVMValueRef fn = LLVMAddFunction(lg.mod, name,
        LLVMFunctionType(llv_baga_vec_ptr_ty(), p, 1, 0));
    llv_h_begin(fn);
    LLVMValueRef m = LLVMGetParam(fn, 0);
    LLVMValueRef v = llv_h_call(llv_baga_rt("baga_vec_new"), NULL, 0, "v");
    LLVMValueRef nb = llv_map_load_nb(m);
    LLVMValueRef b = llv_map_load_b(m);
    LLVMValueRef iv = llv_entry_alloca(lg.i64_ty, "i");
    LLVMBuildStore(lg.builder, LLVMConstInt(lg.i64_ty, 0, 0), iv);
    LLVMValueRef ev = llv_entry_alloca(llv_baga_map_entry_ptr_ty(), "e");
    LLVMBasicBlockRef cond  = LLVMAppendBasicBlockInContext(lg.ctx, fn, "cond");
    LLVMBasicBlockRef body  = LLVMAppendBasicBlockInContext(lg.ctx, fn, "body");
    LLVMBasicBlockRef done  = LLVMAppendBasicBlockInContext(lg.ctx, fn, "done");
    LLVMBasicBlockRef wcond = LLVMAppendBasicBlockInContext(lg.ctx, fn, "wcond");
    LLVMBasicBlockRef wbody = LLVMAppendBasicBlockInContext(lg.ctx, fn, "wbody");
    LLVMBasicBlockRef wdone = LLVMAppendBasicBlockInContext(lg.ctx, fn, "wdone");
    LLVMBuildBr(lg.builder, cond);
    LLVMPositionBuilderAtEnd(lg.builder, cond);
    LLVMValueRef i = LLVMBuildLoad2(lg.builder, lg.i64_ty, iv, "i");
    LLVMValueRef cc = LLVMBuildICmp(lg.builder, LLVMIntSLT, i, nb, "cc");
    LLVMBuildCondBr(lg.builder, cc, body, done);
    LLVMPositionBuilderAtEnd(lg.builder, body);
    LLVMBuildStore(lg.builder, LLVMBuildLoad2(lg.builder,
        llv_baga_map_entry_ptr_ty(),
        LLVMBuildGEP2(lg.builder, llv_baga_map_entry_ptr_ty(), b, &i, 1, "slot"),
        "e0"), ev);
    LLVMBuildBr(lg.builder, wcond);
    LLVMPositionBuilderAtEnd(lg.builder, wcond);
    LLVMValueRef e = LLVMBuildLoad2(lg.builder, llv_baga_map_entry_ptr_ty(), ev, "e");
    LLVMBuildCondBr(lg.builder, LLVMBuildIsNull(lg.builder, e, "isn"),
                    wdone, wbody);
    LLVMPositionBuilderAtEnd(lg.builder, wbody);
    if (key == 0) {
        LLVMValueRef sk = LLVMBuildLoad2(lg.builder, lg.ptr_ty,
            llv_ent_fld(e, 1, "skp"), "sk");
        LLVMValueRef s = LLVMBuildSelect(lg.builder,
            LLVMBuildIsNull(lg.builder, sk, "skn"),
            LLVMBuildGlobalStringPtr(lg.builder, "", "empty"), sk, "s");
        LLVMValueRef pa[] = { v, s };
        llv_h_call(llv_baga_rt("baga_vec_push_str"), pa, 2, "");
    } else if (key == 1) {
        LLVMValueRef pa[] = { v, LLVMBuildLoad2(lg.builder, lg.i64_ty,
            llv_ent_fld(e, 0, "ikp"), "ik") };
        llv_h_call(llv_baga_rt("baga_vec_push_i64"), pa, 2, "");
    } else {
        LLVMValueRef bp = LLVMBuildBitCast(lg.builder, llv_ent_fld(e, 2, "bkp"),
            lg.ptr_ty, "bp");
        LLVMValueRef pa[] = { v, bp, LLVMSizeOf(llv_baga_bytes_ty()) };
        llv_h_call(llv_baga_rt("baga_vec_push_box"), pa, 3, "");
    }
    LLVMBuildStore(lg.builder, LLVMBuildLoad2(lg.builder,
        llv_baga_map_entry_ptr_ty(), llv_ent_fld(e, 9, "nxp"), "nx"), ev);
    LLVMBuildBr(lg.builder, wcond);
    LLVMPositionBuilderAtEnd(lg.builder, wdone);
    LLVMBuildStore(lg.builder, LLVMBuildAdd(lg.builder, i,
        LLVMConstInt(lg.i64_ty, 1, 0), "in"), iv);
    LLVMBuildBr(lg.builder, cond);
    LLVMPositionBuilderAtEnd(lg.builder, done);
    LLVMBuildRet(lg.builder, v);
    return fn;
}

/* L4: struct елементи — generic box helper-и; размерът идва от call site-а
 * (sizeof на mangled struct типа). Елементите са box-нати копия (memcpy при
 * push/set/slice/concat) — огледало на C preamble-а. Без bounds check,
 * както останалите LLVM vec helper-и. Под --rc box-ът е rc_alloc (header) —
 * release_vec го free-ва през базата след destructor-а на полетата. */
LLVMValueRef llv_build_baga_vec_push_box(void) {
    LLVMTypeRef p[] = { llv_baga_vec_ptr_ty(), lg.ptr_ty, lg.i64_ty };
    LLVMValueRef fn = LLVMAddFunction(lg.mod, "baga_vec_push_box",
        LLVMFunctionType(lg.void_ty, p, 3, 0));
    llv_h_begin(fn);
    LLVMValueRef v = LLVMGetParam(fn, 0);
    LLVMValueRef gv[] = { v };
    llv_h_call(llv_baga_rt("baga_vec_grow"), gv, 1, "");
    LLVMValueRef box = llv_rc_alloc_call(LLVMGetParam(fn, 2), "box");
    LLVMValueRef mc[] = { box, LLVMGetParam(fn, 1), LLVMGetParam(fn, 2) };
    llv_h_call(llv_rt_memcpy(), mc, 3, "");
    LLVMValueRef len = llv_vec_load_len(v);
    LLVMValueRef slot = LLVMBuildGEP2(lg.builder, lg.ptr_ty,
        llv_vec_load_data(v), &len, 1, "slot");
    LLVMBuildStore(lg.builder, box, slot);
    LLVMValueRef nl = LLVMBuildAdd(lg.builder, len,
        LLVMConstInt(lg.i64_ty, 1, 0), "nl");
    LLVMBuildStore(lg.builder, nl, llv_vec_field_ptr(v, 1, "lenp"));
    LLVMBuildRetVoid(lg.builder);
    return fn;
}
LLVMValueRef llv_build_baga_vec_get_box(void) {
    LLVMTypeRef p[] = { llv_baga_vec_ptr_ty(), lg.i64_ty };
    LLVMValueRef fn = LLVMAddFunction(lg.mod, "baga_vec_get_box",
        LLVMFunctionType(lg.ptr_ty, p, 2, 0));
    llv_h_begin(fn);
    LLVMBuildRet(lg.builder,
        llv_vec_load_at(LLVMGetParam(fn, 0), LLVMGetParam(fn, 1)));
    return fn;
}
LLVMValueRef llv_build_baga_vec_set_box(void) {
    LLVMTypeRef p[] = { llv_baga_vec_ptr_ty(), lg.i64_ty, lg.ptr_ty, lg.i64_ty };
    LLVMValueRef fn = LLVMAddFunction(lg.mod, "baga_vec_set_box",
        LLVMFunctionType(lg.void_ty, p, 4, 0));
    llv_h_begin(fn);
    LLVMValueRef box = llv_vec_load_at(LLVMGetParam(fn, 0), LLVMGetParam(fn, 1));
    LLVMValueRef mc[] = { box, LLVMGetParam(fn, 2), LLVMGetParam(fn, 3) };
    llv_h_call(llv_rt_memcpy(), mc, 3, "");
    LLVMBuildRetVoid(lg.builder);
    return fn;
}
/* loop i in [from, to): baga_vec_push_box(r, src->data[i], size) */
static void box_copy_loop(LLVMValueRef fn, LLVMValueRef src, LLVMValueRef r,
                          LLVMValueRef from, LLVMValueRef to, LLVMValueRef size) {
    LLVMValueRef iv = LLVMBuildAlloca(lg.builder, lg.i64_ty, "i");
    LLVMBuildStore(lg.builder, from, iv);
    LLVMBasicBlockRef cond = LLVMAppendBasicBlockInContext(lg.ctx, fn, "cond");
    LLVMBasicBlockRef body = LLVMAppendBasicBlockInContext(lg.ctx, fn, "body");
    LLVMBasicBlockRef done = LLVMAppendBasicBlockInContext(lg.ctx, fn, "done");
    LLVMBuildBr(lg.builder, cond);
    LLVMPositionBuilderAtEnd(lg.builder, cond);
    LLVMValueRef i = LLVMBuildLoad2(lg.builder, lg.i64_ty, iv, "i");
    LLVMValueRef cc = LLVMBuildICmp(lg.builder, LLVMIntSLT, i, to, "cc");
    LLVMBuildCondBr(lg.builder, cc, body, done);
    LLVMPositionBuilderAtEnd(lg.builder, body);
    LLVMValueRef e = llv_vec_load_at(src, i);
    LLVMValueRef pa[] = { r, e, size };
    llv_h_call(llv_baga_rt("baga_vec_push_box"), pa, 3, "");
    LLVMValueRef inext = LLVMBuildAdd(lg.builder, i, LLVMConstInt(lg.i64_ty, 1, 0), "in");
    LLVMBuildStore(lg.builder, inext, iv);
    LLVMBuildBr(lg.builder, cond);
    LLVMPositionBuilderAtEnd(lg.builder, done);
}
LLVMValueRef llv_build_baga_vec_slice_box(void) {
    LLVMTypeRef p[] = { llv_baga_vec_ptr_ty(), lg.i64_ty, lg.i64_ty, lg.i64_ty };
    LLVMValueRef fn = LLVMAddFunction(lg.mod, "baga_vec_slice_box",
        LLVMFunctionType(llv_baga_vec_ptr_ty(), p, 4, 0));
    llv_h_begin(fn);
    LLVMValueRef v = LLVMGetParam(fn, 0);
    LLVMValueRef a = LLVMGetParam(fn, 1);
    LLVMValueRef b = LLVMGetParam(fn, 2);
    llv_slice_clamp(v, &a, &b);
    LLVMValueRef r = llv_h_call(llv_baga_rt("baga_vec_new"), NULL, 0, "r");
    box_copy_loop(fn, v, r, a, b, LLVMGetParam(fn, 3));
    LLVMBuildRet(lg.builder, r);
    return fn;
}
LLVMValueRef llv_build_baga_vec_concat_box(void) {
    LLVMTypeRef p[] = { llv_baga_vec_ptr_ty(), llv_baga_vec_ptr_ty(), lg.i64_ty };
    LLVMValueRef fn = LLVMAddFunction(lg.mod, "baga_vec_concat_box",
        LLVMFunctionType(llv_baga_vec_ptr_ty(), p, 3, 0));
    llv_h_begin(fn);
    LLVMValueRef v = LLVMGetParam(fn, 0);
    LLVMValueRef w = LLVMGetParam(fn, 1);
    LLVMValueRef size = LLVMGetParam(fn, 2);
    LLVMValueRef z = LLVMConstInt(lg.i64_ty, 0, 0);
    LLVMValueRef r = llv_h_call(llv_baga_rt("baga_vec_new"), NULL, 0, "r");
    box_copy_loop(fn, v, r, z, llv_vec_load_len(v), size);
    box_copy_loop(fn, w, r, z, llv_vec_load_len(w), size);
    LLVMBuildRet(lg.builder, r);
    return fn;
}

#endif /* BAGA_LLVM */
