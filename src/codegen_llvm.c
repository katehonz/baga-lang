#include "baga.h"

#ifdef BAGA_LLVM

#include <llvm-c/Core.h>
#include <llvm-c/Analysis.h>

/* ============================================================
 *  LLVM IR Codegen — Фаза 3
 *
 *  Генерира LLVM IR директно от AST-то.
 *  Поддръжка: i64/i32/f64/bool/str(литерали+print), binary/unary
 *  изрази, let/присвояване, if/else, while, for (a..b) с
 *  break/continue, match върху i64, enum варианти, предекларирани
 *  функции, contract wrapper (requires/ensures) като в codegen_c.
 *
 *  Принцип: НИКАКВИ тихи стойности — всеки неподдържан конструкт
 *  е compile-time грешка (llvm_unsupported), не константа 0.
 * ============================================================ */

typedef struct {
    LLVMContextRef ctx;
    LLVMModuleRef mod;
    LLVMBuilderRef builder;
    LLVMTypeRef i64_ty;
    LLVMTypeRef i32_ty;
    LLVMTypeRef i1_ty;
    LLVMTypeRef double_ty;
    LLVMTypeRef void_ty;
    LLVMTypeRef ptr_ty;
    LLVMValueRef printf_fn;
    LLVMValueRef fprintf_fn;
    LLVMValueRef exit_fn;
    LLVMValueRef stderr_global;
    LLVMTypeRef cur_ret_ty;   /* върнатият тип на текущата функция */
    Node *program;            /* за enum варианти и spec-ове */
    int tmp_counter;
} LLVMCodegen;

static LLVMCodegen lg;

/* ---- честен отказ ---- */

static void llvm_unsupported(const char *what) {
    fprintf(stderr, "baga: LLVM backend: неподдържан конструкт '%s'\n", what);
    exit(1);
}

static void llvm_unsupported_node(Node *n) {
    char buf[64];
    snprintf(buf, sizeof buf, "AST възел #%d", n ? (int)n->kind : -1);
    llvm_unsupported(buf);
}

/* ---- Type mapping ---- */

static LLVMTypeRef llvm_type(Node *ty);

static LLVMTypeRef llvm_type_resolved(Type *ty) {
    if (!ty) return lg.i64_ty;
    switch (ty->kind) {
        case TYPE_I64: return lg.i64_ty;
        case TYPE_I32: return lg.i32_ty;
        case TYPE_F64: return lg.double_ty;
        case TYPE_BOOL: return lg.i1_ty;
        case TYPE_STR: return lg.ptr_ty;
        case TYPE_VOID: return lg.void_ty;
        case TYPE_STRUCT: llvm_unsupported("структури"); break;
        case TYPE_VEC:    llvm_unsupported("Vec"); break;
        case TYPE_ARRAY:  llvm_unsupported("масиви"); break;
        case TYPE_REF:    llvm_unsupported("референции"); break;
        default:          llvm_unsupported("неизвестен тип"); break;
    }
    return NULL; /* unreachable */
}

static LLVMTypeRef llvm_type(Node *ty) {
    if (!ty) return lg.void_ty;
    if (ty->kind == NODE_TYPE) {
        if (strcmp(ty->type_name, "i64") == 0) return lg.i64_ty;
        if (strcmp(ty->type_name, "i32") == 0) return lg.i32_ty;
        if (strcmp(ty->type_name, "f64") == 0) return lg.double_ty;
        if (strcmp(ty->type_name, "bool") == 0) return lg.i1_ty;
        if (strcmp(ty->type_name, "str") == 0) return lg.ptr_ty;
        if (strcmp(ty->type_name, "void") == 0) return lg.void_ty;
        if (strcmp(ty->type_name, "Vec") == 0) llvm_unsupported("Vec");
        char buf[256];
        snprintf(buf, sizeof buf, "потребителски тип '%s'", ty->type_name);
        llvm_unsupported(buf);
    }
    if (ty->kind == NODE_TYPE_EFFECT) llvm_unsupported("ефекти (!E, ?, catch)");
    if (ty->kind == NODE_TYPE_REF) llvm_unsupported("референции (&T)");
    if (ty->kind == NODE_TYPE_ARRAY) llvm_unsupported("масиви ([T])");
    llvm_unsupported_node(ty);
    return NULL; /* unreachable */
}

/* ---- Name mangling ---- */

static char *llvm_mangle(const char *name) {
    size_t len = strlen(name);
    char *buf = malloc(2 + len * 4 + 1);
    char *o = buf;
    *o++ = 'b'; *o++ = '_';
    for (const unsigned char *p = (const unsigned char *)name; *p; p++) {
        if ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
            (*p >= '0' && *p <= '9') || *p == '_') {
            *o++ = (char)*p;
        } else {
            o += sprintf(o, "_%d", *p);
        }
    }
    *o = '\0';
    return buf;
}

/* ---- Temp register names ---- */

static char *tmp_name(void) {
    char *buf = malloc(16);
    sprintf(buf, "t%d", lg.tmp_counter++);
    return buf;
}

/* ---- Symbol table: име → alloca ---- */

#define LLVM_MAX_VARS 256
typedef struct { char *name; LLVMValueRef alloca; } LLVMVar;
typedef struct {
    LLVMVar vars[LLVM_MAX_VARS];
    int count;
    int scope_marks[64];
    int depth;
} LLVMSymtab;
static LLVMSymtab lg_st;

static void st_reset(void) { lg_st.count = 0; lg_st.depth = 0; }
static void st_push(void) { lg_st.scope_marks[lg_st.depth++] = lg_st.count; }
static void st_pop(void)  { lg_st.count = lg_st.scope_marks[--lg_st.depth]; }
static void st_define(const char *name, LLVMValueRef alloca) {
    if (lg_st.count >= LLVM_MAX_VARS) llvm_unsupported("твърде много локални променливи");
    lg_st.vars[lg_st.count].name = (char *)name;
    lg_st.vars[lg_st.count].alloca = alloca;
    lg_st.count++;
}
static LLVMValueRef st_lookup(const char *name) {
    for (int i = lg_st.count - 1; i >= 0; i--)
        if (strcmp(lg_st.vars[i].name, name) == 0) return lg_st.vars[i].alloca;
    return NULL;
}

/* ---- Type coercion (огледало на C аритметичните преобразувания) ---- */

static LLVMValueRef coerce(LLVMValueRef v, LLVMTypeRef target) {
    LLVMTypeRef t = LLVMTypeOf(v);
    if (t == target) return v;
    LLVMTypeKind tk = LLVMGetTypeKind(t);
    LLVMTypeKind gk = LLVMGetTypeKind(target);
    char *name = tmp_name();
    LLVMValueRef r = NULL;
    if (gk == LLVMDoubleTypeKind && tk == LLVMIntegerTypeKind) {
        r = LLVMGetIntTypeWidth(t) == 1
          ? LLVMBuildUIToFP(lg.builder, v, target, name)
          : LLVMBuildSIToFP(lg.builder, v, target, name);
    } else if (tk == LLVMDoubleTypeKind && gk == LLVMIntegerTypeKind) {
        r = LLVMBuildFPToSI(lg.builder, v, target, name);
    } else if (tk == LLVMIntegerTypeKind && gk == LLVMIntegerTypeKind) {
        unsigned wt = LLVMGetIntTypeWidth(t);
        unsigned wg = LLVMGetIntTypeWidth(target);
        if (wt < wg)
            r = wt == 1 ? LLVMBuildZExt(lg.builder, v, target, name)
                        : LLVMBuildSExt(lg.builder, v, target, name);
        else
            r = LLVMBuildTrunc(lg.builder, v, target, name);
    }
    free(name);
    if (!r) llvm_unsupported("преобразуване между типовете");
    return r;
}

/* C truthiness: i1 → както е; цяло число → != 0 */
static LLVMValueRef to_bool(LLVMValueRef v) {
    LLVMTypeRef t = LLVMTypeOf(v);
    if (t == lg.i1_ty) return v;
    if (LLVMGetTypeKind(t) == LLVMIntegerTypeKind) {
        char *name = tmp_name();
        LLVMValueRef r = LLVMBuildICmp(lg.builder, LLVMIntNE, v,
                                       LLVMConstInt(t, 0, 0), name);
        free(name);
        return r;
    }
    llvm_unsupported("булев контекст върху не-числена стойност");
    return NULL; /* unreachable */
}

/* ---- Expression emission ---- */

static LLVMValueRef emit_expr_llvm(Node *n);

static LLVMValueRef emit_binop_llvm(BinOp op, LLVMValueRef left, LLVMValueRef right) {
    char *name = tmp_name();
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
            if (isdbl) llvm_unsupported("побитово & върху f64");
            result = LLVMBuildAnd(lg.builder, left, right, name); break;
        case OP_BIT_OR:
            if (isdbl) llvm_unsupported("побитово | върху f64");
            result = LLVMBuildOr(lg.builder, left, right, name); break;
        case OP_BIT_XOR:
            if (isdbl) llvm_unsupported("побитово ^ върху f64");
            result = LLVMBuildXor(lg.builder, left, right, name); break;
        case OP_LSHIFT:
            if (isdbl) llvm_unsupported("<< върху f64");
            result = LLVMBuildShl(lg.builder, left, right, name); break;
        case OP_RSHIFT:
            if (isdbl) llvm_unsupported(">> върху f64");
            result = LLVMBuildAShr(lg.builder, left, right, name); break;
        default:     llvm_unsupported("непознат бинарен оператор"); break;
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
        LLVMBuildCall2(lg.builder, LLVMGetElementType(LLVMTypeOf(lg.printf_fn)),
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
            val = emit_expr_llvm(arg);
            if (!val) llvm_unsupported("print върху void стойност");
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
                char *name = tmp_name();
                LLVMValueRef sel = LLVMBuildSelect(lg.builder, val, ts, fs, name);
                free(name);
                fmt_str = "%s\n";
                extra[nextra++] = sel;
            } else if (LLVMGetTypeKind(t) == LLVMIntegerTypeKind) {
                fmt_str = "%lld\n";
                extra[nextra++] = coerce(val, lg.i64_ty);
            } else {
                llvm_unsupported("print върху този тип");
                return; /* unreachable */
            }
        }

        LLVMValueRef fmt = LLVMBuildGlobalStringPtr(lg.builder, fmt_str, "fmt");
        LLVMValueRef args[3];
        args[0] = fmt;
        for (int j = 0; j < nextra; j++) args[1 + j] = extra[j];
        LLVMBuildCall2(lg.builder, LLVMGetElementType(LLVMTypeOf(lg.printf_fn)),
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
        llvm_unsupported("match клон, който не е блок");
    for (int j = 0; j < body->stmts.len; j++) {
        Node *s = body->stmts.data[j];
        if (s->kind == NODE_RETURN) {
            if (s->ret_val && res_alloca) {
                LLVMValueRef v = emit_expr_llvm(s->ret_val);
                if (!v) llvm_unsupported("print в match клон");
                v = coerce(v, LLVMGetAllocatedType(res_alloca));
                LLVMBuildStore(lg.builder, v, res_alloca);
            }
            LLVMBuildBr(lg.builder, merge_bb);
            return;
        }
        if (s->kind == NODE_EXPR_STMT) {
            emit_expr_llvm(s->expr);
            continue;
        }
        llvm_unsupported("оператор в match клон (само изрази)");
    }
}

static LLVMValueRef emit_match_llvm(Node *n) {
    LLVMValueRef mv = emit_expr_llvm(n->match_expr);
    if (!mv || LLVMGetTypeKind(LLVMTypeOf(mv)) != LLVMIntegerTypeKind)
        llvm_unsupported("match върху не-целочислена стойност");
    mv = coerce(mv, lg.i64_ty);

    LLVMTypeRef res_ty = n->type ? llvm_type_resolved(n->type) : lg.i64_ty;
    LLVMValueRef res_alloca = NULL;
    if (res_ty != lg.void_ty) {
        res_alloca = LLVMBuildAlloca(lg.builder, res_ty, "match_res");
        /* codegen_c инициализира _mr = 0 */
        LLVMBuildStore(lg.builder, LLVMConstNull(res_ty), res_alloca);
    }

    LLVMValueRef fn = LLVMGetBasicBlockParent(LLVMGetInsertBlock(lg.builder));
    LLVMBasicBlockRef merge_bb = LLVMAppendBasicBlockInContext(lg.ctx, fn, "match_end");

    for (int i = 0; i < n->match_arms.len; i++) {
        Node *arm = n->match_arms.data[i];
        if (arm->arm_pattern) {
            LLVMValueRef pat = emit_expr_llvm(arm->arm_pattern);
            if (!pat) llvm_unsupported("print в match pattern");
            pat = coerce(pat, lg.i64_ty);
            char *name = tmp_name();
            LLVMValueRef cond = LLVMBuildICmp(lg.builder, LLVMIntEQ, mv, pat, name);
            free(name);
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

    LLVMPositionBuilderAtEnd(lg.builder, merge_bb);
    if (!res_alloca) return NULL;
    char *name = tmp_name();
    LLVMValueRef r = LLVMBuildLoad2(lg.builder, res_ty, res_alloca, name);
    free(name);
    return r;
}

static LLVMValueRef emit_expr_llvm(Node *n) {
    if (!n) llvm_unsupported("празен израз");

    switch (n->kind) {
        case NODE_INT_LIT:
            return LLVMConstInt(lg.i64_ty, (unsigned long long)n->int_val, 1);

        case NODE_FLOAT_LIT: {
            /* codegen_c печата литерала с "%g" в C кода — същата стойност
             * (round-trip през %g), иначе изходът се разминава в последната цифра */
            char buf[64];
            snprintf(buf, sizeof buf, "%g", n->float_val);
            return LLVMConstReal(lg.double_ty, strtod(buf, NULL));
        }

        case NODE_BOOL_LIT:
            return LLVMConstInt(lg.i1_ty, n->bool_val, 0);

        case NODE_STR_LIT:
            return LLVMBuildGlobalStringPtr(lg.builder, n->str_val, "str");

        case NODE_IDENT: {
            LLVMValueRef alloca = st_lookup(n->name);
            if (alloca) {
                char *name = tmp_name();
                LLVMValueRef v = LLVMBuildLoad2(lg.builder,
                    LLVMGetAllocatedType(alloca), alloca, name);
                free(name);
                return v;
            }
            int variant = find_enum_variant(n->name);
            if (variant >= 0)
                return LLVMConstInt(lg.i64_ty, (unsigned long long)variant, 0);
            char buf[256];
            snprintf(buf, sizeof buf, "недефинирано име '%s'", n->name);
            llvm_unsupported(buf);
            return NULL; /* unreachable */
        }

        case NODE_BINARY: {
            /* && / || върху bool (i1); C backend-ът ползва && / || —
             * за чисти операнди резултатът е същият */
            if (n->bin_op == OP_AND || n->bin_op == OP_OR) {
                LLVMValueRef l = to_bool(emit_expr_llvm(n->left));
                LLVMValueRef r = to_bool(emit_expr_llvm(n->right));
                char *name = tmp_name();
                LLVMValueRef v = n->bin_op == OP_AND
                    ? LLVMBuildAnd(lg.builder, l, r, name)
                    : LLVMBuildOr(lg.builder, l, r, name);
                free(name);
                return v;
            }
            /* str == str е чрез strcmp в C backend-а — извън обхвата тук */
            if ((n->bin_op == OP_EQ || n->bin_op == OP_NEQ) &&
                n->left->type && n->right->type &&
                n->left->type->kind == TYPE_STR && n->right->type->kind == TYPE_STR)
                llvm_unsupported("сравнение на низове");
            LLVMValueRef left = emit_expr_llvm(n->left);
            LLVMValueRef right = emit_expr_llvm(n->right);
            if (!left || !right) llvm_unsupported("print в аритметичен израз");
            /* f64 промоция: ако някой операнд е f64, и двата стават f64
             * (като C: int64_t + double → double) */
            int isdbl = LLVMGetTypeKind(LLVMTypeOf(left)) == LLVMDoubleTypeKind ||
                        LLVMGetTypeKind(LLVMTypeOf(right)) == LLVMDoubleTypeKind;
            if (isdbl) {
                left = coerce(left, lg.double_ty);
                right = coerce(right, lg.double_ty);
            } else if (LLVMTypeOf(left) != LLVMTypeOf(right)) {
                /* смесени целочислени ширини → по-широкият тип */
                unsigned wl = LLVMGetIntTypeWidth(LLVMTypeOf(left));
                unsigned wr = LLVMGetIntTypeWidth(LLVMTypeOf(right));
                LLVMTypeRef wide = wl >= wr ? LLVMTypeOf(left) : LLVMTypeOf(right);
                left = coerce(left, wide);
                right = coerce(right, wide);
            }
            return emit_binop_llvm(n->bin_op, left, right);
        }

        case NODE_UNARY: {
            LLVMValueRef v = emit_expr_llvm(n->operand);
            if (!v) llvm_unsupported("print в унарен израз");
            char *name = tmp_name();
            LLVMValueRef r = NULL;
            switch (n->un_op) {
                case UOP_NEG:
                    r = LLVMGetTypeKind(LLVMTypeOf(v)) == LLVMDoubleTypeKind
                      ? LLVMBuildFNeg(lg.builder, v, name)
                      : LLVMBuildNeg(lg.builder, v, name);
                    break;
                case UOP_NOT:
                    if (LLVMTypeOf(v) != lg.i1_ty)
                        llvm_unsupported("! върху не-булев израз");
                    r = LLVMBuildNot(lg.builder, v, name);
                    break;
                case UOP_REF:   llvm_unsupported("референция (&x)"); break;
                case UOP_DEREF: llvm_unsupported("дереференциране (*x)"); break;
            }
            free(name);
            return r;
        }

        case NODE_CALL: {
            if (is_print_call_llvm(n)) {
                emit_print_llvm(n);
                return NULL; /* print е void */
            }
            if (n->callee->kind != NODE_IDENT)
                llvm_unsupported("повикване през израз (не име)");
            char *m = llvm_mangle(n->callee->name);
            LLVMValueRef fn = LLVMGetNamedFunction(lg.mod, m);
            free(m);
            if (!fn) {
                char buf[256];
                snprintf(buf, sizeof buf, "вградена функция '%s'", n->callee->name);
                llvm_unsupported(buf);
            }
            int nargs = n->args.len;
            LLVMValueRef *args = malloc(sizeof(LLVMValueRef) * (size_t)(nargs > 0 ? nargs : 1));
            for (int i = 0; i < nargs; i++) {
                LLVMValueRef a = emit_expr_llvm(n->args.data[i]);
                if (!a) llvm_unsupported("print като аргумент");
                args[i] = coerce(a, LLVMTypeOf(LLVMGetParam(fn, (unsigned)i)));
            }
            LLVMTypeRef fn_ty = LLVMGetElementType(LLVMTypeOf(fn));
            int is_void = LLVMGetReturnType(fn_ty) == lg.void_ty;
            char *name = is_void ? NULL : tmp_name();
            LLVMValueRef result = LLVMBuildCall2(lg.builder, fn_ty, fn, args,
                                                 (unsigned)nargs, is_void ? "" : name);
            free(name);
            free(args);
            return result;
        }

        case NODE_MATCH:
            return emit_match_llvm(n);

        case NODE_ASSIGN: {
            if (n->assign_target->kind != NODE_IDENT)
                llvm_unsupported("присвояване на поле/индекс");
            LLVMValueRef alloca = st_lookup(n->assign_target->name);
            if (!alloca) {
                char buf[256];
                snprintf(buf, sizeof buf, "присвояване на недефинирано име '%s'",
                         n->assign_target->name);
                llvm_unsupported(buf);
            }
            LLVMValueRef v = emit_expr_llvm(n->assign_val);
            if (!v) llvm_unsupported("print в присвояване");
            v = coerce(v, LLVMGetAllocatedType(alloca));
            LLVMBuildStore(lg.builder, v, alloca);
            return v;
        }

        case NODE_IF:       llvm_unsupported("if като израз"); break;
        case NODE_INDEX:    llvm_unsupported("индексиране"); break;
        case NODE_FIELD:    llvm_unsupported("достъп до поле (struct)"); break;
        case NODE_STRUCT_LIT: llvm_unsupported("struct литерал"); break;
        case NODE_TRY:      llvm_unsupported("ефекти (?)"); break;
        case NODE_CATCH:    llvm_unsupported("catch"); break;
        case NODE_RANGE:    llvm_unsupported("диапазон (a..b) извън for"); break;
        default:            llvm_unsupported_node(n); break;
    }
    return NULL; /* unreachable */
}

/* ---- Statement emission ---- */

static void emit_stmt_llvm(Node *n, LLVMBasicBlockRef break_bb, LLVMBasicBlockRef cont_bb);

static void emit_block_llvm(Node *block, LLVMBasicBlockRef break_bb, LLVMBasicBlockRef cont_bb) {
    if (!block) return;
    if (block->kind != NODE_BLOCK)
        llvm_unsupported("тяло, което не е блок");
    st_push();
    for (int i = 0; i < block->stmts.len; i++) {
        /* мъртъв код след terminator — както gcc след return */
        if (LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(lg.builder)))
            break;
        emit_stmt_llvm(block->stmts.data[i], break_bb, cont_bb);
    }
    st_pop();
}

static void emit_stmt_llvm(Node *n, LLVMBasicBlockRef break_bb, LLVMBasicBlockRef cont_bb) {
    if (!n) return;

    switch (n->kind) {
        case NODE_LET: {
            LLVMTypeRef ty;
            if (n->let_type) ty = llvm_type(n->let_type);
            else if (n->let_init && n->let_init->type) ty = llvm_type_resolved(n->let_init->type);
            else ty = lg.i64_ty;
            char *m = llvm_mangle(n->let_name);
            LLVMValueRef alloca = LLVMBuildAlloca(lg.builder, ty, m);
            free(m);
            if (n->let_init) {
                LLVMValueRef val = emit_expr_llvm(n->let_init);
                if (!val) llvm_unsupported("print като стойност на let");
                LLVMBuildStore(lg.builder, coerce(val, ty), alloca);
            }
            st_define(n->let_name, alloca);
            break;
        }

        case NODE_RETURN: {
            if (n->ret_val) {
                LLVMValueRef val = emit_expr_llvm(n->ret_val);
                if (!val) llvm_unsupported("print като return стойност");
                LLVMBuildRet(lg.builder, coerce(val, lg.cur_ret_ty));
            } else {
                if (lg.cur_ret_ty != lg.void_ty)
                    llvm_unsupported("return без стойност в не-void функция");
                LLVMBuildRetVoid(lg.builder);
            }
            break;
        }

        case NODE_IF: {
            LLVMValueRef cond = to_bool(emit_expr_llvm(n->cond));
            LLVMValueRef fn = LLVMGetBasicBlockParent(LLVMGetInsertBlock(lg.builder));
            LLVMBasicBlockRef then_bb = LLVMAppendBasicBlockInContext(lg.ctx, fn, "then");
            LLVMBasicBlockRef else_bb = LLVMAppendBasicBlockInContext(lg.ctx, fn, "else");
            LLVMBasicBlockRef merge_bb = LLVMAppendBasicBlockInContext(lg.ctx, fn, "merge");

            LLVMBuildCondBr(lg.builder, cond, then_bb, else_bb);

            LLVMPositionBuilderAtEnd(lg.builder, then_bb);
            emit_block_llvm(n->then_br, break_bb, cont_bb);
            if (!LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(lg.builder)))
                LLVMBuildBr(lg.builder, merge_bb);

            LLVMPositionBuilderAtEnd(lg.builder, else_bb);
            if (n->else_br) emit_block_llvm(n->else_br, break_bb, cont_bb);
            if (!LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(lg.builder)))
                LLVMBuildBr(lg.builder, merge_bb);

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
            LLVMValueRef cond = to_bool(emit_expr_llvm(n->while_cond));
            LLVMBuildCondBr(lg.builder, cond, body_bb, end_bb);

            LLVMPositionBuilderAtEnd(lg.builder, body_bb);
            emit_block_llvm(n->while_body, end_bb, cond_bb);
            if (!LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(lg.builder)))
                LLVMBuildBr(lg.builder, cond_bb);

            LLVMPositionBuilderAtEnd(lg.builder, end_bb);
            break;
        }

        case NODE_FOR: {
            /* for x in lo..hi { } → x = lo; while x < hi { body; x++ }
             * hi се преизчислява на всяка итерация, както в codegen_c */
            if (!n->for_iter || n->for_iter->kind != NODE_RANGE)
                llvm_unsupported("for без диапазон (a..b)");
            LLVMValueRef lo = emit_expr_llvm(n->for_iter->range_lo);
            if (!lo || LLVMGetTypeKind(LLVMTypeOf(lo)) != LLVMIntegerTypeKind)
                llvm_unsupported("for с не-целочислен диапазон");
            lo = coerce(lo, lg.i64_ty);

            LLVMValueRef fn = LLVMGetBasicBlockParent(LLVMGetInsertBlock(lg.builder));
            char *m = llvm_mangle(n->for_var);
            LLVMValueRef var = LLVMBuildAlloca(lg.builder, lg.i64_ty, m);
            free(m);
            LLVMBuildStore(lg.builder, lo, var);

            LLVMBasicBlockRef cond_bb = LLVMAppendBasicBlockInContext(lg.ctx, fn, "for_cond");
            LLVMBasicBlockRef body_bb = LLVMAppendBasicBlockInContext(lg.ctx, fn, "for_body");
            LLVMBasicBlockRef incr_bb = LLVMAppendBasicBlockInContext(lg.ctx, fn, "for_incr");
            LLVMBasicBlockRef end_bb  = LLVMAppendBasicBlockInContext(lg.ctx, fn, "for_end");

            st_push();
            st_define(n->for_var, var);

            LLVMBuildBr(lg.builder, cond_bb);

            LLVMPositionBuilderAtEnd(lg.builder, cond_bb);
            LLVMValueRef hi = emit_expr_llvm(n->for_iter->range_hi);
            if (!hi || LLVMGetTypeKind(LLVMTypeOf(hi)) != LLVMIntegerTypeKind)
                llvm_unsupported("for с не-целочислен диапазон");
            hi = coerce(hi, lg.i64_ty);
            char *nm = tmp_name();
            LLVMValueRef cur = LLVMBuildLoad2(lg.builder, lg.i64_ty, var, nm);
            free(nm);
            nm = tmp_name();
            LLVMValueRef cond = LLVMBuildICmp(lg.builder, LLVMIntSLT, cur, hi, nm);
            free(nm);
            LLVMBuildCondBr(lg.builder, cond, body_bb, end_bb);

            LLVMPositionBuilderAtEnd(lg.builder, body_bb);
            emit_block_llvm(n->for_body, end_bb, incr_bb);
            if (!LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(lg.builder)))
                LLVMBuildBr(lg.builder, incr_bb);

            LLVMPositionBuilderAtEnd(lg.builder, incr_bb);
            nm = tmp_name();
            LLVMValueRef iv = LLVMBuildLoad2(lg.builder, lg.i64_ty, var, nm);
            free(nm);
            nm = tmp_name();
            LLVMValueRef next = LLVMBuildAdd(lg.builder, iv,
                                             LLVMConstInt(lg.i64_ty, 1, 0), nm);
            free(nm);
            LLVMBuildStore(lg.builder, next, var);
            LLVMBuildBr(lg.builder, cond_bb);

            LLVMPositionBuilderAtEnd(lg.builder, end_bb);
            st_pop();
            break;
        }

        case NODE_EXPR_STMT:
            emit_expr_llvm(n->expr);
            break;

        case NODE_BLOCK:
            emit_block_llvm(n, break_bb, cont_bb);
            break;

        case NODE_BREAK:
            if (!break_bb) llvm_unsupported("break извън цикъл");
            LLVMBuildBr(lg.builder, break_bb);
            break;

        case NODE_CONTINUE:
            if (!cont_bb) llvm_unsupported("continue извън цикъл");
            LLVMBuildBr(lg.builder, cont_bb);
            break;

        default:
            llvm_unsupported_node(n);
            break;
    }
}

/* ---- spec ensures/requires (огледало на codegen_c) ---- */

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
    LLVMBuildCall2(lg.builder, LLVMGetElementType(LLVMTypeOf(lg.fprintf_fn)),
                   lg.fprintf_fn, call_args, 5, "");
    LLVMValueRef eargs[] = { LLVMConstInt(lg.i32_ty, 1, 0) };
    LLVMBuildCall2(lg.builder, LLVMGetElementType(LLVMTypeOf(lg.exit_fn)),
                   lg.exit_fn, eargs, 1, "");
    LLVMBuildUnreachable(lg.builder);
}

/* Една contract проверка: condbr → fail блок с emit_spec_fail_llvm. */
static void emit_spec_check_llvm(LLVMValueRef fn, Node *spec, Node *ensure,
                                 int is_requires, int idx) {
    LLVMValueRef cond = to_bool(emit_expr_llvm(ensure->ensure_expr));
    LLVMBasicBlockRef fail_bb = LLVMAppendBasicBlockInContext(lg.ctx, fn, "spec_fail");
    LLVMBasicBlockRef ok_bb = LLVMAppendBasicBlockInContext(lg.ctx, fn, "spec_ok");
    LLVMBuildCondBr(lg.builder, cond, ok_bb, fail_bb);
    LLVMPositionBuilderAtEnd(lg.builder, fail_bb);
    emit_spec_fail_llvm(spec->spec_name, is_requires, idx, ensure->ensure_text);
    LLVMPositionBuilderAtEnd(lg.builder, ok_bb);
}

/* ---- Function emission ---- */

static LLVMTypeRef fn_ret_type(Node *fn) {
    return fn->ret_type ? llvm_type(fn->ret_type) : lg.void_ty;
}

static LLVMTypeRef fn_type_of(Node *fn, LLVMTypeRef **out_param_tys, int *out_nparams) {
    LLVMTypeRef ret_ty = fn_ret_type(fn);
    int nparams = fn->params.len;
    LLVMTypeRef *param_tys = malloc(sizeof(LLVMTypeRef) * (size_t)(nparams > 0 ? nparams : 1));
    for (int i = 0; i < nparams; i++)
        param_tys[i] = llvm_type(fn->params.data[i]->param_type);
    if (out_param_tys) *out_param_tys = param_tys;
    if (out_nparams) *out_nparams = nparams;
    LLVMTypeRef fn_ty = LLVMFunctionType(ret_ty, param_tys, (unsigned)nparams, 0);
    if (!out_param_tys) free(param_tys);
    return fn_ty;
}

static char *impl_name_of(const char *fn_name) {
    char buf[512];
    snprintf(buf, sizeof buf, "__impl_%s", fn_name);
    return llvm_mangle(buf);
}

/* Първи проход: предекларации на всички функции (като в codegen_c).
 * Функция със spec получава impl (тялото) + wrapper (публичното име). */
static void predeclare_fn_llvm(Node *fn) {
    LLVMTypeRef fn_ty = fn_type_of(fn, NULL, NULL);
    Node *spec = fn->fn_body ? find_ensures_spec_llvm(fn->fn_name) : NULL;
    if (spec) {
        char *im = impl_name_of(fn->fn_name);
        LLVMAddFunction(lg.mod, im, fn_ty);
        free(im);
    }
    char *m = llvm_mangle(fn->fn_name);
    LLVMAddFunction(lg.mod, m, fn_ty);
    free(m);
}

/* Тялото на функцията (за spec функции — под impl името). */
static void emit_fn_llvm(Node *fn, Node *spec) {
    if (!fn->fn_body) return; /* само декларация */

    char *m = spec ? impl_name_of(fn->fn_name) : llvm_mangle(fn->fn_name);
    LLVMValueRef fn_val = LLVMGetNamedFunction(lg.mod, m);
    free(m);

    LLVMTypeRef ret_ty = fn_ret_type(fn);
    lg.cur_ret_ty = ret_ty;

    LLVMBasicBlockRef entry = LLVMAppendBasicBlockInContext(lg.ctx, fn_val, "entry");
    LLVMPositionBuilderAtEnd(lg.builder, entry);

    st_reset();
    st_push();

    /* alloca за параметрите (за променливост) */
    for (int i = 0; i < fn->params.len; i++) {
        char *pm = llvm_mangle(fn->params.data[i]->param_name);
        LLVMValueRef param = LLVMGetParam(fn_val, (unsigned)i);
        LLVMSetValueName2(param, pm, strlen(pm));
        LLVMValueRef alloca = LLVMBuildAlloca(lg.builder, LLVMTypeOf(param), pm);
        LLVMBuildStore(lg.builder, param, alloca);
        free(pm);
        st_define(fn->params.data[i]->param_name, alloca);
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
            LLVMValueRef v = emit_expr_llvm(s->expr);
            if (!v) llvm_unsupported("print като неявен return");
            LLVMBuildRet(lg.builder, coerce(v, ret_ty));
        } else {
            emit_stmt_llvm(s, NULL, NULL);
        }
    }

    st_pop();

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
    char *m = llvm_mangle(fn->fn_name);
    LLVMValueRef wrapper = LLVMGetNamedFunction(lg.mod, m);
    free(m);
    char *im = impl_name_of(fn->fn_name);
    LLVMValueRef impl = LLVMGetNamedFunction(lg.mod, im);
    free(im);

    LLVMTypeRef ret_ty = fn_ret_type(fn);
    lg.cur_ret_ty = ret_ty;

    LLVMBasicBlockRef entry = LLVMAppendBasicBlockInContext(lg.ctx, wrapper, "entry");
    LLVMPositionBuilderAtEnd(lg.builder, entry);

    st_reset();
    st_push();

    /* параметрите на wrapper-а са spec input-ите (като в codegen_c) */
    int np = spec->spec_inputs.len;
    for (int i = 0; i < np; i++) {
        Node *sp = spec->spec_inputs.data[i];
        char *pm = llvm_mangle(sp->param_name);
        LLVMValueRef param = LLVMGetParam(wrapper, (unsigned)i);
        LLVMSetValueName2(param, pm, strlen(pm));
        LLVMValueRef alloca = LLVMBuildAlloca(lg.builder, LLVMTypeOf(param), pm);
        LLVMBuildStore(lg.builder, param, alloca);
        free(pm);
        st_define(sp->param_name, alloca);
    }

    /* предусловия преди повикването */
    for (int j = 0; j < spec->spec_requires.len; j++)
        emit_spec_check_llvm(wrapper, spec, spec->spec_requires.data[j], 1, j + 1);

    /* повикай impl с параметрите */
    LLVMValueRef *args = malloc(sizeof(LLVMValueRef) * (size_t)(np > 0 ? np : 1));
    for (int i = 0; i < np; i++) {
        LLVMValueRef alloca = st_lookup(spec->spec_inputs.data[i]->param_name);
        char *nm = tmp_name();
        args[i] = LLVMBuildLoad2(lg.builder, LLVMGetAllocatedType(alloca), alloca, nm);
        free(nm);
    }
    LLVMTypeRef impl_ty = LLVMGetElementType(LLVMTypeOf(impl));

    if (ret_ty != lg.void_ty) {
        char *nm = tmp_name();
        LLVMValueRef out = LLVMBuildCall2(lg.builder, impl_ty, impl, args, (unsigned)np, nm);
        free(nm);
        LLVMValueRef b_output = LLVMBuildAlloca(lg.builder, ret_ty, "b_output");
        LLVMBuildStore(lg.builder, out, b_output);
        st_define("output", b_output);

        /* гаранции след повикването */
        for (int j = 0; j < spec->spec_ensures.len; j++)
            emit_spec_check_llvm(wrapper, spec, spec->spec_ensures.data[j], 0, j + 1);

        char *nm2 = tmp_name();
        LLVMValueRef r = LLVMBuildLoad2(lg.builder, ret_ty, b_output, nm2);
        free(nm2);
        LLVMBuildRet(lg.builder, r);
    } else {
        /* void функция: само повикай impl след предусловията */
        LLVMBuildCall2(lg.builder, impl_ty, impl, args, (unsigned)np, "");
        LLVMBuildRetVoid(lg.builder);
    }
    free(args);
    st_pop();
}

/* ---- Public API ---- */

void codegen_llvm(Node *program, const char *output_path) {
    lg.ctx = LLVMContextCreate();
    lg.mod = LLVMModuleCreateWithNameInContext("baga_module", lg.ctx);
    lg.builder = LLVMCreateBuilderInContext(lg.ctx);
    lg.tmp_counter = 0;
    lg.program = program;

    lg.i64_ty = LLVMInt64TypeInContext(lg.ctx);
    lg.i32_ty = LLVMInt32TypeInContext(lg.ctx);
    lg.i1_ty = LLVMInt1TypeInContext(lg.ctx);
    lg.double_ty = LLVMDoubleTypeInContext(lg.ctx);
    lg.void_ty = LLVMVoidTypeInContext(lg.ctx);
    lg.ptr_ty = LLVMPointerType(LLVMInt8TypeInContext(lg.ctx), 0);

    /* declare printf / fprintf / exit / stderr */
    {
        LLVMTypeRef printf_args[] = { lg.ptr_ty };
        LLVMTypeRef printf_ty = LLVMFunctionType(lg.i32_ty, printf_args, 1, 1);
        lg.printf_fn = LLVMAddFunction(lg.mod, "printf", printf_ty);

        LLVMTypeRef fprintf_args[] = { lg.ptr_ty, lg.ptr_ty };
        LLVMTypeRef fprintf_ty = LLVMFunctionType(lg.i32_ty, fprintf_args, 2, 1);
        lg.fprintf_fn = LLVMAddFunction(lg.mod, "fprintf", fprintf_ty);

        LLVMTypeRef exit_args[] = { lg.i32_ty };
        LLVMTypeRef exit_ty = LLVMFunctionType(lg.void_ty, exit_args, 1, 0);
        lg.exit_fn = LLVMAddFunction(lg.mod, "exit", exit_ty);

        /* glibc: stderr е external global (FILE*) */
        lg.stderr_global = LLVMAddGlobal(lg.mod, lg.ptr_ty, "stderr");
    }

    /* първи проход: предекларации */
    for (int i = 0; i < program->items.len; i++) {
        Node *item = program->items.data[i];
        if (item->kind == NODE_FN)
            predeclare_fn_llvm(item);
    }

    /* втори проход: тела + wrapper-и */
    for (int i = 0; i < program->items.len; i++) {
        Node *item = program->items.data[i];
        if (item->kind != NODE_FN || !item->fn_body) continue;
        Node *spec = find_ensures_spec_llvm(item->fn_name);
        emit_fn_llvm(item, spec);
        if (spec) emit_wrapper_llvm(item, spec);
    }

    /* emit C main wrapper */
    {
        char *main_m = llvm_mangle("main");
        LLVMValueRef baga_main = LLVMGetNamedFunction(lg.mod, main_m);
        free(main_m);

        if (baga_main) {
            LLVMTypeRef c_main_ty = LLVMFunctionType(lg.i32_ty, NULL, 0, 0);
            LLVMValueRef c_main = LLVMAddFunction(lg.mod, "main", c_main_ty);
            LLVMBasicBlockRef entry = LLVMAppendBasicBlockInContext(lg.ctx, c_main, "entry");
            LLVMPositionBuilderAtEnd(lg.builder, entry);
            LLVMBuildCall2(lg.builder,
                LLVMGetElementType(LLVMTypeOf(baga_main)),
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
    LLVMContextDispose(lg.ctx);
}

#endif /* BAGA_LLVM */
