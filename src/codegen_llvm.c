#include "baga.h"

#ifdef BAGA_LLVM

#include <llvm-c/Core.h>
#include <llvm-c/Analysis.h>
#include <llvm-c/Target.h>

/* ============================================================
 *  LLVM IR Codegen — Фаза 3
 *
 *  Генерира LLVM IR директно от AST-то.
 *  Поддръжка: i64/i32/f64/bool/str, binary/unary изрази, let/
 *  присвояване, if/else, while, for (a..b) с break/continue,
 *  match върху i64, enum варианти, предекларирани функции,
 *  contract wrapper (requires/ensures) като в codegen_c,
 *  str/io builtin-и (len, char_at, substr, concat, str_eq, chr,
 *  ord, read_file), Vec (vec_* builtin-и чрез baga_Vec в IR),
 *  потребителски struct-ове (by value, както в C), ефекти
 *  (?, catch — compile-time тагове, pass-through като codegen_c),
 *  sum enum-и (L3: { i64 tag, [N x i64] u } — LLVM няма union, union
 *  областта е i64 масив с размер по ABI на най-тежкия payload),
 *  Map (верижна хеш-таблица — целият runtime е lazy IR, като C preamble-а).
 *  Builtin helpers са lazy IR функции (baga_rt) — огледало на
 *  C preamble-а в codegen_c.c.
 *
 *  Принцип: НИКАКВИ тихи стойности — всеки неподдържан конструкт
 *  е compile-time грешка (llvm_unsupported), не константа 0.
 * ============================================================ */

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
    LLVMValueRef exit_fn;
    LLVMValueRef stderr_global;
    LLVMTypeRef cur_ret_ty;   /* върнатият тип на текущата функция */
    Node *program;            /* за enum варианти и spec-ове */
    int tmp_counter;
} LLVMCodegen;

static LLVMCodegen lg;

/* ---- честен отказ ---- */

static void llvm_unsupported(const char *what) {
    fprintf(stderr, "baga: LLVM backend: неподдържан конструкт '%s'\n", what);
    exit(1);
}

static void llvm_unsupported_node(Node *n) {
    char buf[64];
    snprintf(buf, sizeof buf, "AST възел #%d", n ? (int)n->kind : -1);
    llvm_unsupported(buf);
}

/* ---- Type mapping ---- */

static LLVMTypeRef llvm_type(Node *ty);
static LLVMTypeRef baga_vec_ptr_ty(void);
static LLVMTypeRef baga_bytes_ty(void);
static LLVMTypeRef baga_map_ptr_ty(void);
static LLVMTypeRef user_struct_ty(const char *name);

/* 1 ако възелът е L3 sum enum (поне един вариант с payload) */
static int is_sum_enum_item(Node *item) {
    if (!item || item->kind != NODE_ENUM) return 0;
    for (int j = 0; j < item->n_variants; j++)
        if (item->enum_payloads && item->enum_payloads[j]) return 1;
    return 0;
}

/* sum enum декларация по име; NULL ако няма такава */
static Node *find_sum_enum(const char *name) {
    if (!name || !lg.program) return NULL;
    for (int i = 0; i < lg.program->items.len; i++) {
        Node *item = lg.program->items.data[i];
        if (is_sum_enum_item(item) && item->enum_name &&
            strcmp(item->enum_name, name) == 0)
            return item;
    }
    return NULL;
}

/* индекс на вариант по име в enum декларация; -1 ако го няма */
static int sum_variant_index(Node *ed, const char *vname) {
    if (!ed || !vname) return -1;
    for (int j = 0; j < ed->n_variants; j++)
        if (strcmp(ed->enum_variants[j], vname) == 0) return j;
    return -1;
}

static LLVMTypeRef llvm_type_resolved(Type *ty) {
    if (!ty) return lg.i64_ty;
    switch (ty->kind) {
        case TYPE_I64: return lg.i64_ty;
        case TYPE_I32: return lg.i32_ty;
        case TYPE_F64: return lg.double_ty;
        case TYPE_BOOL: return lg.i1_ty;
        case TYPE_STR: return lg.ptr_ty;
        case TYPE_VOID: return lg.void_ty;
        case TYPE_STRUCT:
            if (!ty->name) llvm_unsupported("анонимна структура");
            return user_struct_ty(ty->name);
        case TYPE_VEC:    return baga_vec_ptr_ty();
        case TYPE_MAP:    return baga_map_ptr_ty(); break;
        case TYPE_ENUM:
            /* L3: sum enum → named tagged struct (като user struct) */
            if (!ty->name) llvm_unsupported("анонимен enum");
            return user_struct_ty(ty->name);
        case TYPE_BYTES:  return baga_bytes_ty();
        case TYPE_FN:     return lg.i64_ty;   /* L5: cell2(code, env) handle */
        case TYPE_ARRAY:  llvm_unsupported("масиви"); break;
        case TYPE_REF:    llvm_unsupported("референции"); break;
        default:          llvm_unsupported("неизвестен тип"); break;
    }
    return NULL; /* unreachable */
}

static LLVMTypeRef llvm_type(Node *ty) {
    if (!ty) return lg.void_ty;
    if (ty->kind == NODE_TYPE) {
        if (strcmp(ty->type_name, "i64") == 0) return lg.i64_ty;
        if (strcmp(ty->type_name, "i32") == 0) return lg.i32_ty;
        if (strcmp(ty->type_name, "f64") == 0) return lg.double_ty;
        if (strcmp(ty->type_name, "bool") == 0) return lg.i1_ty;
        if (strcmp(ty->type_name, "str") == 0) return lg.ptr_ty;
        if (strcmp(ty->type_name, "void") == 0) return lg.void_ty;
        if (strcmp(ty->type_name, "bytes") == 0) return baga_bytes_ty();
        if (strcmp(ty->type_name, "Vec") == 0) return baga_vec_ptr_ty();
        if (strcmp(ty->type_name, "Map") == 0) return baga_map_ptr_ty();
        /* user struct или L3 sum enum — и двата са named struct типове */
        return user_struct_ty(ty->type_name);
    }
    if (ty->kind == NODE_TYPE_EFFECT) return llvm_type(ty->inner_type);
    if (ty->kind == NODE_TYPE_FN) return lg.i64_ty;   /* L5: cell2 handle */
    if (ty->kind == NODE_TYPE_REF) llvm_unsupported("референции (&T)");
    if (ty->kind == NODE_TYPE_ARRAY) return baga_vec_ptr_ty();   /* [T] == Vec<T> */
    llvm_unsupported_node(ty);
    return NULL; /* unreachable */
}

/* ---- Name mangling ---- */

static char *llvm_mangle(const char *name) {
    size_t len = strlen(name);
    char *buf = malloc(2 + len * 4 + 1);
    char *o = buf;
    *o++ = 'b'; *o++ = '_';
    for (const unsigned char *p = (const unsigned char *)name; *p; p++) {
        if ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
            (*p >= '0' && *p <= '9') || *p == '_') {
            *o++ = (char)*p;
        } else {
            o += sprintf(o, "_%d", *p);
        }
    }
    *o = '\0';
    return buf;
}

/* ---- Temp register names ---- */

static char *tmp_name(void) {
    char *buf = malloc(16);
    sprintf(buf, "t%d", lg.tmp_counter++);
    return buf;
}

/* ---- Symbol table: име → alloca ---- */

#define LLVM_MAX_VARS 256
typedef struct { char *name; LLVMValueRef alloca; } LLVMVar;
typedef struct {
    LLVMVar vars[LLVM_MAX_VARS];
    int count;
    int scope_marks[64];
    int depth;
} LLVMSymtab;
static LLVMSymtab lg_st;

static void st_reset(void) { lg_st.count = 0; lg_st.depth = 0; }
static void st_push(void) { lg_st.scope_marks[lg_st.depth++] = lg_st.count; }
static void st_pop(void)  { lg_st.count = lg_st.scope_marks[--lg_st.depth]; }
static void st_define(const char *name, LLVMValueRef alloca) {
    if (lg_st.count >= LLVM_MAX_VARS) llvm_unsupported("твърде много локални променливи");
    lg_st.vars[lg_st.count].name = (char *)name;
    lg_st.vars[lg_st.count].alloca = alloca;
    lg_st.count++;
}
static LLVMValueRef st_lookup(const char *name) {
    for (int i = lg_st.count - 1; i >= 0; i--)
        if (strcmp(lg_st.vars[i].name, name) == 0) return lg_st.vars[i].alloca;
    return NULL;
}

/* ---- Type coercion (огледало на C аритметичните преобразувания) ---- */

static LLVMValueRef coerce(LLVMValueRef v, LLVMTypeRef target) {
    LLVMTypeRef t = LLVMTypeOf(v);
    if (t == target) return v;
    LLVMTypeKind tk = LLVMGetTypeKind(t);
    LLVMTypeKind gk = LLVMGetTypeKind(target);
    char *name = tmp_name();
    LLVMValueRef r = NULL;
    if (gk == LLVMDoubleTypeKind && tk == LLVMIntegerTypeKind) {
        r = LLVMGetIntTypeWidth(t) == 1
          ? LLVMBuildUIToFP(lg.builder, v, target, name)
          : LLVMBuildSIToFP(lg.builder, v, target, name);
    } else if (tk == LLVMDoubleTypeKind && gk == LLVMIntegerTypeKind) {
        r = LLVMBuildFPToSI(lg.builder, v, target, name);
    } else if (tk == LLVMIntegerTypeKind && gk == LLVMIntegerTypeKind) {
        unsigned wt = LLVMGetIntTypeWidth(t);
        unsigned wg = LLVMGetIntTypeWidth(target);
        if (wt < wg)
            r = wt == 1 ? LLVMBuildZExt(lg.builder, v, target, name)
                        : LLVMBuildSExt(lg.builder, v, target, name);
        else
            r = LLVMBuildTrunc(lg.builder, v, target, name);
    }
    free(name);
    if (!r) llvm_unsupported("преобразуване между типовете");
    return r;
}

/* C truthiness: i1 → както е; цяло число → != 0 */
static LLVMValueRef to_bool(LLVMValueRef v) {
    LLVMTypeRef t = LLVMTypeOf(v);
    if (t == lg.i1_ty) return v;
    if (LLVMGetTypeKind(t) == LLVMIntegerTypeKind) {
        char *name = tmp_name();
        LLVMValueRef r = LLVMBuildICmp(lg.builder, LLVMIntNE, v,
                                       LLVMConstInt(t, 0, 0), name);
        free(name);
        return r;
    }
    llvm_unsupported("булев контекст върху не-числена стойност");
    return NULL; /* unreachable */
}

/* ---- Runtime helpers: baga_* като lazy IR функции ----
 *
 * Огледало на C preamble-а в codegen_c.c: всяка helper функция се
 * генерира най-много веднъж, при първа употреба (baga_rt), като
 * текущият insert block на builder-а се запазва и възстановява. */

static LLVMValueRef baga_rt(const char *name);
static int extern_ret_is_str_llvm(Node *ef);

static LLVMValueRef rt_libc(const char *name, LLVMTypeRef ret,
                            LLVMTypeRef *params, int nparams) {
    LLVMValueRef fn = LLVMGetNamedFunction(lg.mod, name);
    if (fn) return fn;
    return LLVMAddFunction(lg.mod, name,
        LLVMFunctionType(ret, params, (unsigned)nparams, 0));
}

static LLVMValueRef rt_malloc(void) {
    LLVMTypeRef p[] = { lg.i64_ty };
    return rt_libc("malloc", lg.ptr_ty, p, 1);
}
static LLVMValueRef rt_realloc(void) {
    LLVMTypeRef p[] = { lg.ptr_ty, lg.i64_ty };
    return rt_libc("realloc", lg.ptr_ty, p, 2);
}
static LLVMValueRef rt_free(void) {
    LLVMTypeRef p[] = { lg.ptr_ty };
    return rt_libc("free", lg.void_ty, p, 1);
}
static LLVMValueRef rt_memcpy(void) {
    LLVMTypeRef p[] = { lg.ptr_ty, lg.ptr_ty, lg.i64_ty };
    return rt_libc("memcpy", lg.ptr_ty, p, 3);
}
static LLVMValueRef rt_strlen(void) {
    LLVMTypeRef p[] = { lg.ptr_ty };
    return rt_libc("strlen", lg.i64_ty, p, 1);
}
static LLVMValueRef rt_strcmp(void) {
    LLVMTypeRef p[] = { lg.ptr_ty, lg.ptr_ty };
    return rt_libc("strcmp", lg.i32_ty, p, 2);
}

/* повикване на вече декларирана функция в helper тяло */
static LLVMValueRef h_call(LLVMValueRef fn, LLVMValueRef *args, int nargs,
                           const char *name) {
    return LLVMBuildCall2(lg.builder, LLVMGetElementType(LLVMTypeOf(fn)),
                          fn, args, (unsigned)nargs, name);
}

/* entry block + позициониране на builder-а за ново helper тяло */
static void h_begin(LLVMValueRef fn) {
    LLVMBasicBlockRef entry = LLVMAppendBasicBlockInContext(lg.ctx, fn, "entry");
    LLVMPositionBuilderAtEnd(lg.builder, entry);
}

/* alloca винаги в entry block-а на текущата функция: alloca на текущата
 * insert точка в тяло на цикъл заделя нов стек на всяка итерация и
 * препълва стека при дълги цикли (sha256 върху 100KB низ). */
static LLVMValueRef entry_alloca(LLVMTypeRef ty, const char *name) {
    LLVMValueRef fn = LLVMGetBasicBlockParent(LLVMGetInsertBlock(lg.builder));
    LLVMBasicBlockRef entry = LLVMGetEntryBasicBlock(fn);
    LLVMBuilderRef eb = LLVMCreateBuilderInContext(lg.ctx);
    LLVMValueRef first = LLVMGetFirstInstruction(entry);
    if (first) LLVMPositionBuilderBefore(eb, first);
    else LLVMPositionBuilderAtEnd(eb, entry);
    LLVMValueRef a = LLVMBuildAlloca(eb, ty, name);
    LLVMDisposeBuilder(eb);
    return a;
}

/* static int64_t baga_len(const char *s) { return (int64_t)strlen(s); } */
static LLVMValueRef build_baga_len(void) {
    LLVMTypeRef p[] = { lg.ptr_ty };
    LLVMValueRef fn = LLVMAddFunction(lg.mod, "baga_len",
        LLVMFunctionType(lg.i64_ty, p, 1, 0));
    h_begin(fn);
    LLVMValueRef a[] = { LLVMGetParam(fn, 0) };
    LLVMBuildRet(lg.builder, h_call(rt_strlen(), a, 1, "n"));
    return fn;
}

/* static int64_t baga_char_at(const char *s, int64_t i)
 * { return (int64_t)(unsigned char)s[i]; } */
static LLVMValueRef build_baga_char_at(void) {
    LLVMTypeRef p[] = { lg.ptr_ty, lg.i64_ty };
    LLVMValueRef fn = LLVMAddFunction(lg.mod, "baga_char_at",
        LLVMFunctionType(lg.i64_ty, p, 2, 0));
    h_begin(fn);
    LLVMValueRef s = LLVMGetParam(fn, 0);
    LLVMValueRef i = LLVMGetParam(fn, 1);
    LLVMValueRef p8 = LLVMBuildGEP2(lg.builder, lg.i8_ty, s, &i, 1, "p");
    LLVMValueRef ch = LLVMBuildLoad2(lg.builder, lg.i8_ty, p8, "c");
    LLVMBuildRet(lg.builder, LLVMBuildZExt(lg.builder, ch, lg.i64_ty, "r"));
    return fn;
}

/* static const char *baga_substr(const char *s, int64_t a, int64_t b) {
 *     int64_t n = b - a; if (n < 0) n = 0;
 *     char *r = malloc(n + 1); memcpy(r, s + a, n); r[n] = 0; return r; } */
static LLVMValueRef build_baga_substr(void) {
    LLVMTypeRef p[] = { lg.ptr_ty, lg.i64_ty, lg.i64_ty };
    LLVMValueRef fn = LLVMAddFunction(lg.mod, "baga_substr",
        LLVMFunctionType(lg.ptr_ty, p, 3, 0));
    h_begin(fn);
    LLVMValueRef s = LLVMGetParam(fn, 0);
    LLVMValueRef a = LLVMGetParam(fn, 1);
    LLVMValueRef b = LLVMGetParam(fn, 2);
    LLVMValueRef n0 = LLVMBuildSub(lg.builder, b, a, "n0");
    LLVMValueRef neg = LLVMBuildICmp(lg.builder, LLVMIntSLT, n0,
        LLVMConstInt(lg.i64_ty, 0, 0), "neg");
    LLVMValueRef n = LLVMBuildSelect(lg.builder, neg,
        LLVMConstInt(lg.i64_ty, 0, 0), n0, "n");
    LLVMValueRef n1 = LLVMBuildAdd(lg.builder, n,
        LLVMConstInt(lg.i64_ty, 1, 0), "n1");
    LLVMValueRef ma[] = { n1 };
    LLVMValueRef r = h_call(rt_malloc(), ma, 1, "r");
    LLVMValueRef sa = LLVMBuildGEP2(lg.builder, lg.i8_ty, s, &a, 1, "sa");
    LLVMValueRef mc[] = { r, sa, n };
    h_call(rt_memcpy(), mc, 3, "mc");
    LLVMValueRef rn = LLVMBuildGEP2(lg.builder, lg.i8_ty, r, &n, 1, "rn");
    LLVMBuildStore(lg.builder, LLVMConstInt(lg.i8_ty, 0, 0), rn);
    LLVMBuildRet(lg.builder, r);
    return fn;
}

/* static const char *baga_concat(const char *a, const char *b) {
 *     size_t la = strlen(a), lb = strlen(b);
 *     char *r = malloc(la + lb + 1);
 *     memcpy(r, a, la); memcpy(r + la, b, lb + 1); return r; } */
static LLVMValueRef build_baga_concat(void) {
    LLVMTypeRef p[] = { lg.ptr_ty, lg.ptr_ty };
    LLVMValueRef fn = LLVMAddFunction(lg.mod, "baga_concat",
        LLVMFunctionType(lg.ptr_ty, p, 2, 0));
    h_begin(fn);
    LLVMValueRef a = LLVMGetParam(fn, 0);
    LLVMValueRef b = LLVMGetParam(fn, 1);
    LLVMValueRef aa[] = { a };
    LLVMValueRef la = h_call(rt_strlen(), aa, 1, "la");
    LLVMValueRef ba[] = { b };
    LLVMValueRef lb = h_call(rt_strlen(), ba, 1, "lb");
    LLVMValueRef n1 = LLVMBuildAdd(lg.builder,
        LLVMBuildAdd(lg.builder, la, lb, "n"),
        LLVMConstInt(lg.i64_ty, 1, 0), "n1");
    LLVMValueRef ma[] = { n1 };
    LLVMValueRef r = h_call(rt_malloc(), ma, 1, "r");
    LLVMValueRef mc1[] = { r, a, la };
    h_call(rt_memcpy(), mc1, 3, "mc1");
    LLVMValueRef rla = LLVMBuildGEP2(lg.builder, lg.i8_ty, r, &la, 1, "rla");
    LLVMValueRef lb1 = LLVMBuildAdd(lg.builder, lb,
        LLVMConstInt(lg.i64_ty, 1, 0), "lb1");
    LLVMValueRef mc2[] = { rla, b, lb1 };
    h_call(rt_memcpy(), mc2, 3, "mc2");
    LLVMBuildRet(lg.builder, r);
    return fn;
}

/* static int64_t baga_str_eq(const char *a, const char *b)
 * { return strcmp(a, b) == 0; } */
static LLVMValueRef build_baga_str_eq(void) {
    LLVMTypeRef p[] = { lg.ptr_ty, lg.ptr_ty };
    LLVMValueRef fn = LLVMAddFunction(lg.mod, "baga_str_eq",
        LLVMFunctionType(lg.i64_ty, p, 2, 0));
    h_begin(fn);
    LLVMValueRef ab[] = { LLVMGetParam(fn, 0), LLVMGetParam(fn, 1) };
    LLVMValueRef c = h_call(rt_strcmp(), ab, 2, "c");
    LLVMValueRef eq = LLVMBuildICmp(lg.builder, LLVMIntEQ, c,
        LLVMConstInt(lg.i32_ty, 0, 0), "eq");
    LLVMBuildRet(lg.builder, LLVMBuildZExt(lg.builder, eq, lg.i64_ty, "r"));
    return fn;
}

/* static const char *baga_chr(int64_t c) — UTF-8 encode (1–4 bytes);
 * оглежда C runtime helper-а. Преди LP3 LLVM версията орязваше до един
 * байт ((char)c) и се разминаваше с C за code point-ове ≥ 0x80. */
static LLVMValueRef build_baga_chr(void) {
    LLVMTypeRef p[] = { lg.i64_ty };
    LLVMValueRef fn = LLVMAddFunction(lg.mod, "baga_chr",
        LLVMFunctionType(lg.ptr_ty, p, 1, 0));
    h_begin(fn);
    LLVMValueRef c = LLVMGetParam(fn, 0);
    LLVMValueRef five = LLVMConstInt(lg.i64_ty, 5, 0);
    LLVMValueRef ma[] = { five };
    LLVMValueRef r = h_call(rt_malloc(), ma, 1, "r");

    LLVMBasicBlockRef b1 = LLVMAppendBasicBlockInContext(lg.ctx, fn, "u1");
    LLVMBasicBlockRef k2 = LLVMAppendBasicBlockInContext(lg.ctx, fn, "k2");
    LLVMBasicBlockRef b2 = LLVMAppendBasicBlockInContext(lg.ctx, fn, "u2");
    LLVMBasicBlockRef k3 = LLVMAppendBasicBlockInContext(lg.ctx, fn, "k3");
    LLVMBasicBlockRef b3 = LLVMAppendBasicBlockInContext(lg.ctx, fn, "u3");
    LLVMBasicBlockRef b4 = LLVMAppendBasicBlockInContext(lg.ctx, fn, "u4");

    LLVMValueRef lt1 = LLVMBuildICmp(lg.builder, LLVMIntULT, c,
        LLVMConstInt(lg.i64_ty, 0x80, 0), "lt1");
    LLVMBuildCondBr(lg.builder, lt1, b1, k2);

    LLVMPositionBuilderAtEnd(lg.builder, k2);
    LLVMValueRef lt2 = LLVMBuildICmp(lg.builder, LLVMIntULT, c,
        LLVMConstInt(lg.i64_ty, 0x800, 0), "lt2");
    LLVMBuildCondBr(lg.builder, lt2, b2, k3);

    LLVMPositionBuilderAtEnd(lg.builder, k3);
    LLVMValueRef lt3 = LLVMBuildICmp(lg.builder, LLVMIntULT, c,
        LLVMConstInt(lg.i64_ty, 0x10000, 0), "lt3");
    LLVMBuildCondBr(lg.builder, lt3, b3, b4);

    #define CHR_SH(v, n) LLVMBuildLShr(lg.builder, (v), LLVMConstInt(lg.i64_ty, (n), 0), "s")
    #define CHR_AN(v, m) LLVMBuildAnd(lg.builder, (v), LLVMConstInt(lg.i64_ty, (m), 0), "a")
    #define CHR_OR(v, m) LLVMBuildOr(lg.builder, (v), LLVMConstInt(lg.i64_ty, (m), 0), "o")
    #define CHR_ST(idx, val) do { \
        LLVMValueRef _i = LLVMConstInt(lg.i64_ty, (idx), 0); \
        LLVMValueRef _p = LLVMBuildGEP2(lg.builder, lg.i8_ty, r, &_i, 1, "sp"); \
        LLVMBuildStore(lg.builder, LLVMBuildTrunc(lg.builder, (val), lg.i8_ty, "st"), _p); \
    } while (0)

    LLVMPositionBuilderAtEnd(lg.builder, b1);
    CHR_ST(0, c);
    CHR_ST(1, LLVMConstInt(lg.i64_ty, 0, 0));
    LLVMBuildRet(lg.builder, r);

    LLVMPositionBuilderAtEnd(lg.builder, b2);
    CHR_ST(0, CHR_OR(CHR_SH(c, 6), 0xC0));
    CHR_ST(1, CHR_OR(CHR_AN(c, 0x3F), 0x80));
    CHR_ST(2, LLVMConstInt(lg.i64_ty, 0, 0));
    LLVMBuildRet(lg.builder, r);

    LLVMPositionBuilderAtEnd(lg.builder, b3);
    CHR_ST(0, CHR_OR(CHR_SH(c, 12), 0xE0));
    CHR_ST(1, CHR_OR(CHR_AN(CHR_SH(c, 6), 0x3F), 0x80));
    CHR_ST(2, CHR_OR(CHR_AN(c, 0x3F), 0x80));
    CHR_ST(3, LLVMConstInt(lg.i64_ty, 0, 0));
    LLVMBuildRet(lg.builder, r);

    LLVMPositionBuilderAtEnd(lg.builder, b4);
    CHR_ST(0, CHR_OR(CHR_SH(c, 18), 0xF0));
    CHR_ST(1, CHR_OR(CHR_AN(CHR_SH(c, 12), 0x3F), 0x80));
    CHR_ST(2, CHR_OR(CHR_AN(CHR_SH(c, 6), 0x3F), 0x80));
    CHR_ST(3, CHR_OR(CHR_AN(c, 0x3F), 0x80));
    CHR_ST(4, LLVMConstInt(lg.i64_ty, 0, 0));
    LLVMBuildRet(lg.builder, r);

    #undef CHR_SH
    #undef CHR_AN
    #undef CHR_OR
    #undef CHR_ST
    return fn;
}

/* static int64_t baga_ord(const char *s) — UTF-8 code point of s[0..].
 * Mirrors the C runtime helper (1–4 byte sequences) so the LLVM oracle
 * matches byte-for-byte on non-ASCII input. */
static LLVMValueRef build_baga_ord(void) {
    LLVMTypeRef p[] = { lg.ptr_ty };
    LLVMValueRef fn = LLVMAddFunction(lg.mod, "baga_ord",
        LLVMFunctionType(lg.i64_ty, p, 1, 0));
    h_begin(fn);
    LLVMValueRef s = LLVMGetParam(fn, 0);
    LLVMValueRef z = LLVMConstInt(lg.i64_ty, 0, 0);
    LLVMValueRef one = LLVMConstInt(lg.i64_ty, 1, 0);

    /* c8 = s[0]; empty string → 0 */
    LLVMValueRef p0 = LLVMBuildGEP2(lg.builder, lg.i8_ty, s, &z, 1, "p0");
    LLVMValueRef c8 = LLVMBuildLoad2(lg.builder, lg.i8_ty, p0, "c8");
    LLVMValueRef nz = LLVMBuildICmp(lg.builder, LLVMIntNE, c8,
        LLVMConstInt(lg.i8_ty, 0, 0), "nz");
    /* c lives in an alloca so every block sees the same value */
    LLVMValueRef ca = entry_alloca(lg.i64_ty, "ca");
    LLVMBuildStore(lg.builder, LLVMBuildZExt(lg.builder, c8, lg.i64_ty, "c"), ca);

    LLVMBasicBlockRef zero_b  = LLVMAppendBasicBlockInContext(lg.ctx, fn, "zero");
    LLVMBasicBlockRef chk_b   = LLVMAppendBasicBlockInContext(lg.ctx, fn, "chk");
    LLVMBasicBlockRef ascii_b = LLVMAppendBasicBlockInContext(lg.ctx, fn, "ascii");
    LLVMBasicBlockRef two_b   = LLVMAppendBasicBlockInContext(lg.ctx, fn, "two");
    LLVMBasicBlockRef three_b = LLVMAppendBasicBlockInContext(lg.ctx, fn, "three");
    LLVMBasicBlockRef four_b  = LLVMAppendBasicBlockInContext(lg.ctx, fn, "four");
    LLVMBuildCondBr(lg.builder, nz, chk_b, zero_b);

    /* chk: c < 0x80 → ascii; (c&0xE0)==0xC0 → two; (c&0xF0)==0xE0 → three; else four */
    LLVMPositionBuilderAtEnd(lg.builder, chk_b);
    LLVMValueRef c = LLVMBuildLoad2(lg.builder, lg.i64_ty, ca, "c");
    LLVMValueRef is_ascii = LLVMBuildICmp(lg.builder, LLVMIntULT, c,
        LLVMConstInt(lg.i64_ty, 0x80, 0), "is_ascii");
    LLVMBasicBlockRef chk2_b = LLVMAppendBasicBlockInContext(lg.ctx, fn, "chk2");
    LLVMBuildCondBr(lg.builder, is_ascii, ascii_b, chk2_b);

    LLVMPositionBuilderAtEnd(lg.builder, chk2_b);
    LLVMValueRef mE0 = LLVMBuildAnd(lg.builder, c,
        LLVMConstInt(lg.i64_ty, 0xE0, 0), "mE0");
    LLVMValueRef is_two = LLVMBuildICmp(lg.builder, LLVMIntEQ, mE0,
        LLVMConstInt(lg.i64_ty, 0xC0, 0), "is_two");
    LLVMBasicBlockRef chk3_b = LLVMAppendBasicBlockInContext(lg.ctx, fn, "chk3");
    LLVMBuildCondBr(lg.builder, is_two, two_b, chk3_b);

    LLVMPositionBuilderAtEnd(lg.builder, chk3_b);
    LLVMValueRef mF0 = LLVMBuildAnd(lg.builder, c,
        LLVMConstInt(lg.i64_ty, 0xF0, 0), "mF0");
    LLVMValueRef is_three = LLVMBuildICmp(lg.builder, LLVMIntEQ, mF0,
        LLVMConstInt(lg.i64_ty, 0xE0, 0), "is_three");
    LLVMBuildCondBr(lg.builder, is_three, three_b, four_b);

    LLVMPositionBuilderAtEnd(lg.builder, zero_b);
    LLVMBuildRet(lg.builder, z);

    LLVMPositionBuilderAtEnd(lg.builder, ascii_b);
    LLVMBuildRet(lg.builder, LLVMBuildLoad2(lg.builder, lg.i64_ty, ca, "c1"));

    /* continuation bytes: dst = s[idx] & 0x3F (zext i8 → i64 first) */
    #define ORD_CONT(idx, dst, nm) do { \
        LLVMValueRef nm ## p = LLVMBuildGEP2(lg.builder, lg.i8_ty, s, &(idx), 1, #nm "p"); \
        LLVMValueRef nm ## b8 = LLVMBuildLoad2(lg.builder, lg.i8_ty, nm ## p, #nm "b8"); \
        dst = LLVMBuildAnd(lg.builder, \
            LLVMBuildZExt(lg.builder, nm ## b8, lg.i64_ty, #nm "e"), \
            LLVMConstInt(lg.i64_ty, 0x3F, 0), #nm); \
    } while (0)

    LLVMPositionBuilderAtEnd(lg.builder, two_b);
    LLVMValueRef cv2 = LLVMBuildLoad2(lg.builder, lg.i64_ty, ca, "c2");
    LLVMValueRef b1;
    ORD_CONT(one, b1, t1);
    LLVMValueRef head2 = LLVMBuildShl(lg.builder,
        LLVMBuildAnd(lg.builder, cv2, LLVMConstInt(lg.i64_ty, 0x1F, 0), "h2"),
        LLVMConstInt(lg.i64_ty, 6, 0), "h2s");
    LLVMBuildRet(lg.builder, LLVMBuildOr(lg.builder, head2, b1, "r2"));

    LLVMPositionBuilderAtEnd(lg.builder, three_b);
    LLVMValueRef cv3 = LLVMBuildLoad2(lg.builder, lg.i64_ty, ca, "c3");
    LLVMValueRef two_i = LLVMConstInt(lg.i64_ty, 2, 0);
    LLVMValueRef b2;
    ORD_CONT(one, b1, u1);
    ORD_CONT(two_i, b2, u2);
    LLVMValueRef head3 = LLVMBuildShl(lg.builder,
        LLVMBuildAnd(lg.builder, cv3, LLVMConstInt(lg.i64_ty, 0x0F, 0), "h3"),
        LLVMConstInt(lg.i64_ty, 12, 0), "h3s");
    LLVMValueRef mid3 = LLVMBuildShl(lg.builder, b1,
        LLVMConstInt(lg.i64_ty, 6, 0), "m3");
    LLVMBuildRet(lg.builder, LLVMBuildOr(lg.builder,
        LLVMBuildOr(lg.builder, head3, mid3, "hm3"), b2, "r3"));

    LLVMPositionBuilderAtEnd(lg.builder, four_b);
    LLVMValueRef cv4 = LLVMBuildLoad2(lg.builder, lg.i64_ty, ca, "c4");
    LLVMValueRef three_i = LLVMConstInt(lg.i64_ty, 3, 0);
    LLVMValueRef b3;
    ORD_CONT(one, b1, v1);
    ORD_CONT(two_i, b2, v2);
    ORD_CONT(three_i, b3, v3);
    LLVMValueRef head4 = LLVMBuildShl(lg.builder,
        LLVMBuildAnd(lg.builder, cv4, LLVMConstInt(lg.i64_ty, 0x07, 0), "h4"),
        LLVMConstInt(lg.i64_ty, 18, 0), "h4s");
    LLVMValueRef m4a = LLVMBuildShl(lg.builder, b1,
        LLVMConstInt(lg.i64_ty, 12, 0), "m4a");
    LLVMValueRef m4b = LLVMBuildShl(lg.builder, b2,
        LLVMConstInt(lg.i64_ty, 6, 0), "m4b");
    LLVMBuildRet(lg.builder, LLVMBuildOr(lg.builder,
        LLVMBuildOr(lg.builder,
            LLVMBuildOr(lg.builder, head4, m4a, "hm4"), m4b, "hmm4"), b3, "r4"));
    #undef ORD_CONT
    return fn;
}

/* static const char *baga_i64_to_str(int64_t x) — decimal, '-' for negatives.
 * Mirrors the C runtime helper so interpolation is C/LLVM-parity. */
static LLVMValueRef build_baga_i64_to_str(void) {
    LLVMTypeRef p[] = { lg.i64_ty };
    LLVMValueRef fn = LLVMAddFunction(lg.mod, "baga_i64_to_str",
        LLVMFunctionType(lg.ptr_ty, p, 1, 0));
    h_begin(fn);
    LLVMValueRef x = LLVMGetParam(fn, 0);
    LLVMValueRef i0 = LLVMConstInt(lg.i64_ty, 0, 0);
    LLVMValueRef i1 = LLVMConstInt(lg.i64_ty, 1, 0);
    LLVMValueRef i10 = LLVMConstInt(lg.i64_ty, 10, 0);
    LLVMValueRef i48 = LLVMConstInt(lg.i64_ty, 48, 0);
    LLVMValueRef i24 = LLVMConstInt(lg.i64_ty, 24, 0);
    LLVMValueRef tmp = h_call(rt_malloc(), (LLVMValueRef[]){ i24 }, 1, "tmp");
    LLVMValueRef v = entry_alloca(lg.i64_ty, "v");
    LLVMValueRef neg = entry_alloca(lg.i64_ty, "neg");
    LLVMValueRef cnt = entry_alloca(lg.i64_ty, "cnt");
    LLVMValueRef isneg = LLVMBuildICmp(lg.builder, LLVMIntSLT, x, i0, "isneg");
    LLVMBuildStore(lg.builder, LLVMBuildZExt(lg.builder, isneg, lg.i64_ty, "isneg64"), neg);
    LLVMValueRef negv = LLVMBuildSub(lg.builder, i0, x, "negv");
    LLVMValueRef v0 = LLVMBuildSelect(lg.builder, isneg, negv, x, "v0");
    LLVMBuildStore(lg.builder, v0, v);
    LLVMBuildStore(lg.builder, i0, cnt);
    LLVMValueRef isz = LLVMBuildICmp(lg.builder, LLVMIntEQ, v0, i0, "isz");
    LLVMBasicBlockRef zb = LLVMAppendBasicBlockInContext(lg.ctx, fn, "zero");
    LLVMBasicBlockRef lb = LLVMAppendBasicBlockInContext(lg.ctx, fn, "loop");
    LLVMBasicBlockRef lb2 = LLVMAppendBasicBlockInContext(lg.ctx, fn, "loop2");
    LLVMBuildCondBr(lg.builder, isz, zb, lb);
    LLVMPositionBuilderAtEnd(lg.builder, zb);
    LLVMBuildStore(lg.builder, LLVMConstInt(lg.i8_ty, 48, 0),
        LLVMBuildGEP2(lg.builder, lg.i8_ty, tmp, &i0, 1, "zp"));
    LLVMBuildStore(lg.builder, i1, cnt);
    LLVMBuildBr(lg.builder, lb2);
    LLVMPositionBuilderAtEnd(lg.builder, lb);
    LLVMValueRef cv = LLVMBuildLoad2(lg.builder, lg.i64_ty, v, "cv");
    LLVMValueRef rem = LLVMBuildBinOp(lg.builder, LLVMSRem, cv, i10, "rem");
    LLVMValueRef dch = LLVMBuildTrunc(lg.builder, LLVMBuildAdd(lg.builder, rem, i48, "d"), lg.i8_ty, "dch");
    LLVMValueRef ci = LLVMBuildLoad2(lg.builder, lg.i64_ty, cnt, "ci");
    LLVMBuildStore(lg.builder, dch, LLVMBuildGEP2(lg.builder, lg.i8_ty, tmp, &ci, 1, "dp"));
    LLVMBuildStore(lg.builder, LLVMBuildAdd(lg.builder, ci, i1, "ci1"), cnt);
    LLVMBuildStore(lg.builder, LLVMBuildBinOp(lg.builder, LLVMSDiv, cv, i10, "q"), v);
    LLVMBuildBr(lg.builder, lb2);
    LLVMPositionBuilderAtEnd(lg.builder, lb2);
    LLVMValueRef cv2 = LLVMBuildLoad2(lg.builder, lg.i64_ty, v, "cv2");
    LLVMValueRef cont = LLVMBuildICmp(lg.builder, LLVMIntSGT, cv2, i0, "cont");
    LLVMBasicBlockRef rb = LLVMAppendBasicBlockInContext(lg.ctx, fn, "rev");
    LLVMBuildCondBr(lg.builder, cont, lb, rb);
    LLVMPositionBuilderAtEnd(lg.builder, rb);
    LLVMValueRef c = LLVMBuildLoad2(lg.builder, lg.i64_ty, cnt, "c");
    LLVMValueRef isng64 = LLVMBuildLoad2(lg.builder, lg.i64_ty, neg, "isng64");
    LLVMValueRef isng = LLVMBuildICmp(lg.builder, LLVMIntNE, isng64,
        LLVMConstInt(lg.i64_ty, 0, 0), "isng");
    LLVMValueRef tlen = LLVMBuildAdd(lg.builder, c, isng64, "tlen");   /* +1 if negative */
    LLVMValueRef rlen = LLVMBuildAdd(lg.builder, tlen, i1, "rlen");
    LLVMValueRef res = h_call(rt_malloc(), (LLVMValueRef[]){ rlen }, 1, "res");
    LLVMValueRef rp = entry_alloca(lg.i64_ty, "rp");
    LLVMBuildStore(lg.builder, i0, rp);
    LLVMBasicBlockRef mb = LLVMAppendBasicBlockInContext(lg.ctx, fn, "minus");
    LLVMBasicBlockRef rvb = LLVMAppendBasicBlockInContext(lg.ctx, fn, "revloop");
    LLVMBuildCondBr(lg.builder, isng, mb, rvb);
    LLVMPositionBuilderAtEnd(lg.builder, mb);
    LLVMBuildStore(lg.builder, LLVMConstInt(lg.i8_ty, 45, 0),
        LLVMBuildGEP2(lg.builder, lg.i8_ty, res, &i0, 1, "mp"));
    LLVMBuildStore(lg.builder, i1, rp);
    LLVMBuildBr(lg.builder, rvb);
    LLVMPositionBuilderAtEnd(lg.builder, rvb);
    LLVMBasicBlockRef cond = LLVMAppendBasicBlockInContext(lg.ctx, fn, "cond");
    LLVMBasicBlockRef body = LLVMAppendBasicBlockInContext(lg.ctx, fn, "body");
    LLVMBasicBlockRef done = LLVMAppendBasicBlockInContext(lg.ctx, fn, "done");
    LLVMBuildBr(lg.builder, cond);
    LLVMPositionBuilderAtEnd(lg.builder, cond);
    LLVMValueRef rpv = LLVMBuildLoad2(lg.builder, lg.i64_ty, rp, "rpv");
    LLVMValueRef idx = LLVMBuildSub(lg.builder, LLVMBuildSub(lg.builder, tlen, i1, "t1"), rpv, "idx");
    LLVMValueRef done_c = LLVMBuildICmp(lg.builder, LLVMIntSGE, rpv, tlen, "donec");
    LLVMBuildCondBr(lg.builder, done_c, done, body);
    LLVMPositionBuilderAtEnd(lg.builder, body);
    LLVMValueRef ch2 = LLVMBuildLoad2(lg.builder, lg.i8_ty,
        LLVMBuildGEP2(lg.builder, lg.i8_ty, tmp, &idx, 1, "sp"), "ch2");
    LLVMBuildStore(lg.builder, ch2, LLVMBuildGEP2(lg.builder, lg.i8_ty, res, &rpv, 1, "dtp"));
    LLVMBuildStore(lg.builder, LLVMBuildAdd(lg.builder, rpv, i1, "rp1"), rp);
    LLVMBuildBr(lg.builder, cond);
    LLVMPositionBuilderAtEnd(lg.builder, done);
    LLVMValueRef rpv2 = LLVMBuildLoad2(lg.builder, lg.i64_ty, rp, "rpv2");
    LLVMBuildStore(lg.builder, LLVMConstInt(lg.i8_ty, 0, 0),
        LLVMBuildGEP2(lg.builder, lg.i8_ty, res, &rpv2, 1, "np"));
    LLVMBuildRet(lg.builder, res);
    return fn;
}

/* ---- Vec: baga_Vec = { void **data, int64_t len, int64_t cap } ----
 * В IR: { i8*, i64, i64 } с елемент i8* (void*); i64 стойности се
 * пакетират с inttoptr/ptrtoint — точно като (void*)(intptr_t)x в C. */

static LLVMTypeRef baga_vec_ty(void) {
    LLVMTypeRef t = LLVMGetTypeByName(lg.mod, "baga_Vec");
    if (t) return t;
    t = LLVMStructCreateNamed(lg.ctx, "baga_Vec");
    LLVMTypeRef elems[] = { LLVMPointerType(lg.ptr_ty, 0), lg.i64_ty, lg.i64_ty };
    LLVMStructSetBody(t, elems, 3, 0);
    return t;
}

static LLVMTypeRef baga_vec_ptr_ty(void) {
    return LLVMPointerType(baga_vec_ty(), 0);
}

/* baga_bytes = { i8* data, i64 len } — binary-safe buffer, passed by value
 * (must match the C struct layout exactly for output parity). */
static LLVMTypeRef baga_bytes_ty(void) {
    LLVMTypeRef t = LLVMGetTypeByName(lg.mod, "baga_bytes");
    if (t) return t;
    t = LLVMStructCreateNamed(lg.ctx, "baga_bytes");
    LLVMTypeRef elems[] = { lg.ptr_ty, lg.i64_ty };
    LLVMStructSetBody(t, elems, 2, 0);
    return t;
}

/* GEP към поле на baga_Vec (0=data, 1=len, 2=cap) + load */
static LLVMValueRef vec_field_ptr(LLVMValueRef v, unsigned idx, const char *nm) {
    return LLVMBuildStructGEP2(lg.builder, baga_vec_ty(), v, idx, nm);
}
static LLVMValueRef vec_load_len(LLVMValueRef v) {
    return LLVMBuildLoad2(lg.builder, lg.i64_ty, vec_field_ptr(v, 1, "lenp"), "len");
}
static LLVMValueRef vec_load_data(LLVMValueRef v) {
    return LLVMBuildLoad2(lg.builder, LLVMPointerType(lg.ptr_ty, 0),
                          vec_field_ptr(v, 0, "datap"), "data");
}

/* static void baga_vec_grow(baga_Vec *v) {
 *     if (v->len == v->cap) { v->cap *= 2;
 *         v->data = realloc(v->data, v->cap * sizeof(void *)); } } */
static LLVMValueRef build_baga_vec_grow(void) {
    LLVMTypeRef p[] = { baga_vec_ptr_ty() };
    LLVMValueRef fn = LLVMAddFunction(lg.mod, "baga_vec_grow",
        LLVMFunctionType(lg.void_ty, p, 1, 0));
    h_begin(fn);
    LLVMValueRef v = LLVMGetParam(fn, 0);
    LLVMValueRef len = vec_load_len(v);
    LLVMValueRef capp = vec_field_ptr(v, 2, "capp");
    LLVMValueRef cap = LLVMBuildLoad2(lg.builder, lg.i64_ty, capp, "cap");
    LLVMValueRef full = LLVMBuildICmp(lg.builder, LLVMIntEQ, len, cap, "full");
    LLVMBasicBlockRef grow_bb = LLVMAppendBasicBlockInContext(lg.ctx, fn, "grow");
    LLVMBasicBlockRef done_bb = LLVMAppendBasicBlockInContext(lg.ctx, fn, "done");
    LLVMBuildCondBr(lg.builder, full, grow_bb, done_bb);
    LLVMPositionBuilderAtEnd(lg.builder, grow_bb);
    LLVMValueRef ncap = LLVMBuildMul(lg.builder, cap,
        LLVMConstInt(lg.i64_ty, 2, 0), "ncap");
    LLVMBuildStore(lg.builder, ncap, capp);
    LLVMValueRef data = vec_load_data(v);
    LLVMValueRef nbytes = LLVMBuildMul(lg.builder, ncap,
        LLVMConstInt(lg.i64_ty, 8, 0), "nbytes");
    LLVMValueRef ra[] = {
        LLVMBuildBitCast(lg.builder, data, lg.ptr_ty, "raw"),
        nbytes
    };
    LLVMValueRef nd = h_call(rt_realloc(), ra, 2, "nd");
    LLVMBuildStore(lg.builder,
        LLVMBuildBitCast(lg.builder, nd, LLVMPointerType(lg.ptr_ty, 0), "ndc"),
        vec_field_ptr(v, 0, "datap"));
    LLVMBuildBr(lg.builder, done_bb);
    LLVMPositionBuilderAtEnd(lg.builder, done_bb);
    LLVMBuildRetVoid(lg.builder);
    return fn;
}

/* static baga_Vec *baga_vec_new(void) {
 *     baga_Vec *v = malloc(sizeof(baga_Vec));
 *     v->cap = 8; v->len = 0; v->data = malloc(8 * sizeof(void *)); return v; } */
static LLVMValueRef build_baga_vec_new(void) {
    LLVMValueRef fn = LLVMAddFunction(lg.mod, "baga_vec_new",
        LLVMFunctionType(baga_vec_ptr_ty(), NULL, 0, 0));
    h_begin(fn);
    LLVMValueRef sz = LLVMSizeOf(baga_vec_ty());
    LLVMValueRef ma[] = { sz };
    LLVMValueRef raw = h_call(rt_malloc(), ma, 1, "raw");
    LLVMValueRef v = LLVMBuildBitCast(lg.builder, raw, baga_vec_ptr_ty(), "v");
    LLVMBuildStore(lg.builder, LLVMConstInt(lg.i64_ty, 8, 0),
                   vec_field_ptr(v, 2, "capp"));
    LLVMBuildStore(lg.builder, LLVMConstInt(lg.i64_ty, 0, 0),
                   vec_field_ptr(v, 1, "lenp"));
    LLVMValueRef da[] = { LLVMConstInt(lg.i64_ty, 64, 0) };
    LLVMValueRef draw = h_call(rt_malloc(), da, 1, "draw");
    LLVMBuildStore(lg.builder,
        LLVMBuildBitCast(lg.builder, draw, LLVMPointerType(lg.ptr_ty, 0), "d"),
        vec_field_ptr(v, 0, "datap"));
    LLVMBuildRet(lg.builder, v);
    return fn;
}

/* споделена стъпка за push: grow + слот data[len] + len++ */
static LLVMValueRef vec_push_slot(LLVMValueRef v) {
    LLVMValueRef ga[] = { v };
    h_call(baga_rt("baga_vec_grow"), ga, 1, "");
    LLVMValueRef len = vec_load_len(v);
    LLVMValueRef data = vec_load_data(v);
    LLVMValueRef nl = LLVMBuildAdd(lg.builder, len,
        LLVMConstInt(lg.i64_ty, 1, 0), "nl");
    LLVMBuildStore(lg.builder, nl, vec_field_ptr(v, 1, "lenp"));
    return LLVMBuildGEP2(lg.builder, lg.ptr_ty, data, &len, 1, "slot");
}

/* static void baga_vec_push_i64(baga_Vec *v, int64_t x)
 * { baga_vec_grow(v); v->data[v->len++] = (void *)(intptr_t)x; } */
static LLVMValueRef build_baga_vec_push_i64(void) {
    LLVMTypeRef p[] = { baga_vec_ptr_ty(), lg.i64_ty };
    LLVMValueRef fn = LLVMAddFunction(lg.mod, "baga_vec_push_i64",
        LLVMFunctionType(lg.void_ty, p, 2, 0));
    h_begin(fn);
    LLVMValueRef slot = vec_push_slot(LLVMGetParam(fn, 0));
    LLVMValueRef pv = LLVMBuildIntToPtr(lg.builder, LLVMGetParam(fn, 1),
        lg.ptr_ty, "pv");
    LLVMBuildStore(lg.builder, pv, slot);
    LLVMBuildRetVoid(lg.builder);
    return fn;
}

/* static void baga_vec_push_str(baga_Vec *v, const char *s)
 * { baga_vec_grow(v); v->data[v->len++] = (void *)s; } */
static LLVMValueRef build_baga_vec_push_str(void) {
    LLVMTypeRef p[] = { baga_vec_ptr_ty(), lg.ptr_ty };
    LLVMValueRef fn = LLVMAddFunction(lg.mod, "baga_vec_push_str",
        LLVMFunctionType(lg.void_ty, p, 2, 0));
    h_begin(fn);
    LLVMValueRef slot = vec_push_slot(LLVMGetParam(fn, 0));
    LLVMBuildStore(lg.builder, LLVMGetParam(fn, 1), slot);
    LLVMBuildRetVoid(lg.builder);
    return fn;
}

/* load на data[i] (като i8*) */
static LLVMValueRef vec_load_at(LLVMValueRef v, LLVMValueRef i) {
    LLVMValueRef data = vec_load_data(v);
    LLVMValueRef slot = LLVMBuildGEP2(lg.builder, lg.ptr_ty, data, &i, 1, "slot");
    return LLVMBuildLoad2(lg.builder, lg.ptr_ty, slot, "e");
}

/* static int64_t baga_vec_get_i64(baga_Vec *v, int64_t i)
 * { return (int64_t)(intptr_t)v->data[i]; } */
static LLVMValueRef build_baga_vec_get_i64(void) {
    LLVMTypeRef p[] = { baga_vec_ptr_ty(), lg.i64_ty };
    LLVMValueRef fn = LLVMAddFunction(lg.mod, "baga_vec_get_i64",
        LLVMFunctionType(lg.i64_ty, p, 2, 0));
    h_begin(fn);
    LLVMValueRef e = vec_load_at(LLVMGetParam(fn, 0), LLVMGetParam(fn, 1));
    LLVMBuildRet(lg.builder, LLVMBuildPtrToInt(lg.builder, e, lg.i64_ty, "r"));
    return fn;
}

/* static const char *baga_vec_get_str(baga_Vec *v, int64_t i)
 * { return (const char *)v->data[i]; } */
static LLVMValueRef build_baga_vec_get_str(void) {
    LLVMTypeRef p[] = { baga_vec_ptr_ty(), lg.i64_ty };
    LLVMValueRef fn = LLVMAddFunction(lg.mod, "baga_vec_get_str",
        LLVMFunctionType(lg.ptr_ty, p, 2, 0));
    h_begin(fn);
    LLVMBuildRet(lg.builder, vec_load_at(LLVMGetParam(fn, 0), LLVMGetParam(fn, 1)));
    return fn;
}

/* static void baga_vec_set_i64(baga_Vec *v, int64_t i, int64_t x)
 * { v->data[i] = (void *)(intptr_t)x; } */
static LLVMValueRef build_baga_vec_set_i64(void) {
    LLVMTypeRef p[] = { baga_vec_ptr_ty(), lg.i64_ty, lg.i64_ty };
    LLVMValueRef fn = LLVMAddFunction(lg.mod, "baga_vec_set_i64",
        LLVMFunctionType(lg.void_ty, p, 3, 0));
    h_begin(fn);
    LLVMValueRef v = LLVMGetParam(fn, 0);
    LLVMValueRef i = LLVMGetParam(fn, 1);
    LLVMValueRef data = vec_load_data(v);
    LLVMValueRef slot = LLVMBuildGEP2(lg.builder, lg.ptr_ty, data, &i, 1, "slot");
    LLVMValueRef pv = LLVMBuildIntToPtr(lg.builder, LLVMGetParam(fn, 2),
        lg.ptr_ty, "pv");
    LLVMBuildStore(lg.builder, pv, slot);
    LLVMBuildRetVoid(lg.builder);
    return fn;
}

/* static void baga_vec_set_str(baga_Vec *v, int64_t i, const char *s)
 * { v->data[i] = (void *)s; } */
static LLVMValueRef build_baga_vec_set_str(void) {
    LLVMTypeRef p[] = { baga_vec_ptr_ty(), lg.i64_ty, lg.ptr_ty };
    LLVMValueRef fn = LLVMAddFunction(lg.mod, "baga_vec_set_str",
        LLVMFunctionType(lg.void_ty, p, 3, 0));
    h_begin(fn);
    LLVMValueRef v = LLVMGetParam(fn, 0);
    LLVMValueRef i = LLVMGetParam(fn, 1);
    LLVMValueRef data = vec_load_data(v);
    LLVMValueRef slot = LLVMBuildGEP2(lg.builder, lg.ptr_ty, data, &i, 1, "slot");
    LLVMBuildStore(lg.builder, LLVMGetParam(fn, 2), slot);
    LLVMBuildRetVoid(lg.builder);
    return fn;
}

/* static int64_t baga_vec_len(baga_Vec *v) { return v->len; } */
static LLVMValueRef build_baga_vec_len(void) {
    LLVMTypeRef p[] = { baga_vec_ptr_ty() };
    LLVMValueRef fn = LLVMAddFunction(lg.mod, "baga_vec_len",
        LLVMFunctionType(lg.i64_ty, p, 1, 0));
    h_begin(fn);
    LLVMBuildRet(lg.builder, vec_load_len(LLVMGetParam(fn, 0)));
    return fn;
}

/* clamp slice bounds: a=max(a,0); b=min(b,len); b=max(b,a) */
static void slice_clamp(LLVMValueRef v, LLVMValueRef *a, LLVMValueRef *b) {
    LLVMValueRef z = LLVMConstInt(lg.i64_ty, 0, 0);
    LLVMValueRef len = vec_load_len(v);
    LLVMValueRef a0 = LLVMBuildICmp(lg.builder, LLVMIntSLT, *a, z, "a0");
    *a = LLVMBuildSelect(lg.builder, a0, z, *a, "a");
    LLVMValueRef b1 = LLVMBuildICmp(lg.builder, LLVMIntSGT, *b, len, "b1");
    *b = LLVMBuildSelect(lg.builder, b1, len, *b, "b");
    LLVMValueRef b2 = LLVMBuildICmp(lg.builder, LLVMIntSLT, *b, *a, "b2");
    *b = LLVMBuildSelect(lg.builder, b2, *a, *b, "b");
}

/* loop pushing src[a..b) elements into r via push_rt (i64/str/f64) */
static void slice_loop(LLVMValueRef fn, LLVMValueRef src, LLVMValueRef r,
                       LLVMValueRef a, LLVMValueRef b, const char *push_rt) {
    LLVMValueRef iv = LLVMBuildAlloca(lg.builder, lg.i64_ty, "i");
    LLVMBuildStore(lg.builder, a, iv);
    LLVMBasicBlockRef cond = LLVMAppendBasicBlockInContext(lg.ctx, fn, "cond");
    LLVMBasicBlockRef body = LLVMAppendBasicBlockInContext(lg.ctx, fn, "body");
    LLVMBasicBlockRef done = LLVMAppendBasicBlockInContext(lg.ctx, fn, "done");
    LLVMBuildBr(lg.builder, cond);
    LLVMPositionBuilderAtEnd(lg.builder, cond);
    LLVMValueRef i = LLVMBuildLoad2(lg.builder, lg.i64_ty, iv, "i");
    LLVMValueRef cc = LLVMBuildICmp(lg.builder, LLVMIntSLT, i, b, "cc");
    LLVMBuildCondBr(lg.builder, cc, body, done);
    LLVMPositionBuilderAtEnd(lg.builder, body);
    LLVMValueRef e = vec_load_at(src, i);
    if (strcmp(push_rt, "baga_vec_push_i64") == 0)
        e = LLVMBuildPtrToInt(lg.builder, e, lg.i64_ty, "e");
    LLVMValueRef pa[] = { r, e };
    h_call(baga_rt(push_rt), pa, 2, "");
    LLVMValueRef inext = LLVMBuildAdd(lg.builder, i, LLVMConstInt(lg.i64_ty, 1, 0), "in");
    LLVMBuildStore(lg.builder, inext, iv);
    LLVMBuildBr(lg.builder, cond);
    LLVMPositionBuilderAtEnd(lg.builder, done);
}

static LLVMValueRef build_baga_vec_slice(const char *suf, const char *push_rt) {
    char nm[64]; snprintf(nm, sizeof nm, "baga_vec_slice_%s", suf);
    LLVMTypeRef p[] = { baga_vec_ptr_ty(), lg.i64_ty, lg.i64_ty };
    LLVMValueRef fn = LLVMAddFunction(lg.mod, nm,
        LLVMFunctionType(baga_vec_ptr_ty(), p, 3, 0));
    h_begin(fn);
    LLVMValueRef v = LLVMGetParam(fn, 0);
    LLVMValueRef a = LLVMGetParam(fn, 1);
    LLVMValueRef b = LLVMGetParam(fn, 2);
    slice_clamp(v, &a, &b);
    LLVMValueRef r = h_call(baga_rt("baga_vec_new"), NULL, 0, "r");
    slice_loop(fn, v, r, a, b, push_rt);
    LLVMBuildRet(lg.builder, r);
    return fn;
}

/* loop pushing all of v's elements into r */
static void concat_loop(LLVMValueRef fn, LLVMValueRef src, LLVMValueRef r, const char *push_rt) {
    LLVMValueRef len = vec_load_len(src);
    LLVMValueRef z = LLVMConstInt(lg.i64_ty, 0, 0);
    LLVMValueRef iv = LLVMBuildAlloca(lg.builder, lg.i64_ty, "i");
    LLVMBuildStore(lg.builder, z, iv);
    LLVMBasicBlockRef cond = LLVMAppendBasicBlockInContext(lg.ctx, fn, "cond");
    LLVMBasicBlockRef body = LLVMAppendBasicBlockInContext(lg.ctx, fn, "body");
    LLVMBasicBlockRef done = LLVMAppendBasicBlockInContext(lg.ctx, fn, "done");
    LLVMBuildBr(lg.builder, cond);
    LLVMPositionBuilderAtEnd(lg.builder, cond);
    LLVMValueRef i = LLVMBuildLoad2(lg.builder, lg.i64_ty, iv, "i");
    LLVMValueRef cc = LLVMBuildICmp(lg.builder, LLVMIntSLT, i, len, "cc");
    LLVMBuildCondBr(lg.builder, cc, body, done);
    LLVMPositionBuilderAtEnd(lg.builder, body);
    LLVMValueRef e = vec_load_at(src, i);
    if (strcmp(push_rt, "baga_vec_push_i64") == 0)
        e = LLVMBuildPtrToInt(lg.builder, e, lg.i64_ty, "e");
    LLVMValueRef pa[] = { r, e };
    h_call(baga_rt(push_rt), pa, 2, "");
    LLVMValueRef inext = LLVMBuildAdd(lg.builder, i, LLVMConstInt(lg.i64_ty, 1, 0), "in");
    LLVMBuildStore(lg.builder, inext, iv);
    LLVMBuildBr(lg.builder, cond);
    LLVMPositionBuilderAtEnd(lg.builder, done);
}

static LLVMValueRef build_baga_vec_concat(const char *suf, const char *push_rt) {
    char nm[64]; snprintf(nm, sizeof nm, "baga_vec_concat_%s", suf);
    LLVMTypeRef p[] = { baga_vec_ptr_ty(), baga_vec_ptr_ty() };
    LLVMValueRef fn = LLVMAddFunction(lg.mod, nm,
        LLVMFunctionType(baga_vec_ptr_ty(), p, 2, 0));
    h_begin(fn);
    LLVMValueRef v = LLVMGetParam(fn, 0);
    LLVMValueRef w = LLVMGetParam(fn, 1);
    LLVMValueRef r = h_call(baga_rt("baga_vec_new"), NULL, 0, "r");
    concat_loop(fn, v, r, push_rt);
    concat_loop(fn, w, r, push_rt);
    LLVMBuildRet(lg.builder, r);
    return fn;
}

/* ---- arena (mirror на baga_arena_* в codegen_c) ---- */
static LLVMTypeRef baga_arena_ty(void) {
    LLVMTypeRef t = LLVMGetTypeByName(lg.mod, "baga_Arena");
    if (t) return t;
    t = LLVMStructCreateNamed(lg.ctx, "baga_Arena");
    LLVMTypeRef elems[] = { lg.ptr_ty, lg.i64_ty, lg.i64_ty };
    LLVMStructSetBody(t, elems, 3, 0);
    return t;
}
static LLVMTypeRef baga_arena_ptr_ty(void) {
    return LLVMPointerType(baga_arena_ty(), 0);
}
static LLVMValueRef arena_field_ptr(LLVMValueRef a, unsigned idx, const char *nm) {
    return LLVMBuildStructGEP2(lg.builder, baga_arena_ty(), a, idx, nm);
}

/* static int64_t baga_arena_new(void) — handle = pointer към baga_Arena */
static LLVMValueRef build_baga_arena_new(void) {
    LLVMValueRef fn = LLVMAddFunction(lg.mod, "baga_arena_new",
        LLVMFunctionType(lg.i64_ty, NULL, 0, 0));
    h_begin(fn);
    LLVMValueRef sz = LLVMSizeOf(baga_arena_ty());
    LLVMValueRef ma[] = { sz };
    LLVMValueRef raw = h_call(rt_malloc(), ma, 1, "raw");
    LLVMValueRef a = LLVMBuildBitCast(lg.builder, raw, baga_arena_ptr_ty(), "a");
    LLVMBuildStore(lg.builder, LLVMConstInt(lg.i64_ty, 65536, 0),
                   arena_field_ptr(a, 2, "capp"));
    LLVMBuildStore(lg.builder, LLVMConstInt(lg.i64_ty, 0, 0),
                   arena_field_ptr(a, 1, "usedp"));
    LLVMValueRef da[] = { LLVMConstInt(lg.i64_ty, 65536, 0) };
    LLVMValueRef draw = h_call(rt_malloc(), da, 1, "draw");
    LLVMBuildStore(lg.builder, draw, arena_field_ptr(a, 0, "basep"));
    LLVMValueRef h = LLVMBuildPtrToInt(lg.builder, a, lg.i64_ty, "h");
    LLVMBuildRet(lg.builder, h);
    return fn;
}

/* static int64_t baga_arena_alloc(int64_t h, int64_t size) — bump + grow */
static LLVMValueRef build_baga_arena_alloc(void) {
    LLVMTypeRef p[] = { lg.i64_ty, lg.i64_ty };
    LLVMValueRef fn = LLVMAddFunction(lg.mod, "baga_arena_alloc",
        LLVMFunctionType(lg.i64_ty, p, 2, 0));
    h_begin(fn);
    LLVMValueRef a = LLVMBuildIntToPtr(lg.builder, LLVMGetParam(fn, 0),
        baga_arena_ptr_ty(), "a");
    LLVMValueRef size = LLVMGetParam(fn, 1);
    LLVMValueRef usedp = arena_field_ptr(a, 1, "usedp");
    LLVMValueRef capp = arena_field_ptr(a, 2, "capp");
    LLVMValueRef used = LLVMBuildLoad2(lg.builder, lg.i64_ty, usedp, "used");
    LLVMValueRef cap = LLVMBuildLoad2(lg.builder, lg.i64_ty, capp, "cap");
    LLVMValueRef need = LLVMBuildAdd(lg.builder, used, size, "need");
    LLVMValueRef full = LLVMBuildICmp(lg.builder, LLVMIntSGT, need, cap, "full");
    LLVMBasicBlockRef grow_bb = LLVMAppendBasicBlockInContext(lg.ctx, fn, "grow");
    LLVMBasicBlockRef done_bb = LLVMAppendBasicBlockInContext(lg.ctx, fn, "done");
    LLVMBuildCondBr(lg.builder, full, grow_bb, done_bb);
    LLVMPositionBuilderAtEnd(lg.builder, grow_bb);
    LLVMValueRef nc = LLVMBuildMul(lg.builder, need,
        LLVMConstInt(lg.i64_ty, 2, 0), "nc");
    LLVMBuildStore(lg.builder, nc, capp);
    LLVMValueRef base0 = LLVMBuildLoad2(lg.builder, lg.ptr_ty,
        arena_field_ptr(a, 0, "basep"), "base0");
    LLVMValueRef ra[] = { base0, nc };
    LLVMValueRef nd = h_call(rt_realloc(), ra, 2, "nd");
    LLVMBuildStore(lg.builder, nd, arena_field_ptr(a, 0, "basep"));
    LLVMBuildBr(lg.builder, done_bb);
    LLVMPositionBuilderAtEnd(lg.builder, done_bb);
    LLVMValueRef base = LLVMBuildLoad2(lg.builder, lg.ptr_ty,
        arena_field_ptr(a, 0, "basep"), "base");
    LLVMValueRef ptr = LLVMBuildGEP2(lg.builder, lg.i8_ty, base, &used, 1, "p");
    LLVMBuildStore(lg.builder, need, usedp);
    LLVMValueRef r = LLVMBuildPtrToInt(lg.builder, ptr, lg.i64_ty, "r");
    LLVMBuildRet(lg.builder, r);
    return fn;
}

/* static void baga_arena_reset(int64_t h) — used = 0 */
static LLVMValueRef build_baga_arena_reset(void) {
    LLVMTypeRef p[] = { lg.i64_ty };
    LLVMValueRef fn = LLVMAddFunction(lg.mod, "baga_arena_reset",
        LLVMFunctionType(lg.void_ty, p, 1, 0));
    h_begin(fn);
    LLVMValueRef a = LLVMBuildIntToPtr(lg.builder, LLVMGetParam(fn, 0),
        baga_arena_ptr_ty(), "a");
    LLVMBuildStore(lg.builder, LLVMConstInt(lg.i64_ty, 0, 0),
                   arena_field_ptr(a, 1, "usedp"));
    LLVMBuildRetVoid(lg.builder);
    return fn;
}

/* static void baga_arena_free(int64_t h) — free(base); free(a) */
static LLVMValueRef build_baga_arena_free(void) {
    LLVMTypeRef p[] = { lg.i64_ty };
    LLVMValueRef fn = LLVMAddFunction(lg.mod, "baga_arena_free",
        LLVMFunctionType(lg.void_ty, p, 1, 0));
    h_begin(fn);
    LLVMValueRef a = LLVMBuildIntToPtr(lg.builder, LLVMGetParam(fn, 0),
        baga_arena_ptr_ty(), "a");
    LLVMValueRef base = LLVMBuildLoad2(lg.builder, lg.ptr_ty,
        arena_field_ptr(a, 0, "basep"), "base");
    LLVMValueRef fa[] = { base };
    h_call(rt_free(), fa, 1, "");
    LLVMValueRef raw = LLVMBuildBitCast(lg.builder, a, lg.ptr_ty, "raw");
    LLVMValueRef fb[] = { raw };
    h_call(rt_free(), fb, 1, "");
    LLVMBuildRetVoid(lg.builder);
    return fn;
}

/* f64 елементи: double ↔ i64 ↔ ptr (bitcast/inttoptr/ptrtoint) */
static LLVMValueRef build_baga_vec_push_f64(void) {
    LLVMTypeRef p[] = { baga_vec_ptr_ty(), lg.double_ty };
    LLVMValueRef fn = LLVMAddFunction(lg.mod, "baga_vec_push_f64",
        LLVMFunctionType(lg.void_ty, p, 2, 0));
    h_begin(fn);
    LLVMValueRef slot = vec_push_slot(LLVMGetParam(fn, 0));
    LLVMValueRef xi = LLVMBuildBitCast(lg.builder, LLVMGetParam(fn, 1), lg.i64_ty, "xi");
    LLVMValueRef pv = LLVMBuildIntToPtr(lg.builder, xi, lg.ptr_ty, "pv");
    LLVMBuildStore(lg.builder, pv, slot);
    LLVMBuildRetVoid(lg.builder);
    return fn;
}

static LLVMValueRef build_baga_vec_get_f64(void) {
    LLVMTypeRef p[] = { baga_vec_ptr_ty(), lg.i64_ty };
    LLVMValueRef fn = LLVMAddFunction(lg.mod, "baga_vec_get_f64",
        LLVMFunctionType(lg.double_ty, p, 2, 0));
    h_begin(fn);
    LLVMValueRef e = vec_load_at(LLVMGetParam(fn, 0), LLVMGetParam(fn, 1));
    LLVMValueRef ei = LLVMBuildPtrToInt(lg.builder, e, lg.i64_ty, "ei");
    LLVMBuildRet(lg.builder, LLVMBuildBitCast(lg.builder, ei, lg.double_ty, "r"));
    return fn;
}

static LLVMValueRef build_baga_vec_set_f64(void) {
    LLVMTypeRef p[] = { baga_vec_ptr_ty(), lg.i64_ty, lg.double_ty };
    LLVMValueRef fn = LLVMAddFunction(lg.mod, "baga_vec_set_f64",
        LLVMFunctionType(lg.void_ty, p, 3, 0));
    h_begin(fn);
    LLVMValueRef v = LLVMGetParam(fn, 0);
    LLVMValueRef i = LLVMGetParam(fn, 1);
    LLVMValueRef data = vec_load_data(v);
    LLVMValueRef slot = LLVMBuildGEP2(lg.builder, lg.ptr_ty, data, &i, 1, "slot");
    LLVMValueRef xi = LLVMBuildBitCast(lg.builder, LLVMGetParam(fn, 2), lg.i64_ty, "xi");
    LLVMValueRef pv = LLVMBuildIntToPtr(lg.builder, xi, lg.ptr_ty, "pv");
    LLVMBuildStore(lg.builder, pv, slot);
    LLVMBuildRetVoid(lg.builder);
    return fn;
}

/* ---- bytes: baga_bytes = { i8* data, i64 len }, passed by value ---- */

/* store a by-value bytes param into a fresh alloca so its fields are GEP-able */
static LLVMValueRef bytes_param_alloca(LLVMValueRef v) {
    LLVMValueRef a = entry_alloca(baga_bytes_ty(), "ba");
    LLVMBuildStore(lg.builder, v, a);
    return a;
}
static LLVMValueRef bytes_load_data(LLVMValueRef a) {
    return LLVMBuildLoad2(lg.builder, lg.ptr_ty,
        LLVMBuildStructGEP2(lg.builder, baga_bytes_ty(), a, 0, "bd"), "bdata");
}
static LLVMValueRef bytes_load_len(LLVMValueRef a) {
    return LLVMBuildLoad2(lg.builder, lg.i64_ty,
        LLVMBuildStructGEP2(lg.builder, baga_bytes_ty(), a, 1, "bl"), "blen");
}
/* assemble a baga_bytes value from data ptr + len */
static LLVMValueRef bytes_make(LLVMValueRef data, LLVMValueRef len) {
    LLVMValueRef u = LLVMGetUndef(baga_bytes_ty());
    u = LLVMBuildInsertValue(lg.builder, u, data, 0, "b0");
    u = LLVMBuildInsertValue(lg.builder, u, len, 1, "b1");
    return u;
}
/* malloc(max(n,1)) — matches the C helper's non-empty allocation */
static LLVMValueRef bytes_malloc_n(LLVMValueRef n) {
    LLVMValueRef one = LLVMConstInt(lg.i64_ty, 1, 0);
    LLVMValueRef cmp = LLVMBuildICmp(lg.builder, LLVMIntSGT, n, one, "nn");
    LLVMValueRef sz = LLVMBuildSelect(lg.builder, cmp, n, one, "sz");
    LLVMValueRef ma[] = { sz };
    return h_call(rt_malloc(), ma, 1, "bdata");
}

static LLVMValueRef build_baga_bytes_len(void) {
    LLVMTypeRef p[] = { baga_bytes_ty() };
    LLVMValueRef fn = LLVMAddFunction(lg.mod, "baga_bytes_len",
        LLVMFunctionType(lg.i64_ty, p, 1, 0));
    h_begin(fn);
    LLVMValueRef a = bytes_param_alloca(LLVMGetParam(fn, 0));
    LLVMBuildRet(lg.builder, bytes_load_len(a));
    return fn;
}

static LLVMValueRef build_baga_bytes_at(void) {
    LLVMTypeRef p[] = { baga_bytes_ty(), lg.i64_ty };
    LLVMValueRef fn = LLVMAddFunction(lg.mod, "baga_bytes_at",
        LLVMFunctionType(lg.i64_ty, p, 2, 0));
    h_begin(fn);
    LLVMValueRef a = bytes_param_alloca(LLVMGetParam(fn, 0));
    LLVMValueRef i = LLVMGetParam(fn, 1);
    LLVMValueRef data = bytes_load_data(a);
    LLVMValueRef slot = LLVMBuildGEP2(lg.builder, lg.i8_ty, data, &i, 1, "bs");
    LLVMValueRef byte = LLVMBuildLoad2(lg.builder, lg.i8_ty, slot, "byte");
    LLVMBuildRet(lg.builder, LLVMBuildZExt(lg.builder, byte, lg.i64_ty, "bv"));
    return fn;
}

static LLVMValueRef build_baga_bytes_slice(void) {
    LLVMTypeRef p[] = { baga_bytes_ty(), lg.i64_ty, lg.i64_ty };
    LLVMValueRef fn = LLVMAddFunction(lg.mod, "baga_bytes_slice",
        LLVMFunctionType(baga_bytes_ty(), p, 3, 0));
    h_begin(fn);
    LLVMValueRef a = bytes_param_alloca(LLVMGetParam(fn, 0));
    LLVMValueRef lo = LLVMGetParam(fn, 1);
    LLVMValueRef hi = LLVMGetParam(fn, 2);
    LLVMValueRef blen = bytes_load_len(a);
    LLVMValueRef z = LLVMConstInt(lg.i64_ty, 0, 0);
    LLVMValueRef c1 = LLVMBuildICmp(lg.builder, LLVMIntSLT, lo, z, "lo0");
    lo = LLVMBuildSelect(lg.builder, c1, z, lo, "lo");
    LLVMValueRef c2 = LLVMBuildICmp(lg.builder, LLVMIntSGT, hi, blen, "hi0");
    hi = LLVMBuildSelect(lg.builder, c2, blen, hi, "hi");
    LLVMValueRef c3 = LLVMBuildICmp(lg.builder, LLVMIntSLT, hi, lo, "hc");
    hi = LLVMBuildSelect(lg.builder, c3, lo, hi, "hi");
    LLVMValueRef n = LLVMBuildSub(lg.builder, hi, lo, "sn");
    LLVMValueRef data = bytes_load_data(a);
    LLVMValueRef src = LLVMBuildGEP2(lg.builder, lg.i8_ty, data, &lo, 1, "src");
    LLVMValueRef dst = bytes_malloc_n(n);
    LLVMValueRef mc[] = { dst, src, n };
    h_call(rt_memcpy(), mc, 3, "");
    LLVMBuildRet(lg.builder, bytes_make(dst, n));
    return fn;
}

static LLVMValueRef build_baga_bytes_concat(void) {
    LLVMTypeRef p[] = { baga_bytes_ty(), baga_bytes_ty() };
    LLVMValueRef fn = LLVMAddFunction(lg.mod, "baga_bytes_concat",
        LLVMFunctionType(baga_bytes_ty(), p, 2, 0));
    h_begin(fn);
    LLVMValueRef a = bytes_param_alloca(LLVMGetParam(fn, 0));
    LLVMValueRef b = bytes_param_alloca(LLVMGetParam(fn, 1));
    LLVMValueRef al = bytes_load_len(a);
    LLVMValueRef bl = bytes_load_len(b);
    LLVMValueRef n = LLVMBuildAdd(lg.builder, al, bl, "cn");
    LLVMValueRef dst = bytes_malloc_n(n);
    LLVMValueRef ad = bytes_load_data(a);
    LLVMValueRef bd = bytes_load_data(b);
    LLVMValueRef m1[] = { dst, ad, al };
    h_call(rt_memcpy(), m1, 3, "");
    LLVMValueRef off = LLVMBuildGEP2(lg.builder, lg.i8_ty, dst, &al, 1, "off");
    LLVMValueRef m2[] = { off, bd, bl };
    h_call(rt_memcpy(), m2, 3, "");
    LLVMBuildRet(lg.builder, bytes_make(dst, n));
    return fn;
}

/* R51 parity (C backend): str <-> i64 unsafe handle casts — zero-copy
 * handoff through i64 chans. Safe because str memory is never freed. */
static LLVMValueRef build_baga_str_h(void) {
    LLVMTypeRef p[] = { lg.ptr_ty };
    LLVMValueRef fn = LLVMAddFunction(lg.mod, "baga_str_h",
        LLVMFunctionType(lg.i64_ty, p, 1, 0));
    h_begin(fn);
    LLVMBuildRet(lg.builder,
        LLVMBuildPtrToInt(lg.builder, LLVMGetParam(fn, 0), lg.i64_ty, "h"));
    return fn;
}
static LLVMValueRef build_baga_h_str(void) {
    LLVMTypeRef p[] = { lg.i64_ty };
    LLVMValueRef fn = LLVMAddFunction(lg.mod, "baga_h_str",
        LLVMFunctionType(lg.ptr_ty, p, 1, 0));
    h_begin(fn);
    LLVMBuildRet(lg.builder,
        LLVMBuildIntToPtr(lg.builder, LLVMGetParam(fn, 0), lg.ptr_ty, "s"));
    return fn;
}
/* R54 parity: dst[off..off+src.len) = src, bounds-checked no-op on overflow */
static LLVMValueRef build_baga_bytes_put(void) {
    LLVMTypeRef p[] = { baga_bytes_ty(), lg.i64_ty, baga_bytes_ty() };
    LLVMValueRef fn = LLVMAddFunction(lg.mod, "baga_bytes_put",
        LLVMFunctionType(lg.void_ty, p, 3, 0));
    h_begin(fn);
    LLVMValueRef d = bytes_param_alloca(LLVMGetParam(fn, 0));
    LLVMValueRef off = LLVMGetParam(fn, 1);
    LLVMValueRef s = bytes_param_alloca(LLVMGetParam(fn, 2));
    LLVMValueRef dl = bytes_load_len(d);
    LLVMValueRef sl = bytes_load_len(s);
    LLVMValueRef neg = LLVMBuildICmp(lg.builder, LLVMIntSLT, off,
        LLVMConstInt(lg.i64_ty, 0, 0), "neg");
    LLVMValueRef sum = LLVMBuildAdd(lg.builder, off, sl, "sum");
    LLVMValueRef over = LLVMBuildICmp(lg.builder, LLVMIntSGT, sum, dl, "over");
    LLVMValueRef cnd = LLVMBuildOr(lg.builder, neg, over, "cnd");
    LLVMBasicBlockRef bad = LLVMAppendBasicBlock(fn, "bad");
    LLVMBasicBlockRef okb = LLVMAppendBasicBlock(fn, "ok");
    LLVMBuildCondBr(lg.builder, cnd, bad, okb);
    LLVMPositionBuilderAtEnd(lg.builder, bad);
    LLVMBuildRetVoid(lg.builder);
    LLVMPositionBuilderAtEnd(lg.builder, okb);
    LLVMValueRef dd = bytes_load_data(d);
    LLVMValueRef sd = bytes_load_data(s);
    LLVMValueRef dst = LLVMBuildGEP2(lg.builder, lg.i8_ty, dd, &off, 1, "dst");
    LLVMValueRef m[] = { dst, sd, sl };
    h_call(rt_memcpy(), m, 3, "");
    LLVMBuildRetVoid(lg.builder);
    return fn;
}

/* R66 parity (C backend): bytes <-> i64 unsafe handle casts — zero-copy
 * hop through i64 chans. Boxes the {data,len} header with malloc; payload
 * data stays put. Leak-tolerant like the arena (handles live for the
 * process). */
static LLVMValueRef build_baga_bytes_h(void) {
    LLVMTypeRef p[] = { baga_bytes_ty() };
    LLVMValueRef fn = LLVMAddFunction(lg.mod, "baga_bytes_h",
        LLVMFunctionType(lg.i64_ty, p, 1, 0));
    h_begin(fn);
    LLVMValueRef a = bytes_param_alloca(LLVMGetParam(fn, 0));
    LLVMValueRef sz[] = { LLVMConstInt(lg.i64_ty, 16, 0) };
    LLVMValueRef box = h_call(rt_malloc(), sz, 1, "box");
    LLVMValueRef bp = LLVMBuildBitCast(lg.builder, box,
        LLVMPointerType(baga_bytes_ty(), 0), "bp");
    LLVMBuildStore(lg.builder, bytes_load_data(a),
        LLVMBuildStructGEP2(lg.builder, baga_bytes_ty(), bp, 0, "bd"));
    LLVMBuildStore(lg.builder, bytes_load_len(a),
        LLVMBuildStructGEP2(lg.builder, baga_bytes_ty(), bp, 1, "bl"));
    LLVMBuildRet(lg.builder,
        LLVMBuildPtrToInt(lg.builder, box, lg.i64_ty, "h"));
    return fn;
}
static LLVMValueRef build_baga_h_bytes(void) {
    LLVMTypeRef p[] = { lg.i64_ty };
    LLVMValueRef fn = LLVMAddFunction(lg.mod, "baga_h_bytes",
        LLVMFunctionType(baga_bytes_ty(), p, 1, 0));
    h_begin(fn);
    LLVMValueRef h = LLVMGetParam(fn, 0);
    LLVMValueRef isz = LLVMBuildICmp(lg.builder, LLVMIntEQ, h,
        LLVMConstInt(lg.i64_ty, 0, 0), "isz");
    LLVMBasicBlockRef zero_bb = LLVMAppendBasicBlockInContext(lg.ctx, fn, "zero");
    LLVMBasicBlockRef box_bb  = LLVMAppendBasicBlockInContext(lg.ctx, fn, "box");
    LLVMBuildCondBr(lg.builder, isz, zero_bb, box_bb);
    LLVMPositionBuilderAtEnd(lg.builder, zero_bb);
    LLVMBuildRet(lg.builder, bytes_make(LLVMConstNull(lg.ptr_ty),
        LLVMConstInt(lg.i64_ty, 0, 0)));
    LLVMPositionBuilderAtEnd(lg.builder, box_bb);
    LLVMValueRef bp = LLVMBuildIntToPtr(lg.builder, h,
        LLVMPointerType(baga_bytes_ty(), 0), "bp");
    LLVMBuildRet(lg.builder,
        LLVMBuildLoad2(lg.builder, baga_bytes_ty(), bp, "v"));
    return fn;
}

/* static void baga_bounds_fail(const char *fn, int64_t i, int64_t len)
 * — същото съобщение като C бекенда + exit(1) */
static LLVMValueRef build_baga_bounds_fail(void) {
    LLVMTypeRef p[] = { lg.ptr_ty, lg.i64_ty, lg.i64_ty };
    LLVMValueRef fn = LLVMAddFunction(lg.mod, "baga_bounds_fail",
        LLVMFunctionType(lg.void_ty, p, 3, 0));
    h_begin(fn);
    LLVMValueRef err = LLVMBuildLoad2(lg.builder, lg.ptr_ty, lg.stderr_global, "err");
    LLVMValueRef fmt = LLVMBuildGlobalStringPtr(lg.builder,
        "baga: %s: индекс %lld извън границите [0, %lld)\n", "bfmt");
    LLVMValueRef args[] = { err, fmt, LLVMGetParam(fn, 0),
                            LLVMGetParam(fn, 1), LLVMGetParam(fn, 2) };
    LLVMBuildCall2(lg.builder, LLVMGetElementType(LLVMTypeOf(lg.fprintf_fn)),
                   lg.fprintf_fn, args, 5, "");
    LLVMTypeRef ep[] = { lg.i32_ty };
    LLVMValueRef ea[] = { LLVMConstInt(lg.i32_ty, 1, 0) };
    h_call(rt_libc("exit", lg.void_ty, ep, 1), ea, 1, "");
    LLVMBuildUnreachable(lg.builder);
    return fn;
}

/* S2 parity: bytes_new/bytes_set/bytes_push — мутаторите от C бекенда.
 * bytes_set е bounds-checked и прекъсва със същото съобщение. */
static LLVMValueRef build_baga_bytes_new(void) {
    LLVMTypeRef p[] = { lg.i64_ty };
    LLVMValueRef fn = LLVMAddFunction(lg.mod, "baga_bytes_new",
        LLVMFunctionType(baga_bytes_ty(), p, 1, 0));
    h_begin(fn);
    LLVMValueRef n = LLVMGetParam(fn, 0);
    LLVMValueRef neg = LLVMBuildICmp(lg.builder, LLVMIntSLT, n,
        LLVMConstInt(lg.i64_ty, 0, 0), "neg");
    n = LLVMBuildSelect(lg.builder, neg, LLVMConstInt(lg.i64_ty, 0, 0), n, "nn");
    LLVMValueRef dst = bytes_malloc_n(n);
    LLVMTypeRef mt[] = { lg.ptr_ty, lg.i32_ty, lg.i64_ty };
    LLVMValueRef ms[] = { dst, LLVMConstInt(lg.i32_ty, 0, 0), n };
    h_call(rt_libc("memset", lg.ptr_ty, mt, 3), ms, 3, "");
    LLVMBuildRet(lg.builder, bytes_make(dst, n));
    return fn;
}
static LLVMValueRef build_baga_bytes_set(void) {
    LLVMTypeRef p[] = { baga_bytes_ty(), lg.i64_ty, lg.i64_ty };
    LLVMValueRef fn = LLVMAddFunction(lg.mod, "baga_bytes_set",
        LLVMFunctionType(lg.void_ty, p, 3, 0));
    h_begin(fn);
    LLVMValueRef a = bytes_param_alloca(LLVMGetParam(fn, 0));
    LLVMValueRef i = LLVMGetParam(fn, 1);
    LLVMValueRef len = bytes_load_len(a);
    LLVMValueRef lo = LLVMBuildICmp(lg.builder, LLVMIntSLT, i,
        LLVMConstInt(lg.i64_ty, 0, 0), "lo");
    LLVMValueRef hi = LLVMBuildICmp(lg.builder, LLVMIntSGE, i, len, "hi");
    LLVMValueRef bad = LLVMBuildOr(lg.builder, lo, hi, "bad");
    LLVMBasicBlockRef fail = LLVMAppendBasicBlockInContext(lg.ctx, fn, "fail");
    LLVMBasicBlockRef okb  = LLVMAppendBasicBlockInContext(lg.ctx, fn, "ok");
    LLVMBuildCondBr(lg.builder, bad, fail, okb);
    LLVMPositionBuilderAtEnd(lg.builder, fail);
    LLVMValueRef fname = LLVMBuildGlobalStringPtr(lg.builder, "bytes_set", "bfn");
    LLVMValueRef fa[] = { fname, i, len };
    h_call(baga_rt("baga_bounds_fail"), fa, 3, "");
    LLVMBuildUnreachable(lg.builder);
    LLVMPositionBuilderAtEnd(lg.builder, okb);
    LLVMValueRef dst = LLVMBuildGEP2(lg.builder, lg.i8_ty,
        bytes_load_data(a), &i, 1, "dst");
    LLVMValueRef v8 = LLVMBuildTrunc(lg.builder,
        LLVMBuildAnd(lg.builder, LLVMGetParam(fn, 2),
                     LLVMConstInt(lg.i64_ty, 255, 0), "m"),
        lg.i8_ty, "v8");
    LLVMBuildStore(lg.builder, v8, dst);
    LLVMBuildRetVoid(lg.builder);
    return fn;
}
static LLVMValueRef build_baga_bytes_push(void) {
    LLVMTypeRef p[] = { baga_bytes_ty(), lg.i64_ty };
    LLVMValueRef fn = LLVMAddFunction(lg.mod, "baga_bytes_push",
        LLVMFunctionType(baga_bytes_ty(), p, 2, 0));
    h_begin(fn);
    LLVMValueRef a = bytes_param_alloca(LLVMGetParam(fn, 0));
    LLVMValueRef len = bytes_load_len(a);
    LLVMValueRef nl = LLVMBuildAdd(lg.builder, len, LLVMConstInt(lg.i64_ty, 1, 0), "nl");
    LLVMValueRef dst = bytes_malloc_n(nl);
    LLVMValueRef mc[] = { dst, bytes_load_data(a), len };
    h_call(rt_memcpy(), mc, 3, "");
    LLVMValueRef slot = LLVMBuildGEP2(lg.builder, lg.i8_ty, dst, &len, 1, "slot");
    LLVMValueRef v8 = LLVMBuildTrunc(lg.builder,
        LLVMBuildAnd(lg.builder, LLVMGetParam(fn, 1),
                     LLVMConstInt(lg.i64_ty, 255, 0), "m"),
        lg.i8_ty, "v8");
    LLVMBuildStore(lg.builder, v8, slot);
    LLVMBuildRet(lg.builder, bytes_make(dst, nl));
    return fn;
}

static LLVMValueRef build_baga_bytes_from_str(void) {
    LLVMTypeRef p[] = { lg.ptr_ty };
    LLVMValueRef fn = LLVMAddFunction(lg.mod, "baga_bytes_from_str",
        LLVMFunctionType(baga_bytes_ty(), p, 1, 0));
    h_begin(fn);
    LLVMValueRef s = LLVMGetParam(fn, 0);
    LLVMValueRef la[] = { s };
    LLVMValueRef n = h_call(rt_strlen(), la, 1, "n");
    LLVMValueRef dst = bytes_malloc_n(n);
    LLVMValueRef mc[] = { dst, s, n };
    h_call(rt_memcpy(), mc, 3, "");
    LLVMBuildRet(lg.builder, bytes_make(dst, n));
    return fn;
}

static LLVMValueRef build_baga_bytes_to_str(void) {
    LLVMTypeRef p[] = { baga_bytes_ty() };
    LLVMValueRef fn = LLVMAddFunction(lg.mod, "baga_bytes_to_str",
        LLVMFunctionType(lg.ptr_ty, p, 1, 0));
    h_begin(fn);
    LLVMValueRef a = bytes_param_alloca(LLVMGetParam(fn, 0));
    LLVMValueRef n = bytes_load_len(a);
    LLVMValueRef one = LLVMConstInt(lg.i64_ty, 1, 0);
    LLVMValueRef sz = LLVMBuildAdd(lg.builder, n, one, "sz");
    LLVMValueRef ma[] = { sz };
    LLVMValueRef r = h_call(rt_malloc(), ma, 1, "r");
    LLVMValueRef data = bytes_load_data(a);
    LLVMValueRef mc[] = { r, data, n };
    h_call(rt_memcpy(), mc, 3, "");
    LLVMValueRef ep = LLVMBuildGEP2(lg.builder, lg.i8_ty, r, &n, 1, "ep");
    LLVMBuildStore(lg.builder, LLVMConstInt(lg.i8_ty, 0, 0), ep);
    LLVMBuildRet(lg.builder, r);
    return fn;
}

static LLVMValueRef build_baga_hex_val(void) {
    LLVMTypeRef p[] = { lg.i64_ty };
    LLVMValueRef fn = LLVMAddFunction(lg.mod, "baga_hex_val",
        LLVMFunctionType(lg.i64_ty, p, 1, 0));
    h_begin(fn);
    LLVMValueRef c = LLVMGetParam(fn, 0);
    LLVMValueRef neg1 = LLVMConstInt(lg.i64_ty, (uint64_t)-1, 1);
    LLVMValueRef r = entry_alloca(lg.i64_ty, "hv");
    LLVMBuildStore(lg.builder, neg1, r);
    LLVMValueRef c0 = LLVMConstInt(lg.i64_ty, 48, 0), c9 = LLVMConstInt(lg.i64_ty, 57, 0);
    LLVMValueRef ca = LLVMConstInt(lg.i64_ty, 97, 0), cf = LLVMConstInt(lg.i64_ty, 102, 0);
    LLVMValueRef cA = LLVMConstInt(lg.i64_ty, 65, 0), cF = LLVMConstInt(lg.i64_ty, 70, 0);
    LLVMValueRef ten = LLVMConstInt(lg.i64_ty, 10, 0);
    LLVMValueRef d1 = LLVMBuildSub(lg.builder, c, c0, "d1");
    LLVMValueRef in1a = LLVMBuildICmp(lg.builder, LLVMIntSGE, c, c0, "i1a");
    LLVMValueRef in1b = LLVMBuildICmp(lg.builder, LLVMIntSLE, c, c9, "i1b");
    LLVMValueRef in1 = LLVMBuildAnd(lg.builder, in1a, in1b, "i1");
    LLVMBasicBlockRef b1 = LLVMAppendBasicBlockInContext(lg.ctx, fn, "d");
    LLVMBasicBlockRef b2 = LLVMAppendBasicBlockInContext(lg.ctx, fn, "af");
    LLVMBuildCondBr(lg.builder, in1, b1, b2);
    LLVMPositionBuilderAtEnd(lg.builder, b1);
    LLVMBuildStore(lg.builder, d1, r);
    LLVMBuildRet(lg.builder, LLVMBuildLoad2(lg.builder, lg.i64_ty, r, "rv"));
    LLVMPositionBuilderAtEnd(lg.builder, b2);
    LLVMValueRef in2a = LLVMBuildICmp(lg.builder, LLVMIntSGE, c, ca, "i2a");
    LLVMValueRef in2b = LLVMBuildICmp(lg.builder, LLVMIntSLE, c, cf, "i2b");
    LLVMValueRef in2 = LLVMBuildAnd(lg.builder, in2a, in2b, "i2");
    LLVMBasicBlockRef b3 = LLVMAppendBasicBlockInContext(lg.ctx, fn, "lo");
    LLVMBasicBlockRef b4 = LLVMAppendBasicBlockInContext(lg.ctx, fn, "AF");
    LLVMBuildCondBr(lg.builder, in2, b3, b4);
    LLVMPositionBuilderAtEnd(lg.builder, b3);
    LLVMBuildStore(lg.builder, LLVMBuildAdd(lg.builder, LLVMBuildSub(lg.builder, c, ca, "d2"), ten, "v2"), r);
    LLVMBuildRet(lg.builder, LLVMBuildLoad2(lg.builder, lg.i64_ty, r, "rv2"));
    LLVMPositionBuilderAtEnd(lg.builder, b4);
    LLVMValueRef in3a = LLVMBuildICmp(lg.builder, LLVMIntSGE, c, cA, "i3a");
    LLVMValueRef in3b = LLVMBuildICmp(lg.builder, LLVMIntSLE, c, cF, "i3b");
    LLVMValueRef in3 = LLVMBuildAnd(lg.builder, in3a, in3b, "i3");
    LLVMBasicBlockRef b5 = LLVMAppendBasicBlockInContext(lg.ctx, fn, "up");
    LLVMBasicBlockRef b6 = LLVMAppendBasicBlockInContext(lg.ctx, fn, "no");
    LLVMBuildCondBr(lg.builder, in3, b5, b6);
    LLVMPositionBuilderAtEnd(lg.builder, b5);
    LLVMBuildStore(lg.builder, LLVMBuildAdd(lg.builder, LLVMBuildSub(lg.builder, c, cA, "d3"), ten, "v3"), r);
    LLVMBuildRet(lg.builder, LLVMBuildLoad2(lg.builder, lg.i64_ty, r, "rv3"));
    LLVMPositionBuilderAtEnd(lg.builder, b6);
    LLVMBuildRet(lg.builder, neg1);
    return fn;
}

static LLVMValueRef bytes_hex_global(void) {
    LLVMValueRef g = LLVMGetNamedGlobal(lg.mod, "baga_hexchars");
    if (g) return g;
    g = LLVMAddGlobal(lg.mod, LLVMArrayType(lg.i8_ty, 16), "baga_hexchars");
    LLVMSetInitializer(g, LLVMConstStringInContext(lg.ctx, "0123456789abcdef", 16, 1));
    LLVMSetGlobalConstant(g, 1);
    return g;
}

static LLVMValueRef build_baga_hex_encode(void) {
    LLVMTypeRef p[] = { baga_bytes_ty() };
    LLVMValueRef fn = LLVMAddFunction(lg.mod, "baga_hex_encode",
        LLVMFunctionType(lg.ptr_ty, p, 1, 0));
    h_begin(fn);
    LLVMValueRef a = bytes_param_alloca(LLVMGetParam(fn, 0));
    LLVMValueRef n = bytes_load_len(a);
    LLVMValueRef two = LLVMConstInt(lg.i64_ty, 2, 0);
    LLVMValueRef one = LLVMConstInt(lg.i64_ty, 1, 0);
    LLVMValueRef four = LLVMConstInt(lg.i64_ty, 4, 0);
    LLVMValueRef fifteen = LLVMConstInt(lg.i64_ty, 15, 0);
    LLVMValueRef sz = LLVMBuildAdd(lg.builder, LLVMBuildMul(lg.builder, n, two, "s0"), one, "sz");
    LLVMValueRef ma[] = { sz };
    LLVMValueRef r = h_call(rt_malloc(), ma, 1, "r");
    LLVMValueRef hxg = bytes_hex_global();
    LLVMValueRef zidx = LLVMConstInt(lg.i64_ty, 0, 0);
    LLVMValueRef hxarr = LLVMBuildGEP2(lg.builder, LLVMArrayType(lg.i8_ty, 16), hxg,
        (LLVMValueRef[]){ zidx, zidx }, 2, "hxa");
    LLVMValueRef hx = LLVMBuildBitCast(lg.builder, hxarr, lg.ptr_ty, "hx");
    LLVMValueRef data = bytes_load_data(a);
    LLVMBasicBlockRef cond = LLVMAppendBasicBlockInContext(lg.ctx, fn, "cond");
    LLVMBasicBlockRef body = LLVMAppendBasicBlockInContext(lg.ctx, fn, "body");
    LLVMBasicBlockRef done = LLVMAppendBasicBlockInContext(lg.ctx, fn, "done");
    LLVMValueRef iv = entry_alloca(lg.i64_ty, "i");
    LLVMBuildStore(lg.builder, LLVMConstInt(lg.i64_ty, 0, 0), iv);
    LLVMBuildBr(lg.builder, cond);
    LLVMPositionBuilderAtEnd(lg.builder, cond);
    LLVMValueRef i = LLVMBuildLoad2(lg.builder, lg.i64_ty, iv, "i");
    LLVMValueRef cc = LLVMBuildICmp(lg.builder, LLVMIntSLT, i, n, "cc");
    LLVMBuildCondBr(lg.builder, cc, body, done);
    LLVMPositionBuilderAtEnd(lg.builder, body);
    LLVMValueRef bp = LLVMBuildGEP2(lg.builder, lg.i8_ty, data, &i, 1, "bp");
    LLVMValueRef byte = LLVMBuildZExt(lg.builder, LLVMBuildLoad2(lg.builder, lg.i8_ty, bp, "b"), lg.i64_ty, "bv");
    LLVMValueRef hi = LLVMBuildLShr(lg.builder, byte, four, "hi");
    LLVMValueRef lo = LLVMBuildAnd(lg.builder, byte, fifteen, "lo");
    LLVMValueRef i2 = LLVMBuildMul(lg.builder, i, two, "i2");
    LLVMValueRef hch = LLVMBuildLoad2(lg.builder, lg.i8_ty, LLVMBuildGEP2(lg.builder, lg.i8_ty, hx, &hi, 1, "hp"), "hch");
    LLVMBuildStore(lg.builder, hch, LLVMBuildGEP2(lg.builder, lg.i8_ty, r, &i2, 1, "r0"));
    LLVMValueRef i21 = LLVMBuildAdd(lg.builder, i2, one, "i21");
    LLVMValueRef lch = LLVMBuildLoad2(lg.builder, lg.i8_ty, LLVMBuildGEP2(lg.builder, lg.i8_ty, hx, &lo, 1, "lp"), "lch");
    LLVMBuildStore(lg.builder, lch, LLVMBuildGEP2(lg.builder, lg.i8_ty, r, &i21, 1, "r1"));
    LLVMBuildStore(lg.builder, LLVMBuildAdd(lg.builder, i, one, "in"), iv);
    LLVMBuildBr(lg.builder, cond);
    LLVMPositionBuilderAtEnd(lg.builder, done);
    LLVMValueRef endi = LLVMBuildMul(lg.builder, n, two, "endi");
    LLVMBuildStore(lg.builder, LLVMConstInt(lg.i8_ty, 0, 0), LLVMBuildGEP2(lg.builder, lg.i8_ty, r, &endi, 1, "rp"));
    LLVMBuildRet(lg.builder, r);
    return fn;
}

static LLVMValueRef build_baga_hex_decode(void) {
    LLVMTypeRef p[] = { lg.ptr_ty };
    LLVMValueRef fn = LLVMAddFunction(lg.mod, "baga_hex_decode",
        LLVMFunctionType(baga_bytes_ty(), p, 1, 0));
    h_begin(fn);
    LLVMValueRef s = LLVMGetParam(fn, 0);
    LLVMValueRef one = LLVMConstInt(lg.i64_ty, 1, 0);
    LLVMValueRef two = LLVMConstInt(lg.i64_ty, 2, 0);
    LLVMValueRef sixteen = LLVMConstInt(lg.i64_ty, 16, 0);
    LLVMValueRef zero = LLVMConstInt(lg.i64_ty, 0, 0);
    LLVMValueRef neg1 = LLVMConstInt(lg.i64_ty, (uint64_t)-1, 1);
    LLVMValueRef sla[] = { s };
    LLVMValueRef n = h_call(rt_strlen(), sla, 1, "n");
    LLVMValueRef cap = LLVMBuildAdd(lg.builder, LLVMBuildUDiv(lg.builder, n, two, "c0"), one, "cap");
    LLVMValueRef ma[] = { cap };
    LLVMValueRef buf = h_call(rt_malloc(), ma, 1, "buf");
    LLVMValueRef len = entry_alloca(lg.i64_ty, "len");
    LLVMValueRef iv = entry_alloca(lg.i64_ty, "i");
    LLVMBuildStore(lg.builder, zero, len);
    LLVMBuildStore(lg.builder, zero, iv);
    LLVMBasicBlockRef cond = LLVMAppendBasicBlockInContext(lg.ctx, fn, "cond");
    LLVMBasicBlockRef body = LLVMAppendBasicBlockInContext(lg.ctx, fn, "body");
    LLVMBasicBlockRef done = LLVMAppendBasicBlockInContext(lg.ctx, fn, "done");
    LLVMBuildBr(lg.builder, cond);
    LLVMPositionBuilderAtEnd(lg.builder, cond);
    LLVMValueRef i = LLVMBuildLoad2(lg.builder, lg.i64_ty, iv, "i");
    LLVMValueRef i1 = LLVMBuildAdd(lg.builder, i, one, "i1");
    LLVMValueRef cc = LLVMBuildICmp(lg.builder, LLVMIntSLT, i1, n, "cc");
    LLVMBuildCondBr(lg.builder, cc, body, done);
    LLVMPositionBuilderAtEnd(lg.builder, body);
    LLVMValueRef cp0 = LLVMBuildGEP2(lg.builder, lg.i8_ty, s, &i, 1, "cp0");
    LLVMValueRef c0 = LLVMBuildZExt(lg.builder, LLVMBuildLoad2(lg.builder, lg.i8_ty, cp0, "c0v"), lg.i64_ty, "c0");
    LLVMValueRef cp1 = LLVMBuildGEP2(lg.builder, lg.i8_ty, s, &i1, 1, "cp1");
    LLVMValueRef c1 = LLVMBuildZExt(lg.builder, LLVMBuildLoad2(lg.builder, lg.i8_ty, cp1, "c1v"), lg.i64_ty, "c1");
    LLVMValueRef h0 = h_call(baga_rt("baga_hex_val"), &c0, 1, "h0");
    LLVMValueRef h1 = h_call(baga_rt("baga_hex_val"), &c1, 1, "h1");
    LLVMValueRef bad0 = LLVMBuildICmp(lg.builder, LLVMIntSLT, h0, zero, "bad0");
    LLVMValueRef bad1 = LLVMBuildICmp(lg.builder, LLVMIntSLT, h1, zero, "bad1");
    LLVMValueRef bad = LLVMBuildOr(lg.builder, bad0, bad1, "bad");
    LLVMBasicBlockRef skip = LLVMAppendBasicBlockInContext(lg.ctx, fn, "skip");
    LLVMBasicBlockRef take = LLVMAppendBasicBlockInContext(lg.ctx, fn, "take");
    LLVMBuildCondBr(lg.builder, bad, skip, take);
    LLVMPositionBuilderAtEnd(lg.builder, skip);
    LLVMBuildStore(lg.builder, LLVMBuildAdd(lg.builder, i, one, "si"), iv);
    LLVMBuildBr(lg.builder, cond);
    LLVMPositionBuilderAtEnd(lg.builder, take);
    LLVMValueRef byte = LLVMBuildAdd(lg.builder, LLVMBuildMul(lg.builder, h0, sixteen, "m"), h1, "byte");
    LLVMValueRef l = LLVMBuildLoad2(lg.builder, lg.i64_ty, len, "l");
    LLVMValueRef b8 = LLVMBuildTrunc(lg.builder, byte, lg.i8_ty, "b8");
    LLVMBuildStore(lg.builder, b8, LLVMBuildGEP2(lg.builder, lg.i8_ty, buf, &l, 1, "sl"));
    LLVMBuildStore(lg.builder, LLVMBuildAdd(lg.builder, l, one, "l1"), len);
    LLVMBuildStore(lg.builder, LLVMBuildAdd(lg.builder, i, two, "ti"), iv);
    LLVMBuildBr(lg.builder, cond);
    LLVMPositionBuilderAtEnd(lg.builder, done);
    LLVMValueRef fl = LLVMBuildLoad2(lg.builder, lg.i64_ty, len, "fl");
    LLVMBuildRet(lg.builder, bytes_make(buf, fl));
    (void)neg1;
    return fn;
}

static LLVMValueRef build_baga_bytes_from_hex(void) {
    LLVMTypeRef p[] = { lg.ptr_ty };
    LLVMValueRef fn = LLVMAddFunction(lg.mod, "baga_bytes_from_hex",
        LLVMFunctionType(baga_bytes_ty(), p, 1, 0));
    h_begin(fn);
    LLVMValueRef s = LLVMGetParam(fn, 0);
    LLVMValueRef r = h_call(baga_rt("baga_hex_decode"), &s, 1, "r");
    LLVMBuildRet(lg.builder, r);
    return fn;
}

/* static const char *baga_read_file(const char *path) {
 *     FILE *f = fopen(path, "rb"); if (!f) return "";
 *     fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
 *     char *buf = malloc(sz + 1); fread(buf, 1, sz, f);
 *     buf[sz] = 0; fclose(f); return buf; } */
static LLVMValueRef build_baga_read_file(void) {
    LLVMTypeRef p[] = { lg.ptr_ty };
    LLVMValueRef fn = LLVMAddFunction(lg.mod, "baga_read_file",
        LLVMFunctionType(lg.ptr_ty, p, 1, 0));
    LLVMTypeRef fp[] = { lg.ptr_ty, lg.ptr_ty };
    LLVMValueRef fopen_fn = rt_libc("fopen", lg.ptr_ty, fp, 2);
    LLVMTypeRef sp[] = { lg.ptr_ty, lg.i64_ty, lg.i32_ty };
    LLVMValueRef fseek_fn = rt_libc("fseek", lg.i32_ty, sp, 3);
    LLVMTypeRef tp[] = { lg.ptr_ty };
    LLVMValueRef ftell_fn = rt_libc("ftell", lg.i64_ty, tp, 1);
    LLVMTypeRef rp[] = { lg.ptr_ty, lg.i64_ty, lg.i64_ty, lg.ptr_ty };
    LLVMValueRef fread_fn = rt_libc("fread", lg.i64_ty, rp, 4);
    LLVMTypeRef cp[] = { lg.ptr_ty };
    LLVMValueRef fclose_fn = rt_libc("fclose", lg.i32_ty, cp, 1);

    h_begin(fn);
    LLVMValueRef rb = LLVMBuildGlobalStringPtr(lg.builder, "rb", "rb");
    LLVMValueRef fa[] = { LLVMGetParam(fn, 0), rb };
    LLVMValueRef f = h_call(fopen_fn, fa, 2, "f");
    LLVMValueRef isnull = LLVMBuildICmp(lg.builder, LLVMIntEQ, f,
        LLVMConstNull(lg.ptr_ty), "isnull");
    LLVMBasicBlockRef empty_bb = LLVMAppendBasicBlockInContext(lg.ctx, fn, "empty");
    LLVMBasicBlockRef body_bb  = LLVMAppendBasicBlockInContext(lg.ctx, fn, "body");
    LLVMBuildCondBr(lg.builder, isnull, empty_bb, body_bb);

    LLVMPositionBuilderAtEnd(lg.builder, empty_bb);
    LLVMValueRef es = LLVMBuildGlobalStringPtr(lg.builder, "", "empty");
    LLVMBuildRet(lg.builder, es);

    LLVMPositionBuilderAtEnd(lg.builder, body_bb);
    LLVMValueRef se_end[] = { f, LLVMConstInt(lg.i64_ty, 0, 0),
        LLVMConstInt(lg.i32_ty, 2, 0) }; /* SEEK_END */
    h_call(fseek_fn, se_end, 3, "");
    LLVMValueRef ta[] = { f };
    LLVMValueRef sz = h_call(ftell_fn, ta, 1, "sz");
    LLVMValueRef se_set[] = { f, LLVMConstInt(lg.i64_ty, 0, 0),
        LLVMConstInt(lg.i32_ty, 0, 0) }; /* SEEK_SET */
    h_call(fseek_fn, se_set, 3, "");
    LLVMValueRef sz1 = LLVMBuildAdd(lg.builder, sz,
        LLVMConstInt(lg.i64_ty, 1, 0), "sz1");
    LLVMValueRef ma[] = { sz1 };
    LLVMValueRef buf = h_call(rt_malloc(), ma, 1, "buf");
    LLVMValueRef ra[] = { buf, LLVMConstInt(lg.i64_ty, 1, 0), sz, f };
    h_call(fread_fn, ra, 4, "rd");
    LLVMValueRef end = LLVMBuildGEP2(lg.builder, lg.i8_ty, buf, &sz, 1, "end");
    LLVMBuildStore(lg.builder, LLVMConstInt(lg.i8_ty, 0, 0), end);
    LLVMValueRef ca[] = { f };
    h_call(fclose_fn, ca, 1, "");
    LLVMBuildRet(lg.builder, buf);
    return fn;
}

/* ---- Потребителски структури ----
 *
 * Named LLVM struct типове (mangled име, като в codegen_c), създадени
 * в първия проход на codegen_llvm. Стойността е първокласен struct
 * (by value), както C struct в codegen_c. */

static LLVMTypeRef user_struct_ty(const char *name) {
    char *m = llvm_mangle(name);
    LLVMTypeRef t = LLVMGetTypeByName(lg.mod, m);
    free(m);
    if (!t) {
        char buf[256];
        snprintf(buf, sizeof buf, "неизвестна структура '%s'", name);
        llvm_unsupported(buf);
    }
    return t;
}

static Node *find_struct_decl(const char *name) {
    if (!lg.program) return NULL;
    for (int i = 0; i < lg.program->items.len; i++) {
        Node *item = lg.program->items.data[i];
        if (item->kind == NODE_STRUCT && strcmp(item->struct_name, name) == 0)
            return item;
    }
    return NULL;
}

static int struct_field_index(Node *decl, const char *fname) {
    for (int i = 0; i < decl->fields.len; i++)
        if (strcmp(decl->fields.data[i]->fld_name, fname) == 0)
            return i;
    return -1;
}

/* ---- L3: sum enum-и ----
 *
 * Named struct b_<E> = { i64 tag, [N x i64] u }, където
 * N = max(1, ceil(max payload ABI размер / 8)) — LLVM няма union, затова
 * union областта е i64 масив, а payload store/load е GEP до u + bitcast
 * към указател към payload типа. Тагът е индексът на варианта в
 * декларацията (0..n-1) — същият ред като в codegen_c. */

/* тяло на sum enum типа (изисква payload типовете да са sized) */
static void sum_enum_set_body(Node *ed, LLVMTypeRef st) {
    unsigned long long maxsz = 0;
    for (int j = 0; j < ed->n_variants; j++) {
        if (!ed->enum_payloads || !ed->enum_payloads[j]) continue;
        unsigned long long sz =
            LLVMABISizeOfType(lg.td, llvm_type(ed->enum_payloads[j]));
        if (sz > maxsz) maxsz = sz;
    }
    unsigned n = (unsigned)(maxsz / 8) + (maxsz % 8 ? 1 : 0);
    if (n == 0) n = 1;
    LLVMTypeRef elems[] = { lg.i64_ty, LLVMArrayType(lg.i64_ty, n) };
    LLVMStructSetBody(st, elems, 2, 0);
}

/* GEP до union областта + bitcast към указател към payload типа */
static LLVMValueRef sum_payload_ptr(LLVMTypeRef ety, LLVMValueRef ea,
                                    LLVMTypeRef pty) {
    LLVMValueRef up = LLVMBuildStructGEP2(lg.builder, ety, ea, 1, "up");
    return LLVMBuildBitCast(lg.builder, up, LLVMPointerType(pty, 0), "pp");
}

/* lazy IR конструктор b_<E>__b_<V>(payload) — огледало на static inline
 * конструкторите в codegen_c; тялото се генерира при първа употреба */
static LLVMValueRef sum_ctor_fn(Node *ed, int vidx) {
    char *em = llvm_mangle(ed->enum_name);
    char *vm = llvm_mangle(ed->enum_variants[vidx]);
    char full[600];
    snprintf(full, sizeof full, "%s__%s", em, vm);
    free(em); free(vm);
    LLVMValueRef fn = LLVMGetNamedFunction(lg.mod, full);
    if (fn) return fn;
    LLVMTypeRef ety = user_struct_ty(ed->enum_name);
    LLVMTypeRef pty = llvm_type(ed->enum_payloads[vidx]);
    LLVMBasicBlockRef saved = LLVMGetInsertBlock(lg.builder);
    fn = LLVMAddFunction(lg.mod, full, LLVMFunctionType(ety, &pty, 1, 0));
    h_begin(fn);
    LLVMValueRef r = LLVMBuildAlloca(lg.builder, ety, "r");
    LLVMValueRef tagp = LLVMBuildStructGEP2(lg.builder, ety, r, 0, "tagp");
    LLVMBuildStore(lg.builder,
        LLVMConstInt(lg.i64_ty, (unsigned long long)vidx, 0), tagp);
    LLVMBuildStore(lg.builder, LLVMGetParam(fn, 0),
                   sum_payload_ptr(ety, r, pty));
    LLVMBuildRet(lg.builder, LLVMBuildLoad2(lg.builder, ety, r, "rv"));
    if (saved) LLVMPositionBuilderAtEnd(lg.builder, saved);
    return fn;
}

/* payload-less вариант на sum enum като стойност: { tag = j, u = 0 } */
static LLVMValueRef sum_variant_const(Node *ed, int vidx) {
    LLVMTypeRef ety = user_struct_ty(ed->enum_name);
    LLVMValueRef tmp = entry_alloca(ety, "ev");
    LLVMBuildStore(lg.builder, LLVMConstNull(ety), tmp);
    LLVMValueRef tagp = LLVMBuildStructGEP2(lg.builder, ety, tmp, 0, "tagp");
    LLVMBuildStore(lg.builder,
        LLVMConstInt(lg.i64_ty, (unsigned long long)vidx, 0), tagp);
    char *nm = tmp_name();
    LLVMValueRef r = LLVMBuildLoad2(lg.builder, ety, tmp, nm);
    free(nm);
    return r;
}

/* ---- Програмни аргументи (argv) ----
 * main(argc, argv) записва стойностите в IR глобални; baga_arg_count/
 * baga_arg ги четат. baga_arg е с bounds check — извън границите връща "". */
static LLVMValueRef argv_argc_global(void) {
    LLVMValueRef g = LLVMGetNamedGlobal(lg.mod, "baga_argc");
    if (g) return g;
    g = LLVMAddGlobal(lg.mod, lg.i32_ty, "baga_argc");
    LLVMSetInitializer(g, LLVMConstInt(lg.i32_ty, 0, 0));
    return g;
}
static LLVMValueRef argv_argv_global(void) {
    LLVMValueRef g = LLVMGetNamedGlobal(lg.mod, "baga_argv");
    if (g) return g;
    LLVMTypeRef pp = LLVMPointerType(lg.ptr_ty, 0);   /* char **argv */
    g = LLVMAddGlobal(lg.mod, pp, "baga_argv");
    LLVMSetInitializer(g, LLVMConstNull(pp));
    return g;
}

/* static int64_t baga_arg_count(void) { return baga_argc > 0 ? baga_argc - 1 : 0; } */
static LLVMValueRef build_baga_arg_count(void) {
    LLVMValueRef fn = LLVMAddFunction(lg.mod, "baga_arg_count",
        LLVMFunctionType(lg.i64_ty, NULL, 0, 0));
    h_begin(fn);
    LLVMValueRef ac = LLVMBuildLoad2(lg.builder, lg.i32_ty, argv_argc_global(), "ac");
    LLVMValueRef gt = LLVMBuildICmp(lg.builder, LLVMIntSGT, ac,
        LLVMConstInt(lg.i32_ty, 0, 0), "gt");
    LLVMValueRef sub = LLVMBuildSub(lg.builder, ac,
        LLVMConstInt(lg.i32_ty, 1, 0), "sub");
    LLVMValueRef n = LLVMBuildSelect(lg.builder, gt, sub,
        LLVMConstInt(lg.i32_ty, 0, 0), "n");
    LLVMBuildRet(lg.builder, LLVMBuildSExt(lg.builder, n, lg.i64_ty, "r"));
    return fn;
}

/* static const char *baga_arg(int64_t i)
 * { return (i + 1 < baga_argc) ? baga_argv[i + 1] : ""; } */
static LLVMValueRef build_baga_arg(void) {
    LLVMTypeRef p[] = { lg.i64_ty };
    LLVMValueRef fn = LLVMAddFunction(lg.mod, "baga_arg",
        LLVMFunctionType(lg.ptr_ty, p, 1, 0));
    h_begin(fn);
    LLVMValueRef idx = LLVMBuildAdd(lg.builder, LLVMGetParam(fn, 0),
        LLVMConstInt(lg.i64_ty, 1, 0), "idx");
    LLVMValueRef ac = LLVMBuildLoad2(lg.builder, lg.i32_ty, argv_argc_global(), "ac");
    LLVMValueRef ac64 = LLVMBuildSExt(lg.builder, ac, lg.i64_ty, "ac64");
    LLVMValueRef ok = LLVMBuildICmp(lg.builder, LLVMIntSLT, idx, ac64, "ok");
    LLVMBasicBlockRef in_bb  = LLVMAppendBasicBlockInContext(lg.ctx, fn, "inb");
    LLVMBasicBlockRef out_bb = LLVMAppendBasicBlockInContext(lg.ctx, fn, "outb");
    LLVMBuildCondBr(lg.builder, ok, in_bb, out_bb);

    LLVMPositionBuilderAtEnd(lg.builder, in_bb);
    LLVMValueRef av = LLVMBuildLoad2(lg.builder, LLVMPointerType(lg.ptr_ty, 0),
        argv_argv_global(), "av");
    LLVMValueRef slot = LLVMBuildGEP2(lg.builder, lg.ptr_ty, av, &idx, 1, "slot");
    LLVMBuildRet(lg.builder, LLVMBuildLoad2(lg.builder, lg.ptr_ty, slot, "s"));

    LLVMPositionBuilderAtEnd(lg.builder, out_bb);
    LLVMBuildRet(lg.builder, LLVMBuildGlobalStringPtr(lg.builder, "", "empty"));
    return fn;
}

/* static void baga_exit(int64_t c) { exit((int)c); } */
static LLVMValueRef build_baga_exit(void) {
    LLVMTypeRef p[] = { lg.i64_ty };
    LLVMValueRef fn = LLVMAddFunction(lg.mod, "baga_exit",
        LLVMFunctionType(lg.void_ty, p, 1, 0));
    h_begin(fn);
    LLVMValueRef c32 = LLVMBuildTrunc(lg.builder, LLVMGetParam(fn, 0), lg.i32_ty, "c");
    LLVMValueRef args[] = { c32 };
    LLVMBuildCall2(lg.builder, LLVMGetElementType(LLVMTypeOf(lg.exit_fn)),
                   lg.exit_fn, args, 1, "");
    LLVMBuildUnreachable(lg.builder);
    return fn;
}

/* static void baga_eprintln(const char *s) { fprintf(stderr, "%s\n", s); } */
static LLVMValueRef build_baga_eprintln(void) {
    LLVMTypeRef p[] = { lg.ptr_ty };
    LLVMValueRef fn = LLVMAddFunction(lg.mod, "baga_eprintln",
        LLVMFunctionType(lg.void_ty, p, 1, 0));
    h_begin(fn);
    LLVMValueRef fmt = LLVMBuildGlobalStringPtr(lg.builder, "%s\n", "fmt");
    LLVMValueRef err = LLVMBuildLoad2(lg.builder, lg.ptr_ty, lg.stderr_global, "err");
    LLVMValueRef args[] = { err, fmt, LLVMGetParam(fn, 0) };
    LLVMBuildCall2(lg.builder, LLVMGetElementType(LLVMTypeOf(lg.fprintf_fn)),
                   lg.fprintf_fn, args, 3, "");
    LLVMBuildRetVoid(lg.builder);
    return fn;
}

/* ---- Map: baga_Map = { baga_MapEntry **b, i64 nb, i64 len } ----
 *
 * Огледало на C preamble-а (codegen_c): вериги от baga_MapEntry,
 * FNV-1a хеш за str/bytes ключове и splitmix-подобен микс за i64;
 * rehash с удвояване при load > 3/4. ktag: 0=i64, 1=str, 2=bytes.
 * Стойности: iv/fv/sv/bv inline; pv е box (malloc копие) за struct/enum.
 * baga_MapEntry = { i64 ik, i8* sk, baga_bytes bk, i64 ktag,
 *                   i64 iv, double fv, i8* sv, baga_bytes bv, i8* pv,
 *                   baga_MapEntry *next } — полета 0..9. */

static LLVMTypeRef baga_map_entry_ty(void) {
    LLVMTypeRef t = LLVMGetTypeByName(lg.mod, "baga_MapEntry");
    if (t) return t;
    t = LLVMStructCreateNamed(lg.ctx, "baga_MapEntry");
    LLVMTypeRef elems[] = {
        lg.i64_ty, lg.ptr_ty, baga_bytes_ty(), lg.i64_ty,
        lg.i64_ty, lg.double_ty, lg.ptr_ty, baga_bytes_ty(), lg.ptr_ty,
        LLVMPointerType(t, 0),
    };
    LLVMStructSetBody(t, elems, 10, 0);
    return t;
}
static LLVMTypeRef baga_map_entry_ptr_ty(void) {
    return LLVMPointerType(baga_map_entry_ty(), 0);
}
static LLVMTypeRef baga_map_entry_slot_ty(void) {
    return LLVMPointerType(baga_map_entry_ptr_ty(), 0);
}
static LLVMTypeRef baga_map_ty(void) {
    LLVMTypeRef t = LLVMGetTypeByName(lg.mod, "baga_Map");
    if (t) return t;
    t = LLVMStructCreateNamed(lg.ctx, "baga_Map");
    LLVMTypeRef elems[] = {
        baga_map_entry_slot_ty(), lg.i64_ty, lg.i64_ty,
    };
    LLVMStructSetBody(t, elems, 3, 0);
    return t;
}
static LLVMTypeRef baga_map_ptr_ty(void) {
    return LLVMPointerType(baga_map_ty(), 0);
}

static LLVMValueRef map_fld(LLVMValueRef m, unsigned idx, const char *nm) {
    return LLVMBuildStructGEP2(lg.builder, baga_map_ty(), m, idx, nm);
}
static LLVMValueRef ent_fld(LLVMValueRef e, unsigned idx, const char *nm) {
    return LLVMBuildStructGEP2(lg.builder, baga_map_entry_ty(), e, idx, nm);
}
static LLVMValueRef map_load_nb(LLVMValueRef m) {
    return LLVMBuildLoad2(lg.builder, lg.i64_ty, map_fld(m, 1, "nbp"), "nb");
}
static LLVMValueRef map_load_b(LLVMValueRef m) {
    return LLVMBuildLoad2(lg.builder, baga_map_entry_slot_ty(),
                          map_fld(m, 0, "bp"), "b");
}

static LLVMValueRef rt_memcmp(void) {
    LLVMTypeRef p[] = { lg.ptr_ty, lg.ptr_ty, lg.i64_ty };
    return rt_libc("memcmp", lg.i32_ty, p, 3);
}

/* static uint64_t baga_map_hash_str(const char *s) — FNV-1a до NUL */
static LLVMValueRef build_baga_map_hash_str(void) {
    LLVMTypeRef p[] = { lg.ptr_ty };
    LLVMValueRef fn = LLVMAddFunction(lg.mod, "baga_map_hash_str",
        LLVMFunctionType(lg.i64_ty, p, 1, 0));
    h_begin(fn);
    LLVMValueRef h = entry_alloca(lg.i64_ty, "h");
    LLVMBuildStore(lg.builder,
        LLVMConstInt(lg.i64_ty, 1469598103934665603ULL, 0), h);
    LLVMValueRef sp = entry_alloca(lg.ptr_ty, "sp");
    LLVMBuildStore(lg.builder, LLVMGetParam(fn, 0), sp);
    LLVMBasicBlockRef cond = LLVMAppendBasicBlockInContext(lg.ctx, fn, "cond");
    LLVMBasicBlockRef body = LLVMAppendBasicBlockInContext(lg.ctx, fn, "body");
    LLVMBasicBlockRef done = LLVMAppendBasicBlockInContext(lg.ctx, fn, "done");
    LLVMBuildBr(lg.builder, cond);
    LLVMPositionBuilderAtEnd(lg.builder, cond);
    LLVMValueRef sv = LLVMBuildLoad2(lg.builder, lg.ptr_ty, sp, "sv");
    LLVMValueRef c = LLVMBuildLoad2(lg.builder, lg.i8_ty, sv, "c");
    LLVMValueRef is0 = LLVMBuildICmp(lg.builder, LLVMIntEQ, c,
        LLVMConstInt(lg.i8_ty, 0, 0), "is0");
    LLVMBuildCondBr(lg.builder, is0, done, body);
    LLVMPositionBuilderAtEnd(lg.builder, body);
    LLVMValueRef hv = LLVMBuildLoad2(lg.builder, lg.i64_ty, h, "hv");
    LLVMValueRef x = LLVMBuildXor(lg.builder, hv,
        LLVMBuildZExt(lg.builder, c, lg.i64_ty, "cz"), "x");
    LLVMBuildStore(lg.builder, LLVMBuildMul(lg.builder, x,
        LLVMConstInt(lg.i64_ty, 1099511628211ULL, 0), "hm"), h);
    LLVMValueRef one = LLVMConstInt(lg.i64_ty, 1, 0);
    LLVMBuildStore(lg.builder,
        LLVMBuildGEP2(lg.builder, lg.i8_ty, sv, &one, 1, "sn"), sp);
    LLVMBuildBr(lg.builder, cond);
    LLVMPositionBuilderAtEnd(lg.builder, done);
    LLVMBuildRet(lg.builder, LLVMBuildLoad2(lg.builder, lg.i64_ty, h, "r"));
    return fn;
}

/* static uint64_t baga_map_hash_i64(int64_t k) — splitmix-подобен микс */
static LLVMValueRef build_baga_map_hash_i64(void) {
    LLVMTypeRef p[] = { lg.i64_ty };
    LLVMValueRef fn = LLVMAddFunction(lg.mod, "baga_map_hash_i64",
        LLVMFunctionType(lg.i64_ty, p, 1, 0));
    h_begin(fn);
    LLVMValueRef c33 = LLVMConstInt(lg.i64_ty, 33, 0);
    LLVMValueRef x = LLVMGetParam(fn, 0);
    x = LLVMBuildXor(lg.builder, x, LLVMBuildLShr(lg.builder, x, c33, "s1"), "x1");
    x = LLVMBuildMul(lg.builder, x,
        LLVMConstInt(lg.i64_ty, 0xff51afd7ed558ccdULL, 0), "m1");
    x = LLVMBuildXor(lg.builder, x, LLVMBuildLShr(lg.builder, x, c33, "s2"), "x2");
    x = LLVMBuildMul(lg.builder, x,
        LLVMConstInt(lg.i64_ty, 0xc4ceb9fe1a85ec53ULL, 0), "m2");
    x = LLVMBuildXor(lg.builder, x, LLVMBuildLShr(lg.builder, x, c33, "s3"), "x3");
    LLVMBuildRet(lg.builder, x);
    return fn;
}

/* static uint64_t baga_map_hash_bytes(baga_bytes k)
 * — FNV-1a върху data+len (R67, NUL-safe) */
static LLVMValueRef build_baga_map_hash_bytes(void) {
    LLVMTypeRef p[] = { baga_bytes_ty() };
    LLVMValueRef fn = LLVMAddFunction(lg.mod, "baga_map_hash_bytes",
        LLVMFunctionType(lg.i64_ty, p, 1, 0));
    h_begin(fn);
    LLVMValueRef a = bytes_param_alloca(LLVMGetParam(fn, 0));
    LLVMValueRef data = bytes_load_data(a);
    LLVMValueRef len = bytes_load_len(a);
    LLVMValueRef h = entry_alloca(lg.i64_ty, "h");
    LLVMBuildStore(lg.builder,
        LLVMConstInt(lg.i64_ty, 1469598103934665603ULL, 0), h);
    LLVMValueRef iv = entry_alloca(lg.i64_ty, "i");
    LLVMBuildStore(lg.builder, LLVMConstInt(lg.i64_ty, 0, 0), iv);
    LLVMBasicBlockRef cond = LLVMAppendBasicBlockInContext(lg.ctx, fn, "cond");
    LLVMBasicBlockRef body = LLVMAppendBasicBlockInContext(lg.ctx, fn, "body");
    LLVMBasicBlockRef done = LLVMAppendBasicBlockInContext(lg.ctx, fn, "done");
    LLVMBuildBr(lg.builder, cond);
    LLVMPositionBuilderAtEnd(lg.builder, cond);
    LLVMValueRef i = LLVMBuildLoad2(lg.builder, lg.i64_ty, iv, "i");
    LLVMValueRef cc = LLVMBuildICmp(lg.builder, LLVMIntSLT, i, len, "cc");
    LLVMBuildCondBr(lg.builder, cc, body, done);
    LLVMPositionBuilderAtEnd(lg.builder, body);
    LLVMValueRef cb = LLVMBuildLoad2(lg.builder, lg.i8_ty,
        LLVMBuildGEP2(lg.builder, lg.i8_ty, data, &i, 1, "dp"), "cb");
    LLVMValueRef hv = LLVMBuildLoad2(lg.builder, lg.i64_ty, h, "hv");
    LLVMValueRef x = LLVMBuildXor(lg.builder, hv,
        LLVMBuildZExt(lg.builder, cb, lg.i64_ty, "cz"), "x");
    LLVMBuildStore(lg.builder, LLVMBuildMul(lg.builder, x,
        LLVMConstInt(lg.i64_ty, 1099511628211ULL, 0), "hm"), h);
    LLVMBuildStore(lg.builder, LLVMBuildAdd(lg.builder, i,
        LLVMConstInt(lg.i64_ty, 1, 0), "in"), iv);
    LLVMBuildBr(lg.builder, cond);
    LLVMPositionBuilderAtEnd(lg.builder, done);
    LLVMBuildRet(lg.builder, LLVMBuildLoad2(lg.builder, lg.i64_ty, h, "r"));
    return fn;
}

/* занулява кофите b[0..nb) */
static void map_null_buckets(LLVMValueRef fn, LLVMValueRef b, LLVMValueRef nb) {
    LLVMValueRef iv = entry_alloca(lg.i64_ty, "i");
    LLVMBuildStore(lg.builder, LLVMConstInt(lg.i64_ty, 0, 0), iv);
    LLVMBasicBlockRef cond = LLVMAppendBasicBlockInContext(lg.ctx, fn, "cond");
    LLVMBasicBlockRef body = LLVMAppendBasicBlockInContext(lg.ctx, fn, "body");
    LLVMBasicBlockRef done = LLVMAppendBasicBlockInContext(lg.ctx, fn, "done");
    LLVMBuildBr(lg.builder, cond);
    LLVMPositionBuilderAtEnd(lg.builder, cond);
    LLVMValueRef i = LLVMBuildLoad2(lg.builder, lg.i64_ty, iv, "i");
    LLVMValueRef cc = LLVMBuildICmp(lg.builder, LLVMIntSLT, i, nb, "cc");
    LLVMBuildCondBr(lg.builder, cc, body, done);
    LLVMPositionBuilderAtEnd(lg.builder, body);
    LLVMBuildStore(lg.builder, LLVMConstNull(baga_map_entry_ptr_ty()),
        LLVMBuildGEP2(lg.builder, baga_map_entry_ptr_ty(), b, &i, 1, "slot"));
    LLVMBuildStore(lg.builder, LLVMBuildAdd(lg.builder, i,
        LLVMConstInt(lg.i64_ty, 1, 0), "in"), iv);
    LLVMBuildBr(lg.builder, cond);
    LLVMPositionBuilderAtEnd(lg.builder, done);
}

/* static baga_Map *baga_map_new(void) — 16 празни кофи */
static LLVMValueRef build_baga_map_new(void) {
    LLVMValueRef fn = LLVMAddFunction(lg.mod, "baga_map_new",
        LLVMFunctionType(baga_map_ptr_ty(), NULL, 0, 0));
    h_begin(fn);
    LLVMValueRef msz[] = { LLVMSizeOf(baga_map_ty()) };
    LLVMValueRef m = LLVMBuildBitCast(lg.builder,
        h_call(rt_malloc(), msz, 1, "raw"), baga_map_ptr_ty(), "m");
    LLVMBuildStore(lg.builder, LLVMConstInt(lg.i64_ty, 16, 0),
                   map_fld(m, 1, "nbp"));
    LLVMBuildStore(lg.builder, LLVMConstInt(lg.i64_ty, 0, 0),
                   map_fld(m, 2, "lenp"));
    LLVMValueRef bsz[] = { LLVMConstInt(lg.i64_ty, 16 * 8, 0) };
    LLVMValueRef b = LLVMBuildBitCast(lg.builder,
        h_call(rt_malloc(), bsz, 1, "braw"), baga_map_entry_slot_ty(), "b");
    LLVMBuildStore(lg.builder, b, map_fld(m, 0, "bp"));
    map_null_buckets(fn, b, LLVMConstInt(lg.i64_ty, 16, 0));
    LLVMBuildRet(lg.builder, m);
    return fn;
}

/* static baga_MapEntry **baga_map_slot(baga_Map *m, int64_t ik,
 *     const char *sk, uint64_t h)
 * Връща слота на записа или NULL връзката (insert point). */
static LLVMValueRef build_baga_map_slot(void) {
    LLVMTypeRef p[] = { baga_map_ptr_ty(), lg.i64_ty, lg.ptr_ty, lg.i64_ty };
    LLVMValueRef fn = LLVMAddFunction(lg.mod, "baga_map_slot",
        LLVMFunctionType(baga_map_entry_slot_ty(), p, 4, 0));
    h_begin(fn);
    LLVMValueRef m = LLVMGetParam(fn, 0);
    LLVMValueRef ik = LLVMGetParam(fn, 1);
    LLVMValueRef sk = LLVMGetParam(fn, 2);
    LLVMValueRef h = LLVMGetParam(fn, 3);
    LLVMValueRef idx = LLVMBuildURem(lg.builder, h, map_load_nb(m), "idx");
    LLVMValueRef sp = entry_alloca(baga_map_entry_slot_ty(), "sp");
    LLVMBuildStore(lg.builder, LLVMBuildGEP2(lg.builder,
        baga_map_entry_ptr_ty(), map_load_b(m), &idx, 1, "s0"), sp);
    LLVMBasicBlockRef cond = LLVMAppendBasicBlockInContext(lg.ctx, fn, "cond");
    LLVMBasicBlockRef chk  = LLVMAppendBasicBlockInContext(lg.ctx, fn, "chk");
    LLVMBasicBlockRef chks = LLVMAppendBasicBlockInContext(lg.ctx, fn, "chks");
    LLVMBasicBlockRef chki = LLVMAppendBasicBlockInContext(lg.ctx, fn, "chki");
    LLVMBasicBlockRef cmps = LLVMAppendBasicBlockInContext(lg.ctx, fn, "cmps");
    LLVMBasicBlockRef cmpi = LLVMAppendBasicBlockInContext(lg.ctx, fn, "cmpi");
    LLVMBasicBlockRef nextb = LLVMAppendBasicBlockInContext(lg.ctx, fn, "next");
    LLVMBasicBlockRef retb = LLVMAppendBasicBlockInContext(lg.ctx, fn, "ret");
    LLVMBuildBr(lg.builder, cond);
    LLVMPositionBuilderAtEnd(lg.builder, cond);
    LLVMValueRef s = LLVMBuildLoad2(lg.builder, baga_map_entry_slot_ty(), sp, "s");
    LLVMValueRef e = LLVMBuildLoad2(lg.builder, baga_map_entry_ptr_ty(), s, "e");
    LLVMBuildCondBr(lg.builder, LLVMBuildIsNull(lg.builder, e, "isn"), retb, chk);
    LLVMPositionBuilderAtEnd(lg.builder, chk);
    LLVMValueRef esk = LLVMBuildLoad2(lg.builder, lg.ptr_ty,
        ent_fld(e, 1, "skp"), "esk");
    LLVMBuildCondBr(lg.builder, LLVMBuildIsNull(lg.builder, sk, "skn"),
                    chki, chks);
    /* str ключ: (*e)->sk && strcmp((*e)->sk, sk) == 0 */
    LLVMPositionBuilderAtEnd(lg.builder, chks);
    LLVMBuildCondBr(lg.builder, LLVMBuildIsNull(lg.builder, esk, "esn"),
                    nextb, cmps);
    LLVMPositionBuilderAtEnd(lg.builder, cmps);
    LLVMValueRef ca[] = { esk, sk };
    LLVMValueRef cmp = h_call(rt_strcmp(), ca, 2, "cmp");
    LLVMValueRef eq = LLVMBuildICmp(lg.builder, LLVMIntEQ, cmp,
        LLVMConstInt(lg.i32_ty, 0, 0), "eq");
    LLVMBuildCondBr(lg.builder, eq, retb, nextb);
    /* i64 ключ: !(*e)->sk && (*e)->ik == ik */
    LLVMPositionBuilderAtEnd(lg.builder, chki);
    LLVMBuildCondBr(lg.builder, LLVMBuildIsNull(lg.builder, esk, "esn2"),
                    cmpi, nextb);
    LLVMPositionBuilderAtEnd(lg.builder, cmpi);
    LLVMValueRef eik = LLVMBuildLoad2(lg.builder, lg.i64_ty,
        ent_fld(e, 0, "ikp"), "eik");
    LLVMValueRef eqi = LLVMBuildICmp(lg.builder, LLVMIntEQ, eik, ik, "eqi");
    LLVMBuildCondBr(lg.builder, eqi, retb, nextb);
    LLVMPositionBuilderAtEnd(lg.builder, nextb);
    LLVMBuildStore(lg.builder, ent_fld(e, 9, "nxp"), sp);
    LLVMBuildBr(lg.builder, cond);
    LLVMPositionBuilderAtEnd(lg.builder, retb);
    LLVMBuildRet(lg.builder,
        LLVMBuildLoad2(lg.builder, baga_map_entry_slot_ty(), sp, "r"));
    return fn;
}

/* static baga_MapEntry **baga_map_slot_b(baga_Map *m, baga_bytes k,
 *     uint64_t h) — R67: memcmp сравнение, само записи с ktag == 2 */
static LLVMValueRef build_baga_map_slot_b(void) {
    LLVMTypeRef p[] = { baga_map_ptr_ty(), baga_bytes_ty(), lg.i64_ty };
    LLVMValueRef fn = LLVMAddFunction(lg.mod, "baga_map_slot_b",
        LLVMFunctionType(baga_map_entry_slot_ty(), p, 3, 0));
    h_begin(fn);
    LLVMValueRef m = LLVMGetParam(fn, 0);
    LLVMValueRef ka = bytes_param_alloca(LLVMGetParam(fn, 1));
    LLVMValueRef kdata = bytes_load_data(ka);
    LLVMValueRef klen = bytes_load_len(ka);
    LLVMValueRef h = LLVMGetParam(fn, 2);
    LLVMValueRef idx = LLVMBuildURem(lg.builder, h, map_load_nb(m), "idx");
    LLVMValueRef sp = entry_alloca(baga_map_entry_slot_ty(), "sp");
    LLVMBuildStore(lg.builder, LLVMBuildGEP2(lg.builder,
        baga_map_entry_ptr_ty(), map_load_b(m), &idx, 1, "s0"), sp);
    LLVMBasicBlockRef cond = LLVMAppendBasicBlockInContext(lg.ctx, fn, "cond");
    LLVMBasicBlockRef chk  = LLVMAppendBasicBlockInContext(lg.ctx, fn, "chk");
    LLVMBasicBlockRef chkl = LLVMAppendBasicBlockInContext(lg.ctx, fn, "chkl");
    LLVMBasicBlockRef chkm = LLVMAppendBasicBlockInContext(lg.ctx, fn, "chkm");
    LLVMBasicBlockRef cmpm = LLVMAppendBasicBlockInContext(lg.ctx, fn, "cmpm");
    LLVMBasicBlockRef nextb = LLVMAppendBasicBlockInContext(lg.ctx, fn, "next");
    LLVMBasicBlockRef retb = LLVMAppendBasicBlockInContext(lg.ctx, fn, "ret");
    LLVMBuildBr(lg.builder, cond);
    LLVMPositionBuilderAtEnd(lg.builder, cond);
    LLVMValueRef s = LLVMBuildLoad2(lg.builder, baga_map_entry_slot_ty(), sp, "s");
    LLVMValueRef e = LLVMBuildLoad2(lg.builder, baga_map_entry_ptr_ty(), s, "e");
    LLVMBuildCondBr(lg.builder, LLVMBuildIsNull(lg.builder, e, "isn"), retb, chk);
    LLVMPositionBuilderAtEnd(lg.builder, chk);
    LLVMValueRef ktag = LLVMBuildLoad2(lg.builder, lg.i64_ty,
        ent_fld(e, 3, "ktp"), "ktag");
    LLVMValueRef is2 = LLVMBuildICmp(lg.builder, LLVMIntEQ, ktag,
        LLVMConstInt(lg.i64_ty, 2, 0), "is2");
    LLVMBuildCondBr(lg.builder, is2, chkl, nextb);
    LLVMPositionBuilderAtEnd(lg.builder, chkl);
    LLVMValueRef bkp = ent_fld(e, 2, "bkp");
    LLVMValueRef ebl = LLVMBuildLoad2(lg.builder, lg.i64_ty,
        LLVMBuildStructGEP2(lg.builder, baga_bytes_ty(), bkp, 1, "blp"), "ebl");
    LLVMValueRef eql = LLVMBuildICmp(lg.builder, LLVMIntEQ, ebl, klen, "eql");
    LLVMBuildCondBr(lg.builder, eql, chkm, nextb);
    /* k.len == 0 || memcmp((*e)->bk.data, k.data, k.len) == 0 */
    LLVMPositionBuilderAtEnd(lg.builder, chkm);
    LLVMValueRef isz = LLVMBuildICmp(lg.builder, LLVMIntEQ, klen,
        LLVMConstInt(lg.i64_ty, 0, 0), "isz");
    LLVMBuildCondBr(lg.builder, isz, retb, cmpm);
    LLVMPositionBuilderAtEnd(lg.builder, cmpm);
    LLVMValueRef ebd = LLVMBuildLoad2(lg.builder, lg.ptr_ty,
        LLVMBuildStructGEP2(lg.builder, baga_bytes_ty(), bkp, 0, "bdp"), "ebd");
    LLVMValueRef ma[] = { ebd, kdata, klen };
    LLVMValueRef cmp = h_call(rt_memcmp(), ma, 3, "cmp");
    LLVMValueRef eq = LLVMBuildICmp(lg.builder, LLVMIntEQ, cmp,
        LLVMConstInt(lg.i32_ty, 0, 0), "eq");
    LLVMBuildCondBr(lg.builder, eq, retb, nextb);
    LLVMPositionBuilderAtEnd(lg.builder, nextb);
    LLVMBuildStore(lg.builder, ent_fld(e, 9, "nxp"), sp);
    LLVMBuildBr(lg.builder, cond);
    LLVMPositionBuilderAtEnd(lg.builder, retb);
    LLVMBuildRet(lg.builder,
        LLVMBuildLoad2(lg.builder, baga_map_entry_slot_ty(), sp, "r"));
    return fn;
}

/* static void baga_map_rehash(baga_Map *m) — удвояване; хешът се смята
 * наново за всеки запис по ktag (0=i64, 1=str, 2=bytes) */
static LLVMValueRef build_baga_map_rehash(void) {
    LLVMTypeRef p[] = { baga_map_ptr_ty() };
    LLVMValueRef fn = LLVMAddFunction(lg.mod, "baga_map_rehash",
        LLVMFunctionType(lg.void_ty, p, 1, 0));
    h_begin(fn);
    LLVMValueRef m = LLVMGetParam(fn, 0);
    LLVMValueRef onb = map_load_nb(m);
    LLVMValueRef ob = map_load_b(m);
    LLVMValueRef nb2 = LLVMBuildMul(lg.builder, onb,
        LLVMConstInt(lg.i64_ty, 2, 0), "nb2");
    LLVMBuildStore(lg.builder, nb2, map_fld(m, 1, "nbp"));
    LLVMValueRef bsz[] = { LLVMBuildMul(lg.builder, nb2,
        LLVMConstInt(lg.i64_ty, 8, 0), "bsz") };
    LLVMValueRef nb = LLVMBuildBitCast(lg.builder,
        h_call(rt_malloc(), bsz, 1, "braw"), baga_map_entry_slot_ty(), "nb");
    LLVMBuildStore(lg.builder, nb, map_fld(m, 0, "bp"));
    map_null_buckets(fn, nb, nb2);
    /* for j in [0, onb): пренареждане на веригата ob[j] */
    LLVMValueRef jv = entry_alloca(lg.i64_ty, "j");
    LLVMBuildStore(lg.builder, LLVMConstInt(lg.i64_ty, 0, 0), jv);
    LLVMValueRef ev = entry_alloca(baga_map_entry_ptr_ty(), "e");
    LLVMValueRef hv = entry_alloca(lg.i64_ty, "hv");
    LLVMBasicBlockRef cond2 = LLVMAppendBasicBlockInContext(lg.ctx, fn, "ocond");
    LLVMBasicBlockRef body2 = LLVMAppendBasicBlockInContext(lg.ctx, fn, "obody");
    LLVMBasicBlockRef done2 = LLVMAppendBasicBlockInContext(lg.ctx, fn, "odone");
    LLVMBasicBlockRef wcond = LLVMAppendBasicBlockInContext(lg.ctx, fn, "wcond");
    LLVMBasicBlockRef wbody = LLVMAppendBasicBlockInContext(lg.ctx, fn, "wbody");
    LLVMBasicBlockRef wdone = LLVMAppendBasicBlockInContext(lg.ctx, fn, "wdone");
    LLVMBasicBlockRef hb  = LLVMAppendBasicBlockInContext(lg.ctx, fn, "hb");
    LLVMBasicBlockRef hs  = LLVMAppendBasicBlockInContext(lg.ctx, fn, "hs");
    LLVMBasicBlockRef hst = LLVMAppendBasicBlockInContext(lg.ctx, fn, "hst");
    LLVMBasicBlockRef hi  = LLVMAppendBasicBlockInContext(lg.ctx, fn, "hi");
    LLVMBasicBlockRef hj  = LLVMAppendBasicBlockInContext(lg.ctx, fn, "hj");
    LLVMBuildBr(lg.builder, cond2);
    LLVMPositionBuilderAtEnd(lg.builder, cond2);
    LLVMValueRef j = LLVMBuildLoad2(lg.builder, lg.i64_ty, jv, "j");
    LLVMValueRef cc = LLVMBuildICmp(lg.builder, LLVMIntSLT, j, onb, "cc");
    LLVMBuildCondBr(lg.builder, cc, body2, done2);
    LLVMPositionBuilderAtEnd(lg.builder, body2);
    LLVMBuildStore(lg.builder, LLVMBuildLoad2(lg.builder,
        baga_map_entry_ptr_ty(),
        LLVMBuildGEP2(lg.builder, baga_map_entry_ptr_ty(), ob, &j, 1, "slot"),
        "e0"), ev);
    LLVMBuildBr(lg.builder, wcond);
    LLVMPositionBuilderAtEnd(lg.builder, wcond);
    LLVMValueRef e = LLVMBuildLoad2(lg.builder, baga_map_entry_ptr_ty(), ev, "e");
    LLVMBuildCondBr(lg.builder, LLVMBuildIsNull(lg.builder, e, "isn"),
                    wdone, wbody);
    LLVMPositionBuilderAtEnd(lg.builder, wbody);
    LLVMValueRef nx = LLVMBuildLoad2(lg.builder, baga_map_entry_ptr_ty(),
        ent_fld(e, 9, "nxp"), "nx");
    LLVMValueRef ktag = LLVMBuildLoad2(lg.builder, lg.i64_ty,
        ent_fld(e, 3, "ktp"), "ktag");
    LLVMValueRef is2 = LLVMBuildICmp(lg.builder, LLVMIntEQ, ktag,
        LLVMConstInt(lg.i64_ty, 2, 0), "is2");
    LLVMBuildCondBr(lg.builder, is2, hb, hs);
    /* ktag == 2 → hash_bytes(e->bk) */
    LLVMPositionBuilderAtEnd(lg.builder, hb);
    LLVMValueRef bkv = LLVMBuildLoad2(lg.builder, baga_bytes_ty(),
        ent_fld(e, 2, "bkp"), "bkv");
    LLVMValueRef ba[] = { bkv };
    LLVMBuildStore(lg.builder,
        h_call(baga_rt("baga_map_hash_bytes"), ba, 1, "h"), hv);
    LLVMBuildBr(lg.builder, hj);
    /* ktag != 2 → e->sk ? hash_str(e->sk) : hash_i64(e->ik) */
    LLVMPositionBuilderAtEnd(lg.builder, hs);
    LLVMValueRef esk = LLVMBuildLoad2(lg.builder, lg.ptr_ty,
        ent_fld(e, 1, "skp"), "esk");
    LLVMBuildCondBr(lg.builder, LLVMBuildIsNull(lg.builder, esk, "esn"),
                    hi, hst);
    LLVMPositionBuilderAtEnd(lg.builder, hst);
    LLVMValueRef sa[] = { esk };
    LLVMBuildStore(lg.builder,
        h_call(baga_rt("baga_map_hash_str"), sa, 1, "h"), hv);
    LLVMBuildBr(lg.builder, hj);
    LLVMPositionBuilderAtEnd(lg.builder, hi);
    LLVMValueRef eik = LLVMBuildLoad2(lg.builder, lg.i64_ty,
        ent_fld(e, 0, "ikp"), "eik");
    LLVMValueRef ia[] = { eik };
    LLVMBuildStore(lg.builder,
        h_call(baga_rt("baga_map_hash_i64"), ia, 1, "h"), hv);
    LLVMBuildBr(lg.builder, hj);
    /* вмъкване в началото на новата кофа */
    LLVMPositionBuilderAtEnd(lg.builder, hj);
    LLVMValueRef h2 = LLVMBuildLoad2(lg.builder, lg.i64_ty, hv, "h2");
    LLVMValueRef idx = LLVMBuildURem(lg.builder, h2, nb2, "idx");
    LLVMValueRef slotp = LLVMBuildGEP2(lg.builder, baga_map_entry_ptr_ty(),
        nb, &idx, 1, "slotp");
    LLVMValueRef old = LLVMBuildLoad2(lg.builder, baga_map_entry_ptr_ty(),
        slotp, "old");
    LLVMBuildStore(lg.builder, old, ent_fld(e, 9, "nxp"));
    LLVMBuildStore(lg.builder, e, slotp);
    LLVMBuildStore(lg.builder, nx, ev);
    LLVMBuildBr(lg.builder, wcond);
    LLVMPositionBuilderAtEnd(lg.builder, wdone);
    LLVMBuildStore(lg.builder, LLVMBuildAdd(lg.builder, j,
        LLVMConstInt(lg.i64_ty, 1, 0), "jn"), jv);
    LLVMBuildBr(lg.builder, cond2);
    LLVMPositionBuilderAtEnd(lg.builder, done2);
    LLVMBuildRetVoid(lg.builder);
    return fn;
}

/* споделено тяло на put след намиране на слота: създава запис, връзва го,
 * len++, rehash при load > 3/4. ik/sk/bk/ktag са вече изчислени. */
static LLVMValueRef map_put_finish(LLVMValueRef fn, LLVMValueRef m,
                                   LLVMValueRef slot, LLVMValueRef ik,
                                   LLVMValueRef sk, LLVMValueRef bk,
                                   LLVMValueRef ktag) {
    LLVMValueRef found = LLVMBuildLoad2(lg.builder, baga_map_entry_ptr_ty(),
        slot, "found");
    LLVMBasicBlockRef have = LLVMAppendBasicBlockInContext(lg.ctx, fn, "have");
    LLVMBasicBlockRef ins  = LLVMAppendBasicBlockInContext(lg.ctx, fn, "ins");
    LLVMBuildCondBr(lg.builder, LLVMBuildIsNull(lg.builder, found, "isn"),
                    ins, have);
    LLVMPositionBuilderAtEnd(lg.builder, have);
    LLVMBuildRet(lg.builder, found);
    LLVMPositionBuilderAtEnd(lg.builder, ins);
    LLVMValueRef esz[] = { LLVMSizeOf(baga_map_entry_ty()) };
    LLVMValueRef e = LLVMBuildBitCast(lg.builder,
        h_call(rt_malloc(), esz, 1, "eraw"), baga_map_entry_ptr_ty(), "e");
    LLVMBuildStore(lg.builder, ik, ent_fld(e, 0, "ikp"));
    LLVMBuildStore(lg.builder, sk, ent_fld(e, 1, "skp"));
    LLVMBuildStore(lg.builder, bk, ent_fld(e, 2, "bkp"));
    LLVMBuildStore(lg.builder, ktag, ent_fld(e, 3, "ktp"));
    LLVMBuildStore(lg.builder, LLVMConstInt(lg.i64_ty, 0, 0), ent_fld(e, 4, "ivp"));
    LLVMBuildStore(lg.builder, LLVMConstNull(lg.double_ty), ent_fld(e, 5, "fvp"));
    LLVMBuildStore(lg.builder, LLVMConstNull(lg.ptr_ty), ent_fld(e, 6, "svp"));
    LLVMBuildStore(lg.builder, LLVMConstNull(baga_bytes_ty()), ent_fld(e, 7, "bvp"));
    LLVMBuildStore(lg.builder, LLVMConstNull(lg.ptr_ty), ent_fld(e, 8, "pvp"));
    LLVMBuildStore(lg.builder, LLVMConstNull(baga_map_entry_ptr_ty()),
                   ent_fld(e, 9, "nxp"));
    LLVMBuildStore(lg.builder, e, slot);
    LLVMValueRef lenp = map_fld(m, 2, "lenp");
    LLVMValueRef nl = LLVMBuildAdd(lg.builder,
        LLVMBuildLoad2(lg.builder, lg.i64_ty, lenp, "len"),
        LLVMConstInt(lg.i64_ty, 1, 0), "nl");
    LLVMBuildStore(lg.builder, nl, lenp);
    /* if (m->len * 4 > m->nb * 3) baga_map_rehash(m); */
    LLVMValueRef l4 = LLVMBuildMul(lg.builder, nl,
        LLVMConstInt(lg.i64_ty, 4, 0), "l4");
    LLVMValueRef n3 = LLVMBuildMul(lg.builder, map_load_nb(m),
        LLVMConstInt(lg.i64_ty, 3, 0), "n3");
    LLVMValueRef over = LLVMBuildICmp(lg.builder, LLVMIntSGT, l4, n3, "over");
    LLVMBasicBlockRef rb   = LLVMAppendBasicBlockInContext(lg.ctx, fn, "rb");
    LLVMBasicBlockRef done = LLVMAppendBasicBlockInContext(lg.ctx, fn, "done");
    LLVMBuildCondBr(lg.builder, over, rb, done);
    LLVMPositionBuilderAtEnd(lg.builder, rb);
    LLVMValueRef ra[] = { m };
    h_call(baga_rt("baga_map_rehash"), ra, 1, "");
    LLVMBuildBr(lg.builder, done);
    LLVMPositionBuilderAtEnd(lg.builder, done);
    LLVMBuildRet(lg.builder, e);
    return NULL; /* caller-ът не ползва връзката */
}

/* static baga_MapEntry *baga_map_put(baga_Map *m, int64_t ik,
 *     const char *sk, uint64_t h) — ktag = sk ? 1 : 0 */
static LLVMValueRef build_baga_map_put(void) {
    LLVMTypeRef p[] = { baga_map_ptr_ty(), lg.i64_ty, lg.ptr_ty, lg.i64_ty };
    LLVMValueRef fn = LLVMAddFunction(lg.mod, "baga_map_put",
        LLVMFunctionType(baga_map_entry_ptr_ty(), p, 4, 0));
    h_begin(fn);
    LLVMValueRef m = LLVMGetParam(fn, 0);
    LLVMValueRef ik = LLVMGetParam(fn, 1);
    LLVMValueRef sk = LLVMGetParam(fn, 2);
    LLVMValueRef sa[] = { m, ik, sk, LLVMGetParam(fn, 3) };
    LLVMValueRef slot = h_call(baga_rt("baga_map_slot"), sa, 4, "slot");
    LLVMValueRef ktag = LLVMBuildSelect(lg.builder,
        LLVMBuildIsNull(lg.builder, sk, "skn"),
        LLVMConstInt(lg.i64_ty, 0, 0), LLVMConstInt(lg.i64_ty, 1, 0), "ktag");
    map_put_finish(fn, m, slot, ik, sk,
                   LLVMConstNull(baga_bytes_ty()), ktag);
    return fn;
}

/* static baga_MapEntry *baga_map_put_b(baga_Map *m, baga_bytes k,
 *     uint64_t h) — R67 bytes ключ, ktag = 2 */
static LLVMValueRef build_baga_map_put_b(void) {
    LLVMTypeRef p[] = { baga_map_ptr_ty(), baga_bytes_ty(), lg.i64_ty };
    LLVMValueRef fn = LLVMAddFunction(lg.mod, "baga_map_put_b",
        LLVMFunctionType(baga_map_entry_ptr_ty(), p, 3, 0));
    h_begin(fn);
    LLVMValueRef m = LLVMGetParam(fn, 0);
    LLVMValueRef ka = bytes_param_alloca(LLVMGetParam(fn, 1));
    LLVMValueRef kv = LLVMBuildLoad2(lg.builder, baga_bytes_ty(), ka, "kv");
    LLVMValueRef sa[] = { m, kv, LLVMGetParam(fn, 2) };
    LLVMValueRef slot = h_call(baga_rt("baga_map_slot_b"), sa, 3, "slot");
    map_put_finish(fn, m, slot, LLVMConstInt(lg.i64_ty, 0, 0),
                   LLVMConstNull(lg.ptr_ty), kv,
                   LLVMConstInt(lg.i64_ty, 2, 0));
    return fn;
}

/* static int64_t baga_map_len(baga_Map *m) { return m ? m->len : 0; } */
static LLVMValueRef build_baga_map_len(void) {
    LLVMTypeRef p[] = { baga_map_ptr_ty() };
    LLVMValueRef fn = LLVMAddFunction(lg.mod, "baga_map_len",
        LLVMFunctionType(lg.i64_ty, p, 1, 0));
    h_begin(fn);
    LLVMValueRef m = LLVMGetParam(fn, 0);
    LLVMBasicBlockRef nullb = LLVMAppendBasicBlockInContext(lg.ctx, fn, "null");
    LLVMBasicBlockRef haveb = LLVMAppendBasicBlockInContext(lg.ctx, fn, "have");
    LLVMBuildCondBr(lg.builder, LLVMBuildIsNull(lg.builder, m, "isn"),
                    nullb, haveb);
    LLVMPositionBuilderAtEnd(lg.builder, nullb);
    LLVMBuildRet(lg.builder, LLVMConstInt(lg.i64_ty, 0, 0));
    LLVMPositionBuilderAtEnd(lg.builder, haveb);
    LLVMBuildRet(lg.builder,
        LLVMBuildLoad2(lg.builder, lg.i64_ty, map_fld(m, 2, "lenp"), "len"));
    return fn;
}

/* R55: map <-> i64 handle casts (като baga_str_h/baga_h_str) */
static LLVMValueRef build_baga_map_h(void) {
    LLVMTypeRef p[] = { baga_map_ptr_ty() };
    LLVMValueRef fn = LLVMAddFunction(lg.mod, "baga_map_h",
        LLVMFunctionType(lg.i64_ty, p, 1, 0));
    h_begin(fn);
    LLVMBuildRet(lg.builder,
        LLVMBuildPtrToInt(lg.builder, LLVMGetParam(fn, 0), lg.i64_ty, "h"));
    return fn;
}
static LLVMValueRef build_baga_h_map(void) {
    LLVMTypeRef p[] = { lg.i64_ty };
    LLVMValueRef fn = LLVMAddFunction(lg.mod, "baga_h_map",
        LLVMFunctionType(baga_map_ptr_ty(), p, 1, 0));
    h_begin(fn);
    LLVMBuildRet(lg.builder, LLVMBuildIntToPtr(lg.builder,
        LLVMGetParam(fn, 0), baga_map_ptr_ty(), "m"));
    return fn;
}

/* ---- typed map ops: baga_map_{set,get}_<key>_<val>,
 * baga_map_{has,del,keys}_<key>; key: 0=str, 1=i64, 2=bytes;
 * val: 0=i64, 1=str, 2=f64, 3=bytes, 4=box ---- */

/* "str"→0, "i64"→1, "bytes"→2 (пише дължината в len); -1 при непознат */
static int map_key_kind(const char *s, int *len) {
    if (strncmp(s, "str", 3) == 0)   { *len = 3; return 0; }
    if (strncmp(s, "i64", 3) == 0)   { *len = 3; return 1; }
    if (strncmp(s, "bytes", 5) == 0) { *len = 5; return 2; }
    return -1;
}
static int map_val_kind(const char *s) {
    if (strcmp(s, "i64") == 0)   return 0;
    if (strcmp(s, "str") == 0)   return 1;
    if (strcmp(s, "f64") == 0)   return 2;
    if (strcmp(s, "bytes") == 0) return 3;
    if (strcmp(s, "box") == 0)   return 4;
    return -1;
}
static LLVMTypeRef map_key_ty(int key) {
    return key == 0 ? lg.ptr_ty : key == 1 ? lg.i64_ty : baga_bytes_ty();
}
static LLVMTypeRef map_val_ty(int val) {
    switch (val) {
        case 0: return lg.i64_ty;
        case 1: return lg.ptr_ty;
        case 2: return lg.double_ty;
        case 3: return baga_bytes_ty();
        default: return lg.ptr_ty;   /* box */
    }
}

/* e = baga_map_put*(m, k, hash(k)) според вида на ключа */
static LLVMValueRef map_put_call(LLVMValueRef m, int key, LLVMValueRef k) {
    if (key == 2) {
        LLVMValueRef ka = bytes_param_alloca(k);
        LLVMValueRef kv = LLVMBuildLoad2(lg.builder, baga_bytes_ty(), ka, "kv");
        LLVMValueRef ha[] = { kv };
        LLVMValueRef hh = h_call(baga_rt("baga_map_hash_bytes"), ha, 1, "h");
        LLVMValueRef pa[] = { m, kv, hh };
        return h_call(baga_rt("baga_map_put_b"), pa, 3, "e");
    }
    LLVMValueRef ha[] = { k };
    LLVMValueRef hh = h_call(baga_rt(key == 0 ? "baga_map_hash_str"
                                              : "baga_map_hash_i64"), ha, 1, "h");
    LLVMValueRef ik = key == 1 ? k : LLVMConstInt(lg.i64_ty, 0, 0);
    LLVMValueRef sk = key == 0 ? k : LLVMConstNull(lg.ptr_ty);
    LLVMValueRef pa[] = { m, ik, sk, hh };
    return h_call(baga_rt("baga_map_put"), pa, 4, "e");
}

/* slot = baga_map_slot*(m, k, hash(k)) — insert point или слот на записа */
static LLVMValueRef map_slot_call(LLVMValueRef m, int key, LLVMValueRef k) {
    if (key == 2) {
        LLVMValueRef ka = bytes_param_alloca(k);
        LLVMValueRef kv = LLVMBuildLoad2(lg.builder, baga_bytes_ty(), ka, "kv");
        LLVMValueRef ha[] = { kv };
        LLVMValueRef hh = h_call(baga_rt("baga_map_hash_bytes"), ha, 1, "h");
        LLVMValueRef pa[] = { m, kv, hh };
        return h_call(baga_rt("baga_map_slot_b"), pa, 3, "slot");
    }
    LLVMValueRef ha[] = { k };
    LLVMValueRef hh = h_call(baga_rt(key == 0 ? "baga_map_hash_str"
                                              : "baga_map_hash_i64"), ha, 1, "h");
    LLVMValueRef ik = key == 1 ? k : LLVMConstInt(lg.i64_ty, 0, 0);
    LLVMValueRef sk = key == 0 ? k : LLVMConstNull(lg.ptr_ty);
    LLVMValueRef pa[] = { m, ik, sk, hh };
    return h_call(baga_rt("baga_map_slot"), pa, 4, "slot");
}

/* static void baga_map_set_<key>_<val>(m, k, v[, size]):
 * e = put(...); e->fld = v; box: e->pv ??= malloc(size); memcpy */
static LLVMValueRef build_baga_map_set(const char *name, int key, int val) {
    LLVMTypeRef p[4];
    int np = 0;
    p[np++] = baga_map_ptr_ty();
    p[np++] = map_key_ty(key);
    p[np++] = map_val_ty(val);
    if (val == 4) p[np++] = lg.i64_ty;
    LLVMValueRef fn = LLVMAddFunction(lg.mod, name,
        LLVMFunctionType(lg.void_ty, p, (unsigned)np, 0));
    h_begin(fn);
    LLVMValueRef e = map_put_call(LLVMGetParam(fn, 0), key, LLVMGetParam(fn, 1));
    if (val != 4) {
        unsigned fld = val == 0 ? 4 : val == 1 ? 6 : val == 2 ? 5 : 7;
        LLVMBuildStore(lg.builder, LLVMGetParam(fn, 2), ent_fld(e, fld, "vp"));
        LLVMBuildRetVoid(lg.builder);
        return fn;
    }
    /* box: if (!e->pv) e->pv = malloc(size); memcpy(e->pv, src, size) */
    LLVMValueRef size = LLVMGetParam(fn, 3);
    LLVMValueRef pvp = ent_fld(e, 8, "pvp");
    LLVMValueRef pv = LLVMBuildLoad2(lg.builder, lg.ptr_ty, pvp, "pv");
    LLVMBasicBlockRef alb = LLVMAppendBasicBlockInContext(lg.ctx, fn, "alb");
    LLVMBasicBlockRef cpb = LLVMAppendBasicBlockInContext(lg.ctx, fn, "cpb");
    LLVMBuildCondBr(lg.builder, LLVMBuildIsNull(lg.builder, pv, "isn"), alb, cpb);
    LLVMPositionBuilderAtEnd(lg.builder, alb);
    LLVMValueRef ma[] = { size };
    LLVMBuildStore(lg.builder, h_call(rt_malloc(), ma, 1, "box"), pvp);
    LLVMBuildBr(lg.builder, cpb);
    LLVMPositionBuilderAtEnd(lg.builder, cpb);
    LLVMValueRef pv2 = LLVMBuildLoad2(lg.builder, lg.ptr_ty, pvp, "pv2");
    LLVMValueRef mc[] = { pv2, LLVMGetParam(fn, 2), size };
    h_call(rt_memcpy(), mc, 3, "");
    LLVMBuildRetVoid(lg.builder);
    return fn;
}

/* static <val> baga_map_get_<key>_<val>(m, k): e = *slot(...);
 * липсващ ключ → 0 / "" / 0.0 / {NULL,0} / NULL (като codegen_c) */
static LLVMValueRef build_baga_map_get(const char *name, int key, int val) {
    LLVMTypeRef rty = map_val_ty(val);
    LLVMTypeRef p[] = { baga_map_ptr_ty(), map_key_ty(key) };
    LLVMValueRef fn = LLVMAddFunction(lg.mod, name,
        LLVMFunctionType(rty, p, 2, 0));
    h_begin(fn);
    LLVMValueRef slot = map_slot_call(LLVMGetParam(fn, 0), key,
                                      LLVMGetParam(fn, 1));
    LLVMValueRef e = LLVMBuildLoad2(lg.builder, baga_map_entry_ptr_ty(),
                                    slot, "e");
    LLVMValueRef res = entry_alloca(rty, "res");
    LLVMBuildStore(lg.builder, val == 1
        ? LLVMBuildGlobalStringPtr(lg.builder, "", "empty")
        : LLVMConstNull(rty), res);
    LLVMBasicBlockRef have = LLVMAppendBasicBlockInContext(lg.ctx, fn, "have");
    LLVMBasicBlockRef done = LLVMAppendBasicBlockInContext(lg.ctx, fn, "done");
    LLVMBuildCondBr(lg.builder, LLVMBuildIsNull(lg.builder, e, "isn"),
                    done, have);
    LLVMPositionBuilderAtEnd(lg.builder, have);
    unsigned fld = val == 0 ? 4 : val == 1 ? 6 : val == 2 ? 5
                 : val == 3 ? 7 : 8;
    LLVMValueRef fv = LLVMBuildLoad2(lg.builder, rty, ent_fld(e, fld, "fp"), "fv");
    if (val == 1) {
        /* str: (e && e->sv) ? e->sv : "" */
        LLVMBasicBlockRef setb = LLVMAppendBasicBlockInContext(lg.ctx, fn, "set");
        LLVMBuildCondBr(lg.builder, LLVMBuildIsNull(lg.builder, fv, "svn"),
                        done, setb);
        LLVMPositionBuilderAtEnd(lg.builder, setb);
        LLVMBuildStore(lg.builder, fv, res);
        LLVMBuildBr(lg.builder, done);
    } else {
        LLVMBuildStore(lg.builder, fv, res);
        LLVMBuildBr(lg.builder, done);
    }
    LLVMPositionBuilderAtEnd(lg.builder, done);
    LLVMBuildRet(lg.builder, LLVMBuildLoad2(lg.builder, rty, res, "r"));
    return fn;
}

/* static int64_t baga_map_has_<key>(m, k) { return *slot ? 1 : 0; } */
static LLVMValueRef build_baga_map_has(const char *name, int key) {
    LLVMTypeRef p[] = { baga_map_ptr_ty(), map_key_ty(key) };
    LLVMValueRef fn = LLVMAddFunction(lg.mod, name,
        LLVMFunctionType(lg.i64_ty, p, 2, 0));
    h_begin(fn);
    LLVMValueRef slot = map_slot_call(LLVMGetParam(fn, 0), key,
                                      LLVMGetParam(fn, 1));
    LLVMValueRef e = LLVMBuildLoad2(lg.builder, baga_map_entry_ptr_ty(),
                                    slot, "e");
    LLVMBuildRet(lg.builder, LLVMBuildZExt(lg.builder,
        LLVMBuildIsNotNull(lg.builder, e, "has"), lg.i64_ty, "r"));
    return fn;
}

/* static void baga_map_del_<key>(m, k) — извършва записа от веригата */
static LLVMValueRef build_baga_map_del(const char *name, int key) {
    LLVMTypeRef p[] = { baga_map_ptr_ty(), map_key_ty(key) };
    LLVMValueRef fn = LLVMAddFunction(lg.mod, name,
        LLVMFunctionType(lg.void_ty, p, 2, 0));
    h_begin(fn);
    LLVMValueRef m = LLVMGetParam(fn, 0);
    LLVMValueRef slot = map_slot_call(m, key, LLVMGetParam(fn, 1));
    LLVMValueRef e = LLVMBuildLoad2(lg.builder, baga_map_entry_ptr_ty(),
                                    slot, "e");
    LLVMBasicBlockRef delb = LLVMAppendBasicBlockInContext(lg.ctx, fn, "del");
    LLVMBasicBlockRef done = LLVMAppendBasicBlockInContext(lg.ctx, fn, "done");
    LLVMBuildCondBr(lg.builder, LLVMBuildIsNull(lg.builder, e, "isn"),
                    done, delb);
    LLVMPositionBuilderAtEnd(lg.builder, delb);
    LLVMValueRef nx = LLVMBuildLoad2(lg.builder, baga_map_entry_ptr_ty(),
        ent_fld(e, 9, "nxp"), "nx");
    LLVMBuildStore(lg.builder, nx, slot);
    LLVMValueRef lenp = map_fld(m, 2, "lenp");
    LLVMBuildStore(lg.builder, LLVMBuildSub(lg.builder,
        LLVMBuildLoad2(lg.builder, lg.i64_ty, lenp, "len"),
        LLVMConstInt(lg.i64_ty, 1, 0), "nl"), lenp);
    LLVMBuildBr(lg.builder, done);
    LLVMPositionBuilderAtEnd(lg.builder, done);
    LLVMBuildRetVoid(lg.builder);
    return fn;
}

/* static baga_Vec *baga_map_keys_<key>(m) — ключовете в bucket ред
 * (като C: str → sk ? sk : "", bytes → box-нато baga_bytes) */
static LLVMValueRef build_baga_map_keys(const char *name, int key) {
    LLVMTypeRef p[] = { baga_map_ptr_ty() };
    LLVMValueRef fn = LLVMAddFunction(lg.mod, name,
        LLVMFunctionType(baga_vec_ptr_ty(), p, 1, 0));
    h_begin(fn);
    LLVMValueRef m = LLVMGetParam(fn, 0);
    LLVMValueRef v = h_call(baga_rt("baga_vec_new"), NULL, 0, "v");
    LLVMValueRef nb = map_load_nb(m);
    LLVMValueRef b = map_load_b(m);
    LLVMValueRef iv = entry_alloca(lg.i64_ty, "i");
    LLVMBuildStore(lg.builder, LLVMConstInt(lg.i64_ty, 0, 0), iv);
    LLVMValueRef ev = entry_alloca(baga_map_entry_ptr_ty(), "e");
    LLVMBasicBlockRef cond  = LLVMAppendBasicBlockInContext(lg.ctx, fn, "cond");
    LLVMBasicBlockRef body  = LLVMAppendBasicBlockInContext(lg.ctx, fn, "body");
    LLVMBasicBlockRef done  = LLVMAppendBasicBlockInContext(lg.ctx, fn, "done");
    LLVMBasicBlockRef wcond = LLVMAppendBasicBlockInContext(lg.ctx, fn, "wcond");
    LLVMBasicBlockRef wbody = LLVMAppendBasicBlockInContext(lg.ctx, fn, "wbody");
    LLVMBasicBlockRef wdone = LLVMAppendBasicBlockInContext(lg.ctx, fn, "wdone");
    LLVMBuildBr(lg.builder, cond);
    LLVMPositionBuilderAtEnd(lg.builder, cond);
    LLVMValueRef i = LLVMBuildLoad2(lg.builder, lg.i64_ty, iv, "i");
    LLVMValueRef cc = LLVMBuildICmp(lg.builder, LLVMIntSLT, i, nb, "cc");
    LLVMBuildCondBr(lg.builder, cc, body, done);
    LLVMPositionBuilderAtEnd(lg.builder, body);
    LLVMBuildStore(lg.builder, LLVMBuildLoad2(lg.builder,
        baga_map_entry_ptr_ty(),
        LLVMBuildGEP2(lg.builder, baga_map_entry_ptr_ty(), b, &i, 1, "slot"),
        "e0"), ev);
    LLVMBuildBr(lg.builder, wcond);
    LLVMPositionBuilderAtEnd(lg.builder, wcond);
    LLVMValueRef e = LLVMBuildLoad2(lg.builder, baga_map_entry_ptr_ty(), ev, "e");
    LLVMBuildCondBr(lg.builder, LLVMBuildIsNull(lg.builder, e, "isn"),
                    wdone, wbody);
    LLVMPositionBuilderAtEnd(lg.builder, wbody);
    if (key == 0) {
        LLVMValueRef sk = LLVMBuildLoad2(lg.builder, lg.ptr_ty,
            ent_fld(e, 1, "skp"), "sk");
        LLVMValueRef s = LLVMBuildSelect(lg.builder,
            LLVMBuildIsNull(lg.builder, sk, "skn"),
            LLVMBuildGlobalStringPtr(lg.builder, "", "empty"), sk, "s");
        LLVMValueRef pa[] = { v, s };
        h_call(baga_rt("baga_vec_push_str"), pa, 2, "");
    } else if (key == 1) {
        LLVMValueRef pa[] = { v, LLVMBuildLoad2(lg.builder, lg.i64_ty,
            ent_fld(e, 0, "ikp"), "ik") };
        h_call(baga_rt("baga_vec_push_i64"), pa, 2, "");
    } else {
        LLVMValueRef bp = LLVMBuildBitCast(lg.builder, ent_fld(e, 2, "bkp"),
            lg.ptr_ty, "bp");
        LLVMValueRef pa[] = { v, bp, LLVMSizeOf(baga_bytes_ty()) };
        h_call(baga_rt("baga_vec_push_box"), pa, 3, "");
    }
    LLVMBuildStore(lg.builder, LLVMBuildLoad2(lg.builder,
        baga_map_entry_ptr_ty(), ent_fld(e, 9, "nxp"), "nx"), ev);
    LLVMBuildBr(lg.builder, wcond);
    LLVMPositionBuilderAtEnd(lg.builder, wdone);
    LLVMBuildStore(lg.builder, LLVMBuildAdd(lg.builder, i,
        LLVMConstInt(lg.i64_ty, 1, 0), "in"), iv);
    LLVMBuildBr(lg.builder, cond);
    LLVMPositionBuilderAtEnd(lg.builder, done);
    LLVMBuildRet(lg.builder, v);
    return fn;
}

/* L4: struct елементи — generic box helper-и; размерът идва от call site-а
 * (sizeof на mangled struct типа). Елементите са box-нати копия (memcpy при
 * push/set/slice/concat) — огледало на C preamble-а. Без bounds check,
 * както останалите LLVM vec helper-и. */
static LLVMValueRef build_baga_vec_push_box(void) {
    LLVMTypeRef p[] = { baga_vec_ptr_ty(), lg.ptr_ty, lg.i64_ty };
    LLVMValueRef fn = LLVMAddFunction(lg.mod, "baga_vec_push_box",
        LLVMFunctionType(lg.void_ty, p, 3, 0));
    h_begin(fn);
    LLVMValueRef v = LLVMGetParam(fn, 0);
    LLVMValueRef gv[] = { v };
    h_call(baga_rt("baga_vec_grow"), gv, 1, "");
    LLVMValueRef ma[] = { LLVMGetParam(fn, 2) };
    LLVMValueRef box = h_call(rt_malloc(), ma, 1, "box");
    LLVMValueRef mc[] = { box, LLVMGetParam(fn, 1), LLVMGetParam(fn, 2) };
    h_call(rt_memcpy(), mc, 3, "");
    LLVMValueRef len = vec_load_len(v);
    LLVMValueRef slot = LLVMBuildGEP2(lg.builder, lg.ptr_ty,
        vec_load_data(v), &len, 1, "slot");
    LLVMBuildStore(lg.builder, box, slot);
    LLVMValueRef nl = LLVMBuildAdd(lg.builder, len,
        LLVMConstInt(lg.i64_ty, 1, 0), "nl");
    LLVMBuildStore(lg.builder, nl, vec_field_ptr(v, 1, "lenp"));
    LLVMBuildRetVoid(lg.builder);
    return fn;
}
static LLVMValueRef build_baga_vec_get_box(void) {
    LLVMTypeRef p[] = { baga_vec_ptr_ty(), lg.i64_ty };
    LLVMValueRef fn = LLVMAddFunction(lg.mod, "baga_vec_get_box",
        LLVMFunctionType(lg.ptr_ty, p, 2, 0));
    h_begin(fn);
    LLVMBuildRet(lg.builder,
        vec_load_at(LLVMGetParam(fn, 0), LLVMGetParam(fn, 1)));
    return fn;
}
static LLVMValueRef build_baga_vec_set_box(void) {
    LLVMTypeRef p[] = { baga_vec_ptr_ty(), lg.i64_ty, lg.ptr_ty, lg.i64_ty };
    LLVMValueRef fn = LLVMAddFunction(lg.mod, "baga_vec_set_box",
        LLVMFunctionType(lg.void_ty, p, 4, 0));
    h_begin(fn);
    LLVMValueRef box = vec_load_at(LLVMGetParam(fn, 0), LLVMGetParam(fn, 1));
    LLVMValueRef mc[] = { box, LLVMGetParam(fn, 2), LLVMGetParam(fn, 3) };
    h_call(rt_memcpy(), mc, 3, "");
    LLVMBuildRetVoid(lg.builder);
    return fn;
}
/* loop i in [from, to): baga_vec_push_box(r, src->data[i], size) */
static void box_copy_loop(LLVMValueRef fn, LLVMValueRef src, LLVMValueRef r,
                          LLVMValueRef from, LLVMValueRef to, LLVMValueRef size) {
    LLVMValueRef iv = LLVMBuildAlloca(lg.builder, lg.i64_ty, "i");
    LLVMBuildStore(lg.builder, from, iv);
    LLVMBasicBlockRef cond = LLVMAppendBasicBlockInContext(lg.ctx, fn, "cond");
    LLVMBasicBlockRef body = LLVMAppendBasicBlockInContext(lg.ctx, fn, "body");
    LLVMBasicBlockRef done = LLVMAppendBasicBlockInContext(lg.ctx, fn, "done");
    LLVMBuildBr(lg.builder, cond);
    LLVMPositionBuilderAtEnd(lg.builder, cond);
    LLVMValueRef i = LLVMBuildLoad2(lg.builder, lg.i64_ty, iv, "i");
    LLVMValueRef cc = LLVMBuildICmp(lg.builder, LLVMIntSLT, i, to, "cc");
    LLVMBuildCondBr(lg.builder, cc, body, done);
    LLVMPositionBuilderAtEnd(lg.builder, body);
    LLVMValueRef e = vec_load_at(src, i);
    LLVMValueRef pa[] = { r, e, size };
    h_call(baga_rt("baga_vec_push_box"), pa, 3, "");
    LLVMValueRef inext = LLVMBuildAdd(lg.builder, i, LLVMConstInt(lg.i64_ty, 1, 0), "in");
    LLVMBuildStore(lg.builder, inext, iv);
    LLVMBuildBr(lg.builder, cond);
    LLVMPositionBuilderAtEnd(lg.builder, done);
}
static LLVMValueRef build_baga_vec_slice_box(void) {
    LLVMTypeRef p[] = { baga_vec_ptr_ty(), lg.i64_ty, lg.i64_ty, lg.i64_ty };
    LLVMValueRef fn = LLVMAddFunction(lg.mod, "baga_vec_slice_box",
        LLVMFunctionType(baga_vec_ptr_ty(), p, 4, 0));
    h_begin(fn);
    LLVMValueRef v = LLVMGetParam(fn, 0);
    LLVMValueRef a = LLVMGetParam(fn, 1);
    LLVMValueRef b = LLVMGetParam(fn, 2);
    slice_clamp(v, &a, &b);
    LLVMValueRef r = h_call(baga_rt("baga_vec_new"), NULL, 0, "r");
    box_copy_loop(fn, v, r, a, b, LLVMGetParam(fn, 3));
    LLVMBuildRet(lg.builder, r);
    return fn;
}
static LLVMValueRef build_baga_vec_concat_box(void) {
    LLVMTypeRef p[] = { baga_vec_ptr_ty(), baga_vec_ptr_ty(), lg.i64_ty };
    LLVMValueRef fn = LLVMAddFunction(lg.mod, "baga_vec_concat_box",
        LLVMFunctionType(baga_vec_ptr_ty(), p, 3, 0));
    h_begin(fn);
    LLVMValueRef v = LLVMGetParam(fn, 0);
    LLVMValueRef w = LLVMGetParam(fn, 1);
    LLVMValueRef size = LLVMGetParam(fn, 2);
    LLVMValueRef z = LLVMConstInt(lg.i64_ty, 0, 0);
    LLVMValueRef r = h_call(baga_rt("baga_vec_new"), NULL, 0, "r");
    box_copy_loop(fn, v, r, z, vec_load_len(v), size);
    box_copy_loop(fn, w, r, z, vec_load_len(w), size);
    LLVMBuildRet(lg.builder, r);
    return fn;
}

/* lazy dispatcher: връща helper-а, генерирайки тялото му при първа употреба */
static LLVMValueRef baga_rt(const char *name) {
    LLVMValueRef fn = LLVMGetNamedFunction(lg.mod, name);
    if (fn) return fn;
    LLVMBasicBlockRef saved = LLVMGetInsertBlock(lg.builder);
    if      (strcmp(name, "baga_len") == 0)         fn = build_baga_len();
    else if (strcmp(name, "baga_char_at") == 0)     fn = build_baga_char_at();
    else if (strcmp(name, "baga_substr") == 0)      fn = build_baga_substr();
    else if (strcmp(name, "baga_concat") == 0)      fn = build_baga_concat();
    else if (strcmp(name, "baga_str_eq") == 0)      fn = build_baga_str_eq();
    else if (strcmp(name, "baga_chr") == 0)         fn = build_baga_chr();
    else if (strcmp(name, "baga_ord") == 0)         fn = build_baga_ord();
    else if (strcmp(name, "baga_read_file") == 0)   fn = build_baga_read_file();
    else if (strcmp(name, "baga_vec_grow") == 0)    fn = build_baga_vec_grow();
    else if (strcmp(name, "baga_vec_new") == 0)     fn = build_baga_vec_new();
    else if (strcmp(name, "baga_vec_push_i64") == 0) fn = build_baga_vec_push_i64();
    else if (strcmp(name, "baga_vec_push_str") == 0) fn = build_baga_vec_push_str();
    else if (strcmp(name, "baga_vec_get_i64") == 0) fn = build_baga_vec_get_i64();
    else if (strcmp(name, "baga_vec_get_str") == 0) fn = build_baga_vec_get_str();
    else if (strcmp(name, "baga_vec_set_i64") == 0) fn = build_baga_vec_set_i64();
    else if (strcmp(name, "baga_vec_set_str") == 0) fn = build_baga_vec_set_str();
    else if (strcmp(name, "baga_vec_push_f64") == 0) fn = build_baga_vec_push_f64();
    else if (strcmp(name, "baga_vec_get_f64") == 0) fn = build_baga_vec_get_f64();
    else if (strcmp(name, "baga_vec_set_f64") == 0) fn = build_baga_vec_set_f64();
    else if (strcmp(name, "baga_vec_len") == 0)     fn = build_baga_vec_len();
    else if (strcmp(name, "baga_vec_push_box") == 0) fn = build_baga_vec_push_box();
    else if (strcmp(name, "baga_vec_get_box") == 0) fn = build_baga_vec_get_box();
    else if (strcmp(name, "baga_vec_set_box") == 0) fn = build_baga_vec_set_box();
    else if (strcmp(name, "baga_vec_slice_box") == 0) fn = build_baga_vec_slice_box();
    else if (strcmp(name, "baga_vec_concat_box") == 0) fn = build_baga_vec_concat_box();
    else if (strcmp(name, "baga_vec_slice_i64") == 0) fn = build_baga_vec_slice("i64", "baga_vec_push_i64");
    else if (strcmp(name, "baga_vec_slice_str") == 0) fn = build_baga_vec_slice("str", "baga_vec_push_str");
    else if (strcmp(name, "baga_vec_slice_f64") == 0) fn = build_baga_vec_slice("f64", "baga_vec_push_f64");
    else if (strcmp(name, "baga_vec_concat_i64") == 0) fn = build_baga_vec_concat("i64", "baga_vec_push_i64");
    else if (strcmp(name, "baga_vec_concat_str") == 0) fn = build_baga_vec_concat("str", "baga_vec_push_str");
    else if (strcmp(name, "baga_vec_concat_f64") == 0) fn = build_baga_vec_concat("f64", "baga_vec_push_f64");
    else if (strcmp(name, "baga_bytes_len") == 0)   fn = build_baga_bytes_len();
    else if (strcmp(name, "baga_bytes_at") == 0)    fn = build_baga_bytes_at();
    else if (strcmp(name, "baga_bytes_slice") == 0) fn = build_baga_bytes_slice();
    else if (strcmp(name, "baga_bytes_concat") == 0) fn = build_baga_bytes_concat();
    else if (strcmp(name, "baga_str_h") == 0)      fn = build_baga_str_h();
    else if (strcmp(name, "baga_h_str") == 0)      fn = build_baga_h_str();
    else if (strcmp(name, "baga_bytes_put") == 0)  fn = build_baga_bytes_put();
    else if (strcmp(name, "baga_bytes_h") == 0)    fn = build_baga_bytes_h();
    else if (strcmp(name, "baga_h_bytes") == 0)    fn = build_baga_h_bytes();
    else if (strcmp(name, "baga_bounds_fail") == 0) fn = build_baga_bounds_fail();
    else if (strcmp(name, "baga_bytes_new") == 0)  fn = build_baga_bytes_new();
    else if (strcmp(name, "baga_bytes_set") == 0)  fn = build_baga_bytes_set();
    else if (strcmp(name, "baga_bytes_push") == 0) fn = build_baga_bytes_push();
    else if (strcmp(name, "baga_bytes_from_str") == 0) fn = build_baga_bytes_from_str();
    else if (strcmp(name, "baga_bytes_to_str") == 0) fn = build_baga_bytes_to_str();
    else if (strcmp(name, "baga_hex_val") == 0)     fn = build_baga_hex_val();
    else if (strcmp(name, "baga_hex_encode") == 0)  fn = build_baga_hex_encode();
    else if (strcmp(name, "baga_hex_decode") == 0)  fn = build_baga_hex_decode();
    else if (strcmp(name, "baga_bytes_from_hex") == 0) fn = build_baga_bytes_from_hex();
    else if (strcmp(name, "baga_arena_new") == 0)   fn = build_baga_arena_new();
    else if (strcmp(name, "baga_arena_alloc") == 0) fn = build_baga_arena_alloc();
    else if (strcmp(name, "baga_arena_reset") == 0) fn = build_baga_arena_reset();
    else if (strcmp(name, "baga_arena_free") == 0)  fn = build_baga_arena_free();
    else if (strcmp(name, "baga_arg_count") == 0)   fn = build_baga_arg_count();
    else if (strcmp(name, "baga_arg") == 0)         fn = build_baga_arg();
    else if (strcmp(name, "baga_exit") == 0)        fn = build_baga_exit();
    else if (strcmp(name, "baga_eprintln") == 0)    fn = build_baga_eprintln();
    else if (strcmp(name, "baga_i64_to_str") == 0)  fn = build_baga_i64_to_str();
    else if (strcmp(name, "baga_map_hash_str") == 0) fn = build_baga_map_hash_str();
    else if (strcmp(name, "baga_map_hash_i64") == 0) fn = build_baga_map_hash_i64();
    else if (strcmp(name, "baga_map_hash_bytes") == 0) fn = build_baga_map_hash_bytes();
    else if (strcmp(name, "baga_map_new") == 0)     fn = build_baga_map_new();
    else if (strcmp(name, "baga_map_len") == 0)     fn = build_baga_map_len();
    else if (strcmp(name, "baga_map_slot") == 0)    fn = build_baga_map_slot();
    else if (strcmp(name, "baga_map_slot_b") == 0)  fn = build_baga_map_slot_b();
    else if (strcmp(name, "baga_map_put") == 0)     fn = build_baga_map_put();
    else if (strcmp(name, "baga_map_put_b") == 0)   fn = build_baga_map_put_b();
    else if (strcmp(name, "baga_map_rehash") == 0)  fn = build_baga_map_rehash();
    else if (strcmp(name, "baga_map_h") == 0)       fn = build_baga_map_h();
    else if (strcmp(name, "baga_h_map") == 0)       fn = build_baga_h_map();
    /* typed ops: baga_map_{set,get}_<key>_<val> — суфиксите се декодират */
    else if (strncmp(name, "baga_map_set_", 13) == 0 ||
             strncmp(name, "baga_map_get_", 13) == 0) {
        int klen = 0, key, val;
        key = map_key_kind(name + 13, &klen);
        val = key >= 0 && name[13 + klen] == '_'
            ? map_val_kind(name + 13 + klen + 1) : -1;
        if (key < 0 || val < 0) {
            char buf[128];
            snprintf(buf, sizeof buf, "runtime helper '%s'", name);
            llvm_unsupported(buf);
        }
        fn = name[9] == 's' ? build_baga_map_set(name, key, val)
                            : build_baga_map_get(name, key, val);
    }
    else if (strncmp(name, "baga_map_has_", 13) == 0 ||
             strncmp(name, "baga_map_del_", 13) == 0) {
        int klen = 0, key = map_key_kind(name + 13, &klen);
        if (key < 0 || name[13 + klen] != '\0') {
            char buf[128];
            snprintf(buf, sizeof buf, "runtime helper '%s'", name);
            llvm_unsupported(buf);
        }
        fn = name[9] == 'h' ? build_baga_map_has(name, key)
                            : build_baga_map_del(name, key);
    }
    else if (strncmp(name, "baga_map_keys_", 14) == 0) {
        int klen = 0, key = map_key_kind(name + 14, &klen);
        if (key < 0 || name[14 + klen] != '\0') {
            char buf[128];
            snprintf(buf, sizeof buf, "runtime helper '%s'", name);
            llvm_unsupported(buf);
        }
        fn = build_baga_map_keys(name, key);
    }
    else {
        char buf[128];
        snprintf(buf, sizeof buf, "runtime helper '%s'", name);
        llvm_unsupported(buf);
    }
    if (saved) LLVMPositionBuilderAtEnd(lg.builder, saved);
    return fn;
}

/* ---- Expression emission ---- */

static LLVMValueRef emit_expr_llvm(Node *n);
static void emit_stmt_llvm(Node *n, LLVMBasicBlockRef break_bb, LLVMBasicBlockRef cont_bb);

/* ---- L5: fn стойности (closures) ----
 * Огледало на C бекенда: handle = i64 cell2(code, env); cell2 живее в par
 * runtime-а (libbaga_par.so). Именувана функция като стойност взима адреса
 * на лениво генериран wrapper `<mangled>__clo(env, params...)`; ламбдата
 * получава собствен wrapper + env struct с копия на captures. */

static LLVMValueRef rt_cell2(void) {
    LLVMTypeRef p[] = { lg.i64_ty, lg.i64_ty };
    return rt_libc("baga_cell2", lg.i64_ty, p, 2);
}
static LLVMValueRef rt_cell2_0(void) {
    LLVMTypeRef p[] = { lg.i64_ty };
    return rt_libc("baga_cell2_0", lg.i64_ty, p, 1);
}
static LLVMValueRef rt_cell2_1(void) {
    LLVMTypeRef p[] = { lg.i64_ty };
    return rt_libc("baga_cell2_1", lg.i64_ty, p, 1);
}

/* handle = cell2(ptrtoint(code), ptrtoint(env)) */
static LLVMValueRef closure_handle(LLVMValueRef code, LLVMValueRef env) {
    LLVMValueRef ci = LLVMBuildPtrToInt(lg.builder, code, lg.i64_ty, "code");
    LLVMValueRef ei = LLVMBuildPtrToInt(lg.builder, env, lg.i64_ty, "env");
    LLVMValueRef pa[] = { ci, ei };
    return h_call(rt_cell2(), pa, 2, "clo");
}

/* NODE_FN по име (NULL ако липсва) */
static Node *find_user_fn(const char *name) {
    if (!lg.program) return NULL;
    for (int i = 0; i < lg.program->items.len; i++) {
        Node *it = lg.program->items.data[i];
        if (it->kind == NODE_FN && !it->is_extern && it->fn_name &&
            strcmp(it->fn_name, name) == 0)
            return it;
    }
    return NULL;
}

/* fn pointer тип за closure вик: ret(i8* env, params...) върху Type (TYPE_FN) */
static LLVMTypeRef closure_fn_ty(Type *ft, LLVMTypeRef **params_out) {
    int np = ft && ft->kind == TYPE_FN ? ft->nparams : 0;
    LLVMTypeRef *pt = malloc(sizeof(LLVMTypeRef) * (size_t)(np + 1));
    pt[0] = lg.ptr_ty;   /* env */
    for (int i = 0; i < np; i++)
        pt[i + 1] = ft->params[i] ? llvm_type_resolved(ft->params[i]) : lg.i64_ty;
    LLVMTypeRef ret = ft && ft->ret ? llvm_type_resolved(ft->ret) : lg.void_ty;
    if (params_out) *params_out = pt;
    LLVMTypeRef fty = LLVMFunctionType(ret, pt, (unsigned)(np + 1), 0);
    if (!params_out) free(pt);
    return fty;
}

/* wrapper за именувана fn като стойност: вика публичното име (spec-обвивката,
 * ако има такава) — като __clo в codegen_c. Ленив, по веднъж на функция. */
static LLVMValueRef closure_wrapper_named(const char *fn_name) {
    char *m = llvm_mangle(fn_name);
    char wn[600];
    snprintf(wn, sizeof wn, "%s__clo", m);
    LLVMValueRef ex = LLVMGetNamedFunction(lg.mod, wn);
    if (ex) { free(m); return ex; }
    Node *fn = find_user_fn(fn_name);
    if (!fn) {
        char buf[256];
        snprintf(buf, sizeof buf, "fn стойност на непозната функция '%s'", fn_name);
        llvm_unsupported(buf);
    }
    LLVMValueRef target = LLVMGetNamedFunction(lg.mod, m);
    if (!target) {
        free(m);
        llvm_unsupported("fn стойност преди декларация на функцията");
    }
    int np = fn->params.len;
    LLVMTypeRef *pt = malloc(sizeof(LLVMTypeRef) * (size_t)(np + 1));
    pt[0] = lg.ptr_ty;
    for (int i = 0; i < np; i++)
        pt[i + 1] = llvm_type(fn->params.data[i]->param_type);
    LLVMTypeRef ret = fn->ret_type ? llvm_type(fn->ret_type) : lg.void_ty;
    LLVMTypeRef fty = LLVMFunctionType(ret, pt, (unsigned)(np + 1), 0);
    free(pt);
    LLVMValueRef wrap = LLVMAddFunction(lg.mod, wn, fty);
    LLVMBasicBlockRef saved = LLVMGetInsertBlock(lg.builder);
    LLVMBasicBlockRef entry = LLVMAppendBasicBlockInContext(lg.ctx, wrap, "entry");
    LLVMPositionBuilderAtEnd(lg.builder, entry);
    LLVMValueRef *args = malloc(sizeof(LLVMValueRef) * (size_t)(np > 0 ? np : 1));
    for (int i = 0; i < np; i++)
        args[i] = LLVMGetParam(wrap, (unsigned)(i + 1));   /* env се пропуска */
    LLVMTypeRef target_ty = LLVMGetElementType(LLVMTypeOf(target));
    LLVMValueRef r = LLVMBuildCall2(lg.builder, target_ty, target, args,
                                    (unsigned)np, ret == lg.void_ty ? "" : "r");
    free(args);
    if (ret == lg.void_ty) LLVMBuildRetVoid(lg.builder);
    else                   LLVMBuildRet(lg.builder, r);
    free(m);
    if (saved) LLVMPositionBuilderAtEnd(lg.builder, saved);
    return wrap;
}

/* wrapper за ламбда: <mangled>__clo(i8* env, params...); captures се
 * разопаковат като локални копия от env struct-а. Ленив, по веднъж на възел. */
static LLVMValueRef emit_lambda_wrapper(Node *n, LLVMTypeRef env_ty) {
    char *m = llvm_mangle(n->fn_name ? n->fn_name : "__lam_x");
    char wn[600];
    snprintf(wn, sizeof wn, "%s__clo", m);
    free(m);
    LLVMValueRef ex = LLVMGetNamedFunction(lg.mod, wn);
    if (ex) return ex;
    int np = n->params.len;
    LLVMTypeRef *pt = malloc(sizeof(LLVMTypeRef) * (size_t)(np + 1));
    pt[0] = lg.ptr_ty;
    for (int i = 0; i < np; i++)
        pt[i + 1] = llvm_type(n->params.data[i]->param_type);
    LLVMTypeRef ret = n->ret_type ? llvm_type(n->ret_type) : lg.void_ty;
    LLVMTypeRef fty = LLVMFunctionType(ret, pt, (unsigned)(np + 1), 0);
    free(pt);
    LLVMValueRef fn = LLVMAddFunction(lg.mod, wn, fty);

    LLVMBasicBlockRef saved = LLVMGetInsertBlock(lg.builder);
    LLVMTypeRef saved_ret = lg.cur_ret_ty;
    lg.cur_ret_ty = ret;
    LLVMBasicBlockRef entry = LLVMAppendBasicBlockInContext(lg.ctx, fn, "entry");
    LLVMPositionBuilderAtEnd(lg.builder, entry);
    st_push();

    /* captures → локални копия от env */
    if (n->captures.len > 0) {
        LLVMValueRef envp = LLVMBuildBitCast(lg.builder, LLVMGetParam(fn, 0),
            LLVMPointerType(env_ty, 0), "envp");
        for (int i = 0; i < n->captures.len; i++) {
            Node *cap = n->captures.data[i];
            LLVMTypeRef ct = llvm_type_resolved(cap->type);
            LLVMValueRef gep = LLVMBuildStructGEP2(lg.builder, env_ty, envp,
                (unsigned)i, "cp");
            LLVMValueRef v = LLVMBuildLoad2(lg.builder, ct, gep, "cv");
            LLVMValueRef alloca = LLVMBuildAlloca(lg.builder, ct, cap->param_name);
            LLVMBuildStore(lg.builder, v, alloca);
            st_define(cap->param_name, alloca);
        }
    }
    /* параметри → alloca (като emit_fn_llvm) */
    for (int i = 0; i < np; i++) {
        Node *p = n->params.data[i];
        LLVMValueRef param = LLVMGetParam(fn, (unsigned)(i + 1));
        LLVMValueRef alloca = LLVMBuildAlloca(lg.builder, LLVMTypeOf(param),
            p->param_name);
        LLVMBuildStore(lg.builder, param, alloca);
        st_define(p->param_name, alloca);
    }
    /* тяло — огледало на emit_fn_llvm опашката */
    int has_ret = n->ret_type != NULL;
    NodeVec *stmts = n->fn_body ? &n->fn_body->stmts : NULL;
    for (int i = 0; stmts && i < stmts->len; i++) {
        Node *s = stmts->data[i];
        if (LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(lg.builder)))
            break;
        if (has_ret && i == stmts->len - 1 && s->kind == NODE_EXPR_STMT) {
            LLVMValueRef v = emit_expr_llvm(s->expr);
            if (!v) llvm_unsupported("лямбда: неявен return");
            LLVMBuildRet(lg.builder, coerce(v, ret));
        } else {
            emit_stmt_llvm(s, NULL, NULL);
        }
    }
    st_pop();
    if (!LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(lg.builder))) {
        if (ret == lg.void_ty) LLVMBuildRetVoid(lg.builder);
        else                   LLVMBuildRet(lg.builder, LLVMConstNull(ret));
    }
    lg.cur_ret_ty = saved_ret;
    if (saved) LLVMPositionBuilderAtEnd(lg.builder, saved);
    return fn;
}

static LLVMValueRef emit_binop_llvm(BinOp op, LLVMValueRef left, LLVMValueRef right) {
    char *name = tmp_name();
    LLVMValueRef result = NULL;
    int isdbl = LLVMGetTypeKind(LLVMTypeOf(left)) == LLVMDoubleTypeKind;
    switch (op) {
        case OP_ADD: result = isdbl ? LLVMBuildFAdd(lg.builder, left, right, name)
                                    : LLVMBuildAdd(lg.builder, left, right, name); break;
        case OP_SUB: result = isdbl ? LLVMBuildFSub(lg.builder, left, right, name)
                                    : LLVMBuildSub(lg.builder, left, right, name); break;
        case OP_MUL: result = isdbl ? LLVMBuildFMul(lg.builder, left, right, name)
                                    : LLVMBuildMul(lg.builder, left, right, name); break;
        case OP_DIV: result = isdbl ? LLVMBuildFDiv(lg.builder, left, right, name)
                                    : LLVMBuildSDiv(lg.builder, left, right, name); break;
        case OP_MOD: result = isdbl ? LLVMBuildFRem(lg.builder, left, right, name)
                                    : LLVMBuildSRem(lg.builder, left, right, name); break;
        case OP_EQ:  result = isdbl ? LLVMBuildFCmp(lg.builder, LLVMRealOEQ, left, right, name)
                                    : LLVMBuildICmp(lg.builder, LLVMIntEQ, left, right, name); break;
        case OP_NEQ: result = isdbl ? LLVMBuildFCmp(lg.builder, LLVMRealONE, left, right, name)
                                    : LLVMBuildICmp(lg.builder, LLVMIntNE, left, right, name); break;
        case OP_LT:  result = isdbl ? LLVMBuildFCmp(lg.builder, LLVMRealOLT, left, right, name)
                                    : LLVMBuildICmp(lg.builder, LLVMIntSLT, left, right, name); break;
        case OP_GT:  result = isdbl ? LLVMBuildFCmp(lg.builder, LLVMRealOGT, left, right, name)
                                    : LLVMBuildICmp(lg.builder, LLVMIntSGT, left, right, name); break;
        case OP_LE:  result = isdbl ? LLVMBuildFCmp(lg.builder, LLVMRealOLE, left, right, name)
                                    : LLVMBuildICmp(lg.builder, LLVMIntSLE, left, right, name); break;
        case OP_GE:  result = isdbl ? LLVMBuildFCmp(lg.builder, LLVMRealOGE, left, right, name)
                                    : LLVMBuildICmp(lg.builder, LLVMIntSGE, left, right, name); break;
        case OP_BIT_AND:
            if (isdbl) llvm_unsupported("побитово & върху f64");
            result = LLVMBuildAnd(lg.builder, left, right, name); break;
        case OP_BIT_OR:
            if (isdbl) llvm_unsupported("побитово | върху f64");
            result = LLVMBuildOr(lg.builder, left, right, name); break;
        case OP_BIT_XOR:
            if (isdbl) llvm_unsupported("побитово ^ върху f64");
            result = LLVMBuildXor(lg.builder, left, right, name); break;
        case OP_LSHIFT:
            if (isdbl) llvm_unsupported("<< върху f64");
            result = LLVMBuildShl(lg.builder, left, right, name); break;
        case OP_RSHIFT:
            if (isdbl) llvm_unsupported(">> върху f64");
            result = LLVMBuildAShr(lg.builder, left, right, name); break;
        default:     llvm_unsupported("непознат бинарен оператор"); break;
    }
    free(name);
    return result;
}

/* print/println/write — изходът е байт за байт като codegen_c:
 * i64 → "%lld\n", f64 → "%g\n", bool → "true"/"false", str → "%s\n".
 * write върху str е без нов ред. */
static void emit_print_llvm(Node *n) {
    int is_write = strcmp(n->callee->name, "write") == 0;

    if (n->args.len == 0) {
        LLVMValueRef fmt = LLVMBuildGlobalStringPtr(lg.builder, "\n", "fmt");
        LLVMValueRef args[] = { fmt };
        LLVMBuildCall2(lg.builder, LLVMGetElementType(LLVMTypeOf(lg.printf_fn)),
                       lg.printf_fn, args, 1, "");
        return;
    }

    for (int i = 0; i < n->args.len; i++) {
        Node *arg = n->args.data[i];
        LLVMValueRef val;
        const char *fmt_str;
        LLVMValueRef extra[2];
        int nextra = 0;

        if (arg->kind == NODE_STR_LIT) {
            val = LLVMBuildGlobalStringPtr(lg.builder, arg->str_val, "str");
            fmt_str = is_write ? "%s" : "%s\n";
            extra[nextra++] = val;
        } else {
            val = emit_expr_llvm(arg);
            if (!val) llvm_unsupported("print върху void стойност");
            LLVMTypeRef t = LLVMTypeOf(val);
            if (LLVMGetTypeKind(t) == LLVMPointerTypeKind) {
                fmt_str = is_write ? "%s" : "%s\n";
                extra[nextra++] = val;
            } else if (LLVMGetTypeKind(t) == LLVMDoubleTypeKind) {
                fmt_str = "%g\n";
                extra[nextra++] = val;
            } else if (t == lg.i1_ty) {
                LLVMValueRef ts = LLVMBuildGlobalStringPtr(lg.builder, "true", "str");
                LLVMValueRef fs = LLVMBuildGlobalStringPtr(lg.builder, "false", "str");
                char *name = tmp_name();
                LLVMValueRef sel = LLVMBuildSelect(lg.builder, val, ts, fs, name);
                free(name);
                fmt_str = "%s\n";
                extra[nextra++] = sel;
            } else if (LLVMGetTypeKind(t) == LLVMIntegerTypeKind) {
                fmt_str = "%lld\n";
                extra[nextra++] = coerce(val, lg.i64_ty);
            } else {
                llvm_unsupported("print върху този тип");
                return; /* unreachable */
            }
        }

        LLVMValueRef fmt = LLVMBuildGlobalStringPtr(lg.builder, fmt_str, "fmt");
        LLVMValueRef args[3];
        args[0] = fmt;
        for (int j = 0; j < nextra; j++) args[1 + j] = extra[j];
        LLVMBuildCall2(lg.builder, LLVMGetElementType(LLVMTypeOf(lg.printf_fn)),
                       lg.printf_fn, args, (unsigned)(1 + nextra), "");
    }
}

static int is_print_call_llvm(Node *n) {
    if (n->kind != NODE_CALL) return 0;
    if (n->callee->kind != NODE_IDENT) return 0;
    return strcmp(n->callee->name, "print") == 0 ||
           strcmp(n->callee->name, "println") == 0 ||
           strcmp(n->callee->name, "write") == 0;
}

/* enum вариант → i64 константа (индексът в декларацията); -1 ако не е вариант */
static int find_enum_variant(const char *name) {
    if (!lg.program) return -1;
    for (int i = 0; i < lg.program->items.len; i++) {
        Node *item = lg.program->items.data[i];
        if (item->kind != NODE_ENUM) continue;
        for (int j = 0; j < item->n_variants; j++)
            if (strcmp(item->enum_variants[j], name) == 0)
                return j;
    }
    return -1;
}

/* Тяло на match клон — огледало на codegen_c: голият израз е опакован
 * от parser-а като BLOCK с RETURN; RETURN със стойност записва резултата,
 * EXPR_STMT се изпълнява за страничен ефект. */
static void emit_match_arm_llvm(Node *arm, LLVMValueRef res_alloca,
                                LLVMBasicBlockRef merge_bb) {
    Node *body = arm->arm_body;
    if (!body || body->kind != NODE_BLOCK)
        llvm_unsupported("match клон, който не е блок");
    for (int j = 0; j < body->stmts.len; j++) {
        Node *s = body->stmts.data[j];
        if (s->kind == NODE_RETURN) {
            if (s->ret_val) {
                /* void match: стойността се оценява за страничния ефект
                 * (codegen_c emit-ва израза и без _mr присвояване) */
                LLVMValueRef v = emit_expr_llvm(s->ret_val);
                if (res_alloca) {
                    if (!v) llvm_unsupported("print в match клон");
                    v = coerce(v, LLVMGetAllocatedType(res_alloca));
                    LLVMBuildStore(lg.builder, v, res_alloca);
                }
            }
            LLVMBuildBr(lg.builder, merge_bb);
            return;
        }
        if (s->kind == NODE_EXPR_STMT) {
            emit_expr_llvm(s->expr);
            continue;
        }
        llvm_unsupported("оператор в match клон (само изрази)");
    }
}

/* L3: match върху sum enum — верига от tag сравнения (като codegen_c);
 * payload binding-ът е alloca в scope-а на клона (st_push/st_pop). */
static LLVMValueRef emit_match_sum_llvm(Node *n, Node *ed) {
    LLVMTypeRef ety = user_struct_ty(ed->enum_name);
    LLVMValueRef mv = emit_expr_llvm(n->match_expr);
    if (!mv) llvm_unsupported("print като match subject");
    /* subject-ът се оценява веднъж в temp — тагът и payload-овете се
     * четат от него (като _mv в codegen_c) */
    LLVMValueRef mva = entry_alloca(ety, "mv");
    LLVMBuildStore(lg.builder, mv, mva);
    LLVMValueRef tagp = LLVMBuildStructGEP2(lg.builder, ety, mva, 0, "tagp");
    LLVMValueRef tag = LLVMBuildLoad2(lg.builder, lg.i64_ty, tagp, "tag");

    LLVMTypeRef res_ty = n->type ? llvm_type_resolved(n->type) : lg.i64_ty;
    LLVMValueRef res_alloca = NULL;
    if (res_ty != lg.void_ty) {
        res_alloca = entry_alloca(res_ty, "match_res");
        LLVMBuildStore(lg.builder, LLVMConstNull(res_ty), res_alloca);
    }

    LLVMValueRef fn = LLVMGetBasicBlockParent(LLVMGetInsertBlock(lg.builder));
    LLVMBasicBlockRef merge_bb = LLVMAppendBasicBlockInContext(lg.ctx, fn, "match_end");

    for (int i = 0; i < n->match_arms.len; i++) {
        Node *arm = n->match_arms.data[i];
        if (arm->arm_pattern) {
            const char *pvn = arm->arm_pattern->kind == NODE_PATH
                ? arm->arm_pattern->path_variant
                : arm->arm_pattern->kind == NODE_IDENT
                ? arm->arm_pattern->name : NULL;
            int vidx = sum_variant_index(ed, pvn);
            if (vidx < 0)
                llvm_unsupported("match клон, който не е вариант на sum enum");
            LLVMValueRef cond = LLVMBuildICmp(lg.builder, LLVMIntEQ, tag,
                LLVMConstInt(lg.i64_ty, (unsigned long long)vidx, 0), "mcond");
            LLVMBasicBlockRef arm_bb = LLVMAppendBasicBlockInContext(lg.ctx, fn, "match_arm");
            LLVMBasicBlockRef next_bb = LLVMAppendBasicBlockInContext(lg.ctx, fn, "match_next");
            LLVMBuildCondBr(lg.builder, cond, arm_bb, next_bb);
            LLVMPositionBuilderAtEnd(lg.builder, arm_bb);
            st_push();
            if (arm->arm_binding && ed->enum_payloads && ed->enum_payloads[vidx]) {
                LLVMTypeRef pty = llvm_type(ed->enum_payloads[vidx]);
                LLVMValueRef pv = LLVMBuildLoad2(lg.builder, pty,
                    sum_payload_ptr(ety, mva, pty), "pv");
                char *bm = llvm_mangle(arm->arm_binding);
                LLVMValueRef ba = entry_alloca(pty, bm);
                free(bm);
                LLVMBuildStore(lg.builder, pv, ba);
                st_define(arm->arm_binding, ba);
            }
            emit_match_arm_llvm(arm, res_alloca, merge_bb);
            st_pop();
            if (!LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(lg.builder)))
                LLVMBuildBr(lg.builder, merge_bb);
            LLVMPositionBuilderAtEnd(lg.builder, next_bb);
        } else {
            /* wildcard `_` → else клон */
            emit_match_arm_llvm(arm, res_alloca, merge_bb);
            if (!LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(lg.builder)))
                LLVMBuildBr(lg.builder, merge_bb);
        }
    }

    /* без wildcard: последният match_next пада в merge (както codegen_c) */
    if (!LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(lg.builder)))
        LLVMBuildBr(lg.builder, merge_bb);
    LLVMPositionBuilderAtEnd(lg.builder, merge_bb);
    if (!res_alloca) return NULL;
    char *name = tmp_name();
    LLVMValueRef r = LLVMBuildLoad2(lg.builder, res_ty, res_alloca, name);
    free(name);
    return r;
}

static LLVMValueRef emit_match_llvm(Node *n) {
    /* L3: match върху sum enum → tag верига с payload bindings */
    if (n->match_expr && n->match_expr->type &&
        n->match_expr->type->kind == TYPE_ENUM) {
        Node *ed = find_sum_enum(n->match_expr->type->name);
        if (ed) return emit_match_sum_llvm(n, ed);
    }
    LLVMValueRef mv = emit_expr_llvm(n->match_expr);
    if (!mv || LLVMGetTypeKind(LLVMTypeOf(mv)) != LLVMIntegerTypeKind)
        llvm_unsupported("match върху не-целочислена стойност");
    mv = coerce(mv, lg.i64_ty);

    LLVMTypeRef res_ty = n->type ? llvm_type_resolved(n->type) : lg.i64_ty;
    LLVMValueRef res_alloca = NULL;
    if (res_ty != lg.void_ty) {
        res_alloca = entry_alloca(res_ty, "match_res");
        /* codegen_c инициализира _mr = 0 */
        LLVMBuildStore(lg.builder, LLVMConstNull(res_ty), res_alloca);
    }

    LLVMValueRef fn = LLVMGetBasicBlockParent(LLVMGetInsertBlock(lg.builder));
    LLVMBasicBlockRef merge_bb = LLVMAppendBasicBlockInContext(lg.ctx, fn, "match_end");

    for (int i = 0; i < n->match_arms.len; i++) {
        Node *arm = n->match_arms.data[i];
        if (arm->arm_pattern) {
            LLVMValueRef pat = emit_expr_llvm(arm->arm_pattern);
            if (!pat) llvm_unsupported("print в match pattern");
            pat = coerce(pat, lg.i64_ty);
            char *name = tmp_name();
            LLVMValueRef cond = LLVMBuildICmp(lg.builder, LLVMIntEQ, mv, pat, name);
            free(name);
            LLVMBasicBlockRef arm_bb = LLVMAppendBasicBlockInContext(lg.ctx, fn, "match_arm");
            LLVMBasicBlockRef next_bb = LLVMAppendBasicBlockInContext(lg.ctx, fn, "match_next");
            LLVMBuildCondBr(lg.builder, cond, arm_bb, next_bb);
            LLVMPositionBuilderAtEnd(lg.builder, arm_bb);
            emit_match_arm_llvm(arm, res_alloca, merge_bb);
            if (!LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(lg.builder)))
                LLVMBuildBr(lg.builder, merge_bb);
            LLVMPositionBuilderAtEnd(lg.builder, next_bb);
        } else {
            /* wildcard `_` → else клон */
            emit_match_arm_llvm(arm, res_alloca, merge_bb);
            if (!LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(lg.builder)))
                LLVMBuildBr(lg.builder, merge_bb);
        }
    }

    /* без wildcard: последният match_next остава отворен — пада в merge
     * (както codegen_c: _mr остава 0 при непокрит случай) */
    if (!LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(lg.builder)))
        LLVMBuildBr(lg.builder, merge_bb);
    LLVMPositionBuilderAtEnd(lg.builder, merge_bb);
    if (!res_alloca) return NULL;
    char *name = tmp_name();
    LLVMValueRef r = LLVMBuildLoad2(lg.builder, res_ty, res_alloca, name);
    free(name);
    return r;
}

static LLVMValueRef emit_expr_llvm(Node *n) {
    if (!n) llvm_unsupported("празен израз");

    switch (n->kind) {
        case NODE_INT_LIT:
            return LLVMConstInt(lg.i64_ty, (unsigned long long)n->int_val, 1);

        case NODE_FLOAT_LIT:
            /* пълна точност — codegen_c също emit-ва с %.17g (round-trip) */
            return LLVMConstReal(lg.double_ty, n->float_val);

        case NODE_BOOL_LIT:
            return LLVMConstInt(lg.i1_ty, n->bool_val, 0);

        case NODE_BYTES_LIT: {
            LLVMValueRef s = LLVMBuildGlobalStringPtr(lg.builder, n->str_val, "hexlit");
            LLVMValueRef a[] = { s };
            return h_call(baga_rt("baga_bytes_from_hex"), a, 1, "blit");
        }

        case NODE_STR_LIT:
            return LLVMBuildGlobalStringPtr(lg.builder, n->str_val, "str");

        case NODE_IDENT: {
            /* L5: глобална fn като стойност → handle към __clo wrapper-а.
             * Локална fn-typed променлива има type->name == NULL или
             * различно име и минава през st_lookup (i64 handle). */
            if (n->type && n->type->kind == TYPE_FN && n->type->name &&
                strcmp(n->name, n->type->name) == 0) {
                LLVMValueRef wrap = closure_wrapper_named(n->type->name);
                return closure_handle(wrap, LLVMConstNull(lg.ptr_ty));
            }
            LLVMValueRef alloca = st_lookup(n->name);
            if (alloca) {
                char *name = tmp_name();
                LLVMValueRef v = LLVMBuildLoad2(lg.builder,
                    LLVMGetAllocatedType(alloca), alloca, name);
                free(name);
                return v;
            }
            /* L3: payload-less вариант на sum enum → { tag, 0 } стойност */
            if (n->type && n->type->kind == TYPE_ENUM) {
                Node *ed = find_sum_enum(n->type->name);
                int vidx = ed ? sum_variant_index(ed, n->name) : -1;
                if (vidx >= 0) return sum_variant_const(ed, vidx);
            }
            int variant = find_enum_variant(n->name);
            if (variant >= 0)
                return LLVMConstInt(lg.i64_ty, (unsigned long long)variant, 0);
            char buf[256];
            snprintf(buf, sizeof buf, "недефинирано име '%s'", n->name);
            llvm_unsupported(buf);
            return NULL; /* unreachable */
        }

        case NODE_BINARY: {
            /* && / || върху bool (i1); C backend-ът ползва && / || —
             * за чисти операнди резултатът е същият */
            if (n->bin_op == OP_AND || n->bin_op == OP_OR) {
                LLVMValueRef l = to_bool(emit_expr_llvm(n->left));
                LLVMValueRef r = to_bool(emit_expr_llvm(n->right));
                char *name = tmp_name();
                LLVMValueRef v = n->bin_op == OP_AND
                    ? LLVMBuildAnd(lg.builder, l, r, name)
                    : LLVMBuildOr(lg.builder, l, r, name);
                free(name);
                return v;
            }
            /* str == str / str != str — чрез baga_str_eq (като codegen_c: strcmp) */
            if ((n->bin_op == OP_EQ || n->bin_op == OP_NEQ) &&
                n->left->type && n->right->type &&
                n->left->type->kind == TYPE_STR && n->right->type->kind == TYPE_STR) {
                LLVMValueRef l = emit_expr_llvm(n->left);
                LLVMValueRef r = emit_expr_llvm(n->right);
                if (!l || !r) llvm_unsupported("print в сравнение на низове");
                LLVMValueRef sc = baga_rt("baga_str_eq");
                LLVMValueRef args[] = { l, r };
                char *nm = tmp_name();
                LLVMValueRef eq = h_call(sc, args, 2, nm);
                free(nm);
                /* baga_str_eq връща i64 0/1 → bool i1 */
                char *nm2 = tmp_name();
                LLVMValueRef v = LLVMBuildICmp(lg.builder, LLVMIntNE, eq,
                    LLVMConstInt(lg.i64_ty, 0, 0), nm2);
                free(nm2);
                if (n->bin_op == OP_NEQ) {
                    char *nm3 = tmp_name();
                    v = LLVMBuildNot(lg.builder, v, nm3);
                    free(nm3);
                }
                return v;
            }
            LLVMValueRef left = emit_expr_llvm(n->left);
            LLVMValueRef right = emit_expr_llvm(n->right);
            if (!left || !right) llvm_unsupported("print в аритметичен израз");
            /* f64 промоция: ако някой операнд е f64, и двата стават f64
             * (като C: int64_t + double → double) */
            int isdbl = LLVMGetTypeKind(LLVMTypeOf(left)) == LLVMDoubleTypeKind ||
                        LLVMGetTypeKind(LLVMTypeOf(right)) == LLVMDoubleTypeKind;
            if (isdbl) {
                left = coerce(left, lg.double_ty);
                right = coerce(right, lg.double_ty);
            } else if (LLVMTypeOf(left) != LLVMTypeOf(right)) {
                /* смесени целочислени ширини → по-широкият тип */
                unsigned wl = LLVMGetIntTypeWidth(LLVMTypeOf(left));
                unsigned wr = LLVMGetIntTypeWidth(LLVMTypeOf(right));
                LLVMTypeRef wide = wl >= wr ? LLVMTypeOf(left) : LLVMTypeOf(right);
                left = coerce(left, wide);
                right = coerce(right, wide);
            }
            return emit_binop_llvm(n->bin_op, left, right);
        }

        case NODE_UNARY: {
            LLVMValueRef v = emit_expr_llvm(n->operand);
            if (!v) llvm_unsupported("print в унарен израз");
            char *name = tmp_name();
            LLVMValueRef r = NULL;
            switch (n->un_op) {
                case UOP_NEG:
                    r = LLVMGetTypeKind(LLVMTypeOf(v)) == LLVMDoubleTypeKind
                      ? LLVMBuildFNeg(lg.builder, v, name)
                      : LLVMBuildNeg(lg.builder, v, name);
                    break;
                case UOP_NOT:
                    if (LLVMTypeOf(v) != lg.i1_ty)
                        llvm_unsupported("! върху не-булев израз");
                    r = LLVMBuildNot(lg.builder, v, name);
                    break;
                case UOP_REF:   llvm_unsupported("референция (&x)"); break;
                case UOP_DEREF: llvm_unsupported("дереференциране (*x)"); break;
            }
            free(name);
            return r;
        }

        case NODE_CALL: {
            /* L3: конструктор на sum enum — bare Ok(x) или Res::Ok(x).
             * Познава се по върнатия тип (TYPE_ENUM) + име на вариант;
             * обикновена fn, връщаща enum, минава нататък. */
            if (n->type && n->type->kind == TYPE_ENUM &&
                (n->callee->kind == NODE_IDENT || n->callee->kind == NODE_PATH)) {
                Node *ed = find_sum_enum(n->type->name);
                const char *vn = n->callee->kind == NODE_PATH
                    ? n->callee->path_variant : n->callee->name;
                int vidx = ed ? sum_variant_index(ed, vn) : -1;
                if (vidx >= 0) {
                    if (ed->enum_payloads && ed->enum_payloads[vidx]) {
                        if (n->args.len != 1)
                            llvm_unsupported("sum конструктор с != 1 аргумент");
                        LLVMValueRef a = emit_expr_llvm(n->args.data[0]);
                        if (!a) llvm_unsupported("print като payload");
                        LLVMTypeRef pty = llvm_type(ed->enum_payloads[vidx]);
                        a = coerce(a, pty);
                        LLVMValueRef cf = sum_ctor_fn(ed, vidx);
                        char *cn = tmp_name();
                        LLVMValueRef r = h_call(cf, &a, 1, cn);
                        free(cn);
                        return r;
                    }
                    /* payload-less вариант, извикан с () */
                    if (n->args.len == 0) return sum_variant_const(ed, vidx);
                }
            }
            /* extern fn → raw libc symbol, before the print/builtin dispatch
             * (an extern named `write` must not become baga_write) */
            Node *ef = NULL;
            if (n->callee->kind == NODE_IDENT) {
                for (int i = 0; lg.program && i < lg.program->items.len; i++) {
                    Node *it = lg.program->items.data[i];
                    if (it->kind == NODE_FN && it->is_extern &&
                        strcmp(it->fn_name, n->callee->name) == 0) {
                        ef = it;
                        break;
                    }
                }
            }
            if (!ef && is_print_call_llvm(n)) {
                emit_print_llvm(n);
                return NULL; /* print е void */
            }
            /* L5: извикване през fn стойност — handle = cell2(code, env)
             * (checker маркер: TYPE_FN без име; като codegen_c) */
            if (!ef && n->callee->type && n->callee->type->kind == TYPE_FN &&
                !n->callee->type->name) {
                Type *ft = n->callee->type;
                LLVMValueRef h = emit_expr_llvm(n->callee);
                if (!h) llvm_unsupported("fn стойност като callee");
                h = coerce(h, lg.i64_ty);
                LLVMValueRef ca[] = { h };
                LLVMValueRef code = h_call(rt_cell2_0(), ca, 1, "code");
                LLVMValueRef env  = h_call(rt_cell2_1(), ca, 1, "env");
                LLVMTypeRef *pt = NULL;
                LLVMTypeRef fty = closure_fn_ty(ft, &pt);
                LLVMValueRef fp = LLVMBuildIntToPtr(lg.builder, code,
                    LLVMPointerType(fty, 0), "fp");
                int np = ft->nparams;
                LLVMValueRef *args = malloc(sizeof(LLVMValueRef) * (size_t)(np + 1));
                args[0] = LLVMBuildIntToPtr(lg.builder, env, lg.ptr_ty, "envp");
                for (int i = 0; i < np; i++) {
                    LLVMValueRef a = emit_expr_llvm(n->args.data[i]);
                    if (!a) llvm_unsupported("fn повикване: аргумент");
                    args[i + 1] = coerce(a, pt[i + 1]);
                }
                free(pt);
                int is_void = LLVMGetReturnType(fty) == lg.void_ty;
                char *rn = is_void ? NULL : tmp_name();
                LLVMValueRef r = LLVMBuildCall2(lg.builder, fty, fp, args,
                    (unsigned)(np + 1), rn ? rn : "");
                free(rn);
                free(args);
                return r;
            }
            if (n->callee->kind != NODE_IDENT)
                llvm_unsupported("повикване през израз (не име)");
            /* !Par: external helpers from libbaga_par.so (src/baga_par_rt.c) */
            if (!ef && n->callee->kind == NODE_IDENT) {
                const char *cn = n->callee->name;
                LLVMTypeRef i64p1[] = { lg.i64_ty };
                LLVMTypeRef par_fn_ty = LLVMFunctionType(lg.i64_ty, i64p1, 1, 0);
                LLVMTypeRef par_fn_ptr = LLVMPointerType(par_fn_ty, 0);
                if ((strcmp(cn, "go") == 0 || strcmp(cn, "go_bg") == 0) &&
                    n->args.len == 2 && n->args.data[0]->kind == NODE_IDENT) {
                    char *wm = llvm_mangle(n->args.data[0]->name);
                    LLVMValueRef worker = LLVMGetNamedFunction(lg.mod, wm);
                    free(wm);
                    if (!worker) llvm_unsupported("go: worker не е намерена");
                    LLVMValueRef fp = LLVMBuildBitCast(lg.builder, worker, par_fn_ptr, "par_fp");
                    LLVMValueRef arg = emit_expr_llvm(n->args.data[1]);
                    if (!arg) llvm_unsupported("go: arg");
                    arg = coerce(arg, lg.i64_ty);
                    const char *rt = strcmp(cn, "go_bg") == 0 ? "baga_go_bg" : "baga_go";
                    LLVMTypeRef gp[] = { par_fn_ptr, lg.i64_ty };
                    LLVMValueRef gfn = rt_libc(rt, lg.i64_ty, gp, 2);
                    LLVMValueRef ga[] = { fp, arg };
                    return h_call(gfn, ga, 2, tmp_name());
                }
                if (strcmp(cn, "pool_map") == 0 && n->args.len == 3 &&
                    n->args.data[0]->kind == NODE_IDENT) {
                    char *wm = llvm_mangle(n->args.data[0]->name);
                    LLVMValueRef worker = LLVMGetNamedFunction(lg.mod, wm);
                    free(wm);
                    if (!worker) llvm_unsupported("pool_map: worker не е намерена");
                    LLVMValueRef fp = LLVMBuildBitCast(lg.builder, worker, par_fn_ptr, "par_fp");
                    LLVMValueRef vec = emit_expr_llvm(n->args.data[1]);
                    LLVMValueRef nw = emit_expr_llvm(n->args.data[2]);
                    if (!vec || !nw) llvm_unsupported("pool_map args");
                    nw = coerce(nw, lg.i64_ty);
                    LLVMTypeRef pp[] = { par_fn_ptr, baga_vec_ptr_ty(), lg.i64_ty };
                    LLVMValueRef pfn = rt_libc("baga_pool_map", baga_vec_ptr_ty(), pp, 3);
                    LLVMValueRef pa[] = { fp, vec, nw };
                    return h_call(pfn, pa, 3, tmp_name());
                }
                /* plain external baga_* par helpers (i64 args) */
                static const struct { const char *baga; const char *c; int n; } pmap[] = {
                    {"join", "baga_join", 1},
                    {"detach", "baga_detach", 1},
                    {"chan_new", "baga_chan_new", 1},
                    {"chan_send", "baga_chan_send", 2},
                    {"chan_recv", "baga_chan_recv", 1},
                    {"chan_recv2", "baga_chan_recv2", 1},
                    {"chan_try_recv", "baga_chan_try_recv", 1},
                    {"chan_recv_timeout", "baga_chan_recv_timeout", 2},
                    {"chan_select2", "baga_chan_select2", 2},
                    {"chan_select2_wait", "baga_chan_select2_wait", 2},
                    {"chan_select2_timeout", "baga_chan_select2_timeout", 3},
                    {"chan_close", "baga_chan_close", 1},
                    {"chan_len", "baga_chan_len", 1},
                    {"sleep_ms", "baga_sleep_ms", 1},
                    {"mutex_new", "baga_mutex_new", 0},
                    {"mutex_lock", "baga_mutex_lock", 1},
                    {"mutex_unlock", "baga_mutex_unlock", 1},
                    {"signal_watch", "baga_signal_watch", 1},
                    {"signal_check", "baga_signal_check", 0},
                    {"signal_clear", "baga_signal_clear", 0},
                    {"signal_wait", "baga_signal_wait", 1},
                    {"signal_raise", "baga_signal_raise", 1},
                    {"cell2", "baga_cell2", 2},
                    {"cell2_0", "baga_cell2_0", 1},
                    {"cell2_1", "baga_cell2_1", 1},
                };
                for (int pi = 0; pi < (int)(sizeof(pmap) / sizeof(pmap[0])); pi++) {
                    if (strcmp(cn, pmap[pi].baga) != 0) continue;
                    if (n->args.len != pmap[pi].n)
                        llvm_unsupported("!Par arnost");
                    LLVMTypeRef pts[4];
                    for (int k = 0; k < pmap[pi].n; k++) pts[k] = lg.i64_ty;
                    LLVMValueRef pfn = rt_libc(pmap[pi].c, lg.i64_ty, pts, pmap[pi].n);
                    LLVMValueRef pa[4];
                    for (int k = 0; k < pmap[pi].n; k++) {
                        LLVMValueRef a = emit_expr_llvm(n->args.data[k]);
                        if (!a) llvm_unsupported("!Par arg");
                        pa[k] = coerce(a, lg.i64_ty);
                    }
                    return h_call(pfn, pa, pmap[pi].n, tmp_name());
                }
            }
            /* str/io/vec builtin-и → baga_* IR helpers (като в codegen_c) */
            static const struct { const char *baga; const char *rt; } bmap[] = {
                {"len",         "baga_len"},
                {"char_at",     "baga_char_at"},
                {"substr",      "baga_substr"},
                {"concat",      "baga_concat"},
                {"read_file",   "baga_read_file"},
                {"chr",         "baga_chr"},
                {"ord",         "baga_ord"},
                {"str_eq",      "baga_str_eq"},
                {"vec_new",     "baga_vec_new"},
                {"vec_push_str","baga_vec_push_str"},
                {"vec_get_str", "baga_vec_get_str"},
                {"vec_set_str", "baga_vec_set_str"},
                {"vec_len",     "baga_vec_len"},
                {"bytes_len",   "baga_bytes_len"},
                {"bytes_at",    "baga_bytes_at"},
                {"bytes_slice", "baga_bytes_slice"},
                {"bytes_concat","baga_bytes_concat"},
                {"bytes_put",   "baga_bytes_put"},
                {"bytes_new",   "baga_bytes_new"},
                {"bytes_set",   "baga_bytes_set"},
                {"bytes_push",  "baga_bytes_push"},
                {"str_h",       "baga_str_h"},
                {"h_str",       "baga_h_str"},
                {"bytes_h",     "baga_bytes_h"},
                {"h_bytes",     "baga_h_bytes"},
                {"bytes_of_str","baga_bytes_from_str"},
                {"str_of_bytes","baga_bytes_to_str"},
                {"hex_encode",  "baga_hex_encode"},
                {"hex_decode",  "baga_hex_decode"},
                {"arena_new",   "baga_arena_new"},
                {"arena_alloc", "baga_arena_alloc"},
                {"arena_reset", "baga_arena_reset"},
                {"arena_free",  "baga_arena_free"},
                {"arg_count",   "baga_arg_count"},
                {"arg",         "baga_arg"},
                {"exit",        "baga_exit"},
                {"eprintln",    "baga_eprintln"},
                {"map_new",     "baga_map_new"},
                {"map_len",     "baga_map_len"},
                {"map_h",       "baga_map_h"},
                {"h_map",       "baga_h_map"},
            };
            LLVMValueRef fn = NULL;
            if (ef) fn = LLVMGetNamedFunction(lg.mod, ef->fn_name);
            /* типизирани вектори: helper по елементния тип на вектора */
            if (!ef &&
                (strcmp(n->callee->name, "vec_push") == 0 ||
                 strcmp(n->callee->name, "vec_get") == 0 ||
                 strcmp(n->callee->name, "vec_set") == 0 ||
                 strcmp(n->callee->name, "vec_slice") == 0 ||
                 strcmp(n->callee->name, "vec_concat") == 0)) {
                Type *vt = n->args.len > 0 ? n->args.data[0]->type : NULL;
                /* L4/S2/L3: struct, bytes и sum enum елементи → box
                 * helper-и, sizeof от call site-а (огледало на codegen_c) */
                if (vt && vt->kind == TYPE_VEC && vt->elem &&
                    ((vt->elem->kind == TYPE_STRUCT && vt->elem->name) ||
                     (vt->elem->kind == TYPE_ENUM && vt->elem->name) ||
                     vt->elem->kind == TYPE_BYTES)) {
                    LLVMTypeRef sty = vt->elem->kind == TYPE_BYTES
                        ? baga_bytes_ty() : user_struct_ty(vt->elem->name);
                    LLVMValueRef sz = LLVMSizeOf(sty);
                    const char *cn = n->callee->name;
                    LLVMValueRef v = emit_expr_llvm(n->args.data[0]);
                    if (!v) llvm_unsupported("vec аргумент");
                    if (strcmp(cn, "vec_push") == 0) {
                        LLVMValueRef e = emit_expr_llvm(n->args.data[1]);
                        if (!e) llvm_unsupported("vec_push аргумент");
                        LLVMValueRef tmp = entry_alloca(sty, "bx");
                        LLVMBuildStore(lg.builder, e, tmp);
                        LLVMValueRef bp = LLVMBuildBitCast(lg.builder, tmp, lg.ptr_ty, "bp");
                        LLVMValueRef pa[] = { v, bp, sz };
                        return h_call(baga_rt("baga_vec_push_box"), pa, 3, "");
                    }
                    if (strcmp(cn, "vec_get") == 0) {
                        LLVMValueRef i = coerce(emit_expr_llvm(n->args.data[1]), lg.i64_ty);
                        LLVMValueRef pa[] = { v, i };
                        LLVMValueRef box = h_call(baga_rt("baga_vec_get_box"), pa, 2, "box");
                        LLVMValueRef sp = LLVMBuildBitCast(lg.builder, box,
                            LLVMPointerType(sty, 0), "sp");
                        return LLVMBuildLoad2(lg.builder, sty, sp, "v");
                    }
                    if (strcmp(cn, "vec_set") == 0) {
                        LLVMValueRef i = coerce(emit_expr_llvm(n->args.data[1]), lg.i64_ty);
                        LLVMValueRef e = emit_expr_llvm(n->args.data[2]);
                        if (!e) llvm_unsupported("vec_set аргумент");
                        LLVMValueRef tmp = entry_alloca(sty, "bx");
                        LLVMBuildStore(lg.builder, e, tmp);
                        LLVMValueRef bp = LLVMBuildBitCast(lg.builder, tmp, lg.ptr_ty, "bp");
                        LLVMValueRef pa[] = { v, i, bp, sz };
                        return h_call(baga_rt("baga_vec_set_box"), pa, 4, "");
                    }
                    if (strcmp(cn, "vec_slice") == 0) {
                        LLVMValueRef a = coerce(emit_expr_llvm(n->args.data[1]), lg.i64_ty);
                        LLVMValueRef b = coerce(emit_expr_llvm(n->args.data[2]), lg.i64_ty);
                        LLVMValueRef pa[] = { v, a, b, sz };
                        return h_call(baga_rt("baga_vec_slice_box"), pa, 4, "r");
                    }
                    /* vec_concat */
                    LLVMValueRef w = emit_expr_llvm(n->args.data[1]);
                    if (!w) llvm_unsupported("vec_concat аргумент");
                    LLVMValueRef pa[] = { v, w, sz };
                    return h_call(baga_rt("baga_vec_concat_box"), pa, 3, "r");
                }
                const char *suf = "i64";
                if (vt && vt->kind == TYPE_VEC && vt->elem) {
                    if (vt->elem->kind == TYPE_STR) suf = "str";
                    else if (vt->elem->kind == TYPE_F64) suf = "f64";
                    else if (vt->elem->kind == TYPE_STRUCT)
                        llvm_unsupported("Vec<struct> (анонимен — само C бекенда; вж. docs/language-en.md)");
                }
                char rt_name[64];
                snprintf(rt_name, sizeof rt_name, "baga_%s_%s",
                         n->callee->name, suf);
                fn = baga_rt(rt_name);
            }
            /* карти: map_set/map_get по ключ+стойност; has/del/keys по ключ
             * (типовете идват от checker-а; огледало на lowering-а в codegen_c) */
            if (!ef && (strcmp(n->callee->name, "map_set") == 0 ||
                        strcmp(n->callee->name, "map_get") == 0)) {
                Type *mt = n->args.len > 0 ? n->args.data[0]->type : NULL;
                const char *ksuf = "str";
                if (mt && mt->kind == TYPE_MAP && mt->key) {
                    if (mt->key->kind == TYPE_I64) ksuf = "i64";
                    else if (mt->key->kind == TYPE_BYTES) ksuf = "bytes";
                }
                int is_set = strcmp(n->callee->name, "map_set") == 0;
                /* struct / sum-enum стойности → box path (като codegen_c) */
                if (mt && mt->kind == TYPE_MAP && mt->elem && mt->elem->name &&
                    (mt->elem->kind == TYPE_STRUCT ||
                     mt->elem->kind == TYPE_ENUM)) {
                    LLVMTypeRef sty = user_struct_ty(mt->elem->name);
                    char rt_name[64];
                    snprintf(rt_name, sizeof rt_name, "baga_%s_%s_box",
                             n->callee->name, ksuf);
                    LLVMValueRef mv = emit_expr_llvm(n->args.data[0]);
                    LLVMValueRef k = emit_expr_llvm(n->args.data[1]);
                    if (!mv || !k) llvm_unsupported("map аргумент");
                    if (is_set) {
                        LLVMValueRef v = emit_expr_llvm(n->args.data[2]);
                        if (!v) llvm_unsupported("map_set стойност");
                        LLVMValueRef tmp = entry_alloca(sty, "bx");
                        LLVMBuildStore(lg.builder, coerce(v, sty), tmp);
                        LLVMValueRef bp = LLVMBuildBitCast(lg.builder, tmp,
                            lg.ptr_ty, "bp");
                        LLVMValueRef pa[] = { mv, k, bp, LLVMSizeOf(sty) };
                        return h_call(baga_rt(rt_name), pa, 4, "");
                    }
                    LLVMValueRef pa[] = { mv, k };
                    LLVMValueRef box = h_call(baga_rt(rt_name), pa, 2, "box");
                    /* missing key → нулева стойност (zero struct / tag-0 enum),
                     * като `*(%s){0}` в codegen_c */
                    LLVMValueRef tmp = entry_alloca(sty, "mg");
                    LLVMBuildStore(lg.builder, LLVMConstNull(sty), tmp);
                    LLVMValueRef cfn =
                        LLVMGetBasicBlockParent(LLVMGetInsertBlock(lg.builder));
                    LLVMBasicBlockRef have_bb =
                        LLVMAppendBasicBlockInContext(lg.ctx, cfn, "mg_have");
                    LLVMBasicBlockRef done_bb =
                        LLVMAppendBasicBlockInContext(lg.ctx, cfn, "mg_done");
                    LLVMBuildCondBr(lg.builder,
                        LLVMBuildIsNull(lg.builder, box, "isn"), done_bb, have_bb);
                    LLVMPositionBuilderAtEnd(lg.builder, have_bb);
                    LLVMValueRef sp = LLVMBuildBitCast(lg.builder, box,
                        LLVMPointerType(sty, 0), "sp");
                    LLVMBuildStore(lg.builder,
                        LLVMBuildLoad2(lg.builder, sty, sp, "sv"), tmp);
                    LLVMBuildBr(lg.builder, done_bb);
                    LLVMPositionBuilderAtEnd(lg.builder, done_bb);
                    return LLVMBuildLoad2(lg.builder, sty, tmp, "mv");
                }
                const char *vsuf = "i64";
                if (mt && mt->kind == TYPE_MAP && mt->elem) {
                    if (mt->elem->kind == TYPE_STR) vsuf = "str";
                    else if (mt->elem->kind == TYPE_F64) vsuf = "f64";
                    else if (mt->elem->kind == TYPE_BYTES) vsuf = "bytes";
                }
                char rt_name[64];
                snprintf(rt_name, sizeof rt_name, "baga_%s_%s_%s",
                         n->callee->name, ksuf, vsuf);
                fn = baga_rt(rt_name);
            }
            if (!ef && !fn &&
                (strcmp(n->callee->name, "map_has") == 0 ||
                 strcmp(n->callee->name, "map_del") == 0 ||
                 strcmp(n->callee->name, "map_keys") == 0)) {
                Type *mt = n->args.len > 0 ? n->args.data[0]->type : NULL;
                const char *ksuf = "str";
                if (mt && mt->kind == TYPE_MAP && mt->key) {
                    if (mt->key->kind == TYPE_I64) ksuf = "i64";
                    else if (mt->key->kind == TYPE_BYTES) ksuf = "bytes";
                }
                char rt_name[64];
                snprintf(rt_name, sizeof rt_name, "baga_%s_%s",
                         n->callee->name, ksuf);
                fn = baga_rt(rt_name);
            }
            for (int bi = 0; !ef && !fn && bi < (int)(sizeof(bmap) / sizeof(bmap[0])); bi++) {
                if (strcmp(n->callee->name, bmap[bi].baga) == 0) {
                    fn = baga_rt(bmap[bi].rt);
                    break;
                }
            }
            if (!fn) {
                char *m = llvm_mangle(n->callee->name);
                fn = LLVMGetNamedFunction(lg.mod, m);
                free(m);
            }
            /* MEM-1: drop е само за C бекенда; user fn 'drop' е намерена по-горе */
            if (!fn && strcmp(n->callee->name, "drop") == 0)
                llvm_unsupported("drop — само C бекенда; вж. docs/language-en.md");
            if (!fn) {
                char buf[256];
                snprintf(buf, sizeof buf, "вградена функция '%s'", n->callee->name);
                llvm_unsupported(buf);
            }
            int nargs = n->args.len;
            LLVMValueRef *args = malloc(sizeof(LLVMValueRef) * (size_t)(nargs > 0 ? nargs : 1));
            for (int i = 0; i < nargs; i++) {
                LLVMValueRef a = emit_expr_llvm(n->args.data[i]);
                if (!a) llvm_unsupported("print като аргумент");
                args[i] = coerce(a, LLVMTypeOf(LLVMGetParam(fn, (unsigned)i)));
            }
            LLVMTypeRef fn_ty = LLVMGetElementType(LLVMTypeOf(fn));
            int is_void = LLVMGetReturnType(fn_ty) == lg.void_ty;
            char *name = is_void ? NULL : tmp_name();
            LLVMValueRef result = LLVMBuildCall2(lg.builder, fn_ty, fn, args,
                                                 (unsigned)nargs, is_void ? "" : name);
            if (ef && extern_ret_is_str_llvm(ef)) {
                /* str return: NULL → "" (като codegen_c — getenv е тотална) */
                LLVMValueRef empty = LLVMBuildGlobalStringPtr(lg.builder, "", "empty");
                LLVMValueRef isnull = LLVMBuildIsNull(lg.builder, result, "isnull");
                result = LLVMBuildSelect(lg.builder, isnull, empty, result, "nn");
            }
            free(name);
            free(args);
            return result;
        }

        case NODE_MATCH:
            return emit_match_llvm(n);

        case NODE_ASSIGN: {
            if (n->assign_target->kind != NODE_IDENT)
                llvm_unsupported("присвояване на поле/индекс");
            LLVMValueRef alloca = st_lookup(n->assign_target->name);
            if (!alloca) {
                char buf[256];
                snprintf(buf, sizeof buf, "присвояване на недефинирано име '%s'",
                         n->assign_target->name);
                llvm_unsupported(buf);
            }
            LLVMValueRef v = emit_expr_llvm(n->assign_val);
            if (!v) llvm_unsupported("print в присвояване");
            v = coerce(v, LLVMGetAllocatedType(alloca));
            LLVMBuildStore(lg.builder, v, alloca);
            return v;
        }

        case NODE_IF:       llvm_unsupported("if като израз"); break;
        case NODE_INDEX:    llvm_unsupported("индексиране"); break;
        case NODE_FIELD: {
            /* L5/L6: модул.функция като стойност → handle към wrapper-а */
            if (n->type && n->type->kind == TYPE_FN && n->type->name) {
                LLVMValueRef wrap = closure_wrapper_named(n->type->name);
                return closure_handle(wrap, LLVMConstNull(lg.ptr_ty));
            }
            /* obj.field — като в codegen_c (.field); обектът е struct by
             * value, така че адресът е alloca (променлива) или временен spill */
            Node *obj = n->field_obj;
            const char *sname = (obj->type && obj->type->kind == TYPE_STRUCT)
                ? obj->type->name : NULL;
            if (!sname) llvm_unsupported("достъп до поле на не-структура");
            Node *decl = find_struct_decl(sname);
            int idx = decl ? struct_field_index(decl, n->field_name) : -1;
            if (idx < 0) {
                char buf[256];
                snprintf(buf, sizeof buf, "непознато поле '%s' на структура '%s'",
                         n->field_name, sname);
                llvm_unsupported(buf);
            }
            LLVMTypeRef sty = user_struct_ty(sname);
            LLVMValueRef base = NULL;
            if (obj->kind == NODE_IDENT)
                base = st_lookup(obj->name);
            if (!base) {
                LLVMValueRef ov = emit_expr_llvm(obj);
                if (!ov) llvm_unsupported("print като struct обект");
                base = entry_alloca(sty, "ftmp");
                LLVMBuildStore(lg.builder, coerce(ov, sty), base);
            }
            LLVMValueRef gep = LLVMBuildStructGEP2(lg.builder, sty, base,
                                                   (unsigned)idx, "fldp");
            LLVMTypeRef fty = llvm_type(decl->fields.data[idx]->fld_type);
            char *name = tmp_name();
            LLVMValueRef r = LLVMBuildLoad2(lg.builder, fty, gep, name);
            free(name);
            return r;
        }
        case NODE_STRUCT_LIT: {
            /* (Struct){ .f = v, ... } — alloca + store по полета;
             * липсващите полета са нули (като designated init в C) */
            Node *decl = find_struct_decl(n->lit_name);
            if (!decl) {
                char buf[256];
                snprintf(buf, sizeof buf, "неизвестна структура '%s'", n->lit_name);
                llvm_unsupported(buf);
            }
            LLVMTypeRef sty = user_struct_ty(n->lit_name);
            LLVMValueRef tmp = entry_alloca(sty, "slit");
            LLVMBuildStore(lg.builder, LLVMConstNull(sty), tmp);
            for (int i = 0; i < n->n_lit_fields; i++) {
                int idx = struct_field_index(decl, n->lit_fields[i]);
                if (idx < 0) {
                    char buf[256];
                    snprintf(buf, sizeof buf, "непознато поле '%s' на структура '%s'",
                             n->lit_fields[i], n->lit_name);
                    llvm_unsupported(buf);
                }
                LLVMValueRef v = emit_expr_llvm(n->lit_values.data[i]);
                if (!v) llvm_unsupported("print като стойност на поле");
                LLVMTypeRef fty = llvm_type(decl->fields.data[idx]->fld_type);
                LLVMValueRef gep = LLVMBuildStructGEP2(lg.builder, sty, tmp,
                                                       (unsigned)idx, "fld");
                LLVMBuildStore(lg.builder, coerce(v, fty), gep);
            }
            char *name = tmp_name();
            LLVMValueRef r = LLVMBuildLoad2(lg.builder, sty, tmp, name);
            free(name);
            return r;
        }
        case NODE_PATH: {
            /* Enum::Variant — plain enum → i64 таг; L3 sum payload-less
             * вариант → { tag, 0 } стойност (A1, като codegen_c) */
            Node *ed = NULL;
            for (int i = 0; lg.program && i < lg.program->items.len; i++) {
                Node *item = lg.program->items.data[i];
                if (item->kind == NODE_ENUM && item->enum_name &&
                    strcmp(item->enum_name, n->path_enum) == 0) { ed = item; break; }
            }
            int vidx = ed ? sum_variant_index(ed, n->path_variant) : -1;
            if (vidx < 0) {
                char buf[256];
                snprintf(buf, sizeof buf, "непознат път '%s::%s'",
                         n->path_enum ? n->path_enum : "?",
                         n->path_variant ? n->path_variant : "?");
                llvm_unsupported(buf);
            }
            if (is_sum_enum_item(ed)) {
                if (ed->enum_payloads && ed->enum_payloads[vidx])
                    llvm_unsupported("sum вариант с payload без аргумент");
                return sum_variant_const(ed, vidx);
            }
            return LLVMConstInt(lg.i64_ty, (unsigned long long)vidx, 0);
        }
        case NODE_TRY:      return emit_expr_llvm(n->try_expr);
        case NODE_CATCH:    return emit_expr_llvm(n->catch_expr);
        case NODE_TO_STR: {
            /* interpolation: convert inner expr to a string by its type */
            Type *et = n->to_str_expr ? n->to_str_expr->type : NULL;
            TypeKind ek = et ? et->kind : TYPE_STR;
            LLVMValueRef v = emit_expr_llvm(n->to_str_expr);
            if (ek == TYPE_STR) return v;
            if (ek == TYPE_BOOL) {
                /* низовете трябва да са i8* (като NODE_STR_LIT) — суровите
                 * глобали са [N x i8]* и select с различни N е невалиден */
                LLVMValueRef t = LLVMBuildGlobalStringPtr(lg.builder, "true", "tt");
                LLVMValueRef f = LLVMBuildGlobalStringPtr(lg.builder, "false", "ff");
                return LLVMBuildSelect(lg.builder, v, t, f, "bs");
            }
            return h_call(baga_rt("baga_i64_to_str"), &v, 1, "i2s");
        }
        case NODE_RANGE:    llvm_unsupported("диапазон (a..b) извън for"); break;
        case NODE_LAMBDA: {
            /* L5: env struct с копия на captures + wrapper + cell2 handle */
            int nc = n->captures.len;
            LLVMTypeRef env_ty = NULL;
            if (nc > 0) {
                LLVMTypeRef *elems = malloc(sizeof(LLVMTypeRef) * (size_t)nc);
                for (int i = 0; i < nc; i++)
                    elems[i] = llvm_type_resolved(n->captures.data[i]->type);
                env_ty = LLVMStructTypeInContext(lg.ctx, elems, (unsigned)nc, 0);
                free(elems);
            }
            LLVMValueRef wrap = emit_lambda_wrapper(n, env_ty);
            LLVMValueRef envp = LLVMConstNull(lg.ptr_ty);
            if (nc > 0) {
                LLVMValueRef sz = LLVMSizeOf(env_ty);
                LLVMValueRef ma[] = { sz };
                envp = h_call(rt_malloc(), ma, 1, "env");
                LLVMValueRef typed = LLVMBuildBitCast(lg.builder, envp,
                    LLVMPointerType(env_ty, 0), "envt");
                for (int i = 0; i < nc; i++) {
                    Node *cap = n->captures.data[i];
                    LLVMValueRef alloca = st_lookup(cap->param_name);
                    if (!alloca) llvm_unsupported("лямбда: capture не е локална");
                    LLVMValueRef v = LLVMBuildLoad2(lg.builder,
                        LLVMGetAllocatedType(alloca), alloca, "cap");
                    LLVMValueRef gep = LLVMBuildStructGEP2(lg.builder, env_ty,
                        typed, (unsigned)i, "cp");
                    LLVMBuildStore(lg.builder, v, gep);
                }
            }
            return closure_handle(wrap, envp);
        }
        default:            llvm_unsupported_node(n); break;
    }
    return NULL; /* unreachable */
}

/* ---- Statement emission ---- */

static void emit_stmt_llvm(Node *n, LLVMBasicBlockRef break_bb, LLVMBasicBlockRef cont_bb);

static void emit_block_llvm(Node *block, LLVMBasicBlockRef break_bb, LLVMBasicBlockRef cont_bb) {
    if (!block) return;
    if (block->kind != NODE_BLOCK)
        llvm_unsupported("тяло, което не е блок");
    st_push();
    for (int i = 0; i < block->stmts.len; i++) {
        /* мъртъв код след terminator — както gcc след return */
        if (LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(lg.builder)))
            break;
        emit_stmt_llvm(block->stmts.data[i], break_bb, cont_bb);
    }
    st_pop();
}

static void emit_stmt_llvm(Node *n, LLVMBasicBlockRef break_bb, LLVMBasicBlockRef cont_bb) {
    if (!n) return;

    switch (n->kind) {
        case NODE_LET: {
            LLVMTypeRef ty;
            if (n->let_type) ty = llvm_type(n->let_type);
            else if (n->let_init && n->let_init->type) ty = llvm_type_resolved(n->let_init->type);
            else ty = lg.i64_ty;
            char *m = llvm_mangle(n->let_name);
            LLVMValueRef alloca = entry_alloca(ty, m);
            free(m);
            if (n->let_init) {
                LLVMValueRef val = emit_expr_llvm(n->let_init);
                if (!val) llvm_unsupported("print като стойност на let");
                LLVMBuildStore(lg.builder, coerce(val, ty), alloca);
            }
            st_define(n->let_name, alloca);
            break;
        }

        case NODE_RETURN: {
            if (n->ret_val) {
                LLVMValueRef val = emit_expr_llvm(n->ret_val);
                if (!val) llvm_unsupported("print като return стойност");
                LLVMBuildRet(lg.builder, coerce(val, lg.cur_ret_ty));
            } else {
                if (lg.cur_ret_ty != lg.void_ty)
                    llvm_unsupported("return без стойност в не-void функция");
                LLVMBuildRetVoid(lg.builder);
            }
            break;
        }

        case NODE_IF: {
            LLVMValueRef cond = to_bool(emit_expr_llvm(n->cond));
            LLVMValueRef fn = LLVMGetBasicBlockParent(LLVMGetInsertBlock(lg.builder));
            LLVMBasicBlockRef then_bb = LLVMAppendBasicBlockInContext(lg.ctx, fn, "then");
            LLVMBasicBlockRef else_bb = LLVMAppendBasicBlockInContext(lg.ctx, fn, "else");
            LLVMBasicBlockRef merge_bb = LLVMAppendBasicBlockInContext(lg.ctx, fn, "merge");

            LLVMBuildCondBr(lg.builder, cond, then_bb, else_bb);

            LLVMPositionBuilderAtEnd(lg.builder, then_bb);
            emit_block_llvm(n->then_br, break_bb, cont_bb);
            if (!LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(lg.builder)))
                LLVMBuildBr(lg.builder, merge_bb);

            LLVMPositionBuilderAtEnd(lg.builder, else_bb);
            if (n->else_br) emit_block_llvm(n->else_br, break_bb, cont_bb);
            if (!LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(lg.builder)))
                LLVMBuildBr(lg.builder, merge_bb);

            LLVMPositionBuilderAtEnd(lg.builder, merge_bb);
            break;
        }

        case NODE_WHILE: {
            LLVMValueRef fn = LLVMGetBasicBlockParent(LLVMGetInsertBlock(lg.builder));
            LLVMBasicBlockRef cond_bb = LLVMAppendBasicBlockInContext(lg.ctx, fn, "while_cond");
            LLVMBasicBlockRef body_bb = LLVMAppendBasicBlockInContext(lg.ctx, fn, "while_body");
            LLVMBasicBlockRef end_bb = LLVMAppendBasicBlockInContext(lg.ctx, fn, "while_end");

            LLVMBuildBr(lg.builder, cond_bb);

            LLVMPositionBuilderAtEnd(lg.builder, cond_bb);
            LLVMValueRef cond = to_bool(emit_expr_llvm(n->while_cond));
            LLVMBuildCondBr(lg.builder, cond, body_bb, end_bb);

            LLVMPositionBuilderAtEnd(lg.builder, body_bb);
            emit_block_llvm(n->while_body, end_bb, cond_bb);
            if (!LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(lg.builder)))
                LLVMBuildBr(lg.builder, cond_bb);

            LLVMPositionBuilderAtEnd(lg.builder, end_bb);
            break;
        }

        case NODE_FOR: {
            /* for x in lo..hi { } → x = lo; while x < hi { body; x++ }
             * hi се преизчислява на всяка итерация, както в codegen_c */
            if (!n->for_iter || n->for_iter->kind != NODE_RANGE)
                llvm_unsupported("for без диапазон (a..b)");
            LLVMValueRef lo = emit_expr_llvm(n->for_iter->range_lo);
            if (!lo || LLVMGetTypeKind(LLVMTypeOf(lo)) != LLVMIntegerTypeKind)
                llvm_unsupported("for с не-целочислен диапазон");
            lo = coerce(lo, lg.i64_ty);

            LLVMValueRef fn = LLVMGetBasicBlockParent(LLVMGetInsertBlock(lg.builder));
            char *m = llvm_mangle(n->for_var);
            LLVMValueRef var = entry_alloca(lg.i64_ty, m);
            free(m);
            LLVMBuildStore(lg.builder, lo, var);

            LLVMBasicBlockRef cond_bb = LLVMAppendBasicBlockInContext(lg.ctx, fn, "for_cond");
            LLVMBasicBlockRef body_bb = LLVMAppendBasicBlockInContext(lg.ctx, fn, "for_body");
            LLVMBasicBlockRef incr_bb = LLVMAppendBasicBlockInContext(lg.ctx, fn, "for_incr");
            LLVMBasicBlockRef end_bb  = LLVMAppendBasicBlockInContext(lg.ctx, fn, "for_end");

            st_push();
            st_define(n->for_var, var);

            LLVMBuildBr(lg.builder, cond_bb);

            LLVMPositionBuilderAtEnd(lg.builder, cond_bb);
            LLVMValueRef hi = emit_expr_llvm(n->for_iter->range_hi);
            if (!hi || LLVMGetTypeKind(LLVMTypeOf(hi)) != LLVMIntegerTypeKind)
                llvm_unsupported("for с не-целочислен диапазон");
            hi = coerce(hi, lg.i64_ty);
            char *nm = tmp_name();
            LLVMValueRef cur = LLVMBuildLoad2(lg.builder, lg.i64_ty, var, nm);
            free(nm);
            nm = tmp_name();
            LLVMValueRef cond = LLVMBuildICmp(lg.builder, LLVMIntSLT, cur, hi, nm);
            free(nm);
            LLVMBuildCondBr(lg.builder, cond, body_bb, end_bb);

            LLVMPositionBuilderAtEnd(lg.builder, body_bb);
            emit_block_llvm(n->for_body, end_bb, incr_bb);
            if (!LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(lg.builder)))
                LLVMBuildBr(lg.builder, incr_bb);

            LLVMPositionBuilderAtEnd(lg.builder, incr_bb);
            nm = tmp_name();
            LLVMValueRef iv = LLVMBuildLoad2(lg.builder, lg.i64_ty, var, nm);
            free(nm);
            nm = tmp_name();
            LLVMValueRef next = LLVMBuildAdd(lg.builder, iv,
                                             LLVMConstInt(lg.i64_ty, 1, 0), nm);
            free(nm);
            LLVMBuildStore(lg.builder, next, var);
            LLVMBuildBr(lg.builder, cond_bb);

            LLVMPositionBuilderAtEnd(lg.builder, end_bb);
            st_pop();
            break;
        }

        case NODE_EXPR_STMT:
            emit_expr_llvm(n->expr);
            break;

        case NODE_BLOCK:
            emit_block_llvm(n, break_bb, cont_bb);
            break;

        case NODE_BREAK:
            if (!break_bb) llvm_unsupported("break извън цикъл");
            LLVMBuildBr(lg.builder, break_bb);
            break;

        case NODE_CONTINUE:
            if (!cont_bb) llvm_unsupported("continue извън цикъл");
            LLVMBuildBr(lg.builder, cont_bb);
            break;

        case NODE_INVARIANT:
            /* annotation statement — verifier-only, no IR emitted */
            break;

        default:
            llvm_unsupported_node(n);
            break;
    }
}

/* ---- spec ensures/requires (огледало на codegen_c) ---- */

/* Does this extern fn return str? (Effects on the return type are unwrapped.)
 * Mirror на extern_ret_is_str в codegen_c. */
static int extern_ret_is_str_llvm(Node *ef) {
    Node *t = ef->ret_type;
    while (t && t->kind == NODE_TYPE_EFFECT) t = t->inner_type;
    return t && t->kind == NODE_TYPE && strcmp(t->type_name, "str") == 0;
}

/* Намира spec с ensures или requires за дадена функция (NULL ако няма). */
static Node *find_ensures_spec_llvm(const char *fn_name) {
    if (!lg.program) return NULL;
    for (int i = 0; i < lg.program->items.len; i++) {
        Node *it = lg.program->items.data[i];
        if (it->kind == NODE_SPEC && strcmp(it->spec_name, fn_name) == 0 &&
            (it->spec_ensures.len > 0 || it->spec_requires.len > 0))
            return it;
    }
    return NULL;
}

/* fprintf(stderr, "spec '%s': ...", spec, idx, expr) + exit(1) + unreachable.
 * Съобщенията са БАЙТОВО същите като baga_spec_fail в codegen_c.
 * (Редът "  вход:" е само за C/--test-specs — тук не се печата.) */
static void emit_spec_fail_llvm(const char *spec_name, int is_requires,
                                int idx, const char *text) {
    const char *fmt_str = is_requires
        ? "spec '%s': requires #%lld нарушено: %s\n"
        : "spec '%s': ensures #%lld нарушена: %s\n";
    LLVMValueRef fmt = LLVMBuildGlobalStringPtr(lg.builder, fmt_str, "fmt");
    LLVMValueRef name_s = LLVMBuildGlobalStringPtr(lg.builder, spec_name, "str");
    LLVMValueRef expr_s = LLVMBuildGlobalStringPtr(lg.builder, text, "str");
    LLVMValueRef err = LLVMBuildLoad2(lg.builder, lg.ptr_ty, lg.stderr_global, "err");
    LLVMValueRef fargs[] = { fmt, name_s,
        LLVMConstInt(lg.i64_ty, (unsigned long long)idx, 0), expr_s };
    /* fprintf(FILE*, fmt, ...) — stderr е първият аргумент */
    LLVMValueRef call_args[5];
    call_args[0] = err;
    for (int i = 0; i < 4; i++) call_args[1 + i] = fargs[i];
    LLVMBuildCall2(lg.builder, LLVMGetElementType(LLVMTypeOf(lg.fprintf_fn)),
                   lg.fprintf_fn, call_args, 5, "");
    LLVMValueRef eargs[] = { LLVMConstInt(lg.i32_ty, 1, 0) };
    LLVMBuildCall2(lg.builder, LLVMGetElementType(LLVMTypeOf(lg.exit_fn)),
                   lg.exit_fn, eargs, 1, "");
    LLVMBuildUnreachable(lg.builder);
}

/* Една contract проверка: condbr → fail блок с emit_spec_fail_llvm. */
/* v[*] element invariants are verifier-only annotations — no runtime check. */
static int expr_has_elem_ref_llvm(Node *e) {
    if (!e) return 0;
    if (e->kind == NODE_ELEM_REF) return 1;
    if (e->kind == NODE_CALL && e->callee && e->callee->kind == NODE_IDENT &&
        strcmp(e->callee->name, "sorted") == 0) return 1;
    if (e->kind == NODE_BINARY) return expr_has_elem_ref_llvm(e->left) || expr_has_elem_ref_llvm(e->right);
    if (e->kind == NODE_UNARY) return expr_has_elem_ref_llvm(e->operand);
    return 0;
}

static void emit_spec_check_llvm(LLVMValueRef fn, Node *spec, Node *ensure,
                                 int is_requires, int idx) {
    if (expr_has_elem_ref_llvm(ensure->ensure_expr)) return;   /* verifier-only */
    LLVMValueRef cond = to_bool(emit_expr_llvm(ensure->ensure_expr));
    LLVMBasicBlockRef fail_bb = LLVMAppendBasicBlockInContext(lg.ctx, fn, "spec_fail");
    LLVMBasicBlockRef ok_bb = LLVMAppendBasicBlockInContext(lg.ctx, fn, "spec_ok");
    LLVMBuildCondBr(lg.builder, cond, ok_bb, fail_bb);
    LLVMPositionBuilderAtEnd(lg.builder, fail_bb);
    emit_spec_fail_llvm(spec->spec_name, is_requires, idx, ensure->ensure_text);
    LLVMPositionBuilderAtEnd(lg.builder, ok_bb);
}

/* ---- Function emission ---- */

static LLVMTypeRef fn_ret_type(Node *fn) {
    return fn->ret_type ? llvm_type(fn->ret_type) : lg.void_ty;
}

static LLVMTypeRef fn_type_of(Node *fn, LLVMTypeRef **out_param_tys, int *out_nparams) {
    LLVMTypeRef ret_ty = fn_ret_type(fn);
    int nparams = fn->params.len;
    LLVMTypeRef *param_tys = malloc(sizeof(LLVMTypeRef) * (size_t)(nparams > 0 ? nparams : 1));
    for (int i = 0; i < nparams; i++)
        param_tys[i] = llvm_type(fn->params.data[i]->param_type);
    if (out_param_tys) *out_param_tys = param_tys;
    if (out_nparams) *out_nparams = nparams;
    LLVMTypeRef fn_ty = LLVMFunctionType(ret_ty, param_tys, (unsigned)nparams, 0);
    if (!out_param_tys) free(param_tys);
    return fn_ty;
}

static char *impl_name_of(const char *fn_name) {
    char buf[512];
    snprintf(buf, sizeof buf, "__impl_%s", fn_name);
    return llvm_mangle(buf);
}

/* Първи проход: предекларации на всички функции (като в codegen_c).
 * Функция със spec получава impl (тялото) + wrapper (публичното име). */
static void predeclare_fn_llvm(Node *fn) {
    LLVMTypeRef fn_ty = fn_type_of(fn, NULL, NULL);
    if (fn->is_extern) {
        /* extern fn: declare with the raw C name (no b_ mangling) */
        LLVMAddFunction(lg.mod, fn->fn_name, fn_ty);
        return;
    }
    Node *spec = fn->fn_body ? find_ensures_spec_llvm(fn->fn_name) : NULL;
    if (spec) {
        char *im = impl_name_of(fn->fn_name);
        LLVMAddFunction(lg.mod, im, fn_ty);
        free(im);
    }
    char *m = llvm_mangle(fn->fn_name);
    LLVMAddFunction(lg.mod, m, fn_ty);
    free(m);
}

/* Тялото на функцията (за spec функции — под impl името). */
static void emit_fn_llvm(Node *fn, Node *spec) {
    if (!fn->fn_body) return; /* само декларация */

    char *m = spec ? impl_name_of(fn->fn_name) : llvm_mangle(fn->fn_name);
    LLVMValueRef fn_val = LLVMGetNamedFunction(lg.mod, m);
    free(m);

    LLVMTypeRef ret_ty = fn_ret_type(fn);
    lg.cur_ret_ty = ret_ty;

    LLVMBasicBlockRef entry = LLVMAppendBasicBlockInContext(lg.ctx, fn_val, "entry");
    LLVMPositionBuilderAtEnd(lg.builder, entry);

    st_reset();
    st_push();

    /* alloca за параметрите (за променливост) */
    for (int i = 0; i < fn->params.len; i++) {
        char *pm = llvm_mangle(fn->params.data[i]->param_name);
        LLVMValueRef param = LLVMGetParam(fn_val, (unsigned)i);
        LLVMSetValueName2(param, pm, strlen(pm));
        LLVMValueRef alloca = LLVMBuildAlloca(lg.builder, LLVMTypeOf(param), pm);
        LLVMBuildStore(lg.builder, param, alloca);
        free(pm);
        st_define(fn->params.data[i]->param_name, alloca);
    }

    /* тялото; последният EXPR_STMT в не-void функция е неявен return
     * (огледало на codegen_c) */
    int has_ret = fn->ret_type != NULL;
    NodeVec *stmts = &fn->fn_body->stmts;
    for (int i = 0; i < stmts->len; i++) {
        Node *s = stmts->data[i];
        if (LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(lg.builder)))
            break;
        if (has_ret && i == stmts->len - 1 && s->kind == NODE_EXPR_STMT) {
            LLVMValueRef v = emit_expr_llvm(s->expr);
            if (!v) llvm_unsupported("print като неявен return");
            LLVMBuildRet(lg.builder, coerce(v, ret_ty));
        } else {
            emit_stmt_llvm(s, NULL, NULL);
        }
    }

    st_pop();

    /* implicit return ако няма terminator */
    if (!LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(lg.builder))) {
        if (ret_ty == lg.void_ty)
            LLVMBuildRetVoid(lg.builder);
        else
            LLVMBuildRet(lg.builder, LLVMConstNull(ret_ty));
    }
}

/* Wrapper с публичното име: requires преди повикването, ensures след него. */
static void emit_wrapper_llvm(Node *fn, Node *spec) {
    char *m = llvm_mangle(fn->fn_name);
    LLVMValueRef wrapper = LLVMGetNamedFunction(lg.mod, m);
    free(m);
    char *im = impl_name_of(fn->fn_name);
    LLVMValueRef impl = LLVMGetNamedFunction(lg.mod, im);
    free(im);

    LLVMTypeRef ret_ty = fn_ret_type(fn);
    lg.cur_ret_ty = ret_ty;

    LLVMBasicBlockRef entry = LLVMAppendBasicBlockInContext(lg.ctx, wrapper, "entry");
    LLVMPositionBuilderAtEnd(lg.builder, entry);

    st_reset();
    st_push();

    /* параметрите на wrapper-а са spec input-ите (като в codegen_c) */
    int np = spec->spec_inputs.len;
    for (int i = 0; i < np; i++) {
        Node *sp = spec->spec_inputs.data[i];
        char *pm = llvm_mangle(sp->param_name);
        LLVMValueRef param = LLVMGetParam(wrapper, (unsigned)i);
        LLVMSetValueName2(param, pm, strlen(pm));
        LLVMValueRef alloca = LLVMBuildAlloca(lg.builder, LLVMTypeOf(param), pm);
        LLVMBuildStore(lg.builder, param, alloca);
        free(pm);
        st_define(sp->param_name, alloca);
    }

    /* предусловия преди повикването */
    for (int j = 0; j < spec->spec_requires.len; j++)
        emit_spec_check_llvm(wrapper, spec, spec->spec_requires.data[j], 1, j + 1);

    /* повикай impl с параметрите */
    LLVMValueRef *args = malloc(sizeof(LLVMValueRef) * (size_t)(np > 0 ? np : 1));
    for (int i = 0; i < np; i++) {
        LLVMValueRef alloca = st_lookup(spec->spec_inputs.data[i]->param_name);
        char *nm = tmp_name();
        args[i] = LLVMBuildLoad2(lg.builder, LLVMGetAllocatedType(alloca), alloca, nm);
        free(nm);
    }
    LLVMTypeRef impl_ty = LLVMGetElementType(LLVMTypeOf(impl));

    if (ret_ty != lg.void_ty) {
        char *nm = tmp_name();
        LLVMValueRef out = LLVMBuildCall2(lg.builder, impl_ty, impl, args, (unsigned)np, nm);
        free(nm);
        LLVMValueRef b_output = LLVMBuildAlloca(lg.builder, ret_ty, "b_output");
        LLVMBuildStore(lg.builder, out, b_output);
        st_define("output", b_output);

        /* гаранции след повикването */
        for (int j = 0; j < spec->spec_ensures.len; j++)
            emit_spec_check_llvm(wrapper, spec, spec->spec_ensures.data[j], 0, j + 1);

        char *nm2 = tmp_name();
        LLVMValueRef r = LLVMBuildLoad2(lg.builder, ret_ty, b_output, nm2);
        free(nm2);
        LLVMBuildRet(lg.builder, r);
    } else {
        /* void функция: само повикай impl след предусловията */
        LLVMBuildCall2(lg.builder, impl_ty, impl, args, (unsigned)np, "");
        LLVMBuildRetVoid(lg.builder);
    }
    free(args);
    st_pop();
}

/* ---- Public API ---- */

void codegen_llvm(Node *program, const char *output_path) {
    lg.ctx = LLVMContextCreate();
    lg.mod = LLVMModuleCreateWithNameInContext("baga_module", lg.ctx);
    lg.builder = LLVMCreateBuilderInContext(lg.ctx);
    lg.tmp_counter = 0;
    lg.program = program;

    lg.i64_ty = LLVMInt64TypeInContext(lg.ctx);
    lg.i32_ty = LLVMInt32TypeInContext(lg.ctx);
    lg.i1_ty = LLVMInt1TypeInContext(lg.ctx);
    lg.double_ty = LLVMDoubleTypeInContext(lg.ctx);
    lg.void_ty = LLVMVoidTypeInContext(lg.ctx);
    lg.i8_ty = LLVMInt8TypeInContext(lg.ctx);
    lg.ptr_ty = LLVMPointerType(lg.i8_ty, 0);
    /* default data layout — за ABI размерите на sum enum payload-ите */
    lg.td = LLVMCreateTargetData("");

    /* declare printf / fprintf / exit / stderr */
    {
        LLVMTypeRef printf_args[] = { lg.ptr_ty };
        LLVMTypeRef printf_ty = LLVMFunctionType(lg.i32_ty, printf_args, 1, 1);
        lg.printf_fn = LLVMAddFunction(lg.mod, "printf", printf_ty);

        LLVMTypeRef fprintf_args[] = { lg.ptr_ty, lg.ptr_ty };
        LLVMTypeRef fprintf_ty = LLVMFunctionType(lg.i32_ty, fprintf_args, 2, 1);
        lg.fprintf_fn = LLVMAddFunction(lg.mod, "fprintf", fprintf_ty);

        LLVMTypeRef exit_args[] = { lg.i32_ty };
        LLVMTypeRef exit_ty = LLVMFunctionType(lg.void_ty, exit_args, 1, 0);
        lg.exit_fn = LLVMAddFunction(lg.mod, "exit", exit_ty);

        /* glibc: stderr е external global (FILE*) */
        lg.stderr_global = LLVMAddGlobal(lg.mod, lg.ptr_ty, "stderr");
    }

    /* нулев проход: named struct типове + sum enum типове (имена, после тела) */
    for (int i = 0; i < program->items.len; i++) {
        Node *item = program->items.data[i];
        if (item->kind != NODE_STRUCT && !is_sum_enum_item(item)) continue;
        const char *tn = item->kind == NODE_STRUCT
            ? item->struct_name : item->enum_name;
        char *m = llvm_mangle(tn);
        if (!LLVMGetTypeByName(lg.mod, m))
            LLVMStructCreateNamed(lg.ctx, m);
        free(m);
    }
    for (int i = 0; i < program->items.len; i++) {
        Node *item = program->items.data[i];
        if (item->kind != NODE_STRUCT) continue;
        char *m = llvm_mangle(item->struct_name);
        LLVMTypeRef st = LLVMGetTypeByName(lg.mod, m);
        free(m);
        int nf = item->fields.len;
        LLVMTypeRef *elems = malloc(sizeof(LLVMTypeRef) * (size_t)(nf > 0 ? nf : 1));
        for (int j = 0; j < nf; j++)
            elems[j] = llvm_type(item->fields.data[j]->fld_type);
        LLVMStructSetBody(st, elems, (unsigned)nf, 0);
        free(elems);
    }
    /* sum enum тела: fixed-point по sized-ness на payload-ите — payload
     * може да е struct или друг sum enum; цикъл по стойност е грешка */
    for (;;) {
        int remaining = 0, progress = 0;
        for (int i = 0; i < program->items.len; i++) {
            Node *item = program->items.data[i];
            if (!is_sum_enum_item(item)) continue;
            char *m = llvm_mangle(item->enum_name);
            LLVMTypeRef st = LLVMGetTypeByName(lg.mod, m);
            free(m);
            if (LLVMTypeIsSized(st)) continue;
            int ready = 1;
            for (int j = 0; j < item->n_variants && ready; j++) {
                if (!item->enum_payloads || !item->enum_payloads[j]) continue;
                if (!LLVMTypeIsSized(llvm_type(item->enum_payloads[j])))
                    ready = 0;
            }
            if (!ready) { remaining++; continue; }
            sum_enum_set_body(item, st);
            progress = 1;
        }
        if (remaining == 0) break;
        if (!progress)
            llvm_unsupported("циклични sum enum типове (по стойност)");
    }

    /* първи проход: предекларации */
    for (int i = 0; i < program->items.len; i++) {
        Node *item = program->items.data[i];
        if (item->kind == NODE_FN)
            predeclare_fn_llvm(item);
    }

    /* втори проход: тела + wrapper-и */
    for (int i = 0; i < program->items.len; i++) {
        Node *item = program->items.data[i];
        if (item->kind != NODE_FN || !item->fn_body) continue;
        Node *spec = find_ensures_spec_llvm(item->fn_name);
        emit_fn_llvm(item, spec);
        if (spec) emit_wrapper_llvm(item, spec);
    }

    /* emit C main wrapper */
    {
        char *main_m = llvm_mangle("main");
        LLVMValueRef baga_main = LLVMGetNamedFunction(lg.mod, main_m);
        free(main_m);

        if (baga_main) {
            LLVMTypeRef mp[] = { lg.i32_ty, LLVMPointerType(lg.ptr_ty, 0) };
            LLVMTypeRef c_main_ty = LLVMFunctionType(lg.i32_ty, mp, 2, 0);
            LLVMValueRef c_main = LLVMAddFunction(lg.mod, "main", c_main_ty);
            LLVMBasicBlockRef entry = LLVMAppendBasicBlockInContext(lg.ctx, c_main, "entry");
            LLVMPositionBuilderAtEnd(lg.builder, entry);
            LLVMBuildStore(lg.builder, LLVMGetParam(c_main, 0), argv_argc_global());
            LLVMBuildStore(lg.builder, LLVMGetParam(c_main, 1), argv_argv_global());
            LLVMBuildCall2(lg.builder,
                LLVMGetElementType(LLVMTypeOf(baga_main)),
                baga_main, NULL, 0, "");
            LLVMBuildRet(lg.builder, LLVMConstInt(lg.i32_ty, 0, 0));
        }
    }

    /* verify module */
    char *error = NULL;
    LLVMBool broken = LLVMVerifyModule(lg.mod, LLVMPrintMessageAction, &error);
    if (broken) {
        fprintf(stderr, "baga: LLVM verification failed: %s\n", error ? error : "unknown");
        LLVMDisposeMessage(error);
        exit(1);
    }

    /* output */
    if (output_path) {
        LLVMPrintModuleToFile(lg.mod, output_path, &error);
        if (error) {
            fprintf(stderr, "baga: LLVM output error: %s\n", error);
            LLVMDisposeMessage(error);
        }
    } else {
        char *ir = LLVMPrintModuleToString(lg.mod);
        printf("%s", ir);
        LLVMDisposeMessage(ir);
    }

    /* cleanup */
    LLVMDisposeBuilder(lg.builder);
    LLVMDisposeModule(lg.mod);
    LLVMDisposeTargetData(lg.td);
    LLVMContextDispose(lg.ctx);
}

#endif /* BAGA_LLVM */
