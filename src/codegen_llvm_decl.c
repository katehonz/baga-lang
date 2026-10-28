/* codegen_llvm_decl.c — част от codegen_llvm.c (виж codegen_llvm_impl.h). */
#include "baga.h"
#include "codegen_llvm_impl.h"

/* ---- Statement emission ---- */


#ifdef BAGA_LLVM

void llv_emit_stmt_llvm(Node *n, LLVMBasicBlockRef break_bb, LLVMBasicBlockRef cont_bb);
LLVMValueRef llv_emit_block_value_llvm(Node *block);

static void emit_block_llvm(Node *block, LLVMBasicBlockRef break_bb, LLVMBasicBlockRef cont_bb) {
    if (!block) return;
    if (block->kind != NODE_BLOCK)
        llv_llvm_unsupported("тяло, което не е блок");
    llv_st_push();
    llv_lrc_push_scope(lrc_loop_next);
    lrc_loop_next = 0;
    for (int i = 0; i < block->stmts.len; i++) {
        /* мъртъв код след terminator — както gcc след return */
        if (LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(lg.builder)))
            break;
        llv_emit_stmt_llvm(block->stmts.data[i], break_bb, cont_bb);
    }
    llv_lrc_pop_scope();
    llv_st_pop();
}

void llv_emit_stmt_llvm(Node *n, LLVMBasicBlockRef break_bb, LLVMBasicBlockRef cont_bb) {
    if (!n) return;

    switch (n->kind) {
        case NODE_LET: {
            LLVMTypeRef ty;
            if (n->let_type) ty = llv_llvm_type(n->let_type);
            else if (n->let_init && n->let_init->type) ty = llv_llvm_type_resolved(n->let_init->type);
            else ty = lg.i64_ty;
            char *m = llv_llvm_mangle(n->let_name);
            LLVMValueRef alloca = llv_entry_alloca(ty, m);
            free(m);
            /* RC4: temp-ове в init (root-ът е bound — не е temp) */
            LLRcTmpSave tsv;
            if (lg.rc) llv_lrc_tmp_begin(n->let_init, 1, &tsv);
            if (n->let_init) {
                LLVMValueRef val = llv_emit_expr_llvm(n->let_init);
                if (!val) llv_llvm_unsupported("print като стойност на let");
                val = llv_coerce(val, ty);
                /* RC: alias (let x = y) → retain преди store; свежата
                 * конструкция идва с rc=1 от alloc — без retain. Borrowed
                 * init (vec_get/map_get/поле/h_*) също се retain-ва —
                 * референцията става собствена (порт на rc_borrowed_init
                 * сайта codegen_c.c:3940; без borrow elision-а на C —
                 * двойката retain+release е балансирана). tag 5/6 borrowed
                 * не се track-ва (gating-ът по-долу, като C) → и не се
                 * retain-ва (leak-safe). */
                if (lg.rc) {
                    if (n->let_init->kind == NODE_IDENT)
                        llv_lrc_emit_retain_val(llv_lrc_heap_tag(n->let_init->type),
                                            n->let_init->type, NULL, val);
                    else if (llv_lrc_borrowed_init(n->let_init)) {
                        int btag = llv_lrc_heap_tag(n->let_init->type);
                        if (!btag && n->let_type)
                            btag = llv_lrc_heap_tag_node(n->let_type);
                        if (btag && btag < 5)
                            llv_lrc_emit_retain_val(btag, n->let_init->type,
                                                NULL, val);
                    }
                }
                LLVMBuildStore(lg.builder, val, alloca);
            }
            llv_st_define(n->let_name, alloca);
            /* RC: tag от init типа; fallback към анотацията (`let v: Vec<str>
             * = vec_new()` — inferred типът няма elem информация, но kind
             * стига за tag-а). Като rc_heap_tag + rc_heap_tag_node в C.
             * Task 6: tag 5/6 се регистрират САМО при свеж литерал/ctor или
             * alias на track-нат ident (codegen_c.c:3898-3920) — fn
             * резултат/vec_get/поле споделят собствеността и се остават
             * untrack-нати (leak-safe, като C). */
            if (lg.rc) {
                int ltag = n->let_init ? llv_lrc_heap_tag(n->let_init->type) : 0;
                if (!ltag && n->let_type) ltag = llv_lrc_heap_tag_node(n->let_type);
                if (ltag == 5 || ltag == 6) {
                    int fresh = n->let_init &&
                        (n->let_init->kind == NODE_STRUCT_LIT ||
                         llv_lrc_is_enum_ctor(n->let_init));
                    int from_tr = 0;
                    if (n->let_init && n->let_init->kind == NODE_IDENT) {
                        int si = llv_lrc_find(n->let_init->name);
                        if (si >= 0 && !lg.lrc_locals[si].dead) from_tr = 1;
                    }
                    if (!fresh && !from_tr) ltag = 0;
                }
                llv_lrc_track_tag(n->let_name, ltag,
                              n->let_init ? n->let_init->type : NULL,
                              n->let_type, 0);
            }
            if (lg.rc) llv_lrc_tmp_end(&tsv);
            break;
        }

        case NODE_RETURN: {
            if (n->ret_val) {
                /* RC4: temp-ове в return израза (root-ът отива на caller-а) */
                LLRcTmpSave tsv;
                if (lg.rc) llv_lrc_tmp_begin(n->ret_val, 1, &tsv);
                LLVMValueRef val = llv_emit_expr_llvm(n->ret_val);
                if (!val) llv_llvm_unsupported("print като return стойност");
                val = llv_coerce(val, lg.cur_ret_ty);
                /* RC4: release на temp-овете преди release на локалите
                 * (като rc_tmp_release_all в emit_return_val, codegen_c) */
                if (lg.rc) llv_lrc_tmp_release_all();
                /* RC: move на върнатия локал + release на останалите */
                llv_lrc_return_move(n->ret_val, val);
                LLVMBuildRet(lg.builder, val);
                if (lg.rc) llv_lrc_tmp_restore(&tsv);
            } else {
                if (lg.cur_ret_ty != lg.void_ty)
                    llv_llvm_unsupported("return без стойност в не-void функция");
                llv_lrc_release_all(-1);
                LLVMBuildRetVoid(lg.builder);
            }
            break;
        }

        case NODE_IF: {
            /* RC4 v0.3: temp-ове в условието се release-ват веднага след
             * оценката — преди клоновете. В LLVM не трябва GNU ({…}) wrap:
             * temp-овете се emit-ват и release-ват в текущия block. */
            LLRcTmpSave tsv;
            if (lg.rc) llv_lrc_tmp_begin(n->cond, 0, &tsv);
            LLVMValueRef cond = llv_to_bool(llv_emit_expr_llvm(n->cond));
            if (lg.rc) llv_lrc_tmp_end(&tsv);
            LLVMValueRef fn = LLVMGetBasicBlockParent(LLVMGetInsertBlock(lg.builder));
            LLVMBasicBlockRef then_bb = LLVMAppendBasicBlockInContext(lg.ctx, fn, "then");
            LLVMBasicBlockRef else_bb = LLVMAppendBasicBlockInContext(lg.ctx, fn, "else");
            LLVMBasicBlockRef merge_bb = LLVMAppendBasicBlockInContext(lg.ctx, fn, "merge");

            LLVMBuildCondBr(lg.builder, cond, then_bb, else_bb);

            lg.lrc_branch_depth++;   /* then/else са условен поток */
            LLVMPositionBuilderAtEnd(lg.builder, then_bb);
            emit_block_llvm(n->then_br, break_bb, cont_bb);
            if (!LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(lg.builder)))
                LLVMBuildBr(lg.builder, merge_bb);

            LLVMPositionBuilderAtEnd(lg.builder, else_bb);
            if (n->else_br) emit_block_llvm(n->else_br, break_bb, cont_bb);
            if (!LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(lg.builder)))
                LLVMBuildBr(lg.builder, merge_bb);
            lg.lrc_branch_depth--;

            LLVMPositionBuilderAtEnd(lg.builder, merge_bb);
            break;
        }

        case NODE_WHILE: {
            LLVMValueRef fn = LLVMGetBasicBlockParent(LLVMGetInsertBlock(lg.builder));
            LLVMBasicBlockRef cond_bb = LLVMAppendBasicBlockInContext(lg.ctx, fn, "while_cond");
            LLVMBasicBlockRef body_bb = LLVMAppendBasicBlockInContext(lg.ctx, fn, "while_body");
            LLVMBasicBlockRef end_bb = LLVMAppendBasicBlockInContext(lg.ctx, fn, "while_end");

            LLVMBuildBr(lg.builder, cond_bb);

            LLVMPositionBuilderAtEnd(lg.builder, cond_bb);
            /* RC4 v0.3: cond_bb се изпълнява на всяка итерация — temp
             * emission + release тук са естественият per-iteration път
             * (без GNU wrap трика на C) */
            LLRcTmpSave tsv;
            if (lg.rc) llv_lrc_tmp_begin(n->while_cond, 0, &tsv);
            LLVMValueRef cond = llv_to_bool(llv_emit_expr_llvm(n->while_cond));
            if (lg.rc) llv_lrc_tmp_end(&tsv);
            LLVMBuildCondBr(lg.builder, cond, body_bb, end_bb);

            LLVMPositionBuilderAtEnd(lg.builder, body_bb);
            lrc_loop_next = 1;   /* тялото на while е loop scope (break/continue) */
            lg.lrc_branch_depth++;   /* тялото се изпълнява условно/повторно */
            emit_block_llvm(n->while_body, end_bb, cond_bb);
            lg.lrc_branch_depth--;
            if (!LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(lg.builder)))
                LLVMBuildBr(lg.builder, cond_bb);

            LLVMPositionBuilderAtEnd(lg.builder, end_bb);
            break;
        }

        case NODE_FOR: {
            /* for x in lo..hi { } → x = lo; while x < hi { body; x++ }
             * hi се преизчислява на всяка итерация, както в codegen_c */
            if (!n->for_iter || n->for_iter->kind != NODE_RANGE)
                llv_llvm_unsupported("for без диапазон (a..b)");
            /* RC4 v0.3: lo temp-ове се оценяват веднъж (init) и се
             * release-ват веднага след изчислената граница */
            LLRcTmpSave tsv_lo;
            if (lg.rc) llv_lrc_tmp_begin(n->for_iter->range_lo, 0, &tsv_lo);
            LLVMValueRef lo = llv_emit_expr_llvm(n->for_iter->range_lo);
            if (!lo || LLVMGetTypeKind(LLVMTypeOf(lo)) != LLVMIntegerTypeKind)
                llv_llvm_unsupported("for с не-целочислен диапазон");
            lo = llv_coerce(lo, lg.i64_ty);
            if (lg.rc) llv_lrc_tmp_end(&tsv_lo);

            LLVMValueRef fn = LLVMGetBasicBlockParent(LLVMGetInsertBlock(lg.builder));
            char *m = llv_llvm_mangle(n->for_var);
            LLVMValueRef var = llv_entry_alloca(lg.i64_ty, m);
            free(m);
            LLVMBuildStore(lg.builder, lo, var);

            LLVMBasicBlockRef cond_bb = LLVMAppendBasicBlockInContext(lg.ctx, fn, "for_cond");
            LLVMBasicBlockRef body_bb = LLVMAppendBasicBlockInContext(lg.ctx, fn, "for_body");
            LLVMBasicBlockRef incr_bb = LLVMAppendBasicBlockInContext(lg.ctx, fn, "for_incr");
            LLVMBasicBlockRef end_bb  = LLVMAppendBasicBlockInContext(lg.ctx, fn, "for_end");

            llv_st_push();
            llv_st_define(n->for_var, var);

            LLVMBuildBr(lg.builder, cond_bb);

            LLVMPositionBuilderAtEnd(lg.builder, cond_bb);
            /* RC4 v0.3: hi се преоценява на всяка итерация (като в C) —
             * temp emission + release в cond_bb са per-iteration */
            LLRcTmpSave tsv_hi;
            if (lg.rc) llv_lrc_tmp_begin(n->for_iter->range_hi, 0, &tsv_hi);
            LLVMValueRef hi = llv_emit_expr_llvm(n->for_iter->range_hi);
            if (!hi || LLVMGetTypeKind(LLVMTypeOf(hi)) != LLVMIntegerTypeKind)
                llv_llvm_unsupported("for с не-целочислен диапазон");
            hi = llv_coerce(hi, lg.i64_ty);
            if (lg.rc) llv_lrc_tmp_end(&tsv_hi);
            char *nm = llv_tmp_name();
            LLVMValueRef cur = LLVMBuildLoad2(lg.builder, lg.i64_ty, var, nm);
            free(nm);
            nm = llv_tmp_name();
            LLVMValueRef cond = LLVMBuildICmp(lg.builder, LLVMIntSLT, cur, hi, nm);
            free(nm);
            LLVMBuildCondBr(lg.builder, cond, body_bb, end_bb);

            LLVMPositionBuilderAtEnd(lg.builder, body_bb);
            lrc_loop_next = 1;   /* тялото на for е loop scope (break/continue) */
            lg.lrc_branch_depth++;   /* тялото се изпълнява условно/повторно */
            emit_block_llvm(n->for_body, end_bb, incr_bb);
            lg.lrc_branch_depth--;
            if (!LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(lg.builder)))
                LLVMBuildBr(lg.builder, incr_bb);

            LLVMPositionBuilderAtEnd(lg.builder, incr_bb);
            nm = llv_tmp_name();
            LLVMValueRef iv = LLVMBuildLoad2(lg.builder, lg.i64_ty, var, nm);
            free(nm);
            nm = llv_tmp_name();
            LLVMValueRef next = LLVMBuildAdd(lg.builder, iv,
                                             LLVMConstInt(lg.i64_ty, 1, 0), nm);
            free(nm);
            LLVMBuildStore(lg.builder, next, var);
            LLVMBuildBr(lg.builder, cond_bb);

            LLVMPositionBuilderAtEnd(lg.builder, end_bb);
            llv_st_pop();
            break;
        }

        case NODE_EXPR_STMT: {
            /* RC4: temp-ове в израза (root на bare call с heap резултат е
             * дискарднат — пак е temp; assign дясното е bound) */
            LLRcTmpSave tsv;
            if (lg.rc) llv_lrc_tmp_begin(n->expr, 0, &tsv);
            llv_emit_expr_llvm(n->expr);
            if (lg.rc) llv_lrc_tmp_end(&tsv);
            break;
        }

        case NODE_BLOCK:
            emit_block_llvm(n, break_bb, cont_bb);
            break;

        case NODE_BREAK:
            if (!break_bb) llv_llvm_unsupported("break извън цикъл");
            /* RC: release на напуснатите scope-ове до loop тялото преди скока */
            llv_lrc_release_to_loop();
            LLVMBuildBr(lg.builder, break_bb);
            break;

        case NODE_CONTINUE:
            if (!cont_bb) llv_llvm_unsupported("continue извън цикъл");
            llv_lrc_release_to_loop();
            LLVMBuildBr(lg.builder, cont_bb);
            break;

        case NODE_INVARIANT:
            /* annotation statement — verifier-only, no IR emitted */
            break;

        default:
            llv_llvm_unsupported_node(n);
            break;
    }
}

/* ---- spec ensures/requires (огледало на codegen_c) ---- */

/* Does this extern fn return str? (Effects on the return type are unwrapped.)
 * Mirror на extern_ret_is_str в codegen_c. */
int llv_extern_ret_is_str_llvm(Node *ef) {
    Node *t = ef->ret_type;
    while (t && t->kind == NODE_TYPE_EFFECT) t = t->inner_type;
    return t && t->kind == NODE_TYPE && strcmp(t->type_name, "str") == 0;
}

/* Намира spec с ensures или requires за дадена функция (NULL ако няма). */
static Node *find_ensures_spec_llvm(const char *fn_name) {
    if (!lg.program) return NULL;
    for (int i = 0; i < lg.program->items.len; i++) {
        Node *it = lg.program->items.data[i];
        if (it->kind == NODE_SPEC && strcmp(it->spec_name, fn_name) == 0 &&
            (it->spec_ensures.len > 0 || it->spec_requires.len > 0))
            return it;
    }
    return NULL;
}

/* fprintf(stderr, "spec '%s': ...", spec, idx, expr) + exit(1) + unreachable.
 * Съобщенията са БАЙТОВО същите като baga_spec_fail в codegen_c.
 * (Редът "  вход:" е само за C/--test-specs — тук не се печата.) */
static void emit_spec_fail_llvm(const char *spec_name, int is_requires,
                                int idx, const char *text) {
    const char *fmt_str = is_requires
        ? "spec '%s': requires #%lld нарушено: %s\n"
        : "spec '%s': ensures #%lld нарушена: %s\n";
    LLVMValueRef fmt = LLVMBuildGlobalStringPtr(lg.builder, fmt_str, "fmt");
    LLVMValueRef name_s = LLVMBuildGlobalStringPtr(lg.builder, spec_name, "str");
    LLVMValueRef expr_s = LLVMBuildGlobalStringPtr(lg.builder, text, "str");
    LLVMValueRef err = LLVMBuildLoad2(lg.builder, lg.ptr_ty, lg.stderr_global, "err");
    LLVMValueRef fargs[] = { fmt, name_s,
        LLVMConstInt(lg.i64_ty, (unsigned long long)idx, 0), expr_s };
    /* fprintf(FILE*, fmt, ...) — stderr е първият аргумент */
    LLVMValueRef call_args[5];
    call_args[0] = err;
    for (int i = 0; i < 4; i++) call_args[1 + i] = fargs[i];
    LLVMBuildCall2(lg.builder, LLVMGlobalGetValueType(lg.fprintf_fn),
                   lg.fprintf_fn, call_args, 5, "");
    LLVMValueRef eargs[] = { LLVMConstInt(lg.i32_ty, 1, 0) };
    LLVMBuildCall2(lg.builder, LLVMGlobalGetValueType(lg.exit_fn),
                   lg.exit_fn, eargs, 1, "");
    LLVMBuildUnreachable(lg.builder);
}

/* Една contract проверка: condbr → fail блок с emit_spec_fail_llvm. */
/* v[*] element invariants are verifier-only annotations — no runtime check. */
static int expr_has_elem_ref_llvm(Node *e) {
    if (!e) return 0;
    if (e->kind == NODE_ELEM_REF) return 1;
    if (e->kind == NODE_CALL && e->callee && e->callee->kind == NODE_IDENT &&
        (strcmp(e->callee->name, "sorted") == 0 ||
         strcmp(e->callee->name, "world_frozen") == 0)) return 1;   /* W2.1: ghost */
    if (e->kind == NODE_BINARY) return expr_has_elem_ref_llvm(e->left) || expr_has_elem_ref_llvm(e->right);
    if (e->kind == NODE_UNARY) return expr_has_elem_ref_llvm(e->operand);
    return 0;
}

static void emit_spec_check_llvm(LLVMValueRef fn, Node *spec, Node *ensure,
                                 int is_requires, int idx) {
    if (expr_has_elem_ref_llvm(ensure->ensure_expr)) return;   /* verifier-only */
    LLVMValueRef cond = llv_to_bool(llv_emit_expr_llvm(ensure->ensure_expr));
    LLVMBasicBlockRef fail_bb = LLVMAppendBasicBlockInContext(lg.ctx, fn, "spec_fail");
    LLVMBasicBlockRef ok_bb = LLVMAppendBasicBlockInContext(lg.ctx, fn, "spec_ok");
    LLVMBuildCondBr(lg.builder, cond, ok_bb, fail_bb);
    LLVMPositionBuilderAtEnd(lg.builder, fail_bb);
    emit_spec_fail_llvm(spec->spec_name, is_requires, idx, ensure->ensure_text);
    LLVMPositionBuilderAtEnd(lg.builder, ok_bb);
}

/* ---- Function emission ---- */

static LLVMTypeRef fn_ret_type(Node *fn) {
    return fn->ret_type ? llv_llvm_type(fn->ret_type) : lg.void_ty;
}

static LLVMTypeRef fn_type_of(Node *fn, LLVMTypeRef **out_param_tys, int *out_nparams) {
    LLVMTypeRef ret_ty = fn_ret_type(fn);
    int nparams = fn->params.len;
    LLVMTypeRef *param_tys = malloc(sizeof(LLVMTypeRef) * (size_t)(nparams > 0 ? nparams : 1));
    for (int i = 0; i < nparams; i++)
        param_tys[i] = llv_llvm_type(fn->params.data[i]->param_type);
    if (out_param_tys) *out_param_tys = param_tys;
    if (out_nparams) *out_nparams = nparams;
    LLVMTypeRef fn_ty = LLVMFunctionType(ret_ty, param_tys, (unsigned)nparams, 0);
    if (!out_param_tys) free(param_tys);
    return fn_ty;
}

static char *impl_name_of(const char *fn_name) {
    char buf[512];
    snprintf(buf, sizeof buf, "__impl_%.*s", (int)sizeof buf - 8, fn_name);
    return llv_llvm_mangle(buf);
}

/* M21: име за emit (synthetic при инстанция) */
static const char *emit_name_of(Node *fn) {
    if (lg.gen_fn == fn) {
        static __thread char buf[512];
        snprintf(buf, sizeof buf, "%s__i%d", fn->fn_name, lg.gen_inst);
        return buf;
    }
    return fn->fn_name;
}

/* Първи проход: предекларации на всички функции (като в codegen_c).
 * Функция със spec получава impl (тялото) + wrapper (публичното име). */
static void predeclare_fn_llvm(Node *fn) {
    LLVMTypeRef fn_ty = fn_type_of(fn, NULL, NULL);
    if (fn->is_extern) {
        /* extern fn: declare with the raw C name (no b_ mangling) */
        LLVMAddFunction(lg.mod, fn->fn_name, fn_ty);
        return;
    }
    Node *spec = fn->fn_body ? find_ensures_spec_llvm(fn->fn_name) : NULL;
    if (spec) {
        /* M27: generic инстанция — impl носи synth името */
        char *im = impl_name_of(emit_name_of(fn));
        LLVMAddFunction(lg.mod, im, fn_ty);
        free(im);
    }
    char *m = llv_llvm_mangle(emit_name_of(fn));
    LLVMAddFunction(lg.mod, m, fn_ty);
    free(m);
}

/* Тялото на функцията (за spec функции — под impl името). */
static void emit_fn_llvm(Node *fn, Node *spec) {
    if (!fn->fn_body) return; /* само декларация */

    char *m = spec ? impl_name_of(emit_name_of(fn)) : llv_llvm_mangle(emit_name_of(fn));
    LLVMValueRef fn_val = LLVMGetNamedFunction(lg.mod, m);
    free(m);

    LLVMTypeRef ret_ty = fn_ret_type(fn);
    lg.cur_ret_ty = ret_ty;

    LLVMBasicBlockRef entry = LLVMAppendBasicBlockInContext(lg.ctx, fn_val, "entry");
    LLVMPositionBuilderAtEnd(lg.builder, entry);

    llv_st_reset();
    llv_st_push();
    /* RC: нова функция — чист RC стек; scope 0 е scope-ът на функцията */
    lg.lrc_count = 0;
    lg.lrc_depth = 0;
    lg.lrc_tmp_count = 0;   /* RC4: и чист temp регистър (defensive) */
    lg.lrc_tmps_on = 0;
    lg.lrc_tmp_decl = NULL;
    llv_lrc_push_scope(0);
    lg.lrc_fn_base = 0;

    /* alloca за параметрите (за променливост) */
    for (int i = 0; i < fn->params.len; i++) {
        char *pm = llv_llvm_mangle(fn->params.data[i]->param_name);
        LLVMValueRef param = LLVMGetParam(fn_val, (unsigned)i);
        LLVMSetValueName2(param, pm, strlen(pm));
        LLVMValueRef alloca = LLVMBuildAlloca(lg.builder, LLVMTypeOf(param), pm);
        LLVMBuildStore(lg.builder, param, alloca);
        free(pm);
        llv_st_define(fn->params.data[i]->param_name, alloca);
        /* RC: параметрите са borrowed (is_param=1) — track-ват се (за retain
         * при alias/move проверки), но не се release-ват */
        llv_lrc_track_tag(fn->params.data[i]->param_name,
                      llv_lrc_heap_tag_node(fn->params.data[i]->param_type),
                      fn->params.data[i]->type,
                      fn->params.data[i]->param_type, 1);
    }

    /* тялото; последният EXPR_STMT в не-void функция е неявен return
     * (огледало на codegen_c) */
    int has_ret = fn->ret_type != NULL;
    NodeVec *stmts = &fn->fn_body->stmts;
    for (int i = 0; i < stmts->len; i++) {
        Node *s = stmts->data[i];
        if (LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(lg.builder)))
            break;
        if (has_ret && i == stmts->len - 1 && s->kind == NODE_EXPR_STMT) {
            /* RC4: temp-ове в implicit return израза — същият път като
             * explicit return (release преди return-а) */
            LLRcTmpSave tsv;
            if (lg.rc) llv_lrc_tmp_begin(s->expr, 1, &tsv);
            LLVMValueRef v = llv_emit_expr_llvm(s->expr);
            /* raise вече емитира ret — без втори terminator */
            if (!LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(lg.builder))) {
                if (!v) llv_llvm_unsupported("print като неявен return");
                v = llv_coerce(v, ret_ty);
                if (lg.rc) llv_lrc_tmp_release_all();
                /* RC: същата move семантика като explicit return —
                 * release на локалите ПРЕДИ ret (codegen_c: emit_return_val) */
                llv_lrc_return_move(s->expr, v);
                LLVMBuildRet(lg.builder, v);
            }
            if (lg.rc) llv_lrc_tmp_end(&tsv);
        } else {
            llv_emit_stmt_llvm(s, NULL, NULL);
        }
    }

    /* RC: край на fn scope-а — release на останалите локали при fall-through
     * (при explicit return блокът има terminator и това е само pop) */
    llv_lrc_pop_scope();
    llv_st_pop();

    /* implicit return ако няма terminator */
    if (!LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(lg.builder))) {
        if (ret_ty == lg.void_ty)
            LLVMBuildRetVoid(lg.builder);
        else
            LLVMBuildRet(lg.builder, LLVMConstNull(ret_ty));
    }
}

/* Wrapper с публичното име: requires преди повикването, ensures след него. */
static void emit_wrapper_llvm(Node *fn, Node *spec) {
    char *m = llv_llvm_mangle(emit_name_of(fn));
    LLVMValueRef wrapper = LLVMGetNamedFunction(lg.mod, m);
    free(m);
    char *im = impl_name_of(emit_name_of(fn));
    LLVMValueRef impl = LLVMGetNamedFunction(lg.mod, im);
    free(im);

    LLVMTypeRef ret_ty = fn_ret_type(fn);
    lg.cur_ret_ty = ret_ty;

    LLVMBasicBlockRef entry = LLVMAppendBasicBlockInContext(lg.ctx, wrapper, "entry");
    LLVMPositionBuilderAtEnd(lg.builder, entry);

    llv_st_reset();
    llv_st_push();
    /* RC: wrapper-ът е отделна функция — чист RC стек (тялото му не
     * дефинира heap локали; push/pop само за консистентност) */
    lg.lrc_count = 0;
    lg.lrc_depth = 0;
    llv_lrc_push_scope(0);
    lg.lrc_fn_base = 0;

    /* параметрите на wrapper-а са spec input-ите (като в codegen_c) */
    int np = spec->spec_inputs.len;
    for (int i = 0; i < np; i++) {
        Node *sp = spec->spec_inputs.data[i];
        char *pm = llv_llvm_mangle(sp->param_name);
        LLVMValueRef param = LLVMGetParam(wrapper, (unsigned)i);
        LLVMSetValueName2(param, pm, strlen(pm));
        LLVMValueRef alloca = LLVMBuildAlloca(lg.builder, LLVMTypeOf(param), pm);
        LLVMBuildStore(lg.builder, param, alloca);
        free(pm);
        llv_st_define(sp->param_name, alloca);
    }

    /* предусловия преди повикването */
    for (int j = 0; j < spec->spec_requires.len; j++)
        emit_spec_check_llvm(wrapper, spec, spec->spec_requires.data[j], 1, j + 1);

    /* повикай impl с параметрите */
    LLVMValueRef *args = malloc(sizeof(LLVMValueRef) * (size_t)(np > 0 ? np : 1));
    for (int i = 0; i < np; i++) {
        LLVMValueRef alloca = llv_st_lookup(spec->spec_inputs.data[i]->param_name);
        char *nm = llv_tmp_name();
        args[i] = LLVMBuildLoad2(lg.builder, LLVMGetAllocatedType(alloca), alloca, nm);
        free(nm);
    }
    LLVMTypeRef impl_ty = LLVMGlobalGetValueType(impl);

    if (ret_ty != lg.void_ty) {
        char *nm = llv_tmp_name();
        LLVMValueRef out = LLVMBuildCall2(lg.builder, impl_ty, impl, args, (unsigned)np, nm);
        free(nm);
        LLVMValueRef b_output = LLVMBuildAlloca(lg.builder, ret_ty, "b_output");
        LLVMBuildStore(lg.builder, out, b_output);
        llv_st_define("output", b_output);

        /* гаранции след повикването */
        for (int j = 0; j < spec->spec_ensures.len; j++)
            emit_spec_check_llvm(wrapper, spec, spec->spec_ensures.data[j], 0, j + 1);

        char *nm2 = llv_tmp_name();
        LLVMValueRef r = LLVMBuildLoad2(lg.builder, ret_ty, b_output, nm2);
        free(nm2);
        LLVMBuildRet(lg.builder, r);
    } else {
        /* void функция: само повикай impl след предусловията */
        LLVMBuildCall2(lg.builder, impl_ty, impl, args, (unsigned)np, "");
        LLVMBuildRetVoid(lg.builder);
    }
    free(args);
    llv_lrc_pop_scope();
    llv_st_pop();
}

/* ---- Public API ---- */

void codegen_llvm(Node *program, const char *output_path, Checker *chk, int rc) {
    lg.ctx = LLVMContextCreate();
    lg.mod = LLVMModuleCreateWithNameInContext("baga_module", lg.ctx);
    lg.builder = LLVMCreateBuilderInContext(lg.ctx);
    lg.tmp_counter = 0;
    lg.program = program;
    lg.chk = chk;
    lg.rc = rc;
    lg.gen_fn = NULL; lg.gen_inst = -1;
    lg.gen_struct = NULL; lg.gen_struct_inst = -1;

    lg.i64_ty = LLVMInt64TypeInContext(lg.ctx);
    lg.i32_ty = LLVMInt32TypeInContext(lg.ctx);
    lg.i1_ty = LLVMInt1TypeInContext(lg.ctx);
    lg.double_ty = LLVMDoubleTypeInContext(lg.ctx);
    lg.void_ty = LLVMVoidTypeInContext(lg.ctx);
    lg.i8_ty = LLVMInt8TypeInContext(lg.ctx);
    lg.ptr_ty = LLVMPointerType(lg.i8_ty, 0);
    /* default data layout — за ABI размерите на sum enum payload-ите */
    lg.td = LLVMCreateTargetData("");

    /* declare printf / fprintf / exit / stderr */
    {
        LLVMTypeRef printf_args[] = { lg.ptr_ty };
        LLVMTypeRef printf_ty = LLVMFunctionType(lg.i32_ty, printf_args, 1, 1);
        lg.printf_fn = LLVMAddFunction(lg.mod, "printf", printf_ty);

        LLVMTypeRef fprintf_args[] = { lg.ptr_ty, lg.ptr_ty };
        LLVMTypeRef fprintf_ty = LLVMFunctionType(lg.i32_ty, fprintf_args, 2, 1);
        lg.fprintf_fn = LLVMAddFunction(lg.mod, "fprintf", fprintf_ty);

        /* snprintf(buf, size, fmt, ...) — за baga_f64_to_str (%g) */
        LLVMTypeRef snprintf_args[] = { lg.ptr_ty, lg.i64_ty, lg.ptr_ty };
        LLVMTypeRef snprintf_ty = LLVMFunctionType(lg.i32_ty, snprintf_args, 3, 1);
        lg.snprintf_fn = LLVMAddFunction(lg.mod, "snprintf", snprintf_ty);

        LLVMTypeRef exit_args[] = { lg.i32_ty };
        LLVMTypeRef exit_ty = LLVMFunctionType(lg.void_ty, exit_args, 1, 0);
        lg.exit_fn = LLVMAddFunction(lg.mod, "exit", exit_ty);

        /* glibc: stderr е external global (FILE*) */
        lg.stderr_global = LLVMAddGlobal(lg.mod, lg.ptr_ty, "stderr");
    }

    /* нулев проход: named struct типове + sum enum типове (имена, после тела) */
    for (int i = 0; i < program->items.len; i++) {
        Node *item = program->items.data[i];
        if (item->kind == NODE_STRUCT && item->n_struct_params > 0) {
            /* M24: generic struct — named тип per инстанция */
            for (int k = 0; k < item->struct_inst_count; k++) {
                char *cn = llv_llvm_inst_cname(item, k);
                if (!LLVMGetTypeByName(lg.mod, cn))
                    LLVMStructCreateNamed(lg.ctx, cn);
                free(cn);
            }
            continue;
        }
        if (item->kind != NODE_STRUCT && !llv_is_sum_enum_item(item)) continue;
        const char *tn = item->kind == NODE_STRUCT
            ? item->struct_name : item->enum_name;
        char *m = llv_llvm_mangle(tn);
        if (!LLVMGetTypeByName(lg.mod, m))
            LLVMStructCreateNamed(lg.ctx, m);
        free(m);
    }
    for (int i = 0; i < program->items.len; i++) {
        Node *item = program->items.data[i];
        if (item->kind == NODE_STRUCT && item->n_struct_params > 0) {
            /* M24: тела на инстанциите (полета под substitution) */
            for (int k = 0; k < item->struct_inst_count; k++) {
                lg.gen_struct = item;
                lg.gen_struct_inst = k;
                char *cn = llv_llvm_inst_cname(item, k);
                LLVMTypeRef st = LLVMGetTypeByName(lg.mod, cn);
                free(cn);
                int nf = item->fields.len;
                LLVMTypeRef *elems = malloc(sizeof(LLVMTypeRef) * (size_t)(nf > 0 ? nf : 1));
                for (int j = 0; j < nf; j++)
                    elems[j] = llv_llvm_type(item->fields.data[j]->fld_type);
                LLVMStructSetBody(st, elems, (unsigned)nf, 0);
                free(elems);
                lg.gen_struct = NULL;
                lg.gen_struct_inst = -1;
            }
            continue;
        }
        if (item->kind != NODE_STRUCT) continue;
        char *m = llv_llvm_mangle(item->struct_name);
        LLVMTypeRef st = LLVMGetTypeByName(lg.mod, m);
        free(m);
        int nf = item->fields.len;
        LLVMTypeRef *elems = malloc(sizeof(LLVMTypeRef) * (size_t)(nf > 0 ? nf : 1));
        for (int j = 0; j < nf; j++)
            elems[j] = llv_llvm_type(item->fields.data[j]->fld_type);
        LLVMStructSetBody(st, elems, (unsigned)nf, 0);
        free(elems);
    }
    /* sum enum тела: fixed-point по sized-ness на payload-ите — payload
     * може да е struct или друг sum enum; цикъл по стойност е грешка */
    for (;;) {
        int remaining = 0, progress = 0;
        for (int i = 0; i < program->items.len; i++) {
            Node *item = program->items.data[i];
            if (!llv_is_sum_enum_item(item)) continue;
            char *m = llv_llvm_mangle(item->enum_name);
            LLVMTypeRef st = LLVMGetTypeByName(lg.mod, m);
            free(m);
            if (LLVMTypeIsSized(st)) continue;
            int ready = 1;
            for (int j = 0; j < item->n_variants && ready; j++) {
                if (!item->enum_payloads || !item->enum_payloads[j]) continue;
                if (!LLVMTypeIsSized(llv_llvm_type(item->enum_payloads[j])))
                    ready = 0;
            }
            if (!ready) { remaining++; continue; }
            llv_sum_enum_set_body(item, st);
            progress = 1;
        }
        if (remaining == 0) break;
        if (!progress)
            llv_llvm_unsupported("циклични sum enum типове (по стойност)");
    }

    /* M24-RC: per-instance RC helper-и за generic struct инстанции с heap
     * полета — eager, като C бекенда (emit_rc_struct_helpers след
     * typedef-овете). Lazy пътят (llv_lrc_rc_fn_ty) остава за вложени случаи. */
    if (lg.rc) {
        for (int i = 0; i < program->items.len; i++) {
            Node *item = program->items.data[i];
            if (item->kind != NODE_STRUCT || item->n_struct_params == 0)
                continue;
            for (int k = 0; k < item->struct_inst_count; k++) {
                Type it = {0};
                it.kind = TYPE_STRUCT;
                it.name = item->struct_name;
                it.n_targs = item->n_struct_params;
                it.targs = &item->struct_inst_targs[k * item->n_struct_params];
                if (llv_lrc_heap_tag(&it) != 5) continue;
                llv_lrc_struct_inst_rc_fn(&it, 0);
                llv_lrc_struct_inst_rc_fn(&it, 1);
            }
        }
    }

    /* първи проход: предекларации */
    for (int i = 0; i < program->items.len; i++) {
        Node *item = program->items.data[i];
        if (item->kind == NODE_FN) {
            if (item->n_type_params > 0) {
                for (int k = 0; k < item->inst_count; k++) {
                    if (chk) checker_recheck_inst(chk, item, k);
                    lg.gen_fn = item; lg.gen_inst = k;
                    predeclare_fn_llvm(item);
                }
                lg.gen_fn = NULL; lg.gen_inst = -1;
            } else {
                predeclare_fn_llvm(item);
            }
        }
        if (item->kind == NODE_IMPL)
            for (int m = 0; m < item->impl_methods.len; m++)
                predeclare_fn_llvm(item->impl_methods.data[m]);
    }

    /* втори проход: тела + wrapper-и */
    for (int i = 0; i < program->items.len; i++) {
        Node *item = program->items.data[i];
        if (item->kind == NODE_FN && item->fn_body) {
            if (item->n_type_params > 0) {
                for (int k = 0; k < item->inst_count; k++) {
                    if (chk) checker_recheck_inst(chk, item, k);
                    lg.gen_fn = item; lg.gen_inst = k;
                    Node *spec = find_ensures_spec_llvm(item->fn_name);
                    if (spec && chk) checker_recheck_spec(chk, spec, item, k);
                    emit_fn_llvm(item, spec);
                    if (spec) emit_wrapper_llvm(item, spec);
                }
                lg.gen_fn = NULL; lg.gen_inst = -1;
            } else {
                Node *spec = find_ensures_spec_llvm(item->fn_name);
                emit_fn_llvm(item, spec);
                if (spec) emit_wrapper_llvm(item, spec);
            }
        }
        if (item->kind == NODE_IMPL) {
            for (int m = 0; m < item->impl_methods.len; m++) {
                Node *mf = item->impl_methods.data[m];
                if (!mf->fn_body) continue;
                emit_fn_llvm(mf, NULL);
            }
        }
    }

    /* emit C main wrapper */
    {
        char *main_m = llv_llvm_mangle("main");
        LLVMValueRef baga_main = LLVMGetNamedFunction(lg.mod, main_m);
        free(main_m);

        if (baga_main) {
            LLVMTypeRef mp[] = { lg.i32_ty, LLVMPointerType(lg.ptr_ty, 0) };
            LLVMTypeRef c_main_ty = LLVMFunctionType(lg.i32_ty, mp, 2, 0);
            LLVMValueRef c_main = LLVMAddFunction(lg.mod, "main", c_main_ty);
            LLVMBasicBlockRef entry = LLVMAppendBasicBlockInContext(lg.ctx, c_main, "entry");
            LLVMPositionBuilderAtEnd(lg.builder, entry);
            LLVMBuildStore(lg.builder, LLVMGetParam(c_main, 0), llv_argv_argc_global());
            LLVMBuildStore(lg.builder, LLVMGetParam(c_main, 1), llv_argv_argv_global());
            LLVMBuildCall2(lg.builder,
                LLVMGlobalGetValueType(baga_main),
                baga_main, NULL, 0, "");
            LLVMBuildRet(lg.builder, LLVMConstInt(lg.i32_ty, 0, 0));
        }
    }

    /* verify module */
    char *error = NULL;
    LLVMBool broken = LLVMVerifyModule(lg.mod, LLVMPrintMessageAction, &error);
    if (broken) {
        fprintf(stderr, "baga: LLVM verification failed: %s\n", error ? error : "unknown");
        LLVMDisposeMessage(error);
        exit(1);
    }

    /* output */
    if (output_path) {
        LLVMPrintModuleToFile(lg.mod, output_path, &error);
        if (error) {
            fprintf(stderr, "baga: LLVM output error: %s\n", error);
            LLVMDisposeMessage(error);
        }
    } else {
        char *ir = LLVMPrintModuleToString(lg.mod);
        printf("%s", ir);
        LLVMDisposeMessage(ir);
    }

    /* cleanup */
    LLVMDisposeBuilder(lg.builder);
    LLVMDisposeModule(lg.mod);
    LLVMDisposeTargetData(lg.td);
    LLVMContextDispose(lg.ctx);
}

#endif /* BAGA_LLVM */
