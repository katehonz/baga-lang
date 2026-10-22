/* codegen_llvm_util.c — част от codegen_llvm.c (виж codegen_llvm_impl.h). */
#include "baga.h"
#include "codegen_llvm_impl.h"





#ifdef BAGA_LLVM

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
 *  Builtin helpers са lazy IR функции (llv_baga_rt) — огледало на
 *  C preamble-а в codegen_c.c.
 *
 *  Принцип: НИКАКВИ тихи стойности — всеки неподдържан конструкт
 *  е compile-time грешка (llv_llvm_unsupported), не константа 0.
 * ============================================================ */

/* RC-LLVM: scope tracking — огледало на RcLocal/RcScope (baga.h:546-555).
 * Активно само при lg.rc. Статични масиви, както LLVMSymtab. */

/* RC4-LLVM: per-statement temp регистър (порт на RcTmp/rc_tmps,
 * baga.h:560-569). За разлика от C (текстови __rc_tmpN декларации преди
 * statement-а) тук temp изразите се emit-ват веднъж преди root-а в текущия
 * block, а LLVMValueRef се кешира по AST възел — при удар llv_emit_expr_llvm
 * връща кеша (SSA аналог на rc_tmp_emit_sub). */


LLVMCodegen lg;

/* ---- честен отказ ---- */

void llv_llvm_unsupported(const char *what) {
    fprintf(stderr, "baga: LLVM backend: неподдържан конструкт '%s'\n", what);
    exit(1);
}

void llv_llvm_unsupported_node(Node *n) {
    char buf[64];
    snprintf(buf, sizeof buf, "AST възел #%d", n ? (int)n->kind : -1);
    llv_llvm_unsupported(buf);
}

/* ---- Type mapping ---- */

LLVMTypeRef llv_llvm_type(Node *ty);
int type_eq(Type *a, Type *b);
char *llv_llvm_mangle(const char *name);
LLVMTypeRef llv_user_struct_ty(const char *name);
LLVMTypeRef llv_baga_bytes_ty(void);
Node *llv_find_struct_decl(const char *name);

char *llv_llvm_struct_cname(Type *t);
/* M24: named struct тип на instantiated generic struct (по cname) */
LLVMTypeRef llv_user_struct_ty_inst(Type *t) {
    char *cn = llv_llvm_struct_cname(t);
    LLVMTypeRef r = llv_user_struct_ty(cn);
    free(cn);
    return r;
}

/* M24: име на инстанция k на generic struct decl */
char *llv_llvm_inst_cname(Node *s, int k) {
    char *m = llv_llvm_mangle(s->struct_name);
    size_t cap = strlen(m) + 1 + (size_t)s->n_struct_params * 48;
    char *out = malloc(cap);
    if (!out) { fprintf(stderr, "baga: out of memory\n"); exit(1); }
    strcpy(out, m);
    free(m);
    for (int a = 0; a < s->n_struct_params; a++) {
        Type *at = s->struct_inst_targs[k * s->n_struct_params + a];
        if (at->kind == TYPE_STRUCT && at->name) {
            char *am = llv_llvm_mangle(at->name);
            strcat(out, "_"); strcat(out, am + 2);
            free(am);
        } else if (at->kind == TYPE_ENUM && at->name) {
            char *am = llv_llvm_mangle(at->name);
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
char *llv_llvm_struct_cname(Type *t) {
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
        case TYPE_BYTES: return llv_baga_bytes_ty();
        case TYPE_STRUCT: {
            if (t->n_targs > 0) {
                char *cn = llv_llvm_struct_cname(t);
                LLVMTypeRef r = llv_user_struct_ty(cn);
                free(cn);
                return r;
            }
            return llv_user_struct_ty(t->name ? t->name : "anon");
        }
        default: return lg.i64_ty;
    }
}
LLVMTypeRef llv_baga_vec_ptr_ty(void);
LLVMTypeRef llv_baga_bytes_ty(void);
LLVMTypeRef llv_baga_map_ptr_ty(void);
LLVMTypeRef llv_user_struct_ty(const char *name);

/* 1 ако възелът е L3 sum enum (поне един вариант с payload) */
int llv_is_sum_enum_item(Node *item) {
    if (!item || item->kind != NODE_ENUM) return 0;
    for (int j = 0; j < item->n_variants; j++)
        if (item->enum_payloads && item->enum_payloads[j]) return 1;
    return 0;
}

/* sum enum декларация по име; NULL ако няма такава */
Node *llv_find_sum_enum(const char *name) {
    if (!name || !lg.program) return NULL;
    for (int i = 0; i < lg.program->items.len; i++) {
        Node *item = lg.program->items.data[i];
        if (llv_is_sum_enum_item(item) && item->enum_name &&
            strcmp(item->enum_name, name) == 0)
            return item;
    }
    return NULL;
}

/* индекс на вариант по име в enum декларация; -1 ако го няма */
int llv_sum_variant_index(Node *ed, const char *vname) {
    if (!ed || !vname) return -1;
    for (int j = 0; j < ed->n_variants; j++)
        if (strcmp(ed->enum_variants[j], vname) == 0) return j;
    return -1;
}

LLVMTypeRef llv_llvm_type_resolved(Type *ty) {
    if (!ty) return lg.i64_ty;
    switch (ty->kind) {
        case TYPE_I64: return lg.i64_ty;
        case TYPE_I32: return lg.i32_ty;
        case TYPE_F64: return lg.double_ty;
        case TYPE_BOOL: return lg.i1_ty;
        case TYPE_STR: return lg.ptr_ty;
        case TYPE_VOID: return lg.void_ty;
        case TYPE_STRUCT:
            if (!ty->name) llv_llvm_unsupported("анонимна структура");
            /* M24: instantiated generic struct */
            if (ty->n_targs > 0) {
                char *cn = llv_llvm_struct_cname(ty);
                LLVMTypeRef r = llv_user_struct_ty(cn);
                free(cn);
                return r;
            }
            return llv_user_struct_ty(ty->name);
        case TYPE_VEC:    return llv_baga_vec_ptr_ty();
        case TYPE_MAP:    return llv_baga_map_ptr_ty(); break;
        case TYPE_ENUM:
            /* L3: sum enum → named tagged struct (като user struct) */
            if (!ty->name) llv_llvm_unsupported("анонимен enum");
            return llv_user_struct_ty(ty->name);
        case TYPE_BYTES:  return llv_baga_bytes_ty();
        case TYPE_FN:     return lg.i64_ty;   /* L5: cell2(code, env) handle */
        case TYPE_ARRAY:  llv_llvm_unsupported("масиви"); break;
        case TYPE_REF:    llv_llvm_unsupported("референции"); break;
        default:          llv_llvm_unsupported("неизвестен тип"); break;
    }
    return NULL; /* unreachable */
}

LLVMTypeRef llv_llvm_type(Node *ty) {
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
            char *cn = llv_llvm_struct_cname(ty->type);
            LLVMTypeRef t = llv_user_struct_ty(cn);
            free(cn);
            return t;
        }
        if (strcmp(ty->type_name, "i64") == 0) return lg.i64_ty;
        if (strcmp(ty->type_name, "i32") == 0) return lg.i32_ty;
        if (strcmp(ty->type_name, "f64") == 0) return lg.double_ty;
        if (strcmp(ty->type_name, "bool") == 0) return lg.i1_ty;
        if (strcmp(ty->type_name, "str") == 0) return lg.ptr_ty;
        if (strcmp(ty->type_name, "void") == 0) return lg.void_ty;
        if (strcmp(ty->type_name, "bytes") == 0) return llv_baga_bytes_ty();
        if (strcmp(ty->type_name, "Vec") == 0) return llv_baga_vec_ptr_ty();
        if (strcmp(ty->type_name, "Map") == 0) return llv_baga_map_ptr_ty();
        /* LP-final обединяване: enum без payload-и е i64-базиран */
        for (int i = 0; lg.program && i < lg.program->items.len; i++) {
            Node *it = lg.program->items.data[i];
            if (it->kind == NODE_ENUM && it->enum_name &&
                strcmp(it->enum_name, ty->type_name) == 0 &&
                !llv_is_sum_enum_item(it))
                return lg.i64_ty;
        }
        /* user struct или L3 sum enum — и двата са named struct типове */
        return llv_user_struct_ty(ty->type_name);
    }
    if (ty->kind == NODE_TYPE_EFFECT) return llv_llvm_type(ty->inner_type);
    if (ty->kind == NODE_TYPE_FN) return lg.i64_ty;   /* L5: cell2 handle */
    if (ty->kind == NODE_TYPE_REF) llv_llvm_unsupported("референции (&T)");
    if (ty->kind == NODE_TYPE_ARRAY) return llv_baga_vec_ptr_ty();   /* [T] == Vec<T> */
    llv_llvm_unsupported_node(ty);
    return NULL; /* unreachable */
}

/* ---- Name mangling ---- */

char *llv_llvm_mangle(const char *name) {
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

char *llv_tmp_name(void) {
    char *buf = malloc(16);
    sprintf(buf, "t%d", lg.tmp_counter++);
    return buf;
}

/* ---- Symbol table: име → alloca ---- */

LLVMSymtab lg_st;

void llv_st_reset(void) { lg_st.count = 0; lg_st.depth = 0; }
void llv_st_push(void) { lg_st.scope_marks[lg_st.depth++] = lg_st.count; }
void llv_st_pop(void)  { lg_st.count = lg_st.scope_marks[--lg_st.depth]; }
void llv_st_define(const char *name, LLVMValueRef alloca) {
    if (lg_st.count >= LLVM_MAX_VARS) llv_llvm_unsupported("твърде много локални променливи");
    lg_st.vars[lg_st.count].name = (char *)name;
    lg_st.vars[lg_st.count].alloca = alloca;
    lg_st.count++;
}
LLVMValueRef llv_st_lookup(const char *name) {
    for (int i = lg_st.count - 1; i >= 0; i--)
        if (strcmp(lg_st.vars[i].name, name) == 0) return lg_st.vars[i].alloca;
    return NULL;
}

/* ---- Type coercion (огледало на C аритметичните преобразувания) ---- */

LLVMValueRef llv_coerce(LLVMValueRef v, LLVMTypeRef target) {
    LLVMTypeRef t = LLVMTypeOf(v);
    if (t == target) return v;
    LLVMTypeKind tk = LLVMGetTypeKind(t);
    LLVMTypeKind gk = LLVMGetTypeKind(target);
    char *name = llv_tmp_name();
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
    if (!r) llv_llvm_unsupported("преобразуване между типовете");
    return r;
}

/* C truthiness: i1 → както е; цяло число → != 0 */
LLVMValueRef llv_to_bool(LLVMValueRef v) {
    LLVMTypeRef t = LLVMTypeOf(v);
    if (t == lg.i1_ty) return v;
    if (LLVMGetTypeKind(t) == LLVMIntegerTypeKind) {
        char *name = llv_tmp_name();
        LLVMValueRef r = LLVMBuildICmp(lg.builder, LLVMIntNE, v,
                                       LLVMConstInt(t, 0, 0), name);
        free(name);
        return r;
    }
    llv_llvm_unsupported("булев контекст върху не-числена стойност");
    return NULL; /* unreachable */
}

/* ---- Runtime helpers: baga_* като lazy IR функции ----
 *
 * Огледало на C preamble-а в codegen_c.c: всяка helper функция се
 * генерира най-много веднъж, при първа употреба (llv_baga_rt), като
 * текущият insert block на builder-а се запазва и възстановява. */

LLVMValueRef llv_baga_rt(const char *name);
int llv_extern_ret_is_str_llvm(Node *ef);

LLVMValueRef llv_rt_libc(const char *name, LLVMTypeRef ret,
                            LLVMTypeRef *params, int nparams) {
    LLVMValueRef fn = LLVMGetNamedFunction(lg.mod, name);
    if (fn) return fn;
    return LLVMAddFunction(lg.mod, name,
        LLVMFunctionType(ret, params, (unsigned)nparams, 0));
}

LLVMValueRef llv_rt_malloc(void) {
    LLVMTypeRef p[] = { lg.i64_ty };
    return llv_rt_libc("malloc", lg.ptr_ty, p, 1);
}
LLVMValueRef llv_rt_realloc(void) {
    LLVMTypeRef p[] = { lg.ptr_ty, lg.i64_ty };
    return llv_rt_libc("realloc", lg.ptr_ty, p, 2);
}
LLVMValueRef llv_rt_free(void) {
    LLVMTypeRef p[] = { lg.ptr_ty };
    return llv_rt_libc("free", lg.void_ty, p, 1);
}
LLVMValueRef llv_rt_memcpy(void) {
    LLVMTypeRef p[] = { lg.ptr_ty, lg.ptr_ty, lg.i64_ty };
    return llv_rt_libc("memcpy", lg.ptr_ty, p, 3);
}
LLVMValueRef llv_rt_strlen(void) {
    LLVMTypeRef p[] = { lg.ptr_ty };
    return llv_rt_libc("strlen", lg.i64_ty, p, 1);
}
LLVMValueRef llv_rt_strcmp(void) {
    LLVMTypeRef p[] = { lg.ptr_ty, lg.ptr_ty };
    return llv_rt_libc("strcmp", lg.i32_ty, p, 2);
}

/* повикване на вече декларирана функция в helper тяло */
LLVMValueRef llv_h_call(LLVMValueRef fn, LLVMValueRef *args, int nargs,
                           const char *name) {
    return LLVMBuildCall2(lg.builder, LLVMGlobalGetValueType(fn),
                          fn, args, (unsigned)nargs, name);
}

/* entry block + позициониране на builder-а за ново helper тяло */
void llv_h_begin(LLVMValueRef fn) {
    LLVMBasicBlockRef entry = LLVMAppendBasicBlockInContext(lg.ctx, fn, "entry");
    LLVMPositionBuilderAtEnd(lg.builder, entry);
}

#endif /* BAGA_LLVM */
