#include "baga.h"

#ifdef BAGA_LLVM

#include <llvm-c/Core.h>
#include <llvm-c/Analysis.h>

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
 *  (?, catch — compile-time тагове, pass-through като codegen_c).
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
static LLVMTypeRef user_struct_ty(const char *name);

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
        case TYPE_BYTES:  return baga_bytes_ty();
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
        if (strcmp(ty->type_name, "Vec") == 0) return baga_vec_ptr_ty();
        return user_struct_ty(ty->type_name);
    }
    if (ty->kind == NODE_TYPE_EFFECT) return llvm_type(ty->inner_type);
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

/* static const char *baga_chr(int64_t c)
 * { char *r = malloc(2); r[0] = (char)c; r[1] = 0; return r; } */
static LLVMValueRef build_baga_chr(void) {
    LLVMTypeRef p[] = { lg.i64_ty };
    LLVMValueRef fn = LLVMAddFunction(lg.mod, "baga_chr",
        LLVMFunctionType(lg.ptr_ty, p, 1, 0));
    h_begin(fn);
    LLVMValueRef c = LLVMGetParam(fn, 0);
    LLVMValueRef two = LLVMConstInt(lg.i64_ty, 2, 0);
    LLVMValueRef ma[] = { two };
    LLVMValueRef r = h_call(rt_malloc(), ma, 1, "r");
    LLVMValueRef z = LLVMConstInt(lg.i64_ty, 0, 0);
    LLVMValueRef r0 = LLVMBuildGEP2(lg.builder, lg.i8_ty, r, &z, 1, "r0");
    LLVMBuildStore(lg.builder, LLVMBuildTrunc(lg.builder, c, lg.i8_ty, "t"), r0);
    LLVMValueRef one = LLVMConstInt(lg.i64_ty, 1, 0);
    LLVMValueRef r1 = LLVMBuildGEP2(lg.builder, lg.i8_ty, r, &one, 1, "r1");
    LLVMBuildStore(lg.builder, LLVMConstInt(lg.i8_ty, 0, 0), r1);
    LLVMBuildRet(lg.builder, r);
    return fn;
}

/* static int64_t baga_ord(const char *s)
 * { return s[0] ? (int64_t)(unsigned char)s[0] : 0; } */
static LLVMValueRef build_baga_ord(void) {
    LLVMTypeRef p[] = { lg.ptr_ty };
    LLVMValueRef fn = LLVMAddFunction(lg.mod, "baga_ord",
        LLVMFunctionType(lg.i64_ty, p, 1, 0));
    h_begin(fn);
    LLVMValueRef s = LLVMGetParam(fn, 0);
    LLVMValueRef z = LLVMConstInt(lg.i64_ty, 0, 0);
    LLVMValueRef p8 = LLVMBuildGEP2(lg.builder, lg.i8_ty, s, &z, 1, "p");
    LLVMValueRef ch = LLVMBuildLoad2(lg.builder, lg.i8_ty, p8, "c");
    LLVMValueRef nz = LLVMBuildICmp(lg.builder, LLVMIntNE, ch,
        LLVMConstInt(lg.i8_ty, 0, 0), "nz");
    LLVMValueRef ext = LLVMBuildZExt(lg.builder, ch, lg.i64_ty, "e");
    LLVMValueRef r = LLVMBuildSelect(lg.builder, nz, ext,
        LLVMConstInt(lg.i64_ty, 0, 0), "r");
    LLVMBuildRet(lg.builder, r);
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
            if (s->ret_val && res_alloca) {
                LLVMValueRef v = emit_expr_llvm(s->ret_val);
                if (!v) llvm_unsupported("print в match клон");
                v = coerce(v, LLVMGetAllocatedType(res_alloca));
                LLVMBuildStore(lg.builder, v, res_alloca);
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

static LLVMValueRef emit_match_llvm(Node *n) {
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
            LLVMValueRef alloca = st_lookup(n->name);
            if (alloca) {
                char *name = tmp_name();
                LLVMValueRef v = LLVMBuildLoad2(lg.builder,
                    LLVMGetAllocatedType(alloca), alloca, name);
                free(name);
                return v;
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
            if (n->callee->kind != NODE_IDENT)
                llvm_unsupported("повикване през израз (не име)");
            /* concurrency (!Par) — C/pthread only for now; honest refuse */
            if (!ef && n->callee->kind == NODE_IDENT) {
                const char *cn = n->callee->name;
                if (strcmp(cn, "go") == 0 || strcmp(cn, "go_bg") == 0 ||
                    strcmp(cn, "pool_map") == 0 || strcmp(cn, "join") == 0 ||
                    strcmp(cn, "detach") == 0 ||
                    strncmp(cn, "chan_", 5) == 0 ||
                    strcmp(cn, "mutex_new") == 0 || strcmp(cn, "mutex_lock") == 0 ||
                    strcmp(cn, "mutex_unlock") == 0 ||
                    strcmp(cn, "sleep_ms") == 0 ||
                    strcmp(cn, "cell2") == 0 || strcmp(cn, "cell2_0") == 0 ||
                    strcmp(cn, "cell2_1") == 0) {
                    char buf[128];
                    snprintf(buf, sizeof buf, "!Par/%s (само C backend)", cn);
                    llvm_unsupported(buf);
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
                const char *suf = "i64";
                if (vt && vt->kind == TYPE_VEC && vt->elem) {
                    if (vt->elem->kind == TYPE_STR) suf = "str";
                    else if (vt->elem->kind == TYPE_F64) suf = "f64";
                }
                char rt_name[64];
                snprintf(rt_name, sizeof rt_name, "baga_%s_%s",
                         n->callee->name, suf);
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

    /* нулев проход: named struct типове (имена, после тела) */
    for (int i = 0; i < program->items.len; i++) {
        Node *item = program->items.data[i];
        if (item->kind != NODE_STRUCT) continue;
        char *m = llvm_mangle(item->struct_name);
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
    LLVMContextDispose(lg.ctx);
}

#endif /* BAGA_LLVM */
