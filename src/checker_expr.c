/* checker_expr.c — част от checker.c (виж ck_proto.h). */
#include "baga.h"
#include "checker_impl.h"

Type *infer(CheckCtx *ctx, Node *n) {
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

        case NODE_PATH: {
            /* A1: Enum::Variant bare (payload-less) or error if payload required */
            const char *en = n->path_enum;
            const char *vn = n->path_variant;
            int vhit = -1;
            for (int vi = 0; vi < ctx->n_variants; vi++)
                if (strcmp(ctx->variants[vi].enum_name, en) == 0 &&
                    strcmp(ctx->variants[vi].variant, vn) == 0)
                    { vhit = vi; break; }
            if (vhit < 0) {
                check_error(ctx, n->pos, "няма вариант '%s::%s'", en, vn);
                t = type_new(TYPE_ERROR);
                break;
            }
            if (ctx->variants[vhit].payload) {
                check_error(ctx, n->pos,
                    "конструкторът '%s::%s' изисква 1 аргумент (%s)",
                    en, vn, type_str(ctx->variants[vhit].payload));
                t = type_new(TYPE_ERROR);
                break;
            }
            Node *ed = NULL;
            for (int ei = 0; ei < ctx->n_enums; ei++)
                if (strcmp(ctx->enums[ei].name, en) == 0)
                    { ed = ctx->enums[ei].decl; break; }
            if (ed && ed->type && ed->type->kind == TYPE_ENUM) {
                t = type_new(TYPE_ENUM);
                t->name = strdup(en);
            } else {
                t = type_new(TYPE_I64);
            }
            break;
        }

        case NODE_IDENT: {
            /* check local env first */
            Type *vt = env_lookup(ctx, n->name);
            if (vt) {
                /* MEM-2: use-after-drop (error recovery — продължаваме проверката) */
                EnvEntry *e = ck_env_find(ctx, n->name);
                if (e && e->dropped)
                    check_error(ctx, n->pos,
                        "използване на '%s' след free", n->name);
                t = vt; break;
            }
            /* check function registry */
            Type *ft = ck_find_fn(ctx, n->name);
            if (ft) {
                /* M21: generic fn като стойност — не се поддържа (v1):
                 * мономорфизацията няма един wrapper-адрес */
                for (int gi = 0; gi < ctx->n_fns; gi++)
                    if (ctx->fns[gi].fn_type == ft &&
                        ctx->fns[gi].decl->n_type_params > 0)
                        check_error(ctx, n->pos,
                            "generic fn '%s' не може да се ползва като стойност (v1) — обвий я в конкретна функция",
                            n->name);
                /* L5: fn като стойност — codegen-ът взема адреса на wrapper-а
                 * по ПЪЛНОТО (евент. преименувано) име; при дубликати от
                 * няколко модула печели собственият модул, иначе грешка */
                int first = -1, ownidx = -1;
                const char *o1 = NULL, *o2 = NULL;
                const char *own = mod_base(n->pos.file);
                for (int i = 0; i < ctx->n_fns; i++) {
                    if (strcmp(ctx->fns[i].name, n->name) != 0) continue;
                    if (first < 0) first = i;
                    if (strcmp(ctx->fns[i].origin, own) == 0) ownidx = i;
                    if (!o1) o1 = ctx->fns[i].origin;
                    else if (!o2 && strcmp(o1, ctx->fns[i].origin) != 0) o2 = ctx->fns[i].origin;
                }
                int chosen = ownidx >= 0 ? ownidx : first;
                if (o2 && ownidx < 0) {
                    check_error(ctx, n->pos,
                        "нееднозначна fn референция '%s' — уточни с %s.%s или %s.%s",
                        n->name, o1, n->name, o2, n->name);
                }
                Type *vt2 = type_fn(ft->ret, ft->params, ft->nparams);
                vt2->name = ctx->fns[chosen].decl->fn_name;
                n->name = strdup(ctx->fns[chosen].decl->fn_name);  /* codegen: strcmp с type->name */
                t = vt2;
                break;
            }
            /* bare enum variants (A1: unique among sum payload-less / plain) */
            {
                int nhit = 0, hit0 = -1;
                for (int vi = 0; vi < ctx->n_variants; vi++) {
                    if (strcmp(ctx->variants[vi].variant, n->name) != 0) continue;
                    nhit++;
                    if (hit0 < 0) hit0 = vi;
                }
                if (nhit > 1) {
                    /* prefer reporting only sum-enum collisions for bare form */
                    int sum_hits = 0, sum0 = -1;
                    for (int vi = 0; vi < ctx->n_variants; vi++) {
                        if (strcmp(ctx->variants[vi].variant, n->name) != 0) continue;
                        Node *ed = NULL;
                        for (int ei = 0; ei < ctx->n_enums; ei++)
                            if (strcmp(ctx->enums[ei].name, ctx->variants[vi].enum_name) == 0)
                                { ed = ctx->enums[ei].decl; break; }
                        if (ed && ed->type && ed->type->kind == TYPE_ENUM) {
                            sum_hits++;
                            if (sum0 < 0) sum0 = vi;
                        }
                    }
                    if (sum_hits > 1) {
                        check_error(ctx, n->pos,
                            "вариантът '%s' е нееднозначен — ползвай Enum::%s "
                            "(напр. %s::%s)",
                            n->name, n->name,
                            ctx->variants[sum0].enum_name, n->name);
                        t = type_new(TYPE_ERROR);
                        break;
                    }
                    if (sum_hits == 1) hit0 = sum0, nhit = 1;
                }
                if (nhit >= 1 && hit0 >= 0) {
                    if (ctx->variants[hit0].payload) {
                        check_error(ctx, n->pos,
                            "конструкторът '%s' изисква 1 аргумент (%s)",
                            n->name, type_str(ctx->variants[hit0].payload));
                        t = type_new(TYPE_ERROR);
                    } else {
                        Node *ed = NULL;
                        for (int ei = 0; ei < ctx->n_enums; ei++)
                            if (strcmp(ctx->enums[ei].name, ctx->variants[hit0].enum_name) == 0)
                                { ed = ctx->enums[ei].decl; break; }
                        if (ed && ed->type && ed->type->kind == TYPE_ENUM) {
                            t = type_new(TYPE_ENUM);
                            t->name = strdup(ctx->variants[hit0].enum_name);
                        } else {
                            t = type_new(TYPE_I64);
                        }
                    }
                    break;
                }
            }
            if (t) break;
            /* builtins */
            if (strcmp(n->name, "print") == 0 || strcmp(n->name, "println") == 0) {
                t = type_new(TYPE_VOID);
                break;
            }
            int ncand = 0;
            for (int si = 0; si <= ctx->depth; si++)
                ncand += ctx->scopes[si].count;
            const char *vcands[ncand > 0 ? ncand : 1];
            int nvc = 0;
            for (int si = ctx->depth; si >= 0; si--)
                for (int ei = 0; ei < ctx->scopes[si].count; ei++)
                    vcands[nvc++] = ctx->scopes[si].entries[ei].name;
            const char *vsugg = ck_suggest(n->name, vcands, nvc);
            if (vsugg)
                check_error(ctx, n->pos, "недефинирана променлива '%s' — имахте предвид '%s'?",
                            n->name, vsugg);
            else
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
            /* MEM-2: drop-join — drop-нато в then се брои само ако else
             * (или отсъствието му) също го гарантира. */
            int log_base = ctx->n_drop_log;
            Type *tt = infer(ctx, n->then_br);
            int then_end = ctx->n_drop_log;
            /* else-клонът не трябва да вижда drop-овете от then: временно ги вдигни */
            drop_lift_from(ctx, log_base);
            Type *et = n->else_br ? infer(ctx, n->else_br) : type_new(TYPE_VOID);
            if (n->else_br) {
                drop_join2(ctx, log_base, then_end);
            } else {
                /* без else нищо не е сигурно drop-нато — вече вдигнахме
                 * флаговете; нелогнатите (overflow) изобщо не са маркирани */
                ctx->n_drop_log = log_base;
                ctx->drop_log_overflowed = 0;
            }
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
            /* `x[i]` е нисконивов примитив — емитира суров C subscript
             * (codegen_c). Елементният тип не се следи: масивните типове
             * се map-ват към Vec, а Vec/bytes/str се индексират през
             * vec_get/bytes_get/str_char_at. */
            t = type_new(TYPE_I64);
            break;

        case NODE_ELEM_REF:
            /* v[*] — the element value (i64 for Vec<i64>/[i64]); annotation-only */
            infer(ctx, n->elem_obj);
            t = type_new(TYPE_I64);
            break;

        case NODE_FIELD: {
            /* L5/L6: модул.функция като СТОЯНОСТ (без извикване) — когато
             * лявата част не е локална променлива, а име на модул */
            if (n->field_obj->kind == NODE_IDENT &&
                !env_lookup(ctx, n->field_obj->name)) {
                const char *mod = n->field_obj->name;
                for (int i = 0; i < ctx->n_fns; i++) {
                    if (strcmp(ctx->fns[i].origin, mod) == 0 &&
                        strcmp(ctx->fns[i].name, n->field_name) == 0) {
                        Type *ft = ctx->fns[i].fn_type;
                        Type *vt2 = type_fn(ft->ret, ft->params, ft->nparams);
                        vt2->name = ctx->fns[i].decl->fn_name;
                        t = vt2;
                        break;
                    }
                }
                if (t) break;
            }
            Type *ot = infer(ctx, n->field_obj);
            /* resolve field type from struct definition */
            if (ot && ot->kind == TYPE_STRUCT && ot->name) {
                for (int si = 0; si < ctx->n_structs; si++) {
                    if (strcmp(ctx->structs[si].name, ot->name) == 0) {
                        Node *sdecl = ctx->structs[si].decl;
                        for (int fi = 0; fi < sdecl->fields.len; fi++) {
                            Node *fld = sdecl->fields.data[fi];
                            if (strcmp(fld->fld_name, n->field_name) == 0) {
                                /* M24: полетата на generic struct се резолват
                                 * под substitution (параметри → targs) */
                                char **sgn = NULL; Type **sgt = NULL; int sgnc = 0;
                                subst_save(ctx, &sgn, &sgt, &sgnc);
                                ctx->g_names.len = 0;
                                ctx->g_types.len = 0;
                                if (ot->n_targs > 0 && sdecl->n_struct_params > 0) {
                                    int nn = ot->n_targs < sdecl->n_struct_params
                                        ? ot->n_targs : sdecl->n_struct_params;
                                    for (int a = 0; a < nn; a++) {
                                        vec_push(ctx->g_names, sdecl->struct_params[a]);
                                        vec_push(ctx->g_types, ot->targs[a]);
                                    }
                                }
                                t = resolve_type_node(ctx, fld->fld_type);
                                subst_restore(ctx, sgn, sgt, sgnc);
                                break;
                            }
                        }
                        break;
                    }
                }
            }
            if (!t) {
                const char *fsugg = NULL;
                if (ot && ot->kind == TYPE_STRUCT && ot->name) {
                    for (int si = 0; si < ctx->n_structs; si++) {
                        if (strcmp(ctx->structs[si].name, ot->name) != 0) continue;
                        Node *sd = ctx->structs[si].decl;
                        int nf = sd->fields.len;
                        const char *fcands[nf > 0 ? nf : 1];
                        for (int fi = 0; fi < nf; fi++)
                            fcands[fi] = sd->fields.data[fi]->fld_name;
                        fsugg = ck_suggest(n->field_name, fcands, nf);
                        break;
                    }
                }
                if (fsugg)
                    check_error(ctx, n->pos, "непознато поле '.%s' — имахте предвид '.%s'?",
                                n->field_name, fsugg);
                else
                    check_error(ctx, n->pos, "непознато поле '.%s'", n->field_name);
                t = type_new(TYPE_ERROR);
            }
            break;
        }

        case NODE_ASSIGN: {
            infer(ctx, n->assign_target);
            t = NULL;
            /* M25: generic fn като стойност при assign — анотацията е типът
             * на целта (`let mut f: fn(i64)->i64 = …; f = id`) */
            if (n->assign_target->kind == NODE_IDENT &&
                n->assign_val && n->assign_val->kind == NODE_IDENT) {
                Type *tt = env_lookup(ctx, n->assign_target->name);
                if (tt && tt->kind == TYPE_FN &&
                    generic_fn_value_bind(ctx, n->assign_val, tt))
                    t = n->assign_val->type;
            }
            if (!t) t = infer(ctx, n->assign_val);
            if (n->assign_target->kind == NODE_IDENT &&
                !env_is_mut(ctx, n->assign_target->name)) {
                check_error(ctx, n->pos,
                    "променливата '%s' е декларирана с 'let' без 'mut' — присвояването е забранено",
                    n->assign_target->name);
            }
            /* MEM-3: преприсвояване — payload region или handle алиас */
            if (n->assign_target->kind == NODE_IDENT) {
                EnvEntry *pe = ck_env_find(ctx, n->assign_target->name);
                if (pe) mem3_bind(ctx, pe, n->assign_val);
            } else if (n->assign_target->kind == NODE_FIELD) {
                Node *base = n->assign_target;
                while (base && base->kind == NODE_FIELD) base = base->field_obj;
                if (base && base->kind == NODE_IDENT) {
                    EnvEntry *pe = ck_env_find(ctx, base->name);
                    int rid = region_of_expr(ctx, n->assign_val);
                    if (pe && rid > 0) pe->region_id = rid;
                }
            }
            break;
        }

        case NODE_RANGE:
            infer(ctx, n->range_lo);
            infer(ctx, n->range_hi);
            t = type_new(TYPE_I64);
            break;

        case NODE_STRUCT_LIT: {
            /* verify struct exists (L6: scoped по модула на литерала) */
            int amb = 0;
            Node *sdecl = find_struct_scoped(ctx, n->lit_name, n->pos.file, &amb);
            if (amb) {
                char m1[128], m2[128];
                struct_amb_hint(ctx, n->lit_name, m1, m2, sizeof m1);
                check_error(ctx, n->pos,
                    "нееднозначен struct '%s' — има го в модулите '%s' и '%s'; уточни с %s.%s или %s.%s",
                    n->lit_name, m1, m2, m1, n->lit_name, m2, n->lit_name);
            }
            else if (!sdecl) {
                check_error(ctx, n->pos, "непознат struct '%s'", n->lit_name);
            }
            /* M24: generic struct — bind параметрите (явни или от полета) */
            int np = sdecl ? sdecl->n_struct_params : 0;
            Type **sbind = calloc((size_t)(np ? np : 1), sizeof(Type *));
            for (int a = 0; a < n->lit_type_args.len && a < np; a++)
                sbind[a] = resolve_type_node(ctx, n->lit_type_args.data[a]);
            if (np > 0 && n->lit_type_args.len == 0) {
                /* извод от полетата */
                for (int i = 0; i < n->n_lit_fields; i++) {
                    Type *vt = infer(ctx, n->lit_values.data[i]);
                    for (int fi = 0; fi < sdecl->fields.len; fi++) {
                        Node *fld = sdecl->fields.data[fi];
                        if (strcmp(fld->fld_name, n->lit_fields[i]) != 0) continue;
                        bind_type_params(ctx, fld->fld_type, vt,
                                         sdecl->struct_params, np, sbind,
                                         n->pos, n->lit_name);
                        break;
                    }
                }
            }
            char **sgn = NULL; Type **sgt = NULL; int sgnc = 0;
            subst_save(ctx, &sgn, &sgt, &sgnc);
            ctx->g_names.len = 0;
            ctx->g_types.len = 0;
            if (np > 0) {
                for (int a = 0; a < np; a++) {
                    if (!sbind[a]) {
                        check_error(ctx, n->pos,
                            "struct '%s': не мога да изведа '%s' — дай изрично: %s<%s>(…)",
                            n->lit_name, sdecl->struct_params[a],
                            n->lit_name, sdecl->struct_params[a]);
                        sbind[a] = type_new(TYPE_ERROR);
                    }
                    vec_push(ctx->g_names, sdecl->struct_params[a]);
                    vec_push(ctx->g_types, sbind[a]);
                }
            }
            /* check each literal field: name exists + type matches */
            for (int i = 0; i < n->n_lit_fields; i++)
                infer(ctx, n->lit_values.data[i]);
            for (int i = 0; i < n->n_lit_fields; i++) {
                Type *vt = n->lit_values.data[i]->type;
                if (!sdecl) continue;
                int fld_found = 0;
                for (int fi = 0; fi < sdecl->fields.len; fi++) {
                    Node *fld = sdecl->fields.data[fi];
                    if (strcmp(fld->fld_name, n->lit_fields[i]) == 0) {
                        fld_found = 1;
                        Type *ft = resolve_type_node(ctx, fld->fld_type);
                        if (ft && vt && !type_eq(ft, vt)) {
                            check_error(ctx, n->pos,
                                "поле '%s' очаква %s, но получава %s",
                                n->lit_fields[i], type_str(ft), type_str(vt));
                        }
                        break;
                    }
                }
                if (!fld_found) {
                    check_error(ctx, n->pos,
                        "struct '%s' няма поле '%s'", n->lit_name, n->lit_fields[i]);
                }
            }
            /* check for missing fields */
            if (sdecl) {
                for (int fi = 0; fi < sdecl->fields.len; fi++) {
                    Node *fld = sdecl->fields.data[fi];
                    int present = 0;
                    for (int i = 0; i < n->n_lit_fields; i++) {
                        if (strcmp(n->lit_fields[i], fld->fld_name) == 0) {
                            present = 1;
                            break;
                        }
                    }
                    if (!present) {
                        check_error(ctx, n->pos,
                            "липсва поле '%s' в struct '%s'", fld->fld_name, n->lit_name);
                    }
                }
            }
            if (sdecl && strcmp(n->lit_name, sdecl->struct_name) != 0) {
                /* L6: codegen чете lit_name — пренаписваме към финалното име */
                free(n->lit_name);
                n->lit_name = strdup(sdecl->struct_name);
            }
            subst_restore(ctx, sgn, sgt, sgnc);
            Type *st = type_new(TYPE_STRUCT);
            st->name = strdup(sdecl ? sdecl->struct_name : n->lit_name);
            if (np > 0) {
                st->targs = malloc(sizeof(Type *) * (size_t)np);
                st->n_targs = np;
                for (int a = 0; a < np; a++) st->targs[a] = sbind[a];
                /* регистрирай инстанцията */
                int found = -1;
                for (int k = 0; k < sdecl->struct_inst_count; k++) {
                    int same = 1;
                    for (int a = 0; a < np; a++)
                        if (!type_eq(sdecl->struct_inst_targs[k * np + a], st->targs[a])) { same = 0; break; }
                    if (same) { found = k; break; }
                }
                if (found < 0) {
                    int idx = sdecl->struct_inst_count;
                    sdecl->struct_inst_targs = realloc(sdecl->struct_inst_targs,
                        sizeof(Type *) * (size_t)((idx + 1) * np));
                    for (int a = 0; a < np; a++)
                        sdecl->struct_inst_targs[idx * np + a] = st->targs[a];
                    sdecl->struct_inst_count = idx + 1;
                }
            }
            free(sbind);
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

        case NODE_RAISE: {
            /* M20: raise !E(payload) — ефектът трябва да е деклариран от
             * обграждащата функция; payload-ът се сверява с декларацията */
            Type *pl = NULL;
            if (n->raise_payload) {
                pl = infer(ctx, n->raise_payload);
                if (pl->kind != TYPE_I64 && pl->kind != TYPE_STR &&
                    pl->kind != TYPE_BYTES && pl->kind != TYPE_BOOL &&
                    pl->kind != TYPE_F64) {
                    check_error(ctx, n->raise_payload->pos,
                        "payload на !%s може да е i64/str/bytes/bool/f64 (v1), получих %s",
                        n->raise_effect, type_str(pl));
                }
            }
            Type *decl = type_effect_payload(ctx->cur_ret, n->raise_effect);
            if (ctx->cur_ret && !type_has_effect(ctx->cur_ret, n->raise_effect)) {
                check_error(ctx, n->pos,
                    "raise !%s — ефектът не е деклариран в return типа на функцията",
                    n->raise_effect);
            }
            if (decl && !pl) {
                check_error(ctx, n->pos,
                    "ефектът !%s носи payload — нужен е raise !%s(payload)",
                    n->raise_effect, n->raise_effect);
            }
            if (!decl && pl) {
                check_error(ctx, n->pos,
                    "ефектът !%s е без payload — raise !%s не приема стойност",
                    n->raise_effect, n->raise_effect);
            }
            if (decl && pl && !type_eq(decl, pl)) {
                check_error(ctx, n->raise_payload->pos,
                    "payload на !%s е %s, но функцията декларира %s",
                    n->raise_effect, type_str(pl), type_str(decl));
            }
            if (ctx->cur_effects) {
                type_add_effect(ctx->cur_effects, n->raise_effect);
                for (int j = 0; j < ctx->cur_effects->n_effects; j++)
                    if (strcmp(ctx->cur_effects->effects[j], n->raise_effect) == 0)
                        ctx->cur_effects->effect_payloads[j] = decl;
            }
            /* raise дивергира — типът не участва в по-нататъшни проверки */
            t = type_new(TYPE_ERROR);
            break;
        }

        case NODE_CATCH: {
            /* e catch !E [(binding)] => handler — remove effect E from e's type */
            /* MEM-2: две алтернативи — try изразът (нормален път) и handler-ът
             * (прихванат ефект); definitely dropped = drop-нато и в двете */
            int log_base = ctx->n_drop_log;
            Type *et = infer(ctx, n->catch_expr);
            int try_end = ctx->n_drop_log;
            /* LP4: catch на ефект, който изразът не носи, минаваше тихо —
             * мъртъв код и прикритие за правописни грешки в името. */
            if (et->kind != TYPE_ERROR) {
                int has = 0;
                for (int i = 0; i < et->n_effects; i++)
                    if (strcmp(et->effects[i], n->catch_effect) == 0) { has = 1; break; }
                if (!has)
                    check_error(ctx, n->pos,
                        "catch !%s — изразът няма такъв ефект (мъртъв catch; правописна грешка?)",
                        n->catch_effect);
            }
            /* M20: payload handling — binding-ът получава payload-а */
            Type *pl = type_effect_payload(et, n->catch_effect);
            if (pl && !n->catch_binding) {
                check_error(ctx, n->pos,
                    "ефектът !%s носи payload — нужен е catch !%s(name)",
                    n->catch_effect, n->catch_effect);
            }
            if (!pl && n->catch_binding) {
                check_error(ctx, n->pos,
                    "ефектът !%s няма payload — излишен binding '%s'",
                    n->catch_effect, n->catch_binding);
            }
            /* handler-ът не трябва да вижда drop-овете от try израза */
            drop_lift_from(ctx, log_base);
            push_scope(ctx);
            if (pl && n->catch_binding) {
                /* M20: payload-ът е с живота на handler-а */
                env_define(ctx, n->catch_binding, pl, n->pos);
            }
            infer(ctx, n->catch_handler);
            pop_scope(ctx);
            drop_join2(ctx, log_base, try_end);
            t = type_new(et->kind);
            /* keep Vec elem / struct name, like a plain call result does */
            t->elem = et->elem;
            t->name = et->name;
            /* copy all effects except the caught one (M20: и payload-ите) */
            for (int i = 0; i < et->n_effects; i++) {
                if (strcmp(et->effects[i], n->catch_effect) != 0) {
                    type_add_effect(t, et->effects[i]);
                    for (int j = 0; j < t->n_effects; j++)
                        if (strcmp(t->effects[j], et->effects[i]) == 0)
                            t->effect_payloads[j] =
                                et->effect_payloads ? et->effect_payloads[i] : NULL;
                }
            }
            /* remove caught effect from function-level accumulator */
            if (ctx->cur_effects)
                type_remove_effect(ctx->cur_effects, n->catch_effect);
            break;
        }

        case NODE_TO_STR: {
            /* interpolation: convert inner expr to str (str/i64/f64/bool) */
            Type *et = infer(ctx, n->to_str_expr);
            if (et->kind != TYPE_STR && et->kind != TYPE_I64 &&
                et->kind != TYPE_I32 && et->kind != TYPE_F64 &&
                et->kind != TYPE_BOOL && et->kind != TYPE_ERROR) {
                check_error(ctx, n->pos, "неподдържан тип за интерполация: %s (str/i64/f64/bool)", type_str(et));
            }
            t = type_new(TYPE_STR);
            type_merge_effects(t, et);
            if (ctx->cur_effects) type_merge_effects(ctx->cur_effects, et);
            break;
        }

        case NODE_LET: {
            /* M25: generic fn като стойност с явна fn анотация —
             * `let f: fn(i64) -> i64 = id` инстанцира и връзва към
             * инстанцията (преди нормалния infer, който иначе греши) */
            Type *annot_t = n->let_type ? resolve_type_node(ctx, n->let_type)
                                        : NULL;
            Type *init_t;
            if (annot_t && annot_t->kind == TYPE_FN && n->let_init &&
                n->let_init->kind == NODE_IDENT &&
                generic_fn_value_bind(ctx, n->let_init, annot_t)) {
                init_t = n->let_init->type;
            } else {
                init_t = n->let_init ? infer(ctx, n->let_init) : type_new(TYPE_I64);
            }
            if (n->let_init && init_t->kind == TYPE_VOID) {
                check_error(ctx, n->pos,
                    "не може да се присвои void стойност на '%s'", n->let_name);
            }
            Type *decl_t = annot_t ? annot_t : init_t;
            if (n->let_type && n->let_init &&
                decl_t->kind != TYPE_ERROR && init_t->kind != TYPE_ERROR &&
                !type_eq(decl_t, init_t)) {
                /* i64 → f64 е позволено (разширяване); f64 → i64 е грешка.
                 * i64 литерал → i32 анотация също е позволено (стесняване с
                 * проверка на диапазона) — литералът няма собствен живот, а
                 * `let x: i32 = 5` е единственият начин да се получи i32
                 * стойност (иначе типът е неизползваем: `i32` не може да се
                 * инициализира от нищо). */
                int int_lit_ok = 0;
                if (decl_t->kind == TYPE_I32 && init_t->kind == TYPE_I64 &&
                    n->let_init && n->let_init->kind == NODE_INT_LIT) {
                    long long v = n->let_init->int_val;
                    int_lit_ok = 1;  /* литералът се стеснява до i32 */
                    if (v < -2147483648LL || v > 2147483647LL)
                        check_error(ctx, n->pos,
                            "i32 литералът '%lld' е извън диапазона (-2147483648..2147483647)",
                            v);
                }
                if (!(decl_t->kind == TYPE_F64 && init_t->kind == TYPE_I64) &&
                    !int_lit_ok) {
                    check_error(ctx, n->pos,
                        "несъвместими типове: '%s' е %s, но инициализаторът е %s",
                        n->let_name, type_str(decl_t), type_str(init_t));
                }
            }
            /* L5: fn стойности — ефектен договор при wrap + без засенчване
             * на глобални функции (пази --verify sound) */
            if (decl_t->kind == TYPE_FN && init_t->kind == TYPE_FN &&
                !fn_effects_subset(init_t, decl_t)) {
                check_error(ctx, n->pos,
                    "fn стойността на '%s' има ефекти извън анотацията %s",
                    n->let_name, type_str(decl_t));
            }
            if (decl_t->kind == TYPE_FN && ck_find_fn(ctx, n->let_name)) {
                check_error(ctx, n->pos,
                    "fn стойност '%s' засенчи глобална функция — преименувай я",
                    n->let_name);
            }
            env_define_mut(ctx, n->let_name, decl_t, n->is_mut, n->pos);
            {
                EnvEntry *pe = ck_env_find(ctx, n->let_name);
                if (pe) mem3_bind(ctx, pe, n->let_init);
            }
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
            /* MEM-2: drop на променлива извън цикъла = use-after-drop на итерация 2 */
            int saved_loop = ctx->loop_depth;
            ctx->loop_depth = ctx->depth;
            infer(ctx, n->while_body);
            ctx->loop_depth = saved_loop;
            t = type_new(TYPE_VOID);
            break;
        }

        case NODE_FOR: {
            infer(ctx, n->for_iter);
            push_scope(ctx);
            env_define(ctx, n->for_var, type_new(TYPE_I64), n->pos);
            /* MEM-2: loop_depth е depth-ът на самия цикъл; for-променливата е
             * в прясния scope (scope_depth > loop_depth) — не е "външна" */
            int saved_loop = ctx->loop_depth;
            ctx->loop_depth = ctx->depth - 1;
            infer(ctx, n->for_body);
            ctx->loop_depth = saved_loop;
            pop_scope(ctx);
            t = type_new(TYPE_VOID);
            break;
        }

        case NODE_MATCH: {
            Type *st = infer(ctx, n->match_expr);
            if (st && st->kind == TYPE_ENUM && st->name) {
                /* L3: match върху sum enum — вариантни патерни, bindings, пълнота */
                Node *ed = NULL;
                for (int ei = 0; ei < ctx->n_enums; ei++)
                    if (strcmp(ctx->enums[ei].name, st->name) == 0)
                        { ed = ctx->enums[ei].decl; break; }
                if (!ed) { t = type_new(TYPE_ERROR); break; }
                int nv = ed->n_variants;
                int *covered = calloc((size_t)nv, sizeof(int));
                int has_wild = 0;
                t = type_new(TYPE_VOID);
                /* MEM-2: всеки arm е алтернатива — N-arm drop-join */
                int log_base = ctx->n_drop_log;
                int arm_end[64];
                int narm = n->match_arms.len;
                for (int i = 0; i < n->match_arms.len; i++) {
                    Node *arm = n->match_arms.data[i];
                    /* този arm не трябва да вижда drop-овете от предишните */
                    drop_lift_from(ctx, log_base);
                    if (!arm->arm_pattern) {
                        has_wild = 1;
                    } else if (arm->arm_pattern->kind != NODE_IDENT &&
                               arm->arm_pattern->kind != NODE_PATH) {
                        check_error(ctx, arm->arm_pattern->pos,
                            "match върху '%s': патернът трябва да е вариант или '_'",
                            st->name);
                    } else {
                        const char *vn = arm->arm_pattern->kind == NODE_PATH
                            ? arm->arm_pattern->path_variant
                            : arm->arm_pattern->name;
                        if (arm->arm_pattern->kind == NODE_PATH &&
                            strcmp(arm->arm_pattern->path_enum, st->name) != 0) {
                            check_error(ctx, arm->arm_pattern->pos,
                                "патернът '%s::%s' не е за enum '%s'",
                                arm->arm_pattern->path_enum, vn, st->name);
                        }
                        int vidx = -1;
                        for (int j = 0; j < nv; j++)
                            if (strcmp(ed->enum_variants[j], vn) == 0) { vidx = j; break; }
                        if (vidx < 0) {
                            check_error(ctx, arm->arm_pattern->pos,
                                "патернът '%s' не е вариант на enum '%s'", vn, st->name);
                        } else {
                            covered[vidx] = 1;
                            int has_pl = ed->enum_payloads && ed->enum_payloads[vidx];
                            if (has_pl && !arm->arm_binding)
                                check_error(ctx, arm->arm_pattern->pos,
                                    "вариантът '%s' има payload — нужен е binding: %s(v)",
                                    vn, vn);
                            if (!has_pl && arm->arm_binding)
                                check_error(ctx, arm->arm_pattern->pos,
                                    "вариантът '%s' няма payload", vn);
                        }
                    }
                    push_scope(ctx);
                    if (arm->arm_binding && arm->arm_pattern &&
                        (arm->arm_pattern->kind == NODE_IDENT ||
                         arm->arm_pattern->kind == NODE_PATH)) {
                        const char *pvn = arm->arm_pattern->kind == NODE_PATH
                            ? arm->arm_pattern->path_variant
                            : arm->arm_pattern->name;
                        Type *pt = type_new(TYPE_ERROR);
                        for (int j = 0; j < nv; j++)
                            if (strcmp(ed->enum_variants[j], pvn) == 0 &&
                                ed->enum_payloads && ed->enum_payloads[j])
                                pt = resolve_type_node(ctx, ed->enum_payloads[j]);
                        env_define(ctx, arm->arm_binding, pt, arm->pos);
                    }
                    /* синтетичният `return e` в arm тяло е стойност на match,
                     * не return от обграждащата функция — не го проверявай срещу cur_ret */
                    Type *saved_ret = ctx->cur_ret;
                    ctx->cur_ret = NULL;
                    Type *bt = infer(ctx, arm->arm_body);
                    ctx->cur_ret = saved_ret;
                    pop_scope(ctx);
                    if (i < 64) arm_end[i] = ctx->n_drop_log;
                    if (bt->kind == TYPE_VOID && arm->arm_body &&
                        arm->arm_body->kind == NODE_BLOCK &&
                        arm->arm_body->stmts.len > 0) {
                        Node *last = arm->arm_body->stmts.data[arm->arm_body->stmts.len - 1];
                        if (last->kind == NODE_RETURN && last->ret_val && last->ret_val->type)
                            bt = last->ret_val->type;
                    }
                    if (i == 0) t = bt;
                    else if (!type_eq(bt, t))
                        check_error(ctx, arm->pos,
                            "arm #%d на match е от тип %s, но първият arm е %s",
                            i + 1, type_str(bt), type_str(t));
                }
                if (!has_wild)
                    for (int j = 0; j < nv; j++)
                        if (!covered[j])
                            check_error(ctx, n->pos,
                                "match върху '%s' не е пълен — липсва вариант '%s' (или добави '_')",
                                st->name, ed->enum_variants[j]);
                free(covered);
                /* MEM-2: definitely dropped = drop-нато във всеки arm */
                drop_join_arms(ctx, log_base, narm <= 64 ? arm_end : NULL, narm);
                break;
            }
            /* не-enum match: досегашното поведение (първият arm дава типа) */
            t = type_new(TYPE_VOID);
            {
                /* MEM-2: всеки arm е алтернатива — N-arm drop-join */
                int log_base = ctx->n_drop_log;
                int arm_end[64];
                int narm = n->match_arms.len;
                int has_wild = 0;
                for (int i = 0; i < n->match_arms.len; i++) {
                    Node *arm = n->match_arms.data[i];
                    /* този arm не трябва да вижда drop-овете от предишните */
                    drop_lift_from(ctx, log_base);
                    if (arm->arm_pattern) infer(ctx, arm->arm_pattern);
                    else has_wild = 1;  /* `_` — гарантира изпълнение на някой arm */
                    /* infer arm body; extract type from return if wrapped */
                    Type *saved_ret = ctx->cur_ret;
                    ctx->cur_ret = NULL;
                    Type *bt = infer(ctx, arm->arm_body);
                    ctx->cur_ret = saved_ret;
                    if (i < 64) arm_end[i] = ctx->n_drop_log;
                    if (bt->kind == TYPE_VOID && arm->arm_body &&
                        arm->arm_body->kind == NODE_BLOCK &&
                        arm->arm_body->stmts.len > 0) {
                        Node *last = arm->arm_body->stmts.data[arm->arm_body->stmts.len - 1];
                        if (last->kind == NODE_RETURN && last->ret_val && last->ret_val->type)
                            bt = last->ret_val->type;
                    }
                    if (i == 0) t = bt;
                }
                /* MEM-2: не-enum match няма exhaustiveness проверка — без `_`
                 * при runtime може да не се изпълни НИТО ЕДИН arm, затова
                 * intersection-ът е unsound: консервативно вдигане на всичко */
                if (has_wild)
                    drop_join_arms(ctx, log_base, narm <= 64 ? arm_end : NULL, narm);
                else {
                    drop_lift_from(ctx, log_base);
                    ctx->n_drop_log = log_base;
                    ctx->drop_log_overflowed = 0;
                }
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

        case NODE_INVARIANT:
            /* annotation statement (verifier-only): predicates must be bool;
             * c[*] elem refs are allowed here (annotation context) */
            for (int i = 0; i < n->inv_exprs.len; i++) infer(ctx, n->inv_exprs.data[i]);
            t = type_new(TYPE_VOID);
            break;

        case NODE_LAMBDA: {
            /* fn [caps] (params) -> ret { body } — L5. Captures са по
             * стойност и идват от обкръжението; ефектите на тялото се
             * сливат върху ТИПА на ламбдата (създаването ѝ е чисто). */
            if (!n->fn_name) {
                char lbuf[40];
                snprintf(lbuf, sizeof lbuf, "__lam_%d", ctx->n_lambdas++);
                n->fn_name = strdup(lbuf);
            }
            for (int i = 0; i < n->captures.len; i++) {
                Node *cap = n->captures.data[i];
                Type *ct2 = env_lookup(ctx, cap->param_name);
                if (!ct2) {
                    check_error(ctx, cap->pos,
                        "лямбда capture '%s' не е дефиниран в обкръжението", cap->param_name);
                    ct2 = type_new(TYPE_ERROR);
                } else {
                    /* MEM-2: заснетата външна променлива не може да се drop-ва
                     * (лямбдата държи указател към същия буфер) */
                    EnvEntry *ce = ck_env_find(ctx, cap->param_name);
                    if (ce) ce->captured = 1;
                }
                cap->type = ct2;
            }
            int np = n->params.len;
            Type **params = NULL;
            if (np > 0) {
                params = malloc(sizeof(Type *) * (size_t)np);
                for (int j = 0; j < np; j++) {
                    params[j] = resolve_type_node(ctx, n->params.data[j]->param_type);
                    n->params.data[j]->type = params[j];
                }
            }
            Type *ret = n->ret_type ? resolve_type_node(ctx, n->ret_type)
                                    : type_new(TYPE_VOID);
            Type *saved_ret = ctx->cur_ret;
            Type *saved_eff = ctx->cur_effects;
            const char *saved_fn = ctx->cur_fn;
            ctx->cur_ret = ret;
            ctx->cur_effects = type_new(TYPE_VOID);
            ctx->cur_fn = n->fn_name;
            push_scope(ctx);
            for (int i = 0; i < n->captures.len; i++) {
                env_define(ctx, n->captures.data[i]->param_name,
                           n->captures.data[i]->type, n->captures.data[i]->pos);
                /* MEM-2: и вътре в тялото capture-ът не може да се drop-ва */
                EnvEntry *ie = ck_env_find(ctx, n->captures.data[i]->param_name);
                if (ie) ie->captured = 1;
            }
            for (int j = 0; j < np; j++) {
                if (params[j]->kind == TYPE_FN && ck_find_fn(ctx, n->params.data[j]->param_name))
                    check_error(ctx, n->params.data[j]->pos,
                        "fn параметър '%s' засенчи глобална функция — преименувай го",
                        n->params.data[j]->param_name);
                env_define(ctx, n->params.data[j]->param_name, params[j],
                           n->params.data[j]->pos);
                /* MEM-2: параметрите споделят буфера на извикващия */
                EnvEntry *pe = ck_env_find(ctx, n->params.data[j]->param_name);
                if (pe) pe->is_param = 1;
            }
            if (n->fn_body) {
                for (int i = 0; i < n->fn_body->stmts.len; i++)
                    infer(ctx, n->fn_body->stmts.data[i]);
            }
            if (ctx->cur_effects)
                type_merge_effects(ret, ctx->cur_effects);
            pop_scope(ctx);
            ctx->cur_ret = saved_ret;
            ctx->cur_effects = saved_eff;
            ctx->cur_fn = saved_fn;
            t = type_fn(ret, params, np);
            break;
        }

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

