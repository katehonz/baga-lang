/* parser_impl.h — вътрешен header на parser модулите (parser_core.c,
 * parser_decl.c). Типове и прототипи между тях. Публичният API
 * (parse_program, parse_expr_string, node_alloc, print_ast) е в baga.h. */
#ifndef BAGA_PARSER_IMPL_H
#define BAGA_PARSER_IMPL_H

#include "baga.h"
#include <stdint.h>

/* ============================ типове ============================ */


/* ============================ прототипи ============================ */

Node *node_alloc(NodeKind kind, SrcPos pos);   /* 8 */
void node_free(Node *n);   /* 16 */
Token *cur(Parser *p);   /* 194 */
TokenKind peek_kind(Parser *p);   /* 199 */
Token *advance(Parser *p);   /* 203 */
int check(Parser *p, TokenKind k);   /* 209 */
int match(Parser *p, TokenKind k);   /* 213 */
void parser_error(Parser *p, const char *fmt, ...);   /* 218 */
void parser_error_at(Parser *p, SrcPos pos, const char *fmt, ...);   /* W3.4 */
Token *expect(Parser *p, TokenKind k);   /* 229 */
char *expect_ident(Parser *p);   /* 236 */
Node *parse_type(Parser *p);   /* 249 */
void expect_gt_split(Parser *p);   /* 261 */
Node *parse_type_with_effects(Parser *p, Node *base);   /* 269 */
Node *parse_expr(Parser *p);   /* 1240 */
Node *parse_block(Parser *p);   /* 1281 */
Node *parse_stmt(Parser *p);   /* 1295 */
Node *parse_expr_string(const char *text);   /* 1738 */
void print_ast(Node *n, int indent);   /* 1830 */

#endif /* BAGA_PARSER_IMPL_H */
