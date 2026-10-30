/* codegen_stmt.c — част от codegen_c.c (виж cg_proto.h). */
#include "baga.h"
#include "codegen_impl.h"

void emit_stmt(Codegen *cg, Node *n);

/* RC1: return със scope release. move при `return x` (локален), retain при
 * `return p` (параметър), release на всички при `return <израз>` — изразът
 * се оценява ПРЕДИ release-ите, защото може да ползва локалите. */
void emit_return_val(Codegen *cg, Node *val) {
    FILE *f = cg->out;
    if (!cg->rc) {
        emit_indent(cg);
        fprintf(f, "return ");
        emit_expr(cg, val);
        fprintf(f, ";\n");
        return;
    }
    emit_indent(cg);
    if (val->kind == NODE_IDENT) {
        int idx = rc_find(cg, val->name);
        if (idx >= 0 && !cg->rc_locals.data[idx].is_param &&
            !cg->rc_locals.data[idx].dead) {
            /* move: bindingът излиза като собственост на caller-а */
            rc_release_all(cg, idx);
            emit_indent(cg);
            fprintf(f, "return ");
            emit_expr(cg, val);
            fprintf(f, ";\n");
            return;
        }
        if (idx >= 0 && cg->rc_locals.data[idx].is_param &&
            !cg->rc_locals.data[idx].dead) {
            /* заеманият параметър става собственост на caller-а */
            char *mm = mangle_name(val->name);
            rc_emit_retain_val(cg, cg->rc_locals.data[idx].tag,
                               cg->rc_locals.data[idx].type,
                               cg->rc_locals.data[idx].type_node, mm);
            fprintf(f, "\n");
            free(mm);
            emit_indent(cg);
            rc_release_all(cg, -1);
            emit_indent(cg);
            fprintf(f, "return ");
            emit_expr(cg, val);
            fprintf(f, ";\n");
            return;
        }
        if (idx >= 0 || !rc_need_owned_retain(cg, val)) {
            /* dead локал / ident без heap: emit_expr пази lowering-а
             * (sum enum без payload и т.н.) */
            rc_release_all(cg, -1);
            emit_indent(cg);
            fprintf(f, "return ");
            emit_expr(cg, val);
            fprintf(f, ";\n");
            return;
        }
        /* untrack-нат ident с heap (match binding / let = vec_get, или
         * bare variant като `return GBad`) — през __rc_ret, защото
         * emit_expr може да свали варианта до `(E){ .tag = N }` и
         * няма C локал с това име */
    }
    fprintf(f, "{\n");
    cg->indent++;
    emit_indent(cg);
    fprintf(f, "__auto_type __rc_ret = ");
    emit_expr(cg, val);
    fprintf(f, ";\n");
    /* borrowed / if-израз / untracked: caller-ът получава owned
     * (конвенцията „fn резултат = owned") → retain. RC5 v1.0a:
     * rc_heap_tag покрива и struct/enum (tag 5/6), не само str/bytes/Vec.
     * Свеж литерал/ctor и не-borrowed call не се пипат — вече owned. */
    if (rc_need_owned_retain(cg, val)) {
        int rtag = rc_heap_tag(cg, val->type);
        emit_indent(cg);
        rc_emit_retain_val(cg, rtag, val->type, NULL, "__rc_ret");
        fprintf(f, "\n");
    }
    /* RC4: release на temp-овете от return израза — тук, преди самия return
     * (statement-ът приключва с return; rc_tmp_end след това е no-op) */
    rc_tmp_release_all(cg);
    rc_release_all(cg, -1);
    emit_indent(cg);
    fprintf(f, "return __rc_ret;\n");
    cg->indent--;
    emit_indent(cg);
    fprintf(f, "}\n");
}

static void emit_block_scoped(Codegen *cg, Node *block, int is_loop) {
    FILE *f = cg->out;
    fprintf(f, "{\n");
    cg->indent++;
    rc_push_scope(cg, is_loop);
    for (int i = 0; i < block->stmts.len; i++) {
        /* RC2.1: контекст на текущия statement за borrowed-pair анализа
         * (сканира се опашката на този block след let-а) */
        Node *saved_blk = cg->rc_cur_blk;
        int saved_idx = cg->rc_cur_idx;
        cg->rc_cur_blk = block; cg->rc_cur_idx = i;
        emit_stmt(cg, block->stmts.data[i]);
        cg->rc_cur_blk = saved_blk; cg->rc_cur_idx = saved_idx;
    }
    /* RC1: release на heap локалите на scope-а преди затварящата скоба;
     * без --rc rc_pop_scope не emit-ва нищо */
    rc_pop_scope(cg);
    cg->indent--;
    emit_indent(cg);
    fprintf(f, "}");
}

static void emit_block(Codegen *cg, Node *block) {
    emit_block_scoped(cg, block, 0);
}

void emit_stmt(Codegen *cg, Node *n) {
    FILE *f = cg->out;
    if (!n) return;

    switch (n->kind) {
        case NODE_LET: {
            /* RC4: temp-ове в init (root-ът е bound — не е temp) */
            RcTmpVec saved_tmps = {0};
            int saved_on = 0;
            if (cg->rc)
                rc_tmp_begin(cg, n->let_init, 1, &saved_tmps, &saved_on);
            emit_indent(cg);
            /* use inferred type from checker, fall back to explicit annotation */
            if (n->let_type) {
                emit_type(cg, n->let_type);
            } else if (n->let_init && n->let_init->type) {
                Type *it = n->let_init->type;
                switch (it->kind) {
                    case TYPE_F64:  fprintf(f, "double"); break;
                    case TYPE_STR:  fprintf(f, "const char *"); break;
                    case TYPE_BOOL: fprintf(f, "int"); break;
                    case TYPE_I32:  fprintf(f, "int32_t"); break;
                    case TYPE_STRUCT:
                        if (it->name) {
                            /* M24: instantiated generic struct */
                            if (it->n_targs > 0) {
                                char *cn = struct_cname_str(it);
                                fprintf(f, "%s", cn);
                                free(cn);
                            } else {
                                char *sm = mangle_name(it->name);
                                fprintf(f, "%s", sm);
                                free(sm);
                            }
                        }
                        else fprintf(f, "int64_t");
                        break;
                    case TYPE_ENUM:
                        if (it->name) { char *em = mangle_name(it->name); fprintf(f, "%s", em); free(em); }
                        else fprintf(f, "int64_t");
                        break;
                    case TYPE_VEC:
                        fprintf(f, "baga_Vec *");
                        break;
                    case TYPE_MAP:
                        fprintf(f, "baga_Map *");
                        break;
                    case TYPE_BYTES:
                        fprintf(f, "baga_bytes");
                        break;
                    default:        fprintf(f, "int64_t"); break;
                }
            } else {
                fprintf(f, "int64_t");
            }
            fprintf(f, " ");
            {
                char *m = mangle_name(n->let_name);
                fprintf(f, "%s", m);
                free(m);
            }
            if (n->let_init) {
                fprintf(f, " = ");
                emit_expr(cg, n->let_init);
            }
            fprintf(f, ";\n");
            /* RC1: регистрирай heap локала в scope стека; retain при alias —
             * от track-нат ident (двама собственици) или от borrowed израз
             * (vec_get/map_get/поле/h_* — референцията става собствена). */
            if (cg->rc && n->let_name) {
                Type *lt = n->let_init ? n->let_init->type : NULL;
                int tag = rc_heap_tag(cg, lt);
                if (tag == 0 && n->let_type)
                    tag = rc_heap_tag_node(cg, n->let_type);
                if (tag == 5) {
                    int fresh = n->let_init && n->let_init->kind == NODE_STRUCT_LIT;
                    int from_tr = 0;
                    if (n->let_init && n->let_init->kind == NODE_IDENT) {
                        int si = rc_find(cg, n->let_init->name);
                        if (si >= 0 && !cg->rc_locals.data[si].dead)
                            from_tr = 1;
                    }
                    /* RC7: fn резултатът се регистрира (балансът идва от
                     * retain-овете при вграждане/извличане — RC1/RC5).
                     * Предишното „leak-safe" правило (MEM-4) оставяше
                     * ВСИЧКИ fn-върнати struct-ове да текат — в сървър това
                     * е десетки kB на заявка. Код, който споделя една
                     * собственост на две места БЕЗ retain (като pgwire
                     * MEM-4 арената), не трябва да се билдва с --rc. */
                    (void)fresh; (void)from_tr;
                } else if (tag == 6) {
                    /* RC5 v0.6: само свеж ctor (`Ok(x)`) или alias на track-нат
                     * RC7: като tag 5 — fn резултатът също се регистрира. */
                    int fresh = rc_is_enum_ctor(cg, n->let_init);
                    int from_tr = 0;
                    if (n->let_init && n->let_init->kind == NODE_IDENT) {
                        int si = rc_find(cg, n->let_init->name);
                        if (si >= 0 && !cg->rc_locals.data[si].dead)
                            from_tr = 1;
                    }
                    (void)fresh; (void)from_tr;
                }
                if (tag) {
                    int elide = 0;
                    char *mself = mangle_name(n->let_name);
                    if (n->let_init && n->let_init->kind == NODE_IDENT) {
                        int si = rc_find(cg, n->let_init->name);
                        if (si >= 0 && !cg->rc_locals.data[si].dead) {
                            /* RC2: последна употреба на източника → move
                             * (без retain; източникът умира тук) */
                            if (rc_is_move(cg, n->let_init, si)) {
                                rc_do_move(cg, si);
                            } else {
                                emit_indent(cg);
                                rc_emit_retain_val(cg, cg->rc_locals.data[si].tag,
                                                   cg->rc_locals.data[si].type,
                                                   cg->rc_locals.data[si].type_node,
                                                   cg->rc_locals.data[si].name);
                                fprintf(f, "\n");
                            }
                        }
                    } else if (rc_borrowed_init(n->let_init)) {
                        /* RC5: borrowed struct (vec_get/поле) споделя полета —
                         * трябва retain_S, иначе scope release обесва
                         * контейнера (boila_txn_commit underflow).
                         * v0.6: същото за enum payload (tag 6). */
                        if (tag == 5 || tag == 6) {
                            emit_indent(cg);
                            rc_emit_retain_val(cg, tag, lt, n->let_type, mself);
                            fprintf(f, "\n");
                        } else {
                        /* RC2.1: ако x не escape-ва и източникът не се мутира
                         * в scope-а — нито retain, нито регистрация (двойката
                         * retain+release е чиста загуба) */
                        const char *srcn = rc_borrow_src_name(n->let_init);
                        if (srcn && rc_be_ok(cg, n->let_name, srcn)) {
                            cg->rc_elided_pairs++;
                            elide = 1;
                        } else {
                            emit_indent(cg);
                            if (tag == 2)
                                fprintf(f, "baga_rc_retain((void *)%s.data);\n", mself);
                            else
                                fprintf(f, "baga_rc_retain((void *)%s);\n", mself);
                        }
                        }
                    }
                    free(mself);
                    if (!elide)
                        rc_register_node(cg, n->let_name, tag, lt, n->let_type, 0);
                }
            }
            if (cg->rc) rc_tmp_end(cg, &saved_tmps, saved_on);
            /* RC1.3: `let m = mem_mark()` — watermark за rewind dead-marking */
            if (cg->rc && rc_is_mem_call(n->let_init, "mem_mark"))
                vec_push(cg->rc_marks, cg->rc_locals.len);
            break;
        }

        case NODE_RETURN: {
            /* RC4: temp-ове в return израза (root-ът отива на caller-а);
             * release-ът им е ВЪТРЕ в emit_return_val — преди return-а */
            RcTmpVec saved_tmps = {0};
            int saved_on = 0;
            if (cg->rc) {
                if (n->ret_val) {
                    rc_tmp_begin(cg, n->ret_val, 1, &saved_tmps, &saved_on);
                    emit_return_val(cg, n->ret_val);
                    rc_tmp_end(cg, &saved_tmps, saved_on);
                } else {
                    rc_release_all(cg, -1);
                    emit_indent(cg);
                    fprintf(f, "return;\n");
                }
                break;
            }
            emit_indent(cg);
            if (n->ret_val) {
                fprintf(f, "return ");
                emit_expr(cg, n->ret_val);
                fprintf(f, ";\n");
            } else {
                fprintf(f, "return;\n");
            }
            break;
        }

        case NODE_WHILE:
            emit_indent(cg);
            fprintf(f, "while (");
            emit_rc_stmt_expr(cg, n->while_cond);
            fprintf(f, ") ");
            /* RC1: loop тялото е маркиран scope — release на всяка итерация,
             * break/continue release-ват до тук */
            emit_block_scoped(cg, n->while_body, 1);
            fprintf(f, "\n");
            break;

        case NODE_FOR: {
            /* for x in lo..hi { } → for (int64_t x = lo; x < hi; x++)
             * RC4 v0.3: lo temp-ове се hoist-ват веднъж (init); hi — на
             * всяка итерация през emit_rc_stmt_expr (C for cond се преоценява). */
            Node *lo = NULL, *hi = NULL;
            if (n->for_iter && n->for_iter->kind == NODE_RANGE) {
                lo = n->for_iter->range_lo;
                hi = n->for_iter->range_hi;
            }
            RcTmpVec saved_lo = {0};
            int saved_lo_on = 0;
            int wrap_lo = cg->rc && lo && rc_tmp_would_collect(cg, lo);
            if (wrap_lo) {
                emit_indent(cg);
                fprintf(f, "{\n");
                cg->indent++;
                rc_tmp_begin(cg, lo, 0, &saved_lo, &saved_lo_on);
            }
            emit_indent(cg);
            char *m = mangle_name(n->for_var);
            fprintf(f, "for (int64_t %s = ", m);
            if (lo) {
                emit_expr(cg, lo);
                fprintf(f, "; %s < ", m);
                emit_rc_stmt_expr(cg, hi);
                fprintf(f, "; %s++) ", m);
            } else {
                fprintf(f, "0; %s < 0; %s++) ", m, m);
            }
            emit_block_scoped(cg, n->for_body, 1);
            fprintf(f, "\n");
            if (wrap_lo) {
                rc_tmp_end(cg, &saved_lo, saved_lo_on);
                cg->indent--;
                emit_indent(cg);
                fprintf(f, "}\n");
            }
            free(m);
            break;
        }

        case NODE_IF:
            emit_indent(cg);
            fprintf(f, "if (");
            emit_rc_stmt_expr(cg, n->cond);
            fprintf(f, ") ");
            emit_block(cg, n->then_br);
            if (n->else_br) {
                fprintf(f, " else ");
                if (n->else_br->kind == NODE_BLOCK) {
                    /* check if it's a single if (else-if chain) */
                    if (n->else_br->stmts.len == 1 &&
                        n->else_br->stmts.data[0]->kind == NODE_IF) {
                        emit_stmt(cg, n->else_br->stmts.data[0]);
                        return; /* emit_stmt already added newline */
                    }
                    emit_block(cg, n->else_br);
                }
            }
            fprintf(f, "\n");
            break;

        case NODE_EXPR_STMT: {
            /* RC4: temp-ове в израза (root на bare call с heap резултат е
             * дискарднат — пак е temp; assign дясното е bound) */
            RcTmpVec saved_tmps = {0};
            int saved_on = 0;
            if (cg->rc)
                rc_tmp_begin(cg, n->expr, 0, &saved_tmps, &saved_on);
            /* extern fn (incl. one named write/print/println) must bypass the
             * print-builtin dispatch and go through emit_expr's extern path */
            if (is_print_call(n->expr) &&
                !(n->expr->callee->kind == NODE_IDENT &&
                  find_extern_fn(cg, n->expr->callee->name))) {
                emit_print(cg, n->expr);
            } else {
                emit_indent(cg);
                emit_expr(cg, n->expr);
                fprintf(f, ";\n");
            }
            if (cg->rc) rc_tmp_end(cg, &saved_tmps, saved_on);
            /* RC1.3: `mem_rewind(m)` — локалите над watermark-а на m държат
             * върната памет; scope release-ът им би чел overwrite-нат header
             * (bump reuse) → маркират се dead (leak-safe посока) */
            if (cg->rc && rc_is_mem_call(n->expr, "mem_rewind") &&
                cg->rc_marks.len > 0) {
                int wm = cg->rc_marks.data[--cg->rc_marks.len];
                if (wm > cg->rc_locals.len) wm = cg->rc_locals.len;
                for (int mi = wm; mi < cg->rc_locals.len; mi++)
                    cg->rc_locals.data[mi].dead = 1;
            }
            break;
        }

        case NODE_BREAK:
            /* RC1: release на scopes до най-близкото loop тяло преди скока */
            if (cg->rc) rc_release_to_loop(cg);
            emit_indent(cg);
            fprintf(f, "break;\n");
            break;

        case NODE_CONTINUE:
            if (cg->rc) rc_release_to_loop(cg);
            emit_indent(cg);
            fprintf(f, "continue;\n");
            break;

        case NODE_BLOCK:
            emit_indent(cg);
            emit_block(cg, n);
            fprintf(f, "\n");
            break;

        case NODE_INVARIANT:
            /* annotation statement — verifier-only, no code emitted */
            break;

        default:
            c_unsupported_node("statement", n);
            break;
    }
}

/* ---- spec ensures ---- */

/* Намира spec с ensures или requires за дадена функция (NULL ако няма). */
static Node *find_ensures_spec(Codegen *cg, const char *fn_name) {
    if (!cg->program) return NULL;
    for (int i = 0; i < cg->program->items.len; i++) {
        Node *it = cg->program->items.data[i];
        if (it->kind == NODE_SPEC && strcmp(it->spec_name, fn_name) == 0 &&
            (it->spec_ensures.len > 0 || it->spec_requires.len > 0))
            return it;
    }
    return NULL;
}

/* ---- --test-specs ---- */

/* true, ако всички input типове на spec-а са i64/bool */
/* W2.7: spec с договор за свят НЕ е property-testable — --test-specs подава
 * произволни i64, а world handle-ът е указатъл: builtin-ите го дереференсират
 * и случайната стойност е segfault. (Досега някои случаи минаваха само защото
 * emitter-ът махаше мъртвите let-ове.) Договорът се проверява от --verify. */
static int expr_has_world_contract(Node *e) {
    if (!e) return 0;
    if (e->kind == NODE_CALL && e->callee && e->callee->kind == NODE_IDENT) {
        const char *n = e->callee->name;
        if (strcmp(n, "world_frozen") == 0 || strcmp(n, "world_mutable") == 0 ||
            strcmp(n, "world_owned") == 0) return 1;
    }
    if (e->kind == NODE_BINARY)
        return expr_has_world_contract(e->left) || expr_has_world_contract(e->right);
    if (e->kind == NODE_UNARY) return expr_has_world_contract(e->operand);
    return 0;
}

static int spec_has_world_contract(Node *spec) {
    for (int i = 0; i < spec->spec_requires.len; i++)
        if (expr_has_world_contract(spec->spec_requires.data[i]->ensure_expr)) return 1;
    /* W3.1: и гаранция за върнат свят прави spec-а неподходящ за property-тест —
     * резултатът е handle (произволен/лъжлив i64 е segfault при дереференс).
     * Гарацията се проверява статично от --verify и се поема от seatbelt-а. */
    for (int i = 0; i < spec->spec_ensures.len; i++)
        if (expr_has_world_contract(spec->spec_ensures.data[i]->ensure_expr)) return 1;
    return 0;
}

int spec_inputs_testable(Node *spec) {
    if (spec_has_world_contract(spec)) return 0;   /* W2.7 */
    for (int i = 0; i < spec->spec_inputs.len; i++) {
        Node *pt = spec->spec_inputs.data[i]->param_type;
        if (pt->kind != NODE_TYPE || !pt->type_name) return 0;
        if (strcmp(pt->type_name, "i64") != 0 && strcmp(pt->type_name, "bool") != 0)
            return 0;
    }
    return 1;
}

/* emit-ва requires предикат: static int b__req_<mangled>(params) { return (r1) && (r2); } */
static int is_verifier_only_annotation(Node *e);   /* W2.1: дефиниран по-долу */
void emit_requires_predicate(Codegen *cg, Node *spec) {
    FILE *f = cg->out;
    fprintf(f, "static int b__req_");
    char *m = mangle_name(spec->spec_name);
    fprintf(f, "%s", m + 2); /* mangle_name дава b_<име>; искаме b__req_<име> */
    free(m);
    fprintf(f, "(");
    if (spec->spec_inputs.len == 0) {
        fprintf(f, "void");
    } else {
        for (int i = 0; i < spec->spec_inputs.len; i++) {
            if (i > 0) fprintf(f, ", ");
            Node *sp = spec->spec_inputs.data[i];
            emit_type(cg, sp->param_type);
            fprintf(f, " ");
            char *pm = mangle_name(sp->param_name);
            fprintf(f, "%s", pm);
            free(pm);
        }
    }
    fprintf(f, ") {\n    return ");
    int nreq = 0;
    for (int j = 0; j < spec->spec_requires.len; j++) {
        /* W2.1: ghost предикатите (v[*], sorted(v), world_frozen(w)) нямат
         * runtime проверка — иначе --test-specs емитира несъществуваща fn */
        if (is_verifier_only_annotation(spec->spec_requires.data[j]->ensure_expr)) continue;
        if (nreq > 0) fprintf(f, " && ");
        fprintf(f, "(");
        emit_expr(cg, spec->spec_requires.data[j]->ensure_expr);
        fprintf(f, ")");
        nreq++;
    }
    if (nreq == 0) fprintf(f, "1");
    fprintf(f, ";\n}\n\n");
}

/* ---- function emission ---- */

/* v[*] element invariants and the world contract predicates (world_frozen /
 * world_mutable / world_owned) plus sorted(v) are verifier-only annotations —
 * they are not emitted as runtime contract checks. */
static int is_verifier_only_annotation(Node *e) {
    if (!e) return 0;
    if (e->kind == NODE_ELEM_REF) return 1;
    if (e->kind == NODE_CALL && e->callee && e->callee->kind == NODE_IDENT &&
        (strcmp(e->callee->name, "sorted") == 0 ||
         strcmp(e->callee->name, "world_frozen") == 0 ||
         strcmp(e->callee->name, "world_mutable") == 0 ||
         strcmp(e->callee->name, "world_owned") == 0)) return 1;
    if (e->kind == NODE_BINARY) return is_verifier_only_annotation(e->left) || is_verifier_only_annotation(e->right);
    if (e->kind == NODE_UNARY) return is_verifier_only_annotation(e->operand);
    return 0;
}

/* M27: ламбда в generic fn — уникално име per инстанция (споделен AST,
 * едно fn_name; без суфикса две инстанции емитуват един b___lam_N). */
const char *lambda_emit_name(Codegen *cg, Node *n, char *buf, size_t nbuf) {
    const char *base = n->fn_name ? n->fn_name : "__lam_x";
    if (cg->gen_fn && cg->gen_inst >= 0) {
        snprintf(buf, nbuf, "%s__i%d", base, cg->gen_inst);
        return buf;
    }
    return base;
}

/* L5: closure wrapper за потребителска функция — адресът му се взима от fn
 * стойности; вика публичното име (с евент. spec проверки). Емитва се в
 * lambda_out, който в изхода е преди телата на функциите. */
static void emit_clo_wrapper(Codegen *cg, Node *fn) {
    if (fn->is_extern) return;
    FILE *lf = cg->lambda_out ? cg->lambda_out : cg->out;
    /* M25: за generic инстанция (ползвана като fn стойност) името е
     * synth-овото (id__i0) — wrapper-ът вика инстанцията; emit_type
     * resolve-ва типовите променливи под активния gen контекст */
    const char *wn = cg->gen_emit_name ? cg->gen_emit_name : fn->fn_name;
    char *wm = mangle_name(wn);
    FILE *saved = cg->out;
    cg->out = lf;
    fprintf(lf, "static __attribute__((unused)) ");
    if (fn->ret_type) emit_type(cg, fn->ret_type); else fprintf(lf, "void");
    fprintf(lf, " %s__clo(void *_env", wm);
    for (int i = 0; i < fn->params.len; i++) {
        Node *p = fn->params.data[i];
        fprintf(lf, ", ");
        emit_type(cg, p->param_type);
        fprintf(lf, " ");
        char *pm = mangle_name(p->param_name);
        fprintf(lf, "%s", pm);
        free(pm);
    }
    fprintf(lf, ") {\n    (void)_env;\n    ");
    if (fn->ret_type) fprintf(lf, "return ");
    fprintf(lf, "%s(", wm);
    for (int i = 0; i < fn->params.len; i++) {
        if (i > 0) fprintf(lf, ", ");
        char *pm = mangle_name(fn->params.data[i]->param_name);
        fprintf(lf, "%s", pm);
        free(pm);
    }
    fprintf(lf, ");\n}\n\n");
    free(wm);
    cg->out = saved;
}

void emit_fn(Codegen *cg, Node *fn) {
    FILE *f = cg->out;

    Node *ensures_spec = (fn->fn_body)
                       ? find_ensures_spec(cg, fn->fn_name) : NULL;
    /* M27: spec върху generic — wrapper/impl носят synth името на
     * инстанцията (иначе всички инстанции се блъскат в __impl_f / f) */
    const char *emit_nm = cg->gen_emit_name ? cg->gen_emit_name : fn->fn_name;
    if (ensures_spec && cg->chk && cg->gen_fn && cg->gen_inst >= 0)
        checker_recheck_spec(cg->chk, ensures_spec, fn, cg->gen_inst);
    char impl_name_buf[512];
    if (ensures_spec) {
        /* оригиналното тяло става static impl функция */
        snprintf(impl_name_buf, sizeof impl_name_buf, "__impl_%s", emit_nm);
        fprintf(f, "static ");
    }

    /* return type */
    if (fn->ret_type) {
        emit_type(cg, fn->ret_type);
    } else {
        fprintf(f, "void");
    }
    fprintf(f, " ");

    /* name */
    char *m = mangle_name(ensures_spec ? impl_name_buf
                         : (cg->gen_emit_name ? cg->gen_emit_name
                                              : fn->fn_name));
    fprintf(f, "%s", m);
    free(m);

    /* params */
    fprintf(f, "(");
    if (fn->params.len == 0) {
        fprintf(f, "void");
    } else {
        for (int i = 0; i < fn->params.len; i++) {
            if (i > 0) fprintf(f, ", ");
            Node *p = fn->params.data[i];
            emit_type(cg, p->param_type);
            fprintf(f, " ");
            char *pm = mangle_name(p->param_name);
            fprintf(f, "%s", pm);
            free(pm);
        }
    }
    fprintf(f, ") ");

    /* body */
    if (fn->fn_body) {
        int has_ret = fn->ret_type != NULL;
        fprintf(f, "{\n");
        cg->indent++;
        /* M20: ret type node на текущата fn — за ZERO на propagate */
        Node *saved_eff_ret = cg->eff_cur_ret;
        cg->eff_cur_ret = fn->ret_type;
        /* RC2: last-use pre-pass за move elision (преди emission на тялото) */
        if (cg->rc) {
            cg->rc_lus.len = 0;
            cg->rc_marks.len = 0;  /* RC1.3: mark watermark-овете са per-fn */
            rc_lu_stmts(cg, &fn->fn_body->stmts, NULL, 0);
        }
        /* RC2.1: тялото на текущата fn за глобалния alias scan */
        cg->rc_cur_fn = fn->fn_body;
        /* RC1: fn scope — параметрите са заемани (is_param=1): регистрират
         * се за retain при `let x = p` / `return p`, но не се release-ват */
        rc_push_scope(cg, 0);
        int saved_fn_base = cg->rc_fn_base;
        if (cg->rc) cg->rc_fn_base = cg->rc_scopes.len - 1;
        if (cg->rc) {
            for (int i = 0; i < fn->params.len; i++) {
                Node *p = fn->params.data[i];
                rc_register_node(cg, p->param_name,
                            rc_heap_tag_node(cg, p->param_type),
                            p->type, p->param_type, 1);
            }
        }
        NodeVec *stmts = &fn->fn_body->stmts;
        for (int i = 0; i < stmts->len; i++) {
            Node *s = stmts->data[i];
            /* RC2.1: контекст за borrowed-pair анализа (fn body = scope) */
            Node *saved_blk = cg->rc_cur_blk;
            int saved_idx = cg->rc_cur_idx;
            cg->rc_cur_blk = fn->fn_body; cg->rc_cur_idx = i;
            /* implicit return: last expr stmt in non-void fn */
            if (has_ret && i == stmts->len - 1 && s->kind == NODE_EXPR_STMT) {
                /* RC4: temp-ове в implicit return израза — същият път като
                 * explicit return (release преди return-а) */
                RcTmpVec saved_tmps = {0};
                int saved_on = 0;
                if (cg->rc) {
                    rc_tmp_begin(cg, s->expr, 1, &saved_tmps, &saved_on);
                    emit_return_val(cg, s->expr);
                    rc_tmp_end(cg, &saved_tmps, saved_on);
                } else {
                    emit_return_val(cg, s->expr);
                }
            } else {
                emit_stmt(cg, s);
            }
            cg->rc_cur_blk = saved_blk; cg->rc_cur_idx = saved_idx;
        }
        rc_pop_scope(cg);
        cg->rc_fn_base = saved_fn_base;
        cg->indent--;
        emit_indent(cg);
        fprintf(f, "}");
        cg->eff_cur_ret = saved_eff_ret;
    } else {
        fprintf(f, "{}");
    }
    fprintf(f, "\n\n");

    if (!ensures_spec && !cg->gen_emit_name) {
        /* L5: closure wrapper — fn стойностите вземат адреса му; в
         * lambda_out (преди телата на функциите в изхода) */
        emit_clo_wrapper(cg, fn);
        return;
    }
    /* M25: generic инстанция, ползвана като fn стойност (annotated binding),
     * получава __clo wrapper със synth име; останалите инстанции — без */
    if (!ensures_spec && cg->gen_emit_name && fn->inst_as_value &&
        cg->gen_inst >= 0 && cg->gen_inst < fn->inst_count &&
        fn->inst_as_value[cg->gen_inst]) {
        emit_clo_wrapper(cg, fn);
        return;
    }
    if (!ensures_spec) return;  /* M21: generic инстанция — без spec wrapper */

    /* wrapper: публичното име, проверява requires преди и ensures след повикването */
    if (fn->ret_type) {
        emit_type(cg, fn->ret_type);
    } else {
        fprintf(f, "void");
    }
    fprintf(f, " ");
    char *wm = mangle_name(emit_nm);
    fprintf(f, "%s", wm);
    free(wm);
    fprintf(f, "(");
    if (ensures_spec->spec_inputs.len == 0) {
        fprintf(f, "void");
    } else {
        for (int i = 0; i < ensures_spec->spec_inputs.len; i++) {
            if (i > 0) fprintf(f, ", ");
            Node *sp = ensures_spec->spec_inputs.data[i];
            emit_type(cg, sp->param_type);
            fprintf(f, " ");
            char *pm = mangle_name(sp->param_name);
            fprintf(f, "%s", pm);
            free(pm);
        }
    }
    fprintf(f, ") {\n");
    cg->indent++;

    /* провери предусловията преди повикването */
    for (int j = 0; j < ensures_spec->spec_requires.len; j++) {
        Node *rq = ensures_spec->spec_requires.data[j];
        if (is_verifier_only_annotation(rq->ensure_expr)) continue;   /* v[*] / sorted(v) */
        emit_indent(cg);
        fprintf(f, "if (!(");
        emit_expr(cg, rq->ensure_expr);
        fprintf(f, ")) baga_spec_fail(");
        emit_c_string(f, ensures_spec->spec_name);
        fprintf(f, ", \"requires\", %d, ", j + 1);
        emit_c_string(f, rq->ensure_text);
        fprintf(f, ");\n");
    }

    if (fn->ret_type) {
        /* повикай impl и запази резултата като b_output */
        emit_indent(cg);
        emit_type(cg, fn->ret_type);
        fprintf(f, " b_output = ");
        char *im = mangle_name(impl_name_buf);
        fprintf(f, "%s(", im);
        free(im);
        for (int i = 0; i < ensures_spec->spec_inputs.len; i++) {
            if (i > 0) fprintf(f, ", ");
            char *pm = mangle_name(ensures_spec->spec_inputs.data[i]->param_name);
            fprintf(f, "%s", pm);
            free(pm);
        }
        fprintf(f, ");\n");

        /* провери всяка ensures гаранция */
        for (int j = 0; j < ensures_spec->spec_ensures.len; j++) {
            Node *en = ensures_spec->spec_ensures.data[j];
            if (is_verifier_only_annotation(en->ensure_expr)) continue;   /* v[*] / sorted(v) */
            emit_indent(cg);
            fprintf(f, "if (!(");
            emit_expr(cg, en->ensure_expr);
            fprintf(f, ")) baga_spec_fail(");
            emit_c_string(f, ensures_spec->spec_name);
            fprintf(f, ", \"ensures\", %d, ", j + 1);
            emit_c_string(f, en->ensure_text);
            fprintf(f, ");\n");
        }

        emit_indent(cg);
        fprintf(f, "return b_output;\n");
    } else {
        /* void функция: само повикай impl след предусловията */
        emit_indent(cg);
        char *im = mangle_name(impl_name_buf);
        fprintf(f, "%s(", im);
        free(im);
        for (int i = 0; i < ensures_spec->spec_inputs.len; i++) {
            if (i > 0) fprintf(f, ", ");
            char *pm = mangle_name(ensures_spec->spec_inputs.data[i]->param_name);
            fprintf(f, "%s", pm);
            free(pm);
        }
        fprintf(f, ");\n");
        emit_indent(cg);
        fprintf(f, "return;\n");
    }
    cg->indent--;
    emit_indent(cg);
    fprintf(f, "}\n\n");

    /* L5: closure wrapper и за spec-обвитите функции (викa публичното име) */
    emit_clo_wrapper(cg, fn);
}

/* ---- struct / sum-enum emission (topo-ordered) ---- */

int is_sum_enum_item(Node *item) {
    if (!item || item->kind != NODE_ENUM) return 0;
    for (int j = 0; j < item->n_variants; j++)
        if (item->enum_payloads && item->enum_payloads[j]) return 1;
    return 0;
}

/* Named user type from a type AST node (struct or sum enum), or NULL for
 * primitives / Vec / Map / unknown. Unwraps !effects and &T. */
static const char *user_type_name(Node *ty) {
    while (ty) {
        if (ty->kind == NODE_TYPE_EFFECT || ty->kind == NODE_TYPE_REF) {
            ty = ty->inner_type;
            continue;
        }
        if (ty->kind != NODE_TYPE || !ty->type_name) return NULL;
        const char *n = ty->type_name;
        if (strcmp(n, "i64") == 0 || strcmp(n, "i32") == 0 ||
            strcmp(n, "f64") == 0 || strcmp(n, "bool") == 0 ||
            strcmp(n, "str") == 0 || strcmp(n, "bytes") == 0 ||
            strcmp(n, "void") == 0 || strcmp(n, "Vec") == 0 ||
            strcmp(n, "Map") == 0 || strcmp(n, "fn") == 0)
            return NULL;
        return n;
    }
    return NULL;
}

static void emit_struct(Codegen *cg, Node *s) {
    FILE *f = cg->out;
    /* M24: generic struct — typedef per инстанция (мономорфизация) */
    if (s->n_struct_params > 0) {
        for (int k = 0; k < s->struct_inst_count; k++) {
            cg->gen_struct = s;
            cg->gen_struct_inst = k;
            char *m = mangle_name(s->struct_name);
            fprintf(f, "typedef struct {\n");
            for (int i = 0; i < s->fields.len; i++) {
                Node *fld = s->fields.data[i];
                fprintf(f, "    ");
                emit_type(cg, fld->fld_type);
                fprintf(f, " ");
                char *fm = mangle_name(fld->fld_name);
                fprintf(f, "%s", fm);
                free(fm);
                fprintf(f, ";\n");
            }
            fprintf(f, "} %s", m);
            for (int a = 0; a < s->n_struct_params; a++) {
                Type *at = s->struct_inst_targs[k * s->n_struct_params + a];
                if (at->kind == TYPE_STRUCT && at->name) {
                    char *am = mangle_name(at->name);
                    fprintf(f, "_%s", am + 2);
                    free(am);
                } else if (at->kind == TYPE_STR) fprintf(f, "_str");
                else if (at->kind == TYPE_BYTES) fprintf(f, "_bytes");
                else if (at->kind == TYPE_F64) fprintf(f, "_f64");
                else if (at->kind == TYPE_BOOL) fprintf(f, "_bool");
                else if (at->kind == TYPE_VEC) fprintf(f, "_v");
                else if (at->kind == TYPE_MAP) fprintf(f, "_m");
                else if (at->kind == TYPE_ENUM && at->name) {
                    char *am = mangle_name(at->name);
                    fprintf(f, "_%s", am + 2);
                    free(am);
                } else fprintf(f, "_i64");
            }
            fprintf(f, ";\n\n");
            free(m);
            cg->gen_struct = NULL;
            cg->gen_struct_inst = -1;
        }
        return;
    }
    char *m = mangle_name(s->struct_name);
    fprintf(f, "typedef struct {\n");
    for (int i = 0; i < s->fields.len; i++) {
        Node *fld = s->fields.data[i];
        fprintf(f, "    ");
        emit_type(cg, fld->fld_type);
        fprintf(f, " ");
        char *fm = mangle_name(fld->fld_name);
        fprintf(f, "%s", fm);
        free(fm);
        fprintf(f, ";\n");
    }
    fprintf(f, "} %s;\n\n", m);
    free(m);
}

/* RC5: retain/release на преките heap полета за един struct „вид" —
 * k >= 0 → инстанция k на generic decl s (типовите променливи се resolve-ват
 * към struct_inst_targs, M24-RC); k < 0 → не-generic decl. m е C името
 * (per-instance за generic: b_Box_i64). След typedef-а. */
static void emit_rc_struct_helpers_one(Codegen *cg, Node *s, int k,
                                       const char *m) {
    FILE *f = cg->out;
    /* M26: shim дефинициите за вложени Vec полета (relv_str/relv_v_str/…)
     * ПРЕДИ телата — mid-function emission в out е невалидна; резолверите
     * по-долу намират имената вече регистрирани (no-op по dedup) */
    for (int i = 0; i < s->fields.len; i++) {
        char dummy[160];
        Node *fld = s->fields.data[i];
        Type *rt = rc_fld_inst_type(cg, k >= 0 ? s : NULL, k, fld->fld_type);
        if (rt) rc_nested_vec_rel_type(cg, rt, dummy, sizeof dummy);
        else rc_nested_vec_rel_node(cg, fld->fld_type, dummy, sizeof dummy);
    }
    fprintf(f, "static inline void baga_rc_retain_%s(%s s) {\n", m, m);
    for (int i = 0; i < s->fields.len; i++) {
        Node *fld = s->fields.data[i];
        Type *rt = rc_fld_inst_type(cg, k >= 0 ? s : NULL, k, fld->fld_type);
        int tag = rt ? rc_heap_tag(cg, rt) : rc_type_node_tag(fld->fld_type);
        char *fm = mangle_name(fld->fld_name);
        if (tag == 5 || tag == 6) {
            /* M24-RC: вложен struct/enum (вкл. instantiated generic) —
             * рекурсивен retain с per-instance име */
            char *nm = rc_rc_type_name(rt, NULL);
            fprintf(f, "    baga_rc_retain_%s(s.%s);\n", nm, fm);
            free(nm);
        } else if (!tag) {
            /* RC5 v0.5: вложено struct поле — рекурсивен retain */
            const char *nn = rc_nested_struct_field(cg, fld->fld_type);
            if (nn) {
                char *nm = mangle_name(nn);
                fprintf(f, "    baga_rc_retain_%s(s.%s);\n", nm, fm);
                free(nm);
            } else {
                /* RC5 v0.10: enum поле — retain_E по runtime tag */
                const char *en = rc_nested_enum_field(cg, fld->fld_type);
                if (en) {
                    char *nm = mangle_name(en);
                    fprintf(f, "    baga_rc_retain_%s(s.%s);\n", nm, fm);
                    free(nm);
                }
            }
            free(fm);
            continue;
        }
        if (tag == 2)
            fprintf(f, "    baga_rc_retain((void *)s.%s.data);\n", fm);
        else if (tag)
            fprintf(f, "    baga_rc_retain((void *)s.%s);\n", fm);
        free(fm);
    }
    fprintf(f, "}\n");
    fprintf(f, "static inline void baga_rc_release_%s(%s s) {\n", m, m);
    for (int i = 0; i < s->fields.len; i++) {
        Node *fld = s->fields.data[i];
        Type *rt = rc_fld_inst_type(cg, k >= 0 ? s : NULL, k, fld->fld_type);
        int tag = rt ? rc_heap_tag(cg, rt) : rc_type_node_tag(fld->fld_type);
        char *fm = mangle_name(fld->fld_name);
        if (tag == 5 || tag == 6) {
            /* M24-RC: рекурсивен release с per-instance име */
            char *nm = rc_rc_type_name(rt, NULL);
            fprintf(f, "    baga_rc_release_%s(s.%s);\n", nm, fm);
            free(nm);
        } else if (!tag) {
            /* RC5 v0.5: вложено struct поле — рекурсивен release */
            const char *nn = rc_nested_struct_field(cg, fld->fld_type);
            if (nn) {
                char *nm = mangle_name(nn);
                fprintf(f, "    baga_rc_release_%s(s.%s);\n", nm, fm);
                free(nm);
            } else {
                /* RC5 v0.10: enum поле — release_E по runtime tag */
                const char *en = rc_nested_enum_field(cg, fld->fld_type);
                if (en) {
                    char *nm = mangle_name(en);
                    fprintf(f, "    baga_rc_release_%s(s.%s);\n", nm, fm);
                    free(nm);
                }
            }
            free(fm);
            continue;
        }
        if (tag == 1)
            fprintf(f, "    baga_rc_release_str(s.%s);\n", fm);
        else if (tag == 2)
            fprintf(f, "    baga_rc_release_bytes(s.%s);\n", fm);
        else if (tag == 3) {
            char sz[160], rel[160];
            /* M24-RC: resolved тип → Type resolver-и; иначе node */
            int kk = rt ? rc_vec_elem_kind(rt, sz, sizeof sz)
                        : rc_vec_elem_kind_node(fld->fld_type, sz, sizeof sz);
            if (rt) rc_box_rel_ty(cg, rt->elem, rel, sizeof rel);
            else {
                Node *et = fld->fld_type->inner_type;
                rc_box_rel(cg, (et && et->kind == NODE_TYPE) ? et->type_name : NULL,
                           rel, sizeof rel);
            }
            /* RC5 v0.9/M26: вложен Vec — destructor за елементите;
             * resolved типът (rt) е конкретен за инстанцията */
            if (kk == 3) {
                if (rt) rc_nested_vec_rel_type(cg, rt, rel, sizeof rel);
                else rc_nested_vec_rel_node(cg, fld->fld_type, rel, sizeof rel);
            }
            fprintf(f, "    baga_rc_release_vec(s.%s, %d, %s, %s);\n", fm, kk, sz, rel);
        } else if (tag == 4) {
            char sz[160], rel[160];
            int kk = rt ? rc_map_val_tag(rt, sz, sizeof sz)
                        : rc_map_val_tag_node(fld->fld_type, sz, sizeof sz);
            if (rt) rc_box_rel_ty(cg, rt->elem, rel, sizeof rel);
            else {
                Node *vt = fld->fld_type->inner_type2;
                rc_box_rel(cg, (vt && vt->kind == NODE_TYPE) ? vt->type_name : NULL,
                           rel, sizeof rel);
            }
            fprintf(f, "    baga_rc_release_map(s.%s, %d, %s, %s);\n", fm, kk, sz, rel);
        }
        free(fm);
    }
    fprintf(f, "}\n");
    /* RC5 v0.2: shim-ове за box елементи във Vec/Map — release/retain на
     * полетата през указател (container drop/slice/concat не знаят типа
     * статично). */
    fprintf(f, "static void baga_rc_relf_%s(void *p) { baga_rc_release_%s(*(%s *)p); }\n",
            m, m, m);
    fprintf(f, "static void baga_rc_retp_%s(void *p) { baga_rc_retain_%s(*(%s *)p); }\n\n",
            m, m, m);
    /* RC5 v0.9: shim за Vec<S> като елемент на външен Vec — release на box
     * елементите на вътрешния vec през relf (kind 3 на външния не знае
     * елементния тип на вложения). */
    fprintf(f, "static void baga_rc_relv_%s(void *p) { baga_rc_release_vec((baga_Vec *)p, 2, (int64_t)sizeof(%s), baga_rc_relf_%s); }\n",
            m, m, m);
}

/* RC5: retain/release на преките heap полета. След typedef-а.
 * M24-RC: generic struct — по един набор helper-и на инстанция (само ако
 * инстанцията реално има heap полета след resolve). */
void emit_rc_struct_helpers(Codegen *cg, Node *s) {
    if (!cg->rc) return;
    if (s->n_struct_params > 0) {
        for (int k = 0; k < s->struct_inst_count; k++) {
            Type it = {0};
            it.kind = TYPE_STRUCT;
            it.name = s->struct_name;
            it.n_targs = s->n_struct_params;
            it.targs = &s->struct_inst_targs[k * s->n_struct_params];
            if (rc_heap_tag(cg, &it) != 5) continue;
            char *m = struct_cname_str(&it);
            emit_rc_struct_helpers_one(cg, s, k, m);
            free(m);
        }
        return;
    }
    if (!rc_struct_has_heap(cg, s->struct_name)) return;
    char *m = mangle_name(s->struct_name);
    emit_rc_struct_helpers_one(cg, s, -1, m);
    free(m);
}

/* RC5 v0.6: retain/release по runtime tag за enum с heap payload. */
static void emit_rc_enum_helpers(Codegen *cg, Node *item) {
    if (!cg->rc || !rc_enum_has_heap(cg, item)) return;
    FILE *f = cg->out;
    char *em = mangle_name(item->enum_name);
    /* M26: shim дефиниции за вложени Vec payload-и ПРЕДИ телата (като при
     * struct helper-ите — mid-function emission в out е невалидна) */
    for (int j = 0; j < item->n_variants; j++) {
        if (!item->enum_payloads || !item->enum_payloads[j]) continue;
        char dummy[160];
        rc_nested_vec_rel_node(cg, item->enum_payloads[j], dummy, sizeof dummy);
    }
    for (int pass = 0; pass < 2; pass++) {
        fprintf(f, "static inline void baga_rc_%s_%s(%s e) {\n    switch (e.tag) {\n",
                pass ? "release" : "retain", em, em);
        for (int j = 0; j < item->n_variants; j++) {
            if (!item->enum_payloads || !item->enum_payloads[j]) continue;
            Node *pt = item->enum_payloads[j];
            int tag = rc_type_node_tag(pt);
            const char *nn = tag ? NULL : rc_nested_struct_field(cg, pt);
            if (!tag && !nn) continue;
            char *vm = mangle_name(item->enum_variants[j]);
            if (!pass) {
                if (nn) {
                    char *nm = mangle_name(nn);
                    fprintf(f, "        case %d: baga_rc_retain_%s(e.u.v_%s); break;\n",
                            j, nm, vm);
                    free(nm);
                } else if (tag == 2)
                    fprintf(f, "        case %d: baga_rc_retain((void *)e.u.v_%s.data); break;\n",
                            j, vm);
                else
                    fprintf(f, "        case %d: baga_rc_retain((void *)e.u.v_%s); break;\n",
                            j, vm);
            } else {
                if (nn) {
                    char *nm = mangle_name(nn);
                    fprintf(f, "        case %d: baga_rc_release_%s(e.u.v_%s); break;\n",
                            j, nm, vm);
                    free(nm);
                } else if (tag == 1)
                    fprintf(f, "        case %d: baga_rc_release_str(e.u.v_%s); break;\n",
                            j, vm);
                else if (tag == 2)
                    fprintf(f, "        case %d: baga_rc_release_bytes(e.u.v_%s); break;\n",
                            j, vm);
                else if (tag == 3) {
                    char sz[160], rel[160];
                    int k = rc_vec_elem_kind_node(pt, sz, sizeof sz);
                    Node *et = pt->inner_type;
                    rc_box_rel(cg, (et && et->kind == NODE_TYPE) ?
                               et->type_name : NULL, rel, sizeof rel);
                    /* RC5 v0.9: вложен Vec<S> — destructor за S полетата */
                    if (k == 3) rc_nested_vec_rel_node(cg, pt, rel, sizeof rel);
                    fprintf(f, "        case %d: baga_rc_release_vec(e.u.v_%s, %d, %s, %s); break;\n",
                            j, vm, k, sz, rel);
                } else if (tag == 4) {
                    char sz[160], rel[160];
                    int k = rc_map_val_tag_node(pt, sz, sizeof sz);
                    Node *vt = pt->inner_type2;
                    rc_box_rel(cg, (vt && vt->kind == NODE_TYPE) ?
                               vt->type_name : NULL, rel, sizeof rel);
                    fprintf(f, "        case %d: baga_rc_release_map(e.u.v_%s, %d, %s, %s); break;\n",
                            j, vm, k, sz, rel);
                }
            }
            free(vm);
        }
        fprintf(f, "    }\n}\n");
    }
    /* RC5 v0.10: shim-ове за box елементи/стойности във Vec/Map — release/
     * retain на payload-а през указател (container drop/set/del/slice не
     * знаят типа статично). Огледало на struct relf/retp от v0.2. */
    fprintf(f, "static void baga_rc_relf_%s(void *p) { baga_rc_release_%s(*(%s *)p); }\n",
            em, em, em);
    fprintf(f, "static void baga_rc_retp_%s(void *p) { baga_rc_retain_%s(*(%s *)p); }\n",
            em, em, em);
    /* RC5 v1.1: shim за Vec<E> като елемент на външен Vec — огледало на
     * struct relv от v0.9 (kind 3 на външния не знае елементния тип). */
    fprintf(f, "static void baga_rc_relv_%s(void *p) { baga_rc_release_vec((baga_Vec *)p, 2, (int64_t)sizeof(%s), baga_rc_relf_%s); }\n",
            em, em, em);
    fprintf(f, "\n");
    free(em);
}

/* L3 sum enum → tagged C struct + union + static inline constructors.
 * Tag is int64_t to mirror the LLVM lowering ({ i64 tag, [N x i64] u });
 * constructors zero-init so the union never carries stack garbage
 * (LP1 probe: gcc -Wstringop-overread on payload reads). */
static void emit_sum_enum(Codegen *cg, Node *item) {
    FILE *out = cg->out;
    char *em = mangle_name(item->enum_name);
    fprintf(out, "typedef struct {\n    int64_t tag;\n    union {\n");
    for (int j = 0; j < item->n_variants; j++) {
        if (!item->enum_payloads || !item->enum_payloads[j]) continue;
        char *vm = mangle_name(item->enum_variants[j]);
        fprintf(out, "        ");
        emit_type(cg, item->enum_payloads[j]);
        fprintf(out, " v_%s;\n", vm);
        free(vm);
    }
    fprintf(out, "    } u;\n} %s;\n\n", em);
    for (int j = 0; j < item->n_variants; j++) {
        char *vm = mangle_name(item->enum_variants[j]);
        if (item->enum_payloads && item->enum_payloads[j]) {
            fprintf(out, "static inline %s %s__%s(", em, em, vm);
            emit_type(cg, item->enum_payloads[j]);
            fprintf(out, " a0) {\n    %s r = {0}; r.tag = %d; r.u.v_%s = a0; return r;\n}\n\n",
                    em, j, vm);
        } else {
            fprintf(out, "static inline %s %s__%s(void) {\n    %s r = {0}; r.tag = %d; return r;\n}\n\n",
                    em, em, vm, em, j);
        }
        free(vm);
    }
    free(em);
}

/* Emit structs and sum enums in dependency order so sum-enum fields work
 * (Wrap { r: Res }) and struct payloads still work (Circle(Point)). */
void emit_structs_and_sum_enums(Codegen *cg, Node *program) {
    int n = 0;
    for (int i = 0; i < program->items.len; i++) {
        Node *it = program->items.data[i];
        if (it->kind == NODE_STRUCT || is_sum_enum_item(it)) n++;
    }
    if (n == 0) return;

    Node **nodes = (Node **)calloc((size_t)n, sizeof(Node *));
    int *indeg = (int *)calloc((size_t)n, sizeof(int));
    /* adj[j] is list of indices that depend on j (edges j → k) */
    int **adj = (int **)calloc((size_t)n, sizeof(int *));
    int *adj_len = (int *)calloc((size_t)n, sizeof(int));
    int *adj_cap = (int *)calloc((size_t)n, sizeof(int));
    if (!nodes || !indeg || !adj || !adj_len || !adj_cap) {
        fprintf(stderr, "baga: out of memory\n");
        exit(1);
    }

    int k = 0;
    for (int i = 0; i < program->items.len; i++) {
        Node *it = program->items.data[i];
        if (it->kind == NODE_STRUCT || is_sum_enum_item(it))
            nodes[k++] = it;
    }

    /* name → index */
    for (int a = 0; a < n; a++) {
        /* collect dependency names of a */
        const char *deps[64];
        int nd = 0;
        if (nodes[a]->kind == NODE_STRUCT) {
            for (int f = 0; f < nodes[a]->fields.len && nd < 64; f++) {
                const char *dn = user_type_name(nodes[a]->fields.data[f]->fld_type);
                if (dn) deps[nd++] = dn;
            }
        } else {
            for (int v = 0; v < nodes[a]->n_variants && nd < 64; v++) {
                if (!nodes[a]->enum_payloads || !nodes[a]->enum_payloads[v]) continue;
                const char *dn = user_type_name(nodes[a]->enum_payloads[v]);
                if (dn) deps[nd++] = dn;
            }
        }
        for (int d = 0; d < nd; d++) {
            int b = -1;
            for (int j = 0; j < n; j++) {
                const char *bn = nodes[j]->kind == NODE_STRUCT
                    ? nodes[j]->struct_name : nodes[j]->enum_name;
                if (bn && strcmp(bn, deps[d]) == 0) { b = j; break; }
            }
            if (b < 0 || b == a) continue; /* primitive / other / self */
            /* edge b → a (b must be emitted before a) */
            if (adj_len[b] + 1 > adj_cap[b]) {
                int nc = adj_cap[b] ? adj_cap[b] * 2 : 4;
                int *na = (int *)realloc(adj[b], (size_t)nc * sizeof(int));
                if (!na) { fprintf(stderr, "baga: out of memory\n"); exit(1); }
                adj[b] = na;
                adj_cap[b] = nc;
            }
            adj[b][adj_len[b]++] = a;
            indeg[a]++;
        }
    }

    int *queue = (int *)malloc((size_t)n * sizeof(int));
    int *order = (int *)malloc((size_t)n * sizeof(int));
    int qh = 0, qt = 0, on = 0;
    if (!queue || !order) { fprintf(stderr, "baga: out of memory\n"); exit(1); }
    for (int i = 0; i < n; i++)
        if (indeg[i] == 0) queue[qt++] = i;
    while (qh < qt) {
        int u = queue[qh++];
        order[on++] = u;
        for (int e = 0; e < adj_len[u]; e++) {
            int v = adj[u][e];
            indeg[v]--;
            if (indeg[v] == 0) queue[qt++] = v;
        }
    }
    /* cycle or leftover: append remaining in declaration order */
    if (on < n) {
        for (int i = 0; i < n; i++) {
            int seen = 0;
            for (int j = 0; j < on; j++) if (order[j] == i) { seen = 1; break; }
            if (!seen) order[on++] = i;
        }
    }

    for (int i = 0; i < on; i++) {
        Node *it = nodes[order[i]];
        if (it->kind == NODE_STRUCT)
            emit_struct(cg, it);
        else
            emit_sum_enum(cg, it);
    }
    if (cg->rc) {
        /* RC5 v0.2: forward decls на box shims — release_S на по-ранен
         * struct реферира relf на по-късен (Vec<S>/Map<K,S> поле).
         * v0.5: и retain/release — вложеното struct поле рекурсира
         * напред-назад по декларационния ред.
         * v0.6: enum helper-и (struct payload в enum реферира retain_S). */
        for (int i = 0; i < on; i++) {
            Node *it = nodes[order[i]];
            if (it->kind == NODE_STRUCT && it->n_struct_params > 0) {
                /* M24-RC: generic struct — fwd decls per инстанция с heap
                 * полета (името е per-instance: b_Box_i64) */
                for (int k = 0; k < it->struct_inst_count; k++) {
                    Type ity = {0};
                    ity.kind = TYPE_STRUCT;
                    ity.name = it->struct_name;
                    ity.n_targs = it->n_struct_params;
                    ity.targs = &it->struct_inst_targs[k * it->n_struct_params];
                    if (rc_heap_tag(cg, &ity) != 5) continue;
                    char *m = struct_cname_str(&ity);
                    fprintf(cg->out, "static inline void baga_rc_retain_%s(%s s);\n", m, m);
                    fprintf(cg->out, "static inline void baga_rc_release_%s(%s s);\n", m, m);
                    fprintf(cg->out, "static void baga_rc_relf_%s(void *p);\n", m);
                    fprintf(cg->out, "static void baga_rc_retp_%s(void *p);\n", m);
                    fprintf(cg->out, "static void baga_rc_relv_%s(void *p);\n", m);
                    free(m);
                }
            } else if (it->kind == NODE_STRUCT &&
                rc_struct_has_heap(cg, it->struct_name)) {
                char *m = mangle_name(it->struct_name);
                fprintf(cg->out, "static inline void baga_rc_retain_%s(%s s);\n", m, m);
                fprintf(cg->out, "static inline void baga_rc_release_%s(%s s);\n", m, m);
                fprintf(cg->out, "static void baga_rc_relf_%s(void *p);\n", m);
                fprintf(cg->out, "static void baga_rc_retp_%s(void *p);\n", m);
                /* RC5 v0.9: и nested vec shim-ът (Vec<Vec<S>> поле на
                 * по-ранен struct реферира relv на по-късен) */
                fprintf(cg->out, "static void baga_rc_relv_%s(void *p);\n", m);
                free(m);
            } else if (it->kind == NODE_ENUM && rc_enum_has_heap(cg, it)) {
                char *m = mangle_name(it->enum_name);
                fprintf(cg->out, "static inline void baga_rc_retain_%s(%s e);\n", m, m);
                fprintf(cg->out, "static inline void baga_rc_release_%s(%s e);\n", m, m);
                /* RC5 v0.10: и box shim-овете (struct с Vec<E>/Map<K,E> поле
                 * реферира relf/retp на по-късен enum).
                 * RC5 v1.1: relv за Vec<Vec<E>> (като struct v0.9). */
                fprintf(cg->out, "static void baga_rc_relf_%s(void *p);\n", m);
                fprintf(cg->out, "static void baga_rc_retp_%s(void *p);\n", m);
                fprintf(cg->out, "static void baga_rc_relv_%s(void *p);\n", m);
                free(m);
            }
        }
        for (int i = 0; i < on; i++) {
            Node *it = nodes[order[i]];
            if (it->kind == NODE_STRUCT)
                emit_rc_struct_helpers(cg, it);
            else if (it->kind == NODE_ENUM)
                emit_rc_enum_helpers(cg, it);
        }
    }

    for (int i = 0; i < n; i++) free(adj[i]);
    free(adj); free(adj_len); free(adj_cap);
    free(nodes); free(indeg); free(queue); free(order);
}

Node *find_struct_decl(Codegen *cg, const char *name) {
    if (!cg->program) return NULL;
    for (int i = 0; i < cg->program->items.len; i++) {
        Node *it = cg->program->items.data[i];
        if (it->kind == NODE_STRUCT && it->struct_name &&
            strcmp(it->struct_name, name) == 0)
            return it;
    }
    return NULL;
}

Node *find_sum_enum_decl(Codegen *cg, const char *name) {
    if (!cg->program || !name) return NULL;
    for (int i = 0; i < cg->program->items.len; i++) {
        Node *it = cg->program->items.data[i];
        if (is_sum_enum_item(it) && it->enum_name &&
            strcmp(it->enum_name, name) == 0)
            return it;
    }
    return NULL;
}

/* нулев struct литерал с нули по полета: str → "" (не NULL — безопасно за
 * печат/concat), bytes → (baga_bytes){0}, вложен struct → рекурсивно;
 * Vec/Map/числа → 0 (vec_len/map_len търпят NULL). Нужен за map_get на
 * липсващ ключ при Map<K, struct>. */
void emit_zero_struct(Codegen *cg, const char *name) {
    FILE *f = cg->out;
    char *m = mangle_name(name);
    Node *decl = find_struct_decl(cg, name);
    if (!decl) {
        fprintf(f, "(%s){0}", m);
        free(m);
        return;
    }
    fprintf(f, "(%s){", m);
    free(m);
    for (int i = 0; i < decl->fields.len; i++) {
        Node *fld = decl->fields.data[i];
        if (i > 0) fprintf(f, ", ");
        char *fm = mangle_name(fld->fld_name);
        fprintf(f, ".%s = ", fm);
        free(fm);
        Node *ft = fld->fld_type;
        if (ft && ft->kind == NODE_TYPE && ft->type_name) {
            if (strcmp(ft->type_name, "str") == 0) {
                fprintf(f, "\"\"");
                continue;
            }
            if (strcmp(ft->type_name, "bytes") == 0) {
                fprintf(f, "(baga_bytes){0}");
                continue;
            }
            if (strcmp(ft->type_name, "i64") != 0 &&
                strcmp(ft->type_name, "i32") != 0 &&
                strcmp(ft->type_name, "f64") != 0 &&
                strcmp(ft->type_name, "bool") != 0 &&
                strcmp(ft->type_name, "Vec") != 0 &&
                strcmp(ft->type_name, "Map") != 0 &&
                ft->kind == NODE_TYPE) {
                /* sum enum field → zeroed tagged union; else nested struct */
                if (find_sum_enum_decl(cg, ft->type_name)) {
                    char *em = mangle_name(ft->type_name);
                    fprintf(f, "(%s){0}", em);
                    free(em);
                } else {
                    emit_zero_struct(cg, ft->type_name);
                }
                continue;
            }
        }
        fprintf(f, "0");
    }
    fprintf(f, "}");
}

/* ---- forward declarations ---- */

void emit_forward_decls(Codegen *cg, Node *program) {
    FILE *f = cg->out;
    for (int i = 0; i < program->items.len; i++) {
        Node *item = program->items.data[i];
        if (item->kind != NODE_FN) continue;

        if (item->is_extern) {
            /* Skip prototypes that clash with system headers pulled in by
             * #include <pthread.h> (e.g. time.h declares clock_gettime).
             * The call site still links against libc; types are pointer-compatible. */
            if (strcmp(item->fn_name, "clock_gettime") == 0)
                continue;
            /* extern fn: prototype with the raw C name and C ABI types */
            emit_extern_type(f, item->ret_type, 1);
            fprintf(f, " %s(", item->fn_name);
            if (item->params.len == 0) {
                fprintf(f, "void");
            } else {
                for (int j = 0; j < item->params.len; j++) {
                    if (j > 0) fprintf(f, ", ");
                    emit_extern_type(f, item->params.data[j]->param_type, 0);
                }
            }
            fprintf(f, ");\n");
            continue;
        }

        /* M21: generic fn — декларация per инстанция (мономорфизация) */
        if (item->n_type_params > 0 && item->inst_count > 0) {
            for (int k = 0; k < item->inst_count; k++) {
                char nm[512];
                snprintf(nm, sizeof nm, "%s__i%d", item->fn_name, k);
                if (cg->chk) checker_recheck_inst(cg->chk, item, k);
                cg->gen_fn = item; cg->gen_inst = k; cg->gen_emit_name = nm;
                if (item->ret_type) emit_type(cg, item->ret_type);
                else fprintf(f, "void");
                fprintf(f, " ");
                char *m = mangle_name(nm);
                fprintf(f, "%s", m);
                free(m);
                fprintf(f, "(");
                if (item->params.len == 0) {
                    fprintf(f, "void");
                } else {
                    for (int j = 0; j < item->params.len; j++) {
                        if (j > 0) fprintf(f, ", ");
                        Node *p = item->params.data[j];
                        emit_type(cg, p->param_type);
                        fprintf(f, " ");
                        char *pm = mangle_name(p->param_name);
                        fprintf(f, "%s", pm);
                        free(pm);
                    }
                }
                fprintf(f, ");\n");
                cg->gen_fn = NULL; cg->gen_inst = -1; cg->gen_emit_name = NULL;
            }
            continue;
        }

        if (item->ret_type) emit_type(cg, item->ret_type);
        else fprintf(f, "void");
        fprintf(f, " ");
        char *m = mangle_name(item->fn_name);
        fprintf(f, "%s", m);
        free(m);
        fprintf(f, "(");
        if (item->params.len == 0) {
            fprintf(f, "void");
        } else {
            for (int j = 0; j < item->params.len; j++) {
                if (j > 0) fprintf(f, ", ");
                Node *p = item->params.data[j];
                emit_type(cg, p->param_type);
                fprintf(f, " ");
                char *pm = mangle_name(p->param_name);
                fprintf(f, "%s", pm);
                free(pm);
            }
        }
        fprintf(f, ");\n");
    }
    /* M23: impl методите — декларации (вътрешните имена са вече върху
     * fn-ите; статичен dispatch) */
    for (int i = 0; i < program->items.len; i++) {
        Node *item = program->items.data[i];
        if (item->kind != NODE_IMPL) continue;
        for (int m = 0; m < item->impl_methods.len; m++) {
            Node *mf = item->impl_methods.data[m];
            if (mf->ret_type) emit_type(cg, mf->ret_type);
            else fprintf(f, "void");
            fprintf(f, " ");
            char *mm = mangle_name(mf->fn_name);
            fprintf(f, "%s", mm);
            free(mm);
            fprintf(f, "(");
            if (mf->params.len == 0) {
                fprintf(f, "void");
            } else {
                for (int j = 0; j < mf->params.len; j++) {
                    if (j > 0) fprintf(f, ", ");
                    Node *pm = mf->params.data[j];
                    emit_type(cg, pm->param_type);
                    fprintf(f, " ");
                    char *pmn = mangle_name(pm->param_name);
                    fprintf(f, "%s", pmn);
                    free(pmn);
                }
            }
            fprintf(f, ");\n");
        }
    }
    fprintf(f, "\n");
}

