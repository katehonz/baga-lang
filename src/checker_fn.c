/* checker_fn.c — част от checker.c (виж ck_proto.h). */
#include "baga.h"
#include "checker_impl.h"

/* ============================================================
 *  Function checking
 * ============================================================ */

/* M19: fallthrough анализ — може ли изпълнението да падне от края на
 * този statement? Консервативен: ако не може да се докаже обратното,
 * отговорът е "да". return/break/continue/raise не падат; block пада, ако
 * последният stmt пада; if без else пада; while пада, освен literal
 * `while true` без break на това ниво; match винаги пада (arm-ският
 * `return e` е СТОЙНОСТ на match-а, не изход от функцията). */
static int tree_has_break_depth(Node *n, int depth) {
    if (!n) return 0;
    if (n->kind == NODE_BREAK) return depth == 0;
    switch (n->kind) {
        case NODE_BINARY:
            return tree_has_break_depth(n->left, depth) ||
                   tree_has_break_depth(n->right, depth);
        case NODE_UNARY:
            return tree_has_break_depth(n->operand, depth);
        case NODE_CALL:
            for (int i = 0; i < n->args.len; i++)
                if (tree_has_break_depth(n->args.data[i], depth)) return 1;
            return 0;
        case NODE_IF:
            return tree_has_break_depth(n->then_br, depth) ||
                   tree_has_break_depth(n->else_br, depth);
        case NODE_BLOCK:
            for (int i = 0; i < n->stmts.len; i++)
                if (tree_has_break_depth(n->stmts.data[i], depth)) return 1;
            return 0;
        case NODE_WHILE:
            if (tree_has_break_depth(n->while_cond, depth)) return 1;
            return tree_has_break_depth(n->while_body, depth + 1);
        case NODE_FOR:
            return tree_has_break_depth(n->for_body, depth + 1);
        case NODE_MATCH:
            for (int i = 0; i < n->match_arms.len; i++) {
                Node *arm = n->match_arms.data[i];
                if (tree_has_break_depth(arm->arm_body, depth)) return 1;
            }
            return 0;
        case NODE_ASSIGN:
            return tree_has_break_depth(n->assign_target, depth) ||
                   tree_has_break_depth(n->assign_val, depth);
        case NODE_TRY:
            return tree_has_break_depth(n->try_expr, depth);
        case NODE_CATCH:
            return tree_has_break_depth(n->catch_expr, depth) ||
                   tree_has_break_depth(n->catch_handler, depth);
        default:
            return 0;
    }
}

static int stmt_falls_through(Node *n) {
    if (!n) return 1;
    switch (n->kind) {
        case NODE_RETURN:
        case NODE_BREAK:
        case NODE_CONTINUE:
        case NODE_RAISE:
            return 0;
        case NODE_EXPR_STMT:
            return stmt_falls_through(n->expr);
        case NODE_BLOCK: {
            if (n->stmts.len == 0) return 1;
            return stmt_falls_through(n->stmts.data[n->stmts.len - 1]);
        }
        case NODE_IF:
            if (!n->else_br) return 1;
            if (stmt_falls_through(n->then_br)) return 1;
            return stmt_falls_through(n->else_br);
        case NODE_WHILE:
            /* literal `while true` без break на това ниво не излиза */
            if (n->while_cond && n->while_cond->kind == NODE_BOOL_LIT &&
                n->while_cond->bool_val == 1 &&
                !tree_has_break_depth(n->while_body, 0))
                return 0;
            return 1;
        default:
            return 1;
    }
}

/* Тялото на функция: последният изразен statement е implicit return
 * (codegen го превръща в `return expr;` за не-void функции). */
static int fn_body_falls_through(Node *body) {
    if (!body || body->stmts.len == 0) return 1;
    Node *last = body->stmts.data[body->stmts.len - 1];
    if (last->kind == NODE_EXPR_STMT) return 0;
    return stmt_falls_through(last);
}

/* M21/M27: generic_instantiate пренаписва callee->name на `име__iN` върху
 * споделения AST. Преди recheck на следващата инстанция на ОБГРАЖДАЩИЯ
 * generic (wrap<i64> после wrap<str> — и двете викат id(x)) името трябва
 * да се върне, иначе ck_find_fn вижда 'id__i0' и казва „непозната функция". */
static void restore_synth_inst_name(Node *n) {
    if (!n || n->kind != NODE_IDENT || !n->name) return;
    const char *nm = n->name;
    const char *p = strstr(nm, "__i");
    if (!p || p == nm) return;
    const char *d = p + 3;
    if (*d < '0' || *d > '9') return;
    while (*d) {
        if (*d < '0' || *d > '9') return;
        d++;
    }
    size_t blen = (size_t)(p - nm);
    char *base = malloc(blen + 1);
    if (!base) { fprintf(stderr, "baga: out of memory\n"); exit(1); }
    memcpy(base, nm, blen);
    base[blen] = '\0';
    n->name = base;   /* leak-safe: старото synth име не се free-ва */
}

/* M23: връща method call-овете към FIELD форма (rewrite-ът е деструктивен
 * и всяка инстанция започва от чистата форма) И чисти мемоизираните типове
 * (infer кешира през n->type — recheck-ът на инстанция трябва да преинферира
 * всичко с конкретните типове). Разпознава вътрешните имена
 * "Trait.Type.method" по ДВЕТЕ точки. */
static void restore_method_calls(Node *n) {
    if (!n) return;
    n->type = NULL;   /* M21/M23: преинферирай под новата substitution */
    switch (n->kind) {
        case NODE_IDENT:
            restore_synth_inst_name(n);
            return;
        case NODE_CALL: {
            if (n->callee) n->callee->type = NULL;   /* M23: callee-то също */
            if (n->callee->kind == NODE_IDENT)
                restore_synth_inst_name(n->callee);
            if (n->callee->kind == NODE_IDENT && n->callee->name) {
                const char *nm = n->callee->name;
                const char *d1 = strchr(nm, '.');
                if (d1 && strchr(d1 + 1, '.') && n->args.len > 0) {
                    Node *obj = n->args.data[0];
                    n->callee->kind = NODE_FIELD;
                    n->callee->field_name = strdup(strrchr(nm, '.') + 1);
                    n->callee->field_obj = obj;
                    for (int a = 0; a + 1 < n->args.len; a++)
                        n->args.data[a] = n->args.data[a + 1];
                    n->args.len--;
                }
            }
            if (n->callee->kind == NODE_FIELD)
                restore_method_calls(n->callee);   /* field_obj-ът (obj) също */
            for (int i = 0; i < n->args.len; i++)
                restore_method_calls(n->args.data[i]);
            for (int i = 0; i < n->type_args.len; i++)
                restore_method_calls(n->type_args.data[i]);
            return;
        }
        case NODE_BINARY:
            restore_method_calls(n->left);
            restore_method_calls(n->right);
            return;
        case NODE_UNARY: restore_method_calls(n->operand); return;
        case NODE_IF:
            restore_method_calls(n->cond);
            restore_method_calls(n->then_br);
            restore_method_calls(n->else_br);
            return;
        case NODE_BLOCK:
            for (int i = 0; i < n->stmts.len; i++)
                restore_method_calls(n->stmts.data[i]);
            return;
        case NODE_WHILE:
            restore_method_calls(n->while_cond);
            restore_method_calls(n->while_body);
            return;
        case NODE_FOR:
            restore_method_calls(n->for_iter);
            restore_method_calls(n->for_body);
            return;
        case NODE_RETURN: restore_method_calls(n->ret_val); return;
        case NODE_LET:
            restore_method_calls(n->let_type);
            restore_method_calls(n->let_init);
            return;
        case NODE_ASSIGN:
            restore_method_calls(n->assign_target);
            restore_method_calls(n->assign_val);
            return;
        case NODE_MATCH:
            restore_method_calls(n->match_expr);
            for (int i = 0; i < n->match_arms.len; i++)
                restore_method_calls(n->match_arms.data[i]->arm_body);
            return;
        case NODE_FIELD: restore_method_calls(n->field_obj); return;
        case NODE_INDEX:
            restore_method_calls(n->obj);
            restore_method_calls(n->index);
            return;
        case NODE_TRY: restore_method_calls(n->try_expr); return;
        case NODE_CATCH:
            restore_method_calls(n->catch_expr);
            restore_method_calls(n->catch_handler);
            return;
        case NODE_RAISE: restore_method_calls(n->raise_payload); return;
        case NODE_TO_STR: restore_method_calls(n->to_str_expr); return;
        case NODE_STRUCT_LIT:
            for (int i = 0; i < n->n_lit_fields; i++)
                restore_method_calls(n->lit_values.data[i]);
            return;
        case NODE_EXPR_STMT: restore_method_calls(n->expr); return;
        case NODE_LAMBDA: restore_method_calls(n->fn_body); return;
        default: return;
    }
}

/* M23: impl-името на тип (съвпада с pre-pass регистрацията) */
static const char *type_impl_name(Type *t) {    if (!t) return NULL;
    switch (t->kind) {
        case TYPE_STRUCT:
        case TYPE_ENUM:  return t->name;
        case TYPE_I64:   return "i64";
        case TYPE_STR:   return "str";
        case TYPE_BYTES: return "bytes";
        case TYPE_F64:   return "f64";
        case TYPE_BOOL:  return "bool";
        case TYPE_VEC:   return "Vec";
        case TYPE_MAP:   return "Map";
        default:         return NULL;
    }
}

/* M21: свързва типови параметри от типовия възел на параметър с типа на
 * аргумента (извод). Vec/Map/fn структурата се обхожда. */
void bind_type_params(CheckCtx *ctx, Node *tn, Type *at,
                             char **tps, int np, Type **bind,
                             SrcPos pos, const char *fnname) {
    if (!tn || !at) return;
    if (tn->kind == NODE_TYPE_EFFECT) {
        bind_type_params(ctx, tn->inner_type, at, tps, np, bind, pos, fnname);
        return;
    }
    if (tn->kind == NODE_TYPE_REF || tn->kind == NODE_TYPE_ARRAY) {
        bind_type_params(ctx, tn->inner_type, at, tps, np, bind, pos, fnname);
        return;
    }
    if (tn->kind != NODE_TYPE) return;
    for (int i = 0; i < np; i++) {
        if (strcmp(tps[i], tn->type_name) == 0) {
            if (bind[i] && !type_eq(bind[i], at)) {
                check_error(ctx, pos,
                    "'%s': типовият параметър '%s' е изведен и като %s, и като %s — нееднозначно",
                    fnname, tps[i], type_str(bind[i]), type_str(at));
            } else {
                /* M25: плитко копие — substitution връща bind обекта през
                 * resolve_type_node (g_types), а type_add_effect върху него
                 * (`-> T !IO` при check на инстанцията) не трябва да мутира
                 * типа на аргумента/анотацията (споделени обекти) */
                Type *cp = type_new(at->kind);
                *cp = *at;
                bind[i] = cp;
            }
            return;
        }
    }
    if (strcmp(tn->type_name, "Vec") == 0 && at->kind == TYPE_VEC)
        bind_type_params(ctx, tn->inner_type, at->elem, tps, np, bind, pos, fnname);
    if (strcmp(tn->type_name, "Map") == 0 && at->kind == TYPE_MAP) {
        bind_type_params(ctx, tn->inner_type, at->key, tps, np, bind, pos, fnname);
        bind_type_params(ctx, tn->inner_type2, at->elem, tps, np, bind, pos, fnname);
    }
    /* M24: generic struct параметър — Pair<A, B> срещу instantiated тип */
    if (tn->gen_type_args.len > 0 && at->kind == TYPE_STRUCT && at->n_targs > 0) {
        int nn = tn->gen_type_args.len < at->n_targs ? tn->gen_type_args.len : at->n_targs;
        for (int a = 0; a < nn; a++)
            bind_type_params(ctx, tn->gen_type_args.data[a], at->targs[a],
                             tps, np, bind, pos, fnname);
    }
}

/* M27: spec върху generic — типовите параметри на fn се виждат като
 * TYPE_VAR в spec-а; ensures/requires се инферират в този обхват. */
static Node *find_spec_item(CheckCtx *ctx, const char *name) {
    if (!ctx->program || !name) return NULL;
    for (int i = 0; i < ctx->program->items.len; i++) {
        Node *it = ctx->program->items.data[i];
        if (it->kind == NODE_SPEC && strcmp(it->spec_name, name) == 0)
            return it;
    }
    return NULL;
}

/* W2.7: договорите за свят в requires — събира ги за един параметър (през AND,
 * както верификатора). frozen/mutable/owned се изключват взаимно: два договора
 * за един параметър не са UB, но правят функцията неизвикваема (всеки аргумент
 * е обречен от някой от двата discharge-а), което е очевидно — затова е
 * compile-time грешка, а не два контрапримера. */
static void collect_spec_world_contracts(Node *e, const char *pn, int *kinds,
                                         const char **names, int *n, int cap) {
    if (!e || *n >= cap) return;
    if (e->kind == NODE_CALL && e->callee && e->callee->kind == NODE_IDENT &&
        e->args.len == 1 && e->args.data[0]->kind == NODE_IDENT &&
        strcmp(e->args.data[0]->name, pn) == 0) {
        const char *f = e->callee->name;
        int k = strcmp(f, "world_frozen") == 0 ? 1 :
                strcmp(f, "world_mutable") == 0 ? 2 :
                strcmp(f, "world_owned") == 0 ? 3 : 0;
        if (k) { kinds[*n] = k; names[*n] = f; (*n)++; }
        return;
    }
    if (e->kind == NODE_BINARY && e->bin_op == OP_AND) {
        collect_spec_world_contracts(e->left, pn, kinds, names, n, cap);
        collect_spec_world_contracts(e->right, pn, kinds, names, n, cap);
    }
}

static void check_spec_world_contracts(CheckCtx *ctx, Node *item) {
    for (int j = 0; j < item->spec_inputs.len; j++) {
        const char *pn = item->spec_inputs.data[j]->param_name;
        int kinds[8]; const char *names[8]; int n = 0;
        for (int r = 0; r < item->spec_requires.len && n < 8; r++)
            collect_spec_world_contracts(item->spec_requires.data[r]->ensure_expr,
                                         pn, kinds, names, &n, 8);
        for (int a = 1; a < n; a++) {
            if (kinds[a] == kinds[0]) continue;   /* един и същ договор — излишно, но безвредно */
            check_error(ctx, item->pos,
                "spec '%s': параметър '%s' има два договора за свят (%s и %s) — избери един",
                item->spec_name, pn, names[0], names[a]);
            break;
        }
    }
}

/* W3.1: гаранция за върнат свят в ensures — `world_frozen(output)` /
 * `world_owned(output)`. Връща 0/1/3 (frozen/owned) за spec-а на име `fname`;
 * използва се от expr_is_world (let-резултатът е handle) и от верификатора
 * през caller-страната. */
int spec_world_guarantee(CheckCtx *ctx, const char *fname) {
    Node *item = find_spec_item(ctx, fname);
    if (!item) return 0;
    for (int r = 0; r < item->spec_ensures.len; r++) {
        Node *e = item->spec_ensures.data[r]->ensure_expr;
        if (!e || e->kind != NODE_CALL || !e->callee || e->callee->kind != NODE_IDENT ||
            e->args.len != 1) continue;
        int k = strcmp(e->callee->name, "world_frozen") == 0 ? 1 :
                strcmp(e->callee->name, "world_owned") == 0 ? 3 : 0;
        if (k && e->args.data[0]->kind == NODE_IDENT &&
            strcmp(e->args.data[0]->name, "output") == 0) return k;
    }
    return 0;
}

/* W3.1: диагностика на световните гаранции в ensures. Целта трябва да е
 * `output` (гаранция за друго име няма смисъл — не е резултатът),
 * `world_mutable(output)` няма смисъл (резултатът се ОБЕЩАВА, не се пише по
 * време на повикване), и най-много ЕДНА гаранция — две си противоречат. */
static void check_spec_world_guarantees(CheckCtx *ctx, Node *item) {
    const char *gname = NULL; int gk = 0;
    for (int r = 0; r < item->spec_ensures.len; r++) {
        Node *e = item->spec_ensures.data[r]->ensure_expr;
        if (!e || e->kind != NODE_CALL || !e->callee || e->callee->kind != NODE_IDENT ||
            e->args.len != 1 || e->args.data[0]->kind != NODE_IDENT) continue;
        const char *f = e->callee->name;
        int k = strcmp(f, "world_frozen") == 0 ? 1 :
                strcmp(f, "world_mutable") == 0 ? 2 :
                strcmp(f, "world_owned") == 0 ? 3 : 0;
        if (!k) continue;
        if (strcmp(e->args.data[0]->name, "output") != 0) {
            check_error(ctx, item->pos,
                "spec '%s': гаранция за свят важи само за 'output' — '%s(%s)' не е резултатът",
                item->spec_name, f, e->args.data[0]->name);
        }
        if (k == 2) {
            check_error(ctx, item->pos,
                "spec '%s': 'world_mutable' няма смисъл като гаранция — резултатът се "
                "обещава, не се пише по време на повикване; използвай world_frozen "
                "(върнат замразен свят) или world_owned (прехвърлена собственост)",
                item->spec_name);
        }
        if (gk) {
            check_error(ctx, item->pos,
                "spec '%s': две гаранции за свят (%s и %s) — избери една",
                item->spec_name, gname, f);
            break;
        }
        gk = k; gname = f;
    }
}

static void infer_spec_contracts(CheckCtx *ctx, Node *item, Type *fn_ret) {
    if (item->spec_ensures.len > 0) {
        if (fn_ret->kind == TYPE_VOID) {
            check_error(ctx, item->pos,
                "spec '%s': ensures изисква функция с върнат тип",
                item->spec_name);
        } else {
            push_scope(ctx);
            for (int j = 0; j < item->spec_inputs.len; j++) {
                Node *sp = item->spec_inputs.data[j];
                env_define(ctx, sp->param_name,
                           resolve_type_node(ctx, sp->param_type), sp->pos);
            }
            env_define(ctx, "output", fn_ret, item->pos);
            for (int j = 0; j < item->spec_ensures.len; j++) {
                Node *en = item->spec_ensures.data[j];
                Type *et = infer(ctx, en->ensure_expr);
                if (et->kind != TYPE_BOOL && et->kind != TYPE_ERROR) {
                    check_error(ctx, en->pos,
                        "spec '%s': ensures #%d е %s, очаквах bool",
                        item->spec_name, j + 1, type_str(et));
                }
            }
            pop_scope(ctx);
        }
    }
    if (item->spec_requires.len > 0) {
        push_scope(ctx);
        for (int j = 0; j < item->spec_inputs.len; j++) {
            Node *sp = item->spec_inputs.data[j];
            env_define(ctx, sp->param_name,
                       resolve_type_node(ctx, sp->param_type), sp->pos);
        }
        for (int j = 0; j < item->spec_requires.len; j++) {
            Node *rq = item->spec_requires.data[j];
            Type *rt = infer(ctx, rq->ensure_expr);
            if (rt->kind != TYPE_BOOL && rt->kind != TYPE_ERROR) {
                check_error(ctx, rq->pos,
                    "spec '%s': requires #%d е %s, очаквах bool",
                    item->spec_name, j + 1, type_str(rt));
            }
        }
        check_spec_world_contracts(ctx, item);   /* W2.7: по един договор на параметър */
        pop_scope(ctx);
    }
    check_spec_world_guarantees(ctx, item);   /* W3.1: гаранция за върнат свят (без requires също) */
    if (item->spec_decreases) {
        push_scope(ctx);
        for (int j = 0; j < item->spec_inputs.len; j++) {
            Node *sp = item->spec_inputs.data[j];
            env_define(ctx, sp->param_name,
                       resolve_type_node(ctx, sp->param_type), sp->pos);
        }
        Type *dt = infer(ctx, item->spec_decreases);
        if (dt->kind != TYPE_I64 && dt->kind != TYPE_ERROR &&
            dt->kind != TYPE_VAR) {
            check_error(ctx, item->spec_decreases->pos,
                "spec '%s': decreases е %s, очаквах i64",
                item->spec_name, type_str(dt));
        }
        pop_scope(ctx);
    }
}

/* M25: ядрото на generic_instantiate — намира/добавя инстанция по bind[]
 * и проверява тялото под substitution (веднъж per инстанция). Поддържа
 * inst_as_value (parallel маркер). Връща индекса на инстанцията. */
static int generic_inst_add(CheckCtx *ctx, Node *fn, Type **bind, SrcPos pos) {
    int np = fn->n_type_params;
    /* потърси/добави инстанция */
    int idx = -1;
    for (int k = 0; k < fn->inst_count; k++) {
        int same = 1;
        for (int i = 0; i < np; i++)
            if (!type_eq(fn->inst_types[k * np + i], bind[i])) { same = 0; break; }
        if (same) { idx = k; break; }
    }
    if (idx >= 0) { free(bind); return idx; }   /* дубликат — bind не се пази */
    idx = fn->inst_count;
    fn->inst_types = realloc(fn->inst_types,
        sizeof(Type *) * (size_t)((idx + 1) * (np ? np : 1)));
    fn->inst_as_value = realloc(fn->inst_as_value, sizeof(int) * (size_t)(idx + 1));
    if (!fn->inst_types || !fn->inst_as_value) {
        fprintf(stderr, "baga: out of memory\n"); exit(1);
    }
    for (int i = 0; i < np; i++)
        fn->inst_types[idx * np + i] = bind[i];
    fn->inst_as_value[idx] = 0;
    fn->inst_count = idx + 1;
    /* проверка на тялото под substitution (веднъж per инстанция) —
     * M21: пази/възстанови състоянието на обграждащия infer */
    const char *saved_fn = ctx->cur_fn;
    Type *saved_ret = ctx->cur_ret;
    Type *saved_eff = ctx->cur_effects;
    int saved_loop = ctx->loop_depth;
    int saved_dlog = ctx->n_drop_log;
    int saved_ovf = ctx->drop_log_overflowed;
    int saved_flog = ctx->n_freeze_log;
    int saved_fovf = ctx->freeze_log_overflowed;
    EnvEntry *saved_dlog_arr[256];
    EnvEntry *saved_flog_arr[256];
    memcpy(saved_dlog_arr, ctx->drop_log, sizeof saved_dlog_arr);
    memcpy(saved_flog_arr, ctx->freeze_log, sizeof saved_flog_arr);
    ctx->g_names.len = 0; ctx->g_types.len = 0;
    for (int i = 0; i < np; i++) {
        vec_push(ctx->g_names, fn->type_params[i]);
        vec_push(ctx->g_types, bind[i]);
    }
    restore_method_calls(fn->fn_body);
    /* M21: всяка нова инстанция проверява тялото под СВОЯТА
     * substitution — checked флагът (MEM-3) не трябва да го блокира,
     * иначе втората инстанция остава с типовете на първата. */
    FnRec *irec = fn_rec_of(ctx, fn);
    if (irec) { irec->checked = 0; irec->checking = 0; }
    check_fn(ctx, fn);
    /* M27: spec върху generic — ensures/requires под същата substitution
     * (T=str в `x + 1` е compile грешка на инстанцията, не на декларацията) */
    {
        Node *spec = find_spec_item(ctx, fn->fn_name);
        if (spec) {
            Type *fn_ret = fn->ret_type ? resolve_type_node(ctx, fn->ret_type)
                                        : type_new(TYPE_VOID);
            infer_spec_contracts(ctx, spec, fn_ret);
        }
    }
    ctx->cur_fn = saved_fn;
    ctx->cur_ret = saved_ret;
    ctx->cur_effects = saved_eff;
    ctx->loop_depth = saved_loop;
    ctx->n_drop_log = saved_dlog;
    ctx->drop_log_overflowed = saved_ovf;
    ctx->n_freeze_log = saved_flog;
    ctx->freeze_log_overflowed = saved_fovf;
    memcpy(ctx->drop_log, saved_dlog_arr, sizeof saved_dlog_arr);
    memcpy(ctx->freeze_log, saved_flog_arr, sizeof saved_flog_arr);

    /* M23: trait bounds — конкретният тип трябва да имплементира bound-а */
    for (int i = 0; i < np; i++) {
        if (!fn->param_bounds || !fn->param_bounds[i]) continue;
        const char *bound = fn->param_bounds[i];
        Type *bt = fn->inst_types[idx * np + i];
        const char *tname = type_impl_name(bt);
        const char *st = tname;
        if (st && strchr(st, '.')) st = strrchr(st, '.') + 1;
        int ok = 0;
        for (int ii = 0; ii < ctx->n_impls; ii++)
            if (strcmp(ctx->impls[ii].trait, bound) == 0 &&
                st && strcmp(ctx->impls[ii].type_name, st) == 0) { ok = 1; break; }
        if (!ok)
            check_error(ctx, pos,
                "типът %s не имплементира %s (bound на '%s')",
                type_str(bt), bound, fn->type_params[i]);
    }
    return idx;
}

/* M21: инстанциране на generic fn при извикване. Връща substituted ret
 * тип; пренаписва callee->name към синтетичното инстанционно име;
 * оставя ctx->g_names/g_types = избраната substitution (извикващият чисти). */
Type *generic_instantiate(CheckCtx *ctx, Node *fn, Node *call) {
    int np = fn->n_type_params;
    Type **bind = calloc((size_t)(np ? np : 1), sizeof(Type *));

    /* явни типови аргументи: f<i64, str>(…) */
    if (call->type_args.len > 0 && call->type_args.len != np) {
        check_error(ctx, call->pos,
            "'%s' очаква %d типови аргумента, получих %d",
            fn->fn_name, np, call->type_args.len);
    }
    for (int i = 0; i < call->type_args.len && i < np; i++)
        bind[i] = resolve_type_node(ctx, call->type_args.data[i]);

    /* извод от стойностните аргументи */
    for (int i = 0; i < call->args.len && i < fn->params.len; i++) {
        Type *at = call->args.data[i]->type;
        bind_type_params(ctx, fn->params.data[i]->param_type, at,
                         fn->type_params, np, bind, call->pos, fn->fn_name);
    }
    for (int i = 0; i < np; i++) {
        if (!bind[i]) {
            check_error(ctx, call->pos,
                "'%s': не мога да изведа типовия параметър '%s' — дай изричен: %s<%s>(…)",
                fn->fn_name, fn->type_params[i], fn->fn_name, fn->type_params[i]);
            bind[i] = type_new(TYPE_ERROR);
        }
    }

    int idx = generic_inst_add(ctx, fn, bind, call->pos);
    /* bind[] е консумиран от ядрото (при нова инстанция указателите отиват
     * в inst_types; при съществуваща се освобождава) */

    /* синтетичното име — codegen емитва варианта с него */
    size_t need = strlen(fn->fn_name) + 16;
    char *nn = malloc(need);
    if (!nn) { fprintf(stderr, "baga: out of memory\n"); exit(1); }
    snprintf(nn, need, "%s__i%d", fn->fn_name, idx);
    /* старото име НЕ се free-ва — извикващият път може още да го държи
     * за съобщения за грешка (leak-safe: node_free пипа само новото) */
    call->callee->name = nn;

    /* substitution активна за извикващия (params/ret резолване) */
    ctx->g_names.len = 0; ctx->g_types.len = 0;
    for (int i = 0; i < np; i++) {
        vec_push(ctx->g_names, fn->type_params[i]);
        vec_push(ctx->g_types, fn->inst_types[idx * np + i]);
    }
    return fn->ret_type ? resolve_type_node(ctx, fn->ret_type)
                        : type_new(TYPE_VOID);
}

/* M25: `let f: fn(i64) -> i64 = id` — generic fn като стойност с явна fn
 * анотация. Унифицира анотацията със сигнатурата на generic-а по позиции
 * (params + ret — същият bind_type_params като при извикване), инстанцира
 * през M21 ядрото (generic_inst_add), маркира инстанцията като fn стойност
 * (codegen emit-ва __clo wrapper за нея) и пренасочва IDENT-а към нея
 * (име + TYPE_FN маркер със synth име — codegen познава wrapper-а по него).
 * Връща 1, когато id е generic fn (обработен или диагностициран); 0 — не е,
 * и нормалният infer път продължава. Без анотация остава сегашната грешка
 * (NODE_IDENT value пътят). */
int generic_fn_value_bind(CheckCtx *ctx, Node *id, Type *annot) {
    /* намери decl-а като при L5 резолюцията в NODE_IDENT (собственият
     * модул печели при дубликати от няколко модула) */
    int first = -1, ownidx = -1;
    const char *own = mod_base(id->pos.file);
    for (int i = 0; i < ctx->n_fns; i++) {
        if (strcmp(ctx->fns[i].name, id->name) != 0) continue;
        if (first < 0) first = i;
        if (strcmp(ctx->fns[i].origin, own) == 0) ownidx = i;
    }
    if (first < 0) return 0;
    FnRec *rec = &ctx->fns[ownidx >= 0 ? ownidx : first];
    Node *fn = rec->decl;
    if (!fn || fn->n_type_params == 0) return 0;   /* обикновена fn — нормален път */
    int np = fn->n_type_params;
    if (annot->nparams != fn->params.len) {
        check_error(ctx, id->pos,
            "generic fn '%s' не съвпада с анотацията %s по брой параметри (%d != %d)",
            id->name, type_str(annot), annot->nparams, fn->params.len);
        return 1;
    }
    Type **bind = calloc((size_t)(np ? np : 1), sizeof(Type *));
    if (!bind) { fprintf(stderr, "baga: out of memory\n"); exit(1); }
    for (int i = 0; i < fn->params.len; i++)
        bind_type_params(ctx, fn->params.data[i]->param_type, annot->params[i],
                         fn->type_params, np, bind, id->pos, fn->fn_name);
    if (fn->ret_type && annot->ret)
        bind_type_params(ctx, fn->ret_type, annot->ret,
                         fn->type_params, np, bind, id->pos, fn->fn_name);
    for (int i = 0; i < np; i++) {
        if (!bind[i]) {
            check_error(ctx, id->pos,
                "'%s': не мога да изведа типовия параметър '%s' от анотацията %s",
                fn->fn_name, fn->type_params[i], type_str(annot));
            bind[i] = type_new(TYPE_ERROR);
        }
    }
    int idx = generic_inst_add(ctx, fn, bind, id->pos);
    fn->inst_as_value[idx] = 1;

    /* пренасочване към инстанцията: synth име + TYPE_FN маркер */
    size_t need = strlen(fn->fn_name) + 16;
    char *nn = malloc(need);
    if (!nn) { fprintf(stderr, "baga: out of memory\n"); exit(1); }
    snprintf(nn, need, "%s__i%d", fn->fn_name, idx);
    id->name = nn;   /* leak-safe: старото име е AST/грешки (като в generic_instantiate) */
    Type *vt2 = type_fn(annot->ret, annot->params, annot->nparams);
    /* ефектите идват от декларацията на generic-а — анотацията е договорът,
     * срещу който се проверява fn_effects_subset при binding-а */
    Type *gt = rec->fn_type;
    if (gt && gt->ret && gt->ret->n_effects > 0 && vt2->ret) {
        Type *vr = type_new(vt2->ret->kind);
        vr->elem = vt2->ret->elem;
        vr->name = vt2->ret->name;
        vr->effects = gt->ret->effects;
        vr->n_effects = gt->ret->n_effects;
        vt2->ret = vr;
    }
    vt2->name = nn;
    id->type = vt2;
    return 1;
}

static void mem3_collect_rets(CheckCtx *ctx, Node *n,
                              int *rids, int *nr, int *hids, int *nh, int cap,
                              int *tids, int *nt) {
    if (!n || (*nr >= cap && *nh >= cap && *nt >= cap)) return;
    switch (n->kind) {
        case NODE_RETURN:
            if (n->ret_val) {
                /* W3.6: връща ли ЗАМЪРСЕН контейнер (не payload)? */
                int tr = mem3_container_region(ctx, n->ret_val, 0);
                if (tr > 0 && *nt < cap) tids[(*nt)++] = tr;
                int r = region_of_expr(ctx, n->ret_val);
                if (r > 0 && *nr < cap) rids[(*nr)++] = r;
                int h = lookup_arena_id(ctx, n->ret_val);
                if (h <= 0 && n->ret_val->kind == NODE_IDENT) {
                    EnvEntry *e = ck_env_find(ctx, n->ret_val->name);
                    if (e && e->arena_id > 0) h = e->arena_id;
                }
                if (h > 0 && *nh < cap) hids[(*nh)++] = h;
            }
            return;
        case NODE_BLOCK:
            for (int i = 0; i < n->stmts.len; i++)
                mem3_collect_rets(ctx, n->stmts.data[i], rids, nr, hids, nh, cap, tids, nt);
            return;
        case NODE_IF:
            mem3_collect_rets(ctx, n->then_br, rids, nr, hids, nh, cap, tids, nt);
            mem3_collect_rets(ctx, n->else_br, rids, nr, hids, nh, cap, tids, nt);
            return;
        case NODE_WHILE:
            mem3_collect_rets(ctx, n->while_body, rids, nr, hids, nh, cap, tids, nt);
            return;
        case NODE_FOR:
            mem3_collect_rets(ctx, n->for_body, rids, nr, hids, nh, cap, tids, nt);
            return;
        case NODE_MATCH:
            for (int i = 0; i < n->match_arms.len; i++)
                mem3_collect_rets(ctx, n->match_arms.data[i]->arm_body, rids, nr, hids, nh, cap, tids, nt);
            return;
        case NODE_CATCH:
            mem3_collect_rets(ctx, n->catch_expr, rids, nr, hids, nh, cap, tids, nt);
            mem3_collect_rets(ctx, n->catch_handler, rids, nr, hids, nh, cap, tids, nt);
            return;
        case NODE_EXPR_STMT:
            mem3_collect_rets(ctx, n->expr, rids, nr, hids, nh, cap, tids, nt);
            return;
        case NODE_LET:
            mem3_collect_rets(ctx, n->let_init, rids, nr, hids, nh, cap, tids, nt);
            return;
        default:
            return;
    }
}

static int mem3_ids_to_param(CheckCtx *ctx, Node *fn, int *ids, int nids) {
    int param_idx = -1;
    for (int k = 0; k < nids; k++) {
        int pi = -1;
        for (int i = 0; i < fn->params.len; i++) {
            EnvEntry *pe = ck_env_find(ctx, fn->params.data[i]->param_name);
            if (pe && pe->arena_id == ids[k] && pe->arena_id > 0)
                { pi = i; break; }
        }
        if (pi < 0) continue;
        if (param_idx < 0) param_idx = pi;
        else if (param_idx != pi) return -1;
    }
    return param_idx;
}

/* W3.6: индекс на param по име (-1 = не е param). */
static int w36_param_index(Node *fn, const char *name) {
    if (!fn || !name) return -1;
    for (int i = 0; i < fn->params.len; i++)
        if (strcmp(fn->params.data[i]->param_name, name) == 0) return i;
    return -1;
}

static void mem3_summarize_fn(CheckCtx *ctx, Node *fn, FnRec *rec) {
    if (!rec || !fn || !fn->fn_body) return;
    int rids[32], hids[32], tids[32];
    int nr = 0, nh = 0, nt = 0;
    mem3_collect_rets(ctx, fn->fn_body, rids, &nr, hids, &nh, 32, tids, &nt);
    if (fn->fn_body->stmts.len > 0) {
        Node *last = fn->fn_body->stmts.data[fn->fn_body->stmts.len - 1];
        if (last->kind == NODE_EXPR_STMT && last->expr) {
            int tr = mem3_container_region(ctx, last->expr, 0);
            if (tr > 0 && nt < 32) tids[nt++] = tr;   /* W3.6 */
            int r = region_of_expr(ctx, last->expr);
            if (r > 0 && nr < 32) rids[nr++] = r;
            int h = lookup_arena_id(ctx, last->expr);
            if (h <= 0 && last->expr->kind == NODE_IDENT) {
                EnvEntry *e = ck_env_find(ctx, last->expr->name);
                if (e && e->arena_id > 0) h = e->arena_id;
            }
            if (h > 0 && nh < 32) hids[nh++] = h;
        }
    }
    rec->ret_arena_param = mem3_ids_to_param(ctx, fn, rids, nr);
    rec->ret_handle_param = mem3_ids_to_param(ctx, fn, hids, nh);
    /* W3.6: върнат замърсен контейнер (само един param-източник, както
     * ret_arena_param; повече = -1 и не пренасяме). */
    rec->ret_taint_param = mem3_ids_to_param(ctx, fn, tids, nt);
    /* W3.6: тялото замърсява param-контейнер — имената, записани по време на
     * тялото, стават индекси чак тук, докато param-ите са в scope. */
    rec->out_taint_param = -1;
    rec->out_taint_from = -1;
    rec->out_taint_dim = 0;
    if (!ctx->w36_ambiguous && ctx->w36_out_name) {
        int out = w36_param_index(fn, ctx->w36_out_name);
        int from = -1;
        if (ctx->w36_from_name) from = w36_param_index(fn, ctx->w36_from_name);
        if (from < 0 && ctx->w36_from_rid > 0) {
            int ids[1] = { ctx->w36_from_rid };
            from = mem3_ids_to_param(ctx, fn, ids, 1);
        }
        if (out >= 0 && from >= 0) {
            rec->out_taint_param = out;
            rec->out_taint_from = from;
            rec->out_taint_dim = ctx->w36_from_dim;
        }
    }
}

void mem3_ensure_fn(CheckCtx *ctx, Node *fn) {
    if (!fn || fn->is_extern || fn->n_type_params > 0) return;
    FnRec *r = fn_rec_of(ctx, fn);
    if (!r || r->checked || r->checking) return;
    check_fn(ctx, fn);
}

void check_fn(CheckCtx *ctx, Node *fn) {
    FnRec *rec = fn_rec_of(ctx, fn);
    if (rec && rec->checked) return;
    if (rec && rec->checking) return;
    if (rec) rec->checking = 1;

    const char *saved_fn = ctx->cur_fn;
    Type *saved_ret = ctx->cur_ret;
    Type *saved_eff = ctx->cur_effects;
    int saved_loop = ctx->loop_depth;
    int saved_dlog = ctx->n_drop_log;
    int saved_ovf = ctx->drop_log_overflowed;
    int saved_flog = ctx->n_freeze_log;
    int saved_fovf = ctx->freeze_log_overflowed;
    int saved_nfreed = ctx->n_arena_freed;
    /* W1.6: лог-МАСИВИТЕ също се snapshot-ват — check_fn нулира броячите и
     * презаписва записите, а може да се извика посред infer на извикващия
     * (lazy проверка / generic инстанция); без това join-ът на обграждащата
     * функция чете чужди EnvEntry указатели и измисля dropped/frozen. */
    EnvEntry *saved_dlog_arr[256];
    EnvEntry *saved_flog_arr[256];
    memcpy(saved_dlog_arr, ctx->drop_log, sizeof saved_dlog_arr);
    memcpy(saved_flog_arr, ctx->freeze_log, sizeof saved_flog_arr);
    const char *saved_w36_out = ctx->w36_out_name;
    const char *saved_w36_from = ctx->w36_from_name;
    int saved_w36_rid = ctx->w36_from_rid;
    int saved_w36_dim = ctx->w36_from_dim;
    int saved_w36_amb = ctx->w36_ambiguous;
    ctx->w36_out_name = NULL; ctx->w36_from_name = NULL;
    ctx->w36_from_rid = 0; ctx->w36_from_dim = 0; ctx->w36_ambiguous = 0;

    ctx->cur_fn = fn->fn_name;
    ctx->cur_ret = fn->ret_type ? resolve_type_node(ctx, fn->ret_type) : type_new(TYPE_VOID);
    ctx->cur_effects = type_new(TYPE_VOID); /* accumulator for body effects */

    push_scope(ctx);

    /* define params */
    for (int i = 0; i < fn->params.len; i++) {
        Node *p = fn->params.data[i];
        Type *pt = resolve_type_node(ctx, p->param_type);
        p->type = pt;
        if (pt->kind == TYPE_FN && ck_find_fn(ctx, p->param_name)) {
            check_error(ctx, p->pos,
                "fn параметър '%s' засенчи глобална функция — преименувай го",
                p->param_name);
        }
        env_define(ctx, p->param_name, pt, p->pos);
        /* MEM-2: параметрите споделят буфера на извикващия — drop забранен */
        EnvEntry *pe = ck_env_find(ctx, p->param_name);
        if (pe) {
            pe->is_param = 1;
            /* MEM-3: i64 param може да е arena handle за alloc/return
             * резюмета — но НЕ е is_arena (иначе `let x = n` гърми като
             * забравена арена, liveness_struct.baga). */
            if (pt && pt->kind == TYPE_I64) {
                pe->arena_id = ++ctx->next_arena_id;
                /* W1.6: i64 параметър е потенциален world handle — така
                 * seatbelt-ът за double-free / mutate-after-freeze важи и
                 * вътре в callee-то (world_free е двоен free на malloc).
                 * Началното състояние е неизвестно → world_frozen = 0,
                 * т.е. никакво измислено обвинение към извикващия. */
                pe->is_world = 1;
            }
        }
    }

    /* check body — MEM-2: drop-логът е per-функция (drop-ове не пресичат
     * fn граница); без reset block-local drop-ове пълнят лога за целия модул */
    ctx->n_drop_log = 0;
    ctx->drop_log_overflowed = 0;
    ctx->n_freeze_log = 0;
    ctx->freeze_log_overflowed = 0;
    ctx->n_arena_freed = 0;
    if (fn->fn_body) {
        for (int i = 0; i < fn->fn_body->stmts.len; i++)
            infer(ctx, fn->fn_body->stmts.data[i]);
    }

    /* M19: не-void функция не бива да пада от края — връща боклук в C */
    if (fn->fn_body && ctx->cur_ret && ctx->cur_ret->kind != TYPE_VOID &&
        fn_body_falls_through(fn->fn_body)) {
        check_error(ctx, fn->pos,
            "функцията '%s' може да падне от края без return — не-void функциите трябва да връщат стойност на всеки път",
            fn->fn_name);
    }

    /* M19b: типът на implicit return (последният изразен statement)
     * трябва да съвпада с връщания тип — иначе кодгенът връща боклук */
    if (fn->fn_body && fn->fn_body->stmts.len > 0 &&
        ctx->cur_ret && ctx->cur_ret->kind != TYPE_VOID) {
        Node *last = fn->fn_body->stmts.data[fn->fn_body->stmts.len - 1];
        if (last->kind == NODE_EXPR_STMT && last->expr &&
            last->expr->kind != NODE_RAISE &&
            last->expr->type &&
            !type_eq(last->expr->type, ctx->cur_ret)) {
            check_error(ctx, last->expr->pos,
                "implicit return връща %s, но функцията '%s' очаква %s",
                type_str(last->expr->type), fn->fn_name, type_str(ctx->cur_ret));
        }
    }

    /* effect checking: unhandled effects in body vs declared effects */
    if (ctx->cur_effects) {
        for (int i = 0; i < ctx->cur_effects->n_effects; i++) {
            const char *eff = ctx->cur_effects->effects[i];
            if (!type_has_effect(ctx->cur_ret, eff)) {
                check_error(ctx, fn->pos,
                    "необработен ефект !%s във '%s' — декларирай го в return типа или го хвани с catch",
                    eff, fn->fn_name);
            } else {
                /* M20: payload сигнатурата трябва да съвпада с декларацията */
                Type *bp = ctx->cur_effects->effect_payloads
                    ? ctx->cur_effects->effect_payloads[i] : NULL;
                Type *dp = type_effect_payload(ctx->cur_ret, eff);
                if (bp && !dp) {
                    check_error(ctx, fn->pos,
                        "ефектът !%s в тялото на '%s' носи payload %s — декларирай !%s(%s)",
                        eff, fn->fn_name, type_str(bp), eff, type_str(bp));
                } else if (bp && dp && !type_eq(bp, dp)) {
                    check_error(ctx, fn->pos,
                        "payload на !%s в тялото на '%s' е %s, но декларацията е %s",
                        eff, fn->fn_name, type_str(bp), type_str(dp));
                }
            }
        }
    }

    /* MEM-3: резюме „връща region на param k“ — преди pop, докато
     * param EnvEntry-тата още са в scope. */
    mem3_summarize_fn(ctx, fn, rec);

    /* MEM-3: ident, върнат от последния return/implicit return, не тече
     * (собствеността излиза към caller-а). */
    {
        const char *skip = NULL;
        if (fn->fn_body && fn->fn_body->stmts.len > 0) {
            Node *last = fn->fn_body->stmts.data[fn->fn_body->stmts.len - 1];
            Node *ve = NULL;
            if (last->kind == NODE_RETURN && last->ret_val) ve = last->ret_val;
            else if (last->kind == NODE_EXPR_STMT) ve = last->expr;
            if (ve && ve->kind == NODE_IDENT) skip = ve->name;
        }
        pop_scope_skip(ctx, skip);
    }
    if (rec) {
        rec->checking = 0;
        rec->checked = 1;
    }
    ctx->cur_fn = saved_fn;
    ctx->cur_ret = saved_ret;
    ctx->cur_effects = saved_eff;
    ctx->loop_depth = saved_loop;
    ctx->n_drop_log = saved_dlog;
    ctx->drop_log_overflowed = saved_ovf;
    ctx->n_freeze_log = saved_flog;
    ctx->freeze_log_overflowed = saved_fovf;
    memcpy(ctx->drop_log, saved_dlog_arr, sizeof saved_dlog_arr);
    memcpy(ctx->freeze_log, saved_flog_arr, sizeof saved_flog_arr);
    ctx->n_arena_freed = saved_nfreed;
    ctx->w36_out_name = saved_w36_out;
    ctx->w36_from_name = saved_w36_from;
    ctx->w36_from_rid = saved_w36_rid;
    ctx->w36_from_dim = saved_w36_dim;
    ctx->w36_ambiguous = saved_w36_amb;
}

/* ============================================================
 *  Public API
 * ============================================================ */

void check_program(Checker *c, Node *program) {
    CheckCtx ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.chk = c;
    ctx.main_base = mod_base(program->pos.file);
    ctx.program = program;

    /* L6 struct pre-pass: еднакви struct имена от РАЗЛИЧНИ модули се
     * преименуват на "модул.Type" ПРЕДИ pass 1 (fn сигнатурите resolve-ват
     * struct имена още при регистрация). Дубликат в ЕДИН модул е грешка —
     * досега и двата случая излизаха чак в gcc с "conflicting types". */
    for (int i = 0; i < program->items.len; i++) {
        Node *a = program->items.data[i];
        if (a->kind != NODE_STRUCT) continue;
        for (int j = i + 1; j < program->items.len; j++) {
            Node *b = program->items.data[j];
            if (b->kind != NODE_STRUCT) continue;
            if (strcmp(type_short_name(a->struct_name),
                       type_short_name(b->struct_name)) != 0) continue;
            char *oa = mod_base(a->pos.file);
            char *ob = mod_base(b->pos.file);
            if (strcmp(oa, ob) == 0) {
                check_error(&ctx, b->pos,
                    "повторна дефиниция на struct '%s' в модул '%s'",
                    type_short_name(b->struct_name), ob);
            } else {
                for (int k = 0; k < 2; k++) {
                    Node *d = k == 0 ? a : b;
                    const char *org = k == 0 ? oa : ob;
                    if (strchr(d->struct_name, '.')) continue;  /* вече преименуван */
                    size_t need = strlen(org) + 1 + strlen(d->struct_name) + 1;
                    char *nn = malloc(need);
                    if (!nn) { fprintf(stderr, "baga: out of memory\n"); exit(1); }
                    snprintf(nn, need, "%s.%s", org, d->struct_name);
                    d->struct_name = nn;
                }
            }
            free(oa);
            free(ob);
        }
    }

    push_scope(&ctx);

    /* L3 pre-pass: регистрираме enum декларациите (име + тип) ПРЕДИ pass 1,
     * защото сигнатурите и payload-ите се resolve-ват в ред и могат да
     * реферират по-късно деклариран sum enum (fn f(r: Res) преди enum Res).
     * Pass 1 после само регистрира вариантите — без повторна регистрация. */
    for (int i = 0; i < program->items.len; i++) {
        Node *item = program->items.data[i];
        if (item->kind != NODE_ENUM) continue;
        /* L6: enum имената остават глобални — дубликат между модули е ясна
         * грешка тук (досега излизаше в gcc при несвързани варианти) */
        for (int k = 0; k < ctx.n_enums; k++) {
            if (strcmp(ctx.enums[k].name, item->enum_name) == 0) {
                check_error(&ctx, item->pos,
                    "повторна дефиниция на enum '%s' (enum имената са глобални — модулно уточняване не се поддържа)",
                    item->enum_name);
                break;
            }
        }
        int is_sum = 0;
        for (int j = 0; j < item->n_variants; j++)
            if (item->enum_payloads && item->enum_payloads[j]) is_sum = 1;
        if (ctx.n_enums < FNS_MAX) {
            ctx.enums[ctx.n_enums].name = item->enum_name;
            ctx.enums[ctx.n_enums].decl = item;
            ctx.n_enums++;
        } else {
            check_error(&ctx, item->pos,
                "твърде много enum декларации (лимит %d) — раздели програмата",
                FNS_MAX);
        }
        Type *et = type_new(is_sum ? TYPE_ENUM : TYPE_I64);
        et->name = strdup(item->enum_name);
        item->type = et;
    }

    /* M23 pre-pass: регистрирай traits + impls; impl методите влизат в
     * fn регистъра с вътрешни имена "Trait.Type.method" */
    for (int i = 0; i < program->items.len; i++) {
        Node *item = program->items.data[i];
        if (item->kind == NODE_TRAIT) {
            if (ctx.n_traits < FNS_MAX) {
                ctx.traits[ctx.n_traits].name = item->trait_name;
                ctx.traits[ctx.n_traits].decl = item;
                ctx.n_traits++;
            }
        } else if (item->kind == NODE_IMPL) {
            const char *tn = NULL;
            Node *it = item->impl_type;
            while (it && it->kind == NODE_TYPE_EFFECT) it = it->inner_type;
            if (it && it->kind == NODE_TYPE) tn = it->type_name;
            /* типовото име за вътрешния символ (struct → късото име) */
            if (tn && strchr(tn, '.')) tn = strrchr(tn, '.') + 1;
            if (ctx.n_impls < FNS_MAX && tn) {
                ctx.impls[ctx.n_impls].trait = item->impl_trait;
                ctx.impls[ctx.n_impls].type_name = strdup(tn);
                ctx.impls[ctx.n_impls].decl = item;
                ctx.n_impls++;
            }
            for (int m = 0; m < item->impl_methods.len; m++) {
                Node *mf = item->impl_methods.data[m];
                size_t need = strlen(item->impl_trait) + 2 +
                              (tn ? strlen(tn) : 4) +
                              strlen(mf->fn_name) + 4;
                char *nn = malloc(need);
                if (!nn) { fprintf(stderr, "baga: out of memory\n"); exit(1); }
                snprintf(nn, need, "%s.%s.%s", item->impl_trait,
                         tn ? tn : "?", mf->fn_name);
                /* пази старото име за диагностика; codegen ползва новото */
                mf->fn_trait = item->impl_trait;
                mf->fn_name = nn;
            }
        }
    }

    /* pass 1: register all top-level names */
    for (int i = 0; i < program->items.len; i++) {
        Node *item = program->items.data[i];

        if (item->kind == NODE_FN) {
            register_fn(&ctx, item);

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
            } else {
                check_error(&ctx, item->pos,
                    "твърде много struct декларации (лимит %d) — struct '%s' е отрязан",
                    FNS_MAX, item->struct_name);
            }
            Type *st = type_new(TYPE_STRUCT);
            st->name = strdup(item->struct_name);
            item->type = st;
            /* L6: resolve-ваме полетата тут (и пренаписваме AST имената към
             * финалните "модул.Type"), защото codegen емитва typedef-а от
             * AST текстa — иначе нереферирано поле би останало с късо име */
            for (int fi = 0; fi < item->fields.len; fi++)
                resolve_type_node(&ctx, item->fields.data[fi]->fld_type);

        } else if (item->kind == NODE_ENUM) {
            /* името и типът са регистрирани в L3 pre-pass-а (преди pass 1) —
             * тук само регистрираме вариантите; payload-ите вече resolve-ват
             * и по-късно декларирани sum enum-и */
            for (int j = 0; j < item->n_variants; j++) {
                if (ctx.n_variants < FNS_MAX * 4) {
                    ctx.variants[ctx.n_variants].variant = item->enum_variants[j];
                    ctx.variants[ctx.n_variants].enum_name = item->enum_name;
                    ctx.variants[ctx.n_variants].value = j;
                    ctx.variants[ctx.n_variants].payload =
                        (item->enum_payloads && item->enum_payloads[j])
                            ? resolve_type_node(&ctx, item->enum_payloads[j]) : NULL;
                    ctx.n_variants++;
                } else {
                    check_error(&ctx, item->pos,
                        "твърде много enum варианти (лимит %d) — '%s.%s' е отрязан",
                        FNS_MAX * 4, item->enum_name, item->enum_variants[j]);
                }
            }
        }
    }

    /* M23: impl методите влизат в fn регистъра (вътрешните имена вече са
     * сложени от pre-pass-а) */
    for (int i = 0; i < program->items.len; i++) {
        Node *item = program->items.data[i];
        if (item->kind != NODE_IMPL) continue;
        for (int m = 0; m < item->impl_methods.len; m++) {
            Node *mf = item->impl_methods.data[m];
            register_fn(&ctx, mf);
        }
    }

    /* L6 namespaces: еднакви имена от РАЗЛИЧНИ модули са легални — decl-ът
     * получава уникално вътрешно име "модул.функция" ('.' не се лексва в
     * уникален C символ. Дубликат в ЕДИН модул е грешка (досега я хващаше
     * gcc с redefinition). */
    for (int i = 0; i < ctx.n_fns; i++) {
        for (int j = i + 1; j < ctx.n_fns; j++) {
            if (strcmp(ctx.fns[i].name, ctx.fns[j].name) != 0) continue;
            /* forward декларация + реализация (или две декларации) — ОК;
             * дубликатът е само между две тела */
            if (!ctx.fns[i].decl->fn_body || !ctx.fns[j].decl->fn_body) continue;
            if (strcmp(ctx.fns[i].origin, ctx.fns[j].origin) == 0) {
                check_error(&ctx, ctx.fns[j].decl->pos,
                    "повторна дефиниция на функция '%s' в модул '%s'",
                    ctx.fns[j].name, ctx.fns[j].origin);
                continue;
            }
            for (int k = 0; k < 2; k++) {
                Node *d = k == 0 ? ctx.fns[i].decl : ctx.fns[j].decl;
                const char *org = k == 0 ? ctx.fns[i].origin : ctx.fns[j].origin;
                if (d->is_extern) continue;        /* FFI името е договор */
                if (strchr(d->fn_name, '.')) continue;  /* вече преименувана */
                size_t need = strlen(org) + 1 + strlen(d->fn_name) + 1;
                char *nn = malloc(need);
                if (!nn) { fprintf(stderr, "baga: out of memory\n"); exit(1); }
                snprintf(nn, need, "%s.%s", org, d->fn_name);
                d->fn_name = nn;
            }
        }
    }

    /* A1: sum variants may share names across enums (use Enum::Variant).
     * Still forbid the same name twice *inside one enum*, and warn-as-error
     * only when a bare name would always be ambiguous is handled at use site.
     * Function vs variant: allowed; bare call prefers unique variant, else fn. */
    for (int i = 0; i < ctx.n_variants; i++) {
        Node *ed = NULL;
        for (int ei = 0; ei < ctx.n_enums; ei++)
            if (strcmp(ctx.enums[ei].name, ctx.variants[i].enum_name) == 0)
                { ed = ctx.enums[ei].decl; break; }
        if (!ed) continue;
        for (int j = i + 1; j < ctx.n_variants; j++) {
            if (strcmp(ctx.variants[i].enum_name, ctx.variants[j].enum_name) == 0 &&
                strcmp(ctx.variants[i].variant, ctx.variants[j].variant) == 0)
                check_error(&ctx, ed->pos,
                    "повторена дефиниция на вариант '%s' в enum '%s'",
                    ctx.variants[i].variant, ctx.variants[i].enum_name);
        }
    }

    /* pass 2: verify specs against functions */
    for (int i = 0; i < program->items.len; i++) {
        Node *item = program->items.data[i];
        if (item->kind != NODE_SPEC) continue;

        /* find the function this spec describes */
        FnRec *srec = find_fn_rec(&ctx, item->spec_name);
        Type *ft = srec ? srec->fn_type : NULL;
        if (!ft) {
            check_error(&ctx, item->pos,
                "spec '%s' описва функция, която не съществува", item->spec_name);
            continue;
        }

        /* M27: spec върху generic — T в spec-а е типовият параметър на fn */
        Node *sfn = srec->decl;
        const char **saved_tps = ctx.cur_type_params;
        int saved_ntp = ctx.cur_n_type_params;
        if (sfn && sfn->n_type_params > 0) {
            ctx.cur_type_params = (const char **)sfn->type_params;
            ctx.cur_n_type_params = sfn->n_type_params;
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
        Type *fn_ret = ft->ret ? ft->ret : type_new(TYPE_VOID);
        if (item->spec_output) {
            Type *spec_ret = resolve_type_node(&ctx, item->spec_output);
            if (!type_eq(spec_ret, fn_ret)) {
                check_error(&ctx, item->pos,
                    "spec '%s': output е %s, но функцията връща %s",
                    item->spec_name, type_str(spec_ret), type_str(fn_ret));
            }
        }

        infer_spec_contracts(&ctx, item, fn_ret);
        ctx.cur_type_params = saved_tps;
        ctx.cur_n_type_params = saved_ntp;
    }

    /* pass 3: check function bodies — M21: generic fns се проверяват
     * мързеливо при инстанциране (всяко извикване с конкретни типове) */
    for (int i = 0; i < program->items.len; i++) {
        Node *item = program->items.data[i];
        if (item->kind == NODE_FN && item->n_type_params == 0)
            check_fn(&ctx, item);
    }
    /* M23: impl методните тела също се проверяват */
    for (int i = 0; i < program->items.len; i++) {
        Node *item = program->items.data[i];
        if (item->kind != NODE_IMPL) continue;
        for (int m = 0; m < item->impl_methods.len; m++)
            check_fn(&ctx, item->impl_methods.data[m]);
    }

    /* check that main exists (skipped for --check / library mode) */
    if (!c->allow_no_main && !ck_find_fn(&ctx, "main")) {
        SrcPos pos = { 1, 1, NULL };
        check_error(&ctx, pos, "липсва функция 'main'");
    }

    pop_scope(&ctx);

    /* M21: snapshot на регистрите — checker_recheck_inst (codegen при
     * мономорфизация) възстановява ctx от него */
    {
        CheckCtxSnap *s = calloc(1, sizeof(CheckCtxSnap));
        if (!s) { fprintf(stderr, "baga: out of memory\n"); exit(1); }
        s->program = program;
        s->main_base = ctx.main_base;
        s->n_fns = ctx.n_fns;
        memcpy(s->fns, ctx.fns, sizeof(ctx.fns));
        s->n_structs = ctx.n_structs;
        memcpy(s->structs, ctx.structs, sizeof(ctx.structs));
        s->n_enums = ctx.n_enums;
        memcpy(s->enums, ctx.enums, sizeof(ctx.enums));
        s->n_variants = ctx.n_variants;
        memcpy(s->variants, ctx.variants, sizeof(ctx.variants));
        s->n_traits = ctx.n_traits;
        memcpy(s->traits, ctx.traits, sizeof(ctx.traits));
        s->n_impls = ctx.n_impls;
        memcpy(s->impls, ctx.impls, sizeof(ctx.impls));
        s->n_lambdas = ctx.n_lambdas;
        c->gen_snap = s;
    }
}

/* M21: re-infer тялото на generic fn под инстанция k (преди codegen emit) */
void checker_recheck_inst(Checker *chk, Node *fn, int k) {
    CheckCtxSnap *s = chk->gen_snap;
    if (!s) return;
    CheckCtx ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.chk = chk;
    ctx.program = s->program;
    ctx.main_base = s->main_base;
    ctx.n_fns = s->n_fns;
    memcpy(ctx.fns, s->fns, sizeof(ctx.fns));
    ctx.n_structs = s->n_structs;
    memcpy(ctx.structs, s->structs, sizeof(ctx.structs));
    ctx.n_enums = s->n_enums;
    memcpy(ctx.enums, s->enums, sizeof(ctx.enums));
    ctx.n_variants = s->n_variants;
    memcpy(ctx.variants, s->variants, sizeof(ctx.variants));
    ctx.n_traits = s->n_traits;
    memcpy(ctx.traits, s->traits, sizeof(ctx.traits));
    ctx.n_impls = s->n_impls;
    memcpy(ctx.impls, s->impls, sizeof(ctx.impls));
    ctx.n_lambdas = s->n_lambdas;
    int np = fn->n_type_params;
    for (int i = 0; i < np; i++) {
        vec_push(ctx.g_names, fn->type_params[i]);
        vec_push(ctx.g_types, fn->inst_types[k * np + i]);
    }
    /* MEM-3: checked/checking флагът е за основния пас — тук тялото е
     * нулирано от restore_method_calls и ТРЯБВА да се преинферира под
     * инстанцията (иначе кодгенът получава празни типове). Нулираме
     * локалното копие на FnRec — глобалният статус не се пипа. */
    FnRec *rrec = fn_rec_of(&ctx, fn);
    if (rrec) { rrec->checked = 0; rrec->checking = 0; }
    restore_method_calls(fn->fn_body);
    check_fn(&ctx, fn);
}

/* M27: re-infer spec ensures/requires под инстанция k (преди emit на
 * spec wrapper — `output == x` върху str трябва strcmp, не C `==`). */
void checker_recheck_spec(Checker *chk, Node *spec, Node *fn, int k) {
    CheckCtxSnap *s = chk->gen_snap;
    if (!s || !spec || !fn) return;
    CheckCtx ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.chk = chk;
    ctx.program = s->program;
    ctx.main_base = s->main_base;
    ctx.n_fns = s->n_fns;
    memcpy(ctx.fns, s->fns, sizeof(ctx.fns));
    ctx.n_structs = s->n_structs;
    memcpy(ctx.structs, s->structs, sizeof(ctx.structs));
    ctx.n_enums = s->n_enums;
    memcpy(ctx.enums, s->enums, sizeof(ctx.enums));
    ctx.n_variants = s->n_variants;
    memcpy(ctx.variants, s->variants, sizeof(ctx.variants));
    ctx.n_traits = s->n_traits;
    memcpy(ctx.traits, s->traits, sizeof(ctx.traits));
    ctx.n_impls = s->n_impls;
    memcpy(ctx.impls, s->impls, sizeof(ctx.impls));
    ctx.n_lambdas = s->n_lambdas;
    int np = fn->n_type_params;
    for (int i = 0; i < np; i++) {
        vec_push(ctx.g_names, fn->type_params[i]);
        vec_push(ctx.g_types, fn->inst_types[k * np + i]);
    }
    Type *fn_ret = fn->ret_type ? resolve_type_node(&ctx, fn->ret_type)
                                : type_new(TYPE_VOID);
    infer_spec_contracts(&ctx, spec, fn_ret);
    vec_free(ctx.g_names);
    vec_free(ctx.g_types);
}

