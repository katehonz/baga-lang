/* verify_symexec.c — част от verify.c (виж verify_proto.h). */
#include "baga.h"
#include "verify_impl.h"

/* ============================ symbolic execution ============================ */

/* M3 element invariant: forall i in [0, len(vec)): vec[i] <cmp> rhs.
 * cmp is one of EC_LT/EC_LE/EC_GT/EC_GE; rhs is a linear form over scalars. */
/* is_sorted: a relational axiom "sorted(vec)" (forall i: vec[i] <= vec[i+1]);
 * cmp/rhs are unused in that case. */

void ax_init(AxiomList *l) { l->a = NULL; l->n = 0; l->cap = 0; }
void ax_free(AxiomList *l) { for (int i = 0; i < l->n; i++) { free(l->a[i].vec); lin_free(&l->a[i].rhs); } free(l->a); l->a = NULL; l->n = l->cap = 0; }
void ax_push(AxiomList *l, const char *vec, ElemCmp cmp, Lin rhs, int is_sorted) {
    if (l->n == l->cap) { l->cap = l->cap ? l->cap * 2 : 4; l->a = realloc(l->a, (size_t)l->cap * sizeof(ElemAxiom)); }
    l->a[l->n].vec = strdup(vec); l->a[l->n].cmp = cmp; l->a[l->n].rhs = rhs; l->a[l->n].is_sorted = is_sorted; l->n++;
}
void ax_clone_into(AxiomList *dst, AxiomList *src) {
    ax_init(dst);
    for (int i = 0; i < src->n; i++) ax_push(dst, src->a[i].vec, src->a[i].cmp, lin_clone(&src->a[i].rhs), src->a[i].is_sorted);
}

/* Copy the axioms of vector `from` (in `src`) into `dst`, re-anchored on `to`. */
void ax_copy_renamed(AxiomList *dst, AxiomList *src, const char *from, const char *to) {
    for (int i = 0; i < src->n; i++)
        if (strcmp(src->a[i].vec, from) == 0)
            ax_push(dst, to, src->a[i].cmp, lin_clone(&src->a[i].rhs), src->a[i].is_sorted);
}

/* vec_concat result: keep only the axioms BOTH operands share (same cmp+rhs),
 * re-anchored on `to` — those hold for every element of v ++ w. */
static int lin_equal(Lin *a, Lin *b);
void ax_intersect_two(AxiomList *dst, AxiomList *src, const char *v, const char *w, const char *to) {
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
void reads_init(ReadsList *l) { l->r = NULL; l->n = 0; l->cap = 0; }
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
void reads_push(ReadsList *l, Node *call, const char *var) {
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
void reads_push_prod(ReadsList *l, const char *var, Lin fa, Lin fb) {
    ReadEntry *e = reads_slot(l, var);
    lin_free(&e->fa); lin_free(&e->fb);
    e->fa = fa; e->fb = fb; e->is_prod = 1;
    classify_monomial(l, e);
}
/* M9: register integer division by a constant (moves fa; fb is the const divisor). */
void reads_push_div(ReadsList *l, const char *var, Lin fa, Lin fb) {
    ReadEntry *e = reads_slot(l, var);
    lin_free(&e->fa); lin_free(&e->fb);
    e->fa = fa; e->fb = fb; e->is_div = 1;
}
/* M9b: register integer mod by a constant. */
void reads_push_mod(ReadsList *l, const char *var, Lin fa, Lin fb) {
    ReadEntry *e = reads_slot(l, var);
    lin_free(&e->fa); lin_free(&e->fb);
    e->fa = fa; e->fb = fb; e->is_mod = 1;
}
/* M22: register n >> k (аритметично отместване); fb държи БРОЯЧА k (const),
 * не делителя 2^k. */
void reads_push_shr(ReadsList *l, const char *var, Lin fa, Lin fb) {
    ReadEntry *e = reads_slot(l, var);
    lin_free(&e->fa); lin_free(&e->fb);
    e->fa = fa; e->fb = fb; e->is_shr = 1;
}
/* M23: register n >>> k (логическо отместване, zero-fill); fb държи k. */
void reads_push_lshr(ReadsList *l, const char *var, Lin fa, Lin fb) {
    ReadEntry *e = reads_slot(l, var);
    lin_free(&e->fa); lin_free(&e->fb);
    e->fa = fa; e->fb = fb; e->is_lshr = 1;
}
/* M13/M20: n & mask → residue in {0..mask} when mask = 2^k-1 (moves fa). */
void reads_push_bitmask(ReadsList *l, const char *var, Lin fa, int64_t mask) {
    ReadEntry *e = reads_slot(l, var);
    lin_free(&e->fa); lin_free(&e->fb);
    e->fa = fa; e->fb = lin_const(rat_int(mask)); e->is_bitand1 = 1;
}
/* M20: variable n&m / n|m — bounds injected when both sides nonnegative. */
void reads_push_bitop(ReadsList *l, const char *var, Lin fa, Lin fb, int is_or) {
    ReadEntry *e = reads_slot(l, var);
    lin_free(&e->fa); lin_free(&e->fb);
    e->fa = fa; e->fb = fb;
    if (is_or) e->is_bitor = 1; else e->is_bitand = 1;
}
/* M17: register a pair var with its two components (moves fa/fb). */
void reads_push_pair(ReadsList *l, const char *var, Lin fa, Lin fb) {
    ReadEntry *e = reads_slot(l, var);
    lin_free(&e->fa); lin_free(&e->fb);
    e->fa = fa; e->fb = fb; e->is_pair = 1;
}
/* M17: find the pair entry anchored on a bare symbolic var, or NULL. */
ReadEntry *reads_find_pair(ReadsList *l, const char *var) {
    for (int i = 0; i < l->n; i++)
        if (l->r[i].is_pair && strcmp(l->r[i].var, var) == 0) return &l->r[i];
    return NULL;
}
const char *reads_find(ReadsList *l, Node *call) {
    for (int i = 0; i < l->n; i++)
        if (!l->r[i].is_prod && !l->r[i].is_div && !l->r[i].is_mod && !l->r[i].is_bitand1 &&
            l->r[i].call == call) return l->r[i].var;
    return NULL;
}
void reads_clone_into(ReadsList *dst, ReadsList *src) {
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
void prods_clone_into(ProdList *dst, ReadsList *src) {
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
void prods_free(ProdList *l) {
    for (int i = 0; i < l->n; i++) { free(l->p[i].var); lin_free(&l->p[i].fa); lin_free(&l->p[i].fb); }
    free(l->p); l->p = NULL; l->n = l->cap = 0;
}

/* M14: handle protocol state for !Par — join handles and channels are opaque
 * i64 values; we track a ghost protocol state per symbolic handle variable.
 * Keyed by the symbolic var name (e.g. __h0), so source-level aliasing
 * (`let h2 = h`) works: both names hold the same linear form. */

void handles_init(HandleList *l) { l->h = NULL; l->n = 0; l->cap = 0; }
void handles_free(HandleList *l) {
    for (int i = 0; i < l->n; i++) {
        free(l->h[i].var); lin_free(&l->h[i].result);
        free(l->h[i].worker); free(l->h[i].chan_arg);
    }
    free(l->h);
    handles_init(l);
}
HandleEntry *handles_find(HandleList *l, const char *var) {
    for (int i = 0; i < l->n; i++) if (strcmp(l->h[i].var, var) == 0) return &l->h[i];
    return NULL;
}
/* result is moved (may be NULL for channels). Re-setting replaces in place. */
void handles_set(HandleList *l, const char *var, int kind, int state, Lin *result) {
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
        e->wf_child_cons = 0;
        e->wf_imprecise = 0;
        e->wf_noreturn = 0;
        e->wf_maybe_loop = 0;
        e->n_send = e->n_recv = e->n_bg_prod = e->n_bg_cons = e->wf_unknown = 0;
        e->wf_cap = -1;
        e->wf_sel_unknown = 0;
    } else {
        lin_free(&e->result);
    }
    e->kind = kind; e->state = state;
    if (result) { e->result = *result; }
    else { Lin empty; lin_init(&empty); e->result = empty; }
}
void handles_clone_into(HandleList *dst, HandleList *src) {
    handles_init(dst);
    for (int i = 0; i < src->n; i++) {
        Lin r = lin_clone(&src->h[i].result);
        handles_set(dst, src->h[i].var, src->h[i].kind, src->h[i].state, &r);
        HandleEntry *d = handles_find(dst, src->h[i].var);
        d->n_send = src->h[i].n_send; d->n_recv = src->h[i].n_recv;
        d->n_bg_prod = src->h[i].n_bg_prod; d->n_bg_cons = src->h[i].n_bg_cons;
        d->wf_unknown = src->h[i].wf_unknown; d->wf_cap = src->h[i].wf_cap;
        d->wf_sel_unknown = src->h[i].wf_sel_unknown;
        d->wf_recv = src->h[i].wf_recv; d->wf_send = src->h[i].wf_send;
        d->wf_complex = src->h[i].wf_complex;
        d->wf_n_send = src->h[i].wf_n_send; d->wf_n_recv = src->h[i].wf_n_recv;
        d->wf_child_cons = src->h[i].wf_child_cons;
        d->wf_imprecise = src->h[i].wf_imprecise;
        d->wf_noreturn = src->h[i].wf_noreturn;
        d->wf_maybe_loop = src->h[i].wf_maybe_loop;
        d->worker = src->h[i].worker ? strdup(src->h[i].worker) : NULL;
        d->chan_arg = src->h[i].chan_arg ? strdup(src->h[i].chan_arg) : NULL;
    }
}


void state_free(State *s) { env_free(&s->env); vlen_free(&s->vlen); ax_free(&s->ax); cl_free(&s->path); cl_free(&s->read_cons); reads_free(&s->reads); handles_free(&s->handles); }

void states_push(States *ss, State s) {
    if (ss->n == ss->cap) { ss->cap = ss->cap ? ss->cap * 2 : 4; ss->s = realloc(ss->s, (size_t)ss->cap * sizeof(State)); }
    ss->s[ss->n++] = s;
}

/* M5: program context for resolving user-function calls during symexec.
 * Set by verify_fn_collect / verify_program (proofs.c goes through
 * verify_fn_collect too, so it is always initialized before use). */
Node *g_prog = NULL;
const char *g_caller_name = NULL;
int g_call_ctr = 0;
int g_handle_ctr = 0;    /* M14: fresh __hN / __chN handle vars */
int g_partial = 0;        /* direct self-recursion seen during symexec */
int g_term = 0;           /* spec carries a decreases measure (M6) */
int g_term_failed = 0;    /* some termination obligation is not PROVEN */

/* Verified-fact collection for --proofs: while-loop invariants encountered
 * during symexec, each flagged by whether the Hoare checks (init +
 * preservation) proved it trustworthy. */
char **g_inv_texts = NULL;
int  *g_inv_proven = NULL;
int   g_inv_n = 0, g_inv_cap = 0;

/* Render an annotation expression (requires/ensures/invariant fragment) as
 * source-like text for --proofs output. */
void expr_render(Node *e, char *b, size_t n, size_t *o) {
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

void inv_collect_reset(void) {
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

Node *find_spec(Node *prog, const char *name);   /* defined below */
int type_is_i64(Node *t);                        /* defined below */
int ret_has_effects(Node *t);                    /* defined below */

Node *find_fn(Node *prog, const char *name) {
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
int callee_sig_supported(Node *cfn) {
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
int is_vec_builtin_call(Node *n) {
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
int is_par_builtin_call(Node *n) {
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
int is_drop_call(Node *n) {
    if (!n || n->kind != NODE_CALL || !n->callee || n->callee->kind != NODE_IDENT) return 0;
    if (strcmp(n->callee->name, "drop") != 0) return 0;
    if (n->args.len != 1 || n->args.data[0]->kind != NODE_IDENT) return 0;
    if (g_prog && find_fn(g_prog, "drop")) return 0;
    return 1;
}

/* MEM-2: allocators of owned, droppable buffers (vec_new is already covered
 * by is_vec_builtin_call; listed here for the registration hook). The values
 * themselves stay opaque — only the alloc→drop protocol is tracked. */
int is_mem_alloc_call(Node *n) {
    if (!n || n->kind != NODE_CALL || !n->callee || n->callee->kind != NODE_IDENT) return 0;
    const char *nm = n->callee->name;
    return strcmp(nm, "vec_new") == 0 || strcmp(nm, "map_new") == 0 ||
           strcmp(nm, "bytes_new") == 0;
}

/* W0: светове — регион с ownership/freeze протокол (HK_WORLD). Runtime-ът
 * е арената + frozen бит; тук е само разпознаването за гейта и ghost
 * машинерията. Потребителска fn със същото име shadow-ва builtin-а —
 * същият guard като при drop (find_fn). */
static int world_builtin_shadowed(const char *nm) {
    return g_prog && find_fn(g_prog, nm);
}
int is_world_new_call(Node *n) {
    if (!n || n->kind != NODE_CALL || !n->callee || n->callee->kind != NODE_IDENT) return 0;
    return strcmp(n->callee->name, "world_new") == 0 && n->args.len == 0 &&
           !world_builtin_shadowed("world_new");
}
int is_world_op_call(Node *n) {
    if (!n || n->kind != NODE_CALL || !n->callee || n->callee->kind != NODE_IDENT) return 0;
    const char *nm = n->callee->name;
    int is_op = strcmp(nm, "world_alloc") == 0 || strcmp(nm, "world_freeze") == 0 ||
                strcmp(nm, "world_free") == 0 || strcmp(nm, "wvec_push") == 0;
    if (!is_op || world_builtin_shadowed(nm)) return 0;
    return n->args.len >= 1 && n->args.data[0]->kind == NODE_IDENT;
}
int is_world_expr_call(Node *n) {
    if (!n || n->kind != NODE_CALL || !n->callee || n->callee->kind != NODE_IDENT) return 0;
    const char *nm = n->callee->name;
    int is_expr = strcmp(nm, "world_new") == 0 || strcmp(nm, "world_alloc") == 0 ||
                  strcmp(nm, "world_state") == 0 || strcmp(nm, "wvec_new") == 0 ||
                  strcmp(nm, "wvec_push") == 0 || strcmp(nm, "wvec_get") == 0 ||
                  strcmp(nm, "wvec_len") == 0 || strcmp(nm, "world_dump") == 0 ||
                  strcmp(nm, "world_load") == 0;
    return is_expr && !world_builtin_shadowed(nm);
}
int is_world_stmt_call(Node *n) {
    if (!n || n->kind != NODE_CALL || !n->callee || n->callee->kind != NODE_IDENT) return 0;
    const char *nm = n->callee->name;
    int is_stmt = strcmp(nm, "world_freeze") == 0 || strcmp(nm, "world_free") == 0;
    return is_stmt && !world_builtin_shadowed(nm);
}

/* M14 shallow gate for a par builtin call: right arity; for go/go_bg the
 * worker must be a user fn (i64) -> i64 whose only allowed effect is Par
 * itself (channel-using workers are Par by construction); any other effect
 * puts it outside the fragment. Body verifiability is checked later, at
 * eval time, before any ensures are assumed (same discipline as M5). */
int ret_has_unverifiable_effects(Node *t);   /* defined below */
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
        /* W0: world_freeze/world_free — statement-level дисциплина като drop
         * (никога в цикъл или вложен в израз). */
        if (is_world_stmt_call(n)) {
            if (flat || in_expr) return 1;
            return 0;
        }
        /* W0: world_new/world_alloc/world_state — изразни (като mem allocs). */
        if (is_world_expr_call(n)) {
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

int has_unsupported(Node *n) { return has_unsupported_rec(n, 0, 0); }

State clone_state_with(State *cur, Formula *add);
void inject_prod_axioms(State *st);
void symexec_block(Node *blk, States *states, Obligations *ob, int is_nonvoid, Node *spec);
void scan_arith_expr(Node *e, State *st, Obligations *ob);   /* M15 */
void check_drop_uses(Node *e, State *st, Obligations *ob);   /* MEM-2 */
int cl_implied_by(ConsList *path, ConsList *cons);

/* Prove one invariant holds in every body-final state, given the head state
 * (invariant ∧ condition on entry). UNSAT per DNF branch ⇒ holds. */
int has_elem_ref(Node *e);
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
void extract_elem_axiom(Node *e, SEnv *env, AxiomList *ax);
int has_elem_ref(Node *e);

/* Soundness fix (M15): variables assigned or let-bound inside a loop body
 * must be HAVOCED before the invariant is assumed — otherwise the head and
 * post-loop states keep the STALE pre-loop values and the invariant is
 * vacuous (false proofs; e.g. a loop returning -n "proved" output >= 0).
 * With havoc the Hoare rule is the real one: init on the entry state;
 * preservation over fresh values constrained only by invariant ∧ cond;
 * post-loop invariant ∧ ¬cond over fresh values. */
int g_havoc_ctr = 0;

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

int symexec_while(Node *st, States *states, Obligations *ob, int is_nonvoid, Node *spec) {
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
Constraint axiom_instantiate(ElemAxiom *a, const char *read_var);
int axiom_holds_for_value(ElemAxiom *a, Sym *val, State *st);

