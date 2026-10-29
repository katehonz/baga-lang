/* verify_core.c — част от verify.c (виж verify_proto.h). */
#include "baga.h"
#include "verify_impl.h"

/* ============================ rationals ============================ */


static int64_t v_gcd(int64_t a, int64_t b) {
    /* INT64_MIN-safe: work in __int128 so |INT64_MIN| is representable */
    __int128 x = a < 0 ? -(__int128)a : a;
    __int128 y = b < 0 ? -(__int128)b : b;
    while (y) { __int128 t = x % y; x = y; y = t; }
    return x ? (int64_t)x : 1;
}

Rat rat_bad(void) { Rat r; r.num = 0; r.den = 1; r.overflow = 1; return r; }

static Rat rat_mk(int64_t n, int64_t d) {
    if (d == 0) return rat_bad();
    if (d < 0) {
        if (n == INT64_MIN || d == INT64_MIN) return rat_bad();
        n = -n; d = -d;
    }
    int64_t g = v_gcd(n, d);
    Rat r; r.num = n / g; r.den = d / g; r.overflow = 0;
    return r;
}
Rat rat_int(int64_t n) { Rat r; r.num = n; r.den = 1; r.overflow = 0; return r; }
Rat rat_zero(void) { return rat_int(0); }

Rat rat_neg(Rat a) { if (a.overflow || a.num == INT64_MIN) return rat_bad(); Rat r = a; r.num = -r.num; return r; }

Rat rat_add(Rat a, Rat b) {
    if (a.overflow || b.overflow || a.den == 0 || b.den == 0) return rat_bad();
    int64_t g = v_gcd(a.den, b.den);
    int64_t l1 = a.den / g, l2 = b.den / g;
    __int128 n = (__int128)a.num * l2 + (__int128)b.num * l1;
    __int128 d = (__int128)a.den * l2;
    if (n > INT64_MAX || n < INT64_MIN || d > INT64_MAX) return rat_bad();
    return rat_mk((int64_t)n, (int64_t)d);
}
Rat rat_sub(Rat a, Rat b) { return rat_add(a, rat_neg(b)); }
Rat rat_mul(Rat a, Rat b) {
    if (a.overflow || b.overflow || a.den == 0 || b.den == 0) return rat_bad();
    __int128 n = (__int128)a.num * b.num;
    __int128 d = (__int128)a.den * b.den;
    if (n > INT64_MAX || n < INT64_MIN || d > INT64_MAX) return rat_bad();
    return rat_mk((int64_t)n, (int64_t)d);
}
/* -1 if a<b, 0 if equal, 1 if a>b */
int rat_cmp(Rat a, Rat b) {
    if (a.overflow || b.overflow || a.den == 0 || b.den == 0) return 0;
    __int128 l = (__int128)a.num * b.den;
    __int128 r = (__int128)b.num * a.den;
    if (l < r) return -1;
    if (l > r) return 1;
    return 0;
}
static int rat_is_neg(Rat a) { return a.num < 0; }

/* ============================ linear forms ============================ */


void lin_init(Lin *l) { l->c = rat_zero(); l->terms = NULL; l->n = 0; l->cap = 0; l->overflow = 0; }
static void lin_push_raw(Lin *l, const char *var, Rat coeff) {
    if (coeff.overflow) { l->overflow = 1; return; }
    if (coeff.num == 0) return;
    for (int i = 0; i < l->n; i++) {
        if (strcmp(l->terms[i].var, var) == 0) {
            l->terms[i].coeff = rat_add(l->terms[i].coeff, coeff);
            if (l->terms[i].coeff.overflow) l->overflow = 1;
            if (l->terms[i].coeff.num == 0) {
                free(l->terms[i].var);
                l->terms[i] = l->terms[l->n - 1];
                l->n--;
            }
            return;
        }
    }
    if (l->n == l->cap) {
        l->cap = l->cap ? l->cap * 2 : 4;
        l->terms = realloc(l->terms, (size_t)l->cap * sizeof(Term));
    }
    l->terms[l->n].var = strdup(var);
    l->terms[l->n].coeff = coeff;
    l->n++;
}
Lin lin_const(Rat c) { Lin l; lin_init(&l); l.c = c; if (c.overflow) l.overflow = 1; return l; }
Lin lin_var(const char *v) { Lin l; lin_init(&l); lin_push_raw(&l, v, rat_int(1)); return l; }
void lin_free(Lin *l) { for (int i = 0; i < l->n; i++) free(l->terms[i].var); free(l->terms); l->terms = NULL; l->n = l->cap = 0; }

Lin lin_add(Lin *a, Lin *b) {
    Lin r; lin_init(&r);
    if (a->overflow || b->overflow || a->c.overflow || b->c.overflow) { r.overflow = 1; return r; }
    r.c = rat_add(a->c, b->c);
    if (r.c.overflow) { r.overflow = 1; return r; }
    for (int i = 0; i < a->n; i++) lin_push_raw(&r, a->terms[i].var, a->terms[i].coeff);
    for (int i = 0; i < b->n; i++) lin_push_raw(&r, b->terms[i].var, b->terms[i].coeff);
    return r;
}
Lin lin_sub(Lin *a, Lin *b) {
    Lin r; lin_init(&r);
    if (a->overflow || b->overflow || a->c.overflow || b->c.overflow) { r.overflow = 1; return r; }
    r.c = rat_sub(a->c, b->c);
    if (r.c.overflow) { r.overflow = 1; return r; }
    for (int i = 0; i < a->n; i++) lin_push_raw(&r, a->terms[i].var, a->terms[i].coeff);
    for (int i = 0; i < b->n; i++) lin_push_raw(&r, b->terms[i].var, rat_neg(b->terms[i].coeff));
    return r;
}
Lin lin_scale(Lin *a, Rat k) {
    Lin r; lin_init(&r);
    if (a->overflow || k.overflow || a->c.overflow) { r.overflow = 1; return r; }
    r.c = rat_mul(a->c, k);
    if (r.c.overflow) { r.overflow = 1; return r; }
    for (int i = 0; i < a->n; i++) lin_push_raw(&r, a->terms[i].var, rat_mul(a->terms[i].coeff, k));
    return r;
}
Lin lin_neg(Lin *a) { return lin_scale(a, rat_int(-1)); }
int lin_is_const(Lin *a) { return a->n == 0; }
static Rat lin_coeff(Lin *l, const char *var) {
    for (int i = 0; i < l->n; i++) if (strcmp(l->terms[i].var, var) == 0) return l->terms[i].coeff;
    return rat_zero();
}
Lin lin_clone(Lin *a) {
    Lin r; lin_init(&r); r.c = a->c; r.overflow = a->overflow;
    for (int i = 0; i < a->n; i++) lin_push_raw(&r, a->terms[i].var, a->terms[i].coeff);
    return r;
}
/* A handle ghost is a unit-coefficient single-term form with zero constant
 * (e.g. `__h0`, `__ch1`). Anything else is not a tracked handle. */
const char *lin_handle_var(Lin *l) {
    if (!l || l->n != 1 || l->c.num != 0) return NULL;
    if (l->terms[0].coeff.num != l->terms[0].coeff.den) return NULL;
    return l->terms[0].var;
}

/* ============================ symbolic env ============================ */


void env_init(SEnv *e) { e->b = NULL; e->n = 0; e->cap = 0; }
void env_free(SEnv *e) { for (int i = 0; i < e->n; i++) { free(e->b[i].name); lin_free(&e->b[i].lin); } free(e->b); e->b = NULL; e->n = e->cap = 0; }
Binding *env_find(SEnv *e, const char *name) {
    for (int i = 0; i < e->n; i++) if (strcmp(e->b[i].name, name) == 0) return &e->b[i];
    return NULL;
}
void env_bind(SEnv *e, const char *name, Lin lin, int nonlinear) {
    if (nonlinear || lin.overflow) { lin_free(&lin); lin_init(&lin); nonlinear = 1; }
    Binding *b = env_find(e, name);
    if (b) { lin_free(&b->lin); b->lin = lin; b->nonlinear = nonlinear; return; }
    if (e->n == e->cap) { e->cap = e->cap ? e->cap * 2 : 8; e->b = realloc(e->b, (size_t)e->cap * sizeof(Binding)); }
    e->b[e->n].name = strdup(name); e->b[e->n].lin = lin; e->b[e->n].nonlinear = nonlinear; e->n++;
}
void env_clone_into(SEnv *dst, SEnv *src) {
    env_init(dst);
    for (int i = 0; i < src->n; i++) env_bind(dst, src->b[i].name, lin_clone(&src->b[i].lin), src->b[i].nonlinear);
}

/* SymExpr: a linear form, or nonlinear=true when it cannot be represented. */

Sym sym_nonlin(void) { Sym s; lin_init(&s.lin); s.nonlinear = 1; return s; }
Sym sym_lin(Lin l) { Sym s; s.lin = l; s.nonlinear = l.overflow; return s; }

/* Symbolic length of a Vec variable (M2 bounds tracking). */

void vlen_init(VLenMap *m) { m->e = NULL; m->n = 0; m->cap = 0; }
void vlen_free(VLenMap *m) { for (int i = 0; i < m->n; i++) { free(m->e[i].name); lin_free(&m->e[i].len); } free(m->e); m->e = NULL; m->n = m->cap = 0; }
VLenEntry *vlen_find(VLenMap *m, const char *name) {
    for (int i = 0; i < m->n; i++) if (strcmp(m->e[i].name, name) == 0) return &m->e[i];
    return NULL;
}
void vlen_set(VLenMap *m, const char *name, Lin len, int unknown) {
    if (unknown) { lin_free(&len); lin_init(&len); }
    VLenEntry *e = vlen_find(m, name);
    if (e) { lin_free(&e->len); e->len = len; e->unknown = unknown; return; }
    if (m->n == m->cap) { m->cap = m->cap ? m->cap * 2 : 4; m->e = realloc(m->e, (size_t)m->cap * sizeof(VLenEntry)); }
    m->e[m->n].name = strdup(name); m->e[m->n].len = len; m->e[m->n].unknown = unknown; m->n++;
}
void vlen_clone_into(VLenMap *dst, VLenMap *src) {
    vlen_init(dst);
    for (int i = 0; i < src->n; i++) vlen_set(dst, src->e[i].name, lin_clone(&src->e[i].len), src->e[i].unknown);
}

/* M3: a vec_get call resolves to a fresh symbolic read variable (defined later
 * with the State; type declared here so se_from_ast can take it).
 * M8b: a non-constant product x*y also resolves to a fresh symbolic variable
 * __pK, with the two factor linear forms kept (is_prod) so the verifier can
 * later inject TRUE facts (x*x >= 0; fa >= 1 ∧ fb >= 1 ⇒ p >= 1; ...) and the
 * witness search can derive the product's value concretely.
 * M9: integer division by a non-zero constant (is_div): dividend in fa,
 * divisor constant in fb; inject sign of quotient; witness derives trunc div.
 * M9b: n % k for nonzero const k (is_mod): bounds 0..|k|-1 when n has a sign.
 * M13: n & 1 (is_bitand1): residue in {0, mask} over two's complement — NOT C % 2
 * for negative n (where % truncates toward zero). M20 generalizes mask from 1
 * to any 2^k-1 (fb holds the mask).
 * M20: is_bitand / is_bitor — variable bitwise envelope (nonneg bounds).
 * M20: mon_deg — pure power degree of a recorded product (n^4 = deg 4).
 * M21: consecutive factors (diff ±1) inject p>=0; n^-1 rewrites to -n-1.
 * M22: n >> k за k ∈ [0,62] (is_shr): fb държи БРОЯЧА k (не делителя);
 * аритметично отместване = floor(n/2^k) — n ≥ 0 като trunc, n ≤ 0 с
 * floor-аксиоми, неизвестен знак — честно слабо. Witness-ите извличат
 * стойността чрез baga_ashr_i64 (не C `>>`, който е impl-defined).
 * M23: n >>> k за k ∈ [0,62] (is_lshr): логическо отместване (zero-fill).
 * Безусловно 0 ≤ q ≤ 2^(64-k)-1 (знаковият бит е изчистен); n ≥ 0 —
 * floor-обвивката като при >>; n ≤ -1 — q ≥ 2^(63-k). Witness-ите ползват
 * baga_lshr_i64.
 * M17: cell2/pair-returning calls (is_pair): fa/fb are the two components;
 * cell2_0/cell2_1 resolve through the pair table (exact rewrite, no theory). */
const char *reads_find(ReadsList *l, Node *call);
void reads_push_prod(ReadsList *l, const char *var, Lin fa, Lin fb);
void reads_push_div(ReadsList *l, const char *var, Lin fa, Lin fb);
void reads_push_mod(ReadsList *l, const char *var, Lin fa, Lin fb);
void reads_push_shr(ReadsList *l, const char *var, Lin fa, Lin fb);
void reads_push_lshr(ReadsList *l, const char *var, Lin fa, Lin fb);
void reads_push_bitmask(ReadsList *l, const char *var, Lin fa, int64_t mask);
void reads_push_bitop(ReadsList *l, const char *var, Lin fa, Lin fb, int is_or);
void reads_push_pair(ReadsList *l, const char *var, Lin fa, Lin fb);   /* M17 */
ReadEntry *reads_find_pair(ReadsList *l, const char *var);             /* M17 */
void prods_clone_into(ProdList *dst, ReadsList *src);
int lin_eq(Lin *a, Lin *b);   /* defined later; used by M13 XOR identity */
int g_prod_ctr = 0;

Sym se_from_ast(Node *e, SEnv *env, VLenMap *vlen, ReadsList *reads) {
    if (!e) return sym_nonlin();
    switch (e->kind) {
    case NODE_INT_LIT: return sym_lin(lin_const(rat_int(e->int_val)));
    case NODE_IDENT: {
        Binding *b = env_find(env, e->name);
        if (b) {
            if (b->nonlinear) return sym_nonlin();
            return sym_lin(lin_clone(&b->lin));
        }
        return sym_lin(lin_var(e->name));   /* unbound: a fresh symbolic input */
    }
    case NODE_UNARY:
        if (e->un_op == UOP_NEG) { Sym v = se_from_ast(e->operand, env, vlen, reads); if (v.nonlinear) return v; return sym_lin(lin_neg(&v.lin)); }
        return sym_nonlin();
    case NODE_CALL:
        /* a vec_get resolved to a fresh read variable (M3) */
        if (reads) { const char *rv = reads_find(reads, e); if (rv) return sym_lin(lin_var(rv)); }
        /* M17: cell2/pair projections — exact rewrite, no theory */
        if (e->callee && e->callee->kind == NODE_IDENT) {
            const char *pn = e->callee->name;
            if (strcmp(pn, "cell2") == 0 && e->args.len == 2 && reads) {
                Sym a = se_from_ast(e->args.data[0], env, vlen, reads);
                Sym b = se_from_ast(e->args.data[1], env, vlen, reads);
                if (a.nonlinear || b.nonlinear) { lin_free(&a.lin); lin_free(&b.lin); return sym_nonlin(); }
                char pv[64]; snprintf(pv, sizeof pv, "__q%d", g_prod_ctr++);
                reads_push_pair(reads, pv, a.lin, b.lin);   /* moves both */
                return sym_lin(lin_var(pv));
            }
            if ((strcmp(pn, "cell2_0") == 0 || strcmp(pn, "cell2_1") == 0) && e->args.len == 1 && reads) {
                Sym p = se_from_ast(e->args.data[0], env, vlen, reads);
                if (!p.nonlinear && p.lin.n == 1 && p.lin.c.num == 0 &&
                    p.lin.terms[0].coeff.num == p.lin.terms[0].coeff.den) {
                    ReadEntry *pe = reads_find_pair(reads, p.lin.terms[0].var);
                    lin_free(&p.lin);
                    if (pe) return sym_lin(lin_clone(pn[6] == '0' ? &pe->fa : &pe->fb));
                    return sym_nonlin();
                }
                lin_free(&p.lin);
                return sym_nonlin();
            }
        }
        /* vec_len(v) resolves to the tracked symbolic length of v (a concrete
         * linear form for locally-built vectors, a fresh symbolic variable for
         * Vec parameters, so requires like vec_len(v) >= 1 stay linear) */
        if (e->callee && e->callee->kind == NODE_IDENT && strcmp(e->callee->name, "vec_len") == 0 && e->args.len == 1 &&
            e->args.data[0]->kind == NODE_IDENT) {
            VLenEntry *ve = vlen_find(vlen, e->args.data[0]->name);
            if (ve) {
                if (!ve->unknown) return sym_lin(lin_clone(&ve->len));
                { char buf[300]; snprintf(buf, sizeof buf, "__len_%s", e->args.data[0]->name); return sym_lin(lin_var(buf)); }
            }
        }
        return sym_nonlin();
    case NODE_BINARY: {
        Sym l = se_from_ast(e->left, env, vlen, reads);
        Sym r = se_from_ast(e->right, env, vlen, reads);
        if (e->bin_op == OP_ADD) { if (l.nonlinear || r.nonlinear) return sym_nonlin(); return sym_lin(lin_add(&l.lin, &r.lin)); }
        if (e->bin_op == OP_SUB) { if (l.nonlinear || r.nonlinear) return sym_nonlin(); return sym_lin(lin_sub(&l.lin, &r.lin)); }
        if (e->bin_op == OP_MUL) {
            int lc = lin_is_const(&l.lin), rc = lin_is_const(&r.lin);
            if (!l.nonlinear && !r.nonlinear && lc && rc) return sym_lin(lin_const(rat_mul(l.lin.c, r.lin.c)));
            if (!l.nonlinear && !r.nonlinear && lc) return sym_lin(lin_scale(&r.lin, l.lin.c));
            if (!l.nonlinear && !r.nonlinear && rc) return sym_lin(lin_scale(&l.lin, r.lin.c));
            /* M8b: non-constant product of two linear forms — a fresh symbolic
             * var with the factors remembered (only where a ReadsList is
             * threaded; elsewhere honestly nonlinear). */
            if (!l.nonlinear && !r.nonlinear && !l.lin.overflow && !r.lin.overflow && reads) {
                char pv[64]; snprintf(pv, sizeof pv, "__p%d", g_prod_ctr++);
                reads_push_prod(reads, pv, l.lin, r.lin);   /* moves both lins */
                return sym_lin(lin_var(pv));
            }
            return sym_nonlin();
        }
        if (e->bin_op == OP_DIV || e->bin_op == OP_MOD) {
            if (!l.nonlinear && !r.nonlinear && lin_is_const(&l.lin) && lin_is_const(&r.lin) && r.lin.c.num != 0) {
                if (e->bin_op == OP_DIV) {
                    if (r.lin.c.den != 1 || r.lin.c.num == 0) return sym_nonlin();
                    int64_t a = l.lin.c.num, b = r.lin.c.num;
                    return sym_lin(lin_const(rat_int(a / b)));   /* C trunc semantics */
                } else {
                    if (r.lin.c.den != 1 || r.lin.c.num == 0) return sym_nonlin();
                    return sym_lin(lin_const(rat_int(l.lin.c.num % r.lin.c.num)));
                }
            }
            /* M9/M12: dividend linear, divisor linear (const or variable) → fresh */
            if (e->bin_op == OP_DIV && !l.nonlinear && !r.nonlinear &&
                !l.lin.overflow && !r.lin.overflow && reads) {
                /* const 0 divisor stays nonlinear (UB) */
                if (lin_is_const(&r.lin) && r.lin.c.den == 1 && r.lin.c.num == 0)
                    return sym_nonlin();
                char dv[64]; snprintf(dv, sizeof dv, "__d%d", g_prod_ctr++);
                reads_push_div(reads, dv, l.lin, r.lin);
                return sym_lin(lin_var(dv));
            }
            /* M9b/M12: n % d for linear d (const nonzero or variable) */
            if (e->bin_op == OP_MOD && !l.nonlinear && !r.nonlinear &&
                !l.lin.overflow && !r.lin.overflow && reads) {
                if (lin_is_const(&r.lin) && r.lin.c.den == 1 && r.lin.c.num == 0)
                    return sym_nonlin();
                char mv[64]; snprintf(mv, sizeof mv, "__m%d", g_prod_ctr++);
                reads_push_mod(reads, mv, l.lin, r.lin);
                return sym_lin(lin_var(mv));
            }
            return sym_nonlin();
        }
        /* M13/M20: bitwise / shifts — identity envelope, no full BV theory.
         * M20 adds idempotence, n|-1=-1, const-fold, 2^k-1 masks, and a
         * nonnegative variable and/or bound envelope. */
        if (e->bin_op == OP_BIT_OR || e->bin_op == OP_BIT_AND || e->bin_op == OP_BIT_XOR) {
            if (l.nonlinear || r.nonlinear) return sym_nonlin();
            /* both const: exact (two's complement matches C on i64) */
            if (lin_is_const(&l.lin) && lin_is_const(&r.lin) &&
                l.lin.c.den == 1 && r.lin.c.den == 1) {
                int64_t a = l.lin.c.num, b = r.lin.c.num, c = 0;
                if (e->bin_op == OP_BIT_AND) c = a & b;
                else if (e->bin_op == OP_BIT_OR) c = a | b;
                else c = a ^ b;
                lin_free(&l.lin); lin_free(&r.lin);
                return sym_lin(lin_const(rat_int(c)));
            }
            /* n | 0 = n; n & 0 = 0; n ^ 0 = n; n ^ n = 0; n & -1 = n
             * M20: n | n = n; n & n = n; n | -1 = -1 */
            if (e->bin_op == OP_BIT_OR && lin_is_const(&r.lin) && r.lin.c.num == 0 && r.lin.c.den == 1)
                { lin_free(&r.lin); return sym_lin(l.lin); }
            if (e->bin_op == OP_BIT_OR && lin_is_const(&l.lin) && l.lin.c.num == 0 && l.lin.c.den == 1)
                { lin_free(&l.lin); return sym_lin(r.lin); }
            if (e->bin_op == OP_BIT_OR && lin_is_const(&r.lin) && r.lin.c.num == -1 && r.lin.c.den == 1)
                { lin_free(&l.lin); lin_free(&r.lin); return sym_lin(lin_const(rat_int(-1))); }
            if (e->bin_op == OP_BIT_OR && lin_is_const(&l.lin) && l.lin.c.num == -1 && l.lin.c.den == 1)
                { lin_free(&l.lin); lin_free(&r.lin); return sym_lin(lin_const(rat_int(-1))); }
            if (e->bin_op == OP_BIT_OR && lin_eq(&l.lin, &r.lin))
                { lin_free(&r.lin); return sym_lin(l.lin); }
            if (e->bin_op == OP_BIT_AND && lin_is_const(&r.lin) && r.lin.c.num == 0 && r.lin.c.den == 1)
                { lin_free(&l.lin); lin_free(&r.lin); return sym_lin(lin_const(rat_int(0))); }
            if (e->bin_op == OP_BIT_AND && lin_is_const(&l.lin) && l.lin.c.num == 0 && l.lin.c.den == 1)
                { lin_free(&l.lin); lin_free(&r.lin); return sym_lin(lin_const(rat_int(0))); }
            if (e->bin_op == OP_BIT_AND && lin_is_const(&r.lin) && r.lin.c.num == -1 && r.lin.c.den == 1)
                { lin_free(&r.lin); return sym_lin(l.lin); }
            if (e->bin_op == OP_BIT_AND && lin_is_const(&l.lin) && l.lin.c.num == -1 && l.lin.c.den == 1)
                { lin_free(&l.lin); return sym_lin(r.lin); }
            if (e->bin_op == OP_BIT_AND && lin_eq(&l.lin, &r.lin))
                { lin_free(&r.lin); return sym_lin(l.lin); }
            if (e->bin_op == OP_BIT_XOR && lin_is_const(&r.lin) && r.lin.c.num == 0 && r.lin.c.den == 1)
                { lin_free(&r.lin); return sym_lin(l.lin); }
            if (e->bin_op == OP_BIT_XOR && lin_is_const(&l.lin) && l.lin.c.num == 0 && l.lin.c.den == 1)
                { lin_free(&l.lin); return sym_lin(r.lin); }
            if (e->bin_op == OP_BIT_XOR && lin_eq(&l.lin, &r.lin))
                { lin_free(&l.lin); lin_free(&r.lin); return sym_lin(lin_const(rat_int(0))); }
            /* M21: n ^ -1 = ~n = -n-1 on i64 two's complement (exact, even at
             * INT64_MIN: ~MIN = MAX). Linear after rewrite — no BV theory. */
            if (e->bin_op == OP_BIT_XOR && lin_is_const(&r.lin) && r.lin.c.den == 1 && r.lin.c.num == -1) {
                Lin t = lin_neg(&l.lin);
                lin_free(&l.lin); lin_free(&r.lin);
                if (t.overflow) return sym_nonlin();
                t.c = rat_sub(t.c, rat_int(1));
                if (t.c.overflow) { lin_free(&t); return sym_nonlin(); }
                return sym_lin(t);
            }
            if (e->bin_op == OP_BIT_XOR && lin_is_const(&l.lin) && l.lin.c.den == 1 && l.lin.c.num == -1) {
                Lin t = lin_neg(&r.lin);
                lin_free(&l.lin); lin_free(&r.lin);
                if (t.overflow) return sym_nonlin();
                t.c = rat_sub(t.c, rat_int(1));
                if (t.c.overflow) { lin_free(&t); return sym_nonlin(); }
                return sym_lin(t);
            }
            /* n & (2^k-1) → residue in {0 .. 2^k-1} (two's complement). */
            if (e->bin_op == OP_BIT_AND && reads) {
                int64_t mask = 0; int have = 0; Lin nlin;
                if (lin_is_const(&r.lin) && r.lin.c.den == 1 &&
                    r.lin.c.num > 0 && (((uint64_t)r.lin.c.num + 1) & (uint64_t)r.lin.c.num) == 0) {
                    mask = r.lin.c.num; nlin = l.lin; have = 1; lin_free(&r.lin);
                } else if (lin_is_const(&l.lin) && l.lin.c.den == 1 &&
                    l.lin.c.num > 0 && (((uint64_t)l.lin.c.num + 1) & (uint64_t)l.lin.c.num) == 0) {
                    mask = l.lin.c.num; nlin = r.lin; have = 1; lin_free(&l.lin);
                }
                if (have) {
                    char mv[64]; snprintf(mv, sizeof mv, "__b%d", g_prod_ctr++);
                    reads_push_bitmask(reads, mv, nlin, mask);
                    return sym_lin(lin_var(mv));
                }
            }
            /* M20: variable and/or — record and bound later if both >= 0 */
            if (reads && (e->bin_op == OP_BIT_AND || e->bin_op == OP_BIT_OR) &&
                !l.lin.overflow && !r.lin.overflow) {
                char mv[64]; snprintf(mv, sizeof mv, "__b%d", g_prod_ctr++);
                reads_push_bitop(reads, mv, l.lin, r.lin, e->bin_op == OP_BIT_OR);
                return sym_lin(lin_var(mv));
            }
            lin_free(&l.lin); lin_free(&r.lin);
            return sym_nonlin();
        }
        if (e->bin_op == OP_LSHIFT || e->bin_op == OP_RSHIFT || e->bin_op == OP_URSHIFT) {
            if (l.nonlinear || r.nonlinear) return sym_nonlin();
            /* n << k: exact scale by 2^k for k∈[0,62] (overflow treated as wrap-free ℤ).
             * n >> k (M22): аритметично отместване = floor(n/2^k). n ≥ 0 като
             * trunc div; n < 0 с floor-аксиоми; const n — точно (ashr).
             * n >>> k (M23): логическо отместване (zero-fill); const n — точно,
             * иначе запис с безусловни граници (виж inject_prod_axioms). */
            if (lin_is_const(&r.lin) && r.lin.c.den == 1 && r.lin.c.num >= 0 && r.lin.c.num <= 62) {
                int64_t k = r.lin.c.num;
                int64_t pow2 = 1LL << k;
                lin_free(&r.lin);
                if (e->bin_op == OP_LSHIFT) {
                    return sym_lin(lin_scale(&l.lin, rat_int(pow2)));
                }
                if (lin_is_const(&l.lin) && l.lin.c.den == 1) {
                    int64_t v = (e->bin_op == OP_URSHIFT)
                        ? baga_lshr_i64(l.lin.c.num, k)
                        : baga_ashr_i64(l.lin.c.num, k);
                    lin_free(&l.lin);
                    return sym_lin(lin_const(rat_int(v)));
                }
                if (k == 0) return sym_lin(l.lin);   /* n >> 0 = n — точна идентичност */
                if (reads) {
                    char dv[64]; snprintf(dv, sizeof dv, "__d%d", g_prod_ctr++);
                    if (e->bin_op == OP_URSHIFT)
                        reads_push_lshr(reads, dv, l.lin, lin_const(rat_int(k)));
                    else
                        reads_push_shr(reads, dv, l.lin, lin_const(rat_int(k)));
                    return sym_lin(lin_var(dv));
                }
                lin_free(&l.lin);
                return sym_nonlin();
            }
            lin_free(&l.lin); lin_free(&r.lin);
            return sym_nonlin();
        }
        return sym_nonlin();
    }
    default: return sym_nonlin();
    }
}

/* ============================ constraints + DNF ============================ */


void cl_init(ConsList *l) { l->c = NULL; l->n = 0; l->cap = 0; }
void cl_push(ConsList *l, Constraint c) {
    if (l->n == l->cap) { l->cap = l->cap ? l->cap * 2 : 8; l->c = realloc(l->c, (size_t)l->cap * sizeof(Constraint)); }
    l->c[l->n++] = c;
}
void cl_free(ConsList *l) { for (int i = 0; i < l->n; i++) lin_free(&l->c[i].lhs); free(l->c); l->c = NULL; l->n = l->cap = 0; }

void f_init(Formula *f) { f->br = NULL; f->n = 0; f->cap = 0; }
void f_push_branch(Formula *f, ConsList cl) {
    if (f->n == f->cap) { f->cap = f->cap ? f->cap * 2 : 4; f->br = realloc(f->br, (size_t)f->cap * sizeof(ConsList)); }
    f->br[f->n++] = cl;
}
void f_free(Formula *f) { for (int i = 0; i < f->n; i++) cl_free(&f->br[i]); free(f->br); f->br = NULL; f->n = f->cap = 0; }

Constraint mk_cons(Lin lhs, COp op, Rat rhs) { Constraint c; c.lhs = lhs; c.op = op; c.rhs = rhs; return c; }

/* comparison (lhs cmp rhs), optionally negated, -> constraint  expr {<,<=} 0.
 * Positive forms:  a<b -> a-b<0   a<=b -> a-b<=0   a>b -> b-a<0   a>=b -> b-a<=0.
 * Negation applies De Morgan: NOT(expr op 0) == (-expr) (flipped op) 0. */
static int atom_to_cons(Lin lhs, BinOp cmp, Lin rhs, int negated, Constraint *out) {
    Lin d;
    COp op;
    switch (cmp) {
    case OP_LT: d = lin_sub(&lhs, &rhs); op = C_LT; break;
    case OP_LE: d = lin_sub(&lhs, &rhs); op = C_LE; break;
    case OP_GT: d = lin_sub(&rhs, &lhs); op = C_LT; break;
    case OP_GE: d = lin_sub(&rhs, &lhs); op = C_LE; break;
    default: return 0;
    }
    if (d.overflow) { lin_free(&d); return 0; }
    if (negated) {
        Lin nd = lin_neg(&d);
        lin_free(&d);
        d = nd;
        op = (op == C_LT) ? C_LE : C_LT;
    }
    *out = mk_cons(d, op, rat_zero());
    return 1;
}

int is_cmp(BinOp op) { return op == OP_LT || op == OP_LE || op == OP_GT || op == OP_GE; }

/* Build the DNF of a boolean AST (optionally negated). Returns 0 if the AST is
 * outside the convertible fragment (caller treats as UNKNOWN). */
int bool_to_dnf(Node *e, SEnv *env, VLenMap *vlen, ReadsList *reads, int negated, Formula *out);

static int cmp_to_formula(Node *e, SEnv *env, VLenMap *vlen, ReadsList *reads, int negated, Formula *out) {
    /* an element-wise comparison (v[*] >= c) is handled as an ElemAxiom, not a
     * scalar path constraint — treat it as vacuously TRUE here */
    if (e->left->kind == NODE_ELEM_REF || e->right->kind == NODE_ELEM_REF) {
        f_init(out); ConsList cl; cl_init(&cl); f_push_branch(out, cl); return 1;
    }
    /* se_from_ast with reads so products/div in conditions become symbolic (M13). */
    Sym l = se_from_ast(e->left, env, vlen, reads);
    Sym r = se_from_ast(e->right, env, vlen, reads);
    if (l.nonlinear || r.nonlinear) { if (!l.nonlinear) lin_free(&l.lin); if (!r.nonlinear) lin_free(&r.lin); return 0; }
    Constraint c;
    if (!atom_to_cons(l.lin, e->bin_op, r.lin, negated, &c)) { lin_free(&l.lin); lin_free(&r.lin); return 0; }
    lin_free(&l.lin); lin_free(&r.lin);
    ConsList cl; cl_init(&cl); cl_push(&cl, c);
    f_init(out); f_push_branch(out, cl);
    return 1;
}

/* Verifier-only relational predicate sorted(v): not a scalar constraint.
 * Positive occurrence → vacuous TRUE; negated → not convertible (UNKNOWN). */
int is_sorted_call(Node *e) {
    return e && e->kind == NODE_CALL && e->callee && e->callee->kind == NODE_IDENT &&
           strcmp(e->callee->name, "sorted") == 0;
}

/* W2.1/W2.7: verifier-only ghost предикати за ДОГОВОР за свят в requires:
 *   world_frozen(p)  — заемане за четене (W2.1);
 *   world_mutable(p) — изключителен запис по време на повикването (W2.7);
 *   world_owned(p)   — прехвърляне на собствеността (W2.7).
 * Състоянието на свят не е аритметична величина (HK_WORLD е ghost, като
 * sorted(v)): положителна употреба → vacuous TRUE за скаларния фрагмент
 * (ghost-регистрацията е отделна — extract_world_ghost); отрицание → извън
 * фрагмента, т.е. честен UNKNOWN. */
int is_world_ghost_call(Node *e) {
    return world_contract_of(e) != 0;
}

/* 0 = не е договор; 1 = world_frozen; 2 = world_mutable; 3 = world_owned */
int world_contract_of(Node *e) {
    if (!e || e->kind != NODE_CALL || !e->callee || e->callee->kind != NODE_IDENT ||
        e->args.len != 1 || e->args.data[0]->kind != NODE_IDENT) return 0;
    const char *nm = e->callee->name;
    if (strcmp(nm, "world_frozen") == 0) return 1;
    if (strcmp(nm, "world_mutable") == 0) return 2;
    if (strcmp(nm, "world_owned") == 0) return 3;
    return 0;
}

int bool_to_dnf(Node *e, SEnv *env, VLenMap *vlen, ReadsList *reads, int negated, Formula *out) {
    if (!e) return 0;
    if (e->kind == NODE_BOOL_LIT) {
        int v = e->bool_val;
        if (negated) v = !v;
        f_init(out);
        if (v) { ConsList cl; cl_init(&cl); f_push_branch(out, cl); }   /* TRUE: one empty branch */
        /* FALSE: zero branches */
        return 1;
    }
    /* sorted(v) is stored as an ElemAxiom (is_sorted), not a path constraint */
    if (is_sorted_call(e)) {
        if (negated) return 0;   /* ¬sorted is outside the scalar fragment */
        f_init(out); ConsList cl; cl_init(&cl); f_push_branch(out, cl); return 1;
    }
    /* W2.1: world_frozen(w) е ghost (HK_WORLD), не скаларно ограничение */
    if (is_world_ghost_call(e)) {
        if (negated) return 0;   /* ¬world_frozen is outside the fragment */
        f_init(out); ConsList cl; cl_init(&cl); f_push_branch(out, cl); return 1;
    }
    if (e->kind == NODE_UNARY && e->un_op == UOP_NOT)
        return bool_to_dnf(e->operand, env, vlen, reads, !negated, out);
    if (e->kind == NODE_BINARY && is_cmp(e->bin_op))
        return cmp_to_formula(e, env, vlen, reads, negated, out);
    if (e->kind == NODE_BINARY && e->bin_op == OP_EQ) {
        if (!negated) {
            /* a==b -> (a<=b)&&(a>=b) */
            Node l = *e, r = *e;
            l.bin_op = OP_LE; r.bin_op = OP_GE;
            Formula a, b;
            if (!bool_to_dnf(&l, env, vlen, reads, 0, &a)) return 0;
            if (!bool_to_dnf(&r, env, vlen, reads, 0, &b)) { f_free(&a); return 0; }
            /* cartesian product */
            f_init(out);
            for (int i = 0; i < a.n; i++) for (int j = 0; j < b.n; j++) {
                ConsList cl; cl_init(&cl);
                for (int x = 0; x < a.br[i].n; x++) { Constraint c = a.br[i].c[x]; cl_push(&cl, mk_cons(lin_clone(&c.lhs), c.op, c.rhs)); }
                for (int x = 0; x < b.br[j].n; x++) { Constraint c = b.br[j].c[x]; cl_push(&cl, mk_cons(lin_clone(&c.lhs), c.op, c.rhs)); }
                f_push_branch(out, cl);
            }
            f_free(&a); f_free(&b);
            return 1;
        }
        /* ¬(a==b) -> (a<b)∨(a>b)  (needed so ensures output==n can be proven) */
        Node lt = *e, gt = *e;
        lt.bin_op = OP_LT; gt.bin_op = OP_GT;
        Formula L, R;
        if (!bool_to_dnf(&lt, env, vlen, reads, 0, &L)) return 0;
        if (!bool_to_dnf(&gt, env, vlen, reads, 0, &R)) { f_free(&L); return 0; }
        f_init(out);
        for (int i = 0; i < L.n; i++) f_push_branch(out, L.br[i]);
        for (int i = 0; i < R.n; i++) f_push_branch(out, R.br[i]);
        free(L.br); free(R.br);
        return 1;
    }
    if (e->kind == NODE_BINARY && (e->bin_op == OP_AND || e->bin_op == OP_OR)) {
        Formula L, R;
        if (!bool_to_dnf(e->left, env, vlen, reads, negated, &L)) return 0;
        if (!bool_to_dnf(e->right, env, vlen, reads, negated, &R)) { f_free(&L); return 0; }
        int disj = (e->bin_op == OP_OR) ^ negated;   /* OR, or AND-under-negation => union */
        f_init(out);
        if (disj) {
            for (int i = 0; i < L.n; i++) f_push_branch(out, L.br[i]);
            for (int i = 0; i < R.n; i++) f_push_branch(out, R.br[i]);
            free(L.br); free(R.br);   /* branches moved */
        } else {
            for (int i = 0; i < L.n; i++) for (int j = 0; j < R.n; j++) {
                ConsList cl; cl_init(&cl);
                for (int x = 0; x < L.br[i].n; x++) { Constraint c = L.br[i].c[x]; cl_push(&cl, mk_cons(lin_clone(&c.lhs), c.op, c.rhs)); }
                for (int x = 0; x < R.br[j].n; x++) { Constraint c = R.br[j].c[x]; cl_push(&cl, mk_cons(lin_clone(&c.lhs), c.op, c.rhs)); }
                f_push_branch(out, cl);
            }
            f_free(&L); f_free(&R);
        }
        return 1;
    }
    return 0;   /* not convertible -> UNKNOWN */
}

/* ============================ Fourier–Motzkin ============================ */

/* Is a constant constraint contradictory?  0<c needs c>0; 0<=c needs c>=0. */
static int cons_contradictory(Constraint *c) {
    if (c->lhs.n != 0) return 0;
    int s = rat_cmp(c->lhs.c, c->rhs);   /* sign of (0 - rhs)? lhs is const c0: constraint c0 {op} rhs */
    /* value of lhs is c->lhs.c; constraint holds iff c0 op rhs */
    if (c->op == C_LT) return !(s < 0);
    return !(s <= 0);
}


/* Classify constraint wrt variable v. Returns 0 if v absent (neutral). */
static int split_on(Constraint *c, const char *v, Bound *out) {
    Rat a = lin_coeff(&c->lhs, v);
    if (a.num == 0) return 0;
    /* build rest = lhs without v */
    Lin rest; lin_init(&rest); rest.c = c->lhs.c; rest.overflow = c->lhs.overflow;
    for (int i = 0; i < c->lhs.n; i++)
        if (strcmp(c->lhs.terms[i].var, v) != 0)
            lin_push_raw(&rest, c->lhs.terms[i].var, c->lhs.terms[i].coeff);
    /* a*v + rest op rhs  ->  v op' (rhs-rest)/a */
    Lin neg_rest = lin_neg(&rest);
    Lin numer = lin_add(&neg_rest, &(Lin){ .c = c->rhs });   /* rhs - rest */
    lin_free(&neg_rest);
    Rat inv = rat_mk(a.den, a.num);   /* 1/a */
    Lin bound = lin_scale(&numer, inv);
    lin_free(&rest); lin_free(&numer);
    out->rest = bound;
    out->bound = rat_zero();
    /* a*v + rest {op} rhs  ->  v {op'} bound, where bound=(rhs-rest)/a.
     * a>0: v <= bound (upper) or v < bound (upper).
     * a<0: dividing by negative reverses direction: v >= bound (lower) or
     *      v > bound (lower). Strictness is preserved either way (x<c == -x>-c),
     *      so strict == (op==LT) regardless of the sign of a. */
    out->is_lower = rat_is_neg(a);
    out->strict = (c->op == C_LT);
    return 1;
}

/* Combine lower (lo->strict ? L<v : L<=v) and upper (up->strict ? v<U : v<=U)
 * into a constraint on L-U. The result is strict iff EITHER bound is strict:
 *   L<v<=U  => L<U ;  L<=v<U => L<U ;  L<v<U => L<U ;  L<=v<=U => L<=U.
 * (L<v with v=U is impossible, so a strict lower bound also forces L<U.) */
static Constraint combine(Bound *lo, Bound *up) {
    Lin d = lin_sub(&lo->rest, &up->rest);
    COp op = (lo->strict || up->strict) ? C_LT : C_LE;
    return mk_cons(d, op, rat_zero());
}

int fm_sat(ConsList *sys) {
    /* Overflow bail-out: exact rational arithmetic is only exact while it
     * fits int64. An overflowed constraint cannot be trusted — answer SAT
     * ("cannot decide"), which is the conservative direction for every
     * verdict: fewer proofs, never a false proof or a false refutation. */
    for (int i = 0; i < sys->n; i++) {
        Constraint *c = &sys->c[i];
        if (c->lhs.overflow || c->lhs.c.overflow || c->rhs.overflow) { cl_free(sys); return 1; }
        for (int j = 0; j < c->lhs.n; j++)
            if (c->lhs.terms[j].coeff.overflow) { cl_free(sys); return 1; }
    }
    /* M7: integer tightening. Over ℤ-valued variables with integer
     * coefficients,  lhs < rhs  ⟺  lhs <= rhs - 1  exactly. Tightening every
     * integer strict inequality at entry makes the rational FM exact for the
     * classic gap (n > 0 ⇒ n >= 1): the tightened system T satisfies
     * T_ℤ = S_ℤ and T_ℚ ⊆ S_ℚ, so UNSAT_ℚ(T) still implies UNSAT_ℤ(S)
     * (sound — more things proven, never a false proof), and a ℤ-satisfiable
     * S keeps T ℚ-satisfiable (no genuine counterexample is ever hidden).
     * Constraints with rational coefficients are left untouched (honest). */
    for (int i = 0; i < sys->n; i++) {
        Constraint *c = &sys->c[i];
        if (c->op != C_LT || c->lhs.overflow || c->lhs.c.den != 1 || c->rhs.den != 1) continue;
        int all_int = 1;
        for (int j = 0; j < c->lhs.n && all_int; j++)
            if (c->lhs.terms[j].coeff.den != 1) all_int = 0;
        if (!all_int) continue;
        c->op = C_LE;
        c->rhs = rat_sub(c->rhs, rat_int(1));
    }
    /* gather variable names */
    char **vars = NULL; int nv = 0, vcap = 0;
    for (int i = 0; i < sys->n; i++) for (int j = 0; j < sys->c[i].lhs.n; j++) {
        const char *vn = sys->c[i].lhs.terms[j].var;
        int found = 0;
        for (int k = 0; k < nv; k++) if (strcmp(vars[k], vn) == 0) { found = 1; break; }
        if (!found) { if (nv == vcap) { vcap = vcap ? vcap * 2 : 8; vars = realloc(vars, (size_t)vcap * sizeof(char *)); } vars[nv++] = strdup(vn); }
    }
    int result = 1;   /* SAT until contradiction */
    /* a constant contradictory constraint (e.g. 0 < 0) makes it UNSAT outright */
    for (int i = 0; i < sys->n; i++) if (cons_contradictory(&sys->c[i])) { result = 0; break; }
    for (int vi = 0; vi < nv && result; vi++) {
        const char *v = vars[vi];
        Bound *low = NULL; int nl = 0, lcap = 0;
        Bound *up = NULL; int nu = 0, ucap = 0;
        ConsList neutral; cl_init(&neutral);
        for (int i = 0; i < sys->n; i++) {
            Bound b;
            if (!split_on(&sys->c[i], v, &b)) {
                cl_push(&neutral, mk_cons(lin_clone(&sys->c[i].lhs), sys->c[i].op, sys->c[i].rhs));
            } else if (b.is_lower) {
                if (nl == lcap) { lcap = lcap ? lcap * 2 : 8; low = realloc(low, (size_t)lcap * sizeof(Bound)); }
                low[nl++] = b;
            } else {
                if (nu == ucap) { ucap = ucap ? ucap * 2 : 8; up = realloc(up, (size_t)ucap * sizeof(Bound)); }
                up[nu++] = b;
            }
        }
        ConsList next; cl_init(&next);
        for (int i = 0; i < neutral.n; i++) cl_push(&next, mk_cons(lin_clone(&neutral.c[i].lhs), neutral.c[i].op, neutral.c[i].rhs));
        cl_free(&neutral);
        for (int a = 0; a < nl && result; a++) for (int b = 0; b < nu && result; b++) {
            Constraint nc = combine(&low[a], &up[b]);
            if (cons_contradictory(&nc)) { lin_free(&nc.lhs); result = 0; }
            else cl_push(&next, nc);
        }
        for (int i = 0; i < nl; i++) lin_free(&low[i].rest);
        for (int i = 0; i < nu; i++) lin_free(&up[i].rest);
        free(low); free(up);
        cl_free(sys);
        *sys = next;
    }
    if (result) for (int i = 0; i < sys->n; i++) if (cons_contradictory(&sys->c[i])) { result = 0; break; }
    for (int i = 0; i < nv; i++) free(vars[i]);
    free(vars);
    cl_free(sys);
    return result;
}

