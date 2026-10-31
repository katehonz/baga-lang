/* codegen_api.c — част от codegen_c.c (виж cg_proto.h). */
#include "baga.h"
#include "codegen_impl.h"

/* ============================================================
 *  Public API
 * ============================================================ */

#define BAGA_TEST_COUNT 100
#define BAGA_TEST_TRIES 1000000 /* горна граница опити за валиден вход на тест */

static void emit_test_driver(Codegen *cg, Node *program) {
    FILE *f = cg->out;

    /* детерминистичен PRNG (xorshift64) */
    fprintf(f, "static uint64_t baga_seed = 0x243F6A8885A308D3ULL;\n");
    fprintf(f, "static int64_t baga_rand_i64(int64_t lo, int64_t hi) {\n");
    fprintf(f, "    baga_seed ^= baga_seed << 13; baga_seed ^= baga_seed >> 7; baga_seed ^= baga_seed << 17;\n");
    fprintf(f, "    return lo + (int64_t)(baga_seed %% (uint64_t)(hi - lo + 1));\n");
    fprintf(f, "}\n\n");

    /* requires предикати + брой тестируеми */
    int n_tested = 0;
    for (int i = 0; i < program->items.len; i++) {
        Node *it = program->items.data[i];
        if (it->kind != NODE_SPEC) continue;
        if (it->spec_ensures.len == 0 && it->spec_requires.len == 0) continue;
        if (!spec_inputs_testable(it)) continue;
        emit_requires_predicate(cg, it);
        n_tested++;
    }

    fprintf(f, "int main(int argc, char **argv) {\n");
    fprintf(f, "    baga_argc = argc; baga_argv = argv;\n");
    if (n_tested == 0) {
        /* W3.1: изпиши ЗАЩО няма тестируеми (spec-ове с договор/гаранция за
         * свят или неподдържани типове иначе мълчаливо изчезват) */
        for (int i = 0; i < program->items.len; i++) {
            Node *it = program->items.data[i];
            if (it->kind != NODE_SPEC) continue;
            if (it->spec_ensures.len == 0 && it->spec_requires.len == 0) continue;
            fprintf(f, "    printf(");
            emit_c_string(f, it->spec_name);
            fprintf(f, " \": пропусната (не е property-testable — непознат тип, договор за свят или гаранция за върнат свят)\\n\");\n");
        }
        fprintf(f, "    printf(\"няма spec-ове за тестване\\n\");\n");
        fprintf(f, "    return 0;\n}\n");
        return;
    }

    for (int i = 0; i < program->items.len; i++) {
        Node *it = program->items.data[i];
        if (it->kind != NODE_SPEC) continue;
        if (it->spec_ensures.len == 0 && it->spec_requires.len == 0) continue;
        if (!spec_inputs_testable(it)) {
            fprintf(f, "    printf(");
            emit_c_string(f, it->spec_name);
            fprintf(f, " \": пропусната (не е property-testable — непознат тип, договор за свят или гаранция за върнат свят)\\n\");\n");
            continue;
        }
        int np = it->spec_inputs.len;
        char *fm = mangle_name(it->spec_name);
        fprintf(f, "    { int passed = 0, skipped = 0;\n");
        fprintf(f, "      for (int t = 0; t < %d; t++) {\n", BAGA_TEST_COUNT);
        fprintf(f, "          int64_t args[%d];\n", np > 0 ? np : 1);
        fprintf(f, "          int ok = 0;\n");
        fprintf(f, "          for (long tr = 0; tr < %d && !ok; tr++) {\n", BAGA_TEST_TRIES);
        for (int j = 0; j < np; j++) {
            Node *pt = it->spec_inputs.data[j]->param_type;
            if (strcmp(pt->type_name, "bool") == 0)
                fprintf(f, "              args[%d] = baga_rand_i64(0, 1);\n", j);
            else
                fprintf(f, "              args[%d] = baga_rand_i64(-1000, 1000);\n", j);
        }
        fprintf(f, "              ok = b__req_%s(", fm + 2);
        for (int j = 0; j < np; j++) fprintf(f, "%sargs[%d]", j ? ", " : "", j);
        fprintf(f, ");\n");
        fprintf(f, "              if (!ok) skipped++;\n");
        fprintf(f, "          }\n");
        fprintf(f, "          if (!ok) continue;\n");
        fprintf(f, "          baga_cur_nargs = %d;\n", np);
        for (int j = 0; j < np; j++)
            fprintf(f, "          baga_cur_args[%d] = args[%d];\n", j, j);
        fprintf(f, "          (void)%s(", fm);
        for (int j = 0; j < np; j++) fprintf(f, "%sargs[%d]", j ? ", " : "", j);
        fprintf(f, ");\n");
        fprintf(f, "          passed++;\n      }\n");
        fprintf(f, "      printf(");
        emit_c_string(f, it->spec_name);
        fprintf(f, " \": %%d/%d теста минаха (%%d пропуснати от requires)\\n\", passed, skipped);\n", BAGA_TEST_COUNT);
        fprintf(f, "    }\n");
        free(fm);
    }
    fprintf(f, "    printf(\"Всички spec тестове минаха. ⚔️\\n\");\n");
    fprintf(f, "    return 0;\n}\n");
}

void codegen_c(Codegen *cg, Node *program, FILE *out) {
    cg->out = out;
    cg->indent = 0;
    cg->tmp_counter = 0;
    cg->program = program;
    /* RC1: scope стековете се нулират тук (Codegen идва неинициализиран
     * от main.c; rc флагът се задава от main.c преди извикването) */
    cg->rc_locals.data = NULL; cg->rc_locals.len = 0; cg->rc_locals.cap = 0;
    cg->rc_scopes.data = NULL; cg->rc_scopes.len = 0; cg->rc_scopes.cap = 0;
    cg->rc_fn_base = -1;
    cg->rc_lus.data = NULL; cg->rc_lus.len = 0; cg->rc_lus.cap = 0;
    cg->rc_moves = 0;
    cg->rc_cmoves = 0;
    cg->rc_tmps.data = NULL; cg->rc_tmps.len = 0; cg->rc_tmps.cap = 0;
    cg->rc_tmps_on = 0;
    cg->rc_tmp_count = 0;
    cg->rc_tmp_decl = NULL;
    cg->rc_marks.data = NULL; cg->rc_marks.len = 0; cg->rc_marks.cap = 0;
    cg->rc_cur_blk = NULL; cg->rc_cur_idx = -1;
    cg->rc_cur_fn = NULL;
    cg->rc_elided_pairs = 0;
    /* M20: effect tag регистър (детерминистичен pre-pass по-долу) */
    cg->eff_tags.data = NULL; cg->eff_tags.len = 0; cg->eff_tags.cap = 0;
    cg->eff_depth = 0;
    cg->eff_cur_ret = NULL;
    cg->eff_binding = NULL;
    cg->eff_binding_c = NULL;
    /* M20 pre-pass: assign tags на всички payload ефекти в сигнатурите —
     * преди какъвто и да е emit (таг-овете трябва да са стабилни) */
    for (int i = 0; i < program->items.len; i++) {
        Node *item = program->items.data[i];
        if (item->kind != NODE_FN || !item->ret_type) continue;
        Node *t = item->ret_type;
        while (t && t->kind == NODE_TYPE_EFFECT) {
            if (t->effect_payloads) {
                for (int j = 0; j < t->n_effects; j++)
                    if (t->effect_payloads[j])
                        eff_tag(cg, t->effect_names[j]);
            }
            t = t->inner_type;
        }
    }
    /* RC1: размер на per-alloc header-а — 24 B с rc поле, 16 B без */
    int hs = cg->rc ? 32 : 16;

    /* header */
    fprintf(out, "/* Генериран от компилатора на Бага. Фаза 1. */\n");
    fprintf(out, "#include <stdio.h>\n");
    fprintf(out, "#include <stdlib.h>\n");
    fprintf(out, "#include <stdint.h>\n");
    fprintf(out, "#include <string.h>\n");
    fprintf(out, "#include <errno.h>\n");
    fprintf(out, "#include <time.h>\n");
    fprintf(out, "#include <signal.h>\n");
    fprintf(out, "#include <pthread.h>\n");
    fprintf(out, "#include <stdatomic.h>\n");
    fprintf(out, "#include <execinfo.h>\n");
    fprintf(out, "#include <math.h>\n\n");

    /* arena — всички низови/векторни алокации минават тук (без individual free).
     * R52: arena + free lists са THREAD-LOCAL (__thread). Преди това един
     * глобален mutex (baga_alloc_mu) сериализираше ВСЯКА алокация във всички
     * нишки — реален GIL при MT serve (go_bg per conn + workers). Безопасно,
     * защото str е arena-bound (не се free-ва), а free-list блоковете са
     * сменяема сырова памет — cross-thread free попълва списъка на free-ващата
     * нишка. */
    fprintf(out, "typedef struct baga_ABlk { struct baga_ABlk *next; size_t used, cap; char data[]; } baga_ABlk;\n");
    fprintf(out, "static __thread baga_ABlk *baga_arena_head = NULL;\n");
    /* MEM-5 (boilaDB serve): spare верига за non-RC пътя — rewind връща
     * блоковете тук вместо на malloc; baga_alloc ги reuse-ва. Същият модел
     * като RC1 baga_rc_spare. Без него всеки per-turn rewind прави
     * free()+malloc на 8 KB блокове → glibc фрагментация (измерен
     * линеен RSS ръст ~10 KB/заявка) и чист CPU разход. */
    fprintf(out, "static __thread baga_ABlk *baga_spare = NULL;\n");
    /* MEM-6: leak детектор — process-wide телеметрия на арената. Броячите се
     * пипат САМО на chunk-граници (нов ABlk, spare pop, rewind), не на всяка
     * baga_alloc — горещият път не плаща нищо. live = cap на блоковете в
     * активната верига (всички нишки); spare = reclaim-нати, още mapped;
     * peak = high-water на live. Точно метриката, която лови VPS класа бъг:
     * пропуснат mem_rewind ⇒ live расте неограничено. Persist регионът
     * (отделни malloc-и) не се брои в v1 — той е long-lived по дизайн. */
    fprintf(out, "static _Atomic long baga_mt_live = 0;\n");
    fprintf(out, "static _Atomic long baga_mt_spare = 0;\n");
    fprintf(out, "static _Atomic long baga_mt_peak = 0;\n");
    fprintf(out, "static void baga_mt_bump(long d) {\n");
    fprintf(out, "    long v = atomic_fetch_add(&baga_mt_live, d) + d;\n");
    fprintf(out, "    long p = atomic_load_explicit(&baga_mt_peak, memory_order_relaxed);\n");
    fprintf(out, "    while (v > p && !atomic_compare_exchange_weak(&baga_mt_peak, &p, v)) { }\n");
    fprintf(out, "}\n");
    fprintf(out, "static int64_t baga_mem_arena_bytes(void) { return (int64_t)atomic_load(&baga_mt_live); }\n");
    fprintf(out, "static int64_t baga_mem_spare_bytes(void) { return (int64_t)atomic_load(&baga_mt_spare); }\n");
    fprintf(out, "static int64_t baga_mem_peak_bytes(void)  { return (int64_t)atomic_load(&baga_mt_peak); }\n");
    if (cg->leak_check) {
        fprintf(out, "static void baga_mt_report(void) {\n");
        fprintf(out, "    long live = atomic_load(&baga_mt_live), spare = atomic_load(&baga_mt_spare), peak = atomic_load(&baga_mt_peak);\n");
        fprintf(out, "    fprintf(stderr, \"baga mem: live=%%ld spare=%%ld peak=%%ld\\n\", live, spare, peak);\n");
        fprintf(out, "    const char *lim = getenv(\"BAGA_MEM_LIMIT\");\n");
        fprintf(out, "    if (lim && *lim) {\n");
        fprintf(out, "        long l = strtol(lim, NULL, 10);\n");
        fprintf(out, "        if (l > 0 && peak > l) {\n");
        fprintf(out, "            fprintf(stderr, \"baga mem: peak %%ld B надвишава BAGA_MEM_LIMIT %%ld B — вероятен теч\\n\", peak, l);\n");
        fprintf(out, "            fflush(NULL); _Exit(1);\n");
        fprintf(out, "        }\n");
        fprintf(out, "    }\n");
        fprintf(out, "}\n");
        fprintf(out, "__attribute__((constructor)) static void baga_mt_init(void) { atexit(baga_mt_report); }\n");
    }
    /* MEM-1: free list — 16-байтови класове ≤ 1024 B; R18: + pow2 класове
     * 2 KiB..32 MiB — drop рециклира блокове. Големите класове се bump-ват с
     * ПЪЛНИЯ класов размер, за да е консистентен всеки бъдещ free в класа. */
    fprintf(out, "#define BAGA_FL_CLASSES 64   /* free list: 16-байтови класове ≤ 1024 B */\n");
    fprintf(out, "#define BAGA_FL_BIG 12       /* pow2 класове: 2 KiB .. 32 MiB */\n");
    fprintf(out, "static __thread void *baga_fl[BAGA_FL_CLASSES];\n");
    fprintf(out, "static __thread void *baga_fl_big[BAGA_FL_BIG];\n");
    /* MEM-4 (boilaDB Q2): per-request rewind + persist регион.
     * mem_mark/mem_rewind: mark пази (head блок, used); rewind освобождава
     * всички блокове над mark-а и връща used — паметта на заявката се
     * връща към malloc. Free-list записи, сочещи във върнатата памет, се
     * изчистват (иначе двойна употреба). LIFO нарушение/двоен rewind →
     * чиста грешка, не UB.
     * mem_persist_begin/end: докато depth > 0, baga_alloc bump-ва в
     * ОТДЕЛНА верига, която rewind не пипа. Persist и ephemeral имат
     * РАЗДЕЛНИ free lists — иначе drop() на persist страница я слага в
     * ephemeral freelist и следващ ephemeral alloc презаписва live
     * shared state (page cache / memtable) → catalog „таблица липсва".
     * За shared state, който преживява заявката (plan cache, session maps). */
    fprintf(out, "static __thread baga_ABlk *baga_persist_head = NULL;\n");
    fprintf(out, "static __thread int64_t baga_persist_depth = 0;\n");
    fprintf(out, "static __thread void *baga_fl_p[BAGA_FL_CLASSES];\n");
    fprintf(out, "static __thread void *baga_fl_big_p[BAGA_FL_BIG];\n");
    fprintf(out, "static void baga_mem_persist_begin(void) { baga_persist_depth++; }\n");
    fprintf(out, "static void baga_mem_persist_end(void) {\n");
    fprintf(out, "    if (baga_persist_depth <= 0) { fprintf(stderr, \"baga: mem_persist_end без mem_persist_begin\\n\"); exit(1); }\n");
    fprintf(out, "    baga_persist_depth--;\n");
    fprintf(out, "}\n");
    fprintf(out, "static int baga_fl_big_idx(int64_t n) {\n");
    fprintf(out, "    int i = 0; int64_t c = 2048;\n");
    fprintf(out, "    while (i < BAGA_FL_BIG) { if (n <= c) return i; c <<= 1; i++; }\n");
    fprintf(out, "    return -1;\n");
    fprintf(out, "}\n");
    /* MEM-4b: per-alloc header (16 B: magic + persist флаг) → O(1) регион
     * детекция в baga_free. Предишният baga_ptr_in_persist обхождаше веригата
     * persist блокове на ВСЯКО free — O(блокове) на free → квадратичен колапс
     * при compaction/rewind върху голям persist регион (измерено: 1M insert
     * bench спираше да пише — CPU в baga_drop_map/baga_ptr_in_persist). */
    fprintf(out, "#define BAGA_HDR_MAGIC 0xBA6A4D454D344848ULL\n");
    /* RC1: под --rc header-ът получава трето поле rc (24 B); без --rc
     * layout-ът и offset-ите са непроменени (16 B) */
    if (cg->rc) {
        /* RC1-perf: persist битът и epoch са пакетирани в едно поле pe
         * (pe = epoch<<1 | persist) — 32 B header (payload пак 16-подравнен),
         * един запис по-малко на алокация, едно зареждане в rc_hdr. */
        fprintf(out, "typedef struct { uint64_t magic; uint64_t pe; uint64_t rc; uint64_t an; } baga_Hdr;\n");
        /* RC1: обхват на arena блоковете — range guard преди magic check.
         * epoch: mem_rewind я бутва; release на ephemeral стойност от преди
         * rewind е no-op (паметта ѝ е върната — иначе UAF при scope exit
         * след rewind в същия scope). persist стойности са извън epoch.
         * baga_rc_spare: rewind НЕ връща блоковете на malloc, а ги пази в
         * spare веригата за следващи алокации — header-ите остават mapped
         * (epoch е достатъчна защита), а malloc/free обажданията на заявка
         * изчезват (бел. RC1-perf: dead-zone логът отпадна — is_dead беше
         * O(ndead) на всеки retain/release и беше доминиращият overhead). */
        /* NULL = още няма арена — инициализацията с (intptr_t)-1 даваше
         * UBSan „pointer index overflow" в range guard-а при проверка
         * преди първата rc алокация. */
        fprintf(out, "static __thread char *baga_rc_lo = NULL;\n");
        fprintf(out, "static __thread char *baga_rc_hi = NULL;\n");
        fprintf(out, "static __thread uint64_t baga_rc_epoch = 0;\n");
        fprintf(out, "static __thread baga_ABlk *baga_rc_spare = NULL;\n");
        /* RC-dbg: BAGA_ALLOC_STATS=1 → watchdog нишка печати на stderr
         * живите rc обекти по allocation stack (диагностика на течове). */
        fprintf(out, "static _Atomic long baga_st_n = 0, baga_st_b = 0, baga_st_tot = 0;\n");
        fprintf(out, "static _Atomic long baga_st_cls[71];\n");
        fprintf(out, "static volatile int baga_st_on = 0;\n");
        fprintf(out, "static int baga_st_ci(long an) { if (an <= 1024) return (int)((an + 15) / 16) - 1; for (int i = 0; i < 6; i++) if (an <= (2048L << i)) return 64 + i; return 70; }\n");
        fprintf(out, "#define BAGA_ST_NSLOT 8192\n");
        fprintf(out, "#define BAGA_ST_NPTR (1<<18)\n");
        fprintf(out, "typedef struct { _Atomic long n, b; int d; void *bt[8]; } baga_StSite;\n");
        fprintf(out, "static baga_StSite baga_st_sites[BAGA_ST_NSLOT];\n");
        fprintf(out, "typedef struct { void *ptr; int slot; } baga_StPtr;\n");
        fprintf(out, "static baga_StPtr baga_st_ptrs[BAGA_ST_NPTR];\n");
        fprintf(out, "static long baga_st_tomb = 0;\n");
        fprintf(out, "static void baga_st_rehash(void) {\n");
        fprintf(out, "    static baga_StPtr fresh[BAGA_ST_NPTR];\n");
        fprintf(out, "    memset(fresh, 0, sizeof fresh);\n");
        fprintf(out, "    for (unsigned w = 0; w < BAGA_ST_NPTR; w++) {\n");
        fprintf(out, "        void *p = baga_st_ptrs[w].ptr;\n");
        fprintf(out, "        if (!p || p == (void *)1) continue;\n");
        fprintf(out, "        unsigned long h = ((unsigned long)(uintptr_t)p >> 4) & (BAGA_ST_NPTR - 1);\n");
        fprintf(out, "        for (int i = 0; i < 65536; i++) { unsigned q = (unsigned)((h + i) & (BAGA_ST_NPTR - 1)); if (!fresh[q].ptr) { fresh[q] = baga_st_ptrs[w]; break; } }\n");
        fprintf(out, "    }\n");
        fprintf(out, "    memcpy(baga_st_ptrs, fresh, sizeof fresh); baga_st_tomb = 0;\n");
        fprintf(out, "}\n");
        fprintf(out, "static volatile int baga_st_lock = 0;\n");
        fprintf(out, "static void baga_st_spin(void) { while (__sync_lock_test_and_set(&baga_st_lock, 1)) {} }\n");
        fprintf(out, "static void baga_st_un(void) { __sync_lock_release(&baga_st_lock); }\n");
        fprintf(out, "static int baga_st_site(void) {\n");
        fprintf(out, "    void *bt[8]; int d = backtrace(bt, 8);\n");
        fprintf(out, "    unsigned long h = 1469598103934665603ULL;\n");
        fprintf(out, "    for (int i = 0; i < d; i++) h = (h ^ (unsigned long)(uintptr_t)bt[i]) * 1099511628211ULL;\n");
        fprintf(out, "    unsigned k = (unsigned)(h & (BAGA_ST_NSLOT - 1));\n");
        fprintf(out, "    for (int i = 0; i < 64; i++) {\n");
        fprintf(out, "        unsigned q = (k + i) & (BAGA_ST_NSLOT - 1);\n");
        fprintf(out, "        if (baga_st_sites[q].d == d && !memcmp(baga_st_sites[q].bt, bt, (size_t)d * sizeof(void *))) return (int)q;\n");
        fprintf(out, "        if (baga_st_sites[q].d == 0) { baga_st_sites[q].d = d; memcpy(baga_st_sites[q].bt, bt, (size_t)d * sizeof(void *)); return (int)q; }\n");
        fprintf(out, "    }\n");
        fprintf(out, "    return -1;\n");
        fprintf(out, "}\n");
        fprintf(out, "static void baga_st_alloc(void *p, long an) {\n");
        fprintf(out, "    if (!an) return;\n");
        fprintf(out, "    baga_st_spin();\n");
        fprintf(out, "    int s = baga_st_site();\n");
        fprintf(out, "    baga_st_n++; baga_st_b += an; baga_st_tot++; baga_st_cls[baga_st_ci(an)]++;\n");
        fprintf(out, "    if (s >= 0) {\n");
        fprintf(out, "        baga_st_sites[s].n++; baga_st_sites[s].b += an;\n");
        fprintf(out, "        unsigned long h = ((unsigned long)(uintptr_t)p >> 4) & (BAGA_ST_NPTR - 1);\n");
        fprintf(out, "        for (int i = 0; i < 65536; i++) {\n");
        fprintf(out, "            unsigned q = (unsigned)((h + i) & (BAGA_ST_NPTR - 1));\n");
        fprintf(out, "            if (baga_st_ptrs[q].ptr == NULL || baga_st_ptrs[q].ptr == (void *)1 || baga_st_ptrs[q].ptr == p) { baga_st_ptrs[q].ptr = p; baga_st_ptrs[q].slot = s; break; }\n");
        fprintf(out, "        }\n");
        fprintf(out, "    }\n");
        fprintf(out, "    baga_st_un();\n");
        fprintf(out, "}\n");
        fprintf(out, "static void baga_st_free(void *p, long an) {\n");
        fprintf(out, "    if (!an) return;\n");
        fprintf(out, "    baga_st_spin();\n");
        fprintf(out, "    baga_st_n--; baga_st_b -= an; baga_st_cls[baga_st_ci(an)]--;\n");
        fprintf(out, "    unsigned long h = ((unsigned long)(uintptr_t)p >> 4) & (BAGA_ST_NPTR - 1);\n");
        fprintf(out, "    for (int i = 0; i < 65536; i++) {\n");
        fprintf(out, "        unsigned q = (unsigned)((h + i) & (BAGA_ST_NPTR - 1));\n");
        fprintf(out, "        if (baga_st_ptrs[q].ptr == NULL) break;\n");
        fprintf(out, "        if (baga_st_ptrs[q].ptr == p) { int s = baga_st_ptrs[q].slot; if (s >= 0) { baga_st_sites[s].n--; baga_st_sites[s].b -= an; } baga_st_ptrs[q].ptr = (void *)1; if (++baga_st_tomb > BAGA_ST_NPTR / 4) baga_st_rehash(); break; }\n");
        fprintf(out, "    }\n");
        fprintf(out, "    baga_st_un();\n");
        fprintf(out, "}\n");
        fprintf(out, "static void *baga_st_watch(void *arg) {\n");
        fprintf(out, "    (void)arg; long pn = -1, pb = -1;\n");
        fprintf(out, "    for (;;) {\n");
        fprintf(out, "        struct timespec ts = {0, 500000000}; nanosleep(&ts, NULL);\n");
        fprintf(out, "        long n = baga_st_n, b = baga_st_b;\n");
        fprintf(out, "        if (n == pn && b == pb) continue;\n");
        fprintf(out, "        pn = n; pb = b;\n");
        fprintf(out, "        fprintf(stderr, \"baga-alloc: live=%%ld objs %%ld bytes tot=%%ld anchor=%%p\\n\", n, b, baga_st_tot, (void *)baga_st_watch);\n");
        fprintf(out, "        baga_st_spin();\n");
        fprintf(out, "        long sn[8192]; int si[8192]; int m = 0;\n");
        fprintf(out, "        for (int q = 0; q < BAGA_ST_NSLOT; q++) if (baga_st_sites[q].d && baga_st_sites[q].n > 0) { sn[m] = baga_st_sites[q].n; si[m] = q; m++; }\n");
        fprintf(out, "        for (int t = 0; t < 12 && t < m; t++) {\n");
        fprintf(out, "            int bi = t;\n");
        fprintf(out, "            for (int u = t + 1; u < m; u++) if (sn[u] > sn[bi]) bi = u;\n");
        fprintf(out, "            { long tn = sn[t]; sn[t] = sn[bi]; sn[bi] = tn; int ti = si[t]; si[t] = si[bi]; si[bi] = ti; }\n");
        fprintf(out, "            int q = si[t];\n");
        fprintf(out, "            fprintf(stderr, \"  site#%%d live=%%ld %%ldb:\", q, baga_st_sites[q].n, baga_st_sites[q].b);\n");
        fprintf(out, "            for (int i = 0; i < baga_st_sites[q].d; i++) fprintf(stderr, \" %%p\", baga_st_sites[q].bt[i]);\n");
        fprintf(out, "            { int shown = 0;\n");
        fprintf(out, "              for (unsigned w = 0; w < BAGA_ST_NPTR && shown < 2; w++)\n");
        fprintf(out, "                if (baga_st_ptrs[w].ptr && baga_st_ptrs[w].ptr != (void *)1 && baga_st_ptrs[w].slot == q) {\n");
        fprintf(out, "                    const unsigned char *cp = (const unsigned char *)baga_st_ptrs[w].ptr;\n");
        fprintf(out, "                    fprintf(stderr, \" | %%p(rc=%%lu,an=%%lu)=\\\"\", baga_st_ptrs[w].ptr, ((baga_Hdr *)((char *)baga_st_ptrs[w].ptr - 32))->rc, ((baga_Hdr *)((char *)baga_st_ptrs[w].ptr - 32))->an);\n");
        fprintf(out, "                    for (int z = 0; z < 24 && cp[z] >= 32 && cp[z] < 127; z++) fputc(cp[z], stderr);\n");
        fprintf(out, "                    fprintf(stderr, \"\\\"\");\n");
        fprintf(out, "                    shown++;\n");
        fprintf(out, "                } }\n");
        fprintf(out, "            fprintf(stderr, \"\\n\");\n");
        fprintf(out, "        }\n");
        fprintf(out, "        { static int done = 0;\n");
        fprintf(out, "          for (int q = 0; q < BAGA_ST_NSLOT; q++) {\n");
        fprintf(out, "            if (!baga_st_sites[q].d) continue;\n");
        fprintf(out, "            long cnt = 0;\n");
        fprintf(out, "            for (unsigned w = 0; w < BAGA_ST_NPTR; w++) if (baga_st_ptrs[w].ptr && baga_st_ptrs[w].ptr != (void *)1 && baga_st_ptrs[w].slot == q) cnt++;\n");
        fprintf(out, "            if (cnt > 20) {\n");
        fprintf(out, "                fprintf(stderr, \"  MAP site#%%d maplive=%%ld:\", q, cnt);\n");
        fprintf(out, "                for (int i = 0; i < baga_st_sites[q].d; i++) fprintf(stderr, \" %%p\", baga_st_sites[q].bt[i]);\n");
        fprintf(out, "                fprintf(stderr, \"\\n\");\n");
        fprintf(out, "            }\n");
        fprintf(out, "          }\n");
        fprintf(out, "        }\n");
        fprintf(out, "        baga_st_un();\n");
        fprintf(out, "    }\n");
        fprintf(out, "    return NULL;\n");
        fprintf(out, "}\n");
        fprintf(out, "__attribute__((constructor)) static void baga_st_init(void) { if (getenv(\"BAGA_ALLOC_STATS\")) { pthread_t t; baga_st_on = 1; if (pthread_create(&t, NULL, baga_st_watch, NULL) == 0) pthread_detach(t); } }\n");
    }
    else {
        /* STR-1: non-RC persist полето пакетира кешираната дължина — бит 0
         * persist, бит 1 STR флаг, битове 2..63 дължина (len << 2). 16 B
         * header-ът и 16-байтовото подравняване на payload-а остават.
         * baga_lo/hi са range guard-ът за baga_str_len (четене на s - 16
         * пред литерал/argv може да е неподравнена страница). */
        fprintf(out, "typedef struct { uint64_t magic; uint64_t persist; } baga_Hdr;\n");
        fprintf(out, "static __thread char *baga_lo = NULL;\n");
        fprintf(out, "static __thread char *baga_hi = NULL;\n");
    }
    fprintf(out, "static void baga_free(void *p, int64_t n) {\n");
    fprintf(out, "    if (!p || n <= 0) return;\n");
    if (cg->rc)
        fprintf(out, "    if (baga_st_on) baga_st_free(p, n);\n");
    fprintf(out, "    baga_Hdr *h = (baga_Hdr *)((char *)p - %d);\n", hs);
    if (cg->rc)
        fprintf(out, "    int persist = (h->magic == BAGA_HDR_MAGIC) ? (int)(h->pe & 1) : 0;\n");
    else
        fprintf(out, "    int persist = (h->magic == BAGA_HDR_MAGIC) ? (int)(h->persist & 1) : 0;\n");
    /* MEM-7: persist блокът е отделен malloc. drop() от кой да е worker
     * връща паметта на libc. Преди това free пълнеше thread-local freelist,
     * а slab-ът оставаше в нишката, която е bump-нала — RSS растеше с
     * всяка заявка при 8 worker-а (hot span, page, scan snapshot). */
    fprintf(out, "    if (persist) { free(h); return; }\n");
    fprintf(out, "    if (n <= 1024) {\n");
    fprintf(out, "        int c = (int)((n + 15) / 16) - 1;\n");
    fprintf(out, "        void **fl = persist ? &baga_fl_p[c] : &baga_fl[c];\n");
    fprintf(out, "        *(void **)p = *fl; *fl = p;\n");
    fprintf(out, "    } else {\n");
    fprintf(out, "        int i = baga_fl_big_idx(n);\n");
    fprintf(out, "        if (i >= 0) {\n");
    fprintf(out, "            void **fl = persist ? &baga_fl_big_p[i] : &baga_fl_big[i];\n");
    fprintf(out, "            *(void **)p = *fl; *fl = p;\n");
    fprintf(out, "        }\n");
    fprintf(out, "    }\n");
    fprintf(out, "}\n");
    fprintf(out, "static void *baga_alloc(size_t n) {\n");
    fprintf(out, "    size_t rn = (n + 15) & ~(size_t)15;\n");
    fprintf(out, "    size_t an;\n");
    fprintf(out, "    int persist = baga_persist_depth > 0;\n");
    /* MEM-7: persist не влиза в thread-local bump. Иначе drop от друг
     * worker не връща slab-а и RSS расте линейно под BOILA_WORKERS>1. */
    fprintf(out, "    if (persist) {\n");
    fprintf(out, "        size_t pan;\n");
    fprintf(out, "        if (n == 0) pan = 16;\n");
    fprintf(out, "        else if (rn <= 1024) pan = rn;\n");
    fprintf(out, "        else { int bi = baga_fl_big_idx((int64_t)n); pan = bi >= 0 ? ((size_t)2048 << bi) : n; }\n");
    fprintf(out, "        char *blk = (char *)malloc(pan + %d);\n", hs);
    fprintf(out, "        if (!blk) { fprintf(stderr, \"baga: persist malloc\\n\"); exit(1); }\n");
    if (cg->rc)
        fprintf(out, "        { baga_Hdr *hh = (baga_Hdr *)blk; hh->magic = BAGA_HDR_MAGIC; hh->pe = ((uint64_t)baga_rc_epoch << 1) | 1ULL; hh->rc = 1; hh->an = pan; }\n");
    else
        fprintf(out, "        { baga_Hdr *hh = (baga_Hdr *)blk; hh->magic = BAGA_HDR_MAGIC; hh->persist = 1; }\n");
    if (cg->rc)
        fprintf(out, "        if (baga_st_on) baga_st_alloc(blk + %d, (long)pan);\n", hs);
    fprintf(out, "        return blk + %d;\n", hs);
    fprintf(out, "    }\n");
    fprintf(out, "    if (n == 0) {\n");
    fprintf(out, "        an = 0;\n");
    fprintf(out, "    } else if (rn <= 1024) {\n");
    fprintf(out, "        void **fl = persist ? &baga_fl_p[rn / 16 - 1] : &baga_fl[rn / 16 - 1];\n");
    fprintf(out, "        void *fb = *fl;\n");
    /* RC1: freelist блок пази стара rc стойност (0 при release-нат) — reset */
    if (cg->rc)
        fprintf(out, "        if (fb) { *fl = *(void **)fb; { baga_Hdr *_h = (baga_Hdr *)((char *)fb - %d); _h->rc = 1; _h->pe = ((uint64_t)baga_rc_epoch << 1) | (_h->pe & 1); if (baga_st_on) baga_st_alloc(fb, (long)_h->an); } return fb; }\n", hs);
    else
        fprintf(out, "        if (fb) { *fl = *(void **)fb; ((baga_Hdr *)((char *)fb - %d))->persist &= 1; return fb; }\n", hs);
    fprintf(out, "        an = rn;\n");
    fprintf(out, "    } else {\n");
    fprintf(out, "        int bi = baga_fl_big_idx((int64_t)n);\n");
    fprintf(out, "        if (bi >= 0) {\n");
    fprintf(out, "            void **fl = persist ? &baga_fl_big_p[bi] : &baga_fl_big[bi];\n");
    fprintf(out, "            void *fb = *fl;\n");
    if (cg->rc)
        fprintf(out, "            if (fb) { *fl = *(void **)fb; { baga_Hdr *_h = (baga_Hdr *)((char *)fb - %d); _h->rc = 1; _h->pe = ((uint64_t)baga_rc_epoch << 1) | (_h->pe & 1); if (baga_st_on) baga_st_alloc(fb, (long)_h->an); } return fb; }\n", hs);
    else
        fprintf(out, "            if (fb) { *fl = *(void **)fb; ((baga_Hdr *)((char *)fb - %d))->persist &= 1; return fb; }\n", hs);
    fprintf(out, "            an = ((size_t)2048 << bi);\n");
    fprintf(out, "        } else {\n");
    fprintf(out, "            an = n;\n");
    fprintf(out, "        }\n");
    fprintf(out, "    }\n");
    /* MEM-1 fix: малките алокации се bump-ват с КЛАСОВИЯ размер rn (не n) —
     * иначе free-list блок от по-малка заявка обслужва по-голяма в същия
     * клас и я презаписва съседния блок (segfault при review). Същото и за
     * pow2 класовете (R18): винаги пълен класов размер. */
    fprintf(out, "    baga_ABlk **hp = persist ? &baga_persist_head : &baga_arena_head;\n");
    fprintf(out, "    baga_ABlk *b = *hp;\n");
    fprintf(out, "    if (!b || b->used + an + %d > b->cap) {\n", hs);
    fprintf(out, "        size_t cap = an + %d > 8192 ? an + %d : 8192;\n", hs, hs);
    /* RC1: първо spare веригата (блокове, върнати от rewind) — без malloc
     * на всяка заявка; cap стига само ако покрива заявката. Новите адреси
     * вече са в [lo,hi] — range guard не се обновява при spare reuse. */
    if (cg->rc) {
        fprintf(out, "        if (baga_rc_spare && baga_rc_spare->cap >= cap) {\n");
        fprintf(out, "            b = baga_rc_spare; baga_rc_spare = b->next;\n");
        fprintf(out, "            b->next = *hp; b->used = 0;\n");
        fprintf(out, "            baga_mt_bump((long)b->cap); atomic_fetch_sub(&baga_mt_spare, (long)b->cap);\n");
        fprintf(out, "        } else {\n");
        fprintf(out, "            b = (baga_ABlk *)malloc(sizeof(baga_ABlk) + cap);\n");
        fprintf(out, "            b->next = *hp; b->used = 0; b->cap = cap;\n");
        fprintf(out, "            if (!baga_rc_lo || (char *)b->data < baga_rc_lo) baga_rc_lo = (char *)b->data;\n");
        fprintf(out, "            if ((char *)b->data + cap > baga_rc_hi) baga_rc_hi = (char *)b->data + cap;\n");
        fprintf(out, "            baga_mt_bump((long)cap);\n");
        fprintf(out, "        }\n");
        fprintf(out, "        *hp = b;\n");
    } else {
        /* MEM-5: non-RC — първо spare веригата (блокове от rewind), после malloc */
        fprintf(out, "        if (baga_spare && baga_spare->cap >= cap) {\n");
        fprintf(out, "            b = baga_spare; baga_spare = b->next;\n");
        fprintf(out, "            b->next = *hp; b->used = 0;\n");
        fprintf(out, "            baga_mt_bump((long)b->cap); atomic_fetch_sub(&baga_mt_spare, (long)b->cap);\n");
        fprintf(out, "        } else {\n");
        fprintf(out, "            b = (baga_ABlk *)malloc(sizeof(baga_ABlk) + cap);\n");
        fprintf(out, "            b->next = *hp; b->used = 0; b->cap = cap;\n");
        fprintf(out, "            if (!baga_lo || (char *)b->data < baga_lo) baga_lo = (char *)b->data;\n");
        fprintf(out, "            if ((char *)b->data + cap > baga_hi) baga_hi = (char *)b->data + cap;\n");
        fprintf(out, "            baga_mt_bump((long)cap);\n");
        fprintf(out, "        }\n");
        fprintf(out, "        *hp = b;\n");
    }
    fprintf(out, "    }\n");
    fprintf(out, "    void *p = b->data + b->used; b->used += an + %d;\n", hs);
    /* RC1: собственикът на нова алокация е първият binding → rc=1 */
    if (cg->rc)
        fprintf(out, "    baga_Hdr *hh = (baga_Hdr *)p; hh->magic = BAGA_HDR_MAGIC; hh->pe = ((uint64_t)baga_rc_epoch << 1) | (uint64_t)persist; hh->rc = 1; hh->an = an;\n");
    else
        fprintf(out, "    baga_Hdr *hh = (baga_Hdr *)p; hh->magic = BAGA_HDR_MAGIC; hh->persist = (uint64_t)persist;\n");
    if (cg->rc)
        fprintf(out, "    if (baga_st_on) baga_st_alloc((char *)p + %d, (long)an);\n", hs);
    fprintf(out, "    return (char *)p + %d;\n", hs);
    fprintf(out, "}\n");
    /* STR-1: кеширана дължина на низ — non-RC чете през range guard + magic
     * (литерали/argv са извън [baga_lo,baga_hi) → strlen); RC е no-op
     * (strlen) засега. Producer-ите викат baga_set_slen еднакво и в двата
     * режима, така че споделеният код се компилира и под --rc.
     * ИНВАРИАНТ: slen == strlen(s) винаги — str е C низ и NUL го съкращава
     * (str_of_bytes/read_file на двоичен вход); тези producers ползват
     * strlen(r), не „естествената" дължина (иначе LLVM-оракулът лови). */
    /* STR-1 fix: [baga_lo,baga_hi) е ОБХВАТЪТ на всички bump блокове, но
     * блоковете НЕ са непрекъснати — glibc/ASan връщат различни региони за
     * различни размери, затова обхватът може да покрие .rodata и четенето на
     * s-16 пред литерал е out-of-bounds (ASan го хваща; извън ASan е тих
     * прочит на чужда памет или segfault, ако s-16 е некартирана). Обхватът
     * остава като евтин reject, а доказателството е per-блоковата проверка:
     * s-16 се чете само ако s наистина е в някой блок. Блоковете никога не се
     * free-ват (MEM-5 връща ги в spare), значи кешираният hit не увисва. */
    if (!cg->rc) {
        fprintf(out, "static __thread baga_ABlk *baga_blk_hit = NULL;\n");
        fprintf(out, "static inline int baga_str_in_bump(const char *s) {\n");
        fprintf(out, "    baga_ABlk *b = baga_blk_hit;\n");
        fprintf(out, "    if (b && s >= b->data + %d && s < b->data + b->cap) return 1;\n", hs);
        fprintf(out, "    if (!baga_lo || s < baga_lo + %d || s >= baga_hi) return 0;\n", hs);
        fprintf(out, "    for (b = baga_arena_head; b; b = b->next)\n");
        fprintf(out, "        if (s >= b->data + %d && s < b->data + b->cap) { baga_blk_hit = b; return 1; }\n", hs);
        fprintf(out, "    for (b = baga_persist_head; b; b = b->next)\n");
        fprintf(out, "        if (s >= b->data + %d && s < b->data + b->cap) { baga_blk_hit = b; return 1; }\n", hs);
        fprintf(out, "    return 0;\n");
        fprintf(out, "}\n");
    }
    fprintf(out, "static inline __attribute__((always_inline)) int64_t baga_str_len(const char *s) {\n");
    if (cg->rc) {
        fprintf(out, "    return (int64_t)strlen(s);\n");
    } else {
        fprintf(out, "    if (!s || !baga_str_in_bump(s)) return (int64_t)strlen(s);\n");
        fprintf(out, "    baga_Hdr *h = (baga_Hdr *)((char *)s - %d);\n", hs);
        fprintf(out, "    if (h->magic != BAGA_HDR_MAGIC || !(h->persist & 2)) return (int64_t)strlen(s);\n");
        fprintf(out, "    return (int64_t)(h->persist >> 2);\n");
    }
    fprintf(out, "}\n");
    fprintf(out, "static inline __attribute__((always_inline)) void baga_set_slen(const char *s, int64_t n) {\n");
    if (cg->rc) {
        fprintf(out, "    (void)s; (void)n;\n");
    } else {
        fprintf(out, "    baga_Hdr *h = (baga_Hdr *)((char *)s - %d);\n", hs);
        fprintf(out, "    h->persist = (h->persist & 1) | 2 | ((uint64_t)n << 2);\n");
    }
    fprintf(out, "}\n");
    /* RC1: retain/release ядро. magic check прави C литералите и външните
     * буфери „immortal" (no-op); release на rc==0 е underflow → чиста
     * грешка + exit(1) (двоен drop), не UB.
     * Range guard: header-ът се чете само за указатели в обхвата на arena
     * блоковете — литерал/външен буфер никога не се дереференцира (четенето
     * на p-24 пред чужд литерал може да е неподравнена страница). */
    if (cg->rc) {
        fprintf(out, "static inline __attribute__((always_inline)) baga_Hdr *baga_rc_hdr(void *p) {\n");
        fprintf(out, "    if (!p || !baga_rc_lo || (char *)p < baga_rc_lo + %d || (char *)p >= baga_rc_hi) return NULL;\n", hs);
        fprintf(out, "    baga_Hdr *h = (baga_Hdr *)((char *)p - %d);\n", hs);
        fprintf(out, "    if (h->magic != BAGA_HDR_MAGIC) return NULL;\n");
        fprintf(out, "    if (!(h->pe & 1) && (h->pe >> 1) != baga_rc_epoch) return NULL;\n");
        fprintf(out, "    return h;\n");
        fprintf(out, "}\n");
        fprintf(out, "static inline __attribute__((always_inline)) void baga_rc_retain(void *p) {\n");
        fprintf(out, "    baga_Hdr *h = baga_rc_hdr(p);\n");
        fprintf(out, "    if (h) h->rc++;\n");
        fprintf(out, "}\n");
        fprintf(out, "static inline __attribute__((always_inline)) void baga_rc_release_str(const char *s) {\n");
        fprintf(out, "    baga_Hdr *h = baga_rc_hdr((void *)s);\n");
        fprintf(out, "    if (!h) return;\n");
        fprintf(out, "    if (h->rc == 0) { fprintf(stderr, \"baga: rc underflow (str — двоен release)\\n\"); exit(1); }\n");
        /* RC1: free с КЛАСОВИЯ размер an (не strlen+1) — иначе блок от
         * фиксирана алокация (i64_to_str = 24 B) миграва в по-малък
         * freelist клас и класът му остава без reuse (тече bump-ът) */
        fprintf(out, "    if (--h->rc == 0) baga_free((void *)s, (int64_t)h->an);\n");
        fprintf(out, "}\n");
    }
    /* MEM-4: mark/rewind върху ephemeral веригата. persist веригата не се
     * пипа — затова shared state отива там през mem_persist_begin/end. */
    fprintf(out, "typedef struct { baga_ABlk *head; size_t used; } baga_MMark;\n");
    fprintf(out, "static int64_t baga_mem_mark(void) {\n");
    fprintf(out, "    baga_MMark *m = (baga_MMark *)malloc(sizeof(baga_MMark));\n");
    fprintf(out, "    if (!m) { fprintf(stderr, \"baga: mem_mark: няма памет\\n\"); exit(1); }\n");
    fprintf(out, "    m->head = baga_arena_head;\n");
    fprintf(out, "    m->used = baga_arena_head ? baga_arena_head->used : 0;\n");
    fprintf(out, "    return (int64_t)(intptr_t)m;\n");
    fprintf(out, "}\n");
    fprintf(out, "static void baga_mem_rewind(int64_t h) {\n");
    fprintf(out, "    baga_MMark *m = (baga_MMark *)(intptr_t)h;\n");
    fprintf(out, "    if (!m) { fprintf(stderr, \"baga: mem_rewind: невалиден mark\\n\"); exit(1); }\n");
    /* RC1: нова epoch — release на ephemeral стойности от преди rewind
     * става no-op (паметта им се връща по-долу; header-ите им са мъртви) */
    if (cg->rc)
        fprintf(out, "    baga_rc_epoch++;\n");
    fprintf(out, "    baga_ABlk *b = baga_arena_head;\n");
    fprintf(out, "    while (b && b != m->head) {\n");
    fprintf(out, "        baga_ABlk *nx = b->next;\n");
    fprintf(out, "        baga_mt_bump(-(long)b->cap); atomic_fetch_add(&baga_mt_spare, (long)b->cap);\n");
    /* RC1: в spare веригата вместо free() — паметта остава mapped (epoch
     * я прави мъртва за rc_hdr) и се reuse-ва от следващите алокации.
     * MEM-5: същото и за non-RC (baga_spare). */
    if (cg->rc)
        fprintf(out, "        b->next = baga_rc_spare; baga_rc_spare = b;\n");
    else
        fprintf(out, "        b->next = baga_spare; baga_spare = b;\n");
    fprintf(out, "        b = nx;\n");
    fprintf(out, "    }\n");
    fprintf(out, "    if (b != m->head) {\n");
    fprintf(out, "        fprintf(stderr, \"baga: mem_rewind: mark не е в arena веригата (двоен rewind или нарушен LIFO ред)\\n\");\n");
    fprintf(out, "        exit(1);\n");
    fprintf(out, "    }\n");
    fprintf(out, "    baga_arena_head = m->head;\n");
    fprintf(out, "    if (m->head) {\n");
    fprintf(out, "        m->head->used = m->used;\n");
    fprintf(out, "    }\n");
    /* MEM-4c: вместо per-блок freelist scrub (O(блокове × freelist записи) —
     * измерен колапс при 1M insert bench: rewind > statement работа), двете
     * ephemeral freelist таблици се ЧИСТЯТ изцяло за O(1). Записи към
     * върнатите блокове са задължително премахнати; записи към оцелелия под
     * mark-а регион също падат — загуба на recycling, не корупция (freelist
     * е чист кеш; пълни се наново от бъдещи drop-ове). persist freelist-ите
     * не се пипат — persist блокове не се връщат от rewind. */
    fprintf(out, "    memset(baga_fl, 0, sizeof(baga_fl));\n");
    fprintf(out, "    memset(baga_fl_big, 0, sizeof(baga_fl_big));\n");
    fprintf(out, "    free(m);\n");
    fprintf(out, "}\n");

    /* runtime helpers */
    fprintf(out, "static void baga_print_i64(int64_t v) { printf(\"%%lld\\n\", (long long)v); }\n");
    fprintf(out, "static void baga_print_f64(double v)  { printf(\"%%g\\n\", v); }\n");
    fprintf(out, "static void baga_print_str(const char *s) { printf(\"%%s\\n\", s); }\n");
    fprintf(out, "static void baga_write(const char *s) { printf(\"%%s\", s); }\n");
    fprintf(out, "static int64_t baga_len(const char *s) { return baga_str_len(s); }\n");
    fprintf(out, "static void baga_bounds_fail(const char *fn, int64_t i, int64_t len) {\n");
    fprintf(out, "    fprintf(stderr, \"baga: %%s: индекс %%lld извън границите [0, %%lld)\\n\", fn, (long long)i, (long long)len);\n");
    fprintf(out, "    exit(1);\n");
    fprintf(out, "}\n");
    fprintf(out, "static void baga_div_zero_fail(void) {\n");
    fprintf(out, "    fprintf(stderr, \"baga: деление на нула\\n\");\n");
    fprintf(out, "    exit(1);\n");
    fprintf(out, "}\n");
    fprintf(out, "static int64_t baga_char_at(const char *s, int64_t i) {\n");
    fprintf(out, "    int64_t n = baga_str_len(s);\n");
    fprintf(out, "    if (i < 0 || i >= n) baga_bounds_fail(\"char_at\", i, n);\n");
    fprintf(out, "    return (int64_t)(unsigned char)s[i];\n");
    fprintf(out, "}\n");
    fprintf(out, "static int64_t baga_byte_at(const char *s, int64_t i) { return (int64_t)(unsigned char)s[i]; }\n");
    fprintf(out, "static const char *baga_byte_chr(int64_t c) { char *r = baga_alloc(2); r[0] = (char)c; r[1] = 0; baga_set_slen(r, c == 0 ? 0 : 1); return r; }\n");
    fprintf(out, "static const char *baga_substr(const char *s, int64_t a, int64_t b) {\n");
    fprintf(out, "    int64_t n = baga_str_len(s);\n");
    fprintf(out, "    if (a < 0 || a > n) baga_bounds_fail(\"substr\", a, n);\n");
    fprintf(out, "    if (b < 0 || b > n) baga_bounds_fail(\"substr\", b, n);\n");
    fprintf(out, "    int64_t len = b - a; if (len < 0) len = 0;\n");
    fprintf(out, "    char *r = baga_alloc((size_t)len + 1); memcpy(r, s + a, (size_t)len); r[len] = 0; baga_set_slen(r, len); return r;\n");
    fprintf(out, "}\n");
    fprintf(out, "static const char *baga_concat(const char *a, const char *b) {\n");
    fprintf(out, "    size_t la = (size_t)baga_str_len(a), lb = (size_t)baga_str_len(b);\n");
    fprintf(out, "    char *r = baga_alloc(la + lb + 1); memcpy(r, a, la); memcpy(r + la, b, lb + 1); baga_set_slen(r, (int64_t)(la + lb)); return r;\n");
    fprintf(out, "}\n");
    /* P55: една алокация за повторение. Baga-версията правеше
     * concat-двойки под mem_persist и оставяше ~4× повече persist блокове
     * (scratch release на всяка връзка). */
    fprintf(out, "static const char *baga_str_repeat(const char *s, int64_t n) {\n");
    fprintf(out, "    if (!s || n <= 0) return \"\";\n");
    fprintf(out, "    int64_t l = baga_str_len(s);\n");
    fprintf(out, "    if (l == 0) return \"\";\n");
    fprintf(out, "    if (n > (INT64_MAX - 1) / l) { fprintf(stderr, \"baga: str_repeat: прекалено дълъг низ\\n\"); exit(1); }\n");
    fprintf(out, "    int64_t total = l * n;\n");
    fprintf(out, "    char *r = baga_alloc((size_t)total + 1);\n");
    fprintf(out, "    for (int64_t i = 0; i < n; i++) memcpy(r + i * l, s, (size_t)l);\n");
    fprintf(out, "    r[total] = 0; baga_set_slen(r, total); return r;\n");
    fprintf(out, "}\n");
    fprintf(out, "static const char *baga_read_file(const char *path) {\n");
    fprintf(out, "    FILE *f = fopen(path, \"rb\"); if (!f) return \"\";\n");
    fprintf(out, "    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);\n");
    fprintf(out, "    char *buf = baga_alloc((size_t)sz + 1); fread(buf, 1, (size_t)sz, f); buf[sz] = 0; baga_set_slen(buf, (int64_t)strlen(buf)); fclose(f); return buf;\n");
    fprintf(out, "}\n");
    fprintf(out, "static const char *baga_chr(int64_t c) {\n");
    fprintf(out, "    char *r = baga_alloc(5);\n");
    fprintf(out, "    if (c < 0x80) { r[0] = (char)c; r[1] = 0; baga_set_slen(r, c == 0 ? 0 : 1); }\n");
    fprintf(out, "    else if (c < 0x800) { r[0] = (char)(0xC0|(c>>6)); r[1] = (char)(0x80|(c&0x3F)); r[2] = 0; baga_set_slen(r, 2); }\n");
    fprintf(out, "    else if (c < 0x10000) { r[0] = (char)(0xE0|(c>>12)); r[1] = (char)(0x80|((c>>6)&0x3F)); r[2] = (char)(0x80|(c&0x3F)); r[3] = 0; baga_set_slen(r, 3); }\n");
    fprintf(out, "    else { r[0] = (char)(0xF0|(c>>18)); r[1] = (char)(0x80|((c>>12)&0x3F)); r[2] = (char)(0x80|((c>>6)&0x3F)); r[3] = (char)(0x80|(c&0x3F)); r[4] = 0; baga_set_slen(r, 4); }\n");
    fprintf(out, "    return r;\n");
    fprintf(out, "}\n");
    fprintf(out, "static int64_t baga_ord(const char *s) {\n");
    fprintf(out, "    unsigned char c = (unsigned char)s[0]; if (!c) return 0;\n");
    fprintf(out, "    if (c < 0x80) return c;\n");
    fprintf(out, "    if ((c&0xE0)==0xC0) return ((int64_t)(c&0x1F)<<6)|((int64_t)(unsigned char)s[1]&0x3F);\n");
    fprintf(out, "    if ((c&0xF0)==0xE0) return ((int64_t)(c&0x0F)<<12)|(((int64_t)(unsigned char)s[1]&0x3F)<<6)|((int64_t)(unsigned char)s[2]&0x3F);\n");
    fprintf(out, "    return ((int64_t)(c&0x07)<<18)|(((int64_t)(unsigned char)s[1]&0x3F)<<12)|(((int64_t)(unsigned char)s[2]&0x3F)<<6)|((int64_t)(unsigned char)s[3]&0x3F);\n");
    fprintf(out, "}\n");
    fprintf(out, "static const char *baga_i64_to_str(int64_t x) { char *r = baga_alloc(24); int64_t n = (int64_t)snprintf(r, 24, \"%%lld\", (long long)x); baga_set_slen(r, n); return r; }\n");
    fprintf(out, "static const char *baga_f64_to_str(double x) { char *r = baga_alloc(32); int64_t n = (int64_t)snprintf(r, 32, \"%%g\", x); baga_set_slen(r, n); return r; }\n");
    fprintf(out, "static int64_t baga_str_eq(const char *a, const char *b) { return strcmp(a, b) == 0; }\n");
    /* M22: отмествания с маскиран брояч (b & 63) — детерминирано, паритет
     * с LLVM (shl/ashr след and 63). C `>>` върху отрицателни е
     * имплементационно дефиниран; baga_ashr_i64 е аритметично (floor). */
    fprintf(out, "static int64_t baga_ashr_i64(int64_t a, int64_t b) {\n");
    fprintf(out, "    uint64_t m = (uint64_t)b & 63;\n");
    fprintf(out, "    if (m == 0) return a;\n");
    fprintf(out, "    uint64_t u = (uint64_t)a;\n");
    fprintf(out, "    uint64_t s = (a < 0) ? ~UINT64_C(0) : 0;\n");
    fprintf(out, "    return (int64_t)((u >> (unsigned)m) | (s << (unsigned)(64 - m)));\n");
    fprintf(out, "}\n");
    fprintf(out, "static int64_t baga_shl_i64(int64_t a, int64_t b) {\n");
    fprintf(out, "    return (int64_t)((uint64_t)a << ((uint64_t)b & 63));\n");
    fprintf(out, "}\n");
    /* M23: логическо отместване (zero-fill) — паритет с LLVM LShr. */
    fprintf(out, "static int64_t baga_lshr_i64(int64_t a, int64_t b) {\n");
    fprintf(out, "    return (int64_t)((uint64_t)a >> ((uint64_t)b & 63));\n");
    fprintf(out, "}\n");
    fprintf(out, "static int baga_argc = 0;\n");
    fprintf(out, "static char **baga_argv = 0;\n");
    fprintf(out, "static int64_t baga_arg_count(void) { return baga_argc > 0 ? baga_argc - 1 : 0; }\n");
    fprintf(out, "static const char *baga_arg(int64_t i) { return (i + 1 < baga_argc) ? baga_argv[i + 1] : \"\"; }\n");
    fprintf(out, "static void baga_exit(int64_t c) { exit((int)c); }\n");
    fprintf(out, "static void baga_eprintln(const char *s) { fprintf(stderr, \"%%s\\n\", s); }\n");
    fprintf(out, "static int64_t baga_cur_args[16];\n");
    fprintf(out, "static int baga_cur_nargs = 0;\n");
    fprintf(out, "static void baga_spec_fail(const char *spec, const char *kind, int64_t idx, const char *expr) {\n");
    fprintf(out, "    if (strcmp(kind, \"requires\") == 0)\n");
    fprintf(out, "        fprintf(stderr, \"spec '%%s': requires #%%lld нарушено: %%s\\n\", spec, (long long)idx, expr);\n");
    fprintf(out, "    else\n");
    fprintf(out, "        fprintf(stderr, \"spec '%%s': ensures #%%lld нарушена: %%s\\n\", spec, (long long)idx, expr);\n");
    fprintf(out, "    if (baga_cur_nargs > 0) {\n");
    fprintf(out, "        fprintf(stderr, \"  вход: \");\n");
    fprintf(out, "        for (int i = 0; i < baga_cur_nargs; i++)\n");
    fprintf(out, "            fprintf(stderr, \"%%s%%lld\", i ? \", \" : \"\", (long long)baga_cur_args[i]);\n");
    fprintf(out, "        fprintf(stderr, \"\\n\");\n");
    fprintf(out, "    }\n");
    fprintf(out, "    exit(1);\n");
    fprintf(out, "}\n");
    fprintf(out, "\n");
    fprintf(out, "/* binary-safe byte buffer */\n");
    fprintf(out, "typedef struct { unsigned char *data; int64_t len; } baga_bytes;\n");
    /* M20: effect payloads — tagged error slot (thread-local). tag = 0 на
     * нормалния път; raise задава tag + payload; ?/catch четат/нулират. */
    fprintf(out, "typedef struct { int64_t tag; int64_t i; double f; const char *s; baga_bytes b; } baga_eff;\n");
    fprintf(out, "static __thread baga_eff baga_eff_tl;\n");
    /* RC1: release на bytes — rc живее върху data алокацията */
    if (cg->rc) {
        fprintf(out, "static inline __attribute__((always_inline)) void baga_rc_release_bytes(baga_bytes b) {\n");
        fprintf(out, "    baga_Hdr *h = baga_rc_hdr((void *)b.data);\n");
        fprintf(out, "    if (!h) return;\n");
        fprintf(out, "    if (h->rc == 0) { fprintf(stderr, \"baga: rc underflow (bytes — двоен release)\\n\"); exit(1); }\n");
        fprintf(out, "    if (--h->rc == 0) baga_free(b.data, (int64_t)h->an);\n");
        fprintf(out, "}\n");
    }
    fprintf(out, "static int64_t baga_bytes_len(baga_bytes b) { return b.len; }\n");
    fprintf(out, "static int64_t baga_bytes_at(baga_bytes b, int64_t i) {\n");
    fprintf(out, "    if (i < 0 || i >= b.len) baga_bounds_fail(\"bytes_at\", i, b.len);\n");
    fprintf(out, "    return (int64_t)b.data[i];\n");
    fprintf(out, "}\n");
    fprintf(out, "static baga_bytes baga_bytes_slice(baga_bytes b, int64_t a, int64_t c) {\n");
    fprintf(out, "    if (a < 0) a = 0; if (c > b.len) c = b.len; if (c < a) c = a;\n");
    fprintf(out, "    baga_bytes r; r.len = c - a; r.data = baga_alloc((size_t)(r.len ? r.len : 1));\n");
    fprintf(out, "    memcpy(r.data, b.data + a, (size_t)r.len); return r; }\n");
    fprintf(out, "static baga_bytes baga_bytes_concat(baga_bytes a, baga_bytes b) {\n");
    fprintf(out, "    baga_bytes r; r.len = a.len + b.len; r.data = baga_alloc((size_t)(r.len ? r.len : 1));\n");
    fprintf(out, "    memcpy(r.data, a.data, (size_t)a.len); memcpy(r.data + a.len, b.data, (size_t)b.len); return r; }\n");
    /* R54: in-place bulk append — dst[off..off+src.len) = src. Kills the
       O(n²) concat chain when assembling pipelined replies. */
    fprintf(out, "static void baga_bytes_put(baga_bytes d, int64_t off, baga_bytes s) {\n");
    fprintf(out, "    if (off < 0 || off + s.len > d.len) return;\n");
    fprintf(out, "    memcpy(d.data + off, s.data, (size_t)s.len); }\n");
    fprintf(out, "static baga_bytes baga_bytes_new(int64_t n) {\n");
    fprintf(out, "    if (n < 0) n = 0;\n");
    fprintf(out, "    baga_bytes b; b.len = n; b.data = baga_alloc(n > 0 ? n : 1);\n");
    fprintf(out, "    memset(b.data, 0, (size_t)(n > 0 ? n : 1));\n");
    fprintf(out, "    return b;\n");
    fprintf(out, "}\n");
    fprintf(out, "static void baga_bytes_set(baga_bytes b, int64_t i, int64_t v) {\n");
    fprintf(out, "    if (i < 0 || i >= b.len) baga_bounds_fail(\"bytes_set\", i, b.len);\n");
    fprintf(out, "    b.data[i] = (unsigned char)(v & 0xff);\n");
    fprintf(out, "}\n");
    fprintf(out, "static baga_bytes baga_bytes_push(baga_bytes b, int64_t v) {\n");
    fprintf(out, "    baga_bytes r; r.len = b.len + 1; r.data = baga_alloc(r.len);\n");
    fprintf(out, "    memcpy(r.data, b.data, (size_t)b.len);\n");
    fprintf(out, "    r.data[b.len] = (unsigned char)(v & 0xff);\n");
    fprintf(out, "    return r;\n");
    fprintf(out, "}\n");
    fprintf(out, "static baga_bytes baga_bytes_from_str(const char *s) {\n");
    fprintf(out, "    int64_t n = baga_str_len(s); baga_bytes r; r.len = n; r.data = baga_alloc((size_t)(n ? n : 1));\n");
    fprintf(out, "    memcpy(r.data, s, (size_t)n); return r; }\n");
    fprintf(out, "static baga_bytes baga_bytes_lit(const unsigned char *d, int64_t n) {\n");
    fprintf(out, "    baga_bytes r; r.len = n; r.data = baga_alloc((size_t)(n ? n : 1));\n");
    fprintf(out, "    memcpy(r.data, d, (size_t)n); return r; }\n");
    fprintf(out, "static const char *baga_bytes_to_str(baga_bytes b) {\n");
    fprintf(out, "    char *r = baga_alloc((size_t)b.len + 1); memcpy(r, b.data, (size_t)b.len); r[b.len] = 0; baga_set_slen(r, (int64_t)strlen(r)); return r; }\n");
    fprintf(out, "static int baga_hex_val(int c) {\n");
    fprintf(out, "    if (c >= '0' && c <= '9') return c - '0';\n");
    fprintf(out, "    if (c >= 'a' && c <= 'f') return c - 'a' + 10;\n");
    fprintf(out, "    if (c >= 'A' && c <= 'F') return c - 'A' + 10;\n");
    fprintf(out, "    return -1; }\n");
    fprintf(out, "static const char *baga_hex_encode(baga_bytes b) {\n");
    fprintf(out, "    static const char *hx = \"0123456789abcdef\";\n");
    fprintf(out, "    char *r = baga_alloc((size_t)b.len * 2 + 1);\n");
    fprintf(out, "    for (int64_t i = 0; i < b.len; i++) { r[i*2] = hx[b.data[i] >> 4]; r[i*2+1] = hx[b.data[i] & 15]; }\n");
    fprintf(out, "    r[b.len * 2] = 0; baga_set_slen(r, b.len * 2); return r; }\n");
    fprintf(out, "static baga_bytes baga_hex_decode(const char *s) {\n");
    fprintf(out, "    int64_t n = baga_str_len(s); unsigned char *buf = baga_alloc((size_t)(n / 2 + 1)); int64_t len = 0;\n");
    fprintf(out, "    for (int64_t i = 0; i + 1 < n; ) {\n");
    fprintf(out, "        int hi = baga_hex_val(s[i]); int lo = baga_hex_val(s[i+1]);\n");
    fprintf(out, "        if (hi < 0 || lo < 0) { i++; continue; }\n");
    fprintf(out, "        buf[len++] = (unsigned char)(hi * 16 + lo); i += 2; }\n");
    fprintf(out, "    baga_bytes r; r.data = buf; r.len = len; return r; }\n");
    fprintf(out, "static baga_bytes baga_bytes_from_hex(const char *s) { return baga_hex_decode(s); }\n");
    fprintf(out, "\n");
    fprintf(out, "/* dynamic array */\n");
    fprintf(out, "typedef struct { void **data; int64_t len; int64_t cap; } baga_Vec;\n");
    fprintf(out, "static baga_Vec *baga_vec_new(void) {\n");
    fprintf(out, "    baga_Vec *v = baga_alloc(sizeof(baga_Vec));\n");
    fprintf(out, "    v->cap = 8; v->len = 0; v->data = baga_alloc(8 * sizeof(void *)); return v;\n");
    fprintf(out, "}\n");
    /* RC1: release на Vec — rc живее върху STRUCT алокацията (data масивът е
     * притежаван 1:1 от структурата). При rc→0: release на елементите според
     * elem_kind (0 inline, 1 str, 2 struct box, 3 nested vec, 4 bytes box),
     * после free на data + struct. Nested (3) release-ва вътрешните като
     * kind 0 — елементният им тип не е известен по време на изпълнение.
     * RC5 v0.2: elem_rel е destructor на box полетата (NULL → само free).
     * RC5 v0.9: при kind 3 elem_rel е destructor на вложения Vec<S>
     * (baga_rc_relv_<S>; NULL → старото поведение, S полетата текат). */
    if (cg->rc) {
        fprintf(out, "static void baga_rc_release_vec(baga_Vec *v, int elem_kind, int64_t elem_size, void (*elem_rel)(void *)) {\n");
        fprintf(out, "    if (!v) return;\n");
        fprintf(out, "    baga_Hdr *h = baga_rc_hdr((void *)v);\n");
        fprintf(out, "    if (!h) return;\n");
        fprintf(out, "    if (h->rc == 0) { fprintf(stderr, \"baga: rc underflow (vec — двоен release)\\n\"); exit(1); }\n");
        fprintf(out, "    if (--h->rc > 0) return;\n");
        fprintf(out, "    if (elem_kind == 1)\n");
        fprintf(out, "        for (int64_t i = 0; i < v->len; i++) baga_rc_release_str((const char *)v->data[i]);\n");
        fprintf(out, "    else if (elem_kind == 2)\n");
        fprintf(out, "        for (int64_t i = 0; i < v->len; i++) { if (elem_rel) elem_rel(v->data[i]); baga_free(v->data[i], elem_size); }\n");
        fprintf(out, "    else if (elem_kind == 3)\n");
        fprintf(out, "        for (int64_t i = 0; i < v->len; i++) { if (elem_rel) elem_rel(v->data[i]); else baga_rc_release_vec((baga_Vec *)v->data[i], 0, 0, NULL); }\n");
        fprintf(out, "    else if (elem_kind == 4)\n");
        fprintf(out, "        for (int64_t i = 0; i < v->len; i++) { baga_bytes *bb = (baga_bytes *)v->data[i]; baga_rc_release_bytes(*bb); baga_free(bb, elem_size); }\n");
        fprintf(out, "    baga_free(v->data, v->cap * 8);\n");
        fprintf(out, "    baga_free(v, (int64_t)sizeof(baga_Vec));\n");
        fprintf(out, "}\n");
    }
    fprintf(out, "static void baga_vec_grow(baga_Vec *v) {\n");
    /* MEM-4д: старият data буфер се free-ва при doubling — иначе в persist
     * региона всяко vec растене оставя завинаги стълбичка от intermediate
     * буфери (измерено: ~2.7 doubling-стълбички/ред → 29 GB при 250k реда). */
    fprintf(out, "    if (v->len == v->cap) { int64_t ocap = v->cap; void **od = v->data; v->cap *= 2;\n");
    fprintf(out, "        void **nd = (void **)baga_alloc((size_t)v->cap * sizeof(void *));\n");
    fprintf(out, "        memcpy(nd, od, (size_t)v->len * sizeof(void *)); v->data = nd;\n");
    fprintf(out, "        baga_free(od, ocap * (int64_t)sizeof(void *)); }\n");
    fprintf(out, "}\n");
    fprintf(out, "static void baga_vec_push_i64(baga_Vec *v, int64_t x) { baga_vec_grow(v); v->data[v->len++] = (void *)(intptr_t)x; }\n");
    fprintf(out, "static int64_t baga_vec_get_i64(baga_Vec *v, int64_t i) {\n");
    fprintf(out, "    if (i < 0 || i >= v->len) baga_bounds_fail(\"vec_get\", i, v->len);\n");
    fprintf(out, "    return (int64_t)(intptr_t)v->data[i];\n");
    fprintf(out, "}\n");
    fprintf(out, "static void baga_vec_set_i64(baga_Vec *v, int64_t i, int64_t x) {\n");
    fprintf(out, "    if (i < 0 || i >= v->len) baga_bounds_fail(\"vec_set\", i, v->len);\n");
    fprintf(out, "    v->data[i] = (void *)(intptr_t)x;\n");
    fprintf(out, "}\n");
    /* RC1: push/set retain-ват елемента (контейнерът става собственик);
     * set release-ва стария след retain на новия (alias-safe ред) */
    if (cg->rc)
        fprintf(out, "static void baga_vec_push_str(baga_Vec *v, const char *s) { baga_rc_retain((void *)s); baga_vec_grow(v); v->data[v->len++] = (void *)s; }\n");
    else
        fprintf(out, "static void baga_vec_push_str(baga_Vec *v, const char *s) { baga_vec_grow(v); v->data[v->len++] = (void *)s; }\n");
    fprintf(out, "static const char *baga_vec_get_str(baga_Vec *v, int64_t i) {\n");
    fprintf(out, "    if (i < 0 || i >= v->len) baga_bounds_fail(\"vec_get\", i, v->len);\n");
    fprintf(out, "    return (const char *)v->data[i];\n");
    fprintf(out, "}\n");
    fprintf(out, "static void baga_vec_set_str(baga_Vec *v, int64_t i, const char *s) {\n");
    fprintf(out, "    if (i < 0 || i >= v->len) baga_bounds_fail(\"vec_set\", i, v->len);\n");
    /* RC1: retain на новия, release на стария (в този ред — alias-safe) */
    if (cg->rc) fprintf(out, "    baga_rc_retain((void *)s); baga_rc_release_str((const char *)v->data[i]);\n");
    fprintf(out, "    v->data[i] = (void *)s;\n");
    fprintf(out, "}\n");
    /* RC3: move варианти — без retain на новия; източникът е last-use локал,
     * чиято референция преминава в контейнера (codegen го маркира dead).
     * set release-ва стария елемент както обикновено. */
    if (cg->rc) {
        fprintf(out, "static void baga_vec_push_str_move(baga_Vec *v, const char *s) { baga_vec_grow(v); v->data[v->len++] = (void *)s; }\n");
        fprintf(out, "static void baga_vec_set_str_move(baga_Vec *v, int64_t i, const char *s) {\n");
        fprintf(out, "    if (i < 0 || i >= v->len) baga_bounds_fail(\"vec_set\", i, v->len);\n");
        fprintf(out, "    baga_rc_release_str((const char *)v->data[i]); v->data[i] = (void *)s; }\n");
    }
    fprintf(out, "static void baga_vec_push_f64(baga_Vec *v, double x) { union { double d; void *p; } u; u.d = x; baga_vec_grow(v); v->data[v->len++] = u.p; }\n");
    fprintf(out, "static double baga_vec_get_f64(baga_Vec *v, int64_t i) {\n");
    fprintf(out, "    if (i < 0 || i >= v->len) baga_bounds_fail(\"vec_get\", i, v->len);\n");
    fprintf(out, "    union { double d; void *p; } u; u.p = v->data[i]; return u.d;\n");
    fprintf(out, "}\n");
    fprintf(out, "static void baga_vec_set_f64(baga_Vec *v, int64_t i, double x) {\n");
    fprintf(out, "    if (i < 0 || i >= v->len) baga_bounds_fail(\"vec_set\", i, v->len);\n");
    fprintf(out, "    union { double d; void *p; } u; u.d = x; v->data[i] = u.p;\n");
    fprintf(out, "}\n");
    /* bytes елементи: box-нат baga_bytes по указател (като baga_map_*_bytes) */
    fprintf(out, "static void baga_vec_push_bytes(baga_Vec *v, baga_bytes b) {\n");
    fprintf(out, "    baga_vec_grow(v);\n");
    if (cg->rc) fprintf(out, "    baga_rc_retain((void *)b.data);\n");
    fprintf(out, "    baga_bytes *p = baga_alloc(sizeof(baga_bytes)); *p = b;\n");
    fprintf(out, "    v->data[v->len++] = p;\n");
    fprintf(out, "}\n");
    fprintf(out, "static baga_bytes baga_vec_get_bytes(baga_Vec *v, int64_t i) {\n");
    fprintf(out, "    if (i < 0 || i >= v->len) baga_bounds_fail(\"vec_get\", i, v->len);\n");
    fprintf(out, "    return *(baga_bytes *)v->data[i];\n");
    fprintf(out, "}\n");
    fprintf(out, "static void baga_vec_set_bytes(baga_Vec *v, int64_t i, baga_bytes b) {\n");
    fprintf(out, "    if (i < 0 || i >= v->len) baga_bounds_fail(\"vec_set\", i, v->len);\n");
    if (cg->rc)
        fprintf(out, "    baga_rc_retain((void *)b.data); baga_rc_release_bytes(*(baga_bytes *)v->data[i]);\n");
    fprintf(out, "    *(baga_bytes *)v->data[i] = b;\n");
    fprintf(out, "}\n");
    /* RC3: move варианти — без retain на data (виж str по-горе) */
    if (cg->rc) {
        fprintf(out, "static void baga_vec_push_bytes_move(baga_Vec *v, baga_bytes b) {\n");
        fprintf(out, "    baga_vec_grow(v);\n");
        fprintf(out, "    baga_bytes *p = baga_alloc(sizeof(baga_bytes)); *p = b;\n");
        fprintf(out, "    v->data[v->len++] = p; }\n");
        fprintf(out, "static void baga_vec_set_bytes_move(baga_Vec *v, int64_t i, baga_bytes b) {\n");
        fprintf(out, "    if (i < 0 || i >= v->len) baga_bounds_fail(\"vec_set\", i, v->len);\n");
        fprintf(out, "    baga_rc_release_bytes(*(baga_bytes *)v->data[i]); *(baga_bytes *)v->data[i] = b; }\n");
    }
    /* struct елементи (L4): generic box helper-и; размерът идва от call site-а,
     * елементите са box-нати копия (memcpy при push/set/slice/concat) */
    fprintf(out, "static void baga_vec_push_box(baga_Vec *v, const void *src, int64_t size) {\n");
    fprintf(out, "    baga_vec_grow(v);\n");
    fprintf(out, "    void *p = baga_alloc((size_t)size); memcpy(p, src, (size_t)size);\n");
    fprintf(out, "    v->data[v->len++] = p;\n");
    fprintf(out, "}\n");
    fprintf(out, "static void *baga_vec_get_box(baga_Vec *v, int64_t i) {\n");
    fprintf(out, "    if (i < 0 || i >= v->len) baga_bounds_fail(\"vec_get\", i, v->len);\n");
    fprintf(out, "    return v->data[i];\n");
    fprintf(out, "}\n");
    fprintf(out, "static void baga_vec_set_box(baga_Vec *v, int64_t i, const void *src, int64_t size) {\n");
    fprintf(out, "    if (i < 0 || i >= v->len) baga_bounds_fail(\"vec_set\", i, v->len);\n");
    fprintf(out, "    memcpy(v->data[i], src, (size_t)size);\n");
    fprintf(out, "}\n");
    /* RC5 v0.3: overwrite release-ва полетата на стария box преди memcpy
     * (call site-ът вече е retain-нал новото — alias-safe ред). */
    if (cg->rc) {
        fprintf(out, "static void baga_vec_set_box_rc(baga_Vec *v, int64_t i, const void *src, int64_t size, void (*elem_rel)(void *)) {\n");
        fprintf(out, "    if (i < 0 || i >= v->len) baga_bounds_fail(\"vec_set\", i, v->len);\n");
        fprintf(out, "    if (elem_rel) elem_rel(v->data[i]);\n");
        fprintf(out, "    memcpy(v->data[i], src, (size_t)size);\n");
        fprintf(out, "}\n");
    }
    fprintf(out, "static int64_t baga_vec_len(baga_Vec *v) { return v ? v->len : 0; }\n");
    /* bridge: native bytes <-> Vec<i64> (crypto migration path) */
    fprintf(out, "static baga_bytes baga_bytes_from_vec(baga_Vec *v) {\n");
    fprintf(out, "    int64_t n = v ? v->len : 0;\n");
    fprintf(out, "    baga_bytes r; r.len = n; r.data = baga_alloc((size_t)(n ? n : 1));\n");
    fprintf(out, "    for (int64_t i = 0; i < n; i++) r.data[i] = (unsigned char)((int64_t)(intptr_t)v->data[i] & 255);\n");
    fprintf(out, "    return r; }\n");
    fprintf(out, "static baga_Vec *baga_vec_from_bytes(baga_bytes b) {\n");
    fprintf(out, "    baga_Vec *v = baga_vec_new();\n");
    fprintf(out, "    for (int64_t i = 0; i < b.len; i++) baga_vec_push_i64(v, (int64_t)b.data[i]);\n");
    fprintf(out, "    return v; }\n");
    fprintf(out, "static baga_Vec *baga_vec_slice_i64(baga_Vec *v, int64_t a, int64_t b) {\n");
    fprintf(out, "    if (a < 0) a = 0; if (b > v->len) b = v->len; if (b < a) b = a;\n");
    fprintf(out, "    baga_Vec *r = baga_vec_new();\n");
    fprintf(out, "    for (int64_t i = a; i < b; i++) baga_vec_push_i64(r, (int64_t)(intptr_t)v->data[i]);\n");
    fprintf(out, "    return r; }\n");
    fprintf(out, "static baga_Vec *baga_vec_slice_str(baga_Vec *v, int64_t a, int64_t b) {\n");
    fprintf(out, "    if (a < 0) a = 0; if (b > v->len) b = v->len; if (b < a) b = a;\n");
    fprintf(out, "    baga_Vec *r = baga_vec_new();\n");
    fprintf(out, "    for (int64_t i = a; i < b; i++) baga_vec_push_str(r, (const char *)v->data[i]);\n");
    fprintf(out, "    return r; }\n");
    fprintf(out, "static baga_Vec *baga_vec_slice_f64(baga_Vec *v, int64_t a, int64_t b) {\n");
    fprintf(out, "    if (a < 0) a = 0; if (b > v->len) b = v->len; if (b < a) b = a;\n");
    fprintf(out, "    baga_Vec *r = baga_vec_new();\n");
    fprintf(out, "    for (int64_t i = a; i < b; i++) { union { double d; void *p; } u; u.p = v->data[i]; baga_vec_push_f64(r, u.d); }\n");
    fprintf(out, "    return r; }\n");
    fprintf(out, "static baga_Vec *baga_vec_concat_i64(baga_Vec *v, baga_Vec *w) {\n");
    fprintf(out, "    baga_Vec *r = baga_vec_new();\n");
    fprintf(out, "    for (int64_t i = 0; i < v->len; i++) baga_vec_push_i64(r, (int64_t)(intptr_t)v->data[i]);\n");
    fprintf(out, "    for (int64_t i = 0; i < w->len; i++) baga_vec_push_i64(r, (int64_t)(intptr_t)w->data[i]);\n");
    fprintf(out, "    return r; }\n");
    fprintf(out, "static baga_Vec *baga_vec_concat_str(baga_Vec *v, baga_Vec *w) {\n");
    fprintf(out, "    baga_Vec *r = baga_vec_new();\n");
    fprintf(out, "    for (int64_t i = 0; i < v->len; i++) baga_vec_push_str(r, (const char *)v->data[i]);\n");
    fprintf(out, "    for (int64_t i = 0; i < w->len; i++) baga_vec_push_str(r, (const char *)w->data[i]);\n");
    fprintf(out, "    return r; }\n");
    fprintf(out, "static baga_Vec *baga_vec_concat_f64(baga_Vec *v, baga_Vec *w) {\n");
    fprintf(out, "    baga_Vec *r = baga_vec_new();\n");
    fprintf(out, "    for (int64_t i = 0; i < v->len; i++) { union { double d; void *p; } u; u.p = v->data[i]; baga_vec_push_f64(r, u.d); }\n");
    fprintf(out, "    for (int64_t i = 0; i < w->len; i++) { union { double d; void *p; } u; u.p = w->data[i]; baga_vec_push_f64(r, u.d); }\n");
    fprintf(out, "    return r; }\n");
    fprintf(out, "static baga_Vec *baga_vec_slice_bytes(baga_Vec *v, int64_t a, int64_t b) {\n");
    fprintf(out, "    if (a < 0) a = 0; if (b > v->len) b = v->len; if (b < a) b = a;\n");
    fprintf(out, "    baga_Vec *r = baga_vec_new();\n");
    fprintf(out, "    for (int64_t i = a; i < b; i++) baga_vec_push_bytes(r, *(baga_bytes *)v->data[i]);\n");
    fprintf(out, "    return r; }\n");
    fprintf(out, "static baga_Vec *baga_vec_concat_bytes(baga_Vec *v, baga_Vec *w) {\n");
    fprintf(out, "    baga_Vec *r = baga_vec_new();\n");
    fprintf(out, "    for (int64_t i = 0; i < v->len; i++) baga_vec_push_bytes(r, *(baga_bytes *)v->data[i]);\n");
    fprintf(out, "    for (int64_t i = 0; i < w->len; i++) baga_vec_push_bytes(r, *(baga_bytes *)w->data[i]);\n");
    fprintf(out, "    return r; }\n");
    fprintf(out, "static baga_Vec *baga_vec_slice_box(baga_Vec *v, int64_t a, int64_t b, int64_t size) {\n");
    fprintf(out, "    if (a < 0) a = 0; if (b > v->len) b = v->len; if (b < a) b = a;\n");
    fprintf(out, "    baga_Vec *r = baga_vec_new();\n");
    fprintf(out, "    for (int64_t i = a; i < b; i++) baga_vec_push_box(r, v->data[i], size);\n");
    fprintf(out, "    return r; }\n");
    fprintf(out, "static baga_Vec *baga_vec_concat_box(baga_Vec *v, baga_Vec *w, int64_t size) {\n");
    fprintf(out, "    baga_Vec *r = baga_vec_new();\n");
    fprintf(out, "    for (int64_t i = 0; i < v->len; i++) baga_vec_push_box(r, v->data[i], size);\n");
    fprintf(out, "    for (int64_t i = 0; i < w->len; i++) baga_vec_push_box(r, w->data[i], size);\n");
    fprintf(out, "    return r; }\n");
    /* RC5 v0.2: rc варианти — box копието споделя полетата с източника
     * (shallow), затова ги retain-ва; симетрично с release при drop на
     * контейнера. Пътеката без флаг остава бит-идентична. */
    if (cg->rc) {
        fprintf(out, "static baga_Vec *baga_vec_slice_box_rc(baga_Vec *v, int64_t a, int64_t b, int64_t size, void (*elem_retain)(void *)) {\n");
        fprintf(out, "    if (a < 0) a = 0; if (b > v->len) b = v->len; if (b < a) b = a;\n");
        fprintf(out, "    baga_Vec *r = baga_vec_new();\n");
        fprintf(out, "    for (int64_t i = a; i < b; i++) { baga_vec_push_box(r, v->data[i], size); if (elem_retain) elem_retain(v->data[i]); }\n");
        fprintf(out, "    return r; }\n");
        fprintf(out, "static baga_Vec *baga_vec_concat_box_rc(baga_Vec *v, baga_Vec *w, int64_t size, void (*elem_retain)(void *)) {\n");
        fprintf(out, "    baga_Vec *r = baga_vec_new();\n");
        fprintf(out, "    for (int64_t i = 0; i < v->len; i++) { baga_vec_push_box(r, v->data[i], size); if (elem_retain) elem_retain(v->data[i]); }\n");
        fprintf(out, "    for (int64_t i = 0; i < w->len; i++) { baga_vec_push_box(r, w->data[i], size); if (elem_retain) elem_retain(w->data[i]); }\n");
        fprintf(out, "    return r; }\n");
    }
    /* вложени вектори (Vec<Vec<T>>): елементът е baga_Vec* — съхранява се
     * като указател; drop(outer) освобождава рекурсивно вътрешните (mode 3) */
    if (cg->rc)
        fprintf(out, "static void baga_vec_push_vec(baga_Vec *v, baga_Vec *x) { baga_rc_retain((void *)x); baga_vec_grow(v); v->data[v->len++] = (void *)x; }\n");
    else
        fprintf(out, "static void baga_vec_push_vec(baga_Vec *v, baga_Vec *x) { baga_vec_grow(v); v->data[v->len++] = (void *)x; }\n");
    fprintf(out, "static baga_Vec *baga_vec_get_vec(baga_Vec *v, int64_t i) {\n");
    fprintf(out, "    if (i < 0 || i >= v->len) baga_bounds_fail(\"vec_get\", i, v->len);\n");
    fprintf(out, "    return (baga_Vec *)v->data[i];\n");
    fprintf(out, "}\n");
    fprintf(out, "static void baga_vec_set_vec(baga_Vec *v, int64_t i, baga_Vec *x) {\n");
    fprintf(out, "    if (i < 0 || i >= v->len) baga_bounds_fail(\"vec_set\", i, v->len);\n");
    if (cg->rc)
        fprintf(out, "    baga_rc_retain((void *)x); baga_rc_release_vec((baga_Vec *)v->data[i], 0, 0, NULL);\n");
    fprintf(out, "    v->data[i] = (void *)x;\n");
    fprintf(out, "}\n");
    /* RC3: move варианти — без retain (виж str по-горе) */
    if (cg->rc) {
        fprintf(out, "static void baga_vec_push_vec_move(baga_Vec *v, baga_Vec *x) { baga_vec_grow(v); v->data[v->len++] = (void *)x; }\n");
        fprintf(out, "static void baga_vec_set_vec_move(baga_Vec *v, int64_t i, baga_Vec *x) {\n");
        fprintf(out, "    if (i < 0 || i >= v->len) baga_bounds_fail(\"vec_set\", i, v->len);\n");
        fprintf(out, "    baga_rc_release_vec((baga_Vec *)v->data[i], 0, 0, NULL); v->data[i] = (void *)x; }\n");
    }
    /* RC5 v0.9: overwrite на Vec<Vec<S>> — release на стария вътрешен vec с
     * destructor за S полетата (elem_rel = baga_rc_relv_<S>). Retain на новия
     * преди release на стария (alias-safe ред, както set_vec по-горе). */
    if (cg->rc) {
        fprintf(out, "static void baga_vec_set_vec_rc(baga_Vec *v, int64_t i, baga_Vec *x, void (*elem_rel)(void *)) {\n");
        fprintf(out, "    if (i < 0 || i >= v->len) baga_bounds_fail(\"vec_set\", i, v->len);\n");
        fprintf(out, "    baga_rc_retain((void *)x); elem_rel(v->data[i]); v->data[i] = (void *)x; }\n");
        fprintf(out, "static void baga_vec_set_vec_move_rc(baga_Vec *v, int64_t i, baga_Vec *x, void (*elem_rel)(void *)) {\n");
        fprintf(out, "    if (i < 0 || i >= v->len) baga_bounds_fail(\"vec_set\", i, v->len);\n");
        fprintf(out, "    elem_rel(v->data[i]); v->data[i] = (void *)x; }\n");
    }
    fprintf(out, "static baga_Vec *baga_vec_slice_vec(baga_Vec *v, int64_t a, int64_t b) {\n");
    fprintf(out, "    if (a < 0) a = 0; if (b > v->len) b = v->len; if (b < a) b = a;\n");
    fprintf(out, "    baga_Vec *r = baga_vec_new();\n");
    fprintf(out, "    for (int64_t i = a; i < b; i++) baga_vec_push_vec(r, (baga_Vec *)v->data[i]);\n");
    fprintf(out, "    return r; }\n");
    fprintf(out, "static baga_Vec *baga_vec_concat_vec(baga_Vec *v, baga_Vec *w) {\n");
    fprintf(out, "    baga_Vec *r = baga_vec_new();\n");
    fprintf(out, "    for (int64_t i = 0; i < v->len; i++) baga_vec_push_vec(r, (baga_Vec *)v->data[i]);\n");
    fprintf(out, "    for (int64_t i = 0; i < w->len; i++) baga_vec_push_vec(r, (baga_Vec *)w->data[i]);\n");
    fprintf(out, "    return r; }\n");
    fprintf(out, "\n/* hash map: chaining, key i64/str/bytes, value i64/str/f64/bytes (leak-tolerant like baga_Vec) */\n");
    fprintf(out, "typedef struct baga_MapEntry {\n");
    fprintf(out, "    int64_t ik; const char *sk; baga_bytes bk; int64_t ktag;\n");
    fprintf(out, "    int64_t iv; double fv; const char *sv; baga_bytes bv; void *pv;\n");
    fprintf(out, "    struct baga_MapEntry *next;\n");
    fprintf(out, "} baga_MapEntry;\n");
    fprintf(out, "typedef struct { baga_MapEntry **b; int64_t nb; int64_t len; } baga_Map;\n");
    fprintf(out, "static uint64_t baga_map_hash_str(const char *s) {\n");
    fprintf(out, "    uint64_t h = 1469598103934665603ULL;\n");
    fprintf(out, "    while (*s) { h ^= (unsigned char)*s++; h *= 1099511628211ULL; }\n");
    fprintf(out, "    return h; }\n");
    /* R67: bytes ключове — FNV-1a върху data+len (NUL-safe) */
    fprintf(out, "static uint64_t baga_map_hash_bytes(baga_bytes k) {\n");
    fprintf(out, "    uint64_t h = 1469598103934665603ULL;\n");
    fprintf(out, "    for (int64_t i = 0; i < k.len; i++) { h ^= k.data[i]; h *= 1099511628211ULL; }\n");
    fprintf(out, "    return h; }\n");
    fprintf(out, "static uint64_t baga_map_hash_i64(int64_t k) {\n");
    fprintf(out, "    uint64_t x = (uint64_t)k;\n");
    fprintf(out, "    x ^= x >> 33; x *= 0xff51afd7ed558ccdULL; x ^= x >> 33;\n");
    fprintf(out, "    x *= 0xc4ceb9fe1a85ec53ULL; x ^= x >> 33; return x; }\n");
    fprintf(out, "static baga_Map *baga_map_new(void) {\n");
    fprintf(out, "    baga_Map *m = baga_alloc(sizeof(baga_Map));\n");
    fprintf(out, "    m->nb = 16; m->len = 0;\n");
    fprintf(out, "    m->b = baga_alloc(sizeof(baga_MapEntry *) * (size_t)m->nb);\n");
    fprintf(out, "    for (int64_t i = 0; i < m->nb; i++) m->b[i] = NULL;\n");
    fprintf(out, "    return m; }\n");
    /* slot lookup: returns the entry pointer or the NULL link (insert point) */
    fprintf(out, "static baga_MapEntry **baga_map_slot(baga_Map *m, int64_t ik, const char *sk, uint64_t h) {\n");
    fprintf(out, "    baga_MapEntry **e = &m->b[h %% (uint64_t)m->nb];\n");
    fprintf(out, "    while (*e) {\n");
    fprintf(out, "        if (sk ? ((*e)->sk && strcmp((*e)->sk, sk) == 0)\n");
    fprintf(out, "               : (!(*e)->sk && (*e)->ik == ik)) return e;\n");
    fprintf(out, "        e = &(*e)->next; }\n");
    fprintf(out, "    return e; }\n");
    fprintf(out, "static void baga_map_rehash(baga_Map *m) {\n");
    fprintf(out, "    int64_t onb = m->nb; baga_MapEntry **ob = m->b;\n");
    fprintf(out, "    m->nb *= 2;\n");
    fprintf(out, "    m->b = baga_alloc(sizeof(baga_MapEntry *) * (size_t)m->nb);\n");
    fprintf(out, "    for (int64_t i = 0; i < m->nb; i++) m->b[i] = NULL;\n");
    fprintf(out, "    for (int64_t i = 0; i < onb; i++) {\n");
    fprintf(out, "        baga_MapEntry *e = ob[i];\n");
    fprintf(out, "        while (e) {\n");
    fprintf(out, "            baga_MapEntry *nx = e->next;\n");
    fprintf(out, "            uint64_t h = e->ktag == 2 ? baga_map_hash_bytes(e->bk)\n");
    fprintf(out, "                       : e->sk ? baga_map_hash_str(e->sk) : baga_map_hash_i64(e->ik);\n");
    fprintf(out, "            e->next = m->b[h %% (uint64_t)m->nb];\n");
    fprintf(out, "            m->b[h %% (uint64_t)m->nb] = e;\n");
    fprintf(out, "            e = nx; } }\n");
    /* MEM-4д: старият bucket масив се free-ва след rehash — иначе в persist
     * региона всяко удвояване оставя стария масив завинаги (вж. vec_grow). */
    fprintf(out, "    baga_free(ob, onb * (int64_t)sizeof(baga_MapEntry *)); }\n");
    fprintf(out, "static baga_MapEntry *baga_map_put(baga_Map *m, int64_t ik, const char *sk, uint64_t h) {\n");
    /* RC1: ключът се retain-ва при претенция за съхранение; ако entry-то
     * вече съществува, новият ключ не се пъха → retain-ът се пуска */
    if (cg->rc) {
        fprintf(out, "    if (sk) baga_rc_retain((void *)sk);\n");
        fprintf(out, "    baga_MapEntry **slot = baga_map_slot(m, ik, sk, h);\n");
        fprintf(out, "    if (*slot) { if (sk) baga_rc_release_str(sk); return *slot; }\n");
    } else {
        fprintf(out, "    baga_MapEntry **slot = baga_map_slot(m, ik, sk, h);\n");
        fprintf(out, "    if (*slot) return *slot;\n");
    }
    fprintf(out, "    baga_MapEntry *e = baga_alloc(sizeof(baga_MapEntry));\n");
    fprintf(out, "    e->ik = ik; e->sk = sk; e->iv = 0; e->fv = 0; e->sv = NULL;\n");
    fprintf(out, "    e->bv.data = NULL; e->bv.len = 0; e->pv = NULL; e->next = NULL;\n");
    fprintf(out, "    e->ktag = sk ? 1 : 0; e->bk.data = NULL; e->bk.len = 0;\n");
    fprintf(out, "    *slot = e; m->len++;\n");
    fprintf(out, "    if (m->len * 4 > m->nb * 3) baga_map_rehash(m);\n");
    fprintf(out, "    return e; }\n");
    fprintf(out, "static int64_t baga_map_len(baga_Map *m) { return m ? m->len : 0; }\n");
    /* typed variants: baga_map_{set,get}_{str,i64}_{i64,str,f64},
     * has/del/keys по ключ — имената съвпадат с lowering-а по-горе */
    for (int ki = 0; ki < 2; ki++) {
        const char *kn   = ki == 0 ? "str" : "i64";
        const char *karg = ki == 0 ? "const char *k" : "int64_t k";
        const char *hk   = ki == 0 ? "baga_map_hash_str(k)" : "baga_map_hash_i64(k)";
        const char *ikv  = ki == 0 ? "0" : "k";
        const char *skv  = ki == 0 ? "k" : "NULL";
        for (int vi = 0; vi < 3; vi++) {
            const char *vn   = vi == 0 ? "i64" : (vi == 1 ? "str" : "f64");
            const char *varg = vi == 0 ? "int64_t v" : (vi == 1 ? "const char *v" : "double v");
            const char *fld  = vi == 0 ? "iv" : (vi == 1 ? "sv" : "fv");
            fprintf(out, "static void baga_map_set_%s_%s(baga_Map *m, %s, %s) {\n", kn, vn, karg, varg);
            fprintf(out, "    baga_MapEntry *e = baga_map_put(m, %s, %s, %s);\n", ikv, skv, hk);
            /* RC1: стойността се retain-ва; при overwrite — release на старата */
            if (cg->rc && vi == 1)
                fprintf(out, "    baga_rc_retain((void *)v); baga_rc_release_str(e->sv);\n");
            fprintf(out, "    e->%s = v; }\n", fld);
            /* RC3: move вариант — без retain на стойността (last-use локал);
             * release на старата при overwrite както обикновено */
            if (cg->rc && vi == 1) {
                fprintf(out, "static void baga_map_set_%s_str_move(baga_Map *m, %s, const char *v) {\n", kn, karg);
                fprintf(out, "    baga_MapEntry *e = baga_map_put(m, %s, %s, %s);\n", ikv, skv, hk);
                fprintf(out, "    baga_rc_release_str(e->sv); e->sv = v; }\n");
            }
            const char *ret = vi == 0 ? "int64_t" : (vi == 1 ? "const char *" : "double");
            fprintf(out, "static %s baga_map_get_%s_%s(baga_Map *m, %s) {\n", ret, kn, vn, karg);
            fprintf(out, "    baga_MapEntry *e = *baga_map_slot(m, %s, %s, %s);\n", ikv, skv, hk);
            if (vi == 1) fprintf(out, "    return (e && e->sv) ? e->sv : \"\"; }\n");
            else if (vi == 0) fprintf(out, "    return e ? e->iv : 0; }\n");
            else fprintf(out, "    return e ? e->fv : 0; }\n");
        }
        /* bytes values (binary-safe; kvbaga K2 / chat buffers) */
        fprintf(out, "static void baga_map_set_%s_bytes(baga_Map *m, %s, baga_bytes v) {\n", kn, karg);
        fprintf(out, "    baga_MapEntry *e = baga_map_put(m, %s, %s, %s);\n", ikv, skv, hk);
        /* RC1: retain на data; при overwrite — release на старите */
        if (cg->rc)
            fprintf(out, "    baga_rc_retain((void *)v.data); baga_rc_release_bytes(e->bv);\n");
        fprintf(out, "    e->bv = v; }\n");
        /* RC3: move вариант — без retain на data (виж str по-горе) */
        if (cg->rc) {
            fprintf(out, "static void baga_map_set_%s_bytes_move(baga_Map *m, %s, baga_bytes v) {\n", kn, karg);
            fprintf(out, "    baga_MapEntry *e = baga_map_put(m, %s, %s, %s);\n", ikv, skv, hk);
            fprintf(out, "    baga_rc_release_bytes(e->bv); e->bv = v; }\n");
        }
        fprintf(out, "static baga_bytes baga_map_get_%s_bytes(baga_Map *m, %s) {\n", kn, karg);
        fprintf(out, "    baga_MapEntry *e = *baga_map_slot(m, %s, %s, %s);\n", ikv, skv, hk);
        fprintf(out, "    if (!e) { baga_bytes z; z.data = NULL; z.len = 0; return z; }\n");
        fprintf(out, "    return e->bv; }\n");
        /* struct values (L4): box per entry; set copies in, get returns the
         * box (NULL when missing — call site substitutes a zero struct) */
        fprintf(out, "static void baga_map_set_%s_box(baga_Map *m, %s, const void *src, int64_t size) {\n", kn, karg);
        fprintf(out, "    baga_MapEntry *e = baga_map_put(m, %s, %s, %s);\n", ikv, skv, hk);
        fprintf(out, "    if (!e->pv) e->pv = baga_alloc((size_t)size);\n");
        fprintf(out, "    memcpy(e->pv, src, (size_t)size); }\n");
        /* RC5 v0.3: overwrite release-ва полетата на стария box преди memcpy */
        if (cg->rc) {
            fprintf(out, "static void baga_map_set_%s_box_rc(baga_Map *m, %s, const void *src, int64_t size, void (*val_rel)(void *)) {\n", kn, karg);
            fprintf(out, "    baga_MapEntry *e = baga_map_put(m, %s, %s, %s);\n", ikv, skv, hk);
            fprintf(out, "    if (e->pv && val_rel) val_rel(e->pv);\n");
            fprintf(out, "    if (!e->pv) e->pv = baga_alloc((size_t)size);\n");
            fprintf(out, "    memcpy(e->pv, src, (size_t)size); }\n");
        }
        fprintf(out, "static void *baga_map_get_%s_box(baga_Map *m, %s) {\n", kn, karg);
        fprintf(out, "    baga_MapEntry *e = *baga_map_slot(m, %s, %s, %s);\n", ikv, skv, hk);
        fprintf(out, "    return e ? e->pv : NULL; }\n");
        fprintf(out, "static int64_t baga_map_has_%s(baga_Map *m, %s) {\n", kn, karg);
        fprintf(out, "    return *baga_map_slot(m, %s, %s, %s) ? 1 : 0; }\n", ikv, skv, hk);
        fprintf(out, "static void baga_map_del_%s(baga_Map *m, %s) {\n", kn, karg);
        fprintf(out, "    baga_MapEntry **slot = baga_map_slot(m, %s, %s, %s);\n", ikv, skv, hk);
        fprintf(out, "    if (!*slot) return;\n");
        /* MEM-4д: del без free беше вечен leak в persist региона (pin/unpin
         * churn = 112 B на цикъл — 2.4 KB/ред на boilaDB insert bench).
         * Shell-ът се пуска тук. struct/enum box (pv) се пуска от
         * baga_map_del_*_box (MEM-8), не от този вариант. */
        fprintf(out, "    baga_MapEntry *e = *slot; *slot = e->next; m->len--;\n");
        /* RC1: del release-ва ключ + str/bytes стойност (entry shell-ът се
         * free-ва от MEM-4д кодa по-долу, както досега).
         * MEM-8: struct/enum минава през baga_map_del_*_box (пусна pv).
         * Тук pv няма — скалар, str или bytes. */
        if (cg->rc) {
            fprintf(out, "    if (e->ktag == 2) baga_rc_release_bytes(e->bk); else baga_rc_release_str(e->sk);\n");
            fprintf(out, "    baga_rc_release_str(e->sv); baga_rc_release_bytes(e->bv);\n");
        }
        fprintf(out, "    baga_free(e, (int64_t)sizeof(baga_MapEntry)); }\n");
        /* MEM-8: non-rc del на struct/enum. map_get копира стойността,
         * после del трябва да върне box-а. Преди това pv оставаше
         * (del не знаеше val_size) — PG mux take+park течеше по един
         * BoilaPgLive на заявка. */
        fprintf(out, "static void baga_map_del_%s_box(baga_Map *m, %s, int64_t val_size) {\n", kn, karg);
        fprintf(out, "    baga_MapEntry **slot = baga_map_slot(m, %s, %s, %s);\n", ikv, skv, hk);
        fprintf(out, "    if (!*slot) return;\n");
        fprintf(out, "    baga_MapEntry *e = *slot; *slot = e->next; m->len--;\n");
        fprintf(out, "    if (e->pv && val_size > 0) baga_free(e->pv, val_size);\n");
        fprintf(out, "    baga_free(e, (int64_t)sizeof(baga_MapEntry)); }\n");
        /* RC5 v0.3: del с box стойност — release на полетата + free на pv
         * (откаченото entry не се вижда от release_map при drop). */
        if (cg->rc) {
            fprintf(out, "static void baga_map_del_%s_rc(baga_Map *m, %s, int64_t val_size, void (*val_rel)(void *)) {\n", kn, karg);
            fprintf(out, "    baga_MapEntry **slot = baga_map_slot(m, %s, %s, %s);\n", ikv, skv, hk);
            fprintf(out, "    if (!*slot) return;\n");
            fprintf(out, "    baga_MapEntry *e = *slot; *slot = e->next; m->len--;\n");
            fprintf(out, "    if (e->ktag == 2) baga_rc_release_bytes(e->bk); else baga_rc_release_str(e->sk);\n");
            fprintf(out, "    if (e->pv) { if (val_rel) val_rel(e->pv); baga_free(e->pv, val_size); }\n");
            fprintf(out, "    baga_free(e, (int64_t)sizeof(baga_MapEntry)); }\n");
        }
        fprintf(out, "static baga_Vec *baga_map_keys_%s(baga_Map *m) {\n", kn);
        fprintf(out, "    baga_Vec *v = baga_vec_new();\n");
        fprintf(out, "    for (int64_t i = 0; i < m->nb; i++)\n");
        fprintf(out, "        for (baga_MapEntry *e = m->b[i]; e; e = e->next)\n");
        if (ki == 0) fprintf(out, "            baga_vec_push_str(v, e->sk ? e->sk : \"\");\n");
        else         fprintf(out, "            baga_vec_push_i64(v, e->ik);\n");
        fprintf(out, "    return v; }\n");
    }
    /* R67: bytes ключове — отделни slot/put с memcmp (ktag=2 в entry-то);
     * същият набор typed варианти като str/i64 по-горе */
    fprintf(out, "static baga_MapEntry **baga_map_slot_b(baga_Map *m, baga_bytes k, uint64_t h) {\n");
    fprintf(out, "    baga_MapEntry **e = &m->b[h %% (uint64_t)m->nb];\n");
    fprintf(out, "    while (*e) {\n");
    fprintf(out, "        if ((*e)->ktag == 2 && (*e)->bk.len == k.len &&\n");
    fprintf(out, "            (k.len == 0 || memcmp((*e)->bk.data, k.data, (size_t)k.len) == 0)) return e;\n");
    fprintf(out, "        e = &(*e)->next; }\n");
    fprintf(out, "    return e; }\n");
    fprintf(out, "static baga_MapEntry *baga_map_put_b(baga_Map *m, baga_bytes k, uint64_t h) {\n");
    /* RC1: същото като str put — retain на ключа, пускане ако не се пъха */
    if (cg->rc) {
        fprintf(out, "    baga_rc_retain((void *)k.data);\n");
        fprintf(out, "    baga_MapEntry **slot = baga_map_slot_b(m, k, h);\n");
        fprintf(out, "    if (*slot) { baga_rc_release_bytes(k); return *slot; }\n");
    } else {
        fprintf(out, "    baga_MapEntry **slot = baga_map_slot_b(m, k, h);\n");
        fprintf(out, "    if (*slot) return *slot;\n");
    }
    fprintf(out, "    baga_MapEntry *e = baga_alloc(sizeof(baga_MapEntry));\n");
    fprintf(out, "    e->ik = 0; e->sk = NULL; e->bk = k; e->ktag = 2;\n");
    fprintf(out, "    e->iv = 0; e->fv = 0; e->sv = NULL;\n");
    fprintf(out, "    e->bv.data = NULL; e->bv.len = 0; e->pv = NULL; e->next = NULL;\n");
    fprintf(out, "    *slot = e; m->len++;\n");
    fprintf(out, "    if (m->len * 4 > m->nb * 3) baga_map_rehash(m);\n");
    fprintf(out, "    return e; }\n");
    for (int vi = 0; vi < 3; vi++) {
        const char *vn   = vi == 0 ? "i64" : (vi == 1 ? "str" : "f64");
        const char *varg = vi == 0 ? "int64_t v" : (vi == 1 ? "const char *v" : "double v");
        const char *fld  = vi == 0 ? "iv" : (vi == 1 ? "sv" : "fv");
        fprintf(out, "static void baga_map_set_bytes_%s(baga_Map *m, baga_bytes k, %s) {\n", vn, varg);
        fprintf(out, "    baga_MapEntry *e = baga_map_put_b(m, k, baga_map_hash_bytes(k));\n");
        if (cg->rc && vi == 1)
            fprintf(out, "    baga_rc_retain((void *)v); baga_rc_release_str(e->sv);\n");
        fprintf(out, "    e->%s = v; }\n", fld);
        /* RC3: move вариант — без retain на стойността (виж str ключове) */
        if (cg->rc && vi == 1) {
            fprintf(out, "static void baga_map_set_bytes_str_move(baga_Map *m, baga_bytes k, const char *v) {\n");
            fprintf(out, "    baga_MapEntry *e = baga_map_put_b(m, k, baga_map_hash_bytes(k));\n");
            fprintf(out, "    baga_rc_release_str(e->sv); e->sv = v; }\n");
        }
        const char *ret = vi == 0 ? "int64_t" : (vi == 1 ? "const char *" : "double");
        fprintf(out, "static %s baga_map_get_bytes_%s(baga_Map *m, baga_bytes k) {\n", ret, vn);
        fprintf(out, "    baga_MapEntry *e = *baga_map_slot_b(m, k, baga_map_hash_bytes(k));\n");
        if (vi == 1) fprintf(out, "    return (e && e->sv) ? e->sv : \"\"; }\n");
        else if (vi == 0) fprintf(out, "    return e ? e->iv : 0; }\n");
        else fprintf(out, "    return e ? e->fv : 0; }\n");
    }
    fprintf(out, "static void baga_map_set_bytes_bytes(baga_Map *m, baga_bytes k, baga_bytes v) {\n");
    fprintf(out, "    baga_MapEntry *e = baga_map_put_b(m, k, baga_map_hash_bytes(k));\n");
    if (cg->rc)
        fprintf(out, "    baga_rc_retain((void *)v.data); baga_rc_release_bytes(e->bv);\n");
    fprintf(out, "    e->bv = v; }\n");
    /* RC3: move вариант — без retain на data */
    if (cg->rc) {
        fprintf(out, "static void baga_map_set_bytes_bytes_move(baga_Map *m, baga_bytes k, baga_bytes v) {\n");
        fprintf(out, "    baga_MapEntry *e = baga_map_put_b(m, k, baga_map_hash_bytes(k));\n");
        fprintf(out, "    baga_rc_release_bytes(e->bv); e->bv = v; }\n");
    }
    fprintf(out, "static baga_bytes baga_map_get_bytes_bytes(baga_Map *m, baga_bytes k) {\n");
    fprintf(out, "    baga_MapEntry *e = *baga_map_slot_b(m, k, baga_map_hash_bytes(k));\n");
    fprintf(out, "    if (!e) { baga_bytes z; z.data = NULL; z.len = 0; return z; }\n");
    fprintf(out, "    return e->bv; }\n");
    fprintf(out, "static void baga_map_set_bytes_box(baga_Map *m, baga_bytes k, const void *src, int64_t size) {\n");
    fprintf(out, "    baga_MapEntry *e = baga_map_put_b(m, k, baga_map_hash_bytes(k));\n");
    fprintf(out, "    if (!e->pv) e->pv = baga_alloc((size_t)size);\n");
    fprintf(out, "    memcpy(e->pv, src, (size_t)size); }\n");
    /* RC5 v0.3: overwrite release-ва полетата на стария box преди memcpy */
    if (cg->rc) {
        fprintf(out, "static void baga_map_set_bytes_box_rc(baga_Map *m, baga_bytes k, const void *src, int64_t size, void (*val_rel)(void *)) {\n");
        fprintf(out, "    baga_MapEntry *e = baga_map_put_b(m, k, baga_map_hash_bytes(k));\n");
        fprintf(out, "    if (e->pv && val_rel) val_rel(e->pv);\n");
        fprintf(out, "    if (!e->pv) e->pv = baga_alloc((size_t)size);\n");
        fprintf(out, "    memcpy(e->pv, src, (size_t)size); }\n");
    }
    fprintf(out, "static void *baga_map_get_bytes_box(baga_Map *m, baga_bytes k) {\n");
    fprintf(out, "    baga_MapEntry *e = *baga_map_slot_b(m, k, baga_map_hash_bytes(k));\n");
    fprintf(out, "    return e ? e->pv : NULL; }\n");
    fprintf(out, "static int64_t baga_map_has_bytes(baga_Map *m, baga_bytes k) {\n");
    fprintf(out, "    return *baga_map_slot_b(m, k, baga_map_hash_bytes(k)) ? 1 : 0; }\n");
    fprintf(out, "static void baga_map_del_bytes(baga_Map *m, baga_bytes k) {\n");
    fprintf(out, "    baga_MapEntry **slot = baga_map_slot_b(m, k, baga_map_hash_bytes(k));\n");
    fprintf(out, "    if (!*slot) return;\n");
    /* MEM-4д: същото като str/i64 del — entry-то се free-ва (вж. горе). */
    fprintf(out, "    baga_MapEntry *e = *slot; *slot = e->next; m->len--;\n");
    /* RC1: release на bytes ключ + str/bytes стойност преди free на shell-а.
     * MEM-8: struct/enum е baga_map_del_bytes_box (вж. str/i64 варианта). */
    if (cg->rc) {
        fprintf(out, "    baga_rc_release_bytes(e->bk);\n");
        fprintf(out, "    baga_rc_release_str(e->sv); baga_rc_release_bytes(e->bv);\n");
    }
    fprintf(out, "    baga_free(e, (int64_t)sizeof(baga_MapEntry)); }\n");
    fprintf(out, "static void baga_map_del_bytes_box(baga_Map *m, baga_bytes k, int64_t val_size) {\n");
    fprintf(out, "    baga_MapEntry **slot = baga_map_slot_b(m, k, baga_map_hash_bytes(k));\n");
    fprintf(out, "    if (!*slot) return;\n");
    fprintf(out, "    baga_MapEntry *e = *slot; *slot = e->next; m->len--;\n");
    fprintf(out, "    if (e->pv && val_size > 0) baga_free(e->pv, val_size);\n");
    fprintf(out, "    baga_free(e, (int64_t)sizeof(baga_MapEntry)); }\n");
    /* RC5 v0.3: del с box стойност — release на полетата + free на pv */
    if (cg->rc) {
        fprintf(out, "static void baga_map_del_bytes_rc(baga_Map *m, baga_bytes k, int64_t val_size, void (*val_rel)(void *)) {\n");
        fprintf(out, "    baga_MapEntry **slot = baga_map_slot_b(m, k, baga_map_hash_bytes(k));\n");
        fprintf(out, "    if (!*slot) return;\n");
        fprintf(out, "    baga_MapEntry *e = *slot; *slot = e->next; m->len--;\n");
        fprintf(out, "    baga_rc_release_bytes(e->bk);\n");
        fprintf(out, "    if (e->pv) { if (val_rel) val_rel(e->pv); baga_free(e->pv, val_size); }\n");
        fprintf(out, "    baga_free(e, (int64_t)sizeof(baga_MapEntry)); }\n");
    }
    fprintf(out, "static baga_Vec *baga_map_keys_bytes(baga_Map *m) {\n");
    fprintf(out, "    baga_Vec *v = baga_vec_new();\n");
    fprintf(out, "    for (int64_t i = 0; i < m->nb; i++)\n");
    fprintf(out, "        for (baga_MapEntry *e = m->b[i]; e; e = e->next)\n");
    fprintf(out, "            baga_vec_push_bytes(v, e->bk);\n");
    fprintf(out, "    return v; }\n");
    /* RC1: release на Map — rc върху STRUCT алокацията. При rc→0 release-ва
     * ключовете (по ktag/sk на entry-то) и стойностите според val_tag
     * (0 inline, 1 str, 2 bytes, 3 struct box), после free на entries,
     * buckets и struct. RC5 v0.2: val_rel е destructor на box полетата
     * (NULL → само free). */
    if (cg->rc) {
        fprintf(out, "static void baga_rc_release_map(baga_Map *m, int val_tag, int64_t val_size, void (*val_rel)(void *)) {\n");
        fprintf(out, "    if (!m) return;\n");
        fprintf(out, "    baga_Hdr *h = baga_rc_hdr((void *)m);\n");
        fprintf(out, "    if (!h) return;\n");
        fprintf(out, "    if (h->rc == 0) { fprintf(stderr, \"baga: rc underflow (map — двоен release)\\n\"); exit(1); }\n");
        fprintf(out, "    if (--h->rc > 0) return;\n");
        fprintf(out, "    for (int64_t i = 0; i < m->nb; i++) {\n");
        fprintf(out, "        baga_MapEntry *e = m->b[i];\n");
        fprintf(out, "        while (e) {\n");
        fprintf(out, "            baga_MapEntry *nx = e->next;\n");
        fprintf(out, "            if (e->ktag == 2) baga_rc_release_bytes(e->bk);\n");
        fprintf(out, "            else if (e->sk) baga_rc_release_str(e->sk);\n");
        fprintf(out, "            if (val_tag == 1) baga_rc_release_str(e->sv);\n");
        fprintf(out, "            else if (val_tag == 2) baga_rc_release_bytes(e->bv);\n");
        fprintf(out, "            else if (val_tag == 3 && e->pv) { if (val_rel) val_rel(e->pv); baga_free(e->pv, val_size); }\n");
        fprintf(out, "            baga_free(e, (int64_t)sizeof(baga_MapEntry));\n");
        fprintf(out, "            e = nx;\n");
        fprintf(out, "        }\n");
        fprintf(out, "    }\n");
        fprintf(out, "    baga_free(m->b, m->nb * (int64_t)sizeof(baga_MapEntry *));\n");
        fprintf(out, "    baga_free(m, (int64_t)sizeof(baga_Map));\n");
        fprintf(out, "}\n");
    }
    /* MEM-1: drop walkers — рециклират собствените алокации през baga_free.
     * Вътрешните буфери (bytes.data, str полета, env box-ове на closures)
     * остават в arena-та — споделена собственост, документирано. */
    fprintf(out, "\nstatic void baga_cell2_free(int64_t h);\n");
    fprintf(out, "static void baga_drop_bytes(baga_bytes b) { baga_free(b.data, b.len); }\n");
    fprintf(out, "/* elem_kind: 0 = inline (i64/f64), 1 = str (споделени — не се пипат), 2 = box (bytes/struct) */\n");
    /* RC1: под --rc drop ≡ release — елементите се release-ват, не се
     * free-ват директно (може да са retain-нати от друг собственик) */
    if (cg->rc) {
        fprintf(out, "static void baga_drop_vec(baga_Vec *v, int elem_kind, int64_t elem_size) {\n");
        fprintf(out, "    baga_rc_release_vec(v, elem_kind, elem_size, NULL);\n");
        fprintf(out, "}\n");
    } else {
        fprintf(out, "static void baga_drop_vec(baga_Vec *v, int elem_kind, int64_t elem_size) {\n");
    fprintf(out, "    if (!v) return;\n");
    fprintf(out, "    if (elem_kind == 2)\n");
    fprintf(out, "        for (int64_t i = 0; i < v->len; i++) baga_free(v->data[i], elem_size);\n");
    fprintf(out, "    if (elem_kind == 3)\n");
    fprintf(out, "        for (int64_t i = 0; i < v->len; i++) baga_drop_vec((baga_Vec *)v->data[i], 0, 0);\n");
    fprintf(out, "    baga_free(v->data, v->cap * 8);\n");
    fprintf(out, "    baga_free(v, (int64_t)sizeof(baga_Vec));\n");
    fprintf(out, "}\n");
    }
    /* RC1: drop_map ≡ release (val_is_box → val_tag 3); ключовете се
     * release-ват по entry съдържание, затова key tag не е нужен */
    if (cg->rc) {
        fprintf(out, "static void baga_drop_map(baga_Map *m, int val_is_box, int64_t val_size) {\n");
        fprintf(out, "    baga_rc_release_map(m, val_is_box ? 3 : 0, val_size, NULL);\n");
        fprintf(out, "}\n");
    } else {
    fprintf(out, "static void baga_drop_map(baga_Map *m, int val_is_box, int64_t val_size) {\n");
    fprintf(out, "    if (!m) return;\n");
    /* next се пази ПРЕДИ free — baga_free презаписва първата дума на блока,
     * не разчитаме на offset-а на 'next' в layout-а */
    fprintf(out, "    for (int64_t i = 0; i < m->nb; i++) {\n");
    fprintf(out, "        baga_MapEntry *e = m->b[i];\n");
    fprintf(out, "        while (e) {\n");
    fprintf(out, "            baga_MapEntry *nx = e->next;\n");
    fprintf(out, "            if (val_is_box && e->pv) baga_free(e->pv, val_size);\n");
    fprintf(out, "            baga_free(e, (int64_t)sizeof(baga_MapEntry));\n");
    fprintf(out, "            e = nx;\n");
    fprintf(out, "        }\n");
    fprintf(out, "    }\n");
    fprintf(out, "    baga_free(m->b, m->nb * (int64_t)sizeof(baga_MapEntry *));\n");
    fprintf(out, "    baga_free(m, (int64_t)sizeof(baga_Map));\n");
    fprintf(out, "}\n");
    }
    fprintf(out, "static void baga_drop_fn(int64_t h) { baga_cell2_free(h); }\n");
    /* W2.3: вътрешна променяемост на замразен свят — lazy кеш/индекс, който
     * ЖИВЕЕ ИЗВЪН dump-натите байтове (прикачен е към handle-а, не към
     * региона). Замразен свят остава непроменлив ПО СТОЙНОСТ (dump-ът и
     * четенията са идентични преди и след кеш-попълване), а кешът е заключен с
     * mutex, защото замразените светове се споделят между нишки за четене —
     * lazy попълването от два читателя не е race. Договорът е идиомът на
     * Rust OnceCell/lazy_static: стойността трябва да е ФУНКЦИЯ на
     * замразените данни (идемпотентна), иначе два писателя дават
     * недетерминиран резултат (документирано в worlds-bg.md). */
    /* W2.13: kd/p/plen носят стойност-низове и bytes: kd=1 i64 (p=NULL),
     * kd=2 str (p е NUL-терминирано копие, собственост на слота), kd=3 bytes
     * (p е сурово копие с дължина plen). Payload-ът се ОСВОБОДАВА при
     * презапис, del и free — затова четенето връща КОПИЕ в bump арената. */
    fprintf(out, "typedef struct { int64_t *k; int64_t *v; unsigned char *st; unsigned char *kd; char **p; int64_t *plen; int64_t n; int64_t cap; } baga_WCache;\n");
    fprintf(out, "static uint64_t baga_wc_h(int64_t k) { uint64_t x = (uint64_t)k; x ^= x >> 33; x *= 0xff51afd7ed558ccdULL; x ^= x >> 33; return x; }\n");
    fprintf(out, "static void baga_wc_oom(void) { fprintf(stderr, \"baga: wcache: oom\\n\"); exit(1); }\n");
    fprintf(out, "static baga_WCache *baga_wc_new(void) {\n");
    fprintf(out, "    baga_WCache *c = malloc(sizeof(baga_WCache));\n");
    fprintf(out, "    if (!c) baga_wc_oom();\n");
    fprintf(out, "    c->n = 0; c->cap = 16;\n");
    fprintf(out, "    c->k = malloc(16 * sizeof(int64_t)); c->v = malloc(16 * sizeof(int64_t)); c->st = calloc(16, 1);\n");
    fprintf(out, "    c->kd = calloc(16, 1); c->p = calloc(16, sizeof(char *)); c->plen = calloc(16, sizeof(int64_t));\n");
    fprintf(out, "    if (!c->k || !c->v || !c->st || !c->kd || !c->p || !c->plen) baga_wc_oom();\n");
    fprintf(out, "    return c;\n");
    fprintf(out, "}\n");
    /* open addressing, load factor <= 0.75, tombstone при del; растежът
     * рекурсивно препътва (след удвояване факторът е <= 0.375, значи няма
     * втори растеж в същата верига).
     * Терминацията на сондирането не е очевидна и е load-bearing: put ПРЕДИЗЛАГА
     * дупка (tombstone) пред празен слот, а растежът държи живите <= 0.75*cap,
     * затова заетите слотове (живи + tombstone) никога не стигат cap — винаги
     * остава празен слот, който спира while-а. Без това del-овете биха
     * запълнили таблицата с tombstone-и и find би въртял безкрайно. */
    fprintf(out, "static void baga_wc_put(baga_WCache *c, int64_t k, int64_t kind, int64_t v, char *p, int64_t plen) {\n");
    fprintf(out, "    if ((c->n + 1) * 4 > c->cap * 3) {\n");
    fprintf(out, "        int64_t oc = c->cap;\n");
    fprintf(out, "        int64_t *ok = c->k, *ov = c->v, *opn = c->plen;\n");
    fprintf(out, "        unsigned char *os = c->st, *od = c->kd; char **op = c->p;\n");
    fprintf(out, "        c->cap = oc * 2;\n");
    fprintf(out, "        c->k = malloc(c->cap * sizeof(int64_t)); c->v = malloc(c->cap * sizeof(int64_t)); c->st = calloc(c->cap, 1);\n");
    fprintf(out, "        c->kd = calloc(c->cap, 1); c->p = calloc(c->cap, sizeof(char *)); c->plen = calloc(c->cap, sizeof(int64_t));\n");
    fprintf(out, "        if (!c->k || !c->v || !c->st || !c->kd || !c->p || !c->plen) baga_wc_oom();\n");
    fprintf(out, "        c->n = 0;\n");
    fprintf(out, "        for (int64_t i = 0; i < oc; i++) if (os[i] == 1) baga_wc_put(c, ok[i], od[i], ov[i], op[i], opn[i]);\n");
    fprintf(out, "        free(ok); free(ov); free(os); free(od); free(op); free(opn);\n");
    fprintf(out, "    }\n");
    fprintf(out, "    int64_t mask = c->cap - 1;\n");
    fprintf(out, "    int64_t i = (int64_t)(baga_wc_h(k) & (uint64_t)mask), hole = -1;\n");
    fprintf(out, "    while (c->st[i]) {\n");
    fprintf(out, "        if (c->st[i] == 1) { if (c->k[i] == k) { free(c->p[i]); c->v[i] = v; c->kd[i] = (unsigned char)kind; c->p[i] = p; c->plen[i] = plen; return; } }\n");
    fprintf(out, "        else if (hole < 0) hole = i;\n");
    fprintf(out, "        i = (i + 1) & mask;\n");
    fprintf(out, "    }\n");
    fprintf(out, "    if (hole >= 0) i = hole;\n");
    fprintf(out, "    c->k[i] = k; c->v[i] = v; c->kd[i] = (unsigned char)kind; c->p[i] = p; c->plen[i] = plen; c->st[i] = 1; c->n++;\n");
    fprintf(out, "}\n");
    fprintf(out, "static int64_t baga_wc_slot(baga_WCache *c, int64_t k) {\n");
    fprintf(out, "    if (!c || c->n == 0) return -1;\n");
    fprintf(out, "    int64_t mask = c->cap - 1;\n");
    fprintf(out, "    int64_t i = (int64_t)(baga_wc_h(k) & (uint64_t)mask);\n");
    fprintf(out, "    while (c->st[i]) {\n");
    fprintf(out, "        if (c->st[i] == 1 && c->k[i] == k) return i;\n");
    fprintf(out, "        i = (i + 1) & mask;\n");
    fprintf(out, "    }\n");
    fprintf(out, "    return -1;\n");
    fprintf(out, "}\n");
    fprintf(out, "static int baga_wc_find(baga_WCache *c, int64_t k, int64_t *out) {\n");
    fprintf(out, "    int64_t s = baga_wc_slot(c, k);\n");
    fprintf(out, "    if (s < 0) return 0;\n");
    fprintf(out, "    if (out) *out = c->v[s];\n");
    fprintf(out, "    return 1;\n");
    fprintf(out, "}\n");
    fprintf(out, "static void baga_wc_del(baga_WCache *c, int64_t k) {\n");
    fprintf(out, "    int64_t s = baga_wc_slot(c, k);\n");
    fprintf(out, "    if (s < 0) return;\n");
    fprintf(out, "    free(c->p[s]); c->p[s] = NULL; c->st[s] = 2; c->n--;\n");
    fprintf(out, "}\n");
    fprintf(out, "static void baga_wc_free(baga_WCache *c) {\n");
    fprintf(out, "    if (!c) return;\n");
    fprintf(out, "    for (int64_t i = 0; i < c->cap; i++) if (c->st[i] == 1) free(c->p[i]);\n");
    fprintf(out, "    free(c->k); free(c->v); free(c->st); free(c->kd); free(c->p); free(c->plen); free(c);\n");
    fprintf(out, "}\n");
    fprintf(out, "\n/* arena allocator: bump allocation, free-all-at-once */\n");
    /* W2.5: връзки между светове — насочена capability (owner → target).
     * Таблицата е ИЗВЪН региона, защото handle-ите са указатели, не офсети:
     * dump/load не я носи (зареден свят няма връзки — графът се построява
     * наново). Правилото е „целта надживява връзката": free на свързан свят е
     * чиста грешка (linked_by брояч), free на owner освобождава връзките му.
     * Цикли се проверяват при link (bounded DFS) — иначе графът става
     * неосвободим. Връзките се строят преди freeze: замразен свят не пипа
     * таблицата, затова споделянето между нишки не нуждае от заключване тук. */
    fprintf(out, "typedef struct baga_Arena { char *base; int64_t used; int64_t cap; int64_t frozen; pthread_mutex_t wlock; baga_WCache *wcache; struct baga_Arena **links; int64_t n_links; int64_t cap_links; int64_t linked_by; } baga_Arena;\n");
    fprintf(out, "static int baga_world_reaches(baga_Arena *a, baga_Arena *t, int depth, int *steps) {\n");
    fprintf(out, "    if (depth > 64 || --(*steps) < 0) return 1;   /* изчерпан бюджет/дълбочина — консервативно „цикъл\" */\n");
    fprintf(out, "    for (int64_t i = 0; i < a->n_links; i++) {\n");
    fprintf(out, "        baga_Arena *c = a->links[i];\n");
    fprintf(out, "        if (c == t || baga_world_reaches(c, t, depth + 1, steps)) return 1;\n");
    fprintf(out, "    }\n");
    fprintf(out, "    return 0;\n");
    fprintf(out, "}\n");
    fprintf(out, "static int64_t baga_arena_new(void) {\n");
    fprintf(out, "    baga_Arena *a = malloc(sizeof(baga_Arena));\n");
    fprintf(out, "    if (!a) { fprintf(stderr, \"baga: arena_new: oom\\n\"); exit(1); }\n");
    fprintf(out, "    a->cap = 65536; a->used = 0; a->frozen = 0; a->base = malloc((size_t)a->cap);\n");
    fprintf(out, "    if (!a->base) { fprintf(stderr, \"baga: arena_new: oom\\n\"); exit(1); }\n");
    fprintf(out, "    pthread_mutex_init(&a->wlock, NULL); a->wcache = NULL;\n");
    fprintf(out, "    a->links = NULL; a->n_links = 0; a->cap_links = 0; a->linked_by = 0;\n");
    fprintf(out, "    return (int64_t)(intptr_t)a;\n");
    fprintf(out, "}\n");
    fprintf(out, "static int64_t baga_arena_alloc(int64_t h, int64_t size) {\n");
    fprintf(out, "    baga_Arena *a = (baga_Arena *)(intptr_t)h;\n");
    fprintf(out, "    if (!a) { fprintf(stderr, \"baga: arena_alloc: null handle\\n\"); exit(1); }\n");
    fprintf(out, "    if (size < 0) size = 0;\n");
    /* W2.2: overflow guard — иначе used+size или (used+size)*2 прелива и
     * bump-ът пише извън блока при гигантска заявка. */
    fprintf(out, "    if (size > 0 && a->used > INT64_MAX - size) { fprintf(stderr, \"baga: arena_alloc: блокът е прекалено голям\\n\"); exit(1); }\n");
    fprintf(out, "    if (a->used + size > a->cap) {\n");
    fprintf(out, "        int64_t need = a->used + size;\n");
    fprintf(out, "        int64_t nc = need > a->cap * 2 ? need : a->cap * 2;\n");
    fprintf(out, "        a->base = realloc(a->base, (size_t)nc); a->cap = nc;\n");
    fprintf(out, "        if (!a->base) { fprintf(stderr, \"baga: arena_alloc: oom\\n\"); exit(1); }\n");
    fprintf(out, "    }\n");
    fprintf(out, "    char *p = a->base + a->used; a->used += size;\n");
    fprintf(out, "    return (int64_t)(intptr_t)p;\n");
    fprintf(out, "}\n");
    fprintf(out, "static void baga_arena_reset(int64_t h) {\n");
    fprintf(out, "    baga_Arena *a = (baga_Arena *)(intptr_t)h;\n");
    fprintf(out, "    if (!a) { fprintf(stderr, \"baga: arena_reset: null handle\\n\"); exit(1); }\n");
    fprintf(out, "    a->used = 0;\n");
    fprintf(out, "}\n");
    fprintf(out, "static void baga_arena_free(int64_t h) {\n");
    fprintf(out, "    baga_Arena *a = (baga_Arena *)(intptr_t)h;\n");
    fprintf(out, "    if (!a) return;\n");
    /* W2.5: целта на връзката трябва да я надживее — иначе owner-ът остава с
     * висящ handle (use-after-free при world_linked/wvec_*). */
    fprintf(out, "    if (a->linked_by > 0) { fprintf(stderr, \"baga: world_free на свят, свързан от %%lld друг(и) — unlink преди free\\n\", (long long)a->linked_by); exit(1); }\n");
    fprintf(out, "    for (int64_t i = 0; i < a->n_links; i++) a->links[i]->linked_by--;\n");
    fprintf(out, "    free(a->links);\n");
    fprintf(out, "    baga_wc_free(a->wcache); a->wcache = NULL;   /* W2.3: кешът е извън региона */\n");
    fprintf(out, "    pthread_mutex_destroy(&a->wlock);\n");
    fprintf(out, "    free(a->base); free(a);\n");
    fprintf(out, "}\n");
    /* W0: светове — регион с ownership/freeze дисциплина. Runtime-ът е
     * арената + frozen бит (seatbelt: чиста грешка при alloc в замразен
     * свят); протоколът use-after-free/use-after-move/mutate-after-freeze
     * се ДОКАЗВА/ОБОРВА от верификатора (HK_WORLD ghost, като HK_DROP). */
    /* W2.10: светът започва с baga_WHdr (magic + таблица на контейнерите) —
     * инициализацията е в блока на контейнерите по-долу (forward decl). */
    fprintf(out, "static int64_t baga_world_init_hdr(int64_t h);\n");
    fprintf(out, "static int64_t baga_world_new(void) { return baga_world_init_hdr(baga_arena_new()); }\n");
    fprintf(out, "static int64_t baga_world_alloc(int64_t h, int64_t size) {\n");
    fprintf(out, "    baga_Arena *a = (baga_Arena *)(intptr_t)h;\n");
    fprintf(out, "    if (!a) { fprintf(stderr, \"baga: world_alloc: null handle\\n\"); exit(1); }\n");
    fprintf(out, "    if (a->frozen) { fprintf(stderr, \"baga: world_alloc върху замразен свят — mutate-after-freeze\\n\"); exit(1); }\n");
    fprintf(out, "    return baga_arena_alloc(h, size);\n");
    fprintf(out, "}\n");
    fprintf(out, "static int64_t baga_world_freeze(int64_t h) {\n");
    fprintf(out, "    baga_Arena *a = (baga_Arena *)(intptr_t)h;\n");
    fprintf(out, "    if (!a) { fprintf(stderr, \"baga: world_freeze: null handle\\n\"); exit(1); }\n");
    /* W2.2: повторният freeze е нарушение на протокола (verifier-ът го оборва,
     * W1.6 — на компилация). Runtime-ът вече не го приема мълчаливо: чиста
     * грешка, същата дисциплина като mutate-after-freeze. */
    fprintf(out, "    if (a->frozen) { fprintf(stderr, \"baga: повторен world_freeze — святът вече е замразен\\n\"); exit(1); }\n");
    fprintf(out, "    a->frozen = 1;\n");
    fprintf(out, "    return 1;\n");
    fprintf(out, "}\n");
    fprintf(out, "static int64_t baga_world_state(int64_t h) {\n");
    fprintf(out, "    baga_Arena *a = (baga_Arena *)(intptr_t)h;\n");
    fprintf(out, "    if (!a) return 0 - 1;\n");
    fprintf(out, "    return a->frozen;\n");
    fprintf(out, "}\n");
    fprintf(out, "static void baga_world_free(int64_t h) { baga_arena_free(h); }\n");
    fprintf(out, "\n");
    fprintf(out, "/* W1/W2.9/W2.10: контейнери ВЪТРЕ в свят — wvec (последователност) и wmap\n");
    fprintf(out, " * (ключ/стойност). Адресацията е през ИДЕНТИФИКАТОР, не през суров офсет:\n");
    fprintf(out, " * регионът започва с baga_WHdr (magic + таблица на контейнерите), таблицата е\n");
    fprintf(out, " * в самия регион, затова\n");
    fprintf(out, " *   • dump/load носи и magic, и таблицата ⇒ id-тата важат и след зареждане;\n");
    fprintf(out, " *   • runtime-ът ЗНАЕ корените (таблицата) и схемата (ekind/vkind), значи\n");
    fprintf(out, " *     може да компактира сам (world_compact / world_dump_compact) — W1.5\n");
    fprintf(out, " *     ръчният rebuild идеом вече не е единственият път;\n");
    fprintf(out, " *   • произволен i64 не може да се представи за контейнер (id се проверява\n");
    fprintf(out, " *     срещу таблицата, kind — срещу очаквания).\n");
    fprintf(out, " * Вътрешността е offset-адресирана: абсолютни указатели не живеят между две\n");
    fprintf(out, " * извиквания, защото realloc при растеж мести base (офсет-първа дисциплина). */\n");
    fprintf(out, "#define BAGA_WORLD_MAGIC 0x42475700000A01LL\n");
    fprintf(out, "#define BAGA_WORLD_VERSION 1\n");
    fprintf(out, "typedef struct { int64_t magic; int64_t version; int64_t tbl_off; int64_t tbl_len; int64_t tbl_cap; } baga_WHdr;\n");
    fprintf(out, "typedef struct { int64_t off; int64_t kind; } baga_WEnt;   /* kind: 1=wvec, 2=wmap, 3=wrec */\n");
    fprintf(out, "typedef struct { int64_t len; int64_t cap; int64_t data_off; int64_t ekind; } baga_WVec;\n");
    fprintf(out, "typedef struct { int64_t k; int64_t v; int64_t st; } baga_WSlot;\n");
    fprintf(out, "typedef struct { int64_t len; int64_t cap; int64_t slots_off; int64_t vkind; } baga_WMap;\n");
    fprintf(out, "/* Запис в региона: фиксиран брой полета + пер-поле схема (0 = не е зададено,\n");
    fprintf(out, " * 1 = i64, 2 = str, 3 = bytes). За разлика от wvec/wmap записът е ХЕТЕРОГЕНЕН\n");
    fprintf(out, " * по дизайн — схемата не е една за всички елементи, а масив до стойностите, и\n");
    fprintf(out, " * компакцията чете него. Поле-контейнер е просто i64 id (id-тата са\n");
    fprintf(out, " * позиционно независими), значи вложени записи работят без нов вид. */\n");
    fprintf(out, "typedef struct { int64_t n; int64_t data_off; int64_t kinds_off; } baga_WRec;\n");
    fprintf(out, "\n");
    fprintf(out, "static void baga_world_bad(const char *op, const char *what, long long a, long long b) {\n");
    fprintf(out, "    fprintf(stderr, \"baga: %%s: %%s (%%lld, used/len=%%lld)\\n\", op, what, a, b);\n");
    fprintf(out, "    exit(1);\n");
    fprintf(out, "}\n");
    /* W2.14: достъпът до контейнер поевтиня с 30–51%, без да е махната нито
     * една проверка. Промяната е в начина на КОМПИЛИРАНЕ, не в логиката:
     *
     *  1. `baga_world_a` и резолвът на id са always_inline. Като out-of-line
     *     функции не се инлайнваха (error-пътищата с fprintf ги правеха твърде
     *     големи), значи всяка операция плащаше пролог/епилог и губеше CSE със
     *     съседните проверки. Мерено: wvec_get 6.10 → 3.05 ns само от това.
     *  2. Мутаторите не резолвват id-то два пъти: `wvec_push` → `push_raw` и
     *     `wmap_set` → `set_raw` повтаряха world_a + mut_check + резолв, защото
     *     помощниците взимаха (wh, id). Сега взимат (wh, w, off) — офсетът
     *     остава валиден и след alloc, защото регионът е offset-адресиран, а
     *     указателите се презареждат след растеж. wvec_push 13.35 → 11.90 ns.
     *
     * Две хипотези бяха реализирани, мерени и ОТХВЪРЛЕНИ (шест билда,
     * docs/worlds-bg.md §4):
     *  • кеш на id-резолва с „поколение" на региона — НЕУТРАЛЕН (2.96 срещу
     *    2.97 ns при четене), затова не е в кода: носи инвариант за остаряване
     *    на три места срещу нулева измерима полза. Ако някога се върне, записът
     *    трябва да е степен на две (8×24 B прави индекса умножение: +0.79 ns);
     *  • флаг `wvalid` вместо magic/версия — РЕГРЕСИЯ (4.34 срещу 2.96 ns със
     *    същия кеш). Хипотеза за причината (не е от profiler): проверките
     *    зареждат `base`/`used`, които резолвът веднага преизползва.
     *  Извод: „по-малко проверки" не значи „по-бързо", а два билда, различаващи
     *  се по две неща, не доказват нищо за нито едно от тях. */
    fprintf(out, "static inline __attribute__((always_inline)) baga_Arena *baga_world_a(int64_t h, const char *op) {\n");
    fprintf(out, "    baga_Arena *a = (baga_Arena *)(intptr_t)h;\n");
    fprintf(out, "    if (!a) { fprintf(stderr, \"baga: %%s: null world handle\\n\", op); exit(1); }\n");
    fprintf(out, "    if (a->used < (int64_t)sizeof(baga_WHdr))\n");
    fprintf(out, "        { fprintf(stderr, \"baga: %%s: handle-ът не е свят (регионът е твърде малък)\\n\", op); exit(1); }\n");
    fprintf(out, "    baga_WHdr *hd = (baga_WHdr *)a->base;\n");
    fprintf(out, "    if (hd->magic != BAGA_WORLD_MAGIC)\n");
    fprintf(out, "        { fprintf(stderr, \"baga: %%s: handle-ът не е свят (няма magic) — светове са world_new/world_load\\n\", op); exit(1); }\n");
    fprintf(out, "    if (hd->version != BAGA_WORLD_VERSION)\n");
    fprintf(out, "        { fprintf(stderr, \"baga: %%s: неподдържана версия на света %%lld\\n\", op, (long long)hd->version); exit(1); }\n");
    fprintf(out, "    return a;\n");
    fprintf(out, "}\n");
    fprintf(out, "static void baga_world_mut_check(baga_Arena *a, const char *op) {\n");
    fprintf(out, "    if (a->frozen) { fprintf(stderr, \"baga: %%s върху замразен свят — mutate-after-freeze\\n\", op); exit(1); }\n");
    fprintf(out, "}\n");
    fprintf(out, "static int64_t baga_woff(baga_Arena *w, int64_t p) {\n");
    fprintf(out, "    return (int64_t)((char *)(intptr_t)p - w->base);\n");
    fprintf(out, "}\n");
    fprintf(out, "static baga_WHdr *baga_whdr(baga_Arena *w) { return (baga_WHdr *)w->base; }\n");
    fprintf(out, "static int64_t baga_world_init_hdr(int64_t h) {\n");
    fprintf(out, "    baga_Arena *a = (baga_Arena *)(intptr_t)h;\n");
    fprintf(out, "    baga_WHdr *hd = (baga_WHdr *)a->base;\n");
    fprintf(out, "    hd->magic = BAGA_WORLD_MAGIC; hd->version = BAGA_WORLD_VERSION;\n");
    fprintf(out, "    hd->tbl_off = 0; hd->tbl_len = 0; hd->tbl_cap = 0;\n");
    fprintf(out, "    a->used = (int64_t)sizeof(baga_WHdr);\n");
    fprintf(out, "    return h;\n");
    fprintf(out, "}\n");
    fprintf(out, "\n");
    fprintf(out, "/* таблица на контейнерите: id е 1-базиран индекс (0 = невалиден) */\n");
    fprintf(out, "static const char *baga_wkind_name(int64_t k) {\n");
    fprintf(out, "    return k == 1 ? \"wvec\" : k == 2 ? \"wmap\" : k == 3 ? \"wrec\" : \"контейнер\";\n");
    fprintf(out, "}\n");
    /* W2.14: always_inline (виж бележката над baga_world_a). Студеният път за
     * kind-грешката е изнесен: snprintf-ът на 24 места уголемява кода, а не се
     * изпълнява никога. Проверките са ВСИЧКИТЕ — нито една гаранция от W2.2
     * (офсет в региона, kind на контейнера, повредена таблица) не е отслабена. */
    fprintf(out, "static __attribute__((noinline)) void baga_wres_kind_err(int64_t id, int64_t kind, int64_t have, const char *op) {\n");
    fprintf(out, "    char kmsg[48];\n");
    fprintf(out, "    snprintf(kmsg, sizeof kmsg, \"id не е %%s\", baga_wkind_name(kind));\n");
    fprintf(out, "    baga_world_bad(op, kmsg, (long long)id, (long long)have);\n");
    fprintf(out, "}\n");
    fprintf(out, "static inline __attribute__((always_inline)) int64_t baga_wres(baga_Arena *w, int64_t id, int64_t kind, const char *op) {\n");
    fprintf(out, "    baga_WHdr *hd = baga_whdr(w);\n");
    fprintf(out, "    if (id <= 0 || id > hd->tbl_len)\n");
    fprintf(out, "        baga_world_bad(op, \"невалиден контейнер id\", (long long)id, (long long)hd->tbl_len);\n");
    fprintf(out, "    if (hd->tbl_off < 0 || hd->tbl_off > w->used || hd->tbl_cap < 0 ||\n");
    fprintf(out, "        hd->tbl_cap * (int64_t)sizeof(baga_WEnt) > w->used - hd->tbl_off)\n");
    fprintf(out, "        baga_world_bad(op, \"повредена таблица на контейнерите\", (long long)hd->tbl_off, (long long)w->used);\n");
    fprintf(out, "    baga_WEnt *e = (baga_WEnt *)(w->base + hd->tbl_off) + (id - 1);\n");
    fprintf(out, "    if (e->kind != kind) baga_wres_kind_err(id, kind, e->kind, op);\n");
    fprintf(out, "    if (e->off < 0 || e->off > w->used)\n");
    fprintf(out, "        baga_world_bad(op, \"контейнерът сочи извън региона\", (long long)e->off, (long long)w->used);\n");
    fprintf(out, "    return e->off;\n");
    fprintf(out, "}\n");
    fprintf(out, "static int64_t baga_wtbl_add(int64_t wh, int64_t off, int64_t kind) {\n");
    fprintf(out, "    baga_Arena *w = (baga_Arena *)(intptr_t)wh;\n");
    fprintf(out, "    baga_WHdr *hd = baga_whdr(w);\n");
    fprintf(out, "    if (hd->tbl_len == hd->tbl_cap) {\n");
    fprintf(out, "        int64_t ncap = hd->tbl_cap ? hd->tbl_cap * 2 : 16;\n");
    fprintf(out, "        int64_t np = baga_arena_alloc(wh, ncap * (int64_t)sizeof(baga_WEnt));\n");
    fprintf(out, "        int64_t noff = baga_woff(w, np);\n");
    fprintf(out, "        if (hd->tbl_cap > 0)\n");
    fprintf(out, "            memcpy(w->base + noff, w->base + hd->tbl_off, (size_t)(hd->tbl_cap * sizeof(baga_WEnt)));\n");
    fprintf(out, "        hd = baga_whdr(w);            /* base може да е преместен */\n");
    fprintf(out, "        hd->tbl_off = noff; hd->tbl_cap = ncap;\n");
    fprintf(out, "    }\n");
    fprintf(out, "    baga_WEnt *e = (baga_WEnt *)(w->base + hd->tbl_off) + hd->tbl_len;\n");
    fprintf(out, "    e->off = off; e->kind = kind;\n");
    fprintf(out, "    hd->tbl_len++;\n");
    fprintf(out, "    return hd->tbl_len;               /* id */\n");
    fprintf(out, "}\n");
    fprintf(out, "\n");
    fprintf(out, "/* заглавия и елементи — всеки адрес се доказва вътре в региона (W2.2) */\n");
    fprintf(out, "static baga_WVec *baga_wvec_at(baga_Arena *w, int64_t voff, const char *op) {\n");
    fprintf(out, "    if (voff < 0 || voff > w->used - (int64_t)sizeof(baga_WVec))\n");
    fprintf(out, "        baga_world_bad(op, \"невалиден wvec офсет\", (long long)voff, (long long)w->used);\n");
    fprintf(out, "    baga_WVec *v = (baga_WVec *)(w->base + voff);\n");
    fprintf(out, "    if (v->len < 0 || v->cap < v->len || v->data_off < 0 || v->data_off > w->used ||\n");
    fprintf(out, "        v->cap > w->used || v->cap * 8 > w->used - v->data_off)\n");
    fprintf(out, "        baga_world_bad(op, \"повредено wvec заглавие\", (long long)voff, (long long)w->used);\n");
    fprintf(out, "    return v;\n");
    fprintf(out, "}\n");
    fprintf(out, "static int64_t *baga_wvec_data(baga_Arena *w, baga_WVec *v, int64_t i, const char *op) {\n");
    fprintf(out, "    uint64_t off = (uint64_t)v->data_off + (uint64_t)i * 8u;\n");
    fprintf(out, "    if (off > (uint64_t)w->used || (uint64_t)w->used - off < 8u)\n");
    fprintf(out, "        baga_world_bad(op, \"достъп извън региона\", (unsigned long long)off, (long long)w->used);\n");
    fprintf(out, "    return (int64_t *)(w->base + off);\n");
    fprintf(out, "}\n");
    fprintf(out, "static baga_WMap *baga_wmap_at(baga_Arena *w, int64_t moff, const char *op) {\n");
    fprintf(out, "    if (moff < 0 || moff > w->used - (int64_t)sizeof(baga_WMap))\n");
    fprintf(out, "        baga_world_bad(op, \"невалиден wmap офсет\", (long long)moff, (long long)w->used);\n");
    fprintf(out, "    baga_WMap *m = (baga_WMap *)(w->base + moff);\n");
    fprintf(out, "    if (m->len < 0 || m->cap < m->len || m->cap <= 0 || (m->cap & (m->cap - 1)) != 0 ||\n");
    fprintf(out, "        m->slots_off < 0 || m->slots_off > w->used || m->cap > w->used ||\n");
    fprintf(out, "        m->cap * (int64_t)sizeof(baga_WSlot) > w->used - m->slots_off)\n");
    fprintf(out, "        baga_world_bad(op, \"повредено wmap заглавие\", (long long)moff, (long long)w->used);\n");
    fprintf(out, "    return m;\n");
    fprintf(out, "}\n");
    fprintf(out, "static baga_WSlot *baga_wslot(baga_Arena *w, baga_WMap *m, int64_t i, const char *op) {\n");
    fprintf(out, "    int64_t off = m->slots_off + i * (int64_t)sizeof(baga_WSlot);\n");
    fprintf(out, "    if (off < 0 || off > w->used - (int64_t)sizeof(baga_WSlot))\n");
    fprintf(out, "        baga_world_bad(op, \"wmap слот извън региона\", (long long)off, (long long)w->used);\n");
    fprintf(out, "    return (baga_WSlot *)(w->base + off);\n");
    fprintf(out, "}\n");
    fprintf(out, "/* ekind/vkind: 0 = още няма елементи, 1 = i64, 2 = str, 3 = bytes. Контейнерът\n");
    fprintf(out, " * е ХОМОГЕНЕН — смесването е чиста грешка, защото компакцията трябва да знае\n");
    fprintf(out, " * кои елементи са офсети към payload. */\n");
    fprintf(out, "static void baga_kind_set(int64_t *slot, int64_t want, const char *op) {\n");
    fprintf(out, "    if (*slot == 0) { *slot = want; return; }\n");
    fprintf(out, "    if (*slot != want)\n");
    fprintf(out, "        baga_world_bad(op, \"смесени типове елементи в контейнера\", (long long)*slot, (long long)want);\n");
    fprintf(out, "}\n");
    /* Читателската страна на схемата: i64 достъп до контейнер от низове връщаше
     * офсета като число, а str достъп до i64 елемент — боклук, стига стойността
     * случайно да е валиден офсет. Вид 0 значи „още няма елементи" — там няма
     * какво да се прочете накриво, затова не е грешка. */
    fprintf(out, "static void baga_kind_chk(int64_t have, int64_t want, const char *op, const char *what) {\n");
    fprintf(out, "    if (have != 0 && have != want) baga_world_bad(op, what, (long long)have, (long long)want);\n");
    fprintf(out, "}\n");
    fprintf(out, "static int64_t baga_wreg_str_off(baga_Arena *w, int64_t soff, const char *op) {\n");
    fprintf(out, "    if (soff < 0 || soff >= w->used) baga_world_bad(op, \"низът не е офсет в региона\", (long long)soff, (long long)w->used);\n");
    fprintf(out, "    if (!memchr(w->base + soff, 0, (size_t)(w->used - soff)))\n");
    fprintf(out, "        baga_world_bad(op, \"низът няма NUL в региона\", (long long)soff, (long long)w->used);\n");
    fprintf(out, "    return soff;\n");
    fprintf(out, "}\n");
    fprintf(out, "static int64_t baga_wreg_bytes_off(baga_Arena *w, int64_t soff, const char *op) {\n");
    fprintf(out, "    if (soff < 0 || soff > w->used - 8) baga_world_bad(op, \"bytes елементът не е офсет в региона\", (long long)soff, (long long)w->used);\n");
    fprintf(out, "    int64_t n; memcpy(&n, w->base + soff, 8);\n");
    fprintf(out, "    if (n < 0 || n > w->used - (soff + 8))\n");
    fprintf(out, "        baga_world_bad(op, \"дължината на bytes е извън региона\", (long long)n, (long long)w->used);\n");
    fprintf(out, "    return n;\n");
    fprintf(out, "}\n");
    fprintf(out, "/* Payload-ите се КОПИРАТ в региона (dump/load ги носи). Източник от самия\n");
    fprintf(out, " * регион се бележи като офсет ПРЕДИ arena_alloc — растежът мести base и\n");
    fprintf(out, " * указателят би увиснал (W2.2). */\n");
    fprintf(out, "static int64_t baga_wreg_put_str(int64_t wh, baga_Arena *w, const char *s) {\n");
    fprintf(out, "    int64_t n = baga_str_len(s);\n");
    fprintf(out, "    int in_region = (uintptr_t)s >= (uintptr_t)w->base && (uintptr_t)s < (uintptr_t)(w->base + w->used);\n");
    fprintf(out, "    int64_t s_off = in_region ? (int64_t)(s - w->base) : 0;\n");
    fprintf(out, "    int64_t sp = baga_arena_alloc(wh, n + 1);\n");
    fprintf(out, "    if (in_region) s = w->base + s_off;\n");
    fprintf(out, "    int64_t soff = baga_woff(w, sp);\n");
    fprintf(out, "    memcpy(w->base + soff, s, (size_t)n + 1);\n");
    fprintf(out, "    return soff;\n");
    fprintf(out, "}\n");
    fprintf(out, "static int64_t baga_wreg_put_bytes(int64_t wh, baga_Arena *w, baga_bytes b, const char *op) {\n");
    fprintf(out, "    if (b.len < 0 || (b.len > 0 && !b.data)) baga_world_bad(op, \"невалиден bytes аргумент\", (long long)b.len, 0);\n");
    fprintf(out, "    if (b.len > INT64_MAX - 8) { fprintf(stderr, \"baga: %%s: блокът е прекалено голям\\n\", op); exit(1); }\n");
    fprintf(out, "    int in_region = b.len > 0 && (uintptr_t)b.data >= (uintptr_t)w->base &&\n");
    fprintf(out, "                    (uintptr_t)b.data < (uintptr_t)(w->base + w->used);\n");
    fprintf(out, "    int64_t b_off = in_region ? (int64_t)((char *)b.data - w->base) : 0;\n");
    fprintf(out, "    int64_t p = baga_arena_alloc(wh, 8 + b.len);\n");
    fprintf(out, "    if (in_region) b.data = (unsigned char *)(w->base + b_off);\n");
    fprintf(out, "    int64_t off = baga_woff(w, p);\n");
    fprintf(out, "    memcpy(w->base + off, &b.len, 8);\n");
    fprintf(out, "    if (b.len > 0) memcpy(w->base + off + 8, b.data, (size_t)b.len);\n");
    fprintf(out, "    return off;\n");
    fprintf(out, "}\n");
    fprintf(out, "/* wrec: заглавието и двата масива (стойности, схема) се доказват в региона, а\n");
    fprintf(out, " * индексът е спрямо n — броя полета, не спрямо used. */\n");
    fprintf(out, "static baga_WRec *baga_wrec_at(baga_Arena *w, int64_t roff, const char *op) {\n");
    fprintf(out, "    if (roff < 0 || roff > w->used - (int64_t)sizeof(baga_WRec))\n");
    fprintf(out, "        baga_world_bad(op, \"невалиден wrec офсет\", (long long)roff, (long long)w->used);\n");
    fprintf(out, "    baga_WRec *r = (baga_WRec *)(w->base + roff);\n");
    fprintf(out, "    if (r->n <= 0 || r->n > w->used || r->data_off < 0 || r->data_off > w->used ||\n");
    fprintf(out, "        r->kinds_off < 0 || r->kinds_off > w->used ||\n");
    fprintf(out, "        r->n * 8 > w->used - r->data_off || r->n * 8 > w->used - r->kinds_off)\n");
    fprintf(out, "        baga_world_bad(op, \"повредено wrec заглавие\", (long long)roff, (long long)w->used);\n");
    fprintf(out, "    return r;\n");
    fprintf(out, "}\n");
    fprintf(out, "static int64_t *baga_wrec_fld(baga_Arena *w, baga_WRec *r, int64_t i, int kinds, const char *op) {\n");
    fprintf(out, "    if (i < 0 || i >= r->n) baga_bounds_fail(op, i, r->n);\n");
    fprintf(out, "    int64_t off = (kinds ? r->kinds_off : r->data_off) + i * 8;\n");
    fprintf(out, "    if (off < 0 || off > w->used - 8)\n");
    fprintf(out, "        baga_world_bad(op, \"поле извън региона\", (long long)off, (long long)w->used);\n");
    fprintf(out, "    return (int64_t *)(w->base + off);\n");
    fprintf(out, "}\n");
    fprintf(out, "\n");
    fprintf(out, "/* ---- wvec ---- */\n");
    fprintf(out, "static int64_t baga_wvec_new(int64_t wh) {\n");
    fprintf(out, "    baga_Arena *w = baga_world_a(wh, \"wvec_new\");\n");
    fprintf(out, "    baga_world_mut_check(w, \"wvec_new\");\n");
    fprintf(out, "    int64_t hp = baga_arena_alloc(wh, (int64_t)sizeof(baga_WVec));\n");
    fprintf(out, "    int64_t hoff = baga_woff(w, hp);\n");
    fprintf(out, "    int64_t dp = baga_arena_alloc(wh, 64);\n");
    fprintf(out, "    baga_WVec *v = (baga_WVec *)(w->base + hoff);\n");
    fprintf(out, "    v->len = 0; v->cap = 8; v->ekind = 0;\n");
    fprintf(out, "    v->data_off = baga_woff(w, dp);\n");
    fprintf(out, "    return baga_wtbl_add(wh, hoff, 1);\n");
    fprintf(out, "}\n");
    fprintf(out, "static void baga_wvec_grow(int64_t wh, baga_Arena *w, int64_t voff) {\n");
    fprintf(out, "    baga_WVec *v = (baga_WVec *)(w->base + voff);\n");
    fprintf(out, "    int64_t old_off = v->data_off, old_len = v->len, nc = v->cap * 2;\n");
    fprintf(out, "    if ((uint64_t)old_off + (uint64_t)old_len * 8u > (uint64_t)w->used)\n");
    fprintf(out, "        baga_world_bad(\"wvec_push\", \"старият блок е извън региона\", (long long)old_off, (long long)w->used);\n");
    fprintf(out, "    int64_t nd = baga_arena_alloc(wh, nc * 8);\n");
    fprintf(out, "    memcpy((char *)(intptr_t)nd, w->base + old_off, (size_t)old_len * 8);\n");
    fprintf(out, "    v = (baga_WVec *)(w->base + voff);      /* base може да е преместен */\n");
    fprintf(out, "    v->data_off = baga_woff(w, nd);\n");
    fprintf(out, "    v->cap = nc;\n");
    fprintf(out, "}\n");
    /* W2.14: извикващият вече е направил world_a + mut_check + резолв на id —
     * push_raw ги повтаряше, значи всеки push плащаше ДВА резолва. Офсетът
     * остава валиден и след alloc (растеж/низ в региона), защото регионът е
     * offset-адресиран; указателите се презареждат след grow. */
    fprintf(out, "static void baga_wvec_push_raw(int64_t wh, baga_Arena *w, int64_t voff, int64_t x) {\n");
    fprintf(out, "    baga_WVec *v = baga_wvec_at(w, voff, \"wvec_push\");\n");
    fprintf(out, "    if (v->len == v->cap) { baga_wvec_grow(wh, w, voff); v = baga_wvec_at(w, voff, \"wvec_push\"); }\n");
    fprintf(out, "    *baga_wvec_data(w, v, v->len, \"wvec_push\") = x;\n");
    fprintf(out, "    v->len++;\n");
    fprintf(out, "}\n");
    /* публичният push налага ekind; push_str/push_bytes си маркират СВОЯ kind
     * и ползват raw — иначе вътрешното извикване би видяло \"смесени типове\" */
    fprintf(out, "static void baga_wvec_push(int64_t wh, int64_t id, int64_t x) {\n");
    fprintf(out, "    baga_Arena *w = baga_world_a(wh, \"wvec_push\");\n");
    fprintf(out, "    baga_world_mut_check(w, \"wvec_push\");\n");
    fprintf(out, "    int64_t voff = baga_wres(w, id, 1, \"wvec_push\");\n");
    fprintf(out, "    baga_WVec *v = baga_wvec_at(w, voff, \"wvec_push\");\n");
    fprintf(out, "    baga_kind_set(&v->ekind, 1, \"wvec_push\");\n");
    fprintf(out, "    baga_wvec_push_raw(wh, w, voff, x);\n");
    fprintf(out, "}\n");
    fprintf(out, "static int64_t baga_wvec_len(int64_t wh, int64_t id) {\n");
    fprintf(out, "    baga_Arena *w = baga_world_a(wh, \"wvec_len\");\n");
    fprintf(out, "    baga_WVec *v = baga_wvec_at(w, baga_wres(w, id, 1, \"wvec_len\"), \"wvec_len\");\n");
    fprintf(out, "    return v->len;\n");
    fprintf(out, "}\n");
    fprintf(out, "static int64_t baga_wvec_get(int64_t wh, int64_t id, int64_t i) {\n");
    fprintf(out, "    baga_Arena *w = baga_world_a(wh, \"wvec_get\");\n");
    fprintf(out, "    baga_WVec *v = baga_wvec_at(w, baga_wres(w, id, 1, \"wvec_get\"), \"wvec_get\");\n");
    fprintf(out, "    if (i < 0 || i >= v->len) baga_bounds_fail(\"wvec_get\", i, v->len);\n");
    fprintf(out, "    baga_kind_chk(v->ekind, 1, \"wvec_get\", \"контейнерът не е от i64 елементи\");\n");
    fprintf(out, "    return *baga_wvec_data(w, v, i, \"wvec_get\");\n");
    fprintf(out, "}\n");
    fprintf(out, "static void baga_wvec_push_str(int64_t wh, int64_t id, const char *s) {\n");
    fprintf(out, "    baga_Arena *w = baga_world_a(wh, \"wvec_push_str\");\n");
    fprintf(out, "    baga_world_mut_check(w, \"wvec_push_str\");\n");
    fprintf(out, "    int64_t voff = baga_wres(w, id, 1, \"wvec_push_str\");\n");
    fprintf(out, "    baga_WVec *v = baga_wvec_at(w, voff, \"wvec_push_str\");\n");
    fprintf(out, "    baga_kind_set(&v->ekind, 2, \"wvec_push_str\");\n");
    fprintf(out, "    baga_wvec_push_raw(wh, w, voff, baga_wreg_put_str(wh, w, s));\n");
    fprintf(out, "}\n");
    fprintf(out, "static const char *baga_wvec_get_str(int64_t wh, int64_t id, int64_t i) {\n");
    fprintf(out, "    baga_Arena *w = baga_world_a(wh, \"wvec_get_str\");\n");
    fprintf(out, "    baga_WVec *v = baga_wvec_at(w, baga_wres(w, id, 1, \"wvec_get_str\"), \"wvec_get_str\");\n");
    fprintf(out, "    if (i < 0 || i >= v->len) baga_bounds_fail(\"wvec_get_str\", i, v->len);\n");
    fprintf(out, "    baga_kind_chk(v->ekind, 2, \"wvec_get_str\", \"контейнерът не е от низове\");\n");
    fprintf(out, "    return w->base + baga_wreg_str_off(w, *baga_wvec_data(w, v, i, \"wvec_get_str\"), \"wvec_get_str\");\n");
    fprintf(out, "}\n");
    fprintf(out, "static void baga_wvec_push_bytes(int64_t wh, int64_t id, baga_bytes b) {\n");
    fprintf(out, "    baga_Arena *w = baga_world_a(wh, \"wvec_push_bytes\");\n");
    fprintf(out, "    baga_world_mut_check(w, \"wvec_push_bytes\");\n");
    fprintf(out, "    int64_t voff = baga_wres(w, id, 1, \"wvec_push_bytes\");\n");
    fprintf(out, "    baga_WVec *v = baga_wvec_at(w, voff, \"wvec_push_bytes\");\n");
    fprintf(out, "    baga_kind_set(&v->ekind, 3, \"wvec_push_bytes\");\n");
    fprintf(out, "    baga_wvec_push_raw(wh, w, voff, baga_wreg_put_bytes(wh, w, b, \"wvec_push_bytes\"));\n");
    fprintf(out, "}\n");
    fprintf(out, "static baga_bytes baga_wvec_get_bytes(int64_t wh, int64_t id, int64_t i) {\n");
    fprintf(out, "    baga_Arena *w = baga_world_a(wh, \"wvec_get_bytes\");\n");
    fprintf(out, "    baga_WVec *v = baga_wvec_at(w, baga_wres(w, id, 1, \"wvec_get_bytes\"), \"wvec_get_bytes\");\n");
    fprintf(out, "    if (i < 0 || i >= v->len) baga_bounds_fail(\"wvec_get_bytes\", i, v->len);\n");
    fprintf(out, "    baga_kind_chk(v->ekind, 3, \"wvec_get_bytes\", \"контейнерът не е от bytes елементи\");\n");
    fprintf(out, "    int64_t soff = *baga_wvec_data(w, v, i, \"wvec_get_bytes\");\n");
    fprintf(out, "    int64_t n = baga_wreg_bytes_off(w, soff, \"wvec_get_bytes\");\n");
    fprintf(out, "    baga_bytes r; r.len = n; r.data = (unsigned char *)(w->base + soff + 8);\n");
    fprintf(out, "    return r;\n");
    fprintf(out, "}\n");
    fprintf(out, "\n");
    fprintf(out, "/* ---- wmap ---- (baga_wc_h е дефинирана по-нагоре, в wcache блока) */\n");
    fprintf(out, "static void baga_wm_insert(baga_Arena *w, baga_WMap *m, int64_t k, int64_t v, const char *op) {\n");
    fprintf(out, "    int64_t mask = m->cap - 1;\n");
    fprintf(out, "    int64_t i = (int64_t)(baga_wc_h(k) & (uint64_t)mask), hole = -1;\n");
    fprintf(out, "    for (;;) {\n");
    fprintf(out, "        baga_WSlot *s = baga_wslot(w, m, i, op);\n");
    fprintf(out, "        if (s->st == 0) break;\n");
    fprintf(out, "        if (s->st == 1) { if (s->k == k) { s->v = v; return; } }\n");
    fprintf(out, "        else if (hole < 0) hole = i;\n");
    fprintf(out, "        i = (i + 1) & mask;\n");
    fprintf(out, "    }\n");
    fprintf(out, "    if (hole >= 0) i = hole;\n");
    fprintf(out, "    baga_WSlot *s = baga_wslot(w, m, i, op);\n");
    fprintf(out, "    s->k = k; s->v = v; s->st = 1;\n");
    fprintf(out, "    m->len++;\n");
    fprintf(out, "}\n");
    fprintf(out, "static baga_WSlot *baga_wm_find(baga_Arena *w, baga_WMap *m, int64_t k, const char *op) {\n");
    fprintf(out, "    if (m->len == 0) return NULL;\n");
    fprintf(out, "    int64_t mask = m->cap - 1;\n");
    fprintf(out, "    int64_t i = (int64_t)(baga_wc_h(k) & (uint64_t)mask);\n");
    fprintf(out, "    for (;;) {\n");
    fprintf(out, "        baga_WSlot *s = baga_wslot(w, m, i, op);\n");
    fprintf(out, "        if (s->st == 0) return NULL;\n");
    fprintf(out, "        if (s->st == 1 && s->k == k) return s;\n");
    fprintf(out, "        i = (i + 1) & mask;\n");
    fprintf(out, "    }\n");
    fprintf(out, "}\n");
    fprintf(out, "static int64_t baga_wmap_new(int64_t wh) {\n");
    fprintf(out, "    baga_Arena *w = baga_world_a(wh, \"wmap_new\");\n");
    fprintf(out, "    baga_world_mut_check(w, \"wmap_new\");\n");
    fprintf(out, "    int64_t hp = baga_arena_alloc(wh, (int64_t)sizeof(baga_WMap));\n");
    fprintf(out, "    int64_t hoff = baga_woff(w, hp);\n");
    fprintf(out, "    int64_t sp = baga_arena_alloc(wh, 16 * (int64_t)sizeof(baga_WSlot));\n");
    fprintf(out, "    int64_t soff = baga_woff(w, sp);\n");
    fprintf(out, "    memset(w->base + soff, 0, (size_t)(16 * sizeof(baga_WSlot)));\n");
    fprintf(out, "    baga_WMap *m = (baga_WMap *)(w->base + hoff);\n");
    fprintf(out, "    m->len = 0; m->cap = 16; m->slots_off = soff; m->vkind = 0;\n");
    fprintf(out, "    return baga_wtbl_add(wh, hoff, 2);\n");
    fprintf(out, "}\n");
    /* W2.14: като wvec_push_raw — извикващият вече е резолвнал id-то. */
    fprintf(out, "static void baga_wmap_set_raw(int64_t wh, baga_Arena *w, int64_t moff, int64_t k, int64_t v) {\n");
    fprintf(out, "    baga_WMap *m = baga_wmap_at(w, moff, \"wmap_set\");\n");
    fprintf(out, "    if ((m->len + 1) * 4 > m->cap * 3) {\n");
    fprintf(out, "        int64_t ocap = m->cap, ooff = m->slots_off, ncap = ocap * 2;\n");
    fprintf(out, "        if (ooff < 0 || ooff + ocap * (int64_t)sizeof(baga_WSlot) > w->used)\n");
    fprintf(out, "            baga_world_bad(\"wmap_set\", \"старият блок е извън региона\", (long long)ooff, (long long)w->used);\n");
    fprintf(out, "        int64_t np = baga_arena_alloc(wh, ncap * (int64_t)sizeof(baga_WSlot));\n");
    fprintf(out, "        int64_t noff = baga_woff(w, np);\n");
    fprintf(out, "        memset(w->base + noff, 0, (size_t)(ncap * sizeof(baga_WSlot)));\n");
    fprintf(out, "        m = (baga_WMap *)(w->base + moff);\n");
    fprintf(out, "        m->cap = ncap; m->slots_off = noff; m->len = 0;\n");
    fprintf(out, "        for (int64_t i = 0; i < ocap; i++) {\n");
    fprintf(out, "            baga_WSlot *os = (baga_WSlot *)(w->base + ooff + i * (int64_t)sizeof(baga_WSlot));\n");
    fprintf(out, "            if (os->st == 1) baga_wm_insert(w, m, os->k, os->v, \"wmap_set\");\n");
    fprintf(out, "        }\n");
    fprintf(out, "        m = (baga_WMap *)(w->base + moff);\n");
    fprintf(out, "    }\n");
    fprintf(out, "    baga_wm_insert(w, m, k, v, \"wmap_set\");\n");
    fprintf(out, "}\n");
    fprintf(out, "static void baga_wmap_set(int64_t wh, int64_t id, int64_t k, int64_t v) {\n");
    fprintf(out, "    baga_Arena *w = baga_world_a(wh, \"wmap_set\");\n");
    fprintf(out, "    baga_world_mut_check(w, \"wmap_set\");\n");
    fprintf(out, "    int64_t moff = baga_wres(w, id, 2, \"wmap_set\");\n");
    fprintf(out, "    baga_WMap *m = baga_wmap_at(w, moff, \"wmap_set\");\n");
    fprintf(out, "    baga_kind_set(&m->vkind, 1, \"wmap_set\");\n");
    fprintf(out, "    baga_wmap_set_raw(wh, w, moff, k, v);\n");
    fprintf(out, "}\n");
    fprintf(out, "static int64_t baga_wmap_get(int64_t wh, int64_t id, int64_t k) {\n");
    fprintf(out, "    baga_Arena *w = baga_world_a(wh, \"wmap_get\");\n");
    fprintf(out, "    baga_WMap *m = baga_wmap_at(w, baga_wres(w, id, 2, \"wmap_get\"), \"wmap_get\");\n");
    fprintf(out, "    baga_kind_chk(m->vkind, 1, \"wmap_get\", \"контейнерът не е от i64 стойности\");\n");
    fprintf(out, "    baga_WSlot *s = baga_wm_find(w, m, k, \"wmap_get\");\n");
    fprintf(out, "    return s ? s->v : 0;\n");
    fprintf(out, "}\n");
    fprintf(out, "static int64_t baga_wmap_has(int64_t wh, int64_t id, int64_t k) {\n");
    fprintf(out, "    baga_Arena *w = baga_world_a(wh, \"wmap_has\");\n");
    fprintf(out, "    baga_WMap *m = baga_wmap_at(w, baga_wres(w, id, 2, \"wmap_has\"), \"wmap_has\");\n");
    fprintf(out, "    return baga_wm_find(w, m, k, \"wmap_has\") ? 1 : 0;\n");
    fprintf(out, "}\n");
    fprintf(out, "static void baga_wmap_del(int64_t wh, int64_t id, int64_t k) {\n");
    fprintf(out, "    baga_Arena *w = baga_world_a(wh, \"wmap_del\");\n");
    fprintf(out, "    baga_world_mut_check(w, \"wmap_del\");\n");
    fprintf(out, "    baga_WMap *m = baga_wmap_at(w, baga_wres(w, id, 2, \"wmap_del\"), \"wmap_del\");\n");
    fprintf(out, "    baga_WSlot *s = baga_wm_find(w, m, k, \"wmap_del\");\n");
    fprintf(out, "    if (s) { s->st = 2; m->len--; }\n");
    fprintf(out, "}\n");
    fprintf(out, "static int64_t baga_wmap_len(int64_t wh, int64_t id) {\n");
    fprintf(out, "    baga_Arena *w = baga_world_a(wh, \"wmap_len\");\n");
    fprintf(out, "    return baga_wmap_at(w, baga_wres(w, id, 2, \"wmap_len\"), \"wmap_len\")->len;\n");
    fprintf(out, "}\n");
    fprintf(out, "static void baga_wmap_set_str(int64_t wh, int64_t id, int64_t k, const char *s) {\n");
    fprintf(out, "    baga_Arena *w = baga_world_a(wh, \"wmap_set_str\");\n");
    fprintf(out, "    baga_world_mut_check(w, \"wmap_set_str\");\n");
    fprintf(out, "    int64_t moff = baga_wres(w, id, 2, \"wmap_set_str\");\n");
    fprintf(out, "    baga_WMap *m = baga_wmap_at(w, moff, \"wmap_set_str\");\n");
    fprintf(out, "    baga_kind_set(&m->vkind, 2, \"wmap_set_str\");\n");
    fprintf(out, "    baga_wmap_set_raw(wh, w, moff, k, baga_wreg_put_str(wh, w, s));\n");
    fprintf(out, "}\n");
    fprintf(out, "static const char *baga_wmap_get_str(int64_t wh, int64_t id, int64_t k) {\n");
    fprintf(out, "    baga_Arena *w = baga_world_a(wh, \"wmap_get_str\");\n");
    fprintf(out, "    baga_WMap *m = baga_wmap_at(w, baga_wres(w, id, 2, \"wmap_get_str\"), \"wmap_get_str\");\n");
    fprintf(out, "    baga_kind_chk(m->vkind, 2, \"wmap_get_str\", \"контейнерът не е от низове\");\n");
    fprintf(out, "    baga_WSlot *s = baga_wm_find(w, m, k, \"wmap_get_str\");\n");
    fprintf(out, "    if (!s) { fprintf(stderr, \"baga: wmap_get_str: липсващ ключ %%lld\\n\", (long long)k); exit(1); }\n");
    fprintf(out, "    return w->base + baga_wreg_str_off(w, s->v, \"wmap_get_str\");\n");
    fprintf(out, "}\n");
    fprintf(out, "\n");
    fprintf(out, "/* ---- wrec ----: структурни стойности в региона.\n");
    fprintf(out, " * Записът е хетерогенен: всяко поле има свой вид (i64/str/bytes), фиксиран при\n");
    fprintf(out, " * първия запис в него. Не е зададено → четенето е чиста грешка (записът е\n");
    fprintf(out, " * схема, не рядък map); wrec_has пита без да чете. Повторен запис на поле от\n");
    fprintf(out, " * същия вид презаписва стойността — старият payload остава в региона като\n");
    fprintf(out, " * growth garbage и се прибира при компакция (W2.10). */\n");
    fprintf(out, "static int64_t baga_wrec_new(int64_t wh, int64_t n) {\n");
    fprintf(out, "    baga_Arena *w = baga_world_a(wh, \"wrec_new\");\n");
    fprintf(out, "    baga_world_mut_check(w, \"wrec_new\");\n");
    fprintf(out, "    if (n <= 0 || n > INT64_MAX / 16) {\n");
    fprintf(out, "        fprintf(stderr, \"baga: wrec_new: невалиден брой полета %%lld\\n\", (long long)n); exit(1);\n");
    fprintf(out, "    }\n");
    fprintf(out, "    int64_t hp = baga_arena_alloc(wh, (int64_t)sizeof(baga_WRec));\n");
    fprintf(out, "    int64_t hoff = baga_woff(w, hp);\n");
    fprintf(out, "    int64_t dp = baga_arena_alloc(wh, n * 8);\n");
    fprintf(out, "    int64_t doff = baga_woff(w, dp);\n");
    fprintf(out, "    int64_t kp = baga_arena_alloc(wh, n * 8);\n");
    fprintf(out, "    int64_t koff = baga_woff(w, kp);\n");
    fprintf(out, "    baga_WRec *r = (baga_WRec *)(w->base + hoff);\n");
    fprintf(out, "    r->n = n; r->data_off = doff; r->kinds_off = koff;\n");
    fprintf(out, "    memset(w->base + doff, 0, (size_t)(n * 8));\n");
    fprintf(out, "    memset(w->base + koff, 0, (size_t)(n * 8));\n");
    fprintf(out, "    return baga_wtbl_add(wh, hoff, 3);\n");
    fprintf(out, "}\n");
    fprintf(out, "static int64_t baga_wrec_len(int64_t wh, int64_t id) {\n");
    fprintf(out, "    baga_Arena *w = baga_world_a(wh, \"wrec_len\");\n");
    fprintf(out, "    return baga_wrec_at(w, baga_wres(w, id, 3, \"wrec_len\"), \"wrec_len\")->n;\n");
    fprintf(out, "}\n");
    fprintf(out, "static int64_t baga_wrec_has(int64_t wh, int64_t id, int64_t i) {\n");
    fprintf(out, "    baga_Arena *w = baga_world_a(wh, \"wrec_has\");\n");
    fprintf(out, "    baga_WRec *r = baga_wrec_at(w, baga_wres(w, id, 3, \"wrec_has\"), \"wrec_has\");\n");
    fprintf(out, "    return *baga_wrec_fld(w, r, i, 1, \"wrec_has\") != 0;\n");
    fprintf(out, "}\n");
    fprintf(out, "static void baga_wrec_kind(baga_Arena *w, baga_WRec *r, int64_t i, int64_t want, const char *op) {\n");
    fprintf(out, "    int64_t *k = baga_wrec_fld(w, r, i, 1, op);\n");
    fprintf(out, "    if (*k == 0) { *k = want; return; }\n");
    fprintf(out, "    if (*k != want)\n");
    fprintf(out, "        baga_world_bad(op, \"полето вече е от друг вид\", (long long)*k, (long long)want);\n");
    fprintf(out, "}\n");
    fprintf(out, "static void baga_wrec_set(int64_t wh, int64_t id, int64_t i, int64_t v) {\n");
    fprintf(out, "    baga_Arena *w = baga_world_a(wh, \"wrec_set\");\n");
    fprintf(out, "    baga_world_mut_check(w, \"wrec_set\");\n");
    fprintf(out, "    baga_WRec *r = baga_wrec_at(w, baga_wres(w, id, 3, \"wrec_set\"), \"wrec_set\");\n");
    fprintf(out, "    baga_wrec_kind(w, r, i, 1, \"wrec_set\");\n");
    fprintf(out, "    *baga_wrec_fld(w, r, i, 0, \"wrec_set\") = v;\n");
    fprintf(out, "}\n");
    fprintf(out, "static void baga_wrec_set_str(int64_t wh, int64_t id, int64_t i, const char *s) {\n");
    fprintf(out, "    baga_Arena *w = baga_world_a(wh, \"wrec_set_str\");\n");
    fprintf(out, "    baga_world_mut_check(w, \"wrec_set_str\");\n");
    fprintf(out, "    int64_t roff = baga_wres(w, id, 3, \"wrec_set_str\");\n");
    fprintf(out, "    baga_WRec *r = baga_wrec_at(w, roff, \"wrec_set_str\");\n");
    fprintf(out, "    baga_wrec_kind(w, r, i, 2, \"wrec_set_str\");\n");
    fprintf(out, "    int64_t soff = baga_wreg_put_str(wh, w, s);\n");
    fprintf(out, "    r = baga_wrec_at(w, roff, \"wrec_set_str\");   /* base може да е преместен */\n");
    fprintf(out, "    *baga_wrec_fld(w, r, i, 0, \"wrec_set_str\") = soff;\n");
    fprintf(out, "}\n");
    fprintf(out, "static void baga_wrec_set_bytes(int64_t wh, int64_t id, int64_t i, baga_bytes b) {\n");
    fprintf(out, "    baga_Arena *w = baga_world_a(wh, \"wrec_set_bytes\");\n");
    fprintf(out, "    baga_world_mut_check(w, \"wrec_set_bytes\");\n");
    fprintf(out, "    int64_t roff = baga_wres(w, id, 3, \"wrec_set_bytes\");\n");
    fprintf(out, "    baga_WRec *r = baga_wrec_at(w, roff, \"wrec_set_bytes\");\n");
    fprintf(out, "    baga_wrec_kind(w, r, i, 3, \"wrec_set_bytes\");\n");
    fprintf(out, "    int64_t off = baga_wreg_put_bytes(wh, w, b, \"wrec_set_bytes\");\n");
    fprintf(out, "    r = baga_wrec_at(w, roff, \"wrec_set_bytes\");\n");
    fprintf(out, "    *baga_wrec_fld(w, r, i, 0, \"wrec_set_bytes\") = off;\n");
    fprintf(out, "}\n");
    fprintf(out, "static int64_t baga_wrec_rd(baga_Arena *w, baga_WRec *r, int64_t i, int64_t want, const char *op) {\n");
    fprintf(out, "    int64_t k = *baga_wrec_fld(w, r, i, 1, op);\n");
    fprintf(out, "    if (k == 0) baga_world_bad(op, \"полето не е зададено\", (long long)i, 0);\n");
    fprintf(out, "    baga_kind_chk(k, want, op, \"полето е от друг вид\");\n");
    fprintf(out, "    return *baga_wrec_fld(w, r, i, 0, op);\n");
    fprintf(out, "}\n");
    fprintf(out, "static int64_t baga_wrec_get(int64_t wh, int64_t id, int64_t i) {\n");
    fprintf(out, "    baga_Arena *w = baga_world_a(wh, \"wrec_get\");\n");
    fprintf(out, "    baga_WRec *r = baga_wrec_at(w, baga_wres(w, id, 3, \"wrec_get\"), \"wrec_get\");\n");
    fprintf(out, "    return baga_wrec_rd(w, r, i, 1, \"wrec_get\");\n");
    fprintf(out, "}\n");
    fprintf(out, "static const char *baga_wrec_get_str(int64_t wh, int64_t id, int64_t i) {\n");
    fprintf(out, "    baga_Arena *w = baga_world_a(wh, \"wrec_get_str\");\n");
    fprintf(out, "    baga_WRec *r = baga_wrec_at(w, baga_wres(w, id, 3, \"wrec_get_str\"), \"wrec_get_str\");\n");
    fprintf(out, "    int64_t soff = baga_wrec_rd(w, r, i, 2, \"wrec_get_str\");\n");
    fprintf(out, "    return w->base + baga_wreg_str_off(w, soff, \"wrec_get_str\");\n");
    fprintf(out, "}\n");
    fprintf(out, "static baga_bytes baga_wrec_get_bytes(int64_t wh, int64_t id, int64_t i) {\n");
    fprintf(out, "    baga_Arena *w = baga_world_a(wh, \"wrec_get_bytes\");\n");
    fprintf(out, "    baga_WRec *r = baga_wrec_at(w, baga_wres(w, id, 3, \"wrec_get_bytes\"), \"wrec_get_bytes\");\n");
    fprintf(out, "    int64_t soff = baga_wrec_rd(w, r, i, 3, \"wrec_get_bytes\");\n");
    fprintf(out, "    int64_t n = baga_wreg_bytes_off(w, soff, \"wrec_get_bytes\");\n");
    fprintf(out, "    baga_bytes res; res.len = n; res.data = (unsigned char *)(w->base + soff + 8);\n");
    fprintf(out, "    return res;\n");
    fprintf(out, "}\n");
    fprintf(out, "\n");
    fprintf(out, "/* ---- W2.10: компакция ----\n");
    fprintf(out, " * Корените са таблицата на контейнерите, а схемата е в ekind/vkind — затова\n");
    fprintf(out, " * живите байтове са ИЗБРОИМИ и пренареждането е механично: заглавие + текущ\n");
    fprintf(out, " * блок + payload-ите на str/bytes елементите. Мъртвите блокове от растеж\n");
    fprintf(out, " * (стари data/slots) не се копират. id-тата не се променят (таблицата се\n");
    fprintf(out, " * построява в същия ред), значи променливите на програмата остават валидни.\n");
    fprintf(out, " * Заеманите указатели (wvec_get_str/get_bytes) НЕ оцеляват — като при растеж. */\n");
    fprintf(out, "typedef struct { char *base; int64_t used; int64_t cap; } baga_WImg;\n");
    fprintf(out, "\n");
    fprintf(out, "static int64_t baga_wimg_init(baga_Arena *t, int64_t hint) {\n");
    fprintf(out, "    memset(t, 0, sizeof(*t));\n");
    fprintf(out, "    t->cap = hint > 4096 ? hint : 4096;\n");
    fprintf(out, "    t->base = malloc((size_t)t->cap);\n");
    fprintf(out, "    if (!t->base) { fprintf(stderr, \"baga: world_compact: oom\\n\"); exit(1); }\n");
    fprintf(out, "    pthread_mutex_init(&t->wlock, NULL);\n");
    fprintf(out, "    t->wcache = NULL; t->links = NULL; t->n_links = 0; t->cap_links = 0; t->linked_by = 0;\n");
    fprintf(out, "    baga_WHdr *hd = (baga_WHdr *)t->base;\n");
    fprintf(out, "    hd->magic = BAGA_WORLD_MAGIC; hd->version = BAGA_WORLD_VERSION;\n");
    fprintf(out, "    hd->tbl_off = 0; hd->tbl_len = 0; hd->tbl_cap = 0;\n");
    fprintf(out, "    t->used = (int64_t)sizeof(baga_WHdr);\n");
    fprintf(out, "    return (int64_t)(intptr_t)t;\n");
    fprintf(out, "}\n");
    fprintf(out, "static int64_t baga_pow2_at_least(int64_t n) {\n");
    fprintf(out, "    int64_t c = 16;\n");
    fprintf(out, "    while (c < n) c *= 2;\n");
    fprintf(out, "    return c;\n");
    fprintf(out, "}\n");
    fprintf(out, "/* Payload-ите се копират в образа и офсетът им се презаписва — src не се мести,\n");
    fprintf(out, " * t се мести при всяко alloc, затова връщаме офсет, не указател. */\n");
    fprintf(out, "static int64_t baga_wimg_copy_str(int64_t th, baga_Arena *t, baga_Arena *src, int64_t val, const char *op) {\n");
    fprintf(out, "    int64_t soff = baga_wreg_str_off(src, val, op);\n");
    fprintf(out, "    int64_t n = (int64_t)strlen(src->base + soff);\n");
    fprintf(out, "    int64_t np = baga_arena_alloc(th, n + 1);\n");
    fprintf(out, "    memcpy((char *)(intptr_t)np, src->base + soff, (size_t)n + 1);\n");
    fprintf(out, "    return baga_woff(t, np);\n");
    fprintf(out, "}\n");
    fprintf(out, "static int64_t baga_wimg_copy_bytes(int64_t th, baga_Arena *t, baga_Arena *src, int64_t val, const char *op) {\n");
    fprintf(out, "    int64_t n = baga_wreg_bytes_off(src, val, op);   /* дължина на payload-а */\n");
    fprintf(out, "    int64_t bp = baga_arena_alloc(th, 8 + n);\n");
    fprintf(out, "    memcpy((char *)(intptr_t)bp, src->base + val, (size_t)(8 + n));\n");
    fprintf(out, "    return baga_woff(t, bp);\n");
    fprintf(out, "}\n");
    fprintf(out, "static void baga_world_compact_build(baga_Arena *src, baga_WImg *out, const char *op) {\n");
    fprintf(out, "    baga_WHdr *sh = baga_whdr(src);\n");
    fprintf(out, "    baga_Arena t;\n");
    fprintf(out, "    int64_t th = baga_wimg_init(&t, src->used / 2);\n");
    fprintf(out, "    if (sh->tbl_len > 0) {\n");
    fprintf(out, "        if (sh->tbl_off <= 0 || sh->tbl_off > src->used || sh->tbl_cap < sh->tbl_len ||\n");
    fprintf(out, "            sh->tbl_cap * (int64_t)sizeof(baga_WEnt) > src->used - sh->tbl_off)\n");
    fprintf(out, "            baga_world_bad(op, \"повредена таблица на контейнерите\", (long long)sh->tbl_off, (long long)src->used);\n");
    fprintf(out, "    }\n");
    fprintf(out, "    for (int64_t i = 0; i < sh->tbl_len; i++) {\n");
    fprintf(out, "        baga_WEnt ent = ((baga_WEnt *)(src->base + sh->tbl_off))[i];   /* копие: src не се мести */\n");
    fprintf(out, "        if (ent.off < 0 || ent.off > src->used)\n");
    fprintf(out, "            baga_world_bad(op, \"контейнер сочи извън региона\", (long long)ent.off, (long long)src->used);\n");
    fprintf(out, "        if (ent.kind == 1) {\n");
    fprintf(out, "            baga_WVec *v = baga_wvec_at(src, ent.off, op);\n");
    fprintf(out, "            int64_t ncap = v->len > 8 ? v->len : 8;\n");
    fprintf(out, "            int64_t hp = baga_arena_alloc(th, (int64_t)sizeof(baga_WVec));\n");
    fprintf(out, "            int64_t hoff = baga_woff(&t, hp);\n");
    fprintf(out, "            int64_t dp = baga_arena_alloc(th, ncap * 8);\n");
    fprintf(out, "            int64_t doff = baga_woff(&t, dp);\n");
    fprintf(out, "            baga_WVec *nv = (baga_WVec *)(t.base + hoff);\n");
    fprintf(out, "            nv->len = v->len; nv->cap = ncap; nv->data_off = doff; nv->ekind = v->ekind;\n");
    fprintf(out, "            for (int64_t j = 0; j < v->len; j++) {\n");
    fprintf(out, "                int64_t val = *baga_wvec_data(src, v, j, op);\n");
    fprintf(out, "                if (v->ekind == 2) val = baga_wimg_copy_str(th, &t, src, val, op);\n");
    fprintf(out, "                else if (v->ekind == 3) val = baga_wimg_copy_bytes(th, &t, src, val, op);\n");
    fprintf(out, "                *baga_wvec_data(&t, (baga_WVec *)(t.base + hoff), j, op) = val;\n");
    fprintf(out, "            }\n");
    fprintf(out, "            baga_wtbl_add(th, hoff, 1);\n");
    fprintf(out, "        } else if (ent.kind == 2) {\n");
    fprintf(out, "            baga_WMap *m = baga_wmap_at(src, ent.off, op);\n");
    fprintf(out, "            int64_t ncap = baga_pow2_at_least(m->len * 2);\n");
    fprintf(out, "            int64_t hp = baga_arena_alloc(th, (int64_t)sizeof(baga_WMap));\n");
    fprintf(out, "            int64_t hoff = baga_woff(&t, hp);\n");
    fprintf(out, "            int64_t sp = baga_arena_alloc(th, ncap * (int64_t)sizeof(baga_WSlot));\n");
    fprintf(out, "            int64_t soff = baga_woff(&t, sp);\n");
    fprintf(out, "            memset(t.base + soff, 0, (size_t)(ncap * sizeof(baga_WSlot)));\n");
    fprintf(out, "            baga_WMap *nm = (baga_WMap *)(t.base + hoff);\n");
    fprintf(out, "            nm->len = 0; nm->cap = ncap; nm->slots_off = soff; nm->vkind = m->vkind;\n");
    fprintf(out, "            for (int64_t j = 0; j < m->cap; j++) {\n");
    fprintf(out, "                baga_WSlot *os = baga_wslot(src, m, j, op);\n");
    fprintf(out, "                if (os->st != 1) continue;\n");
    fprintf(out, "                int64_t val = os->v;\n");
    fprintf(out, "                if (m->vkind == 2) val = baga_wimg_copy_str(th, &t, src, val, op);\n");
    fprintf(out, "                baga_wm_insert(&t, (baga_WMap *)(t.base + hoff), os->k, val, op);\n");
    fprintf(out, "            }\n");
    fprintf(out, "            baga_wtbl_add(th, hoff, 2);\n");
    fprintf(out, "        } else if (ent.kind == 3) {\n");
    fprintf(out, "            baga_WRec *r = baga_wrec_at(src, ent.off, op);\n");
    fprintf(out, "            int64_t n = r->n;\n");
    fprintf(out, "            int64_t hp = baga_arena_alloc(th, (int64_t)sizeof(baga_WRec));\n");
    fprintf(out, "            int64_t hoff = baga_woff(&t, hp);\n");
    fprintf(out, "            int64_t dp = baga_arena_alloc(th, n * 8);\n");
    fprintf(out, "            int64_t doff = baga_woff(&t, dp);\n");
    fprintf(out, "            int64_t kp = baga_arena_alloc(th, n * 8);\n");
    fprintf(out, "            int64_t koff = baga_woff(&t, kp);\n");
    fprintf(out, "            baga_WRec *nr = (baga_WRec *)(t.base + hoff);\n");
    fprintf(out, "            nr->n = n; nr->data_off = doff; nr->kinds_off = koff;\n");
    fprintf(out, "            for (int64_t j = 0; j < n; j++) {\n");
    fprintf(out, "                int64_t k = *baga_wrec_fld(src, r, j, 1, op);\n");
    fprintf(out, "                int64_t val = *baga_wrec_fld(src, r, j, 0, op);\n");
    fprintf(out, "                if (k == 2) val = baga_wimg_copy_str(th, &t, src, val, op);\n");
    fprintf(out, "                else if (k == 3) val = baga_wimg_copy_bytes(th, &t, src, val, op);\n");
    fprintf(out, "                nr = (baga_WRec *)(t.base + hoff);   /* base може да е преместен */\n");
    fprintf(out, "                *baga_wrec_fld(&t, nr, j, 1, op) = k;\n");
    fprintf(out, "                *baga_wrec_fld(&t, nr, j, 0, op) = val;\n");
    fprintf(out, "            }\n");
    fprintf(out, "            baga_wtbl_add(th, hoff, 3);\n");
    fprintf(out, "        } else {\n");
    fprintf(out, "            baga_world_bad(op, \"непознат вид контейнер в таблицата\", (long long)ent.kind, 0);\n");
    fprintf(out, "        }\n");
    fprintf(out, "    }\n");
    fprintf(out, "    out->base = t.base; out->used = t.used; out->cap = t.cap;\n");
    fprintf(out, "    pthread_mutex_destroy(&t.wlock);\n");
    fprintf(out, "}\n");
    fprintf(out, "static void baga_world_compact(int64_t wh) {\n");
    fprintf(out, "    baga_Arena *w = baga_world_a(wh, \"world_compact\");\n");
    fprintf(out, "    baga_world_mut_check(w, \"world_compact\");\n");
    fprintf(out, "    baga_WImg img;\n");
    fprintf(out, "    baga_world_compact_build(w, &img, \"world_compact\");\n");
    fprintf(out, "    free(w->base);\n");
    fprintf(out, "    w->base = img.base; w->used = img.used; w->cap = img.cap;\n");
    fprintf(out, "}\n");
    fprintf(out, "static baga_bytes baga_world_dump_compact(int64_t wh) {\n");
    fprintf(out, "    baga_Arena *w = baga_world_a(wh, \"world_dump_compact\");\n");
    fprintf(out, "    baga_WImg img;\n");
    fprintf(out, "    baga_world_compact_build(w, &img, \"world_dump_compact\");\n");
    fprintf(out, "    baga_bytes r;\n");
    fprintf(out, "    r.len = img.used;\n");
    fprintf(out, "    r.data = baga_alloc((size_t)(img.used > 0 ? img.used : 1));\n");
    fprintf(out, "    if (img.used > 0) memcpy(r.data, img.base, (size_t)img.used);\n");
    fprintf(out, "    free(img.base);\n");
    fprintf(out, "    return r;\n");
    fprintf(out, "}\n");
    fprintf(out, "\n");
    fprintf(out, "/* dump/load: светът е position-independent — dump е memcpy на региона\n");
    fprintf(out, " * (включително заглавието и таблицата), load го възстановява в НОВА арена и\n");
    fprintf(out, " * id-тата важат веднага. */\n");
    fprintf(out, "static baga_bytes baga_world_dump(int64_t wh) {\n");
    fprintf(out, "    baga_Arena *w = baga_world_a(wh, \"world_dump\");\n");
    fprintf(out, "    baga_bytes r; r.len = w->used;\n");
    fprintf(out, "    r.data = baga_alloc((size_t)(w->used > 0 ? w->used : 1));\n");
    fprintf(out, "    if (w->used > 0) memcpy(r.data, w->base, (size_t)w->used);\n");
    fprintf(out, "    return r;\n");
    fprintf(out, "}\n");
    fprintf(out, "static int64_t baga_world_load(baga_bytes b) {\n");
    fprintf(out, "    if (b.len < 0 || (b.len > 0 && !b.data)) { fprintf(stderr, \"baga: world_load: невалиден blob\\n\"); exit(1); }\n");
    fprintf(out, "    if (b.len < (int64_t)sizeof(baga_WHdr)) { fprintf(stderr, \"baga: world_load: blob-ът не е свят (твърде малък)\\n\"); exit(1); }\n");
    fprintf(out, "    baga_WHdr *bh = (baga_WHdr *)b.data;\n");
    fprintf(out, "    if (bh->magic != BAGA_WORLD_MAGIC) { fprintf(stderr, \"baga: world_load: blob-ът не е свят (няма magic)\\n\"); exit(1); }\n");
    fprintf(out, "    if (bh->version != BAGA_WORLD_VERSION) { fprintf(stderr, \"baga: world_load: неподдържана версия %%lld\\n\", (long long)bh->version); exit(1); }\n");
    fprintf(out, "    baga_Arena *a = malloc(sizeof(baga_Arena));\n");
    fprintf(out, "    if (!a) { fprintf(stderr, \"baga: world_load: oom\\n\"); exit(1); }\n");
    fprintf(out, "    a->used = b.len; a->cap = b.len > 65536 ? b.len : 65536; a->frozen = 0;\n");
    fprintf(out, "    a->base = malloc((size_t)a->cap);\n");
    fprintf(out, "    if (!a->base) { fprintf(stderr, \"baga: world_load: oom\\n\"); exit(1); }\n");
    fprintf(out, "    memcpy(a->base, b.data, (size_t)b.len);\n");
    fprintf(out, "    pthread_mutex_init(&a->wlock, NULL); a->wcache = NULL;\n");
    fprintf(out, "    a->links = NULL; a->n_links = 0; a->cap_links = 0; a->linked_by = 0;\n");
    fprintf(out, "    return (int64_t)(intptr_t)a;\n");
    fprintf(out, "}\n");
    fprintf(out, "static int64_t baga_world_used(int64_t wh) {\n");
    fprintf(out, "    baga_Arena *w = baga_world_a(wh, \"world_used\");\n");
    fprintf(out, "    return w->used;\n");
    fprintf(out, "}\n");
    fprintf(out, "\n");
    /* W2.3: вътрешна променяемост — кеш върху замразен свят. НЯМА frozen
     * проверка (това е смисълът на фичата); всеки достъп е под mutex, защото
     * замразените светове се споделят между нишки-читатели. Стойностите са
     * i64 (id/офсет/брой/хеш) — за низове и структури трябва scratch регион,
     * което е следваща стъпка. */
    fprintf(out, "static void baga_wcache_set(int64_t wh, int64_t k, int64_t v) {\n");
    fprintf(out, "    baga_Arena *w = baga_world_a(wh, \"wcache_set\");\n");
    fprintf(out, "    pthread_mutex_lock(&w->wlock);\n");
    fprintf(out, "    if (!w->wcache) w->wcache = baga_wc_new();\n");
    fprintf(out, "    baga_wc_put(w->wcache, k, 1, v, NULL, 0);\n");
    fprintf(out, "    pthread_mutex_unlock(&w->wlock);\n");
    fprintf(out, "}\n");
    fprintf(out, "static int64_t baga_wcache_get(int64_t wh, int64_t k) {\n");
    fprintf(out, "    baga_Arena *w = baga_world_a(wh, \"wcache_get\");\n");
    fprintf(out, "    pthread_mutex_lock(&w->wlock);\n");
    fprintf(out, "    int64_t out = 0; int bad = 0;\n");
    fprintf(out, "    int64_t s = baga_wc_slot(w->wcache, k);\n");
    fprintf(out, "    if (s >= 0) { if (w->wcache->kd[s] != 1) bad = 1; else out = w->wcache->v[s]; }\n");
    fprintf(out, "    pthread_mutex_unlock(&w->wlock);\n");
    fprintf(out, "    if (bad) baga_world_bad(\"wcache_get\", \"стойността не е i64\", (long long)k, 0);\n");
    fprintf(out, "    return out;\n");
    fprintf(out, "}\n");
    /* W2.15: атомарен read-modify-write за броячи върху замразен свят.
     * Две отделни get+set от различни нишки губят инкременти; тук
     * целият цикъл е под един wlock. */
    fprintf(out, "static int64_t baga_wcache_add(int64_t wh, int64_t k, int64_t d) {\n");
    fprintf(out, "    baga_Arena *w = baga_world_a(wh, \"wcache_add\");\n");
    fprintf(out, "    pthread_mutex_lock(&w->wlock);\n");
    fprintf(out, "    if (!w->wcache) w->wcache = baga_wc_new();\n");
    fprintf(out, "    int64_t s = baga_wc_slot(w->wcache, k);\n");
    fprintf(out, "    int64_t out = 0;\n");
    fprintf(out, "    if (s < 0) { baga_wc_put(w->wcache, k, 1, d, NULL, 0); out = d; }\n");
    fprintf(out, "    else if (w->wcache->kd[s] != 1) { pthread_mutex_unlock(&w->wlock); baga_world_bad(\"wcache_add\", \"стойността не е i64\", (long long)k, 0); }\n");
    fprintf(out, "    else { out = w->wcache->v[s] + d; w->wcache->v[s] = out; }\n");
    fprintf(out, "    pthread_mutex_unlock(&w->wlock);\n");
    fprintf(out, "    return out;\n");
    fprintf(out, "}\n");
    fprintf(out, "static int64_t baga_wcache_has(int64_t wh, int64_t k) {\n");
    fprintf(out, "    baga_Arena *w = baga_world_a(wh, \"wcache_has\");\n");
    fprintf(out, "    pthread_mutex_lock(&w->wlock);\n");
    fprintf(out, "    int hit = baga_wc_find(w->wcache, k, NULL);\n");
    fprintf(out, "    pthread_mutex_unlock(&w->wlock);\n");
    fprintf(out, "    return hit ? 1 : 0;\n");
    fprintf(out, "}\n");
    fprintf(out, "static void baga_wcache_del(int64_t wh, int64_t k) {\n");
    fprintf(out, "    baga_Arena *w = baga_world_a(wh, \"wcache_del\");\n");
    fprintf(out, "    pthread_mutex_lock(&w->wlock);\n");
    fprintf(out, "    baga_wc_del(w->wcache, k);\n");
    fprintf(out, "    pthread_mutex_unlock(&w->wlock);\n");
    fprintf(out, "}\n");
    fprintf(out, "static void baga_wcache_clear(int64_t wh) {\n");
    fprintf(out, "    baga_Arena *w = baga_world_a(wh, \"wcache_clear\");\n");
    fprintf(out, "    pthread_mutex_lock(&w->wlock);\n");
    fprintf(out, "    baga_wc_free(w->wcache); w->wcache = NULL;\n");
    fprintf(out, "    pthread_mutex_unlock(&w->wlock);\n");
    fprintf(out, "}\n");
    fprintf(out, "static int64_t baga_wcache_len(int64_t wh) {\n");
    fprintf(out, "    baga_Arena *w = baga_world_a(wh, \"wcache_len\");\n");
    fprintf(out, "    pthread_mutex_lock(&w->wlock);\n");
    fprintf(out, "    int64_t n = w->wcache ? w->wcache->n : 0;\n");
    fprintf(out, "    pthread_mutex_unlock(&w->wlock);\n");
    fprintf(out, "    return n;\n");
    fprintf(out, "}\n");
    /* W2.13: str/bytes стойности. Записът копира в слота (собственост),
     * четенето връща КОПИЕ в bump арената: кешът се мутатира под mutex от
     * няколко нишки и заеман указател би увиснал при презапис от друга нишка
     * (за разлика от wvec_get_str, където регионът е замразен). Копието
     * преживява world_free — значи не е borrowed payload и не се тагва MEM-3. */
    fprintf(out, "static void baga_wcache_set_str(int64_t wh, int64_t k, const char *s) {\n");
    fprintf(out, "    baga_Arena *w = baga_world_a(wh, \"wcache_set_str\");\n");
    fprintf(out, "    int64_t n = baga_str_len(s);\n");
    fprintf(out, "    char *cp = malloc((size_t)n + 1);\n");
    fprintf(out, "    if (!cp) baga_wc_oom();\n");
    fprintf(out, "    memcpy(cp, s, (size_t)n + 1);\n");
    fprintf(out, "    pthread_mutex_lock(&w->wlock);\n");
    fprintf(out, "    if (!w->wcache) w->wcache = baga_wc_new();\n");
    fprintf(out, "    baga_wc_put(w->wcache, k, 2, 0, cp, n);\n");
    fprintf(out, "    pthread_mutex_unlock(&w->wlock);\n");
    fprintf(out, "}\n");
    fprintf(out, "static const char *baga_wcache_get_str(int64_t wh, int64_t k) {\n");
    fprintf(out, "    baga_Arena *w = baga_world_a(wh, \"wcache_get_str\");\n");
    fprintf(out, "    pthread_mutex_lock(&w->wlock);\n");
    fprintf(out, "    int64_t s = baga_wc_slot(w->wcache, k);\n");
    fprintf(out, "    if (s < 0) { pthread_mutex_unlock(&w->wlock); fprintf(stderr, \"baga: wcache_get_str: липсващ ключ %%lld\\n\", (long long)k); exit(1); }\n");
    fprintf(out, "    if (w->wcache->kd[s] != 2) { pthread_mutex_unlock(&w->wlock); baga_world_bad(\"wcache_get_str\", \"стойността не е низ\", (long long)k, 0); }\n");
    fprintf(out, "    int64_t n = w->wcache->plen[s];\n");
    fprintf(out, "    char *r = baga_alloc((size_t)n + 1);\n");
    fprintf(out, "    memcpy(r, w->wcache->p[s], (size_t)n + 1);\n");
    fprintf(out, "    pthread_mutex_unlock(&w->wlock);\n");
    fprintf(out, "    baga_set_slen(r, n);\n");
    fprintf(out, "    return r;\n");
    fprintf(out, "}\n");
    fprintf(out, "static void baga_wcache_set_bytes(int64_t wh, int64_t k, baga_bytes b) {\n");
    fprintf(out, "    baga_Arena *w = baga_world_a(wh, \"wcache_set_bytes\");\n");
    fprintf(out, "    if (b.len < 0 || (b.len > 0 && !b.data)) baga_world_bad(\"wcache_set_bytes\", \"невалиден bytes аргумент\", (long long)b.len, 0);\n");
    fprintf(out, "    char *cp = malloc(b.len > 0 ? (size_t)b.len : 1);\n");
    fprintf(out, "    if (!cp) baga_wc_oom();\n");
    fprintf(out, "    if (b.len > 0) memcpy(cp, b.data, (size_t)b.len);\n");
    fprintf(out, "    pthread_mutex_lock(&w->wlock);\n");
    fprintf(out, "    if (!w->wcache) w->wcache = baga_wc_new();\n");
    fprintf(out, "    baga_wc_put(w->wcache, k, 3, 0, cp, b.len);\n");
    fprintf(out, "    pthread_mutex_unlock(&w->wlock);\n");
    fprintf(out, "}\n");
    fprintf(out, "static baga_bytes baga_wcache_get_bytes(int64_t wh, int64_t k) {\n");
    fprintf(out, "    baga_Arena *w = baga_world_a(wh, \"wcache_get_bytes\");\n");
    fprintf(out, "    pthread_mutex_lock(&w->wlock);\n");
    fprintf(out, "    int64_t s = baga_wc_slot(w->wcache, k);\n");
    fprintf(out, "    if (s < 0) { pthread_mutex_unlock(&w->wlock); fprintf(stderr, \"baga: wcache_get_bytes: липсващ ключ %%lld\\n\", (long long)k); exit(1); }\n");
    fprintf(out, "    if (w->wcache->kd[s] != 3) { pthread_mutex_unlock(&w->wlock); baga_world_bad(\"wcache_get_bytes\", \"стойността не е bytes\", (long long)k, 0); }\n");
    fprintf(out, "    int64_t n = w->wcache->plen[s];\n");
    fprintf(out, "    baga_bytes r; r.len = n; r.data = baga_alloc((size_t)(n > 0 ? n : 1));\n");
    fprintf(out, "    if (n > 0) memcpy(r.data, w->wcache->p[s], (size_t)n);\n");
    fprintf(out, "    pthread_mutex_unlock(&w->wlock);\n");
    fprintf(out, "    return r;\n");
    fprintf(out, "}\n");
    /* W2.5: връзки между светове — насочена capability owner → target.
     * Правилото е „целта надживява връзката": free на свързан свят е чиста
     * грешка, free на owner освобождава връзките му (децата стават свободни).
     * Графът е ацикличен — проверява се при link, защото цикълът прави и двата
     * свята неосвободими. Връзки се добавят само към МУТАБЕЛЕН owner: графът
     * се строи преди freeze, затова таблицата не се пипа от две нишки. */
    fprintf(out, "static void baga_world_link(int64_t oh, int64_t th) {\n");
    fprintf(out, "    baga_Arena *o = baga_world_a(oh, \"world_link\");\n");
    fprintf(out, "    baga_Arena *t = baga_world_a(th, \"world_link\");\n");
    fprintf(out, "    if (o->frozen) { fprintf(stderr, \"baga: world_link върху замразен свят — графът се строи преди freeze\\n\"); exit(1); }\n");
    fprintf(out, "    if (o == t) { fprintf(stderr, \"baga: world_link към самия свят — цикъл\\n\"); exit(1); }\n");
    fprintf(out, "    int steps = 10000;\n");
    fprintf(out, "    if (baga_world_reaches(t, o, 0, &steps)) { fprintf(stderr, \"baga: world_link създава цикъл между светове (или графът е твърде голям за проверка)\\n\"); exit(1); }\n");
    fprintf(out, "    for (int64_t i = 0; i < o->n_links; i++) if (o->links[i] == t) return;   /* вече свързан */\n");
    fprintf(out, "    if (o->n_links == o->cap_links) {\n");
    fprintf(out, "        int64_t nc = o->cap_links ? o->cap_links * 2 : 4;\n");
    fprintf(out, "        o->links = realloc(o->links, (size_t)nc * sizeof(baga_Arena *));\n");
    fprintf(out, "        if (!o->links) { fprintf(stderr, \"baga: world_link: oom\\n\"); exit(1); }\n");
    fprintf(out, "        o->cap_links = nc;\n");
    fprintf(out, "    }\n");
    fprintf(out, "    o->links[o->n_links++] = t;\n");
    fprintf(out, "    t->linked_by++;\n");
    fprintf(out, "}\n");
    fprintf(out, "static void baga_world_unlink(int64_t oh, int64_t th) {\n");
    fprintf(out, "    baga_Arena *o = baga_world_a(oh, \"world_unlink\");\n");
    fprintf(out, "    baga_Arena *t = baga_world_a(th, \"world_unlink\");\n");
    fprintf(out, "    if (o->frozen) { fprintf(stderr, \"baga: world_unlink върху замразен свят — графът се строи преди freeze\\n\"); exit(1); }\n");
    fprintf(out, "    for (int64_t i = 0; i < o->n_links; i++) {\n");
    fprintf(out, "        if (o->links[i] != t) continue;\n");
    fprintf(out, "        o->links[i] = o->links[--o->n_links];   /* swap-remove: редът не е смислов */\n");
    fprintf(out, "        t->linked_by--;\n");
    fprintf(out, "        return;\n");
    fprintf(out, "    }\n");
    fprintf(out, "}\n");
    fprintf(out, "static int64_t baga_world_links(int64_t oh) {\n");
    fprintf(out, "    baga_Arena *o = baga_world_a(oh, \"world_links\");\n");
    fprintf(out, "    return o->n_links;\n");
    fprintf(out, "}\n");
    fprintf(out, "static int64_t baga_world_linked(int64_t oh, int64_t i) {\n");
    fprintf(out, "    baga_Arena *o = baga_world_a(oh, \"world_linked\");\n");
    fprintf(out, "    if (i < 0 || i >= o->n_links) baga_bounds_fail(\"world_linked\", i, o->n_links);\n");
    fprintf(out, "    return (int64_t)(intptr_t)o->links[i];\n");
    fprintf(out, "}\n");
    fprintf(out, "\n");
    /* ---- concurrency (!Par): OS threads + i64 channels (CSP) ---- */
    /* heap pair first — used by chan_recv2 and as worker context packing */
    fprintf(out, "static int64_t baga_cell2(int64_t a, int64_t b) {\n");
    fprintf(out, "    int64_t *p = (int64_t *)malloc(2 * sizeof(int64_t));\n");
    fprintf(out, "    if (!p) { fprintf(stderr, \"baga: cell2: oom\\n\"); exit(1); }\n");
    fprintf(out, "    p[0] = a; p[1] = b; return (int64_t)(intptr_t)p;\n");
    fprintf(out, "}\n");
    /* MEM-1: cell2 е ЕДИН malloc блок (2×i64) — не arena, истински free() */
    fprintf(out, "static void baga_cell2_free(int64_t h) { if (h) free((void *)(intptr_t)h); }\n");
    fprintf(out, "static int64_t baga_cell2_0(int64_t h) { return ((int64_t *)(intptr_t)h)[0]; }\n");
    fprintf(out, "static int64_t baga_cell2_1(int64_t h) { return ((int64_t *)(intptr_t)h)[1]; }\n");
    /* R51: unsafe str handle casts — zero-copy cross-thread hop (chan i64).
       Без --rc str е arena-bound (никога не се free-ва). RC1.4: под --rc
       handle-ът е immortal escape — retain, иначе scope release на
       източника обесва handle-а (boila_ts_test FPE: map_h(mus) →
       boila_shard_lock върху освободена карта с nb=0). */
    if (cg->rc)
        fprintf(out, "static int64_t baga_str_h(const char *s) { baga_rc_retain((void *)s); return (int64_t)(intptr_t)s; }\n");
    else
        fprintf(out, "static int64_t baga_str_h(const char *s) { return (int64_t)(intptr_t)s; }\n");
    fprintf(out, "static const char *baga_h_str(int64_t h) { return (const char *)(intptr_t)h; }\n");
    /* R66: box baga_bytes header on arena for chan hop; data ptr already arena. */
    fprintf(out, "static int64_t baga_bytes_h(baga_bytes b) {\n");
    if (cg->rc)
        fprintf(out, "    baga_rc_retain((void *)b.data);\n");
    fprintf(out, "    baga_bytes *p = (baga_bytes *)baga_alloc(sizeof(baga_bytes));\n");
    fprintf(out, "    p->data = b.data; p->len = b.len; return (int64_t)(intptr_t)p;\n");
    fprintf(out, "}\n");
    fprintf(out, "static baga_bytes baga_h_bytes(int64_t h) {\n");
    fprintf(out, "    baga_bytes z; z.data = (unsigned char *)\"\"; z.len = 0;\n");
    fprintf(out, "    if (!h) return z; return *(baga_bytes *)(intptr_t)h;\n");
    fprintf(out, "}\n");
    /* R55: unsafe map handle casts — shared map through a go_bg i64 ctx. */
    if (cg->rc)
        fprintf(out, "static int64_t baga_map_h(baga_Map *m) { baga_rc_retain((void *)m); return (int64_t)(intptr_t)m; }\n");
    else
        fprintf(out, "static int64_t baga_map_h(baga_Map *m) { return (int64_t)(intptr_t)m; }\n");
    fprintf(out, "static baga_Map *baga_h_map(int64_t h) { return (baga_Map *)(intptr_t)h; }\n");
    fprintf(out, "typedef int64_t (*baga_par_fn)(int64_t);\n");
    fprintf(out, "typedef struct {\n");
    fprintf(out, "    uint32_t magic;\n");
    fprintf(out, "    baga_par_fn fn; int64_t arg; int64_t result; pthread_t th;\n");
    fprintf(out, "    int joined; int detached;\n");
    fprintf(out, "} baga_JoinHandle;\n");
    fprintf(out, "#define BAGA_HANDLE_MAGIC 0xBA6A0001u\n");
    fprintf(out, "static void *baga_par_trampoline(void *p) {\n");
    fprintf(out, "    baga_JoinHandle *h = (baga_JoinHandle *)p;\n");
    fprintf(out, "    h->result = h->fn(h->arg);\n");
    fprintf(out, "    /* 0=joinable, 1=detach requested, 2=finished (joinable) */\n");
    fprintf(out, "    int old = __sync_lock_test_and_set(&h->detached, 2);\n");
    fprintf(out, "    if (old == 1) free(h); /* parent already detached — we free */\n");
    fprintf(out, "    return NULL;\n");
    fprintf(out, "}\n");
    fprintf(out, "static int64_t baga_go(baga_par_fn fn, int64_t arg) {\n");
    fprintf(out, "    baga_JoinHandle *h = (baga_JoinHandle *)calloc(1, sizeof(baga_JoinHandle));\n");
    fprintf(out, "    if (!h) { fprintf(stderr, \"baga: go: out of memory\\n\"); exit(1); }\n");
    fprintf(out, "    h->fn = fn; h->arg = arg; h->joined = 0; h->detached = 0;\n");
    fprintf(out, "    h->magic = BAGA_HANDLE_MAGIC;\n");
    fprintf(out, "    if (pthread_create(&h->th, NULL, baga_par_trampoline, h) != 0) {\n");
    fprintf(out, "        fprintf(stderr, \"baga: go: pthread_create failed\\n\"); exit(1);\n");
    fprintf(out, "    }\n");
    fprintf(out, "    return (int64_t)(intptr_t)h;\n");
    fprintf(out, "}\n");
    /* Fire-and-forget from the start (cloud accept loops). Returns 0; no join. */
    fprintf(out, "static int64_t baga_go_bg(baga_par_fn fn, int64_t arg) {\n");
    fprintf(out, "    baga_JoinHandle *h = (baga_JoinHandle *)calloc(1, sizeof(baga_JoinHandle));\n");
    fprintf(out, "    if (!h) { fprintf(stderr, \"baga: go_bg: out of memory\\n\"); exit(1); }\n");
    fprintf(out, "    h->fn = fn; h->arg = arg; h->joined = 0; h->detached = 1;\n");
    fprintf(out, "    h->magic = BAGA_HANDLE_MAGIC;\n");
    fprintf(out, "    pthread_attr_t attr; pthread_attr_init(&attr);\n");
    fprintf(out, "    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);\n");
    fprintf(out, "    if (pthread_create(&h->th, &attr, baga_par_trampoline, h) != 0) {\n");
    fprintf(out, "        fprintf(stderr, \"baga: go_bg: pthread_create failed\\n\"); exit(1);\n");
    fprintf(out, "    }\n");
    fprintf(out, "    pthread_attr_destroy(&attr);\n");
    fprintf(out, "    return 0;\n");
    fprintf(out, "}\n");
    fprintf(out, "static int64_t baga_join(int64_t handle) {\n");
    fprintf(out, "    baga_JoinHandle *h = (baga_JoinHandle *)(intptr_t)handle;\n");
    fprintf(out, "    if (!h) return 0;\n");
    fprintf(out, "    if ((uintptr_t)h < 4096) {\n");
    fprintf(out, "        fprintf(stderr, \"baga: join: невалиден handle\\n\"); exit(1);\n");
    fprintf(out, "    }\n");
    fprintf(out, "    if (h->magic != BAGA_HANDLE_MAGIC) {\n");
    fprintf(out, "        fprintf(stderr, \"baga: join: невалиден handle\\n\"); exit(1);\n");
    fprintf(out, "    }\n");
    /* W2.3 (TSan находка): `detached` се пише АТОМНО от trampoline-а, значи и
     * четенето тук трябва да е атомно — смесеният достъп е data race по C11
     * (безвреден на x86, но ThreadSanitizer го брои и моделът го забранява). */
    fprintf(out, "    if (__atomic_load_n(&h->detached, __ATOMIC_ACQUIRE) == 1) {\n");
    fprintf(out, "        fprintf(stderr, \"baga: join: handle detached\\n\"); exit(1);\n");
    fprintf(out, "    }\n");
    fprintf(out, "    if (!h->joined) { pthread_join(h->th, NULL); h->joined = 1; }\n");
    fprintf(out, "    int64_t r = h->result; free(h); return r;\n");
    fprintf(out, "}\n");
    /* detach joinable handle: fire-and-forget. Race-safe with trampoline. */
    fprintf(out, "static int64_t baga_detach(int64_t handle) {\n");
    fprintf(out, "    baga_JoinHandle *h = (baga_JoinHandle *)(intptr_t)handle;\n");
    fprintf(out, "    if (!h || h->magic != BAGA_HANDLE_MAGIC || h->joined) return -1;\n");
    fprintf(out, "    int old = __sync_lock_test_and_set(&h->detached, 1);\n");
    fprintf(out, "    if (old == 2) { free(h); return 0; } /* already finished */\n");
    fprintf(out, "    if (old == 1) return 0; /* double detach */\n");
    fprintf(out, "    pthread_detach(h->th);\n");
    fprintf(out, "    return 0;\n");
    fprintf(out, "}\n");
    fprintf(out, "typedef struct {\n");
    fprintf(out, "    int64_t *buf; int64_t cap, len, head; int closed;\n");
    fprintf(out, "    pthread_mutex_t mu; pthread_cond_t not_empty, not_full;\n");
    fprintf(out, "} baga_Chan;\n");
    fprintf(out, "static int64_t baga_chan_new(int64_t cap) {\n");
    fprintf(out, "    if (cap < 1) cap = 1; /* M1: min buffer 1 (rendezvous = M2) */\n");
    fprintf(out, "    baga_Chan *c = (baga_Chan *)calloc(1, sizeof(baga_Chan));\n");
    fprintf(out, "    if (!c) { fprintf(stderr, \"baga: chan_new: oom\\n\"); exit(1); }\n");
    fprintf(out, "    c->cap = cap; c->buf = (int64_t *)malloc((size_t)cap * sizeof(int64_t));\n");
    fprintf(out, "    if (!c->buf) { fprintf(stderr, \"baga: chan_new: oom\\n\"); exit(1); }\n");
    fprintf(out, "    pthread_mutex_init(&c->mu, NULL);\n");
    fprintf(out, "    pthread_cond_init(&c->not_empty, NULL);\n");
    fprintf(out, "    pthread_cond_init(&c->not_full, NULL);\n");
    fprintf(out, "    return (int64_t)(intptr_t)c;\n");
    fprintf(out, "}\n");
    fprintf(out, "static int64_t baga_chan_send(int64_t ch, int64_t v) {\n");
    fprintf(out, "    baga_Chan *c = (baga_Chan *)(intptr_t)ch;\n");
    fprintf(out, "    if (!c) return -1;\n");
    fprintf(out, "    pthread_mutex_lock(&c->mu);\n");
    fprintf(out, "    while (c->len == c->cap && !c->closed) pthread_cond_wait(&c->not_full, &c->mu);\n");
    fprintf(out, "    if (c->closed) { pthread_mutex_unlock(&c->mu); return -1; }\n");
    fprintf(out, "    int64_t i = (c->head + c->len) %% c->cap;\n");
    fprintf(out, "    c->buf[i] = v; c->len++;\n");
    fprintf(out, "    pthread_cond_signal(&c->not_empty);\n");
    fprintf(out, "    pthread_mutex_unlock(&c->mu);\n");
    fprintf(out, "    return 0;\n");
    fprintf(out, "}\n");
    fprintf(out, "static int64_t baga_chan_recv(int64_t ch) {\n");
    fprintf(out, "    baga_Chan *c = (baga_Chan *)(intptr_t)ch;\n");
    fprintf(out, "    if (!c) return 0;\n");
    fprintf(out, "    pthread_mutex_lock(&c->mu);\n");
    fprintf(out, "    while (c->len == 0 && !c->closed) pthread_cond_wait(&c->not_empty, &c->mu);\n");
    fprintf(out, "    if (c->len == 0) { /* closed + empty */ pthread_mutex_unlock(&c->mu); return 0; }\n");
    fprintf(out, "    int64_t v = c->buf[c->head];\n");
    fprintf(out, "    c->head = (c->head + 1) %% c->cap; c->len--;\n");
    fprintf(out, "    pthread_cond_signal(&c->not_full);\n");
    fprintf(out, "    pthread_mutex_unlock(&c->mu);\n");
    fprintf(out, "    return v;\n");
    fprintf(out, "}\n");
    /* returns cell2(ok, value): ok=1 got value, ok=0 closed+empty (value=0) */
    fprintf(out, "static int64_t baga_chan_recv2(int64_t ch) {\n");
    fprintf(out, "    baga_Chan *c = (baga_Chan *)(intptr_t)ch;\n");
    fprintf(out, "    if (!c) return baga_cell2(0, 0);\n");
    fprintf(out, "    pthread_mutex_lock(&c->mu);\n");
    fprintf(out, "    while (c->len == 0 && !c->closed) pthread_cond_wait(&c->not_empty, &c->mu);\n");
    fprintf(out, "    if (c->len == 0) { pthread_mutex_unlock(&c->mu); return baga_cell2(0, 0); }\n");
    fprintf(out, "    int64_t v = c->buf[c->head];\n");
    fprintf(out, "    c->head = (c->head + 1) %% c->cap; c->len--;\n");
    fprintf(out, "    pthread_cond_signal(&c->not_full);\n");
    fprintf(out, "    pthread_mutex_unlock(&c->mu);\n");
    fprintf(out, "    return baga_cell2(1, v);\n");
    fprintf(out, "}\n");
    /* non-blocking: cell2(1,v)=ok, cell2(0,0)=empty, cell2(2,0)=closed empty */
    fprintf(out, "static int64_t baga_chan_try_recv(int64_t ch) {\n");
    fprintf(out, "    baga_Chan *c = (baga_Chan *)(intptr_t)ch;\n");
    fprintf(out, "    if (!c) return baga_cell2(2, 0);\n");
    fprintf(out, "    pthread_mutex_lock(&c->mu);\n");
    fprintf(out, "    if (c->len == 0) {\n");
    fprintf(out, "        int st = c->closed ? 2 : 0;\n");
    fprintf(out, "        pthread_mutex_unlock(&c->mu);\n");
    fprintf(out, "        return baga_cell2(st, 0);\n");
    fprintf(out, "    }\n");
    fprintf(out, "    int64_t v = c->buf[c->head];\n");
    fprintf(out, "    c->head = (c->head + 1) %% c->cap; c->len--;\n");
    fprintf(out, "    pthread_cond_signal(&c->not_full);\n");
    fprintf(out, "    pthread_mutex_unlock(&c->mu);\n");
    fprintf(out, "    return baga_cell2(1, v);\n");
    fprintf(out, "}\n");
    /* timed: cell2(1,v)=ok, cell2(0,0)=timeout, cell2(2,0)=closed empty */
    fprintf(out, "static int64_t baga_chan_recv_timeout(int64_t ch, int64_t ms) {\n");
    fprintf(out, "    baga_Chan *c = (baga_Chan *)(intptr_t)ch;\n");
    fprintf(out, "    if (!c) return baga_cell2(2, 0);\n");
    fprintf(out, "    if (ms < 0) ms = 0;\n");
    fprintf(out, "    struct timespec abs; clock_gettime(CLOCK_REALTIME, &abs);\n");
    fprintf(out, "    abs.tv_sec += ms / 1000;\n");
    fprintf(out, "    abs.tv_nsec += (ms %% 1000) * 1000000L;\n");
    fprintf(out, "    if (abs.tv_nsec >= 1000000000L) { abs.tv_sec++; abs.tv_nsec -= 1000000000L; }\n");
    fprintf(out, "    pthread_mutex_lock(&c->mu);\n");
    fprintf(out, "    while (c->len == 0 && !c->closed) {\n");
    fprintf(out, "        int rc = pthread_cond_timedwait(&c->not_empty, &c->mu, &abs);\n");
    fprintf(out, "        if (rc == ETIMEDOUT) { pthread_mutex_unlock(&c->mu); return baga_cell2(0, 0); }\n");
    fprintf(out, "    }\n");
    fprintf(out, "    if (c->len == 0) { pthread_mutex_unlock(&c->mu); return baga_cell2(2, 0); }\n");
    fprintf(out, "    int64_t v = c->buf[c->head];\n");
    fprintf(out, "    c->head = (c->head + 1) %% c->cap; c->len--;\n");
    fprintf(out, "    pthread_cond_signal(&c->not_full);\n");
    fprintf(out, "    pthread_mutex_unlock(&c->mu);\n");
    fprintf(out, "    return baga_cell2(1, v);\n");
    fprintf(out, "}\n");
    fprintf(out, "static int64_t baga_sleep_ms(int64_t ms) {\n");
    fprintf(out, "    if (ms <= 0) return 0;\n");
    fprintf(out, "    struct timespec ts; ts.tv_sec = ms / 1000; ts.tv_nsec = (ms %% 1000) * 1000000L;\n");
    fprintf(out, "    while (nanosleep(&ts, &ts) == -1 && errno == EINTR) {}\n");
    fprintf(out, "    return 0;\n");
    fprintf(out, "}\n");
    /* C1: process-global signal slot for graceful shutdown (SIGTERM/SIGINT). */
    fprintf(out, "static volatile sig_atomic_t baga_sig_seen = 0;\n");
    fprintf(out, "static void baga_sig_handler(int s) { baga_sig_seen = s; }\n");
    fprintf(out, "static int64_t baga_signal_watch(int64_t sig) {\n");
    fprintf(out, "    if (sig <= 0 || sig >= 64) return -1;\n");
    fprintf(out, "    struct sigaction sa; memset(&sa, 0, sizeof sa);\n");
    fprintf(out, "    sa.sa_handler = baga_sig_handler;\n");
    fprintf(out, "    sigemptyset(&sa.sa_mask);\n");
    fprintf(out, "    if (sigaction((int)sig, &sa, NULL) != 0) return -1;\n");
    fprintf(out, "    return 0;\n");
    fprintf(out, "}\n");
    fprintf(out, "static int64_t baga_signal_check(void) { return (int64_t)baga_sig_seen; }\n");
    fprintf(out, "static int64_t baga_signal_clear(void) {\n");
    fprintf(out, "    int64_t v = (int64_t)baga_sig_seen; baga_sig_seen = 0; return v;\n");
    fprintf(out, "}\n");
    fprintf(out, "static int64_t baga_signal_wait(int64_t ms) {\n");
    fprintf(out, "    if (baga_sig_seen) return (int64_t)baga_sig_seen;\n");
    fprintf(out, "    if (ms == 0) return 0;\n");
    fprintf(out, "    if (ms < 0) {\n");
    fprintf(out, "        while (!baga_sig_seen) {\n");
    fprintf(out, "            struct timespec ts; ts.tv_sec = 0; ts.tv_nsec = 50000000L;\n");
    fprintf(out, "            nanosleep(&ts, NULL);\n");
    fprintf(out, "        }\n");
    fprintf(out, "        return (int64_t)baga_sig_seen;\n");
    fprintf(out, "    }\n");
    fprintf(out, "    int64_t left = ms;\n");
    fprintf(out, "    while (left > 0) {\n");
    fprintf(out, "        if (baga_sig_seen) return (int64_t)baga_sig_seen;\n");
    fprintf(out, "        int64_t step = left > 50 ? 50 : left;\n");
    fprintf(out, "        struct timespec ts; ts.tv_sec = step / 1000;\n");
    fprintf(out, "        ts.tv_nsec = (step %% 1000) * 1000000L;\n");
    fprintf(out, "        nanosleep(&ts, NULL);\n");
    fprintf(out, "        left -= step;\n");
    fprintf(out, "    }\n");
    fprintf(out, "    return baga_sig_seen ? (int64_t)baga_sig_seen : 0;\n");
    fprintf(out, "}\n");
    fprintf(out, "static int64_t baga_signal_raise(int64_t sig) {\n");
    fprintf(out, "    return raise((int)sig) == 0 ? 0 : -1;\n");
    fprintf(out, "}\n");
    /* non-blocking select over two channels.
     * cell2(which, value): which=0|1 got value; which=2 neither ready;
     * which=3 both closed (and empty). Fair: prefers the emptier buffer first,
     * ties break to c0 then c1. */
    fprintf(out, "static int64_t baga_chan_select2(int64_t c0, int64_t c1) {\n");
    fprintf(out, "    baga_Chan *a = (baga_Chan *)(intptr_t)c0;\n");
    fprintf(out, "    baga_Chan *b = (baga_Chan *)(intptr_t)c1;\n");
    fprintf(out, "    if (!a && !b) return baga_cell2(3, 0);\n");
    fprintf(out, "    int64_t la = 0, lb = 0; int ca = 1, cb = 1;\n");
    fprintf(out, "    if (a) { pthread_mutex_lock(&a->mu); la = a->len; ca = a->closed; pthread_mutex_unlock(&a->mu); }\n");
    fprintf(out, "    if (b) { pthread_mutex_lock(&b->mu); lb = b->len; cb = b->closed; pthread_mutex_unlock(&b->mu); }\n");
    fprintf(out, "    if (la == 0 && lb == 0) {\n");
    fprintf(out, "        if ((!a || ca) && (!b || cb)) return baga_cell2(3, 0);\n");
    fprintf(out, "        return baga_cell2(2, 0);\n");
    fprintf(out, "    }\n");
    fprintf(out, "    int prefer0 = (la >= lb); /* take from fuller first (less likely to starve) */\n");
    fprintf(out, "    if (prefer0 && la > 0) {\n");
    fprintf(out, "        int64_t pr = baga_chan_try_recv(c0);\n");
    fprintf(out, "        if (baga_cell2_0(pr) == 1) return baga_cell2(0, baga_cell2_1(pr));\n");
    fprintf(out, "    }\n");
    fprintf(out, "    if (lb > 0) {\n");
    fprintf(out, "        int64_t pr = baga_chan_try_recv(c1);\n");
    fprintf(out, "        if (baga_cell2_0(pr) == 1) return baga_cell2(1, baga_cell2_1(pr));\n");
    fprintf(out, "    }\n");
    fprintf(out, "    if (la > 0) {\n");
    fprintf(out, "        int64_t pr = baga_chan_try_recv(c0);\n");
    fprintf(out, "        if (baga_cell2_0(pr) == 1) return baga_cell2(0, baga_cell2_1(pr));\n");
    fprintf(out, "    }\n");
    fprintf(out, "    return baga_cell2(2, 0);\n");
    fprintf(out, "}\n");
    /* Blocking select: wait until a value is ready or both channels are closed.
     * Alternates timed waits on each channel (by pointer order) so either side
     * can wake the waiter within ~5ms. Returns same codes as chan_select2. */
    fprintf(out, "static int64_t baga_chan_select2_wait(int64_t c0, int64_t c1) {\n");
    fprintf(out, "    int flip = 0;\n");
    fprintf(out, "    for (;;) {\n");
    fprintf(out, "        int64_t r = baga_chan_select2(c0, c1);\n");
    fprintf(out, "        int64_t w = baga_cell2_0(r);\n");
    fprintf(out, "        if (w != 2) return r;\n");
    fprintf(out, "        baga_Chan *a = (baga_Chan *)(intptr_t)c0;\n");
    fprintf(out, "        baga_Chan *b = (baga_Chan *)(intptr_t)c1;\n");
    fprintf(out, "        baga_Chan *wa = NULL, *wb = NULL;\n");
    fprintf(out, "        if (a && b) {\n");
    fprintf(out, "            if ((uintptr_t)a < (uintptr_t)b) { wa = a; wb = b; }\n");
    fprintf(out, "            else { wa = b; wb = a; }\n");
    fprintf(out, "        } else { wa = a ? a : b; }\n");
    fprintf(out, "        baga_Chan *wait = (flip && wb) ? wb : wa;\n");
    fprintf(out, "        flip = !flip;\n");
    fprintf(out, "        if (!wait) return baga_cell2(3, 0);\n");
    fprintf(out, "        struct timespec abs; clock_gettime(CLOCK_REALTIME, &abs);\n");
    fprintf(out, "        abs.tv_nsec += 5000000L; /* 5ms */\n");
    fprintf(out, "        if (abs.tv_nsec >= 1000000000L) { abs.tv_sec++; abs.tv_nsec -= 1000000000L; }\n");
    fprintf(out, "        pthread_mutex_lock(&wait->mu);\n");
    fprintf(out, "        if (wait->len == 0 && !wait->closed)\n");
    fprintf(out, "            pthread_cond_timedwait(&wait->not_empty, &wait->mu, &abs);\n");
    fprintf(out, "        pthread_mutex_unlock(&wait->mu);\n");
    fprintf(out, "    }\n");
    fprintf(out, "}\n");
    /* Timed select: like select2_wait but give up after ms (return which=2). */
    fprintf(out, "static int64_t baga_chan_select2_timeout(int64_t c0, int64_t c1, int64_t ms) {\n");
    fprintf(out, "    if (ms < 0) ms = 0;\n");
    fprintf(out, "    struct timespec deadline; clock_gettime(CLOCK_REALTIME, &deadline);\n");
    fprintf(out, "    deadline.tv_sec += ms / 1000;\n");
    fprintf(out, "    deadline.tv_nsec += (ms %% 1000) * 1000000L;\n");
    fprintf(out, "    if (deadline.tv_nsec >= 1000000000L) { deadline.tv_sec++; deadline.tv_nsec -= 1000000000L; }\n");
    fprintf(out, "    int flip = 0;\n");
    fprintf(out, "    for (;;) {\n");
    fprintf(out, "        int64_t r = baga_chan_select2(c0, c1);\n");
    fprintf(out, "        int64_t w = baga_cell2_0(r);\n");
    fprintf(out, "        if (w != 2) return r;\n");
    fprintf(out, "        struct timespec now; clock_gettime(CLOCK_REALTIME, &now);\n");
    fprintf(out, "        if (now.tv_sec > deadline.tv_sec ||\n");
    fprintf(out, "            (now.tv_sec == deadline.tv_sec && now.tv_nsec >= deadline.tv_nsec))\n");
    fprintf(out, "            return baga_cell2(2, 0);\n");
    fprintf(out, "        baga_Chan *a = (baga_Chan *)(intptr_t)c0;\n");
    fprintf(out, "        baga_Chan *b = (baga_Chan *)(intptr_t)c1;\n");
    fprintf(out, "        baga_Chan *wa = NULL, *wb = NULL;\n");
    fprintf(out, "        if (a && b) {\n");
    fprintf(out, "            if ((uintptr_t)a < (uintptr_t)b) { wa = a; wb = b; }\n");
    fprintf(out, "            else { wa = b; wb = a; }\n");
    fprintf(out, "        } else { wa = a ? a : b; }\n");
    fprintf(out, "        baga_Chan *wait = (flip && wb) ? wb : wa;\n");
    fprintf(out, "        flip = !flip;\n");
    fprintf(out, "        if (!wait) return baga_cell2(3, 0);\n");
    fprintf(out, "        pthread_mutex_lock(&wait->mu);\n");
    fprintf(out, "        if (wait->len == 0 && !wait->closed)\n");
    fprintf(out, "            pthread_cond_timedwait(&wait->not_empty, &wait->mu, &deadline);\n");
    fprintf(out, "        pthread_mutex_unlock(&wait->mu);\n");
    fprintf(out, "    }\n");
    fprintf(out, "}\n");
    fprintf(out, "static int64_t baga_chan_close(int64_t ch) {\n");
    fprintf(out, "    baga_Chan *c = (baga_Chan *)(intptr_t)ch;\n");
    fprintf(out, "    if (!c) return -1;\n");
    fprintf(out, "    pthread_mutex_lock(&c->mu);\n");
    fprintf(out, "    c->closed = 1;\n");
    fprintf(out, "    pthread_cond_broadcast(&c->not_empty);\n");
    fprintf(out, "    pthread_cond_broadcast(&c->not_full);\n");
    fprintf(out, "    pthread_mutex_unlock(&c->mu);\n");
    fprintf(out, "    return 0;\n");
    fprintf(out, "}\n");
    fprintf(out, "static int64_t baga_chan_len(int64_t ch) {\n");
    fprintf(out, "    baga_Chan *c = (baga_Chan *)(intptr_t)ch;\n");
    fprintf(out, "    if (!c) return 0;\n");
    fprintf(out, "    pthread_mutex_lock(&c->mu);\n");
    fprintf(out, "    int64_t n = c->len;\n");
    fprintf(out, "    pthread_mutex_unlock(&c->mu);\n");
    fprintf(out, "    return n;\n");
    fprintf(out, "}\n");
    /* mutex — opaque i64 handle с double-lock детекция */
    fprintf(out, "typedef struct { pthread_mutex_t mu; _Atomic pthread_t owner; _Atomic int locked; void *bt[16]; int btn; } baga_Mutex;\n");
    fprintf(out, "static int64_t baga_mutex_new(void) {\n");
    fprintf(out, "    baga_Mutex *m = (baga_Mutex *)calloc(1, sizeof(baga_Mutex));\n");
    fprintf(out, "    if (!m) { fprintf(stderr, \"baga: mutex_new: oom\\n\"); exit(1); }\n");
    fprintf(out, "    pthread_mutex_init(&m->mu, NULL);\n");
    fprintf(out, "    atomic_init(&m->owner, pthread_self()); atomic_init(&m->locked, 0);\n");
    fprintf(out, "    return (int64_t)(intptr_t)m;\n");
    fprintf(out, "}\n");
    fprintf(out, "static int64_t baga_mutex_lock(int64_t h) {\n");
    fprintf(out, "    baga_Mutex *m = (baga_Mutex *)(intptr_t)h;\n");
    fprintf(out, "    if (!m) return -1;\n");
    fprintf(out, "    if (atomic_load_explicit(&m->locked, memory_order_acquire) &&\n");
    fprintf(out, "        pthread_equal(atomic_load_explicit(&m->owner, memory_order_relaxed), pthread_self())) {\n");
    fprintf(out, "        /* Re-entrant same-thread lock: НЕ abort-ваме целия сървър.\n");
    fprintf(out, "           Възниква при lock leak от effect-raise (`?`) в заключен регион —\n");
    fprintf(out, "           нишката вече държи mutex-а, значи е вътре в критичния регион;\n");
    fprintf(out, "           пропускаме pthread_lock (пази данните), а нормалният unlock\n");
    fprintf(out, "           после освобождава mutex-а и излекува теча. Печатим предупреждение. */\n");
    fprintf(out, "        void *bt[16]; int btn = backtrace(bt, 16);\n");
    fprintf(out, "        fprintf(stderr, \"baga: mutex_lock: same-thread re-lock (lock leak) — продължаваме; първото заключване беше от %%p\\n\", m->bt[0]);\n");
    fprintf(out, "        backtrace_symbols_fd(bt, btn, 2);\n");
    fprintf(out, "        return 0;\n");
    fprintf(out, "    }\n");
    fprintf(out, "    int rc = pthread_mutex_lock(&m->mu);\n");
    fprintf(out, "    m->bt[0] = __builtin_return_address(0);\n");
    fprintf(out, "    atomic_store_explicit(&m->owner, pthread_self(), memory_order_relaxed);\n");
    fprintf(out, "    atomic_store_explicit(&m->locked, 1, memory_order_release);\n");
    fprintf(out, "    m->btn = 1;\n");
    fprintf(out, "    return (int64_t)rc;\n");
    fprintf(out, "}\n");
    fprintf(out, "static int64_t baga_mutex_unlock(int64_t h) {\n");
    fprintf(out, "    baga_Mutex *m = (baga_Mutex *)(intptr_t)h;\n");
    fprintf(out, "    if (!m) return -1;\n");
    fprintf(out, "    atomic_store_explicit(&m->locked, 0, memory_order_release);\n");
    fprintf(out, "    return (int64_t)pthread_mutex_unlock(&m->mu);\n");
    fprintf(out, "}\n");
    /* Bounded worker pool: pool_map(fn, Vec<i64>, nworkers) -> Vec<i64> */
    fprintf(out, "typedef struct {\n");
    fprintf(out, "    baga_par_fn fn; baga_Vec *in; int64_t jobs; int64_t results;\n");
    fprintf(out, "} baga_PoolCtx;\n");
    fprintf(out, "static int64_t baga_pool_worker(int64_t ctx_h) {\n");
    fprintf(out, "    baga_PoolCtx *ctx = (baga_PoolCtx *)(intptr_t)ctx_h;\n");
    fprintf(out, "    for (;;) {\n");
    fprintf(out, "        int64_t pr = baga_chan_recv2(ctx->jobs);\n");
    fprintf(out, "        if (baga_cell2_0(pr) == 0) break; /* closed + empty */\n");
    fprintf(out, "        int64_t idx = baga_cell2_1(pr);\n");
    fprintf(out, "        int64_t arg = baga_vec_get_i64(ctx->in, idx);\n");
    fprintf(out, "        int64_t r = ctx->fn(arg);\n");
    fprintf(out, "        baga_chan_send(ctx->results, baga_cell2(idx, r));\n");
    fprintf(out, "    }\n");
    fprintf(out, "    return 0;\n");
    fprintf(out, "}\n");
    fprintf(out, "static baga_Vec *baga_pool_map(baga_par_fn fn, baga_Vec *in, int64_t nw) {\n");
    fprintf(out, "    int64_t n = baga_vec_len(in);\n");
    fprintf(out, "    baga_Vec *out = baga_vec_new();\n");
    fprintf(out, "    if (n <= 0) return out;\n");
    fprintf(out, "    for (int64_t i = 0; i < n; i++) baga_vec_push_i64(out, 0);\n");
    fprintf(out, "    if (nw < 1) nw = 1;\n");
    fprintf(out, "    if (nw > n) nw = n;\n");
    fprintf(out, "    int64_t jobs = baga_chan_new(n);\n");
    fprintf(out, "    int64_t results = baga_chan_new(n);\n");
    fprintf(out, "    baga_PoolCtx *ctx = (baga_PoolCtx *)calloc(1, sizeof(baga_PoolCtx));\n");
    fprintf(out, "    if (!ctx) { fprintf(stderr, \"baga: pool_map: oom\\n\"); exit(1); }\n");
    fprintf(out, "    ctx->fn = fn; ctx->in = in; ctx->jobs = jobs; ctx->results = results;\n");
    fprintf(out, "    int64_t *hs = (int64_t *)malloc((size_t)nw * sizeof(int64_t));\n");
    fprintf(out, "    if (!hs) { fprintf(stderr, \"baga: pool_map: oom\\n\"); exit(1); }\n");
    fprintf(out, "    for (int64_t w = 0; w < nw; w++)\n");
    fprintf(out, "        hs[w] = baga_go(baga_pool_worker, (int64_t)(intptr_t)ctx);\n");
    fprintf(out, "    for (int64_t i = 0; i < n; i++) baga_chan_send(jobs, i);\n");
    fprintf(out, "    baga_chan_close(jobs);\n");
    fprintf(out, "    for (int64_t i = 0; i < n; i++) {\n");
    fprintf(out, "        int64_t pair = baga_chan_recv(results);\n");
    fprintf(out, "        int64_t idx = baga_cell2_0(pair);\n");
    fprintf(out, "        int64_t r = baga_cell2_1(pair);\n");
    fprintf(out, "        if (idx >= 0 && idx < n) baga_vec_set_i64(out, idx, r);\n");
    fprintf(out, "    }\n");
    fprintf(out, "    for (int64_t w = 0; w < nw; w++) baga_join(hs[w]);\n");
    fprintf(out, "    free(hs); free(ctx);\n");
    fprintf(out, "    return out;\n");
    fprintf(out, "}\n");
    fprintf(out, "\n");
    /* Маркер за границата runtime → потребителски код. Makefile целта
     * world-rt изрязва всичко преди него, за да построи
     * lib/libbaga_world_rt.so от СЪЩИЯ runtime, който C бекендът вгражда
     * (без втори, разминаващ се port). */
    fprintf(out, "/* BAGA_RUNTIME_END */\n");

    /* plain enums (i64 tags) — no payload deps */
    for (int i = 0; i < program->items.len; i++) {
        Node *item = program->items.data[i];
        if (item->kind != NODE_ENUM || is_sum_enum_item(item)) continue;
        char *em = mangle_name(item->enum_name);
        fprintf(out, "typedef enum {\n");
        for (int j = 0; j < item->n_variants; j++) {
            char *vm = mangle_name(item->enum_variants[j]);
            fprintf(out, "    %s_%s = %d,\n", em, vm, j);
            free(vm);
        }
        fprintf(out, "} %s;\n\n", em);
        free(em);
    }

    /* structs + sum enums in dependency order (L3 fields + struct payloads) */
    emit_structs_and_sum_enums(cg, program);

    /* forward declarations */
    emit_forward_decls(cg, program);

    /* function definitions (L5: през memstream — env struct-овете и
     * wrapper-ите на ламбдите/функциите (lambda_out) трябва да стоят ПРЕДИ
     * телата в изхода, без AST pre-pass) */
    char *fbuf = NULL, *lbuf = NULL;
    size_t fsize = 0, lsize = 0;
    FILE *fa = open_memstream(&fbuf, &fsize);
    cg->lambda_out = open_memstream(&lbuf, &lsize);
    FILE *saved_out = cg->out;
    cg->out = fa;
    for (int i = 0; i < program->items.len; i++) {
        Node *item = program->items.data[i];
        if (item->kind == NODE_FN && item->fn_body) {
            /* M21: generic fn — вариант per инстанция */
            if (item->n_type_params > 0) {
                for (int k = 0; k < item->inst_count; k++) {
                    char nm[512];
                    snprintf(nm, sizeof nm, "%s__i%d", item->fn_name, k);
                    if (cg->chk) checker_recheck_inst(cg->chk, item, k);
                    cg->gen_fn = item; cg->gen_inst = k; cg->gen_emit_name = nm;
                    emit_fn(cg, item);
                    cg->gen_fn = NULL; cg->gen_inst = -1; cg->gen_emit_name = NULL;
                }
                continue;
            }
            emit_fn(cg, item);
        }
        /* M23: impl методите — тела (без clo wrapper; статичен dispatch) */
        if (item->kind == NODE_IMPL) {
            for (int m = 0; m < item->impl_methods.len; m++) {
                Node *mf = item->impl_methods.data[m];
                cg->gen_emit_name = mf->fn_name;
                emit_fn(cg, mf);
                cg->gen_emit_name = NULL;
            }
        }
    }
    fclose(fa);
    fclose(cg->lambda_out);
    cg->out = saved_out;
    cg->lambda_out = NULL;
    fwrite(lbuf, 1, lsize, out);
    fwrite(fbuf, 1, fsize, out);
    free(lbuf);
    free(fbuf);

    if (cg->test_specs) {
        emit_test_driver(cg, program);
    } else {
        /* C main only when the program defines main (libraries may omit it) */
        Node *main_fn = NULL;
        for (int i = 0; i < program->items.len; i++) {
            Node *it = program->items.data[i];
            if (it->kind == NODE_FN && it->fn_body && it->fn_name &&
                strcmp(it->fn_name, "main") == 0) {
                main_fn = it;
                break;
            }
        }
        if (main_fn) {
            /* main -> i64: the returned value is the process exit code
             * (kvbaga K3 — before this the wrapper swallowed it) */
            Node *rt = main_fn->ret_type;
            if (rt && rt->kind == NODE_TYPE_EFFECT) rt = rt->inner_type;
            int ret_int = rt && rt->kind == NODE_TYPE && rt->type_name &&
                (strcmp(rt->type_name, "i64") == 0 ||
                 strcmp(rt->type_name, "i32") == 0);
            fprintf(out, "int main(int argc, char **argv) {\n");
            fprintf(out, "    baga_argc = argc; baga_argv = argv;\n");
            if (ret_int) {
                fprintf(out, "    return (int)b_main();\n");
            } else {
                fprintf(out, "    b_main();\n");
                fprintf(out, "    return 0;\n");
            }
            fprintf(out, "}\n");
        }
    }

    /* RC2/RC2.1/RC3: броячи на елиминациите — коментар в края на изхода */
    if (cg->rc) {
        fprintf(out, "/* RC2: move elisions: %d */\n", cg->rc_moves);
        fprintf(out, "/* RC2.1: borrowed pair elisions: %d */\n", cg->rc_elided_pairs);
        fprintf(out, "/* RC3: container move elisions: %d */\n", cg->rc_cmoves);
        fprintf(out, "/* RC4: temp releases: %d */\n", cg->rc_tmp_count);
    }

    /* RC1: освободи scope стековете на компилатора */
    for (int i = 0; i < cg->rc_locals.len; i++) free(cg->rc_locals.data[i].name);
    vec_free(cg->rc_locals);
    vec_free(cg->rc_scopes);
    vec_free(cg->rc_lus);
    vec_free(cg->rc_tmps);
    vec_free(cg->rc_marks);
}

