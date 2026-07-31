#include "baga.h"

/* ============================================================
 *  Type helpers
 * ============================================================ */

Type *type_new(TypeKind kind) {
    Type *t = calloc(1, sizeof(Type));
    if (!t) { fprintf(stderr, "baga: out of memory\n"); exit(1); }
    t->kind = kind;
    return t;
}

Type *type_fn(Type *ret, Type **params, int nparams) {
    Type *t = type_new(TYPE_FN);
    t->ret = ret;
    t->params = params;
    t->nparams = nparams;
    return t;
}

const char *type_str(Type *t) {
    if (!t) return "void";
    switch (t->kind) {
        case TYPE_VOID:  return "void";
        case TYPE_BOOL:  return "bool";
        case TYPE_I32:   return "i32";
        case TYPE_I64:   return "i64";
        case TYPE_F64:   return "f64";
        case TYPE_STR:   return "str";
        case TYPE_ERROR: return "<грешка>";
        case TYPE_ARRAY: return "[T]";
        case TYPE_REF:   return "&T";
        case TYPE_STRUCT: return t->name ? t->name : "struct";
        case TYPE_FN:    return "fn";
    }
    return "?";
}

int type_eq(Type *a, Type *b) {
    if (!a || !b) return a == b;
    if (a->kind == TYPE_ERROR || b->kind == TYPE_ERROR) return 1;
    if (a->kind != b->kind) return 0;
    if (a->kind == TYPE_STRUCT) {
        return a->name && b->name && strcmp(a->name, b->name) == 0;
    }
    return 1;
}

/* ============================================================
 *  Effect helpers
 * ============================================================ */

void type_add_effect(Type *t, const char *effect) {
    if (!t || !effect) return;
    if (type_has_effect(t, effect)) return;
    t->effects = realloc(t->effects, sizeof(char *) * (size_t)(t->n_effects + 1));
    if (!t->effects) { fprintf(stderr, "baga: out of memory\n"); exit(1); }
    t->effects[t->n_effects++] = strdup(effect);
}

int type_has_effect(Type *t, const char *effect) {
    if (!t || !effect) return 0;
    for (int i = 0; i < t->n_effects; i++) {
        if (strcmp(t->effects[i], effect) == 0) return 1;
    }
    return 0;
}

void type_remove_effect(Type *t, const char *effect) {
    if (!t || !effect) return;
    for (int i = 0; i < t->n_effects; i++) {
        if (strcmp(t->effects[i], effect) == 0) {
            free(t->effects[i]);
            for (int j = i; j < t->n_effects - 1; j++)
                t->effects[j] = t->effects[j + 1];
            t->n_effects--;
            return;
        }
    }
}

void type_merge_effects(Type *dst, Type *src) {
    if (!dst || !src) return;
    for (int i = 0; i < src->n_effects; i++)
        type_add_effect(dst, src->effects[i]);
}

/* Map a type AST node to a Type */
static Type *resolve_type_node(Node *ty) {
    if (!ty) return type_new(TYPE_VOID);
    switch (ty->kind) {
        case NODE_TYPE:
            if (strcmp(ty->type_name, "i32") == 0)  return type_new(TYPE_I32);
            if (strcmp(ty->type_name, "i64") == 0)  return type_new(TYPE_I64);
            if (strcmp(ty->type_name, "f64") == 0)  return type_new(TYPE_F64);
            if (strcmp(ty->type_name, "bool") == 0) return type_new(TYPE_BOOL);
            if (strcmp(ty->type_name, "str") == 0)  return type_new(TYPE_STR);
            if (strcmp(ty->type_name, "void") == 0) return type_new(TYPE_VOID);
            {
                Type *t = type_new(TYPE_STRUCT);
                t->name = strdup(ty->type_name);
                return t;
            }
        case NODE_TYPE_REF: {
            Type *t = type_new(TYPE_REF);
            t->pointee = resolve_type_node(ty->inner_type);
            return t;
        }
        case NODE_TYPE_ARRAY: {
            Type *t = type_new(TYPE_ARRAY);
            t->elem = resolve_type_node(ty->inner_type);
            return t;
        }
        case NODE_TYPE_EFFECT: {
            Type *base = resolve_type_node(ty->inner_type);
            for (int i = 0; i < ty->n_effects; i++)
                type_add_effect(base, ty->effect_names[i]);
            return base;
        }
        default:
            return type_new(TYPE_ERROR);
    }
}

/* ============================================================
 *  Type environment
 * ============================================================ */

#define ENV_MAX 64
#define ENV_VARS 256
#define FNS_MAX  256

typedef struct {
    char *name;
    Type *type;
} EnvEntry;

typedef struct {
    EnvEntry entries[ENV_VARS];
    int count;
} EnvScope;

typedef struct {
    EnvScope scopes[ENV_MAX];
    int depth;

    /* function registry */
    struct {
        char *name;
        Type *fn_type;   /* TYPE_FN */
        Node *decl;      /* NODE_FN */
    } fns[FNS_MAX];
    int n_fns;

    /* struct registry */
    struct {
        char *name;
        Node *decl;
    } structs[FNS_MAX];
    int n_structs;

    /* enum registry */
    struct {
        char *name;
        Node *decl;
    } enums[FNS_MAX];
    int n_enums;

    /* enum variant → value mapping */
    struct {
        char *variant;
        char *enum_name;
        int value;
    } variants[FNS_MAX * 4];
    int n_variants;

    Checker *chk;
    const char *cur_fn;
    Type *cur_ret;   /* expected return type of current function */
    Type *cur_effects; /* accumulated effects in current function body */
} CheckCtx;

static void check_error(CheckCtx *ctx, SrcPos pos, const char *fmt, ...) {
    if (ctx->chk->n_errors >= BAGA_MAX_ERRORS) return;
    char *e = ctx->chk->errors[ctx->chk->n_errors++];
    int off = snprintf(e, 256, "%d:%d: ", pos.line, pos.col);
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(e + off, 256 - (size_t)off, fmt, ap);
    va_end(ap);
}

static void push_scope(CheckCtx *ctx) {
    if (ctx->depth < ENV_MAX) {
        ctx->scopes[ctx->depth].count = 0;
        ctx->depth++;
    }
}

static void pop_scope(CheckCtx *ctx) {
    if (ctx->depth > 0) ctx->depth--;
}

static void env_define(CheckCtx *ctx, const char *name, Type *type, SrcPos pos) {
    if (ctx->depth <= 0) return;
    EnvScope *s = &ctx->scopes[ctx->depth - 1];
    for (int i = 0; i < s->count; i++) {
        if (strcmp(s->entries[i].name, name) == 0) {
            check_error(ctx, pos, "повторно дефиниране на '%s'", name);
            return;
        }
    }
    if (s->count < ENV_VARS) {
        s->entries[s->count].name = (char *)name;
        s->entries[s->count].type = type;
        s->count++;
    }
}

static Type *env_lookup(CheckCtx *ctx, const char *name) {
    for (int d = ctx->depth - 1; d >= 0; d--) {
        EnvScope *s = &ctx->scopes[d];
        for (int i = s->count - 1; i >= 0; i--) {
            if (strcmp(s->entries[i].name, name) == 0)
                return s->entries[i].type;
        }
    }
    return NULL;
}

static Type *find_fn(CheckCtx *ctx, const char *name) {
    for (int i = 0; i < ctx->n_fns; i++) {
        if (strcmp(ctx->fns[i].name, name) == 0)
            return ctx->fns[i].fn_type;
    }
    return NULL;
}

/* ============================================================
 *  Type inference
 * ============================================================ */

static Type *infer(CheckCtx *ctx, Node *n);

static int is_numeric(Type *t) {
    return t && (t->kind == TYPE_I32 || t->kind == TYPE_I64 || t->kind == TYPE_F64);
}

static Type *numeric_promote(Type *a, Type *b) {
    if (a->kind == TYPE_F64 || b->kind == TYPE_F64) return type_new(TYPE_F64);
    if (a->kind == TYPE_I64 || b->kind == TYPE_I64) return type_new(TYPE_I64);
    return type_new(TYPE_I32);
}

static Type *infer_binary(CheckCtx *ctx, Node *n) {
    Type *lt = infer(ctx, n->left);
    Type *rt = infer(ctx, n->right);

    switch (n->bin_op) {
        case OP_ADD: case OP_SUB: case OP_MUL: case OP_DIV: case OP_MOD:
            if (!is_numeric(lt) || !is_numeric(rt)) {
                check_error(ctx, n->pos, "аритметична операция върху не-числови типове (%s, %s)",
                            type_str(lt), type_str(rt));
                return type_new(TYPE_ERROR);
            }
            return numeric_promote(lt, rt);

        case OP_EQ: case OP_NEQ: case OP_LT: case OP_GT: case OP_LE: case OP_GE:
            return type_new(TYPE_BOOL);

        case OP_AND: case OP_OR:
            return type_new(TYPE_BOOL);

        case OP_BIT_AND: case OP_BIT_OR: case OP_BIT_XOR:
        case OP_LSHIFT: case OP_RSHIFT:
            return type_new(TYPE_I64);
    }
    return type_new(TYPE_ERROR);
}

static Type *infer_call(CheckCtx *ctx, Node *n) {
    /* infer arg types */
    for (int i = 0; i < n->args.len; i++)
        infer(ctx, n->args.data[i]);

    /* builtins */
    if (n->callee->kind == NODE_IDENT) {
        const char *name = n->callee->name;

        if (strcmp(name, "print") == 0 || strcmp(name, "println") == 0) {
            n->callee->type = type_new(TYPE_VOID);
            return type_new(TYPE_VOID);
        }

        /* user function */
        Type *ft = find_fn(ctx, name);
        if (ft && ft->kind == TYPE_FN) {
            n->callee->type = ft;
            /* check arg count */
            if (n->args.len != ft->nparams) {
                check_error(ctx, n->pos, "'%s' очаква %d аргумента, получих %d",
                            name, ft->nparams, n->args.len);
            }
            Type *ret = ft->ret ? ft->ret : type_new(TYPE_VOID);
            /* propagate effects from function's return type */
            Type *result = type_new(ret->kind);
            type_merge_effects(result, ret);
            /* accumulate at function level */
            if (ctx->cur_effects)
                type_merge_effects(ctx->cur_effects, ret);
            return result;
        }

        check_error(ctx, n->pos, "непозната функция '%s'", name);
        return type_new(TYPE_ERROR);
    }

    infer(ctx, n->callee);
    return type_new(TYPE_ERROR);
}

static Type *infer(CheckCtx *ctx, Node *n) {
    if (!n) return type_new(TYPE_VOID);
    if (n->type) return n->type;  /* already inferred */

    Type *t = NULL;

    switch (n->kind) {
        case NODE_INT_LIT:
            t = type_new(TYPE_I64);
            break;

        case NODE_FLOAT_LIT:
            t = type_new(TYPE_F64);
            break;

        case NODE_STR_LIT:
            t = type_new(TYPE_STR);
            break;

        case NODE_BOOL_LIT:
            t = type_new(TYPE_BOOL);
            break;

        case NODE_IDENT: {
            /* check local env first */
            Type *vt = env_lookup(ctx, n->name);
            if (vt) { t = vt; break; }
            /* check function registry */
            Type *ft = find_fn(ctx, n->name);
            if (ft) { t = ft; break; }
            /* check enum variants */
            for (int vi = 0; vi < ctx->n_variants; vi++) {
                if (strcmp(ctx->variants[vi].variant, n->name) == 0) {
                    t = type_new(TYPE_I64);
                    break;
                }
            }
            if (t) break;
            /* builtins */
            if (strcmp(n->name, "print") == 0 || strcmp(n->name, "println") == 0) {
                t = type_new(TYPE_VOID);
                break;
            }
            check_error(ctx, n->pos, "недефинирана променлива '%s'", n->name);
            t = type_new(TYPE_ERROR);
            break;
        }

        case NODE_BINARY:
            t = infer_binary(ctx, n);
            break;

        case NODE_UNARY:
            t = infer(ctx, n->operand);
            if (n->un_op == UOP_NOT) t = type_new(TYPE_BOOL);
            if (n->un_op == UOP_REF) {
                Type *ref = type_new(TYPE_REF);
                ref->pointee = t;
                t = ref;
            }
            if (n->un_op == UOP_DEREF) {
                t = (t && t->kind == TYPE_REF && t->pointee) ? t->pointee : type_new(TYPE_ERROR);
            }
            break;

        case NODE_CALL:
            t = infer_call(ctx, n);
            break;

        case NODE_IF: {
            Type *ct = infer(ctx, n->cond);
            if (ct->kind != TYPE_BOOL && ct->kind != TYPE_ERROR) {
                check_error(ctx, n->cond->pos, "очаквах bool в условие, получих %s", type_str(ct));
            }
            Type *tt = infer(ctx, n->then_br);
            Type *et = n->else_br ? infer(ctx, n->else_br) : type_new(TYPE_VOID);
            t = type_eq(tt, et) ? tt : tt;
            break;
        }

        case NODE_BLOCK: {
            push_scope(ctx);
            t = type_new(TYPE_VOID);
            for (int i = 0; i < n->stmts.len; i++) {
                t = infer(ctx, n->stmts.data[i]);
            }
            pop_scope(ctx);
            break;
        }

        case NODE_INDEX:
            infer(ctx, n->obj);
            infer(ctx, n->index);
            t = type_new(TYPE_I64); /* TODO: array elem type */
            break;

        case NODE_FIELD: {
            Type *ot = infer(ctx, n->field_obj);
            /* resolve field type from struct definition */
            if (ot && ot->kind == TYPE_STRUCT && ot->name) {
                for (int si = 0; si < ctx->n_structs; si++) {
                    if (strcmp(ctx->structs[si].name, ot->name) == 0) {
                        Node *sdecl = ctx->structs[si].decl;
                        for (int fi = 0; fi < sdecl->fields.len; fi++) {
                            Node *fld = sdecl->fields.data[fi];
                            if (strcmp(fld->fld_name, n->field_name) == 0) {
                                t = resolve_type_node(fld->fld_type);
                                break;
                            }
                        }
                        break;
                    }
                }
            }
            if (!t) {
                check_error(ctx, n->pos, "непознато поле '.%s'", n->field_name);
                t = type_new(TYPE_ERROR);
            }
            break;
        }

        case NODE_ASSIGN:
            infer(ctx, n->assign_target);
            t = infer(ctx, n->assign_val);
            break;

        case NODE_RANGE:
            infer(ctx, n->range_lo);
            infer(ctx, n->range_hi);
            t = type_new(TYPE_I64);
            break;

        case NODE_STRUCT_LIT: {
            /* verify struct exists */
            int found = 0;
            for (int si = 0; si < ctx->n_structs; si++) {
                if (strcmp(ctx->structs[si].name, n->lit_name) == 0) {
                    found = 1;
                    break;
                }
            }
            if (!found) {
                check_error(ctx, n->pos, "непознат struct '%s'", n->lit_name);
            }
            /* infer field value types */
            for (int i = 0; i < n->lit_values.len; i++)
                infer(ctx, n->lit_values.data[i]);
            Type *st = type_new(TYPE_STRUCT);
            st->name = strdup(n->lit_name);
            t = st;
            break;
        }

        case NODE_TRY: {
            /* e? — infer e, propagate its effects to enclosing function */
            Type *et = infer(ctx, n->try_expr);
            t = type_new(et->kind);
            type_merge_effects(t, et);
            /* also accumulate at function level for checking */
            if (ctx->cur_effects)
                type_merge_effects(ctx->cur_effects, et);
            break;
        }

        case NODE_CATCH: {
            /* e catch !E => handler — remove effect E from e's type */
            Type *et = infer(ctx, n->catch_expr);
            infer(ctx, n->catch_handler);
            t = type_new(et->kind);
            /* copy all effects except the caught one */
            for (int i = 0; i < et->n_effects; i++) {
                if (strcmp(et->effects[i], n->catch_effect) != 0)
                    type_add_effect(t, et->effects[i]);
            }
            /* remove caught effect from function-level accumulator */
            if (ctx->cur_effects)
                type_remove_effect(ctx->cur_effects, n->catch_effect);
            break;
        }

        case NODE_LET: {
            Type *init_t = n->let_init ? infer(ctx, n->let_init) : type_new(TYPE_I64);
            Type *decl_t = n->let_type ? resolve_type_node(n->let_type) : init_t;
            env_define(ctx, n->let_name, decl_t, n->pos);
            t = type_new(TYPE_VOID);
            break;
        }

        case NODE_RETURN: {
            Type *rt = n->ret_val ? infer(ctx, n->ret_val) : type_new(TYPE_VOID);
            if (ctx->cur_ret && !type_eq(rt, ctx->cur_ret)) {
                check_error(ctx, n->pos, "връщам %s, но функцията очаква %s",
                            type_str(rt), type_str(ctx->cur_ret));
            }
            t = type_new(TYPE_VOID);
            break;
        }

        case NODE_WHILE: {
            Type *ct = infer(ctx, n->while_cond);
            if (ct->kind != TYPE_BOOL && ct->kind != TYPE_ERROR) {
                check_error(ctx, n->while_cond->pos, "очаквах bool в условие на while, получих %s",
                            type_str(ct));
            }
            infer(ctx, n->while_body);
            t = type_new(TYPE_VOID);
            break;
        }

        case NODE_FOR: {
            infer(ctx, n->for_iter);
            push_scope(ctx);
            env_define(ctx, n->for_var, type_new(TYPE_I64), n->pos);
            infer(ctx, n->for_body);
            pop_scope(ctx);
            t = type_new(TYPE_VOID);
            break;
        }

        case NODE_MATCH: {
            infer(ctx, n->match_expr);
            t = type_new(TYPE_VOID);
            for (int i = 0; i < n->match_arms.len; i++) {
                Node *arm = n->match_arms.data[i];
                if (arm->arm_pattern) infer(ctx, arm->arm_pattern);
                /* infer arm body; extract type from return if wrapped */
                Type *bt = infer(ctx, arm->arm_body);
                if (bt->kind == TYPE_VOID && arm->arm_body &&
                    arm->arm_body->kind == NODE_BLOCK &&
                    arm->arm_body->stmts.len > 0) {
                    Node *last = arm->arm_body->stmts.data[arm->arm_body->stmts.len - 1];
                    if (last->kind == NODE_RETURN && last->ret_val && last->ret_val->type)
                        bt = last->ret_val->type;
                }
                if (i == 0) t = bt;
            }
            break;
        }

        case NODE_MATCH_ARM:
            if (n->arm_pattern) infer(ctx, n->arm_pattern);
            t = infer(ctx, n->arm_body);
            break;

        case NODE_EXPR_STMT:
            t = infer(ctx, n->expr);
            break;

        default:
            t = type_new(TYPE_VOID);
            break;
    }

    n->type = t;
    return t;
}

/* ============================================================
 *  Function checking
 * ============================================================ */

static void check_fn(CheckCtx *ctx, Node *fn) {
    ctx->cur_fn = fn->fn_name;
    ctx->cur_ret = fn->ret_type ? resolve_type_node(fn->ret_type) : type_new(TYPE_VOID);
    ctx->cur_effects = type_new(TYPE_VOID); /* accumulator for body effects */

    push_scope(ctx);

    /* define params */
    for (int i = 0; i < fn->params.len; i++) {
        Node *p = fn->params.data[i];
        Type *pt = resolve_type_node(p->param_type);
        p->type = pt;
        env_define(ctx, p->param_name, pt, p->pos);
    }

    /* check body */
    if (fn->fn_body) {
        for (int i = 0; i < fn->fn_body->stmts.len; i++)
            infer(ctx, fn->fn_body->stmts.data[i]);
    }

    /* effect checking: unhandled effects in body vs declared effects */
    if (ctx->cur_effects) {
        for (int i = 0; i < ctx->cur_effects->n_effects; i++) {
            const char *eff = ctx->cur_effects->effects[i];
            if (!type_has_effect(ctx->cur_ret, eff)) {
                check_error(ctx, fn->pos,
                    "необработен ефект !%s във '%s' — декларирай го в return типа или го хвани с catch",
                    eff, fn->fn_name);
            }
        }
    }

    pop_scope(ctx);
    ctx->cur_fn = NULL;
    ctx->cur_ret = NULL;
    ctx->cur_effects = NULL;
}

/* ============================================================
 *  Public API
 * ============================================================ */

void check_program(Checker *c, Node *program) {
    CheckCtx ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.chk = c;

    push_scope(&ctx);

    /* pass 1: register all top-level names */
    for (int i = 0; i < program->items.len; i++) {
        Node *item = program->items.data[i];

        if (item->kind == NODE_FN) {
            /* build function type */
            Type *ret = item->ret_type ? resolve_type_node(item->ret_type) : type_new(TYPE_VOID);
            int np = item->params.len;
            Type **params = NULL;
            if (np > 0) {
                params = malloc(sizeof(Type *) * (size_t)np);
                for (int j = 0; j < np; j++)
                    params[j] = resolve_type_node(item->params.data[j]->param_type);
            }
            Type *ft = type_fn(ret, params, np);
            ft->name = strdup(item->fn_name);

            if (ctx.n_fns < FNS_MAX) {
                ctx.fns[ctx.n_fns].name = item->fn_name;
                ctx.fns[ctx.n_fns].fn_type = ft;
                ctx.fns[ctx.n_fns].decl = item;
                ctx.n_fns++;
            }
            item->type = ft;

        } else if (item->kind == NODE_STRUCT) {
            if (ctx.n_structs < FNS_MAX) {
                ctx.structs[ctx.n_structs].name = item->struct_name;
                ctx.structs[ctx.n_structs].decl = item;
                ctx.n_structs++;
            }
            Type *st = type_new(TYPE_STRUCT);
            st->name = strdup(item->struct_name);
            item->type = st;

        } else if (item->kind == NODE_ENUM) {
            if (ctx.n_enums < FNS_MAX) {
                ctx.enums[ctx.n_enums].name = item->enum_name;
                ctx.enums[ctx.n_enums].decl = item;
                ctx.n_enums++;
            }
            /* register variants */
            for (int j = 0; j < item->n_variants; j++) {
                if (ctx.n_variants < FNS_MAX * 4) {
                    ctx.variants[ctx.n_variants].variant = item->enum_variants[j];
                    ctx.variants[ctx.n_variants].enum_name = item->enum_name;
                    ctx.variants[ctx.n_variants].value = j;
                    ctx.n_variants++;
                }
            }
            Type *et = type_new(TYPE_I64);
            et->name = strdup(item->enum_name);
            item->type = et;
        }
    }

    /* pass 2: verify specs against functions */
    for (int i = 0; i < program->items.len; i++) {
        Node *item = program->items.data[i];
        if (item->kind != NODE_SPEC) continue;

        /* find the function this spec describes */
        Type *ft = find_fn(&ctx, item->spec_name);
        if (!ft) {
            check_error(&ctx, item->pos,
                "spec '%s' описва функция, която не съществува", item->spec_name);
            continue;
        }

        /* check input count */
        if (item->spec_inputs.len != ft->nparams) {
            check_error(&ctx, item->pos,
                "spec '%s': input има %d параметъра, но функцията има %d",
                item->spec_name, item->spec_inputs.len, ft->nparams);
        } else {
            /* check each input type */
            for (int j = 0; j < item->spec_inputs.len; j++) {
                Node *sp = item->spec_inputs.data[j];
                Type *spec_t = resolve_type_node(sp->param_type);
                if (!type_eq(spec_t, ft->params[j])) {
                    check_error(&ctx, sp->pos,
                        "spec '%s': параметър '%s' е %s в spec-а, но %s във функцията",
                        item->spec_name, sp->param_name,
                        type_str(spec_t), type_str(ft->params[j]));
                }
            }
        }

        /* check output type */
        if (item->spec_output) {
            Type *spec_ret = resolve_type_node(item->spec_output);
            Type *fn_ret = ft->ret ? ft->ret : type_new(TYPE_VOID);
            if (!type_eq(spec_ret, fn_ret)) {
                check_error(&ctx, item->pos,
                    "spec '%s': output е %s, но функцията връща %s",
                    item->spec_name, type_str(spec_ret), type_str(fn_ret));
            }
        }
    }

    /* pass 3: check function bodies */
    for (int i = 0; i < program->items.len; i++) {
        Node *item = program->items.data[i];
        if (item->kind == NODE_FN)
            check_fn(&ctx, item);
    }

    /* check that main exists */
    if (!find_fn(&ctx, "main")) {
        SrcPos pos = { 1, 1 };
        check_error(&ctx, pos, "липсва функция 'main'");
    }

    pop_scope(&ctx);
}
