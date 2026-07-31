#include "baga.h"
#include <stdarg.h>
#include <ctype.h>

/* ============================================================
 *  Error reporting
 * ============================================================ */

void baga_error(const char *filename, SrcPos pos, const char *fmt, ...) {
    fprintf(stderr, "%s:%d:%d: грешка: ", filename, pos.line, pos.col);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fprintf(stderr, "\n");
}

void baga_info(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fprintf(stderr, "\n");
}

/* ============================================================
 *  Token kind names
 * ============================================================ */

static const char *kind_names[TOK_COUNT] = {
    [TOK_EOF]       = "EOF",
    [TOK_ERROR]     = "ГРЕШКА",
    [TOK_IDENT]     = "идентификатор",
    [TOK_INT_LIT]   = "цяло число",
    [TOK_FLOAT_LIT] = "дробно число",
    [TOK_STR_LIT]   = "низ",
    [TOK_CHAR_LIT]  = "символ",
    [TOK_FN]        = "fn",
    [TOK_LET]       = "let",
    [TOK_MUT]       = "mut",
    [TOK_IF]        = "if",
    [TOK_ELSE]      = "else",
    [TOK_WHILE]     = "while",
    [TOK_FOR]       = "for",
    [TOK_IN]        = "in",
    [TOK_RETURN]    = "return",
    [TOK_MATCH]     = "match",
    [TOK_STRUCT]    = "struct",
    [TOK_IMPL]      = "impl",
    [TOK_SPEC]      = "spec",
    [TOK_ENUM]     = "enum",
    [TOK_TRUE]      = "true",
    [TOK_FALSE]     = "false",
    [TOK_CATCH]     = "catch",
    [TOK_BREAK]    = "break",
    [TOK_CONTINUE] = "continue",
    [TOK_LPAREN]    = "(",
    [TOK_RPAREN]    = ")",
    [TOK_LBRACE]    = "{",
    [TOK_RBRACE]    = "}",
    [TOK_LBRACKET]  = "[",
    [TOK_RBRACKET]  = "]",
    [TOK_COMMA]     = ",",
    [TOK_COLON]     = ":",
    [TOK_DOT]       = ".",
    [TOK_SEMICOLON] = ";",
    [TOK_ARROW]     = "->",
    [TOK_BANG]      = "!",
    [TOK_QUESTION]  = "?",
    [TOK_AMP]       = "&",
    [TOK_PIPE]      = "|",
    [TOK_UNDERSCORE]= "_",
    [TOK_DOTDOT]    = "..",
    [TOK_FAT_ARROW] = "=>",
    [TOK_PLUS]      = "+",
    [TOK_MINUS]     = "-",
    [TOK_STAR]      = "*",
    [TOK_SLASH]     = "/",
    [TOK_PERCENT]   = "%",
    [TOK_EQ]        = "==",
    [TOK_NEQ]       = "!=",
    [TOK_LT]        = "<",
    [TOK_GT]        = ">",
    [TOK_LE]        = "<=",
    [TOK_GE]        = ">=",
    [TOK_ASSIGN]    = "=",
    [TOK_PLUS_ASSIGN]  = "+=",
    [TOK_MINUS_ASSIGN] = "-=",
    [TOK_STAR_ASSIGN]  = "*=",
    [TOK_SLASH_ASSIGN] = "/=",
    [TOK_AND]       = "&&",
    [TOK_OR]        = "||",
    [TOK_NOT]       = "!",
    [TOK_LSHIFT]    = "<<",
    [TOK_RSHIFT]    = ">>",
};

const char *token_kind_str(TokenKind k) {
    if (k >= 0 && k < TOK_COUNT && kind_names[k])
        return kind_names[k];
    return "непознат";
}

/* ============================================================
 *  Lexer
 * ============================================================ */

void lexer_init(Lexer *l, const char *src, int len, const char *filename) {
    l->src = src;
    l->len = len;
    l->pos = 0;
    l->line = 1;
    l->col = 1;
    l->filename = filename;
}

static char lex_peek(Lexer *l) {
    if (l->pos >= l->len) return '\0';
    return l->src[l->pos];
}

static char lex_peek2(Lexer *l) {
    if (l->pos + 1 >= l->len) return '\0';
    return l->src[l->pos + 1];
}

static char lex_advance(Lexer *l) {
    char c = l->src[l->pos++];
    if (c == '\n') {
        l->line++;
        l->col = 1;
    } else {
        l->col++;
    }
    return c;
}

static int is_ident_start(unsigned char c) {
    return (c >= 'a' && c <= 'z') ||
           (c >= 'A' && c <= 'Z') ||
           c == '_' ||
           c >= 0x80;  /* UTF-8 multibyte (Cyrillic, etc.) */
}

static int is_ident_char(unsigned char c) {
    return is_ident_start(c) || (c >= '0' && c <= '9');
}

static int is_digit(char c) {
    return c >= '0' && c <= '9';
}

static char *lex_slice(Lexer *l, int start, int end) {
    int n = end - start;
    char *s = malloc((size_t)n + 1);
    if (!s) { fprintf(stderr, "baga: out of memory\n"); exit(1); }
    memcpy(s, l->src + start, (size_t)n);
    s[n] = '\0';
    return s;
}

static Token make_token(Lexer *l, TokenKind kind, SrcPos pos, char *text) {
    (void)l;
    Token t;
    memset(&t, 0, sizeof(t));
    t.kind = kind;
    t.pos = pos;
    t.text = text;
    return t;
}

static Token make_error(Lexer *l, SrcPos pos, const char *msg) {
    char *s = malloc(strlen(msg) + 1);
    strcpy(s, msg);
    return make_token(l, TOK_ERROR, pos, s);
}

/* Skip whitespace and comments */
static void skip_ws(Lexer *l) {
    for (;;) {
        char c = lex_peek(l);

        /* whitespace */
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
            lex_advance(l);
            continue;
        }

        /* line comment */
        if (c == '/' && lex_peek2(l) == '/') {
            while (l->pos < l->len && lex_peek(l) != '\n')
                lex_advance(l);
            continue;
        }

        /* block comment (nested) */
        if (c == '/' && lex_peek2(l) == '*') {
            lex_advance(l); lex_advance(l);
            int depth = 1;
            while (l->pos < l->len && depth > 0) {
                if (lex_peek(l) == '/' && lex_peek2(l) == '*') {
                    lex_advance(l); lex_advance(l);
                    depth++;
                } else if (lex_peek(l) == '*' && lex_peek2(l) == '/') {
                    lex_advance(l); lex_advance(l);
                    depth--;
                } else {
                    lex_advance(l);
                }
            }
            continue;
        }

        break;
    }
}

/* Keyword lookup */
static TokenKind keyword_kind(const char *s) {
    struct { const char *word; TokenKind kind; } kw[] = {
        {"fn",     TOK_FN},
        {"let",    TOK_LET},
        {"mut",    TOK_MUT},
        {"if",     TOK_IF},
        {"else",   TOK_ELSE},
        {"while",  TOK_WHILE},
        {"for",    TOK_FOR},
        {"in",     TOK_IN},
        {"return", TOK_RETURN},
        {"match",  TOK_MATCH},
        {"struct", TOK_STRUCT},
        {"impl",   TOK_IMPL},
        {"spec",   TOK_SPEC},
        {"enum",   TOK_ENUM},
        {"true",   TOK_TRUE},
        {"false",  TOK_FALSE},
        {"catch",  TOK_CATCH},
        {"break",  TOK_BREAK},
        {"continue", TOK_CONTINUE},
    };
    for (int i = 0; i < (int)(sizeof(kw) / sizeof(kw[0])); i++) {
        if (strcmp(s, kw[i].word) == 0)
            return kw[i].kind;
    }
    return TOK_IDENT;
}

/* Read a string literal (opening quote already consumed) */
static Token lex_string(Lexer *l, SrcPos start) {
    /* growable buffer */
    int cap = 64, len = 0;
    char *buf = malloc((size_t)cap);
    if (!buf) { fprintf(stderr, "baga: out of memory\n"); exit(1); }

    for (;;) {
        if (l->pos >= l->len) {
            free(buf);
            return make_error(l, start, "незатворен низ");
        }
        char c = lex_advance(l);
        if (c == '"') break;
        if (c == '\\') {
            if (l->pos >= l->len) { free(buf); return make_error(l, start, "незатворен низ"); }
            char e = lex_advance(l);
            switch (e) {
                case 'n': c = '\n'; break;
                case 't': c = '\t'; break;
                case 'r': c = '\r'; break;
                case '\\': c = '\\'; break;
                case '"': c = '"'; break;
                case '0': c = '\0'; break;
                default:
                    free(buf);
                    return make_error(l, start, "непозната escape последователност");
            }
        }
        if (len + 1 >= cap) {
            cap *= 2;
            buf = realloc(buf, (size_t)cap);
            if (!buf) { fprintf(stderr, "baga: out of memory\n"); exit(1); }
        }
        buf[len++] = c;
    }
    buf[len] = '\0';
    return make_token(l, TOK_STR_LIT, start, buf);
}

/* Read a char literal (opening quote already consumed) */
static Token lex_char(Lexer *l, SrcPos start) {
    if (l->pos >= l->len)
        return make_error(l, start, "незатворен символ");

    char c = lex_advance(l);
    if (c == '\\') {
        if (l->pos >= l->len)
            return make_error(l, start, "незатворен символ");
        char e = lex_advance(l);
        switch (e) {
            case 'n': c = '\n'; break;
            case 't': c = '\t'; break;
            case 'r': c = '\r'; break;
            case '\\': c = '\\'; break;
            case '\'': c = '\''; break;
            case '0': c = '\0'; break;
            default:
                return make_error(l, start, "непозната escape последователност");
        }
    }

    if (l->pos >= l->len || lex_peek(l) != '\'') {
        return make_error(l, start, "незатворен символ");
    }
    lex_advance(l); /* closing quote */

    char *buf = malloc(2);
    buf[0] = c;
    buf[1] = '\0';
    Token t = make_token(l, TOK_CHAR_LIT, start, buf);
    t.int_val = (unsigned char)c;
    return t;
}

/* Read a number */
static Token lex_number(Lexer *l, SrcPos start) {
    int start_pos = l->pos;
    int is_float = 0;

    /* hex / binary / octal */
    if (lex_peek(l) == '0' && l->pos + 1 < l->len) {
        char next = lex_peek2(l);
        if (next == 'x' || next == 'X') {
            lex_advance(l); lex_advance(l);
            while (l->pos < l->len && (isxdigit((unsigned char)lex_peek(l)) || lex_peek(l) == '_'))
                lex_advance(l);
            char *text = lex_slice(l, start_pos, l->pos);
            Token t = make_token(l, TOK_INT_LIT, start, text);
            t.int_val = strtoll(text, NULL, 16);
            return t;
        }
        if (next == 'b' || next == 'B') {
            lex_advance(l); lex_advance(l);
            while (l->pos < l->len && (lex_peek(l) == '0' || lex_peek(l) == '1' || lex_peek(l) == '_'))
                lex_advance(l);
            char *text = lex_slice(l, start_pos, l->pos);
            Token t = make_token(l, TOK_INT_LIT, start, text);
            t.int_val = strtoll(text, NULL, 2);
            return t;
        }
        if (next == 'o' || next == 'O') {
            lex_advance(l); lex_advance(l);
            while (l->pos < l->len && ((lex_peek(l) >= '0' && lex_peek(l) <= '7') || lex_peek(l) == '_'))
                lex_advance(l);
            char *text = lex_slice(l, start_pos, l->pos);
            Token t = make_token(l, TOK_INT_LIT, start, text);
            t.int_val = strtoll(text, NULL, 8);
            return t;
        }
    }

    /* decimal / float */
    while (l->pos < l->len && (is_digit(lex_peek(l)) || lex_peek(l) == '_'))
        lex_advance(l);

    if (l->pos < l->len && lex_peek(l) == '.' && is_digit(lex_peek2(l))) {
        is_float = 1;
        lex_advance(l); /* '.' */
        while (l->pos < l->len && (is_digit(lex_peek(l)) || lex_peek(l) == '_'))
            lex_advance(l);
    }

    /* exponent */
    if (l->pos < l->len && (lex_peek(l) == 'e' || lex_peek(l) == 'E')) {
        is_float = 1;
        lex_advance(l);
        if (l->pos < l->len && (lex_peek(l) == '+' || lex_peek(l) == '-'))
            lex_advance(l);
        while (l->pos < l->len && is_digit(lex_peek(l)))
            lex_advance(l);
    }

    char *text = lex_slice(l, start_pos, l->pos);

    /* strip underscores for parsing */
    char clean[256];
    int ci = 0;
    for (int i = 0; text[i] && ci < 255; i++) {
        if (text[i] != '_') clean[ci++] = text[i];
    }
    clean[ci] = '\0';

    if (is_float) {
        Token t = make_token(l, TOK_FLOAT_LIT, start, text);
        t.float_val = strtod(clean, NULL);
        return t;
    } else {
        Token t = make_token(l, TOK_INT_LIT, start, text);
        t.int_val = strtoll(clean, NULL, 10);
        return t;
    }
}

Token lexer_next(Lexer *l) {
    skip_ws(l);

    SrcPos start = { l->line, l->col };

    if (l->pos >= l->len)
        return make_token(l, TOK_EOF, start, NULL);

    char c = lex_peek(l);

    /* identifiers and keywords */
    if (is_ident_start((unsigned char)c)) {
        int start_pos = l->pos;
        while (l->pos < l->len && is_ident_char((unsigned char)lex_peek(l)))
            lex_advance(l);
        char *text = lex_slice(l, start_pos, l->pos);

        /* standalone underscore is special */
        if (strcmp(text, "_") == 0) {
            return make_token(l, TOK_UNDERSCORE, start, text);
        }

        TokenKind kw = keyword_kind(text);
        return make_token(l, kw, start, text);
    }

    /* numbers */
    if (is_digit(c))
        return lex_number(l, start);

    /* string literal */
    if (c == '"') {
        lex_advance(l);
        return lex_string(l, start);
    }

    /* char literal */
    if (c == '\'') {
        lex_advance(l);
        return lex_char(l, start);
    }

    /* two-char operators (check before single-char) */
    char c2 = lex_peek2(l);

    if (c == '-' && c2 == '>') { lex_advance(l); lex_advance(l); return make_token(l, TOK_ARROW, start, strdup("->")); }
    if (c == '=' && c2 == '=') { lex_advance(l); lex_advance(l); return make_token(l, TOK_EQ, start, strdup("==")); }
    if (c == '!' && c2 == '=') { lex_advance(l); lex_advance(l); return make_token(l, TOK_NEQ, start, strdup("!=")); }
    if (c == '<' && c2 == '=') { lex_advance(l); lex_advance(l); return make_token(l, TOK_LE, start, strdup("<=")); }
    if (c == '>' && c2 == '=') { lex_advance(l); lex_advance(l); return make_token(l, TOK_GE, start, strdup(">=")); }
    if (c == '&' && c2 == '&') { lex_advance(l); lex_advance(l); return make_token(l, TOK_AND, start, strdup("&&")); }
    if (c == '|' && c2 == '|') { lex_advance(l); lex_advance(l); return make_token(l, TOK_OR, start, strdup("||")); }
    if (c == '<' && c2 == '<') { lex_advance(l); lex_advance(l); return make_token(l, TOK_LSHIFT, start, strdup("<<")); }
    if (c == '>' && c2 == '>') { lex_advance(l); lex_advance(l); return make_token(l, TOK_RSHIFT, start, strdup(">>")); }
    if (c == '+' && c2 == '=') { lex_advance(l); lex_advance(l); return make_token(l, TOK_PLUS_ASSIGN, start, strdup("+=")); }
    if (c == '-' && c2 == '=') { lex_advance(l); lex_advance(l); return make_token(l, TOK_MINUS_ASSIGN, start, strdup("-=")); }
    if (c == '*' && c2 == '=') { lex_advance(l); lex_advance(l); return make_token(l, TOK_STAR_ASSIGN, start, strdup("*=")); }
    if (c == '/' && c2 == '=') { lex_advance(l); lex_advance(l); return make_token(l, TOK_SLASH_ASSIGN, start, strdup("/=")); }
    if (c == '.' && c2 == '.') { lex_advance(l); lex_advance(l); return make_token(l, TOK_DOTDOT, start, strdup("..")); }
    if (c == '=' && c2 == '>') { lex_advance(l); lex_advance(l); return make_token(l, TOK_FAT_ARROW, start, strdup("=>")); }

    /* single-char tokens */
    lex_advance(l);
    switch (c) {
        case '(': return make_token(l, TOK_LPAREN, start, strdup("("));
        case ')': return make_token(l, TOK_RPAREN, start, strdup(")"));
        case '{': return make_token(l, TOK_LBRACE, start, strdup("{"));
        case '}': return make_token(l, TOK_RBRACE, start, strdup("}"));
        case '[': return make_token(l, TOK_LBRACKET, start, strdup("["));
        case ']': return make_token(l, TOK_RBRACKET, start, strdup("]"));
        case ',': return make_token(l, TOK_COMMA, start, strdup(","));
        case ':': return make_token(l, TOK_COLON, start, strdup(":"));
        case '.': return make_token(l, TOK_DOT, start, strdup("."));
        case ';': return make_token(l, TOK_SEMICOLON, start, strdup(";"));
        case '!': return make_token(l, TOK_BANG, start, strdup("!"));
        case '?': return make_token(l, TOK_QUESTION, start, strdup("?"));
        case '&': return make_token(l, TOK_AMP, start, strdup("&"));
        case '|': return make_token(l, TOK_PIPE, start, strdup("|"));
        case '+': return make_token(l, TOK_PLUS, start, strdup("+"));
        case '-': return make_token(l, TOK_MINUS, start, strdup("-"));
        case '*': return make_token(l, TOK_STAR, start, strdup("*"));
        case '/': return make_token(l, TOK_SLASH, start, strdup("/"));
        case '%': return make_token(l, TOK_PERCENT, start, strdup("%"));
        case '<': return make_token(l, TOK_LT, start, strdup("<"));
        case '>': return make_token(l, TOK_GT, start, strdup(">"));
        case '=': return make_token(l, TOK_ASSIGN, start, strdup("="));
        default: {
            char msg[64];
            snprintf(msg, sizeof(msg), "непознат символ '%c' (0x%02x)", c, (unsigned char)c);
            return make_error(l, start, msg);
        }
    }
}
