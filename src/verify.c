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
    if (a < 0) a = -a;
    if (b < 0) b = -b;
    while (b) { int64_t t = a % b; a = b; b = t; }
    return a ? a : 1;
}

static Rat rat_bad(void) { Rat r; r.num = 0; r.den = 1; r.overflow = 1; return r; }

static Rat rat_mk(int64_t n, int64_t d) {
    if (d == 0) return rat_bad();
    if (d < 0) { n = -n; d = -d; }
    int64_t g = v_gcd(n < 0 ? -n : n, d);
    Rat r; r.num = n / g; r.den = d / g; r.overflow = 0;
    return r;
}
static Rat rat_int(int64_t n) { Rat r; r.num = n; r.den = 1; r.overflow = 0; return r; }
static Rat rat_zero(void) { return rat_int(0); }

static Rat rat_neg(Rat a) { if (a.overflow) return rat_bad(); Rat r = a; r.num = -r.num; return r; }

static Rat rat_add(Rat a, Rat b) {
    if (a.overflow || b.overflow || a.den == 0 || b.den == 0) return rat_bad();
    int64_t g = v_gcd(a.den, b.den);
    int64_t l1 = a.den / g, l2 = b.den / g;
    int64_t an = a.num < 0 ? -a.num : a.num;
    int64_t bn = b.num < 0 ? -b.num : b.num;
    if (l2 != 0 && an > INT64_MAX / l2) return rat_bad();
    if (l1 != 0 && bn > INT64_MAX / l1) return rat_bad();
    if (l2 != 0 && a.den > INT64_MAX / l2) return rat_bad();
    return rat_mk(a.num * l2 + b.num * l1, a.den * l2);
}
static Rat rat_sub(Rat a, Rat b) { return rat_add(a, rat_neg(b)); }
static Rat rat_mul(Rat a, Rat b) {
    if (a.overflow || b.overflow || a.den == 0 || b.den == 0) return rat_bad();
    int64_t an = a.num < 0 ? -a.num : a.num;
    int64_t bn = b.num < 0 ? -b.num : b.num;
    if (bn != 0 && an > INT64_MAX / bn) return rat_bad();
    if (b.den != 0 && a.den > INT64_MAX / b.den) return rat_bad();
    return rat_mk(a.num * b.num, a.den * b.den);
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
 * M13: n & 1 (is_bitand1): residue in {0,1} over two's complement — NOT C % 2
 * for negative n (where % truncates toward zero). */
typedef struct {
    Node *call; char *var; Lin fa, fb;
    int is_prod; int is_div; int is_mod; int is_bitand1;
} ReadEntry;
typedef struct { ReadEntry *r; int n, cap; } ReadsList;
typedef struct { char *var; Lin fa, fb; int is_div; int is_mod; int is_bitand1; } ProdEntry;
typedef struct { ProdEntry *p; int n, cap; } ProdList;
static const char *reads_find(ReadsList *l, Node *call);
static void reads_push_prod(ReadsList *l, const char *var, Lin fa, Lin fb);
static void reads_push_div(ReadsList *l, const char *var, Lin fa, Lin fb);
static void reads_push_mod(ReadsList *l, const char *var, Lin fa, Lin fb);
static void reads_push_bitand1(ReadsList *l, const char *var, Lin fa);
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
        /* M13: bitwise / shifts with sound special cases (no full BV theory) */
        if (e->bin_op == OP_BIT_OR || e->bin_op == OP_BIT_AND || e->bin_op == OP_BIT_XOR) {
            if (l.nonlinear || r.nonlinear) return sym_nonlin();
            /* n | 0 = n; n & 0 = 0; n ^ 0 = n; n ^ n = 0; n & -1 = n */
            if (e->bin_op == OP_BIT_OR && lin_is_const(&r.lin) && r.lin.c.num == 0 && r.lin.c.den == 1)
                { lin_free(&r.lin); return sym_lin(l.lin); }
            if (e->bin_op == OP_BIT_OR && lin_is_const(&l.lin) && l.lin.c.num == 0 && l.lin.c.den == 1)
                { lin_free(&l.lin); return sym_lin(r.lin); }
            if (e->bin_op == OP_BIT_AND && lin_is_const(&r.lin) && r.lin.c.num == 0 && r.lin.c.den == 1)
                { lin_free(&l.lin); lin_free(&r.lin); return sym_lin(lin_const(rat_int(0))); }
            if (e->bin_op == OP_BIT_AND && lin_is_const(&l.lin) && l.lin.c.num == 0 && l.lin.c.den == 1)
                { lin_free(&l.lin); lin_free(&r.lin); return sym_lin(lin_const(rat_int(0))); }
            if (e->bin_op == OP_BIT_AND && lin_is_const(&r.lin) && r.lin.c.num == -1 && r.lin.c.den == 1)
                { lin_free(&r.lin); return sym_lin(l.lin); }
            if (e->bin_op == OP_BIT_AND && lin_is_const(&l.lin) && l.lin.c.num == -1 && l.lin.c.den == 1)
                { lin_free(&l.lin); return sym_lin(r.lin); }
            if (e->bin_op == OP_BIT_XOR && lin_is_const(&r.lin) && r.lin.c.num == 0 && r.lin.c.den == 1)
                { lin_free(&r.lin); return sym_lin(l.lin); }
            if (e->bin_op == OP_BIT_XOR && lin_is_const(&l.lin) && l.lin.c.num == 0 && l.lin.c.den == 1)
                { lin_free(&l.lin); return sym_lin(r.lin); }
            if (e->bin_op == OP_BIT_XOR && lin_eq(&l.lin, &r.lin))
                { lin_free(&l.lin); lin_free(&r.lin); return sym_lin(lin_const(rat_int(0))); }
            /* n & 1 → bit residue in {0,1} (two's complement). Not C % 2. */
            if (e->bin_op == OP_BIT_AND && reads &&
                ((lin_is_const(&r.lin) && r.lin.c.den == 1 && r.lin.c.num == 1) ||
                 (lin_is_const(&l.lin) && l.lin.c.den == 1 && l.lin.c.num == 1))) {
                Lin nlin = (lin_is_const(&r.lin) && r.lin.c.num == 1) ? l.lin : r.lin;
                char mv[64]; snprintf(mv, sizeof mv, "__b%d", g_prod_ctr++);
                if (lin_is_const(&r.lin) && r.lin.c.num == 1) lin_free(&r.lin);
                else lin_free(&l.lin);
                reads_push_bitand1(reads, mv, nlin);
                return sym_lin(lin_var(mv));
            }
            lin_free(&l.lin); lin_free(&r.lin);
            return sym_nonlin();
        }
        if (e->bin_op == OP_LSHIFT || e->bin_op == OP_RSHIFT) {
            if (l.nonlinear || r.nonlinear) return sym_nonlin();
            /* n << k: exact scale by 2^k for k∈[0,62] (overflow treated as wrap-free ℤ).
             * n >> k: only as trunc div by 2^k when reads available; inject axioms
             * for nonnegative dividends (matches C >> on n>=0). Negative n → weak. */
            if (lin_is_const(&r.lin) && r.lin.c.den == 1 && r.lin.c.num >= 0 && r.lin.c.num <= 62) {
                int64_t k = r.lin.c.num;
                int64_t pow2 = 1LL << k;
                lin_free(&r.lin);
                if (e->bin_op == OP_LSHIFT) {
                    return sym_lin(lin_scale(&l.lin, rat_int(pow2)));
                }
                if (reads) {
                    Lin d = lin_const(rat_int(pow2));
                    char dv[64]; snprintf(dv, sizeof dv, "__d%d", g_prod_ctr++);
                    reads_push_div(reads, dv, l.lin, d);
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

/* M8/M9/M13: pin every non-abstract var (__c/__p/__d/__m/__b excluded) to its
 * candidate value in sys; then derive every product/div/mod/bit var whose
 * factors are fully pinned and pin it too. Two passes so products of products
 * resolve. */
static void push_pins_and_derived(ConsList *sys, char **vars, IBind *assign, int nv, ProdList *prods) {
    int pcap = nv + (prods ? prods->n : 0) + 1;
    IBind *pins = calloc((size_t)pcap, sizeof(IBind));
    int np = 0;
    for (int i = 0; i < nv; i++) {
        if (strncmp(vars[i], "__c", 3) == 0 || strncmp(vars[i], "__p", 3) == 0 ||
            strncmp(vars[i], "__d", 3) == 0 || strncmp(vars[i], "__m", 3) == 0 ||
            strncmp(vars[i], "__b", 3) == 0) continue;
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
            int64_t pvv;
            if (!ok) continue;
            if (prods->p[pi].is_bitand1) {
                pvv = fv[0] & 1;   /* two's-complement LSB */
            } else if (prods->p[pi].is_div) {
                if (fv[1] == 0) continue;
                pvv = fv[0] / fv[1];   /* C trunc toward zero */
            } else if (prods->p[pi].is_mod) {
                if (fv[1] == 0) continue;
                pvv = fv[0] % fv[1];
            } else {
                if (__builtin_mul_overflow(fv[0], fv[1], &pvv)) continue;
            }
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
typedef struct { ConsList path; ConsList bound; ConsList read_cons; Sym ret; VLenMap vlen; ProdList prods; int bad; int kind; char *label; } Obligation;
typedef struct { Obligation *o; int n, cap; } Obligations;
static void obl_push(Obligations *ob, ConsList path, ConsList bound, ConsList read_cons, Sym ret, VLenMap *vlen, ReadsList *reads, int bad, int kind, const char *label) {
    if (ob->n == ob->cap) { ob->cap = ob->cap ? ob->cap * 2 : 4; ob->o = realloc(ob->o, (size_t)ob->cap * sizeof(Obligation)); }
    ob->o[ob->n].path = path; ob->o[ob->n].bound = bound; ob->o[ob->n].read_cons = read_cons; ob->o[ob->n].ret = ret;
    if (vlen) vlen_clone_into(&ob->o[ob->n].vlen, vlen); else vlen_init(&ob->o[ob->n].vlen);
    if (reads) prods_clone_into(&ob->o[ob->n].prods, reads);
    else { ob->o[ob->n].prods.p = NULL; ob->o[ob->n].prods.n = 0; ob->o[ob->n].prods.cap = 0; }
    ob->o[ob->n].bad = bad;
    ob->o[ob->n].kind = kind; ob->o[ob->n].label = label ? strdup(label) : NULL; ob->n++;
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
    for (int i = 0; i < l->n; i++) { free(l->r[i].var); lin_free(&l->r[i].fa); lin_free(&l->r[i].fb); }
    free(l->r); l->r = NULL; l->n = l->cap = 0;
}
static void reads_push(ReadsList *l, Node *call, const char *var) {
    if (l->n == l->cap) { l->cap = l->cap ? l->cap * 2 : 4; l->r = realloc(l->r, (size_t)l->cap * sizeof(ReadEntry)); }
    l->r[l->n].call = call; l->r[l->n].var = strdup(var);
    lin_init(&l->r[l->n].fa); lin_init(&l->r[l->n].fb);
    l->r[l->n].is_prod = 0; l->r[l->n].is_div = 0; l->r[l->n].is_mod = 0; l->r[l->n].is_bitand1 = 0;
    l->n++;
}
/* M8b: register a product var with its factor linear forms (moves fa/fb). */
static void reads_push_prod(ReadsList *l, const char *var, Lin fa, Lin fb) {
    if (l->n == l->cap) { l->cap = l->cap ? l->cap * 2 : 4; l->r = realloc(l->r, (size_t)l->cap * sizeof(ReadEntry)); }
    l->r[l->n].call = NULL; l->r[l->n].var = strdup(var);
    l->r[l->n].fa = fa; l->r[l->n].fb = fb;
    l->r[l->n].is_prod = 1; l->r[l->n].is_div = 0; l->r[l->n].is_mod = 0; l->r[l->n].is_bitand1 = 0;
    l->n++;
}
/* M9: register integer division by a constant (moves fa; fb is the const divisor). */
static void reads_push_div(ReadsList *l, const char *var, Lin fa, Lin fb) {
    if (l->n == l->cap) { l->cap = l->cap ? l->cap * 2 : 4; l->r = realloc(l->r, (size_t)l->cap * sizeof(ReadEntry)); }
    l->r[l->n].call = NULL; l->r[l->n].var = strdup(var);
    l->r[l->n].fa = fa; l->r[l->n].fb = fb;
    l->r[l->n].is_prod = 0; l->r[l->n].is_div = 1; l->r[l->n].is_mod = 0; l->r[l->n].is_bitand1 = 0;
    l->n++;
}
/* M9b: register integer mod by a constant. */
static void reads_push_mod(ReadsList *l, const char *var, Lin fa, Lin fb) {
    if (l->n == l->cap) { l->cap = l->cap ? l->cap * 2 : 4; l->r = realloc(l->r, (size_t)l->cap * sizeof(ReadEntry)); }
    l->r[l->n].call = NULL; l->r[l->n].var = strdup(var);
    l->r[l->n].fa = fa; l->r[l->n].fb = fb;
    l->r[l->n].is_prod = 0; l->r[l->n].is_div = 0; l->r[l->n].is_mod = 1; l->r[l->n].is_bitand1 = 0;
    l->n++;
}
/* M13: n & 1 → fresh bit residue (moves fa). Always 0..1; not C % 2. */
static void reads_push_bitand1(ReadsList *l, const char *var, Lin fa) {
    if (l->n == l->cap) { l->cap = l->cap ? l->cap * 2 : 4; l->r = realloc(l->r, (size_t)l->cap * sizeof(ReadEntry)); }
    l->r[l->n].call = NULL; l->r[l->n].var = strdup(var);
    l->r[l->n].fa = fa; lin_init(&l->r[l->n].fb); l->r[l->n].fb = lin_const(rat_int(2));
    l->r[l->n].is_prod = 0; l->r[l->n].is_div = 0; l->r[l->n].is_mod = 0; l->r[l->n].is_bitand1 = 1;
    l->n++;
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
        else if (src->r[i].is_bitand1)
            reads_push_bitand1(dst, src->r[i].var, lin_clone(&src->r[i].fa));
        else
            reads_push(dst, src->r[i].call, src->r[i].var);
    }
}

/* Snapshot product/div/mod/bit entries of a reads list (for obligation witnesses). */
static void prods_clone_into(ProdList *dst, ReadsList *src) {
    dst->p = NULL; dst->n = 0; dst->cap = 0;
    for (int i = 0; i < src->n; i++) {
        if (!src->r[i].is_prod && !src->r[i].is_div && !src->r[i].is_mod && !src->r[i].is_bitand1) continue;
        if (dst->n == dst->cap) { dst->cap = dst->cap ? dst->cap * 2 : 4; dst->p = realloc(dst->p, (size_t)dst->cap * sizeof(ProdEntry)); }
        dst->p[dst->n].var = strdup(src->r[i].var);
        dst->p[dst->n].fa = lin_clone(&src->r[i].fa);
        dst->p[dst->n].fb = lin_clone(&src->r[i].fb);
        dst->p[dst->n].is_div = src->r[i].is_div;
        dst->p[dst->n].is_mod = src->r[i].is_mod;
        dst->p[dst->n].is_bitand1 = src->r[i].is_bitand1;
        dst->n++;
    }
}
static void prods_free(ProdList *l) {
    for (int i = 0; i < l->n; i++) { free(l->p[i].var); lin_free(&l->p[i].fa); lin_free(&l->p[i].fb); }
    free(l->p); l->p = NULL; l->n = l->cap = 0;
}

typedef struct { SEnv env; VLenMap vlen; AxiomList ax; ConsList path; ConsList read_cons; ReadsList reads; int bad; } State;

static void state_free(State *s) { env_free(&s->env); vlen_free(&s->vlen); ax_free(&s->ax); cl_free(&s->path); cl_free(&s->read_cons); reads_free(&s->reads); }

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
static int g_partial = 0;        /* direct self-recursion seen during symexec */
static int g_term = 0;           /* spec carries a decreases measure (M6) */
static int g_term_failed = 0;    /* some termination obligation is not PROVEN */

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
        if (is_vec_builtin_call(n)) {
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
    default: return 0;
    }
}

static int has_unsupported(Node *n) { return has_unsupported_rec(n, 0, 0); }

static State clone_state_with(State *cur, Formula *add);
static void inject_prod_axioms(State *st);
static void symexec_block(Node *blk, States *states, Obligations *ob, int is_nonvoid, Node *spec);

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
static int symexec_while(Node *st, States *states, Obligations *ob, int is_nonvoid, Node *spec) {
    (void)is_nonvoid;   /* the loop body is never a return context */
    int trusted = 1;
    States out; out.s = NULL; out.n = 0; out.cap = 0;
    for (int i = 0; i < states->n; i++) {
        State *cur = &states->s[i];

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
        env_free(&cur->env); cl_free(&cur->path);
    }
    free(states->s);
    *states = out;
    /* M3: element axioms declared in the loop invariant hold after the loop
     * (the invariant is assumed established & preserved by the contract).
     * while_invariants stores the raw expression nodes (not NODE_ENSURE). */
    for (int i = 0; i < states->n; i++)
        for (int j = 0; j < st->while_invariants.len; j++)
            extract_elem_axiom(st->while_invariants.data[j], &states->s[i].env, &states->s[i].ax);
    return trusted;
}

/* Track Vec lengths and emit bounds obligations for vec accesses in an
 * expression statement. Only the top-level call and a let-init call are
 * handled; nested vec calls inside larger expressions are not tracked. */
static Constraint axiom_instantiate(ElemAxiom *a, const char *read_var);
static int axiom_holds_for_value(ElemAxiom *a, Sym *val, State *st);
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
        /* M13: n & 1 ∈ {0,1} over two's complement for every n (not C % 2). */
        if (e->is_bitand1) {
            path_push_ge(&st->path, e->var, 0);
            path_push_le(&st->path, e->var, 1);
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
    }
    states->n = 0;
}

static void symexec_stmts(NodeVec *stmts, States *states, Obligations *ob, int is_nonvoid, Node *spec) {
    for (int si = 0; si < stmts->len; si++) {
        Node *st = stmts->data[si];
        int last = (si == stmts->len - 1);
        if (st->kind == NODE_LET) {
            for (int i = 0; i < states->n; i++) {
                if (is_user_call(st->let_init)) {   /* M5: assume-guarantee */
                    Sym r = eval_user_call(st->let_init, &states->s[i], ob, g_caller_name);
                    bind_sym(&states->s[i].env, st->let_name, r);
                    continue;
                }
                bind_sym(&states->s[i].env, st->let_name, se_from_ast(st->let_init, &states->s[i].env, &states->s[i].vlen, &states->s[i].reads));
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
            for (int i = 0; i < states->n; i++)
                bind_sym(&states->s[i].env, st->assign_target->name, se_from_ast(st->assign_val, &states->s[i].env, &states->s[i].vlen, &states->s[i].reads));
        } else if (st->kind == NODE_RETURN) {
            if (is_user_call(st->ret_val)) {   /* M5 */
                for (int i = 0; i < states->n; i++) {
                    Sym r = eval_user_call(st->ret_val, &states->s[i], ob, g_caller_name);
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
            if (is_user_call(st->expr)) {   /* M5 */
                for (int i = 0; i < states->n; i++) {
                    Sym r = eval_user_call(st->expr, &states->s[i], ob, g_caller_name);
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
                if (is_user_call(st->expr)) {   /* M5: check requires, discard result */
                    Sym r = eval_user_call(st->expr, &states->s[i], ob, g_caller_name);
                    lin_free(&r.lin);
                    continue;
                }
                scan_vec_expr(st->expr, &states->s[i], ob, spec);
            }
        } else if (st->kind == NODE_IF) {
            States out; out.s = NULL; out.n = 0; out.cap = 0;
            for (int i = 0; i < states->n; i++) {
                State *cur = &states->s[i];
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
                env_free(&cur->env); cl_free(&cur->path);
            }
            free(states->s);
            *states = out;
        } else if (st->kind == NODE_WHILE) {
            symexec_while(st, states, ob, is_nonvoid, spec);
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
    g_prod_ctr = 0;
    g_term = 0;
    g_term_failed = 0;
    Node *spec = find_spec(prog, fn->fn_name);
    if (!spec) return -1;
    if (spec->spec_decreases) g_term = 1;

    /* fragment gates */
    const char *skip = NULL;
    if (ret_has_effects(fn->ret_type)) skip = "ефекти";
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
    cl_init(&init.path); cl_init(&init.read_cons); reads_init(&init.reads); init.bad = 0;
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
    /* bounds obligations (M2) — check but don't store in EnsVerifyRes */
    for (int i = 0; i < ob.n; i++) {
        if (ob.o[i].kind != 1) continue;
        IBind *wit = NULL; int wn = 0;
        verify_bound_obl(&ob.o[i], &wit, &wn);
        if (wit) { for (int k = 0; k < wn; k++) free(wit[k].name); free(wit); }
    }
    for (int i = 0; i < ob.n; i++) { cl_free(&ob.o[i].path); cl_free(&ob.o[i].bound); cl_free(&ob.o[i].read_cons); lin_free(&ob.o[i].ret.lin); vlen_free(&ob.o[i].vlen); prods_free(&ob.o[i].prods); free(ob.o[i].label); }
    free(ob.o);
    return 0;
}

void fn_verify_res_free(FnVerifyRes *r) {
    if (!r->ens) return;
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
        cl_init(&init2.path); cl_init(&init2.read_cons); reads_init(&init2.reads); init2.bad = 0;
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
                    printf("\n");
                }
            }
            if (r == R_REFUTED) any_refuted = 1;
            if (wit) { for (int k = 0; k < wn; k++) free(wit[k].name); free(wit); }
        }
        if (g_json) printf("]}");
        for (int i = 0; i < ob2.n; i++) { cl_free(&ob2.o[i].path); cl_free(&ob2.o[i].bound); cl_free(&ob2.o[i].read_cons); lin_free(&ob2.o[i].ret.lin); vlen_free(&ob2.o[i].vlen); prods_free(&ob2.o[i].prods); free(ob2.o[i].label); }
        free(ob2.o);
    } else if (g_json) {
        printf(", \"calls\": [], \"bounds\": []}");
    }
    fn_verify_res_free(&res);
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
