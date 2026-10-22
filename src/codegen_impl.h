/* codegen_impl.h — вътрешен header на codegen_c модулите (codegen_util.c,
 * codegen_move.c, codegen_expr.c, codegen_stmt.c, codegen_api.c).
 * Типове и прототипи между тях. Публичният API (codegen_c) е в baga.h. */
#ifndef BAGA_CODEGEN_IMPL_H
#define BAGA_CODEGEN_IMPL_H

#include "baga.h"
#include <stdint.h>
#include <stdio.h>

/* ============================ типове ============================ */

typedef struct {
    const char *x;    /* името на заемания локал */
    const char *src;  /* базовото име на източника (контейнер/struct) */
    int   bad;
} RcBe;

/* ============================ прототипи ============================ */

void c_unsupported_node(const char *what, Node *n);   /* 12 */
void emit_mangled(FILE *f, const char *name);   /* 24 */
char *mangle_name(const char *name);   /* 36 */
Node *find_extern_fn(Codegen *cg, const char *name);   /* 58 */
int extern_ret_is_str(Node *ef);   /* 70 */
void emit_extern_type(FILE *f, Node *ty, int is_ret);   /* 79 */
void emit_indent(Codegen *cg);   /* 95 */
int rc_type_node_tag(Node *ty);   /* 118 */
int rc_struct_has_heap(Codegen *cg, const char *name);   /* 175 */
const char *rc_nested_struct_field(Codegen *cg, Node *fld_type);   /* 181 */
int rc_enum_has_heap(Codegen *cg, Node *item);   /* 211 */
const char *rc_nested_enum_field(Codegen *cg, Node *fld_type);   /* 217 */
int rc_heap_tag(Codegen *cg, Type *t);   /* 285 */
char *rc_rc_type_name(Type *ty, Node *tnode);   /* 294 */
Type *rc_fld_inst_type(Codegen *cg, Node *s, int k, Node *fld_type);   /* 312 */
int rc_heap_tag_node(Codegen *cg, Node *ty);   /* 349 */
__attribute__((unused)) int rc_expr_copies_ident(Node *n, const char *name);   /* 365 */
void rc_emit_retain_val(Codegen *cg, int tag, Type *ty, Node *tnode, const char *cname);   /* 403 */
int rc_find(Codegen *cg, const char *name);   /* 422 */
int rc_borrowed_init(Node *init);   /* 435 */
int rc_vec_elem_kind(Type *vty, char *sz, size_t szn);   /* 478 */
int rc_map_val_tag(Type *mty, char *sz, size_t szn);   /* 500 */
int rc_vec_elem_kind_node(Node *ty, char *sz, size_t szn);   /* 506 */
int rc_map_val_tag_node(Node *ty, char *sz, size_t szn);   /* 529 */
void rc_box_rel(Codegen *cg, const char *type_name, char *rel, size_t reln);   /* 551 */
void rc_box_rel_ty(Codegen *cg, Type *et, char *rel, size_t reln);   /* 569 */
int rc_box_tracked(Codegen *cg, Type *et);   /* 586 */
void rc_nested_vec_rel_type(Codegen *cg, Type *vty, char *rel, size_t reln);   /* 684 */
void rc_nested_vec_rel_node(Codegen *cg, Node *ty, char *rel, size_t reln);   /* 703 */
const char *rc_vec_elem_name(RcLocal *e);   /* 727 */
const char *rc_map_val_name(RcLocal *e);   /* 735 */
int rc_vec_kind_of(RcLocal *e, char *sz, size_t szn);   /* 744 */
int rc_map_tag_of(RcLocal *e, char *sz, size_t szn);   /* 751 */
void rc_vec_nested_rel_of(Codegen *cg, RcLocal *e, char *rel, size_t reln);   /* 759 */
void rc_emit_release(Codegen *cg, RcLocal *e);   /* 766 */
void rc_push_scope(Codegen *cg, int is_loop);   /* 815 */
void rc_pop_scope(Codegen *cg);   /* 822 */
void rc_release_all(Codegen *cg, int skip_idx);   /* 835 */
void rc_release_to_loop(Codegen *cg);   /* 847 */
void rc_register_node(Codegen *cg, const char *name, int tag, Type *ty, Node *type_node, int is_param);   /* 860 */
void rc_register(Codegen *cg, const char *name, int tag, Type *ty, int is_param);   /* 867 */
void rc_lu_stmts(Codegen *cg, NodeVec *stmts, Node *loop, int flags);   /* 1034 */
int rc_is_move(Codegen *cg, Node *ident, int src_idx);   /* 1041 */
void rc_do_move(Codegen *cg, int src_idx);   /* 1060 */
const char *rc_borrow_src_name(Node *init);   /* 1105 */
int rc_is_mem_call(Node *n, const char *name);   /* 1122 */
int rc_be_ok(Codegen *cg, const char *x, const char *src);   /* 1291 */
int rc_field_assign_tag(Codegen *cg, Node *target, int *kind, char *sz, size_t szn, char *rel, size_t reln);   /* 1334 */
void rc_emit_field_release(Codegen *cg, int tag, Node *target, int kind, const char *sz, const char *rel);   /* 1376 */
const char *rc_field_assign_struct(Codegen *cg, Node *target);   /* 1396 */
void rc_emit_struct_field_release(Codegen *cg, const char *stname, Node *target);   /* 1421 */
const char *rc_field_assign_enum(Codegen *cg, Node *target);   /* 1433 */
void rc_emit_enum_field_release(Codegen *cg, const char *enname, Node *target);   /* 1458 */
int rc_tmp_fresh(Codegen *cg, Node *n);   /* 1471 */
int rc_is_enum_ctor(Codegen *cg, Node *n);   /* 1493 */
int rc_is_owned_call(Codegen *cg, Node *n);   /* 1516 */
int rc_tmp_emit_sub(Codegen *cg, Node *n);   /* 1630 */
int rc_tmp_find(Codegen *cg, Node *n);   /* 1642 */
void rc_tmp_consume(Codegen *cg, int i);   /* 1652 */
void rc_tmp_begin(Codegen *cg, Node *root, int root_bound, RcTmpVec *saved, int *saved_on);   /* 1660 */
void rc_tmp_release_all(Codegen *cg);   /* 1699 */
void rc_tmp_end(Codegen *cg, RcTmpVec *saved, int saved_on);   /* 1717 */
int rc_tmp_would_collect(Codegen *cg, Node *n);   /* 1725 */
void emit_rc_stmt_expr_root(Codegen *cg, Node *n, int root_bound);   /* 1747 */
void emit_rc_stmt_expr(Codegen *cg, Node *n);   /* 1797 */
int rc_need_owned_retain(Codegen *cg, Node *val);   /* 1806 */
void rc_emit_match_arm_val(Codegen *cg, Node *rv, int tmp, int is_void);   /* 1842 */
void emit_type(Codegen *cg, Node *ty);   /* 1863 */
char *struct_cname_str(Type *t);   /* 1933 */
void emit_ctype(Codegen *cg, Type *t);   /* 1961 */
void emit_c_string(FILE *f, const char *s);   /* 1991 */
int eff_tag(Codegen *cg, const char *name);   /* 2034 */
int is_print_call(Node *n);   /* 2113 */
void emit_print(Codegen *cg, Node *n);   /* 2122 */
void emit_expr(Codegen *cg, Node *n);   /* 2220 */
void emit_return_val(Codegen *cg, Node *val);   /* 4161 */
void emit_stmt(Codegen *cg, Node *n);   /* 4268 */
int spec_inputs_testable(Node *spec);   /* 4606 */
void emit_requires_predicate(Codegen *cg, Node *spec);   /* 4617 */
const char *lambda_emit_name(Codegen *cg, Node *n, char *buf, size_t nbuf);   /* 4666 */
void emit_fn(Codegen *cg, Node *fn);   /* 4714 */
int is_sum_enum_item(Node *item);   /* 4943 */
void emit_rc_struct_helpers(Codegen *cg, Node *s);   /* 5171 */
void emit_structs_and_sum_enums(Codegen *cg, Node *program);   /* 5313 */
Node *find_struct_decl(Codegen *cg, const char *name);   /* 5472 */
Node *find_sum_enum_decl(Codegen *cg, const char *name);   /* 5483 */
void emit_zero_struct(Codegen *cg, const char *name);   /* 5498 */
void emit_forward_decls(Codegen *cg, Node *program);   /* 5550 */
void codegen_c(Codegen *cg, Node *program, FILE *out);   /* 5748 */

#endif /* BAGA_CODEGEN_IMPL_H */
