#include "baga.h"

/* ============================================================
 *  Type helpers
 * ============================================================ */

Type *type_new(TypeKind kind) {
    Type *t = calloc(1, sizeof(Type));
    if (!t) { fprintf(stderr, "baga: out of memory\n"); exit(1); }
    t->kind = kind;
    return t;
}

Type *type_fn(Type *ret, Type **params, int nparams) {
    Type *t = type_new(TYPE_FN);
    t->ret = ret;
    t->params = params;
    t->nparams = nparams;
    return t;
}

/* Въртящ се буфер: няколко type_str в един printf не трябва да се
 * заличават (предишните два статични буфера се сблъскваха, когато две
 * Vec/Map-та се отпечатваха в едно съобщение). */
static char *type_str_buf(void) {
    static char bufs[8][96];
    static int bi = 0;
    char *b = bufs[bi];
    bi = (bi + 1) % 8;
    return b;
}

const char *type_str(Type *t) {
    if (!t) return "void";
    switch (t->kind) {
        case TYPE_VOID:  return "void";
        case TYPE_BOOL:  return "bool";
        case TYPE_I32:   return "i32";
        case TYPE_I64:   return t->name ? t->name : "i64";
        case TYPE_F64:   return "f64";
        case TYPE_STR:   return "str";
        case TYPE_BYTES: return "bytes";
        case TYPE_ERROR: return "<грешка>";
        case TYPE_ARRAY: return "[T]";
        case TYPE_REF:   return "&T";
        case TYPE_STRUCT: return t->name ? t->name : "struct";
        case TYPE_ENUM:  return t->name ? t->name : "enum";
        case TYPE_FN: {
            char *buf = type_str_buf();
            int off = snprintf(buf, 96, "fn(");
            for (int i = 0; i < t->nparams && off < 80; i++) {
                off += snprintf(buf + off, (size_t)(96 - off), "%s%s",
                                i > 0 ? ", " : "", type_str(t->params[i]));
            }
            snprintf(buf + off, (size_t)(96 - off), ") -> %s", type_str(t->ret));
            return buf;
        }
        case TYPE_VEC: {
            if (!t->elem) return "Vec";
            char *buf = type_str_buf();
            snprintf(buf, 96, "Vec<%s>", type_str(t->elem));
            return buf;
        }
        case TYPE_MAP: {
            if (!t->key && !t->elem) return "Map";
            char *buf = type_str_buf();
            snprintf(buf, 96, "Map<%s, %s>",
                     t->key ? type_str(t->key) : "?",
                     t->elem ? type_str(t->elem) : "?");
            return buf;
        }
    }
    return "?";
}

/* елементно равенство за Vec<T>: вид + за struct — и име (Vec<A> ≠ Vec<B>) */
static int vec_elem_eq(Type *a, Type *b) {
    if (a->kind != b->kind) return 0;
    if (a->kind == TYPE_STRUCT)
        return a->name && b->name && strcmp(a->name, b->name) == 0;
    if (a->kind == TYPE_ENUM)
        return a->name && b->name && strcmp(a->name, b->name) == 0;
    if (a->kind == TYPE_VEC) {
        /* вложени вектори: рекурсия по елементите */
        if (a->elem && b->elem) return vec_elem_eq(a->elem, b->elem);
        return 1;
    }
    return 1;
}

int type_eq(Type *a, Type *b) {
    if (!a || !b) return a == b;
    if (a->kind == TYPE_ERROR || b->kind == TYPE_ERROR) return 1;
    if (a->kind != b->kind) return 0;
    if (a->kind == TYPE_STRUCT) {
        return a->name && b->name && strcmp(a->name, b->name) == 0;
    }
    if (a->kind == TYPE_ENUM)
        return a->name && b->name && strcmp(a->name, b->name) == 0;
    if (a->kind == TYPE_VEC) {
        /* Vec срещу Vec: различни само ако и двата знаят elem и той се различава;
         * Vec без elem (наследен код) съвпада с всеки Vec<T> */
        if (a->elem && b->elem && !vec_elem_eq(a->elem, b->elem)) return 0;
        return 1;
    }
    if (a->kind == TYPE_MAP) {
        /* като Vec: непознат ключ/стойност съвпада с всеки; познатите трябва
         * да съвпадат по вид (и по име за struct стойности) */
        if (a->key && b->key && a->key->kind != b->key->kind) return 0;
        if (a->elem && b->elem && !vec_elem_eq(a->elem, b->elem)) return 0;
        return 1;
    }
    if (a->kind == TYPE_FN) {
        /* структурно: брой и видове параметри + връщан тип (ефектите се
         * проверяват отделно при wrap — виж fn_effects_subset) */
        if (a->nparams != b->nparams) return 0;
        for (int i = 0; i < a->nparams; i++)
            if (!type_eq(a->params[i], b->params[i])) return 0;
        return type_eq(a->ret, b->ret);
    }
    return 1;
}

/* дали аргумент от тип `arg` може да се подаде на параметър от тип `param` */
static int type_assignable(Type *arg, Type *param) {
    if (!arg || !param) return 1;
    if (arg->kind == TYPE_ERROR || param->kind == TYPE_ERROR) return 1;
    /* цялочислено семейство: i32/i64 са съвместими (int литералите са i64) */
    if ((arg->kind == TYPE_I32 || arg->kind == TYPE_I64) &&
        (param->kind == TYPE_I32 || param->kind == TYPE_I64)) return 1;
    return type_eq(arg, param);
}

/* ============================================================
 *  Effect helpers
 * ============================================================ */

void type_add_effect(Type *t, const char *effect) {
    if (!t || !effect) return;
    if (type_has_effect(t, effect)) return;
    t->effects = realloc(t->effects, sizeof(char *) * (size_t)(t->n_effects + 1));
    if (!t->effects) { fprintf(stderr, "baga: out of memory\n"); exit(1); }
    t->effects[t->n_effects++] = strdup(effect);
}

int type_has_effect(Type *t, const char *effect) {
    if (!t || !effect) return 0;
    for (int i = 0; i < t->n_effects; i++) {
        if (strcmp(t->effects[i], effect) == 0) return 1;
    }
    return 0;
}

void type_remove_effect(Type *t, const char *effect) {
    if (!t || !effect) return;
    for (int i = 0; i < t->n_effects; i++) {
        if (strcmp(t->effects[i], effect) == 0) {
            free(t->effects[i]);
            for (int j = i; j < t->n_effects - 1; j++)
                t->effects[j] = t->effects[j + 1];
            t->n_effects--;
            return;
        }
    }
}

void type_merge_effects(Type *dst, Type *src) {
    if (!dst || !src) return;
    for (int i = 0; i < src->n_effects; i++)
        type_add_effect(dst, src->effects[i]);
}

/* ============================================================
 *  Type environment
 * ============================================================ */

#define ENV_MAX 64
#define ENV_VARS 256
/* Per translation unit. http+jsonx+orm+pg already exceeds 256; frameworks
 * (fmrbaga) and multi-product apps need headroom. Silent drop when full. */
#define FNS_MAX  2048

typedef struct EnvEntry EnvEntry;
struct EnvEntry {
    char *name;
    Type *type;
    int is_mut;
    /* MEM-2: drop seatbelt — per-variable live/dropped състояние */
    int dropped;      /* drop() е извикан върху тази локална */
    int is_param;     /* параметър — буферът е на извикващия, drop забранен */
    int captured;     /* заснет от ламбда — drop би оставил висящ указател */
    int scope_depth;  /* ctx->depth при дефиницията (за loop-правилото) */
    /* MEM-3: region — EnvEntry of arena handle that produced this local
     * via arena_alloc (NULL = not arena-allocated / unknown). Freeing the
     * arena marks all locals with region==that entry as dropped. */
    EnvEntry *region;
};

typedef struct {
    EnvEntry entries[ENV_VARS];
    int count;
} EnvScope;

typedef struct {
    EnvScope scopes[ENV_MAX];
    int depth;

    /* function registry */
    struct {
        char *name;          /* късо име (както е в източника) */
        const char *origin;  /* модул: basename на файла без .baga ("" = неизвестен) */
        Type *fn_type;   /* TYPE_FN */
        Node *decl;      /* NODE_FN */
    } fns[FNS_MAX];
    int n_fns;

    /* struct registry */
    struct {
        char *name;
        Node *decl;
    } structs[FNS_MAX];
    int n_structs;

    /* enum registry */
    struct {
        char *name;
        Node *decl;
    } enums[FNS_MAX];
    int n_enums;

    /* enum variant → value mapping */
    struct {
        char *variant;
        char *enum_name;
        int value;
        Type *payload;       /* L3: NULL = plain variant */
    } variants[FNS_MAX * 4];
    int n_variants;

    Checker *chk;
    const char *cur_fn;
    const char *main_base; /* модул на главния файл (L6 предимство при неуточнени извиквания) */
    Node *program;         /* коренът — за L6 scoped struct resolution */
    int n_lambdas;         /* L5: брояч за синтетични имена __lam_N */
    Type *cur_ret;   /* expected return type of current function */
    Type *cur_effects; /* accumulated effects in current function body */
    /* MEM-2: drop seatbelt — drop-лог за if/else join и дълбочина на цикъл */
    EnvEntry *drop_log[256];
    int n_drop_log;
    int drop_log_overflowed; /* логът се е препълнил — join деградира консервативно */
    int loop_depth;   /* 0 = извън цикъл; иначе depth-а на обграждащия цикъл */
} CheckCtx;

/* L6 forward декларации — дефинициите са след import alias таблицата. */
static char *mod_base(const char *path);
static Node *find_struct_scoped(CheckCtx *ctx, const char *name,
                                const char *posfile, int *ambiguous);
static void struct_amb_hint(CheckCtx *ctx, const char *name,
                            char *m1, char *m2, size_t cap);

static void check_error(CheckCtx *ctx, SrcPos pos, const char *fmt, ...) {
    if (ctx->chk->n_errors >= BAGA_MAX_ERRORS) return;
    char *e = ctx->chk->errors[ctx->chk->n_errors++];
    int off = snprintf(e, 256, "%d:%d: ", pos.line, pos.col);
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(e + off, 256 - (size_t)off, fmt, ap);
    va_end(ap);
    /* същата грешка на същата позиция от два прохода — пазим само веднъж */
    if (ctx->chk->n_errors >= 2 &&
        strcmp(ctx->chk->errors[ctx->chk->n_errors - 2], e) == 0)
        ctx->chk->n_errors--;
}

/* Map a type AST node to a Type */
static Type *resolve_type_node(CheckCtx *ctx, Node *ty) {
    if (!ty) return type_new(TYPE_VOID);
    switch (ty->kind) {
        case NODE_TYPE:
            if (strcmp(ty->type_name, "i32") == 0)  return type_new(TYPE_I32);
            if (strcmp(ty->type_name, "i64") == 0)  return type_new(TYPE_I64);
            if (strcmp(ty->type_name, "f64") == 0)  return type_new(TYPE_F64);
            if (strcmp(ty->type_name, "bool") == 0) return type_new(TYPE_BOOL);
            if (strcmp(ty->type_name, "str") == 0)  return type_new(TYPE_STR);
            if (strcmp(ty->type_name, "bytes") == 0) return type_new(TYPE_BYTES);
            if (strcmp(ty->type_name, "void") == 0) return type_new(TYPE_VOID);
            if (strcmp(ty->type_name, "Vec") == 0) {
                Type *t = type_new(TYPE_VEC);
                if (ty->inner_type) {
                    /* Vec<T>: i64 (i32 → i64), str, f64, bytes, struct,
                     * sum enum, fn и вложен Vec */
                    Type *el = resolve_type_node(ctx, ty->inner_type);
                    if (el->kind == TYPE_I32) el = type_new(TYPE_I64);
                    if (el->kind != TYPE_I64 && el->kind != TYPE_STR &&
                        el->kind != TYPE_F64 && el->kind != TYPE_BYTES &&
                        el->kind != TYPE_STRUCT && el->kind != TYPE_FN &&
                        el->kind != TYPE_VEC && el->kind != TYPE_ENUM) {
                        check_error(ctx, ty->pos,
                            "Vec<T>: неподдържан елементен тип %s (поддържат се i64, str, f64, bytes, struct, sum enum, fn и Vec)",
                            type_str(el));
                    } else {
                        t->elem = el;
                    }
                }
                return t;
            }
            if (strcmp(ty->type_name, "Map") == 0) {
                Type *t = type_new(TYPE_MAP);
                if (ty->inner_type || ty->inner_type2) {
                    if (!ty->inner_type || !ty->inner_type2) {
                        check_error(ctx, ty->pos,
                            "Map<K, V>: очаквах два типа — ключ и стойност");
                        return t;
                    }
                    /* Map<K,V>: ключ i64/str/bytes; стойност i64/str/f64/bytes */
                    Type *kt = resolve_type_node(ctx, ty->inner_type);
                    if (kt->kind == TYPE_I32) kt = type_new(TYPE_I64);
                    if (kt->kind != TYPE_I64 && kt->kind != TYPE_STR &&
                        kt->kind != TYPE_BYTES) {
                        check_error(ctx, ty->pos,
                            "Map<K, V>: неподдържан ключов тип %s (поддържат се i64, str и bytes)",
                            type_str(kt));
                    } else {
                        t->key = kt;
                    }
                    Type *vt = resolve_type_node(ctx, ty->inner_type2);
                    if (vt->kind == TYPE_I32) vt = type_new(TYPE_I64);
                    if (vt->kind != TYPE_I64 && vt->kind != TYPE_STR &&
                        vt->kind != TYPE_F64 && vt->kind != TYPE_BYTES &&
                        vt->kind != TYPE_STRUCT && vt->kind != TYPE_FN &&
                        vt->kind != TYPE_ENUM) {
                        check_error(ctx, ty->pos,
                            "Map<K, V>: неподдържан стойностен тип %s (поддържат се i64, str, f64, bytes, struct, sum enum и fn)",
                            type_str(vt));
                    } else {
                        t->elem = vt;
                    }
                }
                return t;
            }
            {
                for (int i = 0; i < ctx->n_enums; i++) {
                    if (strcmp(ctx->enums[i].name, ty->type_name) != 0) continue;
                    if (ctx->enums[i].decl->type &&
                        ctx->enums[i].decl->type->kind == TYPE_ENUM) {
                        Type *t = type_new(TYPE_ENUM);
                        t->name = strdup(ty->type_name);
                        return t;
                    }
                    /* LP-final, обединяване: enum без payload-и е i64-базиран —
                     * вариантите са именувани i64 константи и името е валидна
                     * типова анотация за i64 (преди падаше в struct lookup). */
                    Type *t = type_new(TYPE_I64);
                    t->name = strdup(ty->type_name);
                    return t;
                }
                int amb = 0;
                Node *sd = find_struct_scoped(ctx, ty->type_name, ty->pos.file, &amb);
                if (amb) {
                    char m1[128], m2[128];
                    struct_amb_hint(ctx, ty->type_name, m1, m2, sizeof m1);
                    check_error(ctx, ty->pos,
                        "нееднозначен struct '%s' — има го в модулите '%s' и '%s'; уточни с %s.%s или %s.%s",
                        ty->type_name, m1, m2, m1, ty->type_name, m2, ty->type_name);
                }
                if (sd && strcmp(ty->type_name, sd->struct_name) != 0) {
                    /* L6: codegen чете AST текста — пренаписваме към финалното
                     * "модул.Type" име, за да е консистентен mangling-ът */
                    free(ty->type_name);
                    ty->type_name = strdup(sd->struct_name);
                }
                Type *t = type_new(TYPE_STRUCT);
                t->name = strdup(sd ? sd->struct_name : ty->type_name);
                return t;
            }
        case NODE_TYPE_REF: {
            Type *t = type_new(TYPE_REF);
            t->pointee = resolve_type_node(ctx, ty->inner_type);
            return t;
        }
        case NODE_TYPE_ARRAY: {
            /* [T] is sugar for Vec<T>: the growable baga_Vec, not a raw C
             * pointer. This lets Vec parameters be written as [i64] and used
             * with the vec_* builtins uniformly. */
            Type *t = type_new(TYPE_VEC);
            if (ty->inner_type) {
                Type *el = resolve_type_node(ctx, ty->inner_type);
                if (el->kind == TYPE_I32) el = type_new(TYPE_I64);
                if (el->kind != TYPE_I64 && el->kind != TYPE_STR &&
                    el->kind != TYPE_F64 && el->kind != TYPE_BYTES &&
                    el->kind != TYPE_STRUCT && el->kind != TYPE_FN &&
                    el->kind != TYPE_VEC && el->kind != TYPE_ENUM) {
                    check_error(ctx, ty->pos,
                        "[T]: неподдържан елементен тип %s (поддържат се i64, str, f64, bytes, struct, sum enum, fn и Vec)",
                        type_str(el));
                } else {
                    t->elem = el;
                }
            }
            return t;
        }
        case NODE_TYPE_EFFECT: {
            Type *base = resolve_type_node(ctx, ty->inner_type);
            for (int i = 0; i < ty->n_effects; i++)
                type_add_effect(base, ty->effect_names[i]);
            return base;
        }
        case NODE_TYPE_FN: {
            /* fn(T, ...) -> R — ефектите пътуват върху ret, както при
             * декларациите (ret_type е TYPE_EFFECT обвивка при !E) */
            int np = ty->params.len;
            Type **params = NULL;
            if (np > 0) {
                params = malloc(sizeof(Type *) * (size_t)np);
                for (int j = 0; j < np; j++)
                    params[j] = resolve_type_node(ctx, ty->params.data[j]->param_type);
            }
            Type *ret = ty->ret_type ? resolve_type_node(ctx, ty->ret_type)
                                     : type_new(TYPE_VOID);
            return type_fn(ret, params, np);
        }
        default:
            return type_new(TYPE_ERROR);
    }
}

static void push_scope(CheckCtx *ctx) {
    if (ctx->depth < ENV_MAX) {
        ctx->scopes[ctx->depth].count = 0;
        ctx->depth++;
    }
}

static void pop_scope(CheckCtx *ctx) {
    if (ctx->depth > 0) ctx->depth--;
}

static void env_define(CheckCtx *ctx, const char *name, Type *type, SrcPos pos) {
    if (ctx->depth <= 0) return;
    EnvScope *s = &ctx->scopes[ctx->depth - 1];
    for (int i = 0; i < s->count; i++) {
        if (strcmp(s->entries[i].name, name) == 0) {
            check_error(ctx, pos, "повторно дефиниране на '%s'", name);
            return;
        }
    }
    if (s->count < ENV_VARS) {
        s->entries[s->count].name = (char *)name;
        s->entries[s->count].type = type;
        s->entries[s->count].is_mut = 1;
        s->entries[s->count].dropped = 0;
        s->entries[s->count].is_param = 0;
        s->entries[s->count].captured = 0;
        s->entries[s->count].scope_depth = ctx->depth;
        s->entries[s->count].region = NULL;
        s->count++;
    }
}

static void env_define_mut(CheckCtx *ctx, const char *name, Type *type, int is_mut, SrcPos pos) {
    if (ctx->depth <= 0) return;
    EnvScope *s = &ctx->scopes[ctx->depth - 1];
    for (int i = 0; i < s->count; i++) {
        if (strcmp(s->entries[i].name, name) == 0) {
            check_error(ctx, pos, "повторно дефиниране на '%s'", name);
            return;
        }
    }
    if (s->count < ENV_VARS) {
        s->entries[s->count].name = (char *)name;
        s->entries[s->count].type = type;
        s->entries[s->count].is_mut = is_mut;
        s->entries[s->count].dropped = 0;
        s->entries[s->count].is_param = 0;
        s->entries[s->count].captured = 0;
        s->entries[s->count].scope_depth = ctx->depth;
        s->entries[s->count].region = NULL;
        s->count++;
    }
}

/* MEM-3: mark every local that came from arena `ae` as dropped (region free). */
static void mem3_invalidate_region(CheckCtx *ctx, EnvEntry *ae) {
    if (!ae) return;
    for (int d = 0; d < ctx->depth; d++) {
        EnvScope *s = &ctx->scopes[d];
        for (int i = 0; i < s->count; i++) {
            EnvEntry *e = &s->entries[i];
            if (e->region != ae || e->dropped) continue;
            e->dropped = 1;
            if (ctx->n_drop_log < 256)
                ctx->drop_log[ctx->n_drop_log++] = e;
            else
                ctx->drop_log_overflowed = 1;
        }
    }
}

/* MEM-2: като env_lookup, но връща самата EnvEntry (за drop seatbelt-а).
 * env_lookup пази подписа си. */
static EnvEntry *env_find(CheckCtx *ctx, const char *name) {
    for (int d = ctx->depth - 1; d >= 0; d--) {
        EnvScope *s = &ctx->scopes[d];
        for (int i = s->count - 1; i >= 0; i--) {
            if (strcmp(s->entries[i].name, name) == 0)
                return &s->entries[i];
        }
    }
    return NULL;
}

/* MEM-2: drop-join машинерия — споделена от if/else, match и catch.
 * Логът е append-only; всеки алтернативен клон записва drop-овете си в
 * непрекъснат диапазон. Преди всеки нов клон предишните се "вдигат"
 * (dropped = 0), за да не ги вижда той. */

/* вдигни всички drop-ове, логнати от base нататък */
static void drop_lift_from(CheckCtx *ctx, int base) {
    for (int i = base; i < ctx->n_drop_log; i++) ctx->drop_log[i]->dropped = 0;
}

/* двуклонен join (if/else, catch): [log_base, mid) = първи път,
 * [mid, n_drop_log) = втори път. Остава dropped само маркираното и в двата;
 * оцелелите се компактират на log_base, за да ги види външен конструкт.
 * При препълнен лог — консервативно вдигане на всичко (пропусната грешка,
 * никога измислена); нелогнатите drop-ове изобщо не са маркирани. */
static void drop_join2(CheckCtx *ctx, int log_base, int mid) {
    if (ctx->drop_log_overflowed) {
        drop_lift_from(ctx, log_base);
        ctx->n_drop_log = log_base;
        ctx->drop_log_overflowed = 0;
        return;
    }
    int end = ctx->n_drop_log;
    for (int j = mid; j < end; j++) {
        EnvEntry *e = ctx->drop_log[j];
        int in_first = 0;
        for (int i = log_base; i < mid; i++)
            if (ctx->drop_log[i] == e) { in_first = 1; break; }
        e->dropped = in_first;
    }
    int w = log_base;
    for (int j = mid; j < end; j++) {
        EnvEntry *e = ctx->drop_log[j];
        if (!e->dropped) continue;
        int seen = 0;
        for (int k = log_base; k < w; k++)
            if (ctx->drop_log[k] == e) { seen = 1; break; }
        if (!seen) ctx->drop_log[w++] = e;
    }
    ctx->n_drop_log = w;
}

/* N-arm join (match): arm_end[k] = n_drop_log след arm k. Definitely dropped
 * = drop-нато във ВСЕКИ arm. arm_end NULL (над 64 arm-а) или препълнен лог →
 * консервативно вдигане на всичко. Празен match (0 arm-а): нищо не се пипа. */
static void drop_join_arms(CheckCtx *ctx, int log_base, const int *arm_end, int narm) {
    if (ctx->drop_log_overflowed || !arm_end || narm <= 0) {
        drop_lift_from(ctx, log_base);
        ctx->n_drop_log = log_base;
        ctx->drop_log_overflowed = 0;
        return;
    }
    int w = log_base;
    /* кандидатите са записите от arm 0; оцелява само присъстващият навсякъде */
    for (int i = log_base; i < arm_end[0]; i++) {
        EnvEntry *e = ctx->drop_log[i];
        int everywhere = 1;
        for (int k = 1; k < narm && everywhere; k++) {
            int found = 0;
            for (int j = arm_end[k - 1]; j < arm_end[k]; j++)
                if (ctx->drop_log[j] == e) { found = 1; break; }
            if (!found) everywhere = 0;
        }
        e->dropped = everywhere;
        if (everywhere) {
            int seen = 0;
            for (int m = log_base; m < w; m++)
                if (ctx->drop_log[m] == e) { seen = 1; break; }
            if (!seen) ctx->drop_log[w++] = e;
        }
    }
    /* записи от по-късни arm-ове, липсващи в arm 0 — вдигни */
    for (int k = 1; k < narm; k++) {
        for (int j = arm_end[k - 1]; j < arm_end[k]; j++) {
            EnvEntry *e = ctx->drop_log[j];
            if (!e->dropped) continue;
            int in0 = 0;
            for (int i = log_base; i < arm_end[0]; i++)
                if (ctx->drop_log[i] == e) { in0 = 1; break; }
            if (!in0) e->dropped = 0;
        }
    }
    ctx->n_drop_log = w;
}

static Type *env_lookup(CheckCtx *ctx, const char *name) {
    for (int d = ctx->depth - 1; d >= 0; d--) {
        EnvScope *s = &ctx->scopes[d];
        for (int i = s->count - 1; i >= 0; i--) {
            if (strcmp(s->entries[i].name, name) == 0)
                return s->entries[i].type;
        }
    }
    return NULL;
}

static int env_is_mut(CheckCtx *ctx, const char *name) {
    for (int d = ctx->depth - 1; d >= 0; d--) {
        EnvScope *s = &ctx->scopes[d];
        for (int i = s->count - 1; i >= 0; i--) {
            if (strcmp(s->entries[i].name, name) == 0)
                return s->entries[i].is_mut;
        }
    }
    return 1;
}

static Type *find_fn(CheckCtx *ctx, const char *name) {
    /* пълно (евентуално преименувано „модул.име") име — уникално (L6) */
    for (int i = 0; i < ctx->n_fns; i++) {
        if (ctx->fns[i].decl->fn_name &&
            strcmp(ctx->fns[i].decl->fn_name, name) == 0)
            return ctx->fns[i].fn_type;
    }
    /* късо име — първото съвпадение (историческо поведение) */
    for (int i = 0; i < ctx->n_fns; i++) {
        if (strcmp(ctx->fns[i].name, name) == 0)
            return ctx->fns[i].fn_type;
    }
    return NULL;
}

/* L6: import alias таблица (canonical path → alias). Пълни се от main.c
 * при `import "p" as a`; mod_base я консултира преди basename правилото. */
#define IMPORT_ALIASES_MAX 128
static struct { char *canon; char *alias; } g_import_aliases[IMPORT_ALIASES_MAX];
static int g_n_import_aliases = 0;

const char *baga_note_import_alias(const char *canon_path, const char *alias) {
    for (int i = 0; i < g_n_import_aliases; i++) {
        if (strcmp(g_import_aliases[i].canon, canon_path) == 0)
            return strcmp(g_import_aliases[i].alias, alias) == 0
                 ? NULL : g_import_aliases[i].alias;   /* същият alias — ОК */
    }
    if (g_n_import_aliases >= IMPORT_ALIASES_MAX)
        return "<лимит>";
    g_import_aliases[g_n_import_aliases].canon = strdup(canon_path);
    g_import_aliases[g_n_import_aliases].alias = strdup(alias);
    g_n_import_aliases++;
    return NULL;
}

static const char *import_alias_for(const char *canon_path) {
    for (int i = 0; i < g_n_import_aliases; i++)
        if (strcmp(g_import_aliases[i].canon, canon_path) == 0)
            return g_import_aliases[i].alias;
    return NULL;
}

/* късото име на (евентуално преименуван „модул.Type") тип */
static const char *type_short_name(const char *name) {
    const char *d = strrchr(name, '.');
    return d ? d + 1 : name;
}

/* L6: struct decl по име от гледна точка posfile. Пълното име печели
 * винаги; при късо име — собственият модул, после единственият кандидат;
 * повече от един чужд → *ambiguous = 1 и NULL (поикащият диагностицира). */
static Node *find_struct_scoped(CheckCtx *ctx, const char *name,
                                const char *posfile, int *ambiguous) {
    if (ambiguous) *ambiguous = 0;
    if (!ctx->program) return NULL;
    Node *first = NULL;
    int nfound = 0;
    for (int i = 0; i < ctx->program->items.len; i++) {
        Node *it = ctx->program->items.data[i];
        if (it->kind != NODE_STRUCT) continue;
        if (strcmp(it->struct_name, name) == 0) return it;   /* пълно име */
        if (strcmp(type_short_name(it->struct_name), name) == 0) {
            if (!first) first = it;
            nfound++;
        }
    }
    if (nfound <= 1) return first;
    char *own = mod_base(posfile);
    Node *mine = NULL;
    for (int i = 0; i < ctx->program->items.len; i++) {
        Node *it = ctx->program->items.data[i];
        if (it->kind != NODE_STRUCT) continue;
        if (strcmp(type_short_name(it->struct_name), name) != 0) continue;
        char *org = mod_base(it->pos.file);
        int eq = strcmp(org, own) == 0;
        free(org);
        if (eq) { mine = it; break; }
    }
    free(own);
    if (mine) return mine;
    if (ambiguous) *ambiguous = 1;
    return NULL;
}

/* първите два модула с кандидати за късото име (за ambiguity подсказката) */
static void struct_amb_hint(CheckCtx *ctx, const char *name,
                            char *m1, char *m2, size_t cap) {
    m1[0] = '\0';
    m2[0] = '\0';
    for (int i = 0; ctx->program && i < ctx->program->items.len; i++) {
        Node *it = ctx->program->items.data[i];
        if (it->kind != NODE_STRUCT) continue;
        if (strcmp(type_short_name(it->struct_name), name) != 0) continue;
        char *org = mod_base(it->pos.file);
        if (m1[0] == '\0') {
            snprintf(m1, cap, "%s", org);
        } else if (strcmp(m1, org) != 0 && m2[0] == '\0') {
            snprintf(m2, cap, "%s", org);
        }
        free(org);
        if (m1[0] && m2[0]) return;
    }
}

/* модулно име от път: alias ако е даден (`import … as a`), иначе basename
 * без .baga (нов низ; NULL → "") */
static char *mod_base(const char *path) {
    if (!path) return strdup("");
    const char *al = import_alias_for(path);
    if (al) return strdup(al);
    const char *base = strrchr(path, '/');
    base = base ? base + 1 : path;
    size_t len = strlen(base);
    if (len > 5 && strcmp(base + len - 5, ".baga") == 0) len -= 5;
    char *out = malloc(len + 1);
    if (!out) { fprintf(stderr, "baga: out of memory\n"); exit(1); }
    memcpy(out, base, len);
    out[len] = '\0';
    return out;
}

/* ============================================================
 *  Type inference
 * ============================================================ */

static Type *infer(CheckCtx *ctx, Node *n);

static int is_numeric(Type *t) {
    return t && (t->kind == TYPE_I32 || t->kind == TYPE_I64 || t->kind == TYPE_F64);
}

static Type *numeric_promote(Type *a, Type *b) {
    if (a->kind == TYPE_F64 || b->kind == TYPE_F64) return type_new(TYPE_F64);
    if (a->kind == TYPE_I64 || b->kind == TYPE_I64) return type_new(TYPE_I64);
    return type_new(TYPE_I32);
}

static Type *infer_binary(CheckCtx *ctx, Node *n) {
    Type *lt = infer(ctx, n->left);
    Type *rt = infer(ctx, n->right);

    switch (n->bin_op) {
        case OP_ADD: case OP_SUB: case OP_MUL: case OP_DIV: case OP_MOD:
            if (!is_numeric(lt) || !is_numeric(rt)) {
                check_error(ctx, n->pos, "аритметична операция върху не-числови типове (%s, %s)",
                            type_str(lt), type_str(rt));
                return type_new(TYPE_ERROR);
            }
            return numeric_promote(lt, rt);

        case OP_EQ: case OP_NEQ: case OP_LT: case OP_GT: case OP_LE: case OP_GE:
            return type_new(TYPE_BOOL);

        case OP_AND: case OP_OR:
            return type_new(TYPE_BOOL);

        case OP_BIT_AND: case OP_BIT_OR: case OP_BIT_XOR:
        case OP_LSHIFT: case OP_RSHIFT:
            return type_new(TYPE_I64);
    }
    return type_new(TYPE_ERROR);
}

/* ============================================================
 *  L5: function values & closures
 * ============================================================ */

/* ефектите на fn стойността (върху ret) ⊆ ефектите на анотацията */
static int fn_effects_subset(Type *val, Type *annot) {
    Type *ve = val && val->kind == TYPE_FN ? val->ret : NULL;
    Type *ae = annot && annot->kind == TYPE_FN ? annot->ret : NULL;
    if (!ve) return 1;
    for (int i = 0; i < ve->n_effects; i++)
        if (!ae || !type_has_effect(ae, ve->effects[i])) return 0;
    return 1;
}

/* извикване през fn стойност; аргументите вече са infer-нати */
static Type *call_fn_value(CheckCtx *ctx, Node *n, Type *ft) {
    if (n->args.len != ft->nparams) {
        check_error(ctx, n->pos, "fn стойност %s очаква %d аргумента, получих %d",
                    type_str(ft), ft->nparams, n->args.len);
    }
    int check_n = n->args.len < ft->nparams ? n->args.len : ft->nparams;
    for (int i = 0; i < check_n; i++) {
        Type *at = n->args.data[i]->type;
        Type *pt = ft->params[i];
        if (!type_assignable(at, pt)) {
            check_error(ctx, n->args.data[i]->pos,
                "fn стойност: аргумент #%d е от тип %s, но параметърът е %s",
                i + 1, type_str(at), type_str(pt));
        } else if (at && pt && at->kind == TYPE_FN && pt->kind == TYPE_FN &&
                   !fn_effects_subset(at, pt)) {
            check_error(ctx, n->args.data[i]->pos,
                "fn аргумент #%d има ефекти извън договора %s", i + 1, type_str(pt));
        }
    }
    /* codegen маркер: TYPE_FN без име = стойност (извикване през handle) */
    Type *marker = type_fn(ft->ret, ft->params, ft->nparams);
    n->callee->type = marker;
    Type *ret = ft->ret ? ft->ret : type_new(TYPE_VOID);
    Type *result = type_new(ret->kind);
    result->elem = ret->elem;
    result->name = ret->name;
    /* fn (L5): пази сигнатурата — върнатата closure е извикваема */
    result->ret = ret->ret;
    result->params = ret->params;
    result->nparams = ret->nparams;
    type_merge_effects(result, ret);
    if (ctx->cur_effects)
        type_merge_effects(ctx->cur_effects, ret);
    return result;
}

static Type *infer_call(CheckCtx *ctx, Node *n) {
    /* infer arg types */
    for (int i = 0; i < n->args.len; i++)
        infer(ctx, n->args.data[i]);

    /* L6: модулно-уточнено извикване mod.f(...). Локална променлива с име
     * mod печели — тогава е обикновен field достъп и минава по-долу. */
    if (n->callee->kind == NODE_FIELD &&
        n->callee->field_obj->kind == NODE_IDENT &&
        !env_lookup(ctx, n->callee->field_obj->name)) {
        const char *mod = n->callee->field_obj->name;
        const char *fname = n->callee->field_name;
        int found = -1, mod_known = 0;
        for (int i = 0; i < ctx->n_fns; i++) {
            if (strcmp(ctx->fns[i].origin, mod) != 0) continue;
            mod_known = 1;
            if (strcmp(ctx->fns[i].name, fname) == 0) { found = i; break; }
        }
        if (found < 0) {
            if (mod_known)
                check_error(ctx, n->pos, "модулът '%s' няма функция '%s'", mod, fname);
            else
                check_error(ctx, n->pos, "непознат модул '%s' (няма импортиран %s.baga)", mod, mod);
            return type_new(TYPE_ERROR);
        }
        /* пренаписваме към уникалния вътрешен символ; стандартният път
         * по-долу прави проверките на аргументите. strdup — node_free
         * освобождава callee->name, не трябва да алиасва decl->fn_name */
        n->callee->kind = NODE_IDENT;
        n->callee->name = strdup(ctx->fns[found].decl->fn_name);
    }

    /* L5: извикване през fn стойност — локална/параметър, или произволен
     * израз с TYPE_FN тип (vec_get(t, i)(x), obj.handler(x) и пр.).
     * A1 NODE_PATH is a sum constructor, not a fn value — skip this branch. */
    if (n->callee->kind == NODE_IDENT) {
        Type *lt = env_lookup(ctx, n->callee->name);
        if (lt && lt->kind == TYPE_FN)
            return call_fn_value(ctx, n, lt);
    } else if (n->callee->kind != NODE_PATH) {
        Type *ct = infer(ctx, n->callee);
        if (ct && ct->kind == TYPE_FN)
            return call_fn_value(ctx, n, ct);
        check_error(ctx, n->pos, "извикване на не-функция (%s)", type_str(ct));
        return type_new(TYPE_ERROR);
    }

    /* L3/A1: конструктор на sum enum — Variant(payload) или Enum::Variant(payload).
     * Bare: точно един payload-вариант с това име; иначе Enum::Variant. */
    if (n->callee->kind == NODE_PATH) {
        const char *en = n->callee->path_enum;
        const char *vn = n->callee->path_variant;
        int vhit = -1;
        for (int vi = 0; vi < ctx->n_variants; vi++)
            if (ctx->variants[vi].payload &&
                strcmp(ctx->variants[vi].enum_name, en) == 0 &&
                strcmp(ctx->variants[vi].variant, vn) == 0)
                { vhit = vi; break; }
        if (vhit < 0) {
            check_error(ctx, n->pos,
                "няма payload-вариант '%s::%s'", en, vn);
            return type_new(TYPE_ERROR);
        }
        if (n->args.len != 1) {
            check_error(ctx, n->pos,
                "конструкторът '%s::%s' очаква 1 аргумент, получих %d",
                en, vn, n->args.len);
            return type_new(TYPE_ERROR);
        }
        Type *at = n->args.data[0]->type;
        if (!type_assignable(at, ctx->variants[vhit].payload)) {
            check_error(ctx, n->pos,
                "'%s::%s': аргументът е от тип %s, но payload-ът е %s",
                en, vn, type_str(at), type_str(ctx->variants[vhit].payload));
        }
        Type *t = type_new(TYPE_ENUM);
        t->name = strdup(ctx->variants[vhit].enum_name);
        return t;
    }
    if (n->callee->kind == NODE_IDENT) {
        int vhit = -1, nhit = 0, hit0 = -1;
        for (int vi = 0; vi < ctx->n_variants; vi++) {
            if (!ctx->variants[vi].payload) continue;
            if (strcmp(ctx->variants[vi].variant, n->callee->name) != 0) continue;
            nhit++;
            if (hit0 < 0) hit0 = vi;
        }
        if (nhit > 1) {
            check_error(ctx, n->pos,
                "вариантът '%s' е нееднозначен — ползвай Enum::%s(...) "
                "(напр. %s::%s)",
                n->callee->name, n->callee->name,
                ctx->variants[hit0].enum_name, n->callee->name);
            return type_new(TYPE_ERROR);
        }
        if (nhit == 1) vhit = hit0;
        if (vhit >= 0) {
            if (n->args.len != 1) {
                check_error(ctx, n->pos,
                    "конструкторът '%s' очаква 1 аргумент, получих %d",
                    n->callee->name, n->args.len);
                return type_new(TYPE_ERROR);
            }
            Type *at = n->args.data[0]->type;
            if (!type_assignable(at, ctx->variants[vhit].payload)) {
                check_error(ctx, n->pos,
                    "'%s': аргументът е от тип %s, но payload-ът е %s",
                    n->callee->name, type_str(at),
                    type_str(ctx->variants[vhit].payload));
            }
            Type *t = type_new(TYPE_ENUM);
            t->name = strdup(ctx->variants[vhit].enum_name);
            return t;
        }
    }

    /* builtins */
    if (n->callee->kind == NODE_IDENT) {
        const char *name = n->callee->name;

        /* user-defined (incl. extern) functions shadow builtins.
         * L6: късото име може да идва от няколко модула — печели дефиницията
         * от модула на САМИЯ call site (вкл. главния файл), после
         * единственият друг модул; иначе е нееднозначно и искаме уточнение
         * 'модул.функция'.
         * Forward декларация + реализация (fmrbaga override идиомът):
         * ТИПЪт (ефектите-контракт) идва от декларацията без тяло —
         * историческото first-wins поведение; СИМВОЛЪТ — от тялото.
         * Нееднозначността се мери само между кандидати с тяло. */
        Type *ft_user = NULL;
        {
            const char *own = mod_base(n->pos.file);
            int first = -1, ownidx = -1, fwd = -1, first_body = -1;
            const char *o1 = NULL, *o2 = NULL;
            for (int i = 0; i < ctx->n_fns; i++) {
                if (strcmp(ctx->fns[i].name, name) != 0 &&
                    strcmp(ctx->fns[i].decl->fn_name, name) != 0) continue;
                if (first < 0) first = i;
                if (!ctx->fns[i].decl->fn_body) {
                    if (fwd < 0) fwd = i;
                    continue;
                }
                if (first_body < 0) first_body = i;
                if (strcmp(ctx->fns[i].origin, own) == 0)
                    ownidx = i;
                if (!o1) o1 = ctx->fns[i].origin;
                else if (!o2 && strcmp(o1, ctx->fns[i].origin) != 0) o2 = ctx->fns[i].origin;
            }
            int chosen = -1;
            if (ownidx >= 0) chosen = ownidx;
            else if (!o2) chosen = first_body >= 0 ? first_body : first;
            else {
                check_error(ctx, n->pos,
                    "нееднозначно извикване на '%s' — има я в модулите '%s' и '%s'; уточни с %s.%s или %s.%s",
                    name, o1, o2, o1, name, o2, name);
                chosen = first_body;  /* грешката е записана; продължаваме проверките */
            }
            if (chosen >= 0) {
                ft_user = (fwd >= 0 && ctx->fns[chosen].decl->fn_body)
                    ? ctx->fns[fwd].fn_type      /* контрактът на декларацията */
                    : ctx->fns[chosen].fn_type;
                /* уникалният вътрешен символ (преименуван при дубликати);
                 * strdup — node_free освобождава callee->name */
                n->callee->name = strdup(ctx->fns[chosen].decl->fn_name);
            }
        }
        if (ft_user && ft_user->kind == TYPE_FN) {
            n->callee->type = ft_user;
            if (n->args.len != ft_user->nparams) {
                check_error(ctx, n->pos, "'%s' очаква %d аргумента, получих %d",
                            name, ft_user->nparams, n->args.len);
            }
            int check_n = n->args.len < ft_user->nparams ? n->args.len : ft_user->nparams;
            for (int i = 0; i < check_n; i++) {
                Type *at = n->args.data[i]->type;
                Type *pt = ft_user->params[i];
                if (!type_assignable(at, pt)) {
                    check_error(ctx, n->pos,
                        "'%s': аргумент #%d е от тип %s, но параметърът е %s",
                        name, i + 1, type_str(at), type_str(ft_user->params[i]));
                } else if (at && pt) {
                    /* Неанотиран Vec/Map аргумент: параметърът фиксира
                     * елементния вид — същата мутация като при vec_push
                     * (env пази същия Type указател, така че фиксирането
                     * стига до всички употреби). Без това един по-късен
                     * vec_get фиксира i64 и codegen-ът чете str памет като
                     * i64 (tplbaga P5). */
                    if (at->kind == TYPE_VEC && pt->kind == TYPE_VEC &&
                        !at->elem && pt->elem)
                        at->elem = pt->elem;
                    if (at->kind == TYPE_MAP && pt->kind == TYPE_MAP) {
                        if (!at->key && pt->key) at->key = pt->key;
                        if (!at->elem && pt->elem) at->elem = pt->elem;
                    }
                }
            }
            Type *ret = ft_user->ret ? ft_user->ret : type_new(TYPE_VOID);
            Type *result = type_new(ret->kind);
            /* Vec<T>: keep the element type so e.g. a fn returning Vec<str>
             * can feed a Vec<str> parameter (the fresh Type has elem NULL,
             * which a later vec_get would otherwise fix to i64) */
            result->elem = ret->elem;
            /* struct: keep the name so a returned struct matches the declared
             * return type / struct parameters (type_eq compares by name) */
            result->name = ret->name;
            /* fn (L5): keep the signature so the returned closure is callable */
            result->ret = ret->ret;
            result->params = ret->params;
            result->nparams = ret->nparams;
            type_merge_effects(result, ret);
            if (ctx->cur_effects)
                type_merge_effects(ctx->cur_effects, ret);
            return result;
        }

        if (strcmp(name, "print") == 0 || strcmp(name, "println") == 0 ||
            strcmp(name, "write") == 0) {
            n->callee->type = type_new(TYPE_VOID);
            return type_new(TYPE_VOID);
        }

        /* типизирани вектори: Vec<T> — елементният тип се фиксира при
         * първия push/set чрез мутация на Type->elem (env пази същия
         * указател, така че фиксирането се разпространява до свързването) */
        if (strcmp(name, "vec_new") == 0) {
            n->callee->type = type_new(TYPE_VOID);
            return type_new(TYPE_VEC);      /* elem = NULL: неизвестен */
        }
        if (strcmp(name, "vec_push") == 0 || strcmp(name, "vec_set") == 0 ||
            strcmp(name, "vec_push_str") == 0 || strcmp(name, "vec_set_str") == 0) {
            int is_str_alias = (strstr(name, "_str") != NULL);
            int xidx = (strstr(name, "set") != NULL) ? 2 : 1;   /* индекс на елемента */
            n->callee->type = type_new(TYPE_VOID);
            if (n->args.len != xidx + 1) {
                check_error(ctx, n->pos, "'%s' очаква %d аргумента, получих %d",
                            name, xidx + 1, n->args.len);
                return type_new(TYPE_ERROR);
            }
            Type *vt = n->args.data[0]->type;
            if (!vt || vt->kind != TYPE_VEC) {
                check_error(ctx, n->pos, "'%s' върху не-вектор (%s)", name, type_str(vt));
                return type_new(TYPE_ERROR);
            }
            Type *xt = is_str_alias ? type_new(TYPE_STR) : n->args.data[xidx]->type;
            if (xt->kind == TYPE_I32) xt = type_new(TYPE_I64);
            if (!is_str_alias && xt->kind != TYPE_I64 && xt->kind != TYPE_STR &&
                xt->kind != TYPE_F64 && xt->kind != TYPE_BYTES &&
                xt->kind != TYPE_STRUCT && xt->kind != TYPE_FN &&
                xt->kind != TYPE_VEC && xt->kind != TYPE_ENUM) {
                check_error(ctx, n->pos,
                    "%s: неподдържан елементен тип %s за Vec (поддържат се i64, str, f64, bytes, struct, sum enum, fn и Vec)",
                    name, type_str(xt));
                return type_new(TYPE_ERROR);
            }
            if (!vt->elem) {
                vt->elem = xt;
            } else if (!vec_elem_eq(vt->elem, xt)) {
                check_error(ctx, n->pos, "%s: елемент от тип %s, но векторът е %s",
                            name, type_str(xt), type_str(vt));
            }
            return type_new(TYPE_VOID);
        }
        if (strcmp(name, "vec_get") == 0 || strcmp(name, "vec_get_str") == 0) {
            int is_str_alias = (strstr(name, "_str") != NULL);
            n->callee->type = type_new(TYPE_VOID);
            Type *vt = n->args.len > 0 ? n->args.data[0]->type : NULL;
            if (is_str_alias) {
                if (vt && vt->kind == TYPE_VEC && !vt->elem) vt->elem = type_new(TYPE_STR);
                return type_new(TYPE_STR);
            }
            if (vt && vt->kind == TYPE_VEC) {
                /* Vec с неизвестен елемент: vec_get исторически е само за i64,
                 * фиксираме i64 (стар код с Vec параметри не се чупи) */
                if (!vt->elem) vt->elem = type_new(TYPE_I64);
                return vt->elem;
            }
            check_error(ctx, n->pos, "'%s' върху не-вектор (%s)", name, type_str(vt));
            return type_new(TYPE_ERROR);
        }
        if (strcmp(name, "vec_len") == 0) {
            n->callee->type = type_new(TYPE_VOID);
            return type_new(TYPE_I64);
        }
        if (strcmp(name, "vec_slice") == 0) {
            n->callee->type = type_new(TYPE_VOID);
            Type *vt = n->args.len > 0 ? n->args.data[0]->type : NULL;
            if (!vt || vt->kind != TYPE_VEC) {
                check_error(ctx, n->pos, "'vec_slice' върху не-вектор (%s)", type_str(vt));
                return type_new(TYPE_ERROR);
            }
            Type *r = type_new(TYPE_VEC);
            r->elem = vt->elem;
            return r;
        }
        if (strcmp(name, "vec_concat") == 0) {
            n->callee->type = type_new(TYPE_VOID);
            Type *vt = n->args.len > 0 ? n->args.data[0]->type : NULL;
            Type *wt = n->args.len > 1 ? n->args.data[1]->type : NULL;
            if (!vt || vt->kind != TYPE_VEC || !wt || wt->kind != TYPE_VEC) {
                check_error(ctx, n->pos, "'vec_concat' очаква два вектора");
                return type_new(TYPE_ERROR);
            }
            Type *r = type_new(TYPE_VEC);
            r->elem = vt->elem ? vt->elem : wt->elem;
            return r;
        }

        /* карти: Map<K, V> — ключ i64/str, стойност i64/str/f64/bytes.
         * Чист Map фиксира ключа/стойността при първия map_set —
         * същият механизъм като Vec (env пази същия Type указател). */
        if (strcmp(name, "map_new") == 0) {
            n->callee->type = type_new(TYPE_VOID);
            if (n->args.len != 0) {
                check_error(ctx, n->pos, "'map_new' очаква 0 аргумента, получих %d",
                            n->args.len);
                return type_new(TYPE_ERROR);
            }
            return type_new(TYPE_MAP);      /* key/elem = NULL: неизвестни */
        }
        /* R66: unsafe bytes handle casts — zero-copy hop of binary values
         * across chan(i64) (PARALLEL workers). Boxes a baga_bytes header on
         * the arena; payload data is already arena-bound. */
        if (strcmp(name, "bytes_h") == 0) {
            n->callee->type = type_new(TYPE_VOID);
            if (n->args.len != 1) {
                check_error(ctx, n->pos, "'bytes_h' очаква 1 аргумент, получих %d",
                            n->args.len);
                return type_new(TYPE_ERROR);
            }
            Type *bt = n->args.data[0]->type;
            if (bt && bt->kind != TYPE_BYTES && bt->kind != TYPE_ERROR) {
                check_error(ctx, n->pos, "'bytes_h' очаква bytes, получих %s",
                            type_str(bt));
                return type_new(TYPE_ERROR);
            }
            return type_new(TYPE_I64);
        }
        if (strcmp(name, "h_bytes") == 0) {
            n->callee->type = type_new(TYPE_VOID);
            if (n->args.len != 1) {
                check_error(ctx, n->pos, "'h_bytes' очаква 1 аргумент, получих %d",
                            n->args.len);
                return type_new(TYPE_ERROR);
            }
            return type_new(TYPE_BYTES);
        }
        /* R55: unsafe map handle casts — предаване на споделена карта през
         * i64 контекст на go_bg (като str_h/h_str, R51). Map е baga_Map*
         * (heap, arena-ish живот) — handle-ът е валиден за живота на процеса. */
        if (strcmp(name, "map_h") == 0) {
            n->callee->type = type_new(TYPE_VOID);
            if (n->args.len != 1) {
                check_error(ctx, n->pos, "'map_h' очаква 1 аргумент, получих %d",
                            n->args.len);
                return type_new(TYPE_ERROR);
            }
            return type_new(TYPE_I64);
        }
        if (strcmp(name, "h_map") == 0) {
            n->callee->type = type_new(TYPE_VOID);
            if (n->args.len != 1) {
                check_error(ctx, n->pos, "'h_map' очаква 1 аргумент, получих %d",
                            n->args.len);
                return type_new(TYPE_ERROR);
            }
            return type_new(TYPE_MAP);      /* key/elem = NULL: неизвестни */
        }
        if (strcmp(name, "map_set") == 0) {
            n->callee->type = type_new(TYPE_VOID);
            if (n->args.len != 3) {
                check_error(ctx, n->pos,
                    "'map_set' очаква 3 аргумента (карта, ключ, стойност), получих %d",
                    n->args.len);
                return type_new(TYPE_ERROR);
            }
            Type *mt = n->args.data[0]->type;
            if (!mt || mt->kind != TYPE_MAP) {
                check_error(ctx, n->pos, "'map_set' върху не-карта (%s)", type_str(mt));
                return type_new(TYPE_ERROR);
            }
            Type *kt = n->args.data[1]->type;
            if (kt && kt->kind == TYPE_I32) kt = type_new(TYPE_I64);
            if (!kt || (kt->kind != TYPE_I64 && kt->kind != TYPE_STR &&
                        kt->kind != TYPE_BYTES && kt->kind != TYPE_ERROR)) {
                check_error(ctx, n->pos,
                    "map_set: неподдържан ключов тип %s за Map (поддържат се i64, str и bytes)",
                    type_str(kt));
                return type_new(TYPE_ERROR);
            }
            Type *vt = n->args.data[2]->type;
            if (vt && vt->kind == TYPE_I32) vt = type_new(TYPE_I64);
            if (!vt || (vt->kind != TYPE_I64 && vt->kind != TYPE_STR &&
                        vt->kind != TYPE_F64 && vt->kind != TYPE_BYTES &&
                        vt->kind != TYPE_STRUCT && vt->kind != TYPE_FN &&
                        vt->kind != TYPE_ENUM &&
                        vt->kind != TYPE_ERROR)) {
                check_error(ctx, n->pos,
                    "map_set: неподдържан стойностен тип %s за Map (поддържат се i64, str, f64, bytes, struct, sum enum и fn)",
                    type_str(vt));
                return type_new(TYPE_ERROR);
            }
            if (kt->kind != TYPE_ERROR) {
                if (!mt->key) {
                    mt->key = kt;
                } else if (mt->key->kind != kt->kind) {
                    check_error(ctx, n->pos, "map_set: ключ от тип %s, но картата е %s",
                        type_str(kt), type_str(mt));
                }
            }
            if (vt->kind != TYPE_ERROR) {
                if (!mt->elem) {
                    mt->elem = vt;
                } else if (!vec_elem_eq(mt->elem, vt)) {
                    check_error(ctx, n->pos, "map_set: стойност от тип %s, но картата е %s",
                        type_str(vt), type_str(mt));
                }
            }
            return type_new(TYPE_VOID);
        }
        if (strcmp(name, "map_get") == 0 || strcmp(name, "map_has") == 0 ||
            strcmp(name, "map_del") == 0) {
            n->callee->type = type_new(TYPE_VOID);
            if (n->args.len != 2) {
                check_error(ctx, n->pos, "'%s' очаква 2 аргумента (карта, ключ), получих %d",
                            name, n->args.len);
                return type_new(TYPE_ERROR);
            }
            Type *mt = n->args.data[0]->type;
            if (!mt || mt->kind != TYPE_MAP) {
                check_error(ctx, n->pos, "'%s' върху не-карта (%s)", name, type_str(mt));
                return type_new(TYPE_ERROR);
            }
            Type *kt = n->args.data[1]->type;
            if (kt && kt->kind == TYPE_I32) kt = type_new(TYPE_I64);
            if (kt && kt->kind != TYPE_ERROR) {
                if (kt->kind != TYPE_I64 && kt->kind != TYPE_STR &&
                    kt->kind != TYPE_BYTES) {
                    check_error(ctx, n->pos,
                        "%s: неподдържан ключов тип %s за Map (поддържат се i64, str и bytes)",
                        name, type_str(kt));
                    return type_new(TYPE_ERROR);
                }
                /* чист Map: ключът се фиксира от първата употреба (като Vec) */
                if (!mt->key) {
                    mt->key = kt;
                } else if (mt->key->kind != kt->kind) {
                    check_error(ctx, n->pos, "%s: ключ от тип %s, но картата е %s",
                        name, type_str(kt), type_str(mt));
                }
            }
            if (strcmp(name, "map_has") == 0) return type_new(TYPE_I64);
            if (strcmp(name, "map_del") == 0) return type_new(TYPE_VOID);
            /* map_get: неизвестна стойност → i64 (исторически, като vec_get) */
            if (!mt->elem) mt->elem = type_new(TYPE_I64);
            return mt->elem;
        }
        if (strcmp(name, "map_len") == 0) {
            n->callee->type = type_new(TYPE_VOID);
            return type_new(TYPE_I64);
        }
        if (strcmp(name, "map_keys") == 0) {
            n->callee->type = type_new(TYPE_VOID);
            Type *mt = n->args.len > 0 ? n->args.data[0]->type : NULL;
            if (!mt || mt->kind != TYPE_MAP) {
                check_error(ctx, n->pos, "'map_keys' върху не-карта (%s)", type_str(mt));
                return type_new(TYPE_ERROR);
            }
            /* чист Map: ключовете са str по подразбиране (като vec_get → i64) */
            if (!mt->key) mt->key = type_new(TYPE_STR);
            Type *r = type_new(TYPE_VEC);
            r->elem = mt->key;
            return r;
        }

        /* go / go_bg(fn, arg) — spawn OS thread; fn is a bare function identifier */
        if (strcmp(name, "go") == 0 || strcmp(name, "go_bg") == 0) {
            n->callee->type = type_new(TYPE_VOID);
            if (n->args.len != 2) {
                check_error(ctx, n->pos, "'%s' очаква 2 аргумента (fn, arg), получих %d",
                            name, n->args.len);
                return type_new(TYPE_ERROR);
            }
            Node *fnarg = n->args.data[0];
            if (fnarg->kind != NODE_IDENT) {
                check_error(ctx, n->pos, "'%s': първият аргумент трябва да е име на функция", name);
                return type_new(TYPE_ERROR);
            }
            Type *wft = find_fn(ctx, fnarg->name);
            if (!wft || wft->kind != TYPE_FN) {
                check_error(ctx, n->pos, "'%s': '%s' не е функция", name, fnarg->name);
                return type_new(TYPE_ERROR);
            }
            if (wft->nparams != 1 || !wft->params[0] || wft->params[0]->kind != TYPE_I64) {
                check_error(ctx, n->pos,
                    "'%s': worker '%s' трябва да е fn(i64) -> i64", name, fnarg->name);
                return type_new(TYPE_ERROR);
            }
            if (!wft->ret || wft->ret->kind != TYPE_I64) {
                check_error(ctx, n->pos,
                    "'%s': worker '%s' трябва да връща i64", name, fnarg->name);
                return type_new(TYPE_ERROR);
            }
            Type *at = n->args.data[1]->type;
            if (at && at->kind != TYPE_I64 && at->kind != TYPE_ERROR) {
                check_error(ctx, n->pos, "'%s': arg е %s, очаквах i64", name, type_str(at));
            }
            Type *ret = type_new(TYPE_I64);
            type_add_effect(ret, "Par");
            /* worker effects bubble to the spawn site (cloud handlers may do !IO) */
            if (wft->ret) type_merge_effects(ret, wft->ret);
            if (ctx->cur_effects) {
                type_add_effect(ctx->cur_effects, "Par");
                if (wft->ret) type_merge_effects(ctx->cur_effects, wft->ret);
            }
            return ret;
        }

        /* pool_map(fn, vec, nworkers) -> Vec<i64> !Par — bounded parallel map */
        if (strcmp(name, "pool_map") == 0) {
            n->callee->type = type_new(TYPE_VOID);
            if (n->args.len != 3) {
                check_error(ctx, n->pos,
                    "'pool_map' очаква 3 аргумента (fn, vec, nworkers), получих %d",
                    n->args.len);
                return type_new(TYPE_ERROR);
            }
            Node *fnarg = n->args.data[0];
            if (fnarg->kind != NODE_IDENT) {
                check_error(ctx, n->pos, "'pool_map': първият аргумент трябва да е име на функция");
                return type_new(TYPE_ERROR);
            }
            Type *wft = find_fn(ctx, fnarg->name);
            if (!wft || wft->kind != TYPE_FN) {
                check_error(ctx, n->pos, "'pool_map': '%s' не е функция", fnarg->name);
                return type_new(TYPE_ERROR);
            }
            if (wft->nparams != 1 || !wft->params[0] || wft->params[0]->kind != TYPE_I64) {
                check_error(ctx, n->pos,
                    "'pool_map': worker '%s' трябва да е fn(i64) -> i64", fnarg->name);
                return type_new(TYPE_ERROR);
            }
            if (!wft->ret || wft->ret->kind != TYPE_I64) {
                check_error(ctx, n->pos,
                    "'pool_map': worker '%s' трябва да връща i64", fnarg->name);
                return type_new(TYPE_ERROR);
            }
            Type *vt = n->args.data[1]->type;
            if (!vt || vt->kind != TYPE_VEC) {
                check_error(ctx, n->pos, "'pool_map': вторият аргумент трябва да е Vec");
                return type_new(TYPE_ERROR);
            }
            if (vt->elem && vt->elem->kind != TYPE_I64 && vt->elem->kind != TYPE_ERROR) {
                check_error(ctx, n->pos, "'pool_map': Vec елементите трябва да са i64");
            }
            Type *nt = n->args.data[2]->type;
            if (nt && nt->kind != TYPE_I64 && nt->kind != TYPE_ERROR) {
                check_error(ctx, n->pos, "'pool_map': nworkers е %s, очаквах i64", type_str(nt));
            }
            Type *ret = type_new(TYPE_VEC);
            ret->elem = type_new(TYPE_I64);
            type_add_effect(ret, "Par");
            if (wft->ret) type_merge_effects(ret, wft->ret);
            if (ctx->cur_effects) {
                type_add_effect(ctx->cur_effects, "Par");
                if (wft->ret) type_merge_effects(ctx->cur_effects, wft->ret);
            }
            return ret;
        }

        /* string / io / par builtins. sig: параметри по позиции —
         * 's' str, 'i' i64 (bool се приема), 'f' f64, 'b' bytes, 'a' всякакъв.
         * Преди LP3 таблицата носеше само return тип: грешен тип аргумент
         * (напр. ord(65)) стигаше до C и гърмеше runtime (segfault). */
        struct { const char *name; TypeKind ret; const char *sig; int has_io; int has_par; } builtins[] = {
            {"len",       TYPE_I64, "s", 0, 0},
            {"char_at",   TYPE_I64, "si", 0, 0},
            {"byte_at",   TYPE_I64, "si", 0, 0},
            {"byte_chr",  TYPE_STR, "i", 0, 0},
            {"substr",    TYPE_STR, "sii", 0, 0},
            {"concat",    TYPE_STR, "ss", 0, 0},
            {"read_file", TYPE_STR, "s", 1, 0},
            {"chr",       TYPE_STR, "i", 0, 0},
            {"ord",       TYPE_I64, "s", 0, 0},
            {"i64_to_str", TYPE_STR, "i", 0, 0},
            {"f64_to_str", TYPE_STR, "f", 0, 0},
            {"str_eq",    TYPE_BOOL, "ss", 0, 0},
            {"arg_count", TYPE_I64, "", 0, 0},
            {"arg",       TYPE_STR, "i", 0, 0},
            {"exit",      TYPE_VOID, "i", 0, 0},
            {"eprintln",  TYPE_VOID, "s", 0, 0},
            {"arena_new",   TYPE_I64, "", 0, 0},
            {"arena_alloc", TYPE_I64, "ii", 0, 0},
            {"arena_reset", TYPE_VOID, "i", 0, 0},
            {"arena_free",  TYPE_VOID, "i", 0, 0},
            {"mem_mark",    TYPE_I64, "", 0, 0},
            {"mem_rewind",  TYPE_VOID, "i", 0, 0},
            {"rc_on",       TYPE_BOOL, "", 0, 0},
            {"mem_persist_begin", TYPE_VOID, "", 0, 0},
            {"mem_persist_end",   TYPE_VOID, "", 0, 0},
            {"bytes_len",   TYPE_I64, "b", 0, 0},
            {"bytes_at",    TYPE_I64, "bi", 0, 0},
            {"bytes_slice", TYPE_BYTES, "bii", 0, 0},
            {"bytes_concat", TYPE_BYTES, "bb", 0, 0},
            {"bytes_new",   TYPE_BYTES, "i", 0, 0},
            {"bytes_set",   TYPE_VOID, "bii", 0, 0},
            {"bytes_put",   TYPE_VOID, "bib", 0, 0}, /* R54: dst[off..+len)=src memcpy */
            {"bytes_push",  TYPE_BYTES, "bi", 0, 0},
            {"bytes_of_str", TYPE_BYTES, "s", 0, 0},
            {"str_of_bytes", TYPE_STR, "b", 0, 0},
            {"hex_encode",  TYPE_STR, "b", 0, 0},
            {"hex_decode",  TYPE_BYTES, "s", 0, 0},
            /* concurrency (!Par) — cloud-native fan-out / CSP channels */
            {"join",        TYPE_I64, "i", 0, 1},
            {"detach",      TYPE_I64, "i", 0, 1},
            {"chan_new",    TYPE_I64, "i", 0, 1},
            {"chan_send",   TYPE_I64, "ii", 0, 1},
            {"chan_recv",   TYPE_I64, "i", 0, 1},
            {"chan_recv2",  TYPE_I64, "i", 0, 1}, /* cell2(ok, value) */
            {"chan_try_recv", TYPE_I64, "i", 0, 1}, /* cell2(status, value) */
            {"chan_recv_timeout", TYPE_I64, "ii", 0, 1},
            {"chan_select2", TYPE_I64, "ii", 0, 1}, /* cell2(which, value) */
            {"chan_select2_wait", TYPE_I64, "ii", 0, 1},
            {"chan_select2_timeout", TYPE_I64, "iii", 0, 1},
            {"chan_close",  TYPE_I64, "i", 0, 1},
            {"chan_len",    TYPE_I64, "i", 0, 1},
            {"sleep_ms",    TYPE_I64, "i", 0, 1},
            {"mutex_new",   TYPE_I64, "", 0, 1},
            {"mutex_lock",  TYPE_I64, "i", 0, 1},
            {"mutex_unlock",TYPE_I64, "i", 0, 1},
            /* C1 signals — graceful shutdown (K8s SIGTERM); process-global */
            {"signal_watch", TYPE_I64, "i", 0, 0}, /* install handler; 0 ok, -1 err */
            {"signal_check", TYPE_I64, "", 0, 0}, /* 0 = none, else signo */
            {"signal_clear", TYPE_I64, "", 0, 0}, /* return+clear pending */
            {"signal_wait",  TYPE_I64, "i", 0, 0}, /* ms; <0 = forever; return signo or 0 */
            {"signal_raise", TYPE_I64, "i", 0, 0}, /* raise(sig) to self; 0 ok */
            /* heap pair — pure context packing for single-arg go workers */
            {"cell2",       TYPE_I64, "aa", 0, 0},
            {"cell2_0",     TYPE_I64, "a", 0, 0},
            {"cell2_1",     TYPE_I64, "a", 0, 0},
            /* R51: unsafe str handle casts — zero-copy chan hop (C backend) */
            {"str_h",       TYPE_I64, "s", 0, 0},
            {"h_str",       TYPE_STR, "i", 0, 0},
            /* R66: also in special-case above for TYPE_BYTES return of h_bytes */
            {"bytes_h",     TYPE_I64, "b", 0, 0},
            {"h_bytes",     TYPE_BYTES, "i", 0, 0},
        };
        /* bytes_from_vec / vec_from_bytes — typed bridge (not in the flat table) */
        if (strcmp(name, "bytes_from_vec") == 0) {
            n->callee->type = type_new(TYPE_VOID);
            Type *vt = n->args.len > 0 ? n->args.data[0]->type : NULL;
            if (!vt || vt->kind != TYPE_VEC)
                check_error(ctx, n->pos, "'bytes_from_vec' очаква Vec");
            return type_new(TYPE_BYTES);
        }
        if (strcmp(name, "vec_from_bytes") == 0) {
            n->callee->type = type_new(TYPE_VOID);
            Type *bt = n->args.len > 0 ? n->args.data[0]->type : NULL;
            if (!bt || bt->kind != TYPE_BYTES)
                check_error(ctx, n->pos, "'vec_from_bytes' очаква bytes");
            Type *r = type_new(TYPE_VEC);
            r->elem = type_new(TYPE_I64);
            return r;
        }
        /* MEM-1: drop(x) — само let-локални Vec/Map/bytes/fn; MEM-2: пълен seatbelt */
        if (strcmp(name, "drop") == 0 &&
            !env_lookup(ctx, "drop") && !find_fn(ctx, "drop")) {
            if (n->args.len != 1) {
                check_error(ctx, n->pos, "drop очаква 1 аргумент, получих %d", n->args.len);
                return type_new(TYPE_ERROR);
            }
            Node *a = n->args.data[0];
            Type *at = a->type;
            if (a->kind != NODE_IDENT) {
                check_error(ctx, n->pos, "drop очаква локална променлива (let), не израз");
                return type_new(TYPE_ERROR);
            }
            if (!at || (at->kind != TYPE_VEC && at->kind != TYPE_MAP &&
                        at->kind != TYPE_BYTES && at->kind != TYPE_FN)) {
                check_error(ctx, n->pos, "drop: неподдържан тип %s — drop е за Vec/Map/bytes/fn",
                            at ? type_str(at) : "?");
                return type_new(TYPE_ERROR);
            }
            /* MEM-2: live/dropped правила върху EnvEntry */
            EnvEntry *e = env_find(ctx, a->name);
            if (!e) {
                check_error(ctx, n->pos, "drop: '%s' не е локална променлива", a->name);
                return type_new(TYPE_ERROR);
            }
            if (e->dropped) {
                check_error(ctx, n->pos, "повторен drop на '%s'", a->name);
                return type_new(TYPE_ERROR);
            }
            if (e->is_param) {
                check_error(ctx, n->pos,
                    "drop на параметър '%s' — параметрите споделят буфера на извикващия",
                    a->name);
                return type_new(TYPE_ERROR);
            }
            if (e->captured) {
                check_error(ctx, n->pos,
                    "'%s' е заснет от ламбда — drop би оставил висящ указател", a->name);
                return type_new(TYPE_ERROR);
            }
            /* MEM-2: loop_depth е depth-ът на нивото на loop-израза; променлива,
             * дефинирана на него или по-навън (scope_depth <= loop_depth), е
             * "външна за цикъла" — drop в тялото = use-after-drop на итерация 2 */
            if (ctx->loop_depth && e->scope_depth <= ctx->loop_depth) {
                check_error(ctx, n->pos,
                    "drop на външна за цикъла променлива '%s' — втората итерация би била use-after-drop",
                    a->name);
                return type_new(TYPE_ERROR);
            }
            if (ctx->n_drop_log < 256) {
                e->dropped = 1;
                ctx->drop_log[ctx->n_drop_log++] = e;
            } else {
                /* логът е пълен — НЕ маркираме: консервативно "пропусната
                 * грешка", никога измислена (runtime drop-ът пак се генерира);
                 * join-ът на обграждащия if ще вдигне и логнатите от клоните */
                ctx->drop_log_overflowed = 1;
            }
            return type_new(TYPE_VOID);
        }
        /* MEM-3: arena_free / arena_alloc / arena_reset seatbelt on the
         * arena handle (i64 local). Reuses EnvEntry.dropped + drop_log join
         * machinery — after arena_free(a), a is dead; double free and
         * alloc/reset on a freed handle are compile errors. Full region
         * tagging of arena-allocated payloads is not claimed (values stay
         * untracked pointers). */
        if ((strcmp(name, "arena_free") == 0 || strcmp(name, "arena_alloc") == 0 ||
             strcmp(name, "arena_reset") == 0) &&
            !env_lookup(ctx, name) && !find_fn(ctx, name)) {
            int is_free = strcmp(name, "arena_free") == 0;
            int is_alloc = strcmp(name, "arena_alloc") == 0;
            int need = is_alloc ? 2 : 1;
            if (n->args.len != need) {
                check_error(ctx, n->pos, "'%s' очаква %d аргумент(а), получих %d",
                            name, need, n->args.len);
                return type_new(TYPE_ERROR);
            }
            Node *ah = n->args.data[0];
            if (ah->kind != NODE_IDENT) {
                if (is_free)
                    check_error(ctx, n->pos,
                        "arena_free очаква локална променлива (let), не израз");
                /* alloc/reset on non-ident: still type-check as i64 below */
            } else {
                EnvEntry *e = env_find(ctx, ah->name);
                if (e) {
                    if (e->dropped) {
                        if (is_free)
                            check_error(ctx, n->pos,
                                "повторен arena_free на '%s'", ah->name);
                        else
                            check_error(ctx, n->pos,
                                "използване на арена '%s' след arena_free", ah->name);
                    } else if (is_free) {
                        if (e->is_param)
                            check_error(ctx, n->pos,
                                "arena_free на параметър '%s' — собствеността е на извикващия",
                                ah->name);
                        else if (ctx->loop_depth && e->scope_depth <= ctx->loop_depth)
                            check_error(ctx, n->pos,
                                "arena_free на външна за цикъла арена '%s'", ah->name);
                        else {
                            /* invalidate all arena_alloc results from this handle first */
                            mem3_invalidate_region(ctx, e);
                            if (ctx->n_drop_log < 256) {
                                e->dropped = 1;
                                ctx->drop_log[ctx->n_drop_log++] = e;
                            } else {
                                ctx->drop_log_overflowed = 1;
                            }
                        }
                    }
                }
            }
            n->callee->type = type_new(TYPE_VOID);
            if (is_alloc) return type_new(TYPE_I64);
            return type_new(TYPE_VOID);
        }
        for (int bi = 0; bi < (int)(sizeof(builtins) / sizeof(builtins[0])); bi++) {
            if (strcmp(name, builtins[bi].name) == 0) {
                n->callee->type = type_new(TYPE_VOID);
                /* LP3: arity + типови подпис — преди това грешен тип
                 * аргумент стигаше до C и гърмеше runtime (segfault). */
                const char *sig = builtins[bi].sig;
                int want = (int)strlen(sig);
                if (n->args.len != want) {
                    check_error(ctx, n->pos,
                        "'%s' очаква %d аргумент(а), получих %d",
                        name, want, n->args.len);
                    return type_new(TYPE_ERROR);
                }
                for (int ai = 0; ai < want; ai++) {
                    char wk = sig[ai];
                    if (wk == 'a') continue;
                    Type *at = n->args.data[ai]->type;
                    if (!at || at->kind == TYPE_ERROR) continue;
                    int ok = 0;
                    if (wk == 's')      ok = at->kind == TYPE_STR;
                    else if (wk == 'i') ok = at->kind == TYPE_I64 || at->kind == TYPE_BOOL;
                    else if (wk == 'f') ok = at->kind == TYPE_F64;
                    else if (wk == 'b') ok = at->kind == TYPE_BYTES;
                    if (!ok) {
                        check_error(ctx, n->pos,
                            "'%s': аргумент #%d е от тип %s, но параметърът е %s",
                            name, ai + 1, type_str(at),
                            wk == 's' ? "str" : wk == 'i' ? "i64" :
                            wk == 'f' ? "f64" : "bytes");
                        return type_new(TYPE_ERROR);
                    }
                }
                Type *ret = type_new(builtins[bi].ret);
                if (builtins[bi].has_io) {
                    type_add_effect(ret, "IO");
                    if (ctx->cur_effects) type_add_effect(ctx->cur_effects, "IO");
                }
                if (builtins[bi].has_par) {
                    type_add_effect(ret, "Par");
                    if (ctx->cur_effects) type_add_effect(ctx->cur_effects, "Par");
                }
                return ret;
            }
        }

        /* sorted(v) — relational annotation predicate (verifier-only) */
        if (strcmp(name, "sorted") == 0) {
            n->callee->type = type_new(TYPE_VOID);
            Type *vt = n->args.len > 0 ? n->args.data[0]->type : NULL;
            if (!vt || vt->kind != TYPE_VEC)
                check_error(ctx, n->pos, "'sorted' очаква вектор");
            return type_new(TYPE_BOOL);
        }

        /* L5: локална, която не е fn стойност — по-ясна диагностика */
        Type *lt_local = env_lookup(ctx, name);
        if (lt_local)
            check_error(ctx, n->pos, "извикване на не-функция '%s' (%s)",
                        name, type_str(lt_local));
        else
            check_error(ctx, n->pos, "непозната функция '%s'", name);
        return type_new(TYPE_ERROR);
    }

    infer(ctx, n->callee);
    return type_new(TYPE_ERROR);
}

static Type *infer(CheckCtx *ctx, Node *n) {
    if (!n) return type_new(TYPE_VOID);
    if (n->type) return n->type;  /* already inferred */

    Type *t = NULL;

    switch (n->kind) {
        case NODE_INT_LIT:
            t = type_new(TYPE_I64);
            break;

        case NODE_FLOAT_LIT:
            t = type_new(TYPE_F64);
            break;

        case NODE_STR_LIT:
            t = type_new(TYPE_STR);
            break;

        case NODE_BYTES_LIT:
            t = type_new(TYPE_BYTES);
            break;

        case NODE_BOOL_LIT:
            t = type_new(TYPE_BOOL);
            break;

        case NODE_PATH: {
            /* A1: Enum::Variant bare (payload-less) or error if payload required */
            const char *en = n->path_enum;
            const char *vn = n->path_variant;
            int vhit = -1;
            for (int vi = 0; vi < ctx->n_variants; vi++)
                if (strcmp(ctx->variants[vi].enum_name, en) == 0 &&
                    strcmp(ctx->variants[vi].variant, vn) == 0)
                    { vhit = vi; break; }
            if (vhit < 0) {
                check_error(ctx, n->pos, "няма вариант '%s::%s'", en, vn);
                t = type_new(TYPE_ERROR);
                break;
            }
            if (ctx->variants[vhit].payload) {
                check_error(ctx, n->pos,
                    "конструкторът '%s::%s' изисква 1 аргумент (%s)",
                    en, vn, type_str(ctx->variants[vhit].payload));
                t = type_new(TYPE_ERROR);
                break;
            }
            Node *ed = NULL;
            for (int ei = 0; ei < ctx->n_enums; ei++)
                if (strcmp(ctx->enums[ei].name, en) == 0)
                    { ed = ctx->enums[ei].decl; break; }
            if (ed && ed->type && ed->type->kind == TYPE_ENUM) {
                t = type_new(TYPE_ENUM);
                t->name = strdup(en);
            } else {
                t = type_new(TYPE_I64);
            }
            break;
        }

        case NODE_IDENT: {
            /* check local env first */
            Type *vt = env_lookup(ctx, n->name);
            if (vt) {
                /* MEM-2: use-after-drop (error recovery — продължаваме проверката) */
                EnvEntry *e = env_find(ctx, n->name);
                if (e && e->dropped)
                    check_error(ctx, n->pos,
                        "използване на '%s' след free", n->name);
                t = vt; break;
            }
            /* check function registry */
            Type *ft = find_fn(ctx, n->name);
            if (ft) {
                /* L5: fn като стойност — codegen-ът взема адреса на wrapper-а
                 * по ПЪЛНОТО (евент. преименувано) име; при дубликати от
                 * няколко модула печели собственият модул, иначе грешка */
                int first = -1, ownidx = -1;
                const char *o1 = NULL, *o2 = NULL;
                const char *own = mod_base(n->pos.file);
                for (int i = 0; i < ctx->n_fns; i++) {
                    if (strcmp(ctx->fns[i].name, n->name) != 0) continue;
                    if (first < 0) first = i;
                    if (strcmp(ctx->fns[i].origin, own) == 0) ownidx = i;
                    if (!o1) o1 = ctx->fns[i].origin;
                    else if (!o2 && strcmp(o1, ctx->fns[i].origin) != 0) o2 = ctx->fns[i].origin;
                }
                int chosen = ownidx >= 0 ? ownidx : first;
                if (o2 && ownidx < 0) {
                    check_error(ctx, n->pos,
                        "нееднозначна fn референция '%s' — уточни с %s.%s или %s.%s",
                        n->name, o1, n->name, o2, n->name);
                }
                Type *vt2 = type_fn(ft->ret, ft->params, ft->nparams);
                vt2->name = ctx->fns[chosen].decl->fn_name;
                n->name = strdup(ctx->fns[chosen].decl->fn_name);  /* codegen: strcmp с type->name */
                t = vt2;
                break;
            }
            /* bare enum variants (A1: unique among sum payload-less / plain) */
            {
                int nhit = 0, hit0 = -1;
                for (int vi = 0; vi < ctx->n_variants; vi++) {
                    if (strcmp(ctx->variants[vi].variant, n->name) != 0) continue;
                    nhit++;
                    if (hit0 < 0) hit0 = vi;
                }
                if (nhit > 1) {
                    /* prefer reporting only sum-enum collisions for bare form */
                    int sum_hits = 0, sum0 = -1;
                    for (int vi = 0; vi < ctx->n_variants; vi++) {
                        if (strcmp(ctx->variants[vi].variant, n->name) != 0) continue;
                        Node *ed = NULL;
                        for (int ei = 0; ei < ctx->n_enums; ei++)
                            if (strcmp(ctx->enums[ei].name, ctx->variants[vi].enum_name) == 0)
                                { ed = ctx->enums[ei].decl; break; }
                        if (ed && ed->type && ed->type->kind == TYPE_ENUM) {
                            sum_hits++;
                            if (sum0 < 0) sum0 = vi;
                        }
                    }
                    if (sum_hits > 1) {
                        check_error(ctx, n->pos,
                            "вариантът '%s' е нееднозначен — ползвай Enum::%s "
                            "(напр. %s::%s)",
                            n->name, n->name,
                            ctx->variants[sum0].enum_name, n->name);
                        t = type_new(TYPE_ERROR);
                        break;
                    }
                    if (sum_hits == 1) hit0 = sum0, nhit = 1;
                }
                if (nhit >= 1 && hit0 >= 0) {
                    if (ctx->variants[hit0].payload) {
                        check_error(ctx, n->pos,
                            "конструкторът '%s' изисква 1 аргумент (%s)",
                            n->name, type_str(ctx->variants[hit0].payload));
                        t = type_new(TYPE_ERROR);
                    } else {
                        Node *ed = NULL;
                        for (int ei = 0; ei < ctx->n_enums; ei++)
                            if (strcmp(ctx->enums[ei].name, ctx->variants[hit0].enum_name) == 0)
                                { ed = ctx->enums[ei].decl; break; }
                        if (ed && ed->type && ed->type->kind == TYPE_ENUM) {
                            t = type_new(TYPE_ENUM);
                            t->name = strdup(ctx->variants[hit0].enum_name);
                        } else {
                            t = type_new(TYPE_I64);
                        }
                    }
                    break;
                }
            }
            if (t) break;
            /* builtins */
            if (strcmp(n->name, "print") == 0 || strcmp(n->name, "println") == 0) {
                t = type_new(TYPE_VOID);
                break;
            }
            check_error(ctx, n->pos, "недефинирана променлива '%s'", n->name);
            t = type_new(TYPE_ERROR);
            break;
        }

        case NODE_BINARY:
            t = infer_binary(ctx, n);
            break;

        case NODE_UNARY:
            t = infer(ctx, n->operand);
            if (n->un_op == UOP_NOT) t = type_new(TYPE_BOOL);
            if (n->un_op == UOP_REF) {
                Type *ref = type_new(TYPE_REF);
                ref->pointee = t;
                t = ref;
            }
            if (n->un_op == UOP_DEREF) {
                t = (t && t->kind == TYPE_REF && t->pointee) ? t->pointee : type_new(TYPE_ERROR);
            }
            break;

        case NODE_CALL:
            t = infer_call(ctx, n);
            break;

        case NODE_IF: {
            Type *ct = infer(ctx, n->cond);
            if (ct->kind != TYPE_BOOL && ct->kind != TYPE_ERROR) {
                check_error(ctx, n->cond->pos, "очаквах bool в условие, получих %s", type_str(ct));
            }
            /* MEM-2: drop-join — drop-нато в then се брои само ако else
             * (или отсъствието му) също го гарантира. */
            int log_base = ctx->n_drop_log;
            Type *tt = infer(ctx, n->then_br);
            int then_end = ctx->n_drop_log;
            /* else-клонът не трябва да вижда drop-овете от then: временно ги вдигни */
            drop_lift_from(ctx, log_base);
            Type *et = n->else_br ? infer(ctx, n->else_br) : type_new(TYPE_VOID);
            if (n->else_br) {
                drop_join2(ctx, log_base, then_end);
            } else {
                /* без else нищо не е сигурно drop-нато — вече вдигнахме
                 * флаговете; нелогнатите (overflow) изобщо не са маркирани */
                ctx->n_drop_log = log_base;
                ctx->drop_log_overflowed = 0;
            }
            t = type_eq(tt, et) ? tt : tt;
            break;
        }

        case NODE_BLOCK: {
            push_scope(ctx);
            t = type_new(TYPE_VOID);
            for (int i = 0; i < n->stmts.len; i++) {
                t = infer(ctx, n->stmts.data[i]);
            }
            pop_scope(ctx);
            break;
        }

        case NODE_INDEX:
            infer(ctx, n->obj);
            infer(ctx, n->index);
            t = type_new(TYPE_I64); /* TODO: array elem type */
            break;

        case NODE_ELEM_REF:
            /* v[*] — the element value (i64 for Vec<i64>/[i64]); annotation-only */
            infer(ctx, n->elem_obj);
            t = type_new(TYPE_I64);
            break;

        case NODE_FIELD: {
            /* L5/L6: модул.функция като СТОЯНОСТ (без извикване) — когато
             * лявата част не е локална променлива, а име на модул */
            if (n->field_obj->kind == NODE_IDENT &&
                !env_lookup(ctx, n->field_obj->name)) {
                const char *mod = n->field_obj->name;
                for (int i = 0; i < ctx->n_fns; i++) {
                    if (strcmp(ctx->fns[i].origin, mod) == 0 &&
                        strcmp(ctx->fns[i].name, n->field_name) == 0) {
                        Type *ft = ctx->fns[i].fn_type;
                        Type *vt2 = type_fn(ft->ret, ft->params, ft->nparams);
                        vt2->name = ctx->fns[i].decl->fn_name;
                        t = vt2;
                        break;
                    }
                }
                if (t) break;
            }
            Type *ot = infer(ctx, n->field_obj);
            /* resolve field type from struct definition */
            if (ot && ot->kind == TYPE_STRUCT && ot->name) {
                for (int si = 0; si < ctx->n_structs; si++) {
                    if (strcmp(ctx->structs[si].name, ot->name) == 0) {
                        Node *sdecl = ctx->structs[si].decl;
                        for (int fi = 0; fi < sdecl->fields.len; fi++) {
                            Node *fld = sdecl->fields.data[fi];
                            if (strcmp(fld->fld_name, n->field_name) == 0) {
                                t = resolve_type_node(ctx, fld->fld_type);
                                break;
                            }
                        }
                        break;
                    }
                }
            }
            if (!t) {
                check_error(ctx, n->pos, "непознато поле '.%s'", n->field_name);
                t = type_new(TYPE_ERROR);
            }
            break;
        }

        case NODE_ASSIGN: {
            infer(ctx, n->assign_target);
            t = infer(ctx, n->assign_val);
            if (n->assign_target->kind == NODE_IDENT &&
                !env_is_mut(ctx, n->assign_target->name)) {
                check_error(ctx, n->pos,
                    "променливата '%s' е декларирана с 'let' без 'mut' — присвояването е забранено",
                    n->assign_target->name);
            }
            /* MEM-3: mut p = arena_alloc(a, n) rebinds region; other assigns clear it */
            if (n->assign_target->kind == NODE_IDENT) {
                EnvEntry *pe = env_find(ctx, n->assign_target->name);
                if (pe) {
                    pe->region = NULL;
                    if (n->assign_val && n->assign_val->kind == NODE_CALL &&
                        n->assign_val->callee && n->assign_val->callee->kind == NODE_IDENT &&
                        strcmp(n->assign_val->callee->name, "arena_alloc") == 0 &&
                        n->assign_val->args.len >= 1 &&
                        n->assign_val->args.data[0]->kind == NODE_IDENT) {
                        EnvEntry *ae = env_find(ctx, n->assign_val->args.data[0]->name);
                        if (ae) pe->region = ae;
                    }
                }
            }
            break;
        }

        case NODE_RANGE:
            infer(ctx, n->range_lo);
            infer(ctx, n->range_hi);
            t = type_new(TYPE_I64);
            break;

        case NODE_STRUCT_LIT: {
            /* verify struct exists (L6: scoped по модула на литерала) */
            int amb = 0;
            Node *sdecl = find_struct_scoped(ctx, n->lit_name, n->pos.file, &amb);
            if (amb) {
                char m1[128], m2[128];
                struct_amb_hint(ctx, n->lit_name, m1, m2, sizeof m1);
                check_error(ctx, n->pos,
                    "нееднозначен struct '%s' — има го в модулите '%s' и '%s'; уточни с %s.%s или %s.%s",
                    n->lit_name, m1, m2, m1, n->lit_name, m2, n->lit_name);
            }
            else if (!sdecl) {
                check_error(ctx, n->pos, "непознат struct '%s'", n->lit_name);
            }
            /* check each literal field: name exists + type matches */
            for (int i = 0; i < n->n_lit_fields; i++) {
                Type *vt = infer(ctx, n->lit_values.data[i]);
                if (!sdecl) continue;
                int fld_found = 0;
                for (int fi = 0; fi < sdecl->fields.len; fi++) {
                    Node *fld = sdecl->fields.data[fi];
                    if (strcmp(fld->fld_name, n->lit_fields[i]) == 0) {
                        fld_found = 1;
                        Type *ft = resolve_type_node(ctx, fld->fld_type);
                        if (ft && vt && !type_eq(ft, vt)) {
                            check_error(ctx, n->pos,
                                "поле '%s' очаква %s, но получава %s",
                                n->lit_fields[i], type_str(ft), type_str(vt));
                        }
                        break;
                    }
                }
                if (!fld_found) {
                    check_error(ctx, n->pos,
                        "struct '%s' няма поле '%s'", n->lit_name, n->lit_fields[i]);
                }
            }
            /* check for missing fields */
            if (sdecl) {
                for (int fi = 0; fi < sdecl->fields.len; fi++) {
                    Node *fld = sdecl->fields.data[fi];
                    int present = 0;
                    for (int i = 0; i < n->n_lit_fields; i++) {
                        if (strcmp(n->lit_fields[i], fld->fld_name) == 0) {
                            present = 1;
                            break;
                        }
                    }
                    if (!present) {
                        check_error(ctx, n->pos,
                            "липсва поле '%s' в struct '%s'", fld->fld_name, n->lit_name);
                    }
                }
            }
            if (sdecl && strcmp(n->lit_name, sdecl->struct_name) != 0) {
                /* L6: codegen чете lit_name — пренаписваме към финалното име */
                free(n->lit_name);
                n->lit_name = strdup(sdecl->struct_name);
            }
            Type *st = type_new(TYPE_STRUCT);
            st->name = strdup(sdecl ? sdecl->struct_name : n->lit_name);
            t = st;
            break;
        }

        case NODE_TRY: {
            /* e? — infer e, propagate its effects to enclosing function */
            Type *et = infer(ctx, n->try_expr);
            t = type_new(et->kind);
            /* keep Vec elem / struct name, like a plain call result does */
            t->elem = et->elem;
            t->name = et->name;
            type_merge_effects(t, et);
            /* also accumulate at function level for checking */
            if (ctx->cur_effects)
                type_merge_effects(ctx->cur_effects, et);
            break;
        }

        case NODE_CATCH: {
            /* e catch !E => handler — remove effect E from e's type */
            /* MEM-2: две алтернативи — try изразът (нормален път) и handler-ът
             * (прихванат ефект); definitely dropped = drop-нато и в двете */
            int log_base = ctx->n_drop_log;
            Type *et = infer(ctx, n->catch_expr);
            int try_end = ctx->n_drop_log;
            /* handler-ът не трябва да вижда drop-овете от try израза */
            drop_lift_from(ctx, log_base);
            infer(ctx, n->catch_handler);
            drop_join2(ctx, log_base, try_end);
            /* LP4: catch на ефект, който изразът не носи, минаваше тихо —
             * мъртъв код и прикритие за правописни грешки в името. */
            if (et->kind != TYPE_ERROR) {
                int has = 0;
                for (int i = 0; i < et->n_effects; i++)
                    if (strcmp(et->effects[i], n->catch_effect) == 0) { has = 1; break; }
                if (!has)
                    check_error(ctx, n->pos,
                        "catch !%s — изразът няма такъв ефект (мъртъв catch; правописна грешка?)",
                        n->catch_effect);
            }
            t = type_new(et->kind);
            /* keep Vec elem / struct name, like a plain call result does */
            t->elem = et->elem;
            t->name = et->name;
            /* copy all effects except the caught one */
            for (int i = 0; i < et->n_effects; i++) {
                if (strcmp(et->effects[i], n->catch_effect) != 0)
                    type_add_effect(t, et->effects[i]);
            }
            /* remove caught effect from function-level accumulator */
            if (ctx->cur_effects)
                type_remove_effect(ctx->cur_effects, n->catch_effect);
            break;
        }

        case NODE_TO_STR: {
            /* interpolation: convert inner expr to str (str/i64/f64/bool) */
            Type *et = infer(ctx, n->to_str_expr);
            if (et->kind != TYPE_STR && et->kind != TYPE_I64 &&
                et->kind != TYPE_I32 && et->kind != TYPE_F64 &&
                et->kind != TYPE_BOOL && et->kind != TYPE_ERROR) {
                check_error(ctx, n->pos, "неподдържан тип за интерполация: %s (str/i64/f64/bool)", type_str(et));
            }
            t = type_new(TYPE_STR);
            type_merge_effects(t, et);
            if (ctx->cur_effects) type_merge_effects(ctx->cur_effects, et);
            break;
        }

        case NODE_LET: {
            Type *init_t = n->let_init ? infer(ctx, n->let_init) : type_new(TYPE_I64);
            if (n->let_init && init_t->kind == TYPE_VOID) {
                check_error(ctx, n->pos,
                    "не може да се присвои void стойност на '%s'", n->let_name);
            }
            Type *decl_t = n->let_type ? resolve_type_node(ctx, n->let_type) : init_t;
            if (n->let_type && n->let_init &&
                decl_t->kind != TYPE_ERROR && init_t->kind != TYPE_ERROR &&
                !type_eq(decl_t, init_t)) {
                /* i64 → f64 е позволено (разширяване); f64 → i64 е грешка */
                if (!(decl_t->kind == TYPE_F64 && init_t->kind == TYPE_I64)) {
                    check_error(ctx, n->pos,
                        "несъвместими типове: '%s' е %s, но инициализаторът е %s",
                        n->let_name, type_str(decl_t), type_str(init_t));
                }
            }
            /* L5: fn стойности — ефектен договор при wrap + без засенчване
             * на глобални функции (пази --verify sound) */
            if (decl_t->kind == TYPE_FN && init_t->kind == TYPE_FN &&
                !fn_effects_subset(init_t, decl_t)) {
                check_error(ctx, n->pos,
                    "fn стойността на '%s' има ефекти извън анотацията %s",
                    n->let_name, type_str(decl_t));
            }
            if (decl_t->kind == TYPE_FN && find_fn(ctx, n->let_name)) {
                check_error(ctx, n->pos,
                    "fn стойност '%s' засенчи глобална функция — преименувай я",
                    n->let_name);
            }
            env_define_mut(ctx, n->let_name, decl_t, n->is_mut, n->pos);
            /* MEM-3: let p = arena_alloc(a, n) → p.region = a */
            if (n->let_init && n->let_init->kind == NODE_CALL &&
                n->let_init->callee && n->let_init->callee->kind == NODE_IDENT &&
                strcmp(n->let_init->callee->name, "arena_alloc") == 0 &&
                n->let_init->args.len >= 1 &&
                n->let_init->args.data[0]->kind == NODE_IDENT) {
                EnvEntry *pe = env_find(ctx, n->let_name);
                EnvEntry *ae = env_find(ctx, n->let_init->args.data[0]->name);
                if (pe && ae) pe->region = ae;
            }
            t = type_new(TYPE_VOID);
            break;
        }

        case NODE_RETURN: {
            Type *rt = n->ret_val ? infer(ctx, n->ret_val) : type_new(TYPE_VOID);
            if (ctx->cur_ret && !type_eq(rt, ctx->cur_ret)) {
                check_error(ctx, n->pos, "връщам %s, но функцията очаква %s",
                            type_str(rt), type_str(ctx->cur_ret));
            }
            t = type_new(TYPE_VOID);
            break;
        }

        case NODE_WHILE: {
            Type *ct = infer(ctx, n->while_cond);
            if (ct->kind != TYPE_BOOL && ct->kind != TYPE_ERROR) {
                check_error(ctx, n->while_cond->pos, "очаквах bool в условие на while, получих %s",
                            type_str(ct));
            }
            /* MEM-2: drop на променлива извън цикъла = use-after-drop на итерация 2 */
            int saved_loop = ctx->loop_depth;
            ctx->loop_depth = ctx->depth;
            infer(ctx, n->while_body);
            ctx->loop_depth = saved_loop;
            t = type_new(TYPE_VOID);
            break;
        }

        case NODE_FOR: {
            infer(ctx, n->for_iter);
            push_scope(ctx);
            env_define(ctx, n->for_var, type_new(TYPE_I64), n->pos);
            /* MEM-2: loop_depth е depth-ът на самия цикъл; for-променливата е
             * в прясния scope (scope_depth > loop_depth) — не е "външна" */
            int saved_loop = ctx->loop_depth;
            ctx->loop_depth = ctx->depth - 1;
            infer(ctx, n->for_body);
            ctx->loop_depth = saved_loop;
            pop_scope(ctx);
            t = type_new(TYPE_VOID);
            break;
        }

        case NODE_MATCH: {
            Type *st = infer(ctx, n->match_expr);
            if (st && st->kind == TYPE_ENUM && st->name) {
                /* L3: match върху sum enum — вариантни патерни, bindings, пълнота */
                Node *ed = NULL;
                for (int ei = 0; ei < ctx->n_enums; ei++)
                    if (strcmp(ctx->enums[ei].name, st->name) == 0)
                        { ed = ctx->enums[ei].decl; break; }
                if (!ed) { t = type_new(TYPE_ERROR); break; }
                int nv = ed->n_variants;
                int *covered = calloc((size_t)nv, sizeof(int));
                int has_wild = 0;
                t = type_new(TYPE_VOID);
                /* MEM-2: всеки arm е алтернатива — N-arm drop-join */
                int log_base = ctx->n_drop_log;
                int arm_end[64];
                int narm = n->match_arms.len;
                for (int i = 0; i < n->match_arms.len; i++) {
                    Node *arm = n->match_arms.data[i];
                    /* този arm не трябва да вижда drop-овете от предишните */
                    drop_lift_from(ctx, log_base);
                    if (!arm->arm_pattern) {
                        has_wild = 1;
                    } else if (arm->arm_pattern->kind != NODE_IDENT &&
                               arm->arm_pattern->kind != NODE_PATH) {
                        check_error(ctx, arm->arm_pattern->pos,
                            "match върху '%s': патернът трябва да е вариант или '_'",
                            st->name);
                    } else {
                        const char *vn = arm->arm_pattern->kind == NODE_PATH
                            ? arm->arm_pattern->path_variant
                            : arm->arm_pattern->name;
                        if (arm->arm_pattern->kind == NODE_PATH &&
                            strcmp(arm->arm_pattern->path_enum, st->name) != 0) {
                            check_error(ctx, arm->arm_pattern->pos,
                                "патернът '%s::%s' не е за enum '%s'",
                                arm->arm_pattern->path_enum, vn, st->name);
                        }
                        int vidx = -1;
                        for (int j = 0; j < nv; j++)
                            if (strcmp(ed->enum_variants[j], vn) == 0) { vidx = j; break; }
                        if (vidx < 0) {
                            check_error(ctx, arm->arm_pattern->pos,
                                "патернът '%s' не е вариант на enum '%s'", vn, st->name);
                        } else {
                            covered[vidx] = 1;
                            int has_pl = ed->enum_payloads && ed->enum_payloads[vidx];
                            if (has_pl && !arm->arm_binding)
                                check_error(ctx, arm->arm_pattern->pos,
                                    "вариантът '%s' има payload — нужен е binding: %s(v)",
                                    vn, vn);
                            if (!has_pl && arm->arm_binding)
                                check_error(ctx, arm->arm_pattern->pos,
                                    "вариантът '%s' няма payload", vn);
                        }
                    }
                    push_scope(ctx);
                    if (arm->arm_binding && arm->arm_pattern &&
                        (arm->arm_pattern->kind == NODE_IDENT ||
                         arm->arm_pattern->kind == NODE_PATH)) {
                        const char *pvn = arm->arm_pattern->kind == NODE_PATH
                            ? arm->arm_pattern->path_variant
                            : arm->arm_pattern->name;
                        Type *pt = type_new(TYPE_ERROR);
                        for (int j = 0; j < nv; j++)
                            if (strcmp(ed->enum_variants[j], pvn) == 0 &&
                                ed->enum_payloads && ed->enum_payloads[j])
                                pt = resolve_type_node(ctx, ed->enum_payloads[j]);
                        env_define(ctx, arm->arm_binding, pt, arm->pos);
                    }
                    /* синтетичният `return e` в arm тяло е стойност на match,
                     * не return от обграждащата функция — не го проверявай срещу cur_ret */
                    Type *saved_ret = ctx->cur_ret;
                    ctx->cur_ret = NULL;
                    Type *bt = infer(ctx, arm->arm_body);
                    ctx->cur_ret = saved_ret;
                    pop_scope(ctx);
                    if (i < 64) arm_end[i] = ctx->n_drop_log;
                    if (bt->kind == TYPE_VOID && arm->arm_body &&
                        arm->arm_body->kind == NODE_BLOCK &&
                        arm->arm_body->stmts.len > 0) {
                        Node *last = arm->arm_body->stmts.data[arm->arm_body->stmts.len - 1];
                        if (last->kind == NODE_RETURN && last->ret_val && last->ret_val->type)
                            bt = last->ret_val->type;
                    }
                    if (i == 0) t = bt;
                    else if (!type_eq(bt, t))
                        check_error(ctx, arm->pos,
                            "arm #%d на match е от тип %s, но първият arm е %s",
                            i + 1, type_str(bt), type_str(t));
                }
                if (!has_wild)
                    for (int j = 0; j < nv; j++)
                        if (!covered[j])
                            check_error(ctx, n->pos,
                                "match върху '%s' не е пълен — липсва вариант '%s' (или добави '_')",
                                st->name, ed->enum_variants[j]);
                free(covered);
                /* MEM-2: definitely dropped = drop-нато във всеки arm */
                drop_join_arms(ctx, log_base, narm <= 64 ? arm_end : NULL, narm);
                break;
            }
            /* не-enum match: досегашното поведение (първият arm дава типа) */
            t = type_new(TYPE_VOID);
            {
                /* MEM-2: всеки arm е алтернатива — N-arm drop-join */
                int log_base = ctx->n_drop_log;
                int arm_end[64];
                int narm = n->match_arms.len;
                int has_wild = 0;
                for (int i = 0; i < n->match_arms.len; i++) {
                    Node *arm = n->match_arms.data[i];
                    /* този arm не трябва да вижда drop-овете от предишните */
                    drop_lift_from(ctx, log_base);
                    if (arm->arm_pattern) infer(ctx, arm->arm_pattern);
                    else has_wild = 1;  /* `_` — гарантира изпълнение на някой arm */
                    /* infer arm body; extract type from return if wrapped */
                    Type *saved_ret = ctx->cur_ret;
                    ctx->cur_ret = NULL;
                    Type *bt = infer(ctx, arm->arm_body);
                    ctx->cur_ret = saved_ret;
                    if (i < 64) arm_end[i] = ctx->n_drop_log;
                    if (bt->kind == TYPE_VOID && arm->arm_body &&
                        arm->arm_body->kind == NODE_BLOCK &&
                        arm->arm_body->stmts.len > 0) {
                        Node *last = arm->arm_body->stmts.data[arm->arm_body->stmts.len - 1];
                        if (last->kind == NODE_RETURN && last->ret_val && last->ret_val->type)
                            bt = last->ret_val->type;
                    }
                    if (i == 0) t = bt;
                }
                /* MEM-2: не-enum match няма exhaustiveness проверка — без `_`
                 * при runtime може да не се изпълни НИТО ЕДИН arm, затова
                 * intersection-ът е unsound: консервативно вдигане на всичко */
                if (has_wild)
                    drop_join_arms(ctx, log_base, narm <= 64 ? arm_end : NULL, narm);
                else {
                    drop_lift_from(ctx, log_base);
                    ctx->n_drop_log = log_base;
                    ctx->drop_log_overflowed = 0;
                }
            }
            break;
        }

        case NODE_MATCH_ARM:
            if (n->arm_pattern) infer(ctx, n->arm_pattern);
            t = infer(ctx, n->arm_body);
            break;

        case NODE_EXPR_STMT:
            t = infer(ctx, n->expr);
            break;

        case NODE_INVARIANT:
            /* annotation statement (verifier-only): predicates must be bool;
             * c[*] elem refs are allowed here (annotation context) */
            for (int i = 0; i < n->inv_exprs.len; i++) infer(ctx, n->inv_exprs.data[i]);
            t = type_new(TYPE_VOID);
            break;

        case NODE_LAMBDA: {
            /* fn [caps] (params) -> ret { body } — L5. Captures са по
             * стойност и идват от обкръжението; ефектите на тялото се
             * сливат върху ТИПА на ламбдата (създаването ѝ е чисто). */
            if (!n->fn_name) {
                char lbuf[40];
                snprintf(lbuf, sizeof lbuf, "__lam_%d", ctx->n_lambdas++);
                n->fn_name = strdup(lbuf);
            }
            for (int i = 0; i < n->captures.len; i++) {
                Node *cap = n->captures.data[i];
                Type *ct2 = env_lookup(ctx, cap->param_name);
                if (!ct2) {
                    check_error(ctx, cap->pos,
                        "лямбда capture '%s' не е дефиниран в обкръжението", cap->param_name);
                    ct2 = type_new(TYPE_ERROR);
                } else {
                    /* MEM-2: заснетата външна променлива не може да се drop-ва
                     * (лямбдата държи указател към същия буфер) */
                    EnvEntry *ce = env_find(ctx, cap->param_name);
                    if (ce) ce->captured = 1;
                }
                cap->type = ct2;
            }
            int np = n->params.len;
            Type **params = NULL;
            if (np > 0) {
                params = malloc(sizeof(Type *) * (size_t)np);
                for (int j = 0; j < np; j++) {
                    params[j] = resolve_type_node(ctx, n->params.data[j]->param_type);
                    n->params.data[j]->type = params[j];
                }
            }
            Type *ret = n->ret_type ? resolve_type_node(ctx, n->ret_type)
                                    : type_new(TYPE_VOID);
            Type *saved_ret = ctx->cur_ret;
            Type *saved_eff = ctx->cur_effects;
            const char *saved_fn = ctx->cur_fn;
            ctx->cur_ret = ret;
            ctx->cur_effects = type_new(TYPE_VOID);
            ctx->cur_fn = n->fn_name;
            push_scope(ctx);
            for (int i = 0; i < n->captures.len; i++) {
                env_define(ctx, n->captures.data[i]->param_name,
                           n->captures.data[i]->type, n->captures.data[i]->pos);
                /* MEM-2: и вътре в тялото capture-ът не може да се drop-ва */
                EnvEntry *ie = env_find(ctx, n->captures.data[i]->param_name);
                if (ie) ie->captured = 1;
            }
            for (int j = 0; j < np; j++) {
                if (params[j]->kind == TYPE_FN && find_fn(ctx, n->params.data[j]->param_name))
                    check_error(ctx, n->params.data[j]->pos,
                        "fn параметър '%s' засенчи глобална функция — преименувай го",
                        n->params.data[j]->param_name);
                env_define(ctx, n->params.data[j]->param_name, params[j],
                           n->params.data[j]->pos);
                /* MEM-2: параметрите споделят буфера на извикващия */
                EnvEntry *pe = env_find(ctx, n->params.data[j]->param_name);
                if (pe) pe->is_param = 1;
            }
            if (n->fn_body) {
                for (int i = 0; i < n->fn_body->stmts.len; i++)
                    infer(ctx, n->fn_body->stmts.data[i]);
            }
            if (ctx->cur_effects)
                type_merge_effects(ret, ctx->cur_effects);
            pop_scope(ctx);
            ctx->cur_ret = saved_ret;
            ctx->cur_effects = saved_eff;
            ctx->cur_fn = saved_fn;
            t = type_fn(ret, params, np);
            break;
        }

        case NODE_BREAK:
        case NODE_CONTINUE:
            t = type_new(TYPE_VOID);
            break;

        default:
            t = type_new(TYPE_VOID);
            break;
    }

    n->type = t;
    return t;
}

/* ============================================================
 *  Function checking
 * ============================================================ */

/* M19: fallthrough анализ — може ли изпълнението да падне от края на
 * този statement? Консервативен: ако не може да се докаже обратното,
 * отговорът е "да". return/break/continue не падат; block пада, ако
 * последният stmt пада; if без else пада; while пада, освен literal
 * `while true` без break на това ниво; match винаги пада (arm-ският
 * `return e` е СТОЙНОСТ на match-а, не изход от функцията). */
static int tree_has_break_depth(Node *n, int depth) {
    if (!n) return 0;
    if (n->kind == NODE_BREAK) return depth == 0;
    switch (n->kind) {
        case NODE_BINARY:
            return tree_has_break_depth(n->left, depth) ||
                   tree_has_break_depth(n->right, depth);
        case NODE_UNARY:
            return tree_has_break_depth(n->operand, depth);
        case NODE_CALL:
            for (int i = 0; i < n->args.len; i++)
                if (tree_has_break_depth(n->args.data[i], depth)) return 1;
            return 0;
        case NODE_IF:
            return tree_has_break_depth(n->then_br, depth) ||
                   tree_has_break_depth(n->else_br, depth);
        case NODE_BLOCK:
            for (int i = 0; i < n->stmts.len; i++)
                if (tree_has_break_depth(n->stmts.data[i], depth)) return 1;
            return 0;
        case NODE_WHILE:
            if (tree_has_break_depth(n->while_cond, depth)) return 1;
            return tree_has_break_depth(n->while_body, depth + 1);
        case NODE_FOR:
            return tree_has_break_depth(n->for_body, depth + 1);
        case NODE_MATCH:
            for (int i = 0; i < n->match_arms.len; i++) {
                Node *arm = n->match_arms.data[i];
                if (tree_has_break_depth(arm->arm_body, depth)) return 1;
            }
            return 0;
        case NODE_ASSIGN:
            return tree_has_break_depth(n->assign_target, depth) ||
                   tree_has_break_depth(n->assign_val, depth);
        case NODE_TRY:
            return tree_has_break_depth(n->try_expr, depth);
        case NODE_CATCH:
            return tree_has_break_depth(n->catch_expr, depth) ||
                   tree_has_break_depth(n->catch_handler, depth);
        default:
            return 0;
    }
}

static int stmt_falls_through(Node *n) {
    if (!n) return 1;
    switch (n->kind) {
        case NODE_RETURN:
        case NODE_BREAK:
        case NODE_CONTINUE:
            return 0;
        case NODE_BLOCK: {
            if (n->stmts.len == 0) return 1;
            return stmt_falls_through(n->stmts.data[n->stmts.len - 1]);
        }
        case NODE_IF:
            if (!n->else_br) return 1;
            if (stmt_falls_through(n->then_br)) return 1;
            return stmt_falls_through(n->else_br);
        case NODE_WHILE:
            /* literal `while true` без break на това ниво не излиза */
            if (n->while_cond && n->while_cond->kind == NODE_BOOL_LIT &&
                n->while_cond->bool_val == 1 &&
                !tree_has_break_depth(n->while_body, 0))
                return 0;
            return 1;
        default:
            return 1;
    }
}

/* Тялото на функция: последният изразен statement е implicit return
 * (codegen го превръща в `return expr;` за не-void функции). */
static int fn_body_falls_through(Node *body) {
    if (!body || body->stmts.len == 0) return 1;
    Node *last = body->stmts.data[body->stmts.len - 1];
    if (last->kind == NODE_EXPR_STMT) return 0;
    return stmt_falls_through(last);
}

static void check_fn(CheckCtx *ctx, Node *fn) {
    ctx->cur_fn = fn->fn_name;
    ctx->cur_ret = fn->ret_type ? resolve_type_node(ctx, fn->ret_type) : type_new(TYPE_VOID);
    ctx->cur_effects = type_new(TYPE_VOID); /* accumulator for body effects */

    push_scope(ctx);

    /* define params */
    for (int i = 0; i < fn->params.len; i++) {
        Node *p = fn->params.data[i];
        Type *pt = resolve_type_node(ctx, p->param_type);
        p->type = pt;
        if (pt->kind == TYPE_FN && find_fn(ctx, p->param_name)) {
            check_error(ctx, p->pos,
                "fn параметър '%s' засенчи глобална функция — преименувай го",
                p->param_name);
        }
        env_define(ctx, p->param_name, pt, p->pos);
        /* MEM-2: параметрите споделят буфера на извикващия — drop забранен */
        EnvEntry *pe = env_find(ctx, p->param_name);
        if (pe) pe->is_param = 1;
    }

    /* check body — MEM-2: drop-логът е per-функция (drop-ове не пресичат
     * fn граница); без reset block-local drop-ове пълнят лога за целия модул */
    ctx->n_drop_log = 0;
    ctx->drop_log_overflowed = 0;
    if (fn->fn_body) {
        for (int i = 0; i < fn->fn_body->stmts.len; i++)
            infer(ctx, fn->fn_body->stmts.data[i]);
    }

    /* M19: не-void функция не бива да пада от края — връща боклук в C */
    if (fn->fn_body && ctx->cur_ret && ctx->cur_ret->kind != TYPE_VOID &&
        fn_body_falls_through(fn->fn_body)) {
        check_error(ctx, fn->pos,
            "функцията '%s' може да падне от края без return — не-void функциите трябва да връщат стойност на всеки път",
            fn->fn_name);
    }

    /* M19b: типът на implicit return (последният изразен statement)
     * трябва да съвпада с връщания тип — иначе кодгенът връща боклук */
    if (fn->fn_body && fn->fn_body->stmts.len > 0 &&
        ctx->cur_ret && ctx->cur_ret->kind != TYPE_VOID) {
        Node *last = fn->fn_body->stmts.data[fn->fn_body->stmts.len - 1];
        if (last->kind == NODE_EXPR_STMT && last->expr && last->expr->type &&
            !type_eq(last->expr->type, ctx->cur_ret)) {
            check_error(ctx, last->expr->pos,
                "implicit return връща %s, но функцията '%s' очаква %s",
                type_str(last->expr->type), fn->fn_name, type_str(ctx->cur_ret));
        }
    }

    /* effect checking: unhandled effects in body vs declared effects */
    if (ctx->cur_effects) {
        for (int i = 0; i < ctx->cur_effects->n_effects; i++) {
            const char *eff = ctx->cur_effects->effects[i];
            if (!type_has_effect(ctx->cur_ret, eff)) {
                check_error(ctx, fn->pos,
                    "необработен ефект !%s във '%s' — декларирай го в return типа или го хвани с catch",
                    eff, fn->fn_name);
            }
        }
    }

    pop_scope(ctx);
    ctx->cur_fn = NULL;
    ctx->cur_ret = NULL;
    ctx->cur_effects = NULL;
}

/* ============================================================
 *  Public API
 * ============================================================ */

void check_program(Checker *c, Node *program) {
    CheckCtx ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.chk = c;
    ctx.main_base = mod_base(program->pos.file);
    ctx.program = program;

    /* L6 struct pre-pass: еднакви struct имена от РАЗЛИЧНИ модули се
     * преименуват на "модул.Type" ПРЕДИ pass 1 (fn сигнатурите resolve-ват
     * struct имена още при регистрация). Дубликат в ЕДИН модул е грешка —
     * досега и двата случая излизаха чак в gcc с "conflicting types". */
    for (int i = 0; i < program->items.len; i++) {
        Node *a = program->items.data[i];
        if (a->kind != NODE_STRUCT) continue;
        for (int j = i + 1; j < program->items.len; j++) {
            Node *b = program->items.data[j];
            if (b->kind != NODE_STRUCT) continue;
            if (strcmp(type_short_name(a->struct_name),
                       type_short_name(b->struct_name)) != 0) continue;
            char *oa = mod_base(a->pos.file);
            char *ob = mod_base(b->pos.file);
            if (strcmp(oa, ob) == 0) {
                check_error(&ctx, b->pos,
                    "повторна дефиниция на struct '%s' в модул '%s'",
                    type_short_name(b->struct_name), ob);
            } else {
                for (int k = 0; k < 2; k++) {
                    Node *d = k == 0 ? a : b;
                    const char *org = k == 0 ? oa : ob;
                    if (strchr(d->struct_name, '.')) continue;  /* вече преименуван */
                    size_t need = strlen(org) + 1 + strlen(d->struct_name) + 1;
                    char *nn = malloc(need);
                    if (!nn) { fprintf(stderr, "baga: out of memory\n"); exit(1); }
                    snprintf(nn, need, "%s.%s", org, d->struct_name);
                    d->struct_name = nn;
                }
            }
            free(oa);
            free(ob);
        }
    }

    push_scope(&ctx);

    /* L3 pre-pass: регистрираме enum декларациите (име + тип) ПРЕДИ pass 1,
     * защото сигнатурите и payload-ите се resolve-ват в ред и могат да
     * реферират по-късно деклариран sum enum (fn f(r: Res) преди enum Res).
     * Pass 1 после само регистрира вариантите — без повторна регистрация. */
    for (int i = 0; i < program->items.len; i++) {
        Node *item = program->items.data[i];
        if (item->kind != NODE_ENUM) continue;
        /* L6: enum имената остават глобални — дубликат между модули е ясна
         * грешка тук (досега излизаше в gcc при несвързани варианти) */
        for (int k = 0; k < ctx.n_enums; k++) {
            if (strcmp(ctx.enums[k].name, item->enum_name) == 0) {
                check_error(&ctx, item->pos,
                    "повторна дефиниция на enum '%s' (enum имената са глобални — модулно уточняване не се поддържа)",
                    item->enum_name);
                break;
            }
        }
        int is_sum = 0;
        for (int j = 0; j < item->n_variants; j++)
            if (item->enum_payloads && item->enum_payloads[j]) is_sum = 1;
        if (ctx.n_enums < FNS_MAX) {
            ctx.enums[ctx.n_enums].name = item->enum_name;
            ctx.enums[ctx.n_enums].decl = item;
            ctx.n_enums++;
        } else {
            check_error(&ctx, item->pos,
                "твърде много enum декларации (лимит %d) — раздели програмата",
                FNS_MAX);
        }
        Type *et = type_new(is_sum ? TYPE_ENUM : TYPE_I64);
        et->name = strdup(item->enum_name);
        item->type = et;
    }

    /* pass 1: register all top-level names */
    for (int i = 0; i < program->items.len; i++) {
        Node *item = program->items.data[i];

        if (item->kind == NODE_FN) {
            /* build function type */
            Type *ret = item->ret_type ? resolve_type_node(&ctx, item->ret_type) : type_new(TYPE_VOID);
            int np = item->params.len;
            Type **params = NULL;
            if (np > 0) {
                params = malloc(sizeof(Type *) * (size_t)np);
                for (int j = 0; j < np; j++)
                    params[j] = resolve_type_node(&ctx, item->params.data[j]->param_type);
            }
            Type *ft = type_fn(ret, params, np);
            ft->name = strdup(item->fn_name);

            if (ctx.n_fns < FNS_MAX) {
                ctx.fns[ctx.n_fns].name = ft->name;  /* strdup-натото късо име */
                ctx.fns[ctx.n_fns].origin = mod_base(item->pos.file);
                ctx.fns[ctx.n_fns].fn_type = ft;
                ctx.fns[ctx.n_fns].decl = item;
                ctx.n_fns++;
            } else {
                check_error(&ctx, item->pos,
                    "твърде много функции (лимит %d, FNS_MAX) — fn '%s' е отрязана; "
                    "раздели модулите или вдигни лимита",
                    FNS_MAX, item->fn_name);
            }
            item->type = ft;

            if (item->is_extern) {
                /* extern fn: params restricted to i64, f64, str (void is only
                 * meaningful as a return type — a void param breaks gcc) */
                for (int j = 0; j < item->params.len; j++) {
                    Node *pt = item->params.data[j]->param_type;
                    while (pt && pt->kind == NODE_TYPE_EFFECT) pt = pt->inner_type;
                    if (!pt || pt->kind != NODE_TYPE ||
                        (strcmp(pt->type_name, "i64") != 0 &&
                         strcmp(pt->type_name, "f64") != 0 &&
                         strcmp(pt->type_name, "str") != 0))
                        check_error(&ctx, item->pos,
                            "extern fn '%s': неподдържан тип на параметър (само i64, f64, str)",
                            item->fn_name);
                }
                Node *rt = item->ret_type;
                while (rt && rt->kind == NODE_TYPE_EFFECT) rt = rt->inner_type;
                if (rt && (rt->kind != NODE_TYPE ||
                    (strcmp(rt->type_name, "i64") != 0 &&
                     strcmp(rt->type_name, "f64") != 0 &&
                     strcmp(rt->type_name, "str") != 0 &&
                     strcmp(rt->type_name, "void") != 0)))
                    check_error(&ctx, item->pos,
                        "extern fn '%s': неподдържан връщан тип (само i64, f64, str, void)",
                        item->fn_name);
            }

        } else if (item->kind == NODE_STRUCT) {
            if (ctx.n_structs < FNS_MAX) {
                ctx.structs[ctx.n_structs].name = item->struct_name;
                ctx.structs[ctx.n_structs].decl = item;
                ctx.n_structs++;
            } else {
                check_error(&ctx, item->pos,
                    "твърде много struct декларации (лимит %d) — struct '%s' е отрязан",
                    FNS_MAX, item->struct_name);
            }
            Type *st = type_new(TYPE_STRUCT);
            st->name = strdup(item->struct_name);
            item->type = st;
            /* L6: resolve-ваме полетата тут (и пренаписваме AST имената към
             * финалните "модул.Type"), защото codegen емитва typedef-а от
             * AST текстa — иначе нереферирано поле би останало с късо име */
            for (int fi = 0; fi < item->fields.len; fi++)
                resolve_type_node(&ctx, item->fields.data[fi]->fld_type);

        } else if (item->kind == NODE_ENUM) {
            /* името и типът са регистрирани в L3 pre-pass-а (преди pass 1) —
             * тук само регистрираме вариантите; payload-ите вече resolve-ват
             * и по-късно декларирани sum enum-и */
            for (int j = 0; j < item->n_variants; j++) {
                if (ctx.n_variants < FNS_MAX * 4) {
                    ctx.variants[ctx.n_variants].variant = item->enum_variants[j];
                    ctx.variants[ctx.n_variants].enum_name = item->enum_name;
                    ctx.variants[ctx.n_variants].value = j;
                    ctx.variants[ctx.n_variants].payload =
                        (item->enum_payloads && item->enum_payloads[j])
                            ? resolve_type_node(&ctx, item->enum_payloads[j]) : NULL;
                    ctx.n_variants++;
                } else {
                    check_error(&ctx, item->pos,
                        "твърде много enum варианти (лимит %d) — '%s.%s' е отрязан",
                        FNS_MAX * 4, item->enum_name, item->enum_variants[j]);
                }
            }
        }
    }

    /* L6 namespaces: еднакви имена от РАЗЛИЧНИ модули са легални — decl-ът
     * получава уникално вътрешно име "модул.функция" ('.' не се лексва в
     * идентификатори, така че сблъсък с източника е невъзможен), а с него и
     * уникален C символ. Дубликат в ЕДИН модул е грешка (досега я хващаше
     * gcc с redefinition). */
    for (int i = 0; i < ctx.n_fns; i++) {
        for (int j = i + 1; j < ctx.n_fns; j++) {
            if (strcmp(ctx.fns[i].name, ctx.fns[j].name) != 0) continue;
            /* forward декларация + реализация (или две декларации) — ОК;
             * дубликатът е само между две тела */
            if (!ctx.fns[i].decl->fn_body || !ctx.fns[j].decl->fn_body) continue;
            if (strcmp(ctx.fns[i].origin, ctx.fns[j].origin) == 0) {
                check_error(&ctx, ctx.fns[j].decl->pos,
                    "повторна дефиниция на функция '%s' в модул '%s'",
                    ctx.fns[j].name, ctx.fns[j].origin);
                continue;
            }
            for (int k = 0; k < 2; k++) {
                Node *d = k == 0 ? ctx.fns[i].decl : ctx.fns[j].decl;
                const char *org = k == 0 ? ctx.fns[i].origin : ctx.fns[j].origin;
                if (d->is_extern) continue;        /* FFI името е договор */
                if (strchr(d->fn_name, '.')) continue;  /* вече преименувана */
                size_t need = strlen(org) + 1 + strlen(d->fn_name) + 1;
                char *nn = malloc(need);
                if (!nn) { fprintf(stderr, "baga: out of memory\n"); exit(1); }
                snprintf(nn, need, "%s.%s", org, d->fn_name);
                d->fn_name = nn;
            }
        }
    }

    /* A1: sum variants may share names across enums (use Enum::Variant).
     * Still forbid the same name twice *inside one enum*, and warn-as-error
     * only when a bare name would always be ambiguous is handled at use site.
     * Function vs variant: allowed; bare call prefers unique variant, else fn. */
    for (int i = 0; i < ctx.n_variants; i++) {
        Node *ed = NULL;
        for (int ei = 0; ei < ctx.n_enums; ei++)
            if (strcmp(ctx.enums[ei].name, ctx.variants[i].enum_name) == 0)
                { ed = ctx.enums[ei].decl; break; }
        if (!ed) continue;
        for (int j = i + 1; j < ctx.n_variants; j++) {
            if (strcmp(ctx.variants[i].enum_name, ctx.variants[j].enum_name) == 0 &&
                strcmp(ctx.variants[i].variant, ctx.variants[j].variant) == 0)
                check_error(&ctx, ed->pos,
                    "повторена дефиниция на вариант '%s' в enum '%s'",
                    ctx.variants[i].variant, ctx.variants[i].enum_name);
        }
    }

    /* pass 2: verify specs against functions */
    for (int i = 0; i < program->items.len; i++) {
        Node *item = program->items.data[i];
        if (item->kind != NODE_SPEC) continue;

        /* find the function this spec describes */
        Type *ft = find_fn(&ctx, item->spec_name);
        if (!ft) {
            check_error(&ctx, item->pos,
                "spec '%s' описва функция, която не съществува", item->spec_name);
            continue;
        }

        /* check input count */
        if (item->spec_inputs.len != ft->nparams) {
            check_error(&ctx, item->pos,
                "spec '%s': input има %d параметъра, но функцията има %d",
                item->spec_name, item->spec_inputs.len, ft->nparams);
        } else {
            /* check each input type */
            for (int j = 0; j < item->spec_inputs.len; j++) {
                Node *sp = item->spec_inputs.data[j];
                Type *spec_t = resolve_type_node(&ctx, sp->param_type);
                if (!type_eq(spec_t, ft->params[j])) {
                    check_error(&ctx, sp->pos,
                        "spec '%s': параметър '%s' е %s в spec-а, но %s във функцията",
                        item->spec_name, sp->param_name,
                        type_str(spec_t), type_str(ft->params[j]));
                }
            }
        }

        /* check output type */
        if (item->spec_output) {
            Type *spec_ret = resolve_type_node(&ctx, item->spec_output);
            Type *fn_ret = ft->ret ? ft->ret : type_new(TYPE_VOID);
            if (!type_eq(spec_ret, fn_ret)) {
                check_error(&ctx, item->pos,
                    "spec '%s': output е %s, но функцията връща %s",
                    item->spec_name, type_str(spec_ret), type_str(fn_ret));
            }
        }

        /* check ensures expressions (type-check in scope: inputs + output) */
        if (item->spec_ensures.len > 0) {
            Type *fn_ret = ft->ret ? ft->ret : type_new(TYPE_VOID);
            if (fn_ret->kind == TYPE_VOID) {
                check_error(&ctx, item->pos,
                    "spec '%s': ensures изисква функция с върнат тип",
                    item->spec_name);
            } else {
                push_scope(&ctx);
                for (int j = 0; j < item->spec_inputs.len; j++) {
                    Node *sp = item->spec_inputs.data[j];
                    env_define(&ctx, sp->param_name,
                               resolve_type_node(&ctx, sp->param_type), sp->pos);
                }
                env_define(&ctx, "output", fn_ret, item->pos);
                for (int j = 0; j < item->spec_ensures.len; j++) {
                    Node *en = item->spec_ensures.data[j];
                    Type *et = infer(&ctx, en->ensure_expr);
                    if (et->kind != TYPE_BOOL && et->kind != TYPE_ERROR) {
                        check_error(&ctx, en->pos,
                            "spec '%s': ensures #%d е %s, очаквах bool",
                            item->spec_name, j + 1, type_str(et));
                    }
                }
                pop_scope(&ctx);
            }
        }

        /* check requires expressions (scope: inputs only, no output) */
        if (item->spec_requires.len > 0) {
            push_scope(&ctx);
            for (int j = 0; j < item->spec_inputs.len; j++) {
                Node *sp = item->spec_inputs.data[j];
                env_define(&ctx, sp->param_name,
                           resolve_type_node(&ctx, sp->param_type), sp->pos);
            }
            for (int j = 0; j < item->spec_requires.len; j++) {
                Node *rq = item->spec_requires.data[j];
                Type *rt = infer(&ctx, rq->ensure_expr);
                if (rt->kind != TYPE_BOOL && rt->kind != TYPE_ERROR) {
                    check_error(&ctx, rq->pos,
                        "spec '%s': requires #%d е %s, очаквах bool",
                        item->spec_name, j + 1, type_str(rt));
                }
            }
            pop_scope(&ctx);
        }

        /* check decreases expression (M6; scope: inputs only, must be i64) */
        if (item->spec_decreases) {
            push_scope(&ctx);
            for (int j = 0; j < item->spec_inputs.len; j++) {
                Node *sp = item->spec_inputs.data[j];
                env_define(&ctx, sp->param_name,
                           resolve_type_node(&ctx, sp->param_type), sp->pos);
            }
            Type *dt = infer(&ctx, item->spec_decreases);
            if (dt->kind != TYPE_I64 && dt->kind != TYPE_ERROR) {
                check_error(&ctx, item->spec_decreases->pos,
                    "spec '%s': decreases е %s, очаквах i64",
                    item->spec_name, type_str(dt));
            }
            pop_scope(&ctx);
        }
    }

    /* pass 3: check function bodies */
    for (int i = 0; i < program->items.len; i++) {
        Node *item = program->items.data[i];
        if (item->kind == NODE_FN)
            check_fn(&ctx, item);
    }

    /* check that main exists (skipped for --check / library mode) */
    if (!c->allow_no_main && !find_fn(&ctx, "main")) {
        SrcPos pos = { 1, 1, NULL };
        check_error(&ctx, pos, "липсва функция 'main'");
    }

    pop_scope(&ctx);
}
