/* codegen_move.c — част от codegen_c.c (виж cg_proto.h). */
#include "baga.h"
#include "codegen_impl.h"

/* ---- RC2 (move elision): last-use pre-pass + copy-site проверка ----
 * Дизайн: docs/move-semantics-bg.md. Per функция def-use обход на AST-то;
 * move сайт е копие (let alias / struct-lit embed / field assign / assign),
 * чийто източник е при ПОСЛЕДНАТА си текстуална употреба — retain-ът се
 * пропуска, binding-ът се маркира dead (scope release отпада).
 * Консервативно: shadowing, lambda capture, match arm, loop-carried
 * (употреба в/след цикъл при binding извън него) изключват move. */

/* флагове на обхода: bit0 = в if/match клон, bit1 = в match arm */
#define RC_LU_COND  1
#define RC_LU_MATCH 2

static RcUse *rc_lu_rec(Codegen *cg, const char *name) {
    for (int i = 0; i < cg->rc_lus.len; i++)
        if (strcmp(cg->rc_lus.data[i].name, name) == 0)
            return &cg->rc_lus.data[i];
    vec_push(cg->rc_lus, ((RcUse){ name, NULL, NULL, NULL, 0, 0, 0, 0 }));
    return &cg->rc_lus.data[cg->rc_lus.len - 1];
}

/* употреба на ident в израз — последната текстуална печели (презаписва) */
static void rc_lu_use(Codegen *cg, Node *ident, Node *loop, int flags) {
    RcUse *u = rc_lu_rec(cg, ident->name);
    u->site = ident;
    u->site_loop = loop;
    u->site_in_cond = (flags & RC_LU_COND) != 0;
    if (flags & RC_LU_MATCH) u->in_match = 1;
}

void rc_lu_stmts(Codegen *cg, NodeVec *stmts, Node *loop, int flags);

static void rc_lu_expr(Codegen *cg, Node *n, Node *loop, int flags) {
    if (!n) return;
    switch (n->kind) {
        case NODE_IDENT:
            rc_lu_use(cg, n, loop, flags);
            break;
        case NODE_BINARY:
            rc_lu_expr(cg, n->left, loop, flags);
            rc_lu_expr(cg, n->right, loop, flags);
            break;
        case NODE_UNARY:
            rc_lu_expr(cg, n->operand, loop, flags);
            break;
        case NODE_CALL:
            /* callee ident (fn име) — брои се като употреба (консервативно) */
            rc_lu_expr(cg, n->callee, loop, flags);
            for (int i = 0; i < n->args.len; i++)
                rc_lu_expr(cg, n->args.data[i], loop, flags);
            break;
        case NODE_IF:
            rc_lu_expr(cg, n->cond, loop, flags);
            if (n->then_br && n->then_br->kind == NODE_BLOCK)
                rc_lu_stmts(cg, &n->then_br->stmts, loop, flags | RC_LU_COND);
            if (n->else_br && n->else_br->kind == NODE_BLOCK)
                rc_lu_stmts(cg, &n->else_br->stmts, loop, flags | RC_LU_COND);
            break;
        case NODE_BLOCK:
            rc_lu_stmts(cg, &n->stmts, loop, flags);
            break;
        case NODE_INDEX:
            rc_lu_expr(cg, n->obj, loop, flags);
            rc_lu_expr(cg, n->index, loop, flags);
            break;
        case NODE_ELEM_REF:
            rc_lu_expr(cg, n->elem_obj, loop, flags);
            break;
        case NODE_FIELD:
            rc_lu_expr(cg, n->field_obj, loop, flags);
            break;
        case NODE_ASSIGN:
            /* target-ът е запис, не употреба; само сложни цели четат обекта */
            if (n->assign_target && n->assign_target->kind == NODE_FIELD)
                rc_lu_expr(cg, n->assign_target->field_obj, loop, flags);
            else if (n->assign_target && n->assign_target->kind == NODE_INDEX) {
                rc_lu_expr(cg, n->assign_target->obj, loop, flags);
                rc_lu_expr(cg, n->assign_target->index, loop, flags);
            }
            rc_lu_expr(cg, n->assign_val, loop, flags);
            break;
        case NODE_RANGE:
            rc_lu_expr(cg, n->range_lo, loop, flags);
            rc_lu_expr(cg, n->range_hi, loop, flags);
            break;
        case NODE_STRUCT_LIT:
            for (int i = 0; i < n->n_lit_fields; i++)
                rc_lu_expr(cg, n->lit_values.data[i], loop, flags);
            break;
        case NODE_TRY:
            rc_lu_expr(cg, n->try_expr, loop, flags);
            break;
        case NODE_CATCH:
            rc_lu_expr(cg, n->catch_expr, loop, flags);
            rc_lu_expr(cg, n->catch_handler, loop, flags);
            break;
        case NODE_TO_STR:
            rc_lu_expr(cg, n->to_str_expr, loop, flags);
            break;
        case NODE_LAMBDA:
            /* capture = употреба, която изключва move (env изживява scope-а);
             * тялото е отделна функция — анализира се при нейния emission */
            for (int i = 0; i < n->captures.len; i++)
                rc_lu_rec(cg, n->captures.data[i]->param_name)->captured = 1;
            break;
        case NODE_MATCH:
            rc_lu_expr(cg, n->match_expr, loop, flags);
            for (int i = 0; i < n->match_arms.len; i++) {
                Node *arm = n->match_arms.data[i];
                if (arm->arm_pattern)
                    rc_lu_expr(cg, arm->arm_pattern, loop, flags);
                if (arm->arm_body && arm->arm_body->kind == NODE_BLOCK)
                    rc_lu_stmts(cg, &arm->arm_body->stmts, loop,
                                flags | RC_LU_COND | RC_LU_MATCH);
            }
            break;
        default:
            break;
    }
}

static void rc_lu_stmt(Codegen *cg, Node *n, Node *loop, int flags) {
    if (!n) return;
    switch (n->kind) {
        case NODE_LET:
            {
                RcUse *u = rc_lu_rec(cg, n->let_name);
                if (u->n_lets == 0) u->decl_loop = loop;
                u->n_lets++;
            }
            if (n->let_init) rc_lu_expr(cg, n->let_init, loop, flags);
            break;
        case NODE_RETURN:
            if (n->ret_val) rc_lu_expr(cg, n->ret_val, loop, flags);
            break;
        case NODE_WHILE:
            rc_lu_expr(cg, n->while_cond, loop, flags);
            if (n->while_body && n->while_body->kind == NODE_BLOCK)
                rc_lu_stmts(cg, &n->while_body->stmts, n, flags);
            break;
        case NODE_FOR:
            if (n->for_iter) rc_lu_expr(cg, n->for_iter, loop, flags);
            if (n->for_body && n->for_body->kind == NODE_BLOCK)
                rc_lu_stmts(cg, &n->for_body->stmts, n, flags);
            break;
        case NODE_IF:
            rc_lu_expr(cg, n->cond, loop, flags);
            if (n->then_br && n->then_br->kind == NODE_BLOCK)
                rc_lu_stmts(cg, &n->then_br->stmts, loop, flags | RC_LU_COND);
            if (n->else_br && n->else_br->kind == NODE_BLOCK)
                rc_lu_stmts(cg, &n->else_br->stmts, loop, flags | RC_LU_COND);
            break;
        case NODE_EXPR_STMT:
            rc_lu_expr(cg, n->expr, loop, flags);
            break;
        case NODE_BLOCK:
            rc_lu_stmts(cg, &n->stmts, loop, flags);
            break;
        default:
            break;
    }
}

void rc_lu_stmts(Codegen *cg, NodeVec *stmts, Node *loop, int flags) {
    for (int i = 0; i < stmts->len; i++)
        rc_lu_stmt(cg, stmts->data[i], loop, flags);
}

/* move сайт ли е тази употреба? ident е Node* на конкретната употреба,
 * src_idx — записът на източника в rc_locals (или -1). */
int rc_is_move(Codegen *cg, Node *ident, int src_idx) {
    if (!cg->rc || !ident || ident->kind != NODE_IDENT) return 0;
    if (src_idx < 0 || cg->rc_locals.data[src_idx].is_param) return 0;
    for (int i = 0; i < cg->rc_lus.len; i++) {
        RcUse *u = &cg->rc_lus.data[i];
        if (strcmp(u->name, ident->name) != 0) continue;
        if (u->site != ident || u->n_lets != 1 || u->captured || u->in_match)
            return 0;
        /* loop правило: move в цикъл само ако bindingът е от същата итерация;
         * в клон на if/match вътре в цикъл — не (другият път тече всяка итер.) */
        if (u->site_loop != u->decl_loop) return 0;
        if (u->site_loop && u->site_in_cond) return 0;
        return 1;
    }
    return 0;
}

/* move сайт: пропусни retain-а, маркирай източника dead (site е текстуално
 * последната употреба → текстуалното маркиране е безопасно) */
void rc_do_move(Codegen *cg, int src_idx) {
    cg->rc_locals.data[src_idx].dead = 1;
    cg->rc_moves++;
}

/* ---- RC2.1: elision на borrowed-retain двойки ----
 * `let x = <borrowed>` (vec_get/map_get/struct поле): ако x не escape-ва
 * от scope-а и източникът не се мутира в него, retain-ът при връзване и
 * scope release-ът са чиста загуба — пропускаме и двете (локалът е чисто
 * заеман, невидим за rc). Правила: docs/memory-rc-bg.md §v2.1. */


/* чисти builtin-и: повикване с източника като аргумент не го мутира */
static int rc_be_pure_call(const char *bn) {
    static const char *pure[] = {
        "len", "char_at", "byte_at", "byte_chr", "substr", "concat",
        "chr", "ord", "str_eq", "i64_to_str", "f64_to_str",
        "print", "println", "write", "eprintln",
        "vec_get", "vec_len", "vec_slice", "vec_concat",
        "map_get", "map_len", "map_has",
        "bytes_len", "bytes_at", "bytes_slice", "bytes_concat",
        "bytes_of_str", "str_of_bytes", "hex_encode", "hex_decode",
        "bytes_from_vec", "vec_from_bytes",
    };
    for (size_t i = 0; i < sizeof(pure) / sizeof(pure[0]); i++)
        if (strcmp(bn, pure[i]) == 0) return 1;
    return 0;
}

/* базов ident на верига полета (sel в sel.group_cols); NULL = неразпознат */
static const char *rc_be_base_ident(Node *e) {
    while (e) {
        if (e->kind == NODE_IDENT) return e->name;
        if (e->kind == NODE_FIELD) { e = e->field_obj; continue; }
        return NULL;
    }
    return NULL;
}

/* името на източника на заемането (контейнер/struct), или NULL */
const char *rc_borrow_src_name(Node *init) {
    if (init->kind == NODE_CALL && init->callee &&
        init->callee->kind == NODE_IDENT &&
        (strncmp(init->callee->name, "vec_get", 7) == 0 ||
         strncmp(init->callee->name, "map_get", 7) == 0) &&
        init->args.len >= 1)
        return rc_be_base_ident(init->args.data[0]);
    if (init->kind == NODE_FIELD)
        return rc_be_base_ident(init);
    return NULL;
}

static int rc_be_is(Node *n, const char *name) {
    return n && n->kind == NODE_IDENT && strcmp(n->name, name) == 0;
}

/* RC1.3: mem_mark/mem_rewind ли е това повикване (директно или под ?) */
int rc_is_mem_call(Node *n, const char *name) {
    if (n && n->kind == NODE_TRY) n = n->try_expr;
    return n && n->kind == NODE_CALL && n->callee &&
           n->callee->kind == NODE_IDENT &&
           strcmp(n->callee->name, name) == 0;
}

static void rc_be_expr(RcBe *be, Node *n);
static void rc_be_stmts(RcBe *be, NodeVec *stmts);

static void rc_be_call(RcBe *be, Node *n) {
    const char *bn = (n->callee && n->callee->kind == NODE_IDENT)
                     ? n->callee->name : NULL;
    if (bn && strcmp(bn, "drop") == 0) {
        /* drop на x = release път (трябва нашата референция); на src — мутация */
        for (int i = 0; i < n->args.len; i++)
            if (rc_be_is(n->args.data[i], be->x) ||
                rc_be_is(n->args.data[i], be->src))
                be->bad = 1;
        return;
    }
    if (bn && (strncmp(bn, "vec_push", 8) == 0 || strncmp(bn, "vec_set", 7) == 0 ||
               strncmp(bn, "map_set", 7) == 0 || strncmp(bn, "map_del", 7) == 0 ||
               strncmp(bn, "sort", 4) == 0)) {
        /* x в контейнерна операция = escape (правило а);
         * src (вкл. по база на field) в нея = мутация (правило б) */
        for (int i = 0; i < n->args.len; i++) {
            if (rc_be_is(n->args.data[i], be->x)) be->bad = 1;
            const char *b = rc_be_base_ident(n->args.data[i]);
            if (b && strcmp(b, be->src) == 0) be->bad = 1;
        }
    } else if (bn && !rc_be_pure_call(bn)) {
        /* консервативно: src в не-pure повикване = възможна мутация */
        for (int i = 0; i < n->args.len; i++) {
            const char *b = rc_be_base_ident(n->args.data[i]);
            if (b && strcmp(b, be->src) == 0) be->bad = 1;
        }
    }
    /* x като аргумент на pure/друго повикване = четене (params са borrowed) */
    if (n->callee && n->callee->kind != NODE_IDENT) rc_be_expr(be, n->callee);
    for (int i = 0; i < n->args.len; i++) rc_be_expr(be, n->args.data[i]);
}

static void rc_be_expr(RcBe *be, Node *n) {
    if (!n || be->bad) return;
    switch (n->kind) {
        case NODE_CALL: rc_be_call(be, n); break;
        case NODE_BINARY:
            rc_be_expr(be, n->left); rc_be_expr(be, n->right); break;
        case NODE_UNARY: rc_be_expr(be, n->operand); break;
        case NODE_IF:
            rc_be_expr(be, n->cond);
            if (n->then_br && n->then_br->kind == NODE_BLOCK)
                rc_be_stmts(be, &n->then_br->stmts);
            if (n->else_br && n->else_br->kind == NODE_BLOCK)
                rc_be_stmts(be, &n->else_br->stmts);
            break;
        case NODE_BLOCK: rc_be_stmts(be, &n->stmts); break;
        case NODE_INDEX:
            rc_be_expr(be, n->obj); rc_be_expr(be, n->index); break;
        case NODE_ELEM_REF: rc_be_expr(be, n->elem_obj); break;
        case NODE_FIELD: rc_be_expr(be, n->field_obj); break;
        case NODE_ASSIGN:
            /* преприсвояване на x = прекъсва borrow-а; мутация на src;
             * x/src като стойност = escape/алиас */
            if (rc_be_is(n->assign_target, be->x) ||
                rc_be_is(n->assign_target, be->src))
                be->bad = 1;
            if (n->assign_target && n->assign_target->kind == NODE_FIELD) {
                const char *b = rc_be_base_ident(n->assign_target->field_obj);
                if (b && strcmp(b, be->src) == 0) be->bad = 1;
                rc_be_expr(be, n->assign_target->field_obj);
            } else if (n->assign_target && n->assign_target->kind == NODE_INDEX) {
                const char *b = rc_be_base_ident(n->assign_target->obj);
                if (b && strcmp(b, be->src) == 0) be->bad = 1;
                rc_be_expr(be, n->assign_target->obj);
                rc_be_expr(be, n->assign_target->index);
            }
            if (rc_be_is(n->assign_val, be->x) ||
                rc_be_is(n->assign_val, be->src))
                be->bad = 1;
            else
                rc_be_expr(be, n->assign_val);
            break;
        case NODE_STRUCT_LIT:
            for (int i = 0; i < n->n_lit_fields; i++) {
                Node *val = n->lit_values.data[i];
                /* x embed = escape (правило а); src embed = алиас път —
                 * мутацията през него е невидима за анализа (правило б) */
                if (rc_be_is(val, be->x) || rc_be_is(val, be->src))
                    be->bad = 1;
                else
                    rc_be_expr(be, val);
            }
            break;
        case NODE_LAMBDA:
            /* capture на x (escape) или src (консервативно) */
            for (int i = 0; i < n->captures.len; i++) {
                const char *cn = n->captures.data[i]->param_name;
                if (strcmp(cn, be->x) == 0 || strcmp(cn, be->src) == 0)
                    be->bad = 1;
            }
            break;
        case NODE_RANGE:
            rc_be_expr(be, n->range_lo); rc_be_expr(be, n->range_hi); break;
        case NODE_TRY: rc_be_expr(be, n->try_expr); break;
        case NODE_CATCH:
            rc_be_expr(be, n->catch_expr); rc_be_expr(be, n->catch_handler); break;
        case NODE_TO_STR: rc_be_expr(be, n->to_str_expr); break;
        case NODE_MATCH:
            rc_be_expr(be, n->match_expr);
            for (int i = 0; i < n->match_arms.len; i++) {
                Node *arm = n->match_arms.data[i];
                if (arm->arm_pattern) rc_be_expr(be, arm->arm_pattern);
                if (arm->arm_body && arm->arm_body->kind == NODE_BLOCK)
                    rc_be_stmts(be, &arm->arm_body->stmts);
            }
            break;
        default:
            break;
    }
}

static void rc_be_stmt(RcBe *be, Node *n) {
    if (!n || be->bad) return;
    switch (n->kind) {
        case NODE_LET:
            /* alias на x (escape) или на src (скрита мутация по-късно) */
            if (rc_be_is(n->let_init, be->x) || rc_be_is(n->let_init, be->src))
                be->bad = 1;
            else
                rc_be_expr(be, n->let_init);
            break;
        case NODE_RETURN:
            /* return x = escape; return <израз с x> = четене */
            if (rc_be_is(n->ret_val, be->x)) be->bad = 1;
            else rc_be_expr(be, n->ret_val);
            break;
        case NODE_WHILE:
            rc_be_expr(be, n->while_cond);
            if (n->while_body && n->while_body->kind == NODE_BLOCK)
                rc_be_stmts(be, &n->while_body->stmts);
            break;
        case NODE_FOR:
            if (n->for_iter) rc_be_expr(be, n->for_iter);
            if (n->for_body && n->for_body->kind == NODE_BLOCK)
                rc_be_stmts(be, &n->for_body->stmts);
            break;
        case NODE_IF:
            rc_be_expr(be, n->cond);
            if (n->then_br && n->then_br->kind == NODE_BLOCK)
                rc_be_stmts(be, &n->then_br->stmts);
            if (n->else_br && n->else_br->kind == NODE_BLOCK)
                rc_be_stmts(be, &n->else_br->stmts);
            break;
        case NODE_EXPR_STMT: rc_be_expr(be, n->expr); break;
        case NODE_BLOCK: rc_be_stmts(be, &n->stmts); break;
        default: break;
    }
}

static void rc_be_stmts(RcBe *be, NodeVec *stmts) {
    for (int i = 0; i < stmts->len && !be->bad; i++)
        rc_be_stmt(be, stmts->data[i]);
}

/* допустимо ли е elision за `let x = <borrowed от src>` — (а) опашката на
 * текущия block след let-а (escape/мутация в scope-а), (б) цялата функция
 * (алиас/складиране на src — мутация през тях би била невидима) */
int rc_be_ok(Codegen *cg, const char *x, const char *src) {
    if (!cg->rc || !cg->rc_cur_blk || cg->rc_cur_idx < 0) return 0;
    if (strcmp(x, src) == 0) return 0;
    /* (б) глобален scan: src не бива да излиза от binding-а си никъде във fn */
    if (cg->rc_cur_fn) {
        RcBe whole = { "", src, 0 };
        rc_be_stmts(&whole, &cg->rc_cur_fn->stmts);
        if (whole.bad) return 0;
    } else {
        return 0;
    }
    /* (а) опашка на scope-а: x не escape-ва, src не се мутира */
    RcBe be = { x, src, 0 };
    NodeVec *stmts = &cg->rc_cur_blk->stmts;
    for (int i = cg->rc_cur_idx + 1; i < stmts->len && !be.bad; i++)
        rc_be_stmt(&be, stmts->data[i]);
    return !be.bad;
}

/* ---- RC4 (temporaries tracking): per-statement temp регистър ----
 * Дизайн: docs/memory-rc-bg.md §v0.2. Fresh heap резултат (call с heap тип, * който не е borrowed — vec_get/map_get/поле/h_* — плюс NODE_TO_STR), който
 * не се връзва в локал, е temp: собствената му rc=1 референция иначе тече.
 * Temp-овете се събират pre-order от root израза на statement-а, изчисляват
 * се в __rc_tmpN декларации преди него (вътрешните първи) и се release-ват
 * в края му. Не се слиза в: struct литерал (полетата escape-ват без retain),
 * ламбда (отделна fn), drop(…) аргументи (самият drop е release пътят).
 * Collect не слиза в try/catch, match рамена, if-израз и десния операнд
 * на &&/|| (условна оценка / ранен return) — emission-ът ги wrap-ва
 * локално (v1.1, emit_rc_stmt_expr_root). Root-ът на let/assign/return е
 * bound (собствеността се предава) — само вложените му temp-ове се събират.
 * v0.3: условия на if/while и for-range hi се wrap-ват в GNU ({…}) —
 * temp-овете се оценяват на всяка оценка на условието и се release-ват
 * веднага след нея (преди тялото / клоновете). continue/break не ги пипат. */

void rc_tmp_release_all(Codegen *cg);
void emit_expr(Codegen *cg, Node *n); /* fwd — пълната fwd декларация е по-долу */
void emit_rc_stmt_expr(Codegen *cg, Node *n);
void emit_rc_stmt_expr_root(Codegen *cg, Node *n, int root_bound);

/* RC5 v0.4: цел `ident.field`, където ident е track-нат struct локал (tag 5,
 * не param/dead), а field е пряко heap поле. Връща tag-а на полето (1-4) или
 * 0; за Vec/Map полета пълни kind (elem_kind/val_tag), sz и rel. Само плоско
 * ident.field — по-дълбоки пътеки биха се оценили два пъти. */
int rc_field_assign_tag(Codegen *cg, Node *target, int *kind,
                               char *sz, size_t szn, char *rel, size_t reln) {
    *kind = 0;
    snprintf(sz, szn, "0");
    snprintf(rel, reln, "0");
    if (!cg->rc || !target || target->kind != NODE_FIELD) return 0;
    Node *obj = target->field_obj;
    if (!obj || obj->kind != NODE_IDENT || !target->field_name) return 0;
    int idx = rc_find(cg, obj->name);
    if (idx < 0) return 0;
    RcLocal *e = &cg->rc_locals.data[idx];
    if (e->tag != 5 || e->is_param || e->dead) return 0;
    const char *sn = (e->type && e->type->name) ? e->type->name :
        (e->type_node && e->type_node->type_name) ? e->type_node->type_name : NULL;
    if (!sn) return 0;
    Node *d = find_struct_decl(cg, sn);
    if (!d) return 0;
    for (int i = 0; i < d->fields.len; i++) {
        Node *fld = d->fields.data[i];
        if (!fld->fld_name ||
            strcmp(fld->fld_name, target->field_name) != 0)
            continue;
        int tag = rc_type_node_tag(fld->fld_type);
        if (tag == 3) {
            *kind = rc_vec_elem_kind_node(fld->fld_type, sz, szn);
            Node *et = fld->fld_type->inner_type;
            rc_box_rel(cg, (et && et->kind == NODE_TYPE) ? et->type_name : NULL,
                       rel, reln);
            /* RC5 v0.9: вложен Vec<S> — destructor за S полетата */
            if (*kind == 3) rc_nested_vec_rel_node(cg, fld->fld_type, rel, reln);
        } else if (tag == 4) {
            *kind = rc_map_val_tag_node(fld->fld_type, sz, szn);
            Node *vt = fld->fld_type->inner_type2;
            rc_box_rel(cg, (vt && vt->kind == NODE_TYPE) ? vt->type_name : NULL,
                       rel, reln);
        }
        return tag;
    }
    return 0;
}

/* release на старото поле (в alias-safe ред: след retain на новото) */
void rc_emit_field_release(Codegen *cg, int tag, Node *target, int kind,
                                  const char *sz, const char *rel) {
    FILE *f = cg->out;
    switch (tag) {
        case 1: fprintf(f, "baga_rc_release_str("); break;
        case 2: fprintf(f, "baga_rc_release_bytes("); break;
        case 3: fprintf(f, "baga_rc_release_vec("); break;
        case 4: fprintf(f, "baga_rc_release_map("); break;
        default: return;
    }
    emit_expr(cg, target);
    if (tag == 3 || tag == 4)
        fprintf(f, ", %d, %s, %s", kind, sz, rel);
    fprintf(f, "); ");
}

/* RC5 v0.8: цел `ident.field`, където ident е track-нат struct локал (tag 5,
 * не param/dead), а field е struct-типизирано поле с heap полета (транзитивно,
 * v0.5). Връща името на struct типа на полето или NULL. Същата плоска граница
 * като v0.4 — по-дълбоки пътеки (`a.b.c = x`) биха оценили целта два пъти. */
const char *rc_field_assign_struct(Codegen *cg, Node *target) {
    if (!cg->rc || !target || target->kind != NODE_FIELD) return NULL;
    Node *obj = target->field_obj;
    if (!obj || obj->kind != NODE_IDENT || !target->field_name) return NULL;
    int idx = rc_find(cg, obj->name);
    if (idx < 0) return NULL;
    RcLocal *e = &cg->rc_locals.data[idx];
    if (e->tag != 5 || e->is_param || e->dead) return NULL;
    const char *sn = (e->type && e->type->name) ? e->type->name :
        (e->type_node && e->type_node->type_name) ? e->type_node->type_name : NULL;
    if (!sn) return NULL;
    Node *d = find_struct_decl(cg, sn);
    if (!d) return NULL;
    for (int i = 0; i < d->fields.len; i++) {
        Node *fld = d->fields.data[i];
        if (!fld->fld_name ||
            strcmp(fld->fld_name, target->field_name) != 0)
            continue;
        return rc_nested_struct_field(cg, fld->fld_type);
    }
    return NULL;
}

/* release на старото struct-типизирано поле (в alias-safe ред: след retain
 * на новото) — рекурсивен release_<T> от v0.5 */
void rc_emit_struct_field_release(Codegen *cg, const char *stname,
                                         Node *target) {
    char *sm = mangle_name(stname);
    fprintf(cg->out, "baga_rc_release_%s(", sm);
    emit_expr(cg, target);
    fprintf(cg->out, "); ");
    free(sm);
}

/* RC5 v0.10: цел `ident.field`, където ident е track-нат struct локал (tag 5,
 * не param/dead), а field е enum-типизирано поле с heap payload. Връща името
 * на enum типа или NULL. Същата плоска граница като v0.4/v0.8. */
const char *rc_field_assign_enum(Codegen *cg, Node *target) {
    if (!cg->rc || !target || target->kind != NODE_FIELD) return NULL;
    Node *obj = target->field_obj;
    if (!obj || obj->kind != NODE_IDENT || !target->field_name) return NULL;
    int idx = rc_find(cg, obj->name);
    if (idx < 0) return NULL;
    RcLocal *e = &cg->rc_locals.data[idx];
    if (e->tag != 5 || e->is_param || e->dead) return NULL;
    const char *sn = (e->type && e->type->name) ? e->type->name :
        (e->type_node && e->type_node->type_name) ? e->type_node->type_name : NULL;
    if (!sn) return NULL;
    Node *d = find_struct_decl(cg, sn);
    if (!d) return NULL;
    for (int i = 0; i < d->fields.len; i++) {
        Node *fld = d->fields.data[i];
        if (!fld->fld_name ||
            strcmp(fld->fld_name, target->field_name) != 0)
            continue;
        return rc_nested_enum_field(cg, fld->fld_type);
    }
    return NULL;
}

/* RC5 v0.10: release на старото enum-типизирано поле (в alias-safe ред:
 * след retain на новото) — release_E по runtime tag от v0.6 */
void rc_emit_enum_field_release(Codegen *cg, const char *enname,
                                       Node *target) {
    char *em = mangle_name(enname);
    fprintf(cg->out, "baga_rc_release_%s(", em);
    emit_expr(cg, target);
    fprintf(cg->out, "); ");
    free(em);
}

int rc_is_enum_ctor(Codegen *cg, Node *n);

/* fresh heap temp ли е този възел? (owned rc=1 резултат по конвенция)
 * RC5 v1.0b: rc_heap_tag — struct/enum fn резултатът е owned (v1.0a). */
int rc_tmp_fresh(Codegen *cg, Node *n) {
    if (!n) return 0;
    if (n->kind == NODE_TO_STR) {
        /* to_str върху str е identity (borrowed, емитира се пряко) — fresh
         * е само конверсията от не-str тип (baga_i64_to_str /
         * baga_f64_to_str дават нов rc=1 низ; bool дава литерал, чийто
         * release е guarded no-op). str вътрешност, която е fresh call,
         * се регистрира сама при слизане в to_str_expr. */
        Type *et = n->to_str_expr ? n->to_str_expr->type : NULL;
        return !(et && et->kind == TYPE_STR);
    }
    if (n->kind != NODE_CALL) return 0;
    if (rc_borrowed_init(n)) return 0;
    /* enum ctor е като литерал — payload-ът е owned от ctor сайта;
     * не се регистрира като temp (иначе push(Some(x)) би го пуснал) */
    if (rc_is_enum_ctor(cg, n)) return 0;
    return rc_heap_tag(cg, n->type) != 0;
}

/* конструктор на sum enum с payload ли е това повикване? (огледално на
 * emit_call L3/A1 клона) — payload-ът се копира по стойност БЕЗ retain
 * (като struct литерал), temp в аргумента би бил освободен под краката му */
int rc_is_enum_ctor(Codegen *cg, Node *n) {
    if (!cg->program || !n->callee) return 0;
    if (n->callee->kind != NODE_IDENT && n->callee->kind != NODE_PATH)
        return 0;
    for (int i = 0; i < cg->program->items.len; i++) {
        Node *item = cg->program->items.data[i];
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

/* RC5 v1.0b: не-borrowed call с heap резултат — owned по v1.0a конвенция
 * (не struct lit / не enum ctor; тези се познават отделно). */
int rc_is_owned_call(Codegen *cg, Node *n) {
    if (!n || n->kind != NODE_CALL) return 0;
    if (rc_borrowed_init(n)) return 0;
    if (rc_is_enum_ctor(cg, n)) return 0;
    return rc_heap_tag(cg, n->type) != 0;
}

static void rc_tmp_collect(Codegen *cg, Node *n, int is_root) {
    if (!n) return;
    if (rc_tmp_fresh(cg, n) && !is_root) {
        RcTmp t;
        t.site = n;
        t.type = n->type;
        t.tag = n->kind == NODE_TO_STR ? 1 : rc_heap_tag(cg, n->type);
        snprintf(t.name, sizeof t.name, "__rc_tmp%d", cg->tmp_counter++);
        vec_push(cg->rc_tmps, t);
        /* продължаваме надолу — аргументите може да съдържат temp-ове */
    }
    switch (n->kind) {
        case NODE_BINARY:
            rc_tmp_collect(cg, n->left, 0);
            /* &&/||: десният операнд се оценява условно — не се пипа */
            if (n->bin_op != OP_AND && n->bin_op != OP_OR)
                rc_tmp_collect(cg, n->right, 0);
            break;
        case NODE_UNARY:
            rc_tmp_collect(cg, n->operand, 0);
            break;
        case NODE_CALL:
            /* drop(x) е release пътят на x — аргументът не е temp;
             * enum конструктор копира payload без retain (като struct
             * литерал) — temp в него би обесил payload-а */
            if (n->callee && n->callee->kind == NODE_IDENT &&
                strcmp(n->callee->name, "drop") == 0)
                break;
            if (rc_is_enum_ctor(cg, n)) break;
            rc_tmp_collect(cg, n->callee, 0);
            for (int i = 0; i < n->args.len; i++)
                rc_tmp_collect(cg, n->args.data[i], 0);
            break;
        case NODE_INDEX:
            rc_tmp_collect(cg, n->obj, 0);
            rc_tmp_collect(cg, n->index, 0);
            break;
        case NODE_ELEM_REF:
            rc_tmp_collect(cg, n->elem_obj, 0);
            break;
        case NODE_FIELD:
            rc_tmp_collect(cg, n->field_obj, 0);
            break;
        case NODE_RANGE:
            rc_tmp_collect(cg, n->range_lo, 0);
            rc_tmp_collect(cg, n->range_hi, 0);
            break;
        case NODE_TO_STR:
            rc_tmp_collect(cg, n->to_str_expr, 0);
            break;
        case NODE_MATCH: {
            /* RC5 v0.11: scrutinee temp (`match f() { ... }`). Scrutinee-то
             * се оценява безусловно и точно веднъж, ПРЕДИ рамената, а temp
             * release-ът идва в края на statement-а — СЛЕД телата на
             * рамената, така че borrowed binding-ите (v0.6 конвенция)
             * остават валидни. В рамената не се слиза (условни statement-и).
             * Enum ctor scrutinee (`match Ok(concat(...))`) притежава
             * payload референциите си от ctor сайта (v0.6 пр. 4) и никой не
             * ги release-ва след match-а — регистрира се като temp с tag 6.
             * RC5 v1.0b: fn резултат (`match mk()`) вече е owned (v1.0a) и
             * се регистрира през rc_tmp_fresh / rc_heap_tag — release след
             * рамената. */
            Node *sc = n->match_expr;
            if (sc && sc->kind == NODE_CALL && rc_is_enum_ctor(cg, sc) &&
                rc_heap_tag(cg, sc->type) == 6) {
                RcTmp t;
                t.site = sc;
                t.type = sc->type;
                t.tag = 6;
                snprintf(t.name, sizeof t.name, "__rc_tmp%d", cg->tmp_counter++);
                vec_push(cg->rc_tmps, t);
            } else {
                rc_tmp_collect(cg, sc, 0);
            }
            break;
        }
        case NODE_TRY:
            /* RC8: `g()?` на АРГУМЕНТНА позиция (`f(g()?)`,
             * `vec_push(v, env("X")?)`): резултатът е owned (fresh call),
             * а passthrough клонът на TRY не управлява темпове — целият
             * TRY възел се регистрира като temp. Декларацията emit-ва
             * пълния wrapper (с проверката на ефект слота), така че
             * семантиката на `?` се пази; release е в края на statement-а.
             * Вътрешността НЕ се слиза — wrapper-ът сам си управлява
             * temp-овете (emit_rc_stmt_expr_root в двата клона). Като root
             * (let/return/expr statement) стойността е bound — също без
             * слизане; клоновете я wrap-ват локално. */
            if (!is_root && rc_tmp_fresh(cg, n->try_expr)) {
                RcTmp t;
                t.site = n;
                t.type = n->type;
                t.tag = rc_heap_tag(cg, n->type);
                if (t.tag != 0) {
                    snprintf(t.name, sizeof t.name, "__rc_tmp%d", cg->tmp_counter++);
                    vec_push(cg->rc_tmps, t);
                }
            }
            break;
        /* STRUCT_LIT, LAMBDA, CATCH, IF и останалите — не се
         * слиза (escape/отделна fn/условна оценка — вж. коментара по-горе) */
        default:
            break;
    }
}

/* заместване в emit_expr: temp възелът вече е изчислен в __rc_tmpN.
 * Връща 1 и emit-ва името, ако n е регистриран temp. */
int rc_tmp_emit_sub(Codegen *cg, Node *n) {
    if (!cg->rc || !cg->rc_tmps_on || n == cg->rc_tmp_decl) return 0;
    for (int i = 0; i < cg->rc_tmps.len; i++)
        if (cg->rc_tmps.data[i].site == n) {
            fprintf(cg->out, "%s", cg->rc_tmps.data[i].name);
            return 1;
        }
    return 0;
}

/* RC5 v0.7: индекс на регистриран temp за този AST възел (-1 = не е temp).
 * Ползва се от box push/set сайтовете за move на temp аргумент. */
int rc_tmp_find(Codegen *cg, Node *n) {
    if (!cg->rc || !cg->rc_tmps_on || !n) return -1;
    for (int i = 0; i < cg->rc_tmps.len; i++)
        if (cg->rc_tmps.data[i].site == n) return i;
    return -1;
}

/* RC5 v0.7: temp-ът е прехвърлен (move) в контейнер — краят на statement-а
 * не го release-ва. Маркира се СЛЕД emission на аргумента (иначе
 * rc_tmp_emit_sub губи сайта и би преизчислил извикването inline). */
void rc_tmp_consume(Codegen *cg, int i) {
    if (i >= 0) cg->rc_tmps.data[i].site = NULL;
}

/* начало на statement с temp tracking. root_bound=1: root-ът е bound
 * (let init / assign дясно / return стойност) — не е temp. За assign
 * statement се вика с целия NODE_ASSIGN — дясното е bound, целта се чете.
 * Пази/нулира активния регистър (вложени statement-и в if-изрази/ламбди). */
void rc_tmp_begin(Codegen *cg, Node *root, int root_bound,
                         RcTmpVec *saved, int *saved_on) {
    *saved = cg->rc_tmps;
    *saved_on = cg->rc_tmps_on;
    cg->rc_tmps.data = NULL; cg->rc_tmps.len = 0; cg->rc_tmps.cap = 0;
    cg->rc_tmps_on = 0;
    if (!cg->rc || !root) return;
    if (root->kind == NODE_ASSIGN) {
        /* дясното е bound (или escape в поле — пак не се release-ва от нас);
         * сложните цели четат обекта/индекса — там temp-ове са валидни */
        rc_tmp_collect(cg, root->assign_val, 1);
        Node *t = root->assign_target;
        if (t) {
            if (t->kind == NODE_FIELD) rc_tmp_collect(cg, t->field_obj, 0);
            else if (t->kind == NODE_INDEX) {
                rc_tmp_collect(cg, t->obj, 0);
                rc_tmp_collect(cg, t->index, 0);
            }
        }
    } else {
        rc_tmp_collect(cg, root, root_bound);
    }
    if (cg->rc_tmps.len == 0) return;
    cg->rc_tmps_on = 1;
    FILE *f = cg->out;
    /* декларации в обратен ред на събирането — вътрешните temp-ове първи */
    for (int i = cg->rc_tmps.len - 1; i >= 0; i--) {
        emit_indent(cg);
        fprintf(f, "__auto_type %s = ", cg->rc_tmps.data[i].name);
        cg->rc_tmp_decl = cg->rc_tmps.data[i].site;
        emit_expr(cg, cg->rc_tmps.data[i].site);
        cg->rc_tmp_decl = NULL;
        fprintf(f, ";\n");
    }
}

/* release на активните temp-ове (в реда на събирането = обратен на
 * декларациите) и изчистване; вика се след statement-а (или вътре в
 * return блока — там return-ът е преди края на statement-а) */
void rc_tmp_release_all(Codegen *cg) {
    if (!cg->rc || !cg->rc_tmps_on) return;
    for (int i = 0; i < cg->rc_tmps.len; i++) {
        RcTmp *t = &cg->rc_tmps.data[i];
        /* RC5 v0.7: прехвърлен в контейнер (move) — собствеността е на box-а */
        if (!t->site) continue;
        RcLocal e = {0};
        e.name = (char *)t->name;
        e.tag = t->tag;
        e.type = t->type;
        rc_emit_release(cg, &e);
        cg->rc_tmp_count++;
    }
    cg->rc_tmps.len = 0;
    cg->rc_tmps_on = 0;
}

/* край на statement-а: release (ако не е направен) + възстановяване */
void rc_tmp_end(Codegen *cg, RcTmpVec *saved, int saved_on) {
    rc_tmp_release_all(cg);
    vec_free(cg->rc_tmps);
    cg->rc_tmps = *saved;
    cg->rc_tmps_on = saved_on;
}

/* peek: cond/expr има ли fresh heap temp-ове? Не пипа броячите. */
int rc_tmp_would_collect(Codegen *cg, Node *n) {
    if (!cg->rc || !n) return 0;
    RcTmpVec saved = cg->rc_tmps;
    int saved_on = cg->rc_tmps_on;
    int saved_tc = cg->tmp_counter;
    cg->rc_tmps.data = NULL; cg->rc_tmps.len = 0; cg->rc_tmps.cap = 0;
    rc_tmp_collect(cg, n, 0);
    int ntmp = cg->rc_tmps.len;
    vec_free(cg->rc_tmps);
    cg->rc_tmps = saved;
    cg->rc_tmps_on = saved_on;
    cg->tmp_counter = saved_tc;
    return ntmp;
}

/* RC4 v0.3 / v1.1: израз с локални temp-ове. Ако има — GNU
 * statement-expression `({ decls; __auto_type __rc_cN = expr; releases;
 * __rc_cN; })`. root_bound=1: самият n е bound (if-израз клон / match arm
 * стойност / catch handler) — не се регистрира като temp; вложените се
 * пускат след оценката. root_bound=0: условието на if/while/for и десният
 * операнд на &&/|| — temp-овете в него се пускат веднага след оценката,
 * преди тялото / следващия операнд. Без temp-ове — обикновен emit_expr. */
void emit_rc_stmt_expr_root(Codegen *cg, Node *n, int root_bound) {
    if (!n) { fprintf(cg->out, "0"); return; }
    if (!cg->rc) { emit_expr(cg, n); return; }
    RcTmpVec saved = cg->rc_tmps;
    int saved_on = cg->rc_tmps_on;
    cg->rc_tmps.data = NULL; cg->rc_tmps.len = 0; cg->rc_tmps.cap = 0;
    cg->rc_tmps_on = 0;
    rc_tmp_collect(cg, n, root_bound);
    if (cg->rc_tmps.len == 0) {
        vec_free(cg->rc_tmps);
        cg->rc_tmps = saved;
        cg->rc_tmps_on = saved_on;
        emit_expr(cg, n);
        return;
    }
    cg->rc_tmps_on = 1;
    FILE *f = cg->out;
    fprintf(f, "({\n");
    cg->indent++;
    for (int i = cg->rc_tmps.len - 1; i >= 0; i--) {
        emit_indent(cg);
        fprintf(f, "__auto_type %s = ", cg->rc_tmps.data[i].name);
        cg->rc_tmp_decl = cg->rc_tmps.data[i].site;
        emit_expr(cg, cg->rc_tmps.data[i].site);
        cg->rc_tmp_decl = NULL;
        fprintf(f, ";\n");
    }
    emit_indent(cg);
    if (n->type && n->type->kind == TYPE_VOID) {
        /* print/дискарднат expr — няма стойност за хващане */
        emit_expr(cg, n);
        fprintf(f, ";\n");
        rc_tmp_release_all(cg);
    } else {
        int cnum = cg->tmp_counter++;
        fprintf(f, "__auto_type __rc_c%d = ", cnum);
        emit_expr(cg, n);
        fprintf(f, ";\n");
        rc_tmp_release_all(cg);
        emit_indent(cg);
        fprintf(f, "__rc_c%d;\n", cnum);
    }
    cg->indent--;
    emit_indent(cg);
    fprintf(f, "})");
    vec_free(cg->rc_tmps);
    cg->rc_tmps = saved;
    cg->rc_tmps_on = saved_on;
}

void emit_rc_stmt_expr(Codegen *cg, Node *n) {
    emit_rc_stmt_expr_root(cg, n, 0);
}

/* RC5 v1.0a: трябва ли return/arm стойност да се retain-не, за да е
 * резултатът owned? Свеж литерал/ctor и не-borrowed call (callee вече
 * връща owned по същата конвенция) — не. Match се обработва в рамената.
 * Всичко друго с heap tag (vec_get/поле, ident, if-израз) — да
 * (при съмнение leak-safe). */
int rc_need_owned_retain(Codegen *cg, Node *val) {
    if (!cg->rc || !val) return 0;
    if (!rc_heap_tag(cg, val->type)) return 0;
    if (val->kind == NODE_STRUCT_LIT) return 0;
    if (val->kind == NODE_CALL && rc_is_enum_ctor(cg, val)) return 0;
    if (val->kind == NODE_CALL && !rc_borrowed_init(val)) return 0;
    if (val->kind == NODE_MATCH) return 0;
    /* RC9b: `expr?` е ownership passthrough (при success стойността е
     * точно резултатът на вътрешния израз) — иначе `return f()?` retain-ва
     * свежия owned резултат втори път и тече по един struct на заявка
     * (otel_from_header/act_ready/act_login). Същото и за catch верига
     * без payload ефекти (compile-time фикция — стойността е базовият
     * израз); с payload консервативно retain-ваме (може да е от handler). */
    if (val->kind == NODE_TRY && val->try_expr)
        return rc_need_owned_retain(cg, val->try_expr);
    if (val->kind == NODE_CATCH) {
        Node *scan = val;
        int any_payload = 0;
        while (scan && scan->kind == NODE_CATCH) {
            Type *et = scan->catch_expr ? scan->catch_expr->type : NULL;
            if (type_effect_payload(et, scan->catch_effect)) any_payload = 1;
            scan = scan->catch_expr;
        }
        if (!any_payload && scan) return rc_need_owned_retain(cg, scan);
    }
    return 1;
}

/* RC1.2: стойност на match arm (когато match-ът произвежда heap стойност) —
 * borrowed израз (vec_get/map_get/поле/h_*) или ident (match binding е
 * копие-алиас на payload; track-нат локал е втори собственик) се retain-ва,
 * за да е резултатът owned по конвенцията „fn резултат = owned". Иначе
 * scope release на източника обесва/underflow-ва консуматора (латентен
 * пропуск, маскиран от temp течовете преди RC4 — pg_err/sqlstate).
 * RC5 v1.0a: rc_heap_tag вместо rc_type_tag — struct/enum рамена също
 * (tag 5/6 през retain_S/retain_E; void* cast върху стойност е невалиден). */
void rc_emit_match_arm_val(Codegen *cg, Node *rv, int tmp,
                                  int is_void) {
    FILE *f = cg->out;
    if (is_void) { emit_rc_stmt_expr_root(cg, rv, 1); return; }
    int rtag = (cg->rc && rv) ? rc_heap_tag(cg, rv->type) : 0;
    if (!rtag || !rc_need_owned_retain(cg, rv)) {
        fprintf(f, "_mr%d = ", tmp);
        emit_rc_stmt_expr_root(cg, rv, 1);
        return;
    }
    fprintf(f, "({ __auto_type __rc_m = ");
    emit_rc_stmt_expr_root(cg, rv, 1);
    fprintf(f, "; ");
    rc_emit_retain_val(cg, rtag, rv->type, NULL, "__rc_m");
    fprintf(f, "_mr%d = __rc_m; })", tmp);
}


/* ---- type mapping ---- */

void emit_ctype(Codegen *cg, Type *t);
void emit_type(Codegen *cg, Node *ty) {
    FILE *f = cg->out;
    if (!ty) { fprintf(f, "void"); return; }

    /* M24: instantiated generic struct — checked типът (с targs) печели */
    if (ty->type && ty->type->kind == TYPE_STRUCT && ty->type->n_targs > 0 &&
        ty->kind == NODE_TYPE) {
        emit_ctype(cg, ty->type);
        return;
    }

    /* M24: типова променлива на текущата generic struct инстанция */
    if (cg->gen_struct && ty->type && ty->kind == NODE_TYPE) {
        for (int i = 0; i < cg->gen_struct->n_struct_params; i++)
            if (strcmp(cg->gen_struct->struct_params[i], ty->type_name) == 0) {
                Type *at = cg->gen_struct->struct_inst_targs[
                    cg->gen_struct_inst * cg->gen_struct->n_struct_params + i];
                emit_ctype(cg, at);
                return;
            }
    }

    /* M21: типова променлива на текущата generic инстанция — inst_types
     * (като LLVM), не checked TYPE_VAR върху възела */
    if (cg->gen_fn && ty->kind == NODE_TYPE && ty->type_name) {
        for (int i = 0; i < cg->gen_fn->n_type_params; i++)
            if (strcmp(cg->gen_fn->type_params[i], ty->type_name) == 0) {
                Type *at = cg->gen_fn->inst_types[
                    cg->gen_inst * cg->gen_fn->n_type_params + i];
                emit_ctype(cg, at ? at : ty->type);
                return;
            }
    }

    switch (ty->kind) {
        case NODE_TYPE:
            if (strcmp(ty->type_name, "i32") == 0)   fprintf(f, "int32_t");
            else if (strcmp(ty->type_name, "i64") == 0) fprintf(f, "int64_t");
            else if (strcmp(ty->type_name, "f64") == 0) fprintf(f, "double");
            else if (strcmp(ty->type_name, "bool") == 0) fprintf(f, "int");
            else if (strcmp(ty->type_name, "str") == 0)  fprintf(f, "const char *");
            else if (strcmp(ty->type_name, "bytes") == 0) fprintf(f, "baga_bytes");
            else if (strcmp(ty->type_name, "void") == 0) fprintf(f, "void");
            else if (strcmp(ty->type_name, "Vec") == 0)  fprintf(f, "baga_Vec *");
            else if (strcmp(ty->type_name, "Map") == 0)  fprintf(f, "baga_Map *");
            else {
                /* struct or unknown → use mangled name */
                emit_mangled(f, ty->type_name);
            }
            break;
        case NODE_TYPE_REF:
            emit_type(cg, ty->inner_type);
            fprintf(f, " *");
            break;
        case NODE_TYPE_ARRAY:
            /* [T] == Vec<T>: the growable baga_Vec, not a raw pointer */
            fprintf(f, "baga_Vec *");
            break;
        case NODE_TYPE_EFFECT:
            /* effects don't affect C codegen */
            emit_type(cg, ty->inner_type);
            break;
        default:
            fprintf(f, "int64_t");
            break;
    }
}

/* C spelling от проверен (checker) Type — за fn-стойностните сигнатури (L5) */
/* M24: C име на instantiated generic struct ("b_Pair_i64_str") */
char *struct_cname_str(Type *t) {
    char *m = mangle_name(t->name ? t->name : "anon");
    size_t cap = strlen(m) + 1 + (size_t)(t->n_targs > 0 ? t->n_targs : 0) * 48;
    char *out = malloc(cap);
    if (!out) { fprintf(stderr, "baga: out of memory\n"); exit(1); }
    strcpy(out, m);
    free(m);
    for (int a = 0; a < t->n_targs; a++) {
        Type *at = t->targs[a];
        if (at->kind == TYPE_STRUCT && at->name) {
            char *am = mangle_name(at->name);
            strcat(out, "_"); strcat(out, am + 2);
            free(am);
        } else if (at->kind == TYPE_ENUM && at->name) {
            char *am = mangle_name(at->name);
            strcat(out, "_"); strcat(out, am + 2);
            free(am);
        } else if (at->kind == TYPE_STR) strcat(out, "_str");
        else if (at->kind == TYPE_BYTES) strcat(out, "_bytes");
        else if (at->kind == TYPE_F64) strcat(out, "_f64");
        else if (at->kind == TYPE_BOOL) strcat(out, "_bool");
        else if (at->kind == TYPE_VEC) strcat(out, "_v");
        else if (at->kind == TYPE_MAP) strcat(out, "_m");
        else strcat(out, "_i64");
    }
    return out;
}

void emit_ctype(Codegen *cg, Type *t) {
    FILE *f = cg->out;
    if (!t) { fprintf(f, "void"); return; }
    switch (t->kind) {
        case TYPE_I32:   fprintf(f, "int32_t"); break;
        case TYPE_I64:   fprintf(f, "int64_t"); break;
        case TYPE_F64:   fprintf(f, "double"); break;
        case TYPE_BOOL:  fprintf(f, "int"); break;
        case TYPE_STR:   fprintf(f, "const char *"); break;
        case TYPE_BYTES: fprintf(f, "baga_bytes"); break;
        case TYPE_VOID:  fprintf(f, "void"); break;
        case TYPE_VEC:   fprintf(f, "baga_Vec *"); break;
        case TYPE_MAP:   fprintf(f, "baga_Map *"); break;
        case TYPE_STRUCT:
            /* M24: instantiated generic struct — конкретно C име */
            if (t->n_targs > 0) {
                char *cn = struct_cname_str(t);
                fprintf(f, "%s", cn);
                free(cn);
            } else {
                emit_mangled(f, t->name ? t->name : "anon");
            }
            break;
        case TYPE_ENUM:  emit_mangled(f, t->name ? t->name : "anon"); break;
        default:         fprintf(f, "int64_t"); break;   /* TYPE_FN → handle */
    }
}

