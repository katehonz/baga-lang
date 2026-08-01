/* ============================================================
 *  Cranelift backend — Фаза 3 (JIT през Rust FFI)
 *
 *  Генерира сериализиран стеков bytecode от AST-то (огледално на
 *  codegen_llvm.c), после го подава на Rust staticlib-а, който го
 *  JIT-ва in-process. Поддържа ядрото: i64/f64/bool/str, аритметика,
 *  control flow, match (i64), enum, функции+рекурсия, print, contracts,
 *  arg/arg_count. Struct/Vec/str-builtins/effects → честен отказ.
 *
 *  Принцип: НИКАКВИ тихи стойности — неподдържан конструкт →
 *  "baga: Cranelift backend: неподдържан конструкт '<какво>'" + exit(1).
 * ============================================================ */

#ifdef BAGA_CRANELIFT

#include "baga.h"
#include "baga_clif_rt.h"

/* ---- FFI към Rust staticlib (cranelift/src/lib.rs) ---- */
extern void *baga_jit_new(void);
extern void  baga_jit_free(void *jit);
extern int   baga_jit_intern_str(void *jit, const unsigned char *bytes, size_t len);
extern int   baga_jit_declare(void *jit, const char *name, int ret_ty,
                              const int *param_tys, int nparams);
extern int   baga_jit_define(void *jit, int user_index,
                             const unsigned char *code, size_t code_len);
extern int   baga_jit_run_main(void *jit, int argc, char **argv);

/* ---- bytecode буфер ---- */
typedef struct { unsigned char *d; size_t n, cap; } Buf;

static void buf_u8(Buf *b, unsigned char v) {
    if (b->n == b->cap) { b->cap = b->cap ? b->cap * 2 : 256;
        b->d = realloc(b->d, b->cap); if (!b->d) { fprintf(stderr, "baga: out of memory\n"); exit(1); } }
    b->d[b->n++] = v;
}
static void buf_u16(Buf *b, unsigned short v) { buf_u8(b, v & 0xff); buf_u8(b, (v >> 8) & 0xff); }
static void buf_u32(Buf *b, unsigned int v) {
    for (int i = 0; i < 4; i++) buf_u8(b, (v >> (8 * i)) & 0xff);
}
static void buf_i64(Buf *b, long long v) {
    for (int i = 0; i < 8; i++) buf_u8(b, (unsigned char)((v >> (8 * i)) & 0xff));
}
static void buf_f64(Buf *b, double v) {
    unsigned char raw[8]; memcpy(raw, &v, 8);
    for (int i = 0; i < 8; i++) buf_u8(b, raw[i]);
}

/* ---- контекст ---- */
typedef struct {
    char *jit_name;     /* име в JIT символната таблица */
    char *call_name;    /* име за повикване (NULL = не се вика по име) */
    Node *fn;
    Node *spec;
    int is_wrapper;
    int impl_fn_id;     /* за wrapper: fn_id на impl-а */
} CrFunc;

typedef struct { char *name; int slot; int ty; } CrVar;

typedef struct { int label; int spec_id; int kind_id; int idx; int text_id; } CrFail;

static struct {
    Node *program;
    Buf *cur;             /* текущ bytecode буфер */
    int next_label;
    int terminated;
    int cur_ret_ty;       /* TY_* код на върнатия тип */
    /* symbol table */
    CrVar vars[1024];
    int nvars;
    int scope_marks[128];
    int depth;
    int next_slot;
    /* function table */
    CrFunc funcs[512];
    int nfuncs;
    /* string table */
    char *strs[4096];
    int nstrs;
    /* pending spec fail блокове (за wrapper-и) */
    CrFail fails[256];
    int nfails;
} cg;

/* ---- честен отказ ---- */
static void cr_unsupported(const char *what) {
    fprintf(stderr, "baga: Cranelift backend: неподдържан конструкт '%s'\n", what);
    exit(1);
}

/* ---- типове ---- */
static int ty_code(Type *t) {
    if (!t) return TY_I64;
    switch (t->kind) {
        case TYPE_VOID: return TY_VOID;
        case TYPE_BOOL: return TY_BOOL;
        case TYPE_I32:  return TY_I32;
        case TYPE_I64:  return TY_I64;
        case TYPE_F64:  return TY_F64;
        case TYPE_STR:  return TY_PTR;
        default:        return TY_I64;
    }
}
static int ty_code_node(Node *n) { return n ? ty_code(n->type) : TY_I64; }

/* Тип от type-изразен възел (NODE_TYPE / NODE_TYPE_EFFECT) — по име,
 * огледално на llvm_type. */
static int ty_code_tn(Node *tn) {
    if (!tn) return TY_VOID;
    if (tn->kind == NODE_TYPE_EFFECT) return ty_code_tn(tn->inner_type);
    if (tn->kind != NODE_TYPE) return TY_I64;
    const char *nm = tn->type_name;
    if (!nm) return TY_I64;
    if (strcmp(nm, "i64") == 0) return TY_I64;
    if (strcmp(nm, "i32") == 0) return TY_I32;
    if (strcmp(nm, "f64") == 0) return TY_F64;
    if (strcmp(nm, "bool") == 0) return TY_BOOL;
    if (strcmp(nm, "str") == 0) return TY_PTR;
    if (strcmp(nm, "void") == 0) return TY_VOID;
    cr_unsupported("неподдържан тип (struct/Vec/масив)");
    return TY_I64;
}

/* ---- symbol table ---- */
static void cr_st_push(void) { cg.scope_marks[cg.depth++] = cg.nvars; }
static void cr_st_pop(void)  { cg.nvars = cg.scope_marks[--cg.depth]; }
static void cr_define(const char *name, int slot, int ty) {
    cg.vars[cg.nvars].name = (char *)name;
    cg.vars[cg.nvars].slot = slot;
    cg.vars[cg.nvars].ty = ty;
    cg.nvars++;
}
static CrVar *cr_lookup(const char *name) {
    for (int i = cg.nvars - 1; i >= 0; i--)
        if (strcmp(cg.vars[i].name, name) == 0) return &cg.vars[i];
    return NULL;
}
static int new_slot(int ty) { (void)ty; return cg.next_slot++; }

/* ---- низове ---- */
static int intern_str(const char *s) {
    for (int i = 0; i < cg.nstrs; i++)
        if (strcmp(cg.strs[i], s) == 0) return i;
    cg.strs[cg.nstrs] = (char *)s;
    return cg.nstrs++;
}

/* ---- функции ---- */
static int fn_id_of(const char *name) {
    for (int i = 0; i < cg.nfuncs; i++)
        if (cg.funcs[i].call_name && strcmp(cg.funcs[i].call_name, name) == 0)
            return RT_COUNT + i;
    return -1;
}

/* ---- enum ---- */
static int find_enum_variant(const char *name) {
    for (int i = 0; i < cg.program->items.len; i++) {
        Node *it = cg.program->items.data[i];
        if (it->kind != NODE_ENUM) continue;
        for (int j = 0; j < it->n_variants; j++)
            if (strcmp(it->enum_variants[j], name) == 0) return j;
    }
    return -1;
}

/* ---- spec ---- */
static Node *find_ensures_spec(const char *fn_name) {
    for (int i = 0; i < cg.program->items.len; i++) {
        Node *it = cg.program->items.data[i];
        if (it->kind == NODE_SPEC && strcmp(it->spec_name, fn_name) == 0 &&
            (it->spec_ensures.len > 0 || it->spec_requires.len > 0))
            return it;
    }
    return NULL;
}

/* ---- блокове / скокове ---- */
static int new_label(void) { return cg.next_label++; }
static void begin_block(int L) { buf_u8(cg.cur, CL_LABEL); buf_u32(cg.cur, (unsigned)L); cg.terminated = 0; }
static void emit_br(int L) { buf_u8(cg.cur, CL_BR); buf_u32(cg.cur, (unsigned)L); cg.terminated = 1; }
static void emit_br_false(int L) { buf_u8(cg.cur, CL_BR_FALSE); buf_u32(cg.cur, (unsigned)L); cg.terminated = 1; }

/* ---- binop код ---- */
static int binop_code(BinOp op, int is_f) {
    switch (op) {
        case OP_ADD: return is_f ? B_ADD_F : B_ADD_I;
        case OP_SUB: return is_f ? B_SUB_F : B_SUB_I;
        case OP_MUL: return is_f ? B_MUL_F : B_MUL_I;
        case OP_DIV: return is_f ? B_DIV_F : B_DIV_I;
        case OP_MOD: return is_f ? B_MOD_F : B_MOD_I;
        case OP_EQ:  return is_f ? B_EQ_F  : B_EQ_I;
        case OP_NEQ: return is_f ? B_NEQ_F : B_NEQ_I;
        case OP_LT:  return is_f ? B_LT_F  : B_LT_I;
        case OP_GT:  return is_f ? B_GT_F  : B_GT_I;
        case OP_LE:  return is_f ? B_LE_F  : B_LE_I;
        case OP_GE:  return is_f ? B_GE_F  : B_GE_I;
        default: cr_unsupported("binop"); return 0;
    }
}
static int is_cmp_op(BinOp op) {
    return op == OP_EQ || op == OP_NEQ || op == OP_LT || op == OP_GT ||
           op == OP_LE || op == OP_GE;
}

/* ---- изрази ---- */
static int cr_expr(Node *n);

static int is_print_call(Node *n) {
    if (n->kind != NODE_CALL || n->callee->kind != NODE_IDENT) return 0;
    return strcmp(n->callee->name, "print") == 0 ||
           strcmp(n->callee->name, "println") == 0 ||
           strcmp(n->callee->name, "write") == 0;
}

static void emit_print_cr(Node *n) {
    int is_write = strcmp(n->callee->name, "write") == 0;
    if (n->args.len == 0) {
        buf_u8(cg.cur, CL_CALL); buf_u32(cg.cur, RT_PRINT_NL); buf_u16(cg.cur, 0);
        return;
    }
    for (int i = 0; i < n->args.len; i++) {
        Node *arg = n->args.data[i];
        unsigned int rt;
        if (arg->kind == NODE_STR_LIT) {
            int id = intern_str(arg->str_val);
            buf_u8(cg.cur, CL_SCONST); buf_u32(cg.cur, (unsigned)id);
            rt = is_write ? RT_WRITE_STR : RT_PRINT_STR;
        } else {
            int ty = cr_expr(arg);
            if (ty == TY_PTR)       rt = is_write ? RT_WRITE_STR : RT_PRINT_STR;
            else if (ty == TY_F64)  rt = RT_PRINT_F64;
            else if (ty == TY_BOOL) rt = RT_PRINT_BOOL;
            else if (ty == TY_I64 || ty == TY_I32) rt = RT_PRINT_I64;
            else cr_unsupported("print върху този тип");
        }
        buf_u8(cg.cur, CL_CALL); buf_u32(cg.cur, rt); buf_u16(cg.cur, 1);
    }
}

/* match клон: RETURN записва резултата + скок към merge; EXPR_STMT — ефект */
static void emit_match_arm_cr(Node *arm, int res_slot, int L_end) {
    Node *body = arm->arm_body;
    if (!body || body->kind != NODE_BLOCK)
        cr_unsupported("match клон, който не е блок");
    for (int j = 0; j < body->stmts.len; j++) {
        Node *s = body->stmts.data[j];
        if (cg.terminated) break;
        if (s->kind == NODE_RETURN) {
            if (s->ret_val && res_slot >= 0) {
                cr_expr(s->ret_val);
                buf_u8(cg.cur, CL_STORE); buf_u16(cg.cur, (unsigned short)res_slot);
            }
            emit_br(L_end);
            return;
        }
        if (s->kind == NODE_EXPR_STMT) {
            int ty = cr_expr(s->expr);
            if (ty != TY_VOID) buf_u8(cg.cur, CL_DROP);
            continue;
        }
        cr_unsupported("оператор в match клон (само изрази)");
    }
}

static int emit_match_cr(Node *n) {
    int sty = cr_expr(n->match_expr);
    if (sty != TY_I64 && sty != TY_I32)
        cr_unsupported("match върху не-целочислена стойност");
    int scrut = new_slot(TY_I64);
    buf_u8(cg.cur, CL_ALLOCA); buf_u16(cg.cur, (unsigned short)scrut); buf_u8(cg.cur, TY_I64);
    buf_u8(cg.cur, CL_STORE); buf_u16(cg.cur, (unsigned short)scrut);

    int res_ty = ty_code_node(n);
    int res_slot = -1;
    if (res_ty != TY_VOID) {
        res_slot = new_slot(res_ty);
        buf_u8(cg.cur, CL_ALLOCA); buf_u16(cg.cur, (unsigned short)res_slot); buf_u8(cg.cur, (unsigned char)res_ty);
        if (res_ty == TY_F64) buf_u8(cg.cur, CL_FCONST), buf_f64(cg.cur, 0.0);
        else buf_u8(cg.cur, CL_ICONST), buf_i64(cg.cur, 0);
        buf_u8(cg.cur, CL_STORE); buf_u16(cg.cur, (unsigned short)res_slot);
    }

    int L_end = new_label();
    int has_wildcard = 0;
    for (int i = 0; i < n->match_arms.len; i++) {
        Node *arm = n->match_arms.data[i];
        if (arm->arm_pattern) {
            int L_next = new_label();
            buf_u8(cg.cur, CL_LOAD); buf_u16(cg.cur, (unsigned short)scrut);
            int pty = cr_expr(arm->arm_pattern);
            (void)pty;
            buf_u8(cg.cur, CL_BINOP); buf_u8(cg.cur, B_EQ_I);
            emit_br_false(L_next);
            begin_block(new_label());
            emit_match_arm_cr(arm, res_slot, L_end);
            if (!cg.terminated) emit_br(L_end);
            begin_block(L_next);
        } else {
            has_wildcard = 1;
            emit_match_arm_cr(arm, res_slot, L_end);
            if (!cg.terminated) emit_br(L_end);
        }
    }
    if (!has_wildcard && !cg.terminated) emit_br(L_end);
    begin_block(L_end);

    if (res_slot >= 0) {
        buf_u8(cg.cur, CL_LOAD); buf_u16(cg.cur, (unsigned short)res_slot);
    }
    return res_ty;
}

static int cr_expr(Node *n) {
    if (!n) cr_unsupported("празен израз");
    Buf *b = cg.cur;

    switch (n->kind) {
        case NODE_INT_LIT:
            buf_u8(b, CL_ICONST); buf_i64(b, (long long)n->int_val); return TY_I64;
        case NODE_FLOAT_LIT:
            buf_u8(b, CL_FCONST); buf_f64(b, n->float_val); return TY_F64;
        case NODE_BOOL_LIT:
            buf_u8(b, CL_BCONST); buf_u8(b, n->bool_val ? 1 : 0); return TY_BOOL;
        case NODE_STR_LIT: {
            int id = intern_str(n->str_val);
            buf_u8(b, CL_SCONST); buf_u32(b, (unsigned)id); return TY_PTR;
        }
        case NODE_IDENT: {
            CrVar *v = cr_lookup(n->name);
            if (v) { buf_u8(b, CL_LOAD); buf_u16(b, (unsigned short)v->slot); return v->ty; }
            int var = find_enum_variant(n->name);
            if (var >= 0) { buf_u8(b, CL_ICONST); buf_i64(b, var); return TY_I64; }
            char msg[256]; snprintf(msg, sizeof msg, "недефинирано име '%s'", n->name);
            cr_unsupported(msg); return TY_VOID;
        }
        case NODE_BINARY: {
            if (n->bin_op == OP_AND || n->bin_op == OP_OR) {
                cr_expr(n->left); cr_expr(n->right);
                buf_u8(b, n->bin_op == OP_AND ? CL_AND : CL_OR);
                return TY_BOOL;
            }
            if ((n->bin_op == OP_EQ || n->bin_op == OP_NEQ) &&
                n->left->type && n->right->type &&
                n->left->type->kind == TYPE_STR && n->right->type->kind == TYPE_STR)
                cr_unsupported("сравнение на низове");
            int lt = ty_code_node(n->left), rt = ty_code_node(n->right);
            int is_f = (lt == TY_F64 || rt == TY_F64);
            cr_expr(n->left);
            if (is_f && lt != TY_F64) buf_u8(b, CL_PROMOTE);
            cr_expr(n->right);
            if (is_f && rt != TY_F64) buf_u8(b, CL_PROMOTE);
            buf_u8(b, CL_BINOP); buf_u8(b, (unsigned char)binop_code(n->bin_op, is_f));
            if (is_cmp_op(n->bin_op)) return TY_BOOL;
            return is_f ? TY_F64 : TY_I64;
        }
        case NODE_UNARY: {
            int ty = cr_expr(n->operand);
            switch (n->un_op) {
                case UOP_NEG:
                    buf_u8(b, CL_NEG); buf_u8(b, (unsigned char)(ty == TY_F64 ? TY_F64 : TY_I64));
                    return ty == TY_F64 ? TY_F64 : TY_I64;
                case UOP_NOT:
                    buf_u8(b, CL_NOT); return TY_BOOL;
                case UOP_REF:   cr_unsupported("референция (&x)"); break;
                case UOP_DEREF: cr_unsupported("дереференциране (*x)"); break;
            }
            return TY_VOID;
        }
        case NODE_CALL: {
            if (is_print_call(n)) { emit_print_cr(n); return TY_VOID; }
            if (n->callee->kind != NODE_IDENT)
                cr_unsupported("повикване през израз (не име)");
            const char *cn = n->callee->name;
            if (strcmp(cn, "arg_count") == 0) {
                buf_u8(b, CL_CALL); buf_u32(b, RT_ARG_COUNT); buf_u16(b, 0); return TY_I64;
            }
            if (strcmp(cn, "arg") == 0) {
                cr_expr(n->args.data[0]);
                buf_u8(b, CL_CALL); buf_u32(b, RT_ARG); buf_u16(b, 1); return TY_PTR;
            }
            int fid = fn_id_of(cn);
            if (fid < 0) {
                char msg[256]; snprintf(msg, sizeof msg, "вградена функция '%s'", cn);
                cr_unsupported(msg);
            }
            int nargs = n->args.len;
            for (int i = 0; i < nargs; i++) cr_expr(n->args.data[i]);
            buf_u8(b, CL_CALL); buf_u32(b, (unsigned)fid); buf_u16(b, (unsigned short)nargs);
            /* върнат тип — от декларацията на функцията */
            for (int i = 0; i < cg.nfuncs; i++)
                if (cg.funcs[i].call_name && strcmp(cg.funcs[i].call_name, cn) == 0)
                    return ty_code_tn(cg.funcs[i].fn->ret_type);
            return TY_I64;
        }
        case NODE_MATCH:
            return emit_match_cr(n);
        case NODE_ASSIGN: {
            if (n->assign_target->kind != NODE_IDENT)
                cr_unsupported("присвояване на поле/индекс");
            CrVar *v = cr_lookup(n->assign_target->name);
            if (!v) {
                char msg[256]; snprintf(msg, sizeof msg, "присвояване на недефинирано име '%s'",
                                        n->assign_target->name);
                cr_unsupported(msg);
            }
            cr_expr(n->assign_val);
            buf_u8(b, CL_STORE); buf_u16(b, (unsigned short)v->slot);
            return TY_VOID;
        }
        case NODE_TRY:   return cr_expr(n->try_expr);
        case NODE_CATCH: return cr_expr(n->catch_expr);
        case NODE_IF:    cr_unsupported("if като израз"); break;
        case NODE_INDEX: cr_unsupported("индексиране"); break;
        case NODE_FIELD: cr_unsupported("достъп до поле (struct)"); break;
        case NODE_STRUCT_LIT: cr_unsupported("структурен литерал"); break;
        case NODE_RANGE: cr_unsupported("диапазон (a..b) извън for"); break;
        default: {
            char msg[64]; snprintf(msg, sizeof msg, "AST възел #%d", (int)n->kind);
            cr_unsupported(msg);
        }
    }
    return TY_VOID;
}

/* ---- оператори ---- */
static void cr_stmt(Node *n, int break_lbl, int cont_lbl);

static void cr_block(Node *block, int break_lbl, int cont_lbl) {
    if (!block) return;
    if (block->kind != NODE_BLOCK) cr_unsupported("тяло, което не е блок");
    cr_st_push();
    for (int i = 0; i < block->stmts.len; i++) {
        if (cg.terminated) break;   /* мъртъв код след terminator */
        cr_stmt(block->stmts.data[i], break_lbl, cont_lbl);
    }
    cr_st_pop();
}

static void cr_stmt(Node *n, int break_lbl, int cont_lbl) {
    if (!n) return;
    Buf *b = cg.cur;

    switch (n->kind) {
        case NODE_LET: {
            int ty;
            if (n->let_type) ty = ty_code_tn(n->let_type);
            else if (n->let_init && n->let_init->type) ty = ty_code(n->let_init->type);
            else ty = TY_I64;
            int slot = new_slot(ty);
            buf_u8(b, CL_ALLOCA); buf_u16(b, (unsigned short)slot); buf_u8(b, (unsigned char)ty);
            if (n->let_init) {
                cr_expr(n->let_init);
                buf_u8(b, CL_STORE); buf_u16(b, (unsigned short)slot);
            }
            cr_define(n->let_name, slot, ty);
            break;
        }
        case NODE_RETURN: {
            if (n->ret_val) { cr_expr(n->ret_val); buf_u8(b, CL_RET); }
            else buf_u8(b, CL_RET_VOID);
            cg.terminated = 1;
            break;
        }
        case NODE_IF: {
            cr_expr(n->cond);
            int L_else = new_label(), L_end = new_label();
            emit_br_false(L_else);
            begin_block(new_label());
            cr_block(n->then_br, break_lbl, cont_lbl);
            if (!cg.terminated) emit_br(L_end);
            begin_block(L_else);
            if (n->else_br) cr_block(n->else_br, break_lbl, cont_lbl);
            if (!cg.terminated) emit_br(L_end);
            begin_block(L_end);
            break;
        }
        case NODE_WHILE: {
            int L_cond = new_label(), L_end = new_label();
            emit_br(L_cond);
            begin_block(L_cond);
            cr_expr(n->while_cond);
            emit_br_false(L_end);
            begin_block(new_label());
            cr_block(n->while_body, L_end, L_cond);
            if (!cg.terminated) emit_br(L_cond);
            begin_block(L_end);
            break;
        }
        case NODE_FOR: {
            if (!n->for_iter || n->for_iter->kind != NODE_RANGE)
                cr_unsupported("for без диапазон (a..b)");
            cr_st_push();
            int slot = new_slot(TY_I64);
            buf_u8(b, CL_ALLOCA); buf_u16(b, (unsigned short)slot); buf_u8(b, TY_I64);
            cr_expr(n->for_iter->range_lo);
            buf_u8(b, CL_STORE); buf_u16(b, (unsigned short)slot);
            cr_define(n->for_var, slot, TY_I64);
            int L_cond = new_label(), L_incr = new_label(), L_end = new_label();
            emit_br(L_cond);
            begin_block(L_cond);
            buf_u8(b, CL_LOAD); buf_u16(b, (unsigned short)slot);
            cr_expr(n->for_iter->range_hi);
            buf_u8(b, CL_BINOP); buf_u8(b, B_LT_I);
            emit_br_false(L_end);
            begin_block(new_label());
            cr_block(n->for_body, L_end, L_incr);
            if (!cg.terminated) emit_br(L_incr);
            begin_block(L_incr);
            buf_u8(b, CL_LOAD); buf_u16(b, (unsigned short)slot);
            buf_u8(b, CL_ICONST); buf_i64(b, 1);
            buf_u8(b, CL_BINOP); buf_u8(b, B_ADD_I);
            buf_u8(b, CL_STORE); buf_u16(b, (unsigned short)slot);
            emit_br(L_cond);
            begin_block(L_end);
            cr_st_pop();
            break;
        }
        case NODE_EXPR_STMT: {
            int ty = cr_expr(n->expr);
            if (ty != TY_VOID) buf_u8(b, CL_DROP);
            break;
        }
        case NODE_BLOCK:
            cr_block(n, break_lbl, cont_lbl);
            break;
        case NODE_BREAK:
            if (break_lbl < 0) cr_unsupported("break извън цикъл");
            emit_br(break_lbl);
            break;
        case NODE_CONTINUE:
            if (cont_lbl < 0) cr_unsupported("continue извън цикъл");
            emit_br(cont_lbl);
            break;
        default: {
            char msg[64]; snprintf(msg, sizeof msg, "AST възел #%d", (int)n->kind);
            cr_unsupported(msg);
        }
    }
}

/* ---- contract проверка (за wrapper) ---- */
static void emit_spec_check_cr(Node *spec, Node *ensure, int is_requires, int idx) {
    cr_expr(ensure->ensure_expr);
    int L_fail = new_label();
    emit_br_false(L_fail);
    begin_block(new_label());   /* ok път (fallthrough) */
    cg.fails[cg.nfails].label = L_fail;
    cg.fails[cg.nfails].spec_id = intern_str(spec->spec_name);
    cg.fails[cg.nfails].kind_id = intern_str(is_requires ? "requires" : "ensures");
    cg.fails[cg.nfails].idx = idx;
    cg.fails[cg.nfails].text_id = intern_str(ensure->ensure_text);
    cg.nfails++;
}

/* ---- тяло на функция (impl или обикновена) ---- */
static void cr_emit_fn_body(Node *fn) {
    Buf buf = {0};
    cg.cur = &buf;
    cg.next_label = 0; cg.terminated = 1; cg.nvars = 0; cg.depth = 0;
    cg.next_slot = 0; cg.nfails = 0;
    cg.cur_ret_ty = ty_code_tn(fn->ret_type);

    cr_st_push();
    for (int i = 0; i < fn->params.len; i++) {
        Node *p = fn->params.data[i];
        int ty = ty_code_tn(p->param_type);
        cr_define(p->param_name, i, ty);   /* slot i; интерпретаторът ги store-ва */
    }
    cg.next_slot = fn->params.len;

    begin_block(new_label());   /* entry (label 0) */

    int has_ret = fn->ret_type != NULL;
    NodeVec *stmts = &fn->fn_body->stmts;
    for (int i = 0; i < stmts->len; i++) {
        if (cg.terminated) break;
        Node *s = stmts->data[i];
        if (has_ret && i == stmts->len - 1 && s->kind == NODE_EXPR_STMT) {
            cr_expr(s->expr);
            buf_u8(cg.cur, CL_RET);
            cg.terminated = 1;
        } else {
            cr_stmt(s, -1, -1);
        }
    }
    if (!cg.terminated) {
        if (cg.cur_ret_ty == TY_VOID) {
            buf_u8(cg.cur, CL_RET_VOID);
        } else {
            if (cg.cur_ret_ty == TY_F64) { buf_u8(cg.cur, CL_FCONST); buf_f64(cg.cur, 0.0); }
            else { buf_u8(cg.cur, CL_ICONST); buf_i64(cg.cur, 0); }
            buf_u8(cg.cur, CL_RET);
        }
        cg.terminated = 1;
    }
    cr_st_pop();
    /* buf-ът се копира от извикващия (define pass) — върни през глобален */
    cg.cur = NULL;
    /* запази в funcs чрез външна логика; тук използваме временен глобален */
    extern Buf cr_last_body;
    cr_last_body = buf;
}

Buf cr_last_body;

/* ---- wrapper (requires/ensures) ---- */
static void cr_emit_wrapper(Node *fn, Node *spec) {
    Buf buf = {0};
    cg.cur = &buf;
    cg.next_label = 0; cg.terminated = 1; cg.nvars = 0; cg.depth = 0;
    cg.next_slot = 0; cg.nfails = 0;
    cg.cur_ret_ty = ty_code_tn(fn->ret_type);

    cr_st_push();
    int np = fn->params.len;
    for (int i = 0; i < np; i++) {
        Node *p = fn->params.data[i];
        int ty = ty_code_tn(p->param_type);
        cr_define(p->param_name, i, ty);
    }
    cg.next_slot = np;

    begin_block(new_label());   /* entry (label 0) */

    /* предусловия */
    for (int j = 0; j < spec->spec_requires.len; j++)
        emit_spec_check_cr(spec, spec->spec_requires.data[j], 1, j + 1);

    /* повикай impl */
    int impl_fid = -1;
    for (int i = 0; i < cg.nfuncs; i++)
        if (cg.funcs[i].fn == fn && !cg.funcs[i].is_wrapper) { impl_fid = RT_COUNT + i; break; }
    for (int i = 0; i < np; i++) {
        buf_u8(cg.cur, CL_LOAD); buf_u16(cg.cur, (unsigned short)i);
    }
    buf_u8(cg.cur, CL_CALL); buf_u32(cg.cur, (unsigned)impl_fid); buf_u16(cg.cur, (unsigned short)np);

    if (cg.cur_ret_ty != TY_VOID) {
        int out_slot = new_slot(cg.cur_ret_ty);
        buf_u8(cg.cur, CL_ALLOCA); buf_u16(cg.cur, (unsigned short)out_slot); buf_u8(cg.cur, (unsigned char)cg.cur_ret_ty);
        buf_u8(cg.cur, CL_STORE); buf_u16(cg.cur, (unsigned short)out_slot);
        cr_define("output", out_slot, cg.cur_ret_ty);
        for (int j = 0; j < spec->spec_ensures.len; j++)
            emit_spec_check_cr(spec, spec->spec_ensures.data[j], 0, j + 1);
        buf_u8(cg.cur, CL_LOAD); buf_u16(cg.cur, (unsigned short)out_slot);
        buf_u8(cg.cur, CL_RET);
        cg.terminated = 1;
    } else {
        buf_u8(cg.cur, CL_RET_VOID);
        cg.terminated = 1;
    }

    /* fail блокове (след RET — мъртъв код, но валидни блокове) */
    for (int k = 0; k < cg.nfails; k++) {
        begin_block(cg.fails[k].label);
        buf_u8(cg.cur, CL_SCONST); buf_u32(cg.cur, (unsigned)cg.fails[k].spec_id);
        buf_u8(cg.cur, CL_SCONST); buf_u32(cg.cur, (unsigned)cg.fails[k].kind_id);
        buf_u8(cg.cur, CL_ICONST); buf_i64(cg.cur, cg.fails[k].idx);
        buf_u8(cg.cur, CL_SCONST); buf_u32(cg.cur, (unsigned)cg.fails[k].text_id);
        buf_u8(cg.cur, CL_CALL); buf_u32(cg.cur, RT_SPEC_FAIL); buf_u16(cg.cur, 4);
        /* spec_fail exit-ва, но verifier-ът изисква terminator, съвпадащ със
         * сигнатурата → формален return (стойността е мъртва). */
        if (cg.cur_ret_ty == TY_VOID) {
            buf_u8(cg.cur, CL_RET_VOID);
        } else {
            if (cg.cur_ret_ty == TY_F64) { buf_u8(cg.cur, CL_FCONST); buf_f64(cg.cur, 0.0); }
            else if (cg.cur_ret_ty == TY_BOOL) { buf_u8(cg.cur, CL_BCONST); buf_u8(cg.cur, 0); }
            else { buf_u8(cg.cur, CL_ICONST); buf_i64(cg.cur, 0); }
            buf_u8(cg.cur, CL_RET);
        }
        cg.terminated = 1;
    }

    cr_st_pop();
    cg.cur = NULL;
    extern Buf cr_last_body;
    cr_last_body = buf;
}

/* ---- disassembler (--emit-cranelift) ---- */
static const char *op_name(int op) {
    switch (op) {
        case CL_ICONST: return "ICONST"; case CL_FCONST: return "FCONST";
        case CL_BCONST: return "BCONST"; case CL_SCONST: return "SCONST";
        case CL_LOAD: return "LOAD"; case CL_STORE: return "STORE";
        case CL_ALLOCA: return "ALLOCA"; case CL_BINOP: return "BINOP";
        case CL_AND: return "AND"; case CL_OR: return "OR";
        case CL_NOT: return "NOT"; case CL_NEG: return "NEG";
        case CL_PROMOTE: return "PROMOTE"; case CL_CALL: return "CALL";
        case CL_RET: return "RET"; case CL_RET_VOID: return "RET_VOID";
        case CL_BR: return "BR"; case CL_BR_FALSE: return "BR_FALSE";
        case CL_LABEL: return "LABEL"; case CL_DROP: return "DROP";
        default: return "?";
    }
}
static void disasm(Buf *b, const char *fname) {
    printf("; function %s\n", fname);
    size_t p = 0;
    while (p < b->n) {
        unsigned char op = b->d[p++];
        printf("  %-10s", op_name(op));
        switch (op) {
            case CL_ICONST: { long long v = 0; for (int i=0;i<8;i++) v |= (long long)b->d[p+i]<<(8*i); p+=8; printf("%lld", v); break; }
            case CL_FCONST: { double v; memcpy(&v, &b->d[p], 8); p+=8; printf("%g", v); break; }
            case CL_BCONST: printf("%d", b->d[p++]); break;
            case CL_SCONST: { unsigned id=0; for(int i=0;i<4;i++) id|=(unsigned)b->d[p+i]<<(8*i); p+=4; printf("\"%s\"", id<(unsigned)cg.nstrs?cg.strs[id]:"?"); break; }
            case CL_LOAD: case CL_STORE: { unsigned s=b->d[p]|(b->d[p+1]<<8); p+=2; printf("slot %u", s); break; }
            case CL_ALLOCA: { unsigned s=b->d[p]|(b->d[p+1]<<8); p+=2; printf("slot %u ty %u", s, b->d[p++]); break; }
            case CL_BINOP: printf("op %u", b->d[p++]); break;
            case CL_NEG: printf("ty %u", b->d[p++]); break;
            case CL_CALL: { unsigned f=0; for(int i=0;i<4;i++) f|=(unsigned)b->d[p+i]<<(8*i); p+=4; unsigned na=b->d[p]|(b->d[p+1]<<8); p+=2; printf("fn %u nargs %u", f, na); break; }
            case CL_BR: case CL_BR_FALSE: case CL_LABEL: { unsigned l=0; for(int i=0;i<4;i++) l|=(unsigned)b->d[p+i]<<(8*i); p+=4; printf("L%u", l); break; }
            default: break;
        }
        printf("\n");
    }
    printf("\n");
}

/* ---- public API ---- */
void codegen_cranelift(Node *program, int emit_only, int argc, char **argv) {
    memset(&cg, 0, sizeof cg);
    cg.program = program;
    (void)argc;   /* програмата се пуска без аргументи (вж. baga_jit_run_main) */

    /* 1. таблица на функциите */
    for (int i = 0; i < program->items.len; i++) {
        Node *it = program->items.data[i];
        if (it->kind != NODE_FN || !it->fn_body) continue;
        Node *spec = find_ensures_spec(it->fn_name);
        if (spec) {
            /* impl */
            CrFunc *im = &cg.funcs[cg.nfuncs++];
            char imn[300]; snprintf(imn, sizeof imn, "__impl_%s", it->fn_name);
            im->jit_name = strdup(imn);
            im->call_name = NULL;
            im->fn = it; im->spec = spec; im->is_wrapper = 0;
            int impl_idx = cg.nfuncs - 1;
            /* wrapper */
            CrFunc *wr = &cg.funcs[cg.nfuncs++];
            wr->jit_name = strdup(it->fn_name);
            wr->call_name = it->fn_name;
            wr->fn = it; wr->spec = spec; wr->is_wrapper = 1;
            wr->impl_fn_id = RT_COUNT + impl_idx;
        } else {
            CrFunc *fe = &cg.funcs[cg.nfuncs++];
            const char *jn = strcmp(it->fn_name, "main") == 0 ? "main" : it->fn_name;
            fe->jit_name = strdup(jn);
            fe->call_name = it->fn_name;
            fe->fn = it; fe->spec = NULL; fe->is_wrapper = 0;
        }
    }

    /* 2. генерирай bytecode за всяка функция */
    Buf bodies[512];
    for (int i = 0; i < cg.nfuncs; i++) {
        if (cg.funcs[i].is_wrapper)
            cr_emit_wrapper(cg.funcs[i].fn, cg.funcs[i].spec);
        else
            cr_emit_fn_body(cg.funcs[i].fn);
        bodies[i] = cr_last_body;
    }

    if (emit_only) {
        printf("; низове:\n");
        for (int i = 0; i < cg.nstrs; i++) printf(";   [%d] \"%s\"\n", i, cg.strs[i]);
        printf("\n");
        for (int i = 0; i < cg.nfuncs; i++)
            disasm(&bodies[i], cg.funcs[i].jit_name);
        return;
    }

    /* 3. JIT: declare -> intern strings -> define -> run */
    void *jit = baga_jit_new();
    for (int i = 0; i < cg.nfuncs; i++) {
        Node *fn = cg.funcs[i].fn;
        int ret_ty = ty_code_tn(fn->ret_type);
        int np = fn->params.len;
        int ptys[64];
        for (int j = 0; j < np; j++)
            ptys[j] = ty_code_tn(fn->params.data[j]->param_type);
        baga_jit_declare(jit, cg.funcs[i].jit_name, ret_ty, ptys, np);
    }
    for (int i = 0; i < cg.nstrs; i++)
        baga_jit_intern_str(jit, (const unsigned char *)cg.strs[i], strlen(cg.strs[i]));
    for (int i = 0; i < cg.nfuncs; i++)
        baga_jit_define(jit, i, bodies[i].d, bodies[i].n);

    /* Програмата се изпълнява без аргументи (като C backend-а: system(bin_path)
     * не препраща нищо) → argc=1, та arg_count()==0 и arg(0)=="". */
    int rc = baga_jit_run_main(jit, 1, argv);
    baga_jit_free(jit);
    exit(rc);
}

#endif /* BAGA_CRANELIFT */
