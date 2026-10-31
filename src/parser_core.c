/* parser_core.c — част от parser.c (виж pr_proto.h). */
#include "baga.h"
#include "parser_impl.h"

#include "baga.h"
#include <stdarg.h>

/* ============================================================
 *  AST allocation
 * ============================================================ */

Node *node_alloc(NodeKind kind, SrcPos pos) {
    Node *n = calloc(1, sizeof(Node));
    if (!n) { fprintf(stderr, "baga: out of memory\n"); exit(1); }
    n->kind = kind;
    n->pos = pos;
    return n;
}

void node_free(Node *n) {
    if (!n) return;
    switch (n->kind) {
        case NODE_STR_LIT:
            free(n->str_val);
            break;
        case NODE_BYTES_LIT:
            free(n->str_val);
            break;
        case NODE_IDENT:
            free(n->name);
            break;
        case NODE_PATH:
            free(n->path_enum);
            free(n->path_variant);
            break;
        case NODE_BINARY:
            node_free(n->left);
            node_free(n->right);
            break;
        case NODE_UNARY:
            node_free(n->operand);
            break;
        case NODE_CALL:
            node_free(n->callee);
            for (int i = 0; i < n->args.len; i++) node_free(n->args.data[i]);
            vec_free(n->args);
            break;
        case NODE_IF:
            node_free(n->cond);
            node_free(n->then_br);
            node_free(n->else_br);
            break;
        case NODE_BLOCK:
            for (int i = 0; i < n->stmts.len; i++) node_free(n->stmts.data[i]);
            vec_free(n->stmts);
            break;
        case NODE_INDEX:
            node_free(n->obj);
            node_free(n->index);
            break;
        case NODE_ELEM_REF:
            node_free(n->elem_obj);
            break;
        case NODE_FIELD:
            node_free(n->field_obj);
            free(n->field_name);
            break;
        case NODE_ASSIGN:
            node_free(n->assign_target);
            node_free(n->assign_val);
            break;
        case NODE_RANGE:
            node_free(n->range_lo);
            node_free(n->range_hi);
            break;
        case NODE_STRUCT_LIT:
            free(n->lit_name);
            for (int i = 0; i < n->n_lit_fields; i++) free(n->lit_fields[i]);
            free(n->lit_fields);
            for (int i = 0; i < n->lit_values.len; i++) node_free(n->lit_values.data[i]);
            vec_free(n->lit_values);
            break;
        case NODE_TRY:
            node_free(n->try_expr);
            break;
        case NODE_CATCH:
            node_free(n->catch_expr);
            free(n->catch_effect);
            node_free(n->catch_handler);
            break;
        case NODE_TO_STR:
            node_free(n->to_str_expr);
            break;
        case NODE_LET:
            free(n->let_name);
            node_free(n->let_type);
            node_free(n->let_init);
            break;
        case NODE_RETURN:
            node_free(n->ret_val);
            break;
        case NODE_WHILE:
            node_free(n->while_cond);
            node_free(n->while_body);
            for (int i = 0; i < n->while_invariants.len; i++) node_free(n->while_invariants.data[i]);
            vec_free(n->while_invariants);
            break;
        case NODE_FOR:
            free(n->for_var);
            node_free(n->for_iter);
            node_free(n->for_body);
            break;
        case NODE_MATCH:
            node_free(n->match_expr);
            for (int i = 0; i < n->match_arms.len; i++) node_free(n->match_arms.data[i]);
            vec_free(n->match_arms);
            break;
        case NODE_MATCH_ARM:
            node_free(n->arm_pattern);
            free(n->arm_binding);
            node_free(n->arm_body);
            break;
        case NODE_EXPR_STMT:
            node_free(n->expr);
            break;
        case NODE_INVARIANT:
            for (int i = 0; i < n->inv_exprs.len; i++) node_free(n->inv_exprs.data[i]);
            vec_free(n->inv_exprs);
            break;
        case NODE_FN:
            free(n->fn_name);
            for (int i = 0; i < n->params.len; i++) node_free(n->params.data[i]);
            vec_free(n->params);
            node_free(n->ret_type);
            node_free(n->fn_body);
            break;
        case NODE_PARAM:
            free(n->param_name);
            node_free(n->param_type);
            break;
        case NODE_STRUCT:
            free(n->struct_name);
            for (int i = 0; i < n->fields.len; i++) node_free(n->fields.data[i]);
            vec_free(n->fields);
            break;
        case NODE_FIELD_DECL:
            free(n->fld_name);
            node_free(n->fld_type);
            break;
        case NODE_SPEC:
            free(n->spec_name);
            for (int i = 0; i < n->spec_inputs.len; i++) node_free(n->spec_inputs.data[i]);
            vec_free(n->spec_inputs);
            node_free(n->spec_output);
            for (int i = 0; i < n->n_guarantees; i++) free(n->spec_guarantees[i]);
            free(n->spec_guarantees);
            for (int i = 0; i < n->spec_ensures.len; i++) node_free(n->spec_ensures.data[i]);
            vec_free(n->spec_ensures);
            for (int i = 0; i < n->spec_requires.len; i++) node_free(n->spec_requires.data[i]);
            vec_free(n->spec_requires);
            node_free(n->spec_decreases);
            break;
        case NODE_ENSURE:
            free(n->ensure_text);
            node_free(n->ensure_expr);
            break;
        case NODE_ENUM:
            free(n->enum_name);
            for (int i = 0; i < n->n_variants; i++) free(n->enum_variants[i]);
            free(n->enum_variants);
            for (int i = 0; i < n->n_variants; i++) node_free(n->enum_payloads[i]);
            free(n->enum_payloads);
            break;
        case NODE_TYPE:
        case NODE_TYPE_EFFECT:
            free(n->type_name);
            for (int i = 0; i < n->n_effects; i++) free(n->effect_names[i]);
            free(n->effect_names);
            break;
        case NODE_TYPE_REF:
        case NODE_TYPE_ARRAY:
            node_free(n->inner_type);
            break;
        case NODE_PROGRAM:
            for (int i = 0; i < n->items.len; i++) node_free(n->items.data[i]);
            vec_free(n->items);
            break;
        default:
            break;
    }
    free(n);
}

/* ============================================================
 *  Parser internals
 * ============================================================ */

Token *cur(Parser *p) {
    if (p->pos < p->len) return &p->tokens[p->pos];
    return &p->tokens[p->len - 1]; /* EOF */
}

TokenKind peek_kind(Parser *p) {
    return cur(p)->kind;
}

Token *advance(Parser *p) {
    Token *t = cur(p);
    if (p->pos < p->len - 1) p->pos++;
    return t;
}

int check(Parser *p, TokenKind k) {
    return peek_kind(p) == k;
}

int match(Parser *p, TokenKind k) {
    if (check(p, k)) { advance(p); return 1; }
    return 0;
}

void parser_error(Parser *p, const char *fmt, ...) {
    if (p->n_errors >= BAGA_MAX_ERRORS) return;
    Token *t = cur(p);
    int k = p->n_errors++;
    char *e = p->errors[k];
    p->error_pos[k].line = t->pos.line;
    p->error_pos[k].col = t->pos.col;
    p->error_pos[k].file = p->filename;
    int off = snprintf(e, 256, "%s:%d:%d: ", p->filename, t->pos.line, t->pos.col);
    p->error_off[k] = off > 255 ? 0 : off;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(e + off, 256 - (size_t)off, fmt, ap);
    va_end(ap);
}

/* W3.4: грешка с позиция на ВЪЗЕЛ (не на текущия token) — за диагностики,
 * които гледат цяло поддърво (като world блок) */
void parser_error_at(Parser *p, SrcPos pos, const char *fmt, ...) {
    if (p->n_errors >= BAGA_MAX_ERRORS) return;
    int k = p->n_errors++;
    char *e = p->errors[k];
    p->error_pos[k].line = pos.line;
    p->error_pos[k].col = pos.col;
    p->error_pos[k].file = p->filename;
    int off = snprintf(e, 256, "%s:%d:%d: ", p->filename, pos.line, pos.col);
    p->error_off[k] = off > 255 ? 0 : off;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(e + off, 256 - (size_t)off, fmt, ap);
    va_end(ap);
}

Token *expect(Parser *p, TokenKind k) {
    if (check(p, k)) return advance(p);
    parser_error(p, "очаквах '%s', получих '%s'",
                 token_kind_str(k), token_kind_str(peek_kind(p)));
    return cur(p);
}

char *expect_ident(Parser *p) {
    Token *t = expect(p, TOK_IDENT);
    return t->text ? strdup(t->text) : strdup("<грешка>");
}

/* ============================================================
 *  Type parsing
 * ============================================================ */

static Node *parse_type_rec(Parser *p);

/* Wrapper с дълбочина: изчиства gt_pending след най-външния тип, за да
 * не изтече в следващ type контекст (LP4-final, `>>` цепене). */
Node *parse_type(Parser *p) {
    p->type_depth++;
    Node *r = parse_type_rec(p);
    p->type_depth--;
    if (p->type_depth == 0) p->gt_pending = 0;
    return r;
}

/* Затваряща `>` на генеричен тип. C++11 стил: `>>` затваря ДВА вложени
 * типа — първият консумира токена и оставя втория `>` като pending.
 * M23: `>>>` (токен за логическо отместване) затваря ТРИ — gt_pending е
 * брояч на оставащите `>`-та. */
void expect_gt_split(Parser *p) {
    if (p->gt_pending > 0) { p->gt_pending--; return; }
    if (cur(p)->kind == TOK_URSHIFT) { advance(p); p->gt_pending = 2; return; }
    if (cur(p)->kind == TOK_RSHIFT) { advance(p); p->gt_pending = 1; return; }
    expect(p, TOK_GT);
}

/* Parse effect list: !IO !NotFound !E(str) — M20: опционален payload тип */
Node *parse_type_with_effects(Parser *p, Node *base) {
    if (!check(p, TOK_BANG)) return base;

    Node *eff = node_alloc(NODE_TYPE_EFFECT, base->pos);
    eff->type_name = NULL;
    eff->inner_type = base;
    eff->effect_names = NULL;
    eff->effect_payloads = NULL;
    eff->n_effects = 0;

    VEC(char *) names = {0};
    VEC(Node *) payloads = {0};
    while (match(p, TOK_BANG)) {
        Token *t = expect(p, TOK_IDENT);
        if (t->text) vec_push(names, strdup(t->text));
        /* M20: !E(T) — payload тип */
        Node *pl = NULL;
        if (check(p, TOK_LPAREN)) {
            advance(p);
            pl = parse_type(p);
            expect(p, TOK_RPAREN);
        }
        vec_push(payloads, pl);
    }
    eff->effect_names = names.data;
    eff->effect_payloads = payloads.data;
    eff->n_effects = names.len;
    return eff;
}

static Node *parse_type_rec(Parser *p) {
    SrcPos pos = cur(p)->pos;

    /* fn(T, ...) -> R [!E ...] — функционален тип (L5) */
    if (check(p, TOK_FN)) {
        advance(p);
        expect(p, TOK_LPAREN);
        Node *t = node_alloc(NODE_TYPE_FN, pos);
        t->fn_name = NULL;
        t->params.len = 0; t->params.cap = 0; t->params.data = NULL;
        t->ret_type = NULL;
        t->fn_body = NULL;
        t->is_extern = 0;
        t->captures.len = 0; t->captures.cap = 0; t->captures.data = NULL;
        if (!check(p, TOK_RPAREN)) {
            do {
                Node *pt = parse_type(p);
                Node *prm = node_alloc(NODE_PARAM, pt->pos);
                prm->param_name = NULL;
                prm->param_type = pt;
                vec_push(t->params, prm);
            } while (match(p, TOK_COMMA));
        }
        expect(p, TOK_RPAREN);
        if (match(p, TOK_ARROW)) {
            t->ret_type = parse_type(p);
        }
        /* ефекти директно върху fn типа: fn(i64) -> i64 !IO — същият
         * NODE_TYPE_EFFECT механизъм като при декларациите (върху ret) */
        if (!t->ret_type) {
            Node *v = node_alloc(NODE_TYPE, pos);
            v->type_name = strdup("void");
            t->ret_type = v;
        }
        t->ret_type = parse_type_with_effects(p, t->ret_type);
        return t;
    }

    /* &T — reference */
    if (match(p, TOK_AMP)) {
        Node *inner = parse_type(p);
        Node *t = node_alloc(NODE_TYPE_REF, pos);
        t->inner_type = inner;
        return t;
    }

    /* [T] — array */
    if (match(p, TOK_LBRACKET)) {
        Node *inner = parse_type(p);
        expect(p, TOK_RBRACKET);
        Node *t = node_alloc(NODE_TYPE_ARRAY, pos);
        t->inner_type = inner;
        return t;
    }

    /* named type */
    Token *t = expect(p, TOK_IDENT);
    Node *ty = node_alloc(NODE_TYPE, pos);
    ty->type_name = t->text ? strdup(t->text) : strdup("i32");

    /* L6: модул.Type — уточнен тип (точка в типова позиция е само това) */
    if (check(p, TOK_DOT)) {
        advance(p);
        Token *mt = expect(p, TOK_IDENT);
        size_t need = strlen(ty->type_name) + 1 + strlen(mt->text ? mt->text : "") + 1;
        char *qn = malloc(need);
        if (!qn) { fprintf(stderr, "baga: out of memory\n"); exit(1); }
        snprintf(qn, need, "%s.%s", ty->type_name, mt->text ? mt->text : "");
        free(ty->type_name);
        ty->type_name = qn;
    }

    /* Vec<T> — вектор с анотиран елементен тип */
    if (strcmp(ty->type_name, "Vec") == 0 && match(p, TOK_LT)) {
        ty->inner_type = parse_type(p);
        expect_gt_split(p);
    }
    /* Map<K, V> — хеш-карта с анотирани типове за ключ и стойност */
    if (strcmp(ty->type_name, "Map") == 0 && match(p, TOK_LT)) {
        ty->inner_type = parse_type(p);
        expect(p, TOK_COMMA);
        ty->inner_type2 = parse_type(p);
        expect_gt_split(p);
    }
    /* M24: generic struct — Pair<i64, str> */
    if (match(p, TOK_LT)) {
        do {
            vec_push(ty->gen_type_args, parse_type(p));
        } while (match(p, TOK_COMMA));
        expect_gt_split(p);
    }
    return ty;
}

/* ============================================================
 *  Expression parsing (precedence climbing)
 * ============================================================ */

Node *parse_expr(Parser *p);
Node *parse_block(Parser *p);
Node *parse_stmt(Parser *p);
static Node *parse_unary(Parser *p);

/* ---- string interpolation (G1): "a{x}b" → concat("a", concat(«x», "b")) ---- */

static Node *build_concat_chain(Node **parts, int nparts, SrcPos pos) {
    if (nparts == 1) return parts[0];
    /* build concat(a, concat(b, ...)) calls (right-associated) */
    Node *acc = parts[nparts - 1];
    for (int i = nparts - 2; i >= 0; i--) {
        Node *call = node_alloc(NODE_CALL, pos);
        call->callee = node_alloc(NODE_IDENT, pos);
        call->callee->name = strdup("concat");
        call->args.len = 0; call->args.cap = 0; call->args.data = NULL;
        vec_push(call->args, parts[i]);
        vec_push(call->args, acc);
        acc = call;
    }
    return acc;
}

/* Parse a string literal's (escape-processed) content; if it contains
 * interpolation, return the desugared concat chain, else a plain NODE_STR_LIT.
 * `${expr}` interpolates; `$$` is a literal `$`. (`{` alone is ordinary text,
 * so C-code strings full of braces are unaffected.) */
static Node *parse_interp_string(Parser *p, const char *s, SrcPos pos) {
    int n = (int)strlen(s);
    int has_interp = 0;
    for (int i = 0; i < n; i++) if (s[i] == '$') { has_interp = 1; break; }
    if (!has_interp) {
        Node *lit = node_alloc(NODE_STR_LIT, pos);
        lit->str_val = strdup(s);
        return lit;
    }
    Node *parts[256]; int nparts = 0;
    char *litbuf = malloc((size_t)n + 1); int litlen = 0;
    int i = 0;
    while (i < n) {
        char c = s[i];
        if (c == '$' && i + 1 < n && s[i + 1] == '$') { litbuf[litlen++] = '$'; i += 2; continue; }
        if (c == '$' && i + 1 < n && s[i + 1] == '{') {
            if (litlen > 0) {
                litbuf[litlen] = '\0';
                Node *lit = node_alloc(NODE_STR_LIT, pos); lit->str_val = strdup(litbuf);
                parts[nparts++] = lit; litlen = 0;
            }
            int j = i + 2, depth = 1;
            while (j < n && depth > 0) {
                if (s[j] == '{') depth++;
                else if (s[j] == '}') { depth--; if (depth == 0) break; }
                j++;
            }
            if (depth != 0) { parser_error(p, "незатворена интерполация '${' в низ"); free(litbuf); return NULL; }
            char *exprstr = malloc((size_t)(j - i));
            memcpy(exprstr, s + i + 2, (size_t)(j - i - 2));
            exprstr[j - i - 2] = '\0';
            /* lex + parse the interpolation expression in a sub-parser */
            Lexer lex; lexer_init(&lex, exprstr, (int)strlen(exprstr), p->filename);
            Token *subtoks = NULL; int nsub = 0, capsub = 0;
            for (;;) {
                Token t = lexer_next(&lex);
                if (t.kind == TOK_ERROR) { parser_error(p, "грешка в интерполация: %s", t.text ? t.text : "?"); free(exprstr); free(litbuf); free(subtoks); return NULL; }
                if (nsub == capsub) { capsub = capsub ? capsub * 2 : 16; subtoks = realloc(subtoks, (size_t)capsub * sizeof(Token)); }
                subtoks[nsub++] = t;
                if (t.kind == TOK_EOF) break;
            }
            Parser sub; sub.tokens = subtoks; sub.len = nsub; sub.pos = 0; sub.filename = p->filename; sub.n_errors = 0; sub.in_catch_handler = 0; sub.gt_pending = 0; sub.type_depth = 0;
            Node *e = parse_expr(&sub);
            int bad = (!e || sub.n_errors > 0 || peek_kind(&sub) != TOK_EOF);
            if (bad) parser_error(p, "невалиден израз в интерполация: %s", exprstr);
            for (int ti = 0; ti < nsub; ti++) free(subtoks[ti].text);
            free(subtoks);
            free(exprstr);
            if (bad) { free(litbuf); return NULL; }
            Node *ts = node_alloc(NODE_TO_STR, pos); ts->to_str_expr = e;
            parts[nparts++] = ts;
            i = j + 1;
            continue;
        }
        litbuf[litlen++] = c; i++;
    }
    if (litlen > 0) {
        litbuf[litlen] = '\0';
        Node *lit = node_alloc(NODE_STR_LIT, pos); lit->str_val = strdup(litbuf);
        parts[nparts++] = lit;
    }
    free(litbuf);
    if (nparts == 0) { Node *lit = node_alloc(NODE_STR_LIT, pos); lit->str_val = strdup(""); return lit; }
    return build_concat_chain(parts, nparts, pos);
}

static Node *parse_primary(Parser *p) {
    SrcPos pos = cur(p)->pos;

    /* W3.10: world блок в изразна позиция (print(world g { … }),
     * f(1 + world g { … }), return world g { … }) — преди това това беше
     * обща parse грешка. Блокът притежава свят и е ИЗЯВЛЕНИЕ; тук даваме
     * точна причина и поглъщаме блока, за да няма каскада. */
    if (check(p, TOK_IDENT) && cur(p)->text && strcmp(cur(p)->text, "world") == 0 &&
        p->pos + 2 < p->len && p->tokens[p->pos + 1].kind == TOK_IDENT &&
        p->tokens[p->pos + 2].kind == TOK_LBRACE) {
        const char *wn = p->tokens[p->pos + 1].text ? p->tokens[p->pos + 1].text : "g";
        parser_error_at(p, pos,
            "world %s { … } притежава свят и е изявление, не изразна стойност — "
            "извади го на отделен ред; за резултат ползвай `world_new()`/"
            "`world_load()` в `let`", wn);
        advance(p);
        free(expect_ident(p));
        Node *sw = parse_block(p);
        node_free(sw);
        Node *bad = node_alloc(NODE_INT_LIT, pos);
        bad->int_val = 0;
        return bad;
    }

    /* integer literal */
    if (check(p, TOK_INT_LIT)) {
        Token *t = advance(p);
        Node *n = node_alloc(NODE_INT_LIT, pos);
        n->int_val = t->int_val;
        return n;
    }

    /* float literal */
    if (check(p, TOK_FLOAT_LIT)) {
        Token *t = advance(p);
        Node *n = node_alloc(NODE_FLOAT_LIT, pos);
        n->float_val = t->float_val;
        return n;
    }

    /* string literal (with interpolation desugaring) */
    if (check(p, TOK_STR_LIT)) {
        Token *t = advance(p);
        return parse_interp_string(p, t->text ? t->text : "", pos);
    }

    /* bytes literal x"deadbeef" */
    if (check(p, TOK_BYTES_LIT)) {
        Token *t = advance(p);
        Node *n = node_alloc(NODE_BYTES_LIT, pos);
        n->str_val = strdup(t->text ? t->text : "");
        return n;
    }

    /* char literal → int */
    if (check(p, TOK_CHAR_LIT)) {
        Token *t = advance(p);
        Node *n = node_alloc(NODE_INT_LIT, pos);
        n->int_val = t->int_val;
        return n;
    }

    /* bool */
    if (check(p, TOK_TRUE)) {
        advance(p);
        Node *n = node_alloc(NODE_BOOL_LIT, pos);
        n->bool_val = 1;
        return n;
    }
    if (check(p, TOK_FALSE)) {
        advance(p);
        Node *n = node_alloc(NODE_BOOL_LIT, pos);
        n->bool_val = 0;
        return n;
    }

    /* identifier (or struct literal: Ident { field: val, ... }) */
    if (check(p, TOK_IDENT)) {
        Token *t = advance(p);
        char *name = strdup(t->text ? t->text : "");

        /* L6: модул.Type { ... } — уточнен struct литерал; lookahead-ът
         * `{ IDENT :` по-долу пази `a.b` field достъпа незасегнат */
        if (check(p, TOK_DOT) &&
            p->pos + 4 < p->len &&
            p->tokens[p->pos + 1].kind == TOK_IDENT &&
            p->tokens[p->pos + 2].kind == TOK_LBRACE &&
            p->tokens[p->pos + 3].kind == TOK_IDENT &&
            p->tokens[p->pos + 4].kind == TOK_COLON) {
            advance(p); /* . */
            Token *mt = advance(p); /* Type */
            size_t need = strlen(name) + 1 + strlen(mt->text ? mt->text : "") + 1;
            char *qn = malloc(need);
            if (!qn) { fprintf(stderr, "baga: out of memory\n"); exit(1); }
            snprintf(qn, need, "%s.%s", name, mt->text ? mt->text : "");
            free(name);
            name = qn;
        }

        /* M24: явни типови аргументи на struct литерал — Pair<i64, str> { … }.
         * Lookahead: консумирай <types> само ако след `>` стои `{` (иначе
         * е сравнение). */
        NodeVec lit_type_args = {0};
        if (check(p, TOK_LT)) {
            int save = p->pos;
            int depth = 0, k = p->pos, ok = 0;
            while (k < p->len) {
                TokenKind tk = p->tokens[k].kind;
                if (tk == TOK_LT) depth++;
                else if (tk == TOK_GT) {
                    depth--;
                    if (depth == 0) {
                        if (k + 1 < p->len && p->tokens[k + 1].kind == TOK_LBRACE)
                            ok = 1;
                        break;
                    }
                } else if (tk == TOK_RSHIFT || tk == TOK_URSHIFT) {
                    /* `>>`/`>>>` в типовите аргументи са два/три затварящи
                     * `>` (Box<Vec<str>> { … }) — лексерът ги дава като един
                     * токен; expect_gt_split ги дели при консумацията */
                    depth -= (tk == TOK_URSHIFT) ? 3 : 2;
                    if (depth == 0) {
                        if (k + 1 < p->len && p->tokens[k + 1].kind == TOK_LBRACE)
                            ok = 1;
                        break;
                    }
                    if (depth < 0) break;   /* сравнение/shift, не типови args */
                } else if (tk == TOK_EOF || tk == TOK_LPAREN ||
                           (tk != TOK_IDENT && tk != TOK_COMMA && tk != TOK_GT))
                    break;
                k++;
            }
            if (ok) {
                advance(p);
                /* type_depth докато парсваме аргументите: parse_type
                 * изчиства gt_pending на ниво 0, а затварящият `>` (или
                 * pending половината от `>>`) е на литерала, не на
                 * аргумента — Box<Vec<str>> { … } */
                p->type_depth++;
                do {
                    vec_push(lit_type_args, parse_type(p));
                } while (match(p, TOK_COMMA));
                expect_gt_split(p);
                p->type_depth--;
            } else {
                p->pos = save;
            }
        }

        /* lookahead: { IDENT : → struct literal */
        if (check(p, TOK_LBRACE) &&
            p->pos + 2 < p->len &&
            p->tokens[p->pos + 1].kind == TOK_IDENT &&
            p->tokens[p->pos + 2].kind == TOK_COLON) {

            advance(p); /* consume '{' */
            Node *lit = node_alloc(NODE_STRUCT_LIT, pos);
            lit->lit_name = name;
            lit->lit_fields = NULL;
            lit->n_lit_fields = 0;
            lit->lit_values.len = 0; lit->lit_values.cap = 0; lit->lit_values.data = NULL;
            lit->lit_type_args = lit_type_args;

            VEC(char *) fields = {0};
            while (!check(p, TOK_RBRACE) && !check(p, TOK_EOF)) {
                char *fname = expect_ident(p);
                expect(p, TOK_COLON);
                Node *fval = parse_expr(p);
                vec_push(fields, fname);
                vec_push(lit->lit_values, fval);
                if (!check(p, TOK_RBRACE)) match(p, TOK_COMMA);
            }
            expect(p, TOK_RBRACE);
            lit->lit_fields = fields.data;
            lit->n_lit_fields = fields.len;
            return lit;
        }

        /* A1: Enum::Variant path */
        if (check(p, TOK_COLONCOLON)) {
            advance(p); /* :: */
            Token *vt = expect(p, TOK_IDENT);
            Node *path = node_alloc(NODE_PATH, pos);
            path->path_enum = name;
            path->path_variant = strdup(vt->text ? vt->text : "");
            return path;
        }

        Node *n = node_alloc(NODE_IDENT, pos);
        n->name = name;
        return n;
    }

    /* parenthesized expression */
    if (match(p, TOK_LPAREN)) {
        /* скобите възстановяват пълния изразен контекст — catch вътре
         * в тях се групира вътре (LP4) */
        int saved_catch = p->in_catch_handler;
        p->in_catch_handler = 0;
        Node *e = parse_expr(p);
        p->in_catch_handler = saved_catch;
        expect(p, TOK_RPAREN);
        return e;
    }

    /* if expression */
    if (check(p, TOK_IF)) {
        advance(p);
        Node *n = node_alloc(NODE_IF, pos);
        n->cond = parse_expr(p);
        n->then_br = parse_block(p);
        if (match(p, TOK_ELSE)) {
            if (check(p, TOK_IF)) {
                /* else-if верига с ПРОИЗВОЛНА дълбочина — десugar във вложени
                 * NODE_IF, всяко рамо в свой NODE_BLOCK wrap (форма, която
                 * codegen-ите вече разбират). Преди това второто `else if`
                 * гърмеше: elif->else_br викаше parse_block без проверка за
                 * TOK_IF — `if … else if … else if …` не се парсваше. */
                Node *tail = n;
                int have_else = 1;
                while (have_else && check(p, TOK_IF)) {
                    SrcPos epos = cur(p)->pos;
                    advance(p);
                    Node *elif = node_alloc(NODE_IF, epos);
                    elif->cond = parse_expr(p);
                    elif->then_br = parse_block(p);
                    Node *wrap = node_alloc(NODE_BLOCK, epos);
                    vec_push(wrap->stmts, elif);
                    tail->else_br = wrap;
                    tail = elif;
                    have_else = match(p, TOK_ELSE);
                }
                if (have_else) {
                    tail->else_br = parse_block(p);
                }
            } else {
                n->else_br = parse_block(p);
            }
        }
        return n;
    }

    /* match expression */
    if (check(p, TOK_MATCH)) {
        advance(p);
        Node *n = node_alloc(NODE_MATCH, pos);
        n->match_expr = parse_expr(p);
        expect(p, TOK_LBRACE);
        n->match_arms.len = 0; n->match_arms.cap = 0; n->match_arms.data = NULL;

        while (!check(p, TOK_RBRACE) && !check(p, TOK_EOF)) {
            SrcPos apos = cur(p)->pos;
            Node *arm = node_alloc(NODE_MATCH_ARM, apos);

            /* pattern: _ | Enum::Variant(binding) | Variant(binding)
             *        | Enum::Variant | literal | identifier */
            if (check(p, TOK_UNDERSCORE)) {
                advance(p);
                arm->arm_pattern = NULL;  /* wildcard */
            } else if (check(p, TOK_IDENT) &&
                       p->pos + 1 < p->len &&
                       p->tokens[p->pos + 1].kind == TOK_COLONCOLON &&
                       p->pos + 3 < p->len &&
                       p->tokens[p->pos + 2].kind == TOK_IDENT &&
                       p->tokens[p->pos + 3].kind == TOK_LPAREN) {
                /* Enum::Variant(binding) */
                Token *et = advance(p);
                advance(p); /* :: */
                Token *vt = expect(p, TOK_IDENT);
                advance(p); /* ( */
                Token *bt = expect(p, TOK_IDENT);
                expect(p, TOK_RPAREN);
                Node *pat = node_alloc(NODE_PATH, et->pos);
                pat->path_enum = strdup(et->text ? et->text : "");
                pat->path_variant = strdup(vt->text ? vt->text : "");
                arm->arm_pattern = pat;
                arm->arm_binding = strdup(bt->text ? bt->text : "");
            } else if (check(p, TOK_IDENT) &&
                       p->pos + 1 < p->len &&
                       p->tokens[p->pos + 1].kind == TOK_LPAREN) {
                Token *vt = advance(p);
                advance(p); /* consume '(' */
                Token *bt = expect(p, TOK_IDENT);
                expect(p, TOK_RPAREN);
                Node *pat = node_alloc(NODE_IDENT, vt->pos);
                pat->name = strdup(vt->text ? vt->text : "");
                arm->arm_pattern = pat;
                arm->arm_binding = strdup(bt->text ? bt->text : "");
            } else {
                /* Enum::Variant bare / other expr patterns via unary/postfix */
                arm->arm_pattern = parse_unary(p);
            }

            /* => */
            expect(p, TOK_FAT_ARROW);

            /* body: block or expression */
            if (check(p, TOK_LBRACE)) {
                arm->arm_body = parse_block(p);
            } else {
                SrcPos bpos = cur(p)->pos;
                Node *e = parse_expr(p);
                Node *wrap = node_alloc(NODE_BLOCK, bpos);
                Node *ret = node_alloc(NODE_RETURN, bpos);
                ret->ret_val = e;
                vec_push(wrap->stmts, ret);
                arm->arm_body = wrap;
            }
            match(p, TOK_COMMA);
            vec_push(n->match_arms, arm);
        }
        expect(p, TOK_RBRACE);
        return n;
    }

    /* block expression */
    if (check(p, TOK_LBRACE)) {
        return parse_block(p);
    }

    /* lambda (L5): fn [a, b] (x: i64) -> i64 { body } — captures по стойност */
    if (check(p, TOK_FN)) {
        advance(p);
        Node *n = node_alloc(NODE_LAMBDA, pos);
        n->fn_name = NULL;
        n->params.len = 0; n->params.cap = 0; n->params.data = NULL;
        n->captures.len = 0; n->captures.cap = 0; n->captures.data = NULL;
        n->ret_type = NULL;
        n->is_extern = 0;
        if (match(p, TOK_LBRACKET)) {
            if (!check(p, TOK_RBRACKET)) {
                do {
                    Token *ct = expect(p, TOK_IDENT);
                    Node *cap = node_alloc(NODE_PARAM, ct->pos);
                    cap->param_name = ct->text ? strdup(ct->text) : NULL;
                    cap->param_type = NULL;
                    vec_push(n->captures, cap);
                } while (match(p, TOK_COMMA));
            }
            expect(p, TOK_RBRACKET);
        }
        expect(p, TOK_LPAREN);
        if (!check(p, TOK_RPAREN)) {
            do {
                SrcPos ppos = cur(p)->pos;
                Token *pt = expect(p, TOK_IDENT);
                expect(p, TOK_COLON);
                Node *ptype = parse_type(p);
                Node *prm = node_alloc(NODE_PARAM, ppos);
                prm->param_name = pt->text ? strdup(pt->text) : NULL;
                prm->param_type = ptype;
                vec_push(n->params, prm);
            } while (match(p, TOK_COMMA));
        }
        expect(p, TOK_RPAREN);
        if (match(p, TOK_ARROW)) {
            n->ret_type = parse_type(p);
            n->ret_type = parse_type_with_effects(p, n->ret_type);
        }
        n->fn_body = parse_block(p);
        return n;
    }

    parser_error(p, "очаквах израз, получих '%s'", token_kind_str(peek_kind(p)));
    advance(p); /* skip to avoid infinite loop */
    Node *n = node_alloc(NODE_INT_LIT, pos);
    n->int_val = 0;
    return n;
}

static Node *parse_postfix(Parser *p) {
    Node *e = parse_primary(p);

    for (;;) {
        SrcPos pos = cur(p)->pos;

        /* M21: явни типови аргументи — f<T, U>(...). Lookahead: консумирай
         * <types> само ако след затварящото `>` стои `(` (иначе е сравнение). */
        if (e->kind == NODE_IDENT && check(p, TOK_LT)) {
            int save = p->pos;
            int depth = 0, k = p->pos, ok = 0;
            while (k < p->len) {
                TokenKind tk = p->tokens[k].kind;
                if (tk == TOK_LT) depth++;
                else if (tk == TOK_GT) {
                    depth--;
                    if (depth == 0) {
                        if (k + 1 < p->len && p->tokens[k + 1].kind == TOK_LPAREN)
                            ok = 1;
                        break;
                    }
                } else if (tk == TOK_EOF || tk == TOK_LBRACE ||
                           (tk != TOK_IDENT && tk != TOK_COMMA && tk != TOK_GT))
                    break;
                k++;
            }
            if (ok) {
                advance(p);  /* < */
                Node *call = node_alloc(NODE_CALL, pos);
                call->callee = e;
                call->args.len = 0; call->args.cap = 0; call->args.data = NULL;
                call->type_args.len = 0; call->type_args.cap = 0; call->type_args.data = NULL;
                do {
                    vec_push(call->type_args, parse_type(p));
                } while (match(p, TOK_COMMA));
                expect_gt_split(p);
                expect(p, TOK_LPAREN);
                if (!check(p, TOK_RPAREN)) {
                    vec_push(call->args, parse_expr(p));
                    while (match(p, TOK_COMMA)) {
                        vec_push(call->args, parse_expr(p));
                    }
                }
                expect(p, TOK_RPAREN);
                e = call;
                continue;
            }
            p->pos = save;
        }

        /* function call */
        if (check(p, TOK_LPAREN)) {
            advance(p);
            Node *call = node_alloc(NODE_CALL, pos);
            call->callee = e;
            call->args.len = 0; call->args.cap = 0; call->args.data = NULL;
            if (!check(p, TOK_RPAREN)) {
                vec_push(call->args, parse_expr(p));
                while (match(p, TOK_COMMA)) {
                    vec_push(call->args, parse_expr(p));
                }
            }
            expect(p, TOK_RPAREN);
            e = call;
            continue;
        }

        /* index */
        if (check(p, TOK_LBRACKET)) {
            advance(p);
            /* v[*] — element-wise reference (annotation-only, M3) */
            if (check(p, TOK_STAR)) {
                advance(p);
                expect(p, TOK_RBRACKET);
                Node *er = node_alloc(NODE_ELEM_REF, pos);
                er->elem_obj = e;
                e = er;
                continue;
            }
            Node *idx = node_alloc(NODE_INDEX, pos);
            idx->obj = e;
            idx->index = parse_expr(p);
            expect(p, TOK_RBRACKET);
            e = idx;
            continue;
        }

        /* range: a..b */
        if (check(p, TOK_DOTDOT)) {
            advance(p);
            Node *r = node_alloc(NODE_RANGE, pos);
            r->range_lo = e;
            r->range_hi = parse_unary(p);
            e = r;
            continue;
        }

        /* field access */
        if (check(p, TOK_DOT)) {
            advance(p);
            Token *t = expect(p, TOK_IDENT);
            Node *f = node_alloc(NODE_FIELD, pos);
            f->field_obj = e;
            f->field_name = t->text ? strdup(t->text) : strdup("");
            e = f;
            continue;
        }

        /* ? — effect propagation */
        if (check(p, TOK_QUESTION)) {
            advance(p);
            Node *try = node_alloc(NODE_TRY, pos);
            try->try_expr = e;
            e = try;
            continue;
        }

        /* catch !Effect [(name)] => handler. Веригите се свързват ляво-
         * асоциативно на postfix ниво (docs §13.3); вътре в handler
         * postfix `catch` спира — изрично групиране става с скоби (LP4).
         * M20: `catch !E(binding) => ...` — binding-ът получава payload-а. */
        if (check(p, TOK_CATCH)) {
            if (p->in_catch_handler) break;
            advance(p);
            expect(p, TOK_BANG);
            Token *eff = expect(p, TOK_IDENT);
            char *binding = NULL;
            if (check(p, TOK_LPAREN)) {
                advance(p);
                Token *bn = expect(p, TOK_IDENT);
                binding = bn->text ? strdup(bn->text) : strdup("");
                expect(p, TOK_RPAREN);
            }
            expect(p, TOK_FAT_ARROW);
            p->in_catch_handler = 1;
            Node *handler = parse_unary(p);
            p->in_catch_handler = 0;
            Node *cat = node_alloc(NODE_CATCH, pos);
            cat->catch_expr = e;
            cat->catch_effect = eff->text ? strdup(eff->text) : strdup("");
            cat->catch_binding = binding;
            cat->catch_handler = handler;
            e = cat;
            continue;
        }

        break;
    }
    return e;
}

static Node *parse_unary(Parser *p) {
    SrcPos pos = cur(p)->pos;

    if (check(p, TOK_MINUS)) {
        advance(p);
        Node *n = node_alloc(NODE_UNARY, pos);
        n->un_op = UOP_NEG;
        n->operand = parse_unary(p);
        return n;
    }
    if (check(p, TOK_BANG)) {
        advance(p);
        Node *n = node_alloc(NODE_UNARY, pos);
        n->un_op = UOP_NOT;
        n->operand = parse_unary(p);
        return n;
    }
    if (check(p, TOK_AMP)) {
        advance(p);
        Node *n = node_alloc(NODE_UNARY, pos);
        n->un_op = UOP_REF;
        n->operand = parse_unary(p);
        return n;
    }
    if (check(p, TOK_STAR)) {
        advance(p);
        Node *n = node_alloc(NODE_UNARY, pos);
        n->un_op = UOP_DEREF;
        n->operand = parse_unary(p);
        return n;
    }
    /* M20: raise !E(payload) — предизвиква ефект (дивергира на
     * нормалния път; типът е на обграждащия контекст) */
    if (check(p, TOK_RAISE)) {
        advance(p);
        expect(p, TOK_BANG);
        Token *eff = expect(p, TOK_IDENT);
        Node *n = node_alloc(NODE_RAISE, pos);
        n->raise_effect = eff->text ? strdup(eff->text) : strdup("");
        n->raise_payload = NULL;
        if (check(p, TOK_LPAREN)) {
            advance(p);
            n->raise_payload = parse_expr(p);
            expect(p, TOK_RPAREN);
        }
        return n;
    }

    return parse_postfix(p);
}

static int binop_precedence(TokenKind k) {
    switch (k) {
        case TOK_OR:      return 1;
        case TOK_AND:     return 2;
        case TOK_PIPE:    return 3;    /* |  */
        case TOK_CARET:   return 4;    /* ^  */
        case TOK_AMP:     return 5;    /* &  */
        case TOK_EQ: case TOK_NEQ: return 6;
        case TOK_LT: case TOK_GT: case TOK_LE: case TOK_GE: return 7;
        case TOK_LSHIFT: case TOK_RSHIFT: case TOK_URSHIFT: return 8;
        case TOK_PLUS: case TOK_MINUS: return 9;
        case TOK_STAR: case TOK_SLASH: case TOK_PERCENT: return 10;
        default: return -1;
    }
}

static BinOp token_to_binop(TokenKind k) {
    switch (k) {
        case TOK_PLUS:    return OP_ADD;
        case TOK_MINUS:   return OP_SUB;
        case TOK_STAR:    return OP_MUL;
        case TOK_SLASH:   return OP_DIV;
        case TOK_PERCENT: return OP_MOD;
        case TOK_EQ:      return OP_EQ;
        case TOK_NEQ:     return OP_NEQ;
        case TOK_LT:      return OP_LT;
        case TOK_GT:      return OP_GT;
        case TOK_LE:      return OP_LE;
        case TOK_GE:      return OP_GE;
        case TOK_AND:     return OP_AND;
        case TOK_OR:      return OP_OR;
        case TOK_AMP:     return OP_BIT_AND;
        case TOK_PIPE:    return OP_BIT_OR;
        case TOK_CARET:   return OP_BIT_XOR;
        case TOK_LSHIFT:  return OP_LSHIFT;
        case TOK_RSHIFT:  return OP_RSHIFT;
        case TOK_URSHIFT: return OP_URSHIFT;
        default:          return OP_ADD;
    }
}

static Node *parse_binop_rhs(Parser *p, int min_prec, Node *left) {
    for (;;) {
        TokenKind k = peek_kind(p);
        int prec = binop_precedence(k);
        if (prec < min_prec) break;

        SrcPos pos = cur(p)->pos;
        advance(p);

        Node *right = parse_unary(p);

        /* higher precedence → recurse */
        while (binop_precedence(peek_kind(p)) > prec) {
            right = parse_binop_rhs(p, prec + 1, right);
        }

        Node *bin = node_alloc(NODE_BINARY, pos);
        bin->bin_op = token_to_binop(k);
        bin->left = left;
        bin->right = right;
        left = bin;
    }
    return left;
}

/* Дълбоко копие на дърво от изрази. Използва се при desugar на +=, -=, *=, /=:
 * целта участва два пъти (x = x + val) и двата възела не трябва да делят
 * heap-памет — node_free освобождава всеки от тях. */
static Node *clone_expr(Node *e) {
    if (!e) return NULL;
    Node *c = node_alloc(e->kind, e->pos);
    switch (e->kind) {
        case NODE_INT_LIT:   c->int_val = e->int_val; break;
        case NODE_FLOAT_LIT: c->float_val = e->float_val; break;
        case NODE_BOOL_LIT:  c->bool_val = e->bool_val; break;
        case NODE_STR_LIT:
        case NODE_BYTES_LIT: c->str_val = strdup(e->str_val ? e->str_val : ""); break;
        case NODE_IDENT:     c->name = strdup(e->name ? e->name : ""); break;
        case NODE_PATH:
            c->path_enum = strdup(e->path_enum ? e->path_enum : "");
            c->path_variant = strdup(e->path_variant ? e->path_variant : "");
            break;
        case NODE_BINARY:
            c->bin_op = e->bin_op;
            c->left = clone_expr(e->left);
            c->right = clone_expr(e->right);
            break;
        case NODE_UNARY:
            c->un_op = e->un_op;
            c->operand = clone_expr(e->operand);
            break;
        case NODE_CALL:
            c->callee = clone_expr(e->callee);
            c->args.len = 0; c->args.cap = 0; c->args.data = NULL;
            for (int i = 0; i < e->args.len; i++) vec_push(c->args, clone_expr(e->args.data[i]));
            break;
        case NODE_INDEX:
            c->obj = clone_expr(e->obj);
            c->index = clone_expr(e->index);
            break;
        case NODE_ELEM_REF:
            c->elem_obj = clone_expr(e->elem_obj);
            break;
        case NODE_FIELD:
            c->field_obj = clone_expr(e->field_obj);
            c->field_name = strdup(e->field_name ? e->field_name : "");
            break;
        case NODE_RANGE:
            c->range_lo = clone_expr(e->range_lo);
            c->range_hi = clone_expr(e->range_hi);
            break;
        case NODE_TRY:
            c->try_expr = clone_expr(e->try_expr);
            break;
        case NODE_CATCH:
            c->catch_expr = clone_expr(e->catch_expr);
            c->catch_effect = strdup(e->catch_effect ? e->catch_effect : "");
            c->catch_handler = clone_expr(e->catch_handler);
            break;
        case NODE_TO_STR:
            c->to_str_expr = clone_expr(e->to_str_expr);
            break;
        case NODE_STRUCT_LIT:
            c->lit_name = strdup(e->lit_name ? e->lit_name : "");
            c->n_lit_fields = e->n_lit_fields;
            c->lit_fields = NULL;
            if (e->n_lit_fields > 0) {
                c->lit_fields = malloc(sizeof(char *) * (size_t)e->n_lit_fields);
                for (int i = 0; i < e->n_lit_fields; i++)
                    c->lit_fields[i] = strdup(e->lit_fields[i] ? e->lit_fields[i] : "");
            }
            c->lit_values.len = 0; c->lit_values.cap = 0; c->lit_values.data = NULL;
            for (int i = 0; i < e->lit_values.len; i++) vec_push(c->lit_values, clone_expr(e->lit_values.data[i]));
            break;
        case NODE_IF:
            c->cond = clone_expr(e->cond);
            c->then_br = clone_expr(e->then_br);
            c->else_br = clone_expr(e->else_br);
            break;
        case NODE_MATCH:
            c->match_expr = clone_expr(e->match_expr);
            c->match_arms.len = 0; c->match_arms.cap = 0; c->match_arms.data = NULL;
            for (int i = 0; i < e->match_arms.len; i++) vec_push(c->match_arms, clone_expr(e->match_arms.data[i]));
            break;
        case NODE_MATCH_ARM:
            c->arm_pattern = clone_expr(e->arm_pattern);
            c->arm_body = clone_expr(e->arm_body);
            c->arm_binding = e->arm_binding ? strdup(e->arm_binding) : NULL;
            break;
        case NODE_BLOCK:
            c->stmts.len = 0; c->stmts.cap = 0; c->stmts.data = NULL;
            for (int i = 0; i < e->stmts.len; i++) vec_push(c->stmts, clone_expr(e->stmts.data[i]));
            break;
        case NODE_LET:
            c->let_name = strdup(e->let_name ? e->let_name : "");
            c->is_mut = e->is_mut;
            c->let_type = clone_expr(e->let_type);
            c->let_init = clone_expr(e->let_init);
            break;
        case NODE_RETURN:
            c->ret_val = clone_expr(e->ret_val);
            break;
        case NODE_WHILE:
            c->while_cond = clone_expr(e->while_cond);
            c->while_body = clone_expr(e->while_body);
            c->while_invariants.len = 0; c->while_invariants.cap = 0; c->while_invariants.data = NULL;
            for (int i = 0; i < e->while_invariants.len; i++) vec_push(c->while_invariants, clone_expr(e->while_invariants.data[i]));
            break;
        case NODE_FOR:
            c->for_var = strdup(e->for_var ? e->for_var : "");
            c->for_iter = clone_expr(e->for_iter);
            c->for_body = clone_expr(e->for_body);
            break;
        case NODE_EXPR_STMT:
            c->expr = clone_expr(e->expr);
            break;
        case NODE_INVARIANT:
            c->inv_exprs.len = 0; c->inv_exprs.cap = 0; c->inv_exprs.data = NULL;
            for (int i = 0; i < e->inv_exprs.len; i++) vec_push(c->inv_exprs, clone_expr(e->inv_exprs.data[i]));
            break;
        case NODE_ENSURE:
            c->ensure_text = strdup(e->ensure_text ? e->ensure_text : "");
            c->ensure_expr = clone_expr(e->ensure_expr);
            break;
        case NODE_TYPE:
            c->type_name = strdup(e->type_name ? e->type_name : "");
            break;
        case NODE_TYPE_EFFECT:
            c->type_name = strdup(e->type_name ? e->type_name : "");
            c->n_effects = e->n_effects;
            c->effect_names = NULL;
            if (e->n_effects > 0) {
                c->effect_names = malloc(sizeof(char *) * (size_t)e->n_effects);
                for (int i = 0; i < e->n_effects; i++)
                    c->effect_names[i] = strdup(e->effect_names[i] ? e->effect_names[i] : "");
            }
            break;
        case NODE_TYPE_REF:
        case NODE_TYPE_ARRAY:
            c->inner_type = clone_expr(e->inner_type);
            break;
        default:
            /* NODE_BREAK, NODE_CONTINUE — няма heap данни */
            break;
    }
    return c;
}

Node *parse_expr(Parser *p) {
    Node *e = parse_unary(p);
    e = parse_binop_rhs(p, 1, e);

    /* assignment */
    SrcPos pos = cur(p)->pos;
    if (check(p, TOK_ASSIGN) || check(p, TOK_PLUS_ASSIGN) ||
        check(p, TOK_MINUS_ASSIGN) || check(p, TOK_STAR_ASSIGN) ||
        check(p, TOK_SLASH_ASSIGN)) {
        TokenKind ak = peek_kind(p);
        advance(p);
        Node *val = parse_expr(p);

        if (ak != TOK_ASSIGN) {
            /* += → x = x + val */
            Node *bin = node_alloc(NODE_BINARY, pos);
            switch (ak) {
                case TOK_PLUS_ASSIGN:  bin->bin_op = OP_ADD; break;
                case TOK_MINUS_ASSIGN: bin->bin_op = OP_SUB; break;
                case TOK_STAR_ASSIGN:  bin->bin_op = OP_MUL; break;
                case TOK_SLASH_ASSIGN: bin->bin_op = OP_DIV; break;
                default:               bin->bin_op = OP_ADD; break;
            }
            bin->left = clone_expr(e);
            bin->right = val;
            val = bin;
        }

        Node *assign = node_alloc(NODE_ASSIGN, pos);
        assign->assign_target = e;
        assign->assign_val = val;
        return assign;
    }

    return e;
}

