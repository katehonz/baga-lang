/* codegen_llvm_expr.c — част от codegen_llvm.c (виж codegen_llvm_impl.h). */
#include "baga.h"
#include "codegen_llvm_impl.h"

/* ---- Expression emission ---- */


#ifdef BAGA_LLVM

LLVMValueRef llv_emit_expr_llvm(Node *n);
void llv_emit_stmt_llvm(Node *n, LLVMBasicBlockRef break_bb, LLVMBasicBlockRef cont_bb);
LLVMValueRef llv_emit_block_value_llvm(Node *block);

/* ---- L5: fn стойности (closures) ----
 * Огледало на C бекенда: handle = i64 cell2(code, env); cell2 живее в par
 * runtime-а (libbaga_par.so). Именувана функция като стойност взима адреса
 * на лениво генериран wrapper `<mangled>__clo(env, params...)`; ламбдата
 * получава собствен wrapper + env struct с копия на captures. */

static LLVMValueRef rt_cell2(void) {
    LLVMTypeRef p[] = { lg.i64_ty, lg.i64_ty };
    return llv_rt_libc("baga_cell2", lg.i64_ty, p, 2);
}
static LLVMValueRef rt_cell2_0(void) {
    LLVMTypeRef p[] = { lg.i64_ty };
    return llv_rt_libc("baga_cell2_0", lg.i64_ty, p, 1);
}
static LLVMValueRef rt_cell2_1(void) {
    LLVMTypeRef p[] = { lg.i64_ty };
    return llv_rt_libc("baga_cell2_1", lg.i64_ty, p, 1);
}

/* handle = cell2(ptrtoint(code), ptrtoint(env)) */
static LLVMValueRef closure_handle(LLVMValueRef code, LLVMValueRef env) {
    LLVMValueRef ci = LLVMBuildPtrToInt(lg.builder, code, lg.i64_ty, "code");
    LLVMValueRef ei = LLVMBuildPtrToInt(lg.builder, env, lg.i64_ty, "env");
    LLVMValueRef pa[] = { ci, ei };
    return llv_h_call(rt_cell2(), pa, 2, "clo");
}

/* NODE_FN по име (NULL ако липсва) */
static Node *find_user_fn(const char *name) {
    if (!lg.program) return NULL;
    for (int i = 0; i < lg.program->items.len; i++) {
        Node *it = lg.program->items.data[i];
        if (it->kind == NODE_FN && !it->is_extern && it->fn_name &&
            strcmp(it->fn_name, name) == 0)
            return it;
    }
    return NULL;
}

/* fn pointer тип за closure вик: ret(i8* env, params...) върху Type (TYPE_FN) */
static LLVMTypeRef closure_fn_ty(Type *ft, LLVMTypeRef **params_out) {
    int np = ft && ft->kind == TYPE_FN ? ft->nparams : 0;
    LLVMTypeRef *pt = malloc(sizeof(LLVMTypeRef) * (size_t)(np + 1));
    pt[0] = lg.ptr_ty;   /* env */
    for (int i = 0; i < np; i++)
        pt[i + 1] = ft->params[i] ? llv_llvm_type_resolved(ft->params[i]) : lg.i64_ty;
    LLVMTypeRef ret = ft && ft->ret ? llv_llvm_type_resolved(ft->ret) : lg.void_ty;
    if (params_out) *params_out = pt;
    LLVMTypeRef fty = LLVMFunctionType(ret, pt, (unsigned)(np + 1), 0);
    if (!params_out) free(pt);
    return fty;
}

/* wrapper за именувана fn като стойност: вика публичното име (spec-обвивката,
 * ако има такава) — като __clo в codegen_c. Ленив, по веднъж на функция.
 * M25: и инстанция на generic fn (`id__i0`) — базовият decl + gen контекст
 * (llv_llvm_type resolve-ва типовите променливи към targs на инстанцията). */
static LLVMValueRef closure_wrapper_named(const char *fn_name) {
    char *m = llv_llvm_mangle(fn_name);
    char wn[600];
    snprintf(wn, sizeof wn, "%s__clo", m);
    LLVMValueRef ex = LLVMGetNamedFunction(lg.mod, wn);
    if (ex) { free(m); return ex; }
    Node *fn = find_user_fn(fn_name);
    Node *saved_gf = lg.gen_fn;
    int saved_gi = lg.gen_inst;
    if (!fn) {
        /* M25: synth име на инстанция — `база__iN` */
        const char *sep = strstr(fn_name, "__i");
        int idx = -1;
        char base[256];
        if (sep && sscanf(sep + 3, "%d", &idx) == 1 && idx >= 0 &&
            (size_t)(sep - fn_name) < sizeof base) {
            memcpy(base, fn_name, (size_t)(sep - fn_name));
            base[sep - fn_name] = '\0';
            fn = find_user_fn(base);
        }
        if (!fn || idx < 0 || fn->n_type_params == 0 || idx >= fn->inst_count) {
            char buf[256];
            snprintf(buf, sizeof buf, "fn стойност на непозната функция '%s'", fn_name);
            free(m);
            llv_llvm_unsupported(buf);
        }
        /* gen контекст: llv_llvm_type resolve-ва типовите променливи */
        lg.gen_fn = fn;
        lg.gen_inst = idx;
    }
    LLVMValueRef target = LLVMGetNamedFunction(lg.mod, m);
    if (!target) {
        lg.gen_fn = saved_gf; lg.gen_inst = saved_gi;
        free(m);
        llv_llvm_unsupported("fn стойност преди декларация на функцията");
    }
    int np = fn->params.len;
    LLVMTypeRef *pt = malloc(sizeof(LLVMTypeRef) * (size_t)(np + 1));
    pt[0] = lg.ptr_ty;
    for (int i = 0; i < np; i++)
        pt[i + 1] = llv_llvm_type(fn->params.data[i]->param_type);
    LLVMTypeRef ret = fn->ret_type ? llv_llvm_type(fn->ret_type) : lg.void_ty;
    LLVMTypeRef fty = LLVMFunctionType(ret, pt, (unsigned)(np + 1), 0);
    free(pt);
    LLVMValueRef wrap = LLVMAddFunction(lg.mod, wn, fty);
    LLVMBasicBlockRef saved = LLVMGetInsertBlock(lg.builder);
    LLVMBasicBlockRef entry = LLVMAppendBasicBlockInContext(lg.ctx, wrap, "entry");
    LLVMPositionBuilderAtEnd(lg.builder, entry);
    LLVMValueRef *args = malloc(sizeof(LLVMValueRef) * (size_t)(np > 0 ? np : 1));
    for (int i = 0; i < np; i++)
        args[i] = LLVMGetParam(wrap, (unsigned)(i + 1));   /* env се пропуска */
    LLVMTypeRef target_ty = LLVMGlobalGetValueType(target);
    LLVMValueRef r = LLVMBuildCall2(lg.builder, target_ty, target, args,
                                    (unsigned)np, ret == lg.void_ty ? "" : "r");
    free(args);
    if (ret == lg.void_ty) LLVMBuildRetVoid(lg.builder);
    else                   LLVMBuildRet(lg.builder, r);
    free(m);
    if (saved) LLVMPositionBuilderAtEnd(lg.builder, saved);
    lg.gen_fn = saved_gf; lg.gen_inst = saved_gi;
    return wrap;
}

/* M27: ламбда в generic fn — уникално име per инстанция (споделен AST). */
static const char *lambda_emit_name_llvm(Node *n, char *buf, size_t nbuf) {
    const char *base = n->fn_name ? n->fn_name : "__lam_x";
    if (lg.gen_fn && lg.gen_inst >= 0) {
        snprintf(buf, nbuf, "%s__i%d", base, lg.gen_inst);
        return buf;
    }
    return base;
}

/* wrapper за ламбда: <mangled>__clo(i8* env, params...); captures се
 * разопаковат като локални копия от env struct-а. Ленив, по веднъж на възел. */
static LLVMValueRef emit_lambda_wrapper(Node *n, LLVMTypeRef env_ty) {
    char lnm[256];
    char *m = llv_llvm_mangle(lambda_emit_name_llvm(n, lnm, sizeof lnm));
    char wn[600];
    snprintf(wn, sizeof wn, "%s__clo", m);
    free(m);
    LLVMValueRef ex = LLVMGetNamedFunction(lg.mod, wn);
    if (ex) return ex;
    int np = n->params.len;
    LLVMTypeRef *pt = malloc(sizeof(LLVMTypeRef) * (size_t)(np + 1));
    pt[0] = lg.ptr_ty;
    for (int i = 0; i < np; i++)
        pt[i + 1] = llv_llvm_type(n->params.data[i]->param_type);
    LLVMTypeRef ret = n->ret_type ? llv_llvm_type(n->ret_type) : lg.void_ty;
    LLVMTypeRef fty = LLVMFunctionType(ret, pt, (unsigned)(np + 1), 0);
    free(pt);
    LLVMValueRef fn = LLVMAddFunction(lg.mod, wn, fty);

    LLVMBasicBlockRef saved = LLVMGetInsertBlock(lg.builder);
    LLVMTypeRef saved_ret = lg.cur_ret_ty;
    lg.cur_ret_ty = ret;
    LLVMBasicBlockRef entry = LLVMAppendBasicBlockInContext(lg.ctx, fn, "entry");
    LLVMPositionBuilderAtEnd(lg.builder, entry);
    llv_st_push();
    /* RC: wrapper-ът на ламбдата е отделна функция — собствен fn scope
     * (като codegen_c); външният RC стек се пази и възстановява след тялото */
    int saved_lrc_count = lg.lrc_count;
    int saved_lrc_depth = lg.lrc_depth;
    int saved_lrc_fn_base = lg.lrc_fn_base;
    llv_lrc_push_scope(0);
    lg.lrc_fn_base = lg.lrc_depth - 1;

    /* captures → локални копия от env */
    if (n->captures.len > 0) {
        LLVMValueRef envp = LLVMBuildBitCast(lg.builder, LLVMGetParam(fn, 0),
            LLVMPointerType(env_ty, 0), "envp");
        for (int i = 0; i < n->captures.len; i++) {
            Node *cap = n->captures.data[i];
            LLVMTypeRef ct = llv_llvm_type_resolved(cap->type);
            LLVMValueRef gep = LLVMBuildStructGEP2(lg.builder, env_ty, envp,
                (unsigned)i, "cp");
            LLVMValueRef v = LLVMBuildLoad2(lg.builder, ct, gep, "cv");
            LLVMValueRef alloca = LLVMBuildAlloca(lg.builder, ct, cap->param_name);
            LLVMBuildStore(lg.builder, v, alloca);
            llv_st_define(cap->param_name, alloca);
            /* RC: capture-ите са borrowed (като в codegen_c) */
            llv_lrc_track(cap->param_name, cap->type, 1);
        }
    }
    /* параметри → alloca (като emit_fn_llvm) */
    for (int i = 0; i < np; i++) {
        Node *p = n->params.data[i];
        LLVMValueRef param = LLVMGetParam(fn, (unsigned)(i + 1));
        LLVMValueRef alloca = LLVMBuildAlloca(lg.builder, LLVMTypeOf(param),
            p->param_name);
        LLVMBuildStore(lg.builder, param, alloca);
        llv_st_define(p->param_name, alloca);
        llv_lrc_track_tag(p->param_name, llv_lrc_heap_tag_node(p->param_type), p->type,
                      p->param_type, 1);
    }
    /* тяло — огледало на emit_fn_llvm опашката. Изпълнява се при извикване
     * на затварянето — условен поток спрямо външната функция. */
    int has_ret = n->ret_type != NULL;
    NodeVec *stmts = n->fn_body ? &n->fn_body->stmts : NULL;
    lg.lrc_branch_depth++;
    for (int i = 0; stmts && i < stmts->len; i++) {
        Node *s = stmts->data[i];
        if (LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(lg.builder)))
            break;
        if (has_ret && i == stmts->len - 1 && s->kind == NODE_EXPR_STMT) {
            /* RC4: temp-ове в implicit return (като при explicit return) */
            LLRcTmpSave tsv;
            if (lg.rc) llv_lrc_tmp_begin(s->expr, 1, &tsv);
            LLVMValueRef v = llv_emit_expr_llvm(s->expr);
            if (!LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(lg.builder))) {
                if (!v) llv_llvm_unsupported("лямбда: неявен return");
                v = llv_coerce(v, ret);
                if (lg.rc) llv_lrc_tmp_release_all();
                /* RC: move семантика като explicit return (като emit_fn_llvm) */
                llv_lrc_return_move(s->expr, v);
                LLVMBuildRet(lg.builder, v);
            }
            if (lg.rc) llv_lrc_tmp_end(&tsv);
        } else {
            llv_emit_stmt_llvm(s, NULL, NULL);
        }
    }
    lg.lrc_branch_depth--;
    /* RC: край на lambda scope-а + възстановяване на външния RC стек */
    llv_lrc_pop_scope();
    lg.lrc_count = saved_lrc_count;
    lg.lrc_depth = saved_lrc_depth;
    lg.lrc_fn_base = saved_lrc_fn_base;
    llv_st_pop();
    if (!LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(lg.builder))) {
        if (ret == lg.void_ty) LLVMBuildRetVoid(lg.builder);
        else                   LLVMBuildRet(lg.builder, LLVMConstNull(ret));
    }
    lg.cur_ret_ty = saved_ret;
    if (saved) LLVMPositionBuilderAtEnd(lg.builder, saved);
    return fn;
}

static LLVMValueRef emit_binop_llvm(BinOp op, LLVMValueRef left, LLVMValueRef right) {
    char *name = llv_tmp_name();
    LLVMValueRef result = NULL;
    int isdbl = LLVMGetTypeKind(LLVMTypeOf(left)) == LLVMDoubleTypeKind;
    switch (op) {
        case OP_ADD: result = isdbl ? LLVMBuildFAdd(lg.builder, left, right, name)
                                    : LLVMBuildAdd(lg.builder, left, right, name); break;
        case OP_SUB: result = isdbl ? LLVMBuildFSub(lg.builder, left, right, name)
                                    : LLVMBuildSub(lg.builder, left, right, name); break;
        case OP_MUL: result = isdbl ? LLVMBuildFMul(lg.builder, left, right, name)
                                    : LLVMBuildMul(lg.builder, left, right, name); break;
        case OP_DIV: result = isdbl ? LLVMBuildFDiv(lg.builder, left, right, name)
                                    : LLVMBuildSDiv(lg.builder, left, right, name); break;
        case OP_MOD: result = isdbl ? LLVMBuildFRem(lg.builder, left, right, name)
                                    : LLVMBuildSRem(lg.builder, left, right, name); break;
        case OP_EQ:  result = isdbl ? LLVMBuildFCmp(lg.builder, LLVMRealOEQ, left, right, name)
                                    : LLVMBuildICmp(lg.builder, LLVMIntEQ, left, right, name); break;
        case OP_NEQ: result = isdbl ? LLVMBuildFCmp(lg.builder, LLVMRealONE, left, right, name)
                                    : LLVMBuildICmp(lg.builder, LLVMIntNE, left, right, name); break;
        case OP_LT:  result = isdbl ? LLVMBuildFCmp(lg.builder, LLVMRealOLT, left, right, name)
                                    : LLVMBuildICmp(lg.builder, LLVMIntSLT, left, right, name); break;
        case OP_GT:  result = isdbl ? LLVMBuildFCmp(lg.builder, LLVMRealOGT, left, right, name)
                                    : LLVMBuildICmp(lg.builder, LLVMIntSGT, left, right, name); break;
        case OP_LE:  result = isdbl ? LLVMBuildFCmp(lg.builder, LLVMRealOLE, left, right, name)
                                    : LLVMBuildICmp(lg.builder, LLVMIntSLE, left, right, name); break;
        case OP_GE:  result = isdbl ? LLVMBuildFCmp(lg.builder, LLVMRealOGE, left, right, name)
                                    : LLVMBuildICmp(lg.builder, LLVMIntSGE, left, right, name); break;
        case OP_BIT_AND:
            if (isdbl) llv_llvm_unsupported("побитово & върху f64");
            result = LLVMBuildAnd(lg.builder, left, right, name); break;
        case OP_BIT_OR:
            if (isdbl) llv_llvm_unsupported("побитово | върху f64");
            result = LLVMBuildOr(lg.builder, left, right, name); break;
        case OP_BIT_XOR:
            if (isdbl) llv_llvm_unsupported("побитово ^ върху f64");
            result = LLVMBuildXor(lg.builder, left, right, name); break;
        case OP_LSHIFT:
            if (isdbl) llv_llvm_unsupported("<< върху f64");
            /* M22: маскиран брояч (b & 63) — детерминирано; паритет с
               baga_shl_i64 в C бекенда. */
            {
                LLVMValueRef m = LLVMBuildAnd(lg.builder, right,
                    LLVMConstInt(LLVMTypeOf(right), 63, 0), llv_tmp_name());
                result = LLVMBuildShl(lg.builder, left, m, name);
            }
            break;
        case OP_RSHIFT:
            if (isdbl) llv_llvm_unsupported(">> върху f64");
            /* M22: аритметично отместване (floor) с маскиран брояч — паритет
                с baga_ashr_i64 в C бекенда. */
            {
                LLVMValueRef m = LLVMBuildAnd(lg.builder, right,
                    LLVMConstInt(LLVMTypeOf(right), 63, 0), llv_tmp_name());
                result = LLVMBuildAShr(lg.builder, left, m, name);
            }
            break;
        case OP_URSHIFT:
            if (isdbl) llv_llvm_unsupported(">>> върху f64");
            /* M23: логическо отместване (zero-fill) с маскиран брояч —
                паритет с baga_lshr_i64 в C бекенда. */
            {
                LLVMValueRef m = LLVMBuildAnd(lg.builder, right,
                    LLVMConstInt(LLVMTypeOf(right), 63, 0), llv_tmp_name());
                result = LLVMBuildLShr(lg.builder, left, m, name);
            }
            break;
        default:     llv_llvm_unsupported("непознат бинарен оператор"); break;
    }
    free(name);
    return result;
}

/* print/println/write — изходът е байт за байт като codegen_c:
 * i64 → "%lld\n", f64 → "%g\n", bool → "true"/"false", str → "%s\n".
 * write върху str е без нов ред. */
static void emit_print_llvm(Node *n) {
    int is_write = strcmp(n->callee->name, "write") == 0;

    if (n->args.len == 0) {
        LLVMValueRef fmt = LLVMBuildGlobalStringPtr(lg.builder, "\n", "fmt");
        LLVMValueRef args[] = { fmt };
        LLVMBuildCall2(lg.builder, LLVMGlobalGetValueType(lg.printf_fn),
                       lg.printf_fn, args, 1, "");
        return;
    }

    for (int i = 0; i < n->args.len; i++) {
        Node *arg = n->args.data[i];
        LLVMValueRef val;
        const char *fmt_str;
        LLVMValueRef extra[2];
        int nextra = 0;

        if (arg->kind == NODE_STR_LIT) {
            val = LLVMBuildGlobalStringPtr(lg.builder, arg->str_val, "str");
            fmt_str = is_write ? "%s" : "%s\n";
            extra[nextra++] = val;
        } else {
            val = llv_emit_expr_llvm(arg);
            if (!val) llv_llvm_unsupported("print върху void стойност");
            LLVMTypeRef t = LLVMTypeOf(val);
            if (LLVMGetTypeKind(t) == LLVMPointerTypeKind) {
                fmt_str = is_write ? "%s" : "%s\n";
                extra[nextra++] = val;
            } else if (LLVMGetTypeKind(t) == LLVMDoubleTypeKind) {
                fmt_str = "%g\n";
                extra[nextra++] = val;
            } else if (t == lg.i1_ty) {
                LLVMValueRef ts = LLVMBuildGlobalStringPtr(lg.builder, "true", "str");
                LLVMValueRef fs = LLVMBuildGlobalStringPtr(lg.builder, "false", "str");
                char *name = llv_tmp_name();
                LLVMValueRef sel = LLVMBuildSelect(lg.builder, val, ts, fs, name);
                free(name);
                fmt_str = "%s\n";
                extra[nextra++] = sel;
            } else if (LLVMGetTypeKind(t) == LLVMIntegerTypeKind) {
                fmt_str = "%lld\n";
                extra[nextra++] = llv_coerce(val, lg.i64_ty);
            } else {
                llv_llvm_unsupported("print върху този тип");
                return; /* unreachable */
            }
        }

        LLVMValueRef fmt = LLVMBuildGlobalStringPtr(lg.builder, fmt_str, "fmt");
        LLVMValueRef args[3];
        args[0] = fmt;
        for (int j = 0; j < nextra; j++) args[1 + j] = extra[j];
        LLVMBuildCall2(lg.builder, LLVMGlobalGetValueType(lg.printf_fn),
                       lg.printf_fn, args, (unsigned)(1 + nextra), "");
    }
}

static int is_print_call_llvm(Node *n) {
    if (n->kind != NODE_CALL) return 0;
    if (n->callee->kind != NODE_IDENT) return 0;
    return strcmp(n->callee->name, "print") == 0 ||
           strcmp(n->callee->name, "println") == 0 ||
           strcmp(n->callee->name, "write") == 0;
}

/* enum вариант → i64 константа (индексът в декларацията); -1 ако не е вариант */
static int find_enum_variant(const char *name) {
    if (!lg.program) return -1;
    for (int i = 0; i < lg.program->items.len; i++) {
        Node *item = lg.program->items.data[i];
        if (item->kind != NODE_ENUM) continue;
        for (int j = 0; j < item->n_variants; j++)
            if (strcmp(item->enum_variants[j], name) == 0)
                return j;
    }
    return -1;
}

/* Тяло на match клон — огледало на codegen_c: голият израз е опакован
 * от parser-а като BLOCK с RETURN; RETURN със стойност записва резултата,
 * EXPR_STMT се изпълнява за страничен ефект. */
static void emit_match_arm_llvm(Node *arm, LLVMValueRef res_alloca,
                                LLVMBasicBlockRef merge_bb) {
    Node *body = arm->arm_body;
    if (!body || body->kind != NODE_BLOCK)
        llv_llvm_unsupported("match клон, който не е блок");
    lg.lrc_branch_depth++;   /* телата на рамената са условен поток */
    for (int j = 0; j < body->stmts.len; j++) {
        if (LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(lg.builder))) {
            lg.lrc_branch_depth--;
            return;
        }
        Node *s = body->stmts.data[j];
        if (s->kind == NODE_RETURN) {
            if (s->ret_val) {
                /* RC4 v1.1: вложени temp-ове в рамото (root bound — стойността
                 * отива в match резултата). Порт на emit_rc_stmt_expr_root. */
                LLRcTmpSave tsv;
                if (lg.rc) llv_lrc_tmp_begin(s->ret_val, 1, &tsv);
                /* void match: стойността се оценява за страничния ефект
                 * (codegen_c emit-ва израза и без _mr присвояване) */
                LLVMValueRef v = llv_emit_expr_llvm(s->ret_val);
                if (res_alloca) {
                    if (!v) llv_llvm_unsupported("print в match клон");
                    v = llv_coerce(v, LLVMGetAllocatedType(res_alloca));
                    /* RC: рамото произвежда owned резултат (порт на
                     * rc_emit_match_arm_val) — borrowed/ident стойност се
                     * retain-ва, иначе release на източника обесва
                     * консуматора на match резултата. */
                    if (llv_lrc_need_owned_retain(s->ret_val))
                        llv_lrc_emit_retain_val(llv_lrc_heap_tag(s->ret_val->type),
                                            s->ret_val->type, NULL, v);
                    LLVMBuildStore(lg.builder, v, res_alloca);
                }
                if (lg.rc) llv_lrc_tmp_end(&tsv);
            }
            LLVMBuildBr(lg.builder, merge_bb);
            lg.lrc_branch_depth--;
            return;
        }
        if (s->kind == NODE_EXPR_STMT) {
            LLRcTmpSave tsv;
            if (lg.rc) llv_lrc_tmp_begin(s->expr, 0, &tsv);
            llv_emit_expr_llvm(s->expr);
            if (lg.rc) llv_lrc_tmp_end(&tsv);
            continue;
        }
        llv_llvm_unsupported("оператор в match клон (само изрази)");
    }
    lg.lrc_branch_depth--;
}

/* L3: match върху sum enum — верига от tag сравнения (като codegen_c);
 * payload binding-ът е alloca в scope-а на клона (llv_st_push/llv_st_pop). */
static LLVMValueRef emit_match_sum_llvm(Node *n, Node *ed) {
    LLVMTypeRef ety = llv_user_struct_ty(ed->enum_name);
    LLVMValueRef mv = llv_emit_expr_llvm(n->match_expr);
    if (!mv) llv_llvm_unsupported("print като match subject");
    /* subject-ът се оценява веднъж в temp — тагът и payload-овете се
     * четат от него (като _mv в codegen_c) */
    LLVMValueRef mva = llv_entry_alloca(ety, "mv");
    LLVMBuildStore(lg.builder, mv, mva);
    LLVMValueRef tagp = LLVMBuildStructGEP2(lg.builder, ety, mva, 0, "tagp");
    LLVMValueRef tag = LLVMBuildLoad2(lg.builder, lg.i64_ty, tagp, "tag");

    LLVMTypeRef res_ty = n->type ? llv_llvm_type_resolved(n->type) : lg.i64_ty;
    LLVMValueRef res_alloca = NULL;
    if (res_ty != lg.void_ty) {
        res_alloca = llv_entry_alloca(res_ty, "match_res");
        LLVMBuildStore(lg.builder, LLVMConstNull(res_ty), res_alloca);
    }

    LLVMValueRef fn = LLVMGetBasicBlockParent(LLVMGetInsertBlock(lg.builder));
    LLVMBasicBlockRef merge_bb = LLVMAppendBasicBlockInContext(lg.ctx, fn, "match_end");

    for (int i = 0; i < n->match_arms.len; i++) {
        Node *arm = n->match_arms.data[i];
        if (arm->arm_pattern) {
            const char *pvn = arm->arm_pattern->kind == NODE_PATH
                ? arm->arm_pattern->path_variant
                : arm->arm_pattern->kind == NODE_IDENT
                ? arm->arm_pattern->name : NULL;
            int vidx = llv_sum_variant_index(ed, pvn);
            if (vidx < 0)
                llv_llvm_unsupported("match клон, който не е вариант на sum enum");
            LLVMValueRef cond = LLVMBuildICmp(lg.builder, LLVMIntEQ, tag,
                LLVMConstInt(lg.i64_ty, (unsigned long long)vidx, 0), "mcond");
            LLVMBasicBlockRef arm_bb = LLVMAppendBasicBlockInContext(lg.ctx, fn, "match_arm");
            LLVMBasicBlockRef next_bb = LLVMAppendBasicBlockInContext(lg.ctx, fn, "match_next");
            LLVMBuildCondBr(lg.builder, cond, arm_bb, next_bb);
            LLVMPositionBuilderAtEnd(lg.builder, arm_bb);
            llv_st_push();
            if (arm->arm_binding && ed->enum_payloads && ed->enum_payloads[vidx]) {
                LLVMTypeRef pty = llv_llvm_type(ed->enum_payloads[vidx]);
                LLVMValueRef pv = LLVMBuildLoad2(lg.builder, pty,
                    llv_sum_payload_ptr(ety, mva, pty), "pv");
                char *bm = llv_llvm_mangle(arm->arm_binding);
                LLVMValueRef ba = llv_entry_alloca(pty, bm);
                free(bm);
                LLVMBuildStore(lg.builder, pv, ba);
                llv_st_define(arm->arm_binding, ba);
            }
            emit_match_arm_llvm(arm, res_alloca, merge_bb);
            llv_st_pop();
            if (!LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(lg.builder)))
                LLVMBuildBr(lg.builder, merge_bb);
            LLVMPositionBuilderAtEnd(lg.builder, next_bb);
        } else {
            /* wildcard `_` → else клон */
            emit_match_arm_llvm(arm, res_alloca, merge_bb);
            if (!LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(lg.builder)))
                LLVMBuildBr(lg.builder, merge_bb);
        }
    }

    /* без wildcard: последният match_next пада в merge (както codegen_c) */
    if (!LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(lg.builder)))
        LLVMBuildBr(lg.builder, merge_bb);
    LLVMPositionBuilderAtEnd(lg.builder, merge_bb);
    if (!res_alloca) return NULL;
    char *name = llv_tmp_name();
    LLVMValueRef r = LLVMBuildLoad2(lg.builder, res_ty, res_alloca, name);
    free(name);
    return r;
}

static LLVMValueRef emit_match_llvm(Node *n) {
    /* L3: match върху sum enum → tag верига с payload bindings */
    if (n->match_expr && n->match_expr->type &&
        n->match_expr->type->kind == TYPE_ENUM) {
        Node *ed = llv_find_sum_enum(n->match_expr->type->name);
        if (ed) return emit_match_sum_llvm(n, ed);
    }
    Node *sc = n->match_expr;
    int str_scrut = sc && sc->type && sc->type->kind == TYPE_STR;
    /* Подсилване: низов литерал като патерн ⇒ низов scrutinee (типът може
     * да липсва). Огледало на codegen_c. */
    if (!str_scrut) {
        for (int i = 0; i < n->match_arms.len; i++) {
            Node *a = n->match_arms.data[i];
            if (a->arm_pattern && a->arm_pattern->kind == NODE_STR_LIT) {
                str_scrut = 1;
                break;
            }
        }
    }
    LLVMValueRef mv = llv_emit_expr_llvm(n->match_expr);
    if (!mv) llv_llvm_unsupported("match върху не-целочислена стойност");
    /* str scrutinee: сравнявай СЪДЪРЖАНИЕ (baga_str_eq → strcmp), не
     * указатели. Огледало на codegen_c. Преди тук имаше ptrtoint от двете
     * страни (указателно равенство) — с изчислен низ `match` тихо падаше в
     * `_`. */
    if (!str_scrut) {
        if (LLVMGetTypeKind(LLVMTypeOf(mv)) == LLVMPointerTypeKind)
            mv = LLVMBuildPtrToInt(lg.builder, mv, lg.i64_ty, "mvp");
        if (LLVMGetTypeKind(LLVMTypeOf(mv)) != LLVMIntegerTypeKind)
            llv_llvm_unsupported("match върху не-целочислена стойност");
        mv = llv_coerce(mv, lg.i64_ty);
    }

    LLVMTypeRef res_ty = n->type ? llv_llvm_type_resolved(n->type) : lg.i64_ty;
    LLVMValueRef res_alloca = NULL;
    if (res_ty != lg.void_ty) {
        res_alloca = llv_entry_alloca(res_ty, "match_res");
        /* codegen_c инициализира _mr = 0 */
        LLVMBuildStore(lg.builder, LLVMConstNull(res_ty), res_alloca);
    }

    LLVMValueRef fn = LLVMGetBasicBlockParent(LLVMGetInsertBlock(lg.builder));
    LLVMBasicBlockRef merge_bb = LLVMAppendBasicBlockInContext(lg.ctx, fn, "match_end");

    for (int i = 0; i < n->match_arms.len; i++) {
        Node *arm = n->match_arms.data[i];
        if (arm->arm_pattern) {
            LLVMValueRef pat = llv_emit_expr_llvm(arm->arm_pattern);
            if (!pat) llv_llvm_unsupported("print в match pattern");
            LLVMValueRef cond;
            if (str_scrut) {
                /* съдържание, не указател */
                LLVMValueRef ab[] = { mv, pat };
                LLVMValueRef e =
                    llv_h_call(llv_baga_rt("baga_str_eq"), ab, 2, "seq");
                cond = LLVMBuildICmp(lg.builder, LLVMIntNE, e,
                    LLVMConstInt(lg.i64_ty, 0, 0), "eq");
            } else {
                if (LLVMGetTypeKind(LLVMTypeOf(pat)) == LLVMPointerTypeKind)
                    pat = LLVMBuildPtrToInt(lg.builder, pat, lg.i64_ty, "mpp");
                pat = llv_coerce(pat, lg.i64_ty);
                cond = LLVMBuildICmp(lg.builder, LLVMIntEQ, mv, pat, "meq");
            }
            LLVMBasicBlockRef arm_bb = LLVMAppendBasicBlockInContext(lg.ctx, fn, "match_arm");
            LLVMBasicBlockRef next_bb = LLVMAppendBasicBlockInContext(lg.ctx, fn, "match_next");
            LLVMBuildCondBr(lg.builder, cond, arm_bb, next_bb);
            LLVMPositionBuilderAtEnd(lg.builder, arm_bb);
            emit_match_arm_llvm(arm, res_alloca, merge_bb);
            if (!LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(lg.builder)))
                LLVMBuildBr(lg.builder, merge_bb);
            LLVMPositionBuilderAtEnd(lg.builder, next_bb);
        } else {
            /* wildcard `_` → else клон */
            emit_match_arm_llvm(arm, res_alloca, merge_bb);
            if (!LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(lg.builder)))
                LLVMBuildBr(lg.builder, merge_bb);
        }
    }

    /* без wildcard: последният match_next остава отворен — пада в merge
     * (както codegen_c: _mr остава 0 при непокрит случай) */
    if (!LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(lg.builder)))
        LLVMBuildBr(lg.builder, merge_bb);
    LLVMPositionBuilderAtEnd(lg.builder, merge_bb);
    if (!res_alloca) return NULL;
    char *name = llv_tmp_name();
    LLVMValueRef r = LLVMBuildLoad2(lg.builder, res_ty, res_alloca, name);
    free(name);
    return r;
}

/* MEM-1/Task 4: drop(x) — огледало на codegen_c.c:2141-2243.
 * Под --rc: drop ≡ release + bindingът умира (scope exit вече не го
 * release-ва — иначе underflow). Като C: dead флагът е compile-time и
 * path-insensitive — `if c { drop(v) } drop(v)` се компилира, а вторият
 * release се emit-ва пак и runtime guard-ът решава (underflow = чиста
 * грешка). Изключение от C: доказуем двоен drop в ЛИНЕЙНИЯ поток (и двата
 * drop-а извън условен/цикълен клон — lrc_branch_depth) е compile-time
 * грешка, защото в LLVM след реален free glibc може да затрие magic-а и
 * вторият release да е тих no-op (вж. docs/memory-rc-bg.md). Без --rc:
 * baga_drop_* (plain free — алокациите са malloc БЕЗ header). */
static void lrc_emit_drop(Node *n) {
    if (n->args.len != 1 || n->args.data[0]->kind != NODE_IDENT)
        llv_llvm_unsupported("drop приема единствено име на локална");
    Node *id = n->args.data[0];
    Type *at = id->type;
    if (lg.rc) {
        int di = llv_lrc_find(id->name);
        if (di < 0)
            llv_llvm_unsupported("drop на не-track-нато име (не-heap тип?)");
        LLRcLocal *l = &lg.lrc_locals[di];
        if (l->is_param)
            llv_llvm_unsupported("drop на параметър — споделен буфер на извикващия");
        if (l->dead && l->dead_linear && lg.lrc_branch_depth == 0)
            llv_llvm_unsupported("drop на вече drop-нат binding");
        llv_lrc_emit_release(l);
        l->dead = 1;
        if (lg.lrc_branch_depth == 0) l->dead_linear = 1;
        return;
    }
    if (!at) llv_llvm_unsupported("drop с неизвестен тип");
    LLVMValueRef v = llv_emit_expr_llvm(id);
    if (!v) llv_llvm_unsupported("drop аргумент");
    if (at->kind == TYPE_STR) {
        llv_h_call(llv_baga_rt("baga_drop_str"), &v, 1, "");
    } else if (at->kind == TYPE_BYTES) {
        llv_h_call(llv_baga_rt("baga_drop_bytes"), &v, 1, "");
    } else if (at->kind == TYPE_VEC) {
        /* elem_kind като в codegen_c: struct/enum/bytes → box (2),
         * вложен Vec → 3, str → 1 (споделени — не се пипат), иначе 0 */
        int ek = 0;
        LLVMValueRef esz = LLVMConstInt(lg.i64_ty, 0, 0);
        if (at->elem && at->elem->name &&
            (at->elem->kind == TYPE_STRUCT || at->elem->kind == TYPE_ENUM)) {
            ek = 2;
            esz = LLVMSizeOf(llv_user_struct_ty(at->elem->name));
        } else if (at->elem && at->elem->kind == TYPE_BYTES) {
            ek = 2;
            esz = LLVMSizeOf(llv_baga_bytes_ty());
        } else if (at->elem && at->elem->kind == TYPE_VEC) {
            ek = 3;
        } else if (at->elem && at->elem->kind == TYPE_STR) {
            ek = 1;
        }
        LLVMValueRef pa[] = { v, LLVMConstInt(lg.i64_ty, (uint64_t)ek, 0), esz };
        llv_h_call(llv_baga_rt("baga_drop_vec"), pa, 3, "");
    } else if (at->kind == TYPE_MAP) {
        int vb = 0;
        LLVMValueRef vsz = LLVMConstInt(lg.i64_ty, 0, 0);
        if (at->elem && at->elem->name &&
            (at->elem->kind == TYPE_STRUCT || at->elem->kind == TYPE_ENUM)) {
            vb = 1;
            vsz = LLVMSizeOf(llv_user_struct_ty(at->elem->name));
        }
        LLVMValueRef pa[] = { v, LLVMConstInt(lg.i64_ty, (uint64_t)vb, 0), vsz };
        llv_h_call(llv_baga_rt("baga_drop_map"), pa, 3, "");
    } else {
        llv_llvm_unsupported("drop върху не-heap тип (не str/bytes/Vec/Map)");
    }
}

/* NODE_ASSIGN с NODE_FIELD цел (`s.field = x`) — плоско ident.field през
 * alloca-та на локала (като C границата — по-дълбоки пътеки биха оценили
 * целта два пъти). RC: alias-safe редът на codegen_c (NODE_ASSIGN при
 * FIELD цел): оцени дясното, retain на новото (track-нат ident или
 * borrowed — vec_get/map_get/поле/h_*; fresh литерал/ctor/owned call е
 * move — без retain), после release на старото поле по tag (вкл.
 * struct/enum полета — v0.8/v0.10, per-instance helper-и от M24), store.
 * Без move elision (RC2 prepass няма LLVM еквивалент) — retain + нормален
 * release на източника е наблюдаемо еквивалентен на C move-а. */
static LLVMValueRef emit_field_assign_llvm(Node *n) {
    Node *tgt = n->assign_target;
    Node *obj = tgt->field_obj;
    if (!obj || obj->kind != NODE_IDENT)
        llv_llvm_unsupported("присвояване на поле на не-локален обект");
    LLVMValueRef base = llv_st_lookup(obj->name);
    if (!base)
        llv_llvm_unsupported("присвояване на поле на недефинирано име");
    const char *sname = (obj->type && obj->type->kind == TYPE_STRUCT)
        ? obj->type->name : NULL;
    if (!sname) llv_llvm_unsupported("присвояване на поле на не-структура");
    Node *decl = llv_find_struct_decl(sname);
    int idx = decl ? llv_struct_field_index(decl, tgt->field_name) : -1;
    if (idx < 0) {
        char buf[256];
        snprintf(buf, sizeof buf, "непознато поле '%s' на структура '%s'",
                 tgt->field_name, sname);
        llv_llvm_unsupported(buf);
    }
    Node *fld = decl->fields.data[idx];

    /* M24: instantiated generic struct — типът по cname + gen контекст за
     * resolve на полето (като NODE_FIELD четенето по-горе) */
    LLVMTypeRef sty;
    Node *saved_gs = lg.gen_struct;
    int saved_gi = lg.gen_struct_inst;
    if (obj->type->n_targs > 0) {
        char *cn = llv_llvm_struct_cname(obj->type);
        sty = llv_user_struct_ty(cn);
        free(cn);
        lg.gen_struct = decl;
        for (int k = 0; k < decl->struct_inst_count; k++) {
            int same = 1;
            for (int a = 0; a < decl->n_struct_params; a++)
                if (!type_eq(decl->struct_inst_targs[k * decl->n_struct_params + a],
                             obj->type->targs[a])) { same = 0; break; }
            if (same) { lg.gen_struct_inst = k; break; }
        }
    } else {
        sty = llv_user_struct_ty(sname);
    }
    LLVMValueRef gep = LLVMBuildStructGEP2(lg.builder, sty, base,
                                           (unsigned)idx, "fap");
    LLVMTypeRef fty = llv_llvm_type(fld->fld_type);

    /* RC: resolved тип на полето (per инстанция за generic) + heap tag */
    Type *ft = NULL;
    if (lg.rc) {
        if (obj->type->n_targs > 0)
            ft = llv_lrc_fld_inst_type(obj->type, fld->fld_type);
        if (!ft) ft = fld->fld_type->type;
    }
    int ftag = ft ? llv_lrc_heap_tag(ft) : llv_lrc_heap_tag_node(fld->fld_type);

    LLVMValueRef v = llv_emit_expr_llvm(n->assign_val);
    if (!v) llv_llvm_unsupported("print в присвояване на поле");
    v = llv_coerce(v, fty);
    if (lg.rc && ftag) {
        Node *val = n->assign_val;
        int keep;
        if (val->kind == NODE_IDENT) {
            int si = llv_lrc_find(val->name);
            /* track-нат (не dead) — retain; untrack-нат ident (match
             * binding) — само в struct/enum поле (C: fst/fen пътищата) */
            keep = (si >= 0 && !lg.lrc_locals[si].dead) || ftag >= 5;
        } else if (ftag >= 5) {
            /* v0.8/v0.10: свеж литерал/ctor и owned call — move; останалите
             * (поле/vec_get/untrack-нати изрази) се retain-ват */
            keep = val->kind != NODE_STRUCT_LIT && !llv_lrc_is_enum_ctor(val) &&
                   !(val->kind == NODE_CALL && !llv_lrc_borrowed_init(val));
        } else {
            keep = llv_lrc_borrowed_init(val);
        }
        if (keep)
            llv_lrc_emit_retain_val(ftag, ft, fld->fld_type, v);
        /* release на старото поле — СЛЕД retain на новото (alias-safe:
         * `fw.s = fw.s` не обесва стойността) */
        LLVMValueRef old = LLVMBuildLoad2(lg.builder, fty, gep, "faold");
        llv_lrc_emit_release_val(ftag, ft, old);
    }
    LLVMBuildStore(lg.builder, v, gep);
    lg.gen_struct = saved_gs;
    lg.gen_struct_inst = saved_gi;
    return v;
}

LLVMValueRef llv_emit_expr_llvm(Node *n) {
    if (!n) llv_llvm_unsupported("празен израз");

    /* RC4-LLVM: регистриран temp — стойността вече е изчислена в
     * llv_lrc_tmp_begin; върни кеша (аналог на rc_tmp_emit_sub). lrc_tmp_decl
     * предпазва от самозаместване при emission на самия temp. */
    if (lg.rc && lg.lrc_tmps_on && n != lg.lrc_tmp_decl) {
        for (int i = 0; i < lg.lrc_tmp_count; i++)
            if (lg.lrc_tmps[i].site == n) return lg.lrc_tmps[i].val;
    }

    switch (n->kind) {
        case NODE_INT_LIT:
            return LLVMConstInt(lg.i64_ty, (unsigned long long)n->int_val, 1);

        case NODE_FLOAT_LIT:
            /* пълна точност — codegen_c също emit-ва с %.17g (round-trip) */
            return LLVMConstReal(lg.double_ty, n->float_val);

        case NODE_BOOL_LIT:
            return LLVMConstInt(lg.i1_ty, n->bool_val, 0);

        case NODE_BYTES_LIT: {
            LLVMValueRef s = LLVMBuildGlobalStringPtr(lg.builder, n->str_val, "hexlit");
            LLVMValueRef a[] = { s };
            return llv_h_call(llv_baga_rt("baga_bytes_from_hex"), a, 1, "blit");
        }

        case NODE_STR_LIT:
            return LLVMBuildGlobalStringPtr(lg.builder, n->str_val, "str");

        case NODE_IDENT: {
            /* L5: глобална fn като стойност → handle към __clo wrapper-а.
             * Локална fn-typed променлива има type->name == NULL или
             * различно име и минава през llv_st_lookup (i64 handle). */
            if (n->type && n->type->kind == TYPE_FN && n->type->name &&
                strcmp(n->name, n->type->name) == 0) {
                LLVMValueRef wrap = closure_wrapper_named(n->type->name);
                return closure_handle(wrap, LLVMConstNull(lg.ptr_ty));
            }
            LLVMValueRef alloca = llv_st_lookup(n->name);
            if (alloca) {
                char *name = llv_tmp_name();
                LLVMValueRef v = LLVMBuildLoad2(lg.builder,
                    LLVMGetAllocatedType(alloca), alloca, name);
                free(name);
                return v;
            }
            /* L3: payload-less вариант на sum enum → { tag, 0 } стойност */
            if (n->type && n->type->kind == TYPE_ENUM) {
                Node *ed = llv_find_sum_enum(n->type->name);
                int vidx = ed ? llv_sum_variant_index(ed, n->name) : -1;
                if (vidx >= 0) return llv_sum_variant_const(ed, vidx);
            }
            int variant = find_enum_variant(n->name);
            if (variant >= 0)
                return LLVMConstInt(lg.i64_ty, (unsigned long long)variant, 0);
            char buf[256];
            snprintf(buf, sizeof buf, "недефинирано име '%s'", n->name);
            llv_llvm_unsupported(buf);
            return NULL; /* unreachable */
        }

        case NODE_BINARY: {
            /* && / || върху bool (i1); C backend-ът ползва && / || —
             * за чисти операнди резултатът е същият */
            if (n->bin_op == OP_AND || n->bin_op == OP_OR) {
                LLVMValueRef l = llv_to_bool(llv_emit_expr_llvm(n->left));
                /* RC4 v1.1: temp-ове в десния операнд — локален wrap
                 * (LLVM оценява и двата операнда; wrap-ът пак е leak-free). */
                LLRcTmpSave tsv;
                if (lg.rc) llv_lrc_tmp_begin(n->right, 0, &tsv);
                LLVMValueRef r = llv_to_bool(llv_emit_expr_llvm(n->right));
                if (lg.rc) llv_lrc_tmp_end(&tsv);
                char *name = llv_tmp_name();
                LLVMValueRef v = n->bin_op == OP_AND
                    ? LLVMBuildAnd(lg.builder, l, r, name)
                    : LLVMBuildOr(lg.builder, l, r, name);
                free(name);
                return v;
            }
            /* str == str / str != str — чрез baga_str_eq (като codegen_c: strcmp) */
            if ((n->bin_op == OP_EQ || n->bin_op == OP_NEQ) &&
                n->left->type && n->right->type &&
                n->left->type->kind == TYPE_STR && n->right->type->kind == TYPE_STR) {
                LLVMValueRef l = llv_emit_expr_llvm(n->left);
                LLVMValueRef r = llv_emit_expr_llvm(n->right);
                if (!l || !r) llv_llvm_unsupported("print в сравнение на низове");
                LLVMValueRef sc = llv_baga_rt("baga_str_eq");
                LLVMValueRef args[] = { l, r };
                char *nm = llv_tmp_name();
                LLVMValueRef eq = llv_h_call(sc, args, 2, nm);
                free(nm);
                /* baga_str_eq връща i64 0/1 → bool i1 */
                char *nm2 = llv_tmp_name();
                LLVMValueRef v = LLVMBuildICmp(lg.builder, LLVMIntNE, eq,
                    LLVMConstInt(lg.i64_ty, 0, 0), nm2);
                free(nm2);
                if (n->bin_op == OP_NEQ) {
                    char *nm3 = llv_tmp_name();
                    v = LLVMBuildNot(lg.builder, v, nm3);
                    free(nm3);
                }
                return v;
            }
            LLVMValueRef left = llv_emit_expr_llvm(n->left);
            LLVMValueRef right = llv_emit_expr_llvm(n->right);
            if (!left || !right) llv_llvm_unsupported("print в аритметичен израз");
            /* f64 промоция: ако някой операнд е f64, и двата стават f64
             * (като C: int64_t + double → double) */
            int isdbl = LLVMGetTypeKind(LLVMTypeOf(left)) == LLVMDoubleTypeKind ||
                        LLVMGetTypeKind(LLVMTypeOf(right)) == LLVMDoubleTypeKind;
            if (isdbl) {
                left = llv_coerce(left, lg.double_ty);
                right = llv_coerce(right, lg.double_ty);
            } else if (LLVMTypeOf(left) != LLVMTypeOf(right)) {
                /* смесени целочислени ширини → по-широкият тип */
                unsigned wl = LLVMGetIntTypeWidth(LLVMTypeOf(left));
                unsigned wr = LLVMGetIntTypeWidth(LLVMTypeOf(right));
                LLVMTypeRef wide = wl >= wr ? LLVMTypeOf(left) : LLVMTypeOf(right);
                left = llv_coerce(left, wide);
                right = llv_coerce(right, wide);
            }
            return emit_binop_llvm(n->bin_op, left, right);
        }

        case NODE_UNARY: {
            LLVMValueRef v = llv_emit_expr_llvm(n->operand);
            if (!v) llv_llvm_unsupported("print в унарен израз");
            char *name = llv_tmp_name();
            LLVMValueRef r = NULL;
            switch (n->un_op) {
                case UOP_NEG:
                    r = LLVMGetTypeKind(LLVMTypeOf(v)) == LLVMDoubleTypeKind
                      ? LLVMBuildFNeg(lg.builder, v, name)
                      : LLVMBuildNeg(lg.builder, v, name);
                    break;
                case UOP_NOT:
                    /* C truthiness: (!x) — i1 → Not; цяло число → x == 0;
                     * указател → x == null (като codegen_c `(!x)`) */
                    if (LLVMTypeOf(v) == lg.i1_ty)
                        r = LLVMBuildNot(lg.builder, v, name);
                    else if (LLVMGetTypeKind(LLVMTypeOf(v)) == LLVMIntegerTypeKind)
                        r = LLVMBuildICmp(lg.builder, LLVMIntEQ, v,
                            LLVMConstInt(LLVMTypeOf(v), 0, 0), name);
                    else if (LLVMGetTypeKind(LLVMTypeOf(v)) == LLVMPointerTypeKind)
                        r = LLVMBuildIsNull(lg.builder, v, name);
                    else
                        llv_llvm_unsupported("! върху не-булев израз");
                    break;
                case UOP_REF:   llv_llvm_unsupported("референция (&x)"); break;
                case UOP_DEREF: llv_llvm_unsupported("дереференциране (*x)"); break;
            }
            free(name);
            return r;
        }

        case NODE_CALL: {
            /* RC5 v0.7: индекс на temp аргумент за move-консумация при
             * vec_push/vec_set/map_set (helper-ите retain-ват вътрешно —
             * след повикването temp-ът се консумира и се release-ва веднага;
             * нетен move, като _move helper вариантите в codegen_c) */
            int rc_consume_arg = -1;
            /* L3: конструктор на sum enum — bare Ok(x) или Res::Ok(x).
             * Познава се по върнатия тип (TYPE_ENUM) + име на вариант;
             * обикновена fn, връщаща enum, минава нататък. */
            if (n->type && n->type->kind == TYPE_ENUM &&
                (n->callee->kind == NODE_IDENT || n->callee->kind == NODE_PATH)) {
                Node *ed = llv_find_sum_enum(n->type->name);
                const char *vn = n->callee->kind == NODE_PATH
                    ? n->callee->path_variant : n->callee->name;
                int vidx = ed ? llv_sum_variant_index(ed, vn) : -1;
                if (vidx >= 0) {
                    if (ed->enum_payloads && ed->enum_payloads[vidx]) {
                        if (n->args.len != 1)
                            llv_llvm_unsupported("sum конструктор с != 1 аргумент");
                        LLVMValueRef a = llv_emit_expr_llvm(n->args.data[0]);
                        if (!a) llv_llvm_unsupported("print като payload");
                        LLVMTypeRef pty = llv_llvm_type(ed->enum_payloads[vidx]);
                        a = llv_coerce(a, pty);
                        /* RC (Task 6): payload-ът се споделя по указател —
                         * ident/borrowed се retain-ва (+1 за payload
                         * референцията); fresh е owned — move (codegen_c
                         * RC5 v0.6, codegen_c.c:2056-2088) */
                        llv_lrc_embed_retain(n->args.data[0], a);
                        LLVMValueRef cf = llv_sum_ctor_fn(ed, vidx);
                        char *cn = llv_tmp_name();
                        LLVMValueRef r = llv_h_call(cf, &a, 1, cn);
                        free(cn);
                        return r;
                    }
                    /* payload-less вариант, извикан с () */
                    if (n->args.len == 0) return llv_sum_variant_const(ed, vidx);
                }
            }
            /* extern fn → raw libc symbol, before the print/builtin dispatch
             * (an extern named `write` must not become baga_write) */
            Node *ef = NULL;
            if (n->callee->kind == NODE_IDENT) {
                for (int i = 0; lg.program && i < lg.program->items.len; i++) {
                    Node *it = lg.program->items.data[i];
                    if (it->kind == NODE_FN && it->is_extern &&
                        strcmp(it->fn_name, n->callee->name) == 0) {
                        ef = it;
                        break;
                    }
                }
            }
            if (!ef && is_print_call_llvm(n)) {
                emit_print_llvm(n);
                return NULL; /* print е void */
            }
            /* L5: извикване през fn стойност — handle = cell2(code, env)
             * (checker маркер: TYPE_FN без име; като codegen_c) */
            if (!ef && n->callee->type && n->callee->type->kind == TYPE_FN &&
                !n->callee->type->name) {
                Type *ft = n->callee->type;
                LLVMValueRef h = llv_emit_expr_llvm(n->callee);
                if (!h) llv_llvm_unsupported("fn стойност като callee");
                h = llv_coerce(h, lg.i64_ty);
                LLVMValueRef ca[] = { h };
                LLVMValueRef code = llv_h_call(rt_cell2_0(), ca, 1, "code");
                LLVMValueRef env  = llv_h_call(rt_cell2_1(), ca, 1, "env");
                LLVMTypeRef *pt = NULL;
                LLVMTypeRef fty = closure_fn_ty(ft, &pt);
                LLVMValueRef fp = LLVMBuildIntToPtr(lg.builder, code,
                    LLVMPointerType(fty, 0), "fp");
                int np = ft->nparams;
                LLVMValueRef *args = malloc(sizeof(LLVMValueRef) * (size_t)(np + 1));
                args[0] = LLVMBuildIntToPtr(lg.builder, env, lg.ptr_ty, "envp");
                for (int i = 0; i < np; i++) {
                    LLVMValueRef a = llv_emit_expr_llvm(n->args.data[i]);
                    if (!a) llv_llvm_unsupported("fn повикване: аргумент");
                    args[i + 1] = llv_coerce(a, pt[i + 1]);
                }
                free(pt);
                int is_void = LLVMGetReturnType(fty) == lg.void_ty;
                char *rn = is_void ? NULL : llv_tmp_name();
                LLVMValueRef r = LLVMBuildCall2(lg.builder, fty, fp, args,
                    (unsigned)(np + 1), rn ? rn : "");
                free(rn);
                free(args);
                return r;
            }
            if (n->callee->kind != NODE_IDENT)
                llv_llvm_unsupported("повикване през израз (не име)");
            /* !Par: external helpers from libbaga_par.so (src/baga_par_rt.c) */
            if (!ef && n->callee->kind == NODE_IDENT) {
                const char *cn = n->callee->name;
                LLVMTypeRef i64p1[] = { lg.i64_ty };
                LLVMTypeRef par_fn_ty = LLVMFunctionType(lg.i64_ty, i64p1, 1, 0);
                LLVMTypeRef par_fn_ptr = LLVMPointerType(par_fn_ty, 0);
                if ((strcmp(cn, "go") == 0 || strcmp(cn, "go_bg") == 0) &&
                    n->args.len == 2 && n->args.data[0]->kind == NODE_IDENT) {
                    char *wm = llv_llvm_mangle(n->args.data[0]->name);
                    LLVMValueRef worker = LLVMGetNamedFunction(lg.mod, wm);
                    free(wm);
                    if (!worker) llv_llvm_unsupported("go: worker не е намерена");
                    LLVMValueRef fp = LLVMBuildBitCast(lg.builder, worker, par_fn_ptr, "par_fp");
                    LLVMValueRef arg = llv_emit_expr_llvm(n->args.data[1]);
                    if (!arg) llv_llvm_unsupported("go: arg");
                    arg = llv_coerce(arg, lg.i64_ty);
                    const char *rt = strcmp(cn, "go_bg") == 0 ? "baga_go_bg" : "baga_go";
                    LLVMTypeRef gp[] = { par_fn_ptr, lg.i64_ty };
                    LLVMValueRef gfn = llv_rt_libc(rt, lg.i64_ty, gp, 2);
                    LLVMValueRef ga[] = { fp, arg };
                    return llv_h_call(gfn, ga, 2, llv_tmp_name());
                }
                if (strcmp(cn, "pool_map") == 0 && n->args.len == 3 &&
                    n->args.data[0]->kind == NODE_IDENT) {
                    char *wm = llv_llvm_mangle(n->args.data[0]->name);
                    LLVMValueRef worker = LLVMGetNamedFunction(lg.mod, wm);
                    free(wm);
                    if (!worker) llv_llvm_unsupported("pool_map: worker не е намерена");
                    LLVMValueRef fp = LLVMBuildBitCast(lg.builder, worker, par_fn_ptr, "par_fp");
                    LLVMValueRef vec = llv_emit_expr_llvm(n->args.data[1]);
                    LLVMValueRef nw = llv_emit_expr_llvm(n->args.data[2]);
                    if (!vec || !nw) llv_llvm_unsupported("pool_map args");
                    nw = llv_coerce(nw, lg.i64_ty);
                    LLVMTypeRef pp[] = { par_fn_ptr, llv_baga_vec_ptr_ty(), lg.i64_ty };
                    LLVMValueRef pfn = llv_rt_libc("baga_pool_map", llv_baga_vec_ptr_ty(), pp, 3);
                    LLVMValueRef pa[] = { fp, vec, nw };
                    return llv_h_call(pfn, pa, 3, llv_tmp_name());
                }
                /* plain external baga_* par helpers (i64 args) */
                static const struct { const char *baga; const char *c; int n; } pmap[] = {
                    {"join", "baga_join", 1},
                    {"detach", "baga_detach", 1},
                    {"chan_new", "baga_chan_new", 1},
                    {"chan_send", "baga_chan_send", 2},
                    {"chan_recv", "baga_chan_recv", 1},
                    {"chan_recv2", "baga_chan_recv2", 1},
                    {"chan_try_recv", "baga_chan_try_recv", 1},
                    {"chan_recv_timeout", "baga_chan_recv_timeout", 2},
                    {"chan_select2", "baga_chan_select2", 2},
                    {"chan_select2_wait", "baga_chan_select2_wait", 2},
                    {"chan_select2_timeout", "baga_chan_select2_timeout", 3},
                    {"chan_close", "baga_chan_close", 1},
                    {"chan_len", "baga_chan_len", 1},
                    {"sleep_ms", "baga_sleep_ms", 1},
                    {"mutex_new", "baga_mutex_new", 0},
                    {"mutex_lock", "baga_mutex_lock", 1},
                    {"mutex_unlock", "baga_mutex_unlock", 1},
                    {"signal_watch", "baga_signal_watch", 1},
                    {"signal_check", "baga_signal_check", 0},
                    {"signal_clear", "baga_signal_clear", 0},
                    {"signal_wait", "baga_signal_wait", 1},
                    {"signal_raise", "baga_signal_raise", 1},
                    {"cell2", "baga_cell2", 2},
                    {"cell2_0", "baga_cell2_0", 1},
                    {"cell2_1", "baga_cell2_1", 1},
                };
                for (int pi = 0; pi < (int)(sizeof(pmap) / sizeof(pmap[0])); pi++) {
                    if (strcmp(cn, pmap[pi].baga) != 0) continue;
                    if (n->args.len != pmap[pi].n)
                        llv_llvm_unsupported("!Par arnost");
                    LLVMTypeRef pts[4];
                    for (int k = 0; k < pmap[pi].n; k++) pts[k] = lg.i64_ty;
                    LLVMValueRef pfn = llv_rt_libc(pmap[pi].c, lg.i64_ty, pts, pmap[pi].n);
                    LLVMValueRef pa[4];
                    for (int k = 0; k < pmap[pi].n; k++) {
                        LLVMValueRef a = llv_emit_expr_llvm(n->args.data[k]);
                        if (!a) llv_llvm_unsupported("!Par arg");
                        pa[k] = llv_coerce(a, lg.i64_ty);
                    }
                    return llv_h_call(pfn, pa, pmap[pi].n, llv_tmp_name());
                }
            }
            /* str/io/vec builtin-и → baga_* IR helpers (като в codegen_c) */
            static const struct { const char *baga; const char *rt; } bmap[] = {
                {"len",         "baga_len"},
                {"char_at",     "baga_char_at"},
                {"byte_at",     "baga_byte_at"},
                {"substr",      "baga_substr"},
                {"concat",      "baga_concat"},
                {"read_file",   "baga_read_file"},
                {"chr",         "baga_chr"},
                {"ord",         "baga_ord"},
                {"str_eq",      "baga_str_eq"},
                {"i64_to_str",  "baga_i64_to_str"},
                {"f64_to_str",  "baga_f64_to_str"},
                {"vec_new",     "baga_vec_new"},
                {"vec_push_str","baga_vec_push_str"},
                {"vec_get_str", "baga_vec_get_str"},
                {"vec_set_str", "baga_vec_set_str"},
                {"vec_len",     "baga_vec_len"},
                {"bytes_len",   "baga_bytes_len"},
                {"bytes_at",    "baga_bytes_at"},
                {"bytes_slice", "baga_bytes_slice"},
                {"bytes_concat","baga_bytes_concat"},
                {"bytes_put",   "baga_bytes_put"},
                {"bytes_new",   "baga_bytes_new"},
                {"bytes_set",   "baga_bytes_set"},
                {"bytes_push",  "baga_bytes_push"},
                {"str_h",       "baga_str_h"},
                {"h_str",       "baga_h_str"},
                {"bytes_h",     "baga_bytes_h"},
                {"h_bytes",     "baga_h_bytes"},
                {"bytes_of_str","baga_bytes_from_str"},
                {"str_of_bytes","baga_bytes_to_str"},
                {"hex_encode",  "baga_hex_encode"},
                {"hex_decode",  "baga_hex_decode"},
                {"arena_new",   "baga_arena_new"},
                {"arena_alloc", "baga_arena_alloc"},
                {"arena_reset", "baga_arena_reset"},
                {"arena_free",  "baga_arena_free"},
                {"arg_count",   "baga_arg_count"},
                {"arg",         "baga_arg"},
                {"exit",        "baga_exit"},
                {"eprintln",    "baga_eprintln"},
                {"map_new",     "baga_map_new"},
                {"map_len",     "baga_map_len"},
                {"map_h",       "baga_map_h"},
                {"h_map",       "baga_h_map"},
            };
            LLVMValueRef fn = NULL;
            if (ef) fn = LLVMGetNamedFunction(lg.mod, ef->fn_name);
            /* типизирани вектори: helper по елементния тип на вектора */
            if (!ef &&
                (strcmp(n->callee->name, "vec_push") == 0 ||
                 strcmp(n->callee->name, "vec_get") == 0 ||
                 strcmp(n->callee->name, "vec_set") == 0 ||
                 strcmp(n->callee->name, "vec_slice") == 0 ||
                 strcmp(n->callee->name, "vec_concat") == 0)) {
                Type *vt = n->args.len > 0 ? n->args.data[0]->type : NULL;
                /* L4/S2/L3: struct, bytes и sum enum елементи → box
                 * helper-и, sizeof от call site-а (огледало на codegen_c) */
                if (vt && vt->kind == TYPE_VEC && vt->elem &&
                    ((vt->elem->kind == TYPE_STRUCT && vt->elem->name) ||
                     (vt->elem->kind == TYPE_ENUM && vt->elem->name) ||
                     vt->elem->kind == TYPE_BYTES)) {
                    LLVMTypeRef sty = vt->elem->kind == TYPE_BYTES
                        ? llv_baga_bytes_ty()
                        : (vt->elem->kind == TYPE_STRUCT && vt->elem->n_targs > 0)
                            ? llv_user_struct_ty_inst(vt->elem)
                            : llv_user_struct_ty(vt->elem->name);
                    LLVMValueRef sz = LLVMSizeOf(sty);
                    const char *cn = n->callee->name;
                    LLVMValueRef v = llv_emit_expr_llvm(n->args.data[0]);
                    if (!v) llv_llvm_unsupported("vec аргумент");
                    /* вложен vec_get връща i8* (елементът на външния Vec е
                     * Vec → str helper) — box helper-ите искат %baga_Vec* */
                    if (LLVMTypeOf(v) != llv_baga_vec_ptr_ty())
                        v = LLVMBuildBitCast(lg.builder, v, llv_baga_vec_ptr_ty(), "vc");
                    if (strcmp(cn, "vec_push") == 0) {
                        LLVMValueRef e = llv_emit_expr_llvm(n->args.data[1]);
                        if (!e) llv_llvm_unsupported("vec_push аргумент");
                        /* RC5 v0.7/v1.0b: temp аргумент (owned call/to_str
                         * резултат) — move в box-а, консумирай temp-а
                         * (codegen_c.c:2362-2370) */
                        int tci = llv_lrc_tmp_find(n->args.data[1]);
                        /* RC: bytes елемент — retain на data (контейнерът
                         * става собственик; codegen_c: baga_vec_push_bytes).
                         * struct/enum (Task 6): box копието споделя heap
                         * полетата — retain_<S> (освен при fresh литерал/
                         * ctor — move; като codegen_c.c:2266-2301). */
                        if (lg.rc && vt->elem->kind == TYPE_BYTES) {
                            if (tci < 0) {   /* temp: move — без retain */
                                LLVMValueRef bd = LLVMBuildExtractValue(lg.builder,
                                    e, 0, "bd");
                                llv_h_call(llv_baga_rt("baga_rc_retain"), &bd, 1, "rcr");
                            }
                        } else if (lg.rc) {
                            llv_lrc_embed_retain(n->args.data[1], e);
                        }
                        LLVMValueRef tmp = llv_entry_alloca(sty, "bx");
                        LLVMBuildStore(lg.builder, e, tmp);
                        LLVMValueRef bp = LLVMBuildBitCast(lg.builder, tmp, lg.ptr_ty, "bp");
                        LLVMValueRef pa[] = { v, bp, sz };
                        LLVMValueRef r = llv_h_call(llv_baga_rt("baga_vec_push_box"), pa, 3, "");
                        llv_lrc_tmp_consume(tci);
                        return r;
                    }
                    if (strcmp(cn, "vec_get") == 0) {
                        LLVMValueRef i = llv_coerce(llv_emit_expr_llvm(n->args.data[1]), lg.i64_ty);
                        LLVMValueRef pa[] = { v, i };
                        LLVMValueRef box = llv_h_call(llv_baga_rt("baga_vec_get_box"), pa, 2, "box");
                        LLVMValueRef sp = LLVMBuildBitCast(lg.builder, box,
                            LLVMPointerType(sty, 0), "sp");
                        return LLVMBuildLoad2(lg.builder, sty, sp, "v");
                    }
                    if (strcmp(cn, "vec_set") == 0) {
                        LLVMValueRef i = llv_coerce(llv_emit_expr_llvm(n->args.data[1]), lg.i64_ty);
                        LLVMValueRef e = llv_emit_expr_llvm(n->args.data[2]);
                        if (!e) llv_llvm_unsupported("vec_set аргумент");
                        /* RC5 v0.7/v1.0b: temp аргумент — move (без retain
                         * на новото; старото пак се release-ва), консумирай */
                        int tci = llv_lrc_tmp_find(n->args.data[2]);
                        /* RC: bytes елемент — retain на новата data, release
                         * на старата (в този ред — alias-safe; codegen_c:
                         * baga_vec_set_bytes). struct/enum (Task 6): retain
                         * на новото, после relf на стария box (codegen_c:
                         * baga_vec_set_box_rc). */
                        if (lg.rc && vt->elem->kind == TYPE_BYTES) {
                            if (tci < 0) {   /* temp: move — без retain */
                                LLVMValueRef bd = LLVMBuildExtractValue(lg.builder,
                                    e, 0, "bd");
                                llv_h_call(llv_baga_rt("baga_rc_retain"), &bd, 1, "rcr");
                            }
                            LLVMValueRef ga[] = { v, i };
                            LLVMValueRef ob = llv_h_call(llv_baga_rt("baga_vec_get_box"),
                                ga, 2, "ob");
                            LLVMValueRef obp = LLVMBuildBitCast(lg.builder, ob,
                                LLVMPointerType(llv_baga_bytes_ty(), 0), "obp");
                            LLVMValueRef od = LLVMBuildLoad2(lg.builder, lg.ptr_ty,
                                LLVMBuildStructGEP2(lg.builder, llv_baga_bytes_ty(),
                                    obp, 0, "odp"), "od");
                            llv_h_call(llv_baga_rt("baga_rc_release_bytes"), &od, 1, "");
                        } else if (lg.rc) {
                            llv_lrc_embed_retain(n->args.data[2], e);
                            LLVMValueRef rel = llv_lrc_box_rel_fn_ty(vt->elem);
                            if (rel) {
                                LLVMValueRef ga[] = { v, i };
                                LLVMValueRef ob = llv_h_call(
                                    llv_baga_rt("baga_vec_get_box"), ga, 2, "ob");
                                LLVMTypeRef rfty = LLVMFunctionType(lg.void_ty,
                                    &lg.ptr_ty, 1, 0);
                                LLVMBuildCall2(lg.builder, rfty, rel, &ob, 1, "");
                            }
                        }
                        LLVMValueRef tmp = llv_entry_alloca(sty, "bx");
                        LLVMBuildStore(lg.builder, e, tmp);
                        LLVMValueRef bp = LLVMBuildBitCast(lg.builder, tmp, lg.ptr_ty, "bp");
                        LLVMValueRef pa[] = { v, i, bp, sz };
                        LLVMValueRef r = llv_h_call(llv_baga_rt("baga_vec_set_box"), pa, 4, "");
                        llv_lrc_tmp_consume(tci);
                        return r;
                    }
                    if (strcmp(cn, "vec_slice") == 0) {
                        LLVMValueRef a = llv_coerce(llv_emit_expr_llvm(n->args.data[1]), lg.i64_ty);
                        LLVMValueRef b = llv_coerce(llv_emit_expr_llvm(n->args.data[2]), lg.i64_ty);
                        LLVMValueRef pa[] = { v, a, b, sz };
                        return llv_h_call(llv_baga_rt("baga_vec_slice_box"), pa, 4, "r");
                    }
                    /* vec_concat */
                    LLVMValueRef w = llv_emit_expr_llvm(n->args.data[1]);
                    if (!w) llv_llvm_unsupported("vec_concat аргумент");
                    LLVMValueRef pa[] = { v, w, sz };
                    return llv_h_call(llv_baga_rt("baga_vec_concat_box"), pa, 3, "r");
                }
                const char *suf = "i64";
                if (vt && vt->kind == TYPE_VEC && vt->elem) {
                    if (vt->elem->kind == TYPE_STR) suf = "str";
                    else if (vt->elem->kind == TYPE_F64) suf = "f64";
                    else if (vt->elem->kind == TYPE_VEC) suf = "str";
                    else if (vt->elem->kind == TYPE_STRUCT)
                        llv_llvm_unsupported("Vec<struct> (анонимен — само C бекенда; вж. docs/language-en.md)");
                }
                /* RC5 v0.7: temp стойност (str, и Vec елементи през str
                 * helper-а) — helper-ът retain-ва; маркирай за consume +
                 * release след повикването (нетен move, codegen_c.c:2484) */
                if (lg.rc && strcmp(suf, "str") == 0 &&
                    strcmp(n->callee->name, "vec_get") != 0 &&
                    strcmp(n->callee->name, "vec_slice") != 0 &&
                    strcmp(n->callee->name, "vec_concat") != 0) {
                    int vi = strcmp(n->callee->name, "vec_push") == 0 ? 1 : 2;
                    if (n->args.len > vi &&
                        llv_lrc_tmp_find(n->args.data[vi]) >= 0)
                        rc_consume_arg = vi;
                }
                char rt_name[64];
                snprintf(rt_name, sizeof rt_name, "baga_%s_%s",
                         n->callee->name, suf);
                fn = llv_baga_rt(rt_name);
            }
            /* карти: map_set/map_get по ключ+стойност; has/del/keys по ключ
             * (типовете идват от checker-а; огледало на lowering-а в codegen_c) */
            if (!ef && (strcmp(n->callee->name, "map_set") == 0 ||
                        strcmp(n->callee->name, "map_get") == 0)) {
                Type *mt = n->args.len > 0 ? n->args.data[0]->type : NULL;
                const char *ksuf = "str";
                if (mt && mt->kind == TYPE_MAP && mt->key) {
                    if (mt->key->kind == TYPE_I64) ksuf = "i64";
                    else if (mt->key->kind == TYPE_BYTES) ksuf = "bytes";
                }
                int is_set = strcmp(n->callee->name, "map_set") == 0;
                /* struct / sum-enum стойности → box path (като codegen_c) */
                if (mt && mt->kind == TYPE_MAP && mt->elem && mt->elem->name &&
                    (mt->elem->kind == TYPE_STRUCT ||
                     mt->elem->kind == TYPE_ENUM)) {
                    LLVMTypeRef sty = llv_user_struct_ty(mt->elem->name);
                    char rt_name[64];
                    snprintf(rt_name, sizeof rt_name, "baga_%s_%s_box",
                             n->callee->name, ksuf);
                    LLVMValueRef mv = llv_emit_expr_llvm(n->args.data[0]);
                    /* същият cast като при vec box — вложен достъп е i8* */
                    if (mv && LLVMTypeOf(mv) != llv_baga_map_ptr_ty())
                        mv = LLVMBuildBitCast(lg.builder, mv, llv_baga_map_ptr_ty(), "mc");
                    LLVMValueRef k = llv_emit_expr_llvm(n->args.data[1]);
                    if (!mv || !k) llv_llvm_unsupported("map аргумент");
                    if (is_set) {
                        LLVMValueRef v = llv_emit_expr_llvm(n->args.data[2]);
                        if (!v) llv_llvm_unsupported("map_set стойност");
                        /* RC5 v1.0b: temp стойност (owned call) — move в
                         * box-а (embed_retain вече не retain-ва owned call),
                         * консумирай temp-а (codegen_c.c:2575-2582) */
                        int tci = llv_lrc_tmp_find(n->args.data[2]);
                        /* RC (Task 6): box стойността споделя heap полетата —
                         * retain на новото, после relf на стария box при
                         * overwrite (alias-safe ред; codegen_c:
                         * baga_map_set_<k>_box_rc, codegen_c.c:2514-2522) */
                        if (lg.rc) {
                            llv_lrc_embed_retain(n->args.data[2], v);
                            LLVMValueRef rel = llv_lrc_box_rel_fn_ty(mt->elem);
                            if (rel) {
                                char get_name[64];
                                snprintf(get_name, sizeof get_name,
                                         "baga_map_get_%s_box", ksuf);
                                LLVMValueRef ga[] = { mv, k };
                                LLVMValueRef ob = llv_h_call(llv_baga_rt(get_name),
                                    ga, 2, "ob");
                                LLVMValueRef cfn =
                                    LLVMGetBasicBlockParent(
                                        LLVMGetInsertBlock(lg.builder));
                                LLVMBasicBlockRef rel_bb =
                                    LLVMAppendBasicBlockInContext(lg.ctx,
                                        cfn, "orel");
                                LLVMBasicBlockRef done_bb =
                                    LLVMAppendBasicBlockInContext(lg.ctx,
                                        cfn, "odone");
                                LLVMBuildCondBr(lg.builder,
                                    LLVMBuildIsNull(lg.builder, ob, "isn"),
                                    done_bb, rel_bb);
                                LLVMPositionBuilderAtEnd(lg.builder, rel_bb);
                                LLVMTypeRef rfty = LLVMFunctionType(lg.void_ty,
                                    &lg.ptr_ty, 1, 0);
                                LLVMBuildCall2(lg.builder, rfty, rel, &ob,
                                               1, "");
                                LLVMBuildBr(lg.builder, done_bb);
                                LLVMPositionBuilderAtEnd(lg.builder, done_bb);
                            }
                        }
                        LLVMValueRef tmp = llv_entry_alloca(sty, "bx");
                        LLVMBuildStore(lg.builder, llv_coerce(v, sty), tmp);
                        LLVMValueRef bp = LLVMBuildBitCast(lg.builder, tmp,
                            lg.ptr_ty, "bp");
                        LLVMValueRef pa[] = { mv, k, bp, LLVMSizeOf(sty) };
                        LLVMValueRef r = llv_h_call(llv_baga_rt(rt_name), pa, 4, "");
                        llv_lrc_tmp_consume(tci);
                        return r;
                    }
                    LLVMValueRef pa[] = { mv, k };
                    LLVMValueRef box = llv_h_call(llv_baga_rt(rt_name), pa, 2, "box");
                    /* missing key → нулева стойност (zero struct / tag-0 enum),
                     * като `*(%s){0}` в codegen_c */
                    LLVMValueRef tmp = llv_entry_alloca(sty, "mg");
                    LLVMBuildStore(lg.builder, LLVMConstNull(sty), tmp);
                    LLVMValueRef cfn =
                        LLVMGetBasicBlockParent(LLVMGetInsertBlock(lg.builder));
                    LLVMBasicBlockRef have_bb =
                        LLVMAppendBasicBlockInContext(lg.ctx, cfn, "mg_have");
                    LLVMBasicBlockRef done_bb =
                        LLVMAppendBasicBlockInContext(lg.ctx, cfn, "mg_done");
                    LLVMBuildCondBr(lg.builder,
                        LLVMBuildIsNull(lg.builder, box, "isn"), done_bb, have_bb);
                    LLVMPositionBuilderAtEnd(lg.builder, have_bb);
                    LLVMValueRef sp = LLVMBuildBitCast(lg.builder, box,
                        LLVMPointerType(sty, 0), "sp");
                    LLVMBuildStore(lg.builder,
                        LLVMBuildLoad2(lg.builder, sty, sp, "sv"), tmp);
                    LLVMBuildBr(lg.builder, done_bb);
                    LLVMPositionBuilderAtEnd(lg.builder, done_bb);
                    return LLVMBuildLoad2(lg.builder, sty, tmp, "mv");
                }
                const char *vsuf = "i64";
                if (mt && mt->kind == TYPE_MAP && mt->elem) {
                    if (mt->elem->kind == TYPE_STR) vsuf = "str";
                    else if (mt->elem->kind == TYPE_F64) vsuf = "f64";
                    else if (mt->elem->kind == TYPE_BYTES) vsuf = "bytes";
                }
                /* RC5 v0.7: temp стойност в map_set (str/bytes) — helper-ът
                 * retain-ва; маркирай за consume + release след повикването
                 * (codegen_c.c:2647-2662). Ключът пак е retain+release. */
                if (lg.rc && is_set && n->args.len >= 3 &&
                    (strcmp(vsuf, "str") == 0 || strcmp(vsuf, "bytes") == 0) &&
                    llv_lrc_tmp_find(n->args.data[2]) >= 0)
                    rc_consume_arg = 2;
                char rt_name[64];
                snprintf(rt_name, sizeof rt_name, "baga_%s_%s_%s",
                         n->callee->name, ksuf, vsuf);
                fn = llv_baga_rt(rt_name);
            }
            if (!ef && !fn &&
                (strcmp(n->callee->name, "map_has") == 0 ||
                 strcmp(n->callee->name, "map_del") == 0 ||
                 strcmp(n->callee->name, "map_keys") == 0)) {
                Type *mt = n->args.len > 0 ? n->args.data[0]->type : NULL;
                const char *ksuf = "str";
                if (mt && mt->kind == TYPE_MAP && mt->key) {
                    if (mt->key->kind == TYPE_I64) ksuf = "i64";
                    else if (mt->key->kind == TYPE_BYTES) ksuf = "bytes";
                }
                char rt_name[64];
                snprintf(rt_name, sizeof rt_name, "baga_%s_%s",
                         n->callee->name, ksuf);
                fn = llv_baga_rt(rt_name);
            }
            for (int bi = 0; !ef && !fn && bi < (int)(sizeof(bmap) / sizeof(bmap[0])); bi++) {
                if (strcmp(n->callee->name, bmap[bi].baga) == 0) {
                    fn = llv_baga_rt(bmap[bi].rt);
                    break;
                }
            }
            if (!fn) {
                char *m = llv_llvm_mangle(n->callee->name);
                fn = LLVMGetNamedFunction(lg.mod, m);
                free(m);
            }
            /* MEM-1: drop(x) builtin. user fn 'drop' е resolve-ната по-горе
             * (fn != NULL), затова тук drop е винаги builtin-ът — огледало на
             * checker guard-а (checker.c:1822). */
            if (!fn && strcmp(n->callee->name, "drop") == 0) {
                lrc_emit_drop(n);
                return NULL;  /* drop е statement-израз без стойност */
            }
            /* MEM-4: mark/rewind/persist работят върху bump arena-та, която
             * съществува само в C бекенда (LLVM runtime-ът е plain malloc) */
            if (!fn && (strcmp(n->callee->name, "mem_mark") == 0 ||
                        strcmp(n->callee->name, "mem_rewind") == 0 ||
                        strcmp(n->callee->name, "mem_persist_begin") == 0 ||
                        strcmp(n->callee->name, "mem_persist_end") == 0))
                llv_llvm_unsupported("mem_mark/mem_rewind/mem_persist_* — само C бекенда; вж. docs/language-en.md");
            if (!fn) {
                char buf[256];
                snprintf(buf, sizeof buf, "вградена функция '%s'", n->callee->name);
                llv_llvm_unsupported(buf);
            }
            int nargs = n->args.len;
            LLVMValueRef *args = malloc(sizeof(LLVMValueRef) * (size_t)(nargs > 0 ? nargs : 1));
            for (int i = 0; i < nargs; i++) {
                LLVMValueRef a = llv_emit_expr_llvm(n->args.data[i]);
                if (!a) llv_llvm_unsupported("print като аргумент");
                args[i] = llv_coerce(a, LLVMTypeOf(LLVMGetParam(fn, (unsigned)i)));
            }
            LLVMTypeRef fn_ty = LLVMGlobalGetValueType(fn);
            int is_void = LLVMGetReturnType(fn_ty) == lg.void_ty;
            char *name = is_void ? NULL : llv_tmp_name();
            LLVMValueRef result = LLVMBuildCall2(lg.builder, fn_ty, fn, args,
                                                 (unsigned)nargs, is_void ? "" : name);
            if (rc_consume_arg >= 0) {
                /* RC5 v0.7: helper-ът retain-на своята референция към temp-а;
                 * консумирай го и release-ни temp-овата — нетен move в
                 * контейнера (като _move helper вариантите в codegen_c) */
                int ti = llv_lrc_tmp_find(n->args.data[rc_consume_arg]);
                if (ti >= 0) {
                    LLRcTmp t = lg.lrc_tmps[ti];
                    llv_lrc_tmp_consume(ti);
                    llv_lrc_emit_release_val(t.tag, t.type, t.val);
                }
            }
            if (ef && llv_extern_ret_is_str_llvm(ef)) {
                /* str return: NULL → "" (като codegen_c — getenv е тотална) */
                LLVMValueRef empty = LLVMBuildGlobalStringPtr(lg.builder, "", "empty");
                LLVMValueRef isnull = LLVMBuildIsNull(lg.builder, result, "isnull");
                result = LLVMBuildSelect(lg.builder, isnull, empty, result, "nn");
            }
            free(name);
            free(args);
            return result;
        }

        case NODE_MATCH:
            return emit_match_llvm(n);

        case NODE_ASSIGN: {
            if (n->assign_target->kind == NODE_FIELD)
                return emit_field_assign_llvm(n);
            if (n->assign_target->kind != NODE_IDENT)
                llv_llvm_unsupported("присвояване на индекс");
            LLVMValueRef alloca = llv_st_lookup(n->assign_target->name);
            if (!alloca) {
                char buf[256];
                snprintf(buf, sizeof buf, "присвояване на недефинирано име '%s'",
                         n->assign_target->name);
                llv_llvm_unsupported(buf);
            }
            LLVMValueRef v = llv_emit_expr_llvm(n->assign_val);
            if (!v) llv_llvm_unsupported("print в присвояване");
            v = llv_coerce(v, LLVMGetAllocatedType(alloca));
            /* RC: overwrite на track-нат локал — retain на новото (при alias
             * от IDENT или borrowed дясно — vec_get/map_get/поле/h_*;
             * codegen_c.c:2866 `keep`) ПРЕДИ release на старото:
             * alias-safe редът на codegen_c (`x = x` не обесва стойността).
             * Params/dead — не. */
            int ai = lg.rc ? llv_lrc_find(n->assign_target->name) : -1;
            if (ai >= 0 && !lg.lrc_locals[ai].is_param && !lg.lrc_locals[ai].dead) {
                if (n->assign_val->kind == NODE_IDENT ||
                    llv_lrc_borrowed_init(n->assign_val))
                    llv_lrc_emit_retain_val(lg.lrc_locals[ai].tag,
                                        lg.lrc_locals[ai].type,
                                        lg.lrc_locals[ai].type_node, v);
                llv_lrc_emit_release(&lg.lrc_locals[ai]);
            }
            LLVMBuildStore(lg.builder, v, alloca);
            return v;
        }

        case NODE_IF:       llv_llvm_unsupported("if като израз"); break;
        case NODE_INDEX:    llv_llvm_unsupported("индексиране"); break;
        case NODE_FIELD: {
            /* L5/L6: модул.функция като стойност → handle към wrapper-а */
            if (n->type && n->type->kind == TYPE_FN && n->type->name) {
                LLVMValueRef wrap = closure_wrapper_named(n->type->name);
                return closure_handle(wrap, LLVMConstNull(lg.ptr_ty));
            }
            /* obj.field — като в codegen_c (.field); обектът е struct by
             * value, така че адресът е alloca (променлива) или временен spill */
            Node *obj = n->field_obj;
            const char *sname = (obj->type && obj->type->kind == TYPE_STRUCT)
                ? obj->type->name : NULL;
            if (!sname) llv_llvm_unsupported("достъп до поле на не-структура");
            Node *decl = llv_find_struct_decl(sname);
            int idx = decl ? llv_struct_field_index(decl, n->field_name) : -1;
            if (idx < 0) {
                char buf[256];
                snprintf(buf, sizeof buf, "непознато поле '%s' на структура '%s'",
                         n->field_name, sname);
                llv_llvm_unsupported(buf);
            }
            /* M24: instantiated generic struct — типът по cname */
            LLVMTypeRef sty;
            Node *saved_gs = lg.gen_struct;
            int saved_gi = lg.gen_struct_inst;
            if (obj->type->n_targs > 0) {
                char *cn = llv_llvm_struct_cname(obj->type);
                sty = llv_user_struct_ty(cn);
                free(cn);
                lg.gen_struct = decl;
                for (int k = 0; k < decl->struct_inst_count; k++) {
                    int same = 1;
                    for (int a = 0; a < decl->n_struct_params; a++)
                        if (!type_eq(decl->struct_inst_targs[k * decl->n_struct_params + a],
                                     obj->type->targs[a])) { same = 0; break; }
                    if (same) { lg.gen_struct_inst = k; break; }
                }
            } else {
                sty = llv_user_struct_ty(sname);
            }
            LLVMValueRef base = NULL;
            if (obj->kind == NODE_IDENT)
                base = llv_st_lookup(obj->name);
            if (!base) {
                LLVMValueRef ov = llv_emit_expr_llvm(obj);
                if (!ov) llv_llvm_unsupported("print като struct обект");
                base = llv_entry_alloca(sty, "ftmp");
                LLVMBuildStore(lg.builder, llv_coerce(ov, sty), base);
            }
            LLVMValueRef gep = LLVMBuildStructGEP2(lg.builder, sty, base,
                                                   (unsigned)idx, "fldp");
            LLVMTypeRef fty = llv_llvm_type(decl->fields.data[idx]->fld_type);
            char *name = llv_tmp_name();
            LLVMValueRef r = LLVMBuildLoad2(lg.builder, fty, gep, name);
            free(name);
            lg.gen_struct = saved_gs;
            lg.gen_struct_inst = saved_gi;
            return r;
        }
        case NODE_STRUCT_LIT: {
            /* (Struct){ .f = v, ... } — alloca + store по полета;
             * липсващите полета са нули (като designated init в C) */
            Node *decl = llv_find_struct_decl(n->lit_name);
            if (!decl) {
                char buf[256];
                snprintf(buf, sizeof buf, "неизвестна структура '%s'", n->lit_name);
                llv_llvm_unsupported(buf);
            }
            /* M24: instantiated generic struct — типът по cname */
            LLVMTypeRef sty;
            if (n->type && n->type->kind == TYPE_STRUCT && n->type->n_targs > 0) {
                char *cn = llv_llvm_struct_cname(n->type);
                sty = llv_user_struct_ty(cn);
                free(cn);
            } else {
                sty = llv_user_struct_ty(n->lit_name);
            }
            LLVMValueRef tmp = llv_entry_alloca(sty, "slit");
            LLVMBuildStore(lg.builder, LLVMConstNull(sty), tmp);
            /* M24: полетата на generic struct literal се резолват под
             * substitution (параметри → targs на литерала) */
            Node *saved_gs = lg.gen_struct;
            int saved_gi = lg.gen_struct_inst;
            if (n->type && n->type->n_targs > 0) {
                lg.gen_struct = decl;
                for (int k = 0; k < decl->struct_inst_count; k++) {
                    int same = 1;
                    for (int a = 0; a < decl->n_struct_params; a++)
                        if (!type_eq(decl->struct_inst_targs[k * decl->n_struct_params + a],
                                     n->type->targs[a])) { same = 0; break; }
                    if (same) { lg.gen_struct_inst = k; break; }
                }
            }
            for (int i = 0; i < n->n_lit_fields; i++) {
                int idx = llv_struct_field_index(decl, n->lit_fields[i]);
                if (idx < 0) {
                    char buf[256];
                    snprintf(buf, sizeof buf, "непознато поле '%s' на структура '%s'",
                             n->lit_fields[i], n->lit_name);
                    llv_llvm_unsupported(buf);
                }
                LLVMValueRef v = llv_emit_expr_llvm(n->lit_values.data[i]);
                if (!v) llv_llvm_unsupported("print като стойност на поле");
                LLVMTypeRef fty = llv_llvm_type(decl->fields.data[idx]->fld_type);
                /* RC (Task 6): литералът споделя heap полета по указател —
                 * ident/borrowed стойност се retain-ва (+1 за полето);
                 * fresh израз е owned — move (codegen_c.c:3111-3141) */
                llv_lrc_embed_retain(n->lit_values.data[i], v);
                LLVMValueRef gep = LLVMBuildStructGEP2(lg.builder, sty, tmp,
                                                       (unsigned)idx, "fld");
                LLVMBuildStore(lg.builder, llv_coerce(v, fty), gep);
            }
            char *name = llv_tmp_name();
            LLVMValueRef r = LLVMBuildLoad2(lg.builder, sty, tmp, name);
            free(name);
            lg.gen_struct = saved_gs;
            lg.gen_struct_inst = saved_gi;
            return r;
        }
        case NODE_PATH: {
            /* Enum::Variant — plain enum → i64 таг; L3 sum payload-less
             * вариант → { tag, 0 } стойност (A1, като codegen_c) */
            Node *ed = NULL;
            for (int i = 0; lg.program && i < lg.program->items.len; i++) {
                Node *item = lg.program->items.data[i];
                if (item->kind == NODE_ENUM && item->enum_name &&
                    strcmp(item->enum_name, n->path_enum) == 0) { ed = item; break; }
            }
            int vidx = ed ? llv_sum_variant_index(ed, n->path_variant) : -1;
            if (vidx < 0) {
                char buf[256];
                snprintf(buf, sizeof buf, "непознат път '%s::%s'",
                         n->path_enum ? n->path_enum : "?",
                         n->path_variant ? n->path_variant : "?");
                llv_llvm_unsupported(buf);
            }
            if (llv_is_sum_enum_item(ed)) {
                if (ed->enum_payloads && ed->enum_payloads[vidx])
                    llv_llvm_unsupported("sum вариант с payload без аргумент");
                return llv_sum_variant_const(ed, vidx);
            }
            return LLVMConstInt(lg.i64_ty, (unsigned long long)vidx, 0);
        }
        case NODE_TRY: {
            /* M20: e? — при payload ефекти: изчисли, провери слота, при
             * грешка return-ни нулата; иначе passthrough (и в catch верига) */
            if (!llv_llvm_has_payload_effects(n->try_expr ? n->try_expr->type : NULL) ||
                lg.eff_depth > 0)
                return llv_emit_expr_llvm(n->try_expr);
            LLVMValueRef v = llv_emit_expr_llvm(n->try_expr);
            LLVMValueRef fn = LLVMGetBasicBlockParent(LLVMGetInsertBlock(lg.builder));
            LLVMBasicBlockRef err_bb = LLVMAppendBasicBlockInContext(lg.ctx, fn, "eff_err");
            LLVMBasicBlockRef cont_bb = LLVMAppendBasicBlockInContext(lg.ctx, fn, "eff_cont");
            LLVMValueRef tag = LLVMBuildLoad2(lg.builder, lg.i64_ty,
                                              llv_eff_gep(0, "tagp"), "tag");
            LLVMValueRef bad = LLVMBuildICmp(lg.builder, LLVMIntNE, tag,
                                             LLVMConstInt(lg.i64_ty, 0, 0), "bad");
            LLVMBuildCondBr(lg.builder, bad, err_bb, cont_bb);
            LLVMPositionBuilderAtEnd(lg.builder, err_bb);
            if (lg.rc) llv_lrc_release_all(-1);
            llv_h_ret_zero();
            LLVMPositionBuilderAtEnd(lg.builder, cont_bb);
            return v;
        }
        case NODE_CATCH: {
            /* M20: flatten веригата — base стойност → tag → по веригата
             * handler блокове → phi; последният клон propagate-ва */
            int any_payload = 0;
            {
                Node *scan = n;
                while (scan && scan->kind == NODE_CATCH) {
                    Type *et = scan->catch_expr ? scan->catch_expr->type : NULL;
                    Type *pl = type_effect_payload(et, scan->catch_effect);
                    if (pl) any_payload = 1;
                    scan = scan->catch_expr;
                }
            }
            if (!any_payload) {
                Node *base = n;
                while (base && base->kind == NODE_CATCH) base = base->catch_expr;
                return llv_emit_expr_llvm(base);
            }
            Node *base = n;
            while (base && base->kind == NODE_CATCH) base = base->catch_expr;
            lg.eff_depth++;
            LLVMValueRef v = llv_emit_expr_llvm(base);
            lg.eff_depth--;
            LLVMValueRef fn = LLVMGetBasicBlockParent(LLVMGetInsertBlock(lg.builder));
            LLVMValueRef tag = LLVMBuildLoad2(lg.builder, lg.i64_ty,
                                              llv_eff_gep(0, "tagp"), "tag");
            LLVMBuildStore(lg.builder, LLVMConstInt(lg.i64_ty, 0, 0),
                           llv_eff_gep(0, "tagp"));
            /* събери payload catch-овете */
            int nhandlers = 0;
            Node *scans[64];
            {
                Node *scan = n;
                while (scan && scan->kind == NODE_CATCH && nhandlers < 64) {
                    Type *et = scan->catch_expr ? scan->catch_expr->type : NULL;
                    Type *pl = type_effect_payload(et, scan->catch_effect);
                    if (pl) scans[nhandlers++] = scan;
                    scan = scan->catch_expr;
                }
            }
            /* блокове: decision[0..n-1] + handler[0..n-1] + prop + merge */
            LLVMBasicBlockRef dec[65] = {0}, hbs[65] = {0};
            LLVMValueRef hvs[65] = {0};
            LLVMBasicBlockRef prop_bb = LLVMAppendBasicBlockInContext(lg.ctx, fn, "eff_prop");
            LLVMBasicBlockRef merge_bb = LLVMAppendBasicBlockInContext(lg.ctx, fn, "eff_merge");
            for (int i = 0; i < nhandlers; i++) {
                dec[i] = LLVMAppendBasicBlockInContext(lg.ctx, fn, "eff_dec");
                hbs[i] = LLVMAppendBasicBlockInContext(lg.ctx, fn, "eff_h");
            }
            LLVMBasicBlockRef cur_bb = LLVMGetInsertBlock(lg.builder);
            LLVMBuildCondBr(lg.builder,
                LLVMBuildICmp(lg.builder, LLVMIntEQ, tag,
                              LLVMConstInt(lg.i64_ty, 0, 0), "ok"),
                merge_bb, dec[0]);
            for (int i = 0; i < nhandlers; i++) {
                Node *scan = scans[i];
                Type *et = scan->catch_expr ? scan->catch_expr->type : NULL;
                Type *pl = type_effect_payload(et, scan->catch_effect);
                int tagv = llv_llvm_eff_tag(scan->catch_effect);
                LLVMPositionBuilderAtEnd(lg.builder, dec[i]);
                LLVMBuildCondBr(lg.builder,
                    LLVMBuildICmp(lg.builder, LLVMIntEQ, tag,
                                  LLVMConstInt(lg.i64_ty, (unsigned long long)tagv, 0), "eq"),
                    hbs[i], i + 1 < nhandlers ? dec[i + 1] : prop_bb);
                LLVMPositionBuilderAtEnd(lg.builder, hbs[i]);
                int saved_vars = lg_st.count;
                if (scan->catch_binding) {
                    unsigned fidx = llv_eff_field_of(pl);
                    LLVMValueRef pv = LLVMBuildLoad2(lg.builder,
                        llv_llvm_type_resolved(pl), llv_eff_gep(fidx, "pl"), "pl");
                    LLVMValueRef pa = llv_entry_alloca(llv_llvm_type_resolved(pl), "pld");
                    LLVMBuildStore(lg.builder, pv, pa);
                    llv_st_define(scan->catch_binding, pa);
                }
                hvs[i] = scan->catch_handler->kind == NODE_BLOCK
                    ? llv_emit_block_value_llvm(scan->catch_handler)
                    : llv_emit_expr_llvm(scan->catch_handler);
                if (scan->catch_binding) lg_st.count = saved_vars;
                if (!LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(lg.builder)))
                    LLVMBuildBr(lg.builder, merge_bb);
            }
            LLVMPositionBuilderAtEnd(lg.builder, prop_bb);
            /* възстанови тага за външния контекст (като C бекенда) */
            LLVMBuildStore(lg.builder, tag, llv_eff_gep(0, "tagp"));
            if (lg.rc) llv_lrc_release_all(-1);
            llv_h_ret_zero();
            LLVMPositionBuilderAtEnd(lg.builder, merge_bb);
            if (n->type && llv_llvm_type_resolved(n->type) == lg.void_ty) {
                LLVMBuildRetVoid(lg.builder);
                return NULL;
            }
            LLVMTypeRef rty = n->type ? llv_llvm_type_resolved(n->type) : lg.i64_ty;
            LLVMValueRef phi = LLVMBuildPhi(lg.builder, rty, "effv");
            LLVMAddIncoming(phi, &v, &cur_bb, 1);
            /* входящи само от блокове, които действително достигат merge
             * (хендлър с terminator — напр. return вътре — не стига) */
            for (int i = 0; i < nhandlers; i++) {
                /* входящо само ако хендлърът реално завършва с br към merge
                 * (br-ът е terminator; ret/друго значи „не достига merge“) */
                LLVMValueRef t = LLVMGetBasicBlockTerminator(hbs[i]);
                if (!t || !LLVMIsABranchInst(t) ||
                    LLVMGetOperand(t, 0) != (LLVMValueRef)merge_bb) continue;
                if (!hvs[i]) hvs[i] = LLVMConstNull(rty);
                LLVMAddIncoming(phi, &hvs[i], &hbs[i], 1);
            }
            return phi;
        }
        case NODE_RAISE: {
            /* M20: задай слота и излез (като `?` / llv_h_ret_zero).
             * raise_dead е недостижим блок, за да могат извикващите
             * emit_expr да продължат без инструкция след terminator.
             * RC: release на локалите преди изхода (паритет с C); heap
             * payload, който не е fresh, се retain-ва преди release-ите —
             * fresh payload се move-ва в слота (като в codegen_c). */
            int tagv = llv_llvm_eff_tag(n->raise_effect);
            LLVMBuildStore(lg.builder, LLVMConstInt(lg.i64_ty, (unsigned long long)tagv, 0),
                           llv_eff_gep(0, "tagp"));
            if (n->raise_payload && n->raise_payload->type) {
                Node *p = n->raise_payload;
                LLVMValueRef pv = llv_emit_expr_llvm(p);
                LLVMBuildStore(lg.builder, llv_coerce(pv, llv_llvm_type_resolved(p->type)),
                               llv_eff_gep(llv_eff_field_of(p->type), "pv"));
                if (lg.rc) {
                    int ptag = llv_lrc_heap_tag(p->type);
                    if (ptag && !llv_lrc_tmp_fresh(p) && !llv_lrc_is_enum_ctor(p) &&
                        p->kind != NODE_STRUCT_LIT)
                        llv_lrc_emit_retain_val(ptag, p->type, NULL, pv);
                }
            }
            if (lg.rc) llv_lrc_release_all(-1);
            llv_h_ret_zero();
            {
                LLVMValueRef fn = LLVMGetBasicBlockParent(LLVMGetInsertBlock(lg.builder));
                LLVMBasicBlockRef dead = LLVMAppendBasicBlockInContext(lg.ctx, fn, "raise_dead");
                LLVMPositionBuilderAtEnd(lg.builder, dead);
            }
            return LLVMConstInt(lg.i64_ty, 0, 0);
        }
        case NODE_TO_STR: {
            /* interpolation: convert inner expr to a string by its type */
            Type *et = n->to_str_expr ? n->to_str_expr->type : NULL;
            TypeKind ek = et ? et->kind : TYPE_STR;
            LLVMValueRef v = llv_emit_expr_llvm(n->to_str_expr);
            if (ek == TYPE_STR) return v;
            if (ek == TYPE_BOOL) {
                /* низовете трябва да са i8* (като NODE_STR_LIT) — суровите
                 * глобали са [N x i8]* и select с различни N е невалиден */
                LLVMValueRef t = LLVMBuildGlobalStringPtr(lg.builder, "true", "tt");
                LLVMValueRef f = LLVMBuildGlobalStringPtr(lg.builder, "false", "ff");
                return LLVMBuildSelect(lg.builder, v, t, f, "bs");
            }
            if (ek == TYPE_F64)
                return llv_h_call(llv_baga_rt("baga_f64_to_str"), &v, 1, "f2s");
            return llv_h_call(llv_baga_rt("baga_i64_to_str"), &v, 1, "i2s");
        }
        case NODE_RANGE:    llv_llvm_unsupported("диапазон (a..b) извън for"); break;
        case NODE_LAMBDA: {
            /* L5: env struct с копия на captures + wrapper + cell2 handle */
            int nc = n->captures.len;
            LLVMTypeRef env_ty = NULL;
            if (nc > 0) {
                LLVMTypeRef *elems = malloc(sizeof(LLVMTypeRef) * (size_t)nc);
                for (int i = 0; i < nc; i++)
                    elems[i] = llv_llvm_type_resolved(n->captures.data[i]->type);
                env_ty = LLVMStructTypeInContext(lg.ctx, elems, (unsigned)nc, 0);
                free(elems);
            }
            LLVMValueRef wrap = emit_lambda_wrapper(n, env_ty);
            LLVMValueRef envp = LLVMConstNull(lg.ptr_ty);
            if (nc > 0) {
                LLVMValueRef sz = LLVMSizeOf(env_ty);
                LLVMValueRef ma[] = { sz };
                envp = llv_h_call(llv_rt_malloc(), ma, 1, "env");
                LLVMValueRef typed = LLVMBuildBitCast(lg.builder, envp,
                    LLVMPointerType(env_ty, 0), "envt");
                for (int i = 0; i < nc; i++) {
                    Node *cap = n->captures.data[i];
                    LLVMValueRef alloca = llv_st_lookup(cap->param_name);
                    if (!alloca) llv_llvm_unsupported("лямбда: capture не е локална");
                    LLVMValueRef v = LLVMBuildLoad2(lg.builder,
                        LLVMGetAllocatedType(alloca), alloca, "cap");
                    LLVMValueRef gep = LLVMBuildStructGEP2(lg.builder, env_ty,
                        typed, (unsigned)i, "cp");
                    LLVMBuildStore(lg.builder, v, gep);
                }
            }
            return closure_handle(wrap, envp);
        }
        case NODE_BLOCK:
            /* блок като стойност (`let x = { … }`) — същата implicit-return
             * семантика като catch handler */
            return llv_emit_block_value_llvm(n);
        default:            llv_llvm_unsupported_node(n); break;
    }
    return NULL; /* unreachable */
}

/* Стойност на блок-хендлър на catch: statement-и + последният е стойността
 * (implicit return семантика, като C бекенда) */
LLVMValueRef llv_emit_block_value_llvm(Node *block) {
    if (!block || block->kind != NODE_BLOCK) llv_llvm_unsupported("catch хендлър без тяло");
    llv_st_push();
    /* RC: handler-ът е свой scope — heap let-овете в него се release-ват при
     * изхода му, ДОКАТО llv_st_lookup още resolve-ва (llv_lrc_pop_scope преди llv_st_pop).
     * Без това те се track-ваха във външния scope и при release-а му
     * llv_st_lookup връщаше NULL — тих leak. */
    llv_lrc_push_scope(0);
    int lrc_top = lg.rc ? lg.lrc_scopes[lg.lrc_depth - 1].top : 0;
    LLVMValueRef val = NULL;
    lg.lrc_branch_depth++;   /* handler-ът се изпълнява само при raise */
    for (int i = 0; i < block->stmts.len; i++) {
        if (LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(lg.builder))) break;
        Node *s = block->stmts.data[i];
        if (i == block->stmts.len - 1) {
            /* последният stmt е СТОЙНОСТТА на handler-а (EXPR_STMT или
             * return <израз> — и двата са стойност, не реален ret) */
            Node *ve = NULL;
            if (s->kind == NODE_EXPR_STMT) ve = s->expr;
            else if (s->kind == NODE_RETURN && s->ret_val) ve = s->ret_val;
            if (ve) {
                val = llv_emit_expr_llvm(ve);
                /* RC: move семантика — стойността escape-ва към phi-то на
                 * catch израза; handler-локал IDENT се маркира dead, за да не
                 * бъде release-нат от pop-а. Външен IDENT/параметър е
                 * borrowed копие — не се пипа (C parity). */
                if (lg.rc && ve->kind == NODE_IDENT) {
                    int vi = llv_lrc_find(ve->name);
                    if (vi >= lrc_top && !lg.lrc_locals[vi].is_param)
                        lg.lrc_locals[vi].dead = 1;
                }
            } else {
                llv_emit_stmt_llvm(s, NULL, NULL);
            }
        } else llv_emit_stmt_llvm(s, NULL, NULL);
    }
    lg.lrc_branch_depth--;
    /* ако блокът има terminator (реален return вътре мина през
     * llv_lrc_release_all), pop-ът само прибира записите — без дублиране */
    llv_lrc_pop_scope();
    llv_st_pop();
    return val;
}

#endif /* BAGA_LLVM */
