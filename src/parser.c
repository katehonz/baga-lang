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
            node_free(n->arm_body);
            break;
        case NODE_EXPR_STMT:
            node_free(n->expr);
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

static Token *cur(Parser *p) {
    if (p->pos < p->len) return &p->tokens[p->pos];
    return &p->tokens[p->len - 1]; /* EOF */
}

static TokenKind peek_kind(Parser *p) {
    return cur(p)->kind;
}

static Token *advance(Parser *p) {
    Token *t = cur(p);
    if (p->pos < p->len - 1) p->pos++;
    return t;
}

static int check(Parser *p, TokenKind k) {
    return peek_kind(p) == k;
}

static int match(Parser *p, TokenKind k) {
    if (check(p, k)) { advance(p); return 1; }
    return 0;
}

static void parser_error(Parser *p, const char *fmt, ...) {
    if (p->n_errors >= BAGA_MAX_ERRORS) return;
    Token *t = cur(p);
    char *e = p->errors[p->n_errors++];
    int off = snprintf(e, 256, "%s:%d:%d: ", p->filename, t->pos.line, t->pos.col);
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(e + off, 256 - (size_t)off, fmt, ap);
    va_end(ap);
}

static Token *expect(Parser *p, TokenKind k) {
    if (check(p, k)) return advance(p);
    parser_error(p, "очаквах '%s', получих '%s'",
                 token_kind_str(k), token_kind_str(peek_kind(p)));
    return cur(p);
}

static char *expect_ident(Parser *p) {
    Token *t = expect(p, TOK_IDENT);
    return t->text ? strdup(t->text) : strdup("<грешка>");
}

/* ============================================================
 *  Type parsing
 * ============================================================ */

static Node *parse_type(Parser *p);

/* Parse effect list: !IO !NotFound */
static Node *parse_type_with_effects(Parser *p, Node *base) {
    if (!check(p, TOK_BANG)) return base;

    Node *eff = node_alloc(NODE_TYPE_EFFECT, base->pos);
    eff->type_name = NULL;
    eff->inner_type = base;
    eff->effect_names = NULL;
    eff->n_effects = 0;

    VEC(char *) names = {0};
    while (match(p, TOK_BANG)) {
        Token *t = expect(p, TOK_IDENT);
        if (t->text) vec_push(names, strdup(t->text));
    }
    eff->effect_names = names.data;
    eff->n_effects = names.len;
    return eff;
}

static Node *parse_type(Parser *p) {
    SrcPos pos = cur(p)->pos;

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

    /* Vec<T> — вектор с анотиран елементен тип */
    if (strcmp(ty->type_name, "Vec") == 0 && match(p, TOK_LT)) {
        ty->inner_type = parse_type(p);
        expect(p, TOK_GT);
    }
    return ty;
}

/* ============================================================
 *  Expression parsing (precedence climbing)
 * ============================================================ */

static Node *parse_expr(Parser *p);
static Node *parse_block(Parser *p);
static Node *parse_stmt(Parser *p);
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
            Parser sub; sub.tokens = subtoks; sub.len = nsub; sub.pos = 0; sub.filename = p->filename; sub.n_errors = 0;
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

        Node *n = node_alloc(NODE_IDENT, pos);
        n->name = name;
        return n;
    }

    /* parenthesized expression */
    if (match(p, TOK_LPAREN)) {
        Node *e = parse_expr(p);
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
                /* else if → wrap in block */
                SrcPos epos = cur(p)->pos;
                advance(p);
                Node *elif = node_alloc(NODE_IF, epos);
                elif->cond = parse_expr(p);
                elif->then_br = parse_block(p);
                if (match(p, TOK_ELSE)) {
                    elif->else_br = parse_block(p);
                }
                Node *wrap = node_alloc(NODE_BLOCK, epos);
                vec_push(wrap->stmts, elif);
                n->else_br = wrap;
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

            /* pattern: _ | literal | identifier */
            if (check(p, TOK_UNDERSCORE)) {
                advance(p);
                arm->arm_pattern = NULL;  /* wildcard */
            } else {
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

        /* catch !Effect => handler */
        if (check(p, TOK_CATCH)) {
            advance(p);
            expect(p, TOK_BANG);
            Token *eff = expect(p, TOK_IDENT);
            expect(p, TOK_FAT_ARROW);
            Node *handler = parse_unary(p);
            Node *cat = node_alloc(NODE_CATCH, pos);
            cat->catch_expr = e;
            cat->catch_effect = eff->text ? strdup(eff->text) : strdup("");
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
        case TOK_LSHIFT: case TOK_RSHIFT: return 8;
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

static Node *parse_expr(Parser *p) {
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
            /* shallow copy of target for the left side */
            Node *target_copy = node_alloc(e->kind, e->pos);
            *target_copy = *e;
            bin->left = target_copy;
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

/* ============================================================
 *  Statement parsing
 * ============================================================ */

static Node *parse_block(Parser *p) {
    SrcPos pos = cur(p)->pos;
    expect(p, TOK_LBRACE);
    Node *block = node_alloc(NODE_BLOCK, pos);
    block->stmts.len = 0; block->stmts.cap = 0; block->stmts.data = NULL;

    while (!check(p, TOK_RBRACE) && !check(p, TOK_EOF)) {
        Node *s = parse_stmt(p);
        if (s) vec_push(block->stmts, s);
    }
    expect(p, TOK_RBRACE);
    return block;
}

static Node *parse_stmt(Parser *p) {
    SrcPos pos = cur(p)->pos;

    /* let */
    if (check(p, TOK_LET)) {
        advance(p);
        int is_mut = match(p, TOK_MUT);
        char *name = expect_ident(p);
        Node *type = NULL;
        if (match(p, TOK_COLON)) {
            type = parse_type(p);
        }
        expect(p, TOK_ASSIGN);
        Node *init = parse_expr(p);
        /* optional semicolon */
        match(p, TOK_SEMICOLON);

        Node *n = node_alloc(NODE_LET, pos);
        n->let_name = name;
        n->is_mut = is_mut;
        n->let_type = type;
        n->let_init = init;
        return n;
    }

    /* return */
    if (check(p, TOK_RETURN)) {
        advance(p);
        Node *n = node_alloc(NODE_RETURN, pos);
        n->ret_val = NULL;
        if (!check(p, TOK_RBRACE) && !check(p, TOK_SEMICOLON) && !check(p, TOK_EOF)) {
            n->ret_val = parse_expr(p);
        }
        match(p, TOK_SEMICOLON);
        return n;
    }

    /* break */
    if (check(p, TOK_BREAK)) {
        advance(p);
        match(p, TOK_SEMICOLON);
        return node_alloc(NODE_BREAK, pos);
    }

    /* continue */
    if (check(p, TOK_CONTINUE)) {
        advance(p);
        match(p, TOK_SEMICOLON);
        return node_alloc(NODE_CONTINUE, pos);
    }

    /* while (with optional `invariant e1, e2, ...` clause for --verify) */
    if (check(p, TOK_WHILE)) {
        advance(p);
        Node *n = node_alloc(NODE_WHILE, pos);
        n->while_invariants.len = 0; n->while_invariants.cap = 0; n->while_invariants.data = NULL;
        n->while_cond = parse_expr(p);
        if (check(p, TOK_IDENT) && cur(p)->text && strcmp(cur(p)->text, "invariant") == 0) {
            advance(p);
            vec_push(n->while_invariants, parse_expr(p));
            while (match(p, TOK_COMMA))
                vec_push(n->while_invariants, parse_expr(p));
        }
        n->while_body = parse_block(p);
        return n;
    }

    /* for */
    if (check(p, TOK_FOR)) {
        advance(p);
        char *var = expect_ident(p);
        expect(p, TOK_IN);
        Node *iter = parse_expr(p);
        Node *body = parse_block(p);
        Node *n = node_alloc(NODE_FOR, pos);
        n->for_var = var;
        n->for_iter = iter;
        n->for_body = body;
        return n;
    }

    /* if (as statement) */
    if (check(p, TOK_IF)) {
        return parse_expr(p);
    }

    /* expression statement */
    Node *e = parse_expr(p);
    match(p, TOK_SEMICOLON);
    Node *n = node_alloc(NODE_EXPR_STMT, pos);
    n->expr = e;
    return n;
}

/* ============================================================
 *  Top-level declarations
 * ============================================================ */

static Node *parse_fn(Parser *p) {
    SrcPos pos = cur(p)->pos;
    expect(p, TOK_FN);
    char *name = expect_ident(p);

    expect(p, TOK_LPAREN);
    NodeVec params = {0};
    if (!check(p, TOK_RPAREN)) {
        do {
            SrcPos ppos = cur(p)->pos;
            char *pname = expect_ident(p);
            expect(p, TOK_COLON);
            Node *ptype = parse_type(p);
            Node *param = node_alloc(NODE_PARAM, ppos);
            param->param_name = pname;
            param->param_type = ptype;
            vec_push(params, param);
        } while (match(p, TOK_COMMA));
    }
    expect(p, TOK_RPAREN);

    Node *ret_type = NULL;
    if (match(p, TOK_ARROW)) {
        ret_type = parse_type(p);
        ret_type = parse_type_with_effects(p, ret_type);
    }

    /* forward declaration: fn name(...) -> T  (no body) */
    if (!check(p, TOK_LBRACE)) {
        Node *fn = node_alloc(NODE_FN, pos);
        fn->fn_name = name;
        fn->params = params;
        fn->ret_type = ret_type;
        fn->fn_body = NULL;
        return fn;
    }

    Node *body = parse_block(p);

    Node *fn = node_alloc(NODE_FN, pos);
    fn->fn_name = name;
    fn->params = params;
    fn->ret_type = ret_type;
    fn->fn_body = body;
    return fn;
}

static Node *parse_struct(Parser *p) {
    SrcPos pos = cur(p)->pos;
    expect(p, TOK_STRUCT);
    char *name = expect_ident(p);
    expect(p, TOK_LBRACE);

    NodeVec fields = {0};
    while (!check(p, TOK_RBRACE) && !check(p, TOK_EOF)) {
        SrcPos fpos = cur(p)->pos;
        char *fname = expect_ident(p);
        expect(p, TOK_COLON);
        Node *ftype = parse_type(p);
        Node *fld = node_alloc(NODE_FIELD_DECL, fpos);
        fld->fld_name = fname;
        fld->fld_type = ftype;
        vec_push(fields, fld);
        if (!check(p, TOK_RBRACE)) match(p, TOK_COMMA);
    }
    expect(p, TOK_RBRACE);

    Node *s = node_alloc(NODE_STRUCT, pos);
    s->struct_name = name;
    s->fields = fields;
    return s;
}

static Node *parse_spec(Parser *p) {
    SrcPos pos = cur(p)->pos;
    expect(p, TOK_SPEC);

    /* spec name = function name (identifier) */
    char *name = expect_ident(p);

    expect(p, TOK_LBRACE);

    NodeVec inputs = {0};
    Node *output = NULL;
    VEC(char *) guarantees = {0};
    NodeVec ensures = {0};
    NodeVec requires = {0};
    Node *decreases = NULL;

    while (!check(p, TOK_RBRACE) && !check(p, TOK_EOF)) {
        if (check(p, TOK_IDENT) && cur(p)->text && strcmp(cur(p)->text, "input") == 0) {
            advance(p);
            expect(p, TOK_COLON);
            while (check(p, TOK_IDENT) &&
                   strcmp(cur(p)->text, "output") != 0 &&
                   strcmp(cur(p)->text, "guarantees") != 0 &&
                   strcmp(cur(p)->text, "requires") != 0 &&
                   strcmp(cur(p)->text, "ensures") != 0 &&
                   strcmp(cur(p)->text, "decreases") != 0) {
                SrcPos ppos = cur(p)->pos;
                char *pname = expect_ident(p);
                expect(p, TOK_COLON);
                Node *ptype = parse_type(p);
                Node *param = node_alloc(NODE_PARAM, ppos);
                param->param_name = pname;
                param->param_type = ptype;
                vec_push(inputs, param);
                match(p, TOK_COMMA);
            }
        } else if (check(p, TOK_IDENT) && cur(p)->text && strcmp(cur(p)->text, "output") == 0) {
            advance(p);
            expect(p, TOK_COLON);
            output = parse_type(p);
        } else if (check(p, TOK_IDENT) && cur(p)->text && strcmp(cur(p)->text, "guarantees") == 0) {
            advance(p);
            expect(p, TOK_COLON);
            while (match(p, TOK_MINUS)) {
                char buf[256] = {0};
                int bi = 0;
                while (!check(p, TOK_MINUS) && !check(p, TOK_RBRACE) && !check(p, TOK_EOF) &&
                       !(check(p, TOK_IDENT) && cur(p)->text &&
                         (strcmp(cur(p)->text, "requires") == 0 ||
                          strcmp(cur(p)->text, "decreases") == 0 ||
                          strcmp(cur(p)->text, "ensures") == 0))) {
                    Token *t = advance(p);
                    if (t->text && bi < 250) {
                        if (bi > 0) buf[bi++] = ' ';
                        int tl = (int)strlen(t->text);
                        if (bi + tl < 250) { memcpy(buf + bi, t->text, (size_t)tl); bi += tl; }
                    }
                }
                buf[bi] = '\0';
                vec_push(guarantees, strdup(buf));
            }
        } else if (check(p, TOK_IDENT) && cur(p)->text && strcmp(cur(p)->text, "ensures") == 0) {
            advance(p);
            expect(p, TOK_COLON);
            while (!check(p, TOK_RBRACE) && !check(p, TOK_EOF)) {
                int tstart = p->pos;
                SrcPos epos = cur(p)->pos;
                Node *expr = parse_expr(p);
                /* възстанови текста на израза от токените за съобщенията */
                char buf[512] = {0};
                int bi = 0;
                for (int ti = tstart; ti < p->pos; ti++) {
                    Token *t = &p->tokens[ti];
                    if (!t->text) continue;
                    int tl = (int)strlen(t->text);
                    if (bi > 0 && bi < 500) buf[bi++] = ' ';
                    if (bi + tl < 500) { memcpy(buf + bi, t->text, (size_t)tl); bi += tl; }
                }
                buf[bi] = '\0';
                Node *en = node_alloc(NODE_ENSURE, epos);
                en->ensure_text = strdup(buf);
                en->ensure_expr = expr;
                vec_push(ensures, en);
                if (!match(p, TOK_COMMA)) break;
            }
        } else if (check(p, TOK_IDENT) && cur(p)->text && strcmp(cur(p)->text, "requires") == 0) {
            advance(p);
            expect(p, TOK_COLON);
            while (!check(p, TOK_RBRACE) && !check(p, TOK_EOF)) {
                int tstart = p->pos;
                SrcPos epos = cur(p)->pos;
                Node *expr = parse_expr(p);
                /* възстанови текста на израза от токените за съобщенията */
                char buf[512] = {0};
                int bi = 0;
                for (int ti = tstart; ti < p->pos; ti++) {
                    Token *t = &p->tokens[ti];
                    if (!t->text) continue;
                    int tl = (int)strlen(t->text);
                    if (bi > 0 && bi < 500) buf[bi++] = ' ';
                    if (bi + tl < 500) { memcpy(buf + bi, t->text, (size_t)tl); bi += tl; }
                }
                buf[bi] = '\0';
                Node *en = node_alloc(NODE_ENSURE, epos);
                en->ensure_text = strdup(buf);
                en->ensure_expr = expr;
                vec_push(requires, en);
                if (!match(p, TOK_COMMA)) break;
            }
        } else if (check(p, TOK_IDENT) && cur(p)->text && strcmp(cur(p)->text, "decreases") == 0) {
            /* M6: decreases: <expr> — терминационна мярка над input параметрите */
            advance(p);
            expect(p, TOK_COLON);
            Node *d = parse_expr(p);
            if (decreases) {
                parser_error(p, "spec: decreases е зададен повече от веднъж");
                node_free(d);
            } else {
                decreases = d;
            }
        } else {
            advance(p);
        }
    }
    expect(p, TOK_RBRACE);

    Node *s = node_alloc(NODE_SPEC, pos);
    s->spec_name = name;
    s->spec_inputs = inputs;
    s->spec_output = output;
    s->spec_guarantees = guarantees.data;
    s->n_guarantees = guarantees.len;
    s->spec_ensures = ensures;
    s->spec_requires = requires;
    s->spec_decreases = decreases;
    return s;
}

static Node *parse_enum(Parser *p) {
    SrcPos pos = cur(p)->pos;
    expect(p, TOK_ENUM);
    char *name = expect_ident(p);
    expect(p, TOK_LBRACE);

    VEC(char *) variants = {0};
    while (!check(p, TOK_RBRACE) && !check(p, TOK_EOF)) {
        char *vname = expect_ident(p);
        vec_push(variants, vname);
        if (!check(p, TOK_RBRACE)) match(p, TOK_COMMA);
    }
    expect(p, TOK_RBRACE);

    Node *e = node_alloc(NODE_ENUM, pos);
    e->enum_name = name;
    e->enum_variants = variants.data;
    e->n_variants = variants.len;
    return e;
}

Node *parse_program(Parser *p, Token *tokens, int ntokens, const char *filename) {
    p->tokens = tokens;
    p->len = ntokens;
    p->pos = 0;
    p->filename = filename;
    p->n_errors = 0;

    SrcPos pos = { 1, 1 };
    Node *prog = node_alloc(NODE_PROGRAM, pos);
    prog->items.len = 0; prog->items.cap = 0; prog->items.data = NULL;

    while (!check(p, TOK_EOF)) {
        if (check(p, TOK_FN)) {
            vec_push(prog->items, parse_fn(p));
        } else if (check(p, TOK_EXTERN)) {
            advance(p);
            Node *fn = parse_fn(p);
            fn->is_extern = 1;
            if (fn->fn_body)
                parser_error(p, "extern fn '%s' не може да има тяло", fn->fn_name);
            vec_push(prog->items, fn);
        } else if (check(p, TOK_STRUCT)) {
            vec_push(prog->items, parse_struct(p));
        } else if (check(p, TOK_SPEC)) {
            vec_push(prog->items, parse_spec(p));
        } else if (check(p, TOK_ENUM)) {
            vec_push(prog->items, parse_enum(p));
        } else {
            parser_error(p, "очаквах декларация (fn, struct, spec), получих '%s'",
                         token_kind_str(peek_kind(p)));
            advance(p);
        }
    }

    return prog;
}

/* ============================================================
 *  AST printer (debug)
 * ============================================================ */

static void indent_print(int indent) {
    for (int i = 0; i < indent; i++) fprintf(stderr, "  ");
}

static const char *binop_str(BinOp op) {
    switch (op) {
        case OP_ADD: return "+"; case OP_SUB: return "-";
        case OP_MUL: return "*"; case OP_DIV: return "/";
        case OP_MOD: return "%";
        case OP_EQ: return "=="; case OP_NEQ: return "!=";
        case OP_LT: return "<"; case OP_GT: return ">";
        case OP_LE: return "<="; case OP_GE: return ">=";
        case OP_AND: return "&&"; case OP_OR: return "||";
        case OP_BIT_AND: return "&"; case OP_BIT_OR: return "|";
        case OP_BIT_XOR: return "^"; case OP_LSHIFT: return "<<";
        case OP_RSHIFT: return ">>";
        default: return "?";
    }
}

void print_ast(Node *n, int indent) {
    if (!n) return;
    indent_print(indent);

    switch (n->kind) {
        case NODE_PROGRAM:
            fprintf(stderr, "PROGRAM\n");
            for (int i = 0; i < n->items.len; i++)
                print_ast(n->items.data[i], indent + 1);
            break;
        case NODE_FN:
            fprintf(stderr, "FN %s\n", n->fn_name);
            for (int i = 0; i < n->params.len; i++)
                print_ast(n->params.data[i], indent + 1);
            if (n->ret_type) print_ast(n->ret_type, indent + 1);
            print_ast(n->fn_body, indent + 1);
            break;
        case NODE_PARAM:
            fprintf(stderr, "PARAM %s: ", n->param_name);
            if (n->param_type) print_ast(n->param_type, 0);
            else fprintf(stderr, "\n");
            break;
        case NODE_BLOCK:
            fprintf(stderr, "BLOCK\n");
            for (int i = 0; i < n->stmts.len; i++)
                print_ast(n->stmts.data[i], indent + 1);
            break;
        case NODE_LET:
            fprintf(stderr, "LET %s%s\n", n->is_mut ? "mut " : "", n->let_name);
            print_ast(n->let_init, indent + 1);
            break;
        case NODE_RETURN:
            fprintf(stderr, "RETURN\n");
            if (n->ret_val) print_ast(n->ret_val, indent + 1);
            break;
        case NODE_WHILE:
            fprintf(stderr, "WHILE\n");
            print_ast(n->while_cond, indent + 1);
            for (int i = 0; i < n->while_invariants.len; i++) {
                fprintf(stderr, "%*sINVARIANT\n", indent + 1, "");
                print_ast(n->while_invariants.data[i], indent + 2);
            }
            print_ast(n->while_body, indent + 1);
            break;
        case NODE_FOR:
            fprintf(stderr, "FOR %s\n", n->for_var);
            print_ast(n->for_iter, indent + 1);
            print_ast(n->for_body, indent + 1);
            break;
        case NODE_MATCH:
            fprintf(stderr, "MATCH\n");
            print_ast(n->match_expr, indent + 1);
            for (int i = 0; i < n->match_arms.len; i++)
                print_ast(n->match_arms.data[i], indent + 1);
            break;
        case NODE_MATCH_ARM:
            fprintf(stderr, "ARM\n");
            if (n->arm_pattern) print_ast(n->arm_pattern, indent + 1);
            else { indent_print(indent + 1); fprintf(stderr, "_\n"); }
            print_ast(n->arm_body, indent + 1);
            break;
        case NODE_BREAK:
            fprintf(stderr, "BREAK\n");
            break;
        case NODE_CONTINUE:
            fprintf(stderr, "CONTINUE\n");
            break;
        case NODE_IF:
            fprintf(stderr, "IF\n");
            print_ast(n->cond, indent + 1);
            print_ast(n->then_br, indent + 1);
            if (n->else_br) print_ast(n->else_br, indent + 1);
            break;
        case NODE_EXPR_STMT:
            fprintf(stderr, "EXPR_STMT\n");
            print_ast(n->expr, indent + 1);
            break;
        case NODE_CALL:
            fprintf(stderr, "CALL\n");
            print_ast(n->callee, indent + 1);
            for (int i = 0; i < n->args.len; i++)
                print_ast(n->args.data[i], indent + 1);
            break;
        case NODE_BINARY:
            fprintf(stderr, "BINARY %s\n", binop_str(n->bin_op));
            print_ast(n->left, indent + 1);
            print_ast(n->right, indent + 1);
            break;
        case NODE_UNARY:
            fprintf(stderr, "UNARY\n");
            print_ast(n->operand, indent + 1);
            break;
        case NODE_INT_LIT:
            fprintf(stderr, "INT %lld\n", (long long)n->int_val);
            break;
        case NODE_FLOAT_LIT:
            fprintf(stderr, "FLOAT %g\n", n->float_val);
            break;
        case NODE_STR_LIT:
            fprintf(stderr, "STR \"%s\"\n", n->str_val);
            break;
        case NODE_BYTES_LIT:
            fprintf(stderr, "BYTES x\"%s\"\n", n->str_val);
            break;
        case NODE_BOOL_LIT:
            fprintf(stderr, "BOOL %s\n", n->bool_val ? "true" : "false");
            break;
        case NODE_IDENT:
            fprintf(stderr, "IDENT %s\n", n->name);
            break;
        case NODE_ASSIGN:
            fprintf(stderr, "ASSIGN\n");
            print_ast(n->assign_target, indent + 1);
            print_ast(n->assign_val, indent + 1);
            break;
        case NODE_INDEX:
            fprintf(stderr, "INDEX\n");
            print_ast(n->obj, indent + 1);
            print_ast(n->index, indent + 1);
            break;
        case NODE_ELEM_REF:
            fprintf(stderr, "ELEM_REF [*]\n");
            print_ast(n->elem_obj, indent + 1);
            break;
        case NODE_FIELD:
            fprintf(stderr, "FIELD .%s\n", n->field_name);
            print_ast(n->field_obj, indent + 1);
            break;
        case NODE_RANGE:
            fprintf(stderr, "RANGE\n");
            print_ast(n->range_lo, indent + 1);
            print_ast(n->range_hi, indent + 1);
            break;
        case NODE_STRUCT_LIT:
            fprintf(stderr, "STRUCT_LIT %s\n", n->lit_name);
            for (int i = 0; i < n->n_lit_fields; i++) {
                indent_print(indent + 1);
                fprintf(stderr, ".%s =\n", n->lit_fields[i]);
                print_ast(n->lit_values.data[i], indent + 2);
            }
            break;
        case NODE_TRY:
            fprintf(stderr, "TRY (?)\n");
            print_ast(n->try_expr, indent + 1);
            break;
        case NODE_CATCH:
            fprintf(stderr, "CATCH !%s\n", n->catch_effect);
            print_ast(n->catch_expr, indent + 1);
            print_ast(n->catch_handler, indent + 1);
            break;
        case NODE_TO_STR:
            fprintf(stderr, "TO_STR\n");
            print_ast(n->to_str_expr, indent + 1);
            break;
        case NODE_TYPE:
            fprintf(stderr, "TYPE %s\n", n->type_name);
            break;
        case NODE_TYPE_REF:
            fprintf(stderr, "TYPE_REF\n");
            print_ast(n->inner_type, indent + 1);
            break;
        case NODE_TYPE_ARRAY:
            fprintf(stderr, "TYPE_ARRAY\n");
            print_ast(n->inner_type, indent + 1);
            break;
        case NODE_TYPE_EFFECT:
            fprintf(stderr, "TYPE_EFFECT");
            for (int i = 0; i < n->n_effects; i++)
                fprintf(stderr, " !%s", n->effect_names[i]);
            fprintf(stderr, "\n");
            print_ast(n->inner_type, indent + 1);
            break;
        case NODE_STRUCT:
            fprintf(stderr, "STRUCT %s\n", n->struct_name);
            for (int i = 0; i < n->fields.len; i++)
                print_ast(n->fields.data[i], indent + 1);
            break;
        case NODE_FIELD_DECL:
            fprintf(stderr, "FIELD %s\n", n->fld_name);
            print_ast(n->fld_type, indent + 1);
            break;
        case NODE_SPEC:
            fprintf(stderr, "SPEC \"%s\"\n", n->spec_name);
            for (int i = 0; i < n->spec_ensures.len; i++)
                fprintf(stderr, "  ensures: %s\n", n->spec_ensures.data[i]->ensure_text);
            for (int i = 0; i < n->spec_requires.len; i++)
                fprintf(stderr, "  requires: %s\n", n->spec_requires.data[i]->ensure_text);
            break;
        case NODE_ENUM:
            fprintf(stderr, "ENUM %s", n->enum_name);
            for (int i = 0; i < n->n_variants; i++)
                fprintf(stderr, " %s", n->enum_variants[i]);
            fprintf(stderr, "\n");
            break;
        default:
            fprintf(stderr, "NODE(%d)\n", n->kind);
            break;
    }
}
