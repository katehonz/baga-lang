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
        case TYPE_BYTES: return "bytes";
        case TYPE_ERROR: return "<грешка>";
        case TYPE_ARRAY: return "[T]";
        case TYPE_REF:   return "&T";
        case TYPE_STRUCT: return t->name ? t->name : "struct";
        case TYPE_FN:    return "fn";
        case TYPE_VEC: {
            if (!t->elem) return "Vec";
            static char buf[64];
            snprintf(buf, sizeof buf, "Vec<%s>", type_str(t->elem));
            return buf;
        }
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
    if (a->kind == TYPE_VEC) {
        /* Vec срещу Vec: различни само ако и двата знаят elem и той се различава;
         * Vec без elem (наследен код) съвпада с всеки Vec<T> */
        if (a->elem && b->elem && a->elem->kind != b->elem->kind) return 0;
        return 1;
    }
    return 1;
}

/* дали аргумент от тип `arg` може да се подаде на параметър от тип `param` */
static int type_assignable(Type *arg, Type *param) {
    if (!arg || !param) return 1;
    if (arg->kind == TYPE_ERROR || param->kind == TYPE_ERROR) return 1;
    /* цялочислено семейство: i32/i64 са съвместими (int литералите са i64) */
    if ((arg->kind == TYPE_I32 || arg->kind == TYPE_I64) &&
        (param->kind == TYPE_I32 || param->kind == TYPE_I64)) return 1;
    return type_eq(arg, param);
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
    /* същата грешка на същата позиция от два прохода — пазим само веднъж */
    if (ctx->chk->n_errors >= 2 &&
        strcmp(ctx->chk->errors[ctx->chk->n_errors - 2], e) == 0)
        ctx->chk->n_errors--;
}

/* Map a type AST node to a Type */
static Type *resolve_type_node(CheckCtx *ctx, Node *ty) {
    if (!ty) return type_new(TYPE_VOID);
    switch (ty->kind) {
        case NODE_TYPE:
            if (strcmp(ty->type_name, "i32") == 0)  return type_new(TYPE_I32);
            if (strcmp(ty->type_name, "i64") == 0)  return type_new(TYPE_I64);
            if (strcmp(ty->type_name, "f64") == 0)  return type_new(TYPE_F64);
            if (strcmp(ty->type_name, "bool") == 0) return type_new(TYPE_BOOL);
            if (strcmp(ty->type_name, "str") == 0)  return type_new(TYPE_STR);
            if (strcmp(ty->type_name, "bytes") == 0) return type_new(TYPE_BYTES);
            if (strcmp(ty->type_name, "void") == 0) return type_new(TYPE_VOID);
            if (strcmp(ty->type_name, "Vec") == 0) {
                Type *t = type_new(TYPE_VEC);
                if (ty->inner_type) {
                    /* Vec<T>: елементите са ограничени до i64 (i32 → i64), str и f64 */
                    Type *el = resolve_type_node(ctx, ty->inner_type);
                    if (el->kind == TYPE_I32) el = type_new(TYPE_I64);
                    if (el->kind != TYPE_I64 && el->kind != TYPE_STR &&
                        el->kind != TYPE_F64) {
                        check_error(ctx, ty->pos,
                            "Vec<T>: неподдържан елементен тип %s (поддържат се i64, str и f64)",
                            type_str(el));
                    } else {
                        t->elem = el;
                    }
                }
                return t;
            }
            {
                Type *t = type_new(TYPE_STRUCT);
                t->name = strdup(ty->type_name);
                return t;
            }
        case NODE_TYPE_REF: {
            Type *t = type_new(TYPE_REF);
            t->pointee = resolve_type_node(ctx, ty->inner_type);
            return t;
        }
        case NODE_TYPE_ARRAY: {
            /* [T] is sugar for Vec<T>: the growable baga_Vec, not a raw C
             * pointer. This lets Vec parameters be written as [i64] and used
             * with the vec_* builtins uniformly. */
            Type *t = type_new(TYPE_VEC);
            if (ty->inner_type) {
                Type *el = resolve_type_node(ctx, ty->inner_type);
                if (el->kind == TYPE_I32) el = type_new(TYPE_I64);
                if (el->kind != TYPE_I64 && el->kind != TYPE_STR &&
                    el->kind != TYPE_F64) {
                    check_error(ctx, ty->pos,
                        "[T]: неподдържан елементен тип %s (поддържат се i64, str и f64)",
                        type_str(el));
                } else {
                    t->elem = el;
                }
            }
            return t;
        }
        case NODE_TYPE_EFFECT: {
            Type *base = resolve_type_node(ctx, ty->inner_type);
            for (int i = 0; i < ty->n_effects; i++)
                type_add_effect(base, ty->effect_names[i]);
            return base;
        }
        default:
            return type_new(TYPE_ERROR);
    }
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

        /* user-defined (incl. extern) functions shadow builtins */
        Type *ft_user = find_fn(ctx, name);
        if (ft_user && ft_user->kind == TYPE_FN) {
            n->callee->type = ft_user;
            if (n->args.len != ft_user->nparams) {
                check_error(ctx, n->pos, "'%s' очаква %d аргумента, получих %d",
                            name, ft_user->nparams, n->args.len);
            }
            int check_n = n->args.len < ft_user->nparams ? n->args.len : ft_user->nparams;
            for (int i = 0; i < check_n; i++) {
                Type *at = n->args.data[i]->type;
                if (!type_assignable(at, ft_user->params[i])) {
                    check_error(ctx, n->pos,
                        "'%s': аргумент #%d е от тип %s, но параметърът е %s",
                        name, i + 1, type_str(at), type_str(ft_user->params[i]));
                }
            }
            Type *ret = ft_user->ret ? ft_user->ret : type_new(TYPE_VOID);
            Type *result = type_new(ret->kind);
            /* Vec<T>: keep the element type so e.g. a fn returning Vec<str>
             * can feed a Vec<str> parameter (the fresh Type has elem NULL,
             * which a later vec_get would otherwise fix to i64) */
            result->elem = ret->elem;
            /* struct: keep the name so a returned struct matches the declared
             * return type / struct parameters (type_eq compares by name) */
            result->name = ret->name;
            type_merge_effects(result, ret);
            if (ctx->cur_effects)
                type_merge_effects(ctx->cur_effects, ret);
            return result;
        }

        if (strcmp(name, "print") == 0 || strcmp(name, "println") == 0 ||
            strcmp(name, "write") == 0) {
            n->callee->type = type_new(TYPE_VOID);
            return type_new(TYPE_VOID);
        }

        /* типизирани вектори: Vec<T> — елементният тип се фиксира при
         * първия push/set чрез мутация на Type->elem (env пази същия
         * указател, така че фиксирането се разпространява до свързването) */
        if (strcmp(name, "vec_new") == 0) {
            n->callee->type = type_new(TYPE_VOID);
            return type_new(TYPE_VEC);      /* elem = NULL: неизвестен */
        }
        if (strcmp(name, "vec_push") == 0 || strcmp(name, "vec_set") == 0 ||
            strcmp(name, "vec_push_str") == 0 || strcmp(name, "vec_set_str") == 0) {
            int is_str_alias = (strstr(name, "_str") != NULL);
            int xidx = (strstr(name, "set") != NULL) ? 2 : 1;   /* индекс на елемента */
            n->callee->type = type_new(TYPE_VOID);
            if (n->args.len != xidx + 1) {
                check_error(ctx, n->pos, "'%s' очаква %d аргумента, получих %d",
                            name, xidx + 1, n->args.len);
                return type_new(TYPE_ERROR);
            }
            Type *vt = n->args.data[0]->type;
            if (!vt || vt->kind != TYPE_VEC) {
                check_error(ctx, n->pos, "'%s' върху не-вектор (%s)", name, type_str(vt));
                return type_new(TYPE_ERROR);
            }
            Type *xt = is_str_alias ? type_new(TYPE_STR) : n->args.data[xidx]->type;
            if (xt->kind == TYPE_I32) xt = type_new(TYPE_I64);
            if (!is_str_alias && xt->kind != TYPE_I64 && xt->kind != TYPE_STR &&
                xt->kind != TYPE_F64) {
                check_error(ctx, n->pos,
                    "%s: неподдържан елементен тип %s за Vec (поддържат се i64, str и f64)",
                    name, type_str(xt));
                return type_new(TYPE_ERROR);
            }
            if (!vt->elem) {
                vt->elem = xt;
            } else if (vt->elem->kind != xt->kind) {
                check_error(ctx, n->pos, "%s: елемент от тип %s, но векторът е %s",
                            name, type_str(xt), type_str(vt));
            }
            return type_new(TYPE_VOID);
        }
        if (strcmp(name, "vec_get") == 0 || strcmp(name, "vec_get_str") == 0) {
            int is_str_alias = (strstr(name, "_str") != NULL);
            n->callee->type = type_new(TYPE_VOID);
            Type *vt = n->args.len > 0 ? n->args.data[0]->type : NULL;
            if (is_str_alias) {
                if (vt && vt->kind == TYPE_VEC && !vt->elem) vt->elem = type_new(TYPE_STR);
                return type_new(TYPE_STR);
            }
            if (vt && vt->kind == TYPE_VEC) {
                /* Vec с неизвестен елемент: vec_get исторически е само за i64,
                 * фиксираме i64 (стар код с Vec параметри не се чупи) */
                if (!vt->elem) vt->elem = type_new(TYPE_I64);
                return vt->elem;
            }
            check_error(ctx, n->pos, "'%s' върху не-вектор (%s)", name, type_str(vt));
            return type_new(TYPE_ERROR);
        }
        if (strcmp(name, "vec_len") == 0) {
            n->callee->type = type_new(TYPE_VOID);
            return type_new(TYPE_I64);
        }
        if (strcmp(name, "vec_slice") == 0) {
            n->callee->type = type_new(TYPE_VOID);
            Type *vt = n->args.len > 0 ? n->args.data[0]->type : NULL;
            if (!vt || vt->kind != TYPE_VEC) {
                check_error(ctx, n->pos, "'vec_slice' върху не-вектор (%s)", type_str(vt));
                return type_new(TYPE_ERROR);
            }
            Type *r = type_new(TYPE_VEC);
            r->elem = vt->elem;
            return r;
        }
        if (strcmp(name, "vec_concat") == 0) {
            n->callee->type = type_new(TYPE_VOID);
            Type *vt = n->args.len > 0 ? n->args.data[0]->type : NULL;
            Type *wt = n->args.len > 1 ? n->args.data[1]->type : NULL;
            if (!vt || vt->kind != TYPE_VEC || !wt || wt->kind != TYPE_VEC) {
                check_error(ctx, n->pos, "'vec_concat' очаква два вектора");
                return type_new(TYPE_ERROR);
            }
            Type *r = type_new(TYPE_VEC);
            r->elem = vt->elem ? vt->elem : wt->elem;
            return r;
        }

        /* string / io builtins */
        struct { const char *name; TypeKind ret; int nparams; int has_io; } builtins[] = {
            {"len",       TYPE_I64, 1, 0},
            {"char_at",   TYPE_I64, 2, 0},
            {"substr",    TYPE_STR, 3, 0},
            {"concat",    TYPE_STR, 2, 0},
            {"read_file", TYPE_STR, 1, 1},
            {"chr",       TYPE_STR, 1, 0},
            {"ord",       TYPE_I64, 1, 0},
            {"str_eq",    TYPE_BOOL, 2, 0},
            {"arg_count", TYPE_I64, 0, 0},
            {"arg",       TYPE_STR, 1, 0},
            {"exit",      TYPE_VOID, 1, 0},
            {"eprintln",  TYPE_VOID, 1, 0},
            {"arena_new",   TYPE_I64, 0, 0},
            {"arena_alloc", TYPE_I64, 2, 0},
            {"arena_reset", TYPE_VOID, 1, 0},
            {"arena_free",  TYPE_VOID, 1, 0},
            {"bytes_len",   TYPE_I64, 1, 0},
            {"bytes_at",    TYPE_I64, 2, 0},
            {"bytes_slice", TYPE_BYTES, 3, 0},
            {"bytes_concat", TYPE_BYTES, 2, 0},
            {"bytes_of_str", TYPE_BYTES, 1, 0},
            {"str_of_bytes", TYPE_STR, 1, 0},
            {"hex_encode",  TYPE_STR, 1, 0},
            {"hex_decode",  TYPE_BYTES, 1, 0},
        };
        for (int bi = 0; bi < (int)(sizeof(builtins) / sizeof(builtins[0])); bi++) {
            if (strcmp(name, builtins[bi].name) == 0) {
                n->callee->type = type_new(TYPE_VOID);
                Type *ret = type_new(builtins[bi].ret);
                if (builtins[bi].has_io) {
                    type_add_effect(ret, "IO");
                    if (ctx->cur_effects) type_add_effect(ctx->cur_effects, "IO");
                }
                return ret;
            }
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

        case NODE_BYTES_LIT:
            t = type_new(TYPE_BYTES);
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

        case NODE_ELEM_REF:
            /* v[*] — the element value (i64 for Vec<i64>/[i64]); annotation-only */
            infer(ctx, n->elem_obj);
            t = type_new(TYPE_I64);
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
                                t = resolve_type_node(ctx, fld->fld_type);
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
            /* keep Vec elem / struct name, like a plain call result does */
            t->elem = et->elem;
            t->name = et->name;
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
            /* keep Vec elem / struct name, like a plain call result does */
            t->elem = et->elem;
            t->name = et->name;
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

        case NODE_TO_STR: {
            /* interpolation: convert inner expr to str (str/i64/bool) */
            Type *et = infer(ctx, n->to_str_expr);
            if (et->kind != TYPE_STR && et->kind != TYPE_I64 &&
                et->kind != TYPE_I32 && et->kind != TYPE_BOOL && et->kind != TYPE_ERROR) {
                check_error(ctx, n->pos, "неподдържан тип за интерполация: %s (str/i64/bool)", type_str(et));
            }
            t = type_new(TYPE_STR);
            type_merge_effects(t, et);
            if (ctx->cur_effects) type_merge_effects(ctx->cur_effects, et);
            break;
        }

        case NODE_LET: {
            Type *init_t = n->let_init ? infer(ctx, n->let_init) : type_new(TYPE_I64);
            Type *decl_t = n->let_type ? resolve_type_node(ctx, n->let_type) : init_t;
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

        case NODE_BREAK:
        case NODE_CONTINUE:
            t = type_new(TYPE_VOID);
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
    ctx->cur_ret = fn->ret_type ? resolve_type_node(ctx, fn->ret_type) : type_new(TYPE_VOID);
    ctx->cur_effects = type_new(TYPE_VOID); /* accumulator for body effects */

    push_scope(ctx);

    /* define params */
    for (int i = 0; i < fn->params.len; i++) {
        Node *p = fn->params.data[i];
        Type *pt = resolve_type_node(ctx, p->param_type);
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
            Type *ret = item->ret_type ? resolve_type_node(&ctx, item->ret_type) : type_new(TYPE_VOID);
            int np = item->params.len;
            Type **params = NULL;
            if (np > 0) {
                params = malloc(sizeof(Type *) * (size_t)np);
                for (int j = 0; j < np; j++)
                    params[j] = resolve_type_node(&ctx, item->params.data[j]->param_type);
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

            if (item->is_extern) {
                /* extern fn: params restricted to i64, f64, str (void is only
                 * meaningful as a return type — a void param breaks gcc) */
                for (int j = 0; j < item->params.len; j++) {
                    Node *pt = item->params.data[j]->param_type;
                    while (pt && pt->kind == NODE_TYPE_EFFECT) pt = pt->inner_type;
                    if (!pt || pt->kind != NODE_TYPE ||
                        (strcmp(pt->type_name, "i64") != 0 &&
                         strcmp(pt->type_name, "f64") != 0 &&
                         strcmp(pt->type_name, "str") != 0))
                        check_error(&ctx, item->pos,
                            "extern fn '%s': неподдържан тип на параметър (само i64, f64, str)",
                            item->fn_name);
                }
                Node *rt = item->ret_type;
                while (rt && rt->kind == NODE_TYPE_EFFECT) rt = rt->inner_type;
                if (rt && (rt->kind != NODE_TYPE ||
                    (strcmp(rt->type_name, "i64") != 0 &&
                     strcmp(rt->type_name, "f64") != 0 &&
                     strcmp(rt->type_name, "str") != 0 &&
                     strcmp(rt->type_name, "void") != 0)))
                    check_error(&ctx, item->pos,
                        "extern fn '%s': неподдържан връщан тип (само i64, f64, str, void)",
                        item->fn_name);
            }

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
                Type *spec_t = resolve_type_node(&ctx, sp->param_type);
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
            Type *spec_ret = resolve_type_node(&ctx, item->spec_output);
            Type *fn_ret = ft->ret ? ft->ret : type_new(TYPE_VOID);
            if (!type_eq(spec_ret, fn_ret)) {
                check_error(&ctx, item->pos,
                    "spec '%s': output е %s, но функцията връща %s",
                    item->spec_name, type_str(spec_ret), type_str(fn_ret));
            }
        }

        /* check ensures expressions (type-check in scope: inputs + output) */
        if (item->spec_ensures.len > 0) {
            Type *fn_ret = ft->ret ? ft->ret : type_new(TYPE_VOID);
            if (fn_ret->kind == TYPE_VOID) {
                check_error(&ctx, item->pos,
                    "spec '%s': ensures изисква функция с върнат тип",
                    item->spec_name);
            } else {
                push_scope(&ctx);
                for (int j = 0; j < item->spec_inputs.len; j++) {
                    Node *sp = item->spec_inputs.data[j];
                    env_define(&ctx, sp->param_name,
                               resolve_type_node(&ctx, sp->param_type), sp->pos);
                }
                env_define(&ctx, "output", fn_ret, item->pos);
                for (int j = 0; j < item->spec_ensures.len; j++) {
                    Node *en = item->spec_ensures.data[j];
                    Type *et = infer(&ctx, en->ensure_expr);
                    if (et->kind != TYPE_BOOL && et->kind != TYPE_ERROR) {
                        check_error(&ctx, en->pos,
                            "spec '%s': ensures #%d е %s, очаквах bool",
                            item->spec_name, j + 1, type_str(et));
                    }
                }
                pop_scope(&ctx);
            }
        }

        /* check requires expressions (scope: inputs only, no output) */
        if (item->spec_requires.len > 0) {
            push_scope(&ctx);
            for (int j = 0; j < item->spec_inputs.len; j++) {
                Node *sp = item->spec_inputs.data[j];
                env_define(&ctx, sp->param_name,
                           resolve_type_node(&ctx, sp->param_type), sp->pos);
            }
            for (int j = 0; j < item->spec_requires.len; j++) {
                Node *rq = item->spec_requires.data[j];
                Type *rt = infer(&ctx, rq->ensure_expr);
                if (rt->kind != TYPE_BOOL && rt->kind != TYPE_ERROR) {
                    check_error(&ctx, rq->pos,
                        "spec '%s': requires #%d е %s, очаквах bool",
                        item->spec_name, j + 1, type_str(rt));
                }
            }
            pop_scope(&ctx);
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
