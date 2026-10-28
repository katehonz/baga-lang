/* verify_top.c — част от verify.c (виж verify_proto.h). */
#include "baga.h"
#include "verify_impl.h"

/* ============================ top-level verification ============================ */

Node *find_spec(Node *prog, const char *name) {
    for (int i = 0; i < prog->items.len; i++) {
        Node *it = prog->items.data[i];
        if (it->kind == NODE_SPEC && strcmp(it->spec_name, name) == 0 &&
            (it->spec_ensures.len > 0 || it->spec_requires.len > 0))
            return it;
    }
    return NULL;
}

static Node *unwrap_type(Node *t) {
    while (t && t->kind == NODE_TYPE_EFFECT) t = t->inner_type;
    return t;
}
int type_is_i64(Node *t) {
    t = unwrap_type(t);
    return t && t->kind == NODE_TYPE && t->type_name && strcmp(t->type_name, "i64") == 0;
}
int ret_has_effects(Node *t) {
    return t && t->kind == NODE_TYPE_EFFECT && t->n_effects > 0;
}

/* M18: does the return type declare an effect with this exact name? */
static int ret_has_effect_named(Node *t, const char *name) {
    if (!t || t->kind != NODE_TYPE_EFFECT) return 0;
    for (int i = 0; i < t->n_effects; i++)
        if (strcmp(t->effect_names[i], name) == 0) return 1;
    return 0;
}

/* M14/M18: a return type whose every declared effect is Par or Overflow stays
 * inside the verifiable fragment — concurrency is modeled symbolically (M14)
 * and overflow is discharged by the M15 arith machinery (M18). Any other
 * effect dimension keeps the function out (honest skip). */
int ret_has_unverifiable_effects(Node *t) {
    if (!t || t->kind != NODE_TYPE_EFFECT) return 0;
    for (int i = 0; i < t->n_effects; i++)
        if (strcmp(t->effect_names[i], "Par") != 0 &&
            strcmp(t->effect_names[i], "Overflow") != 0) return 1;
    return 0;
}

/* M18: render a witness binding list as "n = 5, m = 0" (owned string/NULL). */
static char *witness_str(IBind *wit, int wn) {
    if (wn <= 0 || !wit) return NULL;
    int cap = 1;
    for (int k = 0; k < wn; k++) cap += (int)strlen(wit[k].name) + 32;
    char *s = malloc(cap);
    s[0] = 0;
    for (int k = 0; k < wn; k++) {
        char tmp[64];
        snprintf(tmp, sizeof tmp, "%s%s = %lld", k ? ", " : "",
                 wit[k].name, (long long)wit[k].val);
        strcat(s, tmp);
    }
    return s;
}


static const char *res_word(VRes r) {
    switch (r) { case R_PROVEN: return "ДОКАЗАНО"; case R_REFUTED: return "ОБРОЧЕНО"; default: return "НЕ МОГА ДА РЕША"; }
}

/* ---- JSON output mode (--verify --json) ----
 * Machine-readable verdicts so AI agents / CI can consume the judge
 * without parsing Bulgarian prose. The exit code is unchanged:
 * 0 = nothing refuted, 1 = at least one refuted. */
int g_json = 0;
int g_json_first = 1;

void verify_set_json(int on) { g_json = on; }

static const char *res_word_json(int res) {
    switch (res) {
    case 0: return "proven";
    case 1: return "refuted";
    case 2: return "unknown";
    default: return "skipped";
    }
}

static void json_str(const char *s) {
    putchar('"');
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        switch (*p) {
        case '"':  printf("\\\""); break;
        case '\\': printf("\\\\"); break;
        case '\n': printf("\\n"); break;
        case '\r': printf("\\r"); break;
        case '\t': printf("\\t"); break;
        default:
            if (*p < 0x20) printf("\\u%04x", *p);
            else putchar(*p);
        }
    }
    putchar('"');
}

static void json_witness(char **names, long long *vals, int wn) {
    if (wn <= 0 || !names) { printf("null"); return; }
    printf("[");
    for (int k = 0; k < wn; k++) {
        if (k) printf(", ");
        printf("{\"name\": ");
        json_str(names[k]);
        printf(", \"value\": %lld}", vals[k]);
    }
    printf("]");
}

/* Verify a single bounds obligation. */
static VRes verify_bound_obl(Obligation *obl, IBind **wit, int *wn) {
    if (obl->bad) return R_UNKNOWN;
    /* The access is safe iff path ⇒ (idx>=0 ∧ idx<len). Negated, that is
     * path ∧ (idx<0 ∨ idx>=len) — a DISJUNCTION. It is UNSAT (safe) iff every
     * branch  path ∧ ¬bound_i  is UNSAT. */
    int all_proven = 1;
    for (int b = 0; b < obl->bound.n; b++) {
        ConsList sys; cl_init(&sys);
        for (int x = 0; x < obl->path.n; x++) { Constraint *c = &obl->path.c[x]; cl_push(&sys, mk_cons(lin_clone(&c->lhs), c->op, c->rhs)); }
        Constraint *bc = &obl->bound.c[b];
        /* negate  lhs {op} 0  →  (-lhs) {flipped op} 0 */
        COp negop = (bc->op == C_LT) ? C_LE : C_LT;
        cl_push(&sys, mk_cons(lin_neg(&bc->lhs), negop, bc->rhs));
        int bsat = fm_sat(&sys);   /* fm_sat frees sys */
        if (bsat) all_proven = 0;   /* this violation branch is feasible */
    }
    if (obl->bound.n == 0) all_proven = 0;
    if (all_proven) return R_PROVEN;
    IBind *w = NULL; int wnn = 0;
    if (find_bound_witness(obl, &w, &wnn)) { *wit = w; *wn = wnn; return R_REFUTED; }
    return R_UNKNOWN;
}

/* Witness for a refuted call-site requires (M5, kind 2): an assignment that
 * satisfies the caller path and violates one of the callee's requires
 * constraints. Re-checked by direct evaluation — a reported counterexample is
 * genuine. Internal __cN/__rN/__len_ vars are searched but not reported. */
static int find_callreq_witness(Obligation *obl, IBind **witness, int *wn) {
    char **vars = NULL; int nv = 0, cap = 0;
    collect_vars_cl(&obl->path, &vars, &nv, &cap);
    collect_vars_cl(&obl->bound, &vars, &nv, &cap);
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
        int viol = 0;
        for (int i = 0; i < obl->bound.n && !viol; i++) {
            Constraint *c = &obl->bound.c[i];
            Rat rv = c->lhs.c;
            int cok = 1;
            for (int j = 0; j < c->lhs.n; j++) {
                int ok2; int64_t vv = ienv_get(&ie, c->lhs.terms[j].var, &ok2);
                if (!ok2) { cok = 0; break; }
                rv = rat_add(rv, rat_mul(c->lhs.terms[j].coeff, rat_int(vv)));
            }
            if (!cok || rv.overflow) continue;
            int s = rat_cmp(rv, c->rhs);
            int holds = (c->op == C_LT) ? (s < 0) : (s <= 0);
            if (!holds) viol = 1;
        }
        if (viol) {
            /* M8 conclusiveness (както при ensures): pin всички освен __c/__p
             * (+ derived продукти); requires трябва да е невъзможно при тези
             * входове, иначе нарушението зависи от абстрактна стойност →
             * фалшива тревога. */
            ConsList sys; cl_init(&sys);
            for (int x = 0; x < obl->path.n; x++) { Constraint *c = &obl->path.c[x]; cl_push(&sys, mk_cons(lin_clone(&c->lhs), c->op, c->rhs)); }
            push_pins_and_derived(&sys, vars, assign, nv, &obl->prods);
            for (int x = 0; x < obl->bound.n; x++) { Constraint *c = &obl->bound.c[x]; cl_push(&sys, mk_cons(lin_clone(&c->lhs), c->op, c->rhs)); }
            if (fm_sat(&sys)) continue;   /* не е conclusive — търси друг witness */
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

/* ---- M15 verdict machinery ---- */

static void collect_vars_lin(Lin *l, char ***vars, int *nv, int *cap) {
    for (int j = 0; j < l->n; j++) {
        const char *vn = l->terms[j].var;
        int found = 0;
        for (int k = 0; k < *nv; k++) if (strcmp((*vars)[k], vn) == 0) { found = 1; break; }
        if (!found) { if (*nv == *cap) { *cap = *cap ? *cap * 2 : 8; *vars = realloc(*vars, (size_t)*cap * sizeof(char *)); } (*vars)[(*nv)++] = strdup(vn); }
    }
}

/* path ∧ L >= K feasible?  (K - L <= 0) */
static int feas_above(ConsList *path, Lin *L, int64_t K) {
    ConsList sys; cl_init(&sys);
    for (int x = 0; x < path->n; x++) { Constraint *c = &path->c[x]; cl_push(&sys, mk_cons(lin_clone(&c->lhs), c->op, c->rhs)); }
    Lin nl = lin_neg(L);
    nl.c = rat_add(nl.c, rat_int(K));
    if (nl.c.overflow) { lin_free(&nl); cl_free(&sys); return 1; }   /* conservative */
    cl_push(&sys, mk_cons(nl, C_LE, rat_zero()));
    return fm_sat(&sys);
}

/* path ∧ L <= K feasible?  (L - K <= 0) */
static int feas_below(ConsList *path, Lin *L, int64_t K) {
    ConsList sys; cl_init(&sys);
    for (int x = 0; x < path->n; x++) { Constraint *c = &path->c[x]; cl_push(&sys, mk_cons(lin_clone(&c->lhs), c->op, c->rhs)); }
    Lin cl2 = lin_clone(L);
    cl2.c = rat_sub(cl2.c, rat_int(K));
    if (cl2.c.overflow) { lin_free(&cl2); cl_free(&sys); return 1; }
    cl_push(&sys, mk_cons(cl2, C_LE, rat_zero()));
    return fm_sat(&sys);
}

/* Tightest K in [0, 2^62] with path ⊢ -K <= L <= K (binary search over FM
 * feasibility), or 2^62+1 ("too big to matter") when even that is unprovable. */
static int64_t fm_maxabs(ConsList *path, Lin *L) {
    const int64_t CAP = (int64_t)1 << 62;
    #define FITS(K) (!feas_above(path, L, (K) + 1) && !feas_below(path, L, -(K) - 1))
    if (!FITS(CAP)) return CAP + 1;
    int64_t lo = 0, hi = CAP;   /* P(hi) holds */
    while (lo < hi) {
        int64_t mid = lo + (hi - lo) / 2;
        if (FITS(mid)) hi = mid; else lo = mid + 1;
    }
    return lo;
    #undef FITS
}

/* Evaluate a linear form under an assignment into __int128 (no overflow).
 * ok=0 when a variable is missing (e.g. an abstract __c call result) or the
 * form is non-integral. */
static int eval_lin128(Lin *l, IBind *env, int nenv, __int128 *out) {
    if (l->overflow || l->c.overflow || l->c.den != 1) return 0;
    __int128 v = l->c.num;
    for (int j = 0; j < l->n; j++) {
        if (l->terms[j].coeff.overflow || l->terms[j].coeff.den != 1) return 0;
        int ok = 0; int64_t vv = 0;
        for (int k = 0; k < nenv; k++)
            if (strcmp(env[k].name, l->terms[j].var) == 0) { vv = env[k].val; ok = 1; break; }
        if (!ok) return 0;
        v += (__int128)l->terms[j].coeff.num * vv;
    }
    *out = v;
    return 1;
}

/* Pins (free vars at candidate values) + derived product/div/mod/bit values —
 * the value-level mirror of push_pins_and_derived. __c call results stay
 * abstract (never present in the output). */
static IBind *build_eff_env(char **vars, IBind *assign, int nv, ProdList *prods, int *out_n) {
    int cap = nv + (prods ? prods->n : 0) + 1;
    IBind *e = calloc((size_t)cap, sizeof(IBind));
    int ne = 0;
    for (int i = 0; i < nv; i++) {
        if (strncmp(vars[i], "__c", 3) == 0 || strncmp(vars[i], "__p", 3) == 0 ||
            strncmp(vars[i], "__d", 3) == 0 || strncmp(vars[i], "__m", 3) == 0 ||
            strncmp(vars[i], "__b", 3) == 0 || strncmp(vars[i], "__hv", 4) == 0) continue;
        e[ne].name = vars[i]; e[ne].val = assign[i].val; ne++;
    }
    for (int pass = 0; pass < 2; pass++) {
        for (int pi = 0; prods && pi < prods->n; pi++) {
            int have = 0;
            for (int k = 0; k < ne; k++) if (strcmp(e[k].name, prods->p[pi].var) == 0) { have = 1; break; }
            if (have) continue;
            IEnv pe; pe.b = e; pe.n = ne;
            int64_t fv[2] = {0, 0};
            Lin *fs[2] = { &prods->p[pi].fa, &prods->p[pi].fb };
            int ok = 1;
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
            e[ne].name = prods->p[pi].var; e[ne].val = pvv; ne++;
        }
    }
    *out_n = ne;
    return e;
}

/* M15 witness search: an input assignment under which the operation really
 * overflows (or divides by zero). Candidate grid includes large magnitudes —
 * overflow witnesses live near 2^63. Conclusiveness mirrors M8: the violation
 * must not depend on unrealizable abstract values. */
static int find_arith_witness(Obligation *obl, IBind **witness, int *wn) {
    char **vars = NULL; int nv = 0, cap = 0;
    collect_vars_cl(&obl->path, &vars, &nv, &cap);
    collect_vars_lin(&obl->aux1, &vars, &nv, &cap);
    collect_vars_lin(&obl->aux2, &vars, &nv, &cap);
    for (int pi = 0; pi < obl->prods.n; pi++) {
        collect_vars_lin(&obl->prods.p[pi].fa, &vars, &nv, &cap);
        collect_vars_lin(&obl->prods.p[pi].fb, &vars, &nv, &cap);
    }
    static const int64_t acand[] = {
        0, 1, -1, 2, -2, 3, -3, 5, -5, 10, -10, 100, -100,
        1000000, -1000000, 2147483647LL, -2147483648LL,
        3037000499LL, 3037000500LL, 4294967295LL, 4294967296LL, -4294967296LL,
        4611686018427387904LL, -4611686018427387904LL,
        INT64_MAX - 1, INT64_MAX, INT64_MIN + 1, INT64_MIN
    };
    int ncand = (int)(sizeof(acand) / sizeof(acand[0]));
    int total = 1;
    for (int i = 0; i < nv; i++) { if (total > 400000 / ncand) { total = 400001; break; } total *= ncand; }
    if (nv == 0) total = 1;
    IBind *assign = calloc(nv ? nv : 1, sizeof(IBind));
    int found = 0;
    for (int idx = 0; idx < total && !found; idx++) {
        int t = idx;
        for (int i = 0; i < nv; i++) { assign[i].name = vars[i]; assign[i].val = acand[t % ncand]; t /= ncand; }
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
        int ne = 0;
        IBind *eff = build_eff_env(vars, assign, nv, &obl->prods, &ne);
        __int128 a = 0, b = 0;
        int oka = eval_lin128(&obl->aux1, eff, ne, &a);
        int okb = eval_lin128(&obl->aux2, eff, ne, &b);
        int viol = 0, concrete = 0;
        switch (obl->akind) {
        case AK_FIT:
            concrete = oka;
            viol = oka && (a > INT64_MAX || a < INT64_MIN);
            break;
        case AK_MUL:
            concrete = oka && okb;
            viol = concrete && (a * b > INT64_MAX || a * b < INT64_MIN);
            break;
        case AK_DIVZ:
            concrete = oka && okb;
            viol = concrete && (b == 0 || (b == -1 && a == INT64_MIN));
            break;
        }
        free(eff);
        int conclusive = 0;
        if (viol && concrete) {
            /* the violation depends only on pinned/derived values: accept iff
             * the pinned inputs stay feasible under the path */
            ConsList sys; cl_init(&sys);
            for (int x = 0; x < obl->path.n; x++) { Constraint *c = &obl->path.c[x]; cl_push(&sys, mk_cons(lin_clone(&c->lhs), c->op, c->rhs)); }
            push_pins_and_derived(&sys, vars, assign, nv, &obl->prods);
            conclusive = fm_sat(&sys);
        }
        if (!conclusive) continue;
        IBind *w = calloc(nv ? nv : 1, sizeof(IBind));
        int k = 0;
        for (int i = 0; i < nv; i++) {
            if (strncmp(vars[i], "__", 2) == 0) continue;
            w[k].name = strdup(vars[i]); w[k].val = assign[i].val; k++;
        }
        *witness = w; *wn = k;
        found = 1;
    }
    free(assign);
    for (int i = 0; i < nv; i++) free(vars[i]);
    free(vars);
    return found;
}

/* Verify one arithmetic-safety obligation (M15, kind 4). */
static VRes verify_arith_obl(Obligation *obl, IBind **wit, int *wn) {
    if (obl->bad) return R_UNKNOWN;
    if (obl->akind == AK_FIT && obl->aux1.overflow) {
        /* constant arithmetic overflowed i64 at fold time — unconditional */
        *wit = calloc(1, sizeof(IBind)); *wn = 0;
        return R_REFUTED;
    }
    int proven = 0;
    switch (obl->akind) {
    case AK_FIT: {
        /* |L| <= 2^62 provable via FM bound search ⇒ fits i64. The extreme
         * window (2^62, 2^63) is reported UNKNOWN, never falsely proven —
         * documented limitation of the exact-rational core near int64 max. */
        int64_t ma = fm_maxabs(&obl->path, &obl->aux1);
        if (getenv("BAGA_DEBUG_ARITH")) {
            fprintf(stderr, "FIT maxabs=%lld path.n=%d\n", (long long)ma, obl->path.n);
            for (int x = 0; x < obl->path.n; x++) {
                Constraint *c = &obl->path.c[x];
                fprintf(stderr, "  path[%d] c=%lld op=%d terms:", x, (long long)c->lhs.c.num, c->op);
                for (int j = 0; j < c->lhs.n; j++) fprintf(stderr, " %s*%lld", c->lhs.terms[j].var, (long long)c->lhs.terms[j].coeff.num);
                fprintf(stderr, "\n");
            }
        }
        proven = (ma <= ((int64_t)1 << 62));
        break;
    }
    case AK_MUL: {
        int64_t A = fm_maxabs(&obl->path, &obl->aux1);
        int64_t B = fm_maxabs(&obl->path, &obl->aux2);
        __int128 p = (__int128)A * B;
        proven = (p <= INT64_MAX);
        break;
    }
    case AK_DIVZ: {
        /* divisor == 0 feasible? */
        ConsList sys; cl_init(&sys);
        for (int x = 0; x < obl->path.n; x++) { Constraint *c = &obl->path.c[x]; cl_push(&sys, mk_cons(lin_clone(&c->lhs), c->op, c->rhs)); }
        cl_push(&sys, mk_cons(lin_clone(&obl->aux2), C_LE, rat_zero()));
        cl_push(&sys, mk_cons(lin_neg(&obl->aux2), C_LE, rat_zero()));
        int zero_feas = fm_sat(&sys);
        /* divisor == -1 ∧ dividend == INT64_MIN feasible? */
        ConsList s2; cl_init(&s2);
        for (int x = 0; x < obl->path.n; x++) { Constraint *c = &obl->path.c[x]; cl_push(&s2, mk_cons(lin_clone(&c->lhs), c->op, c->rhs)); }
        Lin dp1 = lin_clone(&obl->aux2); dp1.c = rat_add(dp1.c, rat_int(1));
        cl_push(&s2, mk_cons(dp1, C_LE, rat_zero()));                 /* d+1 <= 0 */
        Lin ndp1 = lin_neg(&obl->aux2); ndp1.c = rat_sub(ndp1.c, rat_int(1));
        cl_push(&s2, mk_cons(ndp1, C_LE, rat_zero()));                /* -d-1 <= 0 */
        cl_push(&s2, mk_cons(lin_clone(&obl->aux1), C_LE, rat_int(INT64_MIN)));  /* n <= MIN */
        Lin nn = lin_neg(&obl->aux1); nn.c = rat_sub(nn.c, rat_int(1));
        cl_push(&s2, mk_cons(nn, C_LE, rat_int(INT64_MAX)));          /* n >= MIN */
        int min_feas = fm_sat(&s2);
        proven = !zero_feas && !min_feas;
        break;
    }
    }
    if (proven) return R_PROVEN;
    IBind *w = NULL; int wnn = 0;
    if (find_arith_witness(obl, &w, &wnn)) { *wit = w; *wn = wnn; return R_REFUTED; }
    return R_UNKNOWN;
}

/* Verify a single call-site requires obligation (M5, kind 2): the caller path
 * must imply every (positive) requires constraint of the callee. */
static VRes verify_call_req(Obligation *obl, IBind **wit, int *wn) {
    if (obl->bad) return R_UNKNOWN;
    for (int i = 0; i < obl->bound.n; i++) {
        ConsList sys; cl_init(&sys);
        for (int x = 0; x < obl->path.n; x++) { Constraint *c = &obl->path.c[x]; cl_push(&sys, mk_cons(lin_clone(&c->lhs), c->op, c->rhs)); }
        Constraint *bc = &obl->bound.c[i];
        COp negop = (bc->op == C_LT) ? C_LE : C_LT;
        cl_push(&sys, mk_cons(lin_neg(&bc->lhs), negop, bc->rhs));
        int sat = fm_sat(&sys);   /* frees sys */
        if (sat) {
            IBind *w = NULL; int wnn = 0;
            if (find_callreq_witness(obl, &w, &wnn)) { *wit = w; *wn = wnn; return R_REFUTED; }
            return R_UNKNOWN;
        }
    }
    return R_PROVEN;
}

/* Verify one ensures clause over all obligations of a function. */
static VRes verify_ensures(Node *spec, Node *ens_expr, Obligations *ob,
                           IBind **wit, int *wn) {
    VRes worst = R_PROVEN;
    for (int o = 0; o < ob->n; o++) {
        Obligation *obl = &ob->o[o];
        if (obl->bad) { if (worst == R_PROVEN) worst = R_UNKNOWN; continue; }
        if (obl->kind != 0) continue;   /* bounds / call-requires: handled separately */

        if (obl->ret.nonlinear) { if (worst == R_PROVEN) worst = R_UNKNOWN; continue; }
        /* env with output := return */
        SEnv env2; env_init(&env2);
        env_bind(&env2, "output", lin_clone(&obl->ret.lin), 0);
        /* requires as antecedent constraints */
        ConsList ante; cl_init(&ante);
        int ante_ok = 1;
        for (int r = 0; r < spec->spec_requires.len && ante_ok; r++) {
            Formula rf;
            if (!bool_to_dnf(spec->spec_requires.data[r]->ensure_expr, &env2, &obl->vlen, NULL, 0, &rf)) { ante_ok = 0; break; }
            if (rf.n != 1) ante_ok = 0;   /* disjunctive requires: M0 skips */
            else for (int x = 0; x < rf.br[0].n; x++) { Constraint *c = &rf.br[0].c[x]; cl_push(&ante, mk_cons(lin_clone(&c->lhs), c->op, c->rhs)); }
            f_free(&rf);
        }
        for (int x = 0; x < obl->path.n && ante_ok; x++) { Constraint *c = &obl->path.c[x]; cl_push(&ante, mk_cons(lin_clone(&c->lhs), c->op, c->rhs)); }
        /* M3: instantiated element-axiom constraints on the read value */
        for (int x = 0; x < obl->read_cons.n && ante_ok; x++) { Constraint *c = &obl->read_cons.c[x]; cl_push(&ante, mk_cons(lin_clone(&c->lhs), c->op, c->rhs)); }
        if (!ante_ok) { cl_free(&ante); env_free(&env2); if (worst == R_PROVEN) worst = R_UNKNOWN; continue; }

        /* ensures as DNF, and its negation as DNF (De Morgan handled inside
         * bool_to_dnf, so a disjunctive ensures becomes a conjunction when
         * negated). The obligation  ante ⇒ ensures  is checked per negated
         * branch:  UNSAT(ante ∧ ¬ensures_branch)  ⇒ that branch always holds. */
        Formula ef, nef;
        if (!bool_to_dnf(ens_expr, &env2, &obl->vlen, NULL, 0, &ef)) { cl_free(&ante); env_free(&env2); if (worst == R_PROVEN) worst = R_UNKNOWN; continue; }
        if (!bool_to_dnf(ens_expr, &env2, &obl->vlen, NULL, 1, &nef)) { f_free(&ef); cl_free(&ante); env_free(&env2); if (worst == R_PROVEN) worst = R_UNKNOWN; continue; }

        VRes clause_res = R_PROVEN;
        for (int b = 0; b < nef.n && clause_res != R_REFUTED; b++) {
            ConsList sys; cl_init(&sys);
            for (int x = 0; x < ante.n; x++) { Constraint *c = &ante.c[x]; cl_push(&sys, mk_cons(lin_clone(&c->lhs), c->op, c->rhs)); }
            for (int x = 0; x < nef.br[b].n; x++) {
                Constraint *c = &nef.br[b].c[x];
                cl_push(&sys, mk_cons(lin_clone(&c->lhs), c->op, c->rhs));
            }
            int sat = fm_sat(&sys);   /* frees sys */
            if (!sat) {
                /* PROVEN for this branch */
            } else {
                IBind *w = NULL; int wnn = 0;
                /* rebuild antecedent for witness search */
                ConsList ante2; cl_init(&ante2);
                for (int x = 0; x < ante.n; x++) { Constraint *c = &ante.c[x]; cl_push(&ante2, mk_cons(lin_clone(&c->lhs), c->op, c->rhs)); }
                if (find_counterexample(&ante2, &nef, &ef, ens_expr, &env2, &obl->prods, &w, &wnn)) {
                    clause_res = R_REFUTED;
                    if (!*wit) { *wit = w; *wn = wnn; }
                    else { for (int i = 0; i < wnn; i++) free(w[i].name); free(w); }
                } else {
                    if (clause_res == R_PROVEN) clause_res = R_UNKNOWN;
                }
                cl_free(&ante2);
            }
        }
        f_free(&ef);
        f_free(&nef);
        cl_free(&ante);
        env_free(&env2);
        if (clause_res == R_REFUTED) worst = R_REFUTED;
        else if (clause_res == R_UNKNOWN && worst == R_PROVEN) worst = R_UNKNOWN;
    }
    return worst;
}

/* Verify one function. Returns 1 if any ensures was REFUTED. */
/* ---- M3: extract element axioms (v[*] <cmp> <linear>) from annotations ---- */

/* Does this annotation mention an element reference v[*]? Such invariants are
 * element axioms (handled separately, vacuous at init), not scalar constraints. */
int has_elem_ref(Node *e) {
    if (!e) return 0;
    if (e->kind == NODE_ELEM_REF) return 1;
    if (is_sorted_call(e)) return 1;   /* relational axiom, not a scalar inv */
    if (e->kind == NODE_BINARY) return has_elem_ref(e->left) || has_elem_ref(e->right);
    if (e->kind == NODE_UNARY) return has_elem_ref(e->operand);
    return 0;
}

/* Recognize `v[*] <cmp> <linear>` (or mirrored) and add it as an axiom.
 * Also recognizes the relational predicate `sorted(v)`. */
void extract_elem_axiom(Node *e, SEnv *env, AxiomList *ax) {
    if (!e) return;
    /* sorted(v) — relational axiom */
    if (e->kind == NODE_CALL && e->callee && e->callee->kind == NODE_IDENT &&
        strcmp(e->callee->name, "sorted") == 0 && e->args.len == 1 &&
        e->args.data[0]->kind == NODE_IDENT) {
        Lin dummy; lin_init(&dummy);
        ax_push(ax, e->args.data[0]->name, EC_LE, dummy, 1);
        return;
    }
    if (e->kind != NODE_BINARY) return;
    BinOp op = e->bin_op;
    if (op != OP_LT && op != OP_LE && op != OP_GT && op != OP_GE) return;
    Node *l = e->left, *r = e->right;
    Node *eref = NULL; Node *other = NULL; int elem_on_left = 0;
    if (l->kind == NODE_ELEM_REF) { eref = l; other = r; elem_on_left = 1; }
    else if (r->kind == NODE_ELEM_REF) { eref = r; other = l; elem_on_left = 0; }
    if (!eref || eref->elem_obj->kind != NODE_IDENT) return;
    Sym s = se_from_ast(other, env, NULL, NULL);
    if (s.nonlinear) { lin_free(&s.lin); return; }
    ElemCmp cmp;
    if (elem_on_left) {
        switch (op) { case OP_LT: cmp = EC_LT; break; case OP_LE: cmp = EC_LE; break;
                      case OP_GT: cmp = EC_GT; break; default: cmp = EC_GE; break; }
    } else {
        switch (op) { case OP_LT: cmp = EC_GT; break; case OP_LE: cmp = EC_GE; break;
                      case OP_GT: cmp = EC_LT; break; default: cmp = EC_LE; break; }
    }
    ax_push(ax, eref->elem_obj->name, cmp, s.lin, 0);
}

/* Instantiate an axiom at a concrete read: produce the constraint
 * `read_var <cmp> rhs` (the element value satisfies the predicate).
 * Caller must not pass is_sorted axioms (they yield no scalar fact). */
Constraint axiom_instantiate(ElemAxiom *a, const char *read_var) {
    Lin lhs = lin_var(read_var);
    COp op;
    switch (a->cmp) { case EC_LT: op = C_LT; break; case EC_LE: op = C_LE; break;
                      case EC_GT: op = C_LE; break; default: op = C_LT; break; }
    /* elem <cmp> rhs:
       EC_LT: read < rhs   -> read - rhs < 0
       EC_LE: read <= rhs  -> read - rhs <= 0
       EC_GT: read > rhs   -> rhs - read < 0
       EC_GE: read >= rhs  -> rhs - read <= 0 */
    Lin d;
    if (a->cmp == EC_GT || a->cmp == EC_GE) d = lin_sub(&a->rhs, &lhs);
    else d = lin_sub(&lhs, &a->rhs);
    lin_free(&lhs);
    return mk_cons(d, op, rat_zero());
}

/* Check that a pushed value `val` satisfies the axiom (val <cmp> rhs) under the
 * current path. Returns 1 if proven (so the axiom survives the push).
 * is_sorted axioms are not scalar — caller must use state_proves_all_le. */
int axiom_holds_for_value(ElemAxiom *a, Sym *val, State *st) {
    if (a->is_sorted) return 0;
    if (val->nonlinear) return 0;
    /* obligation: path => val <cmp> rhs; negate -> path ∧ ¬(val <cmp> rhs) UNSAT */
    ConsList sys; cl_init(&sys);
    for (int x = 0; x < st->path.n; x++) { Constraint *c = &st->path.c[x]; cl_push(&sys, mk_cons(lin_clone(&c->lhs), c->op, c->rhs)); }
    /* ¬(val <cmp> rhs): build val<cmp>rhs as a constraint, then negate it */
    Lin lhs = lin_clone(&val->lin);
    COp posop;
    Lin d;
    if (a->cmp == EC_GT || a->cmp == EC_GE) { d = lin_sub(&a->rhs, &lhs); posop = (a->cmp == EC_GT) ? C_LT : C_LE; }
    else { d = lin_sub(&lhs, &a->rhs); posop = (a->cmp == EC_LT) ? C_LT : C_LE; }
    lin_free(&lhs);
    COp negop = (posop == C_LT) ? C_LE : C_LT;
    cl_push(&sys, mk_cons(lin_neg(&d), negop, rat_zero()));
    lin_free(&d);
    int sat = fm_sat(&sys);   /* frees sys */
    return !sat;
}

int verify_fn_collect(Node *prog, Node *fn, FnVerifyRes *out) {
    memset(out, 0, sizeof *out);
    g_prog = prog;              /* M5: resolve user calls during symexec */
    g_caller_name = fn->fn_name;
    g_partial = 0;
    g_call_ctr = 0;
    g_handle_ctr = 0;
    g_havoc_ctr = 0;
    g_prod_ctr = 0;
    g_term = 0;
    g_term_failed = 0;
    inv_collect_reset();
    Node *spec = find_spec(prog, fn->fn_name);
    if (!spec) return -1;
    if (spec->spec_decreases) g_term = 1;

    /* fragment gates */
    const char *skip = NULL;
    if (ret_has_unverifiable_effects(fn->ret_type)) skip = "ефекти (извън Par/Overflow)";
    else if (fn->ret_type && !type_is_i64(fn->ret_type) && unwrap_type(fn->ret_type)->kind != NODE_TYPE) skip = "не-i64 резултат";
    else if (fn->ret_type && unwrap_type(fn->ret_type)->kind == NODE_TYPE &&
             strcmp(unwrap_type(fn->ret_type)->type_name, "i64") != 0) skip = "не-i64 резултат";
    if (!skip) for (int i = 0; i < fn->params.len; i++) {
        Node *pt = unwrap_type(fn->params.data[i]->param_type);
        int ok = type_is_i64(fn->params.data[i]->param_type) ||
                 (pt && pt->kind == NODE_TYPE_ARRAY);
        if (!ok) { skip = "не-i64/Vec параметър"; break; }
    }
    if (!skip && has_unsupported(fn->fn_body)) skip = "цикли/рекурсия/повиквания";

    int ne = spec->spec_ensures.len;
    out->ens = calloc(ne > 0 ? ne : 1, sizeof(EnsVerifyRes));
    out->n_ens = ne;

    if (skip) {
        out->skipped = 1;
        out->skip_reason = skip;
        out->ovf_res = 3;   /* M18: skipped ⇒ no overflow-safety claim */
        for (int j = 0; j < ne; j++) {
            out->ens[j].res = 3;
            out->ens[j].ens_text = spec->spec_ensures.data[j]->ensure_text;
            out->ens[j].skip_reason = skip;
        }
        return 0;
    }

    /* symexec */
    Obligations ob; ob.o = NULL; ob.n = 0; ob.cap = 0;
    State init; env_init(&init.env); vlen_init(&init.vlen); ax_init(&init.ax);
    cl_init(&init.path); cl_init(&init.read_cons); reads_init(&init.reads); handles_init(&init.handles); init.bad = 0;
    for (int i = 0; i < fn->params.len; i++) {
        Node *p = fn->params.data[i];
        env_bind(&init.env, p->param_name, lin_var(p->param_name), 0);
        Node *pt = unwrap_type(p->param_type);
        if (pt && pt->kind == NODE_TYPE_ARRAY) {
            char buf[300]; snprintf(buf, sizeof buf, "__len_%s", p->param_name);
            vlen_set(&init.vlen, p->param_name, lin_var(buf), 0);
        }
    }
    for (int r = 0; r < spec->spec_requires.len; r++)
        extract_elem_axiom(spec->spec_requires.data[r]->ensure_expr, &init.env, &init.ax);
    for (int r = 0; r < spec->spec_requires.len; r++) {
        Formula rf;
        if (bool_to_dnf(spec->spec_requires.data[r]->ensure_expr, &init.env, &init.vlen, &init.reads, 0, &rf)) {
            if (rf.n == 1) for (int x = 0; x < rf.br[0].n; x++) { Constraint *c = &rf.br[0].c[x]; cl_push(&init.path, mk_cons(lin_clone(&c->lhs), c->op, c->rhs)); }
            f_free(&rf);
        }
    }
    States states; states.s = NULL; states.n = 0; states.cap = 0;
    states_push(&states, init);
    int is_nonvoid = (fn->ret_type != NULL);
    push_term_entry_obl(fn, spec, &states.s[0], &ob);   /* M6: D >= 0 при входа */
    symexec_stmts(&fn->fn_body->stmts, &states, &ob, is_nonvoid, spec);
    for (int i = 0; i < states.n; i++) state_free(&states.s[i]);
    free(states.s);

    for (int j = 0; j < ne; j++) {
        IBind *wit = NULL; int wn = 0;
        VRes r = verify_ensures(spec, spec->spec_ensures.data[j]->ensure_expr, &ob, &wit, &wn);
        out->ens[j].res = (int)r;
        out->ens[j].ens_text = spec->spec_ensures.data[j]->ensure_text;
        out->ens[j].wn = wn;
        if (wn > 0 && wit) {
            out->ens[j].wit_names = malloc(wn * sizeof(char *));
            out->ens[j].wit_vals = malloc(wn * sizeof(long long));
            for (int k = 0; k < wn; k++) {
                out->ens[j].wit_names[k] = wit[k].name;
                out->ens[j].wit_vals[k] = (long long)wit[k].val;
            }
            free(wit);
        }
        /* M31: пълен BV фрагмент — когато FM каже НЕ МОГА (променлив
         * XOR/маска, wrap като стойност, символен shift), bit-blasting до
         * 64-битов CNF + in-house DPLL решават ТОЧНО или остават честни.
         * PROVEN само при UNSAT; REFUTED само с witness, презареден през
         * директна интерпретация (M8 дисциплина). */
        if (out->ens[j].res == (int)R_UNKNOWN) {
            char **bwn = NULL; long long *bwv = NULL; int bwn_n = 0;
            int bvr = bv_verify_ensures(fn, spec, j, &bwn, &bwv, &bwn_n);
            if (bvr == 1) {
                out->ens[j].res = (int)R_PROVEN;
            } else if (bvr == 2) {
                out->ens[j].res = (int)R_REFUTED;
                out->ens[j].wit_names = bwn;
                out->ens[j].wit_vals = bwv;
                out->ens[j].wn = bwn_n;
            }
        }
    }

    /* M22: guarantee редове — ако текстът се парсира като израз, се
     * верифицира със същата дисциплина (PROVEN/REFUTED/UNKNOWN);
     * иначе остава проза (res = -1). */
    {
        int ng = spec->n_guarantees;
        out->guar = calloc(ng > 0 ? ng : 1, sizeof(EnsVerifyRes));
        out->n_guar = ng;
        for (int j = 0; j < ng; j++) {
            EnsVerifyRes *g = &out->guar[j];
            g->ens_text = spec->spec_guarantees[j];
            g->res = -1;   /* проза по подразбиране */
            if (skip) {
                g->res = 3;
                g->skip_reason = skip;
                continue;
            }
            Node *gex = parse_expr_string(spec->spec_guarantees[j]);
            if (!gex) continue;
            IBind *wit = NULL; int wn = 0;
            VRes r = verify_ensures(spec, gex, &ob, &wit, &wn);
            g->res = (int)r;
            g->wn = wn;
            if (wn > 0 && wit) {
                g->wit_names = malloc(wn * sizeof(char *));
                g->wit_vals = malloc(wn * sizeof(long long));
                for (int k = 0; k < wn; k++) {
                    g->wit_names[k] = wit[k].name;
                    g->wit_vals[k] = (long long)wit[k].val;
                }
                free(wit);
            }
        }
    }
    /* bounds obligations (M2) — check but don't store in EnsVerifyRes */
    for (int i = 0; i < ob.n; i++) {
        if (ob.o[i].kind != 1) continue;
        IBind *wit = NULL; int wn = 0;
        verify_bound_obl(&ob.o[i], &wit, &wn);
        if (wit) { for (int k = 0; k < wn; k++) free(wit[k].name); free(wit); }
    }
    /* M18: discharge the !Overflow effect. The M15 kind-4 obligations are the
     * effect inference; declaring !Overflow is a permission that discharges
     * them (the type honestly advertises the risk); omitting it claims safety,
     * which the verifier then proves, refutes, or reports UNKNOWN. */
    out->ovf_analyzed = 1;
    out->ovf_declared = ret_has_effect_named(fn->ret_type, "Overflow");
    {
        int natotal = 0, naproven = 0, any_ref = 0;
        IBind *first_wit = NULL; int first_wn = 0;
        for (int i = 0; i < ob.n; i++) {
            if (ob.o[i].kind != 4) continue;
            natotal++;
            IBind *wit = NULL; int wn = 0;
            VRes r = verify_arith_obl(&ob.o[i], &wit, &wn);
            if (r == R_PROVEN) naproven++;
            if (r == R_REFUTED) {
                any_ref = 1;
                if (!first_wit) { first_wit = wit; first_wn = wn; wit = NULL; }
            }
            if (wit) { for (int k = 0; k < wn; k++) free(wit[k].name); free(wit); }
        }
        out->ovf_safe = (natotal == 0) || (naproven == natotal);
        if (out->ovf_safe) {
            out->ovf_res = 0;                          /* safe; declaration (if any) redundant */
        } else if (any_ref) {
            out->ovf_res = out->ovf_declared ? 0 : 1;  /* discharged by declaration, else REFUTED */
            out->ovf_witness = witness_str(first_wit, first_wn);
        } else {
            out->ovf_res = out->ovf_declared ? 0 : 2;  /* discharged by declaration, else UNKNOWN */
        }
        if (first_wit) { for (int k = 0; k < first_wn; k++) free(first_wit[k].name); free(first_wit); }
    }
    for (int i = 0; i < ob.n; i++) obl_free(&ob.o[i]);
    free(ob.o);
    /* transfer verified facts (recursion/termination, invariants) to the caller */
    out->partial = g_partial;
    out->term = g_term;
    out->term_failed = g_term_failed;
    out->inv_texts = g_inv_texts; out->inv_proven = g_inv_proven; out->n_inv = g_inv_n;
    g_inv_texts = NULL; g_inv_proven = NULL; g_inv_n = g_inv_cap = 0;
    return 0;
}

void fn_verify_res_free(FnVerifyRes *r) {
    if (r->ens) {
        for (int j = 0; j < r->n_ens; j++) {
            if (r->ens[j].wit_names) {
                for (int k = 0; k < r->ens[j].wn; k++) free(r->ens[j].wit_names[k]);
                free(r->ens[j].wit_names);
            }
            free(r->ens[j].wit_vals);
        }
        free(r->ens);
        r->ens = NULL;
        r->n_ens = 0;
    }
    /* M22: guarantees */
    if (r->guar) {
        for (int j = 0; j < r->n_guar; j++) {
            if (r->guar[j].wit_names) {
                for (int k = 0; k < r->guar[j].wn; k++) free(r->guar[j].wit_names[k]);
                free(r->guar[j].wit_names);
            }
            free(r->guar[j].wit_vals);
        }
        free(r->guar);
        r->guar = NULL;
        r->n_guar = 0;
    }
    for (int j = 0; j < r->n_inv; j++) free(r->inv_texts[j]);
    free(r->inv_texts); free(r->inv_proven);
    r->inv_texts = NULL; r->inv_proven = NULL; r->n_inv = 0;
    free(r->ovf_witness);
    r->ovf_witness = NULL;
}

static int verify_fn(Node *prog, Node *fn) {
    Node *spec = find_spec(prog, fn->fn_name);
    if (!spec) return 0;

    FnVerifyRes res;
    verify_fn_collect(prog, fn, &res);

    int any_refuted = 0;
    if (g_json) {
        if (!g_json_first) printf(",\n");
        g_json_first = 0;
        printf("    {\"name\": ");
        json_str(fn->fn_name);
        printf(", \"skipped\": %s", res.skipped ? "true" : "false");
        if (res.skipped) { printf(", \"skip_reason\": "); json_str(res.skip_reason); }
        printf(", \"ensures\": [");
        for (int j = 0; j < res.n_ens; j++) {
            EnsVerifyRes *e = &res.ens[j];
            if (j) printf(", ");
            printf("{\"index\": %d, \"text\": ", j + 1);
            json_str(e->ens_text);
            printf(", \"result\": \"%s\"", res_word_json(e->res));
            if (e->res == 3) { printf(", \"skip_reason\": "); json_str(e->skip_reason); }
            printf(", \"counterexample\": ");
            json_witness(e->wit_names, e->wit_vals, e->res == 1 ? e->wn : 0);
            printf("}");
            if (e->res == 1) any_refuted = 1;
        }
        printf("]");
        /* M22: guarantees (res = -1 → проза) */
        printf(", \"guarantees\": [");
        for (int j = 0; j < res.n_guar; j++) {
            EnsVerifyRes *g = &res.guar[j];
            if (j) printf(", ");
            printf("{\"index\": %d, \"text\": ", j + 1);
            json_str(g->ens_text);
            printf(", \"result\": \"%s\"", g->res == -1 ? "prose" : res_word_json(g->res));
            if (g->res == 3) { printf(", \"skip_reason\": "); json_str(g->skip_reason); }
            printf(", \"counterexample\": ");
            json_witness(g->wit_names, g->wit_vals, g->res == 1 ? g->wn : 0);
            printf("}");
            if (g->res == 1) any_refuted = 1;
        }
        printf("]");
        if (g_partial && !res.skipped) {
            if (g_term && !g_term_failed) printf(", \"termination\": \"proven\"");
            else printf(", \"partial_correctness\": true");
        }
    } else {
        printf("verify %s:\n", fn->fn_name);
        for (int j = 0; j < res.n_ens; j++) {
            EnsVerifyRes *e = &res.ens[j];
            if (e->res == 3) {
                printf("  ensures #%d (%s): ПРОПУСНАТО (%s)\n", j + 1, e->ens_text, e->skip_reason);
            } else {
                printf("  ensures #%d (%s): %s\n", j + 1, e->ens_text, res_word((VRes)e->res));
                if (e->res == 1) {
                    any_refuted = 1;
                    printf("    контрапример:");
                    for (int k = 0; k < e->wn; k++) printf(" %s = %lld", e->wit_names[k], e->wit_vals[k]);
                    if (e->wn == 0) printf(" (без свидетел)");
                    printf("\n");
                }
            }
        }
        /* M22: guarantee редове */
        for (int j = 0; j < res.n_guar; j++) {
            EnsVerifyRes *g = &res.guar[j];
            if (g->res == -1) {
                printf("  guarantee #%d (%s): проза — извън верификационния фрагмент\n",
                       j + 1, g->ens_text);
            } else if (g->res == 3) {
                printf("  guarantee #%d (%s): ПРОПУСНАТО (%s)\n", j + 1, g->ens_text, g->skip_reason);
            } else {
                printf("  guarantee #%d (%s): %s\n", j + 1, g->ens_text, res_word((VRes)g->res));
                if (g->res == 1) {
                    any_refuted = 1;
                    printf("    контрапример:");
                    for (int k = 0; k < g->wn; k++) printf(" %s = %lld", g->wit_names[k], g->wit_vals[k]);
                    if (g->wn == 0) printf(" (без свидетел)");
                    printf("\n");
                }
            }
        }
        if (g_partial && !res.skipped) {
            if (g_term && !g_term_failed)
                printf("  (терминация: доказана чрез decreases — пълна коректност)\n");
            else
                printf("  (рекурсия: частична коректност — терминацията не се доказва)\n");
        }
    }
    /* re-run bounds for printing (cheap; keeps output identical) */
    if (!res.skipped) {
        Obligations ob2; ob2.o = NULL; ob2.n = 0; ob2.cap = 0;
        State init2; env_init(&init2.env); vlen_init(&init2.vlen); ax_init(&init2.ax);
        cl_init(&init2.path); cl_init(&init2.read_cons); reads_init(&init2.reads); handles_init(&init2.handles); init2.bad = 0;
        for (int i = 0; i < fn->params.len; i++) {
            Node *p = fn->params.data[i];
            env_bind(&init2.env, p->param_name, lin_var(p->param_name), 0);
            Node *pt = unwrap_type(p->param_type);
            if (pt && pt->kind == NODE_TYPE_ARRAY) {
                char buf[300]; snprintf(buf, sizeof buf, "__len_%s", p->param_name);
                vlen_set(&init2.vlen, p->param_name, lin_var(buf), 0);
            }
        }
        for (int r = 0; r < spec->spec_requires.len; r++)
            extract_elem_axiom(spec->spec_requires.data[r]->ensure_expr, &init2.env, &init2.ax);
        for (int r = 0; r < spec->spec_requires.len; r++) {
            Formula rf;
            if (bool_to_dnf(spec->spec_requires.data[r]->ensure_expr, &init2.env, &init2.vlen, &init2.reads, 0, &rf)) {
                if (rf.n == 1) for (int x = 0; x < rf.br[0].n; x++) { Constraint *c = &rf.br[0].c[x]; cl_push(&init2.path, mk_cons(lin_clone(&c->lhs), c->op, c->rhs)); }
                f_free(&rf);
            }
        }
        States st2; st2.s = NULL; st2.n = 0; st2.cap = 0;
        states_push(&st2, init2);
        push_term_entry_obl(fn, spec, &st2.s[0], &ob2);   /* M6: за отчета */
        symexec_stmts(&fn->fn_body->stmts, &st2, &ob2, (fn->ret_type != NULL), spec);
        for (int i = 0; i < st2.n; i++) state_free(&st2.s[i]);
        free(st2.s);
        /* call-site requires obligations (M5, kind 2) */
        if (g_json) printf(", \"calls\": [");
        int ncall = 0;
        for (int i = 0; i < ob2.n; i++) {
            if (ob2.o[i].kind != 2) continue;
            IBind *wit = NULL; int wn = 0;
            VRes r = verify_call_req(&ob2.o[i], &wit, &wn);
            if (g_json) {
                if (ncall) printf(", ");
                ncall++;
                printf("{\"label\": ");
                json_str(ob2.o[i].label ? ob2.o[i].label : "requires при извикване");
                printf(", \"result\": \"%s\", \"counterexample\": ", res_word_json((int)r));
                if (r == R_REFUTED && wit) {
                    char **names = malloc(wn * sizeof(char *));
                    long long *vals = malloc(wn * sizeof(long long));
                    for (int k = 0; k < wn; k++) { names[k] = wit[k].name; vals[k] = (long long)wit[k].val; }
                    json_witness(names, vals, wn);
                    free(names); free(vals);
                } else {
                    json_witness(NULL, NULL, 0);
                }
                printf("}");
            } else {
                printf("  извикване (%s): %s\n", ob2.o[i].label ? ob2.o[i].label : "requires при извикване", res_word(r));
                if (r == R_REFUTED) {
                    printf("    контрапример:");
                    for (int k = 0; k < wn; k++) printf(" %s = %lld", wit[k].name, (long long)wit[k].val);
                    if (wn == 0) printf(" (без свидетел)");
                    printf("\n");
                }
            }
            if (r == R_REFUTED) any_refuted = 1;
            if (wit) { for (int k = 0; k < wn; k++) free(wit[k].name); free(wit); }
        }
        if (g_json) printf("]");
        /* handle protocol obligations (M14, kind 3) — same discharge/witness
         * machinery as call-site requires */
        if (g_json) printf(", \"protocol\": [");
        int npr = 0;
        for (int i = 0; i < ob2.n; i++) {
            if (ob2.o[i].kind != 3) continue;
            IBind *wit = NULL; int wn = 0;
            VRes r = verify_call_req(&ob2.o[i], &wit, &wn);
            if (g_json) {
                if (npr) printf(", ");
                npr++;
                printf("{\"label\": ");
                json_str(ob2.o[i].label ? ob2.o[i].label : "протокол");
                printf(", \"result\": \"%s\", \"counterexample\": ", res_word_json((int)r));
                if (r == R_REFUTED && wit) {
                    char **names = malloc(wn * sizeof(char *));
                    long long *vals = malloc(wn * sizeof(long long));
                    for (int k = 0; k < wn; k++) { names[k] = wit[k].name; vals[k] = (long long)wit[k].val; }
                    json_witness(names, vals, wn);
                    free(names); free(vals);
                } else {
                    json_witness(NULL, NULL, 0);
                }
                printf("}");
            } else {
                printf("  протокол (%s): %s\n", ob2.o[i].label ? ob2.o[i].label : "handle", res_word(r));
                if (r == R_REFUTED) {
                    printf("    контрапример:");
                    for (int k = 0; k < wn; k++) printf(" %s = %lld", wit[k].name, (long long)wit[k].val);
                    if (wn == 0) printf(" (при всеки вход)");
                    printf("\n");
                }
            }
            if (r == R_REFUTED) any_refuted = 1;
            if (wit) { for (int k = 0; k < wn; k++) free(wit[k].name); free(wit); }
        }
        if (g_json) printf("]");
        if (g_json) printf(", \"bounds\": [");
        int nb = 0;
        for (int i = 0; i < ob2.n; i++) {
            if (ob2.o[i].kind != 1) continue;
            IBind *wit = NULL; int wn = 0;
            VRes r = verify_bound_obl(&ob2.o[i], &wit, &wn);
            if (g_json) {
                if (nb) printf(", ");
                nb++;
                printf("{\"label\": ");
                json_str(ob2.o[i].label ? ob2.o[i].label : "достъп до вектор");
                printf(", \"result\": \"%s\", \"counterexample\": ", res_word_json((int)r));
                if (r == R_REFUTED && wit) {
                    char **names = malloc(wn * sizeof(char *));
                    long long *vals = malloc(wn * sizeof(long long));
                    for (int k = 0; k < wn; k++) { names[k] = wit[k].name; vals[k] = (long long)wit[k].val; }
                    json_witness(names, vals, wn);
                    free(names); free(vals);
                } else {
                    json_witness(NULL, NULL, 0);
                }
                printf("}");
            } else {
                printf("  граница (%s): %s\n", ob2.o[i].label ? ob2.o[i].label : "достъп до вектор", res_word(r));
                if (r == R_REFUTED) {
                    printf("    контрапример:");
                    for (int k = 0; k < wn; k++) printf(" %s = %lld", wit[k].name, (long long)wit[k].val);
                    if (wn == 0) printf(" (без свидетел)");
                    printf("\n");
                }
            }
            if (r == R_REFUTED) any_refuted = 1;
            if (wit) { for (int k = 0; k < wn; k++) free(wit[k].name); free(wit); }
        }
        if (g_json) printf("]");
        /* arithmetic safety obligations (M15, kind 4) */
        if (g_json) printf(", \"arith\": [");
        int na = 0, naproven = 0, natotal = 0;
        for (int i = 0; i < ob2.n; i++) {
            if (ob2.o[i].kind != 4) continue;
            natotal++;
            IBind *wit = NULL; int wn = 0;
            VRes r = verify_arith_obl(&ob2.o[i], &wit, &wn);
            if (r == R_PROVEN) naproven++;
            if (g_json) {
                if (na) printf(", ");
                na++;
                printf("{\"label\": ");
                json_str(ob2.o[i].label ? ob2.o[i].label : "аритметика");
                printf(", \"result\": \"%s\", \"counterexample\": ", res_word_json((int)r));
                if (r == R_REFUTED && wit) {
                    char **names = malloc(wn * sizeof(char *));
                    long long *vals = malloc(wn * sizeof(long long));
                    for (int k = 0; k < wn; k++) { names[k] = wit[k].name; vals[k] = (long long)wit[k].val; }
                    json_witness(names, vals, wn);
                    free(names); free(vals);
                } else {
                    json_witness(NULL, NULL, 0);
                }
                printf("}");
            } else if (r != R_PROVEN) {
                printf("  аритметика (%s): %s\n", ob2.o[i].label ? ob2.o[i].label : "операция", res_word(r));
                if (r == R_REFUTED) {
                    printf("    контрапример:");
                    for (int k = 0; k < wn; k++) printf(" %s = %lld", wit[k].name, (long long)wit[k].val);
                    if (wn == 0) printf(" (при всеки вход)");
                    printf("\n");
                }
            }
            /* M18: a declared !Overflow discharges the overflow — the REFUTED
             * line stays as evidence but no longer fails verification. */
            if (r == R_REFUTED && !res.ovf_declared) any_refuted = 1;
            if (wit) { for (int k = 0; k < wn; k++) free(wit[k].name); free(wit); }
        }
        if (g_json) printf("]");   /* close arith array; object closes after overflow_effect */
        if (!g_json && natotal > 0) {
            printf("  (аритметика: %d/%d операции доказано безопасни", naproven, natotal);
            if (naproven < natotal)
                printf(" — вердиктите за ensures са в идеализирания ℤ модел");
            printf(")\n");
        }
        /* M18: !Overflow effect discharge (verdict computed in verify_fn_collect) */
        if (!g_json) {
            if (res.ovf_safe) {
                if (res.ovf_declared)
                    printf("  ефект !Overflow: деклариран, но аритметиката е доказано безопасна\n");
                else
                    printf("  ефект !Overflow: безопасна — типът е точен\n");
            } else if (res.ovf_declared) {
                if (res.ovf_witness)
                    printf("  ефект !Overflow: деклариран — прелива при %s; типът е честен\n", res.ovf_witness);
                else
                    printf("  ефект !Overflow: деклариран — безопасността не е доказуема; типът е честен\n");
            } else if (res.ovf_res == 1) {
                printf("  ефект !Overflow: прелива при %s, а !Overflow не е деклариран\n",
                       res.ovf_witness ? res.ovf_witness : "(всеки вход)");
                any_refuted = 1;
            } else {
                printf("  ефект !Overflow: безопасността не е доказуема — декларирай !Overflow\n");
            }
        } else {
            printf(", \"overflow_effect\": {\"analyzed\": true, \"declared\": %s, \"safe\": %s, \"result\": \"%s\", \"witness\": ",
                   res.ovf_declared ? "true" : "false",
                   res.ovf_safe ? "true" : "false",
                   res_word_json(res.ovf_res));
            if (res.ovf_res == 1 && res.ovf_witness) json_str(res.ovf_witness);
            else printf("null");
            printf("}}");   /* close overflow_effect object + function object */
            if (res.ovf_res == 1) any_refuted = 1;
        }
        for (int i = 0; i < ob2.n; i++) obl_free(&ob2.o[i]);
        free(ob2.o);
    } else if (g_json) {
        printf(", \"calls\": [], \"protocol\": [], \"bounds\": [], \"arith\": [], \"overflow_effect\": {\"analyzed\": false, \"result\": \"skipped\"}}");
    }
    fn_verify_res_free(&res);
    inv_collect_reset();   /* the reporting re-run above refilled the globals */
    return any_refuted;
}

int verify_program(Node *prog) {
    int any_refuted = 0;
    g_prog = prog;
    if (g_json) { g_json_first = 1; printf("{\n  \"functions\": [\n"); }
    for (int i = 0; i < prog->items.len; i++) {
        Node *it = prog->items.data[i];
        if (it->kind != NODE_FN || !it->fn_body) continue;
        if (!find_spec(prog, it->fn_name)) continue;
        any_refuted |= verify_fn(prog, it);
    }
    if (g_json) printf("\n  ]\n}\n");
    return any_refuted ? 1 : 0;
}
