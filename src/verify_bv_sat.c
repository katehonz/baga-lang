/* verify_bv_sat.c — in-house DPLL SAT solver (M31, пълен BV фрагмент).
 *
 * Нула външни зависимости, както целия верификатор. Chronological
 * backtracking DPLL с unit propagation през occurrence списъци (не watched
 * literals — по-просто за одит; Tseytin-CNFin от bit-blasting е малък) и
 * БЕЗ clause learning (v1). Решенията се разнообразяват с редуваща се
 * фаза (Jeroslow-Wang избор на променлива, phase toggle за полярност).
 *
 * Честност: конфликтният бюджет е таван на работата. Надхвърли ли се —
 * BV_SAT_UNDECIDED и верификаторът остава при „НЕ МОГА ДА РЕША". UNSAT
 * (⇒ ДОКАЗАНО) се връща само при изчерпано търсене.
 *
 * CNF: променливи 1..n_vars; лит. = +var (истина) / -var (лъжа).
 */

#include "verify_impl.h"
#include <stdlib.h>
#include <string.h>

#define BV_OCC_STEP 8

/* lit → слот в occurrence таблицата: lit>0 → 2*lit, lit<0 → 2*(-lit)+1 */
static int occ_slot(int lit) { return lit > 0 ? 2 * lit : 2 * (-lit) + 1; }

void bv_sat_init(BvSat *s, int n_vars_hint, long conflict_budget) {
    memset(s, 0, sizeof *s);
    s->cap_vars = n_vars_hint > 64 ? n_vars_hint : 64;
    s->assign = calloc((size_t)s->cap_vars + 1, sizeof *s->assign);
    s->level = calloc((size_t)s->cap_vars + 1, sizeof *s->level);
    s->reason = calloc((size_t)s->cap_vars + 1, sizeof *s->reason);
    s->seen = calloc((size_t)s->cap_vars + 1, sizeof *s->seen);
    s->learn_cap = 256;
    s->learn_buf = malloc((size_t)s->learn_cap * sizeof *s->learn_buf);
    s->cdcl = 0;
    s->cap_cl = 256;
    s->cl = malloc((size_t)s->cap_cl * sizeof *s->cl);
    s->cap_trail = 256;
    s->trail = malloc((size_t)s->cap_trail * sizeof *s->trail);
    s->cap_lim = 128;
    s->trail_lim = malloc((size_t)s->cap_lim * sizeof *s->trail_lim);
    s->flipped = malloc((size_t)s->cap_lim * sizeof *s->flipped);
    s->occ_cap = s->cap_vars;
    s->occ = calloc((size_t)(2 * s->occ_cap + 2), sizeof *s->occ);
    s->occ_n = calloc((size_t)(2 * s->occ_cap + 2), sizeof *s->occ_n);
    s->budget = conflict_budget > 0 ? conflict_budget : BV_SAT_DEFAULT_BUDGET;
}

void bv_sat_free(BvSat *s) {
    for (int i = 0; i < s->n_cl; i++) free(s->cl[i].lits);
    free(s->cl);
    for (int i = 0; i < 2 * s->occ_cap + 2; i++) free(s->occ[i]);
    free(s->occ); free(s->occ_n);
    free(s->assign); free(s->level); free(s->trail); free(s->trail_lim);
    free(s->flipped); free(s->reason); free(s->seen); free(s->learn_buf);
    memset(s, 0, sizeof *s);
}

static void sat_grow_vars(BvSat *s, int v) {
    if (v < s->cap_vars) return;
    int nc = s->cap_vars;
    while (nc <= v) nc *= 2;
    s->assign = realloc(s->assign, ((size_t)nc + 1) * sizeof *s->assign);
    memset(s->assign + s->cap_vars + 1, 0,
           ((size_t)nc + 1 - ((size_t)s->cap_vars + 1)) * sizeof *s->assign);
    s->level = realloc(s->level, ((size_t)nc + 1) * sizeof *s->level);
    memset(s->level + s->cap_vars + 1, 0,
           ((size_t)nc + 1 - ((size_t)s->cap_vars + 1)) * sizeof *s->level);
    s->reason = realloc(s->reason, ((size_t)nc + 1) * sizeof *s->reason);
    memset(s->reason + s->cap_vars + 1, 0,
           ((size_t)nc + 1 - ((size_t)s->cap_vars + 1)) * sizeof *s->reason);
    s->seen = realloc(s->seen, ((size_t)nc + 1) * sizeof *s->seen);
    memset(s->seen + s->cap_vars + 1, 0,
           ((size_t)nc + 1 - ((size_t)s->cap_vars + 1)) * sizeof *s->seen);
    int nslot_new = 2 * nc + 2, nslot_old = 2 * s->occ_cap + 2;
    s->occ = realloc(s->occ, (size_t)nslot_new * sizeof *s->occ);
    s->occ_n = realloc(s->occ_n, (size_t)nslot_new * sizeof *s->occ_n);
    memset(s->occ + nslot_old, 0, (size_t)(nslot_new - nslot_old) * sizeof *s->occ);
    memset(s->occ_n + nslot_old, 0, (size_t)(nslot_new - nslot_old) * sizeof *s->occ_n);
    s->occ_cap = nc;
    s->cap_vars = nc;
}

int bv_sat_new_var(BvSat *s) {
    int v = ++s->n_vars;
    sat_grow_vars(s, v + 1);
    return v;
}

void bv_sat_add_clause(BvSat *s, const int *lits, int n) {
    if (s->n_cl == s->cap_cl) {
        s->cap_cl *= 2;
        s->cl = realloc(s->cl, (size_t)s->cap_cl * sizeof *s->cl);
    }
    BvClause *c = &s->cl[s->n_cl];
    c->n = n;
    c->lits = n > 0 ? malloc((size_t)n * sizeof(int)) : NULL;
    for (int i = 0; i < n; i++) c->lits[i] = lits[i];
    int ci = s->n_cl++;
    for (int i = 0; i < n; i++) {
        int slot = occ_slot(lits[i]);
        if (s->occ_n[slot] % BV_OCC_STEP == 0)
            s->occ[slot] = realloc(s->occ[slot],
                                   (size_t)(s->occ_n[slot] + BV_OCC_STEP) * sizeof(int));
        s->occ[slot][s->occ_n[slot]++] = ci;
    }
}

static int lit_value(BvSat *s, int lit) {
    int v = lit > 0 ? lit : -lit;
    if (v > s->n_vars || s->assign[v] == 0) return 0;
    int val = s->assign[v] > 0 ? 1 : -1;
    return lit > 0 ? val : -val;
}

static void trail_push(BvSat *s, int lit, int level) {
    if (s->n_trail == s->cap_trail) {
        s->cap_trail *= 2;
        s->trail = realloc(s->trail, (size_t)s->cap_trail * sizeof *s->trail);
    }
    s->trail[s->n_trail++] = lit;
    int v = lit > 0 ? lit : -lit;
    s->assign[v] = lit > 0 ? 1 : -1;
    s->level[v] = level;
    s->reason[v] = 0;   /* по подразбиране — решение; propagate ще го смени */
}

static void trail_push_reason(BvSat *s, int lit, int level, int clause_idx) {
    trail_push(s, lit, level);
    int v = lit > 0 ? lit : -lit;
    s->reason[v] = clause_idx + 1;   /* +1 за да се отличава от 0 */
}

/* Unit propagation върху trail-а (worklist от q_head). 1 = конфликт.
 * При конфликт в s->conflict_cl се записва индексът на falsified клаузата
 * (всички литерали < 0), за да може CDCL да я анализира директно.
 * След backtrack q_head се нулира и trail-ът се ресканва — вече-истинските
 * клаузи излизат евтино (lit_value > 0), цената е приемлива за размерите
 * на blasted вериги. */
static int sat_propagate(BvSat *s) {
    while (s->q_head < s->n_trail) {
        int lit = s->trail[s->q_head++];
        int slot = occ_slot(-lit);   /* клаузи, съдържащи вече-лъжливия -lit */
        for (int oi = 0; oi < s->occ_n[slot]; oi++) {
            int ci = s->occ[slot][oi];
            BvClause *c = &s->cl[ci];
            int unassigned = 0, unit_lit = 0, sat = 0;
            for (int i = 0; i < c->n; i++) {
                int lv = lit_value(s, c->lits[i]);
                if (lv > 0) { sat = 1; break; }
                if (lv == 0) {
                    unassigned++;
                    unit_lit = c->lits[i];
                    if (unassigned > 1) break;
                }
            }
            if (sat) continue;
            if (unassigned == 0) {
                s->conflict_cl = ci;   /* M32: запази конфликтната клауза за анализ */
                return 1;
            }
            if (unassigned == 1) {
                /* M32 CDCL: записваме причината — клаузата, която е unit-имплицирала */
                trail_push_reason(s, unit_lit, s->n_lim, ci);
            }
        }
    }
    return 0;
}

/* Най-ниската неопределена променлива. Входовете на веригата са създадени
 * първи (MSB-first за всяка дума) — решение по вход кара Tseytin
 * propagation-а да определи почти целия circuit; вътрешните порти рядко
 * остават за решаване. O(n_vars) на решение е приемливо: решенията са
 * малко на брой спрямо променливите. */
static int sat_decide_var(BvSat *s) {
    for (int v = 1; v <= s->n_vars; v++)
        if (s->assign[v] == 0) return v;
    return 0;
}

/* Връща 1 ако trail-ът е свит до start (променливите са unassign-нати). */
static void trail_pop_to(BvSat *s, int start) {
    while (s->n_trail > start) {
        int lit = s->trail[--s->n_trail];
        int v = lit > 0 ? lit : -lit;
        s->assign[v] = 0;
        s->reason[v] = 0;
    }
}

/* M32 CDCL: 1-UIP conflict analysis по MiniSat алгоритъма.
 * Работи с trail (имплицитен граф на импликации): започва от конфликтната
 * клауза, обхожда trail назад, резолвира с reason на всеки литерал от
 * текущото ниво, докато остане един (UIP). Връща индекс на learned клаузата.
 * learn_buf[0] винаги е ¬UIP — литералът за пропагация след backjump.
 * Backjump level = макс. level на останалите литерали. */
static int cdcl_analyze(BvSat *s, int conflict_cl, int *backjump_level) {
    int cur_level = s->n_lim;
    int n_learn = 0;
    int counter = 0;
    int p = 0;
    int index = s->n_trail - 1;
    BvClause *cl = &s->cl[conflict_cl];

    for (;;) {
        for (int i = 0; i < cl->n; i++) {
            int q = cl->lits[i];
            if (q == p) continue;
            int v = q > 0 ? q : -q;
            if (s->seen[v] || s->level[v] == 0) continue;
            s->seen[v] = 1;
            if (s->level[v] >= cur_level) {
                counter++;
            } else {
                if (n_learn == s->learn_cap) {
                    s->learn_cap *= 2;
                    s->learn_buf = realloc(s->learn_buf, (size_t)s->learn_cap * sizeof(int));
                }
                s->learn_buf[n_learn++] = q;
                if (s->level[v] > *backjump_level) *backjump_level = s->level[v];
            }
        }
        while (index >= 0) {
            int t = s->trail[index];
            int tv = t > 0 ? t : -t;
            if (s->seen[tv] && s->level[tv] >= cur_level) break;
            index--;
        }
        if (index < 0 || counter <= 0) break;
        p = s->trail[index];
        int pv = p > 0 ? p : -p;
        int r = s->reason[pv];
        s->seen[pv] = 0;
        counter--;
        if (r == 0) break;   /* decision — UIP */
        cl = &s->cl[r - 1];
        index--;
    }
    for (int i = 0; i < s->n_trail; i++) {
        int t = s->trail[i];
        int tv = t > 0 ? t : -t;
        if (s->seen[tv]) s->seen[tv] = 0;
    }
    if (p == 0) return -1;
    if (n_learn + 1 > s->learn_cap) {
        s->learn_cap = (n_learn + 1) * 2;
        s->learn_buf = realloc(s->learn_buf, (size_t)s->learn_cap * sizeof(int));
    }
    for (int i = n_learn; i > 0; i--) s->learn_buf[i] = s->learn_buf[i - 1];
    s->learn_buf[0] = -p;
    n_learn++;
    if (n_learn == 1) *backjump_level = 0;
    /* soundness guard: learned clause must not be a tautology (no v and -v).
     * A tautological learned clause would let propagation skip it, which is
     * harmless for SAT but can mask the real conflict and cause false UNSAT. */
    for (int i = 0; i < n_learn; i++) {
        for (int j = i + 1; j < n_learn; j++) {
            if (s->learn_buf[i] == -s->learn_buf[j]) return -1;
        }
    }
    bv_sat_add_clause(s, s->learn_buf, n_learn);
    return s->n_cl - 1;
}

BvSatResult bv_sat_solve(BvSat *s) {
    /* Ниво 0: assert-натите unit-клаузи (requires, ¬ensures, T/F) трябва
     * сами да влязат в trail-а — propagation-ът е trail--driven и празен
     * trail не би ги докоснал. Фиксирани точка: скан за единици → assign →
     * propagate → повторен скан (propagation-ът ражда нови единици). */
    s->q_head = 0;
    for (;;) {
        int progressed = 0;
        for (int ci = 0; ci < s->n_cl; ci++) {
            BvClause *c = &s->cl[ci];
            int un = 0, ul = 0, sat = 0;
            for (int i = 0; i < c->n; i++) {
                int lv = lit_value(s, c->lits[i]);
                if (lv > 0) { sat = 1; break; }
                if (lv == 0) { un++; ul = c->lits[i]; if (un > 1) break; }
            }
            if (sat) continue;
            if (un == 0) return BV_SAT_UNSAT;
            if (un == 1) { trail_push(s, ul, 0); progressed = 1; }
        }
        if (!progressed) break;
        s->q_head = 0;
        if (sat_propagate(s)) return BV_SAT_UNSAT;
    }
    if (sat_propagate(s)) return BV_SAT_UNSAT;
    for (;;) {
        int v = sat_decide_var(s);
        if (v == 0) return BV_SAT_SAT;           /* пълен модел, без конфликт */
        if (s->n_lim == s->cap_lim) {
            s->cap_lim *= 2;
            s->trail_lim = realloc(s->trail_lim,
                                   (size_t)s->cap_lim * sizeof *s->trail_lim);
            s->flipped = realloc(s->flipped,
                                 (size_t)s->cap_lim * sizeof *s->flipped);
        }
        s->trail_lim[s->n_lim] = s->n_trail;
        s->flipped[s->n_lim] = 0;
        s->n_lim++;
        /* Фазова стратегия (phase_mode): 0 = положителна, 1 = отрицателна,
         * 2 = алтернативна по ниво. Oracle-ът прави рестартове през трите —
         * witness-ите за shift/wrap идентичности живеят в различни региони
         * и нито една фиксирана фаза не ги покрива (измерено: INT64_MIN
         * срещу 0x5555…). */
        int lit = v;
        if (s->phase_mode == 1) lit = -v;
        else if (s->phase_mode == 2 && (s->n_lim % 2) == 0) lit = -v;
        trail_push(s, lit, s->n_lim);
        /* Само новият лит. е прясно-лъжлив за occ-скана: backtrack-ът само
         * UNASSIGN-ва, а върнатото по-горно ниво вече е било безконфликтно. */
        s->q_head = s->n_trail - 1;
        while (sat_propagate(s)) {
            /* Конфликт. M32 CDCL: ако learning е включен, анализирай конфликта
             * (1-UIP), научи клаузата и backjump-ни до съответното ниво.
             * Иначе — старото chronological backtracking с phase flip. */
            if (++s->conflicts > s->budget) return BV_SAT_UNDECIDED;
            if (s->n_lim == 0) return BV_SAT_UNSAT;
            if (s->cdcl) {
                /* M32 AUDIT FIX: sat_propagate вече е записал конфликтната клауза
                 * в s->conflict_cl (всички литерали < 0). Предишната версия
                 * сканираше за "клауза без истинен лит" — но lit_value==0 е
                 * UNASSIGNED, не false — така се хващаше unit/unassigned клауза
                 * като "конфликт" и се строяха грешни learned clauses → false UNSAT. */
                int conf_cl = s->conflict_cl;
                if (conf_cl < 0 || conf_cl >= s->n_cl) return BV_SAT_UNSAT;
                int bj = 0;
                int learned = cdcl_analyze(s, conf_cl, &bj);
                if (learned < 0) return BV_SAT_UNSAT;
                /* backjump: pop до началото на bj нивото */
                while (s->n_lim > bj && s->n_lim > 0) {
                    int top = s->n_lim - 1;
                    trail_pop_to(s, s->trail_lim[top]);
                    s->n_lim = top;
                }
                /* learned clause е unit на bj ниво — UIP литералът (learn_buf[0])
                 * трябва да се пропагира: push-ni го на trail с reason=learned */
                int uip = s->cl[learned].lits[0];
                trail_push_reason(s, uip, s->n_lim, learned);
                s->q_head = s->n_trail - 1;
                if (sat_propagate(s)) {
                    if (s->n_lim == 0) return BV_SAT_UNSAT;
                    continue;   /* нов конфликт — анализирай отново */
                }
                break;
            }
            /* chronological backtracking (v1, без learning) */
            int top = s->n_lim - 1;
            int start = s->trail_lim[top];
            int dec = s->trail[start];
            trail_pop_to(s, start);
            if (!s->flipped[top]) {
                s->flipped[top] = 1;
                s->trail_lim[top] = s->n_trail;
                trail_push(s, -dec, s->n_lim);
                s->q_head = s->n_trail - 1;
            } else {
                s->n_lim = top;
                s->q_head = s->n_trail;
            }
        }
    }
}

int bv_sat_var_value(BvSat *s, int v) {
    if (v <= 0 || v > s->n_vars) return 0;
    return s->assign[v] > 0 ? 1 : 0;
}

/* Рестарт: всички присвоявания се нулират, клаузите остават (няма
 * learning — нищо научено няма). Ползва се от oracle-а за опит с нова
 * фазова стратегия след изчерпан конфликтен бюджет. */
void bv_sat_reset(BvSat *s) {
    for (int i = 0; i < s->n_trail; i++) {
        int lit = s->trail[i];
        int v = lit > 0 ? lit : -lit;
        s->assign[v] = 0;
        s->reason[v] = 0;
    }
    s->n_trail = 0;
    s->n_lim = 0;
    s->q_head = 0;
    s->conflicts = 0;
    /* learned clauses остават — те са валидни и след рестарт (M32) */
}
