/* parser_decl.c — част от parser.c (виж pr_proto.h). */
#include "baga.h"
#include "parser_impl.h"

/* ============================================================
 *  Statement parsing
 * ============================================================ */

/* ---- W3.2: world g { … } — scoped world блок (desugaring в парсера) ----
 * Свежда се до `let g = world_new(); …; world_free(g)` с инжектиране на
 * world_free(g) преди всеки изход, който напуска блока:
 *   • return v (v не е g): v се връзва в temp, за да се оцени ПРЕДИ free-а;
 *     `return g` изнася собствеността (W3.1 гарация) — free не се инжектира;
 *   • break/continue, които не са в цикъл В блока (напускат блока);
 *   • raise — като return (payload-ът се връзва в temp).
 * Свитата форма е обикновена let/free — целият checker/verifier протокол
 * (W1.6 seatbelt, HK_WORLD) работи без промяна. Граници: raise/`?` в дълбока
 * изразна позиция не се инжектира (пътят дивергира — регионът остава вдигнат:
 * leak, не UB); ръчен world_free(g) в блока е compile грешка (двоен free). */
static int w32_ctr = 0;

static Node *w32_ident(const char *name, SrcPos pos) {
    Node *n = node_alloc(NODE_IDENT, pos);
    n->name = strdup(name);
    return n;
}

static Node *w32_call1(const char *fn, Node *arg, SrcPos pos) {
    Node *c = node_alloc(NODE_CALL, pos);
    c->callee = w32_ident(fn, pos);
    c->args.len = 0; c->args.cap = 0; c->args.data = NULL;
    if (arg) vec_push(c->args, arg);
    return c;
}

static Node *w32_free_stmt(const char *name, SrcPos pos) {
    Node *s = node_alloc(NODE_EXPR_STMT, pos);
    s->expr = w32_call1("world_free", w32_ident(name, pos), pos);
    return s;
}

/* дали изразът споменава name (стойност, която напуска блока, се оценява
 * предварително — иначе free-ът я прави use-after-free) */
static int w32_uses(Node *e, const char *name) {
    if (!e) return 0;
    switch (e->kind) {
    case NODE_IDENT: return e->name && strcmp(e->name, name) == 0;
    case NODE_PATH:
    case NODE_INT_LIT: case NODE_FLOAT_LIT: case NODE_STR_LIT:
    case NODE_BYTES_LIT: case NODE_BOOL_LIT: case NODE_LAMBDA:
        return 0;
    case NODE_BINARY: return w32_uses(e->left, name) || w32_uses(e->right, name);
    case NODE_UNARY: return w32_uses(e->operand, name);
    case NODE_CALL:
        for (int i = 0; i < e->args.len; i++) if (w32_uses(e->args.data[i], name)) return 1;
        return 0;
    case NODE_IF: return w32_uses(e->cond, name) || w32_uses(e->then_br, name) || w32_uses(e->else_br, name);
    case NODE_INDEX: return w32_uses(e->obj, name) || w32_uses(e->index, name);
    case NODE_ELEM_REF: return w32_uses(e->elem_obj, name);
    case NODE_FIELD: return w32_uses(e->field_obj, name);
    case NODE_RANGE: return w32_uses(e->range_lo, name) || w32_uses(e->range_hi, name);
    case NODE_STRUCT_LIT:
        for (int i = 0; i < e->lit_values.len; i++) if (w32_uses(e->lit_values.data[i], name)) return 1;
        return 0;
    case NODE_TRY: return w32_uses(e->try_expr, name);
    case NODE_CATCH: return w32_uses(e->catch_expr, name) || w32_uses(e->catch_handler, name);
    case NODE_RAISE: return w32_uses(e->raise_payload, name);
    case NODE_TO_STR: return w32_uses(e->to_str_expr, name);
    case NODE_MATCH:
        if (w32_uses(e->match_expr, name)) return 1;
        for (int i = 0; i < e->match_arms.len; i++) if (w32_uses(e->match_arms.data[i], name)) return 1;
        return 0;
    case NODE_MATCH_ARM: return w32_uses(e->arm_pattern, name) || w32_uses(e->arm_body, name);
    default: return 1;   /* непознат възел — консервативно „споменава" */
    }
}

/* едно изявление → нула или повече, с инжектирани free-ове (възлите се
 * ПРЕМЕСТВАТ, не се клонират) */
static void w32_inject_stmt(NodeVec *dst, Node *s, const char *name, SrcPos pos, int in_loop);

static void w32_inject_vec(NodeVec *dst, NodeVec *src, const char *name, SrcPos pos, int in_loop) {
    for (int i = 0; i < src->len; i++)
        w32_inject_stmt(dst, src->data[i], name, pos, in_loop);
}

/* рамо на if (блок, вложено if при else-if, или израз) — на място */
static void w32_inject_arm(Node **arm, const char *name, SrcPos pos, int in_loop) {
    if (!arm || !*arm) return;
    if ((*arm)->kind == NODE_BLOCK) {
        NodeVec nv = {0};
        w32_inject_vec(&nv, &(*arm)->stmts, name, pos, in_loop);
        free((*arm)->stmts.data);
        (*arm)->stmts = nv;
    } else if ((*arm)->kind == NODE_IF) {
        Node *iff = *arm;
        w32_inject_arm(&iff->then_br, name, pos, in_loop);
        w32_inject_arm(&iff->else_br, name, pos, in_loop);
    }
    /* друг вид рамо (израз) — не съдържа изходи от блока */
}

static void w32_inject_stmt(NodeVec *dst, Node *s, const char *name, SrcPos pos, int in_loop) {
    if (!s) return;
    switch (s->kind) {
    case NODE_RETURN: {
        if (s->ret_val && s->ret_val->kind == NODE_IDENT &&
            s->ret_val->name && strcmp(s->ret_val->name, name) == 0) {
            vec_push(*dst, s);   /* return g — собствеността излиза */
            return;
        }
        if (s->ret_val && w32_uses(s->ret_val, name)) {
            /* оцени стойността ПРЕДИ free-а */
            char tn[64]; snprintf(tn, sizeof tn, "__w32t%d", w32_ctr++);
            Node *lt = node_alloc(NODE_LET, pos);
            lt->let_name = strdup(tn); lt->is_mut = 0; lt->let_type = NULL;
            lt->let_init = s->ret_val;
            s->ret_val = w32_ident(tn, pos);
            vec_push(*dst, lt);
            vec_push(*dst, w32_free_stmt(name, pos));
            vec_push(*dst, s);
            return;
        }
        vec_push(*dst, w32_free_stmt(name, pos));
        vec_push(*dst, s);
        return;
    }
    case NODE_BREAK: case NODE_CONTINUE:
        if (!in_loop) vec_push(*dst, w32_free_stmt(name, pos));   /* напускат блока */
        vec_push(*dst, s);
        return;
    case NODE_RAISE: {
        if (s->raise_payload && w32_uses(s->raise_payload, name)) {
            char tn[64]; snprintf(tn, sizeof tn, "__w32t%d", w32_ctr++);
            Node *lt = node_alloc(NODE_LET, pos);
            lt->let_name = strdup(tn); lt->is_mut = 0; lt->let_type = NULL;
            lt->let_init = s->raise_payload;
            s->raise_payload = w32_ident(tn, pos);
            vec_push(*dst, lt);
        }
        vec_push(*dst, w32_free_stmt(name, pos));
        vec_push(*dst, s);
        return;
    }
    case NODE_EXPR_STMT:
        if (s->expr && s->expr->kind == NODE_RAISE) {
            /* raise като изявление — същата обработка, но възелът е израз */
            Node *r = s->expr;
            if (r->raise_payload && w32_uses(r->raise_payload, name)) {
                char tn[64]; snprintf(tn, sizeof tn, "__w32t%d", w32_ctr++);
                Node *lt = node_alloc(NODE_LET, pos);
                lt->let_name = strdup(tn); lt->is_mut = 0; lt->let_type = NULL;
                lt->let_init = r->raise_payload;
                r->raise_payload = w32_ident(tn, pos);
                vec_push(*dst, lt);
            }
            vec_push(*dst, w32_free_stmt(name, pos));
            vec_push(*dst, s);
            return;
        }
        vec_push(*dst, s);
        return;
    case NODE_BLOCK:
        {
            NodeVec nv = {0};
            w32_inject_vec(&nv, &s->stmts, name, pos, in_loop);
            free(s->stmts.data);
            s->stmts = nv;
        }
        vec_push(*dst, s);
        return;
    case NODE_IF:
        /* инжекция ВЪТРЕ в раменете (блокове или вложени if при else-if);
         * самият if-възел остава цял */
        w32_inject_arm(&s->then_br, name, pos, in_loop);
        w32_inject_arm(&s->else_br, name, pos, in_loop);
        vec_push(*dst, s);
        return;
    case NODE_WHILE: case NODE_FOR:
        /* тялото е блок; вътре break/continue не напускат световния блок */
        {
            Node *body = (s->kind == NODE_WHILE) ? s->while_body : s->for_body;
            NodeVec nv = {0};
            if (body) w32_inject_vec(&nv, &body->stmts, name, pos, in_loop + 1);
            if (body) { free(body->stmts.data); body->stmts = nv; }
        }
        vec_push(*dst, s);
        return;
    case NODE_MATCH:
        for (int i = 0; i < s->match_arms.len; i++) {
            Node *a = s->match_arms.data[i];
            if (!a || a->kind != NODE_MATCH_ARM || !a->arm_body) continue;
            NodeVec nv = {0};
            w32_inject_vec(&nv, &a->arm_body->stmts, name, pos, in_loop);
            free(a->arm_body->stmts.data);
            a->arm_body->stmts = nv;
        }
        vec_push(*dst, s);
        return;
    default:
        vec_push(*dst, s);
        return;
    }
}

/* свежда world name { body } до вложен блок:
 *   { let name = world_new(); …; world_free(name) }
 * Обвивката пази scope-а на блока (sibling блокове не си бъркат имената);
 * trailing free-ът липсва при сигурен изход (return/raise напускат с/без
 * стойност), а преди всеки изход free-ът е инжектиран. */
static int w32_stmt_exits(Node *s);

static int w32_vec_exits(NodeVec *v) {
    return v->len > 0 && w32_stmt_exits(v->data[v->len - 1]);
}

static int w32_arm_exits(Node *arm) {
    if (!arm) return 0;
    if (arm->kind == NODE_BLOCK) return w32_vec_exits(&arm->stmts);
    if (arm->kind == NODE_IF) return w32_stmt_exits(arm);
    return 0;
}

/* дали изписанието НЕ може да продължи нататък (return/raise/изход от блока) */
static int w32_stmt_exits(Node *s) {
    if (!s) return 0;
    switch (s->kind) {
    case NODE_RETURN: case NODE_BREAK: case NODE_CONTINUE: case NODE_RAISE:
        return 1;
    case NODE_EXPR_STMT:
        return s->expr && s->expr->kind == NODE_RAISE;
    case NODE_BLOCK: return w32_vec_exits(&s->stmts);
    case NODE_IF: return w32_arm_exits(s->then_br) && w32_arm_exits(s->else_br);
    default: return 0;
    }
}

static Node *w32_lower(NodeVec *src, const char *name, SrcPos pos) {
    Node *blk = node_alloc(NODE_BLOCK, pos);
    blk->stmts.len = 0; blk->stmts.cap = 0; blk->stmts.data = NULL;
    Node *lt = node_alloc(NODE_LET, pos);
    lt->let_name = strdup(name); lt->is_mut = 0; lt->let_type = NULL;
    lt->let_init = w32_call1("world_new", NULL, pos);
    vec_push(blk->stmts, lt);
    NodeVec body = {0};
    w32_inject_vec(&body, src, name, pos, 0);
    /* trailing free само при възможен fall-through — след безусловен изход е
     * мъртъв код, а мъртвият world_free(g) след free-а по-горе лъжеше и
     * seatbelt-а („използване след free"), и missing-return проверката */
    int exits = w32_vec_exits(&body);
    for (int i = 0; i < body.len; i++) vec_push(blk->stmts, body.data[i]);
    free(body.data);
    if (!exits)
        vec_push(blk->stmts, w32_free_stmt(name, pos));
    free(src->data);
    src->data = NULL; src->len = 0; src->cap = 0;
    return blk;
}

Node *parse_block(Parser *p) {
    SrcPos pos = cur(p)->pos;
    expect(p, TOK_LBRACE);
    Node *block = node_alloc(NODE_BLOCK, pos);
    block->stmts.len = 0; block->stmts.cap = 0; block->stmts.data = NULL;

    while (!check(p, TOK_RBRACE) && !check(p, TOK_EOF)) {
        /* W3.2: world g { … } — contextual ключова дума (world си остава
         * валиден идентификатор извън този контекст: IDENT + IDENT + `{`) */
        if (check(p, TOK_IDENT) && cur(p)->text && strcmp(cur(p)->text, "world") == 0 &&
            p->pos + 2 < p->len && p->tokens[p->pos + 1].kind == TOK_IDENT &&
            p->tokens[p->pos + 2].kind == TOK_LBRACE) {
            SrcPos wpos = cur(p)->pos;
            advance(p);
            char *wname = expect_ident(p);
            Node *body = parse_block(p);
            vec_push(block->stmts, w32_lower(&body->stmts, wname, wpos));
            free(wname);
            node_free(body);
            continue;
        }
        Node *s = parse_stmt(p);
        if (s) vec_push(block->stmts, s);
    }
    expect(p, TOK_RBRACE);
    return block;
}

Node *parse_stmt(Parser *p) {
    SrcPos pos = cur(p)->pos;

    /* let */
    if (check(p, TOK_LET)) {
        advance(p);
        int is_mut = match(p, TOK_MUT);
        char *name = expect_ident(p);
        Node *type = NULL;
        if (match(p, TOK_COLON)) {
            type = parse_type(p);
        }
        expect(p, TOK_ASSIGN);
        Node *init = parse_expr(p);
        /* optional semicolon */
        match(p, TOK_SEMICOLON);

        Node *n = node_alloc(NODE_LET, pos);
        n->let_name = name;
        n->is_mut = is_mut;
        n->let_type = type;
        n->let_init = init;
        return n;
    }

    /* return */
    if (check(p, TOK_RETURN)) {
        advance(p);
        Node *n = node_alloc(NODE_RETURN, pos);
        n->ret_val = NULL;
        if (!check(p, TOK_RBRACE) && !check(p, TOK_SEMICOLON) && !check(p, TOK_EOF)) {
            n->ret_val = parse_expr(p);
        }
        match(p, TOK_SEMICOLON);
        return n;
    }

    /* break */
    if (check(p, TOK_BREAK)) {
        advance(p);
        match(p, TOK_SEMICOLON);
        return node_alloc(NODE_BREAK, pos);
    }

    /* continue */
    if (check(p, TOK_CONTINUE)) {
        advance(p);
        match(p, TOK_SEMICOLON);
        return node_alloc(NODE_CONTINUE, pos);
    }

    /* invariant e1, e2, ... — statement-level annotation for --verify:
       channel content invariants (c[*] <cmp> <lin>) and scalar assumptions */
    if (check(p, TOK_IDENT) && cur(p)->text && strcmp(cur(p)->text, "invariant") == 0) {
        advance(p);
        Node *n = node_alloc(NODE_INVARIANT, pos);
        n->inv_exprs.len = 0; n->inv_exprs.cap = 0; n->inv_exprs.data = NULL;
        vec_push(n->inv_exprs, parse_expr(p));
        while (match(p, TOK_COMMA))
            vec_push(n->inv_exprs, parse_expr(p));
        match(p, TOK_SEMICOLON);
        return n;
    }

    /* while (with optional `invariant e1, e2, ...` clause for --verify) */
    if (check(p, TOK_WHILE)) {
        advance(p);
        Node *n = node_alloc(NODE_WHILE, pos);
        n->while_invariants.len = 0; n->while_invariants.cap = 0; n->while_invariants.data = NULL;
        n->while_cond = parse_expr(p);
        if (check(p, TOK_IDENT) && cur(p)->text && strcmp(cur(p)->text, "invariant") == 0) {
            advance(p);
            vec_push(n->while_invariants, parse_expr(p));
            while (match(p, TOK_COMMA))
                vec_push(n->while_invariants, parse_expr(p));
        }
        n->while_body = parse_block(p);
        return n;
    }

    /* for */
    if (check(p, TOK_FOR)) {
        advance(p);
        char *var = expect_ident(p);
        expect(p, TOK_IN);
        Node *iter = parse_expr(p);
        Node *body = parse_block(p);
        Node *n = node_alloc(NODE_FOR, pos);
        n->for_var = var;
        n->for_iter = iter;
        n->for_body = body;
        return n;
    }

    /* if (as statement) */
    if (check(p, TOK_IF)) {
        return parse_expr(p);
    }

    /* nested block as statement — иначе `{ }` минава през parse_expr и
     * се увива в EXPR_STMT, а emit_expr няма NODE_BLOCK (C мълчаливо
     * гълташе тялото; LLVM отказваше с AST възел #11). */
    if (check(p, TOK_LBRACE)) {
        return parse_block(p);
    }

    /* expression statement */
    Node *e = parse_expr(p);
    match(p, TOK_SEMICOLON);
    Node *n = node_alloc(NODE_EXPR_STMT, pos);
    n->expr = e;
    return n;
}

/* ============================================================
 *  Top-level declarations
 * ============================================================ */

static Node *parse_fn(Parser *p) {
    SrcPos pos = cur(p)->pos;
    expect(p, TOK_FN);
    char *name = expect_ident(p);

    /* M21: типови параметри — fn f<T, U>(...) */
    VEC(char *) type_params = {0};
    VEC(char *) param_bounds = {0};
    if (match(p, TOK_LT)) {
        do {
            Token *tp = expect(p, TOK_IDENT);
            vec_push(type_params, tp->text ? strdup(tp->text) : strdup("T"));
            /* M23: опционален trait bound — fn f<T: Show>(...) */
            char *bnd = NULL;
            if (match(p, TOK_COLON)) {
                Token *bt = expect(p, TOK_IDENT);
                bnd = bt->text ? strdup(bt->text) : strdup("");
            }
            vec_push(param_bounds, bnd);
        } while (match(p, TOK_COMMA));
        expect_gt_split(p);
    }

    expect(p, TOK_LPAREN);
    NodeVec params = {0};
    if (!check(p, TOK_RPAREN)) {
        do {
            SrcPos ppos = cur(p)->pos;
            char *pname = expect_ident(p);
            Node *ptype = NULL;
            /* M23: `self` без тип в trait декларацията; имплицитният тип
             * е на имплементиращия тип */
            if (strcmp(pname, "self") == 0 && !check(p, TOK_COLON)) {
                ptype = NULL;
            } else {
                expect(p, TOK_COLON);
                ptype = parse_type(p);
            }
            Node *param = node_alloc(NODE_PARAM, ppos);
            param->param_name = pname;
            param->param_type = ptype;
            vec_push(params, param);
        } while (match(p, TOK_COMMA));
    }
    expect(p, TOK_RPAREN);

    Node *ret_type = NULL;
    if (match(p, TOK_ARROW)) {
        ret_type = parse_type(p);
        ret_type = parse_type_with_effects(p, ret_type);
    }

    /* forward declaration: fn name(...) -> T  (no body) */
    if (!check(p, TOK_LBRACE)) {
        Node *fn = node_alloc(NODE_FN, pos);
        fn->fn_name = name;
        fn->params = params;
        fn->ret_type = ret_type;
        fn->fn_body = NULL;
        fn->type_params = type_params.data;
        fn->n_type_params = type_params.len;
        fn->param_bounds = param_bounds.data;
        return fn;
    }

    Node *body = parse_block(p);

    Node *fn = node_alloc(NODE_FN, pos);
    fn->fn_name = name;
    fn->params = params;
    fn->ret_type = ret_type;
    fn->fn_body = body;
    fn->type_params = type_params.data;
    fn->n_type_params = type_params.len;
    fn->param_bounds = param_bounds.data;
    return fn;
}

static Node *parse_struct(Parser *p) {
    SrcPos pos = cur(p)->pos;
    expect(p, TOK_STRUCT);
    char *name = expect_ident(p);

    /* M24: generic struct — struct Pair<A, B> { ... } */
    VEC(char *) type_params = {0};
    if (match(p, TOK_LT)) {
        do {
            Token *tp = expect(p, TOK_IDENT);
            vec_push(type_params, tp->text ? strdup(tp->text) : strdup("T"));
        } while (match(p, TOK_COMMA));
        expect_gt_split(p);
    }

    expect(p, TOK_LBRACE);
    NodeVec fields = {0};
    while (!check(p, TOK_RBRACE) && !check(p, TOK_EOF)) {
        SrcPos fpos = cur(p)->pos;
        char *fname = expect_ident(p);
        expect(p, TOK_COLON);
        Node *ftype = parse_type(p);
        Node *fld = node_alloc(NODE_FIELD_DECL, fpos);
        fld->fld_name = fname;
        fld->fld_type = ftype;
        vec_push(fields, fld);
        if (!check(p, TOK_RBRACE)) match(p, TOK_COMMA);
    }
    expect(p, TOK_RBRACE);

    Node *s = node_alloc(NODE_STRUCT, pos);
    s->struct_name = name;
    s->fields = fields;
    s->struct_params = type_params.data;
    s->n_struct_params = type_params.len;
    return s;
}

/* M23: trait Name { fn m(params) -> Ret [!E]; ... } — декларация на
 * методи (без тела); impl-ът ги реализира per тип. */
static Node *parse_trait(Parser *p) {
    SrcPos pos = cur(p)->pos;
    expect(p, TOK_TRAIT);
    char *name = expect_ident(p);
    expect(p, TOK_LBRACE);
    NodeVec methods = {0};
    while (!check(p, TOK_RBRACE) && !check(p, TOK_EOF)) {
        vec_push(methods, parse_fn(p));
    }
    expect(p, TOK_RBRACE);
    Node *tr = node_alloc(NODE_TRAIT, pos);
    tr->trait_name = name;
    tr->trait_methods = methods;
    return tr;
}

/* M23: impl Trait for Type { fn m(...) -> R { body } ... } — методите са
 * обикновени fn-и с вътрешно име "Trait.Type.m" (статичен dispatch). */
static Node *parse_impl(Parser *p) {
    SrcPos pos = cur(p)->pos;
    expect(p, TOK_IMPL);
    char *trait_name = expect_ident(p);
    expect(p, TOK_FOR);
    Node *ty = parse_type(p);
    expect(p, TOK_LBRACE);
    NodeVec methods = {0};
    while (!check(p, TOK_RBRACE) && !check(p, TOK_EOF)) {
        Node *m = parse_fn(p);
        m->fn_trait = trait_name;
        vec_push(methods, m);
    }
    expect(p, TOK_RBRACE);
    Node *im = node_alloc(NODE_IMPL, pos);
    im->impl_trait = trait_name;
    im->impl_type = ty;
    im->impl_methods = methods;
    return im;
}

Node *parse_spec(Parser *p) {    SrcPos pos = cur(p)->pos;
    expect(p, TOK_SPEC);

    /* spec name = function name (identifier) */
    char *name = expect_ident(p);

    expect(p, TOK_LBRACE);

    NodeVec inputs = {0};
    Node *output = NULL;
    VEC(char *) guarantees = {0};
    NodeVec ensures = {0};
    NodeVec requires = {0};
    Node *decreases = NULL;

    while (!check(p, TOK_RBRACE) && !check(p, TOK_EOF)) {
        if (check(p, TOK_IDENT) && cur(p)->text && strcmp(cur(p)->text, "input") == 0) {
            advance(p);
            expect(p, TOK_COLON);
            while (check(p, TOK_IDENT) &&
                   strcmp(cur(p)->text, "output") != 0 &&
                   strcmp(cur(p)->text, "guarantees") != 0 &&
                   strcmp(cur(p)->text, "requires") != 0 &&
                   strcmp(cur(p)->text, "ensures") != 0 &&
                   strcmp(cur(p)->text, "decreases") != 0) {
                SrcPos ppos = cur(p)->pos;
                char *pname = expect_ident(p);
                expect(p, TOK_COLON);
                Node *ptype = parse_type(p);
                Node *param = node_alloc(NODE_PARAM, ppos);
                param->param_name = pname;
                param->param_type = ptype;
                vec_push(inputs, param);
                match(p, TOK_COMMA);
            }
        } else if (check(p, TOK_IDENT) && cur(p)->text && strcmp(cur(p)->text, "output") == 0) {
            advance(p);
            expect(p, TOK_COLON);
            output = parse_type(p);
        } else if (check(p, TOK_IDENT) && cur(p)->text && strcmp(cur(p)->text, "guarantees") == 0) {
            advance(p);
            expect(p, TOK_COLON);
            while (match(p, TOK_MINUS)) {
                char buf[256] = {0};
                int bi = 0;
                while (!check(p, TOK_MINUS) && !check(p, TOK_RBRACE) && !check(p, TOK_EOF) &&
                       !(check(p, TOK_IDENT) && cur(p)->text &&
                         (strcmp(cur(p)->text, "requires") == 0 ||
                          strcmp(cur(p)->text, "decreases") == 0 ||
                          strcmp(cur(p)->text, "ensures") == 0))) {
                    Token *t = advance(p);
                    if (t->text && bi < 250) {
                        if (bi > 0) buf[bi++] = ' ';
                        int tl = (int)strlen(t->text);
                        if (bi + tl < 250) { memcpy(buf + bi, t->text, (size_t)tl); bi += tl; }
                    }
                }
                buf[bi] = '\0';
                vec_push(guarantees, strdup(buf));
            }
        } else if (check(p, TOK_IDENT) && cur(p)->text && strcmp(cur(p)->text, "ensures") == 0) {
            advance(p);
            expect(p, TOK_COLON);
            while (!check(p, TOK_RBRACE) && !check(p, TOK_EOF)) {
                int tstart = p->pos;
                SrcPos epos = cur(p)->pos;
                Node *expr = parse_expr(p);
                /* възстанови текста на израза от токените за съобщенията */
                char buf[512] = {0};
                int bi = 0;
                for (int ti = tstart; ti < p->pos; ti++) {
                    Token *t = &p->tokens[ti];
                    if (!t->text) continue;
                    int tl = (int)strlen(t->text);
                    if (bi > 0 && bi < 500) buf[bi++] = ' ';
                    if (bi + tl < 500) { memcpy(buf + bi, t->text, (size_t)tl); bi += tl; }
                }
                buf[bi] = '\0';
                Node *en = node_alloc(NODE_ENSURE, epos);
                en->ensure_text = strdup(buf);
                en->ensure_expr = expr;
                vec_push(ensures, en);
                if (!match(p, TOK_COMMA)) break;
            }
        } else if (check(p, TOK_IDENT) && cur(p)->text && strcmp(cur(p)->text, "requires") == 0) {
            advance(p);
            expect(p, TOK_COLON);
            while (!check(p, TOK_RBRACE) && !check(p, TOK_EOF)) {
                int tstart = p->pos;
                SrcPos epos = cur(p)->pos;
                Node *expr = parse_expr(p);
                /* възстанови текста на израза от токените за съобщенията */
                char buf[512] = {0};
                int bi = 0;
                for (int ti = tstart; ti < p->pos; ti++) {
                    Token *t = &p->tokens[ti];
                    if (!t->text) continue;
                    int tl = (int)strlen(t->text);
                    if (bi > 0 && bi < 500) buf[bi++] = ' ';
                    if (bi + tl < 500) { memcpy(buf + bi, t->text, (size_t)tl); bi += tl; }
                }
                buf[bi] = '\0';
                Node *en = node_alloc(NODE_ENSURE, epos);
                en->ensure_text = strdup(buf);
                en->ensure_expr = expr;
                vec_push(requires, en);
                if (!match(p, TOK_COMMA)) break;
            }
        } else if (check(p, TOK_IDENT) && cur(p)->text && strcmp(cur(p)->text, "decreases") == 0) {
            /* M6: decreases: <expr> — терминационна мярка над input параметрите */
            advance(p);
            expect(p, TOK_COLON);
            Node *d = parse_expr(p);
            if (decreases) {
                parser_error(p, "spec: decreases е зададен повече от веднъж");
                node_free(d);
            } else {
                decreases = d;
            }
        } else {
            advance(p);
        }
    }
    expect(p, TOK_RBRACE);

    Node *s = node_alloc(NODE_SPEC, pos);
    s->spec_name = name;
    s->spec_inputs = inputs;
    s->spec_output = output;
    s->spec_guarantees = guarantees.data;
    s->n_guarantees = guarantees.len;
    s->spec_ensures = ensures;
    s->spec_requires = requires;
    s->spec_decreases = decreases;
    return s;
}

static Node *parse_enum(Parser *p) {
    SrcPos pos = cur(p)->pos;
    expect(p, TOK_ENUM);
    char *name = expect_ident(p);
    expect(p, TOK_LBRACE);

    VEC(char *) variants = {0};
    VEC(Node *) payloads = {0};
    while (!check(p, TOK_RBRACE) && !check(p, TOK_EOF)) {
        char *vname = expect_ident(p);
        vec_push(variants, vname);
        if (match(p, TOK_LPAREN)) {
            vec_push(payloads, parse_type(p));
            expect(p, TOK_RPAREN);
        } else {
            vec_push(payloads, NULL);
        }
        if (!check(p, TOK_RBRACE)) match(p, TOK_COMMA);
    }
    expect(p, TOK_RBRACE);

    Node *e = node_alloc(NODE_ENUM, pos);
    e->enum_name = name;
    e->enum_variants = variants.data;
    e->enum_payloads = payloads.data;
    e->n_variants = variants.len;
    return e;
}

/* M22: парсира един израз от текст (guarantee редове за верификатора).
 * Връща NULL при неуспех (проза / непокрит синтаксис). */
Node *parse_expr_string(const char *text) {
    Lexer l;
    lexer_init(&l, text, (int)strlen(text), "<guarantee>");
    VEC(Token) toks = {0};
    for (;;) {
        Token t = lexer_next(&l);
        vec_push(toks, t);
        if (t.kind == TOK_EOF) break;
        if (t.kind == TOK_ERROR) { free(toks.data); return NULL; }
    }
    Parser p;
    memset(&p, 0, sizeof(p));
    p.tokens = toks.data;
    p.len = toks.len;
    p.pos = 0;
    p.filename = "<guarantee>";
    Node *e = parse_expr(&p);
    if (p.n_errors > 0 || !check(&p, TOK_EOF)) {
        free(toks.data);
        return NULL;
    }
    free(toks.data);
    return e;
}

Node *parse_program(Parser *p, Token *tokens, int ntokens, const char *filename) {    p->tokens = tokens;
    p->len = ntokens;
    p->pos = 0;
    p->filename = filename;
    p->n_errors = 0;
    p->in_catch_handler = 0;
    p->gt_pending = 0;
    p->type_depth = 0;

    SrcPos pos = { 1, 1, filename };
    Node *prog = node_alloc(NODE_PROGRAM, pos);
    prog->items.len = 0; prog->items.cap = 0; prog->items.data = NULL;

    while (!check(p, TOK_EOF)) {
        if (check(p, TOK_FN)) {
            vec_push(prog->items, parse_fn(p));
        } else if (check(p, TOK_EXTERN)) {
            advance(p);
            Node *fn = parse_fn(p);
            fn->is_extern = 1;
            if (fn->fn_body)
                parser_error(p, "extern fn '%s' не може да има тяло", fn->fn_name);
            vec_push(prog->items, fn);
        } else if (check(p, TOK_STRUCT)) {
            vec_push(prog->items, parse_struct(p));
        } else if (check(p, TOK_TRAIT)) {
            vec_push(prog->items, parse_trait(p));
        } else if (check(p, TOK_IMPL)) {
            vec_push(prog->items, parse_impl(p));
        } else if (check(p, TOK_SPEC)) {
            vec_push(prog->items, parse_spec(p));
        } else if (check(p, TOK_ENUM)) {
            vec_push(prog->items, parse_enum(p));
        } else {
            parser_error(p, "очаквах декларация (fn, struct, spec), получих '%s'",
                         token_kind_str(peek_kind(p)));
            advance(p);
        }
    }

    return prog;
}

/* ============================================================
 *  AST printer (debug)
 * ============================================================ */

static void indent_print(int indent) {
    for (int i = 0; i < indent; i++) fprintf(stderr, "  ");
}

static const char *binop_str(BinOp op) {
    switch (op) {
        case OP_ADD: return "+"; case OP_SUB: return "-";
        case OP_MUL: return "*"; case OP_DIV: return "/";
        case OP_MOD: return "%";
        case OP_EQ: return "=="; case OP_NEQ: return "!=";
        case OP_LT: return "<"; case OP_GT: return ">";
        case OP_LE: return "<="; case OP_GE: return ">=";
        case OP_AND: return "&&"; case OP_OR: return "||";
        case OP_BIT_AND: return "&"; case OP_BIT_OR: return "|";
        case OP_BIT_XOR: return "^"; case OP_LSHIFT: return "<<";
        case OP_RSHIFT: return ">>"; case OP_URSHIFT: return ">>>";
        default: return "?";
    }
}

void print_ast(Node *n, int indent) {
    if (!n) return;
    indent_print(indent);

    switch (n->kind) {
        case NODE_PROGRAM:
            fprintf(stderr, "PROGRAM\n");
            for (int i = 0; i < n->items.len; i++)
                print_ast(n->items.data[i], indent + 1);
            break;
        case NODE_FN:
            fprintf(stderr, "FN %s\n", n->fn_name);
            for (int i = 0; i < n->params.len; i++)
                print_ast(n->params.data[i], indent + 1);
            if (n->ret_type) print_ast(n->ret_type, indent + 1);
            print_ast(n->fn_body, indent + 1);
            break;
        case NODE_PARAM:
            fprintf(stderr, "PARAM %s: ", n->param_name);
            if (n->param_type) print_ast(n->param_type, 0);
            else fprintf(stderr, "\n");
            break;
        case NODE_BLOCK:
            fprintf(stderr, "BLOCK\n");
            for (int i = 0; i < n->stmts.len; i++)
                print_ast(n->stmts.data[i], indent + 1);
            break;
        case NODE_LET:
            fprintf(stderr, "LET %s%s\n", n->is_mut ? "mut " : "", n->let_name);
            print_ast(n->let_init, indent + 1);
            break;
        case NODE_RETURN:
            fprintf(stderr, "RETURN\n");
            if (n->ret_val) print_ast(n->ret_val, indent + 1);
            break;
        case NODE_WHILE:
            fprintf(stderr, "WHILE\n");
            print_ast(n->while_cond, indent + 1);
            for (int i = 0; i < n->while_invariants.len; i++) {
                fprintf(stderr, "%*sINVARIANT\n", indent + 1, "");
                print_ast(n->while_invariants.data[i], indent + 2);
            }
            print_ast(n->while_body, indent + 1);
            break;
        case NODE_FOR:
            fprintf(stderr, "FOR %s\n", n->for_var);
            print_ast(n->for_iter, indent + 1);
            print_ast(n->for_body, indent + 1);
            break;
        case NODE_MATCH:
            fprintf(stderr, "MATCH\n");
            print_ast(n->match_expr, indent + 1);
            for (int i = 0; i < n->match_arms.len; i++)
                print_ast(n->match_arms.data[i], indent + 1);
            break;
        case NODE_MATCH_ARM:
            fprintf(stderr, "ARM\n");
            if (n->arm_pattern) print_ast(n->arm_pattern, indent + 1);
            else { indent_print(indent + 1); fprintf(stderr, "_\n"); }
            print_ast(n->arm_body, indent + 1);
            break;
        case NODE_BREAK:
            fprintf(stderr, "BREAK\n");
            break;
        case NODE_CONTINUE:
            fprintf(stderr, "CONTINUE\n");
            break;
        case NODE_IF:
            fprintf(stderr, "IF\n");
            print_ast(n->cond, indent + 1);
            print_ast(n->then_br, indent + 1);
            if (n->else_br) print_ast(n->else_br, indent + 1);
            break;
        case NODE_EXPR_STMT:
            fprintf(stderr, "EXPR_STMT\n");
            print_ast(n->expr, indent + 1);
            break;
        case NODE_INVARIANT:
            fprintf(stderr, "INVARIANT\n");
            for (int i = 0; i < n->inv_exprs.len; i++) print_ast(n->inv_exprs.data[i], indent + 1);
            break;
        case NODE_CALL:
            fprintf(stderr, "CALL\n");
            print_ast(n->callee, indent + 1);
            for (int i = 0; i < n->args.len; i++)
                print_ast(n->args.data[i], indent + 1);
            break;
        case NODE_BINARY:
            fprintf(stderr, "BINARY %s\n", binop_str(n->bin_op));
            print_ast(n->left, indent + 1);
            print_ast(n->right, indent + 1);
            break;
        case NODE_UNARY:
            fprintf(stderr, "UNARY\n");
            print_ast(n->operand, indent + 1);
            break;
        case NODE_INT_LIT:
            fprintf(stderr, "INT %lld\n", (long long)n->int_val);
            break;
        case NODE_FLOAT_LIT:
            fprintf(stderr, "FLOAT %g\n", n->float_val);
            break;
        case NODE_STR_LIT:
            fprintf(stderr, "STR \"%s\"\n", n->str_val);
            break;
        case NODE_BYTES_LIT:
            fprintf(stderr, "BYTES x\"%s\"\n", n->str_val);
            break;
        case NODE_BOOL_LIT:
            fprintf(stderr, "BOOL %s\n", n->bool_val ? "true" : "false");
            break;
        case NODE_IDENT:
            fprintf(stderr, "IDENT %s\n", n->name);
            break;
        case NODE_PATH:
            fprintf(stderr, "PATH %s::%s\n",
                n->path_enum ? n->path_enum : "?",
                n->path_variant ? n->path_variant : "?");
            break;
        case NODE_ASSIGN:
            fprintf(stderr, "ASSIGN\n");
            print_ast(n->assign_target, indent + 1);
            print_ast(n->assign_val, indent + 1);
            break;
        case NODE_INDEX:
            fprintf(stderr, "INDEX\n");
            print_ast(n->obj, indent + 1);
            print_ast(n->index, indent + 1);
            break;
        case NODE_ELEM_REF:
            fprintf(stderr, "ELEM_REF [*]\n");
            print_ast(n->elem_obj, indent + 1);
            break;
        case NODE_FIELD:
            fprintf(stderr, "FIELD .%s\n", n->field_name);
            print_ast(n->field_obj, indent + 1);
            break;
        case NODE_RANGE:
            fprintf(stderr, "RANGE\n");
            print_ast(n->range_lo, indent + 1);
            print_ast(n->range_hi, indent + 1);
            break;
        case NODE_STRUCT_LIT:
            fprintf(stderr, "STRUCT_LIT %s\n", n->lit_name);
            for (int i = 0; i < n->n_lit_fields; i++) {
                indent_print(indent + 1);
                fprintf(stderr, ".%s =\n", n->lit_fields[i]);
                print_ast(n->lit_values.data[i], indent + 2);
            }
            break;
        case NODE_TRY:
            fprintf(stderr, "TRY (?)\n");
            print_ast(n->try_expr, indent + 1);
            break;
        case NODE_CATCH:
            fprintf(stderr, "CATCH !%s\n", n->catch_effect);
            print_ast(n->catch_expr, indent + 1);
            print_ast(n->catch_handler, indent + 1);
            break;
        case NODE_TO_STR:
            fprintf(stderr, "TO_STR\n");
            print_ast(n->to_str_expr, indent + 1);
            break;
        case NODE_TYPE:
            fprintf(stderr, "TYPE %s\n", n->type_name);
            break;
        case NODE_TYPE_REF:
            fprintf(stderr, "TYPE_REF\n");
            print_ast(n->inner_type, indent + 1);
            break;
        case NODE_TYPE_ARRAY:
            fprintf(stderr, "TYPE_ARRAY\n");
            print_ast(n->inner_type, indent + 1);
            break;
        case NODE_TYPE_EFFECT:
            fprintf(stderr, "TYPE_EFFECT");
            for (int i = 0; i < n->n_effects; i++)
                fprintf(stderr, " !%s", n->effect_names[i]);
            fprintf(stderr, "\n");
            print_ast(n->inner_type, indent + 1);
            break;
        case NODE_STRUCT:
            fprintf(stderr, "STRUCT %s\n", n->struct_name);
            for (int i = 0; i < n->fields.len; i++)
                print_ast(n->fields.data[i], indent + 1);
            break;
        case NODE_FIELD_DECL:
            fprintf(stderr, "FIELD %s\n", n->fld_name);
            print_ast(n->fld_type, indent + 1);
            break;
        case NODE_SPEC:
            fprintf(stderr, "SPEC \"%s\"\n", n->spec_name);
            for (int i = 0; i < n->spec_ensures.len; i++)
                fprintf(stderr, "  ensures: %s\n", n->spec_ensures.data[i]->ensure_text);
            for (int i = 0; i < n->spec_requires.len; i++)
                fprintf(stderr, "  requires: %s\n", n->spec_requires.data[i]->ensure_text);
            break;
        case NODE_ENUM:
            fprintf(stderr, "ENUM %s", n->enum_name);
            for (int i = 0; i < n->n_variants; i++)
                fprintf(stderr, " %s", n->enum_variants[i]);
            fprintf(stderr, "\n");
            break;
        default:
            fprintf(stderr, "NODE(%d)\n", n->kind);
            break;
    }
}

