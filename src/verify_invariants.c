/* verify_invariants.c — част от verify.c (виж verify_proto.h). */
#include "baga.h"
#include "verify_impl.h"

/* ======================= M16: content invariants =======================
 * "Every payload sent on channel c satisfies P" — declared by the
 * statement-level `invariant c[*] <cmp> <lin>` annotation, anchored on the
 * channel's RESOLVED symbolic var (ghost handle), so aliases work. Sends
 * discharge the predicate (or the axiom is dropped, M3 rule); receives
 * instantiate it. At call/go boundaries the callee's `requires c[*] ...`
 * is discharged against the caller's axioms; a callee without matching
 * requires drops them (it may send anything). */

/* Resolve an expression to the symbolic key axioms are anchored on: the bare
 * symbolic var of its value (channel ghost / param name), else the source
 * identifier (M3 local-vec convention), else NULL. */
const char *resolve_key(Node *act, State *st, char *buf, size_t n) {
    Sym av = se_from_ast(act, &st->env, &st->vlen, &st->reads);
    if (!av.nonlinear && av.lin.n == 1 && av.lin.c.num == 0 &&
        av.lin.terms[0].coeff.num == av.lin.terms[0].coeff.den) {
        snprintf(buf, n, "%s", av.lin.terms[0].var);
        lin_free(&av.lin);
        return buf;
    }
    lin_free(&av.lin);
    if (act->kind == NODE_IDENT) { snprintf(buf, n, "%s", act->name); return buf; }
    return NULL;
}

/* Does the caller state hold an axiom on `key` entailing (cmp, rhs)? */
static int axiom_entailed(State *st, const char *key, ElemCmp cmp, Lin *rhs, int is_sorted) {
    for (int i = 0; i < st->ax.n; i++) {
        ElemAxiom *a = &st->ax.a[i];
        if (strcmp(a->vec, key) != 0) continue;
        if (is_sorted) { if (a->is_sorted) return 1; continue; }
        if (a->is_sorted || a->cmp != cmp) continue;
        if (lin_eq(&a->rhs, rhs)) return 1;
        /* a.rhs implies rhs in the cmp direction */
        Lin d;
        if (cmp == EC_GE || cmp == EC_GT) d = lin_sub(rhs, &a->rhs);       /* rhs - a.rhs <= 0 ⟺ a.rhs >= rhs */
        else d = lin_sub(&a->rhs, rhs);                                    /* a.rhs - rhs <= 0 ⟺ a.rhs <= rhs */
        ConsList cons; cl_init(&cons);
        cl_push(&cons, mk_cons(d, C_LE, rat_zero()));
        int ok = cl_implied_by(&st->path, &cons);
        cl_free(&cons);
        if (ok) return 1;
    }
    return 0;
}

void ax_drop_key(AxiomList *ax, const char *key) {
    AxiomList kept; ax_init(&kept);
    for (int i = 0; i < ax->n; i++)
        if (strcmp(ax->a[i].vec, key) != 0)
            ax_push(&kept, ax->a[i].vec, ax->a[i].cmp, lin_clone(&ax->a[i].rhs), ax->a[i].is_sorted);
    ax_free(ax);
    *ax = kept;
}

/* Does this annotation mention an element/content reference on `name`? */
static int elem_req_mentions(Node *e, const char *name) {
    if (!e) return 0;
    if (e->kind == NODE_ELEM_REF)
        return e->elem_obj->kind == NODE_IDENT && strcmp(e->elem_obj->name, name) == 0;
    if (e->kind == NODE_CALL && e->callee && e->callee->kind == NODE_IDENT &&
        strcmp(e->callee->name, "sorted") == 0 && e->args.len == 1)
        return e->args.data[0]->kind == NODE_IDENT && strcmp(e->args.data[0]->name, name) == 0;
    if (e->kind == NODE_BINARY) return elem_req_mentions(e->left, name) || elem_req_mentions(e->right, name);
    if (e->kind == NODE_UNARY) return elem_req_mentions(e->operand, name);
    return 0;
}

void extract_elem_axiom(Node *e, SEnv *env, AxiomList *ax);

/* Extract a content axiom from `obj[*] <cmp> <lin>` (either orientation),
 * anchoring on the object's RESOLVED symbolic var; falls back to the M3
 * source-name extraction (local vecs). Returns 0 if not extractable. */
int extract_axiom_resolved(Node *e, State *st) {
    if (e && e->kind == NODE_BINARY) {
        BinOp op = e->bin_op;
        if (op == OP_LT || op == OP_LE || op == OP_GT || op == OP_GE) {
            Node *l = e->left, *r = e->right;
            Node *eref = NULL, *other = NULL;
            int elem_on_left = 0;
            if (l->kind == NODE_ELEM_REF) { eref = l; other = r; elem_on_left = 1; }
            else if (r->kind == NODE_ELEM_REF) { eref = r; other = l; elem_on_left = 0; }
            if (eref) {
                char kbuf[64];
                const char *key = resolve_key(eref->elem_obj, st, kbuf, sizeof kbuf);
                if (key) {
                    Sym s = se_from_ast(other, &st->env, &st->vlen, &st->reads);
                    if (!s.nonlinear) {
                        ElemCmp cmp;
                        if (elem_on_left) {
                            switch (op) { case OP_LT: cmp = EC_LT; break; case OP_LE: cmp = EC_LE; break;
                                          case OP_GT: cmp = EC_GT; break; default: cmp = EC_GE; break; }
                        } else {
                            switch (op) { case OP_LT: cmp = EC_GT; break; case OP_LE: cmp = EC_GE; break;
                                          case OP_GT: cmp = EC_LT; break; default: cmp = EC_LE; break; }
                        }
                        ax_push(&st->ax, key, cmp, s.lin, 0);
                        return 1;
                    }
                    lin_free(&s.lin);
                }
            }
        }
    }
    int n0 = st->ax.n;
    extract_elem_axiom(e, &st->env, &st->ax);
    return st->ax.n > n0;
}

/* Does the state prove all elements of `vec` are <= `val`? True if there is an
 * element axiom vec[*] <= R (or < R) with path |- R <= val. Sufficient condition
 * used to preserve a sorted axiom across vec_push(vec, val). */
static int state_proves_all_le(State *st, const char *vec, Sym *val) {
    if (val->nonlinear) return 0;
    for (int i = 0; i < st->ax.n; i++) {
        if (st->ax.a[i].is_sorted) continue;
        if (strcmp(st->ax.a[i].vec, vec) != 0) continue;
        if (st->ax.a[i].cmp == EC_LE || st->ax.a[i].cmp == EC_LT) {
            /* need path |- R <= val, i.e. path ∧ (val < R) UNSAT */
            ConsList sys; cl_init(&sys);
            for (int x = 0; x < st->path.n; x++) { Constraint *c = &st->path.c[x]; cl_push(&sys, mk_cons(lin_clone(&c->lhs), c->op, c->rhs)); }
            Lin d = lin_sub(&val->lin, &st->ax.a[i].rhs);   /* val - R < 0 */
            cl_push(&sys, mk_cons(d, C_LT, rat_zero()));
            int sat = fm_sat(&sys);   /* frees sys */
            if (!sat) return 1;
        }
    }
    return 0;
}

void scan_vec_expr(Node *e, State *st, Obligations *ob, Node *spec) {
    if (!e) return;
    if (e->kind != NODE_CALL || e->callee->kind != NODE_IDENT) return;
    const char *nm = e->callee->name;

    if (strcmp(nm, "vec_new") == 0) return;   /* length handled at the let binding */

    if (strcmp(nm, "vec_push") == 0 && e->args.len >= 1 && e->args.data[0]->kind == NODE_IDENT) {
        const char *vname = e->args.data[0]->name;
        VLenEntry *ve = vlen_find(&st->vlen, vname);
        if (ve && !ve->unknown) vlen_set(&st->vlen, vname, lin_add(&ve->len, &(Lin){ .c = rat_int(1) }), 0);
        else vlen_set(&st->vlen, vname, (Lin){0}, 1);
        /* M3 preservation: keep an axiom on v only if the pushed value provably
         * satisfies it; axioms on other vectors are untouched. */
        if (st->ax.n > 0 && e->args.len >= 2) {
            Sym val = se_from_ast(e->args.data[1], &st->env, &st->vlen, &st->reads);
            AxiomList kept; ax_init(&kept);
            for (int i = 0; i < st->ax.n; i++) {
                if (strcmp(st->ax.a[i].vec, vname) == 0) {
                    int survives;
                    if (st->ax.a[i].is_sorted)
                        survives = state_proves_all_le(st, vname, &val);   /* sorted: need all old <= val */
                    else
                        survives = axiom_holds_for_value(&st->ax.a[i], &val, st);
                    if (survives)
                        ax_push(&kept, st->ax.a[i].vec, st->ax.a[i].cmp, lin_clone(&st->ax.a[i].rhs), st->ax.a[i].is_sorted);
                    /* else: dropped (sound — we no longer know it holds) */
                } else {
                    ax_push(&kept, st->ax.a[i].vec, st->ax.a[i].cmp, lin_clone(&st->ax.a[i].rhs), st->ax.a[i].is_sorted);
                }
            }
            lin_free(&val.lin);
            ax_free(&st->ax);
            st->ax = kept;
        }
        return;
    }

    if (strcmp(nm, "vec_get") == 0 && e->args.len >= 2 && e->args.data[0]->kind == NODE_IDENT) {
        const char *vname = e->args.data[0]->name;
        VLenEntry *ve = vlen_find(&st->vlen, vname);
        Sym idx = se_from_ast(e->args.data[1], &st->env, &st->vlen, &st->reads);
        /* M3: the read resolves to a fresh symbolic variable carrying the
         * instantiated element axioms (forall i: P(v[i]) => P(read)). */
        char rv[64]; snprintf(rv, sizeof rv, "__r%d", st->reads.n);
        reads_push(&st->reads, e, rv);
        for (int i = 0; i < st->ax.n; i++) {
            /* sorted is relational — no scalar fact about a single read */
            if (st->ax.a[i].is_sorted) continue;
            if (strcmp(st->ax.a[i].vec, vname) == 0) {
                cl_push(&st->read_cons, axiom_instantiate(&st->ax.a[i], rv));
            }
        }
        Lin lenlin; int have_len = 0;
        if (ve && !ve->unknown) { lenlin = lin_clone(&ve->len); have_len = 1; }
        else if (ve && ve->unknown) { char buf[300]; snprintf(buf, sizeof buf, "__len_%s", vname); lenlin = lin_var(buf); have_len = 1; }
        int unknown = idx.nonlinear;
        /* path = requires ∧ current state path; bound = ¬(0 <= idx < len),
         * kept separate so the SAT proof can use it while the witness search
         * ignores it */
        ConsList opath; cl_init(&opath);
        if (spec) for (int r = 0; r < spec->spec_requires.len; r++) {
            Formula rf;
            if (bool_to_dnf(spec->spec_requires.data[r]->ensure_expr, &st->env, &st->vlen, &st->reads, 0, &rf)) {
                if (rf.n == 1)
                    for (int x = 0; x < rf.br[0].n; x++) { Constraint *c = &rf.br[0].c[x]; cl_push(&opath, mk_cons(lin_clone(&c->lhs), c->op, c->rhs)); }
                f_free(&rf);
            }
        }
        for (int x = 0; x < st->path.n; x++) { Constraint *c = &st->path.c[x]; cl_push(&opath, mk_cons(lin_clone(&c->lhs), c->op, c->rhs)); }
        ConsList obound; cl_init(&obound);
        if (!idx.nonlinear) {
            /* store the POSITIVE in-range bounds (idx >= 0, idx < len); the
             * verifier negates them into a DISJUNCTION (idx<0 OR idx>=len) */
            cl_push(&obound, mk_cons(lin_clone(&idx.lin), C_LE, rat_zero()));   /* 0 <= idx */
            if (have_len) {
                Lin d = lin_sub(&idx.lin, &lenlin);                          /* idx - len < 0 */
                cl_push(&obound, mk_cons(d, C_LT, rat_zero()));
            }
        }
        if (have_len) lin_free(&lenlin);
        char label[160];
        snprintf(label, sizeof label, "достъп до %s[..] е извън границите", vname);
        Sym retsym;
        if (idx.nonlinear) { retsym = sym_nonlin(); } else { retsym = sym_lin(lin_clone(&idx.lin)); }
        obl_push(ob, opath, obound, (ConsList){NULL, 0, 0}, retsym, &st->vlen, &st->reads, unknown, 1, label);
        lin_free(&idx.lin);
        return;
    }

    if (strcmp(nm, "vec_set") == 0 && e->args.len >= 1 && e->args.data[0]->kind == NODE_IDENT) {
        /* M3+: vec_set(v, k, val) preserves an axiom on v when the new value
         * provably satisfies the predicate (the set element then still obeys
         * it, and the others are untouched); otherwise the axiom is dropped
         * (sound over-approximation). */
        const char *vname = e->args.data[0]->name;
        AxiomList kept; ax_init(&kept);
        for (int i = 0; i < st->ax.n; i++) {
            if (strcmp(st->ax.a[i].vec, vname) == 0) {
                int survives = 0;
                /* sorted: a mid-vector write can break order; drop (sound).
                 * Element axioms: keep iff the new value still satisfies P. */
                if (!st->ax.a[i].is_sorted && e->args.len >= 3) {
                    Sym val = se_from_ast(e->args.data[2], &st->env, &st->vlen, &st->reads);
                    survives = axiom_holds_for_value(&st->ax.a[i], &val, st);
                    lin_free(&val.lin);
                }
                if (survives) ax_push(&kept, st->ax.a[i].vec, st->ax.a[i].cmp, lin_clone(&st->ax.a[i].rhs), st->ax.a[i].is_sorted);
            } else {
                ax_push(&kept, st->ax.a[i].vec, st->ax.a[i].cmp, lin_clone(&st->ax.a[i].rhs), st->ax.a[i].is_sorted);
            }
        }
        ax_free(&st->ax);
        st->ax = kept;
        return;
    }
}

/* Does `path` imply every constraint in `cons`? UNSAT(path ∧ ¬c) per c. */
int cl_implied_by(ConsList *path, ConsList *cons) {
    for (int i = 0; i < cons->n; i++) {
        ConsList sys; cl_init(&sys);
        for (int x = 0; x < path->n; x++) { Constraint *c = &path->c[x]; cl_push(&sys, mk_cons(lin_clone(&c->lhs), c->op, c->rhs)); }
        Constraint *cc = &cons->c[i];
        COp negop = (cc->op == C_LT) ? C_LE : C_LT;
        cl_push(&sys, mk_cons(lin_neg(&cc->lhs), negop, cc->rhs));
        int sat = fm_sat(&sys);   /* frees sys */
        if (sat) return 0;
    }
    return 1;
}

int is_user_call(Node *e) {
    return e && e->kind == NODE_CALL && e->callee && e->callee->kind == NODE_IDENT &&
           !is_vec_builtin_call(e) && g_prog && find_spec(g_prog, e->callee->name) &&
           callee_sig_supported(find_fn(g_prog, e->callee->name));
}

/* M8b: structural equality of linear forms (terms are merged canonically,
 * so a zero difference means equal). */
int lin_eq(Lin *a, Lin *b) {
    Lin d = lin_sub(a, b);
    int eq = !d.overflow && d.n == 0 && d.c.num == 0;
    lin_free(&d);
    return eq;
}

/* Does the state's path prove fa >= K? */
static int path_proves_ge(State *st, Lin *fa, int64_t K) {
    Lin t = lin_neg(fa);
    t.c = rat_add(t.c, rat_int(K));   /* K - fa <= 0  ⟺  fa >= K */
    ConsList cons; cl_init(&cons);
    cl_push(&cons, mk_cons(t, C_LE, rat_zero()));
    int r = cl_implied_by(&st->path, &cons);
    cl_free(&cons);
    return r;
}

/* Does the state's path prove fa <= K? */
static int path_proves_le(State *st, Lin *fa, int64_t K) {
    Lin t = lin_clone(fa);
    t.c = rat_sub(t.c, rat_int(K));   /* fa - K <= 0  ⟺  fa <= K */
    ConsList cons; cl_init(&cons);
    cl_push(&cons, mk_cons(t, C_LE, rat_zero()));
    int r = cl_implied_by(&st->path, &cons);
    cl_free(&cons);
    return r;
}

/* Push the fact pv >= K onto the path. */
void path_push_ge(ConsList *path, const char *pv, int64_t K) {
    Lin v = lin_var(pv);
    Lin t = lin_neg(&v);            /* -pv */
    lin_free(&v);
    t.c = rat_add(t.c, rat_int(K)); /* K - pv <= 0  ⟺  pv >= K */
    cl_push(path, mk_cons(t, C_LE, rat_zero()));
}

/* Push the fact pv <= K onto the path. */
void path_push_le(ConsList *path, const char *pv, int64_t K) {
    Lin v = lin_var(pv);
    v.c = rat_sub(v.c, rat_int(K)); /* pv - K <= 0  ⟺  pv <= K */
    cl_push(path, mk_cons(v, C_LE, rat_zero()));
}

/* Push pv - fa >= 0  (i.e. pv >= fa) as fa - pv <= 0. */
static void path_push_ge_lin(ConsList *path, const char *pv, Lin *fa) {
    Lin p = lin_var(pv);
    Lin t = lin_sub(fa, &p);   /* fa - pv */
    lin_free(&p);
    cl_push(path, mk_cons(t, C_LE, rat_zero()));
}

/* Push pv <= fa as pv - fa <= 0. */
static void path_push_le_lin(ConsList *path, const char *pv, Lin *fa) {
    Lin p = lin_var(pv);
    Lin t = lin_sub(&p, fa);
    lin_free(&p);
    cl_push(path, mk_cons(t, C_LE, rat_zero()));
}

/* Push equality lhs == 0 as two inequalities. Moves/frees nothing owned. */
static void path_push_eq_zero(ConsList *path, Lin *expr) {
    Lin a = lin_clone(expr);
    Lin b = lin_neg(expr);
    cl_push(path, mk_cons(a, C_LE, rat_zero()));
    cl_push(path, mk_cons(b, C_LE, rat_zero()));
}

/* M8–M12: inject TRUE facts about product/div/mod vars.
 * M12: divisor ±1 identities; self-div/mod (n/n, n%n); variable divisor
 * when m>=1,n>=0 (sign + remainder bounds + q*m product floor); AM-GM via
 * (fa-fb)^2 = fa²+fb²-2·fa·fb ≥ 0 when those products exist. */
void inject_prod_axioms(State *st) {
    for (int i = 0; i < st->reads.n; i++) {
        ReadEntry *e = &st->reads.r[i];
        /* M13/M20: n & (2^k-1) ∈ {0 .. mask} over two's complement. */
        if (e->is_bitand1) {
            int64_t hi = (lin_is_const(&e->fb) && e->fb.c.den == 1 && e->fb.c.num > 0)
                         ? e->fb.c.num : 1;
            path_push_ge(&st->path, e->var, 0);
            path_push_le(&st->path, e->var, hi);
            continue;
        }
        /* M20: variable n&m / n|m on nonnegative operands. */
        if (e->is_bitand || e->is_bitor) {
            int fa0 = path_proves_ge(st, &e->fa, 0);
            int fb0 = path_proves_ge(st, &e->fb, 0);
            if (fa0 && fb0) {
                path_push_ge(&st->path, e->var, 0);
                if (e->is_bitand) {
                    path_push_le_lin(&st->path, e->var, &e->fa);
                    path_push_le_lin(&st->path, e->var, &e->fb);
                } else {
                    path_push_ge_lin(&st->path, e->var, &e->fa);
                    path_push_ge_lin(&st->path, e->var, &e->fb);
                }
            }
            continue;
        }
        /* M22: n >> k — аритметично отместване = floor(n/2^k) за константно
         * k ∈ [1,62] (k=0 е точна идентичност в se_from_ast). fb държи k.
         * n ≥ 0: floor == trunc — същите аксиоми като делението. n ≤ 0:
         * floor: q ≤ 0, q ≥ n, 0 ≤ n - d·q ≤ d-1. Неизвестен знак — слабо. */
        if (e->is_shr) {
            int64_t k = (lin_is_const(&e->fb) && e->fb.c.den == 1 &&
                         e->fb.c.num >= 1 && e->fb.c.num <= 62) ? e->fb.c.num : 1;
            int64_t d = 1LL << k;
            int n_ge0 = path_proves_ge(st, &e->fa, 0);
            int n_le0 = path_proves_le(st, &e->fa, 0);
            if (n_ge0) {
                path_push_ge(&st->path, e->var, 0);
                Lin qv = lin_var(e->var);
                Lin dq = lin_scale(&qv, rat_int(d));
                lin_free(&qv);
                Lin t = lin_sub(&dq, &e->fa);
                cl_push(&st->path, mk_cons(t, C_LE, rat_zero()));      /* d·q - n <= 0 */
                Lin rem = lin_sub(&e->fa, &dq);
                rem.c = rat_sub(rem.c, rat_int(d - 1));
                cl_push(&st->path, mk_cons(rem, C_LE, rat_zero()));    /* n - d·q <= d-1 */
                lin_free(&dq);
            } else if (n_le0) {
                path_push_le(&st->path, e->var, 0);
                path_push_ge_lin(&st->path, e->var, &e->fa);           /* q >= n */
                Lin qv = lin_var(e->var);
                Lin dq = lin_scale(&qv, rat_int(d));
                lin_free(&qv);
                Lin t = lin_sub(&dq, &e->fa);
                cl_push(&st->path, mk_cons(t, C_LE, rat_zero()));      /* d·q <= n */
                Lin rem = lin_sub(&e->fa, &dq);
                rem.c = rat_sub(rem.c, rat_int(d - 1));
                cl_push(&st->path, mk_cons(rem, C_LE, rat_zero()));    /* n - d·q <= d-1 */
                lin_free(&dq);
            }
            continue;
        }
        /* M23: n >>> k — логическо отместване (zero-fill), константно
         * k ∈ [1,62] (k=0 е точна идентичност в se_from_ast). Знаковият бит
         * е изчистен ⇒ безусловно 0 ≤ q ≤ 2^(64-k)-1. n ≥ 0: floor == trunc
         * (същата обвивка като >>). n ≤ -1: q = floor((n+2^64)/d) ≥ 2^(63-k);
         * точната връзка с 2^64 не е линейна по ℤ-променливите — честно
         * остава само долната граница. */
        if (e->is_lshr) {
            int64_t k = (lin_is_const(&e->fb) && e->fb.c.den == 1 &&
                         e->fb.c.num >= 1 && e->fb.c.num <= 62) ? e->fb.c.num : 1;
            int64_t d = 1LL << k;
            /* 2^(64-k)-1; при k=1 това е INT64_MAX (1LL<<63 би преляло) */
            int64_t hi = (k == 1) ? INT64_MAX : ((1LL << (64 - k)) - 1);
            path_push_ge(&st->path, e->var, 0);
            path_push_le(&st->path, e->var, hi);
            int n_ge0 = path_proves_ge(st, &e->fa, 0);
            int n_le_m1 = path_proves_le(st, &e->fa, -1);
            if (n_ge0) {
                Lin qv = lin_var(e->var);
                Lin dq = lin_scale(&qv, rat_int(d));
                lin_free(&qv);
                Lin t = lin_sub(&dq, &e->fa);
                cl_push(&st->path, mk_cons(t, C_LE, rat_zero()));      /* d·q <= n */
                Lin rem = lin_sub(&e->fa, &dq);
                rem.c = rat_sub(rem.c, rat_int(d - 1));
                cl_push(&st->path, mk_cons(rem, C_LE, rat_zero()));    /* n - d·q <= d-1 */
                lin_free(&dq);
            } else if (n_le_m1) {
                path_push_ge(&st->path, e->var, 1LL << (63 - k));
            }
            continue;
        }
        if (e->is_div) {
            int n_ge0 = path_proves_ge(st, &e->fa, 0);
            int n_le0 = path_proves_le(st, &e->fa, 0);
            int n_ge1 = path_proves_ge(st, &e->fa, 1);
            int n_le_m1 = path_proves_le(st, &e->fa, -1);

            /* M12: n/n = 1 when n ≠ 0 (C trunc) */
            if (lin_eq(&e->fa, &e->fb) && (n_ge1 || n_le_m1)) {
                /* q == 1 */
                Lin q = lin_var(e->var);
                q.c = rat_sub(q.c, rat_int(1));
                path_push_eq_zero(&st->path, &q);
                lin_free(&q);
                continue;
            }

            if (lin_is_const(&e->fb) && e->fb.c.den == 1 && e->fb.c.num != 0) {
                int64_t d = e->fb.c.num;
                /* M12: n/1 = n; n/(-1) = -n always */
                if (d == 1) {
                    Lin q = lin_var(e->var);
                    Lin diff = lin_sub(&e->fa, &q);
                    lin_free(&q);
                    path_push_eq_zero(&st->path, &diff);
                    lin_free(&diff);
                    continue;
                }
                if (d == -1) {
                    /* q + n == 0 */
                    Lin q = lin_var(e->var);
                    Lin s = lin_add(&q, &e->fa);
                    lin_free(&q);
                    path_push_eq_zero(&st->path, &s);
                    lin_free(&s);
                    continue;
                }
                if (d > 0) {
                    if (n_ge0) path_push_ge(&st->path, e->var, 0);
                    if (n_le0) path_push_le(&st->path, e->var, 0);
                    /* M11: C trunc; n = d*q + r with 0 <= r < d when n>=0 */
                    if (n_ge0 || n_le0) {
                        Lin qv = lin_var(e->var);
                        Lin dq = lin_scale(&qv, rat_int(d));
                        lin_free(&qv);
                        if (n_ge0) {
                            Lin t = lin_sub(&dq, &e->fa);
                            cl_push(&st->path, mk_cons(t, C_LE, rat_zero()));
                            Lin rem = lin_sub(&e->fa, &dq);
                            rem.c = rat_sub(rem.c, rat_int(d - 1));
                            cl_push(&st->path, mk_cons(rem, C_LE, rat_zero()));
                        }
                        if (n_le0) {
                            Lin t = lin_sub(&e->fa, &dq);
                            cl_push(&st->path, mk_cons(t, C_LE, rat_zero()));
                        }
                        lin_free(&dq);
                    }
                } else { /* d < 0 const */
                    if (n_ge0) path_push_le(&st->path, e->var, 0);
                    if (n_le0) path_push_ge(&st->path, e->var, 0);
                }
                continue;
            }

            /* M12: 0/m = 0 when m ≠ 0 */
            if (lin_is_const(&e->fa) && e->fa.c.den == 1 && e->fa.c.num == 0) {
                int m_ge1 = path_proves_ge(st, &e->fb, 1);
                int m_le_m1 = path_proves_le(st, &e->fb, -1);
                if (m_ge1 || m_le_m1) {
                    path_push_ge(&st->path, e->var, 0);
                    path_push_le(&st->path, e->var, 0);
                }
                continue;
            }

            /* M12: variable divisor m with m >= 1, n >= 0 (C trunc non-neg) */
            int m_ge1 = path_proves_ge(st, &e->fb, 1);
            if (n_ge0 && m_ge1) {
                path_push_ge(&st->path, e->var, 0);      /* q >= 0 */
                path_push_le_lin(&st->path, e->var, &e->fa); /* q <= n */
            }
            continue;
        }
        if (e->is_mod) {
            int n_ge0 = path_proves_ge(st, &e->fa, 0);
            int n_le0 = path_proves_le(st, &e->fa, 0);
            int n_ge1 = path_proves_ge(st, &e->fa, 1);
            int n_le_m1 = path_proves_le(st, &e->fa, -1);

            /* M12: n%n = 0 when n ≠ 0 */
            if (lin_eq(&e->fa, &e->fb) && (n_ge1 || n_le_m1)) {
                path_push_ge(&st->path, e->var, 0);
                path_push_le(&st->path, e->var, 0);
                continue;
            }

            if (lin_is_const(&e->fb) && e->fb.c.den == 1 && e->fb.c.num != 0) {
                int64_t k = e->fb.c.num;
                int64_t ak = k < 0 ? -k : k;
                if (n_ge0 && k > 0) {
                    path_push_ge(&st->path, e->var, 0);
                    if (ak >= 1) path_push_le(&st->path, e->var, ak - 1);
                } else if (n_le0 && k > 0) {
                    path_push_le(&st->path, e->var, 0);
                    if (ak >= 1) path_push_ge(&st->path, e->var, 1 - ak);
                } else if (n_ge0 && k < 0) {
                    path_push_ge(&st->path, e->var, 0);
                    if (ak >= 1) path_push_le(&st->path, e->var, ak - 1);
                } else if (n_le0 && k < 0) {
                    path_push_le(&st->path, e->var, 0);
                    if (ak >= 1) path_push_ge(&st->path, e->var, 1 - ak);
                }
                continue;
            }

            /* M12: variable m, n>=0, m>=1: 0 <= r and r <= m-1 (r - m <= -1) */
            int m_ge1 = path_proves_ge(st, &e->fb, 1);
            if (n_ge0 && m_ge1) {
                path_push_ge(&st->path, e->var, 0);
                /* r - m + 1 <= 0  ⟺  r <= m - 1 */
                Lin r = lin_var(e->var);
                Lin t = lin_sub(&r, &e->fb);
                lin_free(&r);
                t.c = rat_add(t.c, rat_int(1));
                cl_push(&st->path, mk_cons(t, C_LE, rat_zero()));
            }
            continue;
        }
        if (!e->is_prod) continue;
        if (lin_eq(&e->fa, &e->fb)) {
            /* Square s = v*v over ℤ:
             *   s >= 0, s >= v, s >= -v
             *   s - 2v + 1 = (v-1)^2 >= 0
             *   s + 2v + 1 = (v+1)^2 >= 0 */
            path_push_ge(&st->path, e->var, 0);
            path_push_ge_lin(&st->path, e->var, &e->fa);
            { Lin neg = lin_neg(&e->fa); path_push_ge_lin(&st->path, e->var, &neg); lin_free(&neg); }
            {
                Lin s = lin_var(e->var);
                Lin two = lin_scale(&e->fa, rat_int(2));
                /* -(s - 2v + 1) <= 0  ⟺  -s + 2v - 1 <= 0 */
                Lin t = lin_sub(&two, &s);   /* 2v - s */
                lin_free(&s); lin_free(&two);
                t.c = rat_sub(t.c, rat_int(1));
                cl_push(&st->path, mk_cons(t, C_LE, rat_zero()));
            }
            {
                Lin s = lin_var(e->var);
                Lin two = lin_scale(&e->fa, rat_int(2));
                /* -(s + 2v + 1) <= 0  ⟺  -s - 2v - 1 <= 0 */
                Lin neg2 = lin_neg(&two);
                lin_free(&two);
                Lin t = lin_sub(&neg2, &s);  /* -2v - s */
                lin_free(&neg2); lin_free(&s);
                t.c = rat_sub(t.c, rat_int(1));
                cl_push(&st->path, mk_cons(t, C_LE, rat_zero()));
            }
            continue;
        }
        /* M21: consecutive integers n(n±1) ≥ 0 over ℤ (the product of two
         * linear forms that differ by 1). Also p ≥ the lesser factor:
         * n(n+1) ≥ n. n(n+2) is NOT always ≥ 0 — left UNKNOWN. */
        {
            Lin d = lin_sub(&e->fa, &e->fb);
            if (!d.overflow && lin_is_const(&d) && d.c.den == 1 && !d.c.overflow &&
                (d.c.num == 1 || d.c.num == -1)) {
                path_push_ge(&st->path, e->var, 0);
                if (d.c.num == -1)
                    path_push_ge_lin(&st->path, e->var, &e->fa);  /* fb = fa+1 */
                else
                    path_push_ge_lin(&st->path, e->var, &e->fb);  /* fa = fb+1 */
            }
            lin_free(&d);
        }
        int fa_ge1 = path_proves_ge(st, &e->fa, 1);
        int fb_ge1 = path_proves_ge(st, &e->fb, 1);
        int fa_le_m1 = path_proves_le(st, &e->fa, -1);
        int fb_le_m1 = path_proves_le(st, &e->fb, -1);
        int fa_ge0 = path_proves_ge(st, &e->fa, 0);
        int fb_ge0 = path_proves_ge(st, &e->fb, 0);
        int fa_le0 = path_proves_le(st, &e->fa, 0);
        int fb_le0 = path_proves_le(st, &e->fb, 0);
        if (fa_ge1 && fb_ge1) path_push_ge(&st->path, e->var, 1);
        else if (fa_le_m1 && fb_le_m1) path_push_ge(&st->path, e->var, 1);
        else if (fa_ge0 && fb_ge0) path_push_ge(&st->path, e->var, 0);
        else if (fa_le0 && fb_le0) path_push_ge(&st->path, e->var, 0);

        if (fa_ge1 && fb_le_m1) path_push_le(&st->path, e->var, -1);
        else if (fa_le_m1 && fb_ge1) path_push_le(&st->path, e->var, -1);
        else if (fa_ge0 && fb_le0) path_push_le(&st->path, e->var, 0);
        else if (fa_le0 && fb_ge0) path_push_le(&st->path, e->var, 0);

        /* M10 monotonicity: p = fa*fb
         * fa>=0, fb>=1 ⇒ p >= fa  (p - fa = fa*(fb-1) ≥ 0)
         * fa>=1, fb>=0 ⇒ p >= fb */
        if (fa_ge0 && fb_ge1) path_push_ge_lin(&st->path, e->var, &e->fa);
        if (fa_ge1 && fb_ge0) path_push_ge_lin(&st->path, e->var, &e->fb);

        /* M20: even power of a single base (n^{2k} = (n^k)^2 ≥ 0 over ℤ).
         * Left-associated n*n*n*n is deg 4, not a recorded square of n*n. */
        if (e->mon_deg >= 2 && (e->mon_deg % 2 == 0)) {
            path_push_ge(&st->path, e->var, 0);
            int half = e->mon_deg / 2;
            for (int j = 0; j < st->reads.n; j++) {
                ReadEntry *s = &st->reads.r[j];
                if (s == e || !s->is_prod || s->mon_deg != half) continue;
                if (s->mon_deg < 2 || !lin_eq(&s->mon_base, &e->mon_base)) continue;
                Lin root = lin_var(s->var);
                path_push_ge_lin(&st->path, e->var, &root);
                { Lin neg = lin_neg(&root); path_push_ge_lin(&st->path, e->var, &neg); lin_free(&neg); }
                lin_free(&root);
            }
        }
    }

    /* M10/M12: n = k*q + r (const k) or n = q*m + r (variable m via product). */
    for (int i = 0; i < st->reads.n; i++) {
        ReadEntry *d = &st->reads.r[i];
        if (!d->is_div) continue;
        for (int j = 0; j < st->reads.n; j++) {
            ReadEntry *md = &st->reads.r[j];
            if (!md->is_mod) continue;
            if (!lin_eq(&d->fa, &md->fa) || !lin_eq(&d->fb, &md->fb)) continue;

            if (lin_is_const(&d->fb) && d->fb.c.den == 1 && d->fb.c.num != 0) {
                int64_t k = d->fb.c.num;
                Lin n = lin_clone(&d->fa);
                Lin q = lin_var(d->var);
                Lin kq = lin_scale(&q, rat_int(k));
                lin_free(&q);
                Lin r = lin_var(md->var);
                Lin t = lin_sub(&n, &kq);
                lin_free(&n); lin_free(&kq);
                Lin eq = lin_sub(&t, &r);
                lin_free(&t); lin_free(&r);
                path_push_eq_zero(&st->path, &eq);
                lin_free(&eq);
            } else {
                /* variable m: find product p = q * m (or m * q) */
                for (int pi = 0; pi < st->reads.n; pi++) {
                    ReadEntry *p = &st->reads.r[pi];
                    if (!p->is_prod) continue;
                    /* match factors: {q_var as lin_var, m as d->fb} */
                    Lin qlin = lin_var(d->var);
                    int mq = (lin_eq(&p->fa, &qlin) && lin_eq(&p->fb, &d->fb)) ||
                             (lin_eq(&p->fb, &qlin) && lin_eq(&p->fa, &d->fb));
                    lin_free(&qlin);
                    if (!mq) continue;
                    /* n - p - r == 0 */
                    Lin n = lin_clone(&d->fa);
                    Lin pv = lin_var(p->var);
                    Lin r = lin_var(md->var);
                    Lin t = lin_sub(&n, &pv);
                    lin_free(&n); lin_free(&pv);
                    Lin eq = lin_sub(&t, &r);
                    lin_free(&t); lin_free(&r);
                    path_push_eq_zero(&st->path, &eq);
                    lin_free(&eq);
                    /* floor: when n>=0,m>=1: p <= n */
                    if (path_proves_ge(st, &d->fa, 0) && path_proves_ge(st, &d->fb, 1))
                        path_push_le_lin(&st->path, p->var, &d->fa);
                }
            }
        }
    }

    /* M12: (fa - fb)^2 >= 0 as sa + sb - 2*p >= 0 when sa=fa², sb=fb², p=fa*fb */
    for (int i = 0; i < st->reads.n; i++) {
        ReadEntry *sa = &st->reads.r[i];
        if (!sa->is_prod || !lin_eq(&sa->fa, &sa->fb)) continue;
        for (int j = 0; j < st->reads.n; j++) {
            ReadEntry *sb = &st->reads.r[j];
            if (!sb->is_prod || !lin_eq(&sb->fa, &sb->fb)) continue;
            if (lin_eq(&sa->fa, &sb->fa)) continue; /* same square */
            for (int k = 0; k < st->reads.n; k++) {
                ReadEntry *p = &st->reads.r[k];
                if (!p->is_prod || lin_eq(&p->fa, &p->fb)) continue;
                int match = (lin_eq(&p->fa, &sa->fa) && lin_eq(&p->fb, &sb->fa)) ||
                            (lin_eq(&p->fa, &sb->fa) && lin_eq(&p->fb, &sa->fa));
                if (!match) continue;
                /* sa + sb - 2*p >= 0  ⟺  -sa - sb + 2*p <= 0 */
                Lin a = lin_var(sa->var);
                Lin b = lin_var(sb->var);
                Lin c = lin_var(p->var);
                Lin sum = lin_add(&a, &b);
                lin_free(&a); lin_free(&b);
                Lin two_p = lin_scale(&c, rat_int(2));
                lin_free(&c);
                Lin neg_sum = lin_neg(&sum);
                lin_free(&sum);
                Lin t = lin_add(&neg_sum, &two_p);
                lin_free(&neg_sum); lin_free(&two_p);
                cl_push(&st->path, mk_cons(t, C_LE, rat_zero()));
            }
        }
    }
}

/* W2.1: ghost-конюнктите на requires. Обхождане САМО през AND — при OR/NOT
 * `world_frozen(p)` не е гаранция на извикващия, значи не се разтоварва
 * (callee-то също не го регистрира: extract_world_ghost). */
static int world_req_has_ghost(Node *e) {
    if (!e) return 0;
    if (is_world_ghost_call(e)) return 1;
    return e->kind == NODE_BINARY && e->bin_op == OP_AND &&
           (world_req_has_ghost(e->left) || world_req_has_ghost(e->right));
}

static void collect_world_ghosts(Node *e, Node **out, int *n, int cap, int *trunc) {
    if (!e) return;
    if (is_world_ghost_call(e)) {
        if (*n >= cap) { *trunc = 1; return; }
        out[(*n)++] = e;
        return;
    }
    if (e->kind == NODE_BINARY && e->bin_op == OP_AND) {
        collect_world_ghosts(e->left, out, n, cap, trunc);
        collect_world_ghosts(e->right, out, n, cap, trunc);
    }
}

/* Разтоварва един `world_frozen(p)` конюнкт срещу HK_WORLD състоянието на
 * актуалния аргумент при извикващия. Връща 1 ако е ДОКАЗАНО; иначе push-ва
 * нарушение (kind-3, path-sensitive) или честен UNKNOWN (kind-2, bad) и връща 0. */
static int world_ghost_discharge(Node *g, Node *call, Node *cfn, State *st,
                                 Obligations *ob, const char *callee, const char *rv) {
    const char *pn = g->args.data[0]->name;
    HandleEntry *we = NULL;
    const char *an = NULL;
    for (int j = 0; j < cfn->params.len && j < call->args.len; j++) {
        if (strcmp(cfn->params.data[j]->param_name, pn) != 0) continue;
        Node *aa = call->args.data[j];
        if (aa->kind == NODE_IDENT) {
            an = aa->name;
            we = handles_find(&st->handles, an);
            if (we && we->kind != HK_WORLD) we = NULL;
        }
        break;
    }
    char wlabel[300];
    if (we && we->state == 1) {
        ConsList opath; cl_init(&opath);
        for (int x = 0; x < st->path.n; x++) { Constraint *c = &st->path.c[x]; cl_push(&opath, mk_cons(lin_clone(&c->lhs), c->op, c->rhs)); }
        ConsList empty; cl_init(&empty);
        snprintf(wlabel, sizeof wlabel, "requires world_frozen на '%s' при извикване", callee);
        obl_push(ob, opath, empty, (ConsList){NULL, 0, 0}, sym_lin(lin_var(rv)), &st->vlen, &st->reads, 0, 2, wlabel);
        return 1;
    }
    if (we) {
        snprintf(wlabel, sizeof wlabel,
            "requires world_frozen на '%s': предаден %s свят '%s'", callee,
            we->state == 0 ? "мутабелен" : we->state == 2 ? "освободен" : "вече преместен",
            an);
        push_protocol_violation(st, ob, wlabel);
        return 0;
    }
    ConsList opath; cl_init(&opath);
    for (int x = 0; x < st->path.n; x++) { Constraint *c = &st->path.c[x]; cl_push(&opath, mk_cons(lin_clone(&c->lhs), c->op, c->rhs)); }
    ConsList empty; cl_init(&empty);
    snprintf(wlabel, sizeof wlabel,
        "requires world_frozen на '%s' при извикване — състоянието на аргумента не се следи", callee);
    obl_push(ob, opath, empty, (ConsList){NULL, 0, 0}, sym_lin(lin_var(rv)), &st->vlen, &st->reads, 1, 2, wlabel);
    return 0;
}

/* M5: assume-guarantee for a user call in statement position.
 *  - Discharges the callee's requires against the caller path (conjunctive
 *    clauses only); each becomes a kind-2 obligation reported by verify_fn.
 *  - Only if ALL requires are PROVEN and the callee body is itself in the
 *    verifiable fragment (otherwise its ensures were never proven): assumes
 *    the callee's conjunctive ensures (output := fresh __cN) into the caller
 *    path. For a recursive callee this is the induction hypothesis (Hoare
 *    rule for recursion — partial correctness; termination is NOT proven).
 * Returns the symbolic result (fresh __cN). On undischargable input the state
 * is marked bad (UNKNOWN downstream — never a false proof). */
Sym eval_user_call(Node *call, State *st, Obligations *ob, const char *caller_name) {
    const char *callee = call->callee->name;
    Node *cfn = find_fn(g_prog, callee);
    Node *cspec = find_spec(g_prog, callee);
    if (caller_name && strcmp(callee, caller_name) == 0) g_partial = 1;

    char rv[64]; snprintf(rv, sizeof rv, "__c%d", g_call_ctr++);

    SEnv env2; env_init(&env2);
    int ok = cfn && call->args.len == cfn->params.len;
    for (int i = 0; ok && i < cfn->params.len; i++) {
        /* W2.1: аргумент-свят е opaque handle (HK_WORLD е по ИМЕ, не Lin) —
         * ghost-изискванията се разтоварят по име в цикъла по-долу, затова
         * светът не се bind-ва в env2. Без това целият state ставаше bad
         * (нелинеен аргумент) и протоколът мълчеше вместо да доказва. */
        Node *aa = call->args.data[i];
        if (aa && aa->kind == NODE_IDENT) {
            HandleEntry *he = handles_find(&st->handles, aa->name);
            if (he && he->kind == HK_WORLD) continue;
        }
        Sym a = se_from_ast(call->args.data[i], &st->env, &st->vlen, &st->reads);
        if (a.nonlinear) { lin_free(&a.lin); ok = 0; break; }
        env_bind(&env2, cfn->params.data[i]->param_name, a.lin, 0);   /* moves a.lin */
    }
    env_bind(&env2, "output", lin_var(rv), 0);
    if (!ok) { st->bad = 1; env_free(&env2); return sym_lin(lin_var(rv)); }

    int all_req_proven = 1;
    inject_prod_axioms(st);   /* M8b: фактите за продукти влизат в opath */
    for (int r = 0; r < cspec->spec_requires.len; r++) {
        Node *req = cspec->spec_requires.data[r]->ensure_expr;
        /* W2.1: ghost предикат world_frozen(p) — caller страната на двустранния
         * протокол. Аргументът се разтоварва срещу HK_WORLD състоянието на
         * извикващия: frozen(1) → ДОКАЗАНО (ensures-ът на callee се поема);
         * mutable/freed/moved → ОБРОЧЕНО с witness по живия път (kind-3, като
         * другите протоколни нарушения — мъртъв път остава vacuously PROVEN);
         * неследен handle → честен UNKNOWN и нищо не се поема.
         * Конюнктите се обхождат: callee-то регистрира ghost-а и в `a && b`
         * (extract_world_ghost), значи и тук трябва да се разтовари — иначе
         * assume-guarantee се разминава. Аритметичната част остава за общия
         * път, защото bool_to_dnf третира ghost-а като TRUE. */
        if (world_req_has_ghost(req)) {
            Node *conj[8]; int nconj = 0, wtrunc = 0;
            collect_world_ghosts(req, conj, &nconj, 8, &wtrunc);
            for (int k = 0; k < nconj; k++)
                if (!world_ghost_discharge(conj[k], call, cfn, st, ob, callee, rv))
                    all_req_proven = 0;
            if (wtrunc) {
                /* повече ghost-конюнкта от капацитета: callee-то ги регистрира
                 * ВСИЧКИ (extract_world_ghost няма кап), значи гаранцията не е
                 * разтоварена — честно UNKNOWN, никога приет ensures */
                ConsList opath; cl_init(&opath);
                for (int x = 0; x < st->path.n; x++) { Constraint *c = &st->path.c[x]; cl_push(&opath, mk_cons(lin_clone(&c->lhs), c->op, c->rhs)); }
                ConsList empty; cl_init(&empty);
                char wlabel[300];
                snprintf(wlabel, sizeof wlabel,
                    "requires world_frozen на '%s' при извикване — твърде много ghost-конюнкта", callee);
                obl_push(ob, opath, empty, (ConsList){NULL, 0, 0}, sym_lin(lin_var(rv)), &st->vlen, &st->reads, 1, 2, wlabel);
                all_req_proven = 0;
            }
            if (is_world_ghost_call(req)) continue;   /* чист ghost — нищо аритметично */
        }
        /* M16: element/content requires (v[*] ..., sorted(v)) are discharged
         * against the caller's axioms on the ACTUAL argument — not vacuously */
        if (has_elem_ref(req)) {
            AxiomList tmp; ax_init(&tmp);
            extract_elem_axiom(req, &env2, &tmp);
            int ok = 0;
            if (tmp.n > 0) {
                for (int j = 0; j < cfn->params.len && j < call->args.len && !ok; j++) {
                    if (!elem_req_mentions(req, cfn->params.data[j]->param_name)) continue;
                    char kbuf[64];
                    const char *key = resolve_key(call->args.data[j], st, kbuf, sizeof kbuf);
                    if (key) ok = axiom_entailed(st, key, tmp.a[0].cmp, &tmp.a[0].rhs, tmp.a[0].is_sorted);
                }
            }
            ax_free(&tmp);
            char label[300];
            snprintf(label, sizeof label, "елементен/канален инвариант на '%s' при извикване", callee);
            ConsList opath; cl_init(&opath);
            for (int x = 0; x < st->path.n; x++) { Constraint *c = &st->path.c[x]; cl_push(&opath, mk_cons(lin_clone(&c->lhs), c->op, c->rhs)); }
            ConsList empty; cl_init(&empty);
            obl_push(ob, opath, empty, (ConsList){NULL, 0, 0}, sym_lin(lin_var(rv)), &st->vlen, &st->reads, !ok, 2, label);
            if (!ok) all_req_proven = 0;
            continue;
        }
        char label[300];
        snprintf(label, sizeof label, "requires на '%s' при извикване", callee);
        ConsList opath; cl_init(&opath);
        for (int x = 0; x < st->path.n; x++) { Constraint *c = &st->path.c[x]; cl_push(&opath, mk_cons(lin_clone(&c->lhs), c->op, c->rhs)); }
        Formula rf;
        int rok = bool_to_dnf(cspec->spec_requires.data[r]->ensure_expr, &env2, &st->vlen, &st->reads, 0, &rf);
        if (!rok || rf.n != 1) {
            /* disjunctive/unsupported requires — honest UNKNOWN, nothing assumed */
            if (rok) f_free(&rf);
            ConsList empty; cl_init(&empty);
            obl_push(ob, opath, empty, (ConsList){NULL, 0, 0}, sym_lin(lin_var(rv)), &st->vlen, &st->reads, 1, 2, label);
            all_req_proven = 0;
            continue;
        }
        ConsList pos; cl_init(&pos);
        for (int x = 0; x < rf.br[0].n; x++) { Constraint *c = &rf.br[0].c[x]; cl_push(&pos, mk_cons(lin_clone(&c->lhs), c->op, c->rhs)); }
        f_free(&rf);
        if (!cl_implied_by(&opath, &pos)) all_req_proven = 0;
        obl_push(ob, opath, pos, (ConsList){NULL, 0, 0}, sym_lin(lin_var(rv)), &st->vlen, &st->reads, 0, 2, label);
    }

    /* M16 drop rule: an argument whose callee spec carries no content
     * requires on it may be sent/pushed unchecked inside the callee — the
     * caller's axioms on it can no longer be trusted after the call. */
    if (st->ax.n > 0) {
        for (int j = 0; j < cfn->params.len && j < call->args.len; j++) {
            char kbuf[64];
            const char *key = resolve_key(call->args.data[j], st, kbuf, sizeof kbuf);
            if (!key) continue;
            int has_ax = 0;
            for (int i = 0; i < st->ax.n; i++) if (strcmp(st->ax.a[i].vec, key) == 0) { has_ax = 1; break; }
            if (!has_ax) continue;
            int declared = 0;
            for (int r = 0; r < cspec->spec_requires.len; r++)
                if (elem_req_mentions(cspec->spec_requires.data[r]->ensure_expr, cfn->params.data[j]->param_name)) { declared = 1; break; }
            if (!declared) ax_drop_key(&st->ax, key);
        }
    }

    /* M6: at a self-recursive call with a decreases measure, discharge
     * D[actuals] >= 0 and D[actuals] < D[current] (well-founded descent). */
    if (caller_name && strcmp(callee, caller_name) == 0 && cspec->spec_decreases) {
        Sym d_cur = se_from_ast(cspec->spec_decreases, &st->env, &st->vlen, &st->reads);
        Sym d_new = se_from_ast(cspec->spec_decreases, &env2, &st->vlen, NULL);
        ConsList opath; cl_init(&opath);
        for (int x = 0; x < st->path.n; x++) { Constraint *c = &st->path.c[x]; cl_push(&opath, mk_cons(lin_clone(&c->lhs), c->op, c->rhs)); }
        ConsList pos; cl_init(&pos);
        char label[300];
        if (d_cur.nonlinear || d_new.nonlinear) {
            snprintf(label, sizeof label, "терминация: decreases на '%s' не е линеен при извикването", callee);
            obl_push(ob, opath, pos, (ConsList){NULL, 0, 0}, sym_lin(lin_var(rv)), &st->vlen, &st->reads, 1, 2, label);
            g_term_failed = 1;
        } else {
            snprintf(label, sizeof label, "терминация: decreases на '%s' намалява при рекурсивното извикване", callee);
            cl_push(&pos, mk_cons(lin_neg(&d_new.lin), C_LE, rat_zero()));              /* D' >= 0 */
            cl_push(&pos, mk_cons(lin_sub(&d_new.lin, &d_cur.lin), C_LT, rat_zero())); /* D' < D */
            if (!cl_implied_by(&opath, &pos)) g_term_failed = 1;
            obl_push(ob, opath, pos, (ConsList){NULL, 0, 0}, sym_lin(lin_var(rv)), &st->vlen, &st->reads, 0, 2, label);
        }
        lin_free(&d_cur.lin);
        lin_free(&d_new.lin);
    }

    /* assume ensures only when justified: requires proven AND the callee body
     * verifiable (a skipped callee's ensures were never proven) */
    if (all_req_proven && !has_unsupported(cfn->fn_body)) {
        for (int j = 0; j < cspec->spec_ensures.len; j++) {
            Formula ef;
            if (bool_to_dnf(cspec->spec_ensures.data[j]->ensure_expr, &env2, &st->vlen, &st->reads, 0, &ef)) {
                if (ef.n == 1)
                    for (int x = 0; x < ef.br[0].n; x++) { Constraint *c = &ef.br[0].c[x]; cl_push(&st->path, mk_cons(lin_clone(&c->lhs), c->op, c->rhs)); }
                f_free(&ef);
            }
        }
    }
    env_free(&env2);
    return sym_lin(lin_var(rv));
}

/* M14: protocol violation — a kind-3 obligation whose positive part is
 * unsatisfiable (1 <= 0). On any live path this is REFUTED with the path
 * witness (same machinery as call-site requires); on a dead path the
 * obligation is vacuously PROVEN, which is correct. */
void push_protocol_violation(State *st, Obligations *ob, const char *label) {
    ConsList opath; cl_init(&opath);
    for (int x = 0; x < st->path.n; x++) { Constraint *c = &st->path.c[x]; cl_push(&opath, mk_cons(lin_clone(&c->lhs), c->op, c->rhs)); }
    ConsList pos; cl_init(&pos);
    cl_push(&pos, mk_cons(lin_const(rat_int(1)), C_LE, rat_zero()));
    obl_push(ob, opath, pos, (ConsList){NULL, 0, 0}, sym_lin(lin_const(rat_int(0))), &st->vlen, &st->reads, 0, 3, label);
}

/* M19: tautological kind-3 obligation (0 <= 0). Discharges as PROVEN on a
 * live path — used for structured wait-for acyclicity, never for a guess. */
void push_protocol_ok(State *st, Obligations *ob, const char *label) {
    ConsList opath; cl_init(&opath);
    for (int x = 0; x < st->path.n; x++) { Constraint *c = &st->path.c[x]; cl_push(&opath, mk_cons(lin_clone(&c->lhs), c->op, c->rhs)); }
    ConsList pos; cl_init(&pos);
    cl_push(&pos, mk_cons(lin_const(rat_int(0)), C_LE, rat_zero()));
    obl_push(ob, opath, pos, (ConsList){NULL, 0, 0}, sym_lin(lin_const(rat_int(0))), &st->vlen, &st->reads, 0, 3, label);
}

