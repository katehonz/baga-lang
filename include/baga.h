#ifndef BAGA_H
#define BAGA_H

#define _POSIX_C_SOURCE 200809L

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

/* Semantic version of the language/compiler (keep in sync with VERSION). */
#define BAGA_VERSION "0.9.0"
#define BAGA_VERSION_MAJOR 0
#define BAGA_VERSION_MINOR 9
#define BAGA_VERSION_PATCH 0

/* ============================================================
 *  Util
 * ============================================================ */

#define BAGA_MAX_ERRORS 64

typedef struct {
    int line;
    int col;
    const char *file;   /* origin file (import expansion keeps it; NULL = unknown) */
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
    TOK_AS,
    TOK_EXTERN,
    TOK_RAISE,      /* raise !E(payload) — M20: effect payload нагоре */
    TOK_TRAIT,      /* M23: trait декларация */

    /* punctuation */
    TOK_LPAREN,     /* ( */
    TOK_RPAREN,     /* ) */
    TOK_LBRACE,     /* { */
    TOK_RBRACE,     /* } */
    TOK_LBRACKET,   /* [ */
    TOK_RBRACKET,   /* ] */
    TOK_COMMA,      /* , */
    TOK_COLON,      /* : */
    TOK_COLONCOLON, /* ::  — A1 qualified path Enum::Variant */
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
    NODE_PATH,        /* Enum::Variant — A1 qualified sum variant */
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
    NODE_RAISE,       /* raise !E(payload) — M20: предизвикване на ефект */
    NODE_TO_STR,      /* interpolation: convert inner expr to str (type-directed) */
    NODE_LAMBDA,      /* fn [caps] (params) -> ret { body } — L5 (fn union member) */

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
    NODE_INVARIANT,   /* invariant e1, e2 — annotation statement (verifier-only) */

    /* declarations */
    NODE_FN,
    NODE_PARAM,
    NODE_STRUCT,
    NODE_FIELD_DECL,
    NODE_SPEC,
    NODE_ENUM,
    NODE_ENSURE,      /* ensures елемент: текст + булев израз */
    NODE_TRAIT,       /* M23: trait Name { fn m(...) -> R; ... } */
    NODE_IMPL,        /* M23: impl Trait for Type { fn m(...) -> R { ... } } */

    /* type expressions */
    NODE_TYPE,        /* simple named type: i32, f64, str, bool */
    NODE_TYPE_REF,    /* &T */
    NODE_TYPE_ARRAY,  /* [T] */
    NODE_TYPE_EFFECT, /* T !E1 !E2 */
    NODE_TYPE_FN,     /* fn(T, ...) -> R — L5 (fn union member: params/ret_type) */

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

        /* NODE_PATH — Enum::Variant (A1) */
        struct { char *path_enum; char *path_variant; };

        /* NODE_BINARY */
        struct { BinOp bin_op; Node *left; Node *right; };

        /* NODE_UNARY */
        struct { UnOp un_op; Node *operand; };

        /* NODE_CALL */
        struct { Node *callee; NodeVec args; NodeVec type_args; /* M21: явни типови аргументи f<T>(…) */ };

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
            NodeVec lit_type_args;  /* M24: явни типови аргументи Pair<i64, str> { … } */
            int n_lit_fields;
        };

        /* NODE_TRY */
        struct { Node *try_expr; };

        /* NODE_CATCH */
        struct { Node *catch_expr; char *catch_effect; Node *catch_handler; char *catch_binding; /* M20: payload променлива, NULL = без */ };

        /* NODE_RAISE — M20: raise !E(payload) */
        struct { char *raise_effect; Node *raise_payload; };

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
        struct { Node *arm_pattern; char *arm_binding; Node *arm_body; };

        /* NODE_EXPR_STMT */
        struct { Node *expr; };

        /* NODE_INVARIANT */
        struct { NodeVec inv_exprs; };

        /* NODE_FN, NODE_TYPE_FN (params/ret_type), NODE_LAMBDA (+captures) */
        struct {
            char *fn_name;
            NodeVec params;     /* NODE_PARAM */
            Node *ret_type;     /* NULL → void */
            Node *fn_body;      /* NODE_BLOCK */
            int is_extern;      /* extern fn — no body, links against libc */
            NodeVec captures;   /* NODE_LAMBDA: NODE_PARAM с checked ->type */
            /* M21 generics: типови параметри + инстанции (от checker-а,
             * консумират се от codegen — мономорфизация) */
            char **type_params;
            int n_type_params;
            /* M23: trait bound per type param (NULL = без bound); fn_trait =
             * trait-ът на impl метод (NULL = обикновена fn) */
            char **param_bounds;
            const char *fn_trait;
            Type **inst_types;  /* inst_count × n_type_params конкретни типове */
            int inst_count;
            int inst_cap;
        };

        /* NODE_PARAM */
        struct { char *param_name; Node *param_type; };

        /* NODE_STRUCT */
        struct {
            char *struct_name;
            NodeVec fields; /* NODE_FIELD_DECL */
            /* M24: generic struct — типови параметри + инстанции
             * (struct_inst_targs: struct_inst_count × n_struct_params) */
            char **struct_params;
            int n_struct_params;
            Type **struct_inst_targs;
            int struct_inst_count;
            int struct_inst_cap;
        };

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
            Node **enum_payloads;  /* L3: per-variant payload type node, NULL = plain */
            int n_variants;
        };

        /* NODE_ENSURE */
        struct { char *ensure_text; Node *ensure_expr; };

        /* NODE_TRAIT (M23) */
        struct { char *trait_name; NodeVec trait_methods; /* NODE_FN без тяло */ };

        /* NODE_IMPL (M23) */
        struct { char *impl_trait; Node *impl_type; NodeVec impl_methods; /* NODE_FN с тяло */ };

        /* NODE_TYPE, NODE_TYPE_REF, NODE_TYPE_ARRAY, NODE_TYPE_EFFECT */
        struct {
            char *type_name;          /* for NODE_TYPE */
            Node *inner_type;         /* Vec<T> elem / Map<K,V> key / ref pointee */
            Node *inner_type2;        /* for Map<..,V> */
            NodeVec gen_type_args;    /* M24: типови аргументи на generic struct Pair<i64, str> */
            char **effect_names;      /* for NODE_TYPE_EFFECT */
            Node **effect_payloads;   /* M20: per-effect payload type node, NULL = без payload */
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
    TYPE_MAP,      /* hash map (baga_Map *); key = Type->key, value = Type->elem */
    TYPE_BYTES,    /* binary-safe byte buffer (baga_bytes, by value) */
    TYPE_ENUM,     /* sum enum with payloads — L3; nominal by name */
    TYPE_VAR,      /* M21: типова променлива на generic fn (name = параметъра) */
    TYPE_ERROR,    /* sentinel for error recovery */
} TypeKind;

typedef struct Type Type;

struct Type {
    TypeKind kind;
    /* TYPE_ARRAY / TYPE_VEC element / TYPE_MAP value */
    Type *elem;
    /* TYPE_MAP key type */
    Type *key;
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
    /* M20: payload тип per effect (паралелен на effects[]; NULL = без payload) */
    Type **effect_payloads;
    /* M24: типови аргументи на instantiated generic struct (TYPE_STRUCT) */
    Type **targs;
    int n_targs;
};

/* Type helpers */
Type *type_new(TypeKind kind);
Type *type_fn(Type *ret, Type **params, int nparams);
Type *type_effect_payload(Type *t, const char *effect);
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
    /* LP4-final: `>>` в генерична позиция се цепи на два `>` (C++11);
     * gt_pending носи втория, type_depth пази нивото на parse_type. */
    int     gt_pending;
    int     type_depth;
    /* LP4: while parsing a catch handler, a bare postfix `catch` binds to
     * the outer chain (left-assoc), not to the handler — parens re-enable. */
    int     in_catch_handler;
    /* error reporting */
    char   errors[BAGA_MAX_ERRORS][256];
    int    n_errors;
} Parser;

Node *parse_program(Parser *p, Token *tokens, int ntokens, const char *filename);
/* M22: парсира един израз от текст (guarantee редове за верификатора);
 * NULL при неуспех. */
Node *parse_expr_string(const char *text);

/* ============================================================
 *  Checker
 * ============================================================ */

typedef struct {
    char errors[BAGA_MAX_ERRORS][256];
    int  n_errors;
    int  allow_no_main; /* 1 = library / --check mode: do not require main */
    void *gen_snap;     /* M21: opaque — snapshot на регистрите (checker.c) */
} Checker;

void check_program(Checker *c, Node *program);
/* M21: преди emit на инстанция k на generic fn — re-infer на тялото под
 * substitution (node->type полетата стават конкретни за тази инстанция). */
void checker_recheck_inst(Checker *c, Node *fn, int k);

/* L6: import alias — регистрира се от main.c при `import "p" as a`.
 * Връща NULL при успех, или съществуващия alias при конфликт. */
const char *baga_note_import_alias(const char *canon_path, const char *alias);

/* ============================================================
 *  Codegen (C transpiler)
 * ============================================================ */

/* RC1 (--rc): scope tracking на heap локали за refcount release при изход.
 * Дизайн: docs/memory-rc-bg.md. Само C backend; без --rc е неактивно. */
typedef struct {
    char *name;     /* mangled C име (собственост на записа) */
    int   tag;      /* 1=str, 2=bytes, 3=Vec, 4=Map, 5=struct с heap полета */
    Type *type;     /* inferred тип (borrowed — AST живее до края на codegen) */
    Node *type_node;/* RC1: анотацията `let x: Vec<str>` — резервен източник
                     * на elem тип, когато inferred Type няма elem (vec_new) */
    int   is_param; /* заеман параметър/capture — не се release-ва */
    int   dead;     /* drop()нат binding — scope exit го пропуска */
} RcLocal;
typedef struct { int top; int is_loop; } RcScope;

/* RC4 (temporaries tracking, docs/memory-rc-bg.md §v0.2): запис за един
 * fresh heap temp на текущия statement. Стойността се изчислява веднъж в
 * __rc_tmpN преди statement-а и се release-ва в края му. */
typedef struct {
    Node *site;      /* AST възелът на temp извикването (borrowed) */
    int   tag;       /* 1=str, 2=bytes, 3=Vec, 4=Map */
    Type *type;      /* inferred тип (за elem kind при Vec/Map release) */
    char  name[24];  /* __rc_tmpN */
} RcTmp;
typedef VEC(RcTmp) RcTmpVec;

/* RC2 (move elision, docs/move-semantics-bg.md): last-use запис за едно
 * binding име в текущата функция. Попълва се от pre-pass обход преди
 * emission на всяка fn/ламбда. Имената са borrowed AST указатели. */
typedef struct {
    const char *name;    /* baga име (borrowed от AST) */
    Node *site;          /* Node* на последната текстуална употреба */
    Node *decl_loop;     /* loop, в чието тяло е деклариран (NULL = извън) */
    Node *site_loop;     /* loop, в чието тяло е последната употреба */
    int   site_in_cond;  /* последната употреба е в if/match клон */
    int   n_lets;        /* брой let декларации с името (shadowing → изкл.) */
    int   captured;      /* capture-нат от ламбда във fn → изключва move */
    int   in_match;      /* има употреба в match arm → изключва move */
} RcUse;
typedef VEC(RcUse) RcUseVec;

typedef struct {
    FILE *out;
    int   indent;
    int   tmp_counter;
    Node *program;   /* for enum variant lookup */
    int   test_specs;   /* --test-specs: генерирай тестов драйвър вместо main */
    FILE *lambda_out;   /* L5: env struct-ове + ламбда wrapper-и (преди телата) */
    int   rc;           /* RC1: --rc refcount паметов модел (opt-in) */
    VEC(RcLocal) rc_locals;  /* RC1: стек от track-нати локали */
    VEC(RcScope) rc_scopes;  /* RC1: scope граници (индекс + loop флаг) */
    int   rc_fn_base;   /* RC1: scope индекс на текущата fn/ламбда (return
                         * release-ва само до тук — ламбдите са отделни C
                         * функции и не пипат enclosing локалите) */
    RcUseVec rc_lus;    /* RC2: last-use записи на текущата fn/ламбда */
    int   rc_moves;     /* RC2: брой елиминирани retain-ове (move сайтове) */
    int   rc_cmoves;    /* RC3: брой container move-ове (push/set без retain
                         * при last-use аргумент — docs/move-semantics-bg.md) */
    /* RC4: per-statement temp регистър — активните temp-ове, флаг за
     * заместването в emit_expr и брояч на release-натите temp-ове */
    RcTmpVec rc_tmps;
    int   rc_tmps_on;
    int   rc_tmp_count;
    Node *rc_tmp_decl;  /* temp възелът, чиято декларация се emit-ва в момента
                         * (без самозаместване) */
    /* RC1.3: watermark-ове на mem_mark (rc_locals.len по време на mark
     * statement-а), LIFO стек за текущата fn. При mem_rewind локалите над
     * watermark-а държат върната памет — маркират се dead, за да не
     * release-нат overwrite-нат header (bump reuse след rewind). */
    VEC(int) rc_marks;
    /* RC2.1: borrowed-retain elision — контекст на текущия block statement
     * (за сканиране на опашката на scope-а) + брояч на елиминираните двойки */
    Node *rc_cur_blk;
    int   rc_cur_idx;
    Node *rc_cur_fn;    /* тялото на текущата fn/ламбда (за глобалния
                         * alias scan на източника) */
    int   rc_elided_pairs;
    VEC(char *) eff_tags;  /* M20: effect tag регистър (име → индекс+1) */
    int   eff_depth;       /* M20: >0 = вътре в catch верига (TRY е no-op) */
    Node *eff_cur_ret;     /* M20: ret type node на текущата fn (за ZERO на propagate) */
    const char *eff_binding;   /* M20: catch binding име (baga) в handler */
    const char *eff_binding_c; /* M20: C име на payload temp-а */
    /* M21 generics: мономорфизация */
    Checker *chk;          /* за checker_recheck_inst преди emit на вариант */
    Node *gen_fn;          /* текущата generic fn при emit на вариант */
    int   gen_inst;        /* текущият индекс на инстанцията */
    const char *gen_emit_name; /* синтетичното C-преди-mangle име */
    /* M24: текущата generic struct инстанция при emit на typedef */
    Node *gen_struct;
    int   gen_struct_inst;
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
    /* M22: машинно-проверими guarantee редове (res = -1 за проза) */
    EnsVerifyRes *guar;
    int n_guar;
    int skipped;         /* 1 if the whole function was skipped */
    const char *skip_reason;
    /* facts established during symexec (for --proofs) */
    int partial;         /* direct self-recursion seen */
    int term;            /* spec carries a decreases measure */
    int term_failed;     /* some termination obligation is not PROVEN */
    char **inv_texts;    /* while-loop invariant renderings (owned) */
    int *inv_proven;     /* per-invariant: init+preservation proven (Hoare) */
    int n_inv;
    /* M18: !Overflow effect discharge (the M15 arith obligations are the
     * effect inference; absence of !Overflow is a proof obligation) */
    int ovf_analyzed;    /* 1 if the function was analyzed (not skipped) */
    int ovf_declared;    /* 1 if the return type declares Overflow */
    int ovf_safe;        /* 1 if every kind-4 obligation is proven (or none) */
    int ovf_res;         /* 0=PROVEN, 1=REFUTED, 2=UNKNOWN, 3=SKIPPED */
    char *ovf_witness;   /* counterexample string when REFUTED (owned/NULL) */
} FnVerifyRes;

int verify_fn_collect(Node *prog, Node *fn, FnVerifyRes *out);
void fn_verify_res_free(FnVerifyRes *r);

/* LLVM codegen (optional, requires -DBAGA_LLVM) */
#ifdef BAGA_LLVM
void codegen_llvm(Node *program, const char *output_path, Checker *chk, int rc);
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
