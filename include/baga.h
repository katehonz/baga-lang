#ifndef BAGA_H
#define BAGA_H

#define _POSIX_C_SOURCE 200809L

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

/* Semantic version of the language/compiler (keep in sync with VERSION). */
#define BAGA_VERSION "0.2.0"
#define BAGA_VERSION_MAJOR 0
#define BAGA_VERSION_MINOR 2
#define BAGA_VERSION_PATCH 0

/* ============================================================
 *  Util
 * ============================================================ */

#define BAGA_MAX_ERRORS 64

typedef struct {
    int line;
    int col;
} SrcPos;

/* Growable array — usage:
 *   VEC(int) v = {0};
 *   vec_push(v, 42);
 *   vec_free(v);
 */
#define VEC(T) struct { T *data; int len; int cap; }

#define vec_push(v, item) do { \
    if ((v).len == (v).cap) { \
        (v).cap = (v).cap ? (v).cap * 2 : 8; \
        (v).data = realloc((v).data, (size_t)(v).cap * sizeof(*(v).data)); \
        if (!(v).data) { fprintf(stderr, "baga: out of memory\n"); exit(1); } \
    } \
    (v).data[(v).len++] = (item); \
} while (0)

#define vec_free(v) do { free((v).data); (v).data = NULL; (v).len = (v).cap = 0; } while (0)

/* ============================================================
 *  Tokens
 * ============================================================ */

typedef enum {
    TOK_EOF = 0,
    TOK_ERROR,

    /* literals */
    TOK_IDENT,
    TOK_INT_LIT,
    TOK_FLOAT_LIT,
    TOK_STR_LIT,
    TOK_CHAR_LIT,
    TOK_BYTES_LIT,   /* x"deadbeef" hex bytes literal */

    /* keywords */
    TOK_FN,
    TOK_LET,
    TOK_MUT,
    TOK_IF,
    TOK_ELSE,
    TOK_WHILE,
    TOK_FOR,
    TOK_IN,
    TOK_RETURN,
    TOK_MATCH,
    TOK_STRUCT,
    TOK_IMPL,
    TOK_SPEC,
    TOK_ENUM,
    TOK_TRUE,
    TOK_FALSE,
    TOK_CATCH,
    TOK_BREAK,
    TOK_CONTINUE,
    TOK_IMPORT,
    TOK_EXTERN,

    /* punctuation */
    TOK_LPAREN,     /* ( */
    TOK_RPAREN,     /* ) */
    TOK_LBRACE,     /* { */
    TOK_RBRACE,     /* } */
    TOK_LBRACKET,   /* [ */
    TOK_RBRACKET,   /* ] */
    TOK_COMMA,      /* , */
    TOK_COLON,      /* : */
    TOK_DOT,        /* . */
    TOK_SEMICOLON,  /* ; */
    TOK_ARROW,      /* -> */
    TOK_BANG,       /* ! */
    TOK_QUESTION,   /* ? */
    TOK_AMP,        /* & */
    TOK_PIPE,       /* | */
    TOK_UNDERSCORE, /* _ */
    TOK_DOTDOT,     /* .. */
    TOK_FAT_ARROW,  /* => */

    /* operators */
    TOK_PLUS,       /* + */
    TOK_MINUS,      /* - */
    TOK_STAR,       /* * */
    TOK_SLASH,      /* / */
    TOK_PERCENT,    /* % */
    TOK_EQ,         /* == */
    TOK_NEQ,        /* != */
    TOK_LT,         /* < */
    TOK_GT,         /* > */
    TOK_LE,         /* <= */
    TOK_GE,         /* >= */
    TOK_ASSIGN,     /* = */
    TOK_PLUS_ASSIGN,  /* += */
    TOK_MINUS_ASSIGN, /* -= */
    TOK_STAR_ASSIGN,  /* *= */
    TOK_SLASH_ASSIGN, /* /= */
    TOK_AND,        /* && */
    TOK_OR,         /* || */
    TOK_NOT,        /* !  (same as BANG, context disambiguates) */
    TOK_LSHIFT,     /* << */
    TOK_RSHIFT,     /* >> */
    TOK_CARET,      /* ^ */

    TOK_COUNT
} TokenKind;

typedef struct {
    TokenKind kind;
    SrcPos    pos;
    char     *text;       /* lexeme (heap-allocated) */
    int64_t   int_val;    /* for TOK_INT_LIT */
    double    float_val;  /* for TOK_FLOAT_LIT */
} Token;

/* ============================================================
 *  AST
 * ============================================================ */

typedef enum {
    /* expressions */
    NODE_INT_LIT,
    NODE_FLOAT_LIT,
    NODE_STR_LIT,
    NODE_BYTES_LIT,  /* x"deadbeef" — str_val holds the raw hex text */
    NODE_BOOL_LIT,
    NODE_IDENT,
    NODE_BINARY,
    NODE_UNARY,
    NODE_CALL,
    NODE_IF,
    NODE_BLOCK,
    NODE_INDEX,
    NODE_ELEM_REF,    /* v[*] — "every element of v" (annotation-only, M3) */
    NODE_FIELD,
    NODE_ASSIGN,
    NODE_RANGE,       /* a..b */
    NODE_STRUCT_LIT,  /* Point { x: 1, y: 2 } */
    NODE_TRY,         /* e? — effect propagation */
    NODE_CATCH,       /* e catch !E => handler */
    NODE_TO_STR,      /* interpolation: convert inner expr to str (type-directed) */

    /* statements */
    NODE_LET,
    NODE_RETURN,
    NODE_WHILE,
    NODE_FOR,
    NODE_MATCH,
    NODE_MATCH_ARM,
    NODE_BREAK,
    NODE_CONTINUE,
    NODE_EXPR_STMT,

    /* declarations */
    NODE_FN,
    NODE_PARAM,
    NODE_STRUCT,
    NODE_FIELD_DECL,
    NODE_SPEC,
    NODE_ENUM,
    NODE_ENSURE,      /* ensures елемент: текст + булев израз */

    /* type expressions */
    NODE_TYPE,        /* simple named type: i32, f64, str, bool */
    NODE_TYPE_REF,    /* &T */
    NODE_TYPE_ARRAY,  /* [T] */
    NODE_TYPE_EFFECT, /* T !E1 !E2 */

    /* top level */
    NODE_PROGRAM,
} NodeKind;

typedef enum {
    OP_ADD, OP_SUB, OP_MUL, OP_DIV, OP_MOD,
    OP_EQ, OP_NEQ, OP_LT, OP_GT, OP_LE, OP_GE,
    OP_AND, OP_OR,
    OP_BIT_AND, OP_BIT_OR, OP_BIT_XOR,
    OP_LSHIFT, OP_RSHIFT,
} BinOp;

typedef enum {
    UOP_NEG,
    UOP_NOT,
    UOP_REF,    /* &x */
    UOP_DEREF,  /* *x */
} UnOp;

typedef struct Node Node;
typedef struct Type Type;

/* Dynamic array of Node pointers */
typedef VEC(Node *) NodeVec;

struct Node {
    NodeKind kind;
    SrcPos   pos;
    Type    *type;      /* inferred type (set by checker) */

    union {
        /* NODE_INT_LIT */
        int64_t int_val;

        /* NODE_FLOAT_LIT */
        double float_val;

        /* NODE_STR_LIT */
        char *str_val;

        /* NODE_BOOL_LIT */
        int bool_val;

        /* NODE_IDENT */
        char *name;

        /* NODE_BINARY */
        struct { BinOp bin_op; Node *left; Node *right; };

        /* NODE_UNARY */
        struct { UnOp un_op; Node *operand; };

        /* NODE_CALL */
        struct { Node *callee; NodeVec args; };

        /* NODE_IF (expression or statement) */
        struct { Node *cond; Node *then_br; Node *else_br; };

        /* NODE_BLOCK */
        struct { NodeVec stmts; };

        /* NODE_INDEX */
        struct { Node *obj; Node *index; };

        /* NODE_ELEM_REF (v[*]) */
        struct { Node *elem_obj; };

        /* NODE_FIELD */
        struct { Node *field_obj; char *field_name; };

        /* NODE_ASSIGN */
        struct { Node *assign_target; Node *assign_val; };

        /* NODE_RANGE */
        struct { Node *range_lo; Node *range_hi; };

        /* NODE_STRUCT_LIT */
        struct {
            char *lit_name;
            char **lit_fields;
            NodeVec lit_values;
            int n_lit_fields;
        };

        /* NODE_TRY */
        struct { Node *try_expr; };

        /* NODE_CATCH */
        struct { Node *catch_expr; char *catch_effect; Node *catch_handler; };

        /* NODE_TO_STR */
        struct { Node *to_str_expr; };

        /* NODE_LET */
        struct { char *let_name; int is_mut; Node *let_type; Node *let_init; };

        /* NODE_RETURN */
        struct { Node *ret_val; };

        /* NODE_WHILE */
        struct { Node *while_cond; Node *while_body; NodeVec while_invariants; };

        /* NODE_FOR */
        struct { char *for_var; Node *for_iter; Node *for_body; };

        /* NODE_MATCH */
        struct { Node *match_expr; NodeVec match_arms; };

        /* NODE_MATCH_ARM */
        struct { Node *arm_pattern; Node *arm_body; };

        /* NODE_EXPR_STMT */
        struct { Node *expr; };

        /* NODE_FN */
        struct {
            char *fn_name;
            NodeVec params;     /* NODE_PARAM */
            Node *ret_type;     /* NULL → void */
            Node *fn_body;      /* NODE_BLOCK */
            int is_extern;      /* extern fn — no body, links against libc */
        };

        /* NODE_PARAM */
        struct { char *param_name; Node *param_type; };

        /* NODE_STRUCT */
        struct { char *struct_name; NodeVec fields; /* NODE_FIELD_DECL */ };

        /* NODE_FIELD_DECL */
        struct { char *fld_name; Node *fld_type; };

        /* NODE_SPEC */
        struct {
            char *spec_name;
            NodeVec spec_inputs;   /* NODE_PARAM */
            Node *spec_output;     /* type node */
            char **spec_guarantees;
            int n_guarantees;
            NodeVec spec_ensures;  /* NODE_ENSURE */
            NodeVec spec_requires; /* NODE_ENSURE — предусловия */
            Node *spec_decreases;  /* M6: терминационна мярка (израз над input, NULL ако липсва) */
        };

        /* NODE_ENUM */
        struct {
            char *enum_name;
            char **enum_variants;
            int n_variants;
        };

        /* NODE_ENSURE */
        struct { char *ensure_text; Node *ensure_expr; };

        /* NODE_TYPE, NODE_TYPE_REF, NODE_TYPE_ARRAY, NODE_TYPE_EFFECT */
        struct {
            char *type_name;          /* for NODE_TYPE */
            Node *inner_type;         /* for REF / ARRAY */
            char **effect_names;      /* for NODE_TYPE_EFFECT */
            int n_effects;
        };

        /* NODE_PROGRAM */
        struct { NodeVec items; };
    };
};

/* ============================================================
 *  Type system (resolved types, post-checker)
 * ============================================================ */

typedef enum {
    TYPE_VOID,
    TYPE_BOOL,
    TYPE_I32,
    TYPE_I64,
    TYPE_F64,
    TYPE_STR,
    TYPE_ARRAY,
    TYPE_REF,
    TYPE_STRUCT,
    TYPE_FN,
    TYPE_VEC,      /* dynamic array (baga_Vec *) */
    TYPE_BYTES,    /* binary-safe byte buffer (baga_bytes, by value) */
    TYPE_ERROR,    /* sentinel for error recovery */
} TypeKind;

typedef struct Type Type;

struct Type {
    TypeKind kind;
    /* TYPE_ARRAY */
    Type *elem;
    /* TYPE_REF */
    Type *pointee;
    /* TYPE_STRUCT / TYPE_FN */
    char *name;
    /* TYPE_FN */
    Type *ret;
    Type **params;
    int nparams;
    /* effects (on any type) */
    char **effects;
    int n_effects;
};

/* Type helpers */
Type *type_new(TypeKind kind);
Type *type_fn(Type *ret, Type **params, int nparams);
const char *type_str(Type *t);
int type_eq(Type *a, Type *b);

/* Effect helpers */
void type_add_effect(Type *t, const char *effect);
int  type_has_effect(Type *t, const char *effect);
void type_remove_effect(Type *t, const char *effect);
void type_merge_effects(Type *dst, Type *src);

/* ============================================================
 *  Lexer
 * ============================================================ */

typedef struct {
    const char *src;
    int         len;
    int         pos;
    int         line;
    int         col;
    const char *filename;
} Lexer;

void  lexer_init(Lexer *l, const char *src, int len, const char *filename);
Token lexer_next(Lexer *l);
const char *token_kind_str(TokenKind k);

/* ============================================================
 *  Parser
 * ============================================================ */

typedef struct {
    Token  *tokens;
    int     len;
    int     pos;
    const char *filename;
    /* error reporting */
    char   errors[BAGA_MAX_ERRORS][256];
    int    n_errors;
} Parser;

Node *parse_program(Parser *p, Token *tokens, int ntokens, const char *filename);

/* ============================================================
 *  Checker
 * ============================================================ */

typedef struct {
    char errors[BAGA_MAX_ERRORS][256];
    int  n_errors;
    int  allow_no_main; /* 1 = library / --check mode: do not require main */
} Checker;

void check_program(Checker *c, Node *program);

/* ============================================================
 *  Codegen (C transpiler)
 * ============================================================ */

typedef struct {
    FILE *out;
    int   indent;
    int   tmp_counter;
    Node *program;   /* for enum variant lookup */
    int   test_specs;   /* --test-specs: генерирай тестов драйвър вместо main */
} Codegen;

void codegen_c(Codegen *cg, Node *program, FILE *out);

/* Proof extraction */
void print_proofs(Node *program);

/* Static spec verification (--verify): proves/refutes requires/ensures for
 * pure, non-recursive, loop-free, linear i64 functions. Returns 0 when no
 * contract is refuted, 1 when at least one is. Sound: never reports PROVEN
 * unless the obligation truly holds; otherwise REFUTED (with a counterexample)
 * or UNKNOWN. */
int verify_program(Node *program);

/* Enable machine-readable JSON output for --verify (verdicts, counterexamples). */
void verify_set_json(int on);

/* Collectible verification results (for proof extraction integration) */
typedef struct {
    int res;             /* 0=PROVEN, 1=REFUTED, 2=UNKNOWN, 3=SKIPPED */
    const char *ens_text;/* ensures clause text (borrowed) */
    const char *skip_reason; /* if SKIPPED (borrowed or NULL) */
    char **wit_names;    /* counterexample variable names (owned, may be NULL) */
    long long *wit_vals; /* counterexample values */
    int wn;              /* number of witness bindings */
} EnsVerifyRes;

typedef struct {
    EnsVerifyRes *ens;   /* per-ensures results */
    int n_ens;
    int skipped;         /* 1 if the whole function was skipped */
    const char *skip_reason;
    /* facts established during symexec (for --proofs) */
    int partial;         /* direct self-recursion seen */
    int term;            /* spec carries a decreases measure */
    int term_failed;     /* some termination obligation is not PROVEN */
    char **inv_texts;    /* while-loop invariant renderings (owned) */
    int *inv_proven;     /* per-invariant: init+preservation proven (Hoare) */
    int n_inv;
} FnVerifyRes;

int verify_fn_collect(Node *prog, Node *fn, FnVerifyRes *out);
void fn_verify_res_free(FnVerifyRes *r);

/* LLVM codegen (optional, requires -DBAGA_LLVM) */
#ifdef BAGA_LLVM
void codegen_llvm(Node *program, const char *output_path);
#endif

/* ============================================================
 *  AST helpers
 * ============================================================ */

Node *node_alloc(NodeKind kind, SrcPos pos);
void  node_free(Node *n);
void  print_ast(Node *n, int indent);

/* ============================================================
 *  Error reporting
 * ============================================================ */

void baga_error(const char *filename, SrcPos pos, const char *fmt, ...);
void baga_info(const char *fmt, ...);

#endif /* BAGA_H */
