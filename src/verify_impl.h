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
typedef struct {
    char *vec; ElemCmp cmp; Lin rhs; int is_sorted;
    /* W2.8: 0 = скаларна/sorted аксиома; 1 = world_frozen(елемента);
     * 3 = world_owned(елемента). Светът не е скалар, затова аксиомата не
     * става Constraint — при recv регистрира HK_WORLD за получената стойност,
     * а при send оцелява само ако payload-ът наистина е такъв свят. */
    int world_kind;
} ElemAxiom;
typedef struct { ElemAxiom *a; int n, cap; } AxiomList;
typedef struct {
    char *var; int kind; int state; Lin result;
    /* M19 wait-for: join handle records the worker's first blocking op on
     * the channel argument (if any). Channel handles count this thread's
     * send/recv and anonymous (go_bg) producers. */
    char *worker;     /* HK_JOIN: worker fn name (owned) */
    char *chan_arg;   /* HK_JOIN: channel ghost var, or NULL */
    char *world_arg;  /* HK_JOIN: W2.4 споделен замразен свят (joinable share) */
    int n_shared;     /* HK_WORLD: W2.4 брой незавършили читатели на споделен
                       * замразен свят. frozen + go/chan_send = share (не move);
                       * join декрементира; world_free при n_shared > 0 е
                       * ОБРОЧЕНО — иначе free-ът съблича региона под читателите. */
    int borrowed;     /* HK_WORLD: W2.4 handle-ът е ЗАЕМАН (получател с
                       * `requires: world_frozen`) — право на четене, не
                       * собственост: world_free и препредаване са ОБРОЧЕНИ. */
    int wcontract;    /* HK_WORLD: W2.7 договорът на параметъра —
                       * 0 = няма/собствен, 1 = world_frozen (заемане за четене),
                       * 2 = world_mutable (запис по време на повикването,
                       *     собствеността остава у извикващия),
                       * 3 = world_owned (прехвърлена собственост). */
    char **links;     /* HK_WORLD: W2.5 capability връзки owner → target
                       * (ghost имена, owned). Целта надживява връзката: free
                       * или предаване на свързан свят е ОБРОЧЕНО; цикъл също. */
    int n_links;
    int cap_links;
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
    /* W2.12: канални споделби. chan_send на замразен свят го споделя; ако
     * каналът остане проследим (close + всички носители join-нати + не е минал
     * през не-join-нат път), споделбите му изтичат при world_free. */
    char **chs;       /* HK_WORLD: канали, в които светът е изпратен замразен */
    int *chsn;        /* HK_WORLD: брой споделби по всеки канал */
    int n_chs, cap_chs;
    char *from_chan;  /* HK_WORLD: светът е получен от този канал (chan_recv) */
    int wleak;        /* HK_CHAN: минал през не-join-нат/непроследим път
                       * (go_bg, cell2 пакет, chan_send пейлоад, връщане) —
                       * каналните споделби не изтичат */
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
extern int g_recv_world_kind;   /* W2.8 */
extern char g_recv_world_chan[64];   /* W2.12: from_chan при chan_recv */
/* W3.1: гаранция за върнат свят (`ensures: world_frozen(output)` /
 * `world_owned(output)`). g_guar_kind = какво обещава spec-ът (0/1/3);
 * drain_returns проверява ВСЕКИ върнат handle и push-ва kind-5 задължение при
 * несъответствие; g_guar_unknown = има път с неследен върнат handle (честен
 * UNKNOWN, никога ДОКАЗАНО). g_ret_world_kind е caller-страната: eval_user_call
 * го слага, LET-вързането го консумира и регистрира резултата като HK_WORLD. */
extern int g_guar_kind;
extern int g_guar_unknown;
extern int g_ret_world_kind;
extern int g_json;
extern int g_json_first;

/* ============================ константи ============================ */

#define HK_JOIN 1   /* states: 0=open, 1=joined, 2=detached */
#define HK_CHAN 2   /* states: 0=open, 1=closed */
#define HK_DROP 3   /* MEM-2: alloc→drop; states: 0=live, 1=dropped.
                       * Keyed by the SOURCE variable name (Vec/Map/bytes are
                       * opaque, not symbolic Lin values), unlike M14 handles. */
#define HK_WORLD 4  /* W0: светове — mutable(0) → frozen(1) → freed(2);
                       * 3 = moved (собствеността е прехвърлена през chan_send).
                       * Нарушения: mutate-after-freeze (alloc върху 1),
                       * use-after-free (2), use-after-move (3), повторен
                       * freeze/free. Keyed by source variable name като
                       * HK_DROP; светът е opaque i64 handle, не Lin. */
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
int is_world_ghost_call(Node *e);   /* W2.1: world_frozen(w) — ghost предикат */
int world_contract_of(Node *e);   /* W2.7: 0/1=frozen/2=mutable/3=owned */
int spec_world_contract(Node *spec, Node *wfn, int idx);   /* W2.7: договор за параметър */
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
void ax_push(AxiomList *l, const char *vec, ElemCmp cmp, Lin rhs, int is_sorted, int world_kind);   /* 1229 */
int axiom_world_entailed(State *st, const char *key, int kind);   /* W2.8 */
int world_axiom_holds(State *st, Node *payload, int kind);   /* W2.8 */
int is_world_contract_call(Node *e);   /* W2.8: име на договор, без значение аргументът */
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
void wlink_add(HandleEntry *o, const char *target);   /* W2.5 */
/* W2.12: канални споделби на замразен свят */
void chshare_add(HandleEntry *e, const char *chan, int n);
void chshare_expire(HandleList *l, HandleEntry *e);   /* при world_free */
void chan_escape_stmt(State *st, Node *n);   /* W2.12: маркирай wleak */
void wlink_del(HandleEntry *o, const char *target);   /* W2.5 */
void wlink_clear(HandleEntry *o);   /* W2.5: връзките на освободен owner */
const char *wlink_owner_of(HandleList *l, const char *target);   /* W2.5: обратна връзка */
int wlink_reaches(HandleList *l, const char *from, const char *to);   /* W2.5: цикъл */
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
int is_world_new_call(Node *n);   /* W0: world_new() */
int is_world_scratch_call(Node *n);   /* W2.3: wcache_* — вътрешна променяемост */
int is_world_op_call(Node *n);    /* W0: world_alloc/world_freeze/world_free — пипат състоянието */
int is_world_expr_call(Node *n);  /* W0: world_new/world_alloc/world_state — изразни */
int is_world_stmt_call(Node *n);  /* W0: world_freeze/world_free — statement-level (като drop) */
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
/* W3.1: kind-5 задължение — нарушена гаранция за върнат свят. Като kind 3
 * (path witness), но се докладва като ensures ред, не като протокол. */
void push_guar_violation(State *st, Obligations *ob, const char *label);
void push_unjoined_delegation_obls(States *states, Obligations *ob);   /* W2.6: изход без return */
void push_protocol_ok(State *st, Obligations *ob, const char *label);   /* 2936 */
Sym eval_par_call(Node *call, State *st, Obligations *ob);   /* 3588 */
void eval_drop_call(Node *call, State *st, Obligations *ob);   /* 3817 */
void eval_world_call(Node *call, State *st, Obligations *ob);  /* W0: HK_WORLD преходи */
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
void extract_world_ghost(Node *e, State *st);   /* W2.1: requires world_frozen(p) → HK_WORLD(1) */
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
