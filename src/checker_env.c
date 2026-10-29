/* checker_env.c — част от checker.c (виж ck_proto.h). */
#include "baga.h"
#include "checker_impl.h"

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
        case TYPE_VAR:   return t->name ? t->name : "T";
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
int vec_elem_eq(Type *a, Type *b) {
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
    /* M21: типови променливи са равни по име */
    if (a->kind == TYPE_VAR && b->kind == TYPE_VAR) {
        if (!a->name || !b->name) return 1;
        return strcmp(a->name, b->name) == 0;
    }
    if (a->kind != b->kind) return 0;
    if (a->kind == TYPE_STRUCT) {
        if (!(a->name && b->name && strcmp(a->name, b->name) == 0)) return 0;
        /* M24: instantiated generic struct — аргументите също трябва да
         * съвпадат (по вид/име) */
        if (a->n_targs != b->n_targs) return 0;
        for (int i = 0; i < a->n_targs; i++)
            if (!type_eq(a->targs[i], b->targs[i])) return 0;
        return 1;
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
int type_assignable(Type *arg, Type *param) {
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
    /* M20: payload масивът върви успоредно */
    t->effect_payloads = realloc(t->effect_payloads, sizeof(Type *) * (size_t)t->n_effects);
    if (!t->effect_payloads) { fprintf(stderr, "baga: out of memory\n"); exit(1); }
    t->effect_payloads[t->n_effects - 1] = NULL;
}

/* M20: payload на ефект (NULL = без payload; TYPE_ERROR = непознат) */
Type *type_effect_payload(Type *t, const char *effect) {
    if (!t || !effect) return NULL;
    for (int i = 0; i < t->n_effects; i++) {
        if (strcmp(t->effects[i], effect) == 0) {
            return t->effect_payloads ? t->effect_payloads[i] : NULL;
        }
    }
    return NULL;
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
            if (t->effect_payloads) {
                for (int j = i; j < t->n_effects - 1; j++)
                    t->effect_payloads[j] = t->effect_payloads[j + 1];
            }
            t->n_effects--;
            return;
        }
    }
}

void type_merge_effects(Type *dst, Type *src) {
    if (!dst || !src) return;
    for (int i = 0; i < src->n_effects; i++) {
        Type *pl = src->effect_payloads ? src->effect_payloads[i] : NULL;
        if (type_has_effect(dst, src->effects[i])) {
            /* M20: същият ефект с payload от две места — слей */
            Type *cur = type_effect_payload(dst, src->effects[i]);
            if (!cur && pl) {
                for (int j = 0; j < dst->n_effects; j++)
                    if (strcmp(dst->effects[j], src->effects[i]) == 0)
                        dst->effect_payloads[j] = pl;
            }
        } else {
            type_add_effect(dst, src->effects[i]);
            dst->effect_payloads[dst->n_effects - 1] = pl;
        }
    }
}

/* ============================================================
 *  Type environment
 * ============================================================ */




/* M21: регистърни записи (споделени между CheckCtx и snapshot-а) */
/* M23: trait + impl регистри */


/* L6 forward декларации — дефинициите са след import alias таблицата. */
char *mod_base(const char *path);
Type *resolve_type_node(CheckCtx *ctx, Node *ty);
Node *find_struct_scoped(CheckCtx *ctx, const char *name,
                                const char *posfile, int *ambiguous);
void struct_amb_hint(CheckCtx *ctx, const char *name,
                            char *m1, char *m2, size_t cap);

/* M24: спаси/възстанови substitution — вложените (literal/field) я
 * презаписват, затова се копира настрани */
void subst_save(CheckCtx *ctx, char ***sn, Type ***st, int *n) {
    *n = ctx->g_names.len;
    *sn = malloc(sizeof(char *) * (size_t)(*n ? *n : 1));
    *st = malloc(sizeof(Type *) * (size_t)(*n ? *n : 1));
    if (*n) {
        memcpy(*sn, ctx->g_names.data, sizeof(char *) * (size_t)*n);
        memcpy(*st, ctx->g_types.data, sizeof(Type *) * (size_t)*n);
    }
}
void subst_restore(CheckCtx *ctx, char **sn, Type **st, int n) {
    ctx->g_names.len = 0;
    ctx->g_types.len = 0;
    for (int i = 0; i < n; i++) {
        vec_push(ctx->g_names, sn[i]);
        vec_push(ctx->g_types, st[i]);
    }
    free(sn);
    free(st);
}

/* M21: forward — дефиницията е преди check_fn */
Type *generic_instantiate(CheckCtx *ctx, Node *fn, Node *call);
void check_fn(CheckCtx *ctx, Node *fn);
void mem3_ensure_fn(CheckCtx *ctx, Node *fn);
FnRec *fn_rec_of(CheckCtx *ctx, Node *fn);
FnRec *find_fn_rec(CheckCtx *ctx, const char *name);
void check_error(CheckCtx *ctx, SrcPos pos, const char *fmt, ...);
static void check_warn(CheckCtx *ctx, SrcPos pos, const char *fmt, ...);
void bind_type_params(CheckCtx *ctx, Node *tn, Type *at,
                             char **tps, int np, Type **bind,
                             SrcPos pos, const char *fnname);
/* M25: generic fn като стойност чрез явна fn анотация (деф. след
 * generic_instantiate) */
int generic_fn_value_bind(CheckCtx *ctx, Node *id, Type *annot);

/* M23: регистрира fn (топ-левел или impl метод) в fn регистъра */
void register_fn(CheckCtx *ctx, Node *item) {
    /* M21: типовите параметри се виждат като TYPE_VAR при резолване */
    ctx->cur_type_params = (const char **)item->type_params;
    ctx->cur_n_type_params = item->n_type_params;
    Type *ret = item->ret_type ? resolve_type_node(ctx, item->ret_type) : type_new(TYPE_VOID);
    int np = item->params.len;
    Type **params = NULL;
    if (np > 0) {
        params = malloc(sizeof(Type *) * (size_t)np);
        for (int j = 0; j < np; j++)
            params[j] = resolve_type_node(ctx, item->params.data[j]->param_type);
    }
    Type *ft = type_fn(ret, params, np);
    ft->name = strdup(item->fn_name);
    ctx->cur_type_params = NULL;
    ctx->cur_n_type_params = 0;

    if (ctx->n_fns < FNS_MAX) {
        ctx->fns[ctx->n_fns].name = ft->name;  /* strdup-натото късо име */
        ctx->fns[ctx->n_fns].origin = mod_base(item->pos.file);
        ctx->fns[ctx->n_fns].fn_type = ft;
        ctx->fns[ctx->n_fns].decl = item;
        ctx->fns[ctx->n_fns].ret_arena_param = -1;
        ctx->fns[ctx->n_fns].ret_handle_param = -1;
        ctx->fns[ctx->n_fns].checked = 0;
        ctx->fns[ctx->n_fns].checking = 0;
        ctx->n_fns++;
    } else {
        check_error(ctx, item->pos,
            "твърде много функции (лимит %d, FNS_MAX) — fn '%s' е отрязана; "
            "раздели модулите или вдигни лимита",
            FNS_MAX, item->fn_name);
    }
    item->type = ft;
}

/* M21: snapshot на регистрите за re-check на инстанции от codegen */

void check_error(CheckCtx *ctx, SrcPos pos, const char *fmt, ...) {
    if (ctx->chk->n_errors >= BAGA_MAX_ERRORS) return;
    int k = ctx->chk->n_errors;
    char *e = ctx->chk->errors[k];
    ctx->chk->error_pos[k] = pos;
    int off = snprintf(e, 256, "%d:%d: ", pos.line, pos.col);
    ctx->chk->error_off[k] = off > 255 ? 0 : off;
    ctx->chk->n_errors++;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(e + off, 256 - (size_t)off, fmt, ap);
    va_end(ap);
    /* същата грешка на същата позиция от два прохода — пазим само веднъж */
    if (ctx->chk->n_errors >= 2 &&
        strcmp(ctx->chk->errors[ctx->chk->n_errors - 2], e) == 0)
        ctx->chk->n_errors--;
}

/* MEM-3: предупреждение — не спира компилацията (за разлика от check_error). */
static void check_warn(CheckCtx *ctx, SrcPos pos, const char *fmt, ...) {
    if (ctx->chk->n_warnings >= BAGA_MAX_ERRORS) return;
    int k = ctx->chk->n_warnings;
    char *e = ctx->chk->warnings[k];
    ctx->chk->warning_pos[k] = pos;
    int off = snprintf(e, 256, "%d:%d: предупреждение: ", pos.line, pos.col);
    ctx->chk->warning_off[k] = off > 255 ? 0 : off;
    ctx->chk->n_warnings++;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(e + off, 256 - (size_t)off, fmt, ap);
    va_end(ap);
    if (ctx->chk->n_warnings >= 2 &&
        strcmp(ctx->chk->warnings[ctx->chk->n_warnings - 2], e) == 0)
        ctx->chk->n_warnings--;
}

static int type_is_drop_owned(Type *t) {
    return t && (t->kind == TYPE_VEC || t->kind == TYPE_MAP ||
                 t->kind == TYPE_BYTES || t->kind == TYPE_FN);
}

static int arena_was_freed(CheckCtx *ctx, int id) {
    if (id <= 0) return 0;
    for (int i = 0; i < ctx->n_arena_freed; i++)
        if (ctx->arena_freed_ids[i] == id) return 1;
    return 0;
}

static void arena_note_freed(CheckCtx *ctx, int id) {
    if (id <= 0 || arena_was_freed(ctx, id)) return;
    if (ctx->n_arena_freed < 128)
        ctx->arena_freed_ids[ctx->n_arena_freed++] = id;
}

/* Има ли още жив алиас на този handle в обграждащ scope? */
static int arena_live_outer(CheckCtx *ctx, int id) {
    if (id <= 0 || ctx->depth < 2) return 0;
    for (int d = 0; d < ctx->depth - 1; d++) {
        EnvScope *s = &ctx->scopes[d];
        for (int i = 0; i < s->count; i++) {
            EnvEntry *e = &s->entries[i];
            if (e->is_arena && e->arena_id == id && !e->dropped) return 1;
        }
    }
    return 0;
}

/* Scope-exit leak scan. skip_name — ident, върнат от функцията (move, не leak).
 * Забравена арена (няма arena_free никъде във fn, последният алиас си отива)
 * е грешка винаги. Vec/Map/bytes/fn — само под --warn-leaks. */
static void leak_scan_scope(CheckCtx *ctx, const char *skip_name) {
    if (!ctx->chk || ctx->depth <= 0) return;
    EnvScope *s = &ctx->scopes[ctx->depth - 1];
    int skip_aid = 0;
    if (skip_name) {
        for (int j = 0; j < s->count; j++)
            if (s->entries[j].name &&
                strcmp(s->entries[j].name, skip_name) == 0 &&
                s->entries[j].is_arena)
                skip_aid = s->entries[j].arena_id;
    }
    int seen_aid[64];
    int nseen = 0;
    for (int i = 0; i < s->count; i++) {
        EnvEntry *e = &s->entries[i];
        if (e->is_param || e->dropped) continue;
        if (skip_name && e->name && strcmp(e->name, skip_name) == 0) continue;
        /* return b където b = a (същият arena_id) — не тече handle-ът */
        if (skip_aid && e->is_arena && e->arena_id == skip_aid) continue;
        if (ctx->chk->warn_leaks && type_is_drop_owned(e->type))
            check_warn(ctx, e->pos, "изтичане: '%s' излиза от scope без drop", e->name);
        if (!e->is_arena) continue;
        int dup = 0;
        for (int k = 0; k < nseen; k++)
            if (seen_aid[k] == e->arena_id) { dup = 1; break; }
        if (dup) continue;
        if (nseen < 64) seen_aid[nseen++] = e->arena_id;
        if (arena_live_outer(ctx, e->arena_id)) continue;
        if (arena_was_freed(ctx, e->arena_id)) {
            /* free има във fn, но не на този път — maybe-leak, само --warn-leaks */
            if (ctx->chk->warn_leaks)
                check_warn(ctx, e->pos,
                    "изтичане: арена '%s' излиза от scope без arena_free", e->name);
            continue;
        }
        check_error(ctx, e->pos,
            "арена '%s' излиза от scope без arena_free", e->name);
    }
}

/* Map a type AST node to a Type */
Type *resolve_type_node(CheckCtx *ctx, Node *ty);
static Type *resolve_type_node_inner(CheckCtx *ctx, Node *ty) {
    if (!ty) return type_new(TYPE_VOID);
    switch (ty->kind) {
        case NODE_TYPE:
            /* M21: активна substitution или типова променлива на текущата
             * generic fn? */
            for (int gi = 0; gi < ctx->g_names.len; gi++)
                if (strcmp(ctx->g_names.data[gi], ty->type_name) == 0)
                    return ctx->g_types.data[gi];
            for (int ti = 0; ti < ctx->cur_n_type_params; ti++)
                if (strcmp(ctx->cur_type_params[ti], ty->type_name) == 0) {
                    Type *v = type_new(TYPE_VAR);
                    v->name = strdup(ty->type_name);
                    return v;
                }
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
                        el->kind != TYPE_VEC && el->kind != TYPE_ENUM &&
                        el->kind != TYPE_VAR) {
                        check_error(ctx, ty->pos,
                            "Vec<T>: неподдържан елементен тип %s (поддържат се i64, str, f64, bytes, struct, sum enum, fn, Vec и типови параметри)",
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
                        kt->kind != TYPE_BYTES && kt->kind != TYPE_VAR) {
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
                        vt->kind != TYPE_ENUM && vt->kind != TYPE_VAR) {
                        check_error(ctx, ty->pos,
                            "Map<K, V>: неподдържан стойностен тип %s (поддържат се i64, str, f64, bytes, struct, sum enum, fn и типови параметри)",
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
                /* M24: generic struct — Pair<i64, str> */
                if (ty->gen_type_args.len > 0) {
                    if (!sd || sd->n_struct_params == 0) {
                        check_error(ctx, ty->pos,
                            "struct '%s' не е generic — типови аргументи са излишни",
                            ty->type_name);
                    } else if (ty->gen_type_args.len != sd->n_struct_params) {
                        check_error(ctx, ty->pos,
                            "struct '%s' очаква %d типови аргумента, получих %d",
                            ty->type_name, sd->n_struct_params, ty->gen_type_args.len);
                    } else {
                        Type *gt = type_new(TYPE_STRUCT);
                        gt->name = strdup(sd->struct_name);
                        gt->targs = calloc((size_t)ty->gen_type_args.len, sizeof(Type *));
                        gt->n_targs = ty->gen_type_args.len;
                        int concrete = 1;
                        for (int a = 0; a < ty->gen_type_args.len; a++) {
                            gt->targs[a] = resolve_type_node(ctx, ty->gen_type_args.data[a]);
                            if (gt->targs[a]->kind == TYPE_VAR ||
                                gt->targs[a]->kind == TYPE_ERROR)
                                concrete = 0;
                        }
                        /* регистрирай инстанцията (dedup) — codegen емитва
                         * typedef per инстанция; TYPE_VAR аргументите (в
                         * сигнатури на generic fn) не са реални инстанции */
                        if (!concrete) return gt;
                        int found = -1;
                        for (int k = 0; k < sd->struct_inst_count; k++) {
                            int same = 1;
                            for (int a = 0; a < sd->n_struct_params; a++)
                                if (!type_eq(sd->struct_inst_targs[k * sd->n_struct_params + a],
                                             gt->targs[a])) { same = 0; break; }
                            if (same) { found = k; break; }
                        }
                        if (found < 0) {
                            int idx = sd->struct_inst_count;
                            sd->struct_inst_targs = realloc(sd->struct_inst_targs,
                                sizeof(Type *) * (size_t)((idx + 1) * sd->n_struct_params));
                            for (int a = 0; a < sd->n_struct_params; a++)
                                sd->struct_inst_targs[idx * sd->n_struct_params + a] = gt->targs[a];
                            sd->struct_inst_count = idx + 1;
                        }
                        return gt;
                    }
                }
                Type *t = type_new(TYPE_STRUCT);
                t->name = strdup(sd ? sd->struct_name : ty->type_name);
                /* Честна грешка за „широчинни" имена, които не са типове в
                 * Бага: иначе `u32`/`u64`/`u8` тихо става номинален struct и
                 * `let x: u32 = 5` гърми с подвеждащо „несъвместими типове"
                 * вместо да каже, че типът не съществува. Поддържат се само
                 * i32/i64. */
                if (!sd) {
                    static const char *phantom[] = {
                        "u8", "u16", "u32", "u64", "usize", "isize",
                        "i8", "i16", "f32", "u128", "i128", NULL
                    };
                    for (int pi = 0; phantom[pi]; pi++) {
                        if (strcmp(ty->type_name, phantom[pi]) != 0) continue;
                        check_error(ctx, ty->pos,
                            "непознат тип '%s' — поддържат се само i32 и i64 (цяло число) и f64 (дробно)",
                            ty->type_name);
                        return type_new(TYPE_ERROR);
                    }
                }
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
            for (int i = 0; i < ty->n_effects; i++) {
                type_add_effect(base, ty->effect_names[i]);
                /* M20: payload тип */
                if (ty->effect_payloads && ty->effect_payloads[i]) {
                    Type *pl = resolve_type_node(ctx, ty->effect_payloads[i]);
                    for (int j = 0; j < base->n_effects; j++)
                        if (strcmp(base->effects[j], ty->effect_names[i]) == 0)
                            base->effect_payloads[j] = pl;
                }
            }
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

/* M21: обвивка — резултатът се пази на ty->type (codegen при
 * мономорфизация чете конкретния тип на типовите параметри) */
Type *resolve_type_node(CheckCtx *ctx, Node *ty) {
    Type *t = resolve_type_node_inner(ctx, ty);
    if (ty) ty->type = t;
    return t;
}

void push_scope(CheckCtx *ctx) {
    if (ctx->depth < ENV_MAX) {
        ctx->scopes[ctx->depth].count = 0;
        ctx->depth++;
    }
}

void pop_scope_skip(CheckCtx *ctx, const char *skip_name) {
    leak_scan_scope(ctx, skip_name);
    if (ctx->depth > 0) ctx->depth--;
}

void pop_scope(CheckCtx *ctx) {
    pop_scope_skip(ctx, NULL);
}

void env_define(CheckCtx *ctx, const char *name, Type *type, SrcPos pos) {
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
        s->entries[s->count].is_arena = 0;
        s->entries[s->count].arena_id = 0;
        s->entries[s->count].region_id = 0;
        s->entries[s->count].is_world = 0;
        s->entries[s->count].world_frozen = 0;
        s->entries[s->count].pos = pos;
        s->count++;
    }
}

void env_define_mut(CheckCtx *ctx, const char *name, Type *type, int is_mut, SrcPos pos) {
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
        s->entries[s->count].is_arena = 0;
        s->entries[s->count].arena_id = 0;
        s->entries[s->count].region_id = 0;
        s->entries[s->count].is_world = 0;
        s->entries[s->count].world_frozen = 0;
        s->entries[s->count].pos = pos;
        s->count++;
    }
}

/* MEM-3: маркирай локал като dropped + лог за if/match join */
static void mem3_note_dropped(CheckCtx *ctx, EnvEntry *e) {
    if (!e || e->dropped) return;
    e->dropped = 1;
    if (ctx->n_drop_log < 256)
        ctx->drop_log[ctx->n_drop_log++] = e;
    else
        ctx->drop_log_overflowed = 1;
}

/* MEM-3: payload-и с region_id == arena_id умират с handle-а */
static void mem3_invalidate_region(CheckCtx *ctx, int arena_id) {
    if (arena_id <= 0) return;
    for (int d = 0; d < ctx->depth; d++) {
        EnvScope *s = &ctx->scopes[d];
        for (int i = 0; i < s->count; i++) {
            EnvEntry *e = &s->entries[i];
            if (e->region_id == arena_id) mem3_note_dropped(ctx, e);
        }
    }
}

/* MEM-3: free на handle — payload-и + всички алиаси на същия arena_id */
void mem3_drop_handle(CheckCtx *ctx, EnvEntry *h) {
    if (!h) return;
    int id = h->arena_id;
    arena_note_freed(ctx, id);
    mem3_invalidate_region(ctx, id);
    if (id <= 0) {
        mem3_note_dropped(ctx, h);
        return;
    }
    for (int d = 0; d < ctx->depth; d++) {
        EnvScope *s = &ctx->scopes[d];
        for (int i = 0; i < s->count; i++) {
            EnvEntry *e = &s->entries[i];
            if (e->is_arena && e->arena_id == id) mem3_note_dropped(ctx, e);
        }
    }
}

/* W2.2: free на свят — умират handle-ът, алиасите му И всички заемани от
 * региона низове (`let s = wvec_get_str(w, …)`). Иначе `world_free(w);
 * print(s)` беше heap-use-after-free, който checker-ът приемаше (ASan го
 * потвърди). Същата region-tagging машина като MEM-3, но за is_world —
 * без arena_freed_ids и без is_arena алиас-логиката (световете не са арени). */
void mem3_free_world(CheckCtx *ctx, EnvEntry *h) {
    if (!h) return;
    int id = h->arena_id;
    mem3_note_dropped(ctx, h);
    if (id <= 0) return;
    mem3_invalidate_region(ctx, id);   /* заеманите от региона низове */
    for (int d = 0; d < ctx->depth; d++) {
        EnvScope *s = &ctx->scopes[d];
        for (int i = 0; i < s->count; i++) {
            EnvEntry *e = &s->entries[i];
            if (e->is_world && e->arena_id == id) mem3_note_dropped(ctx, e);
        }
    }
}

/* „Имахте предвид …?“ — Damerau–Levenshtein разстояние (optimal string
 * alignment: съседната транспозиция е 1 редакция, „totla“→„total“).
 * Имена над 63 байта не се подсказват (връщаме 99) — матрицата е фиксирана. */
static int ck_edit_dist(const char *a, const char *b) {
    int la = (int)strlen(a), lb = (int)strlen(b);
    if (la == 0 || lb == 0 || la > 63 || lb > 63) return 99;
    int d[64][64];
    for (int i = 0; i <= la; i++) d[i][0] = i;
    for (int j = 0; j <= lb; j++) d[0][j] = j;
    for (int i = 1; i <= la; i++) {
        for (int j = 1; j <= lb; j++) {
            int cost = a[i - 1] == b[j - 1] ? 0 : 1;
            int v = d[i - 1][j] + 1;
            if (d[i][j - 1] + 1 < v) v = d[i][j - 1] + 1;
            if (d[i - 1][j - 1] + cost < v) v = d[i - 1][j - 1] + cost;
            if (i > 1 && j > 1 && a[i - 1] == b[j - 2] && a[i - 2] == b[j - 1] &&
                d[i - 2][j - 2] + 1 < v)
                v = d[i - 2][j - 2] + 1;
            d[i][j] = v;
        }
    }
    return d[la][lb];
}

/* Най-близкият кандидат в рамките на прага: 1 редакция за имена 3–5 байта,
 * 2 за по-дълги. Под 3 байта не подсказваме — там всичко прилича на всичко.
 * При равно разстояние печели по-ранният кандидат (при променливи това е
 * най-вътрешният scope). */
const char *ck_suggest(const char *name, const char *const *cands, int n) {
    size_t len = strlen(name);
    if (len < 3) return NULL;
    int limit = len >= 6 ? 2 : 1;
    int best = limit + 1;
    const char *pick = NULL;
    for (int i = 0; i < n; i++) {
        if (!cands[i]) continue;
        int dist = ck_edit_dist(name, cands[i]);
        if (dist < best) { best = dist; pick = cands[i]; }
    }
    return best <= limit ? pick : NULL;
}

/* MEM-2: като env_lookup, но връща самата EnvEntry (за drop seatbelt-а).
 * env_lookup пази подписа си. */
EnvEntry *ck_env_find(CheckCtx *ctx, const char *name) {
    for (int d = ctx->depth - 1; d >= 0; d--) {
        EnvScope *s = &ctx->scopes[d];
        for (int i = s->count - 1; i >= 0; i--) {
            if (strcmp(s->entries[i].name, name) == 0)
                return &s->entries[i];
        }
    }
    return NULL;
}

/* MEM-3: от коя арена идва изразът (payload указател)? 0 = неизвестно.
 * Очевидни форми — ident с region_id, arena_alloc(a, n), p±n, if с
 * еднакъв id в двата клона. Несигурно → 0 (leak-safe). */
int region_of_expr(CheckCtx *ctx, Node *n) {
    if (!n) return 0;
    switch (n->kind) {
        case NODE_IDENT: {
            EnvEntry *e = ck_env_find(ctx, n->name);
            return e ? e->region_id : 0;
        }
        case NODE_CALL:
            /* W2.2: wvec_get_str(w, …) е ЗАЕМАН низ от региона на света —
             * умира с него (mem3_free_world), както arena payload-ите. */
            if (n->callee && n->callee->kind == NODE_IDENT &&
                (strcmp(n->callee->name, "wvec_get_str") == 0 ||
                 strcmp(n->callee->name, "wvec_get_bytes") == 0 ||
                 strcmp(n->callee->name, "wmap_get_str") == 0) &&
                n->args.len >= 1 && n->args.data[0]->kind == NODE_IDENT) {
                EnvEntry *h = ck_env_find(ctx, n->args.data[0]->name);
                return (h && h->is_world && h->arena_id > 0) ? h->arena_id : 0;
            }
            if (n->callee && n->callee->kind == NODE_IDENT &&
                strcmp(n->callee->name, "arena_alloc") == 0 &&
                n->args.len >= 1 && n->args.data[0]->kind == NODE_IDENT) {
                EnvEntry *h = ck_env_find(ctx, n->args.data[0]->name);
                return (h && h->arena_id > 0) ? h->arena_id : 0;
            }
            /* потребителска fn, която връща region на param k */
            if (n->callee && n->callee->kind == NODE_IDENT) {
                FnRec *fr = find_fn_rec(ctx, n->callee->name);
                if (fr && fr->checked && fr->ret_arena_param >= 0 &&
                    fr->ret_arena_param < n->args.len &&
                    n->args.data[fr->ret_arena_param]->kind == NODE_IDENT) {
                    EnvEntry *h = ck_env_find(ctx,
                        n->args.data[fr->ret_arena_param]->name);
                    return (h && h->arena_id > 0) ? h->arena_id : 0;
                }
            }
            return 0;
        case NODE_STRUCT_LIT: {
            int rid = 0;
            for (int i = 0; i < n->n_lit_fields; i++) {
                int r = region_of_expr(ctx, n->lit_values.data[i]);
                if (!r) continue;
                if (!rid) rid = r;
                else if (rid != r) return 0;
            }
            return rid;
        }
        case NODE_BINARY: {
            if (n->bin_op != OP_ADD && n->bin_op != OP_SUB) return 0;
            int l = region_of_expr(ctx, n->left);
            int r = region_of_expr(ctx, n->right);
            /* два указателя (p - q / p + q) — резултатът не е в региона */
            if (l && r) return 0;
            if (n->bin_op == OP_SUB) return l;   /* само p - n */
            return l ? l : r;                    /* n + p или p + n */
        }
        case NODE_EXPR_STMT:
            return region_of_expr(ctx, n->expr);
        case NODE_BLOCK:
            if (n->stmts.len == 0) return 0;
            return region_of_expr(ctx, n->stmts.data[n->stmts.len - 1]);
        case NODE_IF: {
            if (!n->else_br) return 0;
            int th = region_of_expr(ctx, n->then_br);
            int el = region_of_expr(ctx, n->else_br);
            return (th && th == el) ? th : 0;
        }
        default:
            return 0;
    }
}

/* MEM-3: handle identity без страничен ефект (без нов arena_new id). */
int lookup_arena_id(CheckCtx *ctx, Node *n) {
    if (!n) return 0;
    if (n->kind == NODE_IDENT) {
        EnvEntry *e = ck_env_find(ctx, n->name);
        if (e && e->is_arena) return e->arena_id;
        return 0;
    }
    if (n->kind == NODE_CALL && n->callee && n->callee->kind == NODE_IDENT) {
        FnRec *fr = find_fn_rec(ctx, n->callee->name);
        if (fr && fr->checked && fr->ret_handle_param >= 0 &&
            fr->ret_handle_param < n->args.len &&
            n->args.data[fr->ret_handle_param]->kind == NODE_IDENT) {
            EnvEntry *h = ck_env_find(ctx, n->args.data[fr->ret_handle_param]->name);
            return (h && h->is_arena) ? h->arena_id : 0;
        }
    }
    return 0;
}

/* MEM-3: handle identity на израз — нов id при arena_new, копие при алиас. */
static int expr_arena_id(CheckCtx *ctx, Node *n) {
    if (!n) return 0;
    if (n->kind == NODE_CALL && n->callee && n->callee->kind == NODE_IDENT &&
        strcmp(n->callee->name, "arena_new") == 0)
        return ++ctx->next_arena_id;
    return lookup_arena_id(ctx, n);
}

/* W1.6: дали изразът ражда world handle — world_new/world_load или алиас */
static int expr_is_world(CheckCtx *ctx, Node *n) {
    if (!n) return 0;
    if (n->kind == NODE_IDENT) {
        EnvEntry *e = ck_env_find(ctx, n->name);
        return (e && e->is_world) ? 1 : 0;
    }
    if (n->kind == NODE_CALL && n->callee && n->callee->kind == NODE_IDENT &&
        !find_fn_rec(ctx, n->callee->name)) {
        const char *f = n->callee->name;
        /* W2.5: `world_linked(p, i)` връща handle на ЦЕЛТА — първокласен свят
         * за seatbelt-а (двоен free, mutate-after-freeze, borrow-и). Кой точно
         * свят е, статично не се знае, затова получава собствен region id. */
        return (strcmp(f, "world_new") == 0 || strcmp(f, "world_load") == 0 ||
                strcmp(f, "world_linked") == 0) ? 1 : 0;
    }
    return 0;
}

void mem3_bind(CheckCtx *ctx, EnvEntry *pe, Node *init) {
    if (!pe || !init) return;
    /* W1.6: world handle — алиасът наследява frozen състоянието на източника
     * (консервативно: по-нататъшните freeze/free се логват по име, MEM-1
     * граница — по-дълбоки графове от алиаси не се следят) */
    if (expr_is_world(ctx, init)) {
        pe->is_world = 1;
        pe->world_frozen = 0;
        if (init->kind == NODE_IDENT) {
            EnvEntry *src = ck_env_find(ctx, init->name);
            if (src && src->world_frozen) world_note_frozen(ctx, pe);
        }
    } else {
        pe->is_world = 0;
        pe->world_frozen = 0;
    }
    int rid = region_of_expr(ctx, init);
    int aid = expr_arena_id(ctx, init);
    if (aid > 0) {
        /* реален handle: arena_new или алиас на is_arena */
        pe->is_arena = 1;
        pe->arena_id = aid;
        pe->region_id = 0;
        return;
    }
    pe->is_arena = 0;
    pe->region_id = rid;
    pe->arena_id = 0;
    /* копие на i64 param — пази potential-handle id без is_arena,
     * за `let b = a; return b` в wrap */
    if (init->kind == NODE_IDENT) {
        EnvEntry *src = ck_env_find(ctx, init->name);
        if (src && src->arena_id > 0) pe->arena_id = src->arena_id;
    }
    /* W2.2: светът получава region id (общо пространство с arena_id-тата), за
     * да умират заеманите от него низове заедно с него. При алиас id-то вече е
     * копирано от източника по-горе — заеманията са за РЕГИОНА, не за името. */
    if (pe->is_world && pe->arena_id == 0)
        pe->arena_id = ++ctx->next_arena_id;
}

/* MEM-2: drop-join машинерия — споделена от if/else, match и catch.
 * Логът е append-only; всеки алтернативен клон записва drop-овете си в
 * непрекъснат диапазон. Преди всеки нов клон предишните се "вдигат"
 * (dropped = 0), за да не ги вижда той. */

/* вдигни всички drop-ове, логнати от base нататък */
void drop_lift_from(CheckCtx *ctx, int base) {
    for (int i = base; i < ctx->n_drop_log; i++) ctx->drop_log[i]->dropped = 0;
}

/* двуклонен join (if/else, catch): [log_base, mid) = първи път,
 * [mid, n_drop_log) = втори път. Остава dropped само маркираното и в двата;
 * оцелелите се компактират на log_base, за да ги види външен конструкт.
 * При препълнен лог — консервативно вдигане на всичко (пропусната грешка,
 * никога измислена); нелогнатите drop-ове изобщо не са маркирани. */
void drop_join2(CheckCtx *ctx, int log_base, int mid) {
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
void drop_join_arms(CheckCtx *ctx, int log_base, const int *arm_end, int narm) {
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

/* W1.6: freeze-join машинерия — огледало на drop-версиите върху
 * world_frozen. Отделен лог, същата семантика: definitely frozen = замразено
 * във всеки клон; maybe-frozen (само в един) се вдига, защото seatbelt-ът
 * не бива да измисля грешки — пътевите случаи ги покрива верификаторът. */

/* маркирай world handle като замразен + лог за if/match join */
void world_note_frozen(CheckCtx *ctx, EnvEntry *e) {
    if (!e || e->world_frozen) return;
    e->world_frozen = 1;
    if (ctx->n_freeze_log < 256)
        ctx->freeze_log[ctx->n_freeze_log++] = e;
    else
        ctx->freeze_log_overflowed = 1;
}

/* вдигни всички freeze-ове, логнати от base нататък */
void freeze_lift_from(CheckCtx *ctx, int base) {
    for (int i = base; i < ctx->n_freeze_log; i++) ctx->freeze_log[i]->world_frozen = 0;
}

/* двуклонен join (if/else, catch) — вж. drop_join2 */
void freeze_join2(CheckCtx *ctx, int log_base, int mid) {
    if (ctx->freeze_log_overflowed) {
        freeze_lift_from(ctx, log_base);
        ctx->n_freeze_log = log_base;
        ctx->freeze_log_overflowed = 0;
        return;
    }
    int end = ctx->n_freeze_log;
    for (int j = mid; j < end; j++) {
        EnvEntry *e = ctx->freeze_log[j];
        int in_first = 0;
        for (int i = log_base; i < mid; i++)
            if (ctx->freeze_log[i] == e) { in_first = 1; break; }
        e->world_frozen = in_first;
    }
    int w = log_base;
    for (int j = mid; j < end; j++) {
        EnvEntry *e = ctx->freeze_log[j];
        if (!e->world_frozen) continue;
        int seen = 0;
        for (int k = log_base; k < w; k++)
            if (ctx->freeze_log[k] == e) { seen = 1; break; }
        if (!seen) ctx->freeze_log[w++] = e;
    }
    ctx->n_freeze_log = w;
}

/* N-arm join (match) — вж. drop_join_arms */
void freeze_join_arms(CheckCtx *ctx, int log_base, const int *arm_end, int narm) {
    if (ctx->freeze_log_overflowed || !arm_end || narm <= 0) {
        freeze_lift_from(ctx, log_base);
        ctx->n_freeze_log = log_base;
        ctx->freeze_log_overflowed = 0;
        return;
    }
    int w = log_base;
    for (int i = log_base; i < arm_end[0]; i++) {
        EnvEntry *e = ctx->freeze_log[i];
        int everywhere = 1;
        for (int k = 1; k < narm && everywhere; k++) {
            int found = 0;
            for (int j = arm_end[k - 1]; j < arm_end[k]; j++)
                if (ctx->freeze_log[j] == e) { found = 1; break; }
            if (!found) everywhere = 0;
        }
        e->world_frozen = everywhere;
        if (everywhere) {
            int seen = 0;
            for (int m = log_base; m < w; m++)
                if (ctx->freeze_log[m] == e) { seen = 1; break; }
            if (!seen) ctx->freeze_log[w++] = e;
        }
    }
    for (int k = 1; k < narm; k++) {
        for (int j = arm_end[k - 1]; j < arm_end[k]; j++) {
            EnvEntry *e = ctx->freeze_log[j];
            if (!e->world_frozen) continue;
            int in0 = 0;
            for (int i = log_base; i < arm_end[0]; i++)
                if (ctx->freeze_log[i] == e) { in0 = 1; break; }
            if (!in0) e->world_frozen = 0;
        }
    }
    ctx->n_freeze_log = w;
}

Type *env_lookup(CheckCtx *ctx, const char *name) {
    for (int d = ctx->depth - 1; d >= 0; d--) {
        EnvScope *s = &ctx->scopes[d];
        for (int i = s->count - 1; i >= 0; i--) {
            if (strcmp(s->entries[i].name, name) == 0)
                return s->entries[i].type;
        }
    }
    return NULL;
}

int env_is_mut(CheckCtx *ctx, const char *name) {
    for (int d = ctx->depth - 1; d >= 0; d--) {
        EnvScope *s = &ctx->scopes[d];
        for (int i = s->count - 1; i >= 0; i--) {
            if (strcmp(s->entries[i].name, name) == 0)
                return s->entries[i].is_mut;
        }
    }
    return 1;
}

Type *ck_find_fn(CheckCtx *ctx, const char *name) {
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

FnRec *find_fn_rec(CheckCtx *ctx, const char *name) {
    if (!name) return NULL;
    for (int i = 0; i < ctx->n_fns; i++) {
        if (ctx->fns[i].decl->fn_name &&
            strcmp(ctx->fns[i].decl->fn_name, name) == 0)
            return &ctx->fns[i];
    }
    for (int i = 0; i < ctx->n_fns; i++) {
        if (strcmp(ctx->fns[i].name, name) == 0)
            return &ctx->fns[i];
    }
    return NULL;
}

FnRec *fn_rec_of(CheckCtx *ctx, Node *fn) {
    if (!fn) return NULL;
    for (int i = 0; i < ctx->n_fns; i++)
        if (ctx->fns[i].decl == fn) return &ctx->fns[i];
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
const char *type_short_name(const char *name) {
    const char *d = strrchr(name, '.');
    return d ? d + 1 : name;
}

/* L6: struct decl по име от гледна точка posfile. Пълното име печели
 * винаги; при късо име — собственият модул, после единственият кандидат;
 * повече от един чужд → *ambiguous = 1 и NULL (поикащият диагностицира). */
Node *find_struct_scoped(CheckCtx *ctx, const char *name,
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
void struct_amb_hint(CheckCtx *ctx, const char *name,
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
char *mod_base(const char *path) {
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

