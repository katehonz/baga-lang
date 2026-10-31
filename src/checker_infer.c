/* checker_infer.c — част от checker.c (виж ck_proto.h). */
#include "baga.h"
#include "checker_impl.h"

/* ============================================================
 *  Type inference
 * ============================================================ */

Type *infer(CheckCtx *ctx, Node *n);

static int is_numeric(Type *t) {
    /* TYPE_VAR: spec върху generic (M27) — аритметиката се проверява
     * отново per инстанция с конкретния тип; тук е оптимистично. */
    return t && (t->kind == TYPE_I32 || t->kind == TYPE_I64 ||
                 t->kind == TYPE_F64 || t->kind == TYPE_VAR);
}

static Type *numeric_promote(Type *a, Type *b) {
    if (a->kind == TYPE_VAR && b->kind == TYPE_VAR) {
        Type *v = type_new(TYPE_VAR);
        v->name = a->name ? strdup(a->name) : NULL;
        return v;
    }
    if (a->kind == TYPE_F64 || b->kind == TYPE_F64) return type_new(TYPE_F64);
    if (a->kind == TYPE_I64 || b->kind == TYPE_I64) return type_new(TYPE_I64);
    if (a->kind == TYPE_VAR || b->kind == TYPE_VAR) return type_new(TYPE_I64);
    return type_new(TYPE_I32);
}

Type *infer_binary(CheckCtx *ctx, Node *n) {
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
        case OP_LSHIFT: case OP_RSHIFT: case OP_URSHIFT:
            return type_new(TYPE_I64);
    }
    return type_new(TYPE_ERROR);
}

/* ============================================================
 *  L5: function values & closures
 * ============================================================ */

/* ефектите на fn стойността (върху ret) ⊆ ефектите на анотацията */
int fn_effects_subset(Type *val, Type *annot) {
    Type *ve = val && val->kind == TYPE_FN ? val->ret : NULL;
    Type *ae = annot && annot->kind == TYPE_FN ? annot->ret : NULL;
    if (!ve) return 1;
    for (int i = 0; i < ve->n_effects; i++)
        if (!ae || !type_has_effect(ae, ve->effects[i])) return 0;
    return 1;
}

/* извикване през fn стойност; аргументите вече са infer-нати */
static Type *call_fn_value(CheckCtx *ctx, Node *n, Type *ft) {
    if (n->args.len != ft->nparams) {
        check_error(ctx, n->pos, "fn стойност %s очаква %d аргумента, получих %d",
                    type_str(ft), ft->nparams, n->args.len);
    }
    int check_n = n->args.len < ft->nparams ? n->args.len : ft->nparams;
    for (int i = 0; i < check_n; i++) {
        Type *at = n->args.data[i]->type;
        Type *pt = ft->params[i];
        if (!type_assignable(at, pt)) {
            check_error(ctx, n->args.data[i]->pos,
                "fn стойност: аргумент #%d е от тип %s, но параметърът е %s",
                i + 1, type_str(at), type_str(pt));
        } else if (at && pt && at->kind == TYPE_FN && pt->kind == TYPE_FN &&
                   !fn_effects_subset(at, pt)) {
            check_error(ctx, n->args.data[i]->pos,
                "fn аргумент #%d има ефекти извън договора %s", i + 1, type_str(pt));
        }
    }
    /* codegen маркер: TYPE_FN без име = стойност (извикване през handle) */
    Type *marker = type_fn(ft->ret, ft->params, ft->nparams);
    n->callee->type = marker;
    Type *ret = ft->ret ? ft->ret : type_new(TYPE_VOID);
    Type *result = type_new(ret->kind);
    result->elem = ret->elem;
    result->name = ret->name;
    /* M24/M25: instantiated generic struct ret — targs-ите трябва да
     * оцелеят (иначе codegen emit-ва голото generic име — `b_Box`) */
    result->targs = ret->targs;
    result->n_targs = ret->n_targs;
    /* fn (L5): пази сигнатурата — върнатата closure е извикваема */
    result->ret = ret->ret;
    result->params = ret->params;
    result->nparams = ret->nparams;
    type_merge_effects(result, ret);
    if (ctx->cur_effects)
        type_merge_effects(ctx->cur_effects, ret);
    return result;
}

Type *infer_call(CheckCtx *ctx, Node *n) {
    /* infer arg types */
    for (int i = 0; i < n->args.len; i++)
        infer(ctx, n->args.data[i]);

    /* L6: модулно-уточнено извикване mod.f(...). Локална променлива с име
     * mod печели — тогава е обикновен field достъп и минава по-долу. */
    if (n->callee->kind == NODE_FIELD &&
        n->callee->field_obj->kind == NODE_IDENT &&
        !env_lookup(ctx, n->callee->field_obj->name)) {
        const char *mod = n->callee->field_obj->name;
        const char *fname = n->callee->field_name;
        int found = -1, mod_known = 0;
        for (int i = 0; i < ctx->n_fns; i++) {
            if (strcmp(ctx->fns[i].origin, mod) != 0) continue;
            mod_known = 1;
            if (strcmp(ctx->fns[i].name, fname) == 0) { found = i; break; }
        }
        if (found < 0) {
            if (mod_known)
                check_error(ctx, n->pos, "модулът '%s' няма функция '%s'", mod, fname);
            else
                check_error(ctx, n->pos, "непознат модул '%s' (няма импортиран %s.baga)", mod, mod);
            return type_new(TYPE_ERROR);
        }
        /* пренаписваме към уникалния вътрешен символ; стандартният път
         * по-долу прави проверките на аргументите. strdup — node_free
         * освобождава callee->name, не трябва да алиасва decl->fn_name */
        n->callee->kind = NODE_IDENT;
        n->callee->name = strdup(ctx->fns[found].decl->fn_name);
    }

    /* M23: методно извикване obj.m(args) — методът се резолва статично от
     * impl регистъра; callee-то става вътрешния символ "Trait.Type.method",
     * а obj се предава като първи аргумент (self). */
    if (n->callee->kind == NODE_FIELD &&
        !(n->callee->field_obj->kind == NODE_IDENT &&
          !env_lookup(ctx, n->callee->field_obj->name))) {
        Type *ot = infer(ctx, n->callee->field_obj);
        if (ot && ot->kind != TYPE_ERROR) {
            const char *tname = ot->name;
            if (ot->kind == TYPE_STRUCT || ot->kind == TYPE_ENUM) tname = ot->name;
            else if (ot->kind == TYPE_I64) tname = "i64";
            else if (ot->kind == TYPE_STR) tname = "str";
            else if (ot->kind == TYPE_BYTES) tname = "bytes";
            else if (ot->kind == TYPE_F64) tname = "f64";
            else if (ot->kind == TYPE_BOOL) tname = "bool";
            else if (ot->kind == TYPE_VEC) tname = "Vec";
            else if (ot->kind == TYPE_MAP) tname = "Map";
            if (tname) {
                const char *st = tname;
                if (strchr(st, '.')) st = strrchr(st, '.') + 1;
                int found = -1, ambiguous = 0;
                for (int ii = 0; ii < ctx->n_impls; ii++) {
                    if (strcmp(ctx->impls[ii].type_name, st) != 0) continue;
                    Node *idecl = ctx->impls[ii].decl;
                    for (int m = 0; m < idecl->impl_methods.len; m++) {
                        Node *mf = idecl->impl_methods.data[m];
                        const char *mn = strrchr(mf->fn_name, '.');
                        mn = mn ? mn + 1 : mf->fn_name;
                        if (strcmp(mn, n->callee->field_name) != 0) continue;
                        if (found >= 0) ambiguous = 1;
                        found = ii;
                    }
                }
                if (ambiguous) {
                    check_error(ctx, n->pos,
                        "методът '%s' е нееднозначен за типа '%s' — два impl-а го дават",
                        n->callee->field_name, tname);
                } else if (found >= 0) {
                    Node *idecl = ctx->impls[found].decl;
                    for (int m = 0; m < idecl->impl_methods.len; m++) {
                        Node *mf = idecl->impl_methods.data[m];
                        const char *mn = strrchr(mf->fn_name, '.');
                        mn = mn ? mn + 1 : mf->fn_name;
                        if (strcmp(mn, n->callee->field_name) != 0) continue;
                        /* пренапиши callee-то и вмъкни obj като self —
                         * obj се хваща ПРЕДИ union презаписването на callee */
                        Node *obj = n->callee->field_obj;
                        n->callee->kind = NODE_IDENT;
                        n->callee->name = strdup(mf->fn_name);
                        NodeVec na = {0};
                        vec_push(na, obj);
                        for (int a = 0; a < n->args.len; a++)
                            vec_push(na, n->args.data[a]);
                        free(n->args.data);
                        n->args = na;
                        break;
                    }
                }
            }
        }
    }

    /* L5: извикване през fn стойност — локална/параметър, или произволен
     * израз с TYPE_FN тип (vec_get(t, i)(x), obj.handler(x) и пр.).
     * A1 NODE_PATH is a sum constructor, not a fn value — skip this branch. */
    if (n->callee->kind == NODE_IDENT) {
        Type *lt = env_lookup(ctx, n->callee->name);
        if (lt && lt->kind == TYPE_FN)
            return call_fn_value(ctx, n, lt);
    } else if (n->callee->kind != NODE_PATH) {
        Type *ct = infer(ctx, n->callee);
        if (ct && ct->kind == TYPE_FN)
            return call_fn_value(ctx, n, ct);
        check_error(ctx, n->pos, "извикване на не-функция (%s)", type_str(ct));
        return type_new(TYPE_ERROR);
    }

    /* L3/A1: конструктор на sum enum — Variant(payload) или Enum::Variant(payload).
     * Bare: точно един payload-вариант с това име; иначе Enum::Variant. */
    if (n->callee->kind == NODE_PATH) {
        const char *en = n->callee->path_enum;
        const char *vn = n->callee->path_variant;
        int vhit = -1;
        for (int vi = 0; vi < ctx->n_variants; vi++)
            if (ctx->variants[vi].payload &&
                strcmp(ctx->variants[vi].enum_name, en) == 0 &&
                strcmp(ctx->variants[vi].variant, vn) == 0)
                { vhit = vi; break; }
        if (vhit < 0) {
            check_error(ctx, n->pos,
                "няма payload-вариант '%s::%s'", en, vn);
            return type_new(TYPE_ERROR);
        }
        if (n->args.len != 1) {
            check_error(ctx, n->pos,
                "конструкторът '%s::%s' очаква 1 аргумент, получих %d",
                en, vn, n->args.len);
            return type_new(TYPE_ERROR);
        }
        Type *at = n->args.data[0]->type;
        if (!type_assignable(at, ctx->variants[vhit].payload)) {
            check_error(ctx, n->pos,
                "'%s::%s': аргументът е от тип %s, но payload-ът е %s",
                en, vn, type_str(at), type_str(ctx->variants[vhit].payload));
        }
        Type *t = type_new(TYPE_ENUM);
        t->name = strdup(ctx->variants[vhit].enum_name);
        return t;
    }
    if (n->callee->kind == NODE_IDENT) {
        int vhit = -1, nhit = 0, hit0 = -1;
        for (int vi = 0; vi < ctx->n_variants; vi++) {
            if (!ctx->variants[vi].payload) continue;
            if (strcmp(ctx->variants[vi].variant, n->callee->name) != 0) continue;
            nhit++;
            if (hit0 < 0) hit0 = vi;
        }
        if (nhit > 1) {
            check_error(ctx, n->pos,
                "вариантът '%s' е нееднозначен — ползвай Enum::%s(...) "
                "(напр. %s::%s)",
                n->callee->name, n->callee->name,
                ctx->variants[hit0].enum_name, n->callee->name);
            return type_new(TYPE_ERROR);
        }
        if (nhit == 1) vhit = hit0;
        if (vhit >= 0) {
            if (n->args.len != 1) {
                check_error(ctx, n->pos,
                    "конструкторът '%s' очаква 1 аргумент, получих %d",
                    n->callee->name, n->args.len);
                return type_new(TYPE_ERROR);
            }
            Type *at = n->args.data[0]->type;
            if (!type_assignable(at, ctx->variants[vhit].payload)) {
                check_error(ctx, n->pos,
                    "'%s': аргументът е от тип %s, но payload-ът е %s",
                    n->callee->name, type_str(at),
                    type_str(ctx->variants[vhit].payload));
            }
            Type *t = type_new(TYPE_ENUM);
            t->name = strdup(ctx->variants[vhit].enum_name);
            return t;
        }
    }

    /* builtins */
    if (n->callee->kind == NODE_IDENT) {
        const char *name = n->callee->name;

        /* user-defined (incl. extern) functions shadow builtins.
         * L6: късото име може да идва от няколко модула — печели дефиницията
         * от модула на САМИЯ call site (вкл. главния файл), после
         * единственият друг модул; иначе е нееднозначно и искаме уточнение
         * 'модул.функция'.
         * Forward декларация + реализация (fmrbaga override идиомът):
         * ТИПЪт (ефектите-контракт) идва от декларацията без тяло —
         * историческото first-wins поведение; СИМВОЛЪТ — от тялото.
         * Нееднозначността се мери само между кандидати с тяло. */
        Type *ft_user = NULL;
        {
            const char *own = mod_base(n->pos.file);
            int first = -1, ownidx = -1, fwd = -1, first_body = -1;
            const char *o1 = NULL, *o2 = NULL;
            for (int i = 0; i < ctx->n_fns; i++) {
                if (strcmp(ctx->fns[i].name, name) != 0 &&
                    strcmp(ctx->fns[i].decl->fn_name, name) != 0) continue;
                if (first < 0) first = i;
                if (!ctx->fns[i].decl->fn_body) {
                    if (fwd < 0) fwd = i;
                    continue;
                }
                if (first_body < 0) first_body = i;
                if (strcmp(ctx->fns[i].origin, own) == 0)
                    ownidx = i;
                if (!o1) o1 = ctx->fns[i].origin;
                else if (!o2 && strcmp(o1, ctx->fns[i].origin) != 0) o2 = ctx->fns[i].origin;
            }
            int chosen = -1;
            Node *chosen_decl = NULL;
            if (ownidx >= 0) chosen = ownidx;
            else if (!o2) chosen = first_body >= 0 ? first_body : first;
            else {
                check_error(ctx, n->pos,
                    "нееднозначно извикване на '%s' — има я в модулите '%s' и '%s'; уточни с %s.%s или %s.%s",
                    name, o1, o2, o1, name, o2, name);
                chosen = first_body;  /* грешката е записана; продължаваме проверките */
            }
            if (chosen >= 0) {
                ft_user = (fwd >= 0 && ctx->fns[chosen].decl->fn_body)
                    ? ctx->fns[fwd].fn_type      /* контрактът на декларацията */
                    : ctx->fns[chosen].fn_type;
                chosen_decl = ctx->fns[chosen].decl;
                /* уникалният вътрешен символ (преименуван при дубликати);
                 * strdup — node_free освобождава callee->name.
                 * M21: за generic извикване името се слага от инстанцията */
                if (!chosen_decl || chosen_decl->n_type_params == 0)
                    n->callee->name = strdup(chosen_decl->fn_name);
            }
        }
        if (ft_user && ft_user->kind == TYPE_FN) {
            n->callee->type = ft_user;
            /* M21: generic извикване — инстанция + substituted проверка */
            {
                Node *gfn = NULL;
                for (int gi = 0; gi < ctx->n_fns; gi++)
                    if (ctx->fns[gi].fn_type == ft_user &&
                        ctx->fns[gi].decl->n_type_params > 0)
                        { gfn = ctx->fns[gi].decl; break; }
                if (gfn) {
                    Type *gret = generic_instantiate(ctx, gfn, n);
                    /* generic_instantiate оставя substitution активна —
                     * резолвай params под нея, после чисти */
                    int np2 = gfn->params.len;
                    if (n->args.len != np2) {
                        check_error(ctx, n->pos, "'%s' очаква %d аргумента, получих %d",
                                    name, np2, n->args.len);
                    }
                    int check_n = n->args.len < np2 ? n->args.len : np2;
                    for (int i = 0; i < check_n; i++) {
                        Type *at = n->args.data[i]->type;
                        Type *pt = resolve_type_node(ctx, gfn->params.data[i]->param_type);
                        if (!type_assignable(at, pt)) {
                            check_error(ctx, n->pos,
                                "'%s': аргумент #%d е от тип %s, но параметърът е %s",
                                name, i + 1, type_str(at), type_str(pt));
                        } else if (at && pt) {
                            /* W3.8: неанотиран Vec/Map аргумент — параметърът
                             * (вече substituted) фиксира елементния вид, точно
                             * както при non-generic извикване. Без това
                             * `let out = vec_new()` + generic push печаташе
                             * указател вместо низа: по-късният vec_get фиксира
                             * i64, а codegen-ът чете str памет като i64. */
                            if (at->kind == TYPE_VEC && pt->kind == TYPE_VEC &&
                                !at->elem && pt->elem)
                                at->elem = pt->elem;
                            if (at->kind == TYPE_MAP && pt->kind == TYPE_MAP) {
                                if (!at->key && pt->key) at->key = pt->key;
                                if (!at->elem && pt->elem) at->elem = pt->elem;
                            }
                        }
                    }
                    ctx->g_names.len = 0; ctx->g_types.len = 0;
                    /* резултат от substituted ret */
                    Type *result = type_new(gret->kind);
                    result->elem = gret->elem;
                    result->name = gret->name;
                    result->ret = gret->ret;
                    result->params = gret->params;
                    result->nparams = gret->nparams;
                    /* M24: instantiated generic struct резултат */
                    if (gret->n_targs > 0) {
                        result->targs = malloc(sizeof(Type *) * (size_t)gret->n_targs);
                        result->n_targs = gret->n_targs;
                        for (int a = 0; a < gret->n_targs; a++)
                            result->targs[a] = gret->targs[a];
                    }
                    type_merge_effects(result, gret);
                    if (ctx->cur_effects)
                        type_merge_effects(ctx->cur_effects, gret);
                    /* W3.7: generic инстанция — същото замърсяване на out-param
                     * контейнер като при non-generic извикване. Резюмето е
                     * изчислено: generic_instantiate вика check_fn за инстанцията,
                     * която изпълнява mem3_summarize_fn. */
                    FnRec *gfr = find_fn_rec(ctx, gfn->fn_name);
                    if (gfr && gfr->checked)
                        mem3_apply_out_taint(ctx, gfr, n);
                    return result;
                }
            }
            if (n->args.len != ft_user->nparams) {
                check_error(ctx, n->pos, "'%s' очаква %d аргумента, получих %d",
                            name, ft_user->nparams, n->args.len);
            }
            int check_n = n->args.len < ft_user->nparams ? n->args.len : ft_user->nparams;
            for (int i = 0; i < check_n; i++) {
                Type *at = n->args.data[i]->type;
                Type *pt = ft_user->params[i];
                if (!type_assignable(at, pt)) {
                    check_error(ctx, n->pos,
                        "'%s': аргумент #%d е от тип %s, но параметърът е %s",
                        name, i + 1, type_str(at), type_str(ft_user->params[i]));
                } else if (at && pt) {
                    /* Неанотиран Vec/Map аргумент: параметърът фиксира
                     * елементния вид — същата мутация като при vec_push
                     * (env пази същия Type указател, така че фиксирането
                     * стига до всички употреби). Без това един по-късен
                     * vec_get фиксира i64 и codegen-ът чете str памет като
                     * i64 (tplbaga P5). */
                    if (at->kind == TYPE_VEC && pt->kind == TYPE_VEC &&
                        !at->elem && pt->elem)
                        at->elem = pt->elem;
                    if (at->kind == TYPE_MAP && pt->kind == TYPE_MAP) {
                        if (!at->key && pt->key) at->key = pt->key;
                        if (!at->elem && pt->elem) at->elem = pt->elem;
                    }
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
            /* M24: instantiated generic struct резултат — targs-ите също
             * (като generic пътя по-горе; иначе codegen вижда голото име) */
            result->targs = ret->targs;
            result->n_targs = ret->n_targs;
            /* fn (L5): keep the signature so the returned closure is callable */
            result->ret = ret->ret;
            result->params = ret->params;
            result->nparams = ret->nparams;
            type_merge_effects(result, ret);
            if (ctx->cur_effects)
                type_merge_effects(ctx->cur_effects, ret);
            {
                FnRec *fr = find_fn_rec(ctx, n->callee->name);
                if (fr) mem3_ensure_fn(ctx, fr->decl);
                /* W3.6: callee замърсява param-контейнер → замърсяваме
                 * аргумента-контейнер тук, с региона на аргумента-източник.
                 * Иначе `stash(out, wvec_get_str(w, …)); world_free(w)`
                 * минаваше тихо (висящ указател в out след free). */
                if (fr && fr->checked) mem3_apply_out_taint(ctx, fr, n);
            }
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
                xt->kind != TYPE_F64 && xt->kind != TYPE_BYTES &&
                xt->kind != TYPE_STRUCT && xt->kind != TYPE_FN &&
                xt->kind != TYPE_VEC && xt->kind != TYPE_ENUM) {
                check_error(ctx, n->pos,
                    "%s: неподдържан елементен тип %s за Vec (поддържат се i64, str, f64, bytes, struct, sum enum, fn и Vec)",
                    name, type_str(xt));
                return type_new(TYPE_ERROR);
            }
            if (!vt->elem) {
                vt->elem = xt;
            } else if (!vec_elem_eq(vt->elem, xt)) {
                check_error(ctx, n->pos, "%s: елемент от тип %s, но векторът е %s",
                            name, type_str(xt), type_str(vt));
            }
            /* W3.5: заемана стойност от регион (свят/арена), записана във
             * вектор, увисва при free — векторът е в bump арената и надживява
             * региона. ASan: heap-use-after-free при print на vec_get след
             * world_free. Отбелязваме контейнера, не елемента. */
            mem3_taint_container(ctx, n->args.data[0], n->args.data[xidx], 0);
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

        /* карти: Map<K, V> — ключ i64/str, стойност i64/str/f64/bytes.
         * Чист Map фиксира ключа/стойността при първия map_set —
         * същият механизъм като Vec (env пази същия Type указател). */
        if (strcmp(name, "map_new") == 0) {
            n->callee->type = type_new(TYPE_VOID);
            if (n->args.len != 0) {
                check_error(ctx, n->pos, "'map_new' очаква 0 аргумента, получих %d",
                            n->args.len);
                return type_new(TYPE_ERROR);
            }
            return type_new(TYPE_MAP);      /* key/elem = NULL: неизвестни */
        }
        /* R66: unsafe bytes handle casts — zero-copy hop of binary values
         * across chan(i64) (PARALLEL workers). Boxes a baga_bytes header on
         * the arena; payload data is already arena-bound. */
        if (strcmp(name, "bytes_h") == 0) {
            n->callee->type = type_new(TYPE_VOID);
            if (n->args.len != 1) {
                check_error(ctx, n->pos, "'bytes_h' очаква 1 аргумент, получих %d",
                            n->args.len);
                return type_new(TYPE_ERROR);
            }
            Type *bt = n->args.data[0]->type;
            if (bt && bt->kind != TYPE_BYTES && bt->kind != TYPE_ERROR) {
                check_error(ctx, n->pos, "'bytes_h' очаква bytes, получих %s",
                            type_str(bt));
                return type_new(TYPE_ERROR);
            }
            return type_new(TYPE_I64);
        }
        if (strcmp(name, "h_bytes") == 0) {
            n->callee->type = type_new(TYPE_VOID);
            if (n->args.len != 1) {
                check_error(ctx, n->pos, "'h_bytes' очаква 1 аргумент, получих %d",
                            n->args.len);
                return type_new(TYPE_ERROR);
            }
            return type_new(TYPE_BYTES);
        }
        /* R55: unsafe map handle casts — предаване на споделена карта през
         * i64 контекст на go_bg (като str_h/h_str, R51). Map е baga_Map*
         * (heap, arena-ish живот) — handle-ът е валиден за живота на процеса. */
        if (strcmp(name, "map_h") == 0) {
            n->callee->type = type_new(TYPE_VOID);
            if (n->args.len != 1) {
                check_error(ctx, n->pos, "'map_h' очаква 1 аргумент, получих %d",
                            n->args.len);
                return type_new(TYPE_ERROR);
            }
            return type_new(TYPE_I64);
        }
        if (strcmp(name, "h_map") == 0) {
            n->callee->type = type_new(TYPE_VOID);
            if (n->args.len != 1) {
                check_error(ctx, n->pos, "'h_map' очаква 1 аргумент, получих %d",
                            n->args.len);
                return type_new(TYPE_ERROR);
            }
            return type_new(TYPE_MAP);      /* key/elem = NULL: неизвестни */
        }
        if (strcmp(name, "map_set") == 0) {
            n->callee->type = type_new(TYPE_VOID);
            if (n->args.len != 3) {
                check_error(ctx, n->pos,
                    "'map_set' очаква 3 аргумента (карта, ключ, стойност), получих %d",
                    n->args.len);
                return type_new(TYPE_ERROR);
            }
            Type *mt = n->args.data[0]->type;
            if (!mt || mt->kind != TYPE_MAP) {
                check_error(ctx, n->pos, "'map_set' върху не-карта (%s)", type_str(mt));
                return type_new(TYPE_ERROR);
            }
            Type *kt = n->args.data[1]->type;
            if (kt && kt->kind == TYPE_I32) kt = type_new(TYPE_I64);
            if (!kt || (kt->kind != TYPE_I64 && kt->kind != TYPE_STR &&
                        kt->kind != TYPE_BYTES && kt->kind != TYPE_ERROR)) {
                check_error(ctx, n->pos,
                    "map_set: неподдържан ключов тип %s за Map (поддържат се i64, str и bytes)",
                    type_str(kt));
                return type_new(TYPE_ERROR);
            }
            Type *vt = n->args.data[2]->type;
            if (vt && vt->kind == TYPE_I32) vt = type_new(TYPE_I64);
            if (!vt || (vt->kind != TYPE_I64 && vt->kind != TYPE_STR &&
                        vt->kind != TYPE_F64 && vt->kind != TYPE_BYTES &&
                        vt->kind != TYPE_STRUCT && vt->kind != TYPE_FN &&
                        vt->kind != TYPE_ENUM &&
                        vt->kind != TYPE_ERROR)) {
                check_error(ctx, n->pos,
                    "map_set: неподдържан стойностен тип %s за Map (поддържат се i64, str, f64, bytes, struct, sum enum и fn)",
                    type_str(vt));
                return type_new(TYPE_ERROR);
            }
            if (kt->kind != TYPE_ERROR) {
                if (!mt->key) {
                    mt->key = kt;
                } else if (mt->key->kind != kt->kind) {
                    check_error(ctx, n->pos, "map_set: ключ от тип %s, но картата е %s",
                        type_str(kt), type_str(mt));
                }
            }
            if (vt->kind != TYPE_ERROR) {
                if (!mt->elem) {
                    mt->elem = vt;
                } else if (!vec_elem_eq(mt->elem, vt)) {
                    check_error(ctx, n->pos, "map_set: стойност от тип %s, но картата е %s",
                        type_str(vt), type_str(mt));
                }
            }
            /* W3.5/W3.9: заемана СТОЙНОСТ увисва при free — и ЗАЕМАН КЛЮЧ
             * също (ключовете са указатели: e->sk = sk). Ключовете се следят
             * отделно, защото map_keys връща точно тях. */
            mem3_taint_container(ctx, n->args.data[0], n->args.data[1], 1);
            mem3_taint_container(ctx, n->args.data[0], n->args.data[2], 0);
            return type_new(TYPE_VOID);
        }
        if (strcmp(name, "map_get") == 0 || strcmp(name, "map_has") == 0 ||
            strcmp(name, "map_del") == 0) {
            n->callee->type = type_new(TYPE_VOID);
            if (n->args.len != 2) {
                check_error(ctx, n->pos, "'%s' очаква 2 аргумента (карта, ключ), получих %d",
                            name, n->args.len);
                return type_new(TYPE_ERROR);
            }
            Type *mt = n->args.data[0]->type;
            if (!mt || mt->kind != TYPE_MAP) {
                check_error(ctx, n->pos, "'%s' върху не-карта (%s)", name, type_str(mt));
                return type_new(TYPE_ERROR);
            }
            Type *kt = n->args.data[1]->type;
            if (kt && kt->kind == TYPE_I32) kt = type_new(TYPE_I64);
            if (kt && kt->kind != TYPE_ERROR) {
                if (kt->kind != TYPE_I64 && kt->kind != TYPE_STR &&
                    kt->kind != TYPE_BYTES) {
                    check_error(ctx, n->pos,
                        "%s: неподдържан ключов тип %s за Map (поддържат се i64, str и bytes)",
                        name, type_str(kt));
                    return type_new(TYPE_ERROR);
                }
                /* чист Map: ключът се фиксира от първата употреба (като Vec) */
                if (!mt->key) {
                    mt->key = kt;
                } else if (mt->key->kind != kt->kind) {
                    check_error(ctx, n->pos, "%s: ключ от тип %s, но картата е %s",
                        name, type_str(kt), type_str(mt));
                }
            }
            if (strcmp(name, "map_has") == 0) return type_new(TYPE_I64);
            if (strcmp(name, "map_del") == 0) return type_new(TYPE_VOID);
            /* map_get: неизвестна стойност → i64 (исторически, като vec_get) */
            if (!mt->elem) mt->elem = type_new(TYPE_I64);
            return mt->elem;
        }
        if (strcmp(name, "map_len") == 0) {
            n->callee->type = type_new(TYPE_VOID);
            return type_new(TYPE_I64);
        }
        if (strcmp(name, "map_keys") == 0) {
            n->callee->type = type_new(TYPE_VOID);
            Type *mt = n->args.len > 0 ? n->args.data[0]->type : NULL;
            if (!mt || mt->kind != TYPE_MAP) {
                check_error(ctx, n->pos, "'map_keys' върху не-карта (%s)", type_str(mt));
                return type_new(TYPE_ERROR);
            }
            /* чист Map: ключовете са str по подразбиране (като vec_get → i64) */
            if (!mt->key) mt->key = type_new(TYPE_STR);
            Type *r = type_new(TYPE_VEC);
            r->elem = mt->key;
            return r;
        }

        /* go / go_bg(fn, arg) — spawn OS thread; fn is a bare function identifier */
        if (strcmp(name, "go") == 0 || strcmp(name, "go_bg") == 0) {
            n->callee->type = type_new(TYPE_VOID);
            if (n->args.len != 2) {
                check_error(ctx, n->pos, "'%s' очаква 2 аргумента (fn, arg), получих %d",
                            name, n->args.len);
                return type_new(TYPE_ERROR);
            }
            Node *fnarg = n->args.data[0];
            if (fnarg->kind != NODE_IDENT) {
                check_error(ctx, n->pos, "'%s': първият аргумент трябва да е име на функция", name);
                return type_new(TYPE_ERROR);
            }
            Type *wft = ck_find_fn(ctx, fnarg->name);
            if (!wft || wft->kind != TYPE_FN) {
                check_error(ctx, n->pos, "'%s': '%s' не е функция", name, fnarg->name);
                return type_new(TYPE_ERROR);
            }
            if (wft->nparams != 1 || !wft->params[0] || wft->params[0]->kind != TYPE_I64) {
                check_error(ctx, n->pos,
                    "'%s': worker '%s' трябва да е fn(i64) -> i64", name, fnarg->name);
                return type_new(TYPE_ERROR);
            }
            if (!wft->ret || wft->ret->kind != TYPE_I64) {
                check_error(ctx, n->pos,
                    "'%s': worker '%s' трябва да връща i64", name, fnarg->name);
                return type_new(TYPE_ERROR);
            }
            Type *at = n->args.data[1]->type;
            if (at && at->kind != TYPE_I64 && at->kind != TYPE_ERROR) {
                check_error(ctx, n->pos, "'%s': arg е %s, очаквах i64", name, type_str(at));
            }
            Type *ret = type_new(TYPE_I64);
            type_add_effect(ret, "Par");
            /* worker effects bubble to the spawn site (cloud handlers may do !IO) */
            if (wft->ret) type_merge_effects(ret, wft->ret);
            if (ctx->cur_effects) {
                type_add_effect(ctx->cur_effects, "Par");
                if (wft->ret) type_merge_effects(ctx->cur_effects, wft->ret);
            }
            return ret;
        }

        /* pool_map(fn, vec, nworkers) -> Vec<i64> !Par — bounded parallel map */
        if (strcmp(name, "pool_map") == 0) {
            n->callee->type = type_new(TYPE_VOID);
            if (n->args.len != 3) {
                check_error(ctx, n->pos,
                    "'pool_map' очаква 3 аргумента (fn, vec, nworkers), получих %d",
                    n->args.len);
                return type_new(TYPE_ERROR);
            }
            Node *fnarg = n->args.data[0];
            if (fnarg->kind != NODE_IDENT) {
                check_error(ctx, n->pos, "'pool_map': първият аргумент трябва да е име на функция");
                return type_new(TYPE_ERROR);
            }
            Type *wft = ck_find_fn(ctx, fnarg->name);
            if (!wft || wft->kind != TYPE_FN) {
                check_error(ctx, n->pos, "'pool_map': '%s' не е функция", fnarg->name);
                return type_new(TYPE_ERROR);
            }
            if (wft->nparams != 1 || !wft->params[0] || wft->params[0]->kind != TYPE_I64) {
                check_error(ctx, n->pos,
                    "'pool_map': worker '%s' трябва да е fn(i64) -> i64", fnarg->name);
                return type_new(TYPE_ERROR);
            }
            if (!wft->ret || wft->ret->kind != TYPE_I64) {
                check_error(ctx, n->pos,
                    "'pool_map': worker '%s' трябва да връща i64", fnarg->name);
                return type_new(TYPE_ERROR);
            }
            Type *vt = n->args.data[1]->type;
            if (!vt || vt->kind != TYPE_VEC) {
                check_error(ctx, n->pos, "'pool_map': вторият аргумент трябва да е Vec");
                return type_new(TYPE_ERROR);
            }
            if (vt->elem && vt->elem->kind != TYPE_I64 && vt->elem->kind != TYPE_ERROR) {
                check_error(ctx, n->pos, "'pool_map': Vec елементите трябва да са i64");
            }
            Type *nt = n->args.data[2]->type;
            if (nt && nt->kind != TYPE_I64 && nt->kind != TYPE_ERROR) {
                check_error(ctx, n->pos, "'pool_map': nworkers е %s, очаквах i64", type_str(nt));
            }
            Type *ret = type_new(TYPE_VEC);
            ret->elem = type_new(TYPE_I64);
            type_add_effect(ret, "Par");
            if (wft->ret) type_merge_effects(ret, wft->ret);
            if (ctx->cur_effects) {
                type_add_effect(ctx->cur_effects, "Par");
                if (wft->ret) type_merge_effects(ctx->cur_effects, wft->ret);
            }
            return ret;
        }

        /* string / io / par builtins. sig: параметри по позиции —
         * 's' str, 'i' i64 (bool се приема), 'f' f64, 'b' bytes, 'a' всякакъв.
         * Преди LP3 таблицата носеше само return тип: грешен тип аргумент
         * (напр. ord(65)) стигаше до C и гърмеше runtime (segfault). */
        struct { const char *name; TypeKind ret; const char *sig; int has_io; int has_par; } builtins[] = {
            {"len",       TYPE_I64, "s", 0, 0},
            {"char_at",   TYPE_I64, "si", 0, 0},
            {"byte_at",   TYPE_I64, "si", 0, 0},
            {"byte_chr",  TYPE_STR, "i", 0, 0},
            {"substr",    TYPE_STR, "sii", 0, 0},
            {"concat",    TYPE_STR, "ss", 0, 0},
            {"read_file", TYPE_STR, "s", 1, 0},
            {"chr",       TYPE_STR, "i", 0, 0},
            {"ord",       TYPE_I64, "s", 0, 0},
            {"i64_to_str", TYPE_STR, "i", 0, 0},
            {"f64_to_str", TYPE_STR, "f", 0, 0},
            {"str_eq",    TYPE_BOOL, "ss", 0, 0},
            {"str_repeat_b", TYPE_STR, "si", 0, 0},
            {"arg_count", TYPE_I64, "", 0, 0},
            {"arg",       TYPE_STR, "i", 0, 0},
            {"exit",      TYPE_VOID, "i", 0, 0},
            {"eprintln",  TYPE_VOID, "s", 0, 0},
            {"arena_new",   TYPE_I64, "", 0, 0},
            {"arena_alloc", TYPE_I64, "ii", 0, 0},
            {"arena_reset", TYPE_VOID, "i", 0, 0},
            {"arena_free",  TYPE_VOID, "i", 0, 0},
            {"world_new",    TYPE_I64, "", 0, 0},
            {"world_alloc",  TYPE_I64, "ii", 0, 0},
            {"world_freeze", TYPE_I64, "i", 0, 0},
            {"world_state",  TYPE_I64, "i", 0, 0},
            {"world_free",   TYPE_VOID, "i", 0, 0},
            {"wvec_new",     TYPE_I64, "i", 0, 0},
            {"wvec_push",    TYPE_VOID, "iii", 0, 0},
            {"wvec_get",     TYPE_I64, "iii", 0, 0},
            {"wvec_len",     TYPE_I64, "ii", 0, 0},
            {"world_dump",   TYPE_BYTES, "i", 0, 0},
            {"world_load",   TYPE_I64, "b", 0, 0},
            {"world_used",   TYPE_I64, "i", 0, 0},
            {"wvec_push_str", TYPE_VOID, "iis", 0, 0},
            {"wvec_get_str",  TYPE_STR, "iii", 0, 0},
            {"wcache_set",    TYPE_VOID, "iii", 0, 0},
            {"wcache_get",    TYPE_I64, "ii", 0, 0},
            {"wcache_add",    TYPE_I64, "iii", 0, 0},
            {"wcache_set_str",   TYPE_VOID, "iis", 0, 0},
            {"wcache_get_str",   TYPE_STR, "ii", 0, 0},
            {"wcache_set_bytes", TYPE_VOID, "iib", 0, 0},
            {"wcache_get_bytes", TYPE_BYTES, "ii", 0, 0},
            {"wcache_has",    TYPE_I64, "ii", 0, 0},
            {"wcache_del",    TYPE_VOID, "ii", 0, 0},
            {"wcache_clear",  TYPE_VOID, "i", 0, 0},
            {"wcache_len",    TYPE_I64, "i", 0, 0},
            {"world_link",    TYPE_VOID, "ii", 0, 0},
            {"world_unlink",  TYPE_VOID, "ii", 0, 0},
            {"world_links",   TYPE_I64, "i", 0, 0},
            {"world_linked",  TYPE_I64, "ii", 0, 0},
            {"wmap_new",      TYPE_I64, "i", 0, 0},
            {"wmap_set",      TYPE_VOID, "iiii", 0, 0},
            {"wmap_get",      TYPE_I64, "iii", 0, 0},
            {"wmap_has",      TYPE_I64, "iii", 0, 0},
            {"wmap_del",      TYPE_VOID, "iii", 0, 0},
            {"wmap_len",      TYPE_I64, "ii", 0, 0},
            {"wmap_set_str",  TYPE_VOID, "iiis", 0, 0},
            {"wmap_get_str",  TYPE_STR, "iii", 0, 0},
            {"wvec_push_bytes", TYPE_VOID, "iib", 0, 0},
            {"wvec_get_bytes", TYPE_BYTES, "iii", 0, 0},
            {"world_compact",      TYPE_VOID, "i", 0, 0},
            {"world_dump_compact", TYPE_BYTES, "i", 0, 0},
            {"wrec_new",       TYPE_I64, "ii", 0, 0},
            {"wrec_len",       TYPE_I64, "ii", 0, 0},
            {"wrec_has",       TYPE_I64, "iii", 0, 0},
            {"wrec_set",       TYPE_VOID, "iiii", 0, 0},
            {"wrec_set_str",   TYPE_VOID, "iiis", 0, 0},
            {"wrec_set_bytes", TYPE_VOID, "iiib", 0, 0},
            {"wrec_get",       TYPE_I64, "iii", 0, 0},
            {"wrec_get_str",   TYPE_STR, "iii", 0, 0},
            {"wrec_get_bytes", TYPE_BYTES, "iii", 0, 0},
            {"mem_mark",    TYPE_I64, "", 0, 0},
            {"mem_rewind",  TYPE_VOID, "i", 0, 0},
            {"mem_arena_bytes", TYPE_I64, "", 0, 0},
            {"mem_spare_bytes", TYPE_I64, "", 0, 0},
            {"mem_peak_bytes",  TYPE_I64, "", 0, 0},
            {"rc_on",       TYPE_BOOL, "", 0, 0},
            {"mem_persist_begin", TYPE_VOID, "", 0, 0},
            {"mem_persist_end",   TYPE_VOID, "", 0, 0},
            {"bytes_len",   TYPE_I64, "b", 0, 0},
            {"bytes_at",    TYPE_I64, "bi", 0, 0},
            {"bytes_slice", TYPE_BYTES, "bii", 0, 0},
            {"bytes_concat", TYPE_BYTES, "bb", 0, 0},
            {"bytes_new",   TYPE_BYTES, "i", 0, 0},
            {"bytes_set",   TYPE_VOID, "bii", 0, 0},
            {"bytes_put",   TYPE_VOID, "bib", 0, 0}, /* R54: dst[off..+len)=src memcpy */
            {"bytes_push",  TYPE_BYTES, "bi", 0, 0},
            {"bytes_of_str", TYPE_BYTES, "s", 0, 0},
            {"str_of_bytes", TYPE_STR, "b", 0, 0},
            {"hex_encode",  TYPE_STR, "b", 0, 0},
            {"hex_decode",  TYPE_BYTES, "s", 0, 0},
            /* concurrency (!Par) — cloud-native fan-out / CSP channels */
            {"join",        TYPE_I64, "i", 0, 1},
            {"detach",      TYPE_I64, "i", 0, 1},
            {"chan_new",    TYPE_I64, "i", 0, 1},
            {"chan_send",   TYPE_I64, "ii", 0, 1},
            {"chan_recv",   TYPE_I64, "i", 0, 1},
            {"chan_recv2",  TYPE_I64, "i", 0, 1}, /* cell2(ok, value) */
            {"chan_try_recv", TYPE_I64, "i", 0, 1}, /* cell2(status, value) */
            {"chan_recv_timeout", TYPE_I64, "ii", 0, 1},
            {"chan_select2", TYPE_I64, "ii", 0, 1}, /* cell2(which, value) */
            {"chan_select2_wait", TYPE_I64, "ii", 0, 1},
            {"chan_select2_timeout", TYPE_I64, "iii", 0, 1},
            {"chan_close",  TYPE_I64, "i", 0, 1},
            {"chan_len",    TYPE_I64, "i", 0, 1},
            {"sleep_ms",    TYPE_I64, "i", 0, 1},
            {"mutex_new",   TYPE_I64, "", 0, 1},
            {"mutex_lock",  TYPE_I64, "i", 0, 1},
            {"mutex_unlock",TYPE_I64, "i", 0, 1},
            /* C1 signals — graceful shutdown (K8s SIGTERM); process-global */
            {"signal_watch", TYPE_I64, "i", 0, 0}, /* install handler; 0 ok, -1 err */
            {"signal_check", TYPE_I64, "", 0, 0}, /* 0 = none, else signo */
            {"signal_clear", TYPE_I64, "", 0, 0}, /* return+clear pending */
            {"signal_wait",  TYPE_I64, "i", 0, 0}, /* ms; <0 = forever; return signo or 0 */
            {"signal_raise", TYPE_I64, "i", 0, 0}, /* raise(sig) to self; 0 ok */
            /* heap pair — pure context packing for single-arg go workers */
            {"cell2",       TYPE_I64, "aa", 0, 0},
            {"cell2_0",     TYPE_I64, "a", 0, 0},
            {"cell2_1",     TYPE_I64, "a", 0, 0},
            {"cell2_free",  TYPE_VOID, "a", 0, 0},
            /* R51: unsafe str handle casts — zero-copy chan hop (C backend) */
            {"str_h",       TYPE_I64, "s", 0, 0},
            {"h_str",       TYPE_STR, "i", 0, 0},
            /* R66: also in special-case above for TYPE_BYTES return of h_bytes */
            {"bytes_h",     TYPE_I64, "b", 0, 0},
            {"h_bytes",     TYPE_BYTES, "i", 0, 0},
        };
        /* bytes_from_vec / vec_from_bytes — typed bridge (not in the flat table) */
        if (strcmp(name, "bytes_from_vec") == 0) {
            n->callee->type = type_new(TYPE_VOID);
            Type *vt = n->args.len > 0 ? n->args.data[0]->type : NULL;
            if (!vt || vt->kind != TYPE_VEC)
                check_error(ctx, n->pos, "'bytes_from_vec' очаква Vec");
            return type_new(TYPE_BYTES);
        }
        if (strcmp(name, "vec_from_bytes") == 0) {
            n->callee->type = type_new(TYPE_VOID);
            Type *bt = n->args.len > 0 ? n->args.data[0]->type : NULL;
            if (!bt || bt->kind != TYPE_BYTES)
                check_error(ctx, n->pos, "'vec_from_bytes' очаква bytes");
            Type *r = type_new(TYPE_VEC);
            r->elem = type_new(TYPE_I64);
            return r;
        }
        /* MEM-1: drop(x) — само let-локални Vec/Map/bytes/fn; MEM-2: пълен seatbelt */
        if (strcmp(name, "drop") == 0 &&
            !env_lookup(ctx, "drop") && !ck_find_fn(ctx, "drop")) {
            if (n->args.len != 1) {
                check_error(ctx, n->pos, "drop очаква 1 аргумент, получих %d", n->args.len);
                return type_new(TYPE_ERROR);
            }
            Node *a = n->args.data[0];
            Type *at = a->type;
            if (a->kind != NODE_IDENT) {
                check_error(ctx, n->pos, "drop очаква локална променлива (let), не израз");
                return type_new(TYPE_ERROR);
            }
            if (!at || (at->kind != TYPE_VEC && at->kind != TYPE_MAP &&
                        at->kind != TYPE_BYTES && at->kind != TYPE_FN)) {
                check_error(ctx, n->pos, "drop: неподдържан тип %s — drop е за Vec/Map/bytes/fn",
                            at ? type_str(at) : "?");
                return type_new(TYPE_ERROR);
            }
            /* MEM-2: live/dropped правила върху EnvEntry */
            EnvEntry *e = ck_env_find(ctx, a->name);
            if (!e) {
                check_error(ctx, n->pos, "drop: '%s' не е локална променлива", a->name);
                return type_new(TYPE_ERROR);
            }
            if (e->dropped) {
                check_error(ctx, n->pos, "повторен drop на '%s'", a->name);
                return type_new(TYPE_ERROR);
            }
            if (e->is_param) {
                check_error(ctx, n->pos,
                    "drop на параметър '%s' — параметрите споделят буфера на извикващия",
                    a->name);
                return type_new(TYPE_ERROR);
            }
            if (e->captured) {
                check_error(ctx, n->pos,
                    "'%s' е заснет от ламбда — drop би оставил висящ указател", a->name);
                return type_new(TYPE_ERROR);
            }
            /* MEM-2: loop_depth е depth-ът на нивото на loop-израза; променлива,
             * дефинирана на него или по-навън (scope_depth <= loop_depth), е
             * "външна за цикъла" — drop в тялото = use-after-drop на итерация 2 */
            if (ctx->loop_depth && e->scope_depth <= ctx->loop_depth) {
                check_error(ctx, n->pos,
                    "drop на външна за цикъла променлива '%s' — втората итерация би била use-after-drop",
                    a->name);
                return type_new(TYPE_ERROR);
            }
            if (ctx->n_drop_log < 256) {
                e->dropped = 1;
                ctx->drop_log[ctx->n_drop_log++] = e;
            } else {
                /* логът е пълен — НЕ маркираме: консервативно "пропусната
                 * грешка", никога измислена (runtime drop-ът пак се генерира);
                 * join-ът на обграждащия if ще вдигне и логнатите от клоните */
                ctx->drop_log_overflowed = 1;
            }
            return type_new(TYPE_VOID);
        }
        /* MEM-3: arena_free / arena_alloc / arena_reset seatbelt on the
         * arena handle (i64 local). Reuses EnvEntry.dropped + drop_log join
         * machinery — after arena_free(a), a is dead; double free and
         * alloc/reset on a freed handle are compile errors. Full region
         * tagging follows ident alias and p±n (region_of_expr); deeper
         * graphs stay untracked. */
        if ((strcmp(name, "arena_free") == 0 || strcmp(name, "arena_alloc") == 0 ||
             strcmp(name, "arena_reset") == 0) &&
            !env_lookup(ctx, name) && !ck_find_fn(ctx, name)) {
            int is_free = strcmp(name, "arena_free") == 0;
            int is_alloc = strcmp(name, "arena_alloc") == 0;
            int need = is_alloc ? 2 : 1;
            if (n->args.len != need) {
                check_error(ctx, n->pos, "'%s' очаква %d аргумент(а), получих %d",
                            name, need, n->args.len);
                return type_new(TYPE_ERROR);
            }
            Node *ah = n->args.data[0];
            if (ah->kind != NODE_IDENT) {
                if (is_free)
                    check_error(ctx, n->pos,
                        "arena_free очаква локална променлива (let), не израз");
                /* alloc/reset on non-ident: still type-check as i64 below */
            } else {
                EnvEntry *e = ck_env_find(ctx, ah->name);
                if (e) {
                    if (e->dropped) {
                        if (is_free)
                            check_error(ctx, n->pos,
                                "повторен arena_free на '%s'", ah->name);
                        else
                            check_error(ctx, n->pos,
                                "използване на арена '%s' след arena_free", ah->name);
                    } else if (is_free) {
                        if (e->is_param)
                            check_error(ctx, n->pos,
                                "arena_free на параметър '%s' — собствеността е на извикващия",
                                ah->name);
                        else if (ctx->loop_depth && e->scope_depth <= ctx->loop_depth)
                            check_error(ctx, n->pos,
                                "arena_free на външна за цикъла арена '%s'", ah->name);
                        else {
                            mem3_drop_handle(ctx, e, n->pos);
                        }
                    }
                }
            }
            n->callee->type = type_new(TYPE_VOID);
            if (is_alloc) return type_new(TYPE_I64);
            return type_new(TYPE_VOID);
        }
        /* W1.6: world seatbelt върху world handle-а (i64 локала). Всеки
         * world_/wvec_ builtin (без world_new и world_load) взима света като
         * аргумент 0. world_free ползва MEM-2 машинерията (dropped + drop_log),
         * така че след него всеки достъп до променливата гърми; world_frozen има
         * собствен freeze_log със същата join-семантика. „Maybe frozen"
         * (замразен само в един клон на if) НЕ е грешка — seatbelt-ът никога не
         * измисля; пътевите случаи ги покрива --verify (HK_WORLD). i64 параметър
         * се брои за потенциален handle (двоен world_free в callee е двоен free()
         * на malloc — истинско UB); алиаси извън `let b = w` и светове, върнати
         * от потребителска fn, остават неследени (MEM-1 граница). */
        if ((strcmp(name, "world_free") == 0 || strcmp(name, "world_freeze") == 0 ||
             strcmp(name, "world_alloc") == 0 || strcmp(name, "wvec_new") == 0 ||
             strcmp(name, "wvec_push") == 0 || strcmp(name, "wvec_push_str") == 0 ||
             strcmp(name, "world_link") == 0 || strcmp(name, "world_unlink") == 0 ||
             strcmp(name, "wmap_new") == 0 || strcmp(name, "wmap_set") == 0 ||
             strcmp(name, "wmap_del") == 0 || strcmp(name, "wmap_set_str") == 0 ||
             strcmp(name, "wvec_push_bytes") == 0 ||
             strcmp(name, "wrec_new") == 0 || strcmp(name, "wrec_set") == 0 ||
             strcmp(name, "wrec_set_str") == 0 || strcmp(name, "wrec_set_bytes") == 0 ||
             strcmp(name, "world_compact") == 0) &&
            !env_lookup(ctx, name) && !ck_find_fn(ctx, name)) {
            int is_free = strcmp(name, "world_free") == 0;
            int is_freeze = strcmp(name, "world_freeze") == 0;
            Node *wh = n->args.len > 0 ? n->args.data[0] : NULL;
            EnvEntry *e = (wh && wh->kind == NODE_IDENT) ? ck_env_find(ctx, wh->name) : NULL;
            if (e && e->is_world && !e->dropped) {
                if (is_free) {
                    if (ctx->loop_depth && e->scope_depth <= ctx->loop_depth)
                        check_error(ctx, n->pos,
                            "world_free на външен за цикъла свят '%s' — втората итерация би била use-after-free",
                            wh->name);
                    else
                        /* W2.2: умират и алиасите, и заеманите от региона
                         * низове (wvec_get_str) — иначе `print(s)` след free е
                         * heap-use-after-free, който минаваше проверката */
                        mem3_free_world(ctx, e, n->pos);
                } else if (is_freeze) {
                    if (e->world_frozen)
                        check_error(ctx, n->pos, "повторен world_freeze на '%s'", wh->name);
                    else
                        world_note_frozen(ctx, e);
                } else if (e->world_frozen) {
                    check_error(ctx, n->pos,
                        "mutate-after-freeze: %s върху замразен свят '%s'", name, wh->name);
                }
            }
        }
        for (int bi = 0; bi < (int)(sizeof(builtins) / sizeof(builtins[0])); bi++) {
            if (strcmp(name, builtins[bi].name) == 0) {
                n->callee->type = type_new(TYPE_VOID);
                /* LP3: arity + типови подпис — преди това грешен тип
                 * аргумент стигаше до C и гърмеше runtime (segfault). */
                const char *sig = builtins[bi].sig;
                int want = (int)strlen(sig);
                if (n->args.len != want) {
                    check_error(ctx, n->pos,
                        "'%s' очаква %d аргумент(а), получих %d",
                        name, want, n->args.len);
                    return type_new(TYPE_ERROR);
                }
                for (int ai = 0; ai < want; ai++) {
                    char wk = sig[ai];
                    if (wk == 'a') continue;
                    Type *at = n->args.data[ai]->type;
                    if (!at || at->kind == TYPE_ERROR) continue;
                    int ok = 0;
                    if (wk == 's')      ok = at->kind == TYPE_STR;
                    else if (wk == 'i') ok = at->kind == TYPE_I64 || at->kind == TYPE_BOOL;
                    else if (wk == 'f') ok = at->kind == TYPE_F64;
                    else if (wk == 'b') ok = at->kind == TYPE_BYTES;
                    if (!ok) {
                        /* Кешът е scratch: i64/str/bytes. Агрегат/контейнер
                         * принадлежи на региона — съветът е wrec + id. */
                        if (strcmp(name, "wcache_set") == 0 && ai == 2 &&
                            (at->kind == TYPE_STRUCT || at->kind == TYPE_ENUM ||
                             at->kind == TYPE_VEC || at->kind == TYPE_MAP))
                            check_error(ctx, n->pos,
                                "'wcache_set' пази i64/str/bytes (scratch стойности); "
                                "агрегат/контейнер сложи в wrec в света и кеширай id-то му");
                        else
                            check_error(ctx, n->pos,
                                "'%s': аргумент #%d е от тип %s, но параметърът е %s",
                                name, ai + 1, type_str(at),
                                wk == 's' ? "str" : wk == 'i' ? "i64" :
                                wk == 'f' ? "f64" : "bytes");
                        return type_new(TYPE_ERROR);
                    }
                }
                Type *ret = type_new(builtins[bi].ret);
                if (builtins[bi].has_io) {
                    type_add_effect(ret, "IO");
                    if (ctx->cur_effects) type_add_effect(ctx->cur_effects, "IO");
                }
                if (builtins[bi].has_par) {
                    type_add_effect(ret, "Par");
                    if (ctx->cur_effects) type_add_effect(ctx->cur_effects, "Par");
                }
                return ret;
            }
        }

        /* sorted(v) — relational annotation predicate (verifier-only) */
        if (strcmp(name, "sorted") == 0) {
            n->callee->type = type_new(TYPE_VOID);
            Type *vt = n->args.len > 0 ? n->args.data[0]->type : NULL;
            if (!vt || vt->kind != TYPE_VEC)
                check_error(ctx, n->pos, "'sorted' очаква вектор");
            return type_new(TYPE_BOOL);
        }

        /* W2.1/W2.7: ghost предикати за ДОГОВОР за свят в requires (като
         * sorted): verifier-only, без runtime проверка и без codegen.
         *   world_frozen(p)  — заемане за четене: p е замразен, write/free/
         *                      препредаване без договор са нарушения;
         *   world_mutable(p) — изключителен запис по време на повикването,
         *                      собствеността остава у извикващия (freeze/free/
         *                      handoff/link са нарушения на договора);
         *   world_owned(p)   — прехвърляне на собствеността: callee прави
         *                      каквото иска, извикващият губи handle-а. */
        if (strcmp(name, "world_frozen") == 0 || strcmp(name, "world_mutable") == 0 ||
            strcmp(name, "world_owned") == 0) {
            n->callee->type = type_new(TYPE_VOID);
            if (n->args.len != 1)
                check_error(ctx, n->pos, "'%s' очаква 1 аргумент, получих %d", name, n->args.len);
            /* W2.8: `world_frozen(c[*])` / `world_owned(c[*])` са КАНАЛНИ
             * аксиоми (rely–guarantee за светове). `world_mutable` през канал
             * няма смисъл: договорът е „запис по време на повикването", а
             * предаването през канал не е синхронно — изпращачът не знае кога
             * приемникът ще го получи. */
            if (n->args.len == 1 && n->args.data[0]->kind == NODE_ELEM_REF &&
                strcmp(name, "world_mutable") == 0)
                check_error(ctx, n->pos,
                    "'world_mutable' не важи за канал (c[*]) — предаването не е синхронно; "
                    "използвай world_frozen (четене) или world_owned (прехвърляне)");
            /* Договорът се разпознава само върху ГОЛО име на параметър или върху
             * канален елемент (c[*]). Всеки друг аргумент — например
             * `world_frozen(cell2_0(ctx))` — минаваше тихо: spec-ът изглеждаше
             * като договор, а верификаторът не поемаше нищо (нито го разтоварваше
             * при извикване). Чиста грешка, не мълчание. */
            if (n->args.len == 1 && n->args.data[0]->kind != NODE_IDENT &&
                n->args.data[0]->kind != NODE_ELEM_REF)
                check_error(ctx, n->pos,
                    "'%s' важи само върху име на параметър или канален елемент (c[*]) — "
                    "друг израз не се регистрира като договор за свят", name);
            Type *wt = n->args.len > 0 ? n->args.data[0]->type : NULL;
            if (!wt || (wt->kind != TYPE_I64 && wt->kind != TYPE_ERROR))
                check_error(ctx, n->pos,
                    "'%s' очаква i64 handle на свят, получих %s", name,
                    wt ? type_str(wt) : "?");
            return type_new(TYPE_BOOL);
        }

        /* L5: локална, която не е fn стойност — по-ясна диагностика */
        Type *lt_local = env_lookup(ctx, name);
        if (lt_local) {
            check_error(ctx, n->pos, "извикване на не-функция '%s' (%s)",
                        name, type_str(lt_local));
        } else {
            /* Подсказка „имахте предвид …?“. Специалните builtin-и по-горе
             * (vec_*, map_*, go, …) не са в таблицата builtins[] — при нов
             * такъв builtin го добави и тук, иначе просто няма подсказка. */
            static const char *const special[] = {
                "print", "println", "write", "sorted", "drop", "world_frozen",
                "world_mutable", "world_owned",
                "vec_new", "vec_push", "vec_set", "vec_push_str", "vec_set_str",
                "vec_get", "vec_get_str", "vec_len", "vec_slice", "vec_concat",
                "vec_from_bytes", "bytes_from_vec",
                "map_new", "map_set", "map_get", "map_has", "map_del",
                "map_len", "map_keys", "map_h", "h_map", "bytes_h", "h_bytes",
                "go", "go_bg", "pool_map",
                "wcache_set", "wcache_get", "wcache_add", "wcache_has", "wcache_del",
                "wcache_clear", "wcache_len",
                "wcache_set_str", "wcache_get_str",
                "wcache_set_bytes", "wcache_get_bytes",
                "world_link", "world_unlink", "world_links", "world_linked",
                "wmap_new", "wmap_set", "wmap_get", "wmap_has", "wmap_del",
                "wmap_len", "wmap_set_str", "wmap_get_str",
                "wvec_push_bytes", "wvec_get_bytes",
                "world_compact", "world_dump_compact",
                "wrec_new", "wrec_len", "wrec_has", "wrec_set",
                "wrec_set_str", "wrec_set_bytes", "wrec_get",
                "wrec_get_str", "wrec_get_bytes",
            };
            int nb = (int)(sizeof builtins / sizeof builtins[0]);
            int ns = (int)(sizeof special / sizeof special[0]);
            const char *fcands[ctx->n_fns + nb + ns];
            int nfc = 0;
            for (int fi = 0; fi < ctx->n_fns; fi++) fcands[nfc++] = ctx->fns[fi].name;
            for (int bi = 0; bi < nb; bi++) fcands[nfc++] = builtins[bi].name;
            for (int si = 0; si < ns; si++) fcands[nfc++] = special[si];
            const char *fsugg = ck_suggest(name, fcands, nfc);
            if (fsugg)
                check_error(ctx, n->pos, "непозната функция '%s' — имахте предвид '%s'?",
                            name, fsugg);
            else
                check_error(ctx, n->pos, "непозната функция '%s'", name);
        }
        return type_new(TYPE_ERROR);
    }

    infer(ctx, n->callee);
    return type_new(TYPE_ERROR);
}

