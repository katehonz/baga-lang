/* codegen_llvm_drop.c — част от codegen_llvm.c (виж codegen_llvm_impl.h). */
#include "baga.h"
#include "codegen_llvm_impl.h"

/* ---- MEM-1: drop helpers (само не-RC пътя; под --rc drop ≡ release) ----

 * Порт на baga_drop_* от codegen_c.c:6188-6214. Внимание: без --rc
 * алокациите са plain malloc БЕЗ header — free(p) директно, никакъв
 * baga_rc_hdr. elem_size/val_size се пазят само за сигнатурен паритет с C
 * (libc free не иска размер). */

/* static void baga_drop_str(i8 *p) { free(p); }
 * checker-ът (checker.c:1835) засега отказва drop върху str — този helper
 * е defensive, за пълнота на dispatch-а. */
#ifdef BAGA_LLVM

static LLVMValueRef build_baga_drop_str(void) {
    LLVMTypeRef p[] = { lg.ptr_ty };
    LLVMValueRef fn = LLVMAddFunction(lg.mod, "baga_drop_str",
        LLVMFunctionType(lg.void_ty, p, 1, 0));
    llv_h_begin(fn);
    LLVMValueRef ptr = LLVMGetParam(fn, 0);
    llv_h_call(llv_rt_free(), &ptr, 1, "");
    LLVMBuildRetVoid(lg.builder);
    return fn;
}

/* static void baga_drop_bytes(baga_bytes b) { free(b.data); } */
static LLVMValueRef build_baga_drop_bytes(void) {
    LLVMTypeRef p[] = { llv_baga_bytes_ty() };
    LLVMValueRef fn = LLVMAddFunction(lg.mod, "baga_drop_bytes",
        LLVMFunctionType(lg.void_ty, p, 1, 0));
    llv_h_begin(fn);
    LLVMValueRef a = llv_bytes_param_alloca(LLVMGetParam(fn, 0));
    LLVMValueRef d = llv_bytes_load_data(a);
    llv_h_call(llv_rt_free(), &d, 1, "");
    LLVMBuildRetVoid(lg.builder);
    return fn;
}

/* static void baga_drop_vec(baga_Vec *v, i64 elem_kind, i64 elem_size)
 * { if (!v) return;
 *   if (elem_kind == 2) for (i < len) free(v->data[i]);
 *   if (elem_kind == 3) for (i < len) baga_drop_vec(v->data[i], 0, 0);
 *   free(v->data); free(v); }
 * elem_kind: 0 = inline (i64/f64), 1 = str (споделени — не се пипат),
 * 2 = box (bytes/struct), 3 = вложен Vec. */
static LLVMValueRef build_baga_drop_vec(void) {
    LLVMTypeRef p[] = { llv_baga_vec_ptr_ty(), lg.i64_ty, lg.i64_ty };
    LLVMValueRef fn = LLVMAddFunction(lg.mod, "baga_drop_vec",
        LLVMFunctionType(lg.void_ty, p, 3, 0));
    llv_h_begin(fn);
    LLVMValueRef v = LLVMGetParam(fn, 0);
    LLVMValueRef ek = LLVMGetParam(fn, 1);
    LLVMValueRef zero = LLVMConstInt(lg.i64_ty, 0, 0);
    LLVMBasicBlockRef go_bb  = LLVMAppendBasicBlockInContext(lg.ctx, fn, "go");
    LLVMBasicBlockRef ret_bb = LLVMAppendBasicBlockInContext(lg.ctx, fn, "ret");
    LLVMBuildCondBr(lg.builder, LLVMBuildIsNull(lg.builder, v, "isn"),
                    ret_bb, go_bb);

    LLVMPositionBuilderAtEnd(lg.builder, go_bb);
    LLVMValueRef data = llv_vec_load_data(v);
    LLVMValueRef len = llv_vec_load_len(v);
    LLVMValueRef bi = llv_entry_alloca(lg.i64_ty, "i");
    LLVMBuildStore(lg.builder, zero, bi);
    LLVMValueRef ni = llv_entry_alloca(lg.i64_ty, "j");
    LLVMBuildStore(lg.builder, zero, ni);
    LLVMBasicBlockRef boxc_bb  = LLVMAppendBasicBlockInContext(lg.ctx, fn, "boxc");
    LLVMBasicBlockRef boxb_bb  = LLVMAppendBasicBlockInContext(lg.ctx, fn, "boxb");
    LLVMBasicBlockRef nchk_bb  = LLVMAppendBasicBlockInContext(lg.ctx, fn, "nchk");
    LLVMBasicBlockRef nestc_bb = LLVMAppendBasicBlockInContext(lg.ctx, fn, "nestc");
    LLVMBasicBlockRef nestb_bb = LLVMAppendBasicBlockInContext(lg.ctx, fn, "nestb");
    LLVMBasicBlockRef fin_bb   = LLVMAppendBasicBlockInContext(lg.ctx, fn, "fin");
    LLVMValueRef is_box = LLVMBuildICmp(lg.builder, LLVMIntEQ, ek,
        LLVMConstInt(lg.i64_ty, 2, 0), "isbox");
    LLVMBuildCondBr(lg.builder, is_box, boxc_bb, nchk_bb);

    /* ek == 2: free на всяко box-нато data[i] */
    LLVMPositionBuilderAtEnd(lg.builder, boxc_bb);
    LLVMValueRef i1 = LLVMBuildLoad2(lg.builder, lg.i64_ty, bi, "i");
    LLVMBuildCondBr(lg.builder, LLVMBuildICmp(lg.builder, LLVMIntSLT, i1, len, "cc"),
                    boxb_bb, fin_bb);
    LLVMPositionBuilderAtEnd(lg.builder, boxb_bb);
    LLVMValueRef e1 = llv_vec_load_at(v, i1);
    llv_h_call(llv_rt_free(), &e1, 1, "");
    LLVMBuildStore(lg.builder, LLVMBuildAdd(lg.builder, i1,
        LLVMConstInt(lg.i64_ty, 1, 0), "in"), bi);
    LLVMBuildBr(lg.builder, boxc_bb);

    /* ek == 3: рекурсивен drop на вложените vec-ове */
    LLVMPositionBuilderAtEnd(lg.builder, nchk_bb);
    LLVMValueRef is_nest = LLVMBuildICmp(lg.builder, LLVMIntEQ, ek,
        LLVMConstInt(lg.i64_ty, 3, 0), "isnest");
    LLVMBuildCondBr(lg.builder, is_nest, nestc_bb, fin_bb);
    LLVMPositionBuilderAtEnd(lg.builder, nestc_bb);
    LLVMValueRef i2 = LLVMBuildLoad2(lg.builder, lg.i64_ty, ni, "j");
    LLVMBuildCondBr(lg.builder, LLVMBuildICmp(lg.builder, LLVMIntSLT, i2, len, "cc"),
                    nestb_bb, fin_bb);
    LLVMPositionBuilderAtEnd(lg.builder, nestb_bb);
    LLVMValueRef e2 = LLVMBuildBitCast(lg.builder, llv_vec_load_at(v, i2),
        llv_baga_vec_ptr_ty(), "nv");
    LLVMValueRef ra[] = { e2, zero, zero };
    llv_h_call(fn, ra, 3, "");
    LLVMBuildStore(lg.builder, LLVMBuildAdd(lg.builder, i2,
        LLVMConstInt(lg.i64_ty, 1, 0), "jn"), ni);
    LLVMBuildBr(lg.builder, nestc_bb);

    LLVMPositionBuilderAtEnd(lg.builder, fin_bb);
    LLVMValueRef dr = LLVMBuildBitCast(lg.builder, data, lg.ptr_ty, "dr");
    llv_h_call(llv_rt_free(), &dr, 1, "");
    LLVMValueRef vr = LLVMBuildBitCast(lg.builder, v, lg.ptr_ty, "vr");
    llv_h_call(llv_rt_free(), &vr, 1, "");
    LLVMBuildBr(lg.builder, ret_bb);

    LLVMPositionBuilderAtEnd(lg.builder, ret_bb);
    LLVMBuildRetVoid(lg.builder);
    return fn;
}

/* static void baga_drop_map(baga_Map *m, i64 val_is_box, i64 val_size)
 * { if (!m) return;
 *   for (i < nb) { e = b[i]; while (e) { nx = e->next;
 *     if (val_is_box && e->pv) free(e->pv); free(e); e = nx; } }
 *   free(b); free(m); }
 * next се пази ПРЕДИ free — като в C (libc free не презаписва, но редът е
 * същият за паритет). */
static LLVMValueRef build_baga_drop_map(void) {
    LLVMTypeRef p[] = { llv_baga_map_ptr_ty(), lg.i64_ty, lg.i64_ty };
    LLVMValueRef fn = LLVMAddFunction(lg.mod, "baga_drop_map",
        LLVMFunctionType(lg.void_ty, p, 3, 0));
    llv_h_begin(fn);
    LLVMValueRef m = LLVMGetParam(fn, 0);
    LLVMValueRef vib = LLVMGetParam(fn, 1);
    LLVMBasicBlockRef go_bb  = LLVMAppendBasicBlockInContext(lg.ctx, fn, "go");
    LLVMBasicBlockRef ret_bb = LLVMAppendBasicBlockInContext(lg.ctx, fn, "ret");
    LLVMBuildCondBr(lg.builder, LLVMBuildIsNull(lg.builder, m, "isn"),
                    ret_bb, go_bb);

    LLVMPositionBuilderAtEnd(lg.builder, go_bb);
    LLVMValueRef b = llv_map_load_b(m);
    LLVMValueRef nb = llv_map_load_nb(m);
    LLVMValueRef iv = llv_entry_alloca(lg.i64_ty, "i");
    LLVMBuildStore(lg.builder, LLVMConstInt(lg.i64_ty, 0, 0), iv);
    LLVMValueRef ev = llv_entry_alloca(llv_baga_map_entry_ptr_ty(), "e");
    LLVMBasicBlockRef oc_bb  = LLVMAppendBasicBlockInContext(lg.ctx, fn, "oc");
    LLVMBasicBlockRef ob_bb  = LLVMAppendBasicBlockInContext(lg.ctx, fn, "ob");
    LLVMBasicBlockRef wc_bb  = LLVMAppendBasicBlockInContext(lg.ctx, fn, "wc");
    LLVMBasicBlockRef wb_bb  = LLVMAppendBasicBlockInContext(lg.ctx, fn, "wb");
    LLVMBasicBlockRef vchk_bb = LLVMAppendBasicBlockInContext(lg.ctx, fn, "vchk");
    LLVMBasicBlockRef pvf_bb = LLVMAppendBasicBlockInContext(lg.ctx, fn, "pvf");
    LLVMBasicBlockRef fe_bb  = LLVMAppendBasicBlockInContext(lg.ctx, fn, "fe");
    LLVMBasicBlockRef inx_bb = LLVMAppendBasicBlockInContext(lg.ctx, fn, "inx");
    LLVMBasicBlockRef fin_bb = LLVMAppendBasicBlockInContext(lg.ctx, fn, "fin");
    LLVMBuildBr(lg.builder, oc_bb);

    LLVMPositionBuilderAtEnd(lg.builder, oc_bb);
    LLVMValueRef i = LLVMBuildLoad2(lg.builder, lg.i64_ty, iv, "i");
    LLVMBuildCondBr(lg.builder, LLVMBuildICmp(lg.builder, LLVMIntSLT, i, nb, "cc"),
                    ob_bb, fin_bb);
    LLVMPositionBuilderAtEnd(lg.builder, ob_bb);
    LLVMValueRef e0 = LLVMBuildLoad2(lg.builder, llv_baga_map_entry_ptr_ty(),
        LLVMBuildGEP2(lg.builder, llv_baga_map_entry_ptr_ty(), b, &i, 1, "slot"), "e0");
    LLVMBuildStore(lg.builder, e0, ev);
    LLVMBuildBr(lg.builder, wc_bb);

    LLVMPositionBuilderAtEnd(lg.builder, wc_bb);
    LLVMValueRef e = LLVMBuildLoad2(lg.builder, llv_baga_map_entry_ptr_ty(), ev, "e");
    LLVMBuildCondBr(lg.builder, LLVMBuildIsNull(lg.builder, e, "isn"),
                    inx_bb, wb_bb);
    LLVMPositionBuilderAtEnd(lg.builder, wb_bb);
    LLVMValueRef nx = LLVMBuildLoad2(lg.builder, llv_baga_map_entry_ptr_ty(),
        llv_ent_fld(e, 9, "nxp"), "nx");
    /* if (val_is_box && e->pv) free(e->pv); */
    LLVMValueRef has_v = LLVMBuildICmp(lg.builder, LLVMIntNE, vib,
        LLVMConstInt(lg.i64_ty, 0, 0), "hasv");
    LLVMBuildCondBr(lg.builder, has_v, vchk_bb, fe_bb);
    LLVMPositionBuilderAtEnd(lg.builder, vchk_bb);
    LLVMValueRef pv = LLVMBuildLoad2(lg.builder, lg.ptr_ty,
        llv_ent_fld(e, 8, "pvp"), "pv");
    LLVMBuildCondBr(lg.builder, LLVMBuildIsNull(lg.builder, pv, "pvn"),
                    fe_bb, pvf_bb);
    LLVMPositionBuilderAtEnd(lg.builder, pvf_bb);
    llv_h_call(llv_rt_free(), &pv, 1, "");
    LLVMBuildBr(lg.builder, fe_bb);

    LLVMPositionBuilderAtEnd(lg.builder, fe_bb);
    LLVMValueRef er = LLVMBuildBitCast(lg.builder, e, lg.ptr_ty, "er");
    llv_h_call(llv_rt_free(), &er, 1, "");
    LLVMBuildStore(lg.builder, nx, ev);
    LLVMBuildBr(lg.builder, wc_bb);

    LLVMPositionBuilderAtEnd(lg.builder, inx_bb);
    LLVMBuildStore(lg.builder, LLVMBuildAdd(lg.builder, i,
        LLVMConstInt(lg.i64_ty, 1, 0), "in"), iv);
    LLVMBuildBr(lg.builder, oc_bb);

    LLVMPositionBuilderAtEnd(lg.builder, fin_bb);
    LLVMValueRef br_ = LLVMBuildBitCast(lg.builder, b, lg.ptr_ty, "br");
    llv_h_call(llv_rt_free(), &br_, 1, "");
    LLVMValueRef mr = LLVMBuildBitCast(lg.builder, m, lg.ptr_ty, "mr");
    llv_h_call(llv_rt_free(), &mr, 1, "");
    LLVMBuildBr(lg.builder, ret_bb);

    LLVMPositionBuilderAtEnd(lg.builder, ret_bb);
    LLVMBuildRetVoid(lg.builder);
    return fn;
}

/* ---- Task 5: RC release за Vec/Map (--rc) ----
 * Порт на baga_rc_release_vec (codegen_c.c:5635) и baga_rc_release_map
 * (codegen_c.c:6161). rc живее върху STRUCT алокацията; data/bucket/entry
 * блоковете са притежавани 1:1 и умират с контейнера. Task 6: 4-тият
 * параметър е elem_rel/val_rel destructor (baga_rc_relf_<S> / relv_<S>;
 * NULL → само free); box-овете под --rc носят header (llv_rc_alloc_call при
 * push/set) и се free-ват през базата. Разлика от C:
 *  - elem_size/val_size са сигнатурен паритет — libc free не иска размер. */

/* споделен пролог: hdr (NULL → ret), underflow (stderr + exit(1)), rc--;
 * позиционира builder-а в блока „rc стигна 0" и връща header базата.
 * *ret_bb е общият изход — тялото трябва да завърши с br към него. */
static LLVMValueRef rc_container_prologue(LLVMValueRef fn, LLVMValueRef ptr,
                                          const char *msg,
                                          LLVMBasicBlockRef *ret_bb) {
    LLVMValueRef h = llv_h_call(llv_baga_rt("baga_rc_hdr"), &ptr, 1, "h");
    LLVMValueRef isn = LLVMBuildIsNull(lg.builder, h, "isn");
    LLVMBasicBlockRef dec_bb = LLVMAppendBasicBlockInContext(lg.ctx, fn, "dec");
    *ret_bb = LLVMAppendBasicBlockInContext(lg.ctx, fn, "ret");
    LLVMBuildCondBr(lg.builder, isn, *ret_bb, dec_bb);

    LLVMPositionBuilderAtEnd(lg.builder, dec_bb);
    LLVMValueRef rcp = llv_rc_hdr_field(h, 2, "rcp");
    LLVMValueRef rc = LLVMBuildLoad2(lg.builder, lg.i64_ty, rcp, "rc");
    LLVMValueRef isz = LLVMBuildICmp(lg.builder, LLVMIntEQ, rc,
        LLVMConstInt(lg.i64_ty, 0, 0), "isz");
    LLVMBasicBlockRef uf_bb = LLVMAppendBasicBlockInContext(lg.ctx, fn, "uf");
    LLVMBasicBlockRef ok_bb = LLVMAppendBasicBlockInContext(lg.ctx, fn, "ok");
    LLVMBuildCondBr(lg.builder, isz, uf_bb, ok_bb);

    LLVMPositionBuilderAtEnd(lg.builder, uf_bb);
    LLVMValueRef err = LLVMBuildLoad2(lg.builder, lg.ptr_ty,
        lg.stderr_global, "err");
    LLVMValueRef fmt = LLVMBuildGlobalStringPtr(lg.builder, msg, "uffmt");
    LLVMValueRef fa[] = { err, fmt };
    LLVMBuildCall2(lg.builder, LLVMGlobalGetValueType(lg.fprintf_fn),
                   lg.fprintf_fn, fa, 2, "");
    LLVMTypeRef ep[] = { lg.i32_ty };
    LLVMValueRef ea[] = { LLVMConstInt(lg.i32_ty, 1, 0) };
    llv_h_call(llv_rt_libc("exit", lg.void_ty, ep, 1), ea, 1, "");
    LLVMBuildUnreachable(lg.builder);

    LLVMPositionBuilderAtEnd(lg.builder, ok_bb);
    LLVMValueRef rc1 = LLVMBuildSub(lg.builder, rc,
        LLVMConstInt(lg.i64_ty, 1, 0), "rc1");
    LLVMBuildStore(lg.builder, rc1, rcp);
    LLVMValueRef isd = LLVMBuildICmp(lg.builder, LLVMIntEQ, rc1,
        LLVMConstInt(lg.i64_ty, 0, 0), "isd");
    LLVMBasicBlockRef dead_bb = LLVMAppendBasicBlockInContext(lg.ctx, fn, "dead");
    LLVMBuildCondBr(lg.builder, isd, dead_bb, *ret_bb);

    LLVMPositionBuilderAtEnd(lg.builder, dead_bb);
    return h;
}

/* static void baga_rc_release_vec(i8 *v, i64 elem_kind, i64 elem_size,
 *                                 i8 *elem_rel)
 * { h = hdr(v); if (!h) ret; if (rc == 0) underflow; if (--rc > 0) ret;
 *   switch (elem_kind): 1 → release_str(data[i]);
 *     2 → if (elem_rel) elem_rel(data[i]); free(hdr(data[i]));
 *     3 → if (elem_rel) elem_rel(data[i]); else release_vec(data[i],0,0,NULL);
 *     4 → release_bytes(data[i]->data) + free(hdr(data[i]));
 *   free(hdr(data)); free(h); }
 * elem_rel е destructor на box полетата (baga_rc_relf_<S> за struct/enum
 * елемент с heap полета, baga_rc_relv_<S> за вложен Vec<S>; NULL → само
 * free — като elem_rel в codegen_c.c:5635). Box-овете под --rc носят
 * header (llv_rc_alloc_call при push) → free през header базата. */
static LLVMValueRef build_baga_rc_release_vec(void) {
    LLVMTypeRef p[] = { lg.ptr_ty, lg.i64_ty, lg.i64_ty, lg.ptr_ty };
    LLVMValueRef fn = LLVMAddFunction(lg.mod, "baga_rc_release_vec",
        LLVMFunctionType(lg.void_ty, p, 4, 0));
    llv_h_begin(fn);
    LLVMBasicBlockRef ret_bb;
    LLVMValueRef h = rc_container_prologue(fn, LLVMGetParam(fn, 0),
        "baga: rc underflow (vec — двоен release)\n", &ret_bb);
    LLVMValueRef v = LLVMBuildBitCast(lg.builder, LLVMGetParam(fn, 0),
        llv_baga_vec_ptr_ty(), "v");
    LLVMValueRef ek = LLVMGetParam(fn, 1);
    LLVMValueRef rel = LLVMGetParam(fn, 3);
    LLVMTypeRef relfty = LLVMFunctionType(lg.void_ty, &lg.ptr_ty, 1, 0);
    LLVMValueRef data = llv_vec_load_data(v);
    LLVMValueRef len = llv_vec_load_len(v);
    LLVMValueRef iv = llv_entry_alloca(lg.i64_ty, "i");
    LLVMValueRef one = LLVMConstInt(lg.i64_ty, 1, 0);
    LLVMValueRef zero = LLVMConstInt(lg.i64_ty, 0, 0);
    LLVMBasicBlockRef c1  = LLVMAppendBasicBlockInContext(lg.ctx, fn, "c1");
    LLVMBasicBlockRef l1c = LLVMAppendBasicBlockInContext(lg.ctx, fn, "l1c");
    LLVMBasicBlockRef l1b = LLVMAppendBasicBlockInContext(lg.ctx, fn, "l1b");
    LLVMBasicBlockRef c2  = LLVMAppendBasicBlockInContext(lg.ctx, fn, "c2");
    LLVMBasicBlockRef l2c = LLVMAppendBasicBlockInContext(lg.ctx, fn, "l2c");
    LLVMBasicBlockRef l2b = LLVMAppendBasicBlockInContext(lg.ctx, fn, "l2b");
    LLVMBasicBlockRef l2r = LLVMAppendBasicBlockInContext(lg.ctx, fn, "l2r");
    LLVMBasicBlockRef l2f = LLVMAppendBasicBlockInContext(lg.ctx, fn, "l2f");
    LLVMBasicBlockRef c3  = LLVMAppendBasicBlockInContext(lg.ctx, fn, "c3");
    LLVMBasicBlockRef l3c = LLVMAppendBasicBlockInContext(lg.ctx, fn, "l3c");
    LLVMBasicBlockRef l3b = LLVMAppendBasicBlockInContext(lg.ctx, fn, "l3b");
    LLVMBasicBlockRef l3r = LLVMAppendBasicBlockInContext(lg.ctx, fn, "l3r");
    LLVMBasicBlockRef l3n = LLVMAppendBasicBlockInContext(lg.ctx, fn, "l3n");
    LLVMBasicBlockRef l3i = LLVMAppendBasicBlockInContext(lg.ctx, fn, "l3i");
    LLVMBasicBlockRef c4  = LLVMAppendBasicBlockInContext(lg.ctx, fn, "c4");
    LLVMBasicBlockRef l4c = LLVMAppendBasicBlockInContext(lg.ctx, fn, "l4c");
    LLVMBasicBlockRef l4b = LLVMAppendBasicBlockInContext(lg.ctx, fn, "l4b");
    LLVMBasicBlockRef fin = LLVMAppendBasicBlockInContext(lg.ctx, fn, "fin");
    LLVMBuildBr(lg.builder, c1);

    /* ek == 1: str елементи — release на всеки */
    LLVMPositionBuilderAtEnd(lg.builder, c1);
    LLVMBuildStore(lg.builder, zero, iv);
    LLVMBuildCondBr(lg.builder, LLVMBuildICmp(lg.builder, LLVMIntEQ, ek,
        one, "is1"), l1c, c2);
    LLVMPositionBuilderAtEnd(lg.builder, l1c);
    LLVMValueRef i1 = LLVMBuildLoad2(lg.builder, lg.i64_ty, iv, "i");
    LLVMBuildCondBr(lg.builder, LLVMBuildICmp(lg.builder, LLVMIntSLT, i1, len, "cc"),
                    l1b, fin);
    LLVMPositionBuilderAtEnd(lg.builder, l1b);
    LLVMValueRef e1 = llv_vec_load_at(v, i1);
    llv_h_call(llv_baga_rt("baga_rc_release_str"), &e1, 1, "");
    LLVMBuildStore(lg.builder, LLVMBuildAdd(lg.builder, i1, one, "in"), iv);
    LLVMBuildBr(lg.builder, l1c);

    /* ek == 2: struct/enum box — destructor на полетата (ако е подаден),
     * после free на box-а през header базата */
    LLVMPositionBuilderAtEnd(lg.builder, c2);
    LLVMBuildStore(lg.builder, zero, iv);
    LLVMBuildCondBr(lg.builder, LLVMBuildICmp(lg.builder, LLVMIntEQ, ek,
        LLVMConstInt(lg.i64_ty, 2, 0), "is2"), l2c, c3);
    LLVMPositionBuilderAtEnd(lg.builder, l2c);
    LLVMValueRef i2 = LLVMBuildLoad2(lg.builder, lg.i64_ty, iv, "i");
    LLVMBuildCondBr(lg.builder, LLVMBuildICmp(lg.builder, LLVMIntSLT, i2, len, "cc"),
                    l2b, fin);
    LLVMPositionBuilderAtEnd(lg.builder, l2b);
    LLVMValueRef e2 = llv_vec_load_at(v, i2);
    LLVMBuildCondBr(lg.builder, LLVMBuildIsNull(lg.builder, rel, "rn"), l2f, l2r);
    LLVMPositionBuilderAtEnd(lg.builder, l2r);
    LLVMValueRef relf2 = LLVMBuildBitCast(lg.builder, rel,
        LLVMPointerType(relfty, 0), "relf");
    LLVMBuildCall2(lg.builder, relfty, relf2, &e2, 1, "");
    LLVMBuildBr(lg.builder, l2f);
    LLVMPositionBuilderAtEnd(lg.builder, l2f);
    LLVMValueRef h2 = llv_h_call(llv_baga_rt("baga_rc_hdr"), &e2, 1, "hb");
    llv_h_call(llv_rt_free(), &h2, 1, "");
    LLVMBuildStore(lg.builder, LLVMBuildAdd(lg.builder, i2, one, "in"), iv);
    LLVMBuildBr(lg.builder, l2c);

    /* ek == 3: nested Vec — с destructor (relv: Vec<S> с heap полета) или
     * release с kind 0 (елементният тип на вътрешния не е известен по време
     * на изпълнение — като codegen_c) */
    LLVMPositionBuilderAtEnd(lg.builder, c3);
    LLVMBuildStore(lg.builder, zero, iv);
    LLVMBuildCondBr(lg.builder, LLVMBuildICmp(lg.builder, LLVMIntEQ, ek,
        LLVMConstInt(lg.i64_ty, 3, 0), "is3"), l3c, c4);
    LLVMPositionBuilderAtEnd(lg.builder, l3c);
    LLVMValueRef i3 = LLVMBuildLoad2(lg.builder, lg.i64_ty, iv, "i");
    LLVMBuildCondBr(lg.builder, LLVMBuildICmp(lg.builder, LLVMIntSLT, i3, len, "cc"),
                    l3b, fin);
    LLVMPositionBuilderAtEnd(lg.builder, l3b);
    LLVMValueRef e3 = llv_vec_load_at(v, i3);
    LLVMBuildCondBr(lg.builder, LLVMBuildIsNull(lg.builder, rel, "rn"), l3n, l3r);
    LLVMPositionBuilderAtEnd(lg.builder, l3r);
    LLVMValueRef relf3 = LLVMBuildBitCast(lg.builder, rel,
        LLVMPointerType(relfty, 0), "relf");
    LLVMBuildCall2(lg.builder, relfty, relf3, &e3, 1, "");
    /* shim пътят НЕ пада във fallback-а — иначе двоен release на вътрешния
     * vec (декремент от shim-а + още един от kind-0 рекурсията). Преди
     * M26 този път никога не се изпълняваше (rel винаги беше NULL). */
    LLVMBuildBr(lg.builder, l3i);
    LLVMPositionBuilderAtEnd(lg.builder, l3n);
    LLVMValueRef ra[] = { e3, zero, zero, LLVMConstNull(lg.ptr_ty) };
    llv_h_call(fn, ra, 4, "");
    LLVMBuildBr(lg.builder, l3i);
    LLVMPositionBuilderAtEnd(lg.builder, l3i);
    LLVMBuildStore(lg.builder, LLVMBuildAdd(lg.builder, i3, one, "in"), iv);
    LLVMBuildBr(lg.builder, l3c);

    /* ek == 4: bytes box — release на data + free на box-а през header-а */
    LLVMPositionBuilderAtEnd(lg.builder, c4);
    LLVMBuildStore(lg.builder, zero, iv);
    LLVMBuildCondBr(lg.builder, LLVMBuildICmp(lg.builder, LLVMIntEQ, ek,
        LLVMConstInt(lg.i64_ty, 4, 0), "is4"), l4c, fin);
    LLVMPositionBuilderAtEnd(lg.builder, l4c);
    LLVMValueRef i4 = LLVMBuildLoad2(lg.builder, lg.i64_ty, iv, "i");
    LLVMBuildCondBr(lg.builder, LLVMBuildICmp(lg.builder, LLVMIntSLT, i4, len, "cc"),
                    l4b, fin);
    LLVMPositionBuilderAtEnd(lg.builder, l4b);
    LLVMValueRef e4 = llv_vec_load_at(v, i4);
    LLVMValueRef bp = LLVMBuildBitCast(lg.builder, e4,
        LLVMPointerType(llv_baga_bytes_ty(), 0), "bp");
    LLVMValueRef bd = LLVMBuildLoad2(lg.builder, lg.ptr_ty,
        LLVMBuildStructGEP2(lg.builder, llv_baga_bytes_ty(), bp, 0, "bdp"), "bd");
    llv_h_call(llv_baga_rt("baga_rc_release_bytes"), &bd, 1, "");
    LLVMValueRef h4 = llv_h_call(llv_baga_rt("baga_rc_hdr"), &e4, 1, "hb");
    llv_h_call(llv_rt_free(), &h4, 1, "");
    LLVMBuildStore(lg.builder, LLVMBuildAdd(lg.builder, i4, one, "in"), iv);
    LLVMBuildBr(lg.builder, l4c);

    /* data масивът носи собствен header (vec_new/grow rc_alloc) → free на
     * базата; struct-ът — през неговия header h */
    LLVMPositionBuilderAtEnd(lg.builder, fin);
    LLVMValueRef od = LLVMBuildBitCast(lg.builder, data, lg.ptr_ty, "od");
    LLVMValueRef hd = llv_h_call(llv_baga_rt("baga_rc_hdr"), &od, 1, "hd");
    llv_h_call(llv_rt_free(), &hd, 1, "");
    llv_h_call(llv_rt_free(), &h, 1, "");
    LLVMBuildBr(lg.builder, ret_bb);

    LLVMPositionBuilderAtEnd(lg.builder, ret_bb);
    LLVMBuildRetVoid(lg.builder);
    return fn;
}

/* static void baga_rc_release_map(i8 *m, i64 val_tag, i64 val_size)
 * { h = hdr(m); if (!h) ret; if (rc == 0) underflow; if (--rc > 0) ret;
 *   за всеки entry: ключ по ktag (2 → bytes, иначе sk → str; NULL → no-op);
 *   стойност по val_tag (1 str, 2 bytes, 3 box → free(pv));
 *   free(entry); free(buckets); free(h); }
 * entries/buckets са plain malloc → plain free (умират с контейнера). */
static LLVMValueRef build_baga_rc_release_map(void) {
    LLVMTypeRef p[] = { lg.ptr_ty, lg.i64_ty, lg.i64_ty, lg.ptr_ty };
    LLVMValueRef fn = LLVMAddFunction(lg.mod, "baga_rc_release_map",
        LLVMFunctionType(lg.void_ty, p, 4, 0));
    llv_h_begin(fn);
    LLVMBasicBlockRef ret_bb;
    LLVMValueRef h = rc_container_prologue(fn, LLVMGetParam(fn, 0),
        "baga: rc underflow (map — двоен release)\n", &ret_bb);
    LLVMValueRef m = LLVMBuildBitCast(lg.builder, LLVMGetParam(fn, 0),
        llv_baga_map_ptr_ty(), "m");
    LLVMValueRef vt = LLVMGetParam(fn, 1);
    /* 4-тият параметър е destructor на box стойностите (baga_rc_relf_<S>;
     * NULL → само free — като val_rel в codegen_c.c:6161) */
    LLVMValueRef rel = LLVMGetParam(fn, 3);
    LLVMTypeRef relfty = LLVMFunctionType(lg.void_ty, &lg.ptr_ty, 1, 0);
    LLVMValueRef b = llv_map_load_b(m);
    LLVMValueRef nb = llv_map_load_nb(m);
    LLVMValueRef iv = llv_entry_alloca(lg.i64_ty, "i");
    LLVMBuildStore(lg.builder, LLVMConstInt(lg.i64_ty, 0, 0), iv);
    LLVMValueRef ev = llv_entry_alloca(llv_baga_map_entry_ptr_ty(), "e");
    LLVMBasicBlockRef oc  = LLVMAppendBasicBlockInContext(lg.ctx, fn, "oc");
    LLVMBasicBlockRef ob  = LLVMAppendBasicBlockInContext(lg.ctx, fn, "ob");
    LLVMBasicBlockRef wc  = LLVMAppendBasicBlockInContext(lg.ctx, fn, "wc");
    LLVMBasicBlockRef wb  = LLVMAppendBasicBlockInContext(lg.ctx, fn, "wb");
    LLVMBasicBlockRef kb  = LLVMAppendBasicBlockInContext(lg.ctx, fn, "kb");
    LLVMBasicBlockRef ks  = LLVMAppendBasicBlockInContext(lg.ctx, fn, "ks");
    LLVMBasicBlockRef va  = LLVMAppendBasicBlockInContext(lg.ctx, fn, "va");
    LLVMBasicBlockRef vs1 = LLVMAppendBasicBlockInContext(lg.ctx, fn, "vs1");
    LLVMBasicBlockRef vc2 = LLVMAppendBasicBlockInContext(lg.ctx, fn, "vc2");
    LLVMBasicBlockRef vb1 = LLVMAppendBasicBlockInContext(lg.ctx, fn, "vb1");
    LLVMBasicBlockRef vc3 = LLVMAppendBasicBlockInContext(lg.ctx, fn, "vc3");
    LLVMBasicBlockRef vpc = LLVMAppendBasicBlockInContext(lg.ctx, fn, "vpc");
    LLVMBasicBlockRef vpf = LLVMAppendBasicBlockInContext(lg.ctx, fn, "vpf");
    LLVMBasicBlockRef fe  = LLVMAppendBasicBlockInContext(lg.ctx, fn, "fe");
    LLVMBasicBlockRef inx = LLVMAppendBasicBlockInContext(lg.ctx, fn, "inx");
    LLVMBasicBlockRef fin = LLVMAppendBasicBlockInContext(lg.ctx, fn, "fin");
    LLVMBuildBr(lg.builder, oc);

    LLVMPositionBuilderAtEnd(lg.builder, oc);
    LLVMValueRef i = LLVMBuildLoad2(lg.builder, lg.i64_ty, iv, "i");
    LLVMBuildCondBr(lg.builder, LLVMBuildICmp(lg.builder, LLVMIntSLT, i, nb, "cc"),
                    ob, fin);
    LLVMPositionBuilderAtEnd(lg.builder, ob);
    LLVMBuildStore(lg.builder, LLVMBuildLoad2(lg.builder,
        llv_baga_map_entry_ptr_ty(),
        LLVMBuildGEP2(lg.builder, llv_baga_map_entry_ptr_ty(), b, &i, 1, "slot"),
        "e0"), ev);
    LLVMBuildBr(lg.builder, wc);

    LLVMPositionBuilderAtEnd(lg.builder, wc);
    LLVMValueRef e = LLVMBuildLoad2(lg.builder, llv_baga_map_entry_ptr_ty(), ev, "e");
    LLVMBuildCondBr(lg.builder, LLVMBuildIsNull(lg.builder, e, "isn"),
                    inx, wb);

    LLVMPositionBuilderAtEnd(lg.builder, wb);
    /* next се пази ПРЕДИ free — като в C/drop_map */
    LLVMValueRef nx = LLVMBuildLoad2(lg.builder, llv_baga_map_entry_ptr_ty(),
        llv_ent_fld(e, 9, "nxp"), "nx");
    LLVMValueRef ktag = LLVMBuildLoad2(lg.builder, lg.i64_ty,
        llv_ent_fld(e, 3, "ktp"), "ktag");
    LLVMBuildCondBr(lg.builder, LLVMBuildICmp(lg.builder, LLVMIntEQ, ktag,
        LLVMConstInt(lg.i64_ty, 2, 0), "is2"), kb, ks);
    LLVMPositionBuilderAtEnd(lg.builder, kb);
    LLVMValueRef bkd = LLVMBuildLoad2(lg.builder, lg.ptr_ty,
        LLVMBuildStructGEP2(lg.builder, llv_baga_bytes_ty(),
            llv_ent_fld(e, 2, "bkp"), 0, "bkdp"), "bkd");
    llv_h_call(llv_baga_rt("baga_rc_release_bytes"), &bkd, 1, "");
    LLVMBuildBr(lg.builder, va);
    LLVMPositionBuilderAtEnd(lg.builder, ks);
    LLVMValueRef sk = LLVMBuildLoad2(lg.builder, lg.ptr_ty,
        llv_ent_fld(e, 1, "skp"), "sk");
    llv_h_call(llv_baga_rt("baga_rc_release_str"), &sk, 1, "");
    LLVMBuildBr(lg.builder, va);

    /* стойност по val_tag: 1 str, 2 bytes, 3 box (само free на pv) */
    LLVMPositionBuilderAtEnd(lg.builder, va);
    LLVMBuildCondBr(lg.builder, LLVMBuildICmp(lg.builder, LLVMIntEQ, vt,
        LLVMConstInt(lg.i64_ty, 1, 0), "is1"), vs1, vc2);
    LLVMPositionBuilderAtEnd(lg.builder, vs1);
    LLVMValueRef sv = LLVMBuildLoad2(lg.builder, lg.ptr_ty,
        llv_ent_fld(e, 6, "svp"), "sv");
    llv_h_call(llv_baga_rt("baga_rc_release_str"), &sv, 1, "");
    LLVMBuildBr(lg.builder, fe);
    LLVMPositionBuilderAtEnd(lg.builder, vc2);
    LLVMBuildCondBr(lg.builder, LLVMBuildICmp(lg.builder, LLVMIntEQ, vt,
        LLVMConstInt(lg.i64_ty, 2, 0), "is2v"), vb1, vc3);
    LLVMPositionBuilderAtEnd(lg.builder, vb1);
    LLVMValueRef bvd = LLVMBuildLoad2(lg.builder, lg.ptr_ty,
        LLVMBuildStructGEP2(lg.builder, llv_baga_bytes_ty(),
            llv_ent_fld(e, 7, "bvp"), 0, "bvdp"), "bvd");
    llv_h_call(llv_baga_rt("baga_rc_release_bytes"), &bvd, 1, "");
    LLVMBuildBr(lg.builder, fe);
    LLVMPositionBuilderAtEnd(lg.builder, vc3);
    LLVMBuildCondBr(lg.builder, LLVMBuildICmp(lg.builder, LLVMIntEQ, vt,
        LLVMConstInt(lg.i64_ty, 3, 0), "is3v"), vpc, fe);
    LLVMPositionBuilderAtEnd(lg.builder, vpc);
    LLVMValueRef pv = LLVMBuildLoad2(lg.builder, lg.ptr_ty,
        llv_ent_fld(e, 8, "pvp"), "pv");
    LLVMBuildCondBr(lg.builder, LLVMBuildIsNull(lg.builder, pv, "pvn"),
                    fe, vpf);
    /* box стойност: destructor на полетата (ако е подаден), после free на
     * box-а през header базата (box-овете под --rc са rc_alloc-нати) */
    LLVMBasicBlockRef vpr = LLVMAppendBasicBlockInContext(lg.ctx, fn, "vpr");
    LLVMBasicBlockRef vpf2 = LLVMAppendBasicBlockInContext(lg.ctx, fn, "vpf2");
    LLVMPositionBuilderAtEnd(lg.builder, vpf);
    LLVMBuildCondBr(lg.builder, LLVMBuildIsNull(lg.builder, rel, "rn"),
                    vpf2, vpr);
    LLVMPositionBuilderAtEnd(lg.builder, vpr);
    LLVMValueRef relfv = LLVMBuildBitCast(lg.builder, rel,
        LLVMPointerType(relfty, 0), "relf");
    LLVMBuildCall2(lg.builder, relfty, relfv, &pv, 1, "");
    LLVMBuildBr(lg.builder, vpf2);
    LLVMPositionBuilderAtEnd(lg.builder, vpf2);
    LLVMValueRef hpv = llv_h_call(llv_baga_rt("baga_rc_hdr"), &pv, 1, "hb");
    llv_h_call(llv_rt_free(), &hpv, 1, "");
    LLVMBuildBr(lg.builder, fe);

    LLVMPositionBuilderAtEnd(lg.builder, fe);
    LLVMValueRef er = LLVMBuildBitCast(lg.builder, e, lg.ptr_ty, "er");
    llv_h_call(llv_rt_free(), &er, 1, "");
    LLVMBuildStore(lg.builder, nx, ev);
    LLVMBuildBr(lg.builder, wc);

    LLVMPositionBuilderAtEnd(lg.builder, inx);
    LLVMBuildStore(lg.builder, LLVMBuildAdd(lg.builder, i,
        LLVMConstInt(lg.i64_ty, 1, 0), "in"), iv);
    LLVMBuildBr(lg.builder, oc);

    LLVMPositionBuilderAtEnd(lg.builder, fin);
    LLVMValueRef braw = LLVMBuildBitCast(lg.builder, b, lg.ptr_ty, "braw");
    llv_h_call(llv_rt_free(), &braw, 1, "");
    llv_h_call(llv_rt_free(), &h, 1, "");
    LLVMBuildBr(lg.builder, ret_bb);

    LLVMPositionBuilderAtEnd(lg.builder, ret_bb);
    LLVMBuildRetVoid(lg.builder);
    return fn;
}

/* ---- Task 6: RC helper-и за struct/enum (порт на RC5) ----
 * Порт на emit_rc_struct_helpers (codegen_c.c:4558) и emit_rc_enum_helpers
 * (codegen_c.c:4657), но като lazy IR (като останалите llv_baga_rt): тялото се
 * генерира при първа нужда и се кешира по име в модула.
 *
 * Представяне: struct/enum стойностите са by-value (named struct тип в
 * alloca), БЕЗ собствен rc header — затова baga_rc_retain_<S> /
 * baga_rc_release_<S> вземат стойността по стойност и retain-ват/release-ват
 * само heap ПОЛЕТАТА (транзитивно: вложен struct/enum поле → рекурсивно
 * повикване). Няма free на самия struct — той е стеков. Това е същата
 * семантика като C, където helper-ите също вземат struct-а по стойност.
 * Enum helper-ът е switch по tag полето ({ i64 tag, [N x i64] u }) —
 * release/retain само на heap payload-а на активния variant.
 *
 * Shim-ове за container сайтове, които не знаят типа статично:
 *   baga_rc_relf_<S>(i8 *p) — release на полетата през указател към box
 *   baga_rc_relv_<S>(i8 *p) — release на Vec<S> елемент (вложен Vec)
 * (огледало на relf/relv в codegen_c; retp не е нужен — retain сайтовете
 * знаят типа статично и викат retain_<S> директно). */

/* heap ли е този type AST възел (поле/payload): пряк str/bytes/Vec/Map или
 * вложен struct/enum с heap съдържание — общ предикат за field/variant
 * обходите по-долу */
static int lrc_pt_heap(Node *pt) {
    if (llv_lrc_tag_node(pt)) return 1;
    Node *t = pt;
    while (t && (t->kind == NODE_TYPE_EFFECT || t->kind == NODE_TYPE_REF))
        t = t->inner_type;
    if (!t || t->kind != NODE_TYPE || !t->type_name) return 0;
    if (llv_lrc_struct_has_heap(t->type_name)) return 1;
    Node *ed = llv_find_sum_enum(t->type_name);
    return ed && llv_lrc_enum_has_heap(ed);
}

/* heap ли е enum payload: пряк tag или struct с heap полета. Enum payload
 * в enum НЕ се брои (leak-safe граница — като rc_enum_has_heap_d /
 * emit_rc_enum_helpers в codegen_c) */
static int lrc_enum_payload_heap(Node *pt) {
    if (llv_lrc_tag_node(pt)) return 1;
    Node *t = pt;
    while (t && (t->kind == NODE_TYPE_EFFECT || t->kind == NODE_TYPE_REF))
        t = t->inner_type;
    return t && t->kind == NODE_TYPE && t->type_name &&
        llv_lrc_struct_has_heap(t->type_name);
}

/* едно поле / variant payload: release или retain според вида му.
 * ft е type AST възелът на полето, v — стойността (extractvalue/load). */
static void lrc_field_rc(Node *ft, LLVMValueRef v, int is_release) {
    int tag = llv_lrc_tag_node(ft);
    if (tag == 1) {
        if (is_release) llv_h_call(llv_baga_rt("baga_rc_release_str"), &v, 1, "");
        else            llv_h_call(llv_baga_rt("baga_rc_retain"), &v, 1, "rcr");
        return;
    }
    if (tag == 2) {
        /* bytes поле е { i8*, i64 } by value — rc живее върху data */
        LLVMValueRef d = LLVMBuildExtractValue(lg.builder, v, 0, "fd");
        if (is_release) llv_h_call(llv_baga_rt("baga_rc_release_bytes"), &d, 1, "");
        else            llv_h_call(llv_baga_rt("baga_rc_retain"), &d, 1, "rcr");
        return;
    }
    if (tag == 3 || tag == 4) {
        if (!is_release) {
            LLVMValueRef p = LLVMBuildBitCast(lg.builder, v, lg.ptr_ty, "fcp");
            llv_h_call(llv_baga_rt("baga_rc_retain"), &p, 1, "rcr");
            return;
        }
        Node *inner = ft;
        while (inner && (inner->kind == NODE_TYPE_EFFECT ||
                         inner->kind == NODE_TYPE_REF))
            inner = inner->inner_type;
        /* Vec: elem от inner_type; Map: val от inner_type2 */
        Node *en = inner ? (tag == 3 ? inner->inner_type : inner->inner_type2)
                         : NULL;
        const char *enm = (en && en->kind == NODE_TYPE) ? en->type_name : NULL;
        int k = tag == 3 ? llv_lrc_vec_elem_kind(NULL, ft)
                         : llv_lrc_map_val_tag(NULL, ft);
        LLVMValueRef rel = (tag == 3 && k == 3)
            ? llv_lrc_nested_vec_rel(NULL, ft)
            : llv_lrc_box_rel(enm);
        LLVMValueRef a[] = {
            LLVMBuildBitCast(lg.builder, v, lg.ptr_ty, "fcp"),
            LLVMConstInt(lg.i64_ty, (uint64_t)k, 0),
            LLVMConstInt(lg.i64_ty, 0, 0),
            rel,
        };
        llv_h_call(llv_baga_rt(tag == 3 ? "baga_rc_release_vec"
                                : "baga_rc_release_map"), a, 4, "");
        return;
    }
    /* tag 0: вложен struct с heap полета или enum с heap payload —
     * рекурсивен retain/release (като rc_nested_struct_field /
     * rc_nested_enum_field в codegen_c) */
    Node *t = ft;
    while (t && (t->kind == NODE_TYPE_EFFECT || t->kind == NODE_TYPE_REF))
        t = t->inner_type;
    if (!t || t->kind != NODE_TYPE || !t->type_name) return;
    if (lrc_pt_heap(ft))
        llv_h_call(llv_lrc_rc_fn(t->type_name, is_release), &v, 1, "");
}

/* име на helper-а: baga_rc_{retain,release}_<mangle> */
static char *lrc_rc_fn_name(const char *type_name, int is_release) {
    char *m = llv_llvm_mangle(type_name);
    size_t cap = strlen(m) + 24;
    char *out = malloc(cap);
    if (!out) { fprintf(stderr, "baga: out of memory\n"); exit(1); }
    snprintf(out, cap, "baga_rc_%s_%s", is_release ? "release" : "retain", m);
    free(m);
    return out;
}

static LLVMValueRef lrc_struct_rc_fn(const char *name, int is_release);
static LLVMValueRef lrc_enum_rc_fn(Node *ed, int is_release);

/* struct или sum enum helper по име на типа (единствено име за двата —
 * struct и enum с едно име не могат да съществуват в една програма) */
LLVMValueRef llv_lrc_rc_fn(const char *type_name, int is_release) {
    Node *ed = llv_find_sum_enum(type_name);
    if (ed) return lrc_enum_rc_fn(ed, is_release);
    return lrc_struct_rc_fn(type_name, is_release);
}

/* M24-RC: resolved тип на поле в контекста на instantiated generic struct
 * стойност st: типова променлива (`v: T`) → targ на инстанцията; вложено
 * instantiated generic поле (`inner: Pair<i64>`) → checked типът на възела
 * (носи targs). Иначе NULL — node логиката (lrc_field_rc). Огледало на
 * rc_fld_inst_type (codegen_c). */
Type *llv_lrc_fld_inst_type(Type *st, Node *fld_type) {
    Node *t = fld_type;
    while (t && (t->kind == NODE_TYPE_EFFECT || t->kind == NODE_TYPE_REF))
        t = t->inner_type;
    if (!t || t->kind != NODE_TYPE || !t->type_name) return NULL;
    Node *d = llv_find_struct_decl(st->name);
    if (d && d->n_struct_params > 0) {
        for (int a = 0; a < d->n_struct_params && a < st->n_targs; a++)
            if (strcmp(t->type_name, d->struct_params[a]) == 0)
                return st->targs[a];
        /* M26: Vec<T> / Map<…, T> поле с типова променлива вътре —
         * синтезирай resolved тип с конкретния elem (дълбок release на
         * вложени Vec-ове; като rc_fld_inst_type в codegen_c) */
        if (strcmp(t->type_name, "Vec") == 0 || strcmp(t->type_name, "Map") == 0) {
            Node *en = t->inner_type2 && strcmp(t->type_name, "Map") == 0
                     ? t->inner_type2 : t->inner_type;
            while (en && (en->kind == NODE_TYPE_EFFECT || en->kind == NODE_TYPE_REF))
                en = en->inner_type;
            if (en && en->kind == NODE_TYPE && en->type_name) {
                for (int a = 0; a < d->n_struct_params && a < st->n_targs; a++)
                    if (strcmp(en->type_name, d->struct_params[a]) == 0) {
                        Type *vt = calloc(1, sizeof(Type));
                        if (!vt) { fprintf(stderr, "baga: out of memory\n"); exit(1); }
                        vt->kind = strcmp(t->type_name, "Vec") == 0
                                 ? TYPE_VEC : TYPE_MAP;
                        vt->elem = st->targs[a];
                        return vt;
                    }
            }
        }
    }
    if (t->type && (t->type->kind == TYPE_STRUCT ||
                    t->type->kind == TYPE_ENUM) && t->type->n_targs > 0)
        return t->type;
    return NULL;
}

/* M24-RC: едно поле по неговия RESOLVED Type (инстанция на generic struct)
 * — огледало на lrc_field_rc, но tag/elem идват от Type, а вложените
 * struct/enum полета рекурсират през llv_lrc_rc_fn_ty (per-instance имена). */
static void lrc_field_rc_ty(Type *ft, LLVMValueRef v, int is_release) {
    int tag = llv_lrc_heap_tag(ft);
    if (tag == 1) {
        if (is_release) llv_h_call(llv_baga_rt("baga_rc_release_str"), &v, 1, "");
        else            llv_h_call(llv_baga_rt("baga_rc_retain"), &v, 1, "rcr");
        return;
    }
    if (tag == 2) {
        LLVMValueRef d = LLVMBuildExtractValue(lg.builder, v, 0, "fd");
        if (is_release) llv_h_call(llv_baga_rt("baga_rc_release_bytes"), &d, 1, "");
        else            llv_h_call(llv_baga_rt("baga_rc_retain"), &d, 1, "rcr");
        return;
    }
    if (tag == 3 || tag == 4) {
        if (!is_release) {
            LLVMValueRef p = LLVMBuildBitCast(lg.builder, v, lg.ptr_ty, "fcp");
            llv_h_call(llv_baga_rt("baga_rc_retain"), &p, 1, "rcr");
            return;
        }
        int k = tag == 3 ? llv_lrc_vec_elem_kind(ft, NULL)
                         : llv_lrc_map_val_tag(ft, NULL);
        LLVMValueRef rel = (tag == 3 && k == 3)
            ? llv_lrc_nested_vec_rel(ft, NULL)
            : llv_lrc_box_rel_ty(ft->elem);
        LLVMValueRef a[] = {
            LLVMBuildBitCast(lg.builder, v, lg.ptr_ty, "fcp"),
            LLVMConstInt(lg.i64_ty, (uint64_t)k, 0),
            LLVMConstInt(lg.i64_ty, 0, 0),
            rel,
        };
        llv_h_call(llv_baga_rt(tag == 3 ? "baga_rc_release_vec"
                                : "baga_rc_release_map"), a, 4, "");
        return;
    }
    if (tag == 5 || tag == 6)
        llv_h_call(llv_lrc_rc_fn_ty(ft, is_release), &v, 1, "");
    /* tag 0: не-heap поле (i64/f64/bool и пр.) — нищо */
}

/* static void baga_rc_{retain,release}_b_Box_i64(b_Box_i64 s)
 * { за всяко heap поле след resolve на типовите променливи: retain/release }
 * M24-RC: per-instance helper за instantiated generic struct. Тялото се
 * генерира при първа нужда и се кешира по име (като lrc_struct_rc_fn). */
LLVMValueRef llv_lrc_struct_inst_rc_fn(Type *t, int is_release) {
    char *cn = llv_llvm_struct_cname(t);            /* "Box_i64" */
    char *full = lrc_rc_fn_name(cn, is_release);
    free(cn);
    LLVMValueRef fn = LLVMGetNamedFunction(lg.mod, full);
    if (fn) { free(full); return fn; }
    Node *decl = llv_find_struct_decl(t->name);
    if (!decl) {
        char buf[256];
        snprintf(buf, sizeof buf, "RC helper за неизвестна структура '%s'",
                 t->name);
        free(full);
        llv_llvm_unsupported(buf);
    }
    LLVMTypeRef sty = llv_user_struct_ty_inst(t);
    /* регистрация ПРЕДИ тялото — рекурсивни препратки намират декларацията */
    fn = LLVMAddFunction(lg.mod, full,
        LLVMFunctionType(lg.void_ty, &sty, 1, 0));
    free(full);
    LLVMBasicBlockRef saved = LLVMGetInsertBlock(lg.builder);
    llv_h_begin(fn);
    LLVMValueRef s = LLVMGetParam(fn, 0);
    for (int i = 0; i < decl->fields.len; i++) {
        Node *ft = decl->fields.data[i]->fld_type;
        Type *rt = llv_lrc_fld_inst_type(t, ft);
        LLVMValueRef v = LLVMBuildExtractValue(lg.builder, s, (unsigned)i, "f");
        if (rt) lrc_field_rc_ty(rt, v, is_release);
        else    lrc_field_rc(ft, v, is_release);
    }
    LLVMBuildRetVoid(lg.builder);
    if (saved) LLVMPositionBuilderAtEnd(lg.builder, saved);
    return fn;
}

/* M24-RC: Type-aware dispatcher — instantiated generic struct (n_targs>0)
 * отива на per-instance helper; иначе по име (enum или обикновен struct) */
LLVMValueRef llv_lrc_rc_fn_ty(Type *t, int is_release) {
    if (t && t->kind == TYPE_STRUCT && t->n_targs > 0)
        return llv_lrc_struct_inst_rc_fn(t, is_release);
    if (!t || !t->name)
        llv_llvm_unsupported("RC helper за тип без име");
    return llv_lrc_rc_fn(t->name, is_release);
}

/* static void baga_rc_{retain,release}_<S>(b_<S> s)
 * { за всяко heap поле: retain/release (транзитивно) } */
static LLVMValueRef lrc_struct_rc_fn(const char *name, int is_release) {
    char *full = lrc_rc_fn_name(name, is_release);
    LLVMValueRef fn = LLVMGetNamedFunction(lg.mod, full);
    if (fn) { free(full); return fn; }
    Node *decl = llv_find_struct_decl(name);
    if (!decl) {
        char buf[256];
        snprintf(buf, sizeof buf, "RC helper за неизвестна структура '%s'", name);
        free(full);
        llv_llvm_unsupported(buf);
    }
    LLVMTypeRef sty = llv_user_struct_ty(name);
    /* регистрацията е ПРЕДИ тялото — рекурсивни struct/enum препратки
     * (S → Vec<E>, E → V(S)) намират вече декларираната функция */
    fn = LLVMAddFunction(lg.mod, full,
        LLVMFunctionType(lg.void_ty, &sty, 1, 0));
    free(full);
    LLVMBasicBlockRef saved = LLVMGetInsertBlock(lg.builder);
    llv_h_begin(fn);
    LLVMValueRef s = LLVMGetParam(fn, 0);
    for (int i = 0; i < decl->fields.len; i++) {
        Node *ft = decl->fields.data[i]->fld_type;
        LLVMValueRef v = LLVMBuildExtractValue(lg.builder, s, (unsigned)i, "f");
        lrc_field_rc(ft, v, is_release);
    }
    LLVMBuildRetVoid(lg.builder);
    if (saved) LLVMPositionBuilderAtEnd(lg.builder, saved);
    return fn;
}

/* static void baga_rc_{retain,release}_<E>(b_<E> e)
 * { switch (e.tag) { case j: retain/release на heap payload-а; } } */
static LLVMValueRef lrc_enum_rc_fn(Node *ed, int is_release) {
    char *full = lrc_rc_fn_name(ed->enum_name, is_release);
    LLVMValueRef fn = LLVMGetNamedFunction(lg.mod, full);
    if (fn) { free(full); return fn; }
    LLVMTypeRef ety = llv_user_struct_ty(ed->enum_name);
    fn = LLVMAddFunction(lg.mod, full,
        LLVMFunctionType(lg.void_ty, &ety, 1, 0));
    free(full);
    LLVMBasicBlockRef saved = LLVMGetInsertBlock(lg.builder);
    llv_h_begin(fn);
    LLVMValueRef e = LLVMGetParam(fn, 0);
    /* payload GEP иска адрес — spill на параметъра (като llv_sum_ctor_fn) */
    LLVMValueRef ea = LLVMBuildAlloca(lg.builder, ety, "ea");
    LLVMBuildStore(lg.builder, e, ea);
    LLVMValueRef tag = LLVMBuildExtractValue(lg.builder, e, 0, "tag");
    LLVMBasicBlockRef ret_bb =
        LLVMAppendBasicBlockInContext(lg.ctx, fn, "ret");
    int ncases = 0;
    for (int j = 0; j < ed->n_variants; j++) {
        Node *pt = ed->enum_payloads ? ed->enum_payloads[j] : NULL;
        if (pt && lrc_enum_payload_heap(pt)) ncases++;
    }
    LLVMValueRef sw = LLVMBuildSwitch(lg.builder, tag, ret_bb,
                                      (unsigned)ncases);
    for (int j = 0; j < ed->n_variants; j++) {
        Node *pt = ed->enum_payloads ? ed->enum_payloads[j] : NULL;
        if (!pt || !lrc_enum_payload_heap(pt)) continue;
        char cnm[32];
        snprintf(cnm, sizeof cnm, "v%d", j);
        LLVMBasicBlockRef cbb =
            LLVMAppendBasicBlockInContext(lg.ctx, fn, cnm);
        LLVMAddCase(sw, LLVMConstInt(lg.i64_ty, (unsigned long long)j, 0),
                    cbb);
        LLVMPositionBuilderAtEnd(lg.builder, cbb);
        LLVMTypeRef pty = llv_llvm_type(pt);
        LLVMValueRef pv = LLVMBuildLoad2(lg.builder, pty,
            llv_sum_payload_ptr(ety, ea, pty), "pv");
        lrc_field_rc(pt, pv, is_release);
        LLVMBuildBr(lg.builder, ret_bb);
    }
    LLVMPositionBuilderAtEnd(lg.builder, ret_bb);
    LLVMBuildRetVoid(lg.builder);
    if (saved) LLVMPositionBuilderAtEnd(lg.builder, saved);
    return fn;
}

/* static void baga_rc_relf_<S>(i8 *p) { release_<S>(load (b_<S> *)p); }
 * Destructor за box елементи/стойности във Vec/Map (контейнерът не знае
 * типа статично) — огледало на baga_rc_relf_<S> в codegen_c. */
static LLVMValueRef lrc_relf_fn(const char *type_name) {
    char *m = llv_llvm_mangle(type_name);
    size_t cap = strlen(m) + 20;
    char *full = malloc(cap);
    if (!full) { fprintf(stderr, "baga: out of memory\n"); exit(1); }
    snprintf(full, cap, "baga_rc_relf_%s", m);
    free(m);
    LLVMValueRef fn = LLVMGetNamedFunction(lg.mod, full);
    if (fn) { free(full); return fn; }
    LLVMTypeRef sty = llv_user_struct_ty(type_name);
    fn = LLVMAddFunction(lg.mod, full,
        LLVMFunctionType(lg.void_ty, &lg.ptr_ty, 1, 0));
    free(full);
    LLVMBasicBlockRef saved = LLVMGetInsertBlock(lg.builder);
    llv_h_begin(fn);
    LLVMValueRef sp = LLVMBuildBitCast(lg.builder, LLVMGetParam(fn, 0),
        LLVMPointerType(sty, 0), "sp");
    LLVMValueRef v = LLVMBuildLoad2(lg.builder, sty, sp, "v");
    llv_h_call(llv_lrc_rc_fn(type_name, 1), &v, 1, "");
    LLVMBuildRetVoid(lg.builder);
    if (saved) LLVMPositionBuilderAtEnd(lg.builder, saved);
    return fn;
}

/* static void baga_rc_relv_<S>(i8 *p)
 * { baga_rc_release_vec(p, 2, 0, baga_rc_relf_<S>); }
 * Shim за Vec<S> като елемент на външен Vec (kind 3 на външния не знае
 * елементния тип на вложения) — огледало на codegen_c.c:4651. */
static LLVMValueRef lrc_relv_fn(const char *type_name) {
    char *m = llv_llvm_mangle(type_name);
    size_t cap = strlen(m) + 20;
    char *full = malloc(cap);
    if (!full) { fprintf(stderr, "baga: out of memory\n"); exit(1); }
    snprintf(full, cap, "baga_rc_relv_%s", m);
    free(m);
    LLVMValueRef fn = LLVMGetNamedFunction(lg.mod, full);
    if (fn) { free(full); return fn; }
    fn = LLVMAddFunction(lg.mod, full,
        LLVMFunctionType(lg.void_ty, &lg.ptr_ty, 1, 0));
    free(full);
    LLVMBasicBlockRef saved = LLVMGetInsertBlock(lg.builder);
    llv_h_begin(fn);
    LLVMValueRef relf = LLVMBuildBitCast(lg.builder,
        lrc_relf_fn(type_name), lg.ptr_ty, "relf");
    LLVMValueRef a[] = {
        LLVMGetParam(fn, 0),
        LLVMConstInt(lg.i64_ty, 2, 0),
        LLVMConstInt(lg.i64_ty, 0, 0),
        relf,
    };
    llv_h_call(llv_baga_rt("baga_rc_release_vec"), a, 4, "");
    LLVMBuildRetVoid(lg.builder);
    if (saved) LLVMPositionBuilderAtEnd(lg.builder, saved);
    return fn;
}

/* box destructor за struct/enum елементен/стойностен тип с heap полета —
 * relf shim-ът като функция (NULL → няма heap полета). Порт на rc_box_rel
 * (codegen_c.c:410); llv_lrc_box_rel е i8* обвивката за container аргументите. */
static LLVMValueRef lrc_box_rel_fn(const char *type_name) {
    if (!type_name) return NULL;
    int heap = llv_lrc_struct_has_heap(type_name);
    if (!heap) {
        Node *ed = llv_find_sum_enum(type_name);
        heap = ed && llv_lrc_enum_has_heap(ed);
    }
    if (!heap) return NULL;
    return lrc_relf_fn(type_name);
}

LLVMValueRef llv_lrc_box_rel(const char *type_name) {
    LLVMValueRef fn = lrc_box_rel_fn(type_name);
    if (!fn) return LLVMConstNull(lg.ptr_ty);
    return LLVMBuildBitCast(lg.builder, fn, lg.ptr_ty, "relf");
}

/* M24-RC: relf shim за instantiated generic struct — baga_rc_relf_b_Box_i64
 * (i8 *p), вика per-instance release helper-а. Огледало на lrc_relf_fn. */
static LLVMValueRef lrc_relf_fn_ty(Type *t) {
    char *cn = llv_llvm_struct_cname(t);
    char *m = llv_llvm_mangle(cn);
    size_t cap = strlen(m) + 20;
    char *full = malloc(cap);
    if (!full) { fprintf(stderr, "baga: out of memory\n"); exit(1); }
    snprintf(full, cap, "baga_rc_relf_%s", m);
    free(m);
    LLVMValueRef fn = LLVMGetNamedFunction(lg.mod, full);
    if (fn) { free(full); free(cn); return fn; }
    LLVMTypeRef sty = llv_user_struct_ty(cn);
    free(cn);
    fn = LLVMAddFunction(lg.mod, full,
        LLVMFunctionType(lg.void_ty, &lg.ptr_ty, 1, 0));
    free(full);
    LLVMBasicBlockRef saved = LLVMGetInsertBlock(lg.builder);
    llv_h_begin(fn);
    LLVMValueRef sp = LLVMBuildBitCast(lg.builder, LLVMGetParam(fn, 0),
        LLVMPointerType(sty, 0), "sp");
    LLVMValueRef v = LLVMBuildLoad2(lg.builder, sty, sp, "v");
    llv_h_call(llv_lrc_struct_inst_rc_fn(t, 1), &v, 1, "");
    LLVMBuildRetVoid(lg.builder);
    if (saved) LLVMPositionBuilderAtEnd(lg.builder, saved);
    return fn;
}

/* M24-RC: Type-aware box destructor — instantiated generic struct елемент/
 * стойност (n_targs>0) получава per-instance relf, само ако инстанцията
 * реално има heap полета (иначе NULL → само free, leak-safe като C) */
LLVMValueRef llv_lrc_box_rel_fn_ty(Type *et) {
    if (et && et->kind == TYPE_STRUCT && et->n_targs > 0)
        return llv_lrc_heap_tag(et) == 5 ? lrc_relf_fn_ty(et) : NULL;
    return lrc_box_rel_fn(et && et->name ? et->name : NULL);
}

LLVMValueRef llv_lrc_box_rel_ty(Type *et) {
    LLVMValueRef fn = llv_lrc_box_rel_fn_ty(et);
    if (!fn) return LLVMConstNull(lg.ptr_ty);
    return LLVMBuildBitCast(lg.builder, fn, lg.ptr_ty, "relf");
}

/* destructor за kind 3 (вложен Vec) елементи — relv shim, когато елементът
 * е Vec<S> и S е struct с heap полета; иначе NULL (старата граница:
 * вътрешният vec се release-ва като kind 0 — leak-safe, като codegen_c).
 * Порт на rc_nested_vec_rel_type + _node (inferred Type първо). */
/* M26: enc на елементния тип за relv shim име: str / bytes / mangled struct
 * име (b_Wrap; per-instance b_Box_i64) / v_<enc> за Vec — като
 * rc_relv_enc в codegen_c (имената съвпадат между бекендите). */
static char *lrc_relv_enc(Type *e) {
    if (!e) return NULL;
    if (e->kind == TYPE_STR)   return strdup("str");
    if (e->kind == TYPE_BYTES) return strdup("bytes");
    if ((e->kind == TYPE_STRUCT || e->kind == TYPE_ENUM) && e->name) {
        if (e->kind == TYPE_STRUCT && e->n_targs > 0) {
            char *cn = llv_llvm_struct_cname(e);
            char *m = llv_llvm_mangle(cn);
            free(cn);
            return m;
        }
        return llv_llvm_mangle(e->name);
    }
    if (e->kind == TYPE_VEC) {
        char *sub = lrc_relv_enc(e->elem);
        if (!sub) return NULL;
        size_t cap = strlen(sub) + 3;
        char *out = malloc(cap);
        if (!out) { fprintf(stderr, "baga: out of memory\n"); exit(1); }
        snprintf(out, cap, "v_%s", sub);
        free(sub);
        return out;
    }
    return NULL;
}

/* M26 + RC5 v1.1: трябва ли shim за Vec<E> като елемент на външен Vec —
 * E носи heap някъде в дълбочина (str/bytes/struct/enum/Vec рекурсивно). */
static int lrc_vec_elem_deep_heap(Type *e) {
    if (!e) return 0;
    switch (e->kind) {
        case TYPE_STR: case TYPE_BYTES: return 1;
        case TYPE_VEC: return lrc_vec_elem_deep_heap(e->elem);
        case TYPE_STRUCT: return llv_lrc_heap_tag(e) == 5;   /* instance-aware */
        case TYPE_ENUM: return llv_lrc_heap_tag(e) == 6;
        default: return 0;
    }
}

static LLVMValueRef lrc_relf_fn_ty(Type *t);

/* M26: baga_rc_relv_<enc(E)>(i8 *p) { baga_rc_release_vec(p, kind, 0, rel); }
 * за E = str/bytes/Vec<…>/instantiated generic struct — lazy, кеширан по
 * име (като останалите helper-и). Обикновен struct/enum → lrc_relv_fn. */
static LLVMValueRef lrc_relv_fn_ty(Type *es) {
    if (es && (es->kind == TYPE_STRUCT || es->kind == TYPE_ENUM) &&
        !(es->kind == TYPE_STRUCT && es->n_targs > 0))
        return lrc_relv_fn(es->name);
    char *enc = lrc_relv_enc(es);
    if (!enc) llv_llvm_unsupported("relv shim за неизвестен елементен тип");
    size_t cap = strlen(enc) + 20;
    char *full = malloc(cap);
    if (!full) { fprintf(stderr, "baga: out of memory\n"); exit(1); }
    snprintf(full, cap, "baga_rc_relv_%s", enc);
    free(enc);
    LLVMValueRef fn = LLVMGetNamedFunction(lg.mod, full);
    if (fn) { free(full); return fn; }
    fn = LLVMAddFunction(lg.mod, full,
        LLVMFunctionType(lg.void_ty, &lg.ptr_ty, 1, 0));
    free(full);
    int kind;
    /* под-shim-ът се генерира ПРЕДИ тялото (възстановява позицията сам);
     * bitcast-ът към него е инструкция ВЪТРЕ в тялото на този shim */
    LLVMValueRef sub = NULL;
    if (es->kind == TYPE_STR)        kind = 1;
    else if (es->kind == TYPE_BYTES) kind = 4;
    else if (es->kind == TYPE_VEC) { kind = 3; sub = lrc_relv_fn_ty(es->elem); }
    else { kind = 2; sub = lrc_relf_fn_ty(es); }   /* instantiated generic struct */
    LLVMBasicBlockRef saved = LLVMGetInsertBlock(lg.builder);
    llv_h_begin(fn);
    LLVMValueRef rel = sub ? LLVMBuildBitCast(lg.builder, sub, lg.ptr_ty, "relf")
                           : LLVMConstNull(lg.ptr_ty);
    LLVMValueRef a[] = {
        LLVMGetParam(fn, 0),
        LLVMConstInt(lg.i64_ty, (uint64_t)kind, 0),
        LLVMConstInt(lg.i64_ty, 0, 0),
        rel,
    };
    llv_h_call(llv_baga_rt("baga_rc_release_vec"), a, 4, "");
    LLVMBuildRetVoid(lg.builder);
    if (saved) LLVMPositionBuilderAtEnd(lg.builder, saved);
    return fn;
}

/* destructor за kind 3 (вложен Vec) елементи — relv shim, когато елементът
 * е Vec<E> и E носи heap в дълбочина (struct с heap полета — v0.9;
 * str/bytes/по-дълбока вложеност — M26, огледало на codegen_c); иначе NULL
 * (runtime-ът release-ва вътрешния с kind 0 — leak-safe граница). */
LLVMValueRef llv_lrc_nested_vec_rel(Type *vty, Node *tn) {
    /* M26: checked типът на възела печели, когато inferred няма elem
     * (vec_new) — конкретен след recheck на инстанция (M21) */
    if (tn && tn->type && tn->type->kind == TYPE_VEC &&
        (!vty || vty->kind != TYPE_VEC || !vty->elem))
        vty = tn->type;
    if (vty && vty->elem && vty->elem->kind == TYPE_VEC && vty->elem->elem) {
        Type *es = vty->elem->elem;
        if (lrc_vec_elem_deep_heap(es))
            return LLVMBuildBitCast(lg.builder, lrc_relv_fn_ty(es),
                                    lg.ptr_ty, "relv");
        return LLVMConstNull(lg.ptr_ty);
    }
    const char *sn = NULL;
    if (vty && vty->elem && vty->elem->kind == TYPE_VEC &&
        vty->elem->elem && vty->elem->elem->kind == TYPE_STRUCT)
        sn = vty->elem->elem->name;
    if (!sn && tn) {
        Node *inner = tn->inner_type;
        if (inner && inner->kind == NODE_TYPE && inner->type_name &&
            strcmp(inner->type_name, "Vec") == 0) {
            Node *es = inner->inner_type;
            if (es && es->kind == NODE_TYPE) sn = es->type_name;
        }
    }
    if (!sn || !llv_lrc_struct_has_heap(sn)) return LLVMConstNull(lg.ptr_ty);
    return LLVMBuildBitCast(lg.builder, lrc_relv_fn(sn), lg.ptr_ty, "relv");
}

/* lazy dispatcher: връща helper-а, генерирайки тялото му при първа употреба */
LLVMValueRef llv_baga_rt(const char *name) {
    LLVMValueRef fn = LLVMGetNamedFunction(lg.mod, name);
    if (fn) return fn;
    LLVMBasicBlockRef saved = LLVMGetInsertBlock(lg.builder);
    if      (strcmp(name, "baga_len") == 0)         fn = llv_build_baga_len();
    else if (strcmp(name, "baga_char_at") == 0)     fn = llv_build_baga_char_at();
    else if (strcmp(name, "baga_byte_at") == 0)     fn = llv_build_baga_byte_at();
    else if (strcmp(name, "baga_substr") == 0)      fn = llv_build_baga_substr();
    else if (strcmp(name, "baga_concat") == 0)      fn = llv_build_baga_concat();
    else if (strcmp(name, "baga_str_eq") == 0)      fn = llv_build_baga_str_eq();
    else if (strcmp(name, "baga_chr") == 0)         fn = llv_build_baga_chr();
    else if (strcmp(name, "baga_ord") == 0)         fn = llv_build_baga_ord();
    else if (strcmp(name, "baga_read_file") == 0)   fn = llv_build_baga_read_file();
    else if (strcmp(name, "baga_vec_grow") == 0)    fn = llv_build_baga_vec_grow();
    else if (strcmp(name, "baga_vec_new") == 0)     fn = llv_build_baga_vec_new();
    else if (strcmp(name, "baga_vec_push_i64") == 0) fn = llv_build_baga_vec_push_i64();
    else if (strcmp(name, "baga_vec_push_str") == 0) fn = llv_build_baga_vec_push_str();
    else if (strcmp(name, "baga_vec_get_i64") == 0) fn = llv_build_baga_vec_get_i64();
    else if (strcmp(name, "baga_vec_get_str") == 0) fn = llv_build_baga_vec_get_str();
    else if (strcmp(name, "baga_vec_set_i64") == 0) fn = llv_build_baga_vec_set_i64();
    else if (strcmp(name, "baga_vec_set_str") == 0) fn = llv_build_baga_vec_set_str();
    else if (strcmp(name, "baga_vec_push_f64") == 0) fn = llv_build_baga_vec_push_f64();
    else if (strcmp(name, "baga_vec_get_f64") == 0) fn = llv_build_baga_vec_get_f64();
    else if (strcmp(name, "baga_vec_set_f64") == 0) fn = llv_build_baga_vec_set_f64();
    else if (strcmp(name, "baga_vec_len") == 0)     fn = llv_build_baga_vec_len();
    else if (strcmp(name, "baga_vec_push_box") == 0) fn = llv_build_baga_vec_push_box();
    else if (strcmp(name, "baga_vec_get_box") == 0) fn = llv_build_baga_vec_get_box();
    else if (strcmp(name, "baga_vec_set_box") == 0) fn = llv_build_baga_vec_set_box();
    else if (strcmp(name, "baga_vec_slice_box") == 0) fn = llv_build_baga_vec_slice_box();
    else if (strcmp(name, "baga_vec_concat_box") == 0) fn = llv_build_baga_vec_concat_box();
    else if (strcmp(name, "baga_vec_slice_i64") == 0) fn = llv_build_baga_vec_slice("i64", "baga_vec_push_i64");
    else if (strcmp(name, "baga_vec_slice_str") == 0) fn = llv_build_baga_vec_slice("str", "baga_vec_push_str");
    else if (strcmp(name, "baga_vec_slice_f64") == 0) fn = llv_build_baga_vec_slice("f64", "baga_vec_push_f64");
    else if (strcmp(name, "baga_vec_concat_i64") == 0) fn = llv_build_baga_vec_concat("i64", "baga_vec_push_i64");
    else if (strcmp(name, "baga_vec_concat_str") == 0) fn = llv_build_baga_vec_concat("str", "baga_vec_push_str");
    else if (strcmp(name, "baga_vec_concat_f64") == 0) fn = llv_build_baga_vec_concat("f64", "baga_vec_push_f64");
    else if (strcmp(name, "baga_bytes_len") == 0)   fn = llv_build_baga_bytes_len();
    else if (strcmp(name, "baga_bytes_at") == 0)    fn = llv_build_baga_bytes_at();
    else if (strcmp(name, "baga_bytes_slice") == 0) fn = llv_build_baga_bytes_slice();
    else if (strcmp(name, "baga_bytes_concat") == 0) fn = llv_build_baga_bytes_concat();
    else if (strcmp(name, "baga_str_h") == 0)      fn = llv_build_baga_str_h();
    else if (strcmp(name, "baga_h_str") == 0)      fn = llv_build_baga_h_str();
    else if (strcmp(name, "baga_bytes_put") == 0)  fn = llv_build_baga_bytes_put();
    else if (strcmp(name, "baga_bytes_h") == 0)    fn = llv_build_baga_bytes_h();
    else if (strcmp(name, "baga_h_bytes") == 0)    fn = llv_build_baga_h_bytes();
    else if (strcmp(name, "baga_bounds_fail") == 0) fn = llv_build_baga_bounds_fail();
    else if (strcmp(name, "baga_bytes_new") == 0)  fn = llv_build_baga_bytes_new();
    else if (strcmp(name, "baga_bytes_set") == 0)  fn = llv_build_baga_bytes_set();
    else if (strcmp(name, "baga_bytes_push") == 0) fn = llv_build_baga_bytes_push();
    else if (strcmp(name, "baga_bytes_from_str") == 0) fn = llv_build_baga_bytes_from_str();
    else if (strcmp(name, "baga_bytes_to_str") == 0) fn = llv_build_baga_bytes_to_str();
    else if (strcmp(name, "baga_hex_val") == 0)     fn = llv_build_baga_hex_val();
    else if (strcmp(name, "baga_hex_encode") == 0)  fn = llv_build_baga_hex_encode();
    else if (strcmp(name, "baga_hex_decode") == 0)  fn = llv_build_baga_hex_decode();
    else if (strcmp(name, "baga_bytes_from_hex") == 0) fn = llv_build_baga_bytes_from_hex();
    else if (strcmp(name, "baga_arena_new") == 0)   fn = llv_build_baga_arena_new();
    else if (strcmp(name, "baga_arena_alloc") == 0) fn = llv_build_baga_arena_alloc();
    else if (strcmp(name, "baga_arena_reset") == 0) fn = llv_build_baga_arena_reset();
    else if (strcmp(name, "baga_arena_free") == 0)  fn = llv_build_baga_arena_free();
    else if (strcmp(name, "baga_arg_count") == 0)   fn = llv_build_baga_arg_count();
    else if (strcmp(name, "baga_arg") == 0)         fn = llv_build_baga_arg();
    else if (strcmp(name, "baga_exit") == 0)        fn = llv_build_baga_exit();
    else if (strcmp(name, "baga_eprintln") == 0)    fn = llv_build_baga_eprintln();
    else if (strcmp(name, "baga_i64_to_str") == 0)  fn = llv_build_baga_i64_to_str();
    else if (strcmp(name, "baga_f64_to_str") == 0)  fn = llv_build_baga_f64_to_str();
    else if (strcmp(name, "baga_str_len") == 0)     fn = llv_build_baga_str_len();
    else if (strcmp(name, "baga_set_slen") == 0)    fn = llv_build_baga_set_slen();
    else if (strcmp(name, "baga_rc_hdr") == 0)      fn = llv_build_baga_rc_hdr();
    else if (strcmp(name, "baga_rc_alloc") == 0)    fn = llv_build_baga_rc_alloc();
    else if (strcmp(name, "baga_rc_retain") == 0)   fn = llv_build_baga_rc_retain();
    else if (strcmp(name, "baga_rc_release_str") == 0)
        fn = llv_build_baga_rc_release("baga_rc_release_str",
            "baga: rc underflow (str — двоен release)\n");
    else if (strcmp(name, "baga_rc_release_bytes") == 0)
        fn = llv_build_baga_rc_release("baga_rc_release_bytes",
            "baga: rc underflow (bytes — двоен release)\n");
    else if (strcmp(name, "baga_rc_release_vec") == 0)
        fn = build_baga_rc_release_vec();
    else if (strcmp(name, "baga_rc_release_map") == 0)
        fn = build_baga_rc_release_map();
    else if (strcmp(name, "baga_drop_str") == 0)   fn = build_baga_drop_str();
    else if (strcmp(name, "baga_drop_bytes") == 0) fn = build_baga_drop_bytes();
    else if (strcmp(name, "baga_drop_vec") == 0)   fn = build_baga_drop_vec();
    else if (strcmp(name, "baga_drop_map") == 0)   fn = build_baga_drop_map();
    else if (strcmp(name, "baga_map_hash_str") == 0) fn = llv_build_baga_map_hash_str();
    else if (strcmp(name, "baga_map_hash_i64") == 0) fn = llv_build_baga_map_hash_i64();
    else if (strcmp(name, "baga_map_hash_bytes") == 0) fn = llv_build_baga_map_hash_bytes();
    else if (strcmp(name, "baga_map_new") == 0)     fn = llv_build_baga_map_new();
    else if (strcmp(name, "baga_map_len") == 0)     fn = llv_build_baga_map_len();
    else if (strcmp(name, "baga_map_slot") == 0)    fn = llv_build_baga_map_slot();
    else if (strcmp(name, "baga_map_slot_b") == 0)  fn = llv_build_baga_map_slot_b();
    else if (strcmp(name, "baga_map_put") == 0)     fn = llv_build_baga_map_put();
    else if (strcmp(name, "baga_map_put_b") == 0)   fn = llv_build_baga_map_put_b();
    else if (strcmp(name, "baga_map_rehash") == 0)  fn = llv_build_baga_map_rehash();
    else if (strcmp(name, "baga_map_h") == 0)       fn = llv_build_baga_map_h();
    else if (strcmp(name, "baga_h_map") == 0)       fn = llv_build_baga_h_map();
    /* typed ops: baga_map_{set,get}_<key>_<val> — суфиксите се декодират */
    else if (strncmp(name, "baga_map_set_", 13) == 0 ||
             strncmp(name, "baga_map_get_", 13) == 0) {
        int klen = 0, key, val;
        key = llv_map_key_kind(name + 13, &klen);
        val = key >= 0 && name[13 + klen] == '_'
            ? llv_map_val_kind(name + 13 + klen + 1) : -1;
        if (key < 0 || val < 0) {
            char buf[128];
            snprintf(buf, sizeof buf, "runtime helper '%s'", name);
            llv_llvm_unsupported(buf);
        }
        fn = name[9] == 's' ? llv_build_baga_map_set(name, key, val)
                            : llv_build_baga_map_get(name, key, val);
    }
    else if (strncmp(name, "baga_map_has_", 13) == 0 ||
             strncmp(name, "baga_map_del_", 13) == 0) {
        int klen = 0, key = llv_map_key_kind(name + 13, &klen);
        if (key < 0 || name[13 + klen] != '\0') {
            char buf[128];
            snprintf(buf, sizeof buf, "runtime helper '%s'", name);
            llv_llvm_unsupported(buf);
        }
        fn = name[9] == 'h' ? llv_build_baga_map_has(name, key)
                            : llv_build_baga_map_del(name, key);
    }
    else if (strncmp(name, "baga_map_keys_", 14) == 0) {
        int klen = 0, key = llv_map_key_kind(name + 14, &klen);
        if (key < 0 || name[14 + klen] != '\0') {
            char buf[128];
            snprintf(buf, sizeof buf, "runtime helper '%s'", name);
            llv_llvm_unsupported(buf);
        }
        fn = llv_build_baga_map_keys(name, key);
    }
    else {
        char buf[128];
        snprintf(buf, sizeof buf, "runtime helper '%s'", name);
        llv_llvm_unsupported(buf);
    }
    if (saved) LLVMPositionBuilderAtEnd(lg.builder, saved);
    return fn;
}

#endif /* BAGA_LLVM */
