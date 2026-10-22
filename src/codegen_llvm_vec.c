/* codegen_llvm_vec.c — част от codegen_llvm.c (виж codegen_llvm_impl.h). */
#include "baga.h"
#include "codegen_llvm_impl.h"

/* ---- Vec: baga_Vec = { void **data, int64_t len, int64_t cap } ----

 * В IR: { i8*, i64, i64 } с елемент i8* (void*); i64 стойности се
 * пакетират с inttoptr/ptrtoint — точно като (void*)(intptr_t)x в C. */

#ifdef BAGA_LLVM

static LLVMTypeRef baga_vec_ty(void) {
    LLVMTypeRef t = LLVMGetTypeByName(lg.mod, "baga_Vec");
    if (t) return t;
    t = LLVMStructCreateNamed(lg.ctx, "baga_Vec");
    LLVMTypeRef elems[] = { LLVMPointerType(lg.ptr_ty, 0), lg.i64_ty, lg.i64_ty };
    LLVMStructSetBody(t, elems, 3, 0);
    return t;
}

LLVMTypeRef llv_baga_vec_ptr_ty(void) {
    return LLVMPointerType(baga_vec_ty(), 0);
}

/* baga_bytes = { i8* data, i64 len } — binary-safe buffer, passed by value
 * (must match the C struct layout exactly for output parity). */
LLVMTypeRef llv_baga_bytes_ty(void) {
    LLVMTypeRef t = LLVMGetTypeByName(lg.mod, "baga_bytes");
    if (t) return t;
    t = LLVMStructCreateNamed(lg.ctx, "baga_bytes");
    LLVMTypeRef elems[] = { lg.ptr_ty, lg.i64_ty };
    LLVMStructSetBody(t, elems, 2, 0);
    return t;
}

/* GEP към поле на baga_Vec (0=data, 1=len, 2=cap) + load */
LLVMValueRef llv_vec_field_ptr(LLVMValueRef v, unsigned idx, const char *nm) {
    return LLVMBuildStructGEP2(lg.builder, baga_vec_ty(), v, idx, nm);
}
LLVMValueRef llv_vec_load_len(LLVMValueRef v) {
    return LLVMBuildLoad2(lg.builder, lg.i64_ty, llv_vec_field_ptr(v, 1, "lenp"), "len");
}
LLVMValueRef llv_vec_load_data(LLVMValueRef v) {
    return LLVMBuildLoad2(lg.builder, LLVMPointerType(lg.ptr_ty, 0),
                          llv_vec_field_ptr(v, 0, "datap"), "data");
}

/* static void baga_vec_grow(baga_Vec *v) {
 *     if (v->len == v->cap) { v->cap *= 2;
 *         v->data = realloc(v->data, v->cap * sizeof(void *)); } }
 * Под --rc: нов rc_alloc + memcpy + free на старата база през header-а
 * (realloc би преместил/дублирал header-а със стария `an`; C прави
 * alloc+memcpy+free винаги — codegen_c.c:5653). */
LLVMValueRef llv_build_baga_vec_grow(void) {
    LLVMTypeRef p[] = { llv_baga_vec_ptr_ty() };
    LLVMValueRef fn = LLVMAddFunction(lg.mod, "baga_vec_grow",
        LLVMFunctionType(lg.void_ty, p, 1, 0));
    llv_h_begin(fn);
    LLVMValueRef v = LLVMGetParam(fn, 0);
    LLVMValueRef len = llv_vec_load_len(v);
    LLVMValueRef capp = llv_vec_field_ptr(v, 2, "capp");
    LLVMValueRef cap = LLVMBuildLoad2(lg.builder, lg.i64_ty, capp, "cap");
    LLVMValueRef full = LLVMBuildICmp(lg.builder, LLVMIntEQ, len, cap, "full");
    LLVMBasicBlockRef grow_bb = LLVMAppendBasicBlockInContext(lg.ctx, fn, "grow");
    LLVMBasicBlockRef done_bb = LLVMAppendBasicBlockInContext(lg.ctx, fn, "done");
    LLVMBuildCondBr(lg.builder, full, grow_bb, done_bb);
    LLVMPositionBuilderAtEnd(lg.builder, grow_bb);
    LLVMValueRef ncap = LLVMBuildMul(lg.builder, cap,
        LLVMConstInt(lg.i64_ty, 2, 0), "ncap");
    LLVMBuildStore(lg.builder, ncap, capp);
    LLVMValueRef data = llv_vec_load_data(v);
    LLVMValueRef nbytes = LLVMBuildMul(lg.builder, ncap,
        LLVMConstInt(lg.i64_ty, 8, 0), "nbytes");
    if (lg.rc) {
        LLVMValueRef nd = llv_rc_alloc_call(nbytes, "nd");
        LLVMValueRef od = LLVMBuildBitCast(lg.builder, data, lg.ptr_ty, "od");
        LLVMValueRef cpy = LLVMBuildMul(lg.builder, len,
            LLVMConstInt(lg.i64_ty, 8, 0), "cpy");
        LLVMValueRef mc[] = { nd, od, cpy };
        llv_h_call(llv_rt_memcpy(), mc, 3, "");
        LLVMValueRef hd = llv_h_call(llv_baga_rt("baga_rc_hdr"), &od, 1, "hd");
        llv_h_call(llv_rt_free(), &hd, 1, "");
        LLVMBuildStore(lg.builder,
            LLVMBuildBitCast(lg.builder, nd, LLVMPointerType(lg.ptr_ty, 0), "ndc"),
            llv_vec_field_ptr(v, 0, "datap"));
    } else {
        LLVMValueRef ra[] = {
            LLVMBuildBitCast(lg.builder, data, lg.ptr_ty, "raw"),
            nbytes
        };
        LLVMValueRef nd = llv_h_call(llv_rt_realloc(), ra, 2, "nd");
        LLVMBuildStore(lg.builder,
            LLVMBuildBitCast(lg.builder, nd, LLVMPointerType(lg.ptr_ty, 0), "ndc"),
            llv_vec_field_ptr(v, 0, "datap"));
    }
    LLVMBuildBr(lg.builder, done_bb);
    LLVMPositionBuilderAtEnd(lg.builder, done_bb);
    LLVMBuildRetVoid(lg.builder);
    return fn;
}

/* static baga_Vec *baga_vec_new(void) {
 *     baga_Vec *v = malloc(sizeof(baga_Vec));
 *     v->cap = 8; v->len = 0; v->data = malloc(8 * sizeof(void *)); return v; }
 * Под --rc и struct-ът, и data масивът носят header (rc живее върху struct-а;
 * data е притежаван 1:1 и се free-ва от release през header базата). */
LLVMValueRef llv_build_baga_vec_new(void) {
    LLVMValueRef fn = LLVMAddFunction(lg.mod, "baga_vec_new",
        LLVMFunctionType(llv_baga_vec_ptr_ty(), NULL, 0, 0));
    llv_h_begin(fn);
    LLVMValueRef sz = LLVMSizeOf(baga_vec_ty());
    LLVMValueRef raw = llv_rc_alloc_call(sz, "raw");
    LLVMValueRef v = LLVMBuildBitCast(lg.builder, raw, llv_baga_vec_ptr_ty(), "v");
    LLVMBuildStore(lg.builder, LLVMConstInt(lg.i64_ty, 8, 0),
                   llv_vec_field_ptr(v, 2, "capp"));
    LLVMBuildStore(lg.builder, LLVMConstInt(lg.i64_ty, 0, 0),
                   llv_vec_field_ptr(v, 1, "lenp"));
    LLVMValueRef draw = llv_rc_alloc_call(LLVMConstInt(lg.i64_ty, 64, 0), "draw");
    LLVMBuildStore(lg.builder,
        LLVMBuildBitCast(lg.builder, draw, LLVMPointerType(lg.ptr_ty, 0), "d"),
        llv_vec_field_ptr(v, 0, "datap"));
    LLVMBuildRet(lg.builder, v);
    return fn;
}

/* споделена стъпка за push: grow + слот data[len] + len++ */
static LLVMValueRef vec_push_slot(LLVMValueRef v) {
    LLVMValueRef ga[] = { v };
    llv_h_call(llv_baga_rt("baga_vec_grow"), ga, 1, "");
    LLVMValueRef len = llv_vec_load_len(v);
    LLVMValueRef data = llv_vec_load_data(v);
    LLVMValueRef nl = LLVMBuildAdd(lg.builder, len,
        LLVMConstInt(lg.i64_ty, 1, 0), "nl");
    LLVMBuildStore(lg.builder, nl, llv_vec_field_ptr(v, 1, "lenp"));
    return LLVMBuildGEP2(lg.builder, lg.ptr_ty, data, &len, 1, "slot");
}

/* static void baga_vec_push_i64(baga_Vec *v, int64_t x)
 * { baga_vec_grow(v); v->data[v->len++] = (void *)(intptr_t)x; } */
LLVMValueRef llv_build_baga_vec_push_i64(void) {
    LLVMTypeRef p[] = { llv_baga_vec_ptr_ty(), lg.i64_ty };
    LLVMValueRef fn = LLVMAddFunction(lg.mod, "baga_vec_push_i64",
        LLVMFunctionType(lg.void_ty, p, 2, 0));
    llv_h_begin(fn);
    LLVMValueRef slot = vec_push_slot(LLVMGetParam(fn, 0));
    LLVMValueRef pv = LLVMBuildIntToPtr(lg.builder, LLVMGetParam(fn, 1),
        lg.ptr_ty, "pv");
    LLVMBuildStore(lg.builder, pv, slot);
    LLVMBuildRetVoid(lg.builder);
    return fn;
}

/* static void baga_vec_push_str(baga_Vec *v, const char *s)
 * { baga_vec_grow(v); v->data[v->len++] = (void *)s; }
 * Под --rc: retain(s) преди grow — контейнерът става собственик
 * (codegen_c.c:5674); release е в baga_rc_release_vec (elem_kind 1). */
LLVMValueRef llv_build_baga_vec_push_str(void) {
    LLVMTypeRef p[] = { llv_baga_vec_ptr_ty(), lg.ptr_ty };
    LLVMValueRef fn = LLVMAddFunction(lg.mod, "baga_vec_push_str",
        LLVMFunctionType(lg.void_ty, p, 2, 0));
    llv_h_begin(fn);
    if (lg.rc) {
        LLVMValueRef s = LLVMGetParam(fn, 1);
        llv_h_call(llv_baga_rt("baga_rc_retain"), &s, 1, "rcr");
    }
    LLVMValueRef slot = vec_push_slot(LLVMGetParam(fn, 0));
    LLVMBuildStore(lg.builder, LLVMGetParam(fn, 1), slot);
    LLVMBuildRetVoid(lg.builder);
    return fn;
}

/* load на data[i] (като i8*) */
LLVMValueRef llv_vec_load_at(LLVMValueRef v, LLVMValueRef i) {
    LLVMValueRef data = llv_vec_load_data(v);
    LLVMValueRef slot = LLVMBuildGEP2(lg.builder, lg.ptr_ty, data, &i, 1, "slot");
    return LLVMBuildLoad2(lg.builder, lg.ptr_ty, slot, "e");
}

/* static int64_t baga_vec_get_i64(baga_Vec *v, int64_t i)
 * { return (int64_t)(intptr_t)v->data[i]; } */
LLVMValueRef llv_build_baga_vec_get_i64(void) {
    LLVMTypeRef p[] = { llv_baga_vec_ptr_ty(), lg.i64_ty };
    LLVMValueRef fn = LLVMAddFunction(lg.mod, "baga_vec_get_i64",
        LLVMFunctionType(lg.i64_ty, p, 2, 0));
    llv_h_begin(fn);
    LLVMValueRef e = llv_vec_load_at(LLVMGetParam(fn, 0), LLVMGetParam(fn, 1));
    LLVMBuildRet(lg.builder, LLVMBuildPtrToInt(lg.builder, e, lg.i64_ty, "r"));
    return fn;
}

/* static const char *baga_vec_get_str(baga_Vec *v, int64_t i)
 * { return (const char *)v->data[i]; } */
LLVMValueRef llv_build_baga_vec_get_str(void) {
    LLVMTypeRef p[] = { llv_baga_vec_ptr_ty(), lg.i64_ty };
    LLVMValueRef fn = LLVMAddFunction(lg.mod, "baga_vec_get_str",
        LLVMFunctionType(lg.ptr_ty, p, 2, 0));
    llv_h_begin(fn);
    LLVMBuildRet(lg.builder, llv_vec_load_at(LLVMGetParam(fn, 0), LLVMGetParam(fn, 1)));
    return fn;
}

/* static void baga_vec_set_i64(baga_Vec *v, int64_t i, int64_t x)
 * { v->data[i] = (void *)(intptr_t)x; } */
LLVMValueRef llv_build_baga_vec_set_i64(void) {
    LLVMTypeRef p[] = { llv_baga_vec_ptr_ty(), lg.i64_ty, lg.i64_ty };
    LLVMValueRef fn = LLVMAddFunction(lg.mod, "baga_vec_set_i64",
        LLVMFunctionType(lg.void_ty, p, 3, 0));
    llv_h_begin(fn);
    LLVMValueRef v = LLVMGetParam(fn, 0);
    LLVMValueRef i = LLVMGetParam(fn, 1);
    LLVMValueRef data = llv_vec_load_data(v);
    LLVMValueRef slot = LLVMBuildGEP2(lg.builder, lg.ptr_ty, data, &i, 1, "slot");
    LLVMValueRef pv = LLVMBuildIntToPtr(lg.builder, LLVMGetParam(fn, 2),
        lg.ptr_ty, "pv");
    LLVMBuildStore(lg.builder, pv, slot);
    LLVMBuildRetVoid(lg.builder);
    return fn;
}

/* static void baga_vec_set_str(baga_Vec *v, int64_t i, const char *s)
 * { v->data[i] = (void *)s; }
 * Под --rc: retain на новия, после release на стария (в този ред —
 * alias-safe, codegen_c.c:5684). */
LLVMValueRef llv_build_baga_vec_set_str(void) {
    LLVMTypeRef p[] = { llv_baga_vec_ptr_ty(), lg.i64_ty, lg.ptr_ty };
    LLVMValueRef fn = LLVMAddFunction(lg.mod, "baga_vec_set_str",
        LLVMFunctionType(lg.void_ty, p, 3, 0));
    llv_h_begin(fn);
    LLVMValueRef v = LLVMGetParam(fn, 0);
    LLVMValueRef i = LLVMGetParam(fn, 1);
    LLVMValueRef data = llv_vec_load_data(v);
    LLVMValueRef slot = LLVMBuildGEP2(lg.builder, lg.ptr_ty, data, &i, 1, "slot");
    if (lg.rc) {
        LLVMValueRef s = LLVMGetParam(fn, 2);
        llv_h_call(llv_baga_rt("baga_rc_retain"), &s, 1, "rcr");
        LLVMValueRef old = LLVMBuildLoad2(lg.builder, lg.ptr_ty, slot, "old");
        llv_h_call(llv_baga_rt("baga_rc_release_str"), &old, 1, "");
    }
    LLVMBuildStore(lg.builder, LLVMGetParam(fn, 2), slot);
    LLVMBuildRetVoid(lg.builder);
    return fn;
}

/* static int64_t baga_vec_len(baga_Vec *v) { return v->len; } */
LLVMValueRef llv_build_baga_vec_len(void) {
    LLVMTypeRef p[] = { llv_baga_vec_ptr_ty() };
    LLVMValueRef fn = LLVMAddFunction(lg.mod, "baga_vec_len",
        LLVMFunctionType(lg.i64_ty, p, 1, 0));
    llv_h_begin(fn);
    LLVMBuildRet(lg.builder, llv_vec_load_len(LLVMGetParam(fn, 0)));
    return fn;
}

/* clamp slice bounds: a=max(a,0); b=min(b,len); b=max(b,a) */
void llv_slice_clamp(LLVMValueRef v, LLVMValueRef *a, LLVMValueRef *b) {
    LLVMValueRef z = LLVMConstInt(lg.i64_ty, 0, 0);
    LLVMValueRef len = llv_vec_load_len(v);
    LLVMValueRef a0 = LLVMBuildICmp(lg.builder, LLVMIntSLT, *a, z, "a0");
    *a = LLVMBuildSelect(lg.builder, a0, z, *a, "a");
    LLVMValueRef b1 = LLVMBuildICmp(lg.builder, LLVMIntSGT, *b, len, "b1");
    *b = LLVMBuildSelect(lg.builder, b1, len, *b, "b");
    LLVMValueRef b2 = LLVMBuildICmp(lg.builder, LLVMIntSLT, *b, *a, "b2");
    *b = LLVMBuildSelect(lg.builder, b2, *a, *b, "b");
}

/* loop pushing src[a..b) elements into r via push_rt (i64/str/f64) */
static void slice_loop(LLVMValueRef fn, LLVMValueRef src, LLVMValueRef r,
                       LLVMValueRef a, LLVMValueRef b, const char *push_rt) {
    LLVMValueRef iv = LLVMBuildAlloca(lg.builder, lg.i64_ty, "i");
    LLVMBuildStore(lg.builder, a, iv);
    LLVMBasicBlockRef cond = LLVMAppendBasicBlockInContext(lg.ctx, fn, "cond");
    LLVMBasicBlockRef body = LLVMAppendBasicBlockInContext(lg.ctx, fn, "body");
    LLVMBasicBlockRef done = LLVMAppendBasicBlockInContext(lg.ctx, fn, "done");
    LLVMBuildBr(lg.builder, cond);
    LLVMPositionBuilderAtEnd(lg.builder, cond);
    LLVMValueRef i = LLVMBuildLoad2(lg.builder, lg.i64_ty, iv, "i");
    LLVMValueRef cc = LLVMBuildICmp(lg.builder, LLVMIntSLT, i, b, "cc");
    LLVMBuildCondBr(lg.builder, cc, body, done);
    LLVMPositionBuilderAtEnd(lg.builder, body);
    LLVMValueRef e = llv_vec_load_at(src, i);
    if (strcmp(push_rt, "baga_vec_push_i64") == 0)
        e = LLVMBuildPtrToInt(lg.builder, e, lg.i64_ty, "e");
    LLVMValueRef pa[] = { r, e };
    llv_h_call(llv_baga_rt(push_rt), pa, 2, "");
    LLVMValueRef inext = LLVMBuildAdd(lg.builder, i, LLVMConstInt(lg.i64_ty, 1, 0), "in");
    LLVMBuildStore(lg.builder, inext, iv);
    LLVMBuildBr(lg.builder, cond);
    LLVMPositionBuilderAtEnd(lg.builder, done);
}

LLVMValueRef llv_build_baga_vec_slice(const char *suf, const char *push_rt) {
    char nm[64]; snprintf(nm, sizeof nm, "baga_vec_slice_%s", suf);
    LLVMTypeRef p[] = { llv_baga_vec_ptr_ty(), lg.i64_ty, lg.i64_ty };
    LLVMValueRef fn = LLVMAddFunction(lg.mod, nm,
        LLVMFunctionType(llv_baga_vec_ptr_ty(), p, 3, 0));
    llv_h_begin(fn);
    LLVMValueRef v = LLVMGetParam(fn, 0);
    LLVMValueRef a = LLVMGetParam(fn, 1);
    LLVMValueRef b = LLVMGetParam(fn, 2);
    llv_slice_clamp(v, &a, &b);
    LLVMValueRef r = llv_h_call(llv_baga_rt("baga_vec_new"), NULL, 0, "r");
    slice_loop(fn, v, r, a, b, push_rt);
    LLVMBuildRet(lg.builder, r);
    return fn;
}

/* loop pushing all of v's elements into r */
static void concat_loop(LLVMValueRef fn, LLVMValueRef src, LLVMValueRef r, const char *push_rt) {
    LLVMValueRef len = llv_vec_load_len(src);
    LLVMValueRef z = LLVMConstInt(lg.i64_ty, 0, 0);
    LLVMValueRef iv = LLVMBuildAlloca(lg.builder, lg.i64_ty, "i");
    LLVMBuildStore(lg.builder, z, iv);
    LLVMBasicBlockRef cond = LLVMAppendBasicBlockInContext(lg.ctx, fn, "cond");
    LLVMBasicBlockRef body = LLVMAppendBasicBlockInContext(lg.ctx, fn, "body");
    LLVMBasicBlockRef done = LLVMAppendBasicBlockInContext(lg.ctx, fn, "done");
    LLVMBuildBr(lg.builder, cond);
    LLVMPositionBuilderAtEnd(lg.builder, cond);
    LLVMValueRef i = LLVMBuildLoad2(lg.builder, lg.i64_ty, iv, "i");
    LLVMValueRef cc = LLVMBuildICmp(lg.builder, LLVMIntSLT, i, len, "cc");
    LLVMBuildCondBr(lg.builder, cc, body, done);
    LLVMPositionBuilderAtEnd(lg.builder, body);
    LLVMValueRef e = llv_vec_load_at(src, i);
    if (strcmp(push_rt, "baga_vec_push_i64") == 0)
        e = LLVMBuildPtrToInt(lg.builder, e, lg.i64_ty, "e");
    LLVMValueRef pa[] = { r, e };
    llv_h_call(llv_baga_rt(push_rt), pa, 2, "");
    LLVMValueRef inext = LLVMBuildAdd(lg.builder, i, LLVMConstInt(lg.i64_ty, 1, 0), "in");
    LLVMBuildStore(lg.builder, inext, iv);
    LLVMBuildBr(lg.builder, cond);
    LLVMPositionBuilderAtEnd(lg.builder, done);
}

LLVMValueRef llv_build_baga_vec_concat(const char *suf, const char *push_rt) {
    char nm[64]; snprintf(nm, sizeof nm, "baga_vec_concat_%s", suf);
    LLVMTypeRef p[] = { llv_baga_vec_ptr_ty(), llv_baga_vec_ptr_ty() };
    LLVMValueRef fn = LLVMAddFunction(lg.mod, nm,
        LLVMFunctionType(llv_baga_vec_ptr_ty(), p, 2, 0));
    llv_h_begin(fn);
    LLVMValueRef v = LLVMGetParam(fn, 0);
    LLVMValueRef w = LLVMGetParam(fn, 1);
    LLVMValueRef r = llv_h_call(llv_baga_rt("baga_vec_new"), NULL, 0, "r");
    concat_loop(fn, v, r, push_rt);
    concat_loop(fn, w, r, push_rt);
    LLVMBuildRet(lg.builder, r);
    return fn;
}

/* ---- arena (mirror на baga_arena_* в codegen_c) ---- */
static LLVMTypeRef baga_arena_ty(void) {
    LLVMTypeRef t = LLVMGetTypeByName(lg.mod, "baga_Arena");
    if (t) return t;
    t = LLVMStructCreateNamed(lg.ctx, "baga_Arena");
    LLVMTypeRef elems[] = { lg.ptr_ty, lg.i64_ty, lg.i64_ty };
    LLVMStructSetBody(t, elems, 3, 0);
    return t;
}
static LLVMTypeRef baga_arena_ptr_ty(void) {
    return LLVMPointerType(baga_arena_ty(), 0);
}
static LLVMValueRef arena_field_ptr(LLVMValueRef a, unsigned idx, const char *nm) {
    return LLVMBuildStructGEP2(lg.builder, baga_arena_ty(), a, idx, nm);
}

/* static int64_t baga_arena_new(void) — handle = pointer към baga_Arena */
LLVMValueRef llv_build_baga_arena_new(void) {
    LLVMValueRef fn = LLVMAddFunction(lg.mod, "baga_arena_new",
        LLVMFunctionType(lg.i64_ty, NULL, 0, 0));
    llv_h_begin(fn);
    LLVMValueRef sz = LLVMSizeOf(baga_arena_ty());
    LLVMValueRef ma[] = { sz };
    LLVMValueRef raw = llv_h_call(llv_rt_malloc(), ma, 1, "raw");
    LLVMValueRef a = LLVMBuildBitCast(lg.builder, raw, baga_arena_ptr_ty(), "a");
    LLVMBuildStore(lg.builder, LLVMConstInt(lg.i64_ty, 65536, 0),
                   arena_field_ptr(a, 2, "capp"));
    LLVMBuildStore(lg.builder, LLVMConstInt(lg.i64_ty, 0, 0),
                   arena_field_ptr(a, 1, "usedp"));
    LLVMValueRef da[] = { LLVMConstInt(lg.i64_ty, 65536, 0) };
    LLVMValueRef draw = llv_h_call(llv_rt_malloc(), da, 1, "draw");
    LLVMBuildStore(lg.builder, draw, arena_field_ptr(a, 0, "basep"));
    LLVMValueRef h = LLVMBuildPtrToInt(lg.builder, a, lg.i64_ty, "h");
    LLVMBuildRet(lg.builder, h);
    return fn;
}

/* static int64_t baga_arena_alloc(int64_t h, int64_t size) — bump + grow */
LLVMValueRef llv_build_baga_arena_alloc(void) {
    LLVMTypeRef p[] = { lg.i64_ty, lg.i64_ty };
    LLVMValueRef fn = LLVMAddFunction(lg.mod, "baga_arena_alloc",
        LLVMFunctionType(lg.i64_ty, p, 2, 0));
    llv_h_begin(fn);
    LLVMValueRef a = LLVMBuildIntToPtr(lg.builder, LLVMGetParam(fn, 0),
        baga_arena_ptr_ty(), "a");
    LLVMValueRef size = LLVMGetParam(fn, 1);
    LLVMValueRef usedp = arena_field_ptr(a, 1, "usedp");
    LLVMValueRef capp = arena_field_ptr(a, 2, "capp");
    LLVMValueRef used = LLVMBuildLoad2(lg.builder, lg.i64_ty, usedp, "used");
    LLVMValueRef cap = LLVMBuildLoad2(lg.builder, lg.i64_ty, capp, "cap");
    LLVMValueRef need = LLVMBuildAdd(lg.builder, used, size, "need");
    LLVMValueRef full = LLVMBuildICmp(lg.builder, LLVMIntSGT, need, cap, "full");
    LLVMBasicBlockRef grow_bb = LLVMAppendBasicBlockInContext(lg.ctx, fn, "grow");
    LLVMBasicBlockRef done_bb = LLVMAppendBasicBlockInContext(lg.ctx, fn, "done");
    LLVMBuildCondBr(lg.builder, full, grow_bb, done_bb);
    LLVMPositionBuilderAtEnd(lg.builder, grow_bb);
    LLVMValueRef nc = LLVMBuildMul(lg.builder, need,
        LLVMConstInt(lg.i64_ty, 2, 0), "nc");
    LLVMBuildStore(lg.builder, nc, capp);
    LLVMValueRef base0 = LLVMBuildLoad2(lg.builder, lg.ptr_ty,
        arena_field_ptr(a, 0, "basep"), "base0");
    LLVMValueRef ra[] = { base0, nc };
    LLVMValueRef nd = llv_h_call(llv_rt_realloc(), ra, 2, "nd");
    LLVMBuildStore(lg.builder, nd, arena_field_ptr(a, 0, "basep"));
    LLVMBuildBr(lg.builder, done_bb);
    LLVMPositionBuilderAtEnd(lg.builder, done_bb);
    LLVMValueRef base = LLVMBuildLoad2(lg.builder, lg.ptr_ty,
        arena_field_ptr(a, 0, "basep"), "base");
    LLVMValueRef ptr = LLVMBuildGEP2(lg.builder, lg.i8_ty, base, &used, 1, "p");
    LLVMBuildStore(lg.builder, need, usedp);
    LLVMValueRef r = LLVMBuildPtrToInt(lg.builder, ptr, lg.i64_ty, "r");
    LLVMBuildRet(lg.builder, r);
    return fn;
}

/* static void baga_arena_reset(int64_t h) — used = 0 */
LLVMValueRef llv_build_baga_arena_reset(void) {
    LLVMTypeRef p[] = { lg.i64_ty };
    LLVMValueRef fn = LLVMAddFunction(lg.mod, "baga_arena_reset",
        LLVMFunctionType(lg.void_ty, p, 1, 0));
    llv_h_begin(fn);
    LLVMValueRef a = LLVMBuildIntToPtr(lg.builder, LLVMGetParam(fn, 0),
        baga_arena_ptr_ty(), "a");
    LLVMBuildStore(lg.builder, LLVMConstInt(lg.i64_ty, 0, 0),
                   arena_field_ptr(a, 1, "usedp"));
    LLVMBuildRetVoid(lg.builder);
    return fn;
}

/* static void baga_arena_free(int64_t h) — free(base); free(a) */
LLVMValueRef llv_build_baga_arena_free(void) {
    LLVMTypeRef p[] = { lg.i64_ty };
    LLVMValueRef fn = LLVMAddFunction(lg.mod, "baga_arena_free",
        LLVMFunctionType(lg.void_ty, p, 1, 0));
    llv_h_begin(fn);
    LLVMValueRef a = LLVMBuildIntToPtr(lg.builder, LLVMGetParam(fn, 0),
        baga_arena_ptr_ty(), "a");
    LLVMValueRef base = LLVMBuildLoad2(lg.builder, lg.ptr_ty,
        arena_field_ptr(a, 0, "basep"), "base");
    LLVMValueRef fa[] = { base };
    llv_h_call(llv_rt_free(), fa, 1, "");
    LLVMValueRef raw = LLVMBuildBitCast(lg.builder, a, lg.ptr_ty, "raw");
    LLVMValueRef fb[] = { raw };
    llv_h_call(llv_rt_free(), fb, 1, "");
    LLVMBuildRetVoid(lg.builder);
    return fn;
}

/* f64 елементи: double ↔ i64 ↔ ptr (bitcast/inttoptr/ptrtoint) */
LLVMValueRef llv_build_baga_vec_push_f64(void) {
    LLVMTypeRef p[] = { llv_baga_vec_ptr_ty(), lg.double_ty };
    LLVMValueRef fn = LLVMAddFunction(lg.mod, "baga_vec_push_f64",
        LLVMFunctionType(lg.void_ty, p, 2, 0));
    llv_h_begin(fn);
    LLVMValueRef slot = vec_push_slot(LLVMGetParam(fn, 0));
    LLVMValueRef xi = LLVMBuildBitCast(lg.builder, LLVMGetParam(fn, 1), lg.i64_ty, "xi");
    LLVMValueRef pv = LLVMBuildIntToPtr(lg.builder, xi, lg.ptr_ty, "pv");
    LLVMBuildStore(lg.builder, pv, slot);
    LLVMBuildRetVoid(lg.builder);
    return fn;
}

LLVMValueRef llv_build_baga_vec_get_f64(void) {
    LLVMTypeRef p[] = { llv_baga_vec_ptr_ty(), lg.i64_ty };
    LLVMValueRef fn = LLVMAddFunction(lg.mod, "baga_vec_get_f64",
        LLVMFunctionType(lg.double_ty, p, 2, 0));
    llv_h_begin(fn);
    LLVMValueRef e = llv_vec_load_at(LLVMGetParam(fn, 0), LLVMGetParam(fn, 1));
    LLVMValueRef ei = LLVMBuildPtrToInt(lg.builder, e, lg.i64_ty, "ei");
    LLVMBuildRet(lg.builder, LLVMBuildBitCast(lg.builder, ei, lg.double_ty, "r"));
    return fn;
}

LLVMValueRef llv_build_baga_vec_set_f64(void) {
    LLVMTypeRef p[] = { llv_baga_vec_ptr_ty(), lg.i64_ty, lg.double_ty };
    LLVMValueRef fn = LLVMAddFunction(lg.mod, "baga_vec_set_f64",
        LLVMFunctionType(lg.void_ty, p, 3, 0));
    llv_h_begin(fn);
    LLVMValueRef v = LLVMGetParam(fn, 0);
    LLVMValueRef i = LLVMGetParam(fn, 1);
    LLVMValueRef data = llv_vec_load_data(v);
    LLVMValueRef slot = LLVMBuildGEP2(lg.builder, lg.ptr_ty, data, &i, 1, "slot");
    LLVMValueRef xi = LLVMBuildBitCast(lg.builder, LLVMGetParam(fn, 2), lg.i64_ty, "xi");
    LLVMValueRef pv = LLVMBuildIntToPtr(lg.builder, xi, lg.ptr_ty, "pv");
    LLVMBuildStore(lg.builder, pv, slot);
    LLVMBuildRetVoid(lg.builder);
    return fn;
}

/* ---- bytes: baga_bytes = { i8* data, i64 len }, passed by value ---- */

/* store a by-value bytes param into a fresh alloca so its fields are GEP-able */
LLVMValueRef llv_bytes_param_alloca(LLVMValueRef v) {
    LLVMValueRef a = llv_entry_alloca(llv_baga_bytes_ty(), "ba");
    LLVMBuildStore(lg.builder, v, a);
    return a;
}
LLVMValueRef llv_bytes_load_data(LLVMValueRef a) {
    return LLVMBuildLoad2(lg.builder, lg.ptr_ty,
        LLVMBuildStructGEP2(lg.builder, llv_baga_bytes_ty(), a, 0, "bd"), "bdata");
}
LLVMValueRef llv_bytes_load_len(LLVMValueRef a) {
    return LLVMBuildLoad2(lg.builder, lg.i64_ty,
        LLVMBuildStructGEP2(lg.builder, llv_baga_bytes_ty(), a, 1, "bl"), "blen");
}
/* assemble a baga_bytes value from data ptr + len */
static LLVMValueRef bytes_make(LLVMValueRef data, LLVMValueRef len) {
    LLVMValueRef u = LLVMGetUndef(llv_baga_bytes_ty());
    u = LLVMBuildInsertValue(lg.builder, u, data, 0, "b0");
    u = LLVMBuildInsertValue(lg.builder, u, len, 1, "b1");
    return u;
}
/* malloc(max(n,1)) — matches the C helper's non-empty allocation */
static LLVMValueRef bytes_malloc_n(LLVMValueRef n) {
    LLVMValueRef one = LLVMConstInt(lg.i64_ty, 1, 0);
    LLVMValueRef cmp = LLVMBuildICmp(lg.builder, LLVMIntSGT, n, one, "nn");
    LLVMValueRef sz = LLVMBuildSelect(lg.builder, cmp, n, one, "sz");
    return llv_rc_alloc_call(sz, "bdata");
}

LLVMValueRef llv_build_baga_bytes_len(void) {
    LLVMTypeRef p[] = { llv_baga_bytes_ty() };
    LLVMValueRef fn = LLVMAddFunction(lg.mod, "baga_bytes_len",
        LLVMFunctionType(lg.i64_ty, p, 1, 0));
    llv_h_begin(fn);
    LLVMValueRef a = llv_bytes_param_alloca(LLVMGetParam(fn, 0));
    LLVMBuildRet(lg.builder, llv_bytes_load_len(a));
    return fn;
}

LLVMValueRef llv_build_baga_bytes_at(void) {
    LLVMTypeRef p[] = { llv_baga_bytes_ty(), lg.i64_ty };
    LLVMValueRef fn = LLVMAddFunction(lg.mod, "baga_bytes_at",
        LLVMFunctionType(lg.i64_ty, p, 2, 0));
    llv_h_begin(fn);
    LLVMValueRef a = llv_bytes_param_alloca(LLVMGetParam(fn, 0));
    LLVMValueRef i = LLVMGetParam(fn, 1);
    LLVMValueRef data = llv_bytes_load_data(a);
    LLVMValueRef slot = LLVMBuildGEP2(lg.builder, lg.i8_ty, data, &i, 1, "bs");
    LLVMValueRef byte = LLVMBuildLoad2(lg.builder, lg.i8_ty, slot, "byte");
    LLVMBuildRet(lg.builder, LLVMBuildZExt(lg.builder, byte, lg.i64_ty, "bv"));
    return fn;
}

LLVMValueRef llv_build_baga_bytes_slice(void) {
    LLVMTypeRef p[] = { llv_baga_bytes_ty(), lg.i64_ty, lg.i64_ty };
    LLVMValueRef fn = LLVMAddFunction(lg.mod, "baga_bytes_slice",
        LLVMFunctionType(llv_baga_bytes_ty(), p, 3, 0));
    llv_h_begin(fn);
    LLVMValueRef a = llv_bytes_param_alloca(LLVMGetParam(fn, 0));
    LLVMValueRef lo = LLVMGetParam(fn, 1);
    LLVMValueRef hi = LLVMGetParam(fn, 2);
    LLVMValueRef blen = llv_bytes_load_len(a);
    LLVMValueRef z = LLVMConstInt(lg.i64_ty, 0, 0);
    LLVMValueRef c1 = LLVMBuildICmp(lg.builder, LLVMIntSLT, lo, z, "lo0");
    lo = LLVMBuildSelect(lg.builder, c1, z, lo, "lo");
    LLVMValueRef c2 = LLVMBuildICmp(lg.builder, LLVMIntSGT, hi, blen, "hi0");
    hi = LLVMBuildSelect(lg.builder, c2, blen, hi, "hi");
    LLVMValueRef c3 = LLVMBuildICmp(lg.builder, LLVMIntSLT, hi, lo, "hc");
    hi = LLVMBuildSelect(lg.builder, c3, lo, hi, "hi");
    LLVMValueRef n = LLVMBuildSub(lg.builder, hi, lo, "sn");
    LLVMValueRef data = llv_bytes_load_data(a);
    LLVMValueRef src = LLVMBuildGEP2(lg.builder, lg.i8_ty, data, &lo, 1, "src");
    LLVMValueRef dst = bytes_malloc_n(n);
    LLVMValueRef mc[] = { dst, src, n };
    llv_h_call(llv_rt_memcpy(), mc, 3, "");
    LLVMBuildRet(lg.builder, bytes_make(dst, n));
    return fn;
}

LLVMValueRef llv_build_baga_bytes_concat(void) {
    LLVMTypeRef p[] = { llv_baga_bytes_ty(), llv_baga_bytes_ty() };
    LLVMValueRef fn = LLVMAddFunction(lg.mod, "baga_bytes_concat",
        LLVMFunctionType(llv_baga_bytes_ty(), p, 2, 0));
    llv_h_begin(fn);
    LLVMValueRef a = llv_bytes_param_alloca(LLVMGetParam(fn, 0));
    LLVMValueRef b = llv_bytes_param_alloca(LLVMGetParam(fn, 1));
    LLVMValueRef al = llv_bytes_load_len(a);
    LLVMValueRef bl = llv_bytes_load_len(b);
    LLVMValueRef n = LLVMBuildAdd(lg.builder, al, bl, "cn");
    LLVMValueRef dst = bytes_malloc_n(n);
    LLVMValueRef ad = llv_bytes_load_data(a);
    LLVMValueRef bd = llv_bytes_load_data(b);
    LLVMValueRef m1[] = { dst, ad, al };
    llv_h_call(llv_rt_memcpy(), m1, 3, "");
    LLVMValueRef off = LLVMBuildGEP2(lg.builder, lg.i8_ty, dst, &al, 1, "off");
    LLVMValueRef m2[] = { off, bd, bl };
    llv_h_call(llv_rt_memcpy(), m2, 3, "");
    LLVMBuildRet(lg.builder, bytes_make(dst, n));
    return fn;
}

/* R51 parity (C backend): str <-> i64 unsafe handle casts — zero-copy
 * handoff through i64 chans. Safe because str memory is never freed. */
LLVMValueRef llv_build_baga_str_h(void) {
    LLVMTypeRef p[] = { lg.ptr_ty };
    LLVMValueRef fn = LLVMAddFunction(lg.mod, "baga_str_h",
        LLVMFunctionType(lg.i64_ty, p, 1, 0));
    llv_h_begin(fn);
    LLVMBuildRet(lg.builder,
        LLVMBuildPtrToInt(lg.builder, LLVMGetParam(fn, 0), lg.i64_ty, "h"));
    return fn;
}
LLVMValueRef llv_build_baga_h_str(void) {
    LLVMTypeRef p[] = { lg.i64_ty };
    LLVMValueRef fn = LLVMAddFunction(lg.mod, "baga_h_str",
        LLVMFunctionType(lg.ptr_ty, p, 1, 0));
    llv_h_begin(fn);
    LLVMBuildRet(lg.builder,
        LLVMBuildIntToPtr(lg.builder, LLVMGetParam(fn, 0), lg.ptr_ty, "s"));
    return fn;
}
/* R54 parity: dst[off..off+src.len) = src, bounds-checked no-op on overflow */
LLVMValueRef llv_build_baga_bytes_put(void) {
    LLVMTypeRef p[] = { llv_baga_bytes_ty(), lg.i64_ty, llv_baga_bytes_ty() };
    LLVMValueRef fn = LLVMAddFunction(lg.mod, "baga_bytes_put",
        LLVMFunctionType(lg.void_ty, p, 3, 0));
    llv_h_begin(fn);
    LLVMValueRef d = llv_bytes_param_alloca(LLVMGetParam(fn, 0));
    LLVMValueRef off = LLVMGetParam(fn, 1);
    LLVMValueRef s = llv_bytes_param_alloca(LLVMGetParam(fn, 2));
    LLVMValueRef dl = llv_bytes_load_len(d);
    LLVMValueRef sl = llv_bytes_load_len(s);
    LLVMValueRef neg = LLVMBuildICmp(lg.builder, LLVMIntSLT, off,
        LLVMConstInt(lg.i64_ty, 0, 0), "neg");
    LLVMValueRef sum = LLVMBuildAdd(lg.builder, off, sl, "sum");
    LLVMValueRef over = LLVMBuildICmp(lg.builder, LLVMIntSGT, sum, dl, "over");
    LLVMValueRef cnd = LLVMBuildOr(lg.builder, neg, over, "cnd");
    LLVMBasicBlockRef bad = LLVMAppendBasicBlock(fn, "bad");
    LLVMBasicBlockRef okb = LLVMAppendBasicBlock(fn, "ok");
    LLVMBuildCondBr(lg.builder, cnd, bad, okb);
    LLVMPositionBuilderAtEnd(lg.builder, bad);
    LLVMBuildRetVoid(lg.builder);
    LLVMPositionBuilderAtEnd(lg.builder, okb);
    LLVMValueRef dd = llv_bytes_load_data(d);
    LLVMValueRef sd = llv_bytes_load_data(s);
    LLVMValueRef dst = LLVMBuildGEP2(lg.builder, lg.i8_ty, dd, &off, 1, "dst");
    LLVMValueRef m[] = { dst, sd, sl };
    llv_h_call(llv_rt_memcpy(), m, 3, "");
    LLVMBuildRetVoid(lg.builder);
    return fn;
}

/* R66 parity (C backend): bytes <-> i64 unsafe handle casts — zero-copy
 * hop through i64 chans. Boxes the {data,len} header with malloc; payload
 * data stays put. Leak-tolerant like the arena (handles live for the
 * process). */
LLVMValueRef llv_build_baga_bytes_h(void) {
    LLVMTypeRef p[] = { llv_baga_bytes_ty() };
    LLVMValueRef fn = LLVMAddFunction(lg.mod, "baga_bytes_h",
        LLVMFunctionType(lg.i64_ty, p, 1, 0));
    llv_h_begin(fn);
    LLVMValueRef a = llv_bytes_param_alloca(LLVMGetParam(fn, 0));
    LLVMValueRef sz[] = { LLVMConstInt(lg.i64_ty, 16, 0) };
    LLVMValueRef box = llv_h_call(llv_rt_malloc(), sz, 1, "box");
    LLVMValueRef bp = LLVMBuildBitCast(lg.builder, box,
        LLVMPointerType(llv_baga_bytes_ty(), 0), "bp");
    LLVMBuildStore(lg.builder, llv_bytes_load_data(a),
        LLVMBuildStructGEP2(lg.builder, llv_baga_bytes_ty(), bp, 0, "bd"));
    LLVMBuildStore(lg.builder, llv_bytes_load_len(a),
        LLVMBuildStructGEP2(lg.builder, llv_baga_bytes_ty(), bp, 1, "bl"));
    LLVMBuildRet(lg.builder,
        LLVMBuildPtrToInt(lg.builder, box, lg.i64_ty, "h"));
    return fn;
}
LLVMValueRef llv_build_baga_h_bytes(void) {
    LLVMTypeRef p[] = { lg.i64_ty };
    LLVMValueRef fn = LLVMAddFunction(lg.mod, "baga_h_bytes",
        LLVMFunctionType(llv_baga_bytes_ty(), p, 1, 0));
    llv_h_begin(fn);
    LLVMValueRef h = LLVMGetParam(fn, 0);
    LLVMValueRef isz = LLVMBuildICmp(lg.builder, LLVMIntEQ, h,
        LLVMConstInt(lg.i64_ty, 0, 0), "isz");
    LLVMBasicBlockRef zero_bb = LLVMAppendBasicBlockInContext(lg.ctx, fn, "zero");
    LLVMBasicBlockRef box_bb  = LLVMAppendBasicBlockInContext(lg.ctx, fn, "box");
    LLVMBuildCondBr(lg.builder, isz, zero_bb, box_bb);
    LLVMPositionBuilderAtEnd(lg.builder, zero_bb);
    LLVMBuildRet(lg.builder, bytes_make(LLVMConstNull(lg.ptr_ty),
        LLVMConstInt(lg.i64_ty, 0, 0)));
    LLVMPositionBuilderAtEnd(lg.builder, box_bb);
    LLVMValueRef bp = LLVMBuildIntToPtr(lg.builder, h,
        LLVMPointerType(llv_baga_bytes_ty(), 0), "bp");
    LLVMBuildRet(lg.builder,
        LLVMBuildLoad2(lg.builder, llv_baga_bytes_ty(), bp, "v"));
    return fn;
}

/* static void baga_bounds_fail(const char *fn, int64_t i, int64_t len)
 * — същото съобщение като C бекенда + exit(1) */
LLVMValueRef llv_build_baga_bounds_fail(void) {
    LLVMTypeRef p[] = { lg.ptr_ty, lg.i64_ty, lg.i64_ty };
    LLVMValueRef fn = LLVMAddFunction(lg.mod, "baga_bounds_fail",
        LLVMFunctionType(lg.void_ty, p, 3, 0));
    llv_h_begin(fn);
    LLVMValueRef err = LLVMBuildLoad2(lg.builder, lg.ptr_ty, lg.stderr_global, "err");
    LLVMValueRef fmt = LLVMBuildGlobalStringPtr(lg.builder,
        "baga: %s: индекс %lld извън границите [0, %lld)\n", "bfmt");
    LLVMValueRef args[] = { err, fmt, LLVMGetParam(fn, 0),
                            LLVMGetParam(fn, 1), LLVMGetParam(fn, 2) };
    LLVMBuildCall2(lg.builder, LLVMGlobalGetValueType(lg.fprintf_fn),
                   lg.fprintf_fn, args, 5, "");
    LLVMTypeRef ep[] = { lg.i32_ty };
    LLVMValueRef ea[] = { LLVMConstInt(lg.i32_ty, 1, 0) };
    llv_h_call(llv_rt_libc("exit", lg.void_ty, ep, 1), ea, 1, "");
    LLVMBuildUnreachable(lg.builder);
    return fn;
}

/* S2 parity: bytes_new/bytes_set/bytes_push — мутаторите от C бекенда.
 * bytes_set е bounds-checked и прекъсва със същото съобщение. */
LLVMValueRef llv_build_baga_bytes_new(void) {
    LLVMTypeRef p[] = { lg.i64_ty };
    LLVMValueRef fn = LLVMAddFunction(lg.mod, "baga_bytes_new",
        LLVMFunctionType(llv_baga_bytes_ty(), p, 1, 0));
    llv_h_begin(fn);
    LLVMValueRef n = LLVMGetParam(fn, 0);
    LLVMValueRef neg = LLVMBuildICmp(lg.builder, LLVMIntSLT, n,
        LLVMConstInt(lg.i64_ty, 0, 0), "neg");
    n = LLVMBuildSelect(lg.builder, neg, LLVMConstInt(lg.i64_ty, 0, 0), n, "nn");
    LLVMValueRef dst = bytes_malloc_n(n);
    LLVMTypeRef mt[] = { lg.ptr_ty, lg.i32_ty, lg.i64_ty };
    LLVMValueRef ms[] = { dst, LLVMConstInt(lg.i32_ty, 0, 0), n };
    llv_h_call(llv_rt_libc("memset", lg.ptr_ty, mt, 3), ms, 3, "");
    LLVMBuildRet(lg.builder, bytes_make(dst, n));
    return fn;
}
LLVMValueRef llv_build_baga_bytes_set(void) {
    LLVMTypeRef p[] = { llv_baga_bytes_ty(), lg.i64_ty, lg.i64_ty };
    LLVMValueRef fn = LLVMAddFunction(lg.mod, "baga_bytes_set",
        LLVMFunctionType(lg.void_ty, p, 3, 0));
    llv_h_begin(fn);
    LLVMValueRef a = llv_bytes_param_alloca(LLVMGetParam(fn, 0));
    LLVMValueRef i = LLVMGetParam(fn, 1);
    LLVMValueRef len = llv_bytes_load_len(a);
    LLVMValueRef lo = LLVMBuildICmp(lg.builder, LLVMIntSLT, i,
        LLVMConstInt(lg.i64_ty, 0, 0), "lo");
    LLVMValueRef hi = LLVMBuildICmp(lg.builder, LLVMIntSGE, i, len, "hi");
    LLVMValueRef bad = LLVMBuildOr(lg.builder, lo, hi, "bad");
    LLVMBasicBlockRef fail = LLVMAppendBasicBlockInContext(lg.ctx, fn, "fail");
    LLVMBasicBlockRef okb  = LLVMAppendBasicBlockInContext(lg.ctx, fn, "ok");
    LLVMBuildCondBr(lg.builder, bad, fail, okb);
    LLVMPositionBuilderAtEnd(lg.builder, fail);
    LLVMValueRef fname = LLVMBuildGlobalStringPtr(lg.builder, "bytes_set", "bfn");
    LLVMValueRef fa[] = { fname, i, len };
    llv_h_call(llv_baga_rt("baga_bounds_fail"), fa, 3, "");
    LLVMBuildUnreachable(lg.builder);
    LLVMPositionBuilderAtEnd(lg.builder, okb);
    LLVMValueRef dst = LLVMBuildGEP2(lg.builder, lg.i8_ty,
        llv_bytes_load_data(a), &i, 1, "dst");
    LLVMValueRef v8 = LLVMBuildTrunc(lg.builder,
        LLVMBuildAnd(lg.builder, LLVMGetParam(fn, 2),
                     LLVMConstInt(lg.i64_ty, 255, 0), "m"),
        lg.i8_ty, "v8");
    LLVMBuildStore(lg.builder, v8, dst);
    LLVMBuildRetVoid(lg.builder);
    return fn;
}
LLVMValueRef llv_build_baga_bytes_push(void) {
    LLVMTypeRef p[] = { llv_baga_bytes_ty(), lg.i64_ty };
    LLVMValueRef fn = LLVMAddFunction(lg.mod, "baga_bytes_push",
        LLVMFunctionType(llv_baga_bytes_ty(), p, 2, 0));
    llv_h_begin(fn);
    LLVMValueRef a = llv_bytes_param_alloca(LLVMGetParam(fn, 0));
    LLVMValueRef len = llv_bytes_load_len(a);
    LLVMValueRef nl = LLVMBuildAdd(lg.builder, len, LLVMConstInt(lg.i64_ty, 1, 0), "nl");
    LLVMValueRef dst = bytes_malloc_n(nl);
    LLVMValueRef mc[] = { dst, llv_bytes_load_data(a), len };
    llv_h_call(llv_rt_memcpy(), mc, 3, "");
    LLVMValueRef slot = LLVMBuildGEP2(lg.builder, lg.i8_ty, dst, &len, 1, "slot");
    LLVMValueRef v8 = LLVMBuildTrunc(lg.builder,
        LLVMBuildAnd(lg.builder, LLVMGetParam(fn, 1),
                     LLVMConstInt(lg.i64_ty, 255, 0), "m"),
        lg.i8_ty, "v8");
    LLVMBuildStore(lg.builder, v8, slot);
    LLVMBuildRet(lg.builder, bytes_make(dst, nl));
    return fn;
}

LLVMValueRef llv_build_baga_bytes_from_str(void) {
    LLVMTypeRef p[] = { lg.ptr_ty };
    LLVMValueRef fn = LLVMAddFunction(lg.mod, "baga_bytes_from_str",
        LLVMFunctionType(llv_baga_bytes_ty(), p, 1, 0));
    llv_h_begin(fn);
    LLVMValueRef s = LLVMGetParam(fn, 0);
    LLVMValueRef la[] = { s };
    LLVMValueRef n = llv_h_call(llv_baga_rt("baga_str_len"), la, 1, "n");
    LLVMValueRef dst = bytes_malloc_n(n);
    LLVMValueRef mc[] = { dst, s, n };
    llv_h_call(llv_rt_memcpy(), mc, 3, "");
    LLVMBuildRet(lg.builder, bytes_make(dst, n));
    return fn;
}

LLVMValueRef llv_build_baga_bytes_to_str(void) {
    LLVMTypeRef p[] = { llv_baga_bytes_ty() };
    LLVMValueRef fn = LLVMAddFunction(lg.mod, "baga_bytes_to_str",
        LLVMFunctionType(lg.ptr_ty, p, 1, 0));
    llv_h_begin(fn);
    LLVMValueRef a = llv_bytes_param_alloca(LLVMGetParam(fn, 0));
    LLVMValueRef n = llv_bytes_load_len(a);
    LLVMValueRef one = LLVMConstInt(lg.i64_ty, 1, 0);
    LLVMValueRef sz = LLVMBuildAdd(lg.builder, n, one, "sz");
    LLVMValueRef r = llv_str_alloc_call(sz, "r");
    LLVMValueRef data = llv_bytes_load_data(a);
    LLVMValueRef mc[] = { r, data, n };
    llv_h_call(llv_rt_memcpy(), mc, 3, "");
    LLVMValueRef ep = LLVMBuildGEP2(lg.builder, lg.i8_ty, r, &n, 1, "ep");
    LLVMBuildStore(lg.builder, LLVMConstInt(lg.i8_ty, 0, 0), ep);
    /* NUL-safe: slen == strlen(r), не n (двоичен вход може да има NUL) */
    LLVMValueRef sla[] = { r };
    LLVMValueRef sl = llv_h_call(llv_rt_strlen(), sla, 1, "sl");
    LLVMValueRef ss[] = { r, sl };
    llv_h_call(llv_baga_rt("baga_set_slen"), ss, 2, "");
    LLVMBuildRet(lg.builder, r);
    return fn;
}

LLVMValueRef llv_build_baga_hex_val(void) {
    LLVMTypeRef p[] = { lg.i64_ty };
    LLVMValueRef fn = LLVMAddFunction(lg.mod, "baga_hex_val",
        LLVMFunctionType(lg.i64_ty, p, 1, 0));
    llv_h_begin(fn);
    LLVMValueRef c = LLVMGetParam(fn, 0);
    LLVMValueRef neg1 = LLVMConstInt(lg.i64_ty, (uint64_t)-1, 1);
    LLVMValueRef r = llv_entry_alloca(lg.i64_ty, "hv");
    LLVMBuildStore(lg.builder, neg1, r);
    LLVMValueRef c0 = LLVMConstInt(lg.i64_ty, 48, 0), c9 = LLVMConstInt(lg.i64_ty, 57, 0);
    LLVMValueRef ca = LLVMConstInt(lg.i64_ty, 97, 0), cf = LLVMConstInt(lg.i64_ty, 102, 0);
    LLVMValueRef cA = LLVMConstInt(lg.i64_ty, 65, 0), cF = LLVMConstInt(lg.i64_ty, 70, 0);
    LLVMValueRef ten = LLVMConstInt(lg.i64_ty, 10, 0);
    LLVMValueRef d1 = LLVMBuildSub(lg.builder, c, c0, "d1");
    LLVMValueRef in1a = LLVMBuildICmp(lg.builder, LLVMIntSGE, c, c0, "i1a");
    LLVMValueRef in1b = LLVMBuildICmp(lg.builder, LLVMIntSLE, c, c9, "i1b");
    LLVMValueRef in1 = LLVMBuildAnd(lg.builder, in1a, in1b, "i1");
    LLVMBasicBlockRef b1 = LLVMAppendBasicBlockInContext(lg.ctx, fn, "d");
    LLVMBasicBlockRef b2 = LLVMAppendBasicBlockInContext(lg.ctx, fn, "af");
    LLVMBuildCondBr(lg.builder, in1, b1, b2);
    LLVMPositionBuilderAtEnd(lg.builder, b1);
    LLVMBuildStore(lg.builder, d1, r);
    LLVMBuildRet(lg.builder, LLVMBuildLoad2(lg.builder, lg.i64_ty, r, "rv"));
    LLVMPositionBuilderAtEnd(lg.builder, b2);
    LLVMValueRef in2a = LLVMBuildICmp(lg.builder, LLVMIntSGE, c, ca, "i2a");
    LLVMValueRef in2b = LLVMBuildICmp(lg.builder, LLVMIntSLE, c, cf, "i2b");
    LLVMValueRef in2 = LLVMBuildAnd(lg.builder, in2a, in2b, "i2");
    LLVMBasicBlockRef b3 = LLVMAppendBasicBlockInContext(lg.ctx, fn, "lo");
    LLVMBasicBlockRef b4 = LLVMAppendBasicBlockInContext(lg.ctx, fn, "AF");
    LLVMBuildCondBr(lg.builder, in2, b3, b4);
    LLVMPositionBuilderAtEnd(lg.builder, b3);
    LLVMBuildStore(lg.builder, LLVMBuildAdd(lg.builder, LLVMBuildSub(lg.builder, c, ca, "d2"), ten, "v2"), r);
    LLVMBuildRet(lg.builder, LLVMBuildLoad2(lg.builder, lg.i64_ty, r, "rv2"));
    LLVMPositionBuilderAtEnd(lg.builder, b4);
    LLVMValueRef in3a = LLVMBuildICmp(lg.builder, LLVMIntSGE, c, cA, "i3a");
    LLVMValueRef in3b = LLVMBuildICmp(lg.builder, LLVMIntSLE, c, cF, "i3b");
    LLVMValueRef in3 = LLVMBuildAnd(lg.builder, in3a, in3b, "i3");
    LLVMBasicBlockRef b5 = LLVMAppendBasicBlockInContext(lg.ctx, fn, "up");
    LLVMBasicBlockRef b6 = LLVMAppendBasicBlockInContext(lg.ctx, fn, "no");
    LLVMBuildCondBr(lg.builder, in3, b5, b6);
    LLVMPositionBuilderAtEnd(lg.builder, b5);
    LLVMBuildStore(lg.builder, LLVMBuildAdd(lg.builder, LLVMBuildSub(lg.builder, c, cA, "d3"), ten, "v3"), r);
    LLVMBuildRet(lg.builder, LLVMBuildLoad2(lg.builder, lg.i64_ty, r, "rv3"));
    LLVMPositionBuilderAtEnd(lg.builder, b6);
    LLVMBuildRet(lg.builder, neg1);
    return fn;
}

static LLVMValueRef bytes_hex_global(void) {
    LLVMValueRef g = LLVMGetNamedGlobal(lg.mod, "baga_hexchars");
    if (g) return g;
    g = LLVMAddGlobal(lg.mod, LLVMArrayType(lg.i8_ty, 16), "baga_hexchars");
    LLVMSetInitializer(g, LLVMConstStringInContext(lg.ctx, "0123456789abcdef", 16, 1));
    LLVMSetGlobalConstant(g, 1);
    return g;
}

LLVMValueRef llv_build_baga_hex_encode(void) {
    LLVMTypeRef p[] = { llv_baga_bytes_ty() };
    LLVMValueRef fn = LLVMAddFunction(lg.mod, "baga_hex_encode",
        LLVMFunctionType(lg.ptr_ty, p, 1, 0));
    llv_h_begin(fn);
    LLVMValueRef a = llv_bytes_param_alloca(LLVMGetParam(fn, 0));
    LLVMValueRef n = llv_bytes_load_len(a);
    LLVMValueRef two = LLVMConstInt(lg.i64_ty, 2, 0);
    LLVMValueRef one = LLVMConstInt(lg.i64_ty, 1, 0);
    LLVMValueRef four = LLVMConstInt(lg.i64_ty, 4, 0);
    LLVMValueRef fifteen = LLVMConstInt(lg.i64_ty, 15, 0);
    LLVMValueRef sz = LLVMBuildAdd(lg.builder, LLVMBuildMul(lg.builder, n, two, "s0"), one, "sz");
    LLVMValueRef r = llv_str_alloc_call(sz, "r");
    LLVMValueRef hxg = bytes_hex_global();
    LLVMValueRef zidx = LLVMConstInt(lg.i64_ty, 0, 0);
    LLVMValueRef hxarr = LLVMBuildGEP2(lg.builder, LLVMArrayType(lg.i8_ty, 16), hxg,
        (LLVMValueRef[]){ zidx, zidx }, 2, "hxa");
    LLVMValueRef hx = LLVMBuildBitCast(lg.builder, hxarr, lg.ptr_ty, "hx");
    LLVMValueRef data = llv_bytes_load_data(a);
    LLVMBasicBlockRef cond = LLVMAppendBasicBlockInContext(lg.ctx, fn, "cond");
    LLVMBasicBlockRef body = LLVMAppendBasicBlockInContext(lg.ctx, fn, "body");
    LLVMBasicBlockRef done = LLVMAppendBasicBlockInContext(lg.ctx, fn, "done");
    LLVMValueRef iv = llv_entry_alloca(lg.i64_ty, "i");
    LLVMBuildStore(lg.builder, LLVMConstInt(lg.i64_ty, 0, 0), iv);
    LLVMBuildBr(lg.builder, cond);
    LLVMPositionBuilderAtEnd(lg.builder, cond);
    LLVMValueRef i = LLVMBuildLoad2(lg.builder, lg.i64_ty, iv, "i");
    LLVMValueRef cc = LLVMBuildICmp(lg.builder, LLVMIntSLT, i, n, "cc");
    LLVMBuildCondBr(lg.builder, cc, body, done);
    LLVMPositionBuilderAtEnd(lg.builder, body);
    LLVMValueRef bp = LLVMBuildGEP2(lg.builder, lg.i8_ty, data, &i, 1, "bp");
    LLVMValueRef byte = LLVMBuildZExt(lg.builder, LLVMBuildLoad2(lg.builder, lg.i8_ty, bp, "b"), lg.i64_ty, "bv");
    LLVMValueRef hi = LLVMBuildLShr(lg.builder, byte, four, "hi");
    LLVMValueRef lo = LLVMBuildAnd(lg.builder, byte, fifteen, "lo");
    LLVMValueRef i2 = LLVMBuildMul(lg.builder, i, two, "i2");
    LLVMValueRef hch = LLVMBuildLoad2(lg.builder, lg.i8_ty, LLVMBuildGEP2(lg.builder, lg.i8_ty, hx, &hi, 1, "hp"), "hch");
    LLVMBuildStore(lg.builder, hch, LLVMBuildGEP2(lg.builder, lg.i8_ty, r, &i2, 1, "r0"));
    LLVMValueRef i21 = LLVMBuildAdd(lg.builder, i2, one, "i21");
    LLVMValueRef lch = LLVMBuildLoad2(lg.builder, lg.i8_ty, LLVMBuildGEP2(lg.builder, lg.i8_ty, hx, &lo, 1, "lp"), "lch");
    LLVMBuildStore(lg.builder, lch, LLVMBuildGEP2(lg.builder, lg.i8_ty, r, &i21, 1, "r1"));
    LLVMBuildStore(lg.builder, LLVMBuildAdd(lg.builder, i, one, "in"), iv);
    LLVMBuildBr(lg.builder, cond);
    LLVMPositionBuilderAtEnd(lg.builder, done);
    LLVMValueRef endi = LLVMBuildMul(lg.builder, n, two, "endi");
    LLVMBuildStore(lg.builder, LLVMConstInt(lg.i8_ty, 0, 0), LLVMBuildGEP2(lg.builder, lg.i8_ty, r, &endi, 1, "rp"));
    LLVMValueRef ss[] = { r, endi };
    llv_h_call(llv_baga_rt("baga_set_slen"), ss, 2, "");
    LLVMBuildRet(lg.builder, r);
    return fn;
}

LLVMValueRef llv_build_baga_hex_decode(void) {
    LLVMTypeRef p[] = { lg.ptr_ty };
    LLVMValueRef fn = LLVMAddFunction(lg.mod, "baga_hex_decode",
        LLVMFunctionType(llv_baga_bytes_ty(), p, 1, 0));
    llv_h_begin(fn);
    LLVMValueRef s = LLVMGetParam(fn, 0);
    LLVMValueRef one = LLVMConstInt(lg.i64_ty, 1, 0);
    LLVMValueRef two = LLVMConstInt(lg.i64_ty, 2, 0);
    LLVMValueRef sixteen = LLVMConstInt(lg.i64_ty, 16, 0);
    LLVMValueRef zero = LLVMConstInt(lg.i64_ty, 0, 0);
    LLVMValueRef neg1 = LLVMConstInt(lg.i64_ty, (uint64_t)-1, 1);
    LLVMValueRef sla[] = { s };
    LLVMValueRef n = llv_h_call(llv_baga_rt("baga_str_len"), sla, 1, "n");
    LLVMValueRef cap = LLVMBuildAdd(lg.builder, LLVMBuildUDiv(lg.builder, n, two, "c0"), one, "cap");
    LLVMValueRef buf = llv_rc_alloc_call(cap, "buf");
    LLVMValueRef len = llv_entry_alloca(lg.i64_ty, "len");
    LLVMValueRef iv = llv_entry_alloca(lg.i64_ty, "i");
    LLVMBuildStore(lg.builder, zero, len);
    LLVMBuildStore(lg.builder, zero, iv);
    LLVMBasicBlockRef cond = LLVMAppendBasicBlockInContext(lg.ctx, fn, "cond");
    LLVMBasicBlockRef body = LLVMAppendBasicBlockInContext(lg.ctx, fn, "body");
    LLVMBasicBlockRef done = LLVMAppendBasicBlockInContext(lg.ctx, fn, "done");
    LLVMBuildBr(lg.builder, cond);
    LLVMPositionBuilderAtEnd(lg.builder, cond);
    LLVMValueRef i = LLVMBuildLoad2(lg.builder, lg.i64_ty, iv, "i");
    LLVMValueRef i1 = LLVMBuildAdd(lg.builder, i, one, "i1");
    LLVMValueRef cc = LLVMBuildICmp(lg.builder, LLVMIntSLT, i1, n, "cc");
    LLVMBuildCondBr(lg.builder, cc, body, done);
    LLVMPositionBuilderAtEnd(lg.builder, body);
    LLVMValueRef cp0 = LLVMBuildGEP2(lg.builder, lg.i8_ty, s, &i, 1, "cp0");
    LLVMValueRef c0 = LLVMBuildZExt(lg.builder, LLVMBuildLoad2(lg.builder, lg.i8_ty, cp0, "c0v"), lg.i64_ty, "c0");
    LLVMValueRef cp1 = LLVMBuildGEP2(lg.builder, lg.i8_ty, s, &i1, 1, "cp1");
    LLVMValueRef c1 = LLVMBuildZExt(lg.builder, LLVMBuildLoad2(lg.builder, lg.i8_ty, cp1, "c1v"), lg.i64_ty, "c1");
    LLVMValueRef h0 = llv_h_call(llv_baga_rt("baga_hex_val"), &c0, 1, "h0");
    LLVMValueRef h1 = llv_h_call(llv_baga_rt("baga_hex_val"), &c1, 1, "h1");
    LLVMValueRef bad0 = LLVMBuildICmp(lg.builder, LLVMIntSLT, h0, zero, "bad0");
    LLVMValueRef bad1 = LLVMBuildICmp(lg.builder, LLVMIntSLT, h1, zero, "bad1");
    LLVMValueRef bad = LLVMBuildOr(lg.builder, bad0, bad1, "bad");
    LLVMBasicBlockRef skip = LLVMAppendBasicBlockInContext(lg.ctx, fn, "skip");
    LLVMBasicBlockRef take = LLVMAppendBasicBlockInContext(lg.ctx, fn, "take");
    LLVMBuildCondBr(lg.builder, bad, skip, take);
    LLVMPositionBuilderAtEnd(lg.builder, skip);
    LLVMBuildStore(lg.builder, LLVMBuildAdd(lg.builder, i, one, "si"), iv);
    LLVMBuildBr(lg.builder, cond);
    LLVMPositionBuilderAtEnd(lg.builder, take);
    LLVMValueRef byte = LLVMBuildAdd(lg.builder, LLVMBuildMul(lg.builder, h0, sixteen, "m"), h1, "byte");
    LLVMValueRef l = LLVMBuildLoad2(lg.builder, lg.i64_ty, len, "l");
    LLVMValueRef b8 = LLVMBuildTrunc(lg.builder, byte, lg.i8_ty, "b8");
    LLVMBuildStore(lg.builder, b8, LLVMBuildGEP2(lg.builder, lg.i8_ty, buf, &l, 1, "sl"));
    LLVMBuildStore(lg.builder, LLVMBuildAdd(lg.builder, l, one, "l1"), len);
    LLVMBuildStore(lg.builder, LLVMBuildAdd(lg.builder, i, two, "ti"), iv);
    LLVMBuildBr(lg.builder, cond);
    LLVMPositionBuilderAtEnd(lg.builder, done);
    LLVMValueRef fl = LLVMBuildLoad2(lg.builder, lg.i64_ty, len, "fl");
    LLVMBuildRet(lg.builder, bytes_make(buf, fl));
    (void)neg1;
    return fn;
}

LLVMValueRef llv_build_baga_bytes_from_hex(void) {
    LLVMTypeRef p[] = { lg.ptr_ty };
    LLVMValueRef fn = LLVMAddFunction(lg.mod, "baga_bytes_from_hex",
        LLVMFunctionType(llv_baga_bytes_ty(), p, 1, 0));
    llv_h_begin(fn);
    LLVMValueRef s = LLVMGetParam(fn, 0);
    LLVMValueRef r = llv_h_call(llv_baga_rt("baga_hex_decode"), &s, 1, "r");
    LLVMBuildRet(lg.builder, r);
    return fn;
}

/* static const char *baga_read_file(const char *path) {
 *     FILE *f = fopen(path, "rb"); if (!f) return "";
 *     fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
 *     char *buf = malloc(sz + 1); fread(buf, 1, sz, f);
 *     buf[sz] = 0; fclose(f); return buf; } */
LLVMValueRef llv_build_baga_read_file(void) {
    LLVMTypeRef p[] = { lg.ptr_ty };
    LLVMValueRef fn = LLVMAddFunction(lg.mod, "baga_read_file",
        LLVMFunctionType(lg.ptr_ty, p, 1, 0));
    LLVMTypeRef fp[] = { lg.ptr_ty, lg.ptr_ty };
    LLVMValueRef fopen_fn = llv_rt_libc("fopen", lg.ptr_ty, fp, 2);
    LLVMTypeRef sp[] = { lg.ptr_ty, lg.i64_ty, lg.i32_ty };
    LLVMValueRef fseek_fn = llv_rt_libc("fseek", lg.i32_ty, sp, 3);
    LLVMTypeRef tp[] = { lg.ptr_ty };
    LLVMValueRef ftell_fn = llv_rt_libc("ftell", lg.i64_ty, tp, 1);
    LLVMTypeRef rp[] = { lg.ptr_ty, lg.i64_ty, lg.i64_ty, lg.ptr_ty };
    LLVMValueRef fread_fn = llv_rt_libc("fread", lg.i64_ty, rp, 4);
    LLVMTypeRef cp[] = { lg.ptr_ty };
    LLVMValueRef fclose_fn = llv_rt_libc("fclose", lg.i32_ty, cp, 1);

    llv_h_begin(fn);
    LLVMValueRef rb = LLVMBuildGlobalStringPtr(lg.builder, "rb", "rb");
    LLVMValueRef fa[] = { LLVMGetParam(fn, 0), rb };
    LLVMValueRef f = llv_h_call(fopen_fn, fa, 2, "f");
    LLVMValueRef isnull = LLVMBuildICmp(lg.builder, LLVMIntEQ, f,
        LLVMConstNull(lg.ptr_ty), "isnull");
    LLVMBasicBlockRef empty_bb = LLVMAppendBasicBlockInContext(lg.ctx, fn, "empty");
    LLVMBasicBlockRef body_bb  = LLVMAppendBasicBlockInContext(lg.ctx, fn, "body");
    LLVMBuildCondBr(lg.builder, isnull, empty_bb, body_bb);

    LLVMPositionBuilderAtEnd(lg.builder, empty_bb);
    LLVMValueRef es = LLVMBuildGlobalStringPtr(lg.builder, "", "empty");
    LLVMBuildRet(lg.builder, es);

    LLVMPositionBuilderAtEnd(lg.builder, body_bb);
    LLVMValueRef se_end[] = { f, LLVMConstInt(lg.i64_ty, 0, 0),
        LLVMConstInt(lg.i32_ty, 2, 0) }; /* SEEK_END */
    llv_h_call(fseek_fn, se_end, 3, "");
    LLVMValueRef ta[] = { f };
    LLVMValueRef sz = llv_h_call(ftell_fn, ta, 1, "sz");
    LLVMValueRef se_set[] = { f, LLVMConstInt(lg.i64_ty, 0, 0),
        LLVMConstInt(lg.i32_ty, 0, 0) }; /* SEEK_SET */
    llv_h_call(fseek_fn, se_set, 3, "");
    LLVMValueRef sz1 = LLVMBuildAdd(lg.builder, sz,
        LLVMConstInt(lg.i64_ty, 1, 0), "sz1");
    LLVMValueRef buf = llv_str_alloc_call(sz1, "buf");
    LLVMValueRef ra[] = { buf, LLVMConstInt(lg.i64_ty, 1, 0), sz, f };
    llv_h_call(fread_fn, ra, 4, "rd");
    LLVMValueRef end = LLVMBuildGEP2(lg.builder, lg.i8_ty, buf, &sz, 1, "end");
    LLVMBuildStore(lg.builder, LLVMConstInt(lg.i8_ty, 0, 0), end);
    LLVMValueRef ca[] = { f };
    llv_h_call(fclose_fn, ca, 1, "");
    /* NUL-safe: slen == strlen(buf), не sz (двоичен файл може да има NUL) */
    LLVMValueRef sla[] = { buf };
    LLVMValueRef sl = llv_h_call(llv_rt_strlen(), sla, 1, "sl");
    LLVMValueRef ss[] = { buf, sl };
    llv_h_call(llv_baga_rt("baga_set_slen"), ss, 2, "");
    LLVMBuildRet(lg.builder, buf);
    return fn;
}

/* ---- Потребителски структури ----
 *
 * Named LLVM struct типове (mangled име, като в codegen_c), създадени
 * в първия проход на codegen_llvm. Стойността е първокласен struct
 * (by value), както C struct в codegen_c. */

LLVMTypeRef llv_user_struct_ty(const char *name) {
    char *m = llv_llvm_mangle(name);
    LLVMTypeRef t = LLVMGetTypeByName(lg.mod, m);
    free(m);
    if (!t) {
        char buf[256];
        snprintf(buf, sizeof buf, "неизвестна структура '%s'", name);
        llv_llvm_unsupported(buf);
    }
    return t;
}

Node *llv_find_struct_decl(const char *name) {
    if (!lg.program) return NULL;
    for (int i = 0; i < lg.program->items.len; i++) {
        Node *item = lg.program->items.data[i];
        if (item->kind == NODE_STRUCT && strcmp(item->struct_name, name) == 0)
            return item;
    }
    return NULL;
}

int llv_struct_field_index(Node *decl, const char *fname) {
    for (int i = 0; i < decl->fields.len; i++)
        if (strcmp(decl->fields.data[i]->fld_name, fname) == 0)
            return i;
    return -1;
}

/* ---- L3: sum enum-и ----
 *
 * Named struct b_<E> = { i64 tag, [N x i64] u }, където
 * N = max(1, ceil(max payload ABI размер / 8)) — LLVM няма union, затова
 * union областта е i64 масив, а payload store/load е GEP до u + bitcast
 * към указател към payload типа. Тагът е индексът на варианта в
 * декларацията (0..n-1) — същият ред като в codegen_c. */

/* тяло на sum enum типа (изисква payload типовете да са sized) */
void llv_sum_enum_set_body(Node *ed, LLVMTypeRef st) {
    unsigned long long maxsz = 0;
    for (int j = 0; j < ed->n_variants; j++) {
        if (!ed->enum_payloads || !ed->enum_payloads[j]) continue;
        unsigned long long sz =
            LLVMABISizeOfType(lg.td, llv_llvm_type(ed->enum_payloads[j]));
        if (sz > maxsz) maxsz = sz;
    }
    unsigned n = (unsigned)(maxsz / 8) + (maxsz % 8 ? 1 : 0);
    if (n == 0) n = 1;
    LLVMTypeRef elems[] = { lg.i64_ty, LLVMArrayType(lg.i64_ty, n) };
    LLVMStructSetBody(st, elems, 2, 0);
}

/* GEP до union областта + bitcast към указател към payload типа */
LLVMValueRef llv_sum_payload_ptr(LLVMTypeRef ety, LLVMValueRef ea,
                                    LLVMTypeRef pty) {
    LLVMValueRef up = LLVMBuildStructGEP2(lg.builder, ety, ea, 1, "up");
    return LLVMBuildBitCast(lg.builder, up, LLVMPointerType(pty, 0), "pp");
}

/* lazy IR конструктор b_<E>__b_<V>(payload) — огледало на static inline
 * конструкторите в codegen_c; тялото се генерира при първа употреба */
LLVMValueRef llv_sum_ctor_fn(Node *ed, int vidx) {
    char *em = llv_llvm_mangle(ed->enum_name);
    char *vm = llv_llvm_mangle(ed->enum_variants[vidx]);
    char full[600];
    snprintf(full, sizeof full, "%s__%s", em, vm);
    free(em); free(vm);
    LLVMValueRef fn = LLVMGetNamedFunction(lg.mod, full);
    if (fn) return fn;
    LLVMTypeRef ety = llv_user_struct_ty(ed->enum_name);
    LLVMTypeRef pty = llv_llvm_type(ed->enum_payloads[vidx]);
    LLVMBasicBlockRef saved = LLVMGetInsertBlock(lg.builder);
    fn = LLVMAddFunction(lg.mod, full, LLVMFunctionType(ety, &pty, 1, 0));
    llv_h_begin(fn);
    LLVMValueRef r = LLVMBuildAlloca(lg.builder, ety, "r");
    LLVMValueRef tagp = LLVMBuildStructGEP2(lg.builder, ety, r, 0, "tagp");
    LLVMBuildStore(lg.builder,
        LLVMConstInt(lg.i64_ty, (unsigned long long)vidx, 0), tagp);
    LLVMBuildStore(lg.builder, LLVMGetParam(fn, 0),
                   llv_sum_payload_ptr(ety, r, pty));
    LLVMBuildRet(lg.builder, LLVMBuildLoad2(lg.builder, ety, r, "rv"));
    if (saved) LLVMPositionBuilderAtEnd(lg.builder, saved);
    return fn;
}

/* payload-less вариант на sum enum като стойност: { tag = j, u = 0 } */
LLVMValueRef llv_sum_variant_const(Node *ed, int vidx) {
    LLVMTypeRef ety = llv_user_struct_ty(ed->enum_name);
    LLVMValueRef tmp = llv_entry_alloca(ety, "ev");
    LLVMBuildStore(lg.builder, LLVMConstNull(ety), tmp);
    LLVMValueRef tagp = LLVMBuildStructGEP2(lg.builder, ety, tmp, 0, "tagp");
    LLVMBuildStore(lg.builder,
        LLVMConstInt(lg.i64_ty, (unsigned long long)vidx, 0), tagp);
    char *nm = llv_tmp_name();
    LLVMValueRef r = LLVMBuildLoad2(lg.builder, ety, tmp, nm);
    free(nm);
    return r;
}

/* ---- Програмни аргументи (argv) ----
 * main(argc, argv) записва стойностите в IR глобални; baga_arg_count/
 * baga_arg ги четат. baga_arg е с bounds check — извън границите връща "". */
LLVMValueRef llv_argv_argc_global(void) {
    LLVMValueRef g = LLVMGetNamedGlobal(lg.mod, "baga_argc");
    if (g) return g;
    g = LLVMAddGlobal(lg.mod, lg.i32_ty, "baga_argc");
    LLVMSetInitializer(g, LLVMConstInt(lg.i32_ty, 0, 0));
    return g;
}
LLVMValueRef llv_argv_argv_global(void) {
    LLVMValueRef g = LLVMGetNamedGlobal(lg.mod, "baga_argv");
    if (g) return g;
    LLVMTypeRef pp = LLVMPointerType(lg.ptr_ty, 0);   /* char **argv */
    g = LLVMAddGlobal(lg.mod, pp, "baga_argv");
    LLVMSetInitializer(g, LLVMConstNull(pp));
    return g;
}

/* static int64_t baga_arg_count(void) { return baga_argc > 0 ? baga_argc - 1 : 0; } */
LLVMValueRef llv_build_baga_arg_count(void) {
    LLVMValueRef fn = LLVMAddFunction(lg.mod, "baga_arg_count",
        LLVMFunctionType(lg.i64_ty, NULL, 0, 0));
    llv_h_begin(fn);
    LLVMValueRef ac = LLVMBuildLoad2(lg.builder, lg.i32_ty, llv_argv_argc_global(), "ac");
    LLVMValueRef gt = LLVMBuildICmp(lg.builder, LLVMIntSGT, ac,
        LLVMConstInt(lg.i32_ty, 0, 0), "gt");
    LLVMValueRef sub = LLVMBuildSub(lg.builder, ac,
        LLVMConstInt(lg.i32_ty, 1, 0), "sub");
    LLVMValueRef n = LLVMBuildSelect(lg.builder, gt, sub,
        LLVMConstInt(lg.i32_ty, 0, 0), "n");
    LLVMBuildRet(lg.builder, LLVMBuildSExt(lg.builder, n, lg.i64_ty, "r"));
    return fn;
}

/* static const char *baga_arg(int64_t i)
 * { return (i + 1 < baga_argc) ? baga_argv[i + 1] : ""; } */
LLVMValueRef llv_build_baga_arg(void) {
    LLVMTypeRef p[] = { lg.i64_ty };
    LLVMValueRef fn = LLVMAddFunction(lg.mod, "baga_arg",
        LLVMFunctionType(lg.ptr_ty, p, 1, 0));
    llv_h_begin(fn);
    LLVMValueRef idx = LLVMBuildAdd(lg.builder, LLVMGetParam(fn, 0),
        LLVMConstInt(lg.i64_ty, 1, 0), "idx");
    LLVMValueRef ac = LLVMBuildLoad2(lg.builder, lg.i32_ty, llv_argv_argc_global(), "ac");
    LLVMValueRef ac64 = LLVMBuildSExt(lg.builder, ac, lg.i64_ty, "ac64");
    LLVMValueRef ok = LLVMBuildICmp(lg.builder, LLVMIntSLT, idx, ac64, "ok");
    LLVMBasicBlockRef in_bb  = LLVMAppendBasicBlockInContext(lg.ctx, fn, "inb");
    LLVMBasicBlockRef out_bb = LLVMAppendBasicBlockInContext(lg.ctx, fn, "outb");
    LLVMBuildCondBr(lg.builder, ok, in_bb, out_bb);

    LLVMPositionBuilderAtEnd(lg.builder, in_bb);
    LLVMValueRef av = LLVMBuildLoad2(lg.builder, LLVMPointerType(lg.ptr_ty, 0),
        llv_argv_argv_global(), "av");
    LLVMValueRef slot = LLVMBuildGEP2(lg.builder, lg.ptr_ty, av, &idx, 1, "slot");
    LLVMBuildRet(lg.builder, LLVMBuildLoad2(lg.builder, lg.ptr_ty, slot, "s"));

    LLVMPositionBuilderAtEnd(lg.builder, out_bb);
    LLVMBuildRet(lg.builder, LLVMBuildGlobalStringPtr(lg.builder, "", "empty"));
    return fn;
}

/* static void baga_exit(int64_t c) { exit((int)c); } */
LLVMValueRef llv_build_baga_exit(void) {
    LLVMTypeRef p[] = { lg.i64_ty };
    LLVMValueRef fn = LLVMAddFunction(lg.mod, "baga_exit",
        LLVMFunctionType(lg.void_ty, p, 1, 0));
    llv_h_begin(fn);
    LLVMValueRef c32 = LLVMBuildTrunc(lg.builder, LLVMGetParam(fn, 0), lg.i32_ty, "c");
    LLVMValueRef args[] = { c32 };
    LLVMBuildCall2(lg.builder, LLVMGlobalGetValueType(lg.exit_fn),
                   lg.exit_fn, args, 1, "");
    LLVMBuildUnreachable(lg.builder);
    return fn;
}

/* static void baga_eprintln(const char *s) { fprintf(stderr, "%s\n", s); } */
LLVMValueRef llv_build_baga_eprintln(void) {
    LLVMTypeRef p[] = { lg.ptr_ty };
    LLVMValueRef fn = LLVMAddFunction(lg.mod, "baga_eprintln",
        LLVMFunctionType(lg.void_ty, p, 1, 0));
    llv_h_begin(fn);
    LLVMValueRef fmt = LLVMBuildGlobalStringPtr(lg.builder, "%s\n", "fmt");
    LLVMValueRef err = LLVMBuildLoad2(lg.builder, lg.ptr_ty, lg.stderr_global, "err");
    LLVMValueRef args[] = { err, fmt, LLVMGetParam(fn, 0) };
    LLVMBuildCall2(lg.builder, LLVMGlobalGetValueType(lg.fprintf_fn),
                   lg.fprintf_fn, args, 3, "");
    LLVMBuildRetVoid(lg.builder);
    return fn;
}

#endif /* BAGA_LLVM */
