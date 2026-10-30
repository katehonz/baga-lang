/* checker_impl.h — вътрешен header на checker модулите (checker_env.c,
 * checker_infer.c, checker_expr.c, checker_fn.c). Типове и прототипи
 * между тях. Публичният API (check_program, type_* и т.н.) е в baga.h. */
#ifndef BAGA_CHECKER_IMPL_H
#define BAGA_CHECKER_IMPL_H

#include "baga.h"
#include <stdint.h>

/* ============================ константи ============================ */

#define ENV_MAX 64
#define ENV_VARS 256
/* Per translation unit. http+jsonx+orm+pg already exceeds 256; frameworks
 * (fmrbaga) and multi-product apps need headroom. Silent drop when full.
 * 4096: boilaDB serve + pgbaga в един TU (boila_scram_test) прескочи 2048
 * с P43 растежа (29 fn-а бяха отрязани). */
#define FNS_MAX  4096

/* ============================ типове ============================ */

typedef struct EnvEntry EnvEntry;
/* W3.5: колко региона може да „замърсяват" един контейнер едновременно. Над
 * капацитета спираме да следим — пропусната грешка, никога измислена (M8). */
#define W35_TAINT_MAX 4
struct EnvEntry {
    char *name;
    Type *type;
    int is_mut;
    /* MEM-2: drop seatbelt — per-variable live/dropped състояние */
    int dropped;      /* drop() е извикан върху тази локална */
    int is_world;     /* W1.6: локалата е world handle (let w = world_new()) */
    int world_frozen; /* W1.6: ОПРЕДЕЛЕН world_freeze (join-консервативно:
                       * maybe-frozen след if-клон остава 0 — верификаторът
                       * покрива пътевите случаи, seatbelt-ът не измисля) */
    int is_param;     /* параметър — буферът е на извикващия, drop забранен */
    int captured;     /* заснет от ламбда — drop би оставил висящ указател */
    int scope_depth;  /* ctx->depth при дефиницията (за loop-правилото) */
    int is_arena;     /* arena handle (arena_new или алиас на такъв) */
    int arena_id;     /* >0: identity на handle-а (споделена от let b = a) */
    int region_id;    /* >0: payload от arena_alloc с този arena_id */
    /* W3.5: контейнер (Vec/Map), в който е записана ЗАЕМЕНА стойност от регион
     * (свят или арена). Самият контейнер живее в bump арената, но елементите
     * му сочат в региона — след free той държи висящи указатели, които
     * `vec_get` връща като обикновени стойности. Затова се следи отделно от
     * region_id: payload-локалата умира при free, контейнерът трябва да бъде
     * ОБВИНЕН на мястото на free-а. */
    int taint[W35_TAINT_MAX];
    int taint_n;
    SrcPos pos;       /* позиция на let (за leak предупреждения) */
};
typedef struct {
    EnvEntry entries[ENV_VARS];
    int count;
} EnvScope;
typedef struct {
    char *name;          /* късо име (както е в източника) */
    const char *origin;  /* модул: basename на файла без .baga ("" = неизвестен) */
    Type *fn_type;   /* TYPE_FN */
    Node *decl;      /* NODE_FN */
    /* MEM-3: -1 = не връща region/handle; иначе индекс на param.
     * ret_arena_param — payload (arena_alloc); ret_handle_param — самия handle. */
    int ret_arena_param;
    int ret_handle_param;
    int checked;
    int checking;
} FnRec;
typedef struct { char *name; Node *decl; } StructRec;
typedef struct { char *name; Node *decl; } EnumRec;
typedef struct { char *name; Node *decl; } TraitRec;
typedef struct {
    char *trait;     /* trait име */
    char *type_name; /* името на типа (struct/builtin) */
    Node *decl;      /* NODE_IMPL */
} ImplRec;
typedef struct {
    char *variant;
    char *enum_name;
    int value;
    Type *payload;       /* L3: NULL = plain variant */
} VariantRec;
typedef struct {
    EnvScope scopes[ENV_MAX];
    int depth;

    /* function registry */
    FnRec fns[FNS_MAX];
    int n_fns;

    /* struct registry */
    StructRec structs[FNS_MAX];
    int n_structs;

    /* enum registry */
    EnumRec enums[FNS_MAX];
    int n_enums;

    /* enum variant → value mapping */
    VariantRec variants[FNS_MAX * 4];
    int n_variants;

    /* M23: trait/impl регистри */
    TraitRec traits[FNS_MAX];
    int n_traits;
    ImplRec impls[FNS_MAX];
    int n_impls;

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
    /* W1.6: паралелен лог за world_frozen — същата lift/join семантика като
     * drop_log (отделен масив, за да не се пипа MEM-2 машинерията) */
    EnvEntry *freeze_log[256];
    int n_freeze_log;
    int freeze_log_overflowed;
    int loop_depth;   /* 0 = извън цикъл; иначе depth-а на обграждащия цикъл */
    int next_arena_id; /* MEM-3: монотонен id за arena_new (handle алиаси) */
    int arena_freed_ids[128]; /* MEM-3: arena_id-та, върху които имаше arena_free */
    int n_arena_freed;
    /* M21 generics: активна substitution (имена → конкретни типове) и
     * типовите параметри на текущо-декларираната fn (за TYPE_VAR) */
    VEC(char *) g_names;
    VEC(Type *) g_types;
    const char **cur_type_params;
    int cur_n_type_params;
} CheckCtx;
typedef struct {
    Node *program;
    const char *main_base;
    FnRec fns[FNS_MAX]; int n_fns;
    StructRec structs[FNS_MAX]; int n_structs;
    EnumRec enums[FNS_MAX]; int n_enums;
    VariantRec variants[FNS_MAX * 4]; int n_variants;
    TraitRec traits[FNS_MAX]; int n_traits;
    ImplRec impls[FNS_MAX]; int n_impls;
    int n_lambdas;
} CheckCtxSnap;

/* ============================ прототипи ============================ */

Type *type_new(TypeKind kind);   /* 7 */
Type *type_fn(Type *ret, Type **params, int nparams);   /* 14 */
const char *type_str(Type *t);   /* 33 */
int vec_elem_eq(Type *a, Type *b);   /* 78 */
int type_eq(Type *a, Type *b);   /* 92 */
int type_assignable(Type *arg, Type *param);   /* 137 */
void type_add_effect(Type *t, const char *effect);   /* 150 */
Type *type_effect_payload(Type *t, const char *effect);   /* 163 */
int type_has_effect(Type *t, const char *effect);   /* 173 */
void type_remove_effect(Type *t, const char *effect);   /* 181 */
void type_merge_effects(Type *dst, Type *src);   /* 198 */
void subst_save(CheckCtx *ctx, char ***sn, Type ***st, int *n);   /* 338 */
void subst_restore(CheckCtx *ctx, char **sn, Type **st, int n);   /* 347 */
void register_fn(CheckCtx *ctx, Node *item);   /* 374 */
void check_error(CheckCtx *ctx, SrcPos pos, const char *fmt, ...);   /* 423 */
Type *resolve_type_node(CheckCtx *ctx, Node *ty);   /* 762 */
void push_scope(CheckCtx *ctx);   /* 768 */
void pop_scope_skip(CheckCtx *ctx, const char *skip_name);   /* 775 */
void pop_scope(CheckCtx *ctx);   /* 780 */
void env_define(CheckCtx *ctx, const char *name, Type *type, SrcPos pos);   /* 784 */
void env_define_mut(CheckCtx *ctx, const char *name, Type *type, int is_mut, SrcPos pos);   /* 809 */
void mem3_drop_handle(CheckCtx *ctx, EnvEntry *h, SrcPos pos);   /* 857 */
void mem3_free_world(CheckCtx *ctx, EnvEntry *h, SrcPos pos);   /* W2.2: free на свят + заеманите низове */
void mem3_taint_container(CheckCtx *ctx, Node *container, int rid);   /* W3.5: заемана стойност влиза в контейнер */
int mem3_container_region(CheckCtx *ctx, Node *container);   /* W3.5: единственият регион на контейнер или 0 */
EnvEntry *ck_env_find(CheckCtx *ctx, const char *name);   /* 877 */
const char *ck_suggest(const char *name, const char *const *cands, int n);
int region_of_expr(CheckCtx *ctx, Node *n);   /* 891 */
int lookup_arena_id(CheckCtx *ctx, Node *n);   /* 953 */
void mem3_bind(CheckCtx *ctx, EnvEntry *pe, Node *init);   /* 981 */
void drop_lift_from(CheckCtx *ctx, int base);   /* 1009 */
void drop_join2(CheckCtx *ctx, int log_base, int mid);   /* 1018 */
void drop_join_arms(CheckCtx *ctx, int log_base, const int *arm_end, int narm);   /* 1048 */
void freeze_lift_from(CheckCtx *ctx, int base);   /* W1.6 */
void freeze_join2(CheckCtx *ctx, int log_base, int mid);   /* W1.6 */
void freeze_join_arms(CheckCtx *ctx, int log_base, const int *arm_end, int narm);   /* W1.6 */
void world_note_frozen(CheckCtx *ctx, EnvEntry *e);   /* W1.6 */
int spec_world_guarantee(CheckCtx *ctx, const char *fname);   /* W3.1: 0/1/3 */
Type *env_lookup(CheckCtx *ctx, const char *name);   /* 1088 */
int env_is_mut(CheckCtx *ctx, const char *name);   /* 1099 */
Type *ck_find_fn(CheckCtx *ctx, const char *name);   /* 1110 */
FnRec *find_fn_rec(CheckCtx *ctx, const char *name);   /* 1125 */
FnRec *fn_rec_of(CheckCtx *ctx, Node *fn);   /* 1139 */
const char *type_short_name(const char *name);   /* 1174 */
Node *find_struct_scoped(CheckCtx *ctx, const char *name, const char *posfile, int *ambiguous);   /* 1182 */
void struct_amb_hint(CheckCtx *ctx, const char *name, char *m1, char *m2, size_t cap);   /* 1216 */
char *mod_base(const char *path);   /* 1237 */
Type *infer_binary(CheckCtx *ctx, Node *n);   /* 1277 */
int fn_effects_subset(Type *val, Type *annot);   /* 1308 */
Type *infer_call(CheckCtx *ctx, Node *n);   /* 1358 */
Type *infer(CheckCtx *ctx, Node *n);   /* 2321 */
void bind_type_params(CheckCtx *ctx, Node *tn, Type *at, char **tps, int np, Type **bind, SrcPos pos, const char *fnname);   /* 3509 */
Type *generic_instantiate(CheckCtx *ctx, Node *fn, Node *call);   /* 3714 */
int generic_fn_value_bind(CheckCtx *ctx, Node *id, Type *annot);   /* 3774 */
void mem3_ensure_fn(CheckCtx *ctx, Node *fn);   /* 3924 */
void check_fn(CheckCtx *ctx, Node *fn);   /* 3931 */
const char *baga_note_import_alias(const char *canon_path, const char *alias);   /* 1152 */
void checker_recheck_inst(Checker *chk, Node *fn, int k);   /* 4424 */
void checker_recheck_spec(Checker *chk, Node *spec, Node *fn, int k);   /* 4462 */
void check_program(Checker *c, Node *program);   /* 4067 */

#endif /* BAGA_CHECKER_IMPL_H */
