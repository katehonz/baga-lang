#include "baga.h"

/* ============================================================
 *  Proof extraction — Фаза 6
 *
 *  Извлича четими proof sketches от AST-то.
 *  Не са формални доказателства — са спецификации
 *  които човек (или AI) може да верифицира.
 * ============================================================ */

static const char *type_node_str(Node *ty) {
    if (!ty) return "void";
    if (ty->kind == NODE_TYPE) return ty->type_name;
    if (ty->kind == NODE_TYPE_REF) return "&T";
    if (ty->kind == NODE_TYPE_ARRAY) return "[T]";
    if (ty->kind == NODE_TYPE_EFFECT) return type_node_str(ty->inner_type);
    return "?";
}

static int has_effects(Node *fn) {
    if (!fn->ret_type) return 0;
    if (fn->ret_type->kind == NODE_TYPE_EFFECT) return 1;
    return 0;
}

static void print_effects(Node *ty) {
    if (!ty) return;
    if (ty->kind == NODE_TYPE_EFFECT) {
        for (int i = 0; i < ty->n_effects; i++)
            printf(" !%s", ty->effect_names[i]);
    }
}

/* Check if function has a base case (if with return) */
static int has_base_case(Node *fn) {
    if (!fn->fn_body) return 0;
    for (int i = 0; i < fn->fn_body->stmts.len; i++) {
        Node *s = fn->fn_body->stmts.data[i];
        if (s->kind == NODE_IF && s->then_br) {
            /* check if then branch has a return */
            if (s->then_br->kind == NODE_BLOCK) {
                for (int j = 0; j < s->then_br->stmts.len; j++) {
                    if (s->then_br->stmts.data[j]->kind == NODE_RETURN)
                        return 1;
                }
            }
        }
    }
    return 0;
}

/* Count return statements */
static int count_returns(Node *block) {
    if (!block || block->kind != NODE_BLOCK) return 0;
    int count = 0;
    for (int i = 0; i < block->stmts.len; i++) {
        Node *s = block->stmts.data[i];
        if (s->kind == NODE_RETURN) count++;
        if (s->kind == NODE_IF) {
            count += count_returns(s->then_br);
            if (s->else_br) count += count_returns(s->else_br);
        }
        if (s->kind == NODE_WHILE) count += count_returns(s->while_body);
        if (s->kind == NODE_BLOCK) count += count_returns(s);
    }
    return count;
}

static Node *find_spec(Node *program, const char *fn_name) {
    for (int i = 0; i < program->items.len; i++) {
        Node *item = program->items.data[i];
        if (item->kind == NODE_SPEC && strcmp(item->spec_name, fn_name) == 0)
            return item;
    }
    return NULL;
}

void print_proofs(Node *program) {
    printf("/* Proof sketches — извлечени от Бага компилатора */\n\n");

    for (int i = 0; i < program->items.len; i++) {
        Node *item = program->items.data[i];
        if (item->kind != NODE_FN || !item->fn_body) continue;

        const char *name = item->fn_name;
        Node *spec = find_spec(program, name);

        /* Run the verifier once up front: its facts (termination, loop
         * invariants, ensures verdicts) feed the theorems below. */
        FnVerifyRes vr;
        int have_vr = 0;
        if (spec && verify_fn_collect(program, item, &vr) == 0) have_vr = 1;

        printf("proofs for %s:\n", name);

        /* 1. Signature theorem */
        printf("  theorem %s_signature:\n", name);
        printf("    ∀");
        for (int j = 0; j < item->params.len; j++) {
            Node *p = item->params.data[j];
            printf(" %s: %s", p->param_name, type_node_str(p->param_type));
            if (j < item->params.len - 1) printf(",");
        }
        printf(". %s(", name);
        for (int j = 0; j < item->params.len; j++) {
            Node *p = item->params.data[j];
            printf("%s", p->param_name);
            if (j < item->params.len - 1) printf(", ");
        }
        printf(") → %s", type_node_str(item->ret_type));
        if (has_effects(item)) print_effects(item->ret_type);
        printf("\n\n");

        /* 2. Termination theorem */
        int base = has_base_case(item);
        int rets = count_returns(item->fn_body);
        printf("  theorem %s_terminates:\n", name);
        printf("    ∀");
        for (int j = 0; j < item->params.len; j++) {
            Node *p = item->params.data[j];
            printf(" %s: %s", p->param_name, type_node_str(p->param_type));
            if (j < item->params.len - 1) printf(",");
        }
        printf(". terminates(%s(", name);
        for (int j = 0; j < item->params.len; j++) {
            printf("%s", item->params.data[j]->param_name);
            if (j < item->params.len - 1) printf(", ");
        }
        printf("))\n");
        if (have_vr && vr.partial && !vr.skipped) {
            if (vr.term && !vr.term_failed)
                printf("    evidence: recursion with decreases measure — proven statically (full correctness)\n");
            else
                printf("    evidence: recursion — partial correctness only; termination not proven\n");
        } else if (base) {
            printf("    evidence: base case with early return");
            if (rets > 1) printf(", %d return paths", rets);
            printf("\n");
        } else {
            printf("    evidence: %d return path(s), no obvious base case\n", rets > 0 ? rets : 1);
        }
        printf("\n");

        /* 3. Purity / effect theorem */
        if (has_effects(item)) {
            printf("  theorem %s_effects:\n", name);
            printf("    %s has effects:", name);
            print_effects(item->ret_type);
            printf("\n");
            printf("    callers must handle or propagate these effects\n\n");
        } else {
            printf("  theorem %s_pure:\n", name);
            printf("    %s is pure (no declared effects)\n\n", name);
        }

        /* 3b. M18: overflow-safety theorem — the !Overflow effect discharge */
        if (have_vr && vr.ovf_analyzed) {
            printf("  theorem %s_overflow_safe:\n", name);
            if (vr.ovf_safe) {
                printf("    ∀ inputs. no arithmetic in %s overflows i64\n", name);
                if (vr.ovf_declared)
                    printf("    status: ДОКАЗАНО (M18 — всички аритметични задължения; !Overflow е излишно)\n\n");
                else
                    printf("    status: ДОКАЗАНО (M18 — всички аритметични задължения; типът е точен)\n\n");
            } else if (vr.ovf_declared) {
                printf("    %s carries !Overflow — overflow is advertised in the type\n", name);
                if (vr.ovf_witness)
                    printf("    status: деклариран честно — прелива при %s (идеализиран ℤ модел)\n\n", vr.ovf_witness);
                else
                    printf("    status: деклариран честно — безопасността не е доказуема (идеализиран ℤ модел)\n\n");
            } else if (vr.ovf_res == 1) {
                printf("    ∀ inputs. no arithmetic in %s overflows i64\n", name);
                printf("    status: ОБРОЧЕНО — прелива при %s, а !Overflow не е деклариран\n\n",
                       vr.ovf_witness ? vr.ovf_witness : "(всеки вход)");
            } else {
                printf("    ∀ inputs. no arithmetic in %s overflows i64\n", name);
                printf("    status: НЕ МОГА ДА РЕША — декларирай !Overflow за честен тип\n\n");
            }
        }

        /* 4. Spec contracts — verified statically when possible */
        if (spec) {
            for (int j = 0; j < spec->n_guarantees; j++)
                printf("  guarantee %s_g%d:\n    %s\n\n", name, j + 1, spec->spec_guarantees[j]);

            if (spec->spec_ensures.len > 0) {
                if (have_vr) {
                    for (int j = 0; j < vr.n_ens; j++) {
                        EnsVerifyRes *e = &vr.ens[j];
                        printf("  theorem %s_ensures_%d:\n", name, j + 1);
                        if (spec->spec_requires.len > 0) {
                            printf("    requires:");
                            for (int r = 0; r < spec->spec_requires.len; r++)
                                printf(" %s%s", spec->spec_requires.data[r]->ensure_text,
                                       r < spec->spec_requires.len - 1 ? "," : "");
                            printf("\n");
                        }
                        printf("    ensures: %s\n", e->ens_text);
                        switch (e->res) {
                        case 0: printf("    status: ДОКАЗАНО (статично, Fourier–Motzkin)\n"); break;
                        case 1:
                            printf("    status: ОБРОЧЕНО\n");
                            printf("    контрапример:");
                            for (int k = 0; k < e->wn; k++)
                                printf(" %s = %lld", e->wit_names[k], e->wit_vals[k]);
                            printf("\n");
                            break;
                        case 2: printf("    status: НЕ МОГА ДА РЕША\n"); break;
                        case 3: printf("    status: ПРОПУСНАТО (%s)\n", e->skip_reason ? e->skip_reason : "извън фрагмента"); break;
                        }
                        printf("\n");
                    }
                } else {
                    for (int j = 0; j < spec->spec_ensures.len; j++) {
                        printf("  theorem %s_ensures_%d:\n", name, j + 1);
                        printf("    ensures: %s\n", spec->spec_ensures.data[j]->ensure_text);
                        printf("    status: RUNTIME-CHECKED\n\n");
                    }
                }
            } else if (spec->n_guarantees == 0) {
                printf("  spec \"%s\": UNVERIFIED — no formal contracts\n\n", spec->spec_name);
            }

            /* 5. Loop invariants — facts established by the Hoare rule (M1) */
            if (have_vr) {
                for (int k = 0; k < vr.n_inv; k++) {
                    printf("  lemma %s_invariant_%d:\n", name, k + 1);
                    printf("    invariant: %s\n", vr.inv_texts[k]);
                    if (vr.inv_proven[k])
                        printf("    status: ДОКАЗАНО (init + preservation, Hoare)\n\n");
                    else
                        printf("    status: НЕ Е ДОКАЗАНА — надолу по веригата е UNKNOWN\n\n");
                }
            }
        }
        if (have_vr) fn_verify_res_free(&vr);
    }
}
