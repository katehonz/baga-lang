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

/* RC-LLVM: scope tracking — огледало на RcLocal/RcScope (baga.h:546-555).
 * Активно само при lg.rc. Статични масиви, както LLVMSymtab. */
typedef struct { const char *name; int tag; Type *type; Node *type_node;
                 int is_param; int dead;
                 int dead_linear; } LLRcLocal;
typedef struct { int top; int is_loop; } LLRcScope;

/* RC4-LLVM: per-statement temp регистър (порт на RcTmp/rc_tmps,
 * baga.h:560-569). За разлика от C (текстови __rc_tmpN декларации преди
 * statement-а) тук temp изразите се emit-ват веднъж преди root-а в текущия
 * block, а LLVMValueRef се кешира по AST възел — при удар emit_expr_llvm
 * връща кеша (SSA аналог на rc_tmp_emit_sub). */
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
int type_eq(Type *a, Type *b);
static char *llvm_mangle(const char *name);
static LLVMTypeRef user_struct_ty(const char *name);
static LLVMTypeRef baga_bytes_ty(void);
static Node *find_struct_decl(const char *name);

static char *llvm_struct_cname(Type *t);
/* M24: named struct тип на instantiated generic struct (по cname) */
static LLVMTypeRef user_struct_ty_inst(Type *t) {
    char *cn = llvm_struct_cname(t);
    LLVMTypeRef r = user_struct_ty(cn);
    free(cn);
    return r;
}

/* M24: име на инстанция k на generic struct decl */
static char *llvm_inst_cname(Node *s, int k) {
    char *m = llvm_mangle(s->struct_name);
    size_t cap = strlen(m) + 1 + (size_t)s->n_struct_params * 48;
    char *out = malloc(cap);
    if (!out) { fprintf(stderr, "baga: out of memory\n"); exit(1); }
    strcpy(out, m);
    free(m);
    for (int a = 0; a < s->n_struct_params; a++) {
        Type *at = s->struct_inst_targs[k * s->n_struct_params + a];
        if (at->kind == TYPE_STRUCT && at->name) {
            char *am = llvm_mangle(at->name);
            strcat(out, "_"); strcat(out, am + 2);
            free(am);
        } else if (at->kind == TYPE_ENUM && at->name) {
            char *am = llvm_mangle(at->name);
            strcat(out, "_"); strcat(out, am + 2);
            free(am);
        } else if (at->kind == TYPE_STR) strcat(out, "_str");
        else if (at->kind == TYPE_BYTES) strcat(out, "_bytes");
        else if (at->kind == TYPE_F64) strcat(out, "_f64");
        else if (at->kind == TYPE_BOOL) strcat(out, "_bool");
        else if (at->kind == TYPE_VEC) strcat(out, "_v");
        else if (at->kind == TYPE_MAP) strcat(out, "_m");
        else strcat(out, "_i64");
    }
    return out;
}

/* M24: C/LLVM име на instantiated generic struct ("b_Pair_i64_str") */
static char *llvm_struct_cname(Type *t) {
    const char *base = t->name ? t->name : "anon";
    size_t cap = strlen(base) + 1 + (size_t)(t->n_targs > 0 ? t->n_targs : 0) * 48;
    char *out = malloc(cap);
    if (!out) { fprintf(stderr, "baga: out of memory\n"); exit(1); }
    strcpy(out, base);
    for (int a = 0; a < t->n_targs; a++) {
        Type *at = t->targs[a];
        if (at->kind == TYPE_STRUCT && at->name) {
            strcat(out, "_"); strcat(out, at->name);
        } else if (at->kind == TYPE_ENUM && at->name) {
            strcat(out, "_"); strcat(out, at->name);
        } else if (at->kind == TYPE_STR) strcat(out, "_str");
        else if (at->kind == TYPE_BYTES) strcat(out, "_bytes");
        else if (at->kind == TYPE_F64) strcat(out, "_f64");
        else if (at->kind == TYPE_BOOL) strcat(out, "_bool");
        else if (at->kind == TYPE_VEC) strcat(out, "_v");
        else if (at->kind == TYPE_MAP) strcat(out, "_m");
        else strcat(out, "_i64");
    }
    return out;
}

/* LLVM тип от проверен Type (за targs substitution) */
static LLVMTypeRef llvm_type_of(Type *t) {
    if (!t) return lg.i64_ty;
    switch (t->kind) {
        case TYPE_I64: case TYPE_I32: case TYPE_ENUM: return lg.i64_ty;
        case TYPE_F64: return lg.double_ty;
        case TYPE_BOOL: return lg.i1_ty;
        case TYPE_STR: case TYPE_VEC: case TYPE_MAP: case TYPE_FN:
            return lg.ptr_ty;
        case TYPE_BYTES: return baga_bytes_ty();
        case TYPE_STRUCT: {
            if (t->n_targs > 0) {
                char *cn = llvm_struct_cname(t);
                LLVMTypeRef r = user_struct_ty(cn);
                free(cn);
                return r;
            }
            return user_struct_ty(t->name ? t->name : "anon");
        }
        default: return lg.i64_ty;
    }
}
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
            /* M24: instantiated generic struct */
            if (ty->n_targs > 0) {
                char *cn = llvm_struct_cname(ty);
                LLVMTypeRef r = user_struct_ty(cn);
                free(cn);
                return r;
            }
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
        /* M24: типова променлива на текущата generic struct инстанция */
        if (lg.gen_struct && ty->type) {
            for (int i = 0; i < lg.gen_struct->n_struct_params; i++)
                if (strcmp(lg.gen_struct->struct_params[i], ty->type_name) == 0) {
                    Type *at = lg.gen_struct->struct_inst_targs[
                        lg.gen_struct_inst * lg.gen_struct->n_struct_params + i];
                    return llvm_type_of(at);
                }
        }
        /* M21: типова променлива на текущата generic fn инстанция */
        if (lg.gen_fn && ty->type) {
            for (int i = 0; i < lg.gen_fn->n_type_params; i++)
                if (strcmp(lg.gen_fn->type_params[i], ty->type_name) == 0) {
                    Type *at = lg.gen_fn->inst_types[lg.gen_inst * lg.gen_fn->n_type_params + i];
                    return llvm_type_of(at);
                }
        }
        /* M24: instantiated generic struct — конкретният тип по cname */
        if (ty->type && ty->type->kind == TYPE_STRUCT && ty->type->n_targs > 0) {
            char *cn = llvm_struct_cname(ty->type);
            LLVMTypeRef t = user_struct_ty(cn);
            free(cn);
            return t;
        }
        if (strcmp(ty->type_name, "i64") == 0) return lg.i64_ty;
        if (strcmp(ty->type_name, "i32") == 0) return lg.i32_ty;
        if (strcmp(ty->type_name, "f64") == 0) return lg.double_ty;
        if (strcmp(ty->type_name, "bool") == 0) return lg.i1_ty;
        if (strcmp(ty->type_name, "str") == 0) return lg.ptr_ty;
        if (strcmp(ty->type_name, "void") == 0) return lg.void_ty;
        if (strcmp(ty->type_name, "bytes") == 0) return baga_bytes_ty();
        if (strcmp(ty->type_name, "Vec") == 0) return baga_vec_ptr_ty();
        if (strcmp(ty->type_name, "Map") == 0) return baga_map_ptr_ty();
        /* LP-final обединяване: enum без payload-и е i64-базиран */
        for (int i = 0; lg.program && i < lg.program->items.len; i++) {
            Node *it = lg.program->items.data[i];
            if (it->kind == NODE_ENUM && it->enum_name &&
                strcmp(it->enum_name, ty->type_name) == 0 &&
                !is_sum_enum_item(it))
                return lg.i64_ty;
        }
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
    } else if (tk == LLVMPointerTypeKind && gk == LLVMPointerTypeKind) {
        /* указател → указател (напр. baga_Vec* като елемент на Vec, LP-final) */
        r = LLVMBuildBitCast(lg.builder, v, target, name);
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

/* ============================ RC runtime (--rc) ============================
 *
 * 32 B header пред payload-а: { i64 magic, i64 pe, i64 rc, i64 an } —
 * същият layout като baga_Hdr в codegen_c (magic = 0xBA6A4D454D344848).
 * rc = 1 при конструкция; rc = 0 → free на базата (payload − 32).
 * pe = 0 винаги в LLVM v1 (няма persist epoch). Header-ът е ПРЕД върнатия
 * указател, така че payload offset-ите в helper-ите не се променят при
 * превключване rt_malloc → baga_rc_alloc. */

#define BAGA_RC_MAGIC 0xBA6A4D454D344848ULL
#define BAGA_RC_HSIZE 32

/* GEP до i64 поле idx на header базата (base е i8* към magic) */
static LLVMValueRef rc_hdr_field(LLVMValueRef base, int idx, const char *nm) {
    LLVMValueRef h = LLVMBuildBitCast(lg.builder, base,
        LLVMPointerType(lg.i64_ty, 0), "hc");
    LLVMValueRef i = LLVMConstInt(lg.i64_ty, (uint64_t)idx, 0);
    return LLVMBuildGEP2(lg.builder, lg.i64_ty, h, &i, 1, nm);
}

/* baga_rc_hdr: page guard + magic check (LLVM няма arena range guard).
 * Четем p-32 само при page offset >= 32; иначе NULL (immortal). */
static LLVMValueRef build_baga_rc_hdr(void) {
    LLVMTypeRef p[] = { lg.ptr_ty };
    LLVMValueRef fn = LLVMAddFunction(lg.mod, "baga_rc_hdr",
        LLVMFunctionType(lg.ptr_ty, p, 1, 0));
    h_begin(fn);
    LLVMValueRef ptr = LLVMGetParam(fn, 0);
    LLVMValueRef pi = LLVMBuildPtrToInt(lg.builder, ptr, lg.i64_ty, "pi");
    LLVMValueRef off = LLVMBuildAnd(lg.builder, pi,
        LLVMConstInt(lg.i64_ty, 4095, 0), "off");
    LLVMValueRef low = LLVMBuildICmp(lg.builder, LLVMIntULT, off,
        LLVMConstInt(lg.i64_ty, BAGA_RC_HSIZE, 0), "low");
    LLVMBasicBlockRef null_bb = LLVMAppendBasicBlockInContext(lg.ctx, fn, "null");
    LLVMBasicBlockRef chk_bb  = LLVMAppendBasicBlockInContext(lg.ctx, fn, "chk");
    LLVMBasicBlockRef hit_bb  = LLVMAppendBasicBlockInContext(lg.ctx, fn, "hit");
    LLVMBuildCondBr(lg.builder, low, null_bb, chk_bb);

    LLVMPositionBuilderAtEnd(lg.builder, chk_bb);
    LLVMValueRef back = LLVMConstInt(lg.i64_ty, (uint64_t)-BAGA_RC_HSIZE, 1);
    LLVMValueRef base = LLVMBuildGEP2(lg.builder, lg.i8_ty, ptr, &back, 1, "base");
    LLVMValueRef magic = LLVMBuildLoad2(lg.builder, lg.i64_ty,
        rc_hdr_field(base, 0, "mp"), "magic");
    LLVMValueRef ok = LLVMBuildICmp(lg.builder, LLVMIntEQ, magic,
        LLVMConstInt(lg.i64_ty, BAGA_RC_MAGIC, 0), "ok");
    LLVMBuildCondBr(lg.builder, ok, hit_bb, null_bb);

    LLVMPositionBuilderAtEnd(lg.builder, hit_bb);
    LLVMBuildRet(lg.builder, base);

    LLVMPositionBuilderAtEnd(lg.builder, null_bb);
    LLVMBuildRet(lg.builder, LLVMConstNull(lg.ptr_ty));
    return fn;
}

/* static i8 *baga_rc_alloc(i64 n)
 * { base = malloc(n + 32); пише header; return base + 32; } */
static LLVMValueRef build_baga_rc_alloc(void) {
    LLVMTypeRef p[] = { lg.i64_ty };
    LLVMValueRef fn = LLVMAddFunction(lg.mod, "baga_rc_alloc",
        LLVMFunctionType(lg.ptr_ty, p, 1, 0));
    h_begin(fn);
    LLVMValueRef n = LLVMGetParam(fn, 0);
    LLVMValueRef nn = LLVMBuildAdd(lg.builder, n,
        LLVMConstInt(lg.i64_ty, BAGA_RC_HSIZE, 0), "nn");
    LLVMValueRef base = h_call(rt_malloc(), &nn, 1, "base");
    LLVMBuildStore(lg.builder, LLVMConstInt(lg.i64_ty, BAGA_RC_MAGIC, 0),
        rc_hdr_field(base, 0, "mp"));
    LLVMBuildStore(lg.builder, LLVMConstInt(lg.i64_ty, 0, 0),
        rc_hdr_field(base, 1, "pep"));
    LLVMBuildStore(lg.builder, LLVMConstInt(lg.i64_ty, 1, 0),
        rc_hdr_field(base, 2, "rcp"));
    LLVMBuildStore(lg.builder, n, rc_hdr_field(base, 3, "anp"));
    LLVMValueRef fwd = LLVMConstInt(lg.i64_ty, BAGA_RC_HSIZE, 0);
    LLVMBuildRet(lg.builder,
        LLVMBuildGEP2(lg.builder, lg.i8_ty, base, &fwd, 1, "pay"));
    return fn;
}

/* baga_rc_retain: rc++ ако header-ът е валиден; връща p. C литералите и
 * външните буфери са „immortal" (hdr = NULL → no-op). */
static LLVMValueRef build_baga_rc_retain(void) {
    LLVMTypeRef p[] = { lg.ptr_ty };
    LLVMValueRef fn = LLVMAddFunction(lg.mod, "baga_rc_retain",
        LLVMFunctionType(lg.ptr_ty, p, 1, 0));
    h_begin(fn);
    LLVMValueRef ptr = LLVMGetParam(fn, 0);
    LLVMValueRef h = h_call(baga_rt("baga_rc_hdr"), &ptr, 1, "h");
    LLVMValueRef isn = LLVMBuildIsNull(lg.builder, h, "isn");
    LLVMBasicBlockRef inc_bb = LLVMAppendBasicBlockInContext(lg.ctx, fn, "inc");
    LLVMBasicBlockRef ret_bb = LLVMAppendBasicBlockInContext(lg.ctx, fn, "ret");
    LLVMBuildCondBr(lg.builder, isn, ret_bb, inc_bb);

    LLVMPositionBuilderAtEnd(lg.builder, inc_bb);
    LLVMValueRef rcp = rc_hdr_field(h, 2, "rcp");
    LLVMValueRef rc = LLVMBuildLoad2(lg.builder, lg.i64_ty, rcp, "rc");
    LLVMBuildStore(lg.builder, LLVMBuildAdd(lg.builder, rc,
        LLVMConstInt(lg.i64_ty, 1, 0), "rc1"), rcp);
    LLVMBuildBr(lg.builder, ret_bb);

    LLVMPositionBuilderAtEnd(lg.builder, ret_bb);
    LLVMBuildRet(lg.builder, ptr);
    return fn;
}

/* baga_rc_release_{str,bytes}: rc--; при 0 → free(base). Underflow
 * (rc вече 0) → съобщение на stderr + exit(1), не UB — същото
 * съобщение като codegen_c. */
static LLVMValueRef build_baga_rc_release(const char *name, const char *msg) {
    LLVMTypeRef p[] = { lg.ptr_ty };
    LLVMValueRef fn = LLVMAddFunction(lg.mod, name,
        LLVMFunctionType(lg.void_ty, p, 1, 0));
    h_begin(fn);
    LLVMValueRef ptr = LLVMGetParam(fn, 0);
    LLVMValueRef h = h_call(baga_rt("baga_rc_hdr"), &ptr, 1, "h");
    LLVMValueRef isn = LLVMBuildIsNull(lg.builder, h, "isn");
    LLVMBasicBlockRef dec_bb = LLVMAppendBasicBlockInContext(lg.ctx, fn, "dec");
    LLVMBasicBlockRef ret_bb = LLVMAppendBasicBlockInContext(lg.ctx, fn, "ret");
    LLVMBuildCondBr(lg.builder, isn, ret_bb, dec_bb);

    LLVMPositionBuilderAtEnd(lg.builder, dec_bb);
    LLVMValueRef rcp = rc_hdr_field(h, 2, "rcp");
    LLVMValueRef rc = LLVMBuildLoad2(lg.builder, lg.i64_ty, rcp, "rc");
    LLVMValueRef isz = LLVMBuildICmp(lg.builder, LLVMIntEQ, rc,
        LLVMConstInt(lg.i64_ty, 0, 0), "isz");
    LLVMBasicBlockRef uf_bb  = LLVMAppendBasicBlockInContext(lg.ctx, fn, "uf");
    LLVMBasicBlockRef ok_bb  = LLVMAppendBasicBlockInContext(lg.ctx, fn, "ok");
    LLVMBuildCondBr(lg.builder, isz, uf_bb, ok_bb);

    LLVMPositionBuilderAtEnd(lg.builder, uf_bb);
    LLVMValueRef err = LLVMBuildLoad2(lg.builder, lg.ptr_ty,
        lg.stderr_global, "err");
    LLVMValueRef fmt = LLVMBuildGlobalStringPtr(lg.builder, msg, "uffmt");
    LLVMValueRef fa[] = { err, fmt };
    LLVMBuildCall2(lg.builder, LLVMGetElementType(LLVMTypeOf(lg.fprintf_fn)),
                   lg.fprintf_fn, fa, 2, "");
    LLVMTypeRef ep[] = { lg.i32_ty };
    LLVMValueRef ea[] = { LLVMConstInt(lg.i32_ty, 1, 0) };
    h_call(rt_libc("exit", lg.void_ty, ep, 1), ea, 1, "");
    LLVMBuildUnreachable(lg.builder);

    LLVMPositionBuilderAtEnd(lg.builder, ok_bb);
    LLVMValueRef rc1 = LLVMBuildSub(lg.builder, rc,
        LLVMConstInt(lg.i64_ty, 1, 0), "rc1");
    LLVMBuildStore(lg.builder, rc1, rcp);
    LLVMValueRef isd = LLVMBuildICmp(lg.builder, LLVMIntEQ, rc1,
        LLVMConstInt(lg.i64_ty, 0, 0), "isd");
    LLVMBasicBlockRef free_bb = LLVMAppendBasicBlockInContext(lg.ctx, fn, "free");
    LLVMBuildCondBr(lg.builder, isd, free_bb, ret_bb);

    LLVMPositionBuilderAtEnd(lg.builder, free_bb);
    LLVMValueRef fr[] = { h };
    h_call(rt_free(), fr, 1, "");
    LLVMBuildBr(lg.builder, ret_bb);

    LLVMPositionBuilderAtEnd(lg.builder, ret_bb);
    LLVMBuildRetVoid(lg.builder);
    return fn;
}

/* malloc сайт → baga_rc_alloc при --rc, иначе чист malloc (без --rc IR е
 * побайтово същият). Всички str/bytes сайтове и следващите RC задачи
 * минават през тази обвивка. */
static LLVMValueRef rc_alloc_call(LLVMValueRef nbytes, const char *name) {
    if (lg.rc) return h_call(baga_rt("baga_rc_alloc"),
                             (LLVMValueRef[]){ coerce(nbytes, lg.i64_ty) }, 1, name);
    return h_call(rt_malloc(), (LLVMValueRef[]){ nbytes }, 1, name);
}

/* порт на rc_type_tag (codegen_c.c:97): 1=str, 2=bytes, 3=Vec, 4=Map,
 * 0=не-heap. Task 3 го ползва за track/retain/release сайтовете. */
static int lrc_type_tag(Type *t) {
    if (!t) return 0;
    switch (t->kind) {
        case TYPE_STR:   return 1;
        case TYPE_BYTES: return 2;
        case TYPE_VEC:   return 3;
        case TYPE_MAP:   return 4;
        default:         return 0;
    }
}

/* --------------------- RC-LLVM: scope tracking ---------------------
 * Огледало на rc_push_scope/rc_pop_scope/rc_release_all/
 * rc_release_to_loop (codegen_c.c:560-603), но върху статичните
 * масиви в lg: alloca-та се намират с st_lookup по име, release/retain
 * са IR повиквания към helper-ите от Task 2. Активно само при lg.rc —
 * без --rc нито една от тези функции не emit-ва IR. */

/* индекс на track-нат локал по име (-1 = няма); търси от върха надолу —
 * вътрешното засенчване печели (като rc_find в codegen_c) */
static int lrc_find(const char *name) {
    if (!lg.rc) return -1;
    for (int i = lg.lrc_count - 1; i >= 0; i--)
        if (strcmp(lg.lrc_locals[i].name, name) == 0) return i;
    return -1;
}

/* регистрира локал с вече изчислен tag (0 → не се track-ва); tnode е
 * анотационният type възел — резерва за elem/val kind, когато inferred
 * Type няма elem информация (като RcLocal.type_node в codegen_c) */
static void lrc_track_tag(const char *name, int tag, Type *t, Node *tnode,
                          int is_param) {
    if (!lg.rc || !tag) return;
    if (lg.lrc_count >= 256)
        llvm_unsupported("прекалено много RC локали в една функция");
    lg.lrc_locals[lg.lrc_count++] = (LLRcLocal){ name, tag, t, tnode, is_param, 0, 0 };
}

static int lrc_heap_tag(Type *t);

/* track-ва само heap локали (tag ∈ {1..6}); type NULL → не се track-ва */
static void lrc_track(const char *name, Type *t, int is_param) {
    if (!t) return;
    lrc_track_tag(name, lrc_heap_tag(t), t, NULL, is_param);
}

/* tag от анотационен type възел — за fn параметрите (като rc_type_node_tag
 * в codegen_c; само str/bytes/Vec/Map — struct/enum идват през
 * lrc_heap_tag_node по-долу) */
static int lrc_tag_node(Node *tn) {
    while (tn && (tn->kind == NODE_TYPE_EFFECT || tn->kind == NODE_TYPE_REF))
        tn = tn->inner_type;
    if (!tn || tn->kind != NODE_TYPE || !tn->type_name) return 0;
    if (strcmp(tn->type_name, "str") == 0)   return 1;
    if (strcmp(tn->type_name, "bytes") == 0) return 2;
    if (strcmp(tn->type_name, "Vec") == 0)   return 3;
    if (strcmp(tn->type_name, "Map") == 0)   return 4;
    return 0;
}

/* ---- Task 6: heap tag 5/6 (порт на RC5, codegen_c.c:123-234) ----
 * Struct/enum стойностите в LLVM са by-value в alloca (named struct тип,
 * БЕЗ rc header — за разлика от heap буферите). Затова tag 5/6 не описват
 * собствен rc на стойността, а „има heap полета/payload-и": release_<S>
 * освобождава heap ПОЛЕТАТА (транзитивно), без да free-ва самия struct
 * (той живее в стека). Lookup-ът е през lg.program (find_struct_decl/
 * find_sum_enum) — същият източник, който C ползва в rc_heap_tag
 * (find_struct_decl там също обхожда cg->program, не Codegen.chk). */

/* взаимна рекурсия struct↔enum (като codegen_c); depth guard 32 срещу
 * циклични декларации */
static int lrc_enum_has_heap_d(Node *item, int depth);
static int lrc_heap_tag_d(Type *t, int depth);

static int lrc_struct_has_heap_d(const char *name, int depth) {
    if (depth > 32) return 0;
    Node *d = find_struct_decl(name);
    if (!d) return 0;
    for (int i = 0; i < d->fields.len; i++) {
        Node *ft = d->fields.data[i]->fld_type;
        if (lrc_tag_node(ft)) return 1;
        Node *t = ft;
        while (t && (t->kind == NODE_TYPE_EFFECT || t->kind == NODE_TYPE_REF))
            t = t->inner_type;
        if (t && t->kind == NODE_TYPE && t->type_name) {
            /* M24-RC: checked типът печели за struct/enum полета —
             * instantiated generic поле (`inner: Pair<i64>`) се проверява
             * per инстанция */
            if (t->type && (t->type->kind == TYPE_STRUCT ||
                            t->type->kind == TYPE_ENUM)) {
                if (lrc_heap_tag_d(t->type, depth + 1)) return 1;
                continue;
            }
            if (lrc_struct_has_heap_d(t->type_name, depth + 1))
                return 1;
            /* enum поле с heap payload също брои (release_S вика release_E) */
            Node *ed = find_sum_enum(t->type_name);
            if (ed && lrc_enum_has_heap_d(ed, depth + 1))
                return 1;
        }
    }
    return 0;
}

static int lrc_struct_has_heap(const char *name) {
    return name ? lrc_struct_has_heap_d(name, 0) : 0;
}

/* enum с поне един variant с heap payload (str/bytes/Vec/Map или struct с
 * heap полета). Enum payload в enum не се брои (leak-safe — като C). */
static int lrc_enum_has_heap_d(Node *item, int depth) {
    if (depth > 32) return 0;
    if (!item || item->kind != NODE_ENUM) return 0;
    for (int j = 0; j < item->n_variants; j++) {
        Node *pt = item->enum_payloads ? item->enum_payloads[j] : NULL;
        if (!pt) continue;
        if (lrc_tag_node(pt)) return 1;
        Node *t = pt;
        while (t && (t->kind == NODE_TYPE_EFFECT || t->kind == NODE_TYPE_REF))
            t = t->inner_type;
        if (t && t->kind == NODE_TYPE && t->type_name &&
            lrc_struct_has_heap_d(t->type_name, depth + 1))
            return 1;
    }
    return 0;
}

static int lrc_enum_has_heap(Node *item) {
    return lrc_enum_has_heap_d(item, 0);
}

/* M24-RC: инстанция на generic struct decl (d) с конкретни targs (vt) —
 * има ли heap полета след resolve на типовите променливи? Огледало на
 * rc_gen_inst_has_heap_d (codegen_c): decl-нивото вижда `v: T` като
 * не-heap дори при T=str, затова проверката е per инстанция. */
static int lrc_gen_inst_has_heap_d(Node *d, Type *vt, int depth) {
    if (depth > 32) return 0;
    for (int i = 0; i < d->fields.len; i++) {
        Node *ft = d->fields.data[i]->fld_type;
        Node *t = ft;
        while (t && (t->kind == NODE_TYPE_EFFECT || t->kind == NODE_TYPE_REF))
            t = t->inner_type;
        if (!t || t->kind != NODE_TYPE || !t->type_name) continue;
        /* типова променлива → targ на инстанцията */
        int is_var = 0;
        for (int a = 0; a < d->n_struct_params && a < vt->n_targs; a++) {
            if (strcmp(t->type_name, d->struct_params[a]) == 0) {
                if (lrc_heap_tag_d(vt->targs[a], depth + 1)) return 1;
                is_var = 1;
                break;
            }
        }
        if (is_var) continue;
        if (lrc_tag_node(ft)) return 1;
        /* вложено поле: checked типът печели (може instantiated generic) */
        if (t->type && (t->type->kind == TYPE_STRUCT ||
                        t->type->kind == TYPE_ENUM)) {
            if (lrc_heap_tag_d(t->type, depth + 1)) return 1;
            continue;
        }
        if (lrc_struct_has_heap_d(t->type_name, depth + 1)) return 1;
        Node *ed = find_sum_enum(t->type_name);
        if (ed && lrc_enum_has_heap_d(ed, depth + 1)) return 1;
    }
    return 0;
}

/* порт на rc_heap_tag (codegen_c.c:208): 5 = struct с heap полета,
 * 6 = sum enum с heap payload. M24-RC: generic struct — per инстанция. */
static int lrc_heap_tag_d(Type *t, int depth) {
    if (depth > 32) return 0;
    int tag = lrc_type_tag(t);
    if (tag) return tag;
    if (t && t->kind == TYPE_STRUCT && t->name) {
        Node *d = find_struct_decl(t->name);
        if (d && d->n_struct_params > 0 && t->n_targs > 0)
            return lrc_gen_inst_has_heap_d(d, t, depth + 1) ? 5 : 0;
        if (lrc_struct_has_heap(t->name)) return 5;
    }
    if (t && t->kind == TYPE_ENUM && t->name) {
        Node *ed = find_sum_enum(t->name);
        if (ed && lrc_enum_has_heap(ed)) return 6;
    }
    return 0;
}

static int lrc_heap_tag(Type *t) {
    return lrc_heap_tag_d(t, 0);
}

/* порт на rc_heap_tag_node (codegen_c.c:221) — същият tag от type AST възел */
static int lrc_heap_tag_node(Node *ty) {
    int tag = lrc_tag_node(ty);
    if (tag) return tag;
    while (ty && (ty->kind == NODE_TYPE_EFFECT || ty->kind == NODE_TYPE_REF))
        ty = ty->inner_type;
    if (ty && ty->kind == NODE_TYPE && ty->type_name) {
        if (lrc_struct_has_heap(ty->type_name))
            return 5;
        Node *ed = find_sum_enum(ty->type_name);
        if (ed && lrc_enum_has_heap(ed)) return 6;
    }
    return 0;
}

/* порт на rc_is_enum_ctor (codegen_c.c:1228): повикване-конструктор на sum
 * enum с payload (bare Ok(x) или Res::Ok(x)) */
static int lrc_is_enum_ctor(Node *n) {
    if (!lg.program || !n || n->kind != NODE_CALL || !n->callee) return 0;
    if (n->callee->kind != NODE_IDENT && n->callee->kind != NODE_PATH)
        return 0;
    for (int i = 0; i < lg.program->items.len; i++) {
        Node *item = lg.program->items.data[i];
        if (item->kind != NODE_ENUM) continue;
        if (n->callee->kind == NODE_PATH &&
            strcmp(item->enum_name, n->callee->path_enum) != 0)
            continue;
        for (int j = 0; j < item->n_variants; j++) {
            const char *vn = n->callee->kind == NODE_PATH
                ? n->callee->path_variant : n->callee->name;
            if (item->enum_payloads && item->enum_payloads[j] &&
                strcmp(item->enum_variants[j], vn) == 0)
                return 1;
        }
    }
    return 0;
}

/* порт на rc_borrowed_init (codegen_c.c:312): vec_get/map_get/struct поле/
 * h_* връщат референция към чужда собственост → при вграждане/връзване се
 * retain-ва, не се пропуска */
static int lrc_borrowed_init(Node *init) {
    if (!init) return 0;
    if (init->kind == NODE_FIELD) return 1;
    if (init->kind == NODE_CALL && init->callee &&
        init->callee->kind == NODE_IDENT) {
        const char *bn = init->callee->name;
        if (strncmp(bn, "vec_get", 7) == 0 || strncmp(bn, "map_get", 7) == 0 ||
            strcmp(bn, "h_str") == 0 || strcmp(bn, "h_bytes") == 0 ||
            strcmp(bn, "h_map") == 0)
            return 1;
    }
    return 0;
}

/* порт на rc_need_owned_retain (codegen_c.c:1504): трябва ли стойността на
 * match рамо да се retain-не, за да е резултатът owned по конвенцията
 * „fn резултат = owned"? Свеж литерал/ctor и не-borrowed call — не;
 * вложен match — не (рамената му вече retain-ват). Ident (match binding е
 * borrowed алиас на payload-а; track-нат локал е втори собственик),
 * vec_get/поле и пр. — да. */
static int lrc_need_owned_retain(Node *val) {
    if (!lg.rc || !val) return 0;
    if (!lrc_heap_tag(val->type)) return 0;
    if (val->kind == NODE_STRUCT_LIT) return 0;
    if (val->kind == NODE_CALL && lrc_is_enum_ctor(val)) return 0;
    if (val->kind == NODE_CALL && !lrc_borrowed_init(val)) return 0;
    if (val->kind == NODE_MATCH) return 0;
    return 1;
}

static void lrc_emit_retain_val(int tag, Type *ty, Node *tnode,
                                LLVMValueRef val);

/* retain на стойност, вграждана в контейнер/struct поле/enum payload
 * (опростен порт на embed сайтовете в codegen_c): fresh израз (struct
 * литерал, enum ctor, owned call) идва с rc=1 от конструкцията — move,
 * без retain; ident/borrowed се retain-ват (+1 за новата референция).
 * Без move elision (RC2 prepass няма LLVM еквивалент) — retain + нормален
 * release на източника е наблюдаемо еквивалентен на C move-а (и двата
 * пъта rc-то е балансирано; разлика само във времето на декрементите). */
static void lrc_embed_retain(Node *val, LLVMValueRef v) {
    if (!lg.rc || !val) return;
    int tag = lrc_heap_tag(val->type);
    if (!tag) return;
    if (val->kind == NODE_IDENT) {
        int si = lrc_find(val->name);
        if (si >= 0 && lg.lrc_locals[si].dead) return;   /* drop()нат — като C */
    } else if (val->kind == NODE_STRUCT_LIT || lrc_is_enum_ctor(val)) {
        return;   /* свеж литерал/ctor — move в новата собственост */
    } else if (!lrc_borrowed_init(val)) {
        return;   /* owned call резултат — move (като rc_tmp consume в C) */
    }
    lrc_emit_retain_val(tag, val->type, NULL, v);
}

/* Task 5: elem_kind за release на Vec — порт на rc_vec_elem_kind +
 * rc_vec_elem_kind_node (codegen_c.c:333-388): 0 inline (i64/f64/bool),
 * 1 str, 2 struct/enum box, 3 nested Vec, 4 bytes box. Inferred Type първо;
 * при 0 — резерва от анотацията (`let v: Vec<str> = vec_new()`). За разлика
 * от C elem_size не се смята — libc free не иска размер (подаваме 0). */
static int lrc_vec_elem_kind(Type *vty, Node *tn) {
    Type *e = vty ? vty->elem : NULL;
    if (e) {
        if (e->kind == TYPE_STR) return 1;
        if (e->kind == TYPE_BYTES) return 4;
        if (e->kind == TYPE_VEC) return 3;
        if (e->name && (e->kind == TYPE_STRUCT || e->kind == TYPE_ENUM)) return 2;
    }
    while (tn && (tn->kind == NODE_TYPE_EFFECT || tn->kind == NODE_TYPE_REF))
        tn = tn->inner_type;
    Node *en = tn ? tn->inner_type : NULL;
    if (!en || en->kind != NODE_TYPE || !en->type_name) return 0;
    if (strcmp(en->type_name, "str") == 0)   return 1;
    if (strcmp(en->type_name, "bytes") == 0) return 4;
    if (strcmp(en->type_name, "Vec") == 0)   return 3;
    if (strcmp(en->type_name, "i64") == 0 || strcmp(en->type_name, "i32") == 0 ||
        strcmp(en->type_name, "f64") == 0 || strcmp(en->type_name, "bool") == 0)
        return 0;
    return 2;
}

/* Task 5: val_tag за release на Map — порт на rc_map_val_tag + _node
 * (codegen_c.c:354-403): 0 inline, 1 str, 2 bytes, 3 struct/enum box.
 * Ключът не е нужен — release_map го познава по entry ktag/sk (като C). */
static int lrc_map_val_tag(Type *mty, Node *tn) {
    Type *v = mty ? mty->elem : NULL;
    if (v) {
        if (v->kind == TYPE_STR) return 1;
        if (v->kind == TYPE_BYTES) return 2;
        if (v->name && (v->kind == TYPE_STRUCT || v->kind == TYPE_ENUM)) return 3;
    }
    while (tn && (tn->kind == NODE_TYPE_EFFECT || tn->kind == NODE_TYPE_REF))
        tn = tn->inner_type;
    Node *vn = tn ? tn->inner_type2 : NULL;
    if (!vn || vn->kind != NODE_TYPE || !vn->type_name) return 0;
    if (strcmp(vn->type_name, "str") == 0)   return 1;
    if (strcmp(vn->type_name, "bytes") == 0) return 2;
    if (strcmp(vn->type_name, "i64") == 0 || strcmp(vn->type_name, "i32") == 0 ||
        strcmp(vn->type_name, "f64") == 0 || strcmp(vn->type_name, "bool") == 0)
        return 0;
    return 3;
}

/* едно release повикване: зарежда стойността от alloca-та и вика
 * baga_rc_release_{str,bytes,vec,map}. Vec/Map (tag 3/4) — Task 5:
 * рекурсивен release на елементите според elem_kind/val_tag; 4-тият
 * аргумент е destructor на box полетата (Task 6: baga_rc_relf_<S>,
 * NULL → само free — като elem_rel/val_rel в codegen_c).
 * Tag 5/6 (Task 6): struct/enum стойността е by-value в alloca —
 * release_<име> освобождава heap полетата/payload-а, без free на
 * самия struct (той е стеков). */
static LLVMValueRef lrc_rc_fn(const char *type_name, int is_release);
static LLVMValueRef lrc_rc_fn_ty(Type *t, int is_release);  /* M24-RC: Type-aware */
static LLVMValueRef lrc_box_rel(const char *type_name);
static LLVMValueRef lrc_box_rel_ty(Type *et);               /* M24-RC: Type-aware */
static LLVMValueRef lrc_nested_vec_rel(Type *vty, Node *tn);

/* M24-RC: LLVM тип на struct/enum стойност — instantiated generic struct
 * (n_targs>0) → per-instance named тип (b_Box_i64), иначе по име */
static LLVMTypeRef lrc_struct_ty_of(Type *ty, Node *tnode) {
    if (ty && ty->kind == TYPE_STRUCT && ty->n_targs > 0)
        return user_struct_ty_inst(ty);
    const char *sn = (ty && ty->name) ? ty->name : NULL;
    if (!sn && tnode) {
        Node *t = tnode;
        while (t && (t->kind == NODE_TYPE_EFFECT || t->kind == NODE_TYPE_REF))
            t = t->inner_type;
        if (t && t->kind == NODE_TYPE) sn = t->type_name;
    }
    return sn ? user_struct_ty(sn) : NULL;
}

/* име на struct/enum типа на локал — inferred Type първо, анотация после
 * (като case 5/6 в rc_emit_release, codegen_c.c:536-556) */
static const char *lrc_local_type_name(LLRcLocal *l) {
    if (l->type && l->type->name) return l->type->name;
    Node *t = l->type_node;
    while (t && (t->kind == NODE_TYPE_EFFECT || t->kind == NODE_TYPE_REF))
        t = t->inner_type;
    if (t && t->kind == NODE_TYPE) return t->type_name;
    return NULL;
}

/* елементен/стойностен тип на Vec/Map локал (inferred Type, после анотация)
 * — порт на rc_vec_elem_name/rc_map_val_name (codegen_c.c:473-487) */
static const char *lrc_vec_elem_name(LLRcLocal *l) {
    if (l->type && l->type->elem && l->type->elem->name)
        return l->type->elem->name;
    Node *it = l->type_node ? l->type_node->inner_type : NULL;
    if (it && it->kind == NODE_TYPE) return it->type_name;
    return NULL;
}

static const char *lrc_map_val_name(LLRcLocal *l) {
    if (l->type && l->type->elem && l->type->elem->name)
        return l->type->elem->name;
    Node *it = l->type_node ? l->type_node->inner_type2 : NULL;
    if (it && it->kind == NODE_TYPE) return it->type_name;
    return NULL;
}

static void lrc_emit_release(LLRcLocal *l) {
    LLVMValueRef alloca = st_lookup(l->name);
    if (!alloca) return;
    LLVMValueRef p;
    const char *rt;
    if (l->tag == 1) {
        p = LLVMBuildLoad2(lg.builder, lg.ptr_ty, alloca, "rcr");
        rt = "baga_rc_release_str";
    } else if (l->tag == 2) {
        /* bytes е { i8* data, i64 len } by value — release-ва се data */
        p = LLVMBuildLoad2(lg.builder, lg.ptr_ty,
            LLVMBuildStructGEP2(lg.builder, baga_bytes_ty(), alloca, 0, "rcbp"),
            "rcbd");
        rt = "baga_rc_release_bytes";
    } else if (l->tag == 3) {
        LLVMValueRef vp = LLVMBuildLoad2(lg.builder, baga_vec_ptr_ty(), alloca, "rcv");
        int ek = lrc_vec_elem_kind(l->type, l->type_node);
        /* destructor: ek 2 → relf на елементния тип; ek 3 → relv на
         * вложения Vec<S> (NULL → старото поведение, leak-safe граница) */
        /* M24-RC: instantiated generic елемент → per-instance relf;
         * иначе досегашният name-based път (с node fallback) */
        LLVMValueRef rel = ek == 3
            ? lrc_nested_vec_rel(l->type, l->type_node)
            : (l->type && l->type->elem && l->type->elem->n_targs > 0
               ? lrc_box_rel_ty(l->type->elem)
               : lrc_box_rel(lrc_vec_elem_name(l)));
        LLVMValueRef a[] = {
            LLVMBuildBitCast(lg.builder, vp, lg.ptr_ty, "rcvp"),
            LLVMConstInt(lg.i64_ty, (uint64_t)ek, 0),
            LLVMConstInt(lg.i64_ty, 0, 0),   /* elem_size — не се ползва */
            rel,
        };
        h_call(baga_rt("baga_rc_release_vec"), a, 4, "");
        return;
    } else if (l->tag == 4) {
        LLVMValueRef mp = LLVMBuildLoad2(lg.builder, baga_map_ptr_ty(), alloca, "rcm");
        LLVMValueRef a[] = {
            LLVMBuildBitCast(lg.builder, mp, lg.ptr_ty, "rcmp"),
            LLVMConstInt(lg.i64_ty,
                (uint64_t)lrc_map_val_tag(l->type, l->type_node), 0),
            LLVMConstInt(lg.i64_ty, 0, 0),   /* val_size — не се ползва */
            /* M24-RC: instantiated generic стойност → per-instance relf */
            (l->type && l->type->elem && l->type->elem->n_targs > 0
             ? lrc_box_rel_ty(l->type->elem)
             : lrc_box_rel(lrc_map_val_name(l))),
        };
        h_call(baga_rt("baga_rc_release_map"), a, 4, "");
        return;
    } else if (l->tag == 5 || l->tag == 6) {
        const char *sn = lrc_local_type_name(l);
        if (!sn) return;
        /* M24-RC: instantiated generic struct — per-instance тип и helper */
        LLVMTypeRef sty = lrc_struct_ty_of(l->type, l->type_node);
        if (!sty) return;
        LLVMValueRef v = LLVMBuildLoad2(lg.builder, sty, alloca, "rcs");
        if (l->type) h_call(lrc_rc_fn_ty(l->type, 1), &v, 1, "");
        else         h_call(lrc_rc_fn(sn, 1), &v, 1, "");
        return;
    } else {
        return;
    }
    h_call(baga_rt(rt), &p, 1, "");
}

/* retain върху вече изчислена стойност (alias копие: let x = y / x = y).
 * tag 3/4 — retain на struct указателя (rc живее върху него, като в C).
 * tag 5/6 — retain_<име> на heap полетата/payload-а (стойността е
 * by-value; ty/tnode дават името на типа, като rc_emit_retain_val в C). */
static void lrc_emit_retain_val(int tag, Type *ty, Node *tnode,
                                LLVMValueRef val) {
    LLVMValueRef p;
    if (tag == 1)      p = val;
    else if (tag == 2) p = LLVMBuildExtractValue(lg.builder, val, 0, "rcbd");
    else if (tag == 3 || tag == 4)
        p = LLVMBuildBitCast(lg.builder, val, lg.ptr_ty, "rccp");
    else if (tag == 5 || tag == 6) {
        /* M24-RC: instantiated generic struct — per-instance helper */
        if (ty && ty->kind == TYPE_STRUCT && ty->n_targs > 0) {
            h_call(lrc_rc_fn_ty(ty, 0), &val, 1, "");
            return;
        }
        const char *sn = (ty && ty->name) ? ty->name : NULL;
        if (!sn && tnode) {
            Node *t = tnode;
            while (t && (t->kind == NODE_TYPE_EFFECT || t->kind == NODE_TYPE_REF))
                t = t->inner_type;
            if (t && t->kind == NODE_TYPE) sn = t->type_name;
        }
        if (!sn) return;
        h_call(lrc_rc_fn(sn, 0), &val, 1, "");
        return;
    }
    else               return;
    h_call(baga_rt("baga_rc_retain"), &p, 1, "rcr");
}

/* ---------------- RC4-LLVM: per-statement temp регистър ----------------
 * Порт на rc_tmp_* (codegen_c.c:1078-1521): fresh heap резултат (owned call
 * с heap тип — не borrowed, не enum ctor — плюс NODE_TO_STR), който не се
 * връзва в локал, е temp и се release-ва в края на statement-а. C hoist-ва
 * temp-овете в __rc_tmpN декларации преди statement-а; тук (SSA) temp
 * изразите се emit-ват първи в текущия block и стойността се кешира по
 * Node* — emit_expr_llvm връща кеша при удар (без двойна оценка). */

static LLVMValueRef emit_expr_llvm(Node *n); /* fwd — пълната дефиниция е по-долу */

/* fresh heap temp ли е този възел? (порт на rc_tmp_fresh, codegen_c.c:1237) */
static int lrc_tmp_fresh(Node *n) {
    if (!n) return 0;
    if (n->kind == NODE_TO_STR) {
        /* to_str върху str е identity (borrowed) — fresh е само конверсията
         * от не-str тип; паритет с rc_tmp_fresh (codegen_c.c). */
        Type *et = n->to_str_expr ? n->to_str_expr->type : NULL;
        return !(et && et->kind == TYPE_STR);
    }
    if (n->kind != NODE_CALL) return 0;
    if (lrc_borrowed_init(n)) return 0;
    /* enum ctor е като литерал — payload-ът е owned от ctor сайта */
    if (lrc_is_enum_ctor(n)) return 0;
    return lrc_heap_tag(n->type) != 0;
}

/* pre-order събиране (порт на rc_tmp_collect, codegen_c.c:1281). Не се слиза
 * в: STRUCT_LIT (полетата escape-ват), LAMBDA, TRY/CATCH, IF-израз, десен
 * операнд на &&/||, drop(…) аргументи, enum ctor аргументи, match рамена. */
static void lrc_tmp_collect(Node *n, int is_root) {
    if (!n) return;
    if (lrc_tmp_fresh(n) && !is_root) {
        if (lg.lrc_tmp_count >= 64)
            llvm_unsupported("прекалено много temp-ове в един statement");
        lg.lrc_tmps[lg.lrc_tmp_count++] = (LLRcTmp){
            n, n->kind == NODE_TO_STR ? 1 : lrc_heap_tag(n->type),
            n->type, NULL, 0 };
        /* продължаваме надолу — аргументите може да съдържат temp-ове */
    }
    switch (n->kind) {
        case NODE_BINARY:
            lrc_tmp_collect(n->left, 0);
            /* &&/||: десният операнд се оценява условно — не се пипа */
            if (n->bin_op != OP_AND && n->bin_op != OP_OR)
                lrc_tmp_collect(n->right, 0);
            break;
        case NODE_UNARY:
            lrc_tmp_collect(n->operand, 0);
            break;
        case NODE_CALL:
            /* drop(x) е release пътят на x; enum ctor копира payload без
             * retain — temp в него би обесил payload-а (като C) */
            if (n->callee && n->callee->kind == NODE_IDENT &&
                strcmp(n->callee->name, "drop") == 0)
                break;
            if (lrc_is_enum_ctor(n)) break;
            lrc_tmp_collect(n->callee, 0);
            for (int i = 0; i < n->args.len; i++)
                lrc_tmp_collect(n->args.data[i], 0);
            break;
        case NODE_INDEX:
            lrc_tmp_collect(n->obj, 0);
            lrc_tmp_collect(n->index, 0);
            break;
        case NODE_ELEM_REF:
            lrc_tmp_collect(n->elem_obj, 0);
            break;
        case NODE_FIELD:
            lrc_tmp_collect(n->field_obj, 0);
            break;
        case NODE_RANGE:
            lrc_tmp_collect(n->range_lo, 0);
            lrc_tmp_collect(n->range_hi, 0);
            break;
        case NODE_TO_STR:
            lrc_tmp_collect(n->to_str_expr, 0);
            break;
        case NODE_MATCH: {
            /* RC5 v0.11: scrutinee temp (`match f() { ... }`). Scrutinee-то
             * се оценява безусловно веднъж ПРЕДИ рамената, а release-ът идва
             * в края на statement-а — СЛЕД рамената, така че borrowed
             * binding-ите (v0.6 конвенция) остават валидни. В рамената не се
             * слиза. Enum ctor scrutinee с heap payload се регистрира с
             * tag 6 (payload референциите са owned от ctor сайта и никой не
             * ги release-ва след match-а); owned fn резултат — през
             * lrc_tmp_fresh/lrc_heap_tag (v1.0a/b). */
            Node *sc = n->match_expr;
            if (sc && sc->kind == NODE_CALL && lrc_is_enum_ctor(sc) &&
                lrc_heap_tag(sc->type) == 6) {
                if (lg.lrc_tmp_count >= 64)
                    llvm_unsupported("прекалено много temp-ове в един statement");
                lg.lrc_tmps[lg.lrc_tmp_count++] =
                    (LLRcTmp){ sc, 6, sc->type, NULL, 0 };
            } else {
                lrc_tmp_collect(sc, 0);
            }
            break;
        }
        default:
            break;
    }
}

/* индекс на регистриран (и неконсумиран) temp за този AST възел (-1 = не е)
 * — порт на rc_tmp_find (codegen_c.c:1378); за move в контейнер сайтовете */
static int lrc_tmp_find(Node *n) {
    if (!lg.rc || !lg.lrc_tmps_on || !n) return -1;
    for (int i = 0; i < lg.lrc_tmp_count; i++)
        if (!lg.lrc_tmps[i].consumed && lg.lrc_tmps[i].site == n) return i;
    return -1;
}

/* temp-ът е прехвърлен (move) в контейнер — краят на statement-а не го
 * release-ва (порт на rc_tmp_consume, codegen_c.c:1388) */
static void lrc_tmp_consume(int i) {
    if (i >= 0) lg.lrc_tmps[i].consumed = 1;
}

/* release на temp по СТОЙНОСТ (temp-овете нямат alloca — за разлика от
 * lrc_emit_release, която зарежда от alloca по име). Същите helper-и и
 * elem/val kind логика като при локалите. */
static void lrc_emit_release_val(int tag, Type *ty, LLVMValueRef val) {
    if (!val) return;
    if (tag == 1) {
        h_call(baga_rt("baga_rc_release_str"), &val, 1, "");
    } else if (tag == 2) {
        LLVMValueRef d = LLVMBuildExtractValue(lg.builder, val, 0, "rtd");
        h_call(baga_rt("baga_rc_release_bytes"), &d, 1, "");
    } else if (tag == 3) {
        int ek = lrc_vec_elem_kind(ty, NULL);
        LLVMValueRef rel = ek == 3
            ? lrc_nested_vec_rel(ty, NULL)
            : (ty && ty->elem && ty->elem->n_targs > 0
               ? lrc_box_rel_ty(ty->elem)
               : lrc_box_rel(ty && ty->elem ? ty->elem->name : NULL));
        LLVMValueRef a[] = {
            LLVMBuildBitCast(lg.builder, val, lg.ptr_ty, "rtv"),
            LLVMConstInt(lg.i64_ty, (uint64_t)ek, 0),
            LLVMConstInt(lg.i64_ty, 0, 0),   /* elem_size — не се ползва */
            rel,
        };
        h_call(baga_rt("baga_rc_release_vec"), a, 4, "");
    } else if (tag == 4) {
        LLVMValueRef a[] = {
            LLVMBuildBitCast(lg.builder, val, lg.ptr_ty, "rtm"),
            LLVMConstInt(lg.i64_ty, (uint64_t)lrc_map_val_tag(ty, NULL), 0),
            LLVMConstInt(lg.i64_ty, 0, 0),   /* val_size — не се ползва */
            (ty && ty->elem && ty->elem->n_targs > 0
             ? lrc_box_rel_ty(ty->elem)
             : lrc_box_rel(ty && ty->elem ? ty->elem->name : NULL)),
        };
        h_call(baga_rt("baga_rc_release_map"), a, 4, "");
    } else if (tag == 5 || tag == 6) {
        /* M24-RC: instantiated generic struct — per-instance helper */
        if (ty && ty->kind == TYPE_STRUCT && ty->n_targs > 0) {
            h_call(lrc_rc_fn_ty(ty, 1), &val, 1, "");
            return;
        }
        const char *sn = (ty && ty->name) ? ty->name : NULL;
        if (!sn) return;
        h_call(lrc_rc_fn(sn, 1), &val, 1, "");
    }
}

/* release на активните неконсумирани temp-ове (в реда на събирането, като
 * rc_tmp_release_all в C) и изчистване на регистъра */
static void lrc_tmp_release_all(void) {
    if (!lg.rc || !lg.lrc_tmps_on) return;
    for (int i = 0; i < lg.lrc_tmp_count; i++) {
        LLRcTmp *t = &lg.lrc_tmps[i];
        if (t->consumed) continue;   /* move в контейнер — box-ът е собственик */
        lrc_emit_release_val(t->tag, t->type, t->val);
    }
    lg.lrc_tmp_count = 0;
    lg.lrc_tmps_on = 0;
}

/* save/restore за вложени statement-и (блок-стойности, ламбди — порт на
 * saved_tmps/saved_on двойката в rc_tmp_begin/rc_tmp_end) */
typedef struct { int count; int on; Node *decl; LLRcTmp e[64]; } LLRcTmpSave;

static void lrc_tmp_save(LLRcTmpSave *sv) {
    sv->count = lg.lrc_tmp_count;
    sv->on = lg.lrc_tmps_on;
    sv->decl = lg.lrc_tmp_decl;
    memcpy(sv->e, lg.lrc_tmps, sizeof sv->e);
}

static void lrc_tmp_restore(LLRcTmpSave *sv) {
    memcpy(lg.lrc_tmps, sv->e, sizeof sv->e);
    lg.lrc_tmp_count = sv->count;
    lg.lrc_tmps_on = sv->on;
    lg.lrc_tmp_decl = sv->decl;
}

/* начало на statement с temp tracking (порт на rc_tmp_begin,
 * codegen_c.c:1396). root_bound=1: root-ът е bound (let init / assign дясно /
 * return стойност) — не е temp. Temp-овете се emit-ват веднага (вътрешните
 * първи — обратен ред на събирането, като декларациите в C) и се кешират. */
static void lrc_tmp_begin(Node *root, int root_bound, LLRcTmpSave *sv) {
    lrc_tmp_save(sv);
    lg.lrc_tmp_count = 0;
    lg.lrc_tmps_on = 0;
    lg.lrc_tmp_decl = NULL;
    if (!lg.rc || !root) return;
    if (root->kind == NODE_ASSIGN) {
        /* дясното е bound; сложните цели четат обекта/индекса */
        lrc_tmp_collect(root->assign_val, 1);
        Node *t = root->assign_target;
        if (t) {
            if (t->kind == NODE_FIELD) lrc_tmp_collect(t->field_obj, 0);
            else if (t->kind == NODE_INDEX) {
                lrc_tmp_collect(t->obj, 0);
                lrc_tmp_collect(t->index, 0);
            }
        }
    } else {
        lrc_tmp_collect(root, root_bound);
    }
    if (lg.lrc_tmp_count == 0) return;
    lg.lrc_tmps_on = 1;
    for (int i = lg.lrc_tmp_count - 1; i >= 0; i--) {
        lg.lrc_tmp_decl = lg.lrc_tmps[i].site;
        lg.lrc_tmps[i].val = emit_expr_llvm(lg.lrc_tmps[i].site);
        lg.lrc_tmp_decl = NULL;
        if (!lg.lrc_tmps[i].val)
            llvm_unsupported("print/drop като temp израз");
    }
}

/* край на statement-а: release (ако не е направен) + възстановяване */
static void lrc_tmp_end(LLRcTmpSave *sv) {
    lrc_tmp_release_all();
    lrc_tmp_restore(sv);
}

static void lrc_push_scope(int is_loop) {
    if (!lg.rc) return;
    if (lg.lrc_depth >= 64)
        llvm_unsupported("прекалено дълбока RC scope вложеност");
    lg.lrc_scopes[lg.lrc_depth++] = (LLRcScope){ lg.lrc_count, is_loop };
}

/* край на блок: release на локалите над top в обратен ред (без params/dead),
 * после pop. Ако блокът вече има terminator (return/break/continue са
 * release-нали по пътя) — само pop, без дублиране. */
static void lrc_pop_scope(void) {
    if (!lg.rc) return;
    LLRcScope sc = lg.lrc_scopes[--lg.lrc_depth];
    if (!LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(lg.builder))) {
        for (int i = lg.lrc_count - 1; i >= sc.top; i--) {
            LLRcLocal *l = &lg.lrc_locals[i];
            if (!l->is_param && !l->dead) lrc_emit_release(l);
        }
    }
    lg.lrc_count = sc.top;
}

/* return: release на всички локали над fn scope-а (без params/dead);
 * skip_idx = returned стойността (move — собствеността отива при caller-а).
 * Пропускът е по ИНДЕКС, не чрез dead флага: dead е compile-time състояние,
 * споделено между пътищата, и би „изтрило" локала и на другите пътища
 * (първият emit-нат return печели) — rc_release_all в codegen_c работи
 * по същия начин (skip_idx). Scope-овете НЕ се попват — функцията
 * приключва; следващата fn emission reset-ва. */
static void lrc_release_all(int skip_idx) {
    if (!lg.rc) return;
    int base = lg.lrc_fn_base >= 0 && lg.lrc_fn_base < lg.lrc_depth
        ? lg.lrc_scopes[lg.lrc_fn_base].top : 0;
    for (int i = lg.lrc_count - 1; i >= base; i--) {
        LLRcLocal *l = &lg.lrc_locals[i];
        if (l->is_param || l->dead || i == skip_idx) continue;
        lrc_emit_release(l);
    }
}

/* break/continue: release на локалите до най-близкия loop scope (вкл. него —
 * нормалният му release в края на тялото се прескача от скока). Само
 * emission: lrc_depth/lrc_count не се пипат, защото LLVM продължава линейно;
 * блокът получава terminator (br) и lrc_pop_scope няма да дублира. */
static void lrc_release_to_loop(void) {
    if (!lg.rc) return;
    for (int i = lg.lrc_depth - 1; i >= 0; i--) {
        if (!lg.lrc_scopes[i].is_loop) continue;
        int top = lg.lrc_scopes[i].top;
        for (int j = lg.lrc_count - 1; j >= top; j--) {
            LLRcLocal *l = &lg.lrc_locals[j];
            if (!l->is_param && !l->dead) lrc_emit_release(l);
        }
        return;
    }
}

/* move семантика за return (explicit или implicit от последен EXPR_STMT):
 * ако изразът е track-нат IDENT — подава се като skip_idx на release_all
 * (собствеността отива при caller-а); върнат ПАРАМЕТЪР се retain-ва
 * (borrowed → owned, codegen_c.c:3739-3755). Израз (не-IDENT) — без skip. */
static void lrc_return_move(Node *expr, LLVMValueRef v) {
    if (!lg.rc) return;
    int ri = expr && expr->kind == NODE_IDENT ? lrc_find(expr->name) : -1;
    if (ri >= 0 && lg.lrc_locals[ri].is_param)
        lrc_emit_retain_val(lg.lrc_locals[ri].tag, lg.lrc_locals[ri].type,
                            lg.lrc_locals[ri].type_node, v);
    lrc_release_all(ri);
}

/* NODE_WHILE/NODE_FOR вдигат този флаг преди emit_block_llvm на тялото —
 * следващият lrc_push_scope е loop scope (прочита се в emit_block_llvm). */
static int lrc_loop_next;

/* ============================ M20 payload runtime ============================ */

static int llvm_has_payload_effects(Type *t) {
    if (!t || !t->effect_payloads) return 0;
    for (int i = 0; i < t->n_effects; i++)
        if (t->effect_payloads[i]) return 1;
    return 0;
}

/* Tag на ефект по име — детерминистичен (първа поява = 1, 2, …). */
static int llvm_eff_tag(const char *name) {
    for (int i = 0; i < lg.eff_tags.len; i++)
        if (strcmp(lg.eff_tags.data[i], name) == 0) return i + 1;
    vec_push(lg.eff_tags, strdup(name));
    return lg.eff_tags.len;
}

/* { i64 tag, i64 i, double f, i8* s, baga_bytes b } */
static LLVMTypeRef baga_eff_ty(void) {
    if (lg.eff_ty) return lg.eff_ty;
    LLVMTypeRef t = LLVMStructCreateNamed(lg.ctx, "baga_eff");
    LLVMTypeRef elems[] = { lg.i64_ty, lg.i64_ty, lg.double_ty,
                            lg.ptr_ty, baga_bytes_ty() };
    LLVMStructSetBody(t, elems, 5, 0);
    lg.eff_ty = t;
    return t;
}

static LLVMValueRef eff_global(void) {
    if (lg.eff_global) return lg.eff_global;
    lg.eff_global = LLVMAddGlobal(lg.mod, baga_eff_ty(), "baga_eff_tl");
    LLVMSetInitializer(lg.eff_global, LLVMConstNull(baga_eff_ty()));
    LLVMSetThreadLocal(lg.eff_global, 1);
    return lg.eff_global;
}

/* GEP до поле idx на глобалния слот (в текущия block) */
static LLVMValueRef eff_gep(unsigned idx, const char *name) {
    LLVMValueRef g = eff_global();
    LLVMValueRef gp = LLVMBuildStructGEP2(lg.builder, baga_eff_ty(), g,
                                          idx, name);
    return gp;
}

/* Полето на payload-а по тип (0=tag, 1=i, 2=f, 3=s, 4=b) */
static unsigned eff_field_of(Type *pt) {
    if (pt->kind == TYPE_F64) return 2;
    if (pt->kind == TYPE_STR) return 3;
    if (pt->kind == TYPE_BYTES) return 4;
    return 1;
}

/* return <нулата на cur_ret_ty> — propagate пътят */
static void h_ret_zero(void) {
    if (lg.cur_ret_ty == lg.void_ty) LLVMBuildRetVoid(lg.builder);
    else LLVMBuildRet(lg.builder, LLVMConstNull(lg.cur_ret_ty));
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
    LLVMValueRef r = rc_alloc_call(n1, "r");
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
    LLVMValueRef r = rc_alloc_call(n1, "r");
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
    LLVMValueRef r = rc_alloc_call(five, "r");

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
    LLVMValueRef res = rc_alloc_call(rlen, "res");
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

/* static const char *baga_f64_to_str(double x) — "%g" като baga_print_f64;
 * оглежда C runtime helper-а (snprintf в 32-байтов buffer от malloc). */
static LLVMValueRef build_baga_f64_to_str(void) {
    LLVMTypeRef p[] = { LLVMDoubleTypeInContext(lg.ctx) };
    LLVMValueRef fn = LLVMAddFunction(lg.mod, "baga_f64_to_str",
        LLVMFunctionType(lg.ptr_ty, p, 1, 0));
    h_begin(fn);
    LLVMValueRef x = LLVMGetParam(fn, 0);
    LLVMValueRef i32 = LLVMConstInt(lg.i64_ty, 32, 0);
    LLVMValueRef r = rc_alloc_call(i32, "r");
    LLVMValueRef fmt = LLVMBuildGlobalStringPtr(lg.builder, "%g", "f64fmt");
    LLVMValueRef args[] = { r, i32, fmt, x };
    LLVMBuildCall2(lg.builder, LLVMGetElementType(LLVMTypeOf(lg.snprintf_fn)),
                   lg.snprintf_fn, args, 4, "");
    LLVMBuildRet(lg.builder, r);
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
 *         v->data = realloc(v->data, v->cap * sizeof(void *)); } }
 * Под --rc: нов rc_alloc + memcpy + free на старата база през header-а
 * (realloc би преместил/дублирал header-а със стария `an`; C прави
 * alloc+memcpy+free винаги — codegen_c.c:5653). */
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
    if (lg.rc) {
        LLVMValueRef nd = rc_alloc_call(nbytes, "nd");
        LLVMValueRef od = LLVMBuildBitCast(lg.builder, data, lg.ptr_ty, "od");
        LLVMValueRef cpy = LLVMBuildMul(lg.builder, len,
            LLVMConstInt(lg.i64_ty, 8, 0), "cpy");
        LLVMValueRef mc[] = { nd, od, cpy };
        h_call(rt_memcpy(), mc, 3, "");
        LLVMValueRef hd = h_call(baga_rt("baga_rc_hdr"), &od, 1, "hd");
        h_call(rt_free(), &hd, 1, "");
        LLVMBuildStore(lg.builder,
            LLVMBuildBitCast(lg.builder, nd, LLVMPointerType(lg.ptr_ty, 0), "ndc"),
            vec_field_ptr(v, 0, "datap"));
    } else {
        LLVMValueRef ra[] = {
            LLVMBuildBitCast(lg.builder, data, lg.ptr_ty, "raw"),
            nbytes
        };
        LLVMValueRef nd = h_call(rt_realloc(), ra, 2, "nd");
        LLVMBuildStore(lg.builder,
            LLVMBuildBitCast(lg.builder, nd, LLVMPointerType(lg.ptr_ty, 0), "ndc"),
            vec_field_ptr(v, 0, "datap"));
    }
    LLVMBuildBr(lg.builder, done_bb);
    LLVMPositionBuilderAtEnd(lg.builder, done_bb);
    LLVMBuildRetVoid(lg.builder);
    return fn;
}

/* static baga_Vec *baga_vec_new(void) {
 *     baga_Vec *v = malloc(sizeof(baga_Vec));
 *     v->cap = 8; v->len = 0; v->data = malloc(8 * sizeof(void *)); return v; }
 * Под --rc и struct-ът, и data масивът носят header (rc живее върху struct-а;
 * data е притежаван 1:1 и се free-ва от release през header базата). */
static LLVMValueRef build_baga_vec_new(void) {
    LLVMValueRef fn = LLVMAddFunction(lg.mod, "baga_vec_new",
        LLVMFunctionType(baga_vec_ptr_ty(), NULL, 0, 0));
    h_begin(fn);
    LLVMValueRef sz = LLVMSizeOf(baga_vec_ty());
    LLVMValueRef raw = rc_alloc_call(sz, "raw");
    LLVMValueRef v = LLVMBuildBitCast(lg.builder, raw, baga_vec_ptr_ty(), "v");
    LLVMBuildStore(lg.builder, LLVMConstInt(lg.i64_ty, 8, 0),
                   vec_field_ptr(v, 2, "capp"));
    LLVMBuildStore(lg.builder, LLVMConstInt(lg.i64_ty, 0, 0),
                   vec_field_ptr(v, 1, "lenp"));
    LLVMValueRef draw = rc_alloc_call(LLVMConstInt(lg.i64_ty, 64, 0), "draw");
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
 * { baga_vec_grow(v); v->data[v->len++] = (void *)s; }
 * Под --rc: retain(s) преди grow — контейнерът става собственик
 * (codegen_c.c:5674); release е в baga_rc_release_vec (elem_kind 1). */
static LLVMValueRef build_baga_vec_push_str(void) {
    LLVMTypeRef p[] = { baga_vec_ptr_ty(), lg.ptr_ty };
    LLVMValueRef fn = LLVMAddFunction(lg.mod, "baga_vec_push_str",
        LLVMFunctionType(lg.void_ty, p, 2, 0));
    h_begin(fn);
    if (lg.rc) {
        LLVMValueRef s = LLVMGetParam(fn, 1);
        h_call(baga_rt("baga_rc_retain"), &s, 1, "rcr");
    }
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
 * { v->data[i] = (void *)s; }
 * Под --rc: retain на новия, после release на стария (в този ред —
 * alias-safe, codegen_c.c:5684). */
static LLVMValueRef build_baga_vec_set_str(void) {
    LLVMTypeRef p[] = { baga_vec_ptr_ty(), lg.i64_ty, lg.ptr_ty };
    LLVMValueRef fn = LLVMAddFunction(lg.mod, "baga_vec_set_str",
        LLVMFunctionType(lg.void_ty, p, 3, 0));
    h_begin(fn);
    LLVMValueRef v = LLVMGetParam(fn, 0);
    LLVMValueRef i = LLVMGetParam(fn, 1);
    LLVMValueRef data = vec_load_data(v);
    LLVMValueRef slot = LLVMBuildGEP2(lg.builder, lg.ptr_ty, data, &i, 1, "slot");
    if (lg.rc) {
        LLVMValueRef s = LLVMGetParam(fn, 2);
        h_call(baga_rt("baga_rc_retain"), &s, 1, "rcr");
        LLVMValueRef old = LLVMBuildLoad2(lg.builder, lg.ptr_ty, slot, "old");
        h_call(baga_rt("baga_rc_release_str"), &old, 1, "");
    }
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
    return rc_alloc_call(sz, "bdata");
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
    LLVMValueRef r = rc_alloc_call(sz, "r");
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
    LLVMValueRef r = rc_alloc_call(sz, "r");
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
    LLVMValueRef buf = rc_alloc_call(cap, "buf");
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
    LLVMValueRef buf = rc_alloc_call(sz1, "buf");
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

/* static baga_Map *baga_map_new(void) — 16 празни кофи.
 * Под --rc struct-ът носи header (rc живее върху него); bucket масивът
 * остава plain malloc — притежаван 1:1, умира с контейнера (като в C:
 * release_map го free-ва директно, никой не го retain-ва). */
static LLVMValueRef build_baga_map_new(void) {
    LLVMValueRef fn = LLVMAddFunction(lg.mod, "baga_map_new",
        LLVMFunctionType(baga_map_ptr_ty(), NULL, 0, 0));
    h_begin(fn);
    LLVMValueRef m = LLVMBuildBitCast(lg.builder,
        rc_alloc_call(LLVMSizeOf(baga_map_ty()), "raw"), baga_map_ptr_ty(), "m");
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
 * len++, rehash при load > 3/4. ik/sk/bk/ktag са вече изчислени.
 * rc_key (само под --rc): 1 = str ключ, 2 = bytes ключ — retain-ът от put
 * се ПУСКА, ако entry-то вече съществува (ключът не се пъха втори път —
 * codegen_c.c:5933/6062). */
static LLVMValueRef map_put_finish(LLVMValueRef fn, LLVMValueRef m,
                                   LLVMValueRef slot, LLVMValueRef ik,
                                   LLVMValueRef sk, LLVMValueRef bk,
                                   LLVMValueRef ktag, int rc_key) {
    LLVMValueRef found = LLVMBuildLoad2(lg.builder, baga_map_entry_ptr_ty(),
        slot, "found");
    LLVMBasicBlockRef have = LLVMAppendBasicBlockInContext(lg.ctx, fn, "have");
    LLVMBasicBlockRef ins  = LLVMAppendBasicBlockInContext(lg.ctx, fn, "ins");
    LLVMBuildCondBr(lg.builder, LLVMBuildIsNull(lg.builder, found, "isn"),
                    ins, have);
    LLVMPositionBuilderAtEnd(lg.builder, have);
    if (lg.rc && rc_key == 1) {
        h_call(baga_rt("baga_rc_release_str"), &sk, 1, "");
    } else if (lg.rc && rc_key == 2) {
        LLVMValueRef kd = LLVMBuildExtractValue(lg.builder, bk, 0, "kd");
        h_call(baga_rt("baga_rc_release_bytes"), &kd, 1, "");
    }
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
 *     const char *sk, uint64_t h) — ktag = sk ? 1 : 0
 * Под --rc: retain(sk) преди slot (претенция за съхранение); ако entry-то
 * съществува, map_put_finish пуска retain-а (codegen_c.c:5930-5933).
 * retain/release на NULL (i64 ключ) са no-op по magic guard-а. */
static LLVMValueRef build_baga_map_put(void) {
    LLVMTypeRef p[] = { baga_map_ptr_ty(), lg.i64_ty, lg.ptr_ty, lg.i64_ty };
    LLVMValueRef fn = LLVMAddFunction(lg.mod, "baga_map_put",
        LLVMFunctionType(baga_map_entry_ptr_ty(), p, 4, 0));
    h_begin(fn);
    LLVMValueRef m = LLVMGetParam(fn, 0);
    LLVMValueRef ik = LLVMGetParam(fn, 1);
    LLVMValueRef sk = LLVMGetParam(fn, 2);
    if (lg.rc)
        h_call(baga_rt("baga_rc_retain"), &sk, 1, "rcr");
    LLVMValueRef sa[] = { m, ik, sk, LLVMGetParam(fn, 3) };
    LLVMValueRef slot = h_call(baga_rt("baga_map_slot"), sa, 4, "slot");
    LLVMValueRef ktag = LLVMBuildSelect(lg.builder,
        LLVMBuildIsNull(lg.builder, sk, "skn"),
        LLVMConstInt(lg.i64_ty, 0, 0), LLVMConstInt(lg.i64_ty, 1, 0), "ktag");
    map_put_finish(fn, m, slot, ik, sk,
                   LLVMConstNull(baga_bytes_ty()), ktag, 1);
    return fn;
}

/* static baga_MapEntry *baga_map_put_b(baga_Map *m, baga_bytes k,
 *     uint64_t h) — R67 bytes ключ, ktag = 2
 * Под --rc: retain(k.data) преди slot; пуска се при съществуващо entry
 * (codegen_c.c:6059-6062). */
static LLVMValueRef build_baga_map_put_b(void) {
    LLVMTypeRef p[] = { baga_map_ptr_ty(), baga_bytes_ty(), lg.i64_ty };
    LLVMValueRef fn = LLVMAddFunction(lg.mod, "baga_map_put_b",
        LLVMFunctionType(baga_map_entry_ptr_ty(), p, 3, 0));
    h_begin(fn);
    LLVMValueRef m = LLVMGetParam(fn, 0);
    LLVMValueRef ka = bytes_param_alloca(LLVMGetParam(fn, 1));
    LLVMValueRef kv = LLVMBuildLoad2(lg.builder, baga_bytes_ty(), ka, "kv");
    if (lg.rc) {
        LLVMValueRef kd = LLVMBuildExtractValue(lg.builder, kv, 0, "kd");
        h_call(baga_rt("baga_rc_retain"), &kd, 1, "rcr");
    }
    LLVMValueRef sa[] = { m, kv, LLVMGetParam(fn, 2) };
    LLVMValueRef slot = h_call(baga_rt("baga_map_slot_b"), sa, 3, "slot");
    map_put_finish(fn, m, slot, LLVMConstInt(lg.i64_ty, 0, 0),
                   LLVMConstNull(lg.ptr_ty), kv,
                   LLVMConstInt(lg.i64_ty, 2, 0), 2);
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
    /* Под --rc: str/bytes стойността се retain-ва; при overwrite — release
     * на старата ПРЕДИ store (codegen_c.c:5961-5962/5982-5983). Старите
     * слотове на нов запис са NULL → release-ът е no-op по magic guard-а.
     * box (struct/enum) стойностите се retain-ват/release-ват от call
     * site-а (той знае типа — baga_map_set_<k>_box е generic). */
    if (lg.rc && val == 1) {
        LLVMValueRef sv = LLVMGetParam(fn, 2);
        h_call(baga_rt("baga_rc_retain"), &sv, 1, "rcr");
        LLVMValueRef old = LLVMBuildLoad2(lg.builder, lg.ptr_ty,
            ent_fld(e, 6, "svp"), "old");
        h_call(baga_rt("baga_rc_release_str"), &old, 1, "");
    } else if (lg.rc && val == 3) {
        LLVMValueRef vd = LLVMBuildExtractValue(lg.builder,
            LLVMGetParam(fn, 2), 0, "vd");
        h_call(baga_rt("baga_rc_retain"), &vd, 1, "rcr");
        LLVMValueRef ob = LLVMBuildLoad2(lg.builder, lg.ptr_ty,
            LLVMBuildStructGEP2(lg.builder, baga_bytes_ty(),
                ent_fld(e, 7, "bvp"), 0, "obdp"), "obd");
        h_call(baga_rt("baga_rc_release_bytes"), &ob, 1, "");
    }
    if (val != 4) {
        unsigned fld = val == 0 ? 4 : val == 1 ? 6 : val == 2 ? 5 : 7;
        LLVMBuildStore(lg.builder, LLVMGetParam(fn, 2), ent_fld(e, fld, "vp"));
        LLVMBuildRetVoid(lg.builder);
        return fn;
    }
    /* box: if (!e->pv) e->pv = malloc(size); memcpy(e->pv, src, size)
     * Под --rc box-ът носи header (rc_alloc_call) — release_map го free-ва
     * през базата след destructor-а на полетата. */
    LLVMValueRef size = LLVMGetParam(fn, 3);
    LLVMValueRef pvp = ent_fld(e, 8, "pvp");
    LLVMValueRef pv = LLVMBuildLoad2(lg.builder, lg.ptr_ty, pvp, "pv");
    LLVMBasicBlockRef alb = LLVMAppendBasicBlockInContext(lg.ctx, fn, "alb");
    LLVMBasicBlockRef cpb = LLVMAppendBasicBlockInContext(lg.ctx, fn, "cpb");
    LLVMBuildCondBr(lg.builder, LLVMBuildIsNull(lg.builder, pv, "isn"), alb, cpb);
    LLVMPositionBuilderAtEnd(lg.builder, alb);
    LLVMBuildStore(lg.builder, rc_alloc_call(size, "box"), pvp);
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

/* static void baga_map_del_<key>(m, k) — извършва записа от веригата.
 * Под --rc: release на ключа (по ktag) и на str/bytes стойността + free на
 * entry shell-а (codegen_c.c:6021-6028). pv (box) не се пипа — както в C
 * (del не знае val_size; release_map/drop_map я чисти). */
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
    if (lg.rc) {
        /* ключ: ktag == 2 → bytes, иначе str (NULL при i64 → no-op) */
        LLVMValueRef ktag = LLVMBuildLoad2(lg.builder, lg.i64_ty,
            ent_fld(e, 3, "ktp"), "ktag");
        LLVMValueRef isb = LLVMBuildICmp(lg.builder, LLVMIntEQ, ktag,
            LLVMConstInt(lg.i64_ty, 2, 0), "isb");
        LLVMBasicBlockRef kb = LLVMAppendBasicBlockInContext(lg.ctx, fn, "kb");
        LLVMBasicBlockRef ks = LLVMAppendBasicBlockInContext(lg.ctx, fn, "ks");
        LLVMBasicBlockRef kd = LLVMAppendBasicBlockInContext(lg.ctx, fn, "kd");
        LLVMBuildCondBr(lg.builder, isb, kb, ks);
        LLVMPositionBuilderAtEnd(lg.builder, kb);
        LLVMValueRef bkd = LLVMBuildLoad2(lg.builder, lg.ptr_ty,
            LLVMBuildStructGEP2(lg.builder, baga_bytes_ty(),
                ent_fld(e, 2, "bkp"), 0, "bkdp"), "bkd");
        h_call(baga_rt("baga_rc_release_bytes"), &bkd, 1, "");
        LLVMBuildBr(lg.builder, kd);
        LLVMPositionBuilderAtEnd(lg.builder, ks);
        LLVMValueRef sk = LLVMBuildLoad2(lg.builder, lg.ptr_ty,
            ent_fld(e, 1, "skp"), "sk");
        h_call(baga_rt("baga_rc_release_str"), &sk, 1, "");
        LLVMBuildBr(lg.builder, kd);
        LLVMPositionBuilderAtEnd(lg.builder, kd);
        /* стойност: sv/bv се release-ват безусловно — неизползваните слотове
         * са NULL и release-ът е no-op (като codegen_c) */
        LLVMValueRef sv = LLVMBuildLoad2(lg.builder, lg.ptr_ty,
            ent_fld(e, 6, "svp"), "sv");
        h_call(baga_rt("baga_rc_release_str"), &sv, 1, "");
        LLVMValueRef bvd = LLVMBuildLoad2(lg.builder, lg.ptr_ty,
            LLVMBuildStructGEP2(lg.builder, baga_bytes_ty(),
                ent_fld(e, 7, "bvp"), 0, "bvdp"), "bvd");
        h_call(baga_rt("baga_rc_release_bytes"), &bvd, 1, "");
        LLVMValueRef er = LLVMBuildBitCast(lg.builder, e, lg.ptr_ty, "er");
        h_call(rt_free(), &er, 1, "");
    }
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
 * както останалите LLVM vec helper-и. Под --rc box-ът е rc_alloc (header) —
 * release_vec го free-ва през базата след destructor-а на полетата. */
static LLVMValueRef build_baga_vec_push_box(void) {
    LLVMTypeRef p[] = { baga_vec_ptr_ty(), lg.ptr_ty, lg.i64_ty };
    LLVMValueRef fn = LLVMAddFunction(lg.mod, "baga_vec_push_box",
        LLVMFunctionType(lg.void_ty, p, 3, 0));
    h_begin(fn);
    LLVMValueRef v = LLVMGetParam(fn, 0);
    LLVMValueRef gv[] = { v };
    h_call(baga_rt("baga_vec_grow"), gv, 1, "");
    LLVMValueRef box = rc_alloc_call(LLVMGetParam(fn, 2), "box");
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

/* ---- MEM-1: drop helpers (само не-RC пътя; под --rc drop ≡ release) ----
 * Порт на baga_drop_* от codegen_c.c:6188-6214. Внимание: без --rc
 * алокациите са plain malloc БЕЗ header — free(p) директно, никакъв
 * baga_rc_hdr. elem_size/val_size се пазят само за сигнатурен паритет с C
 * (libc free не иска размер). */

/* static void baga_drop_str(i8 *p) { free(p); }
 * checker-ът (checker.c:1835) засега отказва drop върху str — този helper
 * е defensive, за пълнота на dispatch-а. */
static LLVMValueRef build_baga_drop_str(void) {
    LLVMTypeRef p[] = { lg.ptr_ty };
    LLVMValueRef fn = LLVMAddFunction(lg.mod, "baga_drop_str",
        LLVMFunctionType(lg.void_ty, p, 1, 0));
    h_begin(fn);
    LLVMValueRef ptr = LLVMGetParam(fn, 0);
    h_call(rt_free(), &ptr, 1, "");
    LLVMBuildRetVoid(lg.builder);
    return fn;
}

/* static void baga_drop_bytes(baga_bytes b) { free(b.data); } */
static LLVMValueRef build_baga_drop_bytes(void) {
    LLVMTypeRef p[] = { baga_bytes_ty() };
    LLVMValueRef fn = LLVMAddFunction(lg.mod, "baga_drop_bytes",
        LLVMFunctionType(lg.void_ty, p, 1, 0));
    h_begin(fn);
    LLVMValueRef a = bytes_param_alloca(LLVMGetParam(fn, 0));
    LLVMValueRef d = bytes_load_data(a);
    h_call(rt_free(), &d, 1, "");
    LLVMBuildRetVoid(lg.builder);
    return fn;
}

/* static void baga_drop_vec(baga_Vec *v, i64 elem_kind, i64 elem_size)
 * { if (!v) return;
 *   if (elem_kind == 2) for (i < len) free(v->data[i]);
 *   if (elem_kind == 3) for (i < len) baga_drop_vec(v->data[i], 0, 0);
 *   free(v->data); free(v); }
 * elem_kind: 0 = inline (i64/f64), 1 = str (споделени — не се пипат),
 * 2 = box (bytes/struct), 3 = вложен Vec. */
static LLVMValueRef build_baga_drop_vec(void) {
    LLVMTypeRef p[] = { baga_vec_ptr_ty(), lg.i64_ty, lg.i64_ty };
    LLVMValueRef fn = LLVMAddFunction(lg.mod, "baga_drop_vec",
        LLVMFunctionType(lg.void_ty, p, 3, 0));
    h_begin(fn);
    LLVMValueRef v = LLVMGetParam(fn, 0);
    LLVMValueRef ek = LLVMGetParam(fn, 1);
    LLVMValueRef zero = LLVMConstInt(lg.i64_ty, 0, 0);
    LLVMBasicBlockRef go_bb  = LLVMAppendBasicBlockInContext(lg.ctx, fn, "go");
    LLVMBasicBlockRef ret_bb = LLVMAppendBasicBlockInContext(lg.ctx, fn, "ret");
    LLVMBuildCondBr(lg.builder, LLVMBuildIsNull(lg.builder, v, "isn"),
                    ret_bb, go_bb);

    LLVMPositionBuilderAtEnd(lg.builder, go_bb);
    LLVMValueRef data = vec_load_data(v);
    LLVMValueRef len = vec_load_len(v);
    LLVMValueRef bi = entry_alloca(lg.i64_ty, "i");
    LLVMBuildStore(lg.builder, zero, bi);
    LLVMValueRef ni = entry_alloca(lg.i64_ty, "j");
    LLVMBuildStore(lg.builder, zero, ni);
    LLVMBasicBlockRef boxc_bb  = LLVMAppendBasicBlockInContext(lg.ctx, fn, "boxc");
    LLVMBasicBlockRef boxb_bb  = LLVMAppendBasicBlockInContext(lg.ctx, fn, "boxb");
    LLVMBasicBlockRef nchk_bb  = LLVMAppendBasicBlockInContext(lg.ctx, fn, "nchk");
    LLVMBasicBlockRef nestc_bb = LLVMAppendBasicBlockInContext(lg.ctx, fn, "nestc");
    LLVMBasicBlockRef nestb_bb = LLVMAppendBasicBlockInContext(lg.ctx, fn, "nestb");
    LLVMBasicBlockRef fin_bb   = LLVMAppendBasicBlockInContext(lg.ctx, fn, "fin");
    LLVMValueRef is_box = LLVMBuildICmp(lg.builder, LLVMIntEQ, ek,
        LLVMConstInt(lg.i64_ty, 2, 0), "isbox");
    LLVMBuildCondBr(lg.builder, is_box, boxc_bb, nchk_bb);

    /* ek == 2: free на всяко box-нато data[i] */
    LLVMPositionBuilderAtEnd(lg.builder, boxc_bb);
    LLVMValueRef i1 = LLVMBuildLoad2(lg.builder, lg.i64_ty, bi, "i");
    LLVMBuildCondBr(lg.builder, LLVMBuildICmp(lg.builder, LLVMIntSLT, i1, len, "cc"),
                    boxb_bb, fin_bb);
    LLVMPositionBuilderAtEnd(lg.builder, boxb_bb);
    LLVMValueRef e1 = vec_load_at(v, i1);
    h_call(rt_free(), &e1, 1, "");
    LLVMBuildStore(lg.builder, LLVMBuildAdd(lg.builder, i1,
        LLVMConstInt(lg.i64_ty, 1, 0), "in"), bi);
    LLVMBuildBr(lg.builder, boxc_bb);

    /* ek == 3: рекурсивен drop на вложените vec-ове */
    LLVMPositionBuilderAtEnd(lg.builder, nchk_bb);
    LLVMValueRef is_nest = LLVMBuildICmp(lg.builder, LLVMIntEQ, ek,
        LLVMConstInt(lg.i64_ty, 3, 0), "isnest");
    LLVMBuildCondBr(lg.builder, is_nest, nestc_bb, fin_bb);
    LLVMPositionBuilderAtEnd(lg.builder, nestc_bb);
    LLVMValueRef i2 = LLVMBuildLoad2(lg.builder, lg.i64_ty, ni, "j");
    LLVMBuildCondBr(lg.builder, LLVMBuildICmp(lg.builder, LLVMIntSLT, i2, len, "cc"),
                    nestb_bb, fin_bb);
    LLVMPositionBuilderAtEnd(lg.builder, nestb_bb);
    LLVMValueRef e2 = LLVMBuildBitCast(lg.builder, vec_load_at(v, i2),
        baga_vec_ptr_ty(), "nv");
    LLVMValueRef ra[] = { e2, zero, zero };
    h_call(fn, ra, 3, "");
    LLVMBuildStore(lg.builder, LLVMBuildAdd(lg.builder, i2,
        LLVMConstInt(lg.i64_ty, 1, 0), "jn"), ni);
    LLVMBuildBr(lg.builder, nestc_bb);

    LLVMPositionBuilderAtEnd(lg.builder, fin_bb);
    LLVMValueRef dr = LLVMBuildBitCast(lg.builder, data, lg.ptr_ty, "dr");
    h_call(rt_free(), &dr, 1, "");
    LLVMValueRef vr = LLVMBuildBitCast(lg.builder, v, lg.ptr_ty, "vr");
    h_call(rt_free(), &vr, 1, "");
    LLVMBuildBr(lg.builder, ret_bb);

    LLVMPositionBuilderAtEnd(lg.builder, ret_bb);
    LLVMBuildRetVoid(lg.builder);
    return fn;
}

/* static void baga_drop_map(baga_Map *m, i64 val_is_box, i64 val_size)
 * { if (!m) return;
 *   for (i < nb) { e = b[i]; while (e) { nx = e->next;
 *     if (val_is_box && e->pv) free(e->pv); free(e); e = nx; } }
 *   free(b); free(m); }
 * next се пази ПРЕДИ free — като в C (libc free не презаписва, но редът е
 * същият за паритет). */
static LLVMValueRef build_baga_drop_map(void) {
    LLVMTypeRef p[] = { baga_map_ptr_ty(), lg.i64_ty, lg.i64_ty };
    LLVMValueRef fn = LLVMAddFunction(lg.mod, "baga_drop_map",
        LLVMFunctionType(lg.void_ty, p, 3, 0));
    h_begin(fn);
    LLVMValueRef m = LLVMGetParam(fn, 0);
    LLVMValueRef vib = LLVMGetParam(fn, 1);
    LLVMBasicBlockRef go_bb  = LLVMAppendBasicBlockInContext(lg.ctx, fn, "go");
    LLVMBasicBlockRef ret_bb = LLVMAppendBasicBlockInContext(lg.ctx, fn, "ret");
    LLVMBuildCondBr(lg.builder, LLVMBuildIsNull(lg.builder, m, "isn"),
                    ret_bb, go_bb);

    LLVMPositionBuilderAtEnd(lg.builder, go_bb);
    LLVMValueRef b = map_load_b(m);
    LLVMValueRef nb = map_load_nb(m);
    LLVMValueRef iv = entry_alloca(lg.i64_ty, "i");
    LLVMBuildStore(lg.builder, LLVMConstInt(lg.i64_ty, 0, 0), iv);
    LLVMValueRef ev = entry_alloca(baga_map_entry_ptr_ty(), "e");
    LLVMBasicBlockRef oc_bb  = LLVMAppendBasicBlockInContext(lg.ctx, fn, "oc");
    LLVMBasicBlockRef ob_bb  = LLVMAppendBasicBlockInContext(lg.ctx, fn, "ob");
    LLVMBasicBlockRef wc_bb  = LLVMAppendBasicBlockInContext(lg.ctx, fn, "wc");
    LLVMBasicBlockRef wb_bb  = LLVMAppendBasicBlockInContext(lg.ctx, fn, "wb");
    LLVMBasicBlockRef vchk_bb = LLVMAppendBasicBlockInContext(lg.ctx, fn, "vchk");
    LLVMBasicBlockRef pvf_bb = LLVMAppendBasicBlockInContext(lg.ctx, fn, "pvf");
    LLVMBasicBlockRef fe_bb  = LLVMAppendBasicBlockInContext(lg.ctx, fn, "fe");
    LLVMBasicBlockRef inx_bb = LLVMAppendBasicBlockInContext(lg.ctx, fn, "inx");
    LLVMBasicBlockRef fin_bb = LLVMAppendBasicBlockInContext(lg.ctx, fn, "fin");
    LLVMBuildBr(lg.builder, oc_bb);

    LLVMPositionBuilderAtEnd(lg.builder, oc_bb);
    LLVMValueRef i = LLVMBuildLoad2(lg.builder, lg.i64_ty, iv, "i");
    LLVMBuildCondBr(lg.builder, LLVMBuildICmp(lg.builder, LLVMIntSLT, i, nb, "cc"),
                    ob_bb, fin_bb);
    LLVMPositionBuilderAtEnd(lg.builder, ob_bb);
    LLVMValueRef e0 = LLVMBuildLoad2(lg.builder, baga_map_entry_ptr_ty(),
        LLVMBuildGEP2(lg.builder, baga_map_entry_ptr_ty(), b, &i, 1, "slot"), "e0");
    LLVMBuildStore(lg.builder, e0, ev);
    LLVMBuildBr(lg.builder, wc_bb);

    LLVMPositionBuilderAtEnd(lg.builder, wc_bb);
    LLVMValueRef e = LLVMBuildLoad2(lg.builder, baga_map_entry_ptr_ty(), ev, "e");
    LLVMBuildCondBr(lg.builder, LLVMBuildIsNull(lg.builder, e, "isn"),
                    inx_bb, wb_bb);
    LLVMPositionBuilderAtEnd(lg.builder, wb_bb);
    LLVMValueRef nx = LLVMBuildLoad2(lg.builder, baga_map_entry_ptr_ty(),
        ent_fld(e, 9, "nxp"), "nx");
    /* if (val_is_box && e->pv) free(e->pv); */
    LLVMValueRef has_v = LLVMBuildICmp(lg.builder, LLVMIntNE, vib,
        LLVMConstInt(lg.i64_ty, 0, 0), "hasv");
    LLVMBuildCondBr(lg.builder, has_v, vchk_bb, fe_bb);
    LLVMPositionBuilderAtEnd(lg.builder, vchk_bb);
    LLVMValueRef pv = LLVMBuildLoad2(lg.builder, lg.ptr_ty,
        ent_fld(e, 8, "pvp"), "pv");
    LLVMBuildCondBr(lg.builder, LLVMBuildIsNull(lg.builder, pv, "pvn"),
                    fe_bb, pvf_bb);
    LLVMPositionBuilderAtEnd(lg.builder, pvf_bb);
    h_call(rt_free(), &pv, 1, "");
    LLVMBuildBr(lg.builder, fe_bb);

    LLVMPositionBuilderAtEnd(lg.builder, fe_bb);
    LLVMValueRef er = LLVMBuildBitCast(lg.builder, e, lg.ptr_ty, "er");
    h_call(rt_free(), &er, 1, "");
    LLVMBuildStore(lg.builder, nx, ev);
    LLVMBuildBr(lg.builder, wc_bb);

    LLVMPositionBuilderAtEnd(lg.builder, inx_bb);
    LLVMBuildStore(lg.builder, LLVMBuildAdd(lg.builder, i,
        LLVMConstInt(lg.i64_ty, 1, 0), "in"), iv);
    LLVMBuildBr(lg.builder, oc_bb);

    LLVMPositionBuilderAtEnd(lg.builder, fin_bb);
    LLVMValueRef br_ = LLVMBuildBitCast(lg.builder, b, lg.ptr_ty, "br");
    h_call(rt_free(), &br_, 1, "");
    LLVMValueRef mr = LLVMBuildBitCast(lg.builder, m, lg.ptr_ty, "mr");
    h_call(rt_free(), &mr, 1, "");
    LLVMBuildBr(lg.builder, ret_bb);

    LLVMPositionBuilderAtEnd(lg.builder, ret_bb);
    LLVMBuildRetVoid(lg.builder);
    return fn;
}

/* ---- Task 5: RC release за Vec/Map (--rc) ----
 * Порт на baga_rc_release_vec (codegen_c.c:5635) и baga_rc_release_map
 * (codegen_c.c:6161). rc живее върху STRUCT алокацията; data/bucket/entry
 * блоковете са притежавани 1:1 и умират с контейнера. Task 6: 4-тият
 * параметър е elem_rel/val_rel destructor (baga_rc_relf_<S> / relv_<S>;
 * NULL → само free); box-овете под --rc носят header (rc_alloc_call при
 * push/set) и се free-ват през базата. Разлика от C:
 *  - elem_size/val_size са сигнатурен паритет — libc free не иска размер. */

/* споделен пролог: hdr (NULL → ret), underflow (stderr + exit(1)), rc--;
 * позиционира builder-а в блока „rc стигна 0" и връща header базата.
 * *ret_bb е общият изход — тялото трябва да завърши с br към него. */
static LLVMValueRef rc_container_prologue(LLVMValueRef fn, LLVMValueRef ptr,
                                          const char *msg,
                                          LLVMBasicBlockRef *ret_bb) {
    LLVMValueRef h = h_call(baga_rt("baga_rc_hdr"), &ptr, 1, "h");
    LLVMValueRef isn = LLVMBuildIsNull(lg.builder, h, "isn");
    LLVMBasicBlockRef dec_bb = LLVMAppendBasicBlockInContext(lg.ctx, fn, "dec");
    *ret_bb = LLVMAppendBasicBlockInContext(lg.ctx, fn, "ret");
    LLVMBuildCondBr(lg.builder, isn, *ret_bb, dec_bb);

    LLVMPositionBuilderAtEnd(lg.builder, dec_bb);
    LLVMValueRef rcp = rc_hdr_field(h, 2, "rcp");
    LLVMValueRef rc = LLVMBuildLoad2(lg.builder, lg.i64_ty, rcp, "rc");
    LLVMValueRef isz = LLVMBuildICmp(lg.builder, LLVMIntEQ, rc,
        LLVMConstInt(lg.i64_ty, 0, 0), "isz");
    LLVMBasicBlockRef uf_bb = LLVMAppendBasicBlockInContext(lg.ctx, fn, "uf");
    LLVMBasicBlockRef ok_bb = LLVMAppendBasicBlockInContext(lg.ctx, fn, "ok");
    LLVMBuildCondBr(lg.builder, isz, uf_bb, ok_bb);

    LLVMPositionBuilderAtEnd(lg.builder, uf_bb);
    LLVMValueRef err = LLVMBuildLoad2(lg.builder, lg.ptr_ty,
        lg.stderr_global, "err");
    LLVMValueRef fmt = LLVMBuildGlobalStringPtr(lg.builder, msg, "uffmt");
    LLVMValueRef fa[] = { err, fmt };
    LLVMBuildCall2(lg.builder, LLVMGetElementType(LLVMTypeOf(lg.fprintf_fn)),
                   lg.fprintf_fn, fa, 2, "");
    LLVMTypeRef ep[] = { lg.i32_ty };
    LLVMValueRef ea[] = { LLVMConstInt(lg.i32_ty, 1, 0) };
    h_call(rt_libc("exit", lg.void_ty, ep, 1), ea, 1, "");
    LLVMBuildUnreachable(lg.builder);

    LLVMPositionBuilderAtEnd(lg.builder, ok_bb);
    LLVMValueRef rc1 = LLVMBuildSub(lg.builder, rc,
        LLVMConstInt(lg.i64_ty, 1, 0), "rc1");
    LLVMBuildStore(lg.builder, rc1, rcp);
    LLVMValueRef isd = LLVMBuildICmp(lg.builder, LLVMIntEQ, rc1,
        LLVMConstInt(lg.i64_ty, 0, 0), "isd");
    LLVMBasicBlockRef dead_bb = LLVMAppendBasicBlockInContext(lg.ctx, fn, "dead");
    LLVMBuildCondBr(lg.builder, isd, dead_bb, *ret_bb);

    LLVMPositionBuilderAtEnd(lg.builder, dead_bb);
    return h;
}

/* static void baga_rc_release_vec(i8 *v, i64 elem_kind, i64 elem_size,
 *                                 i8 *elem_rel)
 * { h = hdr(v); if (!h) ret; if (rc == 0) underflow; if (--rc > 0) ret;
 *   switch (elem_kind): 1 → release_str(data[i]);
 *     2 → if (elem_rel) elem_rel(data[i]); free(hdr(data[i]));
 *     3 → if (elem_rel) elem_rel(data[i]); else release_vec(data[i],0,0,NULL);
 *     4 → release_bytes(data[i]->data) + free(hdr(data[i]));
 *   free(hdr(data)); free(h); }
 * elem_rel е destructor на box полетата (baga_rc_relf_<S> за struct/enum
 * елемент с heap полета, baga_rc_relv_<S> за вложен Vec<S>; NULL → само
 * free — като elem_rel в codegen_c.c:5635). Box-овете под --rc носят
 * header (rc_alloc_call при push) → free през header базата. */
static LLVMValueRef build_baga_rc_release_vec(void) {
    LLVMTypeRef p[] = { lg.ptr_ty, lg.i64_ty, lg.i64_ty, lg.ptr_ty };
    LLVMValueRef fn = LLVMAddFunction(lg.mod, "baga_rc_release_vec",
        LLVMFunctionType(lg.void_ty, p, 4, 0));
    h_begin(fn);
    LLVMBasicBlockRef ret_bb;
    LLVMValueRef h = rc_container_prologue(fn, LLVMGetParam(fn, 0),
        "baga: rc underflow (vec — двоен release)\n", &ret_bb);
    LLVMValueRef v = LLVMBuildBitCast(lg.builder, LLVMGetParam(fn, 0),
        baga_vec_ptr_ty(), "v");
    LLVMValueRef ek = LLVMGetParam(fn, 1);
    LLVMValueRef rel = LLVMGetParam(fn, 3);
    LLVMTypeRef relfty = LLVMFunctionType(lg.void_ty, &lg.ptr_ty, 1, 0);
    LLVMValueRef data = vec_load_data(v);
    LLVMValueRef len = vec_load_len(v);
    LLVMValueRef iv = entry_alloca(lg.i64_ty, "i");
    LLVMValueRef one = LLVMConstInt(lg.i64_ty, 1, 0);
    LLVMValueRef zero = LLVMConstInt(lg.i64_ty, 0, 0);
    LLVMBasicBlockRef c1  = LLVMAppendBasicBlockInContext(lg.ctx, fn, "c1");
    LLVMBasicBlockRef l1c = LLVMAppendBasicBlockInContext(lg.ctx, fn, "l1c");
    LLVMBasicBlockRef l1b = LLVMAppendBasicBlockInContext(lg.ctx, fn, "l1b");
    LLVMBasicBlockRef c2  = LLVMAppendBasicBlockInContext(lg.ctx, fn, "c2");
    LLVMBasicBlockRef l2c = LLVMAppendBasicBlockInContext(lg.ctx, fn, "l2c");
    LLVMBasicBlockRef l2b = LLVMAppendBasicBlockInContext(lg.ctx, fn, "l2b");
    LLVMBasicBlockRef l2r = LLVMAppendBasicBlockInContext(lg.ctx, fn, "l2r");
    LLVMBasicBlockRef l2f = LLVMAppendBasicBlockInContext(lg.ctx, fn, "l2f");
    LLVMBasicBlockRef c3  = LLVMAppendBasicBlockInContext(lg.ctx, fn, "c3");
    LLVMBasicBlockRef l3c = LLVMAppendBasicBlockInContext(lg.ctx, fn, "l3c");
    LLVMBasicBlockRef l3b = LLVMAppendBasicBlockInContext(lg.ctx, fn, "l3b");
    LLVMBasicBlockRef l3r = LLVMAppendBasicBlockInContext(lg.ctx, fn, "l3r");
    LLVMBasicBlockRef l3n = LLVMAppendBasicBlockInContext(lg.ctx, fn, "l3n");
    LLVMBasicBlockRef c4  = LLVMAppendBasicBlockInContext(lg.ctx, fn, "c4");
    LLVMBasicBlockRef l4c = LLVMAppendBasicBlockInContext(lg.ctx, fn, "l4c");
    LLVMBasicBlockRef l4b = LLVMAppendBasicBlockInContext(lg.ctx, fn, "l4b");
    LLVMBasicBlockRef fin = LLVMAppendBasicBlockInContext(lg.ctx, fn, "fin");
    LLVMBuildBr(lg.builder, c1);

    /* ek == 1: str елементи — release на всеки */
    LLVMPositionBuilderAtEnd(lg.builder, c1);
    LLVMBuildStore(lg.builder, zero, iv);
    LLVMBuildCondBr(lg.builder, LLVMBuildICmp(lg.builder, LLVMIntEQ, ek,
        one, "is1"), l1c, c2);
    LLVMPositionBuilderAtEnd(lg.builder, l1c);
    LLVMValueRef i1 = LLVMBuildLoad2(lg.builder, lg.i64_ty, iv, "i");
    LLVMBuildCondBr(lg.builder, LLVMBuildICmp(lg.builder, LLVMIntSLT, i1, len, "cc"),
                    l1b, fin);
    LLVMPositionBuilderAtEnd(lg.builder, l1b);
    LLVMValueRef e1 = vec_load_at(v, i1);
    h_call(baga_rt("baga_rc_release_str"), &e1, 1, "");
    LLVMBuildStore(lg.builder, LLVMBuildAdd(lg.builder, i1, one, "in"), iv);
    LLVMBuildBr(lg.builder, l1c);

    /* ek == 2: struct/enum box — destructor на полетата (ако е подаден),
     * после free на box-а през header базата */
    LLVMPositionBuilderAtEnd(lg.builder, c2);
    LLVMBuildStore(lg.builder, zero, iv);
    LLVMBuildCondBr(lg.builder, LLVMBuildICmp(lg.builder, LLVMIntEQ, ek,
        LLVMConstInt(lg.i64_ty, 2, 0), "is2"), l2c, c3);
    LLVMPositionBuilderAtEnd(lg.builder, l2c);
    LLVMValueRef i2 = LLVMBuildLoad2(lg.builder, lg.i64_ty, iv, "i");
    LLVMBuildCondBr(lg.builder, LLVMBuildICmp(lg.builder, LLVMIntSLT, i2, len, "cc"),
                    l2b, fin);
    LLVMPositionBuilderAtEnd(lg.builder, l2b);
    LLVMValueRef e2 = vec_load_at(v, i2);
    LLVMBuildCondBr(lg.builder, LLVMBuildIsNull(lg.builder, rel, "rn"), l2f, l2r);
    LLVMPositionBuilderAtEnd(lg.builder, l2r);
    LLVMValueRef relf2 = LLVMBuildBitCast(lg.builder, rel,
        LLVMPointerType(relfty, 0), "relf");
    LLVMBuildCall2(lg.builder, relfty, relf2, &e2, 1, "");
    LLVMBuildBr(lg.builder, l2f);
    LLVMPositionBuilderAtEnd(lg.builder, l2f);
    LLVMValueRef h2 = h_call(baga_rt("baga_rc_hdr"), &e2, 1, "hb");
    h_call(rt_free(), &h2, 1, "");
    LLVMBuildStore(lg.builder, LLVMBuildAdd(lg.builder, i2, one, "in"), iv);
    LLVMBuildBr(lg.builder, l2c);

    /* ek == 3: nested Vec — с destructor (relv: Vec<S> с heap полета) или
     * release с kind 0 (елементният тип на вътрешния не е известен по време
     * на изпълнение — като codegen_c) */
    LLVMPositionBuilderAtEnd(lg.builder, c3);
    LLVMBuildStore(lg.builder, zero, iv);
    LLVMBuildCondBr(lg.builder, LLVMBuildICmp(lg.builder, LLVMIntEQ, ek,
        LLVMConstInt(lg.i64_ty, 3, 0), "is3"), l3c, c4);
    LLVMPositionBuilderAtEnd(lg.builder, l3c);
    LLVMValueRef i3 = LLVMBuildLoad2(lg.builder, lg.i64_ty, iv, "i");
    LLVMBuildCondBr(lg.builder, LLVMBuildICmp(lg.builder, LLVMIntSLT, i3, len, "cc"),
                    l3b, fin);
    LLVMPositionBuilderAtEnd(lg.builder, l3b);
    LLVMValueRef e3 = vec_load_at(v, i3);
    LLVMBuildCondBr(lg.builder, LLVMBuildIsNull(lg.builder, rel, "rn"), l3n, l3r);
    LLVMPositionBuilderAtEnd(lg.builder, l3r);
    LLVMValueRef relf3 = LLVMBuildBitCast(lg.builder, rel,
        LLVMPointerType(relfty, 0), "relf");
    LLVMBuildCall2(lg.builder, relfty, relf3, &e3, 1, "");
    LLVMBuildBr(lg.builder, l3n);
    LLVMPositionBuilderAtEnd(lg.builder, l3n);
    LLVMValueRef ra[] = { e3, zero, zero, LLVMConstNull(lg.ptr_ty) };
    h_call(fn, ra, 4, "");
    LLVMBuildStore(lg.builder, LLVMBuildAdd(lg.builder, i3, one, "in"), iv);
    LLVMBuildBr(lg.builder, l3c);

    /* ek == 4: bytes box — release на data + free на box-а през header-а */
    LLVMPositionBuilderAtEnd(lg.builder, c4);
    LLVMBuildStore(lg.builder, zero, iv);
    LLVMBuildCondBr(lg.builder, LLVMBuildICmp(lg.builder, LLVMIntEQ, ek,
        LLVMConstInt(lg.i64_ty, 4, 0), "is4"), l4c, fin);
    LLVMPositionBuilderAtEnd(lg.builder, l4c);
    LLVMValueRef i4 = LLVMBuildLoad2(lg.builder, lg.i64_ty, iv, "i");
    LLVMBuildCondBr(lg.builder, LLVMBuildICmp(lg.builder, LLVMIntSLT, i4, len, "cc"),
                    l4b, fin);
    LLVMPositionBuilderAtEnd(lg.builder, l4b);
    LLVMValueRef e4 = vec_load_at(v, i4);
    LLVMValueRef bp = LLVMBuildBitCast(lg.builder, e4,
        LLVMPointerType(baga_bytes_ty(), 0), "bp");
    LLVMValueRef bd = LLVMBuildLoad2(lg.builder, lg.ptr_ty,
        LLVMBuildStructGEP2(lg.builder, baga_bytes_ty(), bp, 0, "bdp"), "bd");
    h_call(baga_rt("baga_rc_release_bytes"), &bd, 1, "");
    LLVMValueRef h4 = h_call(baga_rt("baga_rc_hdr"), &e4, 1, "hb");
    h_call(rt_free(), &h4, 1, "");
    LLVMBuildStore(lg.builder, LLVMBuildAdd(lg.builder, i4, one, "in"), iv);
    LLVMBuildBr(lg.builder, l4c);

    /* data масивът носи собствен header (vec_new/grow rc_alloc) → free на
     * базата; struct-ът — през неговия header h */
    LLVMPositionBuilderAtEnd(lg.builder, fin);
    LLVMValueRef od = LLVMBuildBitCast(lg.builder, data, lg.ptr_ty, "od");
    LLVMValueRef hd = h_call(baga_rt("baga_rc_hdr"), &od, 1, "hd");
    h_call(rt_free(), &hd, 1, "");
    h_call(rt_free(), &h, 1, "");
    LLVMBuildBr(lg.builder, ret_bb);

    LLVMPositionBuilderAtEnd(lg.builder, ret_bb);
    LLVMBuildRetVoid(lg.builder);
    return fn;
}

/* static void baga_rc_release_map(i8 *m, i64 val_tag, i64 val_size)
 * { h = hdr(m); if (!h) ret; if (rc == 0) underflow; if (--rc > 0) ret;
 *   за всеки entry: ключ по ktag (2 → bytes, иначе sk → str; NULL → no-op);
 *   стойност по val_tag (1 str, 2 bytes, 3 box → free(pv));
 *   free(entry); free(buckets); free(h); }
 * entries/buckets са plain malloc → plain free (умират с контейнера). */
static LLVMValueRef build_baga_rc_release_map(void) {
    LLVMTypeRef p[] = { lg.ptr_ty, lg.i64_ty, lg.i64_ty, lg.ptr_ty };
    LLVMValueRef fn = LLVMAddFunction(lg.mod, "baga_rc_release_map",
        LLVMFunctionType(lg.void_ty, p, 4, 0));
    h_begin(fn);
    LLVMBasicBlockRef ret_bb;
    LLVMValueRef h = rc_container_prologue(fn, LLVMGetParam(fn, 0),
        "baga: rc underflow (map — двоен release)\n", &ret_bb);
    LLVMValueRef m = LLVMBuildBitCast(lg.builder, LLVMGetParam(fn, 0),
        baga_map_ptr_ty(), "m");
    LLVMValueRef vt = LLVMGetParam(fn, 1);
    /* 4-тият параметър е destructor на box стойностите (baga_rc_relf_<S>;
     * NULL → само free — като val_rel в codegen_c.c:6161) */
    LLVMValueRef rel = LLVMGetParam(fn, 3);
    LLVMTypeRef relfty = LLVMFunctionType(lg.void_ty, &lg.ptr_ty, 1, 0);
    LLVMValueRef b = map_load_b(m);
    LLVMValueRef nb = map_load_nb(m);
    LLVMValueRef iv = entry_alloca(lg.i64_ty, "i");
    LLVMBuildStore(lg.builder, LLVMConstInt(lg.i64_ty, 0, 0), iv);
    LLVMValueRef ev = entry_alloca(baga_map_entry_ptr_ty(), "e");
    LLVMBasicBlockRef oc  = LLVMAppendBasicBlockInContext(lg.ctx, fn, "oc");
    LLVMBasicBlockRef ob  = LLVMAppendBasicBlockInContext(lg.ctx, fn, "ob");
    LLVMBasicBlockRef wc  = LLVMAppendBasicBlockInContext(lg.ctx, fn, "wc");
    LLVMBasicBlockRef wb  = LLVMAppendBasicBlockInContext(lg.ctx, fn, "wb");
    LLVMBasicBlockRef kb  = LLVMAppendBasicBlockInContext(lg.ctx, fn, "kb");
    LLVMBasicBlockRef ks  = LLVMAppendBasicBlockInContext(lg.ctx, fn, "ks");
    LLVMBasicBlockRef va  = LLVMAppendBasicBlockInContext(lg.ctx, fn, "va");
    LLVMBasicBlockRef vs1 = LLVMAppendBasicBlockInContext(lg.ctx, fn, "vs1");
    LLVMBasicBlockRef vc2 = LLVMAppendBasicBlockInContext(lg.ctx, fn, "vc2");
    LLVMBasicBlockRef vb1 = LLVMAppendBasicBlockInContext(lg.ctx, fn, "vb1");
    LLVMBasicBlockRef vc3 = LLVMAppendBasicBlockInContext(lg.ctx, fn, "vc3");
    LLVMBasicBlockRef vpc = LLVMAppendBasicBlockInContext(lg.ctx, fn, "vpc");
    LLVMBasicBlockRef vpf = LLVMAppendBasicBlockInContext(lg.ctx, fn, "vpf");
    LLVMBasicBlockRef fe  = LLVMAppendBasicBlockInContext(lg.ctx, fn, "fe");
    LLVMBasicBlockRef inx = LLVMAppendBasicBlockInContext(lg.ctx, fn, "inx");
    LLVMBasicBlockRef fin = LLVMAppendBasicBlockInContext(lg.ctx, fn, "fin");
    LLVMBuildBr(lg.builder, oc);

    LLVMPositionBuilderAtEnd(lg.builder, oc);
    LLVMValueRef i = LLVMBuildLoad2(lg.builder, lg.i64_ty, iv, "i");
    LLVMBuildCondBr(lg.builder, LLVMBuildICmp(lg.builder, LLVMIntSLT, i, nb, "cc"),
                    ob, fin);
    LLVMPositionBuilderAtEnd(lg.builder, ob);
    LLVMBuildStore(lg.builder, LLVMBuildLoad2(lg.builder,
        baga_map_entry_ptr_ty(),
        LLVMBuildGEP2(lg.builder, baga_map_entry_ptr_ty(), b, &i, 1, "slot"),
        "e0"), ev);
    LLVMBuildBr(lg.builder, wc);

    LLVMPositionBuilderAtEnd(lg.builder, wc);
    LLVMValueRef e = LLVMBuildLoad2(lg.builder, baga_map_entry_ptr_ty(), ev, "e");
    LLVMBuildCondBr(lg.builder, LLVMBuildIsNull(lg.builder, e, "isn"),
                    inx, wb);

    LLVMPositionBuilderAtEnd(lg.builder, wb);
    /* next се пази ПРЕДИ free — като в C/drop_map */
    LLVMValueRef nx = LLVMBuildLoad2(lg.builder, baga_map_entry_ptr_ty(),
        ent_fld(e, 9, "nxp"), "nx");
    LLVMValueRef ktag = LLVMBuildLoad2(lg.builder, lg.i64_ty,
        ent_fld(e, 3, "ktp"), "ktag");
    LLVMBuildCondBr(lg.builder, LLVMBuildICmp(lg.builder, LLVMIntEQ, ktag,
        LLVMConstInt(lg.i64_ty, 2, 0), "is2"), kb, ks);
    LLVMPositionBuilderAtEnd(lg.builder, kb);
    LLVMValueRef bkd = LLVMBuildLoad2(lg.builder, lg.ptr_ty,
        LLVMBuildStructGEP2(lg.builder, baga_bytes_ty(),
            ent_fld(e, 2, "bkp"), 0, "bkdp"), "bkd");
    h_call(baga_rt("baga_rc_release_bytes"), &bkd, 1, "");
    LLVMBuildBr(lg.builder, va);
    LLVMPositionBuilderAtEnd(lg.builder, ks);
    LLVMValueRef sk = LLVMBuildLoad2(lg.builder, lg.ptr_ty,
        ent_fld(e, 1, "skp"), "sk");
    h_call(baga_rt("baga_rc_release_str"), &sk, 1, "");
    LLVMBuildBr(lg.builder, va);

    /* стойност по val_tag: 1 str, 2 bytes, 3 box (само free на pv) */
    LLVMPositionBuilderAtEnd(lg.builder, va);
    LLVMBuildCondBr(lg.builder, LLVMBuildICmp(lg.builder, LLVMIntEQ, vt,
        LLVMConstInt(lg.i64_ty, 1, 0), "is1"), vs1, vc2);
    LLVMPositionBuilderAtEnd(lg.builder, vs1);
    LLVMValueRef sv = LLVMBuildLoad2(lg.builder, lg.ptr_ty,
        ent_fld(e, 6, "svp"), "sv");
    h_call(baga_rt("baga_rc_release_str"), &sv, 1, "");
    LLVMBuildBr(lg.builder, fe);
    LLVMPositionBuilderAtEnd(lg.builder, vc2);
    LLVMBuildCondBr(lg.builder, LLVMBuildICmp(lg.builder, LLVMIntEQ, vt,
        LLVMConstInt(lg.i64_ty, 2, 0), "is2v"), vb1, vc3);
    LLVMPositionBuilderAtEnd(lg.builder, vb1);
    LLVMValueRef bvd = LLVMBuildLoad2(lg.builder, lg.ptr_ty,
        LLVMBuildStructGEP2(lg.builder, baga_bytes_ty(),
            ent_fld(e, 7, "bvp"), 0, "bvdp"), "bvd");
    h_call(baga_rt("baga_rc_release_bytes"), &bvd, 1, "");
    LLVMBuildBr(lg.builder, fe);
    LLVMPositionBuilderAtEnd(lg.builder, vc3);
    LLVMBuildCondBr(lg.builder, LLVMBuildICmp(lg.builder, LLVMIntEQ, vt,
        LLVMConstInt(lg.i64_ty, 3, 0), "is3v"), vpc, fe);
    LLVMPositionBuilderAtEnd(lg.builder, vpc);
    LLVMValueRef pv = LLVMBuildLoad2(lg.builder, lg.ptr_ty,
        ent_fld(e, 8, "pvp"), "pv");
    LLVMBuildCondBr(lg.builder, LLVMBuildIsNull(lg.builder, pv, "pvn"),
                    fe, vpf);
    /* box стойност: destructor на полетата (ако е подаден), после free на
     * box-а през header базата (box-овете под --rc са rc_alloc-нати) */
    LLVMBasicBlockRef vpr = LLVMAppendBasicBlockInContext(lg.ctx, fn, "vpr");
    LLVMBasicBlockRef vpf2 = LLVMAppendBasicBlockInContext(lg.ctx, fn, "vpf2");
    LLVMPositionBuilderAtEnd(lg.builder, vpf);
    LLVMBuildCondBr(lg.builder, LLVMBuildIsNull(lg.builder, rel, "rn"),
                    vpf2, vpr);
    LLVMPositionBuilderAtEnd(lg.builder, vpr);
    LLVMValueRef relfv = LLVMBuildBitCast(lg.builder, rel,
        LLVMPointerType(relfty, 0), "relf");
    LLVMBuildCall2(lg.builder, relfty, relfv, &pv, 1, "");
    LLVMBuildBr(lg.builder, vpf2);
    LLVMPositionBuilderAtEnd(lg.builder, vpf2);
    LLVMValueRef hpv = h_call(baga_rt("baga_rc_hdr"), &pv, 1, "hb");
    h_call(rt_free(), &hpv, 1, "");
    LLVMBuildBr(lg.builder, fe);

    LLVMPositionBuilderAtEnd(lg.builder, fe);
    LLVMValueRef er = LLVMBuildBitCast(lg.builder, e, lg.ptr_ty, "er");
    h_call(rt_free(), &er, 1, "");
    LLVMBuildStore(lg.builder, nx, ev);
    LLVMBuildBr(lg.builder, wc);

    LLVMPositionBuilderAtEnd(lg.builder, inx);
    LLVMBuildStore(lg.builder, LLVMBuildAdd(lg.builder, i,
        LLVMConstInt(lg.i64_ty, 1, 0), "in"), iv);
    LLVMBuildBr(lg.builder, oc);

    LLVMPositionBuilderAtEnd(lg.builder, fin);
    LLVMValueRef braw = LLVMBuildBitCast(lg.builder, b, lg.ptr_ty, "braw");
    h_call(rt_free(), &braw, 1, "");
    h_call(rt_free(), &h, 1, "");
    LLVMBuildBr(lg.builder, ret_bb);

    LLVMPositionBuilderAtEnd(lg.builder, ret_bb);
    LLVMBuildRetVoid(lg.builder);
    return fn;
}

/* ---- Task 6: RC helper-и за struct/enum (порт на RC5) ----
 * Порт на emit_rc_struct_helpers (codegen_c.c:4558) и emit_rc_enum_helpers
 * (codegen_c.c:4657), но като lazy IR (като останалите baga_rt): тялото се
 * генерира при първа нужда и се кешира по име в модула.
 *
 * Представяне: struct/enum стойностите са by-value (named struct тип в
 * alloca), БЕЗ собствен rc header — затова baga_rc_retain_<S> /
 * baga_rc_release_<S> вземат стойността по стойност и retain-ват/release-ват
 * само heap ПОЛЕТАТА (транзитивно: вложен struct/enum поле → рекурсивно
 * повикване). Няма free на самия struct — той е стеков. Това е същата
 * семантика като C, където helper-ите също вземат struct-а по стойност.
 * Enum helper-ът е switch по tag полето ({ i64 tag, [N x i64] u }) —
 * release/retain само на heap payload-а на активния variant.
 *
 * Shim-ове за container сайтове, които не знаят типа статично:
 *   baga_rc_relf_<S>(i8 *p) — release на полетата през указател към box
 *   baga_rc_relv_<S>(i8 *p) — release на Vec<S> елемент (вложен Vec)
 * (огледало на relf/relv в codegen_c; retp не е нужен — retain сайтовете
 * знаят типа статично и викат retain_<S> директно). */

/* heap ли е този type AST възел (поле/payload): пряк str/bytes/Vec/Map или
 * вложен struct/enum с heap съдържание — общ предикат за field/variant
 * обходите по-долу */
static int lrc_pt_heap(Node *pt) {
    if (lrc_tag_node(pt)) return 1;
    Node *t = pt;
    while (t && (t->kind == NODE_TYPE_EFFECT || t->kind == NODE_TYPE_REF))
        t = t->inner_type;
    if (!t || t->kind != NODE_TYPE || !t->type_name) return 0;
    if (lrc_struct_has_heap(t->type_name)) return 1;
    Node *ed = find_sum_enum(t->type_name);
    return ed && lrc_enum_has_heap(ed);
}

/* heap ли е enum payload: пряк tag или struct с heap полета. Enum payload
 * в enum НЕ се брои (leak-safe граница — като rc_enum_has_heap_d /
 * emit_rc_enum_helpers в codegen_c) */
static int lrc_enum_payload_heap(Node *pt) {
    if (lrc_tag_node(pt)) return 1;
    Node *t = pt;
    while (t && (t->kind == NODE_TYPE_EFFECT || t->kind == NODE_TYPE_REF))
        t = t->inner_type;
    return t && t->kind == NODE_TYPE && t->type_name &&
        lrc_struct_has_heap(t->type_name);
}

/* едно поле / variant payload: release или retain според вида му.
 * ft е type AST възелът на полето, v — стойността (extractvalue/load). */
static void lrc_field_rc(Node *ft, LLVMValueRef v, int is_release) {
    int tag = lrc_tag_node(ft);
    if (tag == 1) {
        if (is_release) h_call(baga_rt("baga_rc_release_str"), &v, 1, "");
        else            h_call(baga_rt("baga_rc_retain"), &v, 1, "rcr");
        return;
    }
    if (tag == 2) {
        /* bytes поле е { i8*, i64 } by value — rc живее върху data */
        LLVMValueRef d = LLVMBuildExtractValue(lg.builder, v, 0, "fd");
        if (is_release) h_call(baga_rt("baga_rc_release_bytes"), &d, 1, "");
        else            h_call(baga_rt("baga_rc_retain"), &d, 1, "rcr");
        return;
    }
    if (tag == 3 || tag == 4) {
        if (!is_release) {
            LLVMValueRef p = LLVMBuildBitCast(lg.builder, v, lg.ptr_ty, "fcp");
            h_call(baga_rt("baga_rc_retain"), &p, 1, "rcr");
            return;
        }
        Node *inner = ft;
        while (inner && (inner->kind == NODE_TYPE_EFFECT ||
                         inner->kind == NODE_TYPE_REF))
            inner = inner->inner_type;
        /* Vec: elem от inner_type; Map: val от inner_type2 */
        Node *en = inner ? (tag == 3 ? inner->inner_type : inner->inner_type2)
                         : NULL;
        const char *enm = (en && en->kind == NODE_TYPE) ? en->type_name : NULL;
        int k = tag == 3 ? lrc_vec_elem_kind(NULL, ft)
                         : lrc_map_val_tag(NULL, ft);
        LLVMValueRef rel = (tag == 3 && k == 3)
            ? lrc_nested_vec_rel(NULL, ft)
            : lrc_box_rel(enm);
        LLVMValueRef a[] = {
            LLVMBuildBitCast(lg.builder, v, lg.ptr_ty, "fcp"),
            LLVMConstInt(lg.i64_ty, (uint64_t)k, 0),
            LLVMConstInt(lg.i64_ty, 0, 0),
            rel,
        };
        h_call(baga_rt(tag == 3 ? "baga_rc_release_vec"
                                : "baga_rc_release_map"), a, 4, "");
        return;
    }
    /* tag 0: вложен struct с heap полета или enum с heap payload —
     * рекурсивен retain/release (като rc_nested_struct_field /
     * rc_nested_enum_field в codegen_c) */
    Node *t = ft;
    while (t && (t->kind == NODE_TYPE_EFFECT || t->kind == NODE_TYPE_REF))
        t = t->inner_type;
    if (!t || t->kind != NODE_TYPE || !t->type_name) return;
    if (lrc_pt_heap(ft))
        h_call(lrc_rc_fn(t->type_name, is_release), &v, 1, "");
}

/* име на helper-а: baga_rc_{retain,release}_<mangle> */
static char *lrc_rc_fn_name(const char *type_name, int is_release) {
    char *m = llvm_mangle(type_name);
    size_t cap = strlen(m) + 24;
    char *out = malloc(cap);
    if (!out) { fprintf(stderr, "baga: out of memory\n"); exit(1); }
    snprintf(out, cap, "baga_rc_%s_%s", is_release ? "release" : "retain", m);
    free(m);
    return out;
}

static LLVMValueRef lrc_struct_rc_fn(const char *name, int is_release);
static LLVMValueRef lrc_enum_rc_fn(Node *ed, int is_release);

/* struct или sum enum helper по име на типа (единствено име за двата —
 * struct и enum с едно име не могат да съществуват в една програма) */
static LLVMValueRef lrc_rc_fn(const char *type_name, int is_release) {
    Node *ed = find_sum_enum(type_name);
    if (ed) return lrc_enum_rc_fn(ed, is_release);
    return lrc_struct_rc_fn(type_name, is_release);
}

/* M24-RC: resolved тип на поле в контекста на instantiated generic struct
 * стойност st: типова променлива (`v: T`) → targ на инстанцията; вложено
 * instantiated generic поле (`inner: Pair<i64>`) → checked типът на възела
 * (носи targs). Иначе NULL — node логиката (lrc_field_rc). Огледало на
 * rc_fld_inst_type (codegen_c). */
static Type *lrc_fld_inst_type(Type *st, Node *fld_type) {
    Node *t = fld_type;
    while (t && (t->kind == NODE_TYPE_EFFECT || t->kind == NODE_TYPE_REF))
        t = t->inner_type;
    if (!t || t->kind != NODE_TYPE || !t->type_name) return NULL;
    Node *d = find_struct_decl(st->name);
    if (d && d->n_struct_params > 0) {
        for (int a = 0; a < d->n_struct_params && a < st->n_targs; a++)
            if (strcmp(t->type_name, d->struct_params[a]) == 0)
                return st->targs[a];
        /* M26: Vec<T> / Map<…, T> поле с типова променлива вътре —
         * синтезирай resolved тип с конкретния elem (дълбок release на
         * вложени Vec-ове; като rc_fld_inst_type в codegen_c) */
        if (strcmp(t->type_name, "Vec") == 0 || strcmp(t->type_name, "Map") == 0) {
            Node *en = t->inner_type2 && strcmp(t->type_name, "Map") == 0
                     ? t->inner_type2 : t->inner_type;
            while (en && (en->kind == NODE_TYPE_EFFECT || en->kind == NODE_TYPE_REF))
                en = en->inner_type;
            if (en && en->kind == NODE_TYPE && en->type_name) {
                for (int a = 0; a < d->n_struct_params && a < st->n_targs; a++)
                    if (strcmp(en->type_name, d->struct_params[a]) == 0) {
                        Type *vt = calloc(1, sizeof(Type));
                        if (!vt) { fprintf(stderr, "baga: out of memory\n"); exit(1); }
                        vt->kind = strcmp(t->type_name, "Vec") == 0
                                 ? TYPE_VEC : TYPE_MAP;
                        vt->elem = st->targs[a];
                        return vt;
                    }
            }
        }
    }
    if (t->type && (t->type->kind == TYPE_STRUCT ||
                    t->type->kind == TYPE_ENUM) && t->type->n_targs > 0)
        return t->type;
    return NULL;
}

/* M24-RC: едно поле по неговия RESOLVED Type (инстанция на generic struct)
 * — огледало на lrc_field_rc, но tag/elem идват от Type, а вложените
 * struct/enum полета рекурсират през lrc_rc_fn_ty (per-instance имена). */
static void lrc_field_rc_ty(Type *ft, LLVMValueRef v, int is_release) {
    int tag = lrc_heap_tag(ft);
    if (tag == 1) {
        if (is_release) h_call(baga_rt("baga_rc_release_str"), &v, 1, "");
        else            h_call(baga_rt("baga_rc_retain"), &v, 1, "rcr");
        return;
    }
    if (tag == 2) {
        LLVMValueRef d = LLVMBuildExtractValue(lg.builder, v, 0, "fd");
        if (is_release) h_call(baga_rt("baga_rc_release_bytes"), &d, 1, "");
        else            h_call(baga_rt("baga_rc_retain"), &d, 1, "rcr");
        return;
    }
    if (tag == 3 || tag == 4) {
        if (!is_release) {
            LLVMValueRef p = LLVMBuildBitCast(lg.builder, v, lg.ptr_ty, "fcp");
            h_call(baga_rt("baga_rc_retain"), &p, 1, "rcr");
            return;
        }
        int k = tag == 3 ? lrc_vec_elem_kind(ft, NULL)
                         : lrc_map_val_tag(ft, NULL);
        LLVMValueRef rel = (tag == 3 && k == 3)
            ? lrc_nested_vec_rel(ft, NULL)
            : lrc_box_rel_ty(ft->elem);
        LLVMValueRef a[] = {
            LLVMBuildBitCast(lg.builder, v, lg.ptr_ty, "fcp"),
            LLVMConstInt(lg.i64_ty, (uint64_t)k, 0),
            LLVMConstInt(lg.i64_ty, 0, 0),
            rel,
        };
        h_call(baga_rt(tag == 3 ? "baga_rc_release_vec"
                                : "baga_rc_release_map"), a, 4, "");
        return;
    }
    if (tag == 5 || tag == 6)
        h_call(lrc_rc_fn_ty(ft, is_release), &v, 1, "");
    /* tag 0: не-heap поле (i64/f64/bool и пр.) — нищо */
}

/* static void baga_rc_{retain,release}_b_Box_i64(b_Box_i64 s)
 * { за всяко heap поле след resolve на типовите променливи: retain/release }
 * M24-RC: per-instance helper за instantiated generic struct. Тялото се
 * генерира при първа нужда и се кешира по име (като lrc_struct_rc_fn). */
static LLVMValueRef lrc_struct_inst_rc_fn(Type *t, int is_release) {
    char *cn = llvm_struct_cname(t);            /* "Box_i64" */
    char *full = lrc_rc_fn_name(cn, is_release);
    free(cn);
    LLVMValueRef fn = LLVMGetNamedFunction(lg.mod, full);
    if (fn) { free(full); return fn; }
    Node *decl = find_struct_decl(t->name);
    if (!decl) {
        char buf[256];
        snprintf(buf, sizeof buf, "RC helper за неизвестна структура '%s'",
                 t->name);
        free(full);
        llvm_unsupported(buf);
    }
    LLVMTypeRef sty = user_struct_ty_inst(t);
    /* регистрация ПРЕДИ тялото — рекурсивни препратки намират декларацията */
    fn = LLVMAddFunction(lg.mod, full,
        LLVMFunctionType(lg.void_ty, &sty, 1, 0));
    free(full);
    LLVMBasicBlockRef saved = LLVMGetInsertBlock(lg.builder);
    h_begin(fn);
    LLVMValueRef s = LLVMGetParam(fn, 0);
    for (int i = 0; i < decl->fields.len; i++) {
        Node *ft = decl->fields.data[i]->fld_type;
        Type *rt = lrc_fld_inst_type(t, ft);
        LLVMValueRef v = LLVMBuildExtractValue(lg.builder, s, (unsigned)i, "f");
        if (rt) lrc_field_rc_ty(rt, v, is_release);
        else    lrc_field_rc(ft, v, is_release);
    }
    LLVMBuildRetVoid(lg.builder);
    if (saved) LLVMPositionBuilderAtEnd(lg.builder, saved);
    return fn;
}

/* M24-RC: Type-aware dispatcher — instantiated generic struct (n_targs>0)
 * отива на per-instance helper; иначе по име (enum или обикновен struct) */
static LLVMValueRef lrc_rc_fn_ty(Type *t, int is_release) {
    if (t && t->kind == TYPE_STRUCT && t->n_targs > 0)
        return lrc_struct_inst_rc_fn(t, is_release);
    if (!t || !t->name)
        llvm_unsupported("RC helper за тип без име");
    return lrc_rc_fn(t->name, is_release);
}

/* static void baga_rc_{retain,release}_<S>(b_<S> s)
 * { за всяко heap поле: retain/release (транзитивно) } */
static LLVMValueRef lrc_struct_rc_fn(const char *name, int is_release) {
    char *full = lrc_rc_fn_name(name, is_release);
    LLVMValueRef fn = LLVMGetNamedFunction(lg.mod, full);
    if (fn) { free(full); return fn; }
    Node *decl = find_struct_decl(name);
    if (!decl) {
        char buf[256];
        snprintf(buf, sizeof buf, "RC helper за неизвестна структура '%s'", name);
        free(full);
        llvm_unsupported(buf);
    }
    LLVMTypeRef sty = user_struct_ty(name);
    /* регистрацията е ПРЕДИ тялото — рекурсивни struct/enum препратки
     * (S → Vec<E>, E → V(S)) намират вече декларираната функция */
    fn = LLVMAddFunction(lg.mod, full,
        LLVMFunctionType(lg.void_ty, &sty, 1, 0));
    free(full);
    LLVMBasicBlockRef saved = LLVMGetInsertBlock(lg.builder);
    h_begin(fn);
    LLVMValueRef s = LLVMGetParam(fn, 0);
    for (int i = 0; i < decl->fields.len; i++) {
        Node *ft = decl->fields.data[i]->fld_type;
        LLVMValueRef v = LLVMBuildExtractValue(lg.builder, s, (unsigned)i, "f");
        lrc_field_rc(ft, v, is_release);
    }
    LLVMBuildRetVoid(lg.builder);
    if (saved) LLVMPositionBuilderAtEnd(lg.builder, saved);
    return fn;
}

/* static void baga_rc_{retain,release}_<E>(b_<E> e)
 * { switch (e.tag) { case j: retain/release на heap payload-а; } } */
static LLVMValueRef lrc_enum_rc_fn(Node *ed, int is_release) {
    char *full = lrc_rc_fn_name(ed->enum_name, is_release);
    LLVMValueRef fn = LLVMGetNamedFunction(lg.mod, full);
    if (fn) { free(full); return fn; }
    LLVMTypeRef ety = user_struct_ty(ed->enum_name);
    fn = LLVMAddFunction(lg.mod, full,
        LLVMFunctionType(lg.void_ty, &ety, 1, 0));
    free(full);
    LLVMBasicBlockRef saved = LLVMGetInsertBlock(lg.builder);
    h_begin(fn);
    LLVMValueRef e = LLVMGetParam(fn, 0);
    /* payload GEP иска адрес — spill на параметъра (като sum_ctor_fn) */
    LLVMValueRef ea = LLVMBuildAlloca(lg.builder, ety, "ea");
    LLVMBuildStore(lg.builder, e, ea);
    LLVMValueRef tag = LLVMBuildExtractValue(lg.builder, e, 0, "tag");
    LLVMBasicBlockRef ret_bb =
        LLVMAppendBasicBlockInContext(lg.ctx, fn, "ret");
    int ncases = 0;
    for (int j = 0; j < ed->n_variants; j++) {
        Node *pt = ed->enum_payloads ? ed->enum_payloads[j] : NULL;
        if (pt && lrc_enum_payload_heap(pt)) ncases++;
    }
    LLVMValueRef sw = LLVMBuildSwitch(lg.builder, tag, ret_bb,
                                      (unsigned)ncases);
    for (int j = 0; j < ed->n_variants; j++) {
        Node *pt = ed->enum_payloads ? ed->enum_payloads[j] : NULL;
        if (!pt || !lrc_enum_payload_heap(pt)) continue;
        char cnm[32];
        snprintf(cnm, sizeof cnm, "v%d", j);
        LLVMBasicBlockRef cbb =
            LLVMAppendBasicBlockInContext(lg.ctx, fn, cnm);
        LLVMAddCase(sw, LLVMConstInt(lg.i64_ty, (unsigned long long)j, 0),
                    cbb);
        LLVMPositionBuilderAtEnd(lg.builder, cbb);
        LLVMTypeRef pty = llvm_type(pt);
        LLVMValueRef pv = LLVMBuildLoad2(lg.builder, pty,
            sum_payload_ptr(ety, ea, pty), "pv");
        lrc_field_rc(pt, pv, is_release);
        LLVMBuildBr(lg.builder, ret_bb);
    }
    LLVMPositionBuilderAtEnd(lg.builder, ret_bb);
    LLVMBuildRetVoid(lg.builder);
    if (saved) LLVMPositionBuilderAtEnd(lg.builder, saved);
    return fn;
}

/* static void baga_rc_relf_<S>(i8 *p) { release_<S>(load (b_<S> *)p); }
 * Destructor за box елементи/стойности във Vec/Map (контейнерът не знае
 * типа статично) — огледало на baga_rc_relf_<S> в codegen_c. */
static LLVMValueRef lrc_relf_fn(const char *type_name) {
    char *m = llvm_mangle(type_name);
    size_t cap = strlen(m) + 20;
    char *full = malloc(cap);
    if (!full) { fprintf(stderr, "baga: out of memory\n"); exit(1); }
    snprintf(full, cap, "baga_rc_relf_%s", m);
    free(m);
    LLVMValueRef fn = LLVMGetNamedFunction(lg.mod, full);
    if (fn) { free(full); return fn; }
    LLVMTypeRef sty = user_struct_ty(type_name);
    fn = LLVMAddFunction(lg.mod, full,
        LLVMFunctionType(lg.void_ty, &lg.ptr_ty, 1, 0));
    free(full);
    LLVMBasicBlockRef saved = LLVMGetInsertBlock(lg.builder);
    h_begin(fn);
    LLVMValueRef sp = LLVMBuildBitCast(lg.builder, LLVMGetParam(fn, 0),
        LLVMPointerType(sty, 0), "sp");
    LLVMValueRef v = LLVMBuildLoad2(lg.builder, sty, sp, "v");
    h_call(lrc_rc_fn(type_name, 1), &v, 1, "");
    LLVMBuildRetVoid(lg.builder);
    if (saved) LLVMPositionBuilderAtEnd(lg.builder, saved);
    return fn;
}

/* static void baga_rc_relv_<S>(i8 *p)
 * { baga_rc_release_vec(p, 2, 0, baga_rc_relf_<S>); }
 * Shim за Vec<S> като елемент на външен Vec (kind 3 на външния не знае
 * елементния тип на вложения) — огледало на codegen_c.c:4651. */
static LLVMValueRef lrc_relv_fn(const char *type_name) {
    char *m = llvm_mangle(type_name);
    size_t cap = strlen(m) + 20;
    char *full = malloc(cap);
    if (!full) { fprintf(stderr, "baga: out of memory\n"); exit(1); }
    snprintf(full, cap, "baga_rc_relv_%s", m);
    free(m);
    LLVMValueRef fn = LLVMGetNamedFunction(lg.mod, full);
    if (fn) { free(full); return fn; }
    fn = LLVMAddFunction(lg.mod, full,
        LLVMFunctionType(lg.void_ty, &lg.ptr_ty, 1, 0));
    free(full);
    LLVMBasicBlockRef saved = LLVMGetInsertBlock(lg.builder);
    h_begin(fn);
    LLVMValueRef relf = LLVMBuildBitCast(lg.builder,
        lrc_relf_fn(type_name), lg.ptr_ty, "relf");
    LLVMValueRef a[] = {
        LLVMGetParam(fn, 0),
        LLVMConstInt(lg.i64_ty, 2, 0),
        LLVMConstInt(lg.i64_ty, 0, 0),
        relf,
    };
    h_call(baga_rt("baga_rc_release_vec"), a, 4, "");
    LLVMBuildRetVoid(lg.builder);
    if (saved) LLVMPositionBuilderAtEnd(lg.builder, saved);
    return fn;
}

/* box destructor за struct/enum елементен/стойностен тип с heap полета —
 * relf shim-ът като функция (NULL → няма heap полета). Порт на rc_box_rel
 * (codegen_c.c:410); lrc_box_rel е i8* обвивката за container аргументите. */
static LLVMValueRef lrc_box_rel_fn(const char *type_name) {
    if (!type_name) return NULL;
    int heap = lrc_struct_has_heap(type_name);
    if (!heap) {
        Node *ed = find_sum_enum(type_name);
        heap = ed && lrc_enum_has_heap(ed);
    }
    if (!heap) return NULL;
    return lrc_relf_fn(type_name);
}

static LLVMValueRef lrc_box_rel(const char *type_name) {
    LLVMValueRef fn = lrc_box_rel_fn(type_name);
    if (!fn) return LLVMConstNull(lg.ptr_ty);
    return LLVMBuildBitCast(lg.builder, fn, lg.ptr_ty, "relf");
}

/* M24-RC: relf shim за instantiated generic struct — baga_rc_relf_b_Box_i64
 * (i8 *p), вика per-instance release helper-а. Огледало на lrc_relf_fn. */
static LLVMValueRef lrc_relf_fn_ty(Type *t) {
    char *cn = llvm_struct_cname(t);
    char *m = llvm_mangle(cn);
    size_t cap = strlen(m) + 20;
    char *full = malloc(cap);
    if (!full) { fprintf(stderr, "baga: out of memory\n"); exit(1); }
    snprintf(full, cap, "baga_rc_relf_%s", m);
    free(m);
    LLVMValueRef fn = LLVMGetNamedFunction(lg.mod, full);
    if (fn) { free(full); free(cn); return fn; }
    LLVMTypeRef sty = user_struct_ty(cn);
    free(cn);
    fn = LLVMAddFunction(lg.mod, full,
        LLVMFunctionType(lg.void_ty, &lg.ptr_ty, 1, 0));
    free(full);
    LLVMBasicBlockRef saved = LLVMGetInsertBlock(lg.builder);
    h_begin(fn);
    LLVMValueRef sp = LLVMBuildBitCast(lg.builder, LLVMGetParam(fn, 0),
        LLVMPointerType(sty, 0), "sp");
    LLVMValueRef v = LLVMBuildLoad2(lg.builder, sty, sp, "v");
    h_call(lrc_struct_inst_rc_fn(t, 1), &v, 1, "");
    LLVMBuildRetVoid(lg.builder);
    if (saved) LLVMPositionBuilderAtEnd(lg.builder, saved);
    return fn;
}

/* M24-RC: Type-aware box destructor — instantiated generic struct елемент/
 * стойност (n_targs>0) получава per-instance relf, само ако инстанцията
 * реално има heap полета (иначе NULL → само free, leak-safe като C) */
static LLVMValueRef lrc_box_rel_fn_ty(Type *et) {
    if (et && et->kind == TYPE_STRUCT && et->n_targs > 0)
        return lrc_heap_tag(et) == 5 ? lrc_relf_fn_ty(et) : NULL;
    return lrc_box_rel_fn(et && et->name ? et->name : NULL);
}

static LLVMValueRef lrc_box_rel_ty(Type *et) {
    LLVMValueRef fn = lrc_box_rel_fn_ty(et);
    if (!fn) return LLVMConstNull(lg.ptr_ty);
    return LLVMBuildBitCast(lg.builder, fn, lg.ptr_ty, "relf");
}

/* destructor за kind 3 (вложен Vec) елементи — relv shim, когато елементът
 * е Vec<S> и S е struct с heap полета; иначе NULL (старата граница:
 * вътрешният vec се release-ва като kind 0 — leak-safe, като codegen_c).
 * Порт на rc_nested_vec_rel_type + _node (inferred Type първо). */
/* M26: enc на елементния тип за relv shim име: str / bytes / mangled struct
 * име (b_Wrap; per-instance b_Box_i64) / v_<enc> за Vec — като
 * rc_relv_enc в codegen_c (имената съвпадат между бекендите). */
static char *lrc_relv_enc(Type *e) {
    if (!e) return NULL;
    if (e->kind == TYPE_STR)   return strdup("str");
    if (e->kind == TYPE_BYTES) return strdup("bytes");
    if ((e->kind == TYPE_STRUCT || e->kind == TYPE_ENUM) && e->name) {
        if (e->kind == TYPE_STRUCT && e->n_targs > 0) {
            char *cn = llvm_struct_cname(e);
            char *m = llvm_mangle(cn);
            free(cn);
            return m;
        }
        return llvm_mangle(e->name);
    }
    if (e->kind == TYPE_VEC) {
        char *sub = lrc_relv_enc(e->elem);
        if (!sub) return NULL;
        size_t cap = strlen(sub) + 3;
        char *out = malloc(cap);
        if (!out) { fprintf(stderr, "baga: out of memory\n"); exit(1); }
        snprintf(out, cap, "v_%s", sub);
        free(sub);
        return out;
    }
    return NULL;
}

/* M26: трябва ли shim за Vec<E> като елемент на външен Vec — E носи heap
 * някъде в дълбочина. Enum — старото поведение (leak-safe, като C). */
static int lrc_vec_elem_deep_heap(Type *e) {
    if (!e) return 0;
    switch (e->kind) {
        case TYPE_STR: case TYPE_BYTES: return 1;
        case TYPE_VEC: return lrc_vec_elem_deep_heap(e->elem);
        case TYPE_STRUCT: return lrc_heap_tag(e) == 5;   /* instance-aware */
        default: return 0;
    }
}

static LLVMValueRef lrc_relf_fn_ty(Type *t);

/* M26: baga_rc_relv_<enc(E)>(i8 *p) { baga_rc_release_vec(p, kind, 0, rel); }
 * за E = str/bytes/Vec<…>/instantiated generic struct — lazy, кеширан по
 * име (като останалите helper-и). Обикновен struct/enum → lrc_relv_fn. */
static LLVMValueRef lrc_relv_fn_ty(Type *es) {
    if (es && (es->kind == TYPE_STRUCT || es->kind == TYPE_ENUM) &&
        !(es->kind == TYPE_STRUCT && es->n_targs > 0))
        return lrc_relv_fn(es->name);
    char *enc = lrc_relv_enc(es);
    if (!enc) llvm_unsupported("relv shim за неизвестен елементен тип");
    size_t cap = strlen(enc) + 20;
    char *full = malloc(cap);
    if (!full) { fprintf(stderr, "baga: out of memory\n"); exit(1); }
    snprintf(full, cap, "baga_rc_relv_%s", enc);
    free(enc);
    LLVMValueRef fn = LLVMGetNamedFunction(lg.mod, full);
    if (fn) { free(full); return fn; }
    fn = LLVMAddFunction(lg.mod, full,
        LLVMFunctionType(lg.void_ty, &lg.ptr_ty, 1, 0));
    free(full);
    int kind;
    /* под-shim-ът се генерира ПРЕДИ тялото (възстановява позицията сам);
     * bitcast-ът към него е инструкция ВЪТРЕ в тялото на този shim */
    LLVMValueRef sub = NULL;
    if (es->kind == TYPE_STR)        kind = 1;
    else if (es->kind == TYPE_BYTES) kind = 4;
    else if (es->kind == TYPE_VEC) { kind = 3; sub = lrc_relv_fn_ty(es->elem); }
    else { kind = 2; sub = lrc_relf_fn_ty(es); }   /* instantiated generic struct */
    LLVMBasicBlockRef saved = LLVMGetInsertBlock(lg.builder);
    h_begin(fn);
    LLVMValueRef rel = sub ? LLVMBuildBitCast(lg.builder, sub, lg.ptr_ty, "relf")
                           : LLVMConstNull(lg.ptr_ty);
    LLVMValueRef a[] = {
        LLVMGetParam(fn, 0),
        LLVMConstInt(lg.i64_ty, (uint64_t)kind, 0),
        LLVMConstInt(lg.i64_ty, 0, 0),
        rel,
    };
    h_call(baga_rt("baga_rc_release_vec"), a, 4, "");
    LLVMBuildRetVoid(lg.builder);
    if (saved) LLVMPositionBuilderAtEnd(lg.builder, saved);
    return fn;
}

/* destructor за kind 3 (вложен Vec) елементи — relv shim, когато елементът
 * е Vec<E> и E носи heap в дълбочина (struct с heap полета — v0.9;
 * str/bytes/по-дълбока вложеност — M26, огледало на codegen_c); иначе NULL
 * (runtime-ът release-ва вътрешния с kind 0 — leak-safe граница). */
static LLVMValueRef lrc_nested_vec_rel(Type *vty, Node *tn) {
    /* M26: checked типът на възела печели, когато inferred няма elem
     * (vec_new) — конкретен след recheck на инстанция (M21) */
    if (tn && tn->type && tn->type->kind == TYPE_VEC &&
        (!vty || vty->kind != TYPE_VEC || !vty->elem))
        vty = tn->type;
    if (vty && vty->elem && vty->elem->kind == TYPE_VEC && vty->elem->elem) {
        Type *es = vty->elem->elem;
        if (lrc_vec_elem_deep_heap(es))
            return LLVMBuildBitCast(lg.builder, lrc_relv_fn_ty(es),
                                    lg.ptr_ty, "relv");
        return LLVMConstNull(lg.ptr_ty);
    }
    const char *sn = NULL;
    if (vty && vty->elem && vty->elem->kind == TYPE_VEC &&
        vty->elem->elem && vty->elem->elem->kind == TYPE_STRUCT)
        sn = vty->elem->elem->name;
    if (!sn && tn) {
        Node *inner = tn->inner_type;
        if (inner && inner->kind == NODE_TYPE && inner->type_name &&
            strcmp(inner->type_name, "Vec") == 0) {
            Node *es = inner->inner_type;
            if (es && es->kind == NODE_TYPE) sn = es->type_name;
        }
    }
    if (!sn || !lrc_struct_has_heap(sn)) return LLVMConstNull(lg.ptr_ty);
    return LLVMBuildBitCast(lg.builder, lrc_relv_fn(sn), lg.ptr_ty, "relv");
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
    else if (strcmp(name, "baga_f64_to_str") == 0)  fn = build_baga_f64_to_str();
    else if (strcmp(name, "baga_rc_hdr") == 0)      fn = build_baga_rc_hdr();
    else if (strcmp(name, "baga_rc_alloc") == 0)    fn = build_baga_rc_alloc();
    else if (strcmp(name, "baga_rc_retain") == 0)   fn = build_baga_rc_retain();
    else if (strcmp(name, "baga_rc_release_str") == 0)
        fn = build_baga_rc_release("baga_rc_release_str",
            "baga: rc underflow (str — двоен release)\n");
    else if (strcmp(name, "baga_rc_release_bytes") == 0)
        fn = build_baga_rc_release("baga_rc_release_bytes",
            "baga: rc underflow (bytes — двоен release)\n");
    else if (strcmp(name, "baga_rc_release_vec") == 0)
        fn = build_baga_rc_release_vec();
    else if (strcmp(name, "baga_rc_release_map") == 0)
        fn = build_baga_rc_release_map();
    else if (strcmp(name, "baga_drop_str") == 0)   fn = build_baga_drop_str();
    else if (strcmp(name, "baga_drop_bytes") == 0) fn = build_baga_drop_bytes();
    else if (strcmp(name, "baga_drop_vec") == 0)   fn = build_baga_drop_vec();
    else if (strcmp(name, "baga_drop_map") == 0)   fn = build_baga_drop_map();
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
static LLVMValueRef emit_block_value_llvm(Node *block);

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
 * ако има такава) — като __clo в codegen_c. Ленив, по веднъж на функция.
 * M25: и инстанция на generic fn (`id__i0`) — базовият decl + gen контекст
 * (llvm_type resolve-ва типовите променливи към targs на инстанцията). */
static LLVMValueRef closure_wrapper_named(const char *fn_name) {
    char *m = llvm_mangle(fn_name);
    char wn[600];
    snprintf(wn, sizeof wn, "%s__clo", m);
    LLVMValueRef ex = LLVMGetNamedFunction(lg.mod, wn);
    if (ex) { free(m); return ex; }
    Node *fn = find_user_fn(fn_name);
    Node *saved_gf = lg.gen_fn;
    int saved_gi = lg.gen_inst;
    if (!fn) {
        /* M25: synth име на инстанция — `база__iN` */
        const char *sep = strstr(fn_name, "__i");
        int idx = -1;
        char base[256];
        if (sep && sscanf(sep + 3, "%d", &idx) == 1 && idx >= 0 &&
            (size_t)(sep - fn_name) < sizeof base) {
            memcpy(base, fn_name, (size_t)(sep - fn_name));
            base[sep - fn_name] = '\0';
            fn = find_user_fn(base);
        }
        if (!fn || idx < 0 || fn->n_type_params == 0 || idx >= fn->inst_count) {
            char buf[256];
            snprintf(buf, sizeof buf, "fn стойност на непозната функция '%s'", fn_name);
            free(m);
            llvm_unsupported(buf);
        }
        /* gen контекст: llvm_type resolve-ва типовите променливи */
        lg.gen_fn = fn;
        lg.gen_inst = idx;
    }
    LLVMValueRef target = LLVMGetNamedFunction(lg.mod, m);
    if (!target) {
        lg.gen_fn = saved_gf; lg.gen_inst = saved_gi;
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
    lg.gen_fn = saved_gf; lg.gen_inst = saved_gi;
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
    /* RC: wrapper-ът на ламбдата е отделна функция — собствен fn scope
     * (като codegen_c); външният RC стек се пази и възстановява след тялото */
    int saved_lrc_count = lg.lrc_count;
    int saved_lrc_depth = lg.lrc_depth;
    int saved_lrc_fn_base = lg.lrc_fn_base;
    lrc_push_scope(0);
    lg.lrc_fn_base = lg.lrc_depth - 1;

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
            /* RC: capture-ите са borrowed (като в codegen_c) */
            lrc_track(cap->param_name, cap->type, 1);
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
        lrc_track_tag(p->param_name, lrc_heap_tag_node(p->param_type), p->type,
                      p->param_type, 1);
    }
    /* тяло — огледало на emit_fn_llvm опашката. Изпълнява се при извикване
     * на затварянето — условен поток спрямо външната функция. */
    int has_ret = n->ret_type != NULL;
    NodeVec *stmts = n->fn_body ? &n->fn_body->stmts : NULL;
    lg.lrc_branch_depth++;
    for (int i = 0; stmts && i < stmts->len; i++) {
        Node *s = stmts->data[i];
        if (LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(lg.builder)))
            break;
        if (has_ret && i == stmts->len - 1 && s->kind == NODE_EXPR_STMT) {
            /* RC4: temp-ове в implicit return (като при explicit return) */
            LLRcTmpSave tsv;
            if (lg.rc) lrc_tmp_begin(s->expr, 1, &tsv);
            LLVMValueRef v = emit_expr_llvm(s->expr);
            if (!LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(lg.builder))) {
                if (!v) llvm_unsupported("лямбда: неявен return");
                v = coerce(v, ret);
                if (lg.rc) lrc_tmp_release_all();
                /* RC: move семантика като explicit return (като emit_fn_llvm) */
                lrc_return_move(s->expr, v);
                LLVMBuildRet(lg.builder, v);
            }
            if (lg.rc) lrc_tmp_end(&tsv);
        } else {
            emit_stmt_llvm(s, NULL, NULL);
        }
    }
    lg.lrc_branch_depth--;
    /* RC: край на lambda scope-а + възстановяване на външния RC стек */
    lrc_pop_scope();
    lg.lrc_count = saved_lrc_count;
    lg.lrc_depth = saved_lrc_depth;
    lg.lrc_fn_base = saved_lrc_fn_base;
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
    lg.lrc_branch_depth++;   /* телата на рамената са условен поток */
    for (int j = 0; j < body->stmts.len; j++) {
        if (LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(lg.builder))) {
            lg.lrc_branch_depth--;
            return;
        }
        Node *s = body->stmts.data[j];
        if (s->kind == NODE_RETURN) {
            if (s->ret_val) {
                /* void match: стойността се оценява за страничния ефект
                 * (codegen_c emit-ва израза и без _mr присвояване) */
                LLVMValueRef v = emit_expr_llvm(s->ret_val);
                if (res_alloca) {
                    if (!v) llvm_unsupported("print в match клон");
                    v = coerce(v, LLVMGetAllocatedType(res_alloca));
                    /* RC: рамото произвежда owned резултат (порт на
                     * rc_emit_match_arm_val) — borrowed/ident стойност се
                     * retain-ва, иначе release на източника обесва
                     * консуматора на match резултата. */
                    if (lrc_need_owned_retain(s->ret_val))
                        lrc_emit_retain_val(lrc_heap_tag(s->ret_val->type),
                                            s->ret_val->type, NULL, v);
                    LLVMBuildStore(lg.builder, v, res_alloca);
                }
            }
            LLVMBuildBr(lg.builder, merge_bb);
            lg.lrc_branch_depth--;
            return;
        }
        if (s->kind == NODE_EXPR_STMT) {
            emit_expr_llvm(s->expr);
            continue;
        }
        llvm_unsupported("оператор в match клон (само изрази)");
    }
    lg.lrc_branch_depth--;
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
    if (!mv) llvm_unsupported("match върху не-целочислена стойност");
    /* str scrutinee: C бекендът сравнява `int64_t _mv == "литерал"` —
     * указателно равенство (warning в gcc, валиден код); огледало чрез
     * ptrtoint от двете страни */
    if (LLVMGetTypeKind(LLVMTypeOf(mv)) == LLVMPointerTypeKind)
        mv = LLVMBuildPtrToInt(lg.builder, mv, lg.i64_ty, "mvp");
    if (LLVMGetTypeKind(LLVMTypeOf(mv)) != LLVMIntegerTypeKind)
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
            if (LLVMGetTypeKind(LLVMTypeOf(pat)) == LLVMPointerTypeKind)
                pat = LLVMBuildPtrToInt(lg.builder, pat, lg.i64_ty, "mpp");
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

/* MEM-1/Task 4: drop(x) — огледало на codegen_c.c:2141-2243.
 * Под --rc: drop ≡ release + bindingът умира (scope exit вече не го
 * release-ва — иначе underflow). Като C: dead флагът е compile-time и
 * path-insensitive — `if c { drop(v) } drop(v)` се компилира, а вторият
 * release се emit-ва пак и runtime guard-ът решава (underflow = чиста
 * грешка). Изключение от C: доказуем двоен drop в ЛИНЕЙНИЯ поток (и двата
 * drop-а извън условен/цикълен клон — lrc_branch_depth) е compile-time
 * грешка, защото в LLVM след реален free glibc може да затрие magic-а и
 * вторият release да е тих no-op (вж. docs/memory-rc-bg.md). Без --rc:
 * baga_drop_* (plain free — алокациите са malloc БЕЗ header). */
static void lrc_emit_drop(Node *n) {
    if (n->args.len != 1 || n->args.data[0]->kind != NODE_IDENT)
        llvm_unsupported("drop приема единствено име на локална");
    Node *id = n->args.data[0];
    Type *at = id->type;
    if (lg.rc) {
        int di = lrc_find(id->name);
        if (di < 0)
            llvm_unsupported("drop на не-track-нато име (не-heap тип?)");
        LLRcLocal *l = &lg.lrc_locals[di];
        if (l->is_param)
            llvm_unsupported("drop на параметър — споделен буфер на извикващия");
        if (l->dead && l->dead_linear && lg.lrc_branch_depth == 0)
            llvm_unsupported("drop на вече drop-нат binding");
        lrc_emit_release(l);
        l->dead = 1;
        if (lg.lrc_branch_depth == 0) l->dead_linear = 1;
        return;
    }
    if (!at) llvm_unsupported("drop с неизвестен тип");
    LLVMValueRef v = emit_expr_llvm(id);
    if (!v) llvm_unsupported("drop аргумент");
    if (at->kind == TYPE_STR) {
        h_call(baga_rt("baga_drop_str"), &v, 1, "");
    } else if (at->kind == TYPE_BYTES) {
        h_call(baga_rt("baga_drop_bytes"), &v, 1, "");
    } else if (at->kind == TYPE_VEC) {
        /* elem_kind като в codegen_c: struct/enum/bytes → box (2),
         * вложен Vec → 3, str → 1 (споделени — не се пипат), иначе 0 */
        int ek = 0;
        LLVMValueRef esz = LLVMConstInt(lg.i64_ty, 0, 0);
        if (at->elem && at->elem->name &&
            (at->elem->kind == TYPE_STRUCT || at->elem->kind == TYPE_ENUM)) {
            ek = 2;
            esz = LLVMSizeOf(user_struct_ty(at->elem->name));
        } else if (at->elem && at->elem->kind == TYPE_BYTES) {
            ek = 2;
            esz = LLVMSizeOf(baga_bytes_ty());
        } else if (at->elem && at->elem->kind == TYPE_VEC) {
            ek = 3;
        } else if (at->elem && at->elem->kind == TYPE_STR) {
            ek = 1;
        }
        LLVMValueRef pa[] = { v, LLVMConstInt(lg.i64_ty, (uint64_t)ek, 0), esz };
        h_call(baga_rt("baga_drop_vec"), pa, 3, "");
    } else if (at->kind == TYPE_MAP) {
        int vb = 0;
        LLVMValueRef vsz = LLVMConstInt(lg.i64_ty, 0, 0);
        if (at->elem && at->elem->name &&
            (at->elem->kind == TYPE_STRUCT || at->elem->kind == TYPE_ENUM)) {
            vb = 1;
            vsz = LLVMSizeOf(user_struct_ty(at->elem->name));
        }
        LLVMValueRef pa[] = { v, LLVMConstInt(lg.i64_ty, (uint64_t)vb, 0), vsz };
        h_call(baga_rt("baga_drop_map"), pa, 3, "");
    } else {
        llvm_unsupported("drop върху не-heap тип (не str/bytes/Vec/Map)");
    }
}

static LLVMValueRef emit_expr_llvm(Node *n) {
    if (!n) llvm_unsupported("празен израз");

    /* RC4-LLVM: регистриран temp — стойността вече е изчислена в
     * lrc_tmp_begin; върни кеша (аналог на rc_tmp_emit_sub). lrc_tmp_decl
     * предпазва от самозаместване при emission на самия temp. */
    if (lg.rc && lg.lrc_tmps_on && n != lg.lrc_tmp_decl) {
        for (int i = 0; i < lg.lrc_tmp_count; i++)
            if (lg.lrc_tmps[i].site == n) return lg.lrc_tmps[i].val;
    }

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
            /* RC5 v0.7: индекс на temp аргумент за move-консумация при
             * vec_push/vec_set/map_set (helper-ите retain-ват вътрешно —
             * след повикването temp-ът се консумира и се release-ва веднага;
             * нетен move, като _move helper вариантите в codegen_c) */
            int rc_consume_arg = -1;
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
                        /* RC (Task 6): payload-ът се споделя по указател —
                         * ident/borrowed се retain-ва (+1 за payload
                         * референцията); fresh е owned — move (codegen_c
                         * RC5 v0.6, codegen_c.c:2056-2088) */
                        lrc_embed_retain(n->args.data[0], a);
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
                {"i64_to_str",  "baga_i64_to_str"},
                {"f64_to_str",  "baga_f64_to_str"},
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
                        ? baga_bytes_ty()
                        : (vt->elem->kind == TYPE_STRUCT && vt->elem->n_targs > 0)
                            ? user_struct_ty_inst(vt->elem)
                            : user_struct_ty(vt->elem->name);
                    LLVMValueRef sz = LLVMSizeOf(sty);
                    const char *cn = n->callee->name;
                    LLVMValueRef v = emit_expr_llvm(n->args.data[0]);
                    if (!v) llvm_unsupported("vec аргумент");
                    if (strcmp(cn, "vec_push") == 0) {
                        LLVMValueRef e = emit_expr_llvm(n->args.data[1]);
                        if (!e) llvm_unsupported("vec_push аргумент");
                        /* RC5 v0.7/v1.0b: temp аргумент (owned call/to_str
                         * резултат) — move в box-а, консумирай temp-а
                         * (codegen_c.c:2362-2370) */
                        int tci = lrc_tmp_find(n->args.data[1]);
                        /* RC: bytes елемент — retain на data (контейнерът
                         * става собственик; codegen_c: baga_vec_push_bytes).
                         * struct/enum (Task 6): box копието споделя heap
                         * полетата — retain_<S> (освен при fresh литерал/
                         * ctor — move; като codegen_c.c:2266-2301). */
                        if (lg.rc && vt->elem->kind == TYPE_BYTES) {
                            if (tci < 0) {   /* temp: move — без retain */
                                LLVMValueRef bd = LLVMBuildExtractValue(lg.builder,
                                    e, 0, "bd");
                                h_call(baga_rt("baga_rc_retain"), &bd, 1, "rcr");
                            }
                        } else if (lg.rc) {
                            lrc_embed_retain(n->args.data[1], e);
                        }
                        LLVMValueRef tmp = entry_alloca(sty, "bx");
                        LLVMBuildStore(lg.builder, e, tmp);
                        LLVMValueRef bp = LLVMBuildBitCast(lg.builder, tmp, lg.ptr_ty, "bp");
                        LLVMValueRef pa[] = { v, bp, sz };
                        LLVMValueRef r = h_call(baga_rt("baga_vec_push_box"), pa, 3, "");
                        lrc_tmp_consume(tci);
                        return r;
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
                        /* RC5 v0.7/v1.0b: temp аргумент — move (без retain
                         * на новото; старото пак се release-ва), консумирай */
                        int tci = lrc_tmp_find(n->args.data[2]);
                        /* RC: bytes елемент — retain на новата data, release
                         * на старата (в този ред — alias-safe; codegen_c:
                         * baga_vec_set_bytes). struct/enum (Task 6): retain
                         * на новото, после relf на стария box (codegen_c:
                         * baga_vec_set_box_rc). */
                        if (lg.rc && vt->elem->kind == TYPE_BYTES) {
                            if (tci < 0) {   /* temp: move — без retain */
                                LLVMValueRef bd = LLVMBuildExtractValue(lg.builder,
                                    e, 0, "bd");
                                h_call(baga_rt("baga_rc_retain"), &bd, 1, "rcr");
                            }
                            LLVMValueRef ga[] = { v, i };
                            LLVMValueRef ob = h_call(baga_rt("baga_vec_get_box"),
                                ga, 2, "ob");
                            LLVMValueRef obp = LLVMBuildBitCast(lg.builder, ob,
                                LLVMPointerType(baga_bytes_ty(), 0), "obp");
                            LLVMValueRef od = LLVMBuildLoad2(lg.builder, lg.ptr_ty,
                                LLVMBuildStructGEP2(lg.builder, baga_bytes_ty(),
                                    obp, 0, "odp"), "od");
                            h_call(baga_rt("baga_rc_release_bytes"), &od, 1, "");
                        } else if (lg.rc) {
                            lrc_embed_retain(n->args.data[2], e);
                            LLVMValueRef rel = lrc_box_rel_fn_ty(vt->elem);
                            if (rel) {
                                LLVMValueRef ga[] = { v, i };
                                LLVMValueRef ob = h_call(
                                    baga_rt("baga_vec_get_box"), ga, 2, "ob");
                                LLVMTypeRef rfty = LLVMFunctionType(lg.void_ty,
                                    &lg.ptr_ty, 1, 0);
                                LLVMBuildCall2(lg.builder, rfty, rel, &ob, 1, "");
                            }
                        }
                        LLVMValueRef tmp = entry_alloca(sty, "bx");
                        LLVMBuildStore(lg.builder, e, tmp);
                        LLVMValueRef bp = LLVMBuildBitCast(lg.builder, tmp, lg.ptr_ty, "bp");
                        LLVMValueRef pa[] = { v, i, bp, sz };
                        LLVMValueRef r = h_call(baga_rt("baga_vec_set_box"), pa, 4, "");
                        lrc_tmp_consume(tci);
                        return r;
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
                    else if (vt->elem->kind == TYPE_VEC) suf = "str";
                    else if (vt->elem->kind == TYPE_STRUCT)
                        llvm_unsupported("Vec<struct> (анонимен — само C бекенда; вж. docs/language-en.md)");
                }
                /* RC5 v0.7: temp стойност (str, и Vec елементи през str
                 * helper-а) — helper-ът retain-ва; маркирай за consume +
                 * release след повикването (нетен move, codegen_c.c:2484) */
                if (lg.rc && strcmp(suf, "str") == 0 &&
                    strcmp(n->callee->name, "vec_get") != 0 &&
                    strcmp(n->callee->name, "vec_slice") != 0 &&
                    strcmp(n->callee->name, "vec_concat") != 0) {
                    int vi = strcmp(n->callee->name, "vec_push") == 0 ? 1 : 2;
                    if (n->args.len > vi &&
                        lrc_tmp_find(n->args.data[vi]) >= 0)
                        rc_consume_arg = vi;
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
                        /* RC5 v1.0b: temp стойност (owned call) — move в
                         * box-а (embed_retain вече не retain-ва owned call),
                         * консумирай temp-а (codegen_c.c:2575-2582) */
                        int tci = lrc_tmp_find(n->args.data[2]);
                        /* RC (Task 6): box стойността споделя heap полетата —
                         * retain на новото, после relf на стария box при
                         * overwrite (alias-safe ред; codegen_c:
                         * baga_map_set_<k>_box_rc, codegen_c.c:2514-2522) */
                        if (lg.rc) {
                            lrc_embed_retain(n->args.data[2], v);
                            LLVMValueRef rel = lrc_box_rel_fn_ty(mt->elem);
                            if (rel) {
                                char get_name[64];
                                snprintf(get_name, sizeof get_name,
                                         "baga_map_get_%s_box", ksuf);
                                LLVMValueRef ga[] = { mv, k };
                                LLVMValueRef ob = h_call(baga_rt(get_name),
                                    ga, 2, "ob");
                                LLVMValueRef cfn =
                                    LLVMGetBasicBlockParent(
                                        LLVMGetInsertBlock(lg.builder));
                                LLVMBasicBlockRef rel_bb =
                                    LLVMAppendBasicBlockInContext(lg.ctx,
                                        cfn, "orel");
                                LLVMBasicBlockRef done_bb =
                                    LLVMAppendBasicBlockInContext(lg.ctx,
                                        cfn, "odone");
                                LLVMBuildCondBr(lg.builder,
                                    LLVMBuildIsNull(lg.builder, ob, "isn"),
                                    done_bb, rel_bb);
                                LLVMPositionBuilderAtEnd(lg.builder, rel_bb);
                                LLVMTypeRef rfty = LLVMFunctionType(lg.void_ty,
                                    &lg.ptr_ty, 1, 0);
                                LLVMBuildCall2(lg.builder, rfty, rel, &ob,
                                               1, "");
                                LLVMBuildBr(lg.builder, done_bb);
                                LLVMPositionBuilderAtEnd(lg.builder, done_bb);
                            }
                        }
                        LLVMValueRef tmp = entry_alloca(sty, "bx");
                        LLVMBuildStore(lg.builder, coerce(v, sty), tmp);
                        LLVMValueRef bp = LLVMBuildBitCast(lg.builder, tmp,
                            lg.ptr_ty, "bp");
                        LLVMValueRef pa[] = { mv, k, bp, LLVMSizeOf(sty) };
                        LLVMValueRef r = h_call(baga_rt(rt_name), pa, 4, "");
                        lrc_tmp_consume(tci);
                        return r;
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
                /* RC5 v0.7: temp стойност в map_set (str/bytes) — helper-ът
                 * retain-ва; маркирай за consume + release след повикването
                 * (codegen_c.c:2647-2662). Ключът пак е retain+release. */
                if (lg.rc && is_set && n->args.len >= 3 &&
                    (strcmp(vsuf, "str") == 0 || strcmp(vsuf, "bytes") == 0) &&
                    lrc_tmp_find(n->args.data[2]) >= 0)
                    rc_consume_arg = 2;
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
            /* MEM-1: drop(x) builtin. user fn 'drop' е resolve-ната по-горе
             * (fn != NULL), затова тук drop е винаги builtin-ът — огледало на
             * checker guard-а (checker.c:1822). */
            if (!fn && strcmp(n->callee->name, "drop") == 0) {
                lrc_emit_drop(n);
                return NULL;  /* drop е statement-израз без стойност */
            }
            /* MEM-4: mark/rewind/persist работят върху bump arena-та, която
             * съществува само в C бекенда (LLVM runtime-ът е plain malloc) */
            if (!fn && (strcmp(n->callee->name, "mem_mark") == 0 ||
                        strcmp(n->callee->name, "mem_rewind") == 0 ||
                        strcmp(n->callee->name, "mem_persist_begin") == 0 ||
                        strcmp(n->callee->name, "mem_persist_end") == 0))
                llvm_unsupported("mem_mark/mem_rewind/mem_persist_* — само C бекенда; вж. docs/language-en.md");
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
            if (rc_consume_arg >= 0) {
                /* RC5 v0.7: helper-ът retain-на своята референция към temp-а;
                 * консумирай го и release-ни temp-овата — нетен move в
                 * контейнера (като _move helper вариантите в codegen_c) */
                int ti = lrc_tmp_find(n->args.data[rc_consume_arg]);
                if (ti >= 0) {
                    LLRcTmp t = lg.lrc_tmps[ti];
                    lrc_tmp_consume(ti);
                    lrc_emit_release_val(t.tag, t.type, t.val);
                }
            }
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
            /* RC: overwrite на track-нат локал — retain на новото (при alias
             * от IDENT или borrowed дясно — vec_get/map_get/поле/h_*;
             * codegen_c.c:2866 `keep`) ПРЕДИ release на старото:
             * alias-safe редът на codegen_c (`x = x` не обесва стойността).
             * Params/dead — не. */
            int ai = lg.rc ? lrc_find(n->assign_target->name) : -1;
            if (ai >= 0 && !lg.lrc_locals[ai].is_param && !lg.lrc_locals[ai].dead) {
                if (n->assign_val->kind == NODE_IDENT ||
                    lrc_borrowed_init(n->assign_val))
                    lrc_emit_retain_val(lg.lrc_locals[ai].tag,
                                        lg.lrc_locals[ai].type,
                                        lg.lrc_locals[ai].type_node, v);
                lrc_emit_release(&lg.lrc_locals[ai]);
            }
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
            /* M24: instantiated generic struct — типът по cname */
            LLVMTypeRef sty;
            Node *saved_gs = lg.gen_struct;
            int saved_gi = lg.gen_struct_inst;
            if (obj->type->n_targs > 0) {
                char *cn = llvm_struct_cname(obj->type);
                sty = user_struct_ty(cn);
                free(cn);
                lg.gen_struct = decl;
                for (int k = 0; k < decl->struct_inst_count; k++) {
                    int same = 1;
                    for (int a = 0; a < decl->n_struct_params; a++)
                        if (!type_eq(decl->struct_inst_targs[k * decl->n_struct_params + a],
                                     obj->type->targs[a])) { same = 0; break; }
                    if (same) { lg.gen_struct_inst = k; break; }
                }
            } else {
                sty = user_struct_ty(sname);
            }
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
            lg.gen_struct = saved_gs;
            lg.gen_struct_inst = saved_gi;
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
            /* M24: instantiated generic struct — типът по cname */
            LLVMTypeRef sty;
            if (n->type && n->type->kind == TYPE_STRUCT && n->type->n_targs > 0) {
                char *cn = llvm_struct_cname(n->type);
                sty = user_struct_ty(cn);
                free(cn);
            } else {
                sty = user_struct_ty(n->lit_name);
            }
            LLVMValueRef tmp = entry_alloca(sty, "slit");
            LLVMBuildStore(lg.builder, LLVMConstNull(sty), tmp);
            /* M24: полетата на generic struct literal се резолват под
             * substitution (параметри → targs на литерала) */
            Node *saved_gs = lg.gen_struct;
            int saved_gi = lg.gen_struct_inst;
            if (n->type && n->type->n_targs > 0) {
                lg.gen_struct = decl;
                for (int k = 0; k < decl->struct_inst_count; k++) {
                    int same = 1;
                    for (int a = 0; a < decl->n_struct_params; a++)
                        if (!type_eq(decl->struct_inst_targs[k * decl->n_struct_params + a],
                                     n->type->targs[a])) { same = 0; break; }
                    if (same) { lg.gen_struct_inst = k; break; }
                }
            }
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
                /* RC (Task 6): литералът споделя heap полета по указател —
                 * ident/borrowed стойност се retain-ва (+1 за полето);
                 * fresh израз е owned — move (codegen_c.c:3111-3141) */
                lrc_embed_retain(n->lit_values.data[i], v);
                LLVMValueRef gep = LLVMBuildStructGEP2(lg.builder, sty, tmp,
                                                       (unsigned)idx, "fld");
                LLVMBuildStore(lg.builder, coerce(v, fty), gep);
            }
            char *name = tmp_name();
            LLVMValueRef r = LLVMBuildLoad2(lg.builder, sty, tmp, name);
            free(name);
            lg.gen_struct = saved_gs;
            lg.gen_struct_inst = saved_gi;
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
        case NODE_TRY: {
            /* M20: e? — при payload ефекти: изчисли, провери слота, при
             * грешка return-ни нулата; иначе passthrough (и в catch верига) */
            if (!llvm_has_payload_effects(n->try_expr ? n->try_expr->type : NULL) ||
                lg.eff_depth > 0)
                return emit_expr_llvm(n->try_expr);
            LLVMValueRef v = emit_expr_llvm(n->try_expr);
            LLVMValueRef fn = LLVMGetBasicBlockParent(LLVMGetInsertBlock(lg.builder));
            LLVMBasicBlockRef err_bb = LLVMAppendBasicBlockInContext(lg.ctx, fn, "eff_err");
            LLVMBasicBlockRef cont_bb = LLVMAppendBasicBlockInContext(lg.ctx, fn, "eff_cont");
            LLVMValueRef tag = LLVMBuildLoad2(lg.builder, lg.i64_ty,
                                              eff_gep(0, "tagp"), "tag");
            LLVMValueRef bad = LLVMBuildICmp(lg.builder, LLVMIntNE, tag,
                                             LLVMConstInt(lg.i64_ty, 0, 0), "bad");
            LLVMBuildCondBr(lg.builder, bad, err_bb, cont_bb);
            LLVMPositionBuilderAtEnd(lg.builder, err_bb);
            if (lg.rc) lrc_release_all(-1);
            h_ret_zero();
            LLVMPositionBuilderAtEnd(lg.builder, cont_bb);
            return v;
        }
        case NODE_CATCH: {
            /* M20: flatten веригата — base стойност → tag → по веригата
             * handler блокове → phi; последният клон propagate-ва */
            int any_payload = 0;
            {
                Node *scan = n;
                while (scan && scan->kind == NODE_CATCH) {
                    Type *et = scan->catch_expr ? scan->catch_expr->type : NULL;
                    Type *pl = type_effect_payload(et, scan->catch_effect);
                    if (pl) any_payload = 1;
                    scan = scan->catch_expr;
                }
            }
            if (!any_payload) {
                Node *base = n;
                while (base && base->kind == NODE_CATCH) base = base->catch_expr;
                return emit_expr_llvm(base);
            }
            Node *base = n;
            while (base && base->kind == NODE_CATCH) base = base->catch_expr;
            lg.eff_depth++;
            LLVMValueRef v = emit_expr_llvm(base);
            lg.eff_depth--;
            LLVMValueRef fn = LLVMGetBasicBlockParent(LLVMGetInsertBlock(lg.builder));
            LLVMValueRef tag = LLVMBuildLoad2(lg.builder, lg.i64_ty,
                                              eff_gep(0, "tagp"), "tag");
            LLVMBuildStore(lg.builder, LLVMConstInt(lg.i64_ty, 0, 0),
                           eff_gep(0, "tagp"));
            /* събери payload catch-овете */
            int nhandlers = 0;
            Node *scans[64];
            {
                Node *scan = n;
                while (scan && scan->kind == NODE_CATCH && nhandlers < 64) {
                    Type *et = scan->catch_expr ? scan->catch_expr->type : NULL;
                    Type *pl = type_effect_payload(et, scan->catch_effect);
                    if (pl) scans[nhandlers++] = scan;
                    scan = scan->catch_expr;
                }
            }
            /* блокове: decision[0..n-1] + handler[0..n-1] + prop + merge */
            LLVMBasicBlockRef dec[65] = {0}, hbs[65] = {0};
            LLVMValueRef hvs[65] = {0};
            LLVMBasicBlockRef prop_bb = LLVMAppendBasicBlockInContext(lg.ctx, fn, "eff_prop");
            LLVMBasicBlockRef merge_bb = LLVMAppendBasicBlockInContext(lg.ctx, fn, "eff_merge");
            for (int i = 0; i < nhandlers; i++) {
                dec[i] = LLVMAppendBasicBlockInContext(lg.ctx, fn, "eff_dec");
                hbs[i] = LLVMAppendBasicBlockInContext(lg.ctx, fn, "eff_h");
            }
            LLVMBasicBlockRef cur_bb = LLVMGetInsertBlock(lg.builder);
            LLVMBuildCondBr(lg.builder,
                LLVMBuildICmp(lg.builder, LLVMIntEQ, tag,
                              LLVMConstInt(lg.i64_ty, 0, 0), "ok"),
                merge_bb, dec[0]);
            for (int i = 0; i < nhandlers; i++) {
                Node *scan = scans[i];
                Type *et = scan->catch_expr ? scan->catch_expr->type : NULL;
                Type *pl = type_effect_payload(et, scan->catch_effect);
                int tagv = llvm_eff_tag(scan->catch_effect);
                LLVMPositionBuilderAtEnd(lg.builder, dec[i]);
                LLVMBuildCondBr(lg.builder,
                    LLVMBuildICmp(lg.builder, LLVMIntEQ, tag,
                                  LLVMConstInt(lg.i64_ty, (unsigned long long)tagv, 0), "eq"),
                    hbs[i], i + 1 < nhandlers ? dec[i + 1] : prop_bb);
                LLVMPositionBuilderAtEnd(lg.builder, hbs[i]);
                int saved_vars = lg_st.count;
                if (scan->catch_binding) {
                    unsigned fidx = eff_field_of(pl);
                    LLVMValueRef pv = LLVMBuildLoad2(lg.builder,
                        llvm_type_resolved(pl), eff_gep(fidx, "pl"), "pl");
                    LLVMValueRef pa = entry_alloca(llvm_type_resolved(pl), "pld");
                    LLVMBuildStore(lg.builder, pv, pa);
                    st_define(scan->catch_binding, pa);
                }
                hvs[i] = scan->catch_handler->kind == NODE_BLOCK
                    ? emit_block_value_llvm(scan->catch_handler)
                    : emit_expr_llvm(scan->catch_handler);
                if (scan->catch_binding) lg_st.count = saved_vars;
                if (!LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(lg.builder)))
                    LLVMBuildBr(lg.builder, merge_bb);
            }
            LLVMPositionBuilderAtEnd(lg.builder, prop_bb);
            /* възстанови тага за външния контекст (като C бекенда) */
            LLVMBuildStore(lg.builder, tag, eff_gep(0, "tagp"));
            if (lg.rc) lrc_release_all(-1);
            h_ret_zero();
            LLVMPositionBuilderAtEnd(lg.builder, merge_bb);
            if (n->type && llvm_type_resolved(n->type) == lg.void_ty) {
                LLVMBuildRetVoid(lg.builder);
                return NULL;
            }
            LLVMTypeRef rty = n->type ? llvm_type_resolved(n->type) : lg.i64_ty;
            LLVMValueRef phi = LLVMBuildPhi(lg.builder, rty, "effv");
            LLVMAddIncoming(phi, &v, &cur_bb, 1);
            /* входящи само от блокове, които действително достигат merge
             * (хендлър с terminator — напр. return вътре — не стига) */
            for (int i = 0; i < nhandlers; i++) {
                /* входящо само ако хендлърът реално завършва с br към merge
                 * (br-ът е terminator; ret/друго значи „не достига merge“) */
                LLVMValueRef t = LLVMGetBasicBlockTerminator(hbs[i]);
                if (!t || !LLVMIsABranchInst(t) ||
                    LLVMGetOperand(t, 0) != (LLVMValueRef)merge_bb) continue;
                if (!hvs[i]) hvs[i] = LLVMConstNull(rty);
                LLVMAddIncoming(phi, &hvs[i], &hbs[i], 1);
            }
            return phi;
        }
        case NODE_RAISE: {
            /* M20: задай слота и излез (като `?` / h_ret_zero).
             * raise_dead е недостижим блок, за да могат извикващите
             * emit_expr да продължат без инструкция след terminator.
             * RC: release на локалите преди изхода (паритет с C); heap
             * payload, който не е fresh, се retain-ва преди release-ите —
             * fresh payload се move-ва в слота (като в codegen_c). */
            int tagv = llvm_eff_tag(n->raise_effect);
            LLVMBuildStore(lg.builder, LLVMConstInt(lg.i64_ty, (unsigned long long)tagv, 0),
                           eff_gep(0, "tagp"));
            if (n->raise_payload && n->raise_payload->type) {
                Node *p = n->raise_payload;
                LLVMValueRef pv = emit_expr_llvm(p);
                LLVMBuildStore(lg.builder, coerce(pv, llvm_type_resolved(p->type)),
                               eff_gep(eff_field_of(p->type), "pv"));
                if (lg.rc) {
                    int ptag = lrc_heap_tag(p->type);
                    if (ptag && !lrc_tmp_fresh(p) && !lrc_is_enum_ctor(p) &&
                        p->kind != NODE_STRUCT_LIT)
                        lrc_emit_retain_val(ptag, p->type, NULL, pv);
                }
            }
            if (lg.rc) lrc_release_all(-1);
            h_ret_zero();
            {
                LLVMValueRef fn = LLVMGetBasicBlockParent(LLVMGetInsertBlock(lg.builder));
                LLVMBasicBlockRef dead = LLVMAppendBasicBlockInContext(lg.ctx, fn, "raise_dead");
                LLVMPositionBuilderAtEnd(lg.builder, dead);
            }
            return LLVMConstInt(lg.i64_ty, 0, 0);
        }
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
            if (ek == TYPE_F64)
                return h_call(baga_rt("baga_f64_to_str"), &v, 1, "f2s");
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
        case NODE_BLOCK:
            /* блок като стойност (`let x = { … }`) — същата implicit-return
             * семантика като catch handler */
            return emit_block_value_llvm(n);
        default:            llvm_unsupported_node(n); break;
    }
    return NULL; /* unreachable */
}

/* Стойност на блок-хендлър на catch: statement-и + последният е стойността
 * (implicit return семантика, като C бекенда) */
static LLVMValueRef emit_block_value_llvm(Node *block) {
    if (!block || block->kind != NODE_BLOCK) llvm_unsupported("catch хендлър без тяло");
    st_push();
    /* RC: handler-ът е свой scope — heap let-овете в него се release-ват при
     * изхода му, ДОКАТО st_lookup още resolve-ва (lrc_pop_scope преди st_pop).
     * Без това те се track-ваха във външния scope и при release-а му
     * st_lookup връщаше NULL — тих leak. */
    lrc_push_scope(0);
    int lrc_top = lg.rc ? lg.lrc_scopes[lg.lrc_depth - 1].top : 0;
    LLVMValueRef val = NULL;
    lg.lrc_branch_depth++;   /* handler-ът се изпълнява само при raise */
    for (int i = 0; i < block->stmts.len; i++) {
        if (LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(lg.builder))) break;
        Node *s = block->stmts.data[i];
        if (i == block->stmts.len - 1) {
            /* последният stmt е СТОЙНОСТТА на handler-а (EXPR_STMT или
             * return <израз> — и двата са стойност, не реален ret) */
            Node *ve = NULL;
            if (s->kind == NODE_EXPR_STMT) ve = s->expr;
            else if (s->kind == NODE_RETURN && s->ret_val) ve = s->ret_val;
            if (ve) {
                val = emit_expr_llvm(ve);
                /* RC: move семантика — стойността escape-ва към phi-то на
                 * catch израза; handler-локал IDENT се маркира dead, за да не
                 * бъде release-нат от pop-а. Външен IDENT/параметър е
                 * borrowed копие — не се пипа (C parity). */
                if (lg.rc && ve->kind == NODE_IDENT) {
                    int vi = lrc_find(ve->name);
                    if (vi >= lrc_top && !lg.lrc_locals[vi].is_param)
                        lg.lrc_locals[vi].dead = 1;
                }
            } else {
                emit_stmt_llvm(s, NULL, NULL);
            }
        } else emit_stmt_llvm(s, NULL, NULL);
    }
    lg.lrc_branch_depth--;
    /* ако блокът има terminator (реален return вътре мина през
     * lrc_release_all), pop-ът само прибира записите — без дублиране */
    lrc_pop_scope();
    st_pop();
    return val;
}

/* ---- Statement emission ---- */

static void emit_stmt_llvm(Node *n, LLVMBasicBlockRef break_bb, LLVMBasicBlockRef cont_bb);
static LLVMValueRef emit_block_value_llvm(Node *block);

static void emit_block_llvm(Node *block, LLVMBasicBlockRef break_bb, LLVMBasicBlockRef cont_bb) {
    if (!block) return;
    if (block->kind != NODE_BLOCK)
        llvm_unsupported("тяло, което не е блок");
    st_push();
    lrc_push_scope(lrc_loop_next);
    lrc_loop_next = 0;
    for (int i = 0; i < block->stmts.len; i++) {
        /* мъртъв код след terminator — както gcc след return */
        if (LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(lg.builder)))
            break;
        emit_stmt_llvm(block->stmts.data[i], break_bb, cont_bb);
    }
    lrc_pop_scope();
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
            /* RC4: temp-ове в init (root-ът е bound — не е temp) */
            LLRcTmpSave tsv;
            if (lg.rc) lrc_tmp_begin(n->let_init, 1, &tsv);
            if (n->let_init) {
                LLVMValueRef val = emit_expr_llvm(n->let_init);
                if (!val) llvm_unsupported("print като стойност на let");
                val = coerce(val, ty);
                /* RC: alias (let x = y) → retain преди store; свежата
                 * конструкция идва с rc=1 от alloc — без retain. Borrowed
                 * init (vec_get/map_get/поле/h_*) също се retain-ва —
                 * референцията става собствена (порт на rc_borrowed_init
                 * сайта codegen_c.c:3940; без borrow elision-а на C —
                 * двойката retain+release е балансирана). tag 5/6 borrowed
                 * не се track-ва (gating-ът по-долу, като C) → и не се
                 * retain-ва (leak-safe). */
                if (lg.rc) {
                    if (n->let_init->kind == NODE_IDENT)
                        lrc_emit_retain_val(lrc_heap_tag(n->let_init->type),
                                            n->let_init->type, NULL, val);
                    else if (lrc_borrowed_init(n->let_init)) {
                        int btag = lrc_heap_tag(n->let_init->type);
                        if (!btag && n->let_type)
                            btag = lrc_heap_tag_node(n->let_type);
                        if (btag && btag < 5)
                            lrc_emit_retain_val(btag, n->let_init->type,
                                                NULL, val);
                    }
                }
                LLVMBuildStore(lg.builder, val, alloca);
            }
            st_define(n->let_name, alloca);
            /* RC: tag от init типа; fallback към анотацията (`let v: Vec<str>
             * = vec_new()` — inferred типът няма elem информация, но kind
             * стига за tag-а). Като rc_heap_tag + rc_heap_tag_node в C.
             * Task 6: tag 5/6 се регистрират САМО при свеж литерал/ctor или
             * alias на track-нат ident (codegen_c.c:3898-3920) — fn
             * резултат/vec_get/поле споделят собствеността и се остават
             * untrack-нати (leak-safe, като C). */
            if (lg.rc) {
                int ltag = n->let_init ? lrc_heap_tag(n->let_init->type) : 0;
                if (!ltag && n->let_type) ltag = lrc_heap_tag_node(n->let_type);
                if (ltag == 5 || ltag == 6) {
                    int fresh = n->let_init &&
                        (n->let_init->kind == NODE_STRUCT_LIT ||
                         lrc_is_enum_ctor(n->let_init));
                    int from_tr = 0;
                    if (n->let_init && n->let_init->kind == NODE_IDENT) {
                        int si = lrc_find(n->let_init->name);
                        if (si >= 0 && !lg.lrc_locals[si].dead) from_tr = 1;
                    }
                    if (!fresh && !from_tr) ltag = 0;
                }
                lrc_track_tag(n->let_name, ltag,
                              n->let_init ? n->let_init->type : NULL,
                              n->let_type, 0);
            }
            if (lg.rc) lrc_tmp_end(&tsv);
            break;
        }

        case NODE_RETURN: {
            if (n->ret_val) {
                /* RC4: temp-ове в return израза (root-ът отива на caller-а) */
                LLRcTmpSave tsv;
                if (lg.rc) lrc_tmp_begin(n->ret_val, 1, &tsv);
                LLVMValueRef val = emit_expr_llvm(n->ret_val);
                if (!val) llvm_unsupported("print като return стойност");
                val = coerce(val, lg.cur_ret_ty);
                /* RC4: release на temp-овете преди release на локалите
                 * (като rc_tmp_release_all в emit_return_val, codegen_c) */
                if (lg.rc) lrc_tmp_release_all();
                /* RC: move на върнатия локал + release на останалите */
                lrc_return_move(n->ret_val, val);
                LLVMBuildRet(lg.builder, val);
                if (lg.rc) lrc_tmp_restore(&tsv);
            } else {
                if (lg.cur_ret_ty != lg.void_ty)
                    llvm_unsupported("return без стойност в не-void функция");
                lrc_release_all(-1);
                LLVMBuildRetVoid(lg.builder);
            }
            break;
        }

        case NODE_IF: {
            /* RC4 v0.3: temp-ове в условието се release-ват веднага след
             * оценката — преди клоновете. В LLVM не трябва GNU ({…}) wrap:
             * temp-овете се emit-ват и release-ват в текущия block. */
            LLRcTmpSave tsv;
            if (lg.rc) lrc_tmp_begin(n->cond, 0, &tsv);
            LLVMValueRef cond = to_bool(emit_expr_llvm(n->cond));
            if (lg.rc) lrc_tmp_end(&tsv);
            LLVMValueRef fn = LLVMGetBasicBlockParent(LLVMGetInsertBlock(lg.builder));
            LLVMBasicBlockRef then_bb = LLVMAppendBasicBlockInContext(lg.ctx, fn, "then");
            LLVMBasicBlockRef else_bb = LLVMAppendBasicBlockInContext(lg.ctx, fn, "else");
            LLVMBasicBlockRef merge_bb = LLVMAppendBasicBlockInContext(lg.ctx, fn, "merge");

            LLVMBuildCondBr(lg.builder, cond, then_bb, else_bb);

            lg.lrc_branch_depth++;   /* then/else са условен поток */
            LLVMPositionBuilderAtEnd(lg.builder, then_bb);
            emit_block_llvm(n->then_br, break_bb, cont_bb);
            if (!LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(lg.builder)))
                LLVMBuildBr(lg.builder, merge_bb);

            LLVMPositionBuilderAtEnd(lg.builder, else_bb);
            if (n->else_br) emit_block_llvm(n->else_br, break_bb, cont_bb);
            if (!LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(lg.builder)))
                LLVMBuildBr(lg.builder, merge_bb);
            lg.lrc_branch_depth--;

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
            /* RC4 v0.3: cond_bb се изпълнява на всяка итерация — temp
             * emission + release тук са естественият per-iteration път
             * (без GNU wrap трика на C) */
            LLRcTmpSave tsv;
            if (lg.rc) lrc_tmp_begin(n->while_cond, 0, &tsv);
            LLVMValueRef cond = to_bool(emit_expr_llvm(n->while_cond));
            if (lg.rc) lrc_tmp_end(&tsv);
            LLVMBuildCondBr(lg.builder, cond, body_bb, end_bb);

            LLVMPositionBuilderAtEnd(lg.builder, body_bb);
            lrc_loop_next = 1;   /* тялото на while е loop scope (break/continue) */
            lg.lrc_branch_depth++;   /* тялото се изпълнява условно/повторно */
            emit_block_llvm(n->while_body, end_bb, cond_bb);
            lg.lrc_branch_depth--;
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
            /* RC4 v0.3: lo temp-ове се оценяват веднъж (init) и се
             * release-ват веднага след изчислената граница */
            LLRcTmpSave tsv_lo;
            if (lg.rc) lrc_tmp_begin(n->for_iter->range_lo, 0, &tsv_lo);
            LLVMValueRef lo = emit_expr_llvm(n->for_iter->range_lo);
            if (!lo || LLVMGetTypeKind(LLVMTypeOf(lo)) != LLVMIntegerTypeKind)
                llvm_unsupported("for с не-целочислен диапазон");
            lo = coerce(lo, lg.i64_ty);
            if (lg.rc) lrc_tmp_end(&tsv_lo);

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
            /* RC4 v0.3: hi се преоценява на всяка итерация (като в C) —
             * temp emission + release в cond_bb са per-iteration */
            LLRcTmpSave tsv_hi;
            if (lg.rc) lrc_tmp_begin(n->for_iter->range_hi, 0, &tsv_hi);
            LLVMValueRef hi = emit_expr_llvm(n->for_iter->range_hi);
            if (!hi || LLVMGetTypeKind(LLVMTypeOf(hi)) != LLVMIntegerTypeKind)
                llvm_unsupported("for с не-целочислен диапазон");
            hi = coerce(hi, lg.i64_ty);
            if (lg.rc) lrc_tmp_end(&tsv_hi);
            char *nm = tmp_name();
            LLVMValueRef cur = LLVMBuildLoad2(lg.builder, lg.i64_ty, var, nm);
            free(nm);
            nm = tmp_name();
            LLVMValueRef cond = LLVMBuildICmp(lg.builder, LLVMIntSLT, cur, hi, nm);
            free(nm);
            LLVMBuildCondBr(lg.builder, cond, body_bb, end_bb);

            LLVMPositionBuilderAtEnd(lg.builder, body_bb);
            lrc_loop_next = 1;   /* тялото на for е loop scope (break/continue) */
            lg.lrc_branch_depth++;   /* тялото се изпълнява условно/повторно */
            emit_block_llvm(n->for_body, end_bb, incr_bb);
            lg.lrc_branch_depth--;
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

        case NODE_EXPR_STMT: {
            /* RC4: temp-ове в израза (root на bare call с heap резултат е
             * дискарднат — пак е temp; assign дясното е bound) */
            LLRcTmpSave tsv;
            if (lg.rc) lrc_tmp_begin(n->expr, 0, &tsv);
            emit_expr_llvm(n->expr);
            if (lg.rc) lrc_tmp_end(&tsv);
            break;
        }

        case NODE_BLOCK:
            emit_block_llvm(n, break_bb, cont_bb);
            break;

        case NODE_BREAK:
            if (!break_bb) llvm_unsupported("break извън цикъл");
            /* RC: release на напуснатите scope-ове до loop тялото преди скока */
            lrc_release_to_loop();
            LLVMBuildBr(lg.builder, break_bb);
            break;

        case NODE_CONTINUE:
            if (!cont_bb) llvm_unsupported("continue извън цикъл");
            lrc_release_to_loop();
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

/* M21: име за emit (synthetic при инстанция) */
static const char *emit_name_of(Node *fn) {
    if (lg.gen_fn == fn) {
        static __thread char buf[512];
        snprintf(buf, sizeof buf, "%s__i%d", fn->fn_name, lg.gen_inst);
        return buf;
    }
    return fn->fn_name;
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
    char *m = llvm_mangle(emit_name_of(fn));
    LLVMAddFunction(lg.mod, m, fn_ty);
    free(m);
}

/* Тялото на функцията (за spec функции — под impl името). */
static void emit_fn_llvm(Node *fn, Node *spec) {
    if (!fn->fn_body) return; /* само декларация */

    char *m = spec ? impl_name_of(fn->fn_name) : llvm_mangle(emit_name_of(fn));
    LLVMValueRef fn_val = LLVMGetNamedFunction(lg.mod, m);
    free(m);

    LLVMTypeRef ret_ty = fn_ret_type(fn);
    lg.cur_ret_ty = ret_ty;

    LLVMBasicBlockRef entry = LLVMAppendBasicBlockInContext(lg.ctx, fn_val, "entry");
    LLVMPositionBuilderAtEnd(lg.builder, entry);

    st_reset();
    st_push();
    /* RC: нова функция — чист RC стек; scope 0 е scope-ът на функцията */
    lg.lrc_count = 0;
    lg.lrc_depth = 0;
    lg.lrc_tmp_count = 0;   /* RC4: и чист temp регистър (defensive) */
    lg.lrc_tmps_on = 0;
    lg.lrc_tmp_decl = NULL;
    lrc_push_scope(0);
    lg.lrc_fn_base = 0;

    /* alloca за параметрите (за променливост) */
    for (int i = 0; i < fn->params.len; i++) {
        char *pm = llvm_mangle(fn->params.data[i]->param_name);
        LLVMValueRef param = LLVMGetParam(fn_val, (unsigned)i);
        LLVMSetValueName2(param, pm, strlen(pm));
        LLVMValueRef alloca = LLVMBuildAlloca(lg.builder, LLVMTypeOf(param), pm);
        LLVMBuildStore(lg.builder, param, alloca);
        free(pm);
        st_define(fn->params.data[i]->param_name, alloca);
        /* RC: параметрите са borrowed (is_param=1) — track-ват се (за retain
         * при alias/move проверки), но не се release-ват */
        lrc_track_tag(fn->params.data[i]->param_name,
                      lrc_heap_tag_node(fn->params.data[i]->param_type),
                      fn->params.data[i]->type,
                      fn->params.data[i]->param_type, 1);
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
            /* RC4: temp-ове в implicit return израза — същият път като
             * explicit return (release преди return-а) */
            LLRcTmpSave tsv;
            if (lg.rc) lrc_tmp_begin(s->expr, 1, &tsv);
            LLVMValueRef v = emit_expr_llvm(s->expr);
            /* raise вече емитира ret — без втори terminator */
            if (!LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(lg.builder))) {
                if (!v) llvm_unsupported("print като неявен return");
                v = coerce(v, ret_ty);
                if (lg.rc) lrc_tmp_release_all();
                /* RC: същата move семантика като explicit return —
                 * release на локалите ПРЕДИ ret (codegen_c: emit_return_val) */
                lrc_return_move(s->expr, v);
                LLVMBuildRet(lg.builder, v);
            }
            if (lg.rc) lrc_tmp_end(&tsv);
        } else {
            emit_stmt_llvm(s, NULL, NULL);
        }
    }

    /* RC: край на fn scope-а — release на останалите локали при fall-through
     * (при explicit return блокът има terminator и това е само pop) */
    lrc_pop_scope();
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
    /* RC: wrapper-ът е отделна функция — чист RC стек (тялото му не
     * дефинира heap локали; push/pop само за консистентност) */
    lg.lrc_count = 0;
    lg.lrc_depth = 0;
    lrc_push_scope(0);
    lg.lrc_fn_base = 0;

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
    lrc_pop_scope();
    st_pop();
}

/* ---- Public API ---- */

void codegen_llvm(Node *program, const char *output_path, Checker *chk, int rc) {
    lg.ctx = LLVMContextCreate();
    lg.mod = LLVMModuleCreateWithNameInContext("baga_module", lg.ctx);
    lg.builder = LLVMCreateBuilderInContext(lg.ctx);
    lg.tmp_counter = 0;
    lg.program = program;
    lg.chk = chk;
    lg.rc = rc;
    lg.gen_fn = NULL; lg.gen_inst = -1;
    lg.gen_struct = NULL; lg.gen_struct_inst = -1;

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

        /* snprintf(buf, size, fmt, ...) — за baga_f64_to_str (%g) */
        LLVMTypeRef snprintf_args[] = { lg.ptr_ty, lg.i64_ty, lg.ptr_ty };
        LLVMTypeRef snprintf_ty = LLVMFunctionType(lg.i32_ty, snprintf_args, 3, 1);
        lg.snprintf_fn = LLVMAddFunction(lg.mod, "snprintf", snprintf_ty);

        LLVMTypeRef exit_args[] = { lg.i32_ty };
        LLVMTypeRef exit_ty = LLVMFunctionType(lg.void_ty, exit_args, 1, 0);
        lg.exit_fn = LLVMAddFunction(lg.mod, "exit", exit_ty);

        /* glibc: stderr е external global (FILE*) */
        lg.stderr_global = LLVMAddGlobal(lg.mod, lg.ptr_ty, "stderr");
    }

    /* нулев проход: named struct типове + sum enum типове (имена, после тела) */
    for (int i = 0; i < program->items.len; i++) {
        Node *item = program->items.data[i];
        if (item->kind == NODE_STRUCT && item->n_struct_params > 0) {
            /* M24: generic struct — named тип per инстанция */
            for (int k = 0; k < item->struct_inst_count; k++) {
                char *cn = llvm_inst_cname(item, k);
                if (!LLVMGetTypeByName(lg.mod, cn))
                    LLVMStructCreateNamed(lg.ctx, cn);
                free(cn);
            }
            continue;
        }
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
        if (item->kind == NODE_STRUCT && item->n_struct_params > 0) {
            /* M24: тела на инстанциите (полета под substitution) */
            for (int k = 0; k < item->struct_inst_count; k++) {
                lg.gen_struct = item;
                lg.gen_struct_inst = k;
                char *cn = llvm_inst_cname(item, k);
                LLVMTypeRef st = LLVMGetTypeByName(lg.mod, cn);
                free(cn);
                int nf = item->fields.len;
                LLVMTypeRef *elems = malloc(sizeof(LLVMTypeRef) * (size_t)(nf > 0 ? nf : 1));
                for (int j = 0; j < nf; j++)
                    elems[j] = llvm_type(item->fields.data[j]->fld_type);
                LLVMStructSetBody(st, elems, (unsigned)nf, 0);
                free(elems);
                lg.gen_struct = NULL;
                lg.gen_struct_inst = -1;
            }
            continue;
        }
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

    /* M24-RC: per-instance RC helper-и за generic struct инстанции с heap
     * полета — eager, като C бекенда (emit_rc_struct_helpers след
     * typedef-овете). Lazy пътят (lrc_rc_fn_ty) остава за вложени случаи. */
    if (lg.rc) {
        for (int i = 0; i < program->items.len; i++) {
            Node *item = program->items.data[i];
            if (item->kind != NODE_STRUCT || item->n_struct_params == 0)
                continue;
            for (int k = 0; k < item->struct_inst_count; k++) {
                Type it = {0};
                it.kind = TYPE_STRUCT;
                it.name = item->struct_name;
                it.n_targs = item->n_struct_params;
                it.targs = &item->struct_inst_targs[k * item->n_struct_params];
                if (lrc_heap_tag(&it) != 5) continue;
                lrc_struct_inst_rc_fn(&it, 0);
                lrc_struct_inst_rc_fn(&it, 1);
            }
        }
    }

    /* първи проход: предекларации */
    for (int i = 0; i < program->items.len; i++) {
        Node *item = program->items.data[i];
        if (item->kind == NODE_FN) {
            if (item->n_type_params > 0) {
                for (int k = 0; k < item->inst_count; k++) {
                    if (chk) checker_recheck_inst(chk, item, k);
                    lg.gen_fn = item; lg.gen_inst = k;
                    predeclare_fn_llvm(item);
                }
                lg.gen_fn = NULL; lg.gen_inst = -1;
            } else {
                predeclare_fn_llvm(item);
            }
        }
        if (item->kind == NODE_IMPL)
            for (int m = 0; m < item->impl_methods.len; m++)
                predeclare_fn_llvm(item->impl_methods.data[m]);
    }

    /* втори проход: тела + wrapper-и */
    for (int i = 0; i < program->items.len; i++) {
        Node *item = program->items.data[i];
        if (item->kind == NODE_FN && item->fn_body) {
            if (item->n_type_params > 0) {
                for (int k = 0; k < item->inst_count; k++) {
                    if (chk) checker_recheck_inst(chk, item, k);
                    lg.gen_fn = item; lg.gen_inst = k;
                    emit_fn_llvm(item, NULL);
                }
                lg.gen_fn = NULL; lg.gen_inst = -1;
            } else {
                Node *spec = find_ensures_spec_llvm(item->fn_name);
                emit_fn_llvm(item, spec);
                if (spec) emit_wrapper_llvm(item, spec);
            }
        }
        if (item->kind == NODE_IMPL) {
            for (int m = 0; m < item->impl_methods.len; m++) {
                Node *mf = item->impl_methods.data[m];
                if (!mf->fn_body) continue;
                emit_fn_llvm(mf, NULL);
            }
        }
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
