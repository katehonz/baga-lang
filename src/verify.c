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

static Sym se_from_ast(Node *e, SEnv *env, VLenMap *vlen) {
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
        if (e->un_op == UOP_NEG) { Sym v = se_from_ast(e->operand, env, vlen); if (v.nonlinear) return v; return sym_lin(lin_neg(&v.lin)); }
        return sym_nonlin();
    case NODE_CALL:
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
        Sym l = se_from_ast(e->left, env, vlen);
        Sym r = se_from_ast(e->right, env, vlen);
        if (e->bin_op == OP_ADD) { if (l.nonlinear || r.nonlinear) return sym_nonlin(); return sym_lin(lin_add(&l.lin, &r.lin)); }
        if (e->bin_op == OP_SUB) { if (l.nonlinear || r.nonlinear) return sym_nonlin(); return sym_lin(lin_sub(&l.lin, &r.lin)); }
        if (e->bin_op == OP_MUL) {
            int lc = lin_is_const(&l.lin), rc = lin_is_const(&r.lin);
            if (!l.nonlinear && !r.nonlinear && lc && rc) return sym_lin(lin_const(rat_mul(l.lin.c, r.lin.c)));
            if (!l.nonlinear && !r.nonlinear && lc) return sym_lin(lin_scale(&r.lin, l.lin.c));
            if (!l.nonlinear && !r.nonlinear && rc) return sym_lin(lin_scale(&l.lin, r.lin.c));
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
static int bool_to_dnf(Node *e, SEnv *env, VLenMap *vlen, int negated, Formula *out);

static int cmp_to_formula(Node *e, SEnv *env, VLenMap *vlen, int negated, Formula *out) {
    /* se_from_ast resolves identifiers through env (and vec_len through vlen),
     * so `output`, let-bound locals, and vector lengths are substituted by
     * their symbolic values before the comparison becomes a constraint. */
    Sym l = se_from_ast(e->left, env, vlen);
    Sym r = se_from_ast(e->right, env, vlen);
    if (l.nonlinear || r.nonlinear) { if (!l.nonlinear) lin_free(&l.lin); if (!r.nonlinear) lin_free(&r.lin); return 0; }
    Constraint c;
    if (!atom_to_cons(l.lin, e->bin_op, r.lin, negated, &c)) { lin_free(&l.lin); lin_free(&r.lin); return 0; }
    lin_free(&l.lin); lin_free(&r.lin);
    ConsList cl; cl_init(&cl); cl_push(&cl, c);
    f_init(out); f_push_branch(out, cl);
    return 1;
}

static int bool_to_dnf(Node *e, SEnv *env, VLenMap *vlen, int negated, Formula *out) {
    if (!e) return 0;
    if (e->kind == NODE_BOOL_LIT) {
        int v = e->bool_val;
        if (negated) v = !v;
        f_init(out);
        if (v) { ConsList cl; cl_init(&cl); f_push_branch(out, cl); }   /* TRUE: one empty branch */
        /* FALSE: zero branches */
        return 1;
    }
    if (e->kind == NODE_UNARY && e->un_op == UOP_NOT)
        return bool_to_dnf(e->operand, env, vlen, !negated, out);
    if (e->kind == NODE_BINARY && is_cmp(e->bin_op))
        return cmp_to_formula(e, env, vlen, negated, out);
    if (e->kind == NODE_BINARY && e->bin_op == OP_EQ && !negated) {
        /* a==b -> (a<=b)&&(a>=b) */
        Node l = *e, r = *e;
        l.bin_op = OP_LE; r.bin_op = OP_GE;
        Formula a, b;
        if (!bool_to_dnf(&l, env, vlen, 0, &a)) return 0;
        if (!bool_to_dnf(&r, env, vlen, 0, &b)) { f_free(&a); return 0; }
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
    if (e->kind == NODE_BINARY && (e->bin_op == OP_AND || e->bin_op == OP_OR)) {
        Formula L, R;
        if (!bool_to_dnf(e->left, env, vlen, negated, &L)) return 0;
        if (!bool_to_dnf(e->right, env, vlen, negated, &R)) { f_free(&L); return 0; }
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

/* Try to find an integral witness satisfying all `ante` and violating `ens`.
 * Returns 1 and fills witness (caller frees names) on success. */
static int find_counterexample(ConsList *ante, Formula *nef, Node *ensures_ast,
                               SEnv *output_env, IBind **witness, int *wn) {
    char **vars = NULL; int nv = 0, cap = 0;
    collect_vars_cl(ante, &vars, &nv, &cap);
    for (int b = 0; b < nef->n; b++) collect_vars_cl(&nef->br[b], &vars, &nv, &cap);
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
                *witness = calloc(nv ? nv : 1, sizeof(IBind));
                for (int i = 0; i < nv; i++) { (*witness)[i].name = strdup(vars[i]); (*witness)[i].val = assign[i].val; }
                *wn = nv;
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
typedef struct { ConsList path; ConsList bound; Sym ret; VLenMap vlen; int bad; int kind; char *label; } Obligation;
typedef struct { Obligation *o; int n, cap; } Obligations;
static void obl_push(Obligations *ob, ConsList path, ConsList bound, Sym ret, VLenMap *vlen, int bad, int kind, const char *label) {
    if (ob->n == ob->cap) { ob->cap = ob->cap ? ob->cap * 2 : 4; ob->o = realloc(ob->o, (size_t)ob->cap * sizeof(Obligation)); }
    ob->o[ob->n].path = path; ob->o[ob->n].bound = bound; ob->o[ob->n].ret = ret;
    if (vlen) vlen_clone_into(&ob->o[ob->n].vlen, vlen); else vlen_init(&ob->o[ob->n].vlen);
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
            *witness = calloc(nv ? nv : 1, sizeof(IBind));
            for (int i = 0; i < nv; i++) { (*witness)[i].name = strdup(vars[i]); (*witness)[i].val = assign[i].val; }
            *wn = nv;
            found = 1;
        }
    }
    free(assign);
    for (int i = 0; i < nv; i++) free(vars[i]);
    free(vars);
    return found;
}

/* ============================ symbolic execution ============================ */

typedef struct { SEnv env; VLenMap vlen; ConsList path; int bad; } State;

static void state_free(State *s) { env_free(&s->env); vlen_free(&s->vlen); cl_free(&s->path); }

typedef struct { State *s; int n, cap; } States;
static void states_push(States *ss, State s) {
    if (ss->n == ss->cap) { ss->cap = ss->cap ? ss->cap * 2 : 4; ss->s = realloc(ss->s, (size_t)ss->cap * sizeof(State)); }
    ss->s[ss->n++] = s;
}

/* Detect constructs the verifier cannot handle. `flat` treats a while loop as
 * an opaque boundary (its body is checked separately, with its invariant). */
static int is_vec_builtin_call(Node *n) {
    if (n->kind != NODE_CALL || !n->callee || n->callee->kind != NODE_IDENT) return 0;
    const char *nm = n->callee->name;
    return strcmp(nm, "vec_new") == 0 || strcmp(nm, "vec_push") == 0 ||
           strcmp(nm, "vec_get") == 0 || strcmp(nm, "vec_set") == 0 ||
           strcmp(nm, "vec_len") == 0;
}

static int has_unsupported_rec(Node *n, int flat) {
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
            for (int i = 0; i < n->args.len; i++) if (has_unsupported_rec(n->args.data[i], flat)) return 1;
            return 0;
        }
        return 1;   /* any other call: recursion / extern / user fn */
    case NODE_WHILE:
        if (flat) return 1;
        if (n->while_invariants.len == 0) return 1;   /* M1: loops need invariants */
        if (has_unsupported_rec(n->while_cond, 1)) return 1;
        for (int i = 0; i < n->while_invariants.len; i++)
            if (has_unsupported_rec(n->while_invariants.data[i], 1)) return 1;
        return has_unsupported_rec(n->while_body, 0);
    default: break;
    }
    switch (n->kind) {
    case NODE_BINARY: return has_unsupported_rec(n->left, flat) || has_unsupported_rec(n->right, flat);
    case NODE_UNARY: return has_unsupported_rec(n->operand, flat);
    case NODE_IF: return has_unsupported_rec(n->cond, flat) || has_unsupported_rec(n->then_br, flat) || has_unsupported_rec(n->else_br, flat);
    case NODE_BLOCK: for (int i = 0; i < n->stmts.len; i++) if (has_unsupported_rec(n->stmts.data[i], flat)) return 1; return 0;
    case NODE_LET: return has_unsupported_rec(n->let_init, flat);
    case NODE_ASSIGN: return has_unsupported_rec(n->assign_target, flat) || has_unsupported_rec(n->assign_val, flat);
    case NODE_RETURN: return has_unsupported_rec(n->ret_val, flat);
    case NODE_EXPR_STMT: return has_unsupported_rec(n->expr, flat);
    default: return 0;
    }
}

static int has_unsupported(Node *n) { return has_unsupported_rec(n, 0); }

static State clone_state_with(State *cur, Formula *add);
static void symexec_block(Node *blk, States *states, Obligations *ob, int is_nonvoid, Node *spec);

/* Prove one invariant holds in every body-final state, given the head state
 * (invariant ∧ condition on entry). UNSAT per DNF branch ⇒ holds. */
static int check_preservation(State *head, Node *inv, States *body_final) {
    Formula hf;
    if (!bool_to_dnf(inv, &head->env, &head->vlen, 0, &hf)) return 0;
    int holds = 1;
    for (int k = 0; k < body_final->n && holds; k++) {
        State *bf = &body_final->s[k];
        if (bf->bad) { holds = 0; break; }
        Formula bf_inv;
        if (!bool_to_dnf(inv, &bf->env, &bf->vlen, 0, &bf_inv)) { holds = 0; break; }
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
    f_free(&hf);
    return holds;
}

/* Symbolic execution of a while loop with invariants (Hoare):
 *   init        — invariant holds on entry (from the current path);
 *   preservation— invariant ∧ cond, after one body iteration, ⇒ invariant;
 *   post-loop   — invariant ∧ ¬cond continues the fall-through path, but ONLY
 *                 if init and preservation are both PROVEN (soundness gate —
 *                 otherwise any downstream proof depending on it is UNKNOWN).
 * Returns 1 iff the invariant is trustworthy (init ∧ preservation proven). */
static int symexec_while(Node *st, States *states, Obligations *ob, int is_nonvoid, Node *spec) {
    (void)is_nonvoid;   /* the loop body is never a return context */
    int trusted = 1;
    States out; out.s = NULL; out.n = 0; out.cap = 0;
    for (int i = 0; i < states->n; i++) {
        State *cur = &states->s[i];

        /* init: each invariant must follow from the current path */
        for (int j = 0; j < st->while_invariants.len && trusted; j++) {
            Formula invf;
            if (!bool_to_dnf(st->while_invariants.data[j], &cur->env, &cur->vlen, 0, &invf)) { trusted = 0; break; }
            for (int b = 0; b < invf.n && trusted; b++) {
                ConsList sys; cl_init(&sys);
                for (int x = 0; x < cur->path.n; x++) { Constraint *c = &cur->path.c[x]; cl_push(&sys, mk_cons(lin_clone(&c->lhs), c->op, c->rhs)); }
                for (int x = 0; x < invf.br[b].n; x++) {
                    Constraint *c = &invf.br[b].c[x];
                    Lin lhs = lin_neg(&c->lhs);
                    COp op = (c->op == C_LT) ? C_LE : C_LT;
                    cl_push(&sys, mk_cons(lhs, op, c->rhs));
                }
                if (fm_sat(&sys)) trusted = 0;
            }
            f_free(&invf);
        }

        /* preservation: invariant ∧ cond, run one body iteration, re-check */
        Formula cf;
        if (!bool_to_dnf(st->cond, &cur->env, &cur->vlen, 0, &cf)) { trusted = 0; }
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
                    if (!bool_to_dnf(st->while_invariants.data[j], &h.env, &h.vlen, 0, &invf)) { trusted = 0; break; }
                    for (int bb = 0; bb < invf.n; bb++)
                        for (int x = 0; x < invf.br[bb].n; x++) { Constraint *c = &invf.br[bb].c[x]; cl_push(&h.path, mk_cons(lin_clone(&c->lhs), c->op, c->rhs)); }
                    f_free(&invf);
                }
                states_push(&head_ss, h);
            }
            /* the loop body is not a return context: its last statement is NOT
             * an implicit return, so pass is_nonvoid=0 */
            symexec_block(st->while_body, &head_ss, ob, 0, spec);
            for (int j = 0; j < st->while_invariants.len && trusted; j++)
                if (!check_preservation(cur, st->while_invariants.data[j], &head_ss)) trusted = 0;
        }
        for (int k = 0; k < head_ss.n; k++) state_free(&head_ss.s[k]);
        free(head_ss.s);
        f_free(&cf);

        /* post-loop continuation: invariant ∧ ¬cond (only sound when trusted) */
        Formula nf;
        if (!bool_to_dnf(st->cond, &cur->env, &cur->vlen, 1, &nf)) {
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
                    if (!bool_to_dnf(st->while_invariants.data[j], &t.env, &t.vlen, 0, &invf)) { t.bad = 1; break; }
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
    return trusted;
}

/* Track Vec lengths and emit bounds obligations for vec accesses in an
 * expression statement. Only the top-level call and a let-init call are
 * handled; nested vec calls inside larger expressions are not tracked. */
static void scan_vec_expr(Node *e, State *st, Obligations *ob, Node *spec) {
    if (!e) return;
    if (e->kind != NODE_CALL || e->callee->kind != NODE_IDENT) return;
    const char *nm = e->callee->name;

    if (strcmp(nm, "vec_new") == 0) return;   /* length handled at the let binding */

    if (strcmp(nm, "vec_push") == 0 && e->args.len >= 1 && e->args.data[0]->kind == NODE_IDENT) {
        VLenEntry *ve = vlen_find(&st->vlen, e->args.data[0]->name);
        if (ve && !ve->unknown) vlen_set(&st->vlen, e->args.data[0]->name, lin_add(&ve->len, &(Lin){ .c = rat_int(1) }), 0);
        else vlen_set(&st->vlen, e->args.data[0]->name, (Lin){0}, 1);
        return;
    }

    if ((strcmp(nm, "vec_get") == 0 || strcmp(nm, "vec_set") == 0) && e->args.len >= 2 &&
        e->args.data[0]->kind == NODE_IDENT) {
        const char *vname = e->args.data[0]->name;
        VLenEntry *ve = vlen_find(&st->vlen, vname);
        Sym idx = se_from_ast(e->args.data[1], &st->env, &st->vlen);
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
            if (bool_to_dnf(spec->spec_requires.data[r]->ensure_expr, &st->env, &st->vlen, 0, &rf)) {
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
        obl_push(ob, opath, obound, retsym, &st->vlen, unknown, 1, label);
        lin_free(&idx.lin);
        return;
    }
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
    cl_init(&t.path);
    for (int x = 0; x < cur->path.n; x++) { Constraint *c = &cur->path.c[x]; cl_push(&t.path, mk_cons(lin_clone(&c->lhs), c->op, c->rhs)); }
    if (add) for (int b = 0; b < add->n; b++) for (int x = 0; x < add->br[b].n; x++) {
        Constraint *c = &add->br[b].c[x]; cl_push(&t.path, mk_cons(lin_clone(&c->lhs), c->op, c->rhs));
    }
    t.bad = cur->bad;
    return t;
}

static void bind_sym(SEnv *env, const char *name, Sym v) {
    if (v.nonlinear) { Lin empty; lin_init(&empty); env_bind(env, name, empty, 1); }
    else env_bind(env, name, v.lin, 0);
}

static void drain_returns(States *states, Obligations *ob, Node *val_expr) {
    for (int i = 0; i < states->n; i++) {
        Sym r = se_from_ast(val_expr, &states->s[i].env, &states->s[i].vlen);
        ConsList nobound; cl_init(&nobound);
        obl_push(ob, states->s[i].path, nobound, r, &states->s[i].vlen, states->s[i].bad, 0, NULL);
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
                bind_sym(&states->s[i].env, st->let_name, se_from_ast(st->let_init, &states->s[i].env, &states->s[i].vlen));
                /* a fresh vec_new() has length 0 */
                if (st->let_init && st->let_init->kind == NODE_CALL && st->let_init->callee->kind == NODE_IDENT &&
                    strcmp(st->let_init->callee->name, "vec_new") == 0)
                    vlen_set(&states->s[i].vlen, st->let_name, lin_const(rat_int(0)), 0);
                scan_vec_expr(st->let_init, &states->s[i], ob, spec);
            }
        } else if (st->kind == NODE_ASSIGN) {
            if (st->assign_target->kind != NODE_IDENT) { for (int i = 0; i < states->n; i++) states->s[i].bad = 1; continue; }
            for (int i = 0; i < states->n; i++)
                bind_sym(&states->s[i].env, st->assign_target->name, se_from_ast(st->assign_val, &states->s[i].env, &states->s[i].vlen));
        } else if (st->kind == NODE_RETURN) {
            for (int i = 0; i < states->n; i++) scan_vec_expr(st->ret_val, &states->s[i], ob, spec);
            drain_returns(states, ob, st->ret_val);
            return;
        } else if (st->kind == NODE_EXPR_STMT && last && is_nonvoid) {
            for (int i = 0; i < states->n; i++) scan_vec_expr(st->expr, &states->s[i], ob, spec);
            drain_returns(states, ob, st->expr);
            return;
        } else if (st->kind == NODE_EXPR_STMT) {
            /* expression statement: track vec mutations / emit bounds checks */
            for (int i = 0; i < states->n; i++) scan_vec_expr(st->expr, &states->s[i], ob, spec);
        } else if (st->kind == NODE_IF) {
            States out; out.s = NULL; out.n = 0; out.cap = 0;
            for (int i = 0; i < states->n; i++) {
                State *cur = &states->s[i];
                Formula cf, nf;
                int cok = bool_to_dnf(st->cond, &cur->env, &cur->vlen, 0, &cf);
                int nok = bool_to_dnf(st->cond, &cur->env, &cur->vlen, 1, &nf);
                if (!cok || !nok) {
                    if (cok) f_free(&cf);
                    if (nok) f_free(&nf);
                    cur->bad = 1;
                    states_push(&out, *cur);
                    continue;
                }
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

/* Verify one ensures clause over all obligations of a function. */
static VRes verify_ensures(Node *spec, Node *ens_expr, Obligations *ob,
                           IBind **wit, int *wn) {
    VRes worst = R_PROVEN;
    for (int o = 0; o < ob->n; o++) {
        Obligation *obl = &ob->o[o];
        if (obl->bad) { if (worst == R_PROVEN) worst = R_UNKNOWN; continue; }
        if (obl->kind == 1) continue;   /* bounds obligations: handled by verify_bound_obl */

        if (obl->ret.nonlinear) { if (worst == R_PROVEN) worst = R_UNKNOWN; continue; }
        /* env with output := return */
        SEnv env2; env_init(&env2);
        env_bind(&env2, "output", lin_clone(&obl->ret.lin), 0);
        /* requires as antecedent constraints */
        ConsList ante; cl_init(&ante);
        int ante_ok = 1;
        for (int r = 0; r < spec->spec_requires.len && ante_ok; r++) {
            Formula rf;
            if (!bool_to_dnf(spec->spec_requires.data[r]->ensure_expr, &env2, &obl->vlen, 0, &rf)) { ante_ok = 0; break; }
            if (rf.n != 1) ante_ok = 0;   /* disjunctive requires: M0 skips */
            else for (int x = 0; x < rf.br[0].n; x++) { Constraint *c = &rf.br[0].c[x]; cl_push(&ante, mk_cons(lin_clone(&c->lhs), c->op, c->rhs)); }
            f_free(&rf);
        }
        for (int x = 0; x < obl->path.n && ante_ok; x++) { Constraint *c = &obl->path.c[x]; cl_push(&ante, mk_cons(lin_clone(&c->lhs), c->op, c->rhs)); }
        if (!ante_ok) { cl_free(&ante); env_free(&env2); if (worst == R_PROVEN) worst = R_UNKNOWN; continue; }

        /* ensures as DNF, and its negation as DNF (De Morgan handled inside
         * bool_to_dnf, so a disjunctive ensures becomes a conjunction when
         * negated). The obligation  ante ⇒ ensures  is checked per negated
         * branch:  UNSAT(ante ∧ ¬ensures_branch)  ⇒ that branch always holds. */
        Formula ef, nef;
        if (!bool_to_dnf(ens_expr, &env2, &obl->vlen, 0, &ef)) { cl_free(&ante); env_free(&env2); if (worst == R_PROVEN) worst = R_UNKNOWN; continue; }
        if (!bool_to_dnf(ens_expr, &env2, &obl->vlen, 1, &nef)) { f_free(&ef); cl_free(&ante); env_free(&env2); if (worst == R_PROVEN) worst = R_UNKNOWN; continue; }

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
                if (find_counterexample(&ante2, &nef, ens_expr, &env2, &w, &wnn)) {
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
static int verify_fn(Node *prog, Node *fn) {
    Node *spec = find_spec(prog, fn->fn_name);
    if (!spec) return 0;

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

    printf("verify %s:\n", fn->fn_name);
    if (skip) {
        for (int j = 0; j < spec->spec_ensures.len; j++)
            printf("  ensures #%d (%s): ПРОПУСНАТО (%s)\n", j + 1, spec->spec_ensures.data[j]->ensure_text, skip);
        return 0;
    }

    /* symexec */
    Obligations ob; ob.o = NULL; ob.n = 0; ob.cap = 0;
    State init; env_init(&init.env); vlen_init(&init.vlen); cl_init(&init.path); init.bad = 0;
    for (int i = 0; i < fn->params.len; i++) {
        Node *p = fn->params.data[i];
        env_bind(&init.env, p->param_name, lin_var(p->param_name), 0);
        /* a Vec parameter has a symbolic length the caller may constrain via
         * requires (e.g. vec_len(v) >= 1) */
        Node *pt = unwrap_type(p->param_type);
        if (pt && pt->kind == NODE_TYPE_ARRAY) {
            char buf[300]; snprintf(buf, sizeof buf, "__len_%s", p->param_name);
            vlen_set(&init.vlen, p->param_name, lin_var(buf), 0);
        }
    }
    States states; states.s = NULL; states.n = 0; states.cap = 0;
    states_push(&states, init);
    int is_nonvoid = (fn->ret_type != NULL);
    symexec_stmts(&fn->fn_body->stmts, &states, &ob, is_nonvoid, spec);
    for (int i = 0; i < states.n; i++) state_free(&states.s[i]);
    free(states.s);

    int any_refuted = 0;
    for (int j = 0; j < spec->spec_ensures.len; j++) {
        IBind *wit = NULL; int wn = 0;
        VRes r = verify_ensures(spec, spec->spec_ensures.data[j]->ensure_expr, &ob, &wit, &wn);
        printf("  ensures #%d (%s): %s\n", j + 1, spec->spec_ensures.data[j]->ensure_text, res_word(r));
        if (r == R_REFUTED) {
            any_refuted = 1;
            printf("    контрапример:");
            for (int k = 0; k < wn; k++) printf(" %s = %lld", wit[k].name, (long long)wit[k].val);
            printf("\n");
        }
        if (wit) { for (int k = 0; k < wn; k++) free(wit[k].name); free(wit); }
    }
    /* bounds obligations (M2): report each vec access check */
    for (int i = 0; i < ob.n; i++) {
        if (ob.o[i].kind != 1) continue;
        IBind *wit = NULL; int wn = 0;
        VRes r = verify_bound_obl(&ob.o[i], &wit, &wn);
        printf("  граница (%s): %s\n", ob.o[i].label ? ob.o[i].label : "достъп до вектор", res_word(r));
        if (r == R_REFUTED) {
            any_refuted = 1;
            printf("    контрапример:");
            for (int k = 0; k < wn; k++) printf(" %s = %lld", wit[k].name, (long long)wit[k].val);
            printf("\n");
        }
        if (wit) { for (int k = 0; k < wn; k++) free(wit[k].name); free(wit); }
    }
    for (int i = 0; i < ob.n; i++) { cl_free(&ob.o[i].path); cl_free(&ob.o[i].bound); lin_free(&ob.o[i].ret.lin); vlen_free(&ob.o[i].vlen); free(ob.o[i].label); }
    free(ob.o);
    return any_refuted;
}

int verify_program(Node *prog) {
    int any_refuted = 0;
    for (int i = 0; i < prog->items.len; i++) {
        Node *it = prog->items.data[i];
        if (it->kind != NODE_FN || !it->fn_body) continue;
        if (!find_spec(prog, it->fn_name)) continue;
        any_refuted |= verify_fn(prog, it);
    }
    return any_refuted ? 1 : 0;
}
