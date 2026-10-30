/* verify_obl.c — част от verify.c (виж verify_proto.h). */
#include "baga.h"
#include "verify_impl.h"

/* ============================ counterexample search ============================ */


int64_t ienv_get(IEnv *e, const char *name, int *ok) {
    for (int i = 0; i < e->n; i++) if (strcmp(e->b[i].name, name) == 0) { *ok = 1; return e->b[i].val; }
    *ok = 0; return 0;
}

int64_t eval_i64(Node *e, IEnv *env, int *ok) {
    *ok = 1;
    if (!e) { *ok = 0; return 0; }
    switch (e->kind) {
    case NODE_INT_LIT: return e->int_val;
    case NODE_IDENT: return ienv_get(env, e->name, ok);
    case NODE_UNARY:
        if (e->un_op == UOP_NEG) { int o; int64_t v = eval_i64(e->operand, env, &o); *ok = o; return -*ok ? 0 : -v; }
        if (e->un_op == UOP_NOT) { int o; int64_t v = eval_bool(e->operand, env); *ok = 1; (void)o; return !v; }
        *ok = 0; return 0;
    case NODE_BINARY: {
        int lo, ro;
        int64_t l = eval_i64(e->left, env, &lo);
        int64_t r = eval_i64(e->right, env, &ro);
        if (!lo || !ro) { *ok = 0; return 0; }
        switch (e->bin_op) {
        case OP_ADD: return l + r;
        case OP_SUB: return l - r;
        case OP_MUL: return l * r;
        case OP_DIV: if (r == 0) { *ok = 0; return 0; } return l / r;
        case OP_MOD: if (r == 0) { *ok = 0; return 0; } return l % r;
        /* M31: bitwise с маскиран брой (b & 63) — семантиката на езика в
         * двата backend-а. Преди това recheck на witness/ensures с битови
         * операции връщаше ok=0 (мълчалив UNKNOWN); BV oracle-ът разчита
         * на тях за презареждането. */
        case OP_BIT_AND: return l & r;
        case OP_BIT_OR:  return l | r;
        case OP_BIT_XOR: return l ^ r;
        case OP_LSHIFT:  return (int64_t)((uint64_t)l << (r & 63));
        case OP_RSHIFT:  return l >> (r & 63);
        case OP_URSHIFT: return (int64_t)((uint64_t)l >> (r & 63));
        default: *ok = 0; return 0;
        }
    }
    default: *ok = 0; return 0;
    }
}

int eval_bool(Node *e, IEnv *env) {
    if (!e) return 0;
    if (e->kind == NODE_BOOL_LIT) return e->bool_val;
    if (e->kind == NODE_UNARY && e->un_op == UOP_NOT) return !eval_bool(e->operand, env);
    if (e->kind == NODE_BINARY) {
        if (is_cmp(e->bin_op) || e->bin_op == OP_EQ || e->bin_op == OP_NEQ) {
            int lo, ro;
            int64_t l = eval_i64(e->left, env, &lo);
            int64_t r = eval_i64(e->right, env, &ro);
            if (!lo || !ro) return 0;
            switch (e->bin_op) {
            case OP_LT: return l < r;
            case OP_LE: return l <= r;
            case OP_GT: return l > r;
            case OP_GE: return l >= r;
            case OP_EQ: return l == r;
            case OP_NEQ: return l != r;
            default: return 0;
            }
        }
        if (e->bin_op == OP_AND) return eval_bool(e->left, env) && eval_bool(e->right, env);
        if (e->bin_op == OP_OR) return eval_bool(e->left, env) || eval_bool(e->right, env);
    }
    return 0;
}

/* Collect the free variables of a constraint system (for witness search). */
void collect_vars_cl(ConsList *cl, char ***vars, int *nv, int *cap) {
    for (int i = 0; i < cl->n; i++) for (int j = 0; j < cl->c[i].lhs.n; j++) {
        const char *vn = cl->c[i].lhs.terms[j].var;
        int found = 0;
        for (int k = 0; k < *nv; k++) if (strcmp((*vars)[k], vn) == 0) { found = 1; break; }
        if (!found) { if (*nv == *cap) { *cap = *cap ? *cap * 2 : 8; *vars = realloc(*vars, (size_t)*cap * sizeof(char *)); } (*vars)[(*nv)++] = strdup(vn); }
    }
}

/* Concrete value of a recorded product/div/mod/bit entry (M8–M20). */
int prod_concrete(ProdEntry *p, int64_t a, int64_t b, int64_t *out) {
    if (p->is_bitand1) {
        int64_t m = (lin_is_const(&p->fb) && p->fb.c.den == 1) ? p->fb.c.num : 1;
        *out = a & m;
        return 1;
    }
    if (p->is_bitand) { *out = a & b; return 1; }
    if (p->is_bitor) { *out = a | b; return 1; }
    if (p->is_div) { if (b == 0) return 0; *out = a / b; return 1; }
    if (p->is_mod) { if (b == 0) return 0; *out = a % b; return 1; }
    /* M22: аритметично отместване — b е броячът k (не делител). */
    if (p->is_shr) { *out = baga_ashr_i64(a, b); return 1; }
    /* M23: логическо отместване — b е броячът k. */
    if (p->is_lshr) { *out = baga_lshr_i64(a, b); return 1; }
    if (__builtin_mul_overflow(a, b, out)) return 0;
    return 1;
}

/* M8/M9/M13: pin every non-abstract var (__c/__p/__d/__m/__b and M15's
 * __hv/__hvl havoc vars excluded) to its candidate value in sys; then derive
 * every product/div/mod/bit var whose factors are fully pinned and pin it
 * too. Two passes so products of products resolve. */
void push_pins_and_derived(ConsList *sys, char **vars, IBind *assign, int nv, ProdList *prods) {
    int pcap = nv + (prods ? prods->n : 0) + 1;
    IBind *pins = calloc((size_t)pcap, sizeof(IBind));
    int np = 0;
    for (int i = 0; i < nv; i++) {
        if (strncmp(vars[i], "__c", 3) == 0 || strncmp(vars[i], "__p", 3) == 0 ||
            strncmp(vars[i], "__d", 3) == 0 || strncmp(vars[i], "__m", 3) == 0 ||
            strncmp(vars[i], "__b", 3) == 0 || strncmp(vars[i], "__hv", 4) == 0) continue;
        pins[np].name = vars[i]; pins[np].val = assign[i].val; np++;
        Lin l = lin_var(vars[i]);
        l.c = rat_sub(l.c, rat_int(assign[i].val));
        cl_push(sys, mk_cons(l, C_LE, rat_zero()));
        Lin l2 = lin_var(vars[i]);
        Lin nl2 = lin_neg(&l2);
        nl2.c = rat_add(nl2.c, rat_int(assign[i].val));
        cl_push(sys, mk_cons(nl2, C_LE, rat_zero()));
    }
    for (int pass = 0; pass < 2; pass++) {
        for (int pi = 0; prods && pi < prods->n; pi++) {
            int have = 0;
            for (int k = 0; k < np; k++) if (strcmp(pins[k].name, prods->p[pi].var) == 0) { have = 1; break; }
            if (have) continue;
            IEnv pe; pe.b = pins; pe.n = np;
            int64_t fv[2] = {0, 0};
            Lin *fs[2] = { &prods->p[pi].fa, &prods->p[pi].fb };
            int ok = 1;
            /* bitand1 only needs fa; still evaluate both for the shared path */
            int nq = prods->p[pi].is_bitand1 ? 1 : 2;
            for (int q = 0; q < nq && ok; q++) {
                Rat rv = fs[q]->c;
                for (int j = 0; j < fs[q]->n; j++) {
                    int ok2; int64_t vv = ienv_get(&pe, fs[q]->terms[j].var, &ok2);
                    if (!ok2) { ok = 0; break; }
                    rv = rat_add(rv, rat_mul(fs[q]->terms[j].coeff, rat_int(vv)));
                }
                if (ok && (rv.overflow || rv.den != 1)) ok = 0;
                if (ok) fv[q] = rv.num;
            }
            if (!ok) continue;
            int64_t pvv;
            if (!prod_concrete(&prods->p[pi], fv[0], fv[1], &pvv)) continue;
            pins[np].name = prods->p[pi].var; pins[np].val = pvv; np++;
            Lin l = lin_var(prods->p[pi].var);
            l.c = rat_sub(l.c, rat_int(pvv));
            cl_push(sys, mk_cons(l, C_LE, rat_zero()));
            Lin l2 = lin_var(prods->p[pi].var);
            Lin nl2 = lin_neg(&l2);
            nl2.c = rat_add(nl2.c, rat_int(pvv));
            cl_push(sys, mk_cons(nl2, C_LE, rat_zero()));
        }
    }
    free(pins);
}

/* Try to find an integral witness satisfying all `ante` and violating `ens`.
 * Returns 1 and fills witness (caller frees names) on success.
 * M8 conclusiveness: a witness is accepted only if the violation does not
 * depend on the chosen values of call/product fresh vars (__c/__p) — those
 * are determined by the inputs through the callee, but the verifier knows
 * only the assumed ensures about them, so a violation seen only at an
 * unrealizable value would be a false alarm. All other vars (inputs, locals,
 * __r reads, __len lengths) are pinned to the candidate: read/length values
 * are genuinely free (realizable by some vector contents). The check:
 * with the pins, every positive-ensures branch (`ef`) is UNSAT under ante.
 * Sound: the concrete execution's internal values satisfy ante (every path
 * constraint is a true fact), so "all completions violate" ⇒ concrete run
 * violates. */
int find_counterexample(ConsList *ante, Formula *nef, Formula *ef, Node *ensures_ast,
                               SEnv *output_env, ProdList *prods, IBind **witness, int *wn) {
    char **vars = NULL; int nv = 0, cap = 0;
    collect_vars_cl(ante, &vars, &nv, &cap);
    for (int b = 0; b < nef->n; b++) collect_vars_cl(&nef->br[b], &vars, &nv, &cap);
    /* M8b: the product factors' vars must be enumerable to derive products */
    if (prods) for (int pi = 0; pi < prods->n; pi++) {
        Lin *fs[2] = { &prods->p[pi].fa, &prods->p[pi].fb };
        for (int q = 0; q < 2; q++) for (int j = 0; j < fs[q]->n; j++) {
            const char *vn = fs[q]->terms[j].var;
            int found = 0;
            for (int k = 0; k < nv; k++) if (strcmp(vars[k], vn) == 0) { found = 1; break; }
            if (!found) { if (nv == cap) { cap = cap ? cap * 2 : 8; vars = realloc(vars, (size_t)cap * sizeof(char *)); } vars[nv++] = strdup(vn); }
        }
    }
    /* output is derived, not free — drop it */
    for (int i = 0; i < nv; i++) if (strcmp(vars[i], "output") == 0) { free(vars[i]); vars[i] = vars[nv - 1]; nv--; i--; }

    static const int64_t cand[] = { 0, 1, -1, 2, -2, 3, -3, 5, -5, 10, -10, 20, -20, 100, -100 };
    int ncand = (int)(sizeof(cand) / sizeof(cand[0]));

    int total = 1;
    for (int i = 0; i < nv; i++) { if (total > 200000 / ncand) { total = 200001; break; } total *= ncand; }
    if (nv == 0) total = 1;

    IBind *assign = calloc(nv ? nv : 1, sizeof(IBind));
    int found = 0;
    for (int idx = 0; idx < total && !found; idx++) {
        int t = idx;
        for (int i = 0; i < nv; i++) { assign[i].name = vars[i]; assign[i].val = cand[t % ncand]; t /= ncand; }
        /* M8b: product vars are derived, not free — evaluate both factors
         * under the candidate and set the product concretely. A candidate
         * whose product cannot be derived (missing var / overflow) is skipped. */
        int prod_ok = 1;
        for (int pi = 0; prods && pi < prods->n && prod_ok; pi++) {
            IEnv ie0; ie0.b = assign; ie0.n = nv;
            int64_t fv[2] = {0, 0};
            Lin *fs[2] = { &prods->p[pi].fa, &prods->p[pi].fb };
            int nq = prods->p[pi].is_bitand1 ? 1 : 2;
            for (int q = 0; q < nq && prod_ok; q++) {
                Rat rv = fs[q]->c;
                for (int j = 0; j < fs[q]->n; j++) {
                    int ok2; int64_t vv = ienv_get(&ie0, fs[q]->terms[j].var, &ok2);
                    if (!ok2) { prod_ok = 0; break; }
                    rv = rat_add(rv, rat_mul(fs[q]->terms[j].coeff, rat_int(vv)));
                }
                if (prod_ok && (rv.overflow || rv.den != 1)) prod_ok = 0;
                if (prod_ok) fv[q] = rv.num;
            }
            if (!prod_ok) break;
            int64_t pvv;
            if (prods->p[pi].is_bitand1) {
                pvv = fv[0] & 1;
            } else if (prods->p[pi].is_div) {
                if (fv[1] == 0) { prod_ok = 0; break; }
                pvv = fv[0] / fv[1];
            } else if (prods->p[pi].is_mod) {
                if (fv[1] == 0) { prod_ok = 0; break; }
                pvv = fv[0] % fv[1];
            } else if (prods->p[pi].is_shr) {
                /* M22: аритметично отместване — fv[1] е броячът k */
                pvv = baga_ashr_i64(fv[0], fv[1]);
            } else if (prods->p[pi].is_lshr) {
                /* M23: логическо отместване — fv[1] е броячът k */
                pvv = baga_lshr_i64(fv[0], fv[1]);
            } else {
                if (__builtin_mul_overflow(fv[0], fv[1], &pvv)) { prod_ok = 0; break; }
            }
            for (int i = 0; i < nv; i++)
                if (strcmp(vars[i], prods->p[pi].var) == 0) { assign[i].val = pvv; break; }
        }
        if (!prod_ok) continue;
        IEnv ie; ie.b = assign; ie.n = nv;
        /* evaluate antecedent constraints via their linear forms */
        int ante_ok = 1;
        for (int i = 0; i < ante->n && ante_ok; i++) {
            Constraint *c = &ante->c[i];
            Rat rv = c->lhs.c;
            for (int j = 0; j < c->lhs.n; j++) {
                int ok2; int64_t vv = ienv_get(&ie, c->lhs.terms[j].var, &ok2);
                if (!ok2) { ante_ok = 0; break; }
                rv = rat_add(rv, rat_mul(c->lhs.terms[j].coeff, rat_int(vv)));
            }
            if (rv.overflow) { ante_ok = 0; break; }
            int s = rat_cmp(rv, c->rhs);
            ante_ok = (c->op == C_LT) ? (s < 0) : (s <= 0);
        }
        if (!ante_ok) continue;
        /* evaluate ensures with output bound */
        IBind *a2 = calloc(nv + 1, sizeof(IBind));
        for (int i = 0; i < nv; i++) a2[i] = assign[i];
        Binding *ob = env_find(output_env, "output");
        int out_ok = 1;
        int64_t outv = 0;
        if (ob && !ob->nonlinear) {
            Rat rv = ob->lin.c;
            for (int j = 0; j < ob->lin.n; j++) {
                int ok3; int64_t vv = ienv_get(&ie, ob->lin.terms[j].var, &ok3);
                if (!ok3) { out_ok = 0; break; }
                rv = rat_add(rv, rat_mul(ob->lin.terms[j].coeff, rat_int(vv)));
            }
            if (rv.overflow || rv.den != 1) out_ok = 0; else outv = rv.num;
        } else out_ok = 0;
        if (out_ok) {
            a2[nv].name = "output"; a2[nv].val = outv;
            IEnv ie3; ie3.b = a2; ie3.n = nv + 1;
            int ev = eval_bool(ensures_ast, &ie3);
            if (!ev) {
                /* M8: accept only conclusive witnesses (see above) */
                int conclusive = 1;
                for (int b2 = 0; b2 < ef->n && conclusive; b2++) {
                    ConsList sys; cl_init(&sys);
                    for (int x = 0; x < ante->n; x++) { Constraint *c = &ante->c[x]; cl_push(&sys, mk_cons(lin_clone(&c->lhs), c->op, c->rhs)); }
                    push_pins_and_derived(&sys, vars, assign, nv, prods);
                    for (int x = 0; x < ef->br[b2].n; x++) { Constraint *c = &ef->br[b2].c[x]; cl_push(&sys, mk_cons(lin_clone(&c->lhs), c->op, c->rhs)); }
                    if (fm_sat(&sys)) conclusive = 0;   /* frees sys */
                }
                if (!conclusive) { free(a2); continue; }
                IBind *w = calloc(nv ? nv : 1, sizeof(IBind));
                int k = 0;
                for (int i = 0; i < nv; i++) {
                    if (strncmp(vars[i], "__", 2) == 0) continue;   /* internal fresh vars */
                    w[k].name = strdup(vars[i]); w[k].val = assign[i].val; k++;
                }
                *witness = w; *wn = k;
                found = 1;
            }
        }
        free(a2);
    }
    free(assign);
    for (int i = 0; i < nv; i++) free(vars[i]);
    free(vars);
    return found;
}

/* kind: 0 = ensures (postcondition), 1 = bounds (vec access in range).
 * vlen snapshots the vector lengths at the obligation point, so requires and
 * ensures mentioning vec_len resolve against the right state.
 * For bounds obligations: path = state path (requires ∧ branch path);
 * bound = the negated in-range condition (¬(0 <= idx < len)). */
void prods_free(ProdList *p);   /* defined below */
void obl_push(Obligations *ob, ConsList path, ConsList bound, ConsList read_cons, Sym ret, VLenMap *vlen, ReadsList *reads, int bad, int kind, const char *label) {
    if (ob->n == ob->cap) { ob->cap = ob->cap ? ob->cap * 2 : 4; ob->o = realloc(ob->o, (size_t)ob->cap * sizeof(Obligation)); }
    ob->o[ob->n].path = path; ob->o[ob->n].bound = bound; ob->o[ob->n].read_cons = read_cons; ob->o[ob->n].ret = ret;
    if (vlen) vlen_clone_into(&ob->o[ob->n].vlen, vlen); else vlen_init(&ob->o[ob->n].vlen);
    if (reads) prods_clone_into(&ob->o[ob->n].prods, reads);
    else { ob->o[ob->n].prods.p = NULL; ob->o[ob->n].prods.n = 0; ob->o[ob->n].prods.cap = 0; }
    ob->o[ob->n].bad = bad;
    ob->o[ob->n].kind = kind; ob->o[ob->n].label = label ? strdup(label) : NULL;
    ob->o[ob->n].akind = 0; lin_init(&ob->o[ob->n].aux1); lin_init(&ob->o[ob->n].aux2);
    ob->n++;
}

/* M15: obligation free helper (all owned fields). */
void obl_free(Obligation *o) {
    cl_free(&o->path); cl_free(&o->bound); cl_free(&o->read_cons);
    lin_free(&o->ret.lin); vlen_free(&o->vlen); prods_free(&o->prods);
    lin_free(&o->aux1); lin_free(&o->aux2);
    free(o->label);
}

/* Witness search for a bounds obligation: an assignment satisfying the
 * antecedent (obl->path = requires ∧ path ∧ ¬bound) where the access index
 * (obl->ret) really is out of range. Re-checked by direct evaluation, so a
 * reported counterexample is genuine. */
int find_bound_witness(Obligation *obl, IBind **witness, int *wn) {
    char **vars = NULL; int nv = 0, cap = 0;
    collect_vars_cl(&obl->path, &vars, &nv, &cap);
    /* also search the symbolic __len_ vars (Vec parameters) */
    for (int i = 0; i < obl->vlen.n; i++)
        for (int j = 0; j < obl->vlen.e[i].len.n; j++) {
            const char *vn = obl->vlen.e[i].len.terms[j].var;
            int found = 0;
            for (int k = 0; k < nv; k++) if (strcmp(vars[k], vn) == 0) { found = 1; break; }
            if (!found) { if (nv == cap) { cap = cap ? cap * 2 : 8; vars = realloc(vars, (size_t)cap * sizeof(char *)); } vars[nv++] = strdup(vn); }
        }
    static const int64_t cand[] = { 0, 1, -1, 2, -2, 3, -3, 5, -5, 10, -10, 20, -20, 100, -100 };
    int ncand = (int)(sizeof(cand) / sizeof(cand[0]));
    int total = 1;
    for (int i = 0; i < nv; i++) { if (total > 200000 / ncand) { total = 200001; break; } total *= ncand; }
    if (nv == 0) total = 1;
    IBind *assign = calloc(nv ? nv : 1, sizeof(IBind));
    int found = 0;
    for (int idx = 0; idx < total && !found; idx++) {
        int t = idx;
        for (int i = 0; i < nv; i++) { assign[i].name = vars[i]; assign[i].val = cand[t % ncand]; t /= ncand; }
        IEnv ie; ie.b = assign; ie.n = nv;
        int ante_ok = 1;
        for (int i = 0; i < obl->path.n && ante_ok; i++) {
            Constraint *c = &obl->path.c[i];
            Rat rv = c->lhs.c;
            for (int j = 0; j < c->lhs.n; j++) {
                int ok2; int64_t vv = ienv_get(&ie, c->lhs.terms[j].var, &ok2);
                if (!ok2) { ante_ok = 0; break; }
                rv = rat_add(rv, rat_mul(c->lhs.terms[j].coeff, rat_int(vv)));
            }
            if (rv.overflow) { ante_ok = 0; break; }
            int s = rat_cmp(rv, c->rhs);
            ante_ok = (c->op == C_LT) ? (s < 0) : (s <= 0);
        }
        if (!ante_ok) continue;
        /* evaluate the index linear form directly */
        int idxok; int64_t idxv = 0;
        Rat ir = obl->ret.lin.c;
        for (int j = 0; j < obl->ret.lin.n; j++) {
            int ok3; int64_t vv = ienv_get(&ie, obl->ret.lin.terms[j].var, &ok3);
            if (!ok3) { idxok = 0; goto bound_done; }
            ir = rat_add(ir, rat_mul(obl->ret.lin.terms[j].coeff, rat_int(vv)));
        }
        idxok = (!ir.overflow && ir.den == 1);
        idxv = ir.num;
        bound_done:
        if (!idxok) continue;
        int oob = (idxv < 0);
        if (!oob) {
            /* evaluate the accessed vector's length (find the vlen entry whose
             * length vars appear in the antecedent — that's the accessed vec) */
            for (int e = 0; e < obl->vlen.n && !oob; e++) {
                Lin *L = &obl->vlen.e[e].len;
                int relevant = (L->n == 0);   /* a concrete (constant) length always applies */
                for (int j = 0; j < L->n && !relevant; j++)
                    for (int x = 0; x < obl->path.n; x++) {
                        Constraint *c = &obl->path.c[x];
                        for (int y = 0; y < c->lhs.n; y++)
                            if (strcmp(c->lhs.terms[y].var, L->terms[j].var) == 0) { relevant = 1; break; }
                        if (relevant) break;
                    }
                if (!relevant) continue;
                Rat lr = L->c; int lok = 1;
                for (int j = 0; j < L->n; j++) {
                    int ok4; int64_t vv = ienv_get(&ie, L->terms[j].var, &ok4);
                    if (!ok4) { lok = 0; break; }
                    lr = rat_add(lr, rat_mul(L->terms[j].coeff, rat_int(vv)));
                }
                if (lok && !lr.overflow && lr.den == 1) oob = (idxv >= lr.num);
            }
        }
        if (oob) {
            /* M8: ако индексът зависи от __c/__p (абстрактни call/product
             * резултати), изискваме conclusive нарушение — при pinned
             * входове/read-ове НИКОЯ стойност на абстрактните променливи не
             * трябва да връща достъпа в границите, иначе е фалшива тревога. */
            int needs_concl = 0;
            for (int j = 0; j < obl->ret.lin.n; j++)
                if (strncmp(obl->ret.lin.terms[j].var, "__c", 3) == 0 ||
                    strncmp(obl->ret.lin.terms[j].var, "__p", 3) == 0 ||
                    strncmp(obl->ret.lin.terms[j].var, "__d", 3) == 0 ||
                    strncmp(obl->ret.lin.terms[j].var, "__m", 3) == 0 ||
                    strncmp(obl->ret.lin.terms[j].var, "__b", 3) == 0) needs_concl = 1;
            if (needs_concl) {
                ConsList sys; cl_init(&sys);
                for (int x = 0; x < obl->path.n; x++) { Constraint *c = &obl->path.c[x]; cl_push(&sys, mk_cons(lin_clone(&c->lhs), c->op, c->rhs)); }
                push_pins_and_derived(&sys, vars, assign, nv, &obl->prods);
                for (int x = 0; x < obl->bound.n; x++) { Constraint *c = &obl->bound.c[x]; cl_push(&sys, mk_cons(lin_clone(&c->lhs), c->op, c->rhs)); }
                if (fm_sat(&sys)) continue;   /* не е conclusive — търси друг */
            }
            IBind *w = calloc(nv ? nv : 1, sizeof(IBind));
            int k = 0;
            for (int i = 0; i < nv; i++) {
                if (strncmp(vars[i], "__", 2) == 0) continue;   /* internal fresh vars */
                w[k].name = strdup(vars[i]); w[k].val = assign[i].val; k++;
            }
            *witness = w; *wn = k;
            found = 1;
        }
    }
    free(assign);
    for (int i = 0; i < nv; i++) free(vars[i]);
    free(vars);
    return found;
}

/* ======================= M15: arithmetic safety =======================
 * The verifier reasons in idealized ℤ; the runtime is i64. This bridge
 * emits one kind-4 obligation per arithmetic operation: prove the result
 * cannot overflow i64 on this path (and no division by zero / INT64_MIN/-1),
 * refute it with a concrete witness, or stay honestly UNKNOWN. When every
 * arith obligation of a function is PROVEN, the idealized model and the
 * runtime coincide — the ensures verdicts become unconditional. */


static void push_arith_obl(State *st, Obligations *ob, int akind, Lin *a1, Lin *a2, const char *label) {
    ConsList opath; cl_init(&opath);
    for (int x = 0; x < st->path.n; x++) { Constraint *c = &st->path.c[x]; cl_push(&opath, mk_cons(lin_clone(&c->lhs), c->op, c->rhs)); }
    ConsList empty; cl_init(&empty);
    obl_push(ob, opath, empty, (ConsList){NULL, 0, 0}, sym_lin(lin_const(rat_int(0))), &st->vlen, &st->reads, st->bad, 4, label);
    Obligation *o = &ob->o[ob->n - 1];
    o->akind = akind;
    if (a1) { lin_free(&o->aux1); o->aux1 = *a1; }   /* moves */
    if (a2) { lin_free(&o->aux2); o->aux2 = *a2; }
}

/* Recursive scan: every arithmetic node in runtime code gets an obligation.
 * Nonlinear subvalues are skipped — the value is already honestly opaque
 * upstream, so no arith claim is made about it either. */
void scan_arith_expr(Node *e, State *st, Obligations *ob) {
    if (!e) return;
    switch (e->kind) {
    case NODE_UNARY:
        scan_arith_expr(e->operand, st, ob);
        if (e->un_op == UOP_NEG) {
            Sym v = se_from_ast(e->operand, &st->env, &st->vlen, &st->reads);
            if (!v.nonlinear) {
                char eb[512]; size_t off = 0; eb[0] = '\0';
                expr_render(e, eb, sizeof eb, &off);
                char lbl[600]; snprintf(lbl, sizeof lbl, "преливане: %s", eb);
                Lin nl = lin_neg(&v.lin);
                lin_free(&v.lin);
                push_arith_obl(st, ob, AK_FIT, &nl, NULL, lbl);
            } else lin_free(&v.lin);
        }
        return;
    case NODE_CALL:
        for (int i = 0; i < e->args.len; i++) scan_arith_expr(e->args.data[i], st, ob);
        return;
    case NODE_ASSIGN:
        scan_arith_expr(e->assign_val, st, ob);
        return;
    case NODE_BINARY: break;
    default: return;
    }
    scan_arith_expr(e->left, st, ob);
    scan_arith_expr(e->right, st, ob);
    BinOp op = e->bin_op;
    if (op != OP_ADD && op != OP_SUB && op != OP_MUL && op != OP_DIV &&
        op != OP_MOD && op != OP_LSHIFT) return;
    Sym l = se_from_ast(e->left, &st->env, &st->vlen, &st->reads);
    Sym r = se_from_ast(e->right, &st->env, &st->vlen, &st->reads);
    if (l.nonlinear || r.nonlinear) { lin_free(&l.lin); lin_free(&r.lin); return; }
    char eb[512]; size_t off = 0; eb[0] = '\0';
    expr_render(e, eb, sizeof eb, &off);
    char lbl[600];

    if (op == OP_DIV || op == OP_MOD) {
        snprintf(lbl, sizeof lbl, "деление: %s", eb);
        push_arith_obl(st, ob, AK_DIVZ, &l.lin, &r.lin, lbl);   /* moves both */
        return;
    }
    if (op == OP_MUL && !lin_is_const(&l.lin) && !lin_is_const(&r.lin)) {
        snprintf(lbl, sizeof lbl, "преливане: %s", eb);
        push_arith_obl(st, ob, AK_MUL, &l.lin, &r.lin, lbl);    /* moves both */
        return;
    }
    /* linear result forms: a+b, a-b, const*a, n<<k */
    Lin res; int have = 0;
    if (op == OP_ADD) { res = lin_add(&l.lin, &r.lin); have = 1; }
    else if (op == OP_SUB) { res = lin_sub(&l.lin, &r.lin); have = 1; }
    else if (op == OP_MUL) {   /* exactly one side constant */
        if (lin_is_const(&l.lin) && lin_is_const(&r.lin)) {
            res = lin_const(rat_mul(l.lin.c, r.lin.c)); have = 1;
        } else if (lin_is_const(&l.lin)) { res = lin_scale(&r.lin, l.lin.c); have = 1; }
        else if (lin_is_const(&r.lin)) { res = lin_scale(&l.lin, r.lin.c); have = 1; }
    } else if (op == OP_LSHIFT) {
        if (lin_is_const(&r.lin) && r.lin.c.den == 1 &&
            r.lin.c.num >= 0 && r.lin.c.num <= 62) {
            res = lin_scale(&l.lin, rat_int((int64_t)1 << r.lin.c.num)); have = 1;
        }
    }
    lin_free(&l.lin); lin_free(&r.lin);
    if (!have) return;
    if (res.overflow) {   /* constant arithmetic overflowed i64 — unconditional */
        snprintf(lbl, sizeof lbl, "преливане: %s (константно)", eb);
        Lin poison = lin_const(rat_bad());
        push_arith_obl(st, ob, AK_FIT, &poison, NULL, lbl);
        return;
    }
    if (lin_is_const(&res)) { lin_free(&res); return; }   /* exact const, fits */
    snprintf(lbl, sizeof lbl, "преливане: %s", eb);
    push_arith_obl(st, ob, AK_FIT, &res, NULL, lbl);            /* moves res */
}

/* M6: entry obligation for a decreases measure — requires ⇒ D >= 0.
 * Pushed as a kind-2 obligation (same discharge/witness machinery as
 * call-site requires); used in both the collect run and the print re-run. */
void push_term_entry_obl(Node *fn, Node *spec, State *init, Obligations *ob) {
    if (!spec->spec_decreases) return;
    ConsList opath; cl_init(&opath);
    for (int x = 0; x < init->path.n; x++) { Constraint *c = &init->path.c[x]; cl_push(&opath, mk_cons(lin_clone(&c->lhs), c->op, c->rhs)); }
    ConsList pos; cl_init(&pos);
    char label[300];
    Sym d = se_from_ast(spec->spec_decreases, &init->env, &init->vlen, NULL);
    if (d.nonlinear) {
        snprintf(label, sizeof label, "терминация: decreases на '%s' не е линеен", fn->fn_name);
        obl_push(ob, opath, pos, (ConsList){NULL, 0, 0}, sym_lin(lin_const(rat_int(0))), &init->vlen, &init->reads, 1, 2, label);
        g_term_failed = 1;
    } else {
        /* D >= 0  ⟺  -D <= 0 */
        snprintf(label, sizeof label, "терминация: decreases на '%s' >= 0 при входа", fn->fn_name);
        cl_push(&pos, mk_cons(lin_neg(&d.lin), C_LE, rat_zero()));
        if (!cl_implied_by(&opath, &pos)) g_term_failed = 1;
        obl_push(ob, opath, pos, (ConsList){NULL, 0, 0}, sym_lin(lin_const(rat_int(0))), &init->vlen, &init->reads, 0, 2, label);
    }
    lin_free(&d.lin);
}

/* Symbolically execute a statement list over a set of states; returning states
 * are drained into `ob`; `states` holds the fall-through states at the end. */
void symexec_stmts(NodeVec *stmts, States *states, Obligations *ob, int is_nonvoid, Node *spec);

void symexec_block(Node *blk, States *states, Obligations *ob, int is_nonvoid, Node *spec) {
    if (!blk) return;
    if (blk->kind == NODE_BLOCK) { symexec_stmts(&blk->stmts, states, ob, is_nonvoid, spec); return; }
    /* single-statement branch */
    NodeVec one; one.data = &blk; one.len = 1; one.cap = 1;
    symexec_stmts(&one, states, ob, is_nonvoid, spec);
}

State clone_state_with(State *cur, Formula *add) {
    State t;
    env_clone_into(&t.env, &cur->env);
    vlen_clone_into(&t.vlen, &cur->vlen);
    ax_clone_into(&t.ax, &cur->ax);
    reads_clone_into(&t.reads, &cur->reads);
    handles_clone_into(&t.handles, &cur->handles);
    cl_init(&t.path);
    for (int x = 0; x < cur->path.n; x++) { Constraint *c = &cur->path.c[x]; cl_push(&t.path, mk_cons(lin_clone(&c->lhs), c->op, c->rhs)); }
    if (add) for (int b = 0; b < add->n; b++) for (int x = 0; x < add->br[b].n; x++) {
        Constraint *c = &add->br[b].c[x]; cl_push(&t.path, mk_cons(lin_clone(&c->lhs), c->op, c->rhs));
    }
    cl_init(&t.read_cons);
    for (int x = 0; x < cur->read_cons.n; x++) { Constraint *c = &cur->read_cons.c[x]; cl_push(&t.read_cons, mk_cons(lin_clone(&c->lhs), c->op, c->rhs)); }
    t.bad = cur->bad;
    return t;
}

static void bind_sym(SEnv *env, const char *name, Sym v) {
    if (v.nonlinear) { Lin empty; lin_init(&empty); env_bind(env, name, empty, 1); }
    else env_bind(env, name, v.lin, 0);
}

/* W2.6: транзитивно отчитане на делегираните споделби. Работник, получил
 * ЗАЕМАН свят (requires: world_frozen) и делегирал го надолу, дължи join преди
 * края си — иначе внук-читателят надживява join-а на извикващия и неговият
 * world_free съблича региона под все още работещия внук. Проверката е на всеки
 * изход и per-state, значи е path-sensitive и не измисля: join само в един клон
 * ⇒ само другият клон е обречен. */
static void check_unjoined_delegations(State *st, Obligations *ob) {
    if (st->bad) return;
    for (int k = 0; k < st->handles.n; k++) {
        HandleEntry *h = &st->handles.h[k];
        if (h->kind != HK_WORLD || !h->borrowed || h->n_shared <= 0) continue;
        char label[300];
        snprintf(label, sizeof label,
            "делегиран свят '%s' не е join-нат преди края на работника — внук-читател може да го надживее",
            h->var);
        push_protocol_violation(st, ob, label);
        return;   /* едно обвинение на изход е достатъчно */
    }
}

/* W2.6: предпазна мрежа за състояния, които НЕ са минали през return —
 * drain_returns нулира states->n, значи тук остават само пътищата, където
 * функцията пада от края (void тяло). Днес е недоказуемо необходима: всяка
 * делегираща функция е non-void, защото `go`/`go_bg` носят !Par, а ефект без
 * return тип не се парсва — значи изходът винаги минава през drain_returns
 * (изричен return или последен израз). Оставяме я, за да не зависи
 * инвариантът „делегацията е отчетена на всеки изход" от това случайно
 * ограничение на парсера. */
void push_unjoined_delegation_obls(States *states, Obligations *ob) {
    for (int i = 0; i < states->n; i++) check_unjoined_delegations(&states->s[i], ob);
}

static void drain_returns(States *states, Obligations *ob, Node *val_expr) {    for (int i = 0; i < states->n; i++) {
        check_unjoined_delegations(&states->s[i], ob);   /* W2.6 */
        Sym r = se_from_ast(val_expr, &states->s[i].env, &states->s[i].vlen, &states->s[i].reads);
        /* W3.1: гаранция за върнат свят — всеки върнат handle се проверява
         * СРЕЩУ обещаното състояние по този път. Името идва от AST-а
         * (ghost-ите са по име на локала, Sym-ът е вътрешен __pN — както при
         * check_drop_uses). Несъответствие → kind-5 задължение (с
         * контрапример); неследен/не-ident върнат handle → g_guar_unknown
         * (честен UNKNOWN, никога ДОКАЗАНО). Заеманост/незавършени читатели
         * също са несъответствие — иначе гарацията би скрила живи получатели. */
        if (g_guar_kind) {
            const char *hv = (val_expr && val_expr->kind == NODE_IDENT) ? val_expr->name : NULL;
            HandleEntry *h = hv ? handles_find(&states->s[i].handles, hv) : NULL;
            if (!h || h->kind != HK_WORLD) {
                g_guar_unknown = 1;
            } else {
                int want_ok = g_guar_kind == 1 ? (h->state == 1) : (h->state == 0 || h->state == 1);
                if (!h->borrowed && h->n_shared == 0 && want_ok) {
                    /* състоянието е обещаното и светът е чист — отсъствие на
                     * нарушение = доказано по този път */
                } else {
                    char label[240];
                    snprintf(label, sizeof label,
                        "гаранция %s на 'output': върнатият свят '%s' е %s%s%s",
                        g_guar_kind == 1 ? "world_frozen" : "world_owned",
                        hv ? hv : "?",
                        h->state == 1 ? "замразен" : h->state == 2 ? "освободен" :
                        h->state == 3 ? "преместен" : "мутабелен",
                        h->borrowed ? ", заеман" : "",
                        h->n_shared > 0 ? ", с незавършени читатели" : "");
                    push_guar_violation(&states->s[i], ob, label);
                }
            }
        }
        inject_prod_axioms(&states->s[i]);   /* M8b: преди snapshot на path */
        ConsList nobound; cl_init(&nobound);
        ConsList rc; cl_init(&rc);
        for (int x = 0; x < states->s[i].read_cons.n; x++) { Constraint *c = &states->s[i].read_cons.c[x]; cl_push(&rc, mk_cons(lin_clone(&c->lhs), c->op, c->rhs)); }
        obl_push(ob, states->s[i].path, nobound, rc, r, &states->s[i].vlen, &states->s[i].reads, states->s[i].bad, 0, NULL);
        env_free(&states->s[i].env);
        vlen_free(&states->s[i].vlen);
        handles_free(&states->s[i].handles);
    }
    states->n = 0;
}

void symexec_stmts(NodeVec *stmts, States *states, Obligations *ob, int is_nonvoid, Node *spec) {
    for (int si = 0; si < stmts->len; si++) {
        Node *st = stmts->data[si];
        int last = (si == stmts->len - 1);
        for (int i = 0; i < states->n; i++) chan_escape_stmt(&states->s[i], st);   /* W2.12 */
        if (st->kind == NODE_LET) {
            for (int i = 0; i < states->n; i++) {
                scan_arith_expr(st->let_init, &states->s[i], ob);   /* M15 */
                check_drop_uses(st->let_init, &states->s[i], ob);   /* MEM-2 */
                if (is_user_call(st->let_init)) {   /* M5: assume-guarantee */
                    Sym r = eval_user_call(st->let_init, &states->s[i], ob, g_caller_name);
                    bind_sym(&states->s[i].env, st->let_name, r);
                    /* W3.1: резултатът от fn с гаранция за свят е HANDLE —
                     * регистрира се с обещаното състояние (frozen → 1, owned →
                     * 0) и оттук нататък влиза в целия протокол (free,
                     * use-after-free, mutate-after-freeze). Без това върнатият
                     * свят беше неследен (документирана дупка). */
                    if (g_ret_world_kind) {
                        int gk = g_ret_world_kind;
                        g_ret_world_kind = 0;
                        handles_set(&states->s[i].handles, st->let_name, HK_WORLD,
                                    gk == 1 ? 1 : 0, NULL);
                        HandleEntry *hw = handles_find(&states->s[i].handles, st->let_name);
                        if (hw) { hw->borrowed = 0; hw->wcontract = 0; }
                    }
                    continue;
                }
                if (is_par_builtin_call(st->let_init)) {   /* M14: !Par */
                    Sym r = eval_par_call(st->let_init, &states->s[i], ob);
                    bind_sym(&states->s[i].env, st->let_name, r);
                    /* W2.8: свят, получен през канал с договор от аксиомата на
                     * изпращача — регистрира се под ИМЕТО НА ЛОКАЛА (ghost-ите
                     * са по име, recv връща вътрешна __cN стойност). frozen е
                     * заемане за четене (без free, делегиране само с join —
                     * W2.4/W2.6), owned е прехвърлена собственост (sink-ът може
                     * да freeze-не и free-не — W2.7). Аксиомата е оцеляла при
                     * send-а, значи изпращачът я е доказал: не е допускане. */
                    if (g_recv_world_kind) {
                        int kind = g_recv_world_kind;
                        g_recv_world_kind = 0;
                        handles_set(&states->s[i].handles, st->let_name, HK_WORLD,
                                    kind == 1 ? 1 : 0, NULL);
                        HandleEntry *hw = handles_find(&states->s[i].handles, st->let_name);
                        if (hw) {
                            hw->wcontract = kind;
                            hw->borrowed = kind == 1;
                            /* W2.12: от кой канал е получен — world_free на
                             * изпращащия свят не се разрешава, докато този
                             * handle е жив (самополучаване). */
                            free(hw->from_chan);
                            hw->from_chan = g_recv_world_chan[0] ? strdup(g_recv_world_chan) : NULL;
                        }
                    }
                    continue;
                }
                bind_sym(&states->s[i].env, st->let_name, se_from_ast(st->let_init, &states->s[i].env, &states->s[i].vlen, &states->s[i].reads));
                /* MEM-2: a freshly-allocated buffer is live — start the
                 * alloc→drop protocol for this source variable */
                if (is_mem_alloc_call(st->let_init))
                    handles_set(&states->s[i].handles, st->let_name, HK_DROP, 0, NULL);
                /* W0: свеж свят — HK_WORLD стартира от mutable; world_alloc
                 * в init проверява frozen/freed състоянието на източника */
                if (is_world_new_call(st->let_init))
                    handles_set(&states->s[i].handles, st->let_name, HK_WORLD, 0, NULL);
                if (is_world_op_call(st->let_init))
                    eval_world_call(st->let_init, &states->s[i], ob);
                /* a fresh vec_new() has length 0 */
                if (st->let_init && st->let_init->kind == NODE_CALL && st->let_init->callee->kind == NODE_IDENT &&
                    strcmp(st->let_init->callee->name, "vec_new") == 0)
                    vlen_set(&states->s[i].vlen, st->let_name, lin_const(rat_int(0)), 0);
                scan_vec_expr(st->let_init, &states->s[i], ob, spec);
                /* M3+: a slice result inherits the source's invariants; a
                 * concat result inherits the invariants BOTH operands share. */
                if (st->let_init && st->let_init->kind == NODE_CALL && st->let_init->callee->kind == NODE_IDENT &&
                    st->let_init->args.len >= 1 && st->let_init->args.data[0]->kind == NODE_IDENT) {
                    const char *cn = st->let_init->callee->name;
                    const char *src = st->let_init->args.data[0]->name;
                    if (strcmp(cn, "vec_slice") == 0) {
                        ax_copy_renamed(&states->s[i].ax, &states->s[i].ax, src, st->let_name);
                        /* result length = b - a (the slice bounds) */
                        if (st->let_init->args.len >= 3) {
                            Sym a = se_from_ast(st->let_init->args.data[1], &states->s[i].env, &states->s[i].vlen, &states->s[i].reads);
                            Sym b = se_from_ast(st->let_init->args.data[2], &states->s[i].env, &states->s[i].vlen, &states->s[i].reads);
                            if (!a.nonlinear && !b.nonlinear) {
                                Lin bl = lin_sub(&b.lin, &a.lin);
                                vlen_set(&states->s[i].vlen, st->let_name, bl, 0);
                            }
                            lin_free(&a.lin); lin_free(&b.lin);
                        }
                    } else if (strcmp(cn, "vec_concat") == 0 && st->let_init->args.len >= 2 &&
                             st->let_init->args.data[1]->kind == NODE_IDENT) {
                        ax_intersect_two(&states->s[i].ax, &states->s[i].ax, src, st->let_init->args.data[1]->name, st->let_name);
                        /* result length = len(v) + len(w) */
                        VLenEntry *lv = vlen_find(&states->s[i].vlen, src);
                        VLenEntry *lw = vlen_find(&states->s[i].vlen, st->let_init->args.data[1]->name);
                        if (lv && lw && !lv->unknown && !lw->unknown)
                            vlen_set(&states->s[i].vlen, st->let_name, lin_add(&lv->len, &lw->len), 0);
                    }
                }
            }
        } else if (st->kind == NODE_ASSIGN) {
            if (st->assign_target->kind != NODE_IDENT) { for (int i = 0; i < states->n; i++) states->s[i].bad = 1; continue; }
            for (int i = 0; i < states->n; i++) {
                scan_arith_expr(st->assign_val, &states->s[i], ob);   /* M15 */
                check_drop_uses(st->assign_val, &states->s[i], ob);   /* MEM-2 */
                bind_sym(&states->s[i].env, st->assign_target->name, se_from_ast(st->assign_val, &states->s[i].env, &states->s[i].vlen, &states->s[i].reads));
            }
        } else if (st->kind == NODE_RETURN) {
            for (int i = 0; i < states->n; i++) scan_arith_expr(st->ret_val, &states->s[i], ob);   /* M15 */
            for (int i = 0; i < states->n; i++) check_drop_uses(st->ret_val, &states->s[i], ob);   /* MEM-2 */
            for (int i = 0; i < states->n; i++)
                if (is_world_op_call(st->ret_val)) eval_world_call(st->ret_val, &states->s[i], ob);   /* W0 */
            if (is_user_call(st->ret_val)) {   /* M5 */
                for (int i = 0; i < states->n; i++) {
                    Sym r = eval_user_call(st->ret_val, &states->s[i], ob, g_caller_name);
                    env_bind(&states->s[i].env, "__retv", r.lin, r.nonlinear);
                }
                Node id; memset(&id, 0, sizeof id); id.kind = NODE_IDENT; id.name = "__retv";
                drain_returns(states, ob, &id);
                return;
            }
            if (is_par_builtin_call(st->ret_val)) {   /* M14 */
                for (int i = 0; i < states->n; i++) {
                    Sym r = eval_par_call(st->ret_val, &states->s[i], ob);
                    env_bind(&states->s[i].env, "__retv", r.lin, r.nonlinear);
                }
                Node id; memset(&id, 0, sizeof id); id.kind = NODE_IDENT; id.name = "__retv";
                drain_returns(states, ob, &id);
                return;
            }
            for (int i = 0; i < states->n; i++) scan_vec_expr(st->ret_val, &states->s[i], ob, spec);
            drain_returns(states, ob, st->ret_val);
            return;
        } else if (st->kind == NODE_EXPR_STMT && last && is_nonvoid) {
            for (int i = 0; i < states->n; i++) scan_arith_expr(st->expr, &states->s[i], ob);   /* M15 */
            for (int i = 0; i < states->n; i++) check_drop_uses(st->expr, &states->s[i], ob);   /* MEM-2 */
            for (int i = 0; i < states->n; i++)
                if (is_world_op_call(st->expr)) eval_world_call(st->expr, &states->s[i], ob);   /* W0 */
            if (is_user_call(st->expr)) {   /* M5 */
                for (int i = 0; i < states->n; i++) {
                    Sym r = eval_user_call(st->expr, &states->s[i], ob, g_caller_name);
                    env_bind(&states->s[i].env, "__retv", r.lin, r.nonlinear);
                }
                Node id; memset(&id, 0, sizeof id); id.kind = NODE_IDENT; id.name = "__retv";
                drain_returns(states, ob, &id);
                return;
            }
            if (is_par_builtin_call(st->expr)) {   /* M14 */
                for (int i = 0; i < states->n; i++) {
                    Sym r = eval_par_call(st->expr, &states->s[i], ob);
                    env_bind(&states->s[i].env, "__retv", r.lin, r.nonlinear);
                }
                Node id; memset(&id, 0, sizeof id); id.kind = NODE_IDENT; id.name = "__retv";
                drain_returns(states, ob, &id);
                return;
            }
            for (int i = 0; i < states->n; i++) scan_vec_expr(st->expr, &states->s[i], ob, spec);
            drain_returns(states, ob, st->expr);
            return;
        } else if (st->kind == NODE_EXPR_STMT) {
            /* expression statement: track vec mutations / emit bounds checks */
            for (int i = 0; i < states->n; i++) {
                scan_arith_expr(st->expr, &states->s[i], ob);   /* M15 */
                if (is_user_call(st->expr)) {   /* M5: check requires, discard result */
                    Sym r = eval_user_call(st->expr, &states->s[i], ob, g_caller_name);
                    lin_free(&r.lin);
                    continue;
                }
                if (is_par_builtin_call(st->expr)) {   /* M14: discard result */
                    Sym r = eval_par_call(st->expr, &states->s[i], ob);
                    lin_free(&r.lin);
                    continue;
                }
                if (is_drop_call(st->expr)) {   /* MEM-2: consume the buffer */
                    eval_drop_call(st->expr, &states->s[i], ob);
                    continue;
                }
                if (is_world_op_call(st->expr)) {   /* W0: freeze/free/alloc на свят */
                    eval_world_call(st->expr, &states->s[i], ob);
                    continue;
                }
                check_drop_uses(st->expr, &states->s[i], ob);   /* MEM-2 */
                scan_vec_expr(st->expr, &states->s[i], ob, spec);
            }
        } else if (st->kind == NODE_IF) {
            States out; out.s = NULL; out.n = 0; out.cap = 0;
            for (int i = 0; i < states->n; i++) {
                State *cur = &states->s[i];
                scan_arith_expr(st->cond, cur, ob);   /* M15: guard arithmetic */
                check_drop_uses(st->cond, cur, ob);   /* MEM-2: dropped buffer in the guard */
                Formula cf, nf;
                int cok = bool_to_dnf(st->cond, &cur->env, &cur->vlen, &cur->reads, 0, &cf);
                int nok = bool_to_dnf(st->cond, &cur->env, &cur->vlen, &cur->reads, 1, &nf);
                if (!cok || !nok) {
                    if (cok) f_free(&cf);
                    if (nok) f_free(&nf);
                    cur->bad = 1;
                    states_push(&out, *cur);
                    continue;
                }
                /* M13: products/div/mod/bits introduced while reading the
                 * condition must inject their axioms before the fork so both
                 * branches inherit e.g. n*n >= 0 under the shared path. */
                inject_prod_axioms(cur);
                /* then branch: fork per cond DNF branch */
                States then_ss; then_ss.s = NULL; then_ss.n = 0; then_ss.cap = 0;
                for (int b = 0; b < cf.n; b++) {
                    Formula one; f_init(&one);
                    ConsList cl; cl_init(&cl);
                    for (int x = 0; x < cf.br[b].n; x++) { Constraint *c = &cf.br[b].c[x]; cl_push(&cl, mk_cons(lin_clone(&c->lhs), c->op, c->rhs)); }
                    f_push_branch(&one, cl);
                    State t = clone_state_with(cur, &one);
                    f_free(&one);
                    states_push(&then_ss, t);
                }
                symexec_block(st->then_br, &then_ss, ob, 0, spec);   /* branch ≠ return context */
                for (int k = 0; k < then_ss.n; k++) states_push(&out, then_ss.s[k]);
                free(then_ss.s);
                /* else branch (or fall-through when there is none) */
                States else_ss; else_ss.s = NULL; else_ss.n = 0; else_ss.cap = 0;
                for (int b = 0; b < nf.n; b++) {
                    Formula one; f_init(&one);
                    ConsList cl; cl_init(&cl);
                    for (int x = 0; x < nf.br[b].n; x++) { Constraint *c = &nf.br[b].c[x]; cl_push(&cl, mk_cons(lin_clone(&c->lhs), c->op, c->rhs)); }
                    f_push_branch(&one, cl);
                    State t = clone_state_with(cur, &one);
                    f_free(&one);
                    states_push(&else_ss, t);
                }
                if (st->else_br) symexec_block(st->else_br, &else_ss, ob, 0, spec);   /* branch ≠ return context */
                for (int k = 0; k < else_ss.n; k++) states_push(&out, else_ss.s[k]);
                free(else_ss.s);
                f_free(&cf); f_free(&nf);
                env_free(&cur->env); cl_free(&cur->path); handles_free(&cur->handles);
            }
            free(states->s);
            *states = out;
        } else if (st->kind == NODE_WHILE) {
            symexec_while(st, states, ob, is_nonvoid, spec);
        } else if (st->kind == NODE_INVARIANT) {
            /* M16: annotation statement — content axioms (c[*] ...) and
             * scalar assumptions */
            for (int i = 0; i < states->n; i++) {
                for (int j = 0; j < st->inv_exprs.len; j++) {
                    Node *pred = st->inv_exprs.data[j];
                    if (has_elem_ref(pred)) {
                        if (!extract_axiom_resolved(pred, &states->s[i])) states->s[i].bad = 1;
                    } else {
                        Formula af;
                        int aok = bool_to_dnf(pred, &states->s[i].env, &states->s[i].vlen, &states->s[i].reads, 0, &af);
                        if (!aok || af.n != 1) {
                            if (aok) f_free(&af);
                            states->s[i].bad = 1;   /* disjunctive/unsupported assume — honest UNKNOWN */
                        } else {
                            for (int x = 0; x < af.br[0].n; x++) { Constraint *c = &af.br[0].c[x]; cl_push(&states->s[i].path, mk_cons(lin_clone(&c->lhs), c->op, c->rhs)); }
                            f_free(&af);
                        }
                    }
                }
            }
        } else {
            for (int i = 0; i < states->n; i++) states->s[i].bad = 1;
        }
    }
}

