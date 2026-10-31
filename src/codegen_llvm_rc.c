/* codegen_llvm_rc.c — част от codegen_llvm.c (виж codegen_llvm_impl.h). */
#include "baga.h"
#include "codegen_llvm_impl.h"

#ifdef BAGA_LLVM

/* ============================ RC runtime (--rc) ============================

 *
 * 32 B header пред payload-а: { i64 magic, i64 pe, i64 rc, i64 an } —
 * същият layout като baga_Hdr в codegen_c (magic = 0xBA6A4D454D344848).
 * rc = 1 при конструкция; rc = 0 → free на базата (payload − 32).
 * pe = 0 винаги в LLVM v1 (няма persist epoch). Header-ът е ПРЕД върнатия
 * указател, така че payload offset-ите в helper-ите не се променят при
 * превключване llv_rt_malloc → baga_rc_alloc. */


/* GEP до i64 поле idx на header базата (base е i8* към magic) */
LLVMValueRef llv_rc_hdr_field(LLVMValueRef base, int idx, const char *nm) {
    LLVMValueRef h = LLVMBuildBitCast(lg.builder, base,
        LLVMPointerType(lg.i64_ty, 0), "hc");
    LLVMValueRef i = LLVMConstInt(lg.i64_ty, (uint64_t)idx, 0);
    return LLVMBuildGEP2(lg.builder, lg.i64_ty, h, &i, 1, nm);
}

/* baga_rc_hdr: page guard + magic check (LLVM няма arena range guard).
 * Четем p-32 само при page offset >= 32; иначе NULL (immortal). */
LLVMValueRef llv_build_baga_rc_hdr(void) {
    LLVMTypeRef p[] = { lg.ptr_ty };
    LLVMValueRef fn = LLVMAddFunction(lg.mod, "baga_rc_hdr",
        LLVMFunctionType(lg.ptr_ty, p, 1, 0));
    llv_h_begin(fn);
    LLVMValueRef ptr = LLVMGetParam(fn, 0);
    LLVMValueRef pi = LLVMBuildPtrToInt(lg.builder, ptr, lg.i64_ty, "pi");
    LLVMValueRef off = LLVMBuildAnd(lg.builder, pi,
        LLVMConstInt(lg.i64_ty, 4095, 0), "off");
    LLVMValueRef low = LLVMBuildICmp(lg.builder, LLVMIntULT, off,
        LLVMConstInt(lg.i64_ty, BAGA_RC_HSIZE, 0), "low");
    LLVMBasicBlockRef null_bb = LLVMAppendBasicBlockInContext(lg.ctx, fn, "null");
    LLVMBasicBlockRef chk_bb  = LLVMAppendBasicBlockInContext(lg.ctx, fn, "chk");
    LLVMBasicBlockRef hit_bb  = LLVMAppendBasicBlockInContext(lg.ctx, fn, "hit");
    LLVMBuildCondBr(lg.builder, low, null_bb, chk_bb);

    LLVMPositionBuilderAtEnd(lg.builder, chk_bb);
    LLVMValueRef back = LLVMConstInt(lg.i64_ty, (uint64_t)-BAGA_RC_HSIZE, 1);
    LLVMValueRef base = LLVMBuildGEP2(lg.builder, lg.i8_ty, ptr, &back, 1, "base");
    LLVMValueRef magic = LLVMBuildLoad2(lg.builder, lg.i64_ty,
        llv_rc_hdr_field(base, 0, "mp"), "magic");
    LLVMValueRef ok = LLVMBuildICmp(lg.builder, LLVMIntEQ, magic,
        LLVMConstInt(lg.i64_ty, BAGA_RC_MAGIC, 0), "ok");
    LLVMBuildCondBr(lg.builder, ok, hit_bb, null_bb);

    LLVMPositionBuilderAtEnd(lg.builder, hit_bb);
    LLVMBuildRet(lg.builder, base);

    LLVMPositionBuilderAtEnd(lg.builder, null_bb);
    LLVMBuildRet(lg.builder, LLVMConstNull(lg.ptr_ty));
    return fn;
}

/* static i8 *baga_rc_alloc(i64 n)
 * { base = malloc(n + 32); пише header; return base + 32; } */
LLVMValueRef llv_build_baga_rc_alloc(void) {
    LLVMTypeRef p[] = { lg.i64_ty };
    LLVMValueRef fn = LLVMAddFunction(lg.mod, "baga_rc_alloc",
        LLVMFunctionType(lg.ptr_ty, p, 1, 0));
    llv_h_begin(fn);
    LLVMValueRef n = LLVMGetParam(fn, 0);
    LLVMValueRef nn = LLVMBuildAdd(lg.builder, n,
        LLVMConstInt(lg.i64_ty, BAGA_RC_HSIZE, 0), "nn");
    LLVMValueRef base = llv_h_call(llv_rt_malloc(), &nn, 1, "base");
    LLVMBuildStore(lg.builder, LLVMConstInt(lg.i64_ty, BAGA_RC_MAGIC, 0),
        llv_rc_hdr_field(base, 0, "mp"));
    LLVMBuildStore(lg.builder, LLVMConstInt(lg.i64_ty, 0, 0),
        llv_rc_hdr_field(base, 1, "pep"));
    LLVMBuildStore(lg.builder, LLVMConstInt(lg.i64_ty, 1, 0),
        llv_rc_hdr_field(base, 2, "rcp"));
    LLVMBuildStore(lg.builder, n, llv_rc_hdr_field(base, 3, "anp"));
    LLVMValueRef fwd = LLVMConstInt(lg.i64_ty, BAGA_RC_HSIZE, 0);
    LLVMBuildRet(lg.builder,
        LLVMBuildGEP2(lg.builder, lg.i8_ty, base, &fwd, 1, "pay"));
    return fn;
}

/* STR-1: кеширана дължина на низ в pe полето на header-а (бит 1 STR
 * флаг, битове 2..63 len<<2). Работи и в двата режима: str сайтовете
 * алокират с header (llv_str_alloc_call → baga_rc_alloc), литерали/външни
 * буфери са без header → baga_rc_hdr = NULL → strlen fallback.
 * ИНВАРИАНТ: slen == strlen(s) — str е C низ и NUL го съкращава
 * (bytes_to_str/read_file на двоичен вход), иначе LLVM-оракулът лови. */
LLVMValueRef llv_build_baga_str_len(void) {
    LLVMTypeRef p[] = { lg.ptr_ty };
    LLVMValueRef fn = LLVMAddFunction(lg.mod, "baga_str_len",
        LLVMFunctionType(lg.i64_ty, p, 1, 0));
    llv_h_begin(fn);
    LLVMValueRef s = LLVMGetParam(fn, 0);
    LLVMValueRef h = llv_h_call(llv_baga_rt("baga_rc_hdr"), &s, 1, "h");
    LLVMValueRef isn = LLVMBuildIsNull(lg.builder, h, "isn");
    LLVMBasicBlockRef slow_bb = LLVMAppendBasicBlockInContext(lg.ctx, fn, "slow");
    LLVMBasicBlockRef chk_bb  = LLVMAppendBasicBlockInContext(lg.ctx, fn, "chk");
    LLVMBasicBlockRef hit_bb  = LLVMAppendBasicBlockInContext(lg.ctx, fn, "hit");
    LLVMBuildCondBr(lg.builder, isn, slow_bb, chk_bb);

    LLVMPositionBuilderAtEnd(lg.builder, chk_bb);
    LLVMValueRef pe = LLVMBuildLoad2(lg.builder, lg.i64_ty,
        llv_rc_hdr_field(h, 1, "pep"), "pe");
    LLVMValueRef strb = LLVMBuildAnd(lg.builder, pe,
        LLVMConstInt(lg.i64_ty, 2, 0), "strb");
    LLVMValueRef has = LLVMBuildICmp(lg.builder, LLVMIntNE, strb,
        LLVMConstInt(lg.i64_ty, 0, 0), "has");
    LLVMBuildCondBr(lg.builder, has, hit_bb, slow_bb);

    LLVMPositionBuilderAtEnd(lg.builder, hit_bb);
    LLVMBuildRet(lg.builder, LLVMBuildLShr(lg.builder, pe,
        LLVMConstInt(lg.i64_ty, 2, 0), "n"));

    LLVMPositionBuilderAtEnd(lg.builder, slow_bb);
    LLVMValueRef a2[] = { s };
    LLVMBuildRet(lg.builder, llv_h_call(llv_rt_strlen(), a2, 1, "sn"));
    return fn;
}

LLVMValueRef llv_build_baga_set_slen(void) {
    LLVMTypeRef p[] = { lg.ptr_ty, lg.i64_ty };
    LLVMValueRef fn = LLVMAddFunction(lg.mod, "baga_set_slen",
        LLVMFunctionType(lg.void_ty, p, 2, 0));
    llv_h_begin(fn);
    LLVMValueRef s = LLVMGetParam(fn, 0);
    LLVMValueRef n = LLVMGetParam(fn, 1);
    LLVMValueRef h = llv_h_call(llv_baga_rt("baga_rc_hdr"), &s, 1, "h");
    LLVMValueRef isn = LLVMBuildIsNull(lg.builder, h, "isn");
    LLVMBasicBlockRef set_bb = LLVMAppendBasicBlockInContext(lg.ctx, fn, "set");
    LLVMBasicBlockRef ret_bb = LLVMAppendBasicBlockInContext(lg.ctx, fn, "ret");
    LLVMBuildCondBr(lg.builder, isn, ret_bb, set_bb);

    LLVMPositionBuilderAtEnd(lg.builder, set_bb);
    LLVMValueRef pep = llv_rc_hdr_field(h, 1, "pep");
    LLVMValueRef pe = LLVMBuildLoad2(lg.builder, lg.i64_ty, pep, "pe");
    LLVMValueRef n2 = LLVMBuildShl(lg.builder, n,
        LLVMConstInt(lg.i64_ty, 2, 0), "n2");
    LLVMValueRef v = LLVMBuildOr(lg.builder,
        LLVMBuildOr(lg.builder,
            LLVMBuildAnd(lg.builder, pe, LLVMConstInt(lg.i64_ty, 1, 0), "p0"),
            n2, "o1"),
        LLVMConstInt(lg.i64_ty, 2, 0), "v");
    LLVMBuildStore(lg.builder, v, pep);
    LLVMBuildBr(lg.builder, ret_bb);

    LLVMPositionBuilderAtEnd(lg.builder, ret_bb);
    LLVMBuildRetVoid(lg.builder);
    return fn;
}

/* baga_rc_retain: rc++ ако header-ът е валиден; връща p. C литералите и
 * външните буфери са „immortal" (hdr = NULL → no-op). */
LLVMValueRef llv_build_baga_rc_retain(void) {
    LLVMTypeRef p[] = { lg.ptr_ty };
    LLVMValueRef fn = LLVMAddFunction(lg.mod, "baga_rc_retain",
        LLVMFunctionType(lg.ptr_ty, p, 1, 0));
    llv_h_begin(fn);
    LLVMValueRef ptr = LLVMGetParam(fn, 0);
    LLVMValueRef h = llv_h_call(llv_baga_rt("baga_rc_hdr"), &ptr, 1, "h");
    LLVMValueRef isn = LLVMBuildIsNull(lg.builder, h, "isn");
    LLVMBasicBlockRef inc_bb = LLVMAppendBasicBlockInContext(lg.ctx, fn, "inc");
    LLVMBasicBlockRef ret_bb = LLVMAppendBasicBlockInContext(lg.ctx, fn, "ret");
    LLVMBuildCondBr(lg.builder, isn, ret_bb, inc_bb);

    LLVMPositionBuilderAtEnd(lg.builder, inc_bb);
    LLVMValueRef rcp = llv_rc_hdr_field(h, 2, "rcp");
    LLVMValueRef rc = LLVMBuildLoad2(lg.builder, lg.i64_ty, rcp, "rc");
    LLVMBuildStore(lg.builder, LLVMBuildAdd(lg.builder, rc,
        LLVMConstInt(lg.i64_ty, 1, 0), "rc1"), rcp);
    LLVMBuildBr(lg.builder, ret_bb);

    LLVMPositionBuilderAtEnd(lg.builder, ret_bb);
    LLVMBuildRet(lg.builder, ptr);
    return fn;
}

/* baga_rc_release_{str,bytes}: rc--; при 0 → free(base). Underflow
 * (rc вече 0) → съобщение на stderr + exit(1), не UB — същото
 * съобщение като codegen_c. */
LLVMValueRef llv_build_baga_rc_release(const char *name, const char *msg) {
    LLVMTypeRef p[] = { lg.ptr_ty };
    LLVMValueRef fn = LLVMAddFunction(lg.mod, name,
        LLVMFunctionType(lg.void_ty, p, 1, 0));
    llv_h_begin(fn);
    LLVMValueRef ptr = LLVMGetParam(fn, 0);
    LLVMValueRef h = llv_h_call(llv_baga_rt("baga_rc_hdr"), &ptr, 1, "h");
    LLVMValueRef isn = LLVMBuildIsNull(lg.builder, h, "isn");
    LLVMBasicBlockRef dec_bb = LLVMAppendBasicBlockInContext(lg.ctx, fn, "dec");
    LLVMBasicBlockRef ret_bb = LLVMAppendBasicBlockInContext(lg.ctx, fn, "ret");
    LLVMBuildCondBr(lg.builder, isn, ret_bb, dec_bb);

    LLVMPositionBuilderAtEnd(lg.builder, dec_bb);
    LLVMValueRef rcp = llv_rc_hdr_field(h, 2, "rcp");
    LLVMValueRef rc = LLVMBuildLoad2(lg.builder, lg.i64_ty, rcp, "rc");
    LLVMValueRef isz = LLVMBuildICmp(lg.builder, LLVMIntEQ, rc,
        LLVMConstInt(lg.i64_ty, 0, 0), "isz");
    LLVMBasicBlockRef uf_bb  = LLVMAppendBasicBlockInContext(lg.ctx, fn, "uf");
    LLVMBasicBlockRef ok_bb  = LLVMAppendBasicBlockInContext(lg.ctx, fn, "ok");
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
    LLVMBasicBlockRef free_bb = LLVMAppendBasicBlockInContext(lg.ctx, fn, "free");
    LLVMBuildCondBr(lg.builder, isd, free_bb, ret_bb);

    LLVMPositionBuilderAtEnd(lg.builder, free_bb);
    LLVMValueRef fr[] = { h };
    llv_h_call(llv_rt_free(), fr, 1, "");
    LLVMBuildBr(lg.builder, ret_bb);

    LLVMPositionBuilderAtEnd(lg.builder, ret_bb);
    LLVMBuildRetVoid(lg.builder);
    return fn;
}

/* malloc сайт → baga_rc_alloc при --rc, иначе чист malloc (без --rc IR е
 * побайтово същият). bytes/Vec/Map/box сайтовете минават през тази обвивка.
 * str сайтовете ползват llv_str_alloc_call (винаги header — виж по-долу). */
LLVMValueRef llv_rc_alloc_call(LLVMValueRef nbytes, const char *name) {
    if (lg.rc) return llv_h_call(llv_baga_rt("baga_rc_alloc"),
                             (LLVMValueRef[]){ llv_coerce(nbytes, lg.i64_ty) }, 1, name);
    return llv_h_call(llv_rt_malloc(), (LLVMValueRef[]){ nbytes }, 1, name);
}

/* str alloc → винаги baga_rc_alloc (32 B header magic+pe+rc+an), и в двата
 * режима — така кешът на дължината (baga_str_len/set_slen през pe) работи
 * и без --rc. bytes/Vec/Map/box остават header-less в non-RC (llv_rc_alloc_call),
 * а baga_rc_hdr ги различава по magic+page guard → strlen fallback. */
LLVMValueRef llv_str_alloc_call(LLVMValueRef nbytes, const char *name) {
    return llv_h_call(llv_baga_rt("baga_rc_alloc"),
                  (LLVMValueRef[]){ llv_coerce(nbytes, lg.i64_ty) }, 1, name);
}

/* порт на rc_type_tag (codegen_c.c:97): 1=str, 2=bytes, 3=Vec, 4=Map,
 * 0=не-heap. Task 3 го ползва за track/retain/release сайтовете. */
static int lrc_type_tag(Type *t) {
    if (!t) return 0;
    switch (t->kind) {
        case TYPE_STR:   return 1;
        case TYPE_BYTES: return 2;
        case TYPE_VEC:   return 3;
        case TYPE_MAP:   return 4;
        default:         return 0;
    }
}

/* --------------------- RC-LLVM: scope tracking ---------------------
 * Огледало на rc_push_scope/rc_pop_scope/rc_release_all/
 * rc_release_to_loop (codegen_c.c:560-603), но върху статичните
 * масиви в lg: alloca-та се намират с llv_st_lookup по име, release/retain
 * са IR повиквания към helper-ите от Task 2. Активно само при lg.rc —
 * без --rc нито една от тези функции не emit-ва IR. */

/* индекс на track-нат локал по име (-1 = няма); търси от върха надолу —
 * вътрешното засенчване печели (като rc_find в codegen_c) */
int llv_lrc_find(const char *name) {
    if (!lg.rc) return -1;
    for (int i = lg.lrc_count - 1; i >= 0; i--)
        if (strcmp(lg.lrc_locals[i].name, name) == 0) return i;
    return -1;
}

/* регистрира локал с вече изчислен tag (0 → не се track-ва); tnode е
 * анотационният type възел — резерва за elem/val kind, когато inferred
 * Type няма elem информация (като RcLocal.type_node в codegen_c) */
void llv_lrc_track_tag(const char *name, int tag, Type *t, Node *tnode,
                          int is_param) {
    if (!lg.rc || !tag) return;
    if (lg.lrc_count >= 256)
        llv_llvm_unsupported("прекалено много RC локали в една функция");
    lg.lrc_locals[lg.lrc_count++] = (LLRcLocal){ name, tag, t, tnode, is_param, 0, 0 };
}

int llv_lrc_heap_tag(Type *t);

/* track-ва само heap локали (tag ∈ {1..6}); type NULL → не се track-ва */
void llv_lrc_track(const char *name, Type *t, int is_param) {
    if (!t) return;
    llv_lrc_track_tag(name, llv_lrc_heap_tag(t), t, NULL, is_param);
}

/* tag от анотационен type възел — за fn параметрите (като rc_type_node_tag
 * в codegen_c; само str/bytes/Vec/Map — struct/enum идват през
 * llv_lrc_heap_tag_node по-долу) */
int llv_lrc_tag_node(Node *tn) {
    while (tn && (tn->kind == NODE_TYPE_EFFECT || tn->kind == NODE_TYPE_REF))
        tn = tn->inner_type;
    if (!tn || tn->kind != NODE_TYPE || !tn->type_name) return 0;
    if (strcmp(tn->type_name, "str") == 0)   return 1;
    if (strcmp(tn->type_name, "bytes") == 0) return 2;
    if (strcmp(tn->type_name, "Vec") == 0)   return 3;
    if (strcmp(tn->type_name, "Map") == 0)   return 4;
    return 0;
}

/* ---- Task 6: heap tag 5/6 (порт на RC5, codegen_c.c:123-234) ----
 * Struct/enum стойностите в LLVM са by-value в alloca (named struct тип,
 * БЕЗ rc header — за разлика от heap буферите). Затова tag 5/6 не описват
 * собствен rc на стойността, а „има heap полета/payload-и": release_<S>
 * освобождава heap ПОЛЕТАТА (транзитивно), без да free-ва самия struct
 * (той живее в стека). Lookup-ът е през lg.program (llv_find_struct_decl/
 * llv_find_sum_enum) — същият източник, който C ползва в rc_heap_tag
 * (llv_find_struct_decl там също обхожда cg->program, не Codegen.chk). */

/* взаимна рекурсия struct↔enum (като codegen_c); depth guard 32 срещу
 * циклични декларации */
static int lrc_enum_has_heap_d(Node *item, int depth);
static int lrc_heap_tag_d(Type *t, int depth);

static int lrc_struct_has_heap_d(const char *name, int depth) {
    if (depth > 32) return 0;
    Node *d = llv_find_struct_decl(name);
    if (!d) return 0;
    for (int i = 0; i < d->fields.len; i++) {
        Node *ft = d->fields.data[i]->fld_type;
        if (llv_lrc_tag_node(ft)) return 1;
        Node *t = ft;
        while (t && (t->kind == NODE_TYPE_EFFECT || t->kind == NODE_TYPE_REF))
            t = t->inner_type;
        if (t && t->kind == NODE_TYPE && t->type_name) {
            /* M24-RC: checked типът печели за struct/enum полета —
             * instantiated generic поле (`inner: Pair<i64>`) се проверява
             * per инстанция */
            if (t->type && (t->type->kind == TYPE_STRUCT ||
                            t->type->kind == TYPE_ENUM)) {
                if (lrc_heap_tag_d(t->type, depth + 1)) return 1;
                continue;
            }
            if (lrc_struct_has_heap_d(t->type_name, depth + 1))
                return 1;
            /* enum поле с heap payload също брои (release_S вика release_E) */
            Node *ed = llv_find_sum_enum(t->type_name);
            if (ed && lrc_enum_has_heap_d(ed, depth + 1))
                return 1;
        }
    }
    return 0;
}

int llv_lrc_struct_has_heap(const char *name) {
    return name ? lrc_struct_has_heap_d(name, 0) : 0;
}

/* enum с поне един variant с heap payload (str/bytes/Vec/Map или struct с
 * heap полета). Enum payload в enum не се брои (leak-safe — като C). */
static int lrc_enum_has_heap_d(Node *item, int depth) {
    if (depth > 32) return 0;
    if (!item || item->kind != NODE_ENUM) return 0;
    for (int j = 0; j < item->n_variants; j++) {
        Node *pt = item->enum_payloads ? item->enum_payloads[j] : NULL;
        if (!pt) continue;
        if (llv_lrc_tag_node(pt)) return 1;
        Node *t = pt;
        while (t && (t->kind == NODE_TYPE_EFFECT || t->kind == NODE_TYPE_REF))
            t = t->inner_type;
        if (t && t->kind == NODE_TYPE && t->type_name &&
            lrc_struct_has_heap_d(t->type_name, depth + 1))
            return 1;
    }
    return 0;
}

int llv_lrc_enum_has_heap(Node *item) {
    return lrc_enum_has_heap_d(item, 0);
}

/* M24-RC: инстанция на generic struct decl (d) с конкретни targs (vt) —
 * има ли heap полета след resolve на типовите променливи? Огледало на
 * rc_gen_inst_has_heap_d (codegen_c): decl-нивото вижда `v: T` като
 * не-heap дори при T=str, затова проверката е per инстанция. */
static int lrc_gen_inst_has_heap_d(Node *d, Type *vt, int depth) {
    if (depth > 32) return 0;
    for (int i = 0; i < d->fields.len; i++) {
        Node *ft = d->fields.data[i]->fld_type;
        Node *t = ft;
        while (t && (t->kind == NODE_TYPE_EFFECT || t->kind == NODE_TYPE_REF))
            t = t->inner_type;
        if (!t || t->kind != NODE_TYPE || !t->type_name) continue;
        /* типова променлива → targ на инстанцията */
        int is_var = 0;
        for (int a = 0; a < d->n_struct_params && a < vt->n_targs; a++) {
            if (strcmp(t->type_name, d->struct_params[a]) == 0) {
                if (lrc_heap_tag_d(vt->targs[a], depth + 1)) return 1;
                is_var = 1;
                break;
            }
        }
        if (is_var) continue;
        if (llv_lrc_tag_node(ft)) return 1;
        /* вложено поле: checked типът печели (може instantiated generic) */
        if (t->type && (t->type->kind == TYPE_STRUCT ||
                        t->type->kind == TYPE_ENUM)) {
            if (lrc_heap_tag_d(t->type, depth + 1)) return 1;
            continue;
        }
        if (lrc_struct_has_heap_d(t->type_name, depth + 1)) return 1;
        Node *ed = llv_find_sum_enum(t->type_name);
        if (ed && lrc_enum_has_heap_d(ed, depth + 1)) return 1;
    }
    return 0;
}

/* порт на rc_heap_tag (codegen_c.c:208): 5 = struct с heap полета,
 * 6 = sum enum с heap payload. M24-RC: generic struct — per инстанция. */
static int lrc_heap_tag_d(Type *t, int depth) {
    if (depth > 32) return 0;
    int tag = lrc_type_tag(t);
    if (tag) return tag;
    if (t && t->kind == TYPE_STRUCT && t->name) {
        Node *d = llv_find_struct_decl(t->name);
        if (d && d->n_struct_params > 0 && t->n_targs > 0)
            return lrc_gen_inst_has_heap_d(d, t, depth + 1) ? 5 : 0;
        if (llv_lrc_struct_has_heap(t->name)) return 5;
    }
    if (t && t->kind == TYPE_ENUM && t->name) {
        Node *ed = llv_find_sum_enum(t->name);
        if (ed && llv_lrc_enum_has_heap(ed)) return 6;
    }
    return 0;
}

int llv_lrc_heap_tag(Type *t) {
    return lrc_heap_tag_d(t, 0);
}

/* порт на rc_heap_tag_node (codegen_c.c:221) — същият tag от type AST възел */
int llv_lrc_heap_tag_node(Node *ty) {
    int tag = llv_lrc_tag_node(ty);
    if (tag) return tag;
    while (ty && (ty->kind == NODE_TYPE_EFFECT || ty->kind == NODE_TYPE_REF))
        ty = ty->inner_type;
    if (ty && ty->kind == NODE_TYPE && ty->type_name) {
        if (llv_lrc_struct_has_heap(ty->type_name))
            return 5;
        Node *ed = llv_find_sum_enum(ty->type_name);
        if (ed && llv_lrc_enum_has_heap(ed)) return 6;
    }
    return 0;
}

/* порт на rc_is_enum_ctor (codegen_c.c:1228): повикване-конструктор на sum
 * enum с payload (bare Ok(x) или Res::Ok(x)) */
int llv_lrc_is_enum_ctor(Node *n) {
    if (!lg.program || !n || n->kind != NODE_CALL || !n->callee) return 0;
    if (n->callee->kind != NODE_IDENT && n->callee->kind != NODE_PATH)
        return 0;
    for (int i = 0; i < lg.program->items.len; i++) {
        Node *item = lg.program->items.data[i];
        if (item->kind != NODE_ENUM) continue;
        if (n->callee->kind == NODE_PATH &&
            strcmp(item->enum_name, n->callee->path_enum) != 0)
            continue;
        for (int j = 0; j < item->n_variants; j++) {
            const char *vn = n->callee->kind == NODE_PATH
                ? n->callee->path_variant : n->callee->name;
            if (item->enum_payloads && item->enum_payloads[j] &&
                strcmp(item->enum_variants[j], vn) == 0)
                return 1;
        }
    }
    return 0;
}

/* порт на rc_borrowed_init (codegen_c.c:312): vec_get/map_get/struct поле/
 * h_* връщат референция към чужда собственост → при вграждане/връзване се
 * retain-ва, не се пропуска */
int llv_lrc_borrowed_init(Node *init) {
    if (!init) return 0;
    if (init->kind == NODE_FIELD) return 1;
    if (init->kind == NODE_CALL && init->callee &&
        init->callee->kind == NODE_IDENT) {
        const char *bn = init->callee->name;
        if (strncmp(bn, "vec_get", 7) == 0 || strncmp(bn, "map_get", 7) == 0 ||
            strcmp(bn, "h_str") == 0 || strcmp(bn, "h_bytes") == 0 ||
            strcmp(bn, "h_map") == 0)
            return 1;
    }
    return 0;
}

/* порт на rc_need_owned_retain (codegen_c.c:1504): трябва ли стойността на
 * match рамо да се retain-не, за да е резултатът owned по конвенцията
 * „fn резултат = owned"? Свеж литерал/ctor и не-borrowed call — не;
 * вложен match — не (рамената му вече retain-ват). Ident (match binding е
 * borrowed алиас на payload-а; track-нат локал е втори собственик),
 * vec_get/поле и пр. — да. */
int llv_lrc_need_owned_retain(Node *val) {
    if (!lg.rc || !val) return 0;
    if (!llv_lrc_heap_tag(val->type)) return 0;
    if (val->kind == NODE_STRUCT_LIT) return 0;
    if (val->kind == NODE_CALL && llv_lrc_is_enum_ctor(val)) return 0;
    if (val->kind == NODE_CALL && !llv_lrc_borrowed_init(val)) return 0;
    if (val->kind == NODE_MATCH) return 0;
    return 1;
}

void llv_lrc_emit_retain_val(int tag, Type *ty, Node *tnode,
                                LLVMValueRef val);

/* retain на стойност, вграждана в контейнер/struct поле/enum payload
 * (опростен порт на embed сайтовете в codegen_c): fresh израз (struct
 * литерал, enum ctor, owned call) идва с rc=1 от конструкцията — move,
 * без retain; ident/borrowed се retain-ват (+1 за новата референция).
 * Без move elision (RC2 prepass няма LLVM еквивалент) — retain + нормален
 * release на източника е наблюдаемо еквивалентен на C move-а (и двата
 * пъта rc-то е балансирано; разлика само във времето на декрементите). */
void llv_lrc_embed_retain(Node *val, LLVMValueRef v) {
    if (!lg.rc || !val) return;
    int tag = llv_lrc_heap_tag(val->type);
    if (!tag) return;
    if (val->kind == NODE_IDENT) {
        int si = llv_lrc_find(val->name);
        if (si >= 0 && lg.lrc_locals[si].dead) return;   /* drop()нат — като C */
    } else if (val->kind == NODE_STRUCT_LIT || llv_lrc_is_enum_ctor(val)) {
        return;   /* свеж литерал/ctor — move в новата собственост */
    } else if (!llv_lrc_borrowed_init(val)) {
        return;   /* owned call резултат — move (като rc_tmp consume в C) */
    }
    llv_lrc_emit_retain_val(tag, val->type, NULL, v);
}

/* Task 5: elem_kind за release на Vec — порт на rc_vec_elem_kind +
 * rc_vec_elem_kind_node (codegen_c.c:333-388): 0 inline (i64/f64/bool),
 * 1 str, 2 struct/enum box, 3 nested Vec, 4 bytes box. Inferred Type първо;
 * при 0 — резерва от анотацията (`let v: Vec<str> = vec_new()`). За разлика
 * от C elem_size не се смята — libc free не иска размер (подаваме 0). */
int llv_lrc_vec_elem_kind(Type *vty, Node *tn) {
    Type *e = vty ? vty->elem : NULL;
    if (e) {
        if (e->kind == TYPE_STR) return 1;
        if (e->kind == TYPE_BYTES) return 4;
        if (e->kind == TYPE_VEC) return 3;
        if (e->name && (e->kind == TYPE_STRUCT || e->kind == TYPE_ENUM)) return 2;
    }
    while (tn && (tn->kind == NODE_TYPE_EFFECT || tn->kind == NODE_TYPE_REF))
        tn = tn->inner_type;
    Node *en = tn ? tn->inner_type : NULL;
    if (!en || en->kind != NODE_TYPE || !en->type_name) return 0;
    if (strcmp(en->type_name, "str") == 0)   return 1;
    if (strcmp(en->type_name, "bytes") == 0) return 4;
    if (strcmp(en->type_name, "Vec") == 0)   return 3;
    if (strcmp(en->type_name, "i64") == 0 || strcmp(en->type_name, "i32") == 0 ||
        strcmp(en->type_name, "f64") == 0 || strcmp(en->type_name, "bool") == 0)
        return 0;
    return 2;
}

/* Task 5: val_tag за release на Map — порт на rc_map_val_tag + _node
 * (codegen_c.c:354-403): 0 inline, 1 str, 2 bytes, 3 struct/enum box.
 * Ключът не е нужен — release_map го познава по entry ktag/sk (като C). */
int llv_lrc_map_val_tag(Type *mty, Node *tn) {
    Type *v = mty ? mty->elem : NULL;
    if (v) {
        if (v->kind == TYPE_STR) return 1;
        if (v->kind == TYPE_BYTES) return 2;
        if (v->name && (v->kind == TYPE_STRUCT || v->kind == TYPE_ENUM)) return 3;
    }
    while (tn && (tn->kind == NODE_TYPE_EFFECT || tn->kind == NODE_TYPE_REF))
        tn = tn->inner_type;
    Node *vn = tn ? tn->inner_type2 : NULL;
    if (!vn || vn->kind != NODE_TYPE || !vn->type_name) return 0;
    if (strcmp(vn->type_name, "str") == 0)   return 1;
    if (strcmp(vn->type_name, "bytes") == 0) return 2;
    if (strcmp(vn->type_name, "i64") == 0 || strcmp(vn->type_name, "i32") == 0 ||
        strcmp(vn->type_name, "f64") == 0 || strcmp(vn->type_name, "bool") == 0)
        return 0;
    return 3;
}

/* едно release повикване: зарежда стойността от alloca-та и вика
 * baga_rc_release_{str,bytes,vec,map}. Vec/Map (tag 3/4) — Task 5:
 * рекурсивен release на елементите според elem_kind/val_tag; 4-тият
 * аргумент е destructor на box полетата (Task 6: baga_rc_relf_<S>,
 * NULL → само free — като elem_rel/val_rel в codegen_c).
 * Tag 5/6 (Task 6): struct/enum стойността е by-value в alloca —
 * release_<име> освобождава heap полетата/payload-а, без free на
 * самия struct (той е стеков). */
LLVMValueRef llv_lrc_rc_fn(const char *type_name, int is_release);
LLVMValueRef llv_lrc_rc_fn_ty(Type *t, int is_release);  /* M24-RC: Type-aware */
LLVMValueRef llv_lrc_box_rel(const char *type_name);
LLVMValueRef llv_lrc_box_rel_ty(Type *et);               /* M24-RC: Type-aware */
LLVMValueRef llv_lrc_nested_vec_rel(Type *vty, Node *tn);

/* M24-RC: LLVM тип на struct/enum стойност — instantiated generic struct
 * (n_targs>0) → per-instance named тип (b_Box_i64), иначе по име */
static LLVMTypeRef lrc_struct_ty_of(Type *ty, Node *tnode) {
    if (ty && ty->kind == TYPE_STRUCT && ty->n_targs > 0)
        return llv_user_struct_ty_inst(ty);
    const char *sn = (ty && ty->name) ? ty->name : NULL;
    if (!sn && tnode) {
        Node *t = tnode;
        while (t && (t->kind == NODE_TYPE_EFFECT || t->kind == NODE_TYPE_REF))
            t = t->inner_type;
        if (t && t->kind == NODE_TYPE) sn = t->type_name;
    }
    return sn ? llv_user_struct_ty(sn) : NULL;
}

/* име на struct/enum типа на локал — inferred Type първо, анотация после
 * (като case 5/6 в rc_emit_release, codegen_c.c:536-556) */
static const char *lrc_local_type_name(LLRcLocal *l) {
    if (l->type && l->type->name) return l->type->name;
    Node *t = l->type_node;
    while (t && (t->kind == NODE_TYPE_EFFECT || t->kind == NODE_TYPE_REF))
        t = t->inner_type;
    if (t && t->kind == NODE_TYPE) return t->type_name;
    return NULL;
}

/* елементен/стойностен тип на Vec/Map локал (inferred Type, после анотация)
 * — порт на rc_vec_elem_name/rc_map_val_name (codegen_c.c:473-487) */
static const char *lrc_vec_elem_name(LLRcLocal *l) {
    if (l->type && l->type->elem && l->type->elem->name)
        return l->type->elem->name;
    Node *it = l->type_node ? l->type_node->inner_type : NULL;
    if (it && it->kind == NODE_TYPE) return it->type_name;
    return NULL;
}

static const char *lrc_map_val_name(LLRcLocal *l) {
    if (l->type && l->type->elem && l->type->elem->name)
        return l->type->elem->name;
    Node *it = l->type_node ? l->type_node->inner_type2 : NULL;
    if (it && it->kind == NODE_TYPE) return it->type_name;
    return NULL;
}

void llv_lrc_emit_release(LLRcLocal *l) {
    LLVMValueRef alloca = llv_st_lookup(l->name);
    if (!alloca) return;
    LLVMValueRef p;
    const char *rt;
    if (l->tag == 1) {
        p = LLVMBuildLoad2(lg.builder, lg.ptr_ty, alloca, "rcr");
        rt = "baga_rc_release_str";
    } else if (l->tag == 2) {
        /* bytes е { i8* data, i64 len } by value — release-ва се data */
        p = LLVMBuildLoad2(lg.builder, lg.ptr_ty,
            LLVMBuildStructGEP2(lg.builder, llv_baga_bytes_ty(), alloca, 0, "rcbp"),
            "rcbd");
        rt = "baga_rc_release_bytes";
    } else if (l->tag == 3) {
        LLVMValueRef vp = LLVMBuildLoad2(lg.builder, llv_baga_vec_ptr_ty(), alloca, "rcv");
        int ek = llv_lrc_vec_elem_kind(l->type, l->type_node);
        /* destructor: ek 2 → relf на елементния тип; ek 3 → relv на
         * вложения Vec<S> (NULL → старото поведение, leak-safe граница) */
        /* M24-RC: instantiated generic елемент → per-instance relf;
         * иначе досегашният name-based път (с node fallback) */
        LLVMValueRef rel = ek == 3
            ? llv_lrc_nested_vec_rel(l->type, l->type_node)
            : (l->type && l->type->elem && l->type->elem->n_targs > 0
               ? llv_lrc_box_rel_ty(l->type->elem)
               : llv_lrc_box_rel(lrc_vec_elem_name(l)));
        LLVMValueRef a[] = {
            LLVMBuildBitCast(lg.builder, vp, lg.ptr_ty, "rcvp"),
            LLVMConstInt(lg.i64_ty, (uint64_t)ek, 0),
            LLVMConstInt(lg.i64_ty, 0, 0),   /* elem_size — не се ползва */
            rel,
        };
        llv_h_call(llv_baga_rt("baga_rc_release_vec"), a, 4, "");
        return;
    } else if (l->tag == 4) {
        LLVMValueRef mp = LLVMBuildLoad2(lg.builder, llv_baga_map_ptr_ty(), alloca, "rcm");
        LLVMValueRef a[] = {
            LLVMBuildBitCast(lg.builder, mp, lg.ptr_ty, "rcmp"),
            LLVMConstInt(lg.i64_ty,
                (uint64_t)llv_lrc_map_val_tag(l->type, l->type_node), 0),
            LLVMConstInt(lg.i64_ty, 0, 0),   /* val_size — не се ползва */
            /* M24-RC: instantiated generic стойност → per-instance relf */
            (l->type && l->type->elem && l->type->elem->n_targs > 0
             ? llv_lrc_box_rel_ty(l->type->elem)
             : llv_lrc_box_rel(lrc_map_val_name(l))),
        };
        llv_h_call(llv_baga_rt("baga_rc_release_map"), a, 4, "");
        return;
    } else if (l->tag == 5 || l->tag == 6) {
        const char *sn = lrc_local_type_name(l);
        if (!sn) return;
        /* M24-RC: instantiated generic struct — per-instance тип и helper */
        LLVMTypeRef sty = lrc_struct_ty_of(l->type, l->type_node);
        if (!sty) return;
        LLVMValueRef v = LLVMBuildLoad2(lg.builder, sty, alloca, "rcs");
        if (l->type) llv_h_call(llv_lrc_rc_fn_ty(l->type, 1), &v, 1, "");
        else         llv_h_call(llv_lrc_rc_fn(sn, 1), &v, 1, "");
        return;
    } else {
        return;
    }
    llv_h_call(llv_baga_rt(rt), &p, 1, "");
}

/* retain върху вече изчислена стойност (alias копие: let x = y / x = y).
 * tag 3/4 — retain на struct указателя (rc живее върху него, като в C).
 * tag 5/6 — retain_<име> на heap полетата/payload-а (стойността е
 * by-value; ty/tnode дават името на типа, като rc_emit_retain_val в C). */
void llv_lrc_emit_retain_val(int tag, Type *ty, Node *tnode,
                                LLVMValueRef val) {
    LLVMValueRef p;
    if (tag == 1)      p = val;
    else if (tag == 2) p = LLVMBuildExtractValue(lg.builder, val, 0, "rcbd");
    else if (tag == 3 || tag == 4)
        p = LLVMBuildBitCast(lg.builder, val, lg.ptr_ty, "rccp");
    else if (tag == 5 || tag == 6) {
        /* M24-RC: instantiated generic struct — per-instance helper */
        if (ty && ty->kind == TYPE_STRUCT && ty->n_targs > 0) {
            llv_h_call(llv_lrc_rc_fn_ty(ty, 0), &val, 1, "");
            return;
        }
        const char *sn = (ty && ty->name) ? ty->name : NULL;
        if (!sn && tnode) {
            Node *t = tnode;
            while (t && (t->kind == NODE_TYPE_EFFECT || t->kind == NODE_TYPE_REF))
                t = t->inner_type;
            if (t && t->kind == NODE_TYPE) sn = t->type_name;
        }
        if (!sn) return;
        llv_h_call(llv_lrc_rc_fn(sn, 0), &val, 1, "");
        return;
    }
    else               return;
    llv_h_call(llv_baga_rt("baga_rc_retain"), &p, 1, "rcr");
}

/* ---------------- RC4-LLVM: per-statement temp регистър ----------------
 * Порт на rc_tmp_* (codegen_c.c:1078-1521): fresh heap резултат (owned call
 * с heap тип — не borrowed, не enum ctor — плюс NODE_TO_STR), който не се
 * връзва в локал, е temp и се release-ва в края на statement-а. C hoist-ва
 * temp-овете в __rc_tmpN декларации преди statement-а; тук (SSA) temp
 * изразите се emit-ват първи в текущия block и стойността се кешира по
 * Node* — llv_emit_expr_llvm връща кеша при удар (без двойна оценка). */

LLVMValueRef llv_emit_expr_llvm(Node *n); /* fwd — пълната дефиниция е по-долу */

/* fresh heap temp ли е този възел? (порт на rc_tmp_fresh, codegen_c.c:1237) */
int llv_lrc_tmp_fresh(Node *n) {
    if (!n) return 0;
    if (n->kind == NODE_TO_STR) {
        /* to_str върху str е identity (borrowed) — fresh е само конверсията
         * от не-str тип; паритет с rc_tmp_fresh (codegen_c.c). */
        Type *et = n->to_str_expr ? n->to_str_expr->type : NULL;
        return !(et && et->kind == TYPE_STR);
    }
    if (n->kind != NODE_CALL) return 0;
    if (llv_lrc_borrowed_init(n)) return 0;
    /* enum ctor е като литерал — payload-ът е owned от ctor сайта */
    if (llv_lrc_is_enum_ctor(n)) return 0;
    return llv_lrc_heap_tag(n->type) != 0;
}

/* pre-order събиране (порт на rc_tmp_collect, codegen_c.c:1281). Не се слиза
 * в: STRUCT_LIT (полетата escape-ват), LAMBDA, TRY/CATCH, IF-израз, десен
 * операнд на &&/||, drop(…) аргументи, enum ctor аргументи, match рамена. */
static void lrc_tmp_collect(Node *n, int is_root) {
    if (!n) return;
    if (llv_lrc_tmp_fresh(n) && !is_root) {
        if (lg.lrc_tmp_count >= 64)
            llv_llvm_unsupported("прекалено много temp-ове в един statement");
        lg.lrc_tmps[lg.lrc_tmp_count++] = (LLRcTmp){
            n, n->kind == NODE_TO_STR ? 1 : llv_lrc_heap_tag(n->type),
            n->type, NULL, 0 };
        /* продължаваме надолу — аргументите може да съдържат temp-ове */
    }
    switch (n->kind) {
        case NODE_BINARY:
            lrc_tmp_collect(n->left, 0);
            /* &&/||: десният операнд се оценява условно — не се пипа */
            if (n->bin_op != OP_AND && n->bin_op != OP_OR)
                lrc_tmp_collect(n->right, 0);
            break;
        case NODE_UNARY:
            lrc_tmp_collect(n->operand, 0);
            break;
        case NODE_CALL:
            /* drop(x) е release пътят на x; enum ctor копира payload без
             * retain — temp в него би обесил payload-а (като C) */
            if (n->callee && n->callee->kind == NODE_IDENT &&
                strcmp(n->callee->name, "drop") == 0)
                break;
            if (llv_lrc_is_enum_ctor(n)) break;
            lrc_tmp_collect(n->callee, 0);
            for (int i = 0; i < n->args.len; i++)
                lrc_tmp_collect(n->args.data[i], 0);
            break;
        case NODE_INDEX:
            lrc_tmp_collect(n->obj, 0);
            lrc_tmp_collect(n->index, 0);
            break;
        case NODE_ELEM_REF:
            lrc_tmp_collect(n->elem_obj, 0);
            break;
        case NODE_FIELD:
            lrc_tmp_collect(n->field_obj, 0);
            break;
        case NODE_RANGE:
            lrc_tmp_collect(n->range_lo, 0);
            lrc_tmp_collect(n->range_hi, 0);
            break;
        case NODE_TO_STR:
            lrc_tmp_collect(n->to_str_expr, 0);
            break;
        case NODE_MATCH: {
            /* RC5 v0.11: scrutinee temp (`match f() { ... }`). Scrutinee-то
             * се оценява безусловно веднъж ПРЕДИ рамената, а release-ът идва
             * в края на statement-а — СЛЕД рамената, така че borrowed
             * binding-ите (v0.6 конвенция) остават валидни. В рамената не се
             * слиза. Enum ctor scrutinee с heap payload се регистрира с
             * tag 6 (payload референциите са owned от ctor сайта и никой не
             * ги release-ва след match-а); owned fn резултат — през
             * llv_lrc_tmp_fresh/llv_lrc_heap_tag (v1.0a/b). */
            Node *sc = n->match_expr;
            if (sc && sc->kind == NODE_CALL && llv_lrc_is_enum_ctor(sc) &&
                llv_lrc_heap_tag(sc->type) == 6) {
                if (lg.lrc_tmp_count >= 64)
                    llv_llvm_unsupported("прекалено много temp-ове в един statement");
                lg.lrc_tmps[lg.lrc_tmp_count++] =
                    (LLRcTmp){ sc, 6, sc->type, NULL, 0 };
            } else {
                lrc_tmp_collect(sc, 0);
            }
            break;
        }
        default:
            break;
    }
}

/* индекс на регистриран (и неконсумиран) temp за този AST възел (-1 = не е)
 * — порт на rc_tmp_find (codegen_c.c:1378); за move в контейнер сайтовете */
int llv_lrc_tmp_find(Node *n) {
    if (!lg.rc || !lg.lrc_tmps_on || !n) return -1;
    for (int i = 0; i < lg.lrc_tmp_count; i++)
        if (!lg.lrc_tmps[i].consumed && lg.lrc_tmps[i].site == n) return i;
    return -1;
}

/* temp-ът е прехвърлен (move) в контейнер — краят на statement-а не го
 * release-ва (порт на rc_tmp_consume, codegen_c.c:1388) */
void llv_lrc_tmp_consume(int i) {
    if (i >= 0) lg.lrc_tmps[i].consumed = 1;
}

/* release на temp по СТОЙНОСТ (temp-овете нямат alloca — за разлика от
 * llv_lrc_emit_release, която зарежда от alloca по име). Същите helper-и и
 * elem/val kind логика като при локалите. */
void llv_lrc_emit_release_val(int tag, Type *ty, LLVMValueRef val) {
    if (!val) return;
    if (tag == 1) {
        llv_h_call(llv_baga_rt("baga_rc_release_str"), &val, 1, "");
    } else if (tag == 2) {
        LLVMValueRef d = LLVMBuildExtractValue(lg.builder, val, 0, "rtd");
        llv_h_call(llv_baga_rt("baga_rc_release_bytes"), &d, 1, "");
    } else if (tag == 3) {
        int ek = llv_lrc_vec_elem_kind(ty, NULL);
        LLVMValueRef rel = ek == 3
            ? llv_lrc_nested_vec_rel(ty, NULL)
            : (ty && ty->elem && ty->elem->n_targs > 0
               ? llv_lrc_box_rel_ty(ty->elem)
               : llv_lrc_box_rel(ty && ty->elem ? ty->elem->name : NULL));
        LLVMValueRef a[] = {
            LLVMBuildBitCast(lg.builder, val, lg.ptr_ty, "rtv"),
            LLVMConstInt(lg.i64_ty, (uint64_t)ek, 0),
            LLVMConstInt(lg.i64_ty, 0, 0),   /* elem_size — не се ползва */
            rel,
        };
        llv_h_call(llv_baga_rt("baga_rc_release_vec"), a, 4, "");
    } else if (tag == 4) {
        LLVMValueRef a[] = {
            LLVMBuildBitCast(lg.builder, val, lg.ptr_ty, "rtm"),
            LLVMConstInt(lg.i64_ty, (uint64_t)llv_lrc_map_val_tag(ty, NULL), 0),
            LLVMConstInt(lg.i64_ty, 0, 0),   /* val_size — не се ползва */
            (ty && ty->elem && ty->elem->n_targs > 0
             ? llv_lrc_box_rel_ty(ty->elem)
             : llv_lrc_box_rel(ty && ty->elem ? ty->elem->name : NULL)),
        };
        llv_h_call(llv_baga_rt("baga_rc_release_map"), a, 4, "");
    } else if (tag == 5 || tag == 6) {
        /* M24-RC: instantiated generic struct — per-instance helper */
        if (ty && ty->kind == TYPE_STRUCT && ty->n_targs > 0) {
            llv_h_call(llv_lrc_rc_fn_ty(ty, 1), &val, 1, "");
            return;
        }
        const char *sn = (ty && ty->name) ? ty->name : NULL;
        if (!sn) return;
        llv_h_call(llv_lrc_rc_fn(sn, 1), &val, 1, "");
    }
}

/* release на активните неконсумирани temp-ове (в реда на събирането, като
 * rc_tmp_release_all в C) и изчистване на регистъра */
void llv_lrc_tmp_release_all(void) {
    if (!lg.rc || !lg.lrc_tmps_on) return;
    for (int i = 0; i < lg.lrc_tmp_count; i++) {
        LLRcTmp *t = &lg.lrc_tmps[i];
        if (t->consumed) continue;   /* move в контейнер — box-ът е собственик */
        llv_lrc_emit_release_val(t->tag, t->type, t->val);
    }
    lg.lrc_tmp_count = 0;
    lg.lrc_tmps_on = 0;
}

/* save/restore за вложени statement-и (блок-стойности, ламбди — порт на
 * saved_tmps/saved_on двойката в rc_tmp_begin/rc_tmp_end) */

static void lrc_tmp_save(LLRcTmpSave *sv) {
    sv->count = lg.lrc_tmp_count;
    sv->on = lg.lrc_tmps_on;
    sv->decl = lg.lrc_tmp_decl;
    memcpy(sv->e, lg.lrc_tmps, sizeof sv->e);
}

void llv_lrc_tmp_restore(LLRcTmpSave *sv) {
    memcpy(lg.lrc_tmps, sv->e, sizeof sv->e);
    lg.lrc_tmp_count = sv->count;
    lg.lrc_tmps_on = sv->on;
    lg.lrc_tmp_decl = sv->decl;
}

/* начало на statement с temp tracking (порт на rc_tmp_begin,
 * codegen_c.c:1396). root_bound=1: root-ът е bound (let init / assign дясно /
 * return стойност) — не е temp. Temp-овете се emit-ват веднага (вътрешните
 * първи — обратен ред на събирането, като декларациите в C) и се кешират. */
void llv_lrc_tmp_begin(Node *root, int root_bound, LLRcTmpSave *sv) {
    lrc_tmp_save(sv);
    lg.lrc_tmp_count = 0;
    lg.lrc_tmps_on = 0;
    lg.lrc_tmp_decl = NULL;
    if (!lg.rc || !root) return;
    if (root->kind == NODE_ASSIGN) {
        /* дясното е bound; сложните цели четат обекта/индекса */
        lrc_tmp_collect(root->assign_val, 1);
        Node *t = root->assign_target;
        if (t) {
            if (t->kind == NODE_FIELD) lrc_tmp_collect(t->field_obj, 0);
            else if (t->kind == NODE_INDEX) {
                lrc_tmp_collect(t->obj, 0);
                lrc_tmp_collect(t->index, 0);
            }
        }
    } else {
        lrc_tmp_collect(root, root_bound);
    }
    if (lg.lrc_tmp_count == 0) return;
    lg.lrc_tmps_on = 1;
    for (int i = lg.lrc_tmp_count - 1; i >= 0; i--) {
        lg.lrc_tmp_decl = lg.lrc_tmps[i].site;
        lg.lrc_tmps[i].val = llv_emit_expr_llvm(lg.lrc_tmps[i].site);
        lg.lrc_tmp_decl = NULL;
        if (!lg.lrc_tmps[i].val)
            llv_llvm_unsupported("print/drop като temp израз");
    }
}

/* край на statement-а: release (ако не е направен) + възстановяване */
void llv_lrc_tmp_end(LLRcTmpSave *sv) {
    llv_lrc_tmp_release_all();
    llv_lrc_tmp_restore(sv);
}

void llv_lrc_push_scope(int is_loop) {
    if (!lg.rc) return;
    if (lg.lrc_depth >= 64)
        llv_llvm_unsupported("прекалено дълбока RC scope вложеност");
    lg.lrc_scopes[lg.lrc_depth++] = (LLRcScope){ lg.lrc_count, is_loop };
}

/* край на блок: release на локалите над top в обратен ред (без params/dead),
 * после pop. Ако блокът вече има terminator (return/break/continue са
 * release-нали по пътя) — само pop, без дублиране. */
void llv_lrc_pop_scope(void) {
    if (!lg.rc) return;
    LLRcScope sc = lg.lrc_scopes[--lg.lrc_depth];
    if (!LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(lg.builder))) {
        for (int i = lg.lrc_count - 1; i >= sc.top; i--) {
            LLRcLocal *l = &lg.lrc_locals[i];
            if (!l->is_param && !l->dead) llv_lrc_emit_release(l);
        }
    }
    lg.lrc_count = sc.top;
}

/* return: release на всички локали над fn scope-а (без params/dead);
 * skip_idx = returned стойността (move — собствеността отива при caller-а).
 * Пропускът е по ИНДЕКС, не чрез dead флага: dead е compile-time състояние,
 * споделено между пътищата, и би „изтрило" локала и на другите пътища
 * (първият emit-нат return печели) — rc_release_all в codegen_c работи
 * по същия начин (skip_idx). Scope-овете НЕ се попват — функцията
 * приключва; следващата fn emission reset-ва. */
void llv_lrc_release_all(int skip_idx) {
    if (!lg.rc) return;
    int base = lg.lrc_fn_base >= 0 && lg.lrc_fn_base < lg.lrc_depth
        ? lg.lrc_scopes[lg.lrc_fn_base].top : 0;
    for (int i = lg.lrc_count - 1; i >= base; i--) {
        LLRcLocal *l = &lg.lrc_locals[i];
        if (l->is_param || l->dead || i == skip_idx) continue;
        llv_lrc_emit_release(l);
    }
}

/* break/continue: release на локалите до най-близкия loop scope (вкл. него —
 * нормалният му release в края на тялото се прескача от скока). Само
 * emission: lrc_depth/lrc_count не се пипат, защото LLVM продължава линейно;
 * блокът получава terminator (br) и llv_lrc_pop_scope няма да дублира. */
void llv_lrc_release_to_loop(void) {
    if (!lg.rc) return;
    for (int i = lg.lrc_depth - 1; i >= 0; i--) {
        if (!lg.lrc_scopes[i].is_loop) continue;
        int top = lg.lrc_scopes[i].top;
        for (int j = lg.lrc_count - 1; j >= top; j--) {
            LLRcLocal *l = &lg.lrc_locals[j];
            if (!l->is_param && !l->dead) llv_lrc_emit_release(l);
        }
        return;
    }
}

/* move семантика за return (explicit или implicit от последен EXPR_STMT):
 * ако изразът е track-нат IDENT — подава се като skip_idx на release_all
 * (собствеността отива при caller-а); върнат ПАРАМЕТЪР се retain-ва
 * (borrowed → owned, codegen_c.c:3739-3755). Израз (не-IDENT) — без skip. */
void llv_lrc_return_move(Node *expr, LLVMValueRef v) {
    if (!lg.rc) return;
    int ri = expr && expr->kind == NODE_IDENT ? llv_lrc_find(expr->name) : -1;
    if (ri >= 0 && lg.lrc_locals[ri].is_param)
        llv_lrc_emit_retain_val(lg.lrc_locals[ri].tag, lg.lrc_locals[ri].type,
                            lg.lrc_locals[ri].type_node, v);
    llv_lrc_release_all(ri);
}

/* NODE_WHILE/NODE_FOR вдигат този флаг преди emit_block_llvm на тялото —
 * следващият llv_lrc_push_scope е loop scope (прочита се в emit_block_llvm). */
int lrc_loop_next;

/* ============================ M20 payload runtime ============================ */

int llv_llvm_has_payload_effects(Type *t) {
    if (!t || !t->effect_payloads) return 0;
    for (int i = 0; i < t->n_effects; i++)
        if (t->effect_payloads[i]) return 1;
    return 0;
}

/* Tag на ефект по име — детерминистичен (първа поява = 1, 2, …). */
int llv_llvm_eff_tag(const char *name) {
    for (int i = 0; i < lg.eff_tags.len; i++)
        if (strcmp(lg.eff_tags.data[i], name) == 0) return i + 1;
    vec_push(lg.eff_tags, strdup(name));
    return lg.eff_tags.len;
}

/* { i64 tag, i64 i, double f, i8* s, baga_bytes b } */
static LLVMTypeRef baga_eff_ty(void) {
    if (lg.eff_ty) return lg.eff_ty;
    LLVMTypeRef t = LLVMStructCreateNamed(lg.ctx, "baga_eff");
    LLVMTypeRef elems[] = { lg.i64_ty, lg.i64_ty, lg.double_ty,
                            lg.ptr_ty, llv_baga_bytes_ty() };
    LLVMStructSetBody(t, elems, 5, 0);
    lg.eff_ty = t;
    return t;
}

static LLVMValueRef eff_global(void) {
    if (lg.eff_global) return lg.eff_global;
    lg.eff_global = LLVMAddGlobal(lg.mod, baga_eff_ty(), "baga_eff_tl");
    LLVMSetInitializer(lg.eff_global, LLVMConstNull(baga_eff_ty()));
    LLVMSetThreadLocal(lg.eff_global, 1);
    return lg.eff_global;
}

/* GEP до поле idx на глобалния слот (в текущия block) */
LLVMValueRef llv_eff_gep(unsigned idx, const char *name) {
    LLVMValueRef g = eff_global();
    LLVMValueRef gp = LLVMBuildStructGEP2(lg.builder, baga_eff_ty(), g,
                                          idx, name);
    return gp;
}

/* Полето на payload-а по тип (0=tag, 1=i, 2=f, 3=s, 4=b) */
unsigned llv_eff_field_of(Type *pt) {
    if (pt->kind == TYPE_F64) return 2;
    if (pt->kind == TYPE_STR) return 3;
    if (pt->kind == TYPE_BYTES) return 4;
    return 1;
}

/* return <нулата на cur_ret_ty> — propagate пътят */
void llv_h_ret_zero(void) {
    if (lg.cur_ret_ty == lg.void_ty) LLVMBuildRetVoid(lg.builder);
    else LLVMBuildRet(lg.builder, LLVMConstNull(lg.cur_ret_ty));
}

/* alloca винаги в entry block-а на текущата функция: alloca на текущата
 * insert точка в тяло на цикъл заделя нов стек на всяка итерация и
 * препълва стека при дълги цикли (sha256 върху 100KB низ). */
LLVMValueRef llv_entry_alloca(LLVMTypeRef ty, const char *name) {
    LLVMValueRef fn = LLVMGetBasicBlockParent(LLVMGetInsertBlock(lg.builder));
    LLVMBasicBlockRef entry = LLVMGetEntryBasicBlock(fn);
    LLVMBuilderRef eb = LLVMCreateBuilderInContext(lg.ctx);
    LLVMValueRef first = LLVMGetFirstInstruction(entry);
    if (first) LLVMPositionBuilderBefore(eb, first);
    else LLVMPositionBuilderAtEnd(eb, entry);
    LLVMValueRef a = LLVMBuildAlloca(eb, ty, name);
    LLVMDisposeBuilder(eb);
    return a;
}

/* static int64_t baga_len(const char *s) { return baga_str_len(s); } */
LLVMValueRef llv_build_baga_len(void) {
    LLVMTypeRef p[] = { lg.ptr_ty };
    LLVMValueRef fn = LLVMAddFunction(lg.mod, "baga_len",
        LLVMFunctionType(lg.i64_ty, p, 1, 0));
    llv_h_begin(fn);
    LLVMValueRef a[] = { LLVMGetParam(fn, 0) };
    LLVMBuildRet(lg.builder, llv_h_call(llv_baga_rt("baga_str_len"), a, 1, "n"));
    return fn;
}

/* static int64_t baga_char_at(const char *s, int64_t i)
 * { return (int64_t)(unsigned char)s[i]; } */
LLVMValueRef llv_build_baga_char_at(void) {
    LLVMTypeRef p[] = { lg.ptr_ty, lg.i64_ty };
    LLVMValueRef fn = LLVMAddFunction(lg.mod, "baga_char_at",
        LLVMFunctionType(lg.i64_ty, p, 2, 0));
    llv_h_begin(fn);
    LLVMValueRef s = LLVMGetParam(fn, 0);
    LLVMValueRef i = LLVMGetParam(fn, 1);
    LLVMValueRef p8 = LLVMBuildGEP2(lg.builder, lg.i8_ty, s, &i, 1, "p");
    LLVMValueRef ch = LLVMBuildLoad2(lg.builder, lg.i8_ty, p8, "c");
    LLVMBuildRet(lg.builder, LLVMBuildZExt(lg.builder, ch, lg.i64_ty, "r"));
    return fn;
}

/* static int64_t baga_byte_at(const char *s, int64_t i)
 * { return (int64_t)(unsigned char)s[i]; } — същото тяло като char_at
 * (огледало на codegen_c.c:5857), отделно име за паритет */
LLVMValueRef llv_build_baga_byte_at(void) {
    LLVMTypeRef p[] = { lg.ptr_ty, lg.i64_ty };
    LLVMValueRef fn = LLVMAddFunction(lg.mod, "baga_byte_at",
        LLVMFunctionType(lg.i64_ty, p, 2, 0));
    llv_h_begin(fn);
    LLVMValueRef s = LLVMGetParam(fn, 0);
    LLVMValueRef i = LLVMGetParam(fn, 1);
    LLVMValueRef p8 = LLVMBuildGEP2(lg.builder, lg.i8_ty, s, &i, 1, "p");
    LLVMValueRef ch = LLVMBuildLoad2(lg.builder, lg.i8_ty, p8, "c");
    LLVMBuildRet(lg.builder, LLVMBuildZExt(lg.builder, ch, lg.i64_ty, "r"));
    return fn;
}

/* static const char *baga_substr(const char *s, int64_t a, int64_t b) {
 *     int64_t n = b - a; if (n < 0) n = 0;
 *     char *r = malloc(n + 1); memcpy(r, s + a, n); r[n] = 0; return r; } */
LLVMValueRef llv_build_baga_substr(void) {
    LLVMTypeRef p[] = { lg.ptr_ty, lg.i64_ty, lg.i64_ty };
    LLVMValueRef fn = LLVMAddFunction(lg.mod, "baga_substr",
        LLVMFunctionType(lg.ptr_ty, p, 3, 0));
    llv_h_begin(fn);
    LLVMValueRef s = LLVMGetParam(fn, 0);
    LLVMValueRef a = LLVMGetParam(fn, 1);
    LLVMValueRef b = LLVMGetParam(fn, 2);
    LLVMValueRef n0 = LLVMBuildSub(lg.builder, b, a, "n0");
    LLVMValueRef neg = LLVMBuildICmp(lg.builder, LLVMIntSLT, n0,
        LLVMConstInt(lg.i64_ty, 0, 0), "neg");
    LLVMValueRef n = LLVMBuildSelect(lg.builder, neg,
        LLVMConstInt(lg.i64_ty, 0, 0), n0, "n");
    LLVMValueRef n1 = LLVMBuildAdd(lg.builder, n,
        LLVMConstInt(lg.i64_ty, 1, 0), "n1");
    LLVMValueRef r = llv_str_alloc_call(n1, "r");
    LLVMValueRef sa = LLVMBuildGEP2(lg.builder, lg.i8_ty, s, &a, 1, "sa");
    LLVMValueRef mc[] = { r, sa, n };
    llv_h_call(llv_rt_memcpy(), mc, 3, "mc");
    LLVMValueRef rn = LLVMBuildGEP2(lg.builder, lg.i8_ty, r, &n, 1, "rn");
    LLVMBuildStore(lg.builder, LLVMConstInt(lg.i8_ty, 0, 0), rn);
    LLVMValueRef ss[] = { r, n };
    llv_h_call(llv_baga_rt("baga_set_slen"), ss, 2, "");
    LLVMBuildRet(lg.builder, r);
    return fn;
}

/* static const char *baga_concat(const char *a, const char *b) {
 *     size_t la = strlen(a), lb = strlen(b);
 *     char *r = malloc(la + lb + 1);
 *     memcpy(r, a, la); memcpy(r + la, b, lb + 1); return r; } */
LLVMValueRef llv_build_baga_concat(void) {
    LLVMTypeRef p[] = { lg.ptr_ty, lg.ptr_ty };
    LLVMValueRef fn = LLVMAddFunction(lg.mod, "baga_concat",
        LLVMFunctionType(lg.ptr_ty, p, 2, 0));
    llv_h_begin(fn);
    LLVMValueRef a = LLVMGetParam(fn, 0);
    LLVMValueRef b = LLVMGetParam(fn, 1);
    LLVMValueRef aa[] = { a };
    LLVMValueRef la = llv_h_call(llv_baga_rt("baga_str_len"), aa, 1, "la");
    LLVMValueRef ba[] = { b };
    LLVMValueRef lb = llv_h_call(llv_baga_rt("baga_str_len"), ba, 1, "lb");
    LLVMValueRef n1 = LLVMBuildAdd(lg.builder,
        LLVMBuildAdd(lg.builder, la, lb, "n"),
        LLVMConstInt(lg.i64_ty, 1, 0), "n1");
    LLVMValueRef r = llv_str_alloc_call(n1, "r");
    LLVMValueRef mc1[] = { r, a, la };
    llv_h_call(llv_rt_memcpy(), mc1, 3, "mc1");
    LLVMValueRef rla = LLVMBuildGEP2(lg.builder, lg.i8_ty, r, &la, 1, "rla");
    LLVMValueRef lb1 = LLVMBuildAdd(lg.builder, lb,
        LLVMConstInt(lg.i64_ty, 1, 0), "lb1");
    LLVMValueRef mc2[] = { rla, b, lb1 };
    llv_h_call(llv_rt_memcpy(), mc2, 3, "mc2");
    LLVMValueRef ss[] = { r, LLVMBuildAdd(lg.builder, la, lb, "lab") };
    llv_h_call(llv_baga_rt("baga_set_slen"), ss, 2, "");
    LLVMBuildRet(lg.builder, r);
    return fn;
}

/* static int64_t baga_str_eq(const char *a, const char *b)
 * { return strcmp(a, b) == 0; } */
LLVMValueRef llv_build_baga_str_eq(void) {
    LLVMTypeRef p[] = { lg.ptr_ty, lg.ptr_ty };
    LLVMValueRef fn = LLVMAddFunction(lg.mod, "baga_str_eq",
        LLVMFunctionType(lg.i64_ty, p, 2, 0));
    llv_h_begin(fn);
    LLVMValueRef ab[] = { LLVMGetParam(fn, 0), LLVMGetParam(fn, 1) };
    LLVMValueRef c = llv_h_call(llv_rt_strcmp(), ab, 2, "c");
    LLVMValueRef eq = LLVMBuildICmp(lg.builder, LLVMIntEQ, c,
        LLVMConstInt(lg.i32_ty, 0, 0), "eq");
    LLVMBuildRet(lg.builder, LLVMBuildZExt(lg.builder, eq, lg.i64_ty, "r"));
    return fn;
}

/* static const char *baga_str_repeat(const char *s, int64_t n) — една
 * алокация (P55); RHS е чист, без concat-двойки. */
LLVMValueRef llv_build_baga_str_repeat(void) {
    LLVMTypeRef p[] = { lg.ptr_ty, lg.i64_ty };
    LLVMValueRef fn = LLVMAddFunction(lg.mod, "baga_str_repeat",
        LLVMFunctionType(lg.ptr_ty, p, 2, 0));
    llv_h_begin(fn);
    LLVMValueRef s = LLVMGetParam(fn, 0);
    LLVMValueRef n = LLVMGetParam(fn, 1);
    LLVMValueRef la[] = { s };
    LLVMValueRef l = llv_h_call(llv_baga_rt("baga_str_len"), la, 1, "l");
    LLVMValueRef npos = LLVMBuildSelect(lg.builder,
        LLVMBuildICmp(lg.builder, LLVMIntSGT, n, LLVMConstInt(lg.i64_ty, 0, 0), "npos?"),
        n, LLVMConstInt(lg.i64_ty, 0, 0), "npos");
    LLVMValueRef total = LLVMBuildMul(lg.builder, l, npos, "total");
    LLVMValueRef total1 = LLVMBuildAdd(lg.builder, total, LLVMConstInt(lg.i64_ty, 1, 0), "total1");
    LLVMValueRef r = llv_str_alloc_call(total1, "r");
    LLVMValueRef iv = LLVMBuildAlloca(lg.builder, lg.i64_ty, "i");
    LLVMBuildStore(lg.builder, LLVMConstInt(lg.i64_ty, 0, 0), iv);
    LLVMBasicBlockRef cond = LLVMAppendBasicBlockInContext(lg.ctx, fn, "rep_cond");
    LLVMBasicBlockRef body = LLVMAppendBasicBlockInContext(lg.ctx, fn, "rep_body");
    LLVMBasicBlockRef done = LLVMAppendBasicBlockInContext(lg.ctx, fn, "rep_done");
    LLVMBuildBr(lg.builder, cond);
    LLVMPositionBuilderAtEnd(lg.builder, cond);
    LLVMValueRef i = LLVMBuildLoad2(lg.builder, lg.i64_ty, iv, "i");
    LLVMValueRef cc = LLVMBuildICmp(lg.builder, LLVMIntSLT, i, npos, "cc");
    LLVMBuildCondBr(lg.builder, cc, body, done);
    LLVMPositionBuilderAtEnd(lg.builder, body);
    LLVMValueRef off = LLVMBuildMul(lg.builder, i, l, "off");
    LLVMValueRef dp = LLVMBuildGEP2(lg.builder, lg.i8_ty, r, &off, 1, "dp");
    LLVMBuildMemCpy(lg.builder, dp, 1, s, 1, l);
    LLVMValueRef inext = LLVMBuildAdd(lg.builder, i, LLVMConstInt(lg.i64_ty, 1, 0), "in");
    LLVMBuildStore(lg.builder, inext, iv);
    LLVMBuildBr(lg.builder, cond);
    LLVMPositionBuilderAtEnd(lg.builder, done);
    LLVMValueRef zp = LLVMBuildGEP2(lg.builder, lg.i8_ty, r, &total, 1, "zp");
    LLVMBuildStore(lg.builder, LLVMConstInt(lg.i8_ty, 0, 0), zp);
    LLVMValueRef ss[] = { r, total };
    llv_h_call(llv_baga_rt("baga_set_slen"), ss, 2, "");
    LLVMBuildRet(lg.builder, r);
    return fn;
}

/* static const char *baga_byte_chr(int64_t c) — един суров байт (C parity:
 * baga_alloc(2), r[0]=(char)c, r[1]=0, set_slen(c==0?0:1)). */
LLVMValueRef llv_build_baga_byte_chr(void) {
    LLVMTypeRef p[] = { lg.i64_ty };
    LLVMValueRef fn = LLVMAddFunction(lg.mod, "baga_byte_chr",
        LLVMFunctionType(lg.ptr_ty, p, 1, 0));
    llv_h_begin(fn);
    LLVMValueRef c = LLVMGetParam(fn, 0);
    LLVMValueRef r = llv_str_alloc_call(LLVMConstInt(lg.i64_ty, 2, 0), "r");
    LLVMValueRef i0 = LLVMConstInt(lg.i64_ty, 0, 0);
    LLVMValueRef p0 = LLVMBuildGEP2(lg.builder, lg.i8_ty, r, &i0, 1, "p0");
    LLVMBuildStore(lg.builder, LLVMBuildTrunc(lg.builder, c, lg.i8_ty, "c8"), p0);
    LLVMValueRef i1 = LLVMConstInt(lg.i64_ty, 1, 0);
    LLVMValueRef p1 = LLVMBuildGEP2(lg.builder, lg.i8_ty, r, &i1, 1, "p1");
    LLVMBuildStore(lg.builder, LLVMConstInt(lg.i8_ty, 0, 0), p1);
    LLVMValueRef isz0 = LLVMBuildICmp(lg.builder, LLVMIntEQ, c,
        LLVMConstInt(lg.i64_ty, 0, 0), "isz0");
    LLVMValueRef ln = LLVMBuildSelect(lg.builder, isz0,
        LLVMConstInt(lg.i64_ty, 0, 0), LLVMConstInt(lg.i64_ty, 1, 0), "ln");
    LLVMValueRef ss[] = { r, ln };
    llv_h_call(llv_baga_rt("baga_set_slen"), ss, 2, "");
    LLVMBuildRet(lg.builder, r);
    return fn;
}

/* static const char *baga_chr(int64_t c) — UTF-8 encode (1–4 bytes);
 * оглежда C runtime helper-а. Преди LP3 LLVM версията орязваше до един
 * байт ((char)c) и се разминаваше с C за code point-ове ≥ 0x80. */
LLVMValueRef llv_build_baga_chr(void) {
    LLVMTypeRef p[] = { lg.i64_ty };
    LLVMValueRef fn = LLVMAddFunction(lg.mod, "baga_chr",
        LLVMFunctionType(lg.ptr_ty, p, 1, 0));
    llv_h_begin(fn);
    LLVMValueRef c = LLVMGetParam(fn, 0);
    LLVMValueRef five = LLVMConstInt(lg.i64_ty, 5, 0);
    LLVMValueRef r = llv_str_alloc_call(five, "r");

    LLVMBasicBlockRef b1 = LLVMAppendBasicBlockInContext(lg.ctx, fn, "u1");
    LLVMBasicBlockRef k2 = LLVMAppendBasicBlockInContext(lg.ctx, fn, "k2");
    LLVMBasicBlockRef b2 = LLVMAppendBasicBlockInContext(lg.ctx, fn, "u2");
    LLVMBasicBlockRef k3 = LLVMAppendBasicBlockInContext(lg.ctx, fn, "k3");
    LLVMBasicBlockRef b3 = LLVMAppendBasicBlockInContext(lg.ctx, fn, "u3");
    LLVMBasicBlockRef b4 = LLVMAppendBasicBlockInContext(lg.ctx, fn, "u4");

    LLVMValueRef lt1 = LLVMBuildICmp(lg.builder, LLVMIntULT, c,
        LLVMConstInt(lg.i64_ty, 0x80, 0), "lt1");
    LLVMBuildCondBr(lg.builder, lt1, b1, k2);

    LLVMPositionBuilderAtEnd(lg.builder, k2);
    LLVMValueRef lt2 = LLVMBuildICmp(lg.builder, LLVMIntULT, c,
        LLVMConstInt(lg.i64_ty, 0x800, 0), "lt2");
    LLVMBuildCondBr(lg.builder, lt2, b2, k3);

    LLVMPositionBuilderAtEnd(lg.builder, k3);
    LLVMValueRef lt3 = LLVMBuildICmp(lg.builder, LLVMIntULT, c,
        LLVMConstInt(lg.i64_ty, 0x10000, 0), "lt3");
    LLVMBuildCondBr(lg.builder, lt3, b3, b4);

    #define CHR_SH(v, n) LLVMBuildLShr(lg.builder, (v), LLVMConstInt(lg.i64_ty, (n), 0), "s")
    #define CHR_AN(v, m) LLVMBuildAnd(lg.builder, (v), LLVMConstInt(lg.i64_ty, (m), 0), "a")
    #define CHR_OR(v, m) LLVMBuildOr(lg.builder, (v), LLVMConstInt(lg.i64_ty, (m), 0), "o")
    #define CHR_ST(idx, val) do { \
        LLVMValueRef _i = LLVMConstInt(lg.i64_ty, (idx), 0); \
        LLVMValueRef _p = LLVMBuildGEP2(lg.builder, lg.i8_ty, r, &_i, 1, "sp"); \
        LLVMBuildStore(lg.builder, LLVMBuildTrunc(lg.builder, (val), lg.i8_ty, "st"), _p); \
    } while (0)

    LLVMPositionBuilderAtEnd(lg.builder, b1);
    CHR_ST(0, c);
    CHR_ST(1, LLVMConstInt(lg.i64_ty, 0, 0));
    {
        LLVMValueRef isz0 = LLVMBuildICmp(lg.builder, LLVMIntEQ, c,
            LLVMConstInt(lg.i64_ty, 0, 0), "isz0");
        LLVMValueRef ln = LLVMBuildSelect(lg.builder, isz0,
            LLVMConstInt(lg.i64_ty, 0, 0), LLVMConstInt(lg.i64_ty, 1, 0), "ln");
        LLVMValueRef ss[] = { r, ln };
        llv_h_call(llv_baga_rt("baga_set_slen"), ss, 2, "");
    }
    LLVMBuildRet(lg.builder, r);

    LLVMPositionBuilderAtEnd(lg.builder, b2);
    CHR_ST(0, CHR_OR(CHR_SH(c, 6), 0xC0));
    CHR_ST(1, CHR_OR(CHR_AN(c, 0x3F), 0x80));
    CHR_ST(2, LLVMConstInt(lg.i64_ty, 0, 0));
    {
        LLVMValueRef ss[] = { r, LLVMConstInt(lg.i64_ty, 2, 0) };
        llv_h_call(llv_baga_rt("baga_set_slen"), ss, 2, "");
    }
    LLVMBuildRet(lg.builder, r);

    LLVMPositionBuilderAtEnd(lg.builder, b3);
    CHR_ST(0, CHR_OR(CHR_SH(c, 12), 0xE0));
    CHR_ST(1, CHR_OR(CHR_AN(CHR_SH(c, 6), 0x3F), 0x80));
    CHR_ST(2, CHR_OR(CHR_AN(c, 0x3F), 0x80));
    CHR_ST(3, LLVMConstInt(lg.i64_ty, 0, 0));
    {
        LLVMValueRef ss[] = { r, LLVMConstInt(lg.i64_ty, 3, 0) };
        llv_h_call(llv_baga_rt("baga_set_slen"), ss, 2, "");
    }
    LLVMBuildRet(lg.builder, r);

    LLVMPositionBuilderAtEnd(lg.builder, b4);
    CHR_ST(0, CHR_OR(CHR_SH(c, 18), 0xF0));
    CHR_ST(1, CHR_OR(CHR_AN(CHR_SH(c, 12), 0x3F), 0x80));
    CHR_ST(2, CHR_OR(CHR_AN(CHR_SH(c, 6), 0x3F), 0x80));
    CHR_ST(3, CHR_OR(CHR_AN(c, 0x3F), 0x80));
    CHR_ST(4, LLVMConstInt(lg.i64_ty, 0, 0));
    {
        LLVMValueRef ss[] = { r, LLVMConstInt(lg.i64_ty, 4, 0) };
        llv_h_call(llv_baga_rt("baga_set_slen"), ss, 2, "");
    }
    LLVMBuildRet(lg.builder, r);

    #undef CHR_SH
    #undef CHR_AN
    #undef CHR_OR
    #undef CHR_ST
    return fn;
}

/* static int64_t baga_ord(const char *s) — UTF-8 code point of s[0..].
 * Mirrors the C runtime helper (1–4 byte sequences) so the LLVM oracle
 * matches byte-for-byte on non-ASCII input. */
LLVMValueRef llv_build_baga_ord(void) {
    LLVMTypeRef p[] = { lg.ptr_ty };
    LLVMValueRef fn = LLVMAddFunction(lg.mod, "baga_ord",
        LLVMFunctionType(lg.i64_ty, p, 1, 0));
    llv_h_begin(fn);
    LLVMValueRef s = LLVMGetParam(fn, 0);
    LLVMValueRef z = LLVMConstInt(lg.i64_ty, 0, 0);
    LLVMValueRef one = LLVMConstInt(lg.i64_ty, 1, 0);

    /* c8 = s[0]; empty string → 0 */
    LLVMValueRef p0 = LLVMBuildGEP2(lg.builder, lg.i8_ty, s, &z, 1, "p0");
    LLVMValueRef c8 = LLVMBuildLoad2(lg.builder, lg.i8_ty, p0, "c8");
    LLVMValueRef nz = LLVMBuildICmp(lg.builder, LLVMIntNE, c8,
        LLVMConstInt(lg.i8_ty, 0, 0), "nz");
    /* c lives in an alloca so every block sees the same value */
    LLVMValueRef ca = llv_entry_alloca(lg.i64_ty, "ca");
    LLVMBuildStore(lg.builder, LLVMBuildZExt(lg.builder, c8, lg.i64_ty, "c"), ca);

    LLVMBasicBlockRef zero_b  = LLVMAppendBasicBlockInContext(lg.ctx, fn, "zero");
    LLVMBasicBlockRef chk_b   = LLVMAppendBasicBlockInContext(lg.ctx, fn, "chk");
    LLVMBasicBlockRef ascii_b = LLVMAppendBasicBlockInContext(lg.ctx, fn, "ascii");
    LLVMBasicBlockRef two_b   = LLVMAppendBasicBlockInContext(lg.ctx, fn, "two");
    LLVMBasicBlockRef three_b = LLVMAppendBasicBlockInContext(lg.ctx, fn, "three");
    LLVMBasicBlockRef four_b  = LLVMAppendBasicBlockInContext(lg.ctx, fn, "four");
    LLVMBuildCondBr(lg.builder, nz, chk_b, zero_b);

    /* chk: c < 0x80 → ascii; (c&0xE0)==0xC0 → two; (c&0xF0)==0xE0 → three; else four */
    LLVMPositionBuilderAtEnd(lg.builder, chk_b);
    LLVMValueRef c = LLVMBuildLoad2(lg.builder, lg.i64_ty, ca, "c");
    LLVMValueRef is_ascii = LLVMBuildICmp(lg.builder, LLVMIntULT, c,
        LLVMConstInt(lg.i64_ty, 0x80, 0), "is_ascii");
    LLVMBasicBlockRef chk2_b = LLVMAppendBasicBlockInContext(lg.ctx, fn, "chk2");
    LLVMBuildCondBr(lg.builder, is_ascii, ascii_b, chk2_b);

    LLVMPositionBuilderAtEnd(lg.builder, chk2_b);
    LLVMValueRef mE0 = LLVMBuildAnd(lg.builder, c,
        LLVMConstInt(lg.i64_ty, 0xE0, 0), "mE0");
    LLVMValueRef is_two = LLVMBuildICmp(lg.builder, LLVMIntEQ, mE0,
        LLVMConstInt(lg.i64_ty, 0xC0, 0), "is_two");
    LLVMBasicBlockRef chk3_b = LLVMAppendBasicBlockInContext(lg.ctx, fn, "chk3");
    LLVMBuildCondBr(lg.builder, is_two, two_b, chk3_b);

    LLVMPositionBuilderAtEnd(lg.builder, chk3_b);
    LLVMValueRef mF0 = LLVMBuildAnd(lg.builder, c,
        LLVMConstInt(lg.i64_ty, 0xF0, 0), "mF0");
    LLVMValueRef is_three = LLVMBuildICmp(lg.builder, LLVMIntEQ, mF0,
        LLVMConstInt(lg.i64_ty, 0xE0, 0), "is_three");
    LLVMBuildCondBr(lg.builder, is_three, three_b, four_b);

    LLVMPositionBuilderAtEnd(lg.builder, zero_b);
    LLVMBuildRet(lg.builder, z);

    LLVMPositionBuilderAtEnd(lg.builder, ascii_b);
    LLVMBuildRet(lg.builder, LLVMBuildLoad2(lg.builder, lg.i64_ty, ca, "c1"));

    /* continuation bytes: dst = s[idx] & 0x3F (zext i8 → i64 first) */
    #define ORD_CONT(idx, dst, nm) do { \
        LLVMValueRef nm ## p = LLVMBuildGEP2(lg.builder, lg.i8_ty, s, &(idx), 1, #nm "p"); \
        LLVMValueRef nm ## b8 = LLVMBuildLoad2(lg.builder, lg.i8_ty, nm ## p, #nm "b8"); \
        dst = LLVMBuildAnd(lg.builder, \
            LLVMBuildZExt(lg.builder, nm ## b8, lg.i64_ty, #nm "e"), \
            LLVMConstInt(lg.i64_ty, 0x3F, 0), #nm); \
    } while (0)

    LLVMPositionBuilderAtEnd(lg.builder, two_b);
    LLVMValueRef cv2 = LLVMBuildLoad2(lg.builder, lg.i64_ty, ca, "c2");
    LLVMValueRef b1;
    ORD_CONT(one, b1, t1);
    LLVMValueRef head2 = LLVMBuildShl(lg.builder,
        LLVMBuildAnd(lg.builder, cv2, LLVMConstInt(lg.i64_ty, 0x1F, 0), "h2"),
        LLVMConstInt(lg.i64_ty, 6, 0), "h2s");
    LLVMBuildRet(lg.builder, LLVMBuildOr(lg.builder, head2, b1, "r2"));

    LLVMPositionBuilderAtEnd(lg.builder, three_b);
    LLVMValueRef cv3 = LLVMBuildLoad2(lg.builder, lg.i64_ty, ca, "c3");
    LLVMValueRef two_i = LLVMConstInt(lg.i64_ty, 2, 0);
    LLVMValueRef b2;
    ORD_CONT(one, b1, u1);
    ORD_CONT(two_i, b2, u2);
    LLVMValueRef head3 = LLVMBuildShl(lg.builder,
        LLVMBuildAnd(lg.builder, cv3, LLVMConstInt(lg.i64_ty, 0x0F, 0), "h3"),
        LLVMConstInt(lg.i64_ty, 12, 0), "h3s");
    LLVMValueRef mid3 = LLVMBuildShl(lg.builder, b1,
        LLVMConstInt(lg.i64_ty, 6, 0), "m3");
    LLVMBuildRet(lg.builder, LLVMBuildOr(lg.builder,
        LLVMBuildOr(lg.builder, head3, mid3, "hm3"), b2, "r3"));

    LLVMPositionBuilderAtEnd(lg.builder, four_b);
    LLVMValueRef cv4 = LLVMBuildLoad2(lg.builder, lg.i64_ty, ca, "c4");
    LLVMValueRef three_i = LLVMConstInt(lg.i64_ty, 3, 0);
    LLVMValueRef b3;
    ORD_CONT(one, b1, v1);
    ORD_CONT(two_i, b2, v2);
    ORD_CONT(three_i, b3, v3);
    LLVMValueRef head4 = LLVMBuildShl(lg.builder,
        LLVMBuildAnd(lg.builder, cv4, LLVMConstInt(lg.i64_ty, 0x07, 0), "h4"),
        LLVMConstInt(lg.i64_ty, 18, 0), "h4s");
    LLVMValueRef m4a = LLVMBuildShl(lg.builder, b1,
        LLVMConstInt(lg.i64_ty, 12, 0), "m4a");
    LLVMValueRef m4b = LLVMBuildShl(lg.builder, b2,
        LLVMConstInt(lg.i64_ty, 6, 0), "m4b");
    LLVMBuildRet(lg.builder, LLVMBuildOr(lg.builder,
        LLVMBuildOr(lg.builder,
            LLVMBuildOr(lg.builder, head4, m4a, "hm4"), m4b, "hmm4"), b3, "r4"));
    #undef ORD_CONT
    return fn;
}

/* static const char *baga_i64_to_str(int64_t x) — decimal, '-' for negatives.
 * Mirrors the C runtime helper so interpolation is C/LLVM-parity. */
LLVMValueRef llv_build_baga_i64_to_str(void) {
    LLVMTypeRef p[] = { lg.i64_ty };
    LLVMValueRef fn = LLVMAddFunction(lg.mod, "baga_i64_to_str",
        LLVMFunctionType(lg.ptr_ty, p, 1, 0));
    llv_h_begin(fn);
    LLVMValueRef x = LLVMGetParam(fn, 0);
    LLVMValueRef i0 = LLVMConstInt(lg.i64_ty, 0, 0);
    LLVMValueRef i1 = LLVMConstInt(lg.i64_ty, 1, 0);
    LLVMValueRef i10 = LLVMConstInt(lg.i64_ty, 10, 0);
    LLVMValueRef i48 = LLVMConstInt(lg.i64_ty, 48, 0);
    LLVMValueRef i24 = LLVMConstInt(lg.i64_ty, 24, 0);
    LLVMValueRef tmp = llv_h_call(llv_rt_malloc(), (LLVMValueRef[]){ i24 }, 1, "tmp");
    LLVMValueRef v = llv_entry_alloca(lg.i64_ty, "v");
    LLVMValueRef neg = llv_entry_alloca(lg.i64_ty, "neg");
    LLVMValueRef cnt = llv_entry_alloca(lg.i64_ty, "cnt");
    LLVMValueRef isneg = LLVMBuildICmp(lg.builder, LLVMIntSLT, x, i0, "isneg");
    LLVMBuildStore(lg.builder, LLVMBuildZExt(lg.builder, isneg, lg.i64_ty, "isneg64"), neg);
    LLVMValueRef negv = LLVMBuildSub(lg.builder, i0, x, "negv");
    LLVMValueRef v0 = LLVMBuildSelect(lg.builder, isneg, negv, x, "v0");
    LLVMBuildStore(lg.builder, v0, v);
    LLVMBuildStore(lg.builder, i0, cnt);
    LLVMValueRef isz = LLVMBuildICmp(lg.builder, LLVMIntEQ, v0, i0, "isz");
    LLVMBasicBlockRef zb = LLVMAppendBasicBlockInContext(lg.ctx, fn, "zero");
    LLVMBasicBlockRef lb = LLVMAppendBasicBlockInContext(lg.ctx, fn, "loop");
    LLVMBasicBlockRef lb2 = LLVMAppendBasicBlockInContext(lg.ctx, fn, "loop2");
    LLVMBuildCondBr(lg.builder, isz, zb, lb);
    LLVMPositionBuilderAtEnd(lg.builder, zb);
    LLVMBuildStore(lg.builder, LLVMConstInt(lg.i8_ty, 48, 0),
        LLVMBuildGEP2(lg.builder, lg.i8_ty, tmp, &i0, 1, "zp"));
    LLVMBuildStore(lg.builder, i1, cnt);
    LLVMBuildBr(lg.builder, lb2);
    LLVMPositionBuilderAtEnd(lg.builder, lb);
    LLVMValueRef cv = LLVMBuildLoad2(lg.builder, lg.i64_ty, v, "cv");
    LLVMValueRef rem = LLVMBuildBinOp(lg.builder, LLVMSRem, cv, i10, "rem");
    LLVMValueRef dch = LLVMBuildTrunc(lg.builder, LLVMBuildAdd(lg.builder, rem, i48, "d"), lg.i8_ty, "dch");
    LLVMValueRef ci = LLVMBuildLoad2(lg.builder, lg.i64_ty, cnt, "ci");
    LLVMBuildStore(lg.builder, dch, LLVMBuildGEP2(lg.builder, lg.i8_ty, tmp, &ci, 1, "dp"));
    LLVMBuildStore(lg.builder, LLVMBuildAdd(lg.builder, ci, i1, "ci1"), cnt);
    LLVMBuildStore(lg.builder, LLVMBuildBinOp(lg.builder, LLVMSDiv, cv, i10, "q"), v);
    LLVMBuildBr(lg.builder, lb2);
    LLVMPositionBuilderAtEnd(lg.builder, lb2);
    LLVMValueRef cv2 = LLVMBuildLoad2(lg.builder, lg.i64_ty, v, "cv2");
    LLVMValueRef cont = LLVMBuildICmp(lg.builder, LLVMIntSGT, cv2, i0, "cont");
    LLVMBasicBlockRef rb = LLVMAppendBasicBlockInContext(lg.ctx, fn, "rev");
    LLVMBuildCondBr(lg.builder, cont, lb, rb);
    LLVMPositionBuilderAtEnd(lg.builder, rb);
    LLVMValueRef c = LLVMBuildLoad2(lg.builder, lg.i64_ty, cnt, "c");
    LLVMValueRef isng64 = LLVMBuildLoad2(lg.builder, lg.i64_ty, neg, "isng64");
    LLVMValueRef isng = LLVMBuildICmp(lg.builder, LLVMIntNE, isng64,
        LLVMConstInt(lg.i64_ty, 0, 0), "isng");
    LLVMValueRef tlen = LLVMBuildAdd(lg.builder, c, isng64, "tlen");   /* +1 if negative */
    LLVMValueRef rlen = LLVMBuildAdd(lg.builder, tlen, i1, "rlen");
    LLVMValueRef res = llv_str_alloc_call(rlen, "res");
    LLVMValueRef rp = llv_entry_alloca(lg.i64_ty, "rp");
    LLVMBuildStore(lg.builder, i0, rp);
    LLVMBasicBlockRef mb = LLVMAppendBasicBlockInContext(lg.ctx, fn, "minus");
    LLVMBasicBlockRef rvb = LLVMAppendBasicBlockInContext(lg.ctx, fn, "revloop");
    LLVMBuildCondBr(lg.builder, isng, mb, rvb);
    LLVMPositionBuilderAtEnd(lg.builder, mb);
    LLVMBuildStore(lg.builder, LLVMConstInt(lg.i8_ty, 45, 0),
        LLVMBuildGEP2(lg.builder, lg.i8_ty, res, &i0, 1, "mp"));
    LLVMBuildStore(lg.builder, i1, rp);
    LLVMBuildBr(lg.builder, rvb);
    LLVMPositionBuilderAtEnd(lg.builder, rvb);
    LLVMBasicBlockRef cond = LLVMAppendBasicBlockInContext(lg.ctx, fn, "cond");
    LLVMBasicBlockRef body = LLVMAppendBasicBlockInContext(lg.ctx, fn, "body");
    LLVMBasicBlockRef done = LLVMAppendBasicBlockInContext(lg.ctx, fn, "done");
    LLVMBuildBr(lg.builder, cond);
    LLVMPositionBuilderAtEnd(lg.builder, cond);
    LLVMValueRef rpv = LLVMBuildLoad2(lg.builder, lg.i64_ty, rp, "rpv");
    LLVMValueRef idx = LLVMBuildSub(lg.builder, LLVMBuildSub(lg.builder, tlen, i1, "t1"), rpv, "idx");
    LLVMValueRef done_c = LLVMBuildICmp(lg.builder, LLVMIntSGE, rpv, tlen, "donec");
    LLVMBuildCondBr(lg.builder, done_c, done, body);
    LLVMPositionBuilderAtEnd(lg.builder, body);
    LLVMValueRef ch2 = LLVMBuildLoad2(lg.builder, lg.i8_ty,
        LLVMBuildGEP2(lg.builder, lg.i8_ty, tmp, &idx, 1, "sp"), "ch2");
    LLVMBuildStore(lg.builder, ch2, LLVMBuildGEP2(lg.builder, lg.i8_ty, res, &rpv, 1, "dtp"));
    LLVMBuildStore(lg.builder, LLVMBuildAdd(lg.builder, rpv, i1, "rp1"), rp);
    LLVMBuildBr(lg.builder, cond);
    LLVMPositionBuilderAtEnd(lg.builder, done);
    LLVMValueRef rpv2 = LLVMBuildLoad2(lg.builder, lg.i64_ty, rp, "rpv2");
    LLVMBuildStore(lg.builder, LLVMConstInt(lg.i8_ty, 0, 0),
        LLVMBuildGEP2(lg.builder, lg.i8_ty, res, &rpv2, 1, "np"));
    LLVMValueRef ss[] = { res, tlen };
    llv_h_call(llv_baga_rt("baga_set_slen"), ss, 2, "");
    LLVMBuildRet(lg.builder, res);
    return fn;
}

/* static const char *baga_f64_to_str(double x) — "%g" като baga_print_f64;
 * оглежда C runtime helper-а (snprintf в 32-байтов buffer от malloc). */
LLVMValueRef llv_build_baga_f64_to_str(void) {
    LLVMTypeRef p[] = { LLVMDoubleTypeInContext(lg.ctx) };
    LLVMValueRef fn = LLVMAddFunction(lg.mod, "baga_f64_to_str",
        LLVMFunctionType(lg.ptr_ty, p, 1, 0));
    llv_h_begin(fn);
    LLVMValueRef x = LLVMGetParam(fn, 0);
    LLVMValueRef i32 = LLVMConstInt(lg.i64_ty, 32, 0);
    LLVMValueRef r = llv_str_alloc_call(i32, "r");
    LLVMValueRef fmt = LLVMBuildGlobalStringPtr(lg.builder, "%g", "f64fmt");
    LLVMValueRef args[] = { r, i32, fmt, x };
    LLVMValueRef n32 = LLVMBuildCall2(lg.builder, LLVMGlobalGetValueType(lg.snprintf_fn),
                   lg.snprintf_fn, args, 4, "sn");
    LLVMValueRef ss[] = { r, LLVMBuildSExt(lg.builder, n32, lg.i64_ty, "sn64") };
    llv_h_call(llv_baga_rt("baga_set_slen"), ss, 2, "");
    LLVMBuildRet(lg.builder, r);
    return fn;
}

#endif /* BAGA_LLVM */
