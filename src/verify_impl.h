/* verify_impl.h — вътрешен header на verify модулите (verify_core.c,
 * verify_obl.c, verify_symexec.c, verify_invariants.c, verify_waitfor.c,
 * verify_top.c). Типове, споделено състояние и прототипи между тях.
 * Публичният API (verify_program, verify_fn_collect, fn_verify_res_free)
 * е в baga.h. */
#ifndef BAGA_VERIFY_IMPL_H
#define BAGA_VERIFY_IMPL_H

#include "baga.h"
#include <stdint.h>
#include <stdio.h>

/* ============================ типове ============================ */

typedef struct { int64_t num, den; int overflow; } Rat;
typedef struct { char *var; Rat coeff; } Term;
typedef struct { Rat c; Term *terms; int n, cap; int overflow; } Lin;
typedef struct { char *name; Lin lin; int nonlinear; } Binding;
typedef struct { Binding *b; int n, cap; } SEnv;
typedef struct { Lin lin; int nonlinear; } Sym;
typedef struct { char *name; Lin len; int unknown; } VLenEntry;
typedef struct { VLenEntry *e; int n, cap; } VLenMap;
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
typedef enum { C_LT, C_LE } COp;
typedef struct { Lin lhs; COp op; Rat rhs; } Constraint;
typedef struct { Constraint *c; int n, cap; } ConsList;
typedef struct { ConsList *br; int n, cap; } Formula;   /* DNF: OR of branches */
typedef struct { Lin rest; Rat bound; int strict; int is_lower; } Bound;
typedef struct { char *name; int64_t val; } IBind;
typedef struct { IBind *b; int n; } IEnv;
typedef struct { ConsList path; ConsList bound; ConsList read_cons; Sym ret; VLenMap vlen; ProdList prods; int bad; int kind; char *label; int akind; Lin aux1, aux2; } Obligation;
typedef struct { Obligation *o; int n, cap; } Obligations;
typedef enum { EC_LT, EC_LE, EC_GT, EC_GE } ElemCmp;
typedef struct { char *vec; ElemCmp cmp; Lin rhs; int is_sorted; } ElemAxiom;
typedef struct { ElemAxiom *a; int n, cap; } AxiomList;
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
    int wf_child_cons; /* HK_JOIN: M29 guaranteed recvs of unjoined children */
    int wf_maybe_loop; /* HK_JOIN: M30 a server loop — completion not guaranteed */
    int wf_imprecise; /* HK_JOIN: counts are guaranteed minimums (M27 branches) */
    int wf_noreturn;  /* HK_JOIN: worker provably never completes (M28) */
    int n_send;       /* HK_CHAN: sends on this thread */
    int n_recv;       /* HK_CHAN: recvs on this thread */
    int n_bg_prod;    /* HK_CHAN: summed sends of go_bg send-first workers (M25) */
    int n_bg_cons;    /* HK_CHAN: go_bg workers that recv-first (M24 credits) */
    int wf_cap;       /* HK_CHAN: constant buffer capacity, -1 = unknown (M24) */
    int wf_sel_unknown; /* HK_CHAN: a select2_wait consumed from an unknown side (M26) */
    int wf_unknown;   /* HK_CHAN: a spawned worker was too complex to classify */
} HandleEntry;
typedef struct { HandleEntry *h; int n, cap; } HandleList;
typedef struct { SEnv env; VLenMap vlen; AxiomList ax; ConsList path; ConsList read_cons; ReadsList reads; HandleList handles; int bad; } State;
typedef struct { State *s; int n, cap; } States;
typedef struct WfScan {
    int recv_first, send_first, n_recv, n_send, complex, saw_op;
    int imprecise;   /* M27: counts are guaranteed minimums (branchy body) */
    int noreturn;    /* M28: a while-true without exit on the straight path */
    int maybe_loop;  /* M30: passed a while-true with exit — later stmts and
                        completion are not guaranteed */
    int child_cons;  /* M29: guaranteed recvs of unjoined/detached children */
    int depth;       /* M29: nested-go recursion depth (bounded) */
    struct WfChild *kids; int nkids, kidcap;   /* M29: spawned children */
} WfScan;
typedef struct WfChild {
    char hvar[64];
    WfScan sc;
    int folded;
    int detached;
} WfChild;
typedef enum { R_PROVEN, R_REFUTED, R_UNKNOWN } VRes;

/* ============================ споделено състояние ============================ */

extern int g_prod_ctr;
extern Node * g_prog;
extern const char * g_caller_name;
extern int g_call_ctr;
extern int g_handle_ctr;
extern int g_partial;
extern int g_term;
extern int g_term_failed;
extern char ** g_inv_texts;
extern int * g_inv_proven;
extern int g_inv_n;
extern int g_inv_cap;
extern int g_havoc_ctr;
extern int g_json;
extern int g_json_first;

/* ============================ константи ============================ */

#define HK_JOIN 1   /* states: 0=open, 1=joined, 2=detached */
#define HK_CHAN 2   /* states: 0=open, 1=closed */
#define HK_DROP 3   /* MEM-2: alloc→drop; states: 0=live, 1=dropped.
                       * Keyed by the SOURCE variable name (Vec/Map/bytes are
                       * opaque, not symbolic Lin values), unlike M14 handles. */
#define AK_FIT  1   /* aux1 (linear result form) must fit i64 */
#define AK_MUL  2   /* aux1 * aux2 (factor forms) must fit i64 */
#define AK_DIVZ 3   /* aux2 (divisor) != 0; not (aux1 = INT64_MIN && aux2 = -1) */

/* ============================ прототипи ============================ */

Rat rat_bad(void);   /* 35 */
Rat rat_int(int64_t n);   /* 47 */
Rat rat_zero(void);   /* 48 */
Rat rat_add(Rat a, Rat b);   /* 52 */
Rat rat_sub(Rat a, Rat b);   /* 61 */
Rat rat_mul(Rat a, Rat b);   /* 62 */
int rat_cmp(Rat a, Rat b);   /* 70 */
void lin_init(Lin *l);   /* 85 */
Lin lin_const(Rat c);   /* 109 */
Lin lin_var(const char *v);   /* 110 */
void lin_free(Lin *l);   /* 111 */
Lin lin_add(Lin *a, Lin *b);   /* 113 */
Lin lin_sub(Lin *a, Lin *b);   /* 122 */
Lin lin_scale(Lin *a, Rat k);   /* 131 */
Lin lin_neg(Lin *a);   /* 139 */
int lin_is_const(Lin *a);   /* 140 */
Lin lin_clone(Lin *a);   /* 145 */
const char *lin_handle_var(Lin *l);   /* 152 */
void env_init(SEnv *e);   /* 163 */
void env_free(SEnv *e);   /* 164 */
Binding *env_find(SEnv *e, const char *name);   /* 165 */
void env_bind(SEnv *e, const char *name, Lin lin, int nonlinear);   /* 169 */
void env_clone_into(SEnv *dst, SEnv *src);   /* 176 */
Sym sym_nonlin(void);   /* 184 */
Sym sym_lin(Lin l);   /* 185 */
void vlen_init(VLenMap *m);   /* 191 */
void vlen_free(VLenMap *m);   /* 192 */
VLenEntry *vlen_find(VLenMap *m, const char *name);   /* 193 */
void vlen_set(VLenMap *m, const char *name, Lin len, int unknown);   /* 197 */
void vlen_clone_into(VLenMap *dst, VLenMap *src);   /* 204 */
Sym se_from_ast(Node *e, SEnv *env, VLenMap *vlen, ReadsList *reads);   /* 260 */
void cl_init(ConsList *l);   /* 501 */
void cl_push(ConsList *l, Constraint c);   /* 502 */
void cl_free(ConsList *l);   /* 506 */
void f_init(Formula *f);   /* 509 */
void f_push_branch(Formula *f, ConsList cl);   /* 510 */
void f_free(Formula *f);   /* 514 */
Constraint mk_cons(Lin lhs, COp op, Rat rhs);   /* 516 */
int is_cmp(BinOp op);   /* 542 */
int is_sorted_call(Node *e);   /* 568 */
int bool_to_dnf(Node *e, SEnv *env, VLenMap *vlen, ReadsList *reads, int negated, Formula *out);   /* 573 */
int fm_sat(ConsList *sys);   /* 698 */
int64_t ienv_get(IEnv *e, const char *name, int *ok);   /* 781 */
void collect_vars_cl(ConsList *cl, char ***vars, int *nv, int *cap);   /* 843 */
int prod_concrete(ProdEntry *p, int64_t a, int64_t b, int64_t *out);   /* 853 */
void push_pins_and_derived(ConsList *sys, char **vars, IBind *assign, int nv, ProdList *prods);   /* 875 */
int find_counterexample(ConsList *ante, Formula *nef, Formula *ef, Node *ensures_ast, SEnv *output_env, ProdList *prods, IBind **witness, int *wn);   /* 942 */
void obl_push(Obligations *ob, ConsList path, ConsList bound, ConsList read_cons, Sym ret, VLenMap *vlen, ReadsList *reads, int bad, int kind, const char *label);   /* 1086 */
void obl_free(Obligation *o);   /* 1099 */
int find_bound_witness(Obligation *obl, IBind **witness, int *wn);   /* 1110 */
void ax_init(AxiomList *l);   /* 1227 */
void ax_free(AxiomList *l);   /* 1228 */
void ax_push(AxiomList *l, const char *vec, ElemCmp cmp, Lin rhs, int is_sorted);   /* 1229 */
void ax_clone_into(AxiomList *dst, AxiomList *src);   /* 1233 */
void ax_copy_renamed(AxiomList *dst, AxiomList *src, const char *from, const char *to);   /* 1239 */
void ax_intersect_two(AxiomList *dst, AxiomList *src, const char *v, const char *w, const char *to);   /* 1248 */
void reads_init(ReadsList *l);   /* 1280 */
void reads_push(ReadsList *l, Node *call, const char *var);   /* 1295 */
void reads_push_prod(ReadsList *l, const char *var, Lin fa, Lin fb);   /* 1331 */
void reads_push_div(ReadsList *l, const char *var, Lin fa, Lin fb);   /* 1338 */
void reads_push_mod(ReadsList *l, const char *var, Lin fa, Lin fb);   /* 1344 */
void reads_push_shr(ReadsList *l, const char *var, Lin fa, Lin fb);   /* 1351 */
void reads_push_lshr(ReadsList *l, const char *var, Lin fa, Lin fb);   /* 1357 */
void reads_push_bitmask(ReadsList *l, const char *var, Lin fa, int64_t mask);   /* 1363 */
void reads_push_bitop(ReadsList *l, const char *var, Lin fa, Lin fb, int is_or);   /* 1369 */
void reads_push_pair(ReadsList *l, const char *var, Lin fa, Lin fb);   /* 1376 */
ReadEntry *reads_find_pair(ReadsList *l, const char *var);   /* 1382 */
const char *reads_find(ReadsList *l, Node *call);   /* 1387 */
void reads_clone_into(ReadsList *dst, ReadsList *src);   /* 1393 */
void prods_clone_into(ProdList *dst, ReadsList *src);   /* 1420 */
void prods_free(ProdList *l);   /* 1440 */
void handles_init(HandleList *l);   /* 1480 */
void handles_free(HandleList *l);   /* 1481 */
HandleEntry *handles_find(HandleList *l, const char *var);   /* 1489 */
void handles_set(HandleList *l, const char *var, int kind, int state, Lin *result);   /* 1494 */
void handles_clone_into(HandleList *dst, HandleList *src);   /* 1519 */
void state_free(State *s);   /* 1543 */
void states_push(States *ss, State s);   /* 1546 */
void expr_render(Node *e, char *b, size_t n, size_t *o);   /* 1571 */
void inv_collect_reset(void);   /* 1618 */
Node *find_fn(Node *prog, const char *name);   /* 1641 */
int callee_sig_supported(Node *cfn);   /* 1655 */
int is_vec_builtin_call(Node *n);   /* 1668 */
int is_par_builtin_call(Node *n);   /* 1688 */
int is_drop_call(Node *n);   /* 1704 */
int is_mem_alloc_call(Node *n);   /* 1715 */
int has_unsupported(Node *n);   /* 1821 */
int symexec_while(Node *st, States *states, Obligations *ob, int is_nonvoid, Node *spec);   /* 1928 */
const char *resolve_key(Node *act, State *st, char *buf, size_t n);   /* 2064 */
void ax_drop_key(AxiomList *ax, const char *key);   /* 2098 */
int extract_axiom_resolved(Node *e, State *st);   /* 2125 */
void scan_vec_expr(Node *e, State *st, Obligations *ob, Node *spec);   /* 2182 */
int cl_implied_by(ConsList *path, ConsList *cons);   /* 2301 */
int is_user_call(Node *e);   /* 2314 */
int lin_eq(Lin *a, Lin *b);   /* 2322 */
void path_push_ge(ConsList *path, const char *pv, int64_t K);   /* 2352 */
void path_push_le(ConsList *path, const char *pv, int64_t K);   /* 2361 */
void inject_prod_axioms(State *st);   /* 2395 */
Sym eval_user_call(Node *call, State *st, Obligations *ob, const char *caller_name);   /* 2797 */
void push_protocol_violation(State *st, Obligations *ob, const char *label);   /* 2926 */
void push_protocol_ok(State *st, Obligations *ob, const char *label);   /* 2936 */
Sym eval_par_call(Node *call, State *st, Obligations *ob);   /* 3588 */
void eval_drop_call(Node *call, State *st, Obligations *ob);   /* 3817 */
void check_drop_uses(Node *e, State *st, Obligations *ob);   /* 3860 */
void scan_arith_expr(Node *e, State *st, Obligations *ob);   /* 3891 */
void push_term_entry_obl(Node *fn, Node *spec, State *init, Obligations *ob);   /* 3970 */
void symexec_block(Node *blk, States *states, Obligations *ob, int is_nonvoid, Node *spec);   /* 3995 */
State clone_state_with(State *cur, Formula *add);   /* 4003 */
void symexec_stmts(NodeVec *stmts, States *states, Obligations *ob, int is_nonvoid, Node *spec);   /* 4041 */
Node *find_spec(Node *prog, const char *name);   /* 4258 */
int type_is_i64(Node *t);   /* 4272 */
int ret_has_effects(Node *t);   /* 4276 */
int ret_has_unverifiable_effects(Node *t);   /* 4292 */
int has_elem_ref(Node *e);   /* 4825 */
void extract_elem_axiom(Node *e, SEnv *env, AxiomList *ax);   /* 4836 */
Constraint axiom_instantiate(ElemAxiom *a, const char *read_var);   /* 4870 */
int axiom_holds_for_value(ElemAxiom *a, Sym *val, State *st);   /* 4890 */
int verify_program(Node *prog);   /* 5432 */

/* ============================ M31: пълен BV фрагмент ============================
 * Bit-blasting до 64-битов CNF + in-house DPLL (verify_bv_sat.c) за цели,
 * които FM ядрото не може да линеаризира (променлив XOR, wrap като стойност,
 * символни маски/shift-ове). Изолиран от ℤ ядрото: стартира САМО когато
 * FM е казал НЕ МОГА, и само върху straight-line i64 функции без деление,
 * извиквания и цикли. Бюджетите (гейтове/конфликти) пазят честността —
 * надхвърлят ли се, остава UNKNOWN. */

typedef struct { int *lits; int n; } BvClause;
typedef enum { BV_SAT_SAT, BV_SAT_UNSAT, BV_SAT_UNDECIDED } BvSatResult;
#define BV_SAT_DEFAULT_BUDGET 30000L
typedef struct {
    int n_vars, cap_vars;
    signed char *assign;   /* 0 = неопределена, 1 = истина, -1 = лъжа */
    int *level;            /* var → ниво на решение */
    int *trail, n_trail, cap_trail;
    int *trail_lim, n_lim, cap_lim;
    int *flipped;          /* ниво → опитана ли е обратната фаза */
    int q_head;            /* worklist указател в trail */
    int phase_mode;        /* 0 = положителна, 1 = отрицателна, 2 = алтернативна */
    BvClause *cl; int n_cl, cap_cl;
    int **occ; int *occ_n; int occ_cap;   /* occurrence списъци по лит. */
    long conflicts, budget;
} BvSat;
void bv_sat_init(BvSat *s, int n_vars_hint, long conflict_budget);
void bv_sat_free(BvSat *s);
int bv_sat_new_var(BvSat *s);
void bv_sat_add_clause(BvSat *s, const int *lits, int n);
BvSatResult bv_sat_solve(BvSat *s);
int bv_sat_var_value(BvSat *s, int v);
void bv_sat_reset(BvSat *s);

/* 64-битова дума = 64 SAT литерала (lit[0] = LSB). 0/1 лит. са материални
 * константи (var 1 = TRUE, var 2 = FALSE, assert-нати). */
typedef struct { int lit[64]; } BvWord;

#define BV_GATE_BUDGET 60000   /* таван на гейтовете в една цел */

/* M31 oracle за едно ensures твърдение върху fn (spec, индекс).
 * Връща: 1 = ДОКАЗАНО (UNSAT), 2 = ОБРОЧЕНО (SAT + презареден witness),
 * 0 = извън фрагмента/бюджета — остава UNKNOWN. Witness-ът е за
 * параметрите на fn-а (names/vals като в EnsVerifyRes). */
int bv_verify_ensures(Node *fn, Node *spec, int ens_idx,
                      char ***wit_names, long long **wit_vals, int *wn);

/* Директен интерпретатор (verify_obl.c) — M8 презареждане на свидетели;
 * ползва се и от BV recheck-а. */
int64_t eval_i64(Node *e, IEnv *env, int *ok);
int eval_bool(Node *e, IEnv *env);

#endif /* BAGA_VERIFY_IMPL_H */
