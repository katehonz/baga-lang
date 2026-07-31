#include "baga.h"

/* ============================================================
 *  Phase 1 checker: scope tracking + basic validation
 *
 *  Full type inference and effect checking come in Phase 4.
 *  For now: catch undefined variables, duplicate definitions,
 *  and structural errors.
 * ============================================================ */

#define SCOPE_MAX 64
#define SCOPE_VARS 256

typedef struct {
    char *name;
    NodeKind decl_kind;  /* NODE_FN, NODE_LET, NODE_PARAM, NODE_STRUCT */
} ScopeEntry;

typedef struct {
    ScopeEntry entries[SCOPE_VARS];
    int count;
} Scope;

typedef struct {
    Scope  scopes[SCOPE_MAX];
    int    depth;
    Checker *chk;
    const char *cur_fn;  /* name of enclosing function */
} CheckCtx;

static void check_node(CheckCtx *ctx, Node *n);

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
    if (ctx->depth < SCOPE_MAX) {
        ctx->scopes[ctx->depth].count = 0;
        ctx->depth++;
    }
}

static void pop_scope(CheckCtx *ctx) {
    if (ctx->depth > 0) ctx->depth--;
}

static void define(CheckCtx *ctx, const char *name, NodeKind kind, SrcPos pos) {
    if (ctx->depth <= 0) return;
    Scope *s = &ctx->scopes[ctx->depth - 1];
    /* check duplicate in current scope */
    for (int i = 0; i < s->count; i++) {
        if (strcmp(s->entries[i].name, name) == 0) {
            check_error(ctx, pos, "повторно дефиниране на '%s'", name);
            return;
        }
    }
    if (s->count < SCOPE_VARS) {
        s->entries[s->count].name = (char *)name;
        s->entries[s->count].decl_kind = kind;
        s->count++;
    }
}

static ScopeEntry *lookup(CheckCtx *ctx, const char *name) {
    for (int d = ctx->depth - 1; d >= 0; d--) {
        Scope *s = &ctx->scopes[d];
        for (int i = s->count - 1; i >= 0; i--) {
            if (strcmp(s->entries[i].name, name) == 0)
                return &s->entries[i];
        }
    }
    return NULL;
}

/* ============================================================
 *  Expression checking
 * ============================================================ */

static void check_expr(CheckCtx *ctx, Node *n) {
    if (!n) return;

    switch (n->kind) {
        case NODE_INT_LIT:
        case NODE_FLOAT_LIT:
        case NODE_STR_LIT:
        case NODE_BOOL_LIT:
            break;

        case NODE_IDENT:
            if (!lookup(ctx, n->name)) {
                /* allow builtins */
                if (strcmp(n->name, "print") != 0 &&
                    strcmp(n->name, "println") != 0 &&
                    strcmp(n->name, "len") != 0) {
                    check_error(ctx, n->pos, "недефинирана променлива '%s'", n->name);
                }
            }
            break;

        case NODE_BINARY:
            check_expr(ctx, n->left);
            check_expr(ctx, n->right);
            break;

        case NODE_UNARY:
            check_expr(ctx, n->operand);
            break;

        case NODE_CALL:
            check_expr(ctx, n->callee);
            for (int i = 0; i < n->args.len; i++)
                check_expr(ctx, n->args.data[i]);
            break;

        case NODE_IF:
            check_expr(ctx, n->cond);
            check_node(ctx, n->then_br);
            if (n->else_br) check_node(ctx, n->else_br);
            break;

        case NODE_BLOCK:
            push_scope(ctx);
            for (int i = 0; i < n->stmts.len; i++)
                check_node(ctx, n->stmts.data[i]);
            pop_scope(ctx);
            break;

        case NODE_INDEX:
            check_expr(ctx, n->obj);
            check_expr(ctx, n->index);
            break;

        case NODE_FIELD:
            check_expr(ctx, n->field_obj);
            break;

        case NODE_ASSIGN:
            check_expr(ctx, n->assign_target);
            check_expr(ctx, n->assign_val);
            break;

        case NODE_RANGE:
            check_expr(ctx, n->range_lo);
            check_expr(ctx, n->range_hi);
            break;

        default:
            break;
    }
}

/* ============================================================
 *  Statement / declaration checking
 * ============================================================ */

static void check_node(CheckCtx *ctx, Node *n) {
    if (!n) return;

    switch (n->kind) {
        case NODE_LET:
            check_expr(ctx, n->let_init);
            define(ctx, n->let_name, NODE_LET, n->pos);
            break;

        case NODE_RETURN:
            if (!ctx->cur_fn) {
                check_error(ctx, n->pos, "'return' извън функция");
            }
            check_expr(ctx, n->ret_val);
            break;

        case NODE_WHILE:
            check_expr(ctx, n->while_cond);
            check_node(ctx, n->while_body);
            break;

        case NODE_FOR:
            check_expr(ctx, n->for_iter);
            push_scope(ctx);
            define(ctx, n->for_var, NODE_LET, n->pos);
            check_node(ctx, n->for_body);
            pop_scope(ctx);
            break;

        case NODE_EXPR_STMT:
            check_expr(ctx, n->expr);
            break;

        case NODE_BLOCK:
            check_expr(ctx, n);
            break;

        case NODE_IF:
            check_expr(ctx, n);
            break;

        default:
            break;
    }
}

static void check_fn(CheckCtx *ctx, Node *fn) {
    ctx->cur_fn = fn->fn_name;
    push_scope(ctx);

    /* define params */
    for (int i = 0; i < fn->params.len; i++) {
        Node *p = fn->params.data[i];
        define(ctx, p->param_name, NODE_PARAM, p->pos);
    }

    /* check body */
    if (fn->fn_body) {
        /* body is a block — check its statements in current scope */
        for (int i = 0; i < fn->fn_body->stmts.len; i++)
            check_node(ctx, fn->fn_body->stmts.data[i]);
    }

    pop_scope(ctx);
    ctx->cur_fn = NULL;
}

/* ============================================================
 *  Public API
 * ============================================================ */

void check_program(Checker *c, Node *program) {
    CheckCtx ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.chk = c;
    ctx.depth = 0;

    /* global scope */
    push_scope(&ctx);

    /* first pass: register all top-level names */
    for (int i = 0; i < program->items.len; i++) {
        Node *item = program->items.data[i];
        if (item->kind == NODE_FN) {
            define(&ctx, item->fn_name, NODE_FN, item->pos);
        } else if (item->kind == NODE_STRUCT) {
            define(&ctx, item->struct_name, NODE_STRUCT, item->pos);
        }
    }

    /* second pass: check bodies */
    for (int i = 0; i < program->items.len; i++) {
        Node *item = program->items.data[i];
        if (item->kind == NODE_FN) {
            check_fn(&ctx, item);
        }
    }

    /* check that main exists */
    if (!lookup(&ctx, "main")) {
        SrcPos pos = { 1, 1 };
        check_error(&ctx, pos, "липсва функция 'main'");
    }

    pop_scope(&ctx);
}
