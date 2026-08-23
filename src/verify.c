/* verify.c — static spec verification (depth pillar I, milestone M0).
 *
 * Proves/refutes `requires`/`ensures` contracts for pure, non-recursive,
 * loop-free, LINEAR i64 functions. Sound by construction: the only path to
 * PROVEN is "the negated obligation is unsatisfiable even over the rationals",
 * which implies unsatisfiable over the integers. Anything we cannot decide is
 * reported UNKNOWN, never PROVEN. A REFUTED result always carries a concrete
 * integral counterexample re-checked by direct interpretation.
 *
 * Pipeline: symexec the body (state = env of symbolic linear forms + a path
 * condition) -> at each return, build the obligation  requires ∧ path ⇒
 * ensures[output := return]  -> negate -> linear constraints -> Fourier–Motzkin.
 *
 * Zero dependencies; exact rational arithmetic with overflow bail-out.
 */

#include "baga.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* ============================ rationals ============================ */

typedef struct { int64_t num, den; int overflow; } Rat;

static int64_t v_gcd(int64_t a, int64_t b) {
    /* INT64_MIN-safe: work in __int128 so |INT64_MIN| is representable */
    __int128 x = a < 0 ? -(__int128)a : a;
    __int128 y = b < 0 ? -(__int128)b : b;
    while (y) { __int128 t = x % y; x = y; y = t; }
    return x ? (int64_t)x : 1;
}

static Rat rat_bad(void) { Rat r; r.num = 0; r.den = 1; r.overflow = 1; return r; }

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
static Rat rat_int(int64_t n) { Rat r; r.num = n; r.den = 1; r.overflow = 0; return r; }
static Rat rat_zero(void) { return rat_int(0); }

static Rat rat_neg(Rat a) { if (a.overflow || a.num == INT64_MIN) return rat_bad(); Rat r = a; r.num = -r.num; return r; }

static Rat rat_add(Rat a, Rat b) {
    if (a.overflow || b.overflow || a.den == 0 || b.den == 0) return rat_bad();
    int64_t g = v_gcd(a.den, b.den);
    int64_t l1 = a.den / g, l2 = b.den / g;
    __int128 n = (__int128)a.num * l2 + (__int128)b.num * l1;
    __int128 d = (__int128)a.den * l2;
    if (n > INT64_MAX || n < INT64_MIN || d > INT64_MAX) return rat_bad();
    return rat_mk((int64_t)n, (int64_t)d);
}
static Rat rat_sub(Rat a, Rat b) { return rat_add(a, rat_neg(b)); }
static Rat rat_mul(Rat a, Rat b) {
    if (a.overflow || b.overflow || a.den == 0 || b.den == 0) return rat_bad();
    __int128 n = (__int128)a.num * b.num;
    __int128 d = (__int128)a.den * b.den;
    if (n > INT64_MAX || n < INT64_MIN || d > INT64_MAX) return rat_bad();
    return rat_mk((int64_t)n, (int64_t)d);
}
/* -1 if a<b, 0 if equal, 1 if a>b */
static int rat_cmp(Rat a, Rat b) {
    if (a.overflow || b.overflow || a.den == 0 || b.den == 0) return 0;
    __int128 l = (__int128)a.num * b.den;
    __int128 r = (__int128)b.num * a.den;
    if (l < r) return -1;
    if (l > r) return 1;
    return 0;
}
static int rat_is_neg(Rat a) { return a.num < 0; }

/* ============================ linear forms ============================ */

typedef struct { char *var; Rat coeff; } Term;
typedef struct { Rat c; Term *terms; int n, cap; int overflow; } Lin;

static void lin_init(Lin *l) { l->c = rat_zero(); l->terms = NULL; l->n = 0; l->cap = 0; l->overflow = 0; }
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
static Lin lin_const(Rat c) { Lin l; lin_init(&l); l.c = c; if (c.overflow) l.overflow = 1; return l; }
static Lin lin_var(const char *v) { Lin l; lin_init(&l); lin_push_raw(&l, v, rat_int(1)); return l; }
static void lin_free(Lin *l) { for (int i = 0; i < l->n; i++) free(l->terms[i].var); free(l->terms); l->terms = NULL; l->n = l->cap = 0; }

static Lin lin_add(Lin *a, Lin *b) {
    Lin r; lin_init(&r);
    if (a->overflow || b->overflow || a->c.overflow || b->c.overflow) { r.overflow = 1; return r; }
    r.c = rat_add(a->c, b->c);
    if (r.c.overflow) { r.overflow = 1; return r; }
    for (int i = 0; i < a->n; i++) lin_push_raw(&r, a->terms[i].var, a->terms[i].coeff);
    for (int i = 0; i < b->n; i++) lin_push_raw(&r, b->terms[i].var, b->terms[i].coeff);
    return r;
}
static Lin lin_sub(Lin *a, Lin *b) {
    Lin r; lin_init(&r);
    if (a->overflow || b->overflow || a->c.overflow || b->c.overflow) { r.overflow = 1; return r; }
    r.c = rat_sub(a->c, b->c);
    if (r.c.overflow) { r.overflow = 1; return r; }
    for (int i = 0; i < a->n; i++) lin_push_raw(&r, a->terms[i].var, a->terms[i].coeff);
    for (int i = 0; i < b->n; i++) lin_push_raw(&r, b->terms[i].var, rat_neg(b->terms[i].coeff));
    return r;
}
static Lin lin_scale(Lin *a, Rat k) {
    Lin r; lin_init(&r);
    if (a->overflow || k.overflow || a->c.overflow) { r.overflow = 1; return r; }
    r.c = rat_mul(a->c, k);
    if (r.c.overflow) { r.overflow = 1; return r; }
    for (int i = 0; i < a->n; i++) lin_push_raw(&r, a->terms[i].var, rat_mul(a->terms[i].coeff, k));
    return r;
}
static Lin lin_neg(Lin *a) { return lin_scale(a, rat_int(-1)); }
static int lin_is_const(Lin *a) { return a->n == 0; }
static Rat lin_coeff(Lin *l, const char *var) {
    for (int i = 0; i < l->n; i++) if (strcmp(l->terms[i].var, var) == 0) return l->terms[i].coeff;
    return rat_zero();
}
static Lin lin_clone(Lin *a) {
    Lin r; lin_init(&r); r.c = a->c; r.overflow = a->overflow;
    for (int i = 0; i < a->n; i++) lin_push_raw(&r, a->terms[i].var, a->terms[i].coeff);
    return r;
}
/* A handle ghost is a unit-coefficient single-term form with zero constant
 * (e.g. `__h0`, `__ch1`). Anything else is not a tracked handle. */
static const char *lin_handle_var(Lin *l) {
    if (!l || l->n != 1 || l->c.num != 0) return NULL;
    if (l->terms[0].coeff.num != l->terms[0].coeff.den) return NULL;
    return l->terms[0].var;
}

/* ============================ symbolic env ============================ */

typedef struct { char *name; Lin lin; int nonlinear; } Binding;
typedef struct { Binding *b; int n, cap; } SEnv;

static void env_init(SEnv *e) { e->b = NULL; e->n = 0; e->cap = 0; }
static void env_free(SEnv *e) { for (int i = 0; i < e->n; i++) { free(e->b[i].name); lin_free(&e->b[i].lin); } free(e->b); e->b = NULL; e->n = e->cap = 0; }
static Binding *env_find(SEnv *e, const char *name) {
    for (int i = 0; i < e->n; i++) if (strcmp(e->b[i].name, name) == 0) return &e->b[i];
    return NULL;
}
static void env_bind(SEnv *e, const char *name, Lin lin, int nonlinear) {
    if (nonlinear || lin.overflow) { lin_free(&lin); lin_init(&lin); nonlinear = 1; }
    Binding *b = env_find(e, name);
    if (b) { lin_free(&b->lin); b->lin = lin; b->nonlinear = nonlinear; return; }
    if (e->n == e->cap) { e->cap = e->cap ? e->cap * 2 : 8; e->b = realloc(e->b, (size_t)e->cap * sizeof(Binding)); }
    e->b[e->n].name = strdup(name); e->b[e->n].lin = lin; e->b[e->n].nonlinear = nonlinear; e->n++;
}
static void env_clone_into(SEnv *dst, SEnv *src) {
    env_init(dst);
    for (int i = 0; i < src->n; i++) env_bind(dst, src->b[i].name, lin_clone(&src->b[i].lin), src->b[i].nonlinear);
}

/* SymExpr: a linear form, or nonlinear=true when it cannot be represented. */
typedef struct { Lin lin; int nonlinear; } Sym;

static Sym sym_nonlin(void) { Sym s; lin_init(&s.lin); s.nonlinear = 1; return s; }
static Sym sym_lin(Lin l) { Sym s; s.lin = l; s.nonlinear = l.overflow; return s; }

/* Symbolic length of a Vec variable (M2 bounds tracking). */
typedef struct { char *name; Lin len; int unknown; } VLenEntry;
typedef struct { VLenEntry *e; int n, cap; } VLenMap;

static void vlen_init(VLenMap *m) { m->e = NULL; m->n = 0; m->cap = 0; }
static void vlen_free(VLenMap *m) { for (int i = 0; i < m->n; i++) { free(m->e[i].name); lin_free(&m->e[i].len); } free(m->e); m->e = NULL; m->n = m->cap = 0; }
static VLenEntry *vlen_find(VLenMap *m, const char *name) {
    for (int i = 0; i < m->n; i++) if (strcmp(m->e[i].name, name) == 0) return &m->e[i];
    return NULL;
}
static void vlen_set(VLenMap *m, const char *name, Lin len, int unknown) {
    if (unknown) { lin_free(&len); lin_init(&len); }
    VLenEntry *e = vlen_find(m, name);
    if (e) { lin_free(&e->len); e->len = len; e->unknown = unknown; return; }
    if (m->n == m->cap) { m->cap = m->cap ? m->cap * 2 : 4; m->e = realloc(m->e, (size_t)m->cap * sizeof(VLenEntry)); }
    m->e[m->n].name = strdup(name); m->e[m->n].len = len; m->e[m->n].unknown = unknown; m->n++;
}
static void vlen_clone_into(VLenMap *dst, VLenMap *src) {
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
typedef struct {
    Node *call; char *var; Lin fa, fb;
    int is_prod; int is_div; int is_mod; int is_bitand1; int is_pair;
    int is_bitand; int is_bitor; int mon_deg; Lin mon_base;
    int is_shr; int is_lshr;
} ReadEntry;
typedef struct { ReadEntry *r; int n, cap; } ReadsList;
typedef struct {
    char *var; Lin fa, fb; int is_div; int is_mod; int is_bitand1;
    int is_bitand; int is_bitor; int is_shr; int is_lshr;
} ProdEntry;
typedef struct { ProdEntry *p; int n, cap; } ProdList;
static const char *reads_find(ReadsList *l, Node *call);
static void reads_push_prod(ReadsList *l, const char *var, Lin fa, Lin fb);
static void reads_push_div(ReadsList *l, const char *var, Lin fa, Lin fb);
static void reads_push_mod(ReadsList *l, const char *var, Lin fa, Lin fb);
static void reads_push_shr(ReadsList *l, const char *var, Lin fa, Lin fb);
static void reads_push_lshr(ReadsList *l, const char *var, Lin fa, Lin fb);
static void reads_push_bitmask(ReadsList *l, const char *var, Lin fa, int64_t mask);
static void reads_push_bitop(ReadsList *l, const char *var, Lin fa, Lin fb, int is_or);
static void reads_push_pair(ReadsList *l, const char *var, Lin fa, Lin fb);   /* M17 */
static ReadEntry *reads_find_pair(ReadsList *l, const char *var);             /* M17 */
static void prods_clone_into(ProdList *dst, ReadsList *src);
static int lin_eq(Lin *a, Lin *b);   /* defined later; used by M13 XOR identity */
static int g_prod_ctr = 0;

static Sym se_from_ast(Node *e, SEnv *env, VLenMap *vlen, ReadsList *reads) {
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

typedef enum { C_LT, C_LE } COp;
typedef struct { Lin lhs; COp op; Rat rhs; } Constraint;

typedef struct { Constraint *c; int n, cap; } ConsList;
static void cl_init(ConsList *l) { l->c = NULL; l->n = 0; l->cap = 0; }
static void cl_push(ConsList *l, Constraint c) {
    if (l->n == l->cap) { l->cap = l->cap ? l->cap * 2 : 8; l->c = realloc(l->c, (size_t)l->cap * sizeof(Constraint)); }
    l->c[l->n++] = c;
}
static void cl_free(ConsList *l) { for (int i = 0; i < l->n; i++) lin_free(&l->c[i].lhs); free(l->c); l->c = NULL; l->n = l->cap = 0; }

typedef struct { ConsList *br; int n, cap; } Formula;   /* DNF: OR of branches */
static void f_init(Formula *f) { f->br = NULL; f->n = 0; f->cap = 0; }
static void f_push_branch(Formula *f, ConsList cl) {
    if (f->n == f->cap) { f->cap = f->cap ? f->cap * 2 : 4; f->br = realloc(f->br, (size_t)f->cap * sizeof(ConsList)); }
    f->br[f->n++] = cl;
}
static void f_free(Formula *f) { for (int i = 0; i < f->n; i++) cl_free(&f->br[i]); free(f->br); f->br = NULL; f->n = f->cap = 0; }

static Constraint mk_cons(Lin lhs, COp op, Rat rhs) { Constraint c; c.lhs = lhs; c.op = op; c.rhs = rhs; return c; }

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

static int is_cmp(BinOp op) { return op == OP_LT || op == OP_LE || op == OP_GT || op == OP_GE; }

/* Build the DNF of a boolean AST (optionally negated). Returns 0 if the AST is
 * outside the convertible fragment (caller treats as UNKNOWN). */
static int bool_to_dnf(Node *e, SEnv *env, VLenMap *vlen, ReadsList *reads, int negated, Formula *out);

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
static int is_sorted_call(Node *e) {
    return e && e->kind == NODE_CALL && e->callee && e->callee->kind == NODE_IDENT &&
           strcmp(e->callee->name, "sorted") == 0;
}

static int bool_to_dnf(Node *e, SEnv *env, VLenMap *vlen, ReadsList *reads, int negated, Formula *out) {
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

typedef struct { Lin rest; Rat bound; int strict; int is_lower; } Bound;

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

static int fm_sat(ConsList *sys) {
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

/* ============================ counterexample search ============================ */

typedef struct { char *name; int64_t val; } IBind;
typedef struct { IBind *b; int n; } IEnv;

static int64_t ienv_get(IEnv *e, const char *name, int *ok) {
    for (int i = 0; i < e->n; i++) if (strcmp(e->b[i].name, name) == 0) { *ok = 1; return e->b[i].val; }
    *ok = 0; return 0;
}

static int eval_bool(Node *e, IEnv *env);

static int64_t eval_i64(Node *e, IEnv *env, int *ok) {
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
        default: *ok = 0; return 0;
        }
    }
    default: *ok = 0; return 0;
    }
}

static int eval_bool(Node *e, IEnv *env) {
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
static void collect_vars_cl(ConsList *cl, char ***vars, int *nv, int *cap) {
    for (int i = 0; i < cl->n; i++) for (int j = 0; j < cl->c[i].lhs.n; j++) {
        const char *vn = cl->c[i].lhs.terms[j].var;
        int found = 0;
        for (int k = 0; k < *nv; k++) if (strcmp((*vars)[k], vn) == 0) { found = 1; break; }
        if (!found) { if (*nv == *cap) { *cap = *cap ? *cap * 2 : 8; *vars = realloc(*vars, (size_t)*cap * sizeof(char *)); } (*vars)[(*nv)++] = strdup(vn); }
    }
}

/* Concrete value of a recorded product/div/mod/bit entry (M8–M20). */
static int prod_concrete(ProdEntry *p, int64_t a, int64_t b, int64_t *out) {
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
static void push_pins_and_derived(ConsList *sys, char **vars, IBind *assign, int nv, ProdList *prods) {
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
static int find_counterexample(ConsList *ante, Formula *nef, Formula *ef, Node *ensures_ast,
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
typedef struct { ConsList path; ConsList bound; ConsList read_cons; Sym ret; VLenMap vlen; ProdList prods; int bad; int kind; char *label; int akind; Lin aux1, aux2; } Obligation;
typedef struct { Obligation *o; int n, cap; } Obligations;
static void prods_free(ProdList *p);   /* defined below */
static void obl_push(Obligations *ob, ConsList path, ConsList bound, ConsList read_cons, Sym ret, VLenMap *vlen, ReadsList *reads, int bad, int kind, const char *label) {
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
static void obl_free(Obligation *o) {
    cl_free(&o->path); cl_free(&o->bound); cl_free(&o->read_cons);
    lin_free(&o->ret.lin); vlen_free(&o->vlen); prods_free(&o->prods);
    lin_free(&o->aux1); lin_free(&o->aux2);
    free(o->label);
}

/* Witness search for a bounds obligation: an assignment satisfying the
 * antecedent (obl->path = requires ∧ path ∧ ¬bound) where the access index
 * (obl->ret) really is out of range. Re-checked by direct evaluation, so a
 * reported counterexample is genuine. */
static int find_bound_witness(Obligation *obl, IBind **witness, int *wn) {
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

/* ============================ symbolic execution ============================ */

/* M3 element invariant: forall i in [0, len(vec)): vec[i] <cmp> rhs.
 * cmp is one of EC_LT/EC_LE/EC_GT/EC_GE; rhs is a linear form over scalars. */
typedef enum { EC_LT, EC_LE, EC_GT, EC_GE } ElemCmp;
/* is_sorted: a relational axiom "sorted(vec)" (forall i: vec[i] <= vec[i+1]);
 * cmp/rhs are unused in that case. */
typedef struct { char *vec; ElemCmp cmp; Lin rhs; int is_sorted; } ElemAxiom;
typedef struct { ElemAxiom *a; int n, cap; } AxiomList;

static void ax_init(AxiomList *l) { l->a = NULL; l->n = 0; l->cap = 0; }
static void ax_free(AxiomList *l) { for (int i = 0; i < l->n; i++) { free(l->a[i].vec); lin_free(&l->a[i].rhs); } free(l->a); l->a = NULL; l->n = l->cap = 0; }
static void ax_push(AxiomList *l, const char *vec, ElemCmp cmp, Lin rhs, int is_sorted) {
    if (l->n == l->cap) { l->cap = l->cap ? l->cap * 2 : 4; l->a = realloc(l->a, (size_t)l->cap * sizeof(ElemAxiom)); }
    l->a[l->n].vec = strdup(vec); l->a[l->n].cmp = cmp; l->a[l->n].rhs = rhs; l->a[l->n].is_sorted = is_sorted; l->n++;
}
static void ax_clone_into(AxiomList *dst, AxiomList *src) {
    ax_init(dst);
    for (int i = 0; i < src->n; i++) ax_push(dst, src->a[i].vec, src->a[i].cmp, lin_clone(&src->a[i].rhs), src->a[i].is_sorted);
}

/* Copy the axioms of vector `from` (in `src`) into `dst`, re-anchored on `to`. */
static void ax_copy_renamed(AxiomList *dst, AxiomList *src, const char *from, const char *to) {
    for (int i = 0; i < src->n; i++)
        if (strcmp(src->a[i].vec, from) == 0)
            ax_push(dst, to, src->a[i].cmp, lin_clone(&src->a[i].rhs), src->a[i].is_sorted);
}

/* vec_concat result: keep only the axioms BOTH operands share (same cmp+rhs),
 * re-anchored on `to` — those hold for every element of v ++ w. */
static int lin_equal(Lin *a, Lin *b);
static void ax_intersect_two(AxiomList *dst, AxiomList *src, const char *v, const char *w, const char *to) {
    for (int i = 0; i < src->n; i++) {
        if (strcmp(src->a[i].vec, v) != 0) continue;
        /* sorted(v) ∧ sorted(w) does not imply sorted(v ++ w) — never transfer */
        if (src->a[i].is_sorted) continue;
        for (int j = 0; j < src->n; j++)
            if (strcmp(src->a[j].vec, w) == 0 && !src->a[j].is_sorted &&
                src->a[j].cmp == src->a[i].cmp &&
                lin_equal(&src->a[i].rhs, &src->a[j].rhs)) {
                ax_push(dst, to, src->a[i].cmp, lin_clone(&src->a[i].rhs), 0);
                break;
            }
    }
}

/* Structural equality of two linear forms (same constant + same terms). */
static int lin_equal(Lin *a, Lin *b) {
    if (a->n != b->n) return 0;
    if (a->c.num != b->c.num || a->c.den != b->c.den) return 0;
    for (int i = 0; i < a->n; i++) {
        int found = 0;
        for (int j = 0; j < b->n; j++)
            if (strcmp(a->terms[i].var, b->terms[j].var) == 0 &&
                a->terms[i].coeff.num == b->terms[j].coeff.num &&
                a->terms[i].coeff.den == b->terms[j].coeff.den) { found = 1; break; }
        if (!found) return 0;
    }
    return 1;
}

/* M3: a vec_get call resolves to a fresh symbolic read variable, so its value
 * can carry instantiated element-axiom constraints. Keyed by the call node. */
static void reads_init(ReadsList *l) { l->r = NULL; l->n = 0; l->cap = 0; }
static void reads_free(ReadsList *l) {
    for (int i = 0; i < l->n; i++) {
        free(l->r[i].var); lin_free(&l->r[i].fa); lin_free(&l->r[i].fb); lin_free(&l->r[i].mon_base);
    }
    free(l->r); l->r = NULL; l->n = l->cap = 0;
}
static ReadEntry *reads_slot(ReadsList *l, const char *var) {
    if (l->n == l->cap) { l->cap = l->cap ? l->cap * 2 : 4; l->r = realloc(l->r, (size_t)l->cap * sizeof(ReadEntry)); }
    ReadEntry *e = &l->r[l->n++];
    memset(e, 0, sizeof(*e));
    e->var = strdup(var);
    lin_init(&e->fa); lin_init(&e->fb); lin_init(&e->mon_base);
    return e;
}
static void reads_push(ReadsList *l, Node *call, const char *var) {
    ReadEntry *e = reads_slot(l, var);
    e->call = call;
}
static ReadEntry *reads_find_prod_lin(ReadsList *l, Lin *form, ReadEntry *self) {
    const char *v = lin_handle_var(form);
    if (!v) return NULL;
    for (int i = 0; i < l->n; i++) {
        if (&l->r[i] == self) continue;
        if (l->r[i].is_prod && strcmp(l->r[i].var, v) == 0) return &l->r[i];
    }
    return NULL;
}
/* M20: classify a recorded product as a pure power n^k when the factors
 * share one base (square, cube, quartic, …). Mixed products stay deg 0. */
static void classify_monomial(ReadsList *l, ReadEntry *e) {
    if (lin_eq(&e->fa, &e->fb)) {
        e->mon_deg = 2;
        e->mon_base = lin_clone(&e->fa);
        return;
    }
    ReadEntry *pa = reads_find_prod_lin(l, &e->fa, e);
    ReadEntry *pb = reads_find_prod_lin(l, &e->fb, e);
    if (pa && pa->mon_deg >= 2 && lin_eq(&e->fb, &pa->mon_base)) {
        e->mon_deg = pa->mon_deg + 1;
        e->mon_base = lin_clone(&e->fb);
    } else if (pb && pb->mon_deg >= 2 && lin_eq(&e->fa, &pb->mon_base)) {
        e->mon_deg = pb->mon_deg + 1;
        e->mon_base = lin_clone(&e->fa);
    } else if (pa && pb && pa->mon_deg >= 2 && pb->mon_deg >= 2 &&
               lin_eq(&pa->mon_base, &pb->mon_base)) {
        e->mon_deg = pa->mon_deg + pb->mon_deg;
        e->mon_base = lin_clone(&pa->mon_base);
    }
}
/* M8b: register a product var with its factor linear forms (moves fa/fb). */
static void reads_push_prod(ReadsList *l, const char *var, Lin fa, Lin fb) {
    ReadEntry *e = reads_slot(l, var);
    lin_free(&e->fa); lin_free(&e->fb);
    e->fa = fa; e->fb = fb; e->is_prod = 1;
    classify_monomial(l, e);
}
/* M9: register integer division by a constant (moves fa; fb is the const divisor). */
static void reads_push_div(ReadsList *l, const char *var, Lin fa, Lin fb) {
    ReadEntry *e = reads_slot(l, var);
    lin_free(&e->fa); lin_free(&e->fb);
    e->fa = fa; e->fb = fb; e->is_div = 1;
}
/* M9b: register integer mod by a constant. */
static void reads_push_mod(ReadsList *l, const char *var, Lin fa, Lin fb) {
    ReadEntry *e = reads_slot(l, var);
    lin_free(&e->fa); lin_free(&e->fb);
    e->fa = fa; e->fb = fb; e->is_mod = 1;
}
/* M22: register n >> k (аритметично отместване); fb държи БРОЯЧА k (const),
 * не делителя 2^k. */
static void reads_push_shr(ReadsList *l, const char *var, Lin fa, Lin fb) {
    ReadEntry *e = reads_slot(l, var);
    lin_free(&e->fa); lin_free(&e->fb);
    e->fa = fa; e->fb = fb; e->is_shr = 1;
}
/* M23: register n >>> k (логическо отместване, zero-fill); fb държи k. */
static void reads_push_lshr(ReadsList *l, const char *var, Lin fa, Lin fb) {
    ReadEntry *e = reads_slot(l, var);
    lin_free(&e->fa); lin_free(&e->fb);
    e->fa = fa; e->fb = fb; e->is_lshr = 1;
}
/* M13/M20: n & mask → residue in {0..mask} when mask = 2^k-1 (moves fa). */
static void reads_push_bitmask(ReadsList *l, const char *var, Lin fa, int64_t mask) {
    ReadEntry *e = reads_slot(l, var);
    lin_free(&e->fa); lin_free(&e->fb);
    e->fa = fa; e->fb = lin_const(rat_int(mask)); e->is_bitand1 = 1;
}
/* M20: variable n&m / n|m — bounds injected when both sides nonnegative. */
static void reads_push_bitop(ReadsList *l, const char *var, Lin fa, Lin fb, int is_or) {
    ReadEntry *e = reads_slot(l, var);
    lin_free(&e->fa); lin_free(&e->fb);
    e->fa = fa; e->fb = fb;
    if (is_or) e->is_bitor = 1; else e->is_bitand = 1;
}
/* M17: register a pair var with its two components (moves fa/fb). */
static void reads_push_pair(ReadsList *l, const char *var, Lin fa, Lin fb) {
    ReadEntry *e = reads_slot(l, var);
    lin_free(&e->fa); lin_free(&e->fb);
    e->fa = fa; e->fb = fb; e->is_pair = 1;
}
/* M17: find the pair entry anchored on a bare symbolic var, or NULL. */
static ReadEntry *reads_find_pair(ReadsList *l, const char *var) {
    for (int i = 0; i < l->n; i++)
        if (l->r[i].is_pair && strcmp(l->r[i].var, var) == 0) return &l->r[i];
    return NULL;
}
static const char *reads_find(ReadsList *l, Node *call) {
    for (int i = 0; i < l->n; i++)
        if (!l->r[i].is_prod && !l->r[i].is_div && !l->r[i].is_mod && !l->r[i].is_bitand1 &&
            l->r[i].call == call) return l->r[i].var;
    return NULL;
}
static void reads_clone_into(ReadsList *dst, ReadsList *src) {
    reads_init(dst);
    for (int i = 0; i < src->n; i++) {
        if (src->r[i].is_prod)
            reads_push_prod(dst, src->r[i].var, lin_clone(&src->r[i].fa), lin_clone(&src->r[i].fb));
        else if (src->r[i].is_div)
            reads_push_div(dst, src->r[i].var, lin_clone(&src->r[i].fa), lin_clone(&src->r[i].fb));
        else if (src->r[i].is_mod)
            reads_push_mod(dst, src->r[i].var, lin_clone(&src->r[i].fa), lin_clone(&src->r[i].fb));
        else if (src->r[i].is_shr)
            reads_push_shr(dst, src->r[i].var, lin_clone(&src->r[i].fa), lin_clone(&src->r[i].fb));
        else if (src->r[i].is_lshr)
            reads_push_lshr(dst, src->r[i].var, lin_clone(&src->r[i].fa), lin_clone(&src->r[i].fb));
        else if (src->r[i].is_bitand1) {
            int64_t mask = (lin_is_const(&src->r[i].fb) && src->r[i].fb.c.den == 1)
                           ? src->r[i].fb.c.num : 1;
            reads_push_bitmask(dst, src->r[i].var, lin_clone(&src->r[i].fa), mask);
        } else if (src->r[i].is_bitand || src->r[i].is_bitor)
            reads_push_bitop(dst, src->r[i].var, lin_clone(&src->r[i].fa), lin_clone(&src->r[i].fb), src->r[i].is_bitor);
        else if (src->r[i].is_pair)
            reads_push_pair(dst, src->r[i].var, lin_clone(&src->r[i].fa), lin_clone(&src->r[i].fb));
        else
            reads_push(dst, src->r[i].call, src->r[i].var);
    }
}

/* Snapshot product/div/mod/bit entries of a reads list (for obligation witnesses). */
static void prods_clone_into(ProdList *dst, ReadsList *src) {
    dst->p = NULL; dst->n = 0; dst->cap = 0;
    for (int i = 0; i < src->n; i++) {
        if (!src->r[i].is_prod && !src->r[i].is_div && !src->r[i].is_mod &&
            !src->r[i].is_shr && !src->r[i].is_lshr &&
            !src->r[i].is_bitand1 && !src->r[i].is_bitand && !src->r[i].is_bitor) continue;
        if (dst->n == dst->cap) { dst->cap = dst->cap ? dst->cap * 2 : 4; dst->p = realloc(dst->p, (size_t)dst->cap * sizeof(ProdEntry)); }
        dst->p[dst->n].var = strdup(src->r[i].var);
        dst->p[dst->n].fa = lin_clone(&src->r[i].fa);
        dst->p[dst->n].fb = lin_clone(&src->r[i].fb);
        dst->p[dst->n].is_div = src->r[i].is_div;
        dst->p[dst->n].is_mod = src->r[i].is_mod;
        dst->p[dst->n].is_shr = src->r[i].is_shr;
        dst->p[dst->n].is_lshr = src->r[i].is_lshr;
        dst->p[dst->n].is_bitand1 = src->r[i].is_bitand1;
        dst->p[dst->n].is_bitand = src->r[i].is_bitand;
        dst->p[dst->n].is_bitor = src->r[i].is_bitor;
        dst->n++;
    }
}
static void prods_free(ProdList *l) {
    for (int i = 0; i < l->n; i++) { free(l->p[i].var); lin_free(&l->p[i].fa); lin_free(&l->p[i].fb); }
    free(l->p); l->p = NULL; l->n = l->cap = 0;
}

/* M14: handle protocol state for !Par — join handles and channels are opaque
 * i64 values; we track a ghost protocol state per symbolic handle variable.
 * Keyed by the symbolic var name (e.g. __h0), so source-level aliasing
 * (`let h2 = h`) works: both names hold the same linear form. */
#define HK_JOIN 1   /* states: 0=open, 1=joined, 2=detached */
#define HK_CHAN 2   /* states: 0=open, 1=closed */
#define HK_DROP 3   /* MEM-2: alloc→drop; states: 0=live, 1=dropped.
                       * Keyed by the SOURCE variable name (Vec/Map/bytes are
                       * opaque, not symbolic Lin values), unlike M14 handles. */
typedef struct {
    char *var; int kind; int state; Lin result;
    /* M19 wait-for: join handle records the worker's first blocking op on
     * the channel argument (if any). Channel handles count this thread's
     * send/recv and anonymous (go_bg) producers. */
    char *worker;     /* HK_JOIN: worker fn name (owned) */
    char *chan_arg;   /* HK_JOIN: channel ghost var, or NULL */
    int wf_recv;      /* HK_JOIN: worker recvs param before send/return */
    int wf_send;      /* HK_JOIN: worker sends param before recv */
    int wf_complex;   /* HK_JOIN: scan inconclusive (if/loop/nested) */
    int wf_n_send;    /* HK_JOIN: worker sends on the channel arg (!wf_complex) */
    int wf_n_recv;    /* HK_JOIN: worker recvs on the channel arg (<=1 in fragment) */
    int n_send;       /* HK_CHAN: sends on this thread */
    int n_recv;       /* HK_CHAN: recvs on this thread */
    int n_bg_prod;    /* HK_CHAN: summed sends of go_bg send-first workers (M25) */
    int n_bg_cons;    /* HK_CHAN: go_bg workers that recv-first (M24 credits) */
    int wf_cap;       /* HK_CHAN: constant buffer capacity, -1 = unknown (M24) */
    int wf_unknown;   /* HK_CHAN: a spawned worker was too complex to classify */
} HandleEntry;
typedef struct { HandleEntry *h; int n, cap; } HandleList;

static void handles_init(HandleList *l) { l->h = NULL; l->n = 0; l->cap = 0; }
static void handles_free(HandleList *l) {
    for (int i = 0; i < l->n; i++) {
        free(l->h[i].var); lin_free(&l->h[i].result);
        free(l->h[i].worker); free(l->h[i].chan_arg);
    }
    free(l->h);
    handles_init(l);
}
static HandleEntry *handles_find(HandleList *l, const char *var) {
    for (int i = 0; i < l->n; i++) if (strcmp(l->h[i].var, var) == 0) return &l->h[i];
    return NULL;
}
/* result is moved (may be NULL for channels). Re-setting replaces in place. */
static void handles_set(HandleList *l, const char *var, int kind, int state, Lin *result) {
    HandleEntry *e = handles_find(l, var);
    if (!e) {
        if (l->n == l->cap) { l->cap = l->cap ? l->cap * 2 : 4; l->h = realloc(l->h, (size_t)l->cap * sizeof(HandleEntry)); }
        e = &l->h[l->n++];
        e->var = strdup(var);
        Lin empty; lin_init(&empty);
        e->result = empty;
        e->worker = NULL; e->chan_arg = NULL;
        e->wf_recv = e->wf_send = e->wf_complex = 0;
        e->wf_n_send = e->wf_n_recv = 0;
        e->n_send = e->n_recv = e->n_bg_prod = e->n_bg_cons = e->wf_unknown = 0;
        e->wf_cap = -1;
    } else {
        lin_free(&e->result);
    }
    e->kind = kind; e->state = state;
    if (result) { e->result = *result; }
    else { Lin empty; lin_init(&empty); e->result = empty; }
}
static void handles_clone_into(HandleList *dst, HandleList *src) {
    handles_init(dst);
    for (int i = 0; i < src->n; i++) {
        Lin r = lin_clone(&src->h[i].result);
        handles_set(dst, src->h[i].var, src->h[i].kind, src->h[i].state, &r);
        HandleEntry *d = handles_find(dst, src->h[i].var);
        d->n_send = src->h[i].n_send; d->n_recv = src->h[i].n_recv;
        d->n_bg_prod = src->h[i].n_bg_prod; d->n_bg_cons = src->h[i].n_bg_cons;
        d->wf_unknown = src->h[i].wf_unknown; d->wf_cap = src->h[i].wf_cap;
        d->wf_recv = src->h[i].wf_recv; d->wf_send = src->h[i].wf_send;
        d->wf_complex = src->h[i].wf_complex;
        d->wf_n_send = src->h[i].wf_n_send; d->wf_n_recv = src->h[i].wf_n_recv;
        d->worker = src->h[i].worker ? strdup(src->h[i].worker) : NULL;
        d->chan_arg = src->h[i].chan_arg ? strdup(src->h[i].chan_arg) : NULL;
    }
}

typedef struct { SEnv env; VLenMap vlen; AxiomList ax; ConsList path; ConsList read_cons; ReadsList reads; HandleList handles; int bad; } State;

static void state_free(State *s) { env_free(&s->env); vlen_free(&s->vlen); ax_free(&s->ax); cl_free(&s->path); cl_free(&s->read_cons); reads_free(&s->reads); handles_free(&s->handles); }

typedef struct { State *s; int n, cap; } States;
static void states_push(States *ss, State s) {
    if (ss->n == ss->cap) { ss->cap = ss->cap ? ss->cap * 2 : 4; ss->s = realloc(ss->s, (size_t)ss->cap * sizeof(State)); }
    ss->s[ss->n++] = s;
}

/* M5: program context for resolving user-function calls during symexec.
 * Set by verify_fn_collect / verify_program (proofs.c goes through
 * verify_fn_collect too, so it is always initialized before use). */
static Node *g_prog = NULL;
static const char *g_caller_name = NULL;
static int g_call_ctr = 0;
static int g_handle_ctr = 0;    /* M14: fresh __hN / __chN handle vars */
static int g_partial = 0;        /* direct self-recursion seen during symexec */
static int g_term = 0;           /* spec carries a decreases measure (M6) */
static int g_term_failed = 0;    /* some termination obligation is not PROVEN */

/* Verified-fact collection for --proofs: while-loop invariants encountered
 * during symexec, each flagged by whether the Hoare checks (init +
 * preservation) proved it trustworthy. */
static char **g_inv_texts = NULL;
static int  *g_inv_proven = NULL;
static int   g_inv_n = 0, g_inv_cap = 0;

/* Render an annotation expression (requires/ensures/invariant fragment) as
 * source-like text for --proofs output. */
static void expr_render(Node *e, char *b, size_t n, size_t *o) {
    if (!e || *o >= n - 1) return;
    #define ER_PUT(...) do { \
        int w = snprintf(b + *o, n - *o, __VA_ARGS__); \
        if (w > 0) { *o += (size_t)w; if (*o >= n) *o = n - 1; } \
    } while (0)
    static const char *binop_str[] = {
        "+", "-", "*", "/", "%", "==", "!=", "<", ">", "<=", ">=",
        "&&", "||", "&", "|", "^", "<<", ">>"
    };
    switch (e->kind) {
    case NODE_INT_LIT:  ER_PUT("%lld", (long long)e->int_val); break;
    case NODE_BOOL_LIT: ER_PUT("%s", e->bool_val ? "true" : "false"); break;
    case NODE_IDENT:    ER_PUT("%s", e->name); break;
    case NODE_ELEM_REF:
        expr_render(e->elem_obj, b, n, o);
        ER_PUT("[*]");
        break;
    case NODE_UNARY: {
        const char *op = e->un_op == UOP_NEG ? "-" : e->un_op == UOP_NOT ? "!" :
                         e->un_op == UOP_REF ? "&" : "*";
        ER_PUT("%s", op);
        expr_render(e->operand, b, n, o);
        break;
    }
    case NODE_BINARY:
        ER_PUT("(");
        expr_render(e->left, b, n, o);
        ER_PUT(" %s ", binop_str[e->bin_op]);
        expr_render(e->right, b, n, o);
        ER_PUT(")");
        break;
    case NODE_CALL:
        if (e->callee && e->callee->kind == NODE_IDENT) ER_PUT("%s", e->callee->name);
        else ER_PUT("?");
        ER_PUT("(");
        for (int i = 0; i < e->args.len; i++) {
            if (i) ER_PUT(", ");
            expr_render(e->args.data[i], b, n, o);
        }
        ER_PUT(")");
        break;
    default: ER_PUT("?"); break;
    }
    #undef ER_PUT
}

static void inv_collect_reset(void) {
    for (int i = 0; i < g_inv_n; i++) free(g_inv_texts[i]);
    free(g_inv_texts); free(g_inv_proven);
    g_inv_texts = NULL; g_inv_proven = NULL; g_inv_n = g_inv_cap = 0;
}

static void inv_collect_push(Node *inv, int proven) {
    if (g_inv_n == g_inv_cap) {
        g_inv_cap = g_inv_cap ? g_inv_cap * 2 : 8;
        g_inv_texts = realloc(g_inv_texts, (size_t)g_inv_cap * sizeof(char *));
        g_inv_proven = realloc(g_inv_proven, (size_t)g_inv_cap * sizeof(int));
    }
    char buf[512]; size_t off = 0; buf[0] = '\0';
    expr_render(inv, buf, sizeof buf, &off);
    g_inv_texts[g_inv_n] = strdup(buf);
    g_inv_proven[g_inv_n] = proven;
    g_inv_n++;
}

static Node *find_spec(Node *prog, const char *name);   /* defined below */
static int type_is_i64(Node *t);                        /* defined below */
static int ret_has_effects(Node *t);                    /* defined below */

static Node *find_fn(Node *prog, const char *name) {
    if (!prog) return NULL;
    for (int i = 0; i < prog->items.len; i++) {
        Node *it = prog->items.data[i];
        if (it->kind == NODE_FN && it->fn_body && it->fn_name && strcmp(it->fn_name, name) == 0)
            return it;
    }
    return NULL;
}

/* M5 call gate (shallow): the callee must have a body, an i64 signature and
 * no effects. Whether its BODY is verifiable is checked separately in
 * eval_user_call before its ensures may be assumed — this gate alone never
 * justifies an assumption, so the checks stay local (no call-graph walk). */
static int callee_sig_supported(Node *cfn) {
    if (!cfn || !cfn->ret_type) return 0;   /* void callee: no result to bind (M5) */
    if (ret_has_effects(cfn->ret_type)) return 0;
    if (!type_is_i64(cfn->ret_type)) return 0;
    for (int i = 0; i < cfn->params.len; i++)
        if (!type_is_i64(cfn->params.data[i]->param_type)) return 0;
    return 1;
}

/* Detect constructs the verifier cannot handle. `flat` treats a while loop as
 * an opaque boundary (its body is checked separately, with its invariant).
 * `in_expr` marks expression-nested positions — M5 user calls are supported
 * only at statement level (let init / return value / expression statement). */
static int is_vec_builtin_call(Node *n) {
    if (n->kind != NODE_CALL || !n->callee || n->callee->kind != NODE_IDENT) return 0;
    const char *nm = n->callee->name;
    return strcmp(nm, "vec_new") == 0 || strcmp(nm, "vec_push") == 0 ||
           strcmp(nm, "vec_get") == 0 || strcmp(nm, "vec_set") == 0 ||
           strcmp(nm, "vec_len") == 0 || strcmp(nm, "vec_slice") == 0 ||
           strcmp(nm, "vec_concat") == 0;
}

/* M17: pure pair builtins — allowed anywhere, including inside expressions
 * (`if cell2_0(r) == 1` is the normal usage pattern). */
static int is_pair_builtin_call(Node *n) {
    if (n->kind != NODE_CALL || !n->callee || n->callee->kind != NODE_IDENT) return 0;
    const char *nm = n->callee->name;
    return strcmp(nm, "cell2") == 0 || strcmp(nm, "cell2_0") == 0 || strcmp(nm, "cell2_1") == 0;
}

/* M14: the !Par builtins modeled by the verifier. Mutexes and pool_map stay
 * outside the fragment (honest skip). M17 adds the pair-returning channel
 * APIs (status, value) with ranges for the status component. */
static int is_par_builtin_call(Node *n) {
    if (n->kind != NODE_CALL || !n->callee || n->callee->kind != NODE_IDENT) return 0;
    const char *nm = n->callee->name;
    return strcmp(nm, "go") == 0 || strcmp(nm, "go_bg") == 0 ||
           strcmp(nm, "join") == 0 || strcmp(nm, "detach") == 0 ||
           strcmp(nm, "chan_new") == 0 || strcmp(nm, "chan_send") == 0 ||
           strcmp(nm, "chan_recv") == 0 || strcmp(nm, "chan_close") == 0 ||
           strcmp(nm, "chan_recv2") == 0 || strcmp(nm, "chan_try_recv") == 0 ||
           strcmp(nm, "chan_recv_timeout") == 0 || strcmp(nm, "chan_select2") == 0 ||
           strcmp(nm, "chan_select2_wait") == 0 || strcmp(nm, "chan_select2_timeout") == 0;
}

/* MEM-2: the drop(x) builtin — one bare-ident argument (the checker enforces
 * the rest). A user fn named `drop` shadows the builtin, same guard as the
 * checker (a let-bound `drop` variable is not visible here; such a call just
 * finds no HK_DROP handle and makes no claims — sound). */
static int is_drop_call(Node *n) {
    if (!n || n->kind != NODE_CALL || !n->callee || n->callee->kind != NODE_IDENT) return 0;
    if (strcmp(n->callee->name, "drop") != 0) return 0;
    if (n->args.len != 1 || n->args.data[0]->kind != NODE_IDENT) return 0;
    if (g_prog && find_fn(g_prog, "drop")) return 0;
    return 1;
}

/* MEM-2: allocators of owned, droppable buffers (vec_new is already covered
 * by is_vec_builtin_call; listed here for the registration hook). The values
 * themselves stay opaque — only the alloc→drop protocol is tracked. */
static int is_mem_alloc_call(Node *n) {
    if (!n || n->kind != NODE_CALL || !n->callee || n->callee->kind != NODE_IDENT) return 0;
    const char *nm = n->callee->name;
    return strcmp(nm, "vec_new") == 0 || strcmp(nm, "map_new") == 0 ||
           strcmp(nm, "bytes_new") == 0;
}

/* M14 shallow gate for a par builtin call: right arity; for go/go_bg the
 * worker must be a user fn (i64) -> i64 whose only allowed effect is Par
 * itself (channel-using workers are Par by construction); any other effect
 * puts it outside the fragment. Body verifiability is checked later, at
 * eval time, before any ensures are assumed (same discipline as M5). */
static int ret_has_unverifiable_effects(Node *t);   /* defined below */
static int worker_sig_supported(Node *cfn) {
    if (!cfn || !cfn->ret_type) return 0;
    if (ret_has_unverifiable_effects(cfn->ret_type)) return 0;
    if (!type_is_i64(cfn->ret_type)) return 0;
    for (int i = 0; i < cfn->params.len; i++)
        if (!type_is_i64(cfn->params.data[i]->param_type)) return 0;
    return 1;
}

static int par_call_gate(Node *n) {
    const char *nm = n->callee->name;
    if (strcmp(nm, "go") == 0 || strcmp(nm, "go_bg") == 0) {
        if (n->args.len != 2) return 0;
        Node *w = n->args.data[0];
        if (!w || w->kind != NODE_IDENT || !g_prog) return 0;
        return worker_sig_supported(find_fn(g_prog, w->name));
    }
    if (strcmp(nm, "chan_send") == 0) return n->args.len == 2;
    if (strcmp(nm, "chan_recv_timeout") == 0 || strcmp(nm, "chan_select2") == 0 ||
        strcmp(nm, "chan_select2_wait") == 0) return n->args.len == 2;
    if (strcmp(nm, "chan_select2_timeout") == 0) return n->args.len == 3;
    return n->args.len == 1;   /* join/detach/chan_new/chan_recv/chan_close/recv2/try_recv */
}

static int has_unsupported_rec(Node *n, int flat, int in_expr) {
    if (!n) return 0;
    switch (n->kind) {
    case NODE_FOR: case NODE_MATCH:
    case NODE_TRY: case NODE_CATCH:
    case NODE_BREAK: case NODE_CONTINUE:
    case NODE_INDEX: case NODE_FIELD:
    case NODE_STRUCT_LIT: case NODE_RANGE:
        return 1;
    case NODE_CALL:
        if (is_vec_builtin_call(n) || is_pair_builtin_call(n)) {
            for (int i = 0; i < n->args.len; i++) if (has_unsupported_rec(n->args.data[i], flat, 1)) return 1;
            return 0;
        }
        /* M14: par builtins at statement level only — never inside a loop
         * (flat) and never nested in an expression. */
        if (is_par_builtin_call(n)) {
            if (flat || in_expr || !par_call_gate(n)) return 1;
            for (int i = 0; i < n->args.len; i++) if (has_unsupported_rec(n->args.data[i], flat, 1)) return 1;
            return 0;
        }
        /* MEM-2: drop(x) — same discipline as the par builtins: statement
         * level only, never inside a loop or nested in an expression. The
         * argument is a bare ident by construction, nothing to recurse into. */
        if (is_drop_call(n)) {
            if (flat || in_expr) return 1;
            return 0;
        }
        /* MEM-2: map_new/bytes_new — owned opaque buffers; allowed like the
         * vec builtins (vec_new itself is covered by is_vec_builtin_call). */
        if (is_mem_alloc_call(n)) {
            for (int i = 0; i < n->args.len; i++) if (has_unsupported_rec(n->args.data[i], flat, 1)) return 1;
            return 0;
        }
        /* M5: a user call in statement position is supported when the callee
         * carries a spec and has an i64 signature (shallow gate). */
        if (!in_expr && g_prog && n->callee && n->callee->kind == NODE_IDENT &&
            find_spec(g_prog, n->callee->name) &&
            callee_sig_supported(find_fn(g_prog, n->callee->name))) {
            for (int i = 0; i < n->args.len; i++) if (has_unsupported_rec(n->args.data[i], flat, 1)) return 1;
            return 0;
        }
        return 1;   /* any other call: nested / spec-less / extern */
    case NODE_WHILE:
        if (flat) return 1;
        if (n->while_invariants.len == 0) return 1;   /* M1: loops need invariants */
        if (has_unsupported_rec(n->while_cond, 1, 1)) return 1;
        for (int i = 0; i < n->while_invariants.len; i++)
            if (has_unsupported_rec(n->while_invariants.data[i], 1, 1)) return 1;
        return has_unsupported_rec(n->while_body, 0, 0);
    default: break;
    }
    switch (n->kind) {
    case NODE_BINARY: return has_unsupported_rec(n->left, 1, 1) || has_unsupported_rec(n->right, 1, 1);
    case NODE_UNARY: return has_unsupported_rec(n->operand, 1, 1);
    case NODE_IF: return has_unsupported_rec(n->cond, flat, 1) || has_unsupported_rec(n->then_br, flat, 0) || has_unsupported_rec(n->else_br, flat, 0);
    case NODE_BLOCK: for (int i = 0; i < n->stmts.len; i++) if (has_unsupported_rec(n->stmts.data[i], flat, 0)) return 1; return 0;
    case NODE_LET: return has_unsupported_rec(n->let_init, flat, 0);
    case NODE_ASSIGN: return has_unsupported_rec(n->assign_target, 1, 1) || has_unsupported_rec(n->assign_val, 1, 1);
    case NODE_RETURN: return has_unsupported_rec(n->ret_val, flat, 0);
    case NODE_EXPR_STMT: return has_unsupported_rec(n->expr, flat, 0);
    case NODE_INVARIANT:
        for (int i = 0; i < n->inv_exprs.len; i++)
            if (has_unsupported_rec(n->inv_exprs.data[i], 1, 1)) return 1;
        return 0;
    default: return 0;
    }
}

static int has_unsupported(Node *n) { return has_unsupported_rec(n, 0, 0); }

static State clone_state_with(State *cur, Formula *add);
static void inject_prod_axioms(State *st);
static void symexec_block(Node *blk, States *states, Obligations *ob, int is_nonvoid, Node *spec);
static void scan_arith_expr(Node *e, State *st, Obligations *ob);   /* M15 */
static void check_drop_uses(Node *e, State *st, Obligations *ob);   /* MEM-2 */
static int cl_implied_by(ConsList *path, ConsList *cons);

/* Prove one invariant holds in every body-final state, given the head state
 * (invariant ∧ condition on entry). UNSAT per DNF branch ⇒ holds. */
static int has_elem_ref(Node *e);
static int check_preservation(State *head, Node *inv, States *body_final) {
    (void)head;
    /* element axioms (v[*] ...) are preserved by the vec_push axiom rule
     * (axiom_holds_for_value), not by this scalar path check — skip it */
    if (has_elem_ref(inv)) return 1;
    int holds = 1;
    for (int k = 0; k < body_final->n && holds; k++) {
        State *bf = &body_final->s[k];
        if (bf->bad) { holds = 0; break; }
        Formula bf_inv;
        if (!bool_to_dnf(inv, &bf->env, &bf->vlen, &bf->reads, 0, &bf_inv)) { holds = 0; break; }
        for (int b = 0; b < bf_inv.n && holds; b++) {
            ConsList sys; cl_init(&sys);
            for (int x = 0; x < bf->path.n; x++) { Constraint *c = &bf->path.c[x]; cl_push(&sys, mk_cons(lin_clone(&c->lhs), c->op, c->rhs)); }
            for (int x = 0; x < bf_inv.br[b].n; x++) {
                Constraint *c = &bf_inv.br[b].c[x];
                Lin lhs = lin_neg(&c->lhs);
                COp op = (c->op == C_LT) ? C_LE : C_LT;
                cl_push(&sys, mk_cons(lhs, op, c->rhs));
            }
            if (fm_sat(&sys)) holds = 0;   /* SAT ⇒ a body outcome breaks the invariant */
        }
        f_free(&bf_inv);
    }
    return holds;
}

/* Symbolic execution of a while loop with invariants (Hoare):
 *   init        — invariant holds on entry (from the current path);
 *   preservation— invariant ∧ cond, after one body iteration, ⇒ invariant;
 *   post-loop   — invariant ∧ ¬cond continues the fall-through path, but ONLY
 *                 if init and preservation are both PROVEN (soundness gate —
 *                 otherwise any downstream proof depending on it is UNKNOWN).
 * Returns 1 iff the invariant is trustworthy (init ∧ preservation proven). */
static void extract_elem_axiom(Node *e, SEnv *env, AxiomList *ax);
static int has_elem_ref(Node *e);

/* Soundness fix (M15): variables assigned or let-bound inside a loop body
 * must be HAVOCED before the invariant is assumed — otherwise the head and
 * post-loop states keep the STALE pre-loop values and the invariant is
 * vacuous (false proofs; e.g. a loop returning -n "proved" output >= 0).
 * With havoc the Hoare rule is the real one: init on the entry state;
 * preservation over fresh values constrained only by invariant ∧ cond;
 * post-loop invariant ∧ ¬cond over fresh values. */
static int g_havoc_ctr = 0;

static void collect_assigned_rec(Node *n, const char ***names, int *nn, int *cap) {
    if (!n) return;
    const char *vn = NULL;
    switch (n->kind) {
    case NODE_EXPR_STMT: collect_assigned_rec(n->expr, names, nn, cap); return;
    case NODE_ASSIGN:
        if (n->assign_target->kind == NODE_IDENT) vn = n->assign_target->name;
        collect_assigned_rec(n->assign_val, names, nn, cap);
        break;
    case NODE_LET:
        vn = n->let_name;   /* body-local let leaks/shadows в плоския env */
        collect_assigned_rec(n->let_init, names, nn, cap);
        break;
    case NODE_IF:
        collect_assigned_rec(n->then_br, names, nn, cap);
        collect_assigned_rec(n->else_br, names, nn, cap);
        return;
    case NODE_WHILE:
        collect_assigned_rec(n->while_body, names, nn, cap);
        return;
    case NODE_BLOCK:
        for (int i = 0; i < n->stmts.len; i++) collect_assigned_rec(n->stmts.data[i], names, nn, cap);
        return;
    default: return;
    }
    if (vn) {
        int found = 0;
        for (int k = 0; k < *nn; k++) if (strcmp((*names)[k], vn) == 0) { found = 1; break; }
        if (!found) {
            if (*nn == *cap) { *cap = *cap ? *cap * 2 : 8; *names = realloc(*names, (size_t)*cap * sizeof(char *)); }
            (*names)[(*nn)++] = vn;
        }
    }
}

/* Rebind every name to a fresh abstract symbolic value (__-prefixed, so the
 * witness machinery treats it as non-reportable); tracked Vec lengths of
 * reassigned vectors are havoced too. */
static void havoc_vars(State *st, const char **names, int nn) {
    for (int i = 0; i < nn; i++) {
        char hv[64]; snprintf(hv, sizeof hv, "__hv%d", g_havoc_ctr++);
        env_bind(&st->env, names[i], lin_var(hv), 0);
        if (vlen_find(&st->vlen, names[i])) {
            char lb[300]; snprintf(lb, sizeof lb, "__hvl%d", g_havoc_ctr++);
            vlen_set(&st->vlen, names[i], lin_var(lb), 0);
        }
    }
}

static int symexec_while(Node *st, States *states, Obligations *ob, int is_nonvoid, Node *spec) {
    (void)is_nonvoid;   /* the loop body is never a return context */
    int trusted = 1;
    States out; out.s = NULL; out.n = 0; out.cap = 0;
    const char **havoc = NULL; int nhav = 0, hcap = 0;   /* loop-assigned vars (borrowed AST names) */
    collect_assigned_rec(st->while_body, &havoc, &nhav, &hcap);
    for (int i = 0; i < states->n; i++) {
        State *cur = &states->s[i];
        scan_arith_expr(st->cond, cur, ob);   /* M15: loop-guard arithmetic (entry) */
        check_drop_uses(st->cond, cur, ob);   /* MEM-2: dropped buffer in the guard */

        /* init: each invariant must follow from the current path */
        for (int j = 0; j < st->while_invariants.len && trusted; j++) {
            /* element axioms (v[*] ...) are vacuous at init (empty/any vec) and
             * are tracked separately — skip the scalar init check for them */
            if (has_elem_ref(st->while_invariants.data[j])) continue;
            Formula invf;
            if (!bool_to_dnf(st->while_invariants.data[j], &cur->env, &cur->vlen, &cur->reads, 0, &invf)) { trusted = 0; break; }
            for (int b = 0; b < invf.n && trusted; b++) {
                ConsList sys; cl_init(&sys);
                for (int x = 0; x < cur->path.n; x++) { Constraint *c = &cur->path.c[x]; cl_push(&sys, mk_cons(lin_clone(&c->lhs), c->op, c->rhs)); }
                for (int x = 0; x < invf.br[b].n; x++) {
                    Constraint *c = &invf.br[b].c[x];
                    Lin lhs = lin_neg(&c->lhs);
                    COp op = (c->op == C_LT) ? C_LE : C_LT;
                    cl_push(&sys, mk_cons(lhs, op, c->rhs));
                }
                int s = fm_sat(&sys);
                if (s) trusted = 0;
            }
            f_free(&invf);
        }

        /* preservation: invariant ∧ cond, run one body iteration, re-check */
        Formula cf;
        if (!bool_to_dnf(st->cond, &cur->env, &cur->vlen, &cur->reads, 0, &cf)) { trusted = 0; }
        /* M13: inject product/bit axioms from the loop condition */
        inject_prod_axioms(cur);
        States head_ss; head_ss.s = NULL; head_ss.n = 0; head_ss.cap = 0;
        if (trusted) {
            for (int b = 0; b < cf.n; b++) {
                Formula one; f_init(&one);
                ConsList cl; cl_init(&cl);
                for (int x = 0; x < cf.br[b].n; x++) { Constraint *c = &cf.br[b].c[x]; cl_push(&cl, mk_cons(lin_clone(&c->lhs), c->op, c->rhs)); }
                f_push_branch(&one, cl);
                State h = clone_state_with(cur, &one);
                f_free(&one);
                havoc_vars(&h, havoc, nhav);   /* M15: no stale pre-loop values */
                for (int j = 0; j < st->while_invariants.len; j++) {
                    Formula invf;
                    if (!bool_to_dnf(st->while_invariants.data[j], &h.env, &h.vlen, &h.reads, 0, &invf)) { trusted = 0; break; }
                    for (int bb = 0; bb < invf.n; bb++)
                        for (int x = 0; x < invf.br[bb].n; x++) { Constraint *c = &invf.br[bb].c[x]; cl_push(&h.path, mk_cons(lin_clone(&c->lhs), c->op, c->rhs)); }
                    f_free(&invf);
                }
                states_push(&head_ss, h);
            }
            /* the loop body is not a return context: its last statement is NOT
             * an implicit return, so pass is_nonvoid=0 */
            symexec_block(st->while_body, &head_ss, ob, 0, spec);
            for (int j = 0; j < st->while_invariants.len && trusted; j++) {
                int ok = check_preservation(cur, st->while_invariants.data[j], &head_ss);
                if (!ok) trusted = 0;
            }
        }
        for (int k = 0; k < head_ss.n; k++) state_free(&head_ss.s[k]);
        free(head_ss.s);
        f_free(&cf);

        /* post-loop continuation: invariant ∧ ¬cond (only sound when trusted) */
        Formula nf;
        if (!bool_to_dnf(st->cond, &cur->env, &cur->vlen, &cur->reads, 1, &nf)) {
            cur->bad = 1;
            states_push(&out, *cur);
            continue;
        }
        States post_ss; post_ss.s = NULL; post_ss.n = 0; post_ss.cap = 0;
        for (int b = 0; b < nf.n; b++) {
            Formula one; f_init(&one);
            ConsList cl; cl_init(&cl);
            for (int x = 0; x < nf.br[b].n; x++) { Constraint *c = &nf.br[b].c[x]; cl_push(&cl, mk_cons(lin_clone(&c->lhs), c->op, c->rhs)); }
            f_push_branch(&one, cl);
            State t = clone_state_with(cur, &one);
            f_free(&one);
            havoc_vars(&t, havoc, nhav);   /* M15: post-loop values are fresh */
            if (trusted) {
                for (int j = 0; j < st->while_invariants.len; j++) {
                    Formula invf;
                    if (!bool_to_dnf(st->while_invariants.data[j], &t.env, &t.vlen, &t.reads, 0, &invf)) { t.bad = 1; break; }
                    for (int bb = 0; bb < invf.n; bb++)
                        for (int x = 0; x < invf.br[bb].n; x++) { Constraint *c = &invf.br[bb].c[x]; cl_push(&t.path, mk_cons(lin_clone(&c->lhs), c->op, c->rhs)); }
                    f_free(&invf);
                }
            } else {
                t.bad = 1;   /* invariant not proven ⇒ downstream may not rely on it */
            }
            states_push(&post_ss, t);
        }
        for (int k = 0; k < post_ss.n; k++) states_push(&out, post_ss.s[k]);
        free(post_ss.s);
        f_free(&nf);
        env_free(&cur->env); cl_free(&cur->path); handles_free(&cur->handles);
    }
    free(states->s);
    *states = out;
    /* M3: element axioms declared in the loop invariant hold after the loop
     * (the invariant is assumed established & preserved by the contract).
     * while_invariants stores the raw expression nodes (not NODE_ENSURE). */
    for (int i = 0; i < states->n; i++)
        for (int j = 0; j < st->while_invariants.len; j++)
            extract_elem_axiom(st->while_invariants.data[j], &states->s[i].env, &states->s[i].ax);
    /* record the loop's invariants for --proofs (verified facts, M1) */
    for (int j = 0; j < st->while_invariants.len; j++)
        inv_collect_push(st->while_invariants.data[j], trusted);
    free((void *)havoc);
    return trusted;
}

/* Track Vec lengths and emit bounds obligations for vec accesses in an
 * expression statement. Only the top-level call and a let-init call are
 * handled; nested vec calls inside larger expressions are not tracked. */
static Constraint axiom_instantiate(ElemAxiom *a, const char *read_var);
static int axiom_holds_for_value(ElemAxiom *a, Sym *val, State *st);

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
static const char *resolve_key(Node *act, State *st, char *buf, size_t n) {
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

static void ax_drop_key(AxiomList *ax, const char *key) {
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

static void extract_elem_axiom(Node *e, SEnv *env, AxiomList *ax);

/* Extract a content axiom from `obj[*] <cmp> <lin>` (either orientation),
 * anchoring on the object's RESOLVED symbolic var; falls back to the M3
 * source-name extraction (local vecs). Returns 0 if not extractable. */
static int extract_axiom_resolved(Node *e, State *st) {
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

static void scan_vec_expr(Node *e, State *st, Obligations *ob, Node *spec) {
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
static int cl_implied_by(ConsList *path, ConsList *cons) {
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

static int is_user_call(Node *e) {
    return e && e->kind == NODE_CALL && e->callee && e->callee->kind == NODE_IDENT &&
           !is_vec_builtin_call(e) && g_prog && find_spec(g_prog, e->callee->name) &&
           callee_sig_supported(find_fn(g_prog, e->callee->name));
}

/* M8b: structural equality of linear forms (terms are merged canonically,
 * so a zero difference means equal). */
static int lin_eq(Lin *a, Lin *b) {
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
static void path_push_ge(ConsList *path, const char *pv, int64_t K) {
    Lin v = lin_var(pv);
    Lin t = lin_neg(&v);            /* -pv */
    lin_free(&v);
    t.c = rat_add(t.c, rat_int(K)); /* K - pv <= 0  ⟺  pv >= K */
    cl_push(path, mk_cons(t, C_LE, rat_zero()));
}

/* Push the fact pv <= K onto the path. */
static void path_push_le(ConsList *path, const char *pv, int64_t K) {
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
static void inject_prod_axioms(State *st) {
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
static Sym eval_user_call(Node *call, State *st, Obligations *ob, const char *caller_name) {
    const char *callee = call->callee->name;
    Node *cfn = find_fn(g_prog, callee);
    Node *cspec = find_spec(g_prog, callee);
    if (caller_name && strcmp(callee, caller_name) == 0) g_partial = 1;

    char rv[64]; snprintf(rv, sizeof rv, "__c%d", g_call_ctr++);

    SEnv env2; env_init(&env2);
    int ok = cfn && call->args.len == cfn->params.len;
    for (int i = 0; ok && i < cfn->params.len; i++) {
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
static void push_protocol_violation(State *st, Obligations *ob, const char *label) {
    ConsList opath; cl_init(&opath);
    for (int x = 0; x < st->path.n; x++) { Constraint *c = &st->path.c[x]; cl_push(&opath, mk_cons(lin_clone(&c->lhs), c->op, c->rhs)); }
    ConsList pos; cl_init(&pos);
    cl_push(&pos, mk_cons(lin_const(rat_int(1)), C_LE, rat_zero()));
    obl_push(ob, opath, pos, (ConsList){NULL, 0, 0}, sym_lin(lin_const(rat_int(0))), &st->vlen, &st->reads, 0, 3, label);
}

/* M19: tautological kind-3 obligation (0 <= 0). Discharges as PROVEN on a
 * live path — used for structured wait-for acyclicity, never for a guess. */
static void push_protocol_ok(State *st, Obligations *ob, const char *label) {
    ConsList opath; cl_init(&opath);
    for (int x = 0; x < st->path.n; x++) { Constraint *c = &st->path.c[x]; cl_push(&opath, mk_cons(lin_clone(&c->lhs), c->op, c->rhs)); }
    ConsList pos; cl_init(&pos);
    cl_push(&pos, mk_cons(lin_const(rat_int(0)), C_LE, rat_zero()));
    obl_push(ob, opath, pos, (ConsList){NULL, 0, 0}, sym_lin(lin_const(rat_int(0))), &st->vlen, &st->reads, 0, 3, label);
}

/* ======================= M19: wait-for acyclicity =======================
 * Structural liveness fragment (docs/thesis-open-problems.md §1.4).
 * A cycle is REFUTED when a thread waits on a recv/join that can only be
 * satisfied by an action the waiter itself is blocking. Matched sequential
 * send/recv, join-after-send, and recv-after-a-send-first worker are PROVEN.
 * if/while bodies, nested go, recv2/select, packed args, and non-constant
 * loop bounds are no-claim (honest incompleteness) — never a false PROVEN.
 *
 * M24: send-blocking on a full bounded buffer (baga_chan_send waits on
 * not_full, src/baga_par_rt.c). With a constant capacity (chan_new literal)
 * a parent send is checked against outstanding = n_send - n_recv so far
 * minus recv credits (recv-first workers already spawned, go_bg consumers
 * included): outstanding - credits >= cap with no complex worker on the
 * channel is a kind-3 REFUTED; otherwise "свободен слот" is PROVEN. A join
 * of a send-only worker is REFUTED when wf_n_send > cap + parent recvs +
 * other consumer credits — the worker can never finish. Competing producers
 * (parent sends before the join on that channel, go_bg send-first) keep the
 * join check honest no-claim; symbolic capacity keeps both checks silent.
 *
 * M25: counted loops in workers (known-N fan-in). A `for i in lo..hi` with
 * constant bounds is scanned once and its counts scaled by k = hi - lo
 * (hi exclusive); loop-body aliases do not leak. Producer coverage is now
 * exact — wf_producer_capacity sums wf_n_send over classified send-first
 * workers — so a parent recv past parent sends + producer capacity is
 * REFUTED (previously no-claim past one), and multi-recv workers are honest
 * credits instead of complex. while/if bodies, non-constant ranges, and
 * nested go stay no-claim. */

typedef struct {
    int recv_first, send_first, n_recv, n_send, complex, saw_op;
} WfScan;

static int wf_name_is(const char **names, int nn, const char *s) {
    for (int i = 0; i < nn; i++) if (strcmp(names[i], s) == 0) return 1;
    return 0;
}

static void wf_scan_node(Node *n, const char **names, int *nn, int nmax, WfScan *s);

static void wf_scan_call(Node *call, const char **names, int nn, WfScan *s) {
    if (!call || call->kind != NODE_CALL) return;
    if (!is_par_builtin_call(call)) { s->complex = 1; return; }  /* hidden send/recv */
    const char *nm = call->callee->name;
    if (strcmp(nm, "go") == 0 || strcmp(nm, "go_bg") == 0 ||
        strcmp(nm, "join") == 0 || strcmp(nm, "detach") == 0) {
        s->complex = 1;
        return;
    }
    int on_p = call->args.len >= 1 && call->args.data[0]->kind == NODE_IDENT &&
               wf_name_is(names, nn, call->args.data[0]->name);
    if (!on_p) {
        if (strcmp(nm, "chan_recv") == 0 || strcmp(nm, "chan_send") == 0) s->complex = 1;
        return;
    }
    if (strcmp(nm, "chan_recv") == 0) {
        s->n_recv++;
        if (!s->saw_op) { s->saw_op = 1; s->recv_first = 1; }
    } else if (strcmp(nm, "chan_send") == 0) {
        s->n_send++;
        if (!s->saw_op) { s->saw_op = 1; s->send_first = 1; }
    } else if (strcmp(nm, "chan_close") == 0 || strcmp(nm, "chan_new") == 0) {
        /* non-blocking */
    } else {
        s->complex = 1;   /* recv2 / try_recv / select* — not in this fragment */
    }
}

static void wf_scan_node(Node *n, const char **names, int *nn, int nmax, WfScan *s) {
    if (!n || s->complex) return;
    switch (n->kind) {
    case NODE_BLOCK:
        for (int i = 0; i < n->stmts.len; i++) wf_scan_node(n->stmts.data[i], names, nn, nmax, s);
        return;
    case NODE_LET:
        wf_scan_node(n->let_init, names, nn, nmax, s);
        if (n->let_init && n->let_init->kind == NODE_IDENT &&
            wf_name_is(names, *nn, n->let_init->name)) {
            if (*nn < nmax) names[(*nn)++] = n->let_name;
            else s->complex = 1;
        }
        return;
    case NODE_EXPR_STMT: wf_scan_node(n->expr, names, nn, nmax, s); return;
    case NODE_RETURN:    wf_scan_node(n->ret_val, names, nn, nmax, s); return;
    case NODE_ASSIGN:    wf_scan_node(n->assign_val, names, nn, nmax, s); return;
    case NODE_CALL:      wf_scan_call(n, names, *nn, s); return;
    case NODE_BINARY:
        wf_scan_node(n->left, names, nn, nmax, s);
        wf_scan_node(n->right, names, nn, nmax, s);
        return;
    case NODE_UNARY:     wf_scan_node(n->operand, names, nn, nmax, s); return;
    case NODE_IF: case NODE_WHILE: case NODE_MATCH:
        s->complex = 1;
        return;
    case NODE_FOR: {
        /* M25: counted loop over a constant range (hi exclusive, codegen
         * emits x < hi). Scan the body once, scale its counts by k — the
         * known-N discipline of §1.4. Aliases born in the body stay in the
         * body (loop scope); an empty range executes nothing. */
        if (!n->for_iter || n->for_iter->kind != NODE_RANGE ||
            !n->for_iter->range_lo || !n->for_iter->range_hi ||
            n->for_iter->range_lo->kind != NODE_INT_LIT ||
            n->for_iter->range_hi->kind != NODE_INT_LIT) { s->complex = 1; return; }
        long long k = (long long)n->for_iter->range_hi->int_val -
                      (long long)n->for_iter->range_lo->int_val;
        if (k <= 0) return;
        WfScan bs; memset(&bs, 0, sizeof bs);
        int saved_nn = *nn;
        wf_scan_node(n->for_body, names, nn, nmax, &bs);
        *nn = saved_nn;
        if (bs.complex) { s->complex = 1; return; }
        if (k > 1000000 ||
            (long long)(bs.n_send + bs.n_recv) * k > 1000000000LL) { s->complex = 1; return; }
        s->n_send += (int)(bs.n_send * k);
        s->n_recv += (int)(bs.n_recv * k);
        if (!s->saw_op && bs.saw_op) {
            s->saw_op = 1;
            s->recv_first = bs.recv_first;
            s->send_first = bs.send_first;
        }
        return;
    }
    default: return;
    }
}

/* M25: exact producer capacity — the total sends of classified send-first
 * workers on `ch` (join handles carry wf_n_send; n_bg_prod is the summed
 * send count of go_bg send-first workers). Complex workers keep the set
 * unknown — never a false REFUTED. */
static int wf_producer_capacity(State *st, HandleEntry *ch, int *unknown) {
    if (!ch) return 0;
    if (ch->wf_unknown) *unknown = 1;
    int cap = ch->n_bg_prod;
    for (int i = 0; i < st->handles.n; i++) {
        HandleEntry *h = &st->handles.h[i];
        if (h->kind != HK_JOIN || !h->chan_arg) continue;
        if (strcmp(h->chan_arg, ch->var) != 0) continue;
        if (h->wf_complex) *unknown = 1;
        else if (h->wf_send) cap += h->wf_n_send;
    }
    return cap;
}

static void wf_check_recv(State *st, Obligations *ob, HandleEntry *ch) {
    if (!ch) return;
    ch->n_recv++;
    int unk = 0;
    int capw = wf_producer_capacity(st, ch, &unk);
    int unmatched = ch->n_recv - ch->n_send;
    if (unmatched <= 0) {
        push_protocol_ok(st, ob, "wait-for: последователни send/recv");
    } else if (unmatched <= capw) {
        push_protocol_ok(st, ob, "wait-for: recv след producer");
    } else if (!unk) {
        push_protocol_violation(st, ob, "wait-for цикъл: recv без send");
    }
}

/* M24: parent send on a constant-capacity channel. A recv credit is a
 * recv-first worker already spawned (join handle or go_bg) — it is blocked
 * on recv, so it will consume one item. Blocked forever iff the items in
 * flight before this send, minus every credit, still fill the buffer and no
 * complex worker could be a hidden consumer. */
static void wf_check_send(State *st, Obligations *ob, HandleEntry *ch) {
    if (!ch || ch->wf_cap < 0) return;
    if (ch->state != 0) return;   /* closed: send never blocks (returns -1) */
    int credits = ch->n_bg_cons, unknown = ch->wf_unknown;
    for (int i = 0; i < st->handles.n; i++) {
        HandleEntry *h = &st->handles.h[i];
        if (h->kind != HK_JOIN || !h->chan_arg) continue;
        if (strcmp(h->chan_arg, ch->var) != 0) continue;
        if (h->wf_complex) unknown = 1;
        else if (h->wf_recv) credits += h->wf_n_recv;
    }
    if (ch->n_send - ch->n_recv - credits >= ch->wf_cap) {
        if (!unknown)
            push_protocol_violation(st, ob, "wait-for цикъл: send върху пълен буфер без consumer");
    } else {
        push_protocol_ok(st, ob, "wait-for: send — свободен слот");
    }
}

static void wf_check_join(State *st, Obligations *ob, HandleEntry *jh) {
    if (!jh || !jh->chan_arg || jh->wf_complex) return;
    HandleEntry *ch = handles_find(&st->handles, jh->chan_arg);
    if (!ch || ch->kind != HK_CHAN) return;
    if (jh->wf_recv) {
        int unk = 0;
        int capw = wf_producer_capacity(st, ch, &unk);
        if (ch->n_send > 0) {
            push_protocol_ok(st, ob, "wait-for: join след send");
        } else if (capw > 0) {
            push_protocol_ok(st, ob, "wait-for: join, друг producer");
        } else if (!unk) {
            push_protocol_violation(st, ob, "wait-for цикъл: join преди send");
        }
    }
    /* M24: a send-only worker must fit — its sends complete iff
     * wf_n_send <= cap + parent recvs so far + other consumer credits.
     * Competing producers on the channel make room accounting dishonest:
     * no-claim. */
    if (jh->wf_send && jh->wf_n_recv == 0 && ch->wf_cap >= 0 &&
        ch->state == 0 && ch->n_send == 0 && ch->n_bg_prod == 0) {
        int credits = ch->n_bg_cons, unknown = ch->wf_unknown, rivals = 0;
        for (int i = 0; i < st->handles.n; i++) {
            HandleEntry *h = &st->handles.h[i];
            if (h == jh || h->kind != HK_JOIN || !h->chan_arg) continue;
            if (strcmp(h->chan_arg, ch->var) != 0) continue;
            if (h->wf_complex) unknown = 1;
            else if (h->wf_recv) credits += h->wf_n_recv;
            else if (h->wf_send) rivals = 1;
        }
        if (rivals) return;
        int room = ch->wf_cap + ch->n_recv + credits;
        if (jh->wf_n_send > room) {
            if (!unknown)
                push_protocol_violation(st, ob, "wait-for цикъл: worker send върху пълен буфер");
        } else {
            push_protocol_ok(st, ob, "wait-for: worker send се побира в буфера");
        }
    }
}

/* M14: symbolic semantics of the !Par builtins (statement level only).
 *
 * go(f, x): f must be a pure verifiable (i64)->i64 user fn (gated earlier).
 * Fork-join determinism: the spawned computation of a pure worker is the
 * function call itself, so the worker's contract applies via the M5
 * assume–guarantee machinery — the join handle carries the symbolic result.
 * join(h)/detach(h): the runtime handle protocol (spawn → join | detach;
 * join-after-detach is fatal, src/baga_par_rt.c) becomes a static protocol:
 * a second consume on the same handle is a kind-3 violation (REFUTED).
 * Channels: open/closed ghost state; send returns 0 (ok) or -1 (closed) —
 * the interval -1 <= s <= 0 is exact; on a known-closed channel s == -1.
 * recv payload is unconstrained (any payload, or 0 on closed+empty).
 * M19 wait-for: join-before-send with a recv-first worker, and recv with no
 * matching send/producer, are kind-3 REFUTED; matched sequential send/recv
 * and join-after-send are PROVEN. Returns the symbolic value of the call. */
/* M17: a (status, value) pair result for the pair-returning channel APIs.
 * status: fresh __c var with [0, hi] range on the path. value: fresh __c var
 * with M16 content axioms instantiated — from the single channel, or for
 * select2* only the axioms BOTH channels share (same cmp, structurally equal
 * rhs): the value may come from either, so only common facts are honest. */
static Sym make_status_pair(State *st, Node *c0, Node *c1, int hi) {
    char sv[64], vv[64], pv[64];
    snprintf(sv, sizeof sv, "__c%d", g_call_ctr++);
    snprintf(vv, sizeof vv, "__c%d", g_call_ctr++);
    path_push_ge(&st->path, sv, 0);
    path_push_le(&st->path, sv, hi);
    char kbuf0[64], kbuf1[64];
    const char *key0 = resolve_key(c0, st, kbuf0, sizeof kbuf0);
    const char *key1 = c1 ? resolve_key(c1, st, kbuf1, sizeof kbuf1) : NULL;
    for (int i = 0; i < st->ax.n; i++) {
        ElemAxiom *a = &st->ax.a[i];
        if (a->is_sorted || !key0 || strcmp(a->vec, key0) != 0) continue;
        if (!c1) {
            cl_push(&st->path, axiom_instantiate(a, vv));
        } else if (key1) {
            for (int j = 0; j < st->ax.n; j++) {
                ElemAxiom *b = &st->ax.a[j];
                if (b->is_sorted || strcmp(b->vec, key1) != 0 || b->cmp != a->cmp) continue;
                if (lin_eq(&a->rhs, &b->rhs)) { cl_push(&st->path, axiom_instantiate(a, vv)); break; }
            }
        }
    }
    snprintf(pv, sizeof pv, "__q%d", g_prod_ctr++);
    reads_push_pair(&st->reads, pv, lin_var(sv), lin_var(vv));
    return sym_lin(lin_var(pv));
}

static Sym eval_par_call(Node *call, State *st, Obligations *ob) {
    const char *nm = call->callee->name;

    /* M17: pair-returning channel APIs */
    if (strncmp(nm, "chan_select2", 12) == 0 || strcmp(nm, "chan_recv2") == 0 ||
        strcmp(nm, "chan_try_recv") == 0 || strcmp(nm, "chan_recv_timeout") == 0) {
        int hi = 1;
        if (strcmp(nm, "chan_recv2") != 0)
            hi = (strncmp(nm, "chan_select2", 12) == 0) ? 3 : 2;
        Node *c1 = (strncmp(nm, "chan_select2", 12) == 0) ? call->args.data[1] : NULL;
        return make_status_pair(st, call->args.data[0], c1, hi);
    }

    if (strcmp(nm, "go") == 0 || strcmp(nm, "go_bg") == 0) {
        Node *w = call->args.data[0];
        const char *wname = w->name;
        Node *wfn = find_fn(g_prog, wname);
        Sym res;
        if (find_spec(g_prog, wname)) {
            /* synthesize `wname(arg)` and reuse M5: discharge requires
             * (kind-2 obligations), assume ensures when justified */
            Node fake; memset(&fake, 0, sizeof fake);
            fake.kind = NODE_CALL; fake.callee = w;
            NodeVec fa; fa.data = &call->args.data[1]; fa.len = 1; fa.cap = 1;
            fake.args = fa;
            res = eval_user_call(&fake, st, ob, g_caller_name);
        } else {
            /* spec-less worker: nothing to discharge or assume (honest) */
            Sym a = se_from_ast(call->args.data[1], &st->env, &st->vlen, &st->reads);
            lin_free(&a.lin);
            /* M16 drop rule: a spec-less worker may send anything on the
             * channel argument — the caller's axioms on it are dropped */
            char kbuf[64];
            const char *key = resolve_key(call->args.data[1], st, kbuf, sizeof kbuf);
            if (key) ax_drop_key(&st->ax, key);
            char rv[64]; snprintf(rv, sizeof rv, "__c%d", g_call_ctr++);
            res = sym_lin(lin_var(rv));
        }
        /* M19: classify the worker's first blocking op on the channel arg */
        HandleEntry *ch = NULL;
        {
            Sym carg = se_from_ast(call->args.data[1], &st->env, &st->vlen, &st->reads);
            const char *cv = !carg.nonlinear ? lin_handle_var(&carg.lin) : NULL;
            if (cv) {
                ch = handles_find(&st->handles, cv);
                if (ch && ch->kind != HK_CHAN) ch = NULL;
            }
            lin_free(&carg.lin);
        }
        WfScan sc; memset(&sc, 0, sizeof sc);
        if (wfn && wfn->fn_body && wfn->params.len >= 1) {
            const char *nms[16]; int nn = 0;
            nms[nn++] = wfn->params.data[0]->param_name;
            wf_scan_node(wfn->fn_body, nms, &nn, 16, &sc);
        } else if (wfn) {
            sc.complex = 1;
        }
        if (strcmp(nm, "go_bg") == 0) {   /* detached at birth: no handle */
            if (ch) {
                if (sc.complex) ch->wf_unknown = 1;
                else if (sc.send_first) ch->n_bg_prod += sc.n_send;   /* M25: summed sends */
                else if (sc.recv_first) ch->n_bg_cons++;   /* M24 credit */
            }
            lin_free(&res.lin);
            return sym_lin(lin_const(rat_int(0)));
        }
        char hv[64]; snprintf(hv, sizeof hv, "__h%d", g_handle_ctr++);
        if (res.nonlinear) { Lin empty; lin_init(&empty); handles_set(&st->handles, hv, HK_JOIN, 0, &empty); }
        else handles_set(&st->handles, hv, HK_JOIN, 0, &res.lin);   /* moves res.lin */
        {
            HandleEntry *jh = handles_find(&st->handles, hv);
            if (jh) {
                jh->worker = strdup(wname);
                if (ch) jh->chan_arg = strdup(ch->var);
                jh->wf_recv = sc.recv_first && !sc.complex;
                jh->wf_send = sc.send_first && !sc.complex;
                jh->wf_complex = sc.complex;
                jh->wf_n_send = sc.complex ? 0 : sc.n_send;
                jh->wf_n_recv = sc.complex ? 0 : sc.n_recv;
            }
            if (ch && sc.complex) ch->wf_unknown = 1;
        }
        return sym_lin(lin_var(hv));
    }

    if (strcmp(nm, "join") == 0 || strcmp(nm, "detach") == 0) {
        int is_join = (nm[0] == 'j');
        Sym hv = se_from_ast(call->args.data[0], &st->env, &st->vlen, &st->reads);
        HandleEntry *e = NULL;
        if (!hv.nonlinear && hv.lin.n == 1 && hv.lin.c.num == 0 &&
            hv.lin.terms[0].coeff.num == hv.lin.terms[0].coeff.den)
            e = handles_find(&st->handles, hv.lin.terms[0].var);
        if (!e || e->kind != HK_JOIN) {
            /* unknown handle (e.g. an i64 parameter): no protocol claims —
             * join returns an unconstrained value, detach 0 (sound) */
            lin_free(&hv.lin);
            if (!is_join) return sym_lin(lin_const(rat_int(0)));
            char rv[64]; snprintf(rv, sizeof rv, "__c%d", g_call_ctr++);
            return sym_lin(lin_var(rv));
        }
        if (e->state != 0) {
            char label[300];
            snprintf(label, sizeof label, "%s след %s по същия handle",
                     is_join ? "join" : "detach",
                     e->state == 1 ? "join" : "detach");
            push_protocol_violation(st, ob, label);
        } else {
            e->state = is_join ? 1 : 2;
            if (is_join) wf_check_join(st, ob, e);
        }
        if (!is_join) { lin_free(&hv.lin); return sym_lin(lin_const(rat_int(0))); }
        lin_free(&hv.lin);
        return sym_lin(lin_clone(&e->result));
    }

    if (strcmp(nm, "chan_new") == 0) {
        Sym cap = se_from_ast(call->args.data[0], &st->env, &st->vlen, &st->reads);
        int ccap = -1;   /* M24: constant capacity only; symbolic = unknown */
        if (!cap.nonlinear && cap.lin.n == 0 && cap.lin.c.den == 1) {
            ccap = (int)cap.lin.c.num;
            if (ccap < 1) ccap = 1;   /* runtime clamps: baga_chan_new M1 */
        }
        lin_free(&cap.lin);
        char hv[64]; snprintf(hv, sizeof hv, "__ch%d", g_handle_ctr++);
        handles_set(&st->handles, hv, HK_CHAN, 0, NULL);
        HandleEntry *e = handles_find(&st->handles, hv);
        if (e) e->wf_cap = ccap;
        return sym_lin(lin_var(hv));
    }

    /* chan_send / chan_recv / chan_close */
    Sym cv = se_from_ast(call->args.data[0], &st->env, &st->vlen, &st->reads);
    HandleEntry *e = NULL;
    if (!cv.nonlinear && cv.lin.n == 1 && cv.lin.c.num == 0 &&
        cv.lin.terms[0].coeff.num == cv.lin.terms[0].coeff.den)
        e = handles_find(&st->handles, cv.lin.terms[0].var);
    if (e && e->kind != HK_CHAN) e = NULL;   /* not a channel: no claims */
    lin_free(&cv.lin);

    if (strcmp(nm, "chan_close") == 0) {
        if (e) e->state = 1;   /* idempotent */
        return sym_lin(lin_const(rat_int(0)));
    }
    if (strcmp(nm, "chan_recv") == 0) {
        char rv[64]; snprintf(rv, sizeof rv, "__c%d", g_call_ctr++);
        /* M16: a received payload provably satisfies every content axiom
         * anchored on this channel */
        char kbuf[64];
        const char *key = resolve_key(call->args.data[0], st, kbuf, sizeof kbuf);
        if (key)
            for (int i = 0; i < st->ax.n; i++) {
                if (st->ax.a[i].is_sorted || strcmp(st->ax.a[i].vec, key) != 0) continue;
                cl_push(&st->path, axiom_instantiate(&st->ax.a[i], rv));
            }
        wf_check_recv(st, ob, e);   /* M19: wait-for */
        return sym_lin(lin_var(rv));   /* otherwise unconstrained payload */
    }
    /* chan_send */
    if (e) { wf_check_send(st, ob, e); e->n_send++; }   /* M24: full-buffer */
    Sym v = se_from_ast(call->args.data[1], &st->env, &st->vlen, &st->reads);
    /* M16: the payload must provably satisfy every content axiom anchored on
     * this channel; an axiom that cannot be discharged is dropped (M3 rule —
     * downstream receives then honestly get nothing) */
    {
        char kbuf[64];
        const char *key = resolve_key(call->args.data[0], st, kbuf, sizeof kbuf);
        if (key && st->ax.n > 0) {
            AxiomList kept; ax_init(&kept);
            for (int i = 0; i < st->ax.n; i++) {
                if (!st->ax.a[i].is_sorted && strcmp(st->ax.a[i].vec, key) == 0) {
                    if (axiom_holds_for_value(&st->ax.a[i], &v, st))
                        ax_push(&kept, st->ax.a[i].vec, st->ax.a[i].cmp, lin_clone(&st->ax.a[i].rhs), 0);
                    /* else: dropped (sound) */
                } else {
                    ax_push(&kept, st->ax.a[i].vec, st->ax.a[i].cmp, lin_clone(&st->ax.a[i].rhs), st->ax.a[i].is_sorted);
                }
            }
            ax_free(&st->ax);
            st->ax = kept;
        }
    }
    lin_free(&v.lin);
    char rv[64]; snprintf(rv, sizeof rv, "__c%d", g_call_ctr++);
    if (e && e->state == 1) {
        /* known closed: send definitely fails → s == -1 */
        Lin s = lin_var(rv);
        cl_push(&st->path, mk_cons(lin_clone(&s), C_LE, rat_int(-1)));   /* s <= -1 */
        Lin neg = lin_neg(&s);
        cl_push(&st->path, mk_cons(neg, C_LE, rat_int(1)));              /* -s <= 1 */
        lin_free(&s);
    } else {
        path_push_ge(&st->path, rv, -1);   /* s >= -1 */
        path_push_le(&st->path, rv, 0);    /* s <= 0  */
    }
    return sym_lin(lin_var(rv));
}

/* MEM-2: drop(x) — consume the alloc→drop protocol state of an owned buffer
 * (Vec/Map/bytes/fn). Keyed by the SOURCE variable name, not by a Lin term:
 * these values are opaque (nonlinear) in the scalar machinery. A second drop
 * on a live path is a kind-3 protocol violation (REFUTED with the path
 * witness); on a dead path it is vacuously PROVEN, which is correct. An
 * untracked value (e.g. a fn-typed local, which is never registered) makes
 * no claims — the checker has already validated the type. */
static void eval_drop_call(Node *call, State *st, Obligations *ob) {
    Node *a = call->args.data[0];
    HandleEntry *e = handles_find(&st->handles, a->name);
    if (!e || e->kind != HK_DROP) return;   /* untracked: no claims */
    if (e->state != 0) {
        char label[300];
        snprintf(label, sizeof label, "повторен drop на '%s'", a->name);
        push_protocol_violation(st, ob, label);
    } else {
        e->state = 1;
    }
}

/* MEM-2: flag ident USES of an already-dropped buffer anywhere in a
 * statement-level expression (call args, operands). `seen` dedups per
 * statement, so one statement yields at most one violation per variable. */
static void check_drop_uses_rec(Node *e, State *st, Obligations *ob, const char **seen, int *nseen) {
    if (!e) return;
    switch (e->kind) {
    case NODE_IDENT: {
        HandleEntry *h = handles_find(&st->handles, e->name);
        if (!h || h->kind != HK_DROP || h->state != 1) return;
        for (int k = 0; k < *nseen; k++) if (strcmp(seen[k], e->name) == 0) return;
        if (*nseen < 16) seen[(*nseen)++] = e->name;
        char label[300];
        snprintf(label, sizeof label, "използване след drop на '%s'", e->name);
        push_protocol_violation(st, ob, label);
        return;
    }
    case NODE_CALL:
        if (is_drop_call(e)) return;   /* drop consumes the buffer; not a "use" */
        for (int i = 0; i < e->args.len; i++) check_drop_uses_rec(e->args.data[i], st, ob, seen, nseen);
        return;
    case NODE_BINARY:
        check_drop_uses_rec(e->left, st, ob, seen, nseen);
        check_drop_uses_rec(e->right, st, ob, seen, nseen);
        return;
    case NODE_UNARY:
        check_drop_uses_rec(e->operand, st, ob, seen, nseen);
        return;
    default: return;   /* INDEX/FIELD/... are outside the fragment (gated) */
    }
}
static void check_drop_uses(Node *e, State *st, Obligations *ob) {
    const char *seen[16]; int nseen = 0;
    check_drop_uses_rec(e, st, ob, seen, &nseen);
}

/* ======================= M15: arithmetic safety =======================
 * The verifier reasons in idealized ℤ; the runtime is i64. This bridge
 * emits one kind-4 obligation per arithmetic operation: prove the result
 * cannot overflow i64 on this path (and no division by zero / INT64_MIN/-1),
 * refute it with a concrete witness, or stay honestly UNKNOWN. When every
 * arith obligation of a function is PROVEN, the idealized model and the
 * runtime coincide — the ensures verdicts become unconditional. */

#define AK_FIT  1   /* aux1 (linear result form) must fit i64 */
#define AK_MUL  2   /* aux1 * aux2 (factor forms) must fit i64 */
#define AK_DIVZ 3   /* aux2 (divisor) != 0; not (aux1 = INT64_MIN && aux2 = -1) */

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
static void scan_arith_expr(Node *e, State *st, Obligations *ob) {
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
static void push_term_entry_obl(Node *fn, Node *spec, State *init, Obligations *ob) {
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
static void symexec_stmts(NodeVec *stmts, States *states, Obligations *ob, int is_nonvoid, Node *spec);

static void symexec_block(Node *blk, States *states, Obligations *ob, int is_nonvoid, Node *spec) {
    if (!blk) return;
    if (blk->kind == NODE_BLOCK) { symexec_stmts(&blk->stmts, states, ob, is_nonvoid, spec); return; }
    /* single-statement branch */
    NodeVec one; one.data = &blk; one.len = 1; one.cap = 1;
    symexec_stmts(&one, states, ob, is_nonvoid, spec);
}

static State clone_state_with(State *cur, Formula *add) {
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

static void drain_returns(States *states, Obligations *ob, Node *val_expr) {
    for (int i = 0; i < states->n; i++) {
        Sym r = se_from_ast(val_expr, &states->s[i].env, &states->s[i].vlen, &states->s[i].reads);
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

static void symexec_stmts(NodeVec *stmts, States *states, Obligations *ob, int is_nonvoid, Node *spec) {
    for (int si = 0; si < stmts->len; si++) {
        Node *st = stmts->data[si];
        int last = (si == stmts->len - 1);
        if (st->kind == NODE_LET) {
            for (int i = 0; i < states->n; i++) {
                scan_arith_expr(st->let_init, &states->s[i], ob);   /* M15 */
                check_drop_uses(st->let_init, &states->s[i], ob);   /* MEM-2 */
                if (is_user_call(st->let_init)) {   /* M5: assume-guarantee */
                    Sym r = eval_user_call(st->let_init, &states->s[i], ob, g_caller_name);
                    bind_sym(&states->s[i].env, st->let_name, r);
                    continue;
                }
                if (is_par_builtin_call(st->let_init)) {   /* M14: !Par */
                    Sym r = eval_par_call(st->let_init, &states->s[i], ob);
                    bind_sym(&states->s[i].env, st->let_name, r);
                    continue;
                }
                bind_sym(&states->s[i].env, st->let_name, se_from_ast(st->let_init, &states->s[i].env, &states->s[i].vlen, &states->s[i].reads));
                /* MEM-2: a freshly-allocated buffer is live — start the
                 * alloc→drop protocol for this source variable */
                if (is_mem_alloc_call(st->let_init))
                    handles_set(&states->s[i].handles, st->let_name, HK_DROP, 0, NULL);
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

/* ============================ top-level verification ============================ */

static Node *find_spec(Node *prog, const char *name) {
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
static int type_is_i64(Node *t) {
    t = unwrap_type(t);
    return t && t->kind == NODE_TYPE && t->type_name && strcmp(t->type_name, "i64") == 0;
}
static int ret_has_effects(Node *t) {
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
static int ret_has_unverifiable_effects(Node *t) {
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

typedef enum { R_PROVEN, R_REFUTED, R_UNKNOWN } VRes;

static const char *res_word(VRes r) {
    switch (r) { case R_PROVEN: return "ДОКАЗАНО"; case R_REFUTED: return "ОБРОЧЕНО"; default: return "НЕ МОГА ДА РЕША"; }
}

/* ---- JSON output mode (--verify --json) ----
 * Machine-readable verdicts so AI agents / CI can consume the judge
 * without parsing Bulgarian prose. The exit code is unchanged:
 * 0 = nothing refuted, 1 = at least one refuted. */
static int g_json = 0;
static int g_json_first = 1;

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
static int has_elem_ref(Node *e) {
    if (!e) return 0;
    if (e->kind == NODE_ELEM_REF) return 1;
    if (is_sorted_call(e)) return 1;   /* relational axiom, not a scalar inv */
    if (e->kind == NODE_BINARY) return has_elem_ref(e->left) || has_elem_ref(e->right);
    if (e->kind == NODE_UNARY) return has_elem_ref(e->operand);
    return 0;
}

/* Recognize `v[*] <cmp> <linear>` (or mirrored) and add it as an axiom.
 * Also recognizes the relational predicate `sorted(v)`. */
static void extract_elem_axiom(Node *e, SEnv *env, AxiomList *ax) {
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
static Constraint axiom_instantiate(ElemAxiom *a, const char *read_var) {
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
static int axiom_holds_for_value(ElemAxiom *a, Sym *val, State *st) {
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
