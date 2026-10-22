/* codegen_llvm_impl.h — вътрешен header на codegen_llvm модулите
 * (codegen_llvm_util.c, _rc.c, _vec.c, _map.c, _drop.c, _expr.c, _decl.c).
 * Типове и прототипи между тях. Публичният API (codegen_llvm) е в baga.h.
 * Само за baga-llvm (BAGA_LLVM) — без флага всичко изчезва. */
#ifndef BAGA_CODEGEN_LLVM_IMPL_H
#define BAGA_CODEGEN_LLVM_IMPL_H

#ifdef BAGA_LLVM

#include "baga.h"
#include <llvm-c/Core.h>
#include <llvm-c/Analysis.h>
#include <llvm-c/Target.h>
#include <stdint.h>
#include <stdio.h>

/* ============================ константи ============================ */

#define BAGA_RC_MAGIC 0xBA6A4D454D344848ULL
#define BAGA_RC_HSIZE 32
#define LLVM_MAX_VARS 256

/* ============================ типове ============================ */

typedef struct { const char *name; int tag; Type *type; Node *type_node;
                 int is_param; int dead;
                 int dead_linear; } LLRcLocal;
typedef struct { int top; int is_loop; } LLRcScope;
typedef struct { Node *site; int tag; Type *type; LLVMValueRef val;
                 int consumed; } LLRcTmp;
typedef struct {
    LLVMContextRef ctx;
    LLVMModuleRef mod;
    LLVMBuilderRef builder;
    LLVMTypeRef i64_ty;
    LLVMTypeRef i32_ty;
    LLVMTypeRef i8_ty;
    LLVMTypeRef i1_ty;
    LLVMTypeRef double_ty;
    LLVMTypeRef void_ty;
    LLVMTypeRef ptr_ty;
    LLVMTargetDataRef td;
    LLVMValueRef printf_fn;
    LLVMValueRef fprintf_fn;
    LLVMValueRef snprintf_fn;
    LLVMValueRef exit_fn;
    LLVMValueRef stderr_global;
    LLVMTypeRef cur_ret_ty;   /* върнатият тип на текущата функция */
    Node *program;            /* за enum варианти и spec-ове */
    int tmp_counter;
    /* M24: текущата generic struct инстанция при emit на типа */
    Node *gen_struct;
    int   gen_struct_inst;
    /* M21: текущата generic fn инстанция + Checker за recheck */
    Node *gen_fn;
    int   gen_inst;
    Checker *chk;
    int   rc;             /* --rc: RC паметов модел (паритет с C бекенда) */
    /* RC-LLVM scope стек: локали + scope граници на текущата функция */
    LLRcLocal lrc_locals[256];
    int   lrc_count;
    LLRcScope lrc_scopes[64];
    int   lrc_depth;
    int   lrc_fn_base;    /* индекс на scope-а на текущата функция */
    int   lrc_branch_depth; /* >0 = emit-ваме тяло на условен/цикълен клон */
    /* RC4-LLVM: активните temp-ове на текущия statement (само при lg.rc) */
    LLRcTmp lrc_tmps[64];
    int   lrc_tmp_count;
    int   lrc_tmps_on;
    Node *lrc_tmp_decl;   /* temp възелът, чиято стойност се emit-ва в момента
                           * (без самозаместване — като rc_tmp_decl в C) */
    /* M20: effect payload runtime */
    VEC(char *) eff_tags;   /* tag регистър (име → индекс+1) */
    int   eff_depth;        /* >0 = вътре в catch верига (TRY е no-op) */
    LLVMValueRef eff_global;/* global baga_eff_tl */
    LLVMTypeRef eff_ty;     /* { i64, i64, double, i8*, baga_bytes } */
} LLVMCodegen;
typedef struct { char *name; LLVMValueRef alloca; } LLVMVar;
typedef struct {
    LLVMVar vars[LLVM_MAX_VARS];
    int count;
    int scope_marks[64];
    int depth;
} LLVMSymtab;
typedef struct { int count; int on; Node *decl; LLRcTmp e[64]; } LLRcTmpSave;


/* ============================ споделено състояние ============================ */

extern LLVMCodegen lg;
extern LLVMSymtab lg_st;
extern int lrc_loop_next;

/* ============================ прототипи ============================ */

void llv_llvm_unsupported(const char *what);   /* 98 */
void llv_llvm_unsupported_node(Node *n);   /* 103 */
LLVMTypeRef llv_user_struct_ty_inst(Type *t);   /* 120 */
char *llv_llvm_inst_cname(Node *s, int k);   /* 128 */
char *llv_llvm_struct_cname(Type *t);   /* 157 */
int llv_is_sum_enum_item(Node *item);   /* 208 */
Node *llv_find_sum_enum(const char *name);   /* 216 */
int llv_sum_variant_index(Node *ed, const char *vname);   /* 228 */
LLVMTypeRef llv_llvm_type_resolved(Type *ty);   /* 235 */
LLVMTypeRef llv_llvm_type(Node *ty);   /* 269 */
char *llv_llvm_mangle(const char *name);   /* 326 */
char *llv_tmp_name(void);   /* 345 */
void llv_st_reset(void);   /* 363 */
void llv_st_push(void);   /* 364 */
void llv_st_pop(void);   /* 365 */
void llv_st_define(const char *name, LLVMValueRef alloca);   /* 366 */
LLVMValueRef llv_st_lookup(const char *name);   /* 372 */
LLVMValueRef llv_coerce(LLVMValueRef v, LLVMTypeRef target);   /* 380 */
LLVMValueRef llv_to_bool(LLVMValueRef v);   /* 411 */
LLVMValueRef llv_rt_libc(const char *name, LLVMTypeRef ret, LLVMTypeRef *params, int nparams);   /* 434 */
LLVMValueRef llv_rt_malloc(void);   /* 442 */
LLVMValueRef llv_rt_realloc(void);   /* 446 */
LLVMValueRef llv_rt_free(void);   /* 450 */
LLVMValueRef llv_rt_memcpy(void);   /* 454 */
LLVMValueRef llv_rt_strlen(void);   /* 458 */
LLVMValueRef llv_rt_strcmp(void);   /* 462 */
LLVMValueRef llv_h_call(LLVMValueRef fn, LLVMValueRef *args, int nargs, const char *name);   /* 468 */
void llv_h_begin(LLVMValueRef fn);   /* 475 */
LLVMValueRef llv_rc_hdr_field(LLVMValueRef base, int idx, const char *nm);   /* 493 */
LLVMValueRef llv_build_baga_rc_hdr(void);   /* 502 */
LLVMValueRef llv_build_baga_rc_alloc(void);   /* 537 */
LLVMValueRef llv_build_baga_str_len(void);   /* 565 */
LLVMValueRef llv_build_baga_set_slen(void);   /* 597 */
LLVMValueRef llv_build_baga_rc_retain(void);   /* 630 */
LLVMValueRef llv_build_baga_rc_release(const char *name, const char *msg);   /* 657 */
LLVMValueRef llv_rc_alloc_call(LLVMValueRef nbytes, const char *name);   /* 712 */
LLVMValueRef llv_str_alloc_call(LLVMValueRef nbytes, const char *name);   /* 722 */
int llv_lrc_find(const char *name);   /* 749 */
void llv_lrc_track_tag(const char *name, int tag, Type *t, Node *tnode, int is_param);   /* 759 */
void llv_lrc_track(const char *name, Type *t, int is_param);   /* 770 */
int llv_lrc_tag_node(Node *tn);   /* 778 */
int llv_lrc_struct_has_heap(const char *name);   /* 833 */
int llv_lrc_enum_has_heap(Node *item);   /* 856 */
int llv_lrc_heap_tag(Type *t);   /* 915 */
int llv_lrc_heap_tag_node(Node *ty);   /* 920 */
int llv_lrc_is_enum_ctor(Node *n);   /* 936 */
int llv_lrc_borrowed_init(Node *init);   /* 960 */
int llv_lrc_need_owned_retain(Node *val);   /* 980 */
void llv_lrc_embed_retain(Node *val, LLVMValueRef v);   /* 1000 */
int llv_lrc_vec_elem_kind(Type *vty, Node *tn);   /* 1020 */
int llv_lrc_map_val_tag(Type *mty, Node *tn);   /* 1044 */
void llv_lrc_emit_release(LLRcLocal *l);   /* 1121 */
void llv_lrc_emit_retain_val(int tag, Type *ty, Node *tnode, LLVMValueRef val);   /* 1189 */
int llv_lrc_tmp_fresh(Node *n);   /* 1228 */
int llv_lrc_tmp_find(Node *n);   /* 1322 */
void llv_lrc_tmp_consume(int i);   /* 1331 */
void llv_lrc_emit_release_val(int tag, Type *ty, LLVMValueRef val);   /* 1338 */
void llv_lrc_tmp_release_all(void);   /* 1383 */
void llv_lrc_tmp_restore(LLRcTmpSave *sv);   /* 1405 */
void llv_lrc_tmp_begin(Node *root, int root_bound, LLRcTmpSave *sv);   /* 1416 */
void llv_lrc_tmp_end(LLRcTmpSave *sv);   /* 1448 */
void llv_lrc_push_scope(int is_loop);   /* 1453 */
void llv_lrc_pop_scope(void);   /* 1463 */
void llv_lrc_release_all(int skip_idx);   /* 1482 */
void llv_lrc_release_to_loop(void);   /* 1497 */
void llv_lrc_return_move(Node *expr, LLVMValueRef v);   /* 1514 */
int llv_llvm_has_payload_effects(Type *t);   /* 1529 */
int llv_llvm_eff_tag(const char *name);   /* 1537 */
LLVMValueRef llv_eff_gep(unsigned idx, const char *name);   /* 1564 */
unsigned llv_eff_field_of(Type *pt);   /* 1572 */
void llv_h_ret_zero(void);   /* 1580 */
LLVMValueRef llv_entry_alloca(LLVMTypeRef ty, const char *name);   /* 1588 */
LLVMValueRef llv_build_baga_len(void);   /* 1601 */
LLVMValueRef llv_build_baga_char_at(void);   /* 1613 */
LLVMValueRef llv_build_baga_byte_at(void);   /* 1629 */
LLVMValueRef llv_build_baga_substr(void);   /* 1645 */
LLVMValueRef llv_build_baga_concat(void);   /* 1676 */
LLVMValueRef llv_build_baga_str_eq(void);   /* 1706 */
LLVMValueRef llv_build_baga_chr(void);   /* 1722 */
LLVMValueRef llv_build_baga_ord(void);   /* 1817 */
LLVMValueRef llv_build_baga_i64_to_str(void);   /* 1927 */
LLVMValueRef llv_build_baga_f64_to_str(void);   /* 2018 */
LLVMTypeRef llv_baga_vec_ptr_ty(void);   /* 2049 */
LLVMTypeRef llv_baga_bytes_ty(void);   /* 2055 */
LLVMValueRef llv_vec_field_ptr(LLVMValueRef v, unsigned idx, const char *nm);   /* 2065 */
LLVMValueRef llv_vec_load_len(LLVMValueRef v);   /* 2068 */
LLVMValueRef llv_vec_load_data(LLVMValueRef v);   /* 2071 */
LLVMValueRef llv_build_baga_vec_grow(void);   /* 2082 */
LLVMValueRef llv_build_baga_vec_new(void);   /* 2135 */
LLVMValueRef llv_build_baga_vec_push_i64(void);   /* 2168 */
LLVMValueRef llv_build_baga_vec_push_str(void);   /* 2185 */
LLVMValueRef llv_vec_load_at(LLVMValueRef v, LLVMValueRef i);   /* 2201 */
LLVMValueRef llv_build_baga_vec_get_i64(void);   /* 2209 */
LLVMValueRef llv_build_baga_vec_get_str(void);   /* 2221 */
LLVMValueRef llv_build_baga_vec_set_i64(void);   /* 2232 */
LLVMValueRef llv_build_baga_vec_set_str(void);   /* 2252 */
LLVMValueRef llv_build_baga_vec_len(void);   /* 2273 */
void llv_slice_clamp(LLVMValueRef v, LLVMValueRef *a, LLVMValueRef *b);   /* 2283 */
LLVMValueRef llv_build_baga_vec_slice(const char *suf, const char *push_rt);   /* 2319 */
LLVMValueRef llv_build_baga_vec_concat(const char *suf, const char *push_rt);   /* 2361 */
LLVMValueRef llv_build_baga_arena_new(void);   /* 2393 */
LLVMValueRef llv_build_baga_arena_alloc(void);   /* 2414 */
LLVMValueRef llv_build_baga_arena_reset(void);   /* 2452 */
LLVMValueRef llv_build_baga_arena_free(void);   /* 2466 */
LLVMValueRef llv_build_baga_vec_push_f64(void);   /* 2485 */
LLVMValueRef llv_build_baga_vec_get_f64(void);   /* 2498 */
LLVMValueRef llv_build_baga_vec_set_f64(void);   /* 2509 */
LLVMValueRef llv_bytes_param_alloca(LLVMValueRef v);   /* 2528 */
LLVMValueRef llv_bytes_load_data(LLVMValueRef a);   /* 2533 */
LLVMValueRef llv_bytes_load_len(LLVMValueRef a);   /* 2537 */
LLVMValueRef llv_build_baga_bytes_len(void);   /* 2556 */
LLVMValueRef llv_build_baga_bytes_at(void);   /* 2566 */
LLVMValueRef llv_build_baga_bytes_slice(void);   /* 2580 */
LLVMValueRef llv_build_baga_bytes_concat(void);   /* 2606 */
LLVMValueRef llv_build_baga_str_h(void);   /* 2630 */
LLVMValueRef llv_build_baga_h_str(void);   /* 2639 */
LLVMValueRef llv_build_baga_bytes_put(void);   /* 2649 */
LLVMValueRef llv_build_baga_bytes_h(void);   /* 2683 */
LLVMValueRef llv_build_baga_h_bytes(void);   /* 2701 */
LLVMValueRef llv_build_baga_bounds_fail(void);   /* 2725 */
LLVMValueRef llv_build_baga_bytes_new(void);   /* 2746 */
LLVMValueRef llv_build_baga_bytes_set(void);   /* 2762 */
LLVMValueRef llv_build_baga_bytes_push(void);   /* 2793 */
LLVMValueRef llv_build_baga_bytes_from_str(void);   /* 2814 */
LLVMValueRef llv_build_baga_bytes_to_str(void);   /* 2829 */
LLVMValueRef llv_build_baga_hex_val(void);   /* 2853 */
LLVMValueRef llv_build_baga_hex_encode(void);   /* 2910 */
LLVMValueRef llv_build_baga_hex_decode(void);   /* 2961 */
LLVMValueRef llv_build_baga_bytes_from_hex(void);   /* 3020 */
LLVMValueRef llv_build_baga_read_file(void);   /* 3036 */
LLVMTypeRef llv_user_struct_ty(const char *name);   /* 3098 */
Node *llv_find_struct_decl(const char *name);   /* 3110 */
int llv_struct_field_index(Node *decl, const char *fname);   /* 3120 */
void llv_sum_enum_set_body(Node *ed, LLVMTypeRef st);   /* 3136 */
LLVMValueRef llv_sum_payload_ptr(LLVMTypeRef ety, LLVMValueRef ea, LLVMTypeRef pty);   /* 3151 */
LLVMValueRef llv_sum_ctor_fn(Node *ed, int vidx);   /* 3159 */
LLVMValueRef llv_sum_variant_const(Node *ed, int vidx);   /* 3184 */
LLVMValueRef llv_argv_argc_global(void);   /* 3200 */
LLVMValueRef llv_argv_argv_global(void);   /* 3207 */
LLVMValueRef llv_build_baga_arg_count(void);   /* 3217 */
LLVMValueRef llv_build_baga_arg(void);   /* 3234 */
LLVMValueRef llv_build_baga_exit(void);   /* 3260 */
LLVMValueRef llv_build_baga_eprintln(void);   /* 3274 */
LLVMTypeRef llv_baga_map_entry_ptr_ty(void);   /* 3310 */
LLVMTypeRef llv_baga_map_ptr_ty(void);   /* 3326 */
LLVMValueRef llv_ent_fld(LLVMValueRef e, unsigned idx, const char *nm);   /* 3333 */
LLVMValueRef llv_map_load_nb(LLVMValueRef m);   /* 3336 */
LLVMValueRef llv_map_load_b(LLVMValueRef m);   /* 3339 */
LLVMValueRef llv_build_baga_map_hash_str(void);   /* 3350 */
LLVMValueRef llv_build_baga_map_hash_i64(void);   /* 3386 */
LLVMValueRef llv_build_baga_map_hash_bytes(void);   /* 3406 */
LLVMValueRef llv_build_baga_map_new(void);   /* 3468 */
LLVMValueRef llv_build_baga_map_slot(void);   /* 3490 */
LLVMValueRef llv_build_baga_map_slot_b(void);   /* 3551 */
LLVMValueRef llv_build_baga_map_rehash(void);   /* 3613 */
LLVMValueRef llv_build_baga_map_put(void);   /* 3783 */
LLVMValueRef llv_build_baga_map_put_b(void);   /* 3807 */
LLVMValueRef llv_build_baga_map_len(void);   /* 3828 */
LLVMValueRef llv_build_baga_map_h(void);   /* 3847 */
LLVMValueRef llv_build_baga_h_map(void);   /* 3856 */
int llv_map_key_kind(const char *s, int *len);   /* 3871 */
int llv_map_val_kind(const char *s);   /* 3877 */
LLVMValueRef llv_build_baga_map_set(const char *name, int key, int val);   /* 3938 */
LLVMValueRef llv_build_baga_map_get(const char *name, int key, int val);   /* 3997 */
LLVMValueRef llv_build_baga_map_has(const char *name, int key);   /* 4037 */
LLVMValueRef llv_build_baga_map_del(const char *name, int key);   /* 4055 */
LLVMValueRef llv_build_baga_map_keys(const char *name, int key);   /* 4118 */
LLVMValueRef llv_build_baga_vec_push_box(void);   /* 4187 */
LLVMValueRef llv_build_baga_vec_get_box(void);   /* 4208 */
LLVMValueRef llv_build_baga_vec_set_box(void);   /* 4217 */
LLVMValueRef llv_build_baga_vec_slice_box(void);   /* 4250 */
LLVMValueRef llv_build_baga_vec_concat_box(void);   /* 4264 */
LLVMValueRef llv_lrc_rc_fn(const char *type_name, int is_release);   /* 4953 */
Type *llv_lrc_fld_inst_type(Type *st, Node *fld_type);   /* 4964 */
LLVMValueRef llv_lrc_struct_inst_rc_fn(Type *t, int is_release);   /* 5047 */
LLVMValueRef llv_lrc_rc_fn_ty(Type *t, int is_release);   /* 5083 */
LLVMValueRef llv_lrc_box_rel(const char *type_name);   /* 5244 */
LLVMValueRef llv_lrc_box_rel_fn_ty(Type *et);   /* 5281 */
LLVMValueRef llv_lrc_box_rel_ty(Type *et);   /* 5287 */
LLVMValueRef llv_lrc_nested_vec_rel(Type *vty, Node *tn);   /* 5388 */
LLVMValueRef llv_baga_rt(const char *name);   /* 5418 */
LLVMValueRef llv_emit_expr_llvm(Node *n);   /* 6338 */
LLVMValueRef llv_emit_block_value_llvm(Node *block);   /* 7459 */
void llv_emit_stmt_llvm(Node *n, LLVMBasicBlockRef break_bb, LLVMBasicBlockRef cont_bb);   /* 7525 */
int llv_extern_ret_is_str_llvm(Node *ef);   /* 7796 */
void codegen_llvm(Node *program, const char *output_path, Checker *chk, int rc);   /* 8085 */

#endif /* BAGA_LLVM */

#endif /* BAGA_CODEGEN_LLVM_IMPL_H */
