/* verify_bv.c — M31: пълен bitvector фрагмент (thesis-open-problems §2,
 * option 1: bounded BV on demand).
 *
 * Когато FM ядрото каже „НЕ МОГА ДА РЕША" за ensures върху права i64
 * функция (без цикли/извиквания/деление), целта се bit-blast-ва до
 * 64-битов CNF и се решава от in-house DPLL (verify_bv_sat.c). Семантиката
 * е ТОЧНА: two's-complement wrap на +/−, маскиран брой на shift-овете
 * (b & 63), signed сравнения — същата като в двата backend-а.
 *
 * Soundness дисциплина (M8):
 *   - UNSAT ⇒ ДОКАЗАНО — само при точно моделиран фрагмент (definitely-
 *     return анализ, без неизвестни конструкции) и изчерпано търсене;
 *     бюджети (гейтове/променливи/конфликти) ⇒ честен UNKNOWN;
 *   - SAT ⇒ witness-ът се ПРЕЗАРЕЖДА през директна интерпретация на
 *     тялото (eval_i64/eval_bool): изискваме requires ∧ ¬ensures(output)
 *     да са истински за конкретните параметри; иначе UNKNOWN (параноя
 *     срещу разминаване blast/интерпретатор — никога фалшиво ОБРОЧЕНО).
 *
 * Изолиран от ℤ ядрото: не пипа Obligations/Lin/SEnv — стартира само като
 * fallback на verify_ensures (verify_top.c).
 */

#include "baga.h"
#include "verify_impl.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define BV_T 1            /* var 1 = TRUE (assert-нат при init) */
#define BV_F 2            /* var 2 = FALSE (assert-нат при init) */
#define BV_VAR_BUDGET 40000

typedef struct {
    BvSat sat;
    int gates;
    int failed;             /* извън фрагмента/бюджета → честен UNKNOWN */
    /* symbol env: име → 64-битова дума */
    char **names; BvWord *vals; int n_env, cap_env;
    /* изход: mux по path conditions на return-ите */
    BvWord ret; int ret_cond; int has_ret; int pc;
    /* Регистър на операндите на портите (по var) — за структурен колапс
     * при изграждането: (a^b)^b = a, a∧(a∨b) = a, a∨(a∧b) = a. Без него
     * чистият DPLL (без clause learning) е експоненциален върху parity/
     * дистрибутивни формули — колапсът ги свива до тривиални преди solve. */
    int *op_kind;           /* 0 = не е регистрирана, 1=and, 2=or, 3=xor */
    int *op_a, *op_b;
    int op_cap;
} BvCtx;

/* ============================ контекст ============================ */

static void bv_ctx_init(BvCtx *c) {
    memset(c, 0, sizeof *c);
    bv_sat_init(&c->sat, 512, 0);
    c->pc = BV_T;
    c->ret_cond = BV_F;
}

static void bv_assert_lit(BvCtx *c, int lit) {
    int cl[1] = { lit };
    bv_sat_add_clause(&c->sat, cl, 1);
}

static void bv_ctx_free(BvCtx *c) {
    for (int i = 0; i < c->n_env; i++) free(c->names[i]);
    free(c->names); free(c->vals);
    free(c->op_kind); free(c->op_a); free(c->op_b);
    bv_sat_free(&c->sat);
}

static void op_record(BvCtx *c, int g, int kind, int a, int b) {
    if (g >= c->op_cap) {
        int nc = c->op_cap ? c->op_cap * 2 : 256;
        while (nc <= g) nc *= 2;
        c->op_kind = realloc(c->op_kind, (size_t)nc * sizeof *c->op_kind);
        c->op_a = realloc(c->op_a, (size_t)nc * sizeof *c->op_a);
        c->op_b = realloc(c->op_b, (size_t)nc * sizeof *c->op_b);
        memset(c->op_kind + c->op_cap, 0, (size_t)(nc - c->op_cap) * sizeof *c->op_kind);
        c->op_cap = nc;
    }
    c->op_kind[g] = kind;
    c->op_a[g] = a;
    c->op_b[g] = b;
}

/* a е порта от даден вид с единия операнд == b? Връща другия операнд или 0. */
static int op_match(BvCtx *c, int a, int kind, int b) {
    if (a <= 0 || a >= c->op_cap || c->op_kind[a] != kind) return 0;
    if (c->op_a[a] == b) return c->op_b[a];
    if (c->op_b[a] == b) return c->op_a[a];
    return 0;
}

/* env_bind копира името; env_find връща указател към думата или NULL. */
static void bv_env_bind(BvCtx *c, const char *name, BvWord w) {
    for (int i = 0; i < c->n_env; i++) {
        if (strcmp(c->names[i], name) == 0) { c->vals[i] = w; return; }
    }
    if (c->n_env == c->cap_env) {
        c->cap_env = c->cap_env ? c->cap_env * 2 : 16;
        c->names = realloc(c->names, (size_t)c->cap_env * sizeof *c->names);
        c->vals = realloc(c->vals, (size_t)c->cap_env * sizeof *c->vals);
    }
    c->names[c->n_env] = strdup(name);
    c->vals[c->n_env] = w;
    c->n_env++;
}

static BvWord *bv_env_find(BvCtx *c, const char *name) {
    for (int i = 0; i < c->n_env; i++)
        if (strcmp(c->names[i], name) == 0) return &c->vals[i];
    return NULL;
}

/* ============================ порти (Tseytin) ============================ */

static int bv_budget_ok(BvCtx *c, int add) {
    if (c->failed) return 0;
    c->gates += add;
    if (c->gates > BV_GATE_BUDGET || c->sat.n_vars > BV_VAR_BUDGET) {
        c->failed = 1;
        return 0;
    }
    return 1;
}

/* Tseytin: отрицанието е БЕЗПЛАТНО — литералът -a. Отделна порта би
 * блокирала a == -b колапсите (напр. x − x не се свива до 0) и би
 * оскъпила всяка верига. */
static int g_not(BvCtx *c, int a) {
    (void)c;
    return -a;
}

static int g_and(BvCtx *c, int a, int b) {
    if (c->failed) return BV_F;
    if (a == BV_F || b == BV_F) return BV_F;
    if (a == BV_T) return b;
    if (b == BV_T) return a;
    if (a == b) return a;
    if (a == -b) return BV_F;
    /* абсорбция: (a∨x) ∧ a = a — ако a е OR-порта, съдържаща b, резултатът
     * е b; ако b е OR-порта, съдържаща a — a */
    if (op_match(c, a, 2, b)) return b;
    if (op_match(c, b, 2, a)) return a;
    if (!bv_budget_ok(c, 1)) return BV_F;
    int g = bv_sat_new_var(&c->sat);
    int c1[2] = { -g, a }, c2[2] = { -g, b }, c3[3] = { g, -a, -b };
    bv_sat_add_clause(&c->sat, c1, 2);
    bv_sat_add_clause(&c->sat, c2, 2);
    bv_sat_add_clause(&c->sat, c3, 3);
    op_record(c, g, 1, a, b);
    return g;
}

static int g_or(BvCtx *c, int a, int b) {
    if (c->failed) return BV_F;
    if (a == BV_T || b == BV_T) return BV_T;
    if (a == BV_F) return b;
    if (b == BV_F) return a;
    if (a == b) return a;
    if (a == -b) return BV_T;
    /* абсорбция: (a∧x) ∨ a = a */
    if (op_match(c, a, 1, b)) return b;
    if (op_match(c, b, 1, a)) return a;
    if (!bv_budget_ok(c, 1)) return BV_F;
    int g = bv_sat_new_var(&c->sat);
    int c1[2] = { g, -a }, c2[2] = { g, -b }, c3[3] = { -g, a, b };
    bv_sat_add_clause(&c->sat, c1, 2);
    bv_sat_add_clause(&c->sat, c2, 2);
    bv_sat_add_clause(&c->sat, c3, 3);
    op_record(c, g, 2, a, b);
    return g;
}

static int g_xor(BvCtx *c, int a, int b) {
    if (c->failed) return BV_F;
    if (a == BV_F) return b;
    if (b == BV_F) return a;
    if (a == BV_T) return g_not(c, b);
    if (b == BV_T) return g_not(c, a);
    if (a == b) return BV_F;
    if (a == -b) return BV_T;
    /* (a^b)^b = a — колапс на xor-вериги при изграждането */
    {
        int m = op_match(c, a, 3, b);
        if (m) return m;
        m = op_match(c, b, 3, a);
        if (m) return m;
    }
    if (!bv_budget_ok(c, 1)) return BV_F;
    int g = bv_sat_new_var(&c->sat);
    int c1[3] = { -g, -a, -b }, c2[3] = { -g, a, b };
    int c3[3] = { g, -a, b }, c4[3] = { g, a, -b };
    bv_sat_add_clause(&c->sat, c1, 3);
    bv_sat_add_clause(&c->sat, c2, 3);
    bv_sat_add_clause(&c->sat, c3, 3);
    bv_sat_add_clause(&c->sat, c4, 3);
    op_record(c, g, 3, a, b);
    return g;
}

static int g_ite(BvCtx *c, int s, int a, int b) {
    if (c->failed) return BV_F;
    if (s == BV_T) return a;
    if (s == BV_F) return b;
    if (a == b) return a;
    /* Константно рамо → чиста конюнкция/дизюнкция (barrel shifter-ите
     * се свиват драстично: ite(m, F, x) = ¬m ∧ x). */
    if (a == BV_F) return g_and(c, -s, b);
    if (b == BV_F) return g_and(c, s, a);
    if (a == BV_T) return g_or(c, -s, b);
    if (b == BV_T) return g_or(c, s, a);
    if (!bv_budget_ok(c, 1)) return BV_F;
    int g = bv_sat_new_var(&c->sat);
    int c1[3] = { -s, -g, a }, c2[3] = { -s, g, -a };
    int c3[3] = { s, -g, b }, c4[3] = { s, g, -b };
    bv_sat_add_clause(&c->sat, c1, 3);
    bv_sat_add_clause(&c->sat, c2, 3);
    bv_sat_add_clause(&c->sat, c3, 3);
    bv_sat_add_clause(&c->sat, c4, 3);
    return g;
}

/* ============================ думи (64 бита) ============================ */

static BvWord w_const(int64_t v) {
    BvWord w;
    uint64_t u = (uint64_t)v;
    for (int i = 0; i < 64; i++) w.lit[i] = (u >> i) & 1 ? BV_T : BV_F;
    return w;
}

static BvWord w_fresh(BvCtx *c) {
    BvWord w;
    memset(&w, 0, sizeof w);
    if (!bv_budget_ok(c, 64)) return w;
    /* MSB първи (най-нисък var номер): decide_var взима най-ниската
     * неопределена променлива → входните битове се решават MSB-first —
     * силната посока за signed сравнения и shift-witness-и. */
    for (int i = 63; i >= 0; i--) w.lit[i] = bv_sat_new_var(&c->sat);
    return w;
}

static BvWord w_not(BvCtx *c, BvWord a) {
    BvWord r;
    for (int i = 0; i < 64; i++) r.lit[i] = g_not(c, a.lit[i]);
    return r;
}
static BvWord w_and(BvCtx *c, BvWord a, BvWord b) {
    BvWord r;
    for (int i = 0; i < 64; i++) r.lit[i] = g_and(c, a.lit[i], b.lit[i]);
    return r;
}
static BvWord w_or(BvCtx *c, BvWord a, BvWord b) {
    BvWord r;
    for (int i = 0; i < 64; i++) r.lit[i] = g_or(c, a.lit[i], b.lit[i]);
    return r;
}
static BvWord w_xor(BvCtx *c, BvWord a, BvWord b) {
    BvWord r;
    for (int i = 0; i < 64; i++) r.lit[i] = g_xor(c, a.lit[i], b.lit[i]);
    return r;
}
static BvWord w_ite(BvCtx *c, int s, BvWord a, BvWord b) {
    BvWord r;
    for (int i = 0; i < 64; i++) r.lit[i] = g_ite(c, s, a.lit[i], b.lit[i]);
    return r;
}

/* ripple-carry събиране с carry-in литерал (wrap е естествен: 65-ият
 * carry излиза от думата — two's-complement mod 2^64). */
static BvWord w_add(BvCtx *c, BvWord a, BvWord b, int cin) {
    BvWord r;
    int carry = cin;
    for (int i = 0; i < 64; i++) {
        int x = g_xor(c, a.lit[i], b.lit[i]);
        r.lit[i] = g_xor(c, x, carry);
        carry = g_or(c, g_and(c, a.lit[i], b.lit[i]), g_and(c, x, carry));
    }
    return r;
}
static BvWord w_sub(BvCtx *c, BvWord a, BvWord b) {
    return w_add(c, a, w_not(c, b), BV_T);
}
static BvWord w_neg(BvCtx *c, BvWord a) {
    return w_sub(c, w_const(0), a);
}

static int w_eq_lit(BvCtx *c, BvWord a, BvWord b) {
    int e = BV_T;
    for (int i = 0; i < 64; i++)
        e = g_and(c, e, g_not(c, g_xor(c, a.lit[i], b.lit[i])));
    return e;
}

/* signed a < b: sign(a−b) XOR overflow(a−b) */
static int w_slt_lit(BvCtx *c, BvWord a, BvWord b) {
    BvWord d = w_sub(c, a, b);
    int sign = d.lit[63];
    int ovf = g_and(c, g_xor(c, a.lit[63], b.lit[63]),
                    g_xor(c, d.lit[63], a.lit[63]));
    return g_xor(c, sign, ovf);
}

/* Константни shift-ове — само пренареждане на жици (нула гейта).
 * Броят се маскира (k & 63) — семантиката на езика в двата backend-а. */
static BvWord w_shlc(BvWord a, int k) {
    BvWord r; k &= 63;
    for (int i = 0; i < 64; i++) r.lit[i] = i >= k ? a.lit[i - k] : BV_F;
    return r;
}
static BvWord w_ashrc(BvWord a, int k) {
    BvWord r; k &= 63;
    for (int i = 0; i < 64; i++) r.lit[i] = i + k < 64 ? a.lit[i + k] : a.lit[63];
    return r;
}
static BvWord w_lshrc(BvWord a, int k) {
    BvWord r; k &= 63;
    for (int i = 0; i < 64; i++) r.lit[i] = i + k < 64 ? a.lit[i + k] : BV_F;
    return r;
}

/* Променлив брой: barrel mux по битовете на маскирания count (6 слоя;
 * аритметичната композиция ashr(2^i)∘… е точна — знакът се носи от
 * текущия out[63] на всеки слой). kind: 0 = <<, 1 = >> (arith), 2 = >>>. */
static BvWord w_shift_var(BvCtx *c, BvWord a, BvWord cnt, int kind) {
    BvWord m = w_and(c, cnt, w_const(63));
    BvWord out = a;
    for (int j = 0; j < 6; j++) {
        BvWord sh = kind == 0 ? w_shlc(out, 1 << j)
                    : kind == 1 ? w_ashrc(out, 1 << j)
                    : w_lshrc(out, 1 << j);
        for (int i = 0; i < 64; i++)
            out.lit[i] = g_ite(c, m.lit[j], sh.lit[i], out.lit[i]);
    }
    return out;
}

/* ============================ изрази ============================ */

static BvWord bv_eval_i64(BvCtx *c, Node *e);
static int bv_eval_bool(BvCtx *c, Node *e);

static BvWord bv_eval_i64(BvCtx *c, Node *e) {
    BvWord bad;
    memset(&bad, 0, sizeof bad);
    if (!e || c->failed) return bad;
    switch (e->kind) {
    case NODE_INT_LIT:  return w_const(e->int_val);
    case NODE_BOOL_LIT: {
        BvWord w = w_const(0);
        w.lit[0] = e->bool_val ? BV_T : BV_F;
        return w;
    }
    case NODE_IDENT: {
        BvWord *w = bv_env_find(c, e->name);
        if (!w) { c->failed = 1; return bad; }
        return *w;
    }
    case NODE_UNARY:
        if (e->un_op == UOP_NEG) return w_neg(c, bv_eval_i64(c, e->operand));
        if (e->un_op == UOP_NOT) {
            BvWord w = w_const(0);
            w.lit[0] = g_not(c, bv_eval_bool(c, e->operand));
            return w;
        }
        c->failed = 1; return bad;
    case NODE_BINARY: {
        if (e->bin_op == OP_LSHIFT || e->bin_op == OP_RSHIFT || e->bin_op == OP_URSHIFT) {
            BvWord a = bv_eval_i64(c, e->left);
            if (c->failed) return bad;
            int kind = e->bin_op == OP_LSHIFT ? 0 : e->bin_op == OP_RSHIFT ? 1 : 2;
            if (e->right && e->right->kind == NODE_INT_LIT) {
                int k = (int)(e->right->int_val & 63);
                return kind == 0 ? w_shlc(a, k) : kind == 1 ? w_ashrc(a, k) : w_lshrc(a, k);
            }
            BvWord b = bv_eval_i64(c, e->right);
            if (c->failed) return bad;
            return w_shift_var(c, a, b, kind);
        }
        BvWord a = bv_eval_i64(c, e->left);
        BvWord b = bv_eval_i64(c, e->right);
        if (c->failed) return bad;
        switch (e->bin_op) {
        case OP_ADD: return w_add(c, a, b, BV_F);
        case OP_SUB: return w_sub(c, a, b);
        case OP_BIT_AND: return w_and(c, a, b);
        case OP_BIT_OR:  return w_or(c, a, b);
        case OP_BIT_XOR: return w_xor(c, a, b);
        default: c->failed = 1; return bad;   /* *, /, % — извън BV v1 */
        }
    }
    default: c->failed = 1; return bad;
    }
}

static int bv_eval_bool(BvCtx *c, Node *e) {
    if (!e || c->failed) return BV_F;
    switch (e->kind) {
    case NODE_BOOL_LIT: return e->bool_val ? BV_T : BV_F;
    case NODE_IDENT: {
        BvWord *w = bv_env_find(c, e->name);
        if (!w) { c->failed = 1; return BV_F; }
        return w->lit[0];
    }
    case NODE_UNARY:
        if (e->un_op == UOP_NOT) return g_not(c, bv_eval_bool(c, e->operand));
        c->failed = 1; return BV_F;
    case NODE_BINARY:
        if (e->bin_op == OP_AND)
            return g_and(c, bv_eval_bool(c, e->left), bv_eval_bool(c, e->right));
        if (e->bin_op == OP_OR)
            return g_or(c, bv_eval_bool(c, e->left), bv_eval_bool(c, e->right));
        {
            BvWord a = bv_eval_i64(c, e->left);
            BvWord b = bv_eval_i64(c, e->right);
            if (c->failed) return BV_F;
            switch (e->bin_op) {
            case OP_EQ:  return w_eq_lit(c, a, b);
            case OP_NEQ: return g_not(c, w_eq_lit(c, a, b));
            case OP_LT:  return w_slt_lit(c, a, b);
            case OP_GT:  return w_slt_lit(c, b, a);
            case OP_LE:  return g_not(c, w_slt_lit(c, b, a));
            case OP_GE:  return g_not(c, w_slt_lit(c, a, b));
            default: c->failed = 1; return BV_F;
            }
        }
    default: c->failed = 1; return BV_F;
    }
}

/* ============================ statements (фрагмент) ============================
 * bv_stmt/bv_stmts връщат 1 когато пътеката ОПРЕДЕЛЕНО връща (definitely
 * returns). Oracle-ът изисква definitely-return на целия fn — иначе
 * изходната дума е частична и PROVEN би бил не sound. */

static int bv_stmts(BvCtx *c, NodeVec *stmts);

static int bv_stmt(BvCtx *c, Node *s) {
    if (!s || c->failed) return 0;
    switch (s->kind) {
    case NODE_BLOCK: return bv_stmts(c, &s->stmts);
    case NODE_LET: {
        BvWord w = s->let_init ? bv_eval_i64(c, s->let_init) : w_const(0);
        if (c->failed) return 0;
        bv_env_bind(c, s->let_name, w);
        return 0;
    }
    case NODE_ASSIGN: {
        if (!s->assign_target || s->assign_target->kind != NODE_IDENT) { c->failed = 1; return 0; }
        if (!bv_env_find(c, s->assign_target->name)) { c->failed = 1; return 0; }
        BvWord w = bv_eval_i64(c, s->assign_val);
        if (c->failed) return 0;
        bv_env_bind(c, s->assign_target->name, w);
        return 0;
    }
    case NODE_IF: {
        int cond = bv_eval_bool(c, s->cond);
        if (c->failed) return 0;
        int mark = c->n_env;
        BvWord *snap = malloc((size_t)(mark > 0 ? mark : 1) * sizeof *snap);
        for (int i = 0; i < mark; i++) snap[i] = c->vals[i];
        int saved_pc = c->pc, saved_rc = c->ret_cond, saved_hr = c->has_ret;
        BvWord saved_ret = c->ret;

        /* then клон */
        c->pc = g_and(c, saved_pc, cond);
        int then_d = bv_stmts(c, &s->then_br->stmts);
        int n_then = c->n_env;
        BvWord *then_vals = malloc((size_t)(n_then > 0 ? n_then : 1) * sizeof *then_vals);
        for (int i = 0; i < n_then; i++) then_vals[i] = c->vals[i];
        BvWord then_ret = c->ret;
        int then_rc = c->ret_cond, then_hr = c->has_ret;

        /* restore за else клона; branch-local имена (над mark) излизат от scope */
        for (int i = mark; i < c->n_env; i++) free(c->names[i]);
        c->n_env = mark;
        for (int i = 0; i < mark; i++) c->vals[i] = snap[i];
        c->pc = g_and(c, saved_pc, g_not(c, cond));
        c->ret = saved_ret; c->ret_cond = saved_rc; c->has_ret = saved_hr;
        int else_d = 0;
        if (s->else_br) else_d = bv_stmts(c, &s->else_br->stmts);

        /* merge: пред-съществуващите имена — ite(cond, then, else) */
        for (int i = 0; i < mark; i++)
            c->vals[i] = w_ite(c, cond, then_vals[i], c->vals[i]);
        for (int i = mark; i < c->n_env; i++) free(c->names[i]);
        c->n_env = mark;
        /* ret merge (стойностите на непередаващи се клонове са saved_*) */
        if (then_hr || c->has_ret) {
            c->ret = w_ite(c, cond, then_hr ? then_ret : saved_ret,
                           c->has_ret ? c->ret : saved_ret);
            c->ret_cond = g_or(c, then_rc, c->ret_cond);
            c->has_ret = 1;
        }
        c->pc = saved_pc;
        free(snap); free(then_vals);
        return then_d && else_d;
    }
    case NODE_RETURN: {
        BvWord w = s->ret_val ? bv_eval_i64(c, s->ret_val) : w_const(0);
        if (c->failed) return 0;
        if (!c->has_ret) {
            c->ret = w;
            c->ret_cond = c->pc;
            c->has_ret = 1;
        } else {
            c->ret = w_ite(c, c->pc, w, c->ret);
            c->ret_cond = g_or(c, c->ret_cond, c->pc);
        }
        return 1;
    }
    default:
        c->failed = 1;   /* цикли, match, извиквания, ефекти — извън BV v1 */
        return 0;
    }
}

static int bv_stmts(BvCtx *c, NodeVec *stmts) {
    int def_ret = 0;
    for (int i = 0; i < stmts->len; i++) {
        if (c->failed) return 0;
        int r = bv_stmt(c, stmts->data[i]);
        if (r) def_ret = 1;
        /* нищо след return в същия блок — pc-mux-ът не различава
         * последователни return-и (консервативно) */
        if (stmts->data[i] && stmts->data[i]->kind == NODE_RETURN && i + 1 < stmts->len) {
            c->failed = 1;
            return 0;
        }
    }
    return def_ret;
}

/* ============================ конкретна интерпретация (M8 recheck) ====== */

static void ienv_bind_local(IEnv *env, const char *name, int64_t val) {
    for (int i = 0; i < env->n; i++)
        if (strcmp(env->b[i].name, name) == 0) { env->b[i].val = val; return; }
    env->b = realloc(env->b, (size_t)(env->n + 1) * sizeof *env->b);
    env->b[env->n].name = strdup(name);
    env->b[env->n].val = val;
    env->n++;
}

static int bv_interp(NodeVec *stmts, IEnv *env, int64_t *ret, int *has_ret) {
    for (int i = 0; i < stmts->len; i++) {
        Node *s = stmts->data[i];
        switch (s->kind) {
        case NODE_BLOCK:
            if (!bv_interp(&s->stmts, env, ret, has_ret)) return 0;
            break;
        case NODE_LET: {
            int ok = 1;
            int64_t v = s->let_init ? eval_i64(s->let_init, env, &ok) : 0;
            if (!ok) return 0;
            ienv_bind_local(env, s->let_name, v);
            break;
        }
        case NODE_ASSIGN: {
            if (!s->assign_target || s->assign_target->kind != NODE_IDENT) return 0;
            int ok = 1;
            int64_t v = eval_i64(s->assign_val, env, &ok);
            if (!ok) return 0;
            ienv_bind_local(env, s->assign_target->name, v);
            break;
        }
        case NODE_IF: {
            int cnd = eval_bool(s->cond, env);
            Node *br = cnd ? s->then_br : s->else_br;
            if (!br) break;
            if (!bv_interp(&br->stmts, env, ret, has_ret)) return 0;
            break;
        }
        case NODE_RETURN: {
            int ok = 1;
            int64_t v = s->ret_val ? eval_i64(s->ret_val, env, &ok) : 0;
            if (s->ret_val && !ok) return 0;
            *ret = v; *has_ret = 1;
            return 1;
        }
        default: return 0;
        }
        if (*has_ret) return 1;
    }
    return 1;
}

/* ============================ oracle ============================ */

int bv_verify_ensures(Node *fn, Node *spec, int ens_idx,
                      char ***wit_names, long long **wit_vals, int *wn) {
    *wit_names = NULL; *wit_vals = NULL; *wn = 0;
    if (!fn || !spec || ens_idx < 0 || ens_idx >= spec->spec_ensures.len) return 0;
    if (!fn->fn_body) return 0;

    BvCtx c;
    bv_ctx_init(&c);
    int t = bv_sat_new_var(&c.sat);   /* var 1 */
    int f = bv_sat_new_var(&c.sat);   /* var 2 */
    if (t != BV_T || f != BV_F) { bv_ctx_free(&c); return 0; }
    bv_assert_lit(&c, BV_T);
    bv_assert_lit(&c, -BV_F);

    /* параметри — само i64 */
    for (int i = 0; i < fn->params.len; i++) {
        Node *p = fn->params.data[i];
        if (!type_is_i64(p->param_type)) { bv_ctx_free(&c); return 0; }
        BvWord w = w_fresh(&c);
        if (c.failed) { bv_ctx_free(&c); return 0; }
        bv_env_bind(&c, p->param_name, w);
    }

    /* requires */
    for (int r = 0; r < spec->spec_requires.len; r++) {
        int lit = bv_eval_bool(&c, spec->spec_requires.data[r]->ensure_expr);
        if (c.failed) { bv_ctx_free(&c); return 0; }
        bv_assert_lit(&c, lit);
    }

    /* тяло — изискваме definitely-return (иначе изходът е частичен) */
    int dret = bv_stmts(&c, &fn->fn_body->stmts);
    if (c.failed || !c.has_ret || !dret) { bv_ctx_free(&c); return 0; }
    bv_env_bind(&c, "output", c.ret);

    /* ¬ensures[ens_idx] */
    int elit = bv_eval_bool(&c, spec->spec_ensures.data[ens_idx]->ensure_expr);
    if (c.failed) { bv_ctx_free(&c); return 0; }
    bv_assert_lit(&c, g_not(&c, elit));

    /* Решаване с рестартове през три фазови стратегии (pos/neg/alt):
     * witness-ите на shift/wrap твърденията са в различни региони.
     * Budget на стратегия — изчерпат ли се всички, честен UNKNOWN. */
    BvSatResult r = BV_SAT_UNDECIDED;
    for (int mode = 0; mode < 3 && r == BV_SAT_UNDECIDED; mode++) {
        if (mode) bv_sat_reset(&c.sat);
        c.sat.phase_mode = mode;
        r = bv_sat_solve(&c.sat);
    }
    if (r == BV_SAT_UNDECIDED) { bv_ctx_free(&c); return 0; }
    if (r == BV_SAT_UNSAT) { bv_ctx_free(&c); return 1; }   /* ДОКАЗАНО */

    /* SAT: witness от модела (param думите са пресни положителни var-ове) */
    int np = fn->params.len;
    int64_t *pv = malloc((size_t)(np > 0 ? np : 1) * sizeof *pv);
    for (int i = 0; i < np; i++) {
        BvWord *w = bv_env_find(&c, fn->params.data[i]->param_name);
        uint64_t u = 0;
        for (int b = 0; b < 64; b++)
            if (bv_sat_var_value(&c.sat, w->lit[b])) u |= (uint64_t)1 << b;
        pv[i] = (int64_t)u;
    }
    bv_ctx_free(&c);

    /* M8 презареждане: интерпретирай тялото върху witness-а и потвърди
     * requires ∧ ¬ensures(output). Разминаване → честен UNKNOWN. */
    IEnv env; env.b = NULL; env.n = 0;
    for (int i = 0; i < np; i++)
        ienv_bind_local(&env, fn->params.data[i]->param_name, pv[i]);
    int64_t ret = 0; int has_ret = 0;
    int ok = bv_interp(&fn->fn_body->stmts, &env, &ret, &has_ret);
    if (ok && has_ret) {
        ienv_bind_local(&env, "output", ret);
        for (int q = 0; ok && q < spec->spec_requires.len; q++)
            if (!eval_bool(spec->spec_requires.data[q]->ensure_expr, &env)) ok = 0;
        if (ok && eval_bool(spec->spec_ensures.data[ens_idx]->ensure_expr, &env)) ok = 0;
    } else {
        ok = 0;
    }
    if (!ok) {
        for (int i = 0; i < env.n; i++) free(env.b[i].name);
        free(env.b); free(pv);
        return 0;
    }
    *wn = np;
    if (np > 0) {
        *wit_names = malloc((size_t)np * sizeof(char *));
        *wit_vals = malloc((size_t)np * sizeof(long long));
        for (int i = 0; i < np; i++) {
            (*wit_names)[i] = strdup(fn->params.data[i]->param_name);
            (*wit_vals)[i] = (long long)pv[i];
        }
    }
    for (int i = 0; i < env.n; i++) free(env.b[i].name);
    free(env.b); free(pv);
    return 2;   /* ОБРОЧЕНО с презареден контрапример */
}
