#include "baga.h"

/* ============================================================
 *  C code generator (Phase 1: C transpiler)
 *
 *  baga → C → gcc → binary
 * ============================================================ */

/* ---- name mangling ----
 * Cyrillic (and other non-ASCII) identifiers become valid C
 * by encoding each non-ASCII byte as _XX hex.
 * All user names get a b_ prefix to avoid C keyword clashes.
 */

static void emit_mangled(FILE *f, const char *name) {
    fputc('b', f); fputc('_', f);
    for (const unsigned char *p = (const unsigned char *)name; *p; p++) {
        if ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
            (*p >= '0' && *p <= '9') || *p == '_') {
            fputc(*p, f);
        } else {
            fprintf(f, "_%02x", *p);
        }
    }
}

static char *mangle_name(const char *name) {
    /* worst case: every byte → _XX = 3 chars, plus b_ prefix + nul */
    size_t len = strlen(name);
    char *buf = malloc(2 + len * 3 + 1);
    if (!buf) { fprintf(stderr, "baga: out of memory\n"); exit(1); }
    char *o = buf;
    *o++ = 'b'; *o++ = '_';
    for (const unsigned char *p = (const unsigned char *)name; *p; p++) {
        if ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
            (*p >= '0' && *p <= '9') || *p == '_') {
            *o++ = (char)*p;
        } else {
            o += sprintf(o, "_%02x", *p);
        }
    }
    *o = '\0';
    return buf;
}

/* ---- extern fn (FFI) ---- */

/* Find an `extern fn` declaration by baga name, or NULL. */
static Node *find_extern_fn(Codegen *cg, const char *name) {
    if (!cg->program) return NULL;
    for (int i = 0; i < cg->program->items.len; i++) {
        Node *it = cg->program->items.data[i];
        if (it->kind == NODE_FN && it->is_extern &&
            strcmp(it->fn_name, name) == 0)
            return it;
    }
    return NULL;
}

/* Does this extern fn return str? (Effects on the return type are unwrapped.) */
static int extern_ret_is_str(Node *ef) {
    Node *t = ef->ret_type;
    while (t && t->kind == NODE_TYPE_EFFECT) t = t->inner_type;
    return t && t->kind == NODE_TYPE && strcmp(t->type_name, "str") == 0;
}

/* C type for an extern prototype. str params are `const char *`, but a str
 * return is `char *` so the prototype is compatible with libc declarations
 * from the headers we emit (e.g. `char *getenv(const char *)` in stdlib.h). */
static void emit_extern_type(FILE *f, Node *ty, int is_ret) {
    while (ty && ty->kind == NODE_TYPE_EFFECT) ty = ty->inner_type;
    if (!ty) { fprintf(f, "void"); return; }
    if (ty->kind == NODE_TYPE) {
        if (strcmp(ty->type_name, "i64") == 0)       fprintf(f, "int64_t");
        else if (strcmp(ty->type_name, "f64") == 0)  fprintf(f, "double");
        else if (strcmp(ty->type_name, "str") == 0)  fprintf(f, is_ret ? "char *" : "const char *");
        else if (strcmp(ty->type_name, "void") == 0) fprintf(f, "void");
        else                                         fprintf(f, "int64_t");
    } else {
        fprintf(f, "int64_t");
    }
}

/* ---- indentation ---- */

static void emit_indent(Codegen *cg) {
    for (int i = 0; i < cg->indent; i++)
        fprintf(cg->out, "    ");
}

/* ---- type mapping ---- */

static void emit_type(Codegen *cg, Node *ty) {
    FILE *f = cg->out;
    if (!ty) { fprintf(f, "void"); return; }

    switch (ty->kind) {
        case NODE_TYPE:
            if (strcmp(ty->type_name, "i32") == 0)   fprintf(f, "int32_t");
            else if (strcmp(ty->type_name, "i64") == 0) fprintf(f, "int64_t");
            else if (strcmp(ty->type_name, "f64") == 0) fprintf(f, "double");
            else if (strcmp(ty->type_name, "bool") == 0) fprintf(f, "int");
            else if (strcmp(ty->type_name, "str") == 0)  fprintf(f, "const char *");
            else if (strcmp(ty->type_name, "bytes") == 0) fprintf(f, "baga_bytes");
            else if (strcmp(ty->type_name, "void") == 0) fprintf(f, "void");
            else if (strcmp(ty->type_name, "Vec") == 0)  fprintf(f, "baga_Vec *");
            else if (strcmp(ty->type_name, "Map") == 0)  fprintf(f, "baga_Map *");
            else {
                /* struct or unknown → use mangled name */
                emit_mangled(f, ty->type_name);
            }
            break;
        case NODE_TYPE_REF:
            emit_type(cg, ty->inner_type);
            fprintf(f, " *");
            break;
        case NODE_TYPE_ARRAY:
            /* [T] == Vec<T>: the growable baga_Vec, not a raw pointer */
            fprintf(f, "baga_Vec *");
            break;
        case NODE_TYPE_EFFECT:
            /* effects don't affect C codegen */
            emit_type(cg, ty->inner_type);
            break;
        default:
            fprintf(f, "int64_t");
            break;
    }
}

/* C spelling от проверен (checker) Type — за fn-стойностните сигнатури (L5) */
static void emit_ctype(Codegen *cg, Type *t) {
    FILE *f = cg->out;
    if (!t) { fprintf(f, "void"); return; }
    switch (t->kind) {
        case TYPE_I32:   fprintf(f, "int32_t"); break;
        case TYPE_I64:   fprintf(f, "int64_t"); break;
        case TYPE_F64:   fprintf(f, "double"); break;
        case TYPE_BOOL:  fprintf(f, "int"); break;
        case TYPE_STR:   fprintf(f, "const char *"); break;
        case TYPE_BYTES: fprintf(f, "baga_bytes"); break;
        case TYPE_VOID:  fprintf(f, "void"); break;
        case TYPE_VEC:   fprintf(f, "baga_Vec *"); break;
        case TYPE_MAP:   fprintf(f, "baga_Map *"); break;
        case TYPE_STRUCT: emit_mangled(f, t->name ? t->name : "anon"); break;
        case TYPE_ENUM:  emit_mangled(f, t->name ? t->name : "anon"); break;
        default:         fprintf(f, "int64_t"); break;   /* TYPE_FN → handle */
    }
}

/* ---- C string escaping ---- */

static void emit_c_string(FILE *f, const char *s) {
    fputc('"', f);
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        switch (*p) {
            case '"':  fprintf(f, "\\\""); break;
            case '\\': fprintf(f, "\\\\"); break;
            case '\n': fprintf(f, "\\n"); break;
            case '\t': fprintf(f, "\\t"); break;
            case '\r': fprintf(f, "\\r"); break;
            case '\0': fprintf(f, "\\0"); break;
            default:
                if (*p >= 0x20 && *p < 0x7f)
                    fputc(*p, f);
                else
                    /* 3-цифрен осмичен escape: \xHH е лаком и би погълнал
                     * следваща ASCII hex цифра ("ел0" → \xbb0); \ooo е
                     * ограничен до точно 3 цифри по стандарт */
                    fprintf(f, "\\%03o", *p);
                break;
        }
    }
    fputc('"', f);
}

/* ---- expression emission ---- */

static void emit_expr(Codegen *cg, Node *n);
static void emit_stmt(Codegen *cg, Node *n);
static void emit_zero_struct(Codegen *cg, const char *name);

static const char *binop_c(BinOp op) {
    switch (op) {
        case OP_ADD: return "+";   case OP_SUB: return "-";
        case OP_MUL: return "*";   case OP_DIV: return "/";
        case OP_MOD: return "%";
        case OP_EQ:  return "==";  case OP_NEQ: return "!=";
        case OP_LT:  return "<";   case OP_GT:  return ">";
        case OP_LE:  return "<=";  case OP_GE:  return ">=";
        case OP_AND: return "&&";  case OP_OR:  return "||";
        case OP_BIT_AND: return "&"; case OP_BIT_OR: return "|";
        case OP_BIT_XOR: return "^";
        case OP_LSHIFT: return "<<"; case OP_RSHIFT: return ">>";
    }
    return "+";
}

static int is_print_call(Node *n) {
    if (n->kind != NODE_CALL) return 0;
    Node *callee = n->callee;
    if (callee->kind != NODE_IDENT) return 0;
    return strcmp(callee->name, "print") == 0 ||
           strcmp(callee->name, "println") == 0 ||
           strcmp(callee->name, "write") == 0;
}

static void emit_print(Codegen *cg, Node *n) {
    FILE *f = cg->out;
    int is_write = (n->callee->kind == NODE_IDENT && strcmp(n->callee->name, "write") == 0);

    if (n->args.len == 0) {
        emit_indent(cg);
        fprintf(f, "printf(\"\\n\");\n");
        return;
    }

    /* print each argument, dispatching on inferred type */
    for (int i = 0; i < n->args.len; i++) {
        Node *arg = n->args.data[i];
        Type *at = arg->type;
        TypeKind ak = at ? at->kind : TYPE_I64;

        emit_indent(cg);

        if (ak == TYPE_STR || arg->kind == NODE_STR_LIT) {
            if (arg->kind == NODE_STR_LIT) {
                if (is_write)
                    fprintf(f, "baga_write(");
                else
                    fprintf(f, "printf(\"%%s\\n\", ");
                emit_c_string(f, arg->str_val);
                fprintf(f, ");\n");
            } else {
                if (is_write)
                    fprintf(f, "baga_write(");
                else
                    fprintf(f, "baga_print_str(");
                emit_expr(cg, arg);
                fprintf(f, ");\n");
            }
        } else if (ak == TYPE_F64) {
            fprintf(f, "baga_print_f64(");
            emit_expr(cg, arg);
            fprintf(f, ");\n");
        } else if (ak == TYPE_BOOL) {
            fprintf(f, "printf(\"%%s\\n\", (");
            emit_expr(cg, arg);
            fprintf(f, ") ? \"true\" : \"false\");\n");
        } else {
            /* default: integer */
            fprintf(f, "baga_print_i64((int64_t)(");
            emit_expr(cg, arg);
            fprintf(f, "));\n");
        }
    }
}

static void emit_expr(Codegen *cg, Node *n) {
    FILE *f = cg->out;
    if (!n) return;

    switch (n->kind) {
        case NODE_INT_LIT:
            fprintf(f, "%lldLL", (long long)n->int_val);
            break;

        case NODE_FLOAT_LIT:
            /* %.17g round-trip-ва IEEE double без загуба на точност */
            fprintf(f, "%.17g", n->float_val);
            break;

        case NODE_STR_LIT:
            emit_c_string(f, n->str_val);
            break;

        case NODE_BYTES_LIT:
            fprintf(f, "baga_bytes_from_hex(");
            emit_c_string(f, n->str_val);
            fprintf(f, ")");
            break;

        case NODE_BOOL_LIT:
            fprintf(f, "%d", n->bool_val);
            break;

        case NODE_IDENT: {
            /* L5: глобална fn като стойност → handle към wrapper-а. Локална
             * fn-typed променлива има type->name == NULL или различно име. */
            if (n->type && n->type->kind == TYPE_FN && n->type->name &&
                strcmp(n->name, n->type->name) == 0) {
                char *m = mangle_name(n->type->name);
                fprintf(f, "(int64_t)baga_cell2((int64_t)(void *)%s__clo, 0)", m);
                free(m);
                break;
            }
            /* check if it's an enum variant */
            int found_variant = 0;
            if (cg->program) {
                for (int i = 0; i < cg->program->items.len && !found_variant; i++) {
                    Node *item = cg->program->items.data[i];
                    if (item->kind != NODE_ENUM) continue;
                    for (int j = 0; j < item->n_variants; j++) {
                        if (strcmp(item->enum_variants[j], n->name) == 0) {
                            int is_sum = 0;
                            for (int k = 0; k < item->n_variants; k++)
                                if (item->enum_payloads && item->enum_payloads[k]) is_sum = 1;
                            char *em = mangle_name(item->enum_name);
                            char *vm = mangle_name(item->enum_variants[j]);
                            if (is_sum)
                                fprintf(f, "(%s){ .tag = %d }", em, j);   /* payload-less variant */
                            else
                                fprintf(f, "%s_%s", em, vm);
                            free(em); free(vm);
                            found_variant = 1;
                            break;
                        }
                    }
                }
            }
            if (!found_variant) {
                char *m = mangle_name(n->name);
                fprintf(f, "%s", m);
                free(m);
            }
            break;
        }

        case NODE_BINARY:
            /* string comparison: == and != use strcmp */
            if ((n->bin_op == OP_EQ || n->bin_op == OP_NEQ) &&
                n->left->type && n->right->type &&
                n->left->type->kind == TYPE_STR && n->right->type->kind == TYPE_STR) {
                if (n->bin_op == OP_EQ)
                    fprintf(f, "(strcmp(");
                else
                    fprintf(f, "(strcmp(");
                emit_expr(cg, n->left);
                fprintf(f, ", ");
                emit_expr(cg, n->right);
                fprintf(f, ") %s 0)", n->bin_op == OP_EQ ? "==" : "!=");
            } else if ((n->bin_op == OP_DIV || n->bin_op == OP_MOD) &&
                       (!n->right->type || n->right->type->kind == TYPE_I64)) {
                /* i64 деление/modulo: guard за нула */
                fprintf(f, "({ int64_t _a = (");
                emit_expr(cg, n->left);
                fprintf(f, "); int64_t _b = (");
                emit_expr(cg, n->right);
                fprintf(f, "); if (_b == 0) baga_div_zero_fail(); _a %s _b; })",
                        n->bin_op == OP_DIV ? "/" : "%");
            } else {
                fprintf(f, "(");
                emit_expr(cg, n->left);
                fprintf(f, " %s ", binop_c(n->bin_op));
                emit_expr(cg, n->right);
                fprintf(f, ")");
            }
            break;

        case NODE_UNARY:
            switch (n->un_op) {
                case UOP_NEG:   fprintf(f, "(-"); emit_expr(cg, n->operand); fprintf(f, ")"); break;
                case UOP_NOT:   fprintf(f, "(!"); emit_expr(cg, n->operand); fprintf(f, ")"); break;
                case UOP_REF:   fprintf(f, "(&"); emit_expr(cg, n->operand); fprintf(f, ")"); break;
                case UOP_DEREF: fprintf(f, "(*"); emit_expr(cg, n->operand); fprintf(f, ")"); break;
            }
            break;

        case NODE_CALL:
            /* L3: конструктор на sum enum → Em__Vm(arg) */
            if (n->callee->kind == NODE_IDENT && cg->program) {
                int emitted = 0;
                for (int i = 0; i < cg->program->items.len && !emitted; i++) {
                    Node *item = cg->program->items.data[i];
                    if (item->kind != NODE_ENUM) continue;
                    for (int j = 0; j < item->n_variants; j++) {
                        if (item->enum_payloads && item->enum_payloads[j] &&
                            strcmp(item->enum_variants[j], n->callee->name) == 0) {
                            char *em = mangle_name(item->enum_name);
                            char *vm = mangle_name(item->enum_variants[j]);
                            fprintf(f, "%s__%s(", em, vm);
                            if (n->args.len > 0) emit_expr(cg, n->args.data[0]);
                            fprintf(f, ")");
                            free(em); free(vm);
                            emitted = 1;
                            break;
                        }
                    }
                }
                if (emitted) break;
            }
            /* extern fn → direct libc call, no mangling, before builtin dispatch */
            if (n->callee->kind == NODE_IDENT) {
                Node *ef = find_extern_fn(cg, n->callee->name);
                if (ef) {
                    /* clock_gettime: system prototype from time.h (via pthread.h)
                     * is (clockid_t, struct timespec *); baga passes (i64, str buffer). */
                    if (strcmp(ef->fn_name, "clock_gettime") == 0 && n->args.len == 2) {
                        fprintf(f, "((int64_t)clock_gettime((int)(");
                        emit_expr(cg, n->args.data[0]);
                        fprintf(f, "), (struct timespec *)(void *)(");
                        emit_expr(cg, n->args.data[1]);
                        fprintf(f, ")))");
                        break;
                    }
                    int str_ret = extern_ret_is_str(ef);
                    if (str_ret) fprintf(f, "({ const char *_er = %s(", ef->fn_name);
                    else         fprintf(f, "%s(", ef->fn_name);
                    for (int i = 0; i < n->args.len; i++) {
                        if (i > 0) fprintf(f, ", ");
                        emit_expr(cg, n->args.data[i]);
                    }
                    if (str_ret) fprintf(f, "); _er ? _er : \"\"; })");
                    else         fprintf(f, ")");
                    break;
                }
            }
            if (is_print_call(n)) {
                /* handled at statement level */
                emit_print(cg, n);
                return;
            }
            /* string/io builtins → C helpers */
            if (n->callee->kind == NODE_IDENT) {
                const char *bn = n->callee->name;
                /* MEM-1: drop(x) builtin. Огледало на checker guard-а: не се
                 * задейства при user fn 'drop' (скан на program) нито при fn
                 * стойност (callee->type == TYPE_FN); extern е хванат по-горе. */
                if (strcmp(bn, "drop") == 0 && n->args.len == 1 &&
                    !(n->callee->type && n->callee->type->kind == TYPE_FN)) {
                    int user_drop = 0;
                    if (cg->program) {
                        for (int i = 0; i < cg->program->items.len; i++) {
                            Node *it = cg->program->items.data[i];
                            if (it->kind == NODE_FN && it->fn_name &&
                                strcmp(it->fn_name, "drop") == 0) { user_drop = 1; break; }
                        }
                    }
                    if (!user_drop) {
                        Type *at = n->args.data[0]->type;
                        if (at && at->kind == TYPE_BYTES) {
                            fprintf(f, "baga_drop_bytes(");
                            emit_expr(cg, n->args.data[0]);
                            fprintf(f, ")");
                        } else if (at && at->kind == TYPE_FN) {
                            fprintf(f, "baga_drop_fn(");
                            emit_expr(cg, n->args.data[0]);
                            fprintf(f, ")");
                        } else if (at && at->kind == TYPE_VEC) {
                            if (at->elem && at->elem->kind == TYPE_STRUCT && at->elem->name) {
                                char *mn = mangle_name(at->elem->name);
                                fprintf(f, "baga_drop_vec(");
                                emit_expr(cg, n->args.data[0]);
                                fprintf(f, ", 2, (int64_t)sizeof(%s))", mn);
                                free(mn);
                            } else if (at->elem && at->elem->kind == TYPE_BYTES) {
                                fprintf(f, "baga_drop_vec(");
                                emit_expr(cg, n->args.data[0]);
                                fprintf(f, ", 2, (int64_t)sizeof(baga_bytes))");
                            } else if (at->elem && at->elem->kind == TYPE_STR) {
                                fprintf(f, "baga_drop_vec(");
                                emit_expr(cg, n->args.data[0]);
                                fprintf(f, ", 1, 0)");
                            } else {
                                fprintf(f, "baga_drop_vec(");
                                emit_expr(cg, n->args.data[0]);
                                fprintf(f, ", 0, 0)");
                            }
                        } else if (at && at->kind == TYPE_MAP) {
                            if (at->elem && at->elem->kind == TYPE_STRUCT && at->elem->name) {
                                char *mn = mangle_name(at->elem->name);
                                fprintf(f, "baga_drop_map(");
                                emit_expr(cg, n->args.data[0]);
                                fprintf(f, ", 1, (int64_t)sizeof(%s))", mn);
                                free(mn);
                            } else {
                                fprintf(f, "baga_drop_map(");
                                emit_expr(cg, n->args.data[0]);
                                fprintf(f, ", 0, 0)");
                            }
                        } else {
                            /* типът е NULL/unknown (checker вече е репортвал) */
                            fprintf(f, "0 /* drop */");
                        }
                        goto call_done;
                    }
                }
                /* типизирани вектори: helper по елементния тип на вектора */
                if (strcmp(bn, "vec_push") == 0 || strcmp(bn, "vec_get") == 0 ||
                    strcmp(bn, "vec_set") == 0) {
                    Type *vt = n->args.len > 0 ? n->args.data[0]->type : NULL;
                    /* struct елементи: box-нато копие през generic box helper-ите
                     * (statement expression дава lvalue на произволен rvalue) */
                    if (vt && vt->kind == TYPE_VEC && vt->elem &&
                        vt->elem->kind == TYPE_STRUCT && vt->elem->name) {
                        char *mn = mangle_name(vt->elem->name);
                        if (strcmp(bn, "vec_get") == 0) {
                            fprintf(f, "(*(%s *)baga_vec_get_box(", mn);
                            for (int i = 0; i < n->args.len; i++) {
                                if (i > 0) fprintf(f, ", ");
                                emit_expr(cg, n->args.data[i]);
                            }
                            fprintf(f, "))");
                        } else if (strcmp(bn, "vec_push") == 0) {
                            fprintf(f, "({ %s _bx = (", mn);
                            emit_expr(cg, n->args.data[1]);
                            fprintf(f, "); baga_vec_push_box(");
                            emit_expr(cg, n->args.data[0]);
                            fprintf(f, ", &_bx, (int64_t)sizeof(%s)); })", mn);
                        } else {
                            fprintf(f, "({ %s _bx = (", mn);
                            emit_expr(cg, n->args.data[2]);
                            fprintf(f, "); baga_vec_set_box(");
                            emit_expr(cg, n->args.data[0]);
                            fprintf(f, ", ");
                            emit_expr(cg, n->args.data[1]);
                            fprintf(f, ", &_bx, (int64_t)sizeof(%s)); })", mn);
                        }
                        free(mn);
                        goto call_done;
                    }
                    const char *suf = "i64";
                    if (vt && vt->kind == TYPE_VEC && vt->elem) {
                        if (vt->elem->kind == TYPE_STR) suf = "str";
                        else if (vt->elem->kind == TYPE_F64) suf = "f64";
                        else if (vt->elem->kind == TYPE_BYTES) suf = "bytes";
                    }
                    fprintf(f, "baga_%s_%s(", bn, suf);
                    for (int i = 0; i < n->args.len; i++) {
                        if (i > 0) fprintf(f, ", ");
                        emit_expr(cg, n->args.data[i]);
                    }
                    fprintf(f, ")");
                    goto call_done;
                }
                if (strcmp(bn, "vec_slice") == 0 || strcmp(bn, "vec_concat") == 0) {
                    Type *vt = n->args.len > 0 ? n->args.data[0]->type : NULL;
                    /* struct елементи: размерът идва от call site-а */
                    if (vt && vt->kind == TYPE_VEC && vt->elem &&
                        vt->elem->kind == TYPE_STRUCT && vt->elem->name) {
                        char *mn = mangle_name(vt->elem->name);
                        fprintf(f, "baga_%s_box(", bn);
                        for (int i = 0; i < n->args.len; i++) {
                            if (i > 0) fprintf(f, ", ");
                            emit_expr(cg, n->args.data[i]);
                        }
                        fprintf(f, ", (int64_t)sizeof(%s))", mn);
                        free(mn);
                        goto call_done;
                    }
                    const char *suf = "i64";
                    if (vt && vt->kind == TYPE_VEC && vt->elem) {
                        if (vt->elem->kind == TYPE_STR) suf = "str";
                        else if (vt->elem->kind == TYPE_F64) suf = "f64";
                        else if (vt->elem->kind == TYPE_BYTES) suf = "bytes";
                    }
                    fprintf(f, "baga_%s_%s(", bn, suf);
                    for (int i = 0; i < n->args.len; i++) {
                        if (i > 0) fprintf(f, ", ");
                        emit_expr(cg, n->args.data[i]);
                    }
                    fprintf(f, ")");
                    goto call_done;
                }
                /* карти: map_set/map_get по ключ+стойност; has/del/keys по ключ
                 * (типовете идват от checker-а; чист Map → str/i64 по подразбиране) */
                if (strcmp(bn, "map_set") == 0 || strcmp(bn, "map_get") == 0) {
                    Type *mt = n->args.len > 0 ? n->args.data[0]->type : NULL;
                    /* struct стойности: box-нато копие през generic box helper-ите */
                    if (mt && mt->kind == TYPE_MAP && mt->elem &&
                        mt->elem->kind == TYPE_STRUCT && mt->elem->name) {
                        const char *ksuf = "str";
                        if (mt->key && mt->key->kind == TYPE_I64) ksuf = "i64";
                        char *mn = mangle_name(mt->elem->name);
                        if (strcmp(bn, "map_set") == 0) {
                            fprintf(f, "({ %s _bx = (", mn);
                            emit_expr(cg, n->args.data[2]);
                            fprintf(f, "); baga_map_set_%s_box(", ksuf);
                            emit_expr(cg, n->args.data[0]);
                            fprintf(f, ", ");
                            emit_expr(cg, n->args.data[1]);
                            fprintf(f, ", &_bx, (int64_t)sizeof(%s)); })", mn);
                        } else {
                            /* липсващ ключ → нулев struct (безопасни полета:
                             * str → "", вложен struct → рекурсивно) */
                            fprintf(f, "({ void *_bp = baga_map_get_%s_box(", ksuf);
                            for (int i = 0; i < n->args.len; i++) {
                                if (i > 0) fprintf(f, ", ");
                                emit_expr(cg, n->args.data[i]);
                            }
                            fprintf(f, "); _bp ? *(%s *)_bp : ", mn);
                            emit_zero_struct(cg, mt->elem->name);
                            fprintf(f, "; })");
                        }
                        free(mn);
                        goto call_done;
                    }
                    const char *ksuf = "str", *vsuf = "i64";
                    if (mt && mt->kind == TYPE_MAP) {
                        if (mt->key && mt->key->kind == TYPE_I64) ksuf = "i64";
                        if (mt->elem) {
                            if (mt->elem->kind == TYPE_STR) vsuf = "str";
                            else if (mt->elem->kind == TYPE_F64) vsuf = "f64";
                            else if (mt->elem->kind == TYPE_BYTES) vsuf = "bytes";
                        }
                    }
                    fprintf(f, "baga_%s_%s_%s(", bn, ksuf, vsuf);
                    for (int i = 0; i < n->args.len; i++) {
                        if (i > 0) fprintf(f, ", ");
                        emit_expr(cg, n->args.data[i]);
                    }
                    fprintf(f, ")");
                    goto call_done;
                }
                if (strcmp(bn, "map_has") == 0 || strcmp(bn, "map_del") == 0 ||
                    strcmp(bn, "map_keys") == 0) {
                    Type *mt = n->args.len > 0 ? n->args.data[0]->type : NULL;
                    const char *ksuf = "str";
                    if (mt && mt->kind == TYPE_MAP && mt->key &&
                        mt->key->kind == TYPE_I64) ksuf = "i64";
                    fprintf(f, "baga_%s_%s(", bn, ksuf);
                    for (int i = 0; i < n->args.len; i++) {
                        if (i > 0) fprintf(f, ", ");
                        emit_expr(cg, n->args.data[i]);
                    }
                    fprintf(f, ")");
                    goto call_done;
                }
                /* go / go_bg / pool_map — first arg is a function identifier */
                if ((strcmp(bn, "go") == 0 || strcmp(bn, "go_bg") == 0) &&
                    n->args.len == 2 && n->args.data[0]->kind == NODE_IDENT) {
                    char *wm = mangle_name(n->args.data[0]->name);
                    fprintf(f, "%s((baga_par_fn)%s, ",
                            strcmp(bn, "go_bg") == 0 ? "baga_go_bg" : "baga_go", wm);
                    free(wm);
                    emit_expr(cg, n->args.data[1]);
                    fprintf(f, ")");
                    goto call_done;
                }
                if (strcmp(bn, "pool_map") == 0 && n->args.len == 3 &&
                    n->args.data[0]->kind == NODE_IDENT) {
                    char *wm = mangle_name(n->args.data[0]->name);
                    fprintf(f, "baga_pool_map((baga_par_fn)%s, ", wm);
                    free(wm);
                    emit_expr(cg, n->args.data[1]);
                    fprintf(f, ", ");
                    emit_expr(cg, n->args.data[2]);
                    fprintf(f, ")");
                    goto call_done;
                }
                struct { const char *baga; const char *c; } bmap[] = {
                    {"len",       "baga_len"},
                    {"char_at",   "baga_char_at"},
                    {"byte_at",   "baga_byte_at"},
                    {"byte_chr",  "baga_byte_chr"},
                    {"substr",    "baga_substr"},
                    {"concat",    "baga_concat"},
                    {"read_file", "baga_read_file"},
                    {"chr",       "baga_chr"},
                    {"ord",       "baga_ord"},
                    {"str_eq",    "baga_str_eq"},
                    {"vec_new",     "baga_vec_new"},
                    {"vec_push_str","baga_vec_push_str"},
                    {"vec_get_str", "baga_vec_get_str"},
                    {"vec_set_str", "baga_vec_set_str"},
                    {"vec_len",     "baga_vec_len"},
                    {"map_new",     "baga_map_new"},
                    {"map_len",     "baga_map_len"},
                    {"arena_new",   "baga_arena_new"},
                    {"arena_alloc", "baga_arena_alloc"},
                    {"arena_reset", "baga_arena_reset"},
                    {"arena_free",  "baga_arena_free"},
                    {"arg_count",   "baga_arg_count"},
                    {"arg",         "baga_arg"},
                    {"exit",        "baga_exit"},
                    {"eprintln",    "baga_eprintln"},
                    {"bytes_len",   "baga_bytes_len"},
                    {"bytes_at",    "baga_bytes_at"},
                    {"bytes_slice", "baga_bytes_slice"},
                    {"bytes_concat","baga_bytes_concat"},
                    {"bytes_new",  "baga_bytes_new"},
                    {"bytes_set",  "baga_bytes_set"},
                    {"bytes_push", "baga_bytes_push"},
                    {"bytes_of_str","baga_bytes_from_str"},
                    {"str_of_bytes","baga_bytes_to_str"},
                    {"hex_encode",  "baga_hex_encode"},
                    {"hex_decode",  "baga_hex_decode"},
                    {"bytes_from_vec","baga_bytes_from_vec"},
                    {"vec_from_bytes","baga_vec_from_bytes"},
                    {"join",        "baga_join"},
                    {"detach",      "baga_detach"},
                    {"chan_new",    "baga_chan_new"},
                    {"chan_send",   "baga_chan_send"},
                    {"chan_recv",   "baga_chan_recv"},
                    {"chan_recv2",  "baga_chan_recv2"},
                    {"chan_try_recv","baga_chan_try_recv"},
                    {"chan_recv_timeout","baga_chan_recv_timeout"},
                    {"chan_select2","baga_chan_select2"},
                    {"chan_select2_wait","baga_chan_select2_wait"},
                    {"chan_select2_timeout","baga_chan_select2_timeout"},
                    {"chan_close",  "baga_chan_close"},
                    {"chan_len",    "baga_chan_len"},
                    {"sleep_ms",    "baga_sleep_ms"},
                    {"mutex_new",   "baga_mutex_new"},
                    {"mutex_lock",  "baga_mutex_lock"},
                    {"mutex_unlock","baga_mutex_unlock"},
                    {"signal_watch","baga_signal_watch"},
                    {"signal_check","baga_signal_check"},
                    {"signal_clear","baga_signal_clear"},
                    {"signal_wait", "baga_signal_wait"},
                    {"signal_raise","baga_signal_raise"},
                    {"cell2",       "baga_cell2"},
                    {"cell2_0",     "baga_cell2_0"},
                    {"cell2_1",     "baga_cell2_1"},
                };
                for (int bi = 0; bi < (int)(sizeof(bmap) / sizeof(bmap[0])); bi++) {
                    if (strcmp(bn, bmap[bi].baga) == 0) {
                        fprintf(f, "%s(", bmap[bi].c);
                        for (int i = 0; i < n->args.len; i++) {
                            if (i > 0) fprintf(f, ", ");
                            emit_expr(cg, n->args.data[i]);
                        }
                        fprintf(f, ")");
                        goto call_done;
                    }
                }
            }
            /* L5: извикване през fn стойност (checker маркер: TYPE_FN без име) */
            if (n->callee->type && n->callee->type->kind == TYPE_FN &&
                !n->callee->type->name) {
                Type *ft2 = n->callee->type;
                fprintf(f, "({ int64_t _h = (int64_t)(");
                emit_expr(cg, n->callee);
                fprintf(f, "); ((");
                emit_ctype(cg, ft2->ret);
                fprintf(f, " (*)(void *");
                for (int i = 0; i < ft2->nparams; i++) {
                    fprintf(f, ", ");
                    emit_ctype(cg, ft2->params[i]);
                }
                fprintf(f, "))(intptr_t)baga_cell2_0(_h))((void *)(intptr_t)baga_cell2_1(_h)");
                for (int i = 0; i < n->args.len; i++) {
                    fprintf(f, ", ");
                    emit_expr(cg, n->args.data[i]);
                }
                fprintf(f, "); })");   /* stmt-expr: последният израз с ';' е стойността */
                goto call_done;
            }
            {
                char *m = NULL;
                if (n->callee->kind == NODE_IDENT) {
                    m = mangle_name(n->callee->name);
                    fprintf(f, "%s(", m);
                } else {
                    emit_expr(cg, n->callee);
                    fprintf(f, "(");
                }
                for (int i = 0; i < n->args.len; i++) {
                    if (i > 0) fprintf(f, ", ");
                    emit_expr(cg, n->args.data[i]);
                }
                fprintf(f, ")");
                free(m);
            }
            call_done:
            break;

        case NODE_IF: {
            /* if as expression → GCC statement expression */
            fprintf(f, "({");
            fprintf(f, "if (");
            emit_expr(cg, n->cond);
            fprintf(f, ") { ");
            /* emit then block inline */
            if (n->then_br && n->then_br->kind == NODE_BLOCK) {
                for (int i = 0; i < n->then_br->stmts.len; i++) {
                    Node *s = n->then_br->stmts.data[i];
                    if (s->kind == NODE_EXPR_STMT) {
                        /* last expr → result */
                        if (i == n->then_br->stmts.len - 1) {
                            emit_expr(cg, s->expr);
                            fprintf(f, "; ");
                        } else {
                            emit_expr(cg, s->expr);
                            fprintf(f, "; ");
                        }
                    }
                }
            }
            fprintf(f, "} ");
            if (n->else_br) {
                fprintf(f, "else { ");
                if (n->else_br->kind == NODE_BLOCK) {
                    for (int i = 0; i < n->else_br->stmts.len; i++) {
                        Node *s = n->else_br->stmts.data[i];
                        if (s->kind == NODE_EXPR_STMT) {
                            emit_expr(cg, s->expr);
                            fprintf(f, "; ");
                        }
                    }
                }
                fprintf(f, "} ");
            }
            fprintf(f, "})");
            break;
        }

        case NODE_INDEX:
            emit_expr(cg, n->obj);
            fprintf(f, "[");
            emit_expr(cg, n->index);
            fprintf(f, "]");
            break;

        case NODE_FIELD: {
            /* L5/L6: модул.функция като стойност → handle към wrapper-а */
            if (n->type && n->type->kind == TYPE_FN && n->type->name) {
                char *m = mangle_name(n->type->name);
                fprintf(f, "(int64_t)baga_cell2((int64_t)(void *)%s__clo, 0)", m);
                free(m);
                break;
            }
            emit_expr(cg, n->field_obj);
            char *fm = mangle_name(n->field_name);
            fprintf(f, ".%s", fm);
            free(fm);
            break;
        }

        case NODE_ASSIGN:
            emit_expr(cg, n->assign_target);
            fprintf(f, " = ");
            emit_expr(cg, n->assign_val);
            break;

        case NODE_RANGE:
            /* Phase 1: ranges only in for loops, handled there */
            fprintf(f, "0");
            break;

        case NODE_STRUCT_LIT: {
            char *sm = mangle_name(n->lit_name);
            fprintf(f, "(%s){ ", sm);
            for (int i = 0; i < n->n_lit_fields; i++) {
                if (i > 0) fprintf(f, ", ");
                char *fm = mangle_name(n->lit_fields[i]);
                fprintf(f, ".%s = ", fm);
                emit_expr(cg, n->lit_values.data[i]);
                free(fm);
            }
            fprintf(f, " }");
            free(sm);
            break;
        }

        case NODE_CATCH: {
            /* Phase 1: effects are compile-time only; emit the expression */
            emit_expr(cg, n->catch_expr);
            break;
        }

        case NODE_TRY:
            /* e? — effects checked at compile time, emit e */
            emit_expr(cg, n->try_expr);
            break;

        case NODE_TO_STR: {
            /* interpolation: convert inner expr to a C string by its type */
            Type *et = n->to_str_expr ? n->to_str_expr->type : NULL;
            TypeKind ek = et ? et->kind : TYPE_STR;
            if (ek == TYPE_STR) {
                emit_expr(cg, n->to_str_expr);
            } else if (ek == TYPE_BOOL) {
                fprintf(f, "((");
                emit_expr(cg, n->to_str_expr);
                fprintf(f, ") ? \"true\" : \"false\")");
            } else {   /* i64 / i32 */
                fprintf(f, "baga_i64_to_str(");
                emit_expr(cg, n->to_str_expr);
                fprintf(f, ")");
            }
            break;
        }

        case NODE_LAMBDA: {
            /* L5: env struct + wrapper се емитват в lambda_out (в изхода —
             * преди телата на функциите); тялото на wrapper-а — в собствен
             * memstream, за да не се преплитат вложени ламбди. Тук остава
             * само statement expression-ът, който строи handle-а. */
            Type *ft = n->type;
            char *lm = mangle_name(n->fn_name ? n->fn_name : "__lam_x");
            char env_name[128];
            snprintf(env_name, sizeof env_name, "b_env%s", lm + 1);  /* lm е "b_..." → b_env_lam... */
            FILE *lf = cg->lambda_out ? cg->lambda_out : f;

            char *bb = NULL;
            size_t bs = 0;
            FILE *mb = open_memstream(&bb, &bs);
            FILE *saved = cg->out;
            cg->out = mb;
            /* env struct */
            fprintf(mb, "typedef struct {\n");
            for (int i = 0; i < n->captures.len; i++) {
                Node *cap = n->captures.data[i];
                fprintf(mb, "    ");
                emit_ctype(cg, cap->type);
                fprintf(mb, " ");
                char *cm = mangle_name(cap->param_name);
                fprintf(mb, "%s;\n", cm);
                free(cm);
            }
            fprintf(mb, "} %s;\n\n", env_name);
            /* wrapper */
            fprintf(mb, "static ");
            emit_ctype(cg, ft && ft->kind == TYPE_FN ? ft->ret : NULL);
            fprintf(mb, " %s(void *_env", lm);
            for (int i = 0; i < n->params.len; i++) {
                Node *p = n->params.data[i];
                fprintf(mb, ", ");
                emit_type(cg, p->param_type);
                fprintf(mb, " ");
                char *pm = mangle_name(p->param_name);
                fprintf(mb, "%s", pm);
                free(pm);
            }
            fprintf(mb, ") {\n");
            if (n->captures.len > 0) {
                fprintf(mb, "    %s *_e = (%s *)_env;\n", env_name, env_name);
                for (int i = 0; i < n->captures.len; i++) {
                    Node *cap = n->captures.data[i];
                    fprintf(mb, "    ");
                    emit_ctype(cg, cap->type);
                    fprintf(mb, " ");
                    char *cm = mangle_name(cap->param_name);
                    fprintf(mb, "%s = _e->%s;\n", cm, cm);
                    free(cm);
                }
            }
            if (n->fn_body) {
                int has_ret = n->ret_type != NULL;
                NodeVec *stmts = &n->fn_body->stmts;
                cg->indent++;
                for (int i = 0; i < stmts->len; i++) {
                    Node *s = stmts->data[i];
                    if (has_ret && i == stmts->len - 1 && s->kind == NODE_EXPR_STMT) {
                        cg->indent--;
                        emit_indent(cg);
                        fprintf(mb, "return ");
                        emit_expr(cg, s->expr);
                        fprintf(mb, ";\n");
                        cg->indent++;
                    } else {
                        emit_stmt(cg, s);
                    }
                }
                cg->indent--;
            }
            fprintf(mb, "}\n\n");
            fclose(mb);
            cg->out = saved;
            fwrite(bb, 1, bs, lf);
            free(bb);

            /* handle: ({ env *e = alloc; e->cap = local; cell2(wrapper, env) }) */
            fprintf(f, "({ %s *_e = baga_alloc(sizeof(%s)); ", env_name, env_name);
            for (int i = 0; i < n->captures.len; i++) {
                char *cm = mangle_name(n->captures.data[i]->param_name);
                fprintf(f, "_e->%s = %s; ", cm, cm);
                free(cm);
            }
            fprintf(f, "(int64_t)baga_cell2((int64_t)(void *)%s, (int64_t)(void *)_e); })", lm);
            free(lm);
            break;
        }

        case NODE_MATCH: {
            int tmp = cg->tmp_counter++;
            int is_enum = n->match_expr->type &&
                          n->match_expr->type->kind == TYPE_ENUM &&
                          n->match_expr->type->name;
            /* match като оператор (void) — без _mr резултатна променлива */
            int is_void = n->type && n->type->kind == TYPE_VOID;
            /* result C type from inferred type */
            char *rm = NULL;
            const char *ctype = "int64_t";
            if (n->type) {
                switch (n->type->kind) {
                    case TYPE_STR:   ctype = "const char *"; break;
                    case TYPE_F64:   ctype = "double"; break;
                    case TYPE_BOOL:  ctype = "int"; break;
                    case TYPE_BYTES: ctype = "baga_bytes"; break;
                    case TYPE_VEC:   ctype = "baga_Vec *"; break;
                    case TYPE_MAP:   ctype = "baga_Map *"; break;
                    case TYPE_STRUCT:
                    case TYPE_ENUM:
                        rm = mangle_name(n->type->name); ctype = rm; break;
                    default:         ctype = "int64_t"; break;
                }
            }
            if (!is_enum) {
                /* GCC statement expression */
                if (is_void) fprintf(f, "({ int64_t _mv%d = ", tmp);
                else fprintf(f, "({ %s _mr%d = 0; int64_t _mv%d = ", ctype, tmp, tmp);
                emit_expr(cg, n->match_expr);
                fprintf(f, "; ");
                for (int i = 0; i < n->match_arms.len; i++) {
                    Node *arm = n->match_arms.data[i];
                    if (arm->arm_pattern) {
                        if (i > 0) fprintf(f, "else ");
                        fprintf(f, "if (_mv%d == ", tmp);
                        emit_expr(cg, arm->arm_pattern);
                        fprintf(f, ") { ");
                    } else {
                        /* wildcard → else */
                        fprintf(f, "else { ");
                    }
                    /* emit arm body */
                    if (arm->arm_body && arm->arm_body->kind == NODE_BLOCK) {
                        for (int j = 0; j < arm->arm_body->stmts.len; j++) {
                            Node *s = arm->arm_body->stmts.data[j];
                            if (s->kind == NODE_RETURN && s->ret_val) {
                                if (!is_void) fprintf(f, "_mr%d = ", tmp);
                                emit_expr(cg, s->ret_val);
                                fprintf(f, "; ");
                            } else if (s->kind == NODE_EXPR_STMT) {
                                emit_expr(cg, s->expr);
                                fprintf(f, "; ");
                            }
                        }
                    }
                    fprintf(f, "} ");
                }
                if (is_void) fprintf(f, "})");
                else fprintf(f, "_mr%d; })", tmp);
                free(rm);
                break;
            }
            /* L3: match върху sum enum — сравнение по .tag, binding от .u.v_Var */
            Node *ed = NULL;
            for (int i = 0; i < cg->program->items.len; i++) {
                Node *item = cg->program->items.data[i];
                if (item->kind == NODE_ENUM &&
                    strcmp(item->enum_name, n->match_expr->type->name) == 0)
                { ed = item; break; }
            }
            fprintf(f, "({ ");
            if (!is_void) fprintf(f, "%s _mr%d = {0}; ", ctype, tmp);
            {
                char *sm = mangle_name(n->match_expr->type->name);
                fprintf(f, "%s _mv%d = ", sm, tmp);
                free(sm);
            }
            emit_expr(cg, n->match_expr);
            fprintf(f, "; ");
            for (int i = 0; i < n->match_arms.len; i++) {
                Node *arm = n->match_arms.data[i];
                if (arm->arm_pattern) {
                    int vidx = -1;
                    if (ed)
                        for (int j = 0; j < ed->n_variants; j++)
                            if (strcmp(ed->enum_variants[j], arm->arm_pattern->name) == 0)
                            { vidx = j; break; }
                    if (i > 0) fprintf(f, "else ");
                    fprintf(f, "if (_mv%d.tag == %d) { ", tmp, vidx);
                    if (arm->arm_binding && ed && vidx >= 0 &&
                        ed->enum_payloads && ed->enum_payloads[vidx]) {
                        char *bm = mangle_name(arm->arm_binding);
                        char *vm = mangle_name(ed->enum_variants[vidx]);
                        emit_type(cg, ed->enum_payloads[vidx]);
                        fprintf(f, " %s = _mv%d.u.v_%s; ", bm, tmp, vm);
                        free(bm); free(vm);
                    }
                } else {
                    fprintf(f, "else { ");
                }
                if (arm->arm_body && arm->arm_body->kind == NODE_BLOCK) {
                    for (int j = 0; j < arm->arm_body->stmts.len; j++) {
                        Node *s = arm->arm_body->stmts.data[j];
                        if (s->kind == NODE_RETURN && s->ret_val) {
                            if (!is_void) fprintf(f, "_mr%d = ", tmp);
                            emit_expr(cg, s->ret_val);
                            fprintf(f, "; ");
                        } else if (s->kind == NODE_EXPR_STMT) {
                            emit_expr(cg, s->expr);
                            fprintf(f, "; ");
                        }
                    }
                }
                fprintf(f, "} ");
            }
            if (is_void) fprintf(f, "})");
            else fprintf(f, "_mr%d; })", tmp);
            free(rm);
            break;
        }

        default:
            fprintf(f, "0 /* unhandled expr %d */", n->kind);
            break;
    }
}

/* ---- statement emission ---- */

static void emit_stmt(Codegen *cg, Node *n);

static void emit_block(Codegen *cg, Node *block) {
    FILE *f = cg->out;
    fprintf(f, "{\n");
    cg->indent++;
    for (int i = 0; i < block->stmts.len; i++)
        emit_stmt(cg, block->stmts.data[i]);
    cg->indent--;
    emit_indent(cg);
    fprintf(f, "}");
}

static void emit_stmt(Codegen *cg, Node *n) {
    FILE *f = cg->out;
    if (!n) return;

    switch (n->kind) {
        case NODE_LET:
            emit_indent(cg);
            /* use inferred type from checker, fall back to explicit annotation */
            if (n->let_type) {
                emit_type(cg, n->let_type);
            } else if (n->let_init && n->let_init->type) {
                Type *it = n->let_init->type;
                switch (it->kind) {
                    case TYPE_F64:  fprintf(f, "double"); break;
                    case TYPE_STR:  fprintf(f, "const char *"); break;
                    case TYPE_BOOL: fprintf(f, "int"); break;
                    case TYPE_I32:  fprintf(f, "int32_t"); break;
                    case TYPE_STRUCT:
                        if (it->name) { char *sm = mangle_name(it->name); fprintf(f, "%s", sm); free(sm); }
                        else fprintf(f, "int64_t");
                        break;
                    case TYPE_ENUM:
                        if (it->name) { char *em = mangle_name(it->name); fprintf(f, "%s", em); free(em); }
                        else fprintf(f, "int64_t");
                        break;
                    case TYPE_VEC:
                        fprintf(f, "baga_Vec *");
                        break;
                    case TYPE_MAP:
                        fprintf(f, "baga_Map *");
                        break;
                    case TYPE_BYTES:
                        fprintf(f, "baga_bytes");
                        break;
                    default:        fprintf(f, "int64_t"); break;
                }
            } else {
                fprintf(f, "int64_t");
            }
            fprintf(f, " ");
            {
                char *m = mangle_name(n->let_name);
                fprintf(f, "%s", m);
                free(m);
            }
            if (n->let_init) {
                fprintf(f, " = ");
                emit_expr(cg, n->let_init);
            }
            fprintf(f, ";\n");
            break;

        case NODE_RETURN:
            emit_indent(cg);
            if (n->ret_val) {
                fprintf(f, "return ");
                emit_expr(cg, n->ret_val);
                fprintf(f, ";\n");
            } else {
                fprintf(f, "return;\n");
            }
            break;

        case NODE_WHILE:
            emit_indent(cg);
            fprintf(f, "while (");
            emit_expr(cg, n->while_cond);
            fprintf(f, ") ");
            emit_block(cg, n->while_body);
            fprintf(f, "\n");
            break;

        case NODE_FOR: {
            /* for x in lo..hi { } → for (int64_t x = lo; x < hi; x++) */
            emit_indent(cg);
            char *m = mangle_name(n->for_var);
            fprintf(f, "for (int64_t %s = ", m);
            if (n->for_iter && n->for_iter->kind == NODE_RANGE) {
                emit_expr(cg, n->for_iter->range_lo);
                fprintf(f, "; %s < ", m);
                emit_expr(cg, n->for_iter->range_hi);
                fprintf(f, "; %s++) ", m);
            } else {
                fprintf(f, "0; %s < 0; %s++) ", m, m);
            }
            emit_block(cg, n->for_body);
            fprintf(f, "\n");
            free(m);
            break;
        }

        case NODE_IF:
            emit_indent(cg);
            fprintf(f, "if (");
            emit_expr(cg, n->cond);
            fprintf(f, ") ");
            emit_block(cg, n->then_br);
            if (n->else_br) {
                fprintf(f, " else ");
                if (n->else_br->kind == NODE_BLOCK) {
                    /* check if it's a single if (else-if chain) */
                    if (n->else_br->stmts.len == 1 &&
                        n->else_br->stmts.data[0]->kind == NODE_IF) {
                        emit_stmt(cg, n->else_br->stmts.data[0]);
                        return; /* emit_stmt already added newline */
                    }
                    emit_block(cg, n->else_br);
                }
            }
            fprintf(f, "\n");
            break;

        case NODE_EXPR_STMT:
            /* extern fn (incl. one named write/print/println) must bypass the
             * print-builtin dispatch and go through emit_expr's extern path */
            if (is_print_call(n->expr) &&
                !(n->expr->callee->kind == NODE_IDENT &&
                  find_extern_fn(cg, n->expr->callee->name))) {
                emit_print(cg, n->expr);
            } else {
                emit_indent(cg);
                emit_expr(cg, n->expr);
                fprintf(f, ";\n");
            }
            break;

        case NODE_BREAK:
            emit_indent(cg);
            fprintf(f, "break;\n");
            break;

        case NODE_CONTINUE:
            emit_indent(cg);
            fprintf(f, "continue;\n");
            break;

        case NODE_BLOCK:
            emit_indent(cg);
            emit_block(cg, n);
            fprintf(f, "\n");
            break;

        case NODE_INVARIANT:
            /* annotation statement — verifier-only, no code emitted */
            break;

        default:
            emit_indent(cg);
            fprintf(f, "/* unhandled stmt %d */;\n", n->kind);
            break;
    }
}

/* ---- spec ensures ---- */

/* Намира spec с ensures или requires за дадена функция (NULL ако няма). */
static Node *find_ensures_spec(Codegen *cg, const char *fn_name) {
    if (!cg->program) return NULL;
    for (int i = 0; i < cg->program->items.len; i++) {
        Node *it = cg->program->items.data[i];
        if (it->kind == NODE_SPEC && strcmp(it->spec_name, fn_name) == 0 &&
            (it->spec_ensures.len > 0 || it->spec_requires.len > 0))
            return it;
    }
    return NULL;
}

/* ---- --test-specs ---- */

/* true, ако всички input типове на spec-а са i64/bool */
static int spec_inputs_testable(Node *spec) {
    for (int i = 0; i < spec->spec_inputs.len; i++) {
        Node *pt = spec->spec_inputs.data[i]->param_type;
        if (pt->kind != NODE_TYPE || !pt->type_name) return 0;
        if (strcmp(pt->type_name, "i64") != 0 && strcmp(pt->type_name, "bool") != 0)
            return 0;
    }
    return 1;
}

/* emit-ва requires предикат: static int b__req_<mangled>(params) { return (r1) && (r2); } */
static void emit_requires_predicate(Codegen *cg, Node *spec) {
    FILE *f = cg->out;
    fprintf(f, "static int b__req_");
    char *m = mangle_name(spec->spec_name);
    fprintf(f, "%s", m + 2); /* mangle_name дава b_<име>; искаме b__req_<име> */
    free(m);
    fprintf(f, "(");
    if (spec->spec_inputs.len == 0) {
        fprintf(f, "void");
    } else {
        for (int i = 0; i < spec->spec_inputs.len; i++) {
            if (i > 0) fprintf(f, ", ");
            Node *sp = spec->spec_inputs.data[i];
            emit_type(cg, sp->param_type);
            fprintf(f, " ");
            char *pm = mangle_name(sp->param_name);
            fprintf(f, "%s", pm);
            free(pm);
        }
    }
    fprintf(f, ") {\n    return ");
    if (spec->spec_requires.len == 0) {
        fprintf(f, "1");
    }
    for (int j = 0; j < spec->spec_requires.len; j++) {
        if (j > 0) fprintf(f, " && ");
        fprintf(f, "(");
        emit_expr(cg, spec->spec_requires.data[j]->ensure_expr);
        fprintf(f, ")");
    }
    fprintf(f, ";\n}\n\n");
}

/* ---- function emission ---- */

/* v[*] element invariants and sorted(v) are verifier-only annotations — they
 * are not emitted as runtime contract checks. */
static int is_verifier_only_annotation(Node *e) {
    if (!e) return 0;
    if (e->kind == NODE_ELEM_REF) return 1;
    if (e->kind == NODE_CALL && e->callee && e->callee->kind == NODE_IDENT &&
        strcmp(e->callee->name, "sorted") == 0) return 1;
    if (e->kind == NODE_BINARY) return is_verifier_only_annotation(e->left) || is_verifier_only_annotation(e->right);
    if (e->kind == NODE_UNARY) return is_verifier_only_annotation(e->operand);
    return 0;
}

/* L5: closure wrapper за потребителска функция — адресът му се взима от fn
 * стойности; вика публичното име (с евент. spec проверки). Емитва се в
 * lambda_out, който в изхода е преди телата на функциите. */
static void emit_clo_wrapper(Codegen *cg, Node *fn) {
    if (fn->is_extern) return;
    FILE *lf = cg->lambda_out ? cg->lambda_out : cg->out;
    char *wm = mangle_name(fn->fn_name);
    FILE *saved = cg->out;
    cg->out = lf;
    fprintf(lf, "static __attribute__((unused)) ");
    if (fn->ret_type) emit_type(cg, fn->ret_type); else fprintf(lf, "void");
    fprintf(lf, " %s__clo(void *_env", wm);
    for (int i = 0; i < fn->params.len; i++) {
        Node *p = fn->params.data[i];
        fprintf(lf, ", ");
        emit_type(cg, p->param_type);
        fprintf(lf, " ");
        char *pm = mangle_name(p->param_name);
        fprintf(lf, "%s", pm);
        free(pm);
    }
    fprintf(lf, ") {\n    (void)_env;\n    ");
    if (fn->ret_type) fprintf(lf, "return ");
    fprintf(lf, "%s(", wm);
    for (int i = 0; i < fn->params.len; i++) {
        if (i > 0) fprintf(lf, ", ");
        char *pm = mangle_name(fn->params.data[i]->param_name);
        fprintf(lf, "%s", pm);
        free(pm);
    }
    fprintf(lf, ");\n}\n\n");
    free(wm);
    cg->out = saved;
}

static void emit_fn(Codegen *cg, Node *fn) {
    FILE *f = cg->out;

    Node *ensures_spec = (fn->fn_body)
                       ? find_ensures_spec(cg, fn->fn_name) : NULL;
    char impl_name_buf[512];
    if (ensures_spec) {
        /* оригиналното тяло става static impl функция */
        snprintf(impl_name_buf, sizeof impl_name_buf, "__impl_%s", fn->fn_name);
        fprintf(f, "static ");
    }

    /* return type */
    if (fn->ret_type) {
        emit_type(cg, fn->ret_type);
    } else {
        fprintf(f, "void");
    }
    fprintf(f, " ");

    /* name */
    char *m = mangle_name(ensures_spec ? impl_name_buf : fn->fn_name);
    fprintf(f, "%s", m);
    free(m);

    /* params */
    fprintf(f, "(");
    if (fn->params.len == 0) {
        fprintf(f, "void");
    } else {
        for (int i = 0; i < fn->params.len; i++) {
            if (i > 0) fprintf(f, ", ");
            Node *p = fn->params.data[i];
            emit_type(cg, p->param_type);
            fprintf(f, " ");
            char *pm = mangle_name(p->param_name);
            fprintf(f, "%s", pm);
            free(pm);
        }
    }
    fprintf(f, ") ");

    /* body */
    if (fn->fn_body) {
        int has_ret = fn->ret_type != NULL;
        fprintf(f, "{\n");
        cg->indent++;
        NodeVec *stmts = &fn->fn_body->stmts;
        for (int i = 0; i < stmts->len; i++) {
            Node *s = stmts->data[i];
            /* implicit return: last expr stmt in non-void fn */
            if (has_ret && i == stmts->len - 1 && s->kind == NODE_EXPR_STMT) {
                emit_indent(cg);
                fprintf(f, "return ");
                emit_expr(cg, s->expr);
                fprintf(f, ";\n");
            } else {
                emit_stmt(cg, s);
            }
        }
        cg->indent--;
        emit_indent(cg);
        fprintf(f, "}");
    } else {
        fprintf(f, "{}");
    }
    fprintf(f, "\n\n");

    if (!ensures_spec) {
        /* L5: closure wrapper — fn стойностите вземат адреса му; в
         * lambda_out (преди телата на функциите в изхода) */
        emit_clo_wrapper(cg, fn);
        return;
    }

    /* wrapper: публичното име, проверява requires преди и ensures след повикването */
    if (fn->ret_type) {
        emit_type(cg, fn->ret_type);
    } else {
        fprintf(f, "void");
    }
    fprintf(f, " ");
    char *wm = mangle_name(fn->fn_name);
    fprintf(f, "%s", wm);
    free(wm);
    fprintf(f, "(");
    if (ensures_spec->spec_inputs.len == 0) {
        fprintf(f, "void");
    } else {
        for (int i = 0; i < ensures_spec->spec_inputs.len; i++) {
            if (i > 0) fprintf(f, ", ");
            Node *sp = ensures_spec->spec_inputs.data[i];
            emit_type(cg, sp->param_type);
            fprintf(f, " ");
            char *pm = mangle_name(sp->param_name);
            fprintf(f, "%s", pm);
            free(pm);
        }
    }
    fprintf(f, ") {\n");
    cg->indent++;

    /* провери предусловията преди повикването */
    for (int j = 0; j < ensures_spec->spec_requires.len; j++) {
        Node *rq = ensures_spec->spec_requires.data[j];
        if (is_verifier_only_annotation(rq->ensure_expr)) continue;   /* v[*] / sorted(v) */
        emit_indent(cg);
        fprintf(f, "if (!(");
        emit_expr(cg, rq->ensure_expr);
        fprintf(f, ")) baga_spec_fail(");
        emit_c_string(f, ensures_spec->spec_name);
        fprintf(f, ", \"requires\", %d, ", j + 1);
        emit_c_string(f, rq->ensure_text);
        fprintf(f, ");\n");
    }

    if (fn->ret_type) {
        /* повикай impl и запази резултата като b_output */
        emit_indent(cg);
        emit_type(cg, fn->ret_type);
        fprintf(f, " b_output = ");
        char *im = mangle_name(impl_name_buf);
        fprintf(f, "%s(", im);
        free(im);
        for (int i = 0; i < ensures_spec->spec_inputs.len; i++) {
            if (i > 0) fprintf(f, ", ");
            char *pm = mangle_name(ensures_spec->spec_inputs.data[i]->param_name);
            fprintf(f, "%s", pm);
            free(pm);
        }
        fprintf(f, ");\n");

        /* провери всяка ensures гаранция */
        for (int j = 0; j < ensures_spec->spec_ensures.len; j++) {
            Node *en = ensures_spec->spec_ensures.data[j];
            if (is_verifier_only_annotation(en->ensure_expr)) continue;   /* v[*] / sorted(v) */
            emit_indent(cg);
            fprintf(f, "if (!(");
            emit_expr(cg, en->ensure_expr);
            fprintf(f, ")) baga_spec_fail(");
            emit_c_string(f, ensures_spec->spec_name);
            fprintf(f, ", \"ensures\", %d, ", j + 1);
            emit_c_string(f, en->ensure_text);
            fprintf(f, ");\n");
        }

        emit_indent(cg);
        fprintf(f, "return b_output;\n");
    } else {
        /* void функция: само повикай impl след предусловията */
        emit_indent(cg);
        char *im = mangle_name(impl_name_buf);
        fprintf(f, "%s(", im);
        free(im);
        for (int i = 0; i < ensures_spec->spec_inputs.len; i++) {
            if (i > 0) fprintf(f, ", ");
            char *pm = mangle_name(ensures_spec->spec_inputs.data[i]->param_name);
            fprintf(f, "%s", pm);
            free(pm);
        }
        fprintf(f, ");\n");
        emit_indent(cg);
        fprintf(f, "return;\n");
    }
    cg->indent--;
    emit_indent(cg);
    fprintf(f, "}\n\n");

    /* L5: closure wrapper и за spec-обвитите функции (викa публичното име) */
    emit_clo_wrapper(cg, fn);
}

/* ---- struct emission ---- */

static void emit_struct(Codegen *cg, Node *s) {
    FILE *f = cg->out;
    char *m = mangle_name(s->struct_name);
    fprintf(f, "typedef struct {\n");
    for (int i = 0; i < s->fields.len; i++) {
        Node *fld = s->fields.data[i];
        fprintf(f, "    ");
        emit_type(cg, fld->fld_type);
        fprintf(f, " ");
        char *fm = mangle_name(fld->fld_name);
        fprintf(f, "%s", fm);
        free(fm);
        fprintf(f, ";\n");
    }
    fprintf(f, "} %s;\n\n", m);
    free(m);
}

static Node *find_struct_decl(Codegen *cg, const char *name) {
    if (!cg->program) return NULL;
    for (int i = 0; i < cg->program->items.len; i++) {
        Node *it = cg->program->items.data[i];
        if (it->kind == NODE_STRUCT && it->struct_name &&
            strcmp(it->struct_name, name) == 0)
            return it;
    }
    return NULL;
}

/* нулев struct литерал с нули по полета: str → "" (не NULL — безопасно за
 * печат/concat), bytes → (baga_bytes){0}, вложен struct → рекурсивно;
 * Vec/Map/числа → 0 (vec_len/map_len търпят NULL). Нужен за map_get на
 * липсващ ключ при Map<K, struct>. */
static void emit_zero_struct(Codegen *cg, const char *name) {
    FILE *f = cg->out;
    char *m = mangle_name(name);
    Node *decl = find_struct_decl(cg, name);
    if (!decl) {
        fprintf(f, "(%s){0}", m);
        free(m);
        return;
    }
    fprintf(f, "(%s){", m);
    free(m);
    for (int i = 0; i < decl->fields.len; i++) {
        Node *fld = decl->fields.data[i];
        if (i > 0) fprintf(f, ", ");
        char *fm = mangle_name(fld->fld_name);
        fprintf(f, ".%s = ", fm);
        free(fm);
        Node *ft = fld->fld_type;
        if (ft && ft->kind == NODE_TYPE && ft->type_name) {
            if (strcmp(ft->type_name, "str") == 0) {
                fprintf(f, "\"\"");
                continue;
            }
            if (strcmp(ft->type_name, "bytes") == 0) {
                fprintf(f, "(baga_bytes){0}");
                continue;
            }
            if (strcmp(ft->type_name, "i64") != 0 &&
                strcmp(ft->type_name, "i32") != 0 &&
                strcmp(ft->type_name, "f64") != 0 &&
                strcmp(ft->type_name, "bool") != 0 &&
                strcmp(ft->type_name, "Vec") != 0 &&
                strcmp(ft->type_name, "Map") != 0 &&
                ft->kind == NODE_TYPE) {
                /* не-примитив, не-Vec/Map → вложен struct */
                emit_zero_struct(cg, ft->type_name);
                continue;
            }
        }
        fprintf(f, "0");
    }
    fprintf(f, "}");
}

/* ---- forward declarations ---- */

static void emit_forward_decls(Codegen *cg, Node *program) {
    FILE *f = cg->out;
    for (int i = 0; i < program->items.len; i++) {
        Node *item = program->items.data[i];
        if (item->kind != NODE_FN) continue;

        if (item->is_extern) {
            /* Skip prototypes that clash with system headers pulled in by
             * #include <pthread.h> (e.g. time.h declares clock_gettime).
             * The call site still links against libc; types are pointer-compatible. */
            if (strcmp(item->fn_name, "clock_gettime") == 0)
                continue;
            /* extern fn: prototype with the raw C name and C ABI types */
            emit_extern_type(f, item->ret_type, 1);
            fprintf(f, " %s(", item->fn_name);
            if (item->params.len == 0) {
                fprintf(f, "void");
            } else {
                for (int j = 0; j < item->params.len; j++) {
                    if (j > 0) fprintf(f, ", ");
                    emit_extern_type(f, item->params.data[j]->param_type, 0);
                }
            }
            fprintf(f, ");\n");
            continue;
        }

        if (item->ret_type) emit_type(cg, item->ret_type);
        else fprintf(f, "void");
        fprintf(f, " ");
        char *m = mangle_name(item->fn_name);
        fprintf(f, "%s", m);
        free(m);
        fprintf(f, "(");
        if (item->params.len == 0) {
            fprintf(f, "void");
        } else {
            for (int j = 0; j < item->params.len; j++) {
                if (j > 0) fprintf(f, ", ");
                Node *p = item->params.data[j];
                emit_type(cg, p->param_type);
                fprintf(f, " ");
                char *pm = mangle_name(p->param_name);
                fprintf(f, "%s", pm);
                free(pm);
            }
        }
        fprintf(f, ");\n");
    }
    fprintf(f, "\n");
}

/* ============================================================
 *  Public API
 * ============================================================ */

#define BAGA_TEST_COUNT 100
#define BAGA_TEST_TRIES 1000000 /* горна граница опити за валиден вход на тест */

static void emit_test_driver(Codegen *cg, Node *program) {
    FILE *f = cg->out;

    /* детерминистичен PRNG (xorshift64) */
    fprintf(f, "static uint64_t baga_seed = 0x243F6A8885A308D3ULL;\n");
    fprintf(f, "static int64_t baga_rand_i64(int64_t lo, int64_t hi) {\n");
    fprintf(f, "    baga_seed ^= baga_seed << 13; baga_seed ^= baga_seed >> 7; baga_seed ^= baga_seed << 17;\n");
    fprintf(f, "    return lo + (int64_t)(baga_seed %% (uint64_t)(hi - lo + 1));\n");
    fprintf(f, "}\n\n");

    /* requires предикати + брой тестируеми */
    int n_tested = 0;
    for (int i = 0; i < program->items.len; i++) {
        Node *it = program->items.data[i];
        if (it->kind != NODE_SPEC) continue;
        if (it->spec_ensures.len == 0 && it->spec_requires.len == 0) continue;
        if (!spec_inputs_testable(it)) continue;
        emit_requires_predicate(cg, it);
        n_tested++;
    }

    fprintf(f, "int main(int argc, char **argv) {\n");
    fprintf(f, "    baga_argc = argc; baga_argv = argv;\n");
    if (n_tested == 0) {
        fprintf(f, "    printf(\"няма spec-ове за тестване\\n\");\n");
        fprintf(f, "    return 0;\n}\n");
        return;
    }

    for (int i = 0; i < program->items.len; i++) {
        Node *it = program->items.data[i];
        if (it->kind != NODE_SPEC) continue;
        if (it->spec_ensures.len == 0 && it->spec_requires.len == 0) continue;
        if (!spec_inputs_testable(it)) {
            fprintf(f, "    printf(");
            emit_c_string(f, it->spec_name);
            fprintf(f, " \": пропусната (неподдържан тип за --test-specs)\\n\");\n");
            continue;
        }
        int np = it->spec_inputs.len;
        char *fm = mangle_name(it->spec_name);
        fprintf(f, "    { int passed = 0, skipped = 0;\n");
        fprintf(f, "      for (int t = 0; t < %d; t++) {\n", BAGA_TEST_COUNT);
        fprintf(f, "          int64_t args[%d];\n", np > 0 ? np : 1);
        fprintf(f, "          int ok = 0;\n");
        fprintf(f, "          for (long tr = 0; tr < %d && !ok; tr++) {\n", BAGA_TEST_TRIES);
        for (int j = 0; j < np; j++) {
            Node *pt = it->spec_inputs.data[j]->param_type;
            if (strcmp(pt->type_name, "bool") == 0)
                fprintf(f, "              args[%d] = baga_rand_i64(0, 1);\n", j);
            else
                fprintf(f, "              args[%d] = baga_rand_i64(-1000, 1000);\n", j);
        }
        fprintf(f, "              ok = b__req_%s(", fm + 2);
        for (int j = 0; j < np; j++) fprintf(f, "%sargs[%d]", j ? ", " : "", j);
        fprintf(f, ");\n");
        fprintf(f, "              if (!ok) skipped++;\n");
        fprintf(f, "          }\n");
        fprintf(f, "          if (!ok) continue;\n");
        fprintf(f, "          baga_cur_nargs = %d;\n", np);
        for (int j = 0; j < np; j++)
            fprintf(f, "          baga_cur_args[%d] = args[%d];\n", j, j);
        fprintf(f, "          (void)%s(", fm);
        for (int j = 0; j < np; j++) fprintf(f, "%sargs[%d]", j ? ", " : "", j);
        fprintf(f, ");\n");
        fprintf(f, "          passed++;\n      }\n");
        fprintf(f, "      printf(");
        emit_c_string(f, it->spec_name);
        fprintf(f, " \": %%d/%d теста минаха (%%d пропуснати от requires)\\n\", passed, skipped);\n", BAGA_TEST_COUNT);
        fprintf(f, "    }\n");
        free(fm);
    }
    fprintf(f, "    printf(\"Всички spec тестове минаха. ⚔️\\n\");\n");
    fprintf(f, "    return 0;\n}\n");
}

void codegen_c(Codegen *cg, Node *program, FILE *out) {
    cg->out = out;
    cg->indent = 0;
    cg->tmp_counter = 0;
    cg->program = program;

    /* header */
    fprintf(out, "/* Генериран от компилатора на Бага. Фаза 1. */\n");
    fprintf(out, "#include <stdio.h>\n");
    fprintf(out, "#include <stdlib.h>\n");
    fprintf(out, "#include <stdint.h>\n");
    fprintf(out, "#include <string.h>\n");
    fprintf(out, "#include <errno.h>\n");
    fprintf(out, "#include <time.h>\n");
    fprintf(out, "#include <signal.h>\n");
    fprintf(out, "#include <pthread.h>\n\n");

    /* arena — всички низови/векторни алокации минават тук (без individual free).
     * baga_alloc_mu: глобалният arena се ползва от всички нишки (go/go_bg);
     * без ключ две нишки могат да получат един и същ блок (race → corruption). */
    fprintf(out, "typedef struct baga_ABlk { struct baga_ABlk *next; size_t used, cap; char data[]; } baga_ABlk;\n");
    fprintf(out, "static baga_ABlk *baga_arena_head = NULL;\n");
    fprintf(out, "static pthread_mutex_t baga_alloc_mu = PTHREAD_MUTEX_INITIALIZER;\n");
    /* MEM-1: free list — 16-байтови класове ≤ 1024 B; drop рециклира блокове */
    fprintf(out, "#define BAGA_FL_CLASSES 64   /* free list: 16-байтови класове ≤ 1024 B */\n");
    fprintf(out, "static void *baga_fl[BAGA_FL_CLASSES];\n");
    fprintf(out, "static void baga_free(void *p, int64_t n) {\n");
    fprintf(out, "    if (!p || n <= 0 || n > 1024) return;\n");
    fprintf(out, "    int c = (int)((n + 15) / 16) - 1;\n");
    fprintf(out, "    pthread_mutex_lock(&baga_alloc_mu);\n");
    fprintf(out, "    *(void **)p = baga_fl[c];\n");
    fprintf(out, "    baga_fl[c] = p;\n");
    fprintf(out, "    pthread_mutex_unlock(&baga_alloc_mu);\n");
    fprintf(out, "}\n");
    fprintf(out, "static void *baga_alloc(size_t n) {\n");
    fprintf(out, "    size_t rn = (n + 15) & ~(size_t)15;\n");
    fprintf(out, "    if (rn >= 16 && rn <= 1024) {\n");
    fprintf(out, "        pthread_mutex_lock(&baga_alloc_mu);\n");
    fprintf(out, "        void *fb = baga_fl[rn / 16 - 1];\n");
    fprintf(out, "        if (fb) { baga_fl[rn / 16 - 1] = *(void **)fb; pthread_mutex_unlock(&baga_alloc_mu); return fb; }\n");
    fprintf(out, "        pthread_mutex_unlock(&baga_alloc_mu);\n");
    fprintf(out, "    }\n");
    /* MEM-1 fix: малките алокации се bump-ват с КЛАСОВИЯ размер rn (не n) —
     * иначе free-list блок от по-малка заявка обслужва по-голяма в същия
     * клас и я презаписва съседния блок (segfault при review). >1024 B:
     * точен n, както преди. До 15 B slack на малка алокация. */
    fprintf(out, "    size_t an = (rn <= 1024) ? rn : n;\n");
    fprintf(out, "    pthread_mutex_lock(&baga_alloc_mu);\n");
    fprintf(out, "    baga_ABlk *b = baga_arena_head;\n");
    fprintf(out, "    if (!b || b->used + an > b->cap) {\n");
    fprintf(out, "        size_t cap = an > 8192 ? an : 8192;\n");
    fprintf(out, "        b = (baga_ABlk *)malloc(sizeof(baga_ABlk) + cap);\n");
    fprintf(out, "        b->next = baga_arena_head; b->used = 0; b->cap = cap;\n");
    fprintf(out, "        baga_arena_head = b;\n");
    fprintf(out, "    }\n");
    fprintf(out, "    void *p = b->data + b->used; b->used += an;\n");
    fprintf(out, "    pthread_mutex_unlock(&baga_alloc_mu);\n");
    fprintf(out, "    return p;\n");
    fprintf(out, "}\n");

    /* runtime helpers */
    fprintf(out, "static void baga_print_i64(int64_t v) { printf(\"%%lld\\n\", (long long)v); }\n");
    fprintf(out, "static void baga_print_f64(double v)  { printf(\"%%g\\n\", v); }\n");
    fprintf(out, "static void baga_print_str(const char *s) { printf(\"%%s\\n\", s); }\n");
    fprintf(out, "static void baga_write(const char *s) { printf(\"%%s\", s); }\n");
    fprintf(out, "static int64_t baga_len(const char *s) { return (int64_t)strlen(s); }\n");
    fprintf(out, "static void baga_bounds_fail(const char *fn, int64_t i, int64_t len) {\n");
    fprintf(out, "    fprintf(stderr, \"baga: %%s: индекс %%lld извън границите [0, %%lld)\\n\", fn, (long long)i, (long long)len);\n");
    fprintf(out, "    exit(1);\n");
    fprintf(out, "}\n");
    fprintf(out, "static void baga_div_zero_fail(void) {\n");
    fprintf(out, "    fprintf(stderr, \"baga: деление на нула\\n\");\n");
    fprintf(out, "    exit(1);\n");
    fprintf(out, "}\n");
    fprintf(out, "static int64_t baga_char_at(const char *s, int64_t i) {\n");
    fprintf(out, "    int64_t n = (int64_t)strlen(s);\n");
    fprintf(out, "    if (i < 0 || i >= n) baga_bounds_fail(\"char_at\", i, n);\n");
    fprintf(out, "    return (int64_t)(unsigned char)s[i];\n");
    fprintf(out, "}\n");
    fprintf(out, "static int64_t baga_byte_at(const char *s, int64_t i) { return (int64_t)(unsigned char)s[i]; }\n");
    fprintf(out, "static const char *baga_byte_chr(int64_t c) { char *r = baga_alloc(2); r[0] = (char)c; r[1] = 0; return r; }\n");
    fprintf(out, "static const char *baga_substr(const char *s, int64_t a, int64_t b) {\n");
    fprintf(out, "    int64_t n = (int64_t)strlen(s);\n");
    fprintf(out, "    if (a < 0 || a > n) baga_bounds_fail(\"substr\", a, n);\n");
    fprintf(out, "    if (b < 0 || b > n) baga_bounds_fail(\"substr\", b, n);\n");
    fprintf(out, "    int64_t len = b - a; if (len < 0) len = 0;\n");
    fprintf(out, "    char *r = baga_alloc((size_t)len + 1); memcpy(r, s + a, (size_t)len); r[len] = 0; return r;\n");
    fprintf(out, "}\n");
    fprintf(out, "static const char *baga_concat(const char *a, const char *b) {\n");
    fprintf(out, "    size_t la = strlen(a), lb = strlen(b);\n");
    fprintf(out, "    char *r = baga_alloc(la + lb + 1); memcpy(r, a, la); memcpy(r + la, b, lb + 1); return r;\n");
    fprintf(out, "}\n");
    fprintf(out, "static const char *baga_read_file(const char *path) {\n");
    fprintf(out, "    FILE *f = fopen(path, \"rb\"); if (!f) return \"\";\n");
    fprintf(out, "    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);\n");
    fprintf(out, "    char *buf = baga_alloc((size_t)sz + 1); fread(buf, 1, (size_t)sz, f); buf[sz] = 0; fclose(f); return buf;\n");
    fprintf(out, "}\n");
    fprintf(out, "static const char *baga_chr(int64_t c) {\n");
    fprintf(out, "    char *r = baga_alloc(5);\n");
    fprintf(out, "    if (c < 0x80) { r[0] = (char)c; r[1] = 0; }\n");
    fprintf(out, "    else if (c < 0x800) { r[0] = (char)(0xC0|(c>>6)); r[1] = (char)(0x80|(c&0x3F)); r[2] = 0; }\n");
    fprintf(out, "    else if (c < 0x10000) { r[0] = (char)(0xE0|(c>>12)); r[1] = (char)(0x80|((c>>6)&0x3F)); r[2] = (char)(0x80|(c&0x3F)); r[3] = 0; }\n");
    fprintf(out, "    else { r[0] = (char)(0xF0|(c>>18)); r[1] = (char)(0x80|((c>>12)&0x3F)); r[2] = (char)(0x80|((c>>6)&0x3F)); r[3] = (char)(0x80|(c&0x3F)); r[4] = 0; }\n");
    fprintf(out, "    return r;\n");
    fprintf(out, "}\n");
    fprintf(out, "static int64_t baga_ord(const char *s) {\n");
    fprintf(out, "    unsigned char c = (unsigned char)s[0]; if (!c) return 0;\n");
    fprintf(out, "    if (c < 0x80) return c;\n");
    fprintf(out, "    if ((c&0xE0)==0xC0) return ((int64_t)(c&0x1F)<<6)|((int64_t)(unsigned char)s[1]&0x3F);\n");
    fprintf(out, "    if ((c&0xF0)==0xE0) return ((int64_t)(c&0x0F)<<12)|(((int64_t)(unsigned char)s[1]&0x3F)<<6)|((int64_t)(unsigned char)s[2]&0x3F);\n");
    fprintf(out, "    return ((int64_t)(c&0x07)<<18)|(((int64_t)(unsigned char)s[1]&0x3F)<<12)|(((int64_t)(unsigned char)s[2]&0x3F)<<6)|((int64_t)(unsigned char)s[3]&0x3F);\n");
    fprintf(out, "}\n");
    fprintf(out, "static const char *baga_i64_to_str(int64_t x) { char *r = baga_alloc(24); snprintf(r, 24, \"%%lld\", (long long)x); return r; }\n");
    fprintf(out, "static int64_t baga_str_eq(const char *a, const char *b) { return strcmp(a, b) == 0; }\n");
    fprintf(out, "static int baga_argc = 0;\n");
    fprintf(out, "static char **baga_argv = 0;\n");
    fprintf(out, "static int64_t baga_arg_count(void) { return baga_argc > 0 ? baga_argc - 1 : 0; }\n");
    fprintf(out, "static const char *baga_arg(int64_t i) { return (i + 1 < baga_argc) ? baga_argv[i + 1] : \"\"; }\n");
    fprintf(out, "static void baga_exit(int64_t c) { exit((int)c); }\n");
    fprintf(out, "static void baga_eprintln(const char *s) { fprintf(stderr, \"%%s\\n\", s); }\n");
    fprintf(out, "static int64_t baga_cur_args[16];\n");
    fprintf(out, "static int baga_cur_nargs = 0;\n");
    fprintf(out, "static void baga_spec_fail(const char *spec, const char *kind, int64_t idx, const char *expr) {\n");
    fprintf(out, "    if (strcmp(kind, \"requires\") == 0)\n");
    fprintf(out, "        fprintf(stderr, \"spec '%%s': requires #%%lld нарушено: %%s\\n\", spec, (long long)idx, expr);\n");
    fprintf(out, "    else\n");
    fprintf(out, "        fprintf(stderr, \"spec '%%s': ensures #%%lld нарушена: %%s\\n\", spec, (long long)idx, expr);\n");
    fprintf(out, "    if (baga_cur_nargs > 0) {\n");
    fprintf(out, "        fprintf(stderr, \"  вход: \");\n");
    fprintf(out, "        for (int i = 0; i < baga_cur_nargs; i++)\n");
    fprintf(out, "            fprintf(stderr, \"%%s%%lld\", i ? \", \" : \"\", (long long)baga_cur_args[i]);\n");
    fprintf(out, "        fprintf(stderr, \"\\n\");\n");
    fprintf(out, "    }\n");
    fprintf(out, "    exit(1);\n");
    fprintf(out, "}\n");
    fprintf(out, "\n");
    fprintf(out, "/* binary-safe byte buffer */\n");
    fprintf(out, "typedef struct { unsigned char *data; int64_t len; } baga_bytes;\n");
    fprintf(out, "static int64_t baga_bytes_len(baga_bytes b) { return b.len; }\n");
    fprintf(out, "static int64_t baga_bytes_at(baga_bytes b, int64_t i) {\n");
    fprintf(out, "    if (i < 0 || i >= b.len) baga_bounds_fail(\"bytes_at\", i, b.len);\n");
    fprintf(out, "    return (int64_t)b.data[i];\n");
    fprintf(out, "}\n");
    fprintf(out, "static baga_bytes baga_bytes_slice(baga_bytes b, int64_t a, int64_t c) {\n");
    fprintf(out, "    if (a < 0) a = 0; if (c > b.len) c = b.len; if (c < a) c = a;\n");
    fprintf(out, "    baga_bytes r; r.len = c - a; r.data = baga_alloc((size_t)(r.len ? r.len : 1));\n");
    fprintf(out, "    memcpy(r.data, b.data + a, (size_t)r.len); return r; }\n");
    fprintf(out, "static baga_bytes baga_bytes_concat(baga_bytes a, baga_bytes b) {\n");
    fprintf(out, "    baga_bytes r; r.len = a.len + b.len; r.data = baga_alloc((size_t)(r.len ? r.len : 1));\n");
    fprintf(out, "    memcpy(r.data, a.data, (size_t)a.len); memcpy(r.data + a.len, b.data, (size_t)b.len); return r; }\n");
    fprintf(out, "static baga_bytes baga_bytes_new(int64_t n) {\n");
    fprintf(out, "    if (n < 0) n = 0;\n");
    fprintf(out, "    baga_bytes b; b.len = n; b.data = baga_alloc(n > 0 ? n : 1);\n");
    fprintf(out, "    memset(b.data, 0, (size_t)(n > 0 ? n : 1));\n");
    fprintf(out, "    return b;\n");
    fprintf(out, "}\n");
    fprintf(out, "static void baga_bytes_set(baga_bytes b, int64_t i, int64_t v) {\n");
    fprintf(out, "    if (i < 0 || i >= b.len) baga_bounds_fail(\"bytes_set\", i, b.len);\n");
    fprintf(out, "    b.data[i] = (unsigned char)(v & 0xff);\n");
    fprintf(out, "}\n");
    fprintf(out, "static baga_bytes baga_bytes_push(baga_bytes b, int64_t v) {\n");
    fprintf(out, "    baga_bytes r; r.len = b.len + 1; r.data = baga_alloc(r.len);\n");
    fprintf(out, "    memcpy(r.data, b.data, (size_t)b.len);\n");
    fprintf(out, "    r.data[b.len] = (unsigned char)(v & 0xff);\n");
    fprintf(out, "    return r;\n");
    fprintf(out, "}\n");
    fprintf(out, "static baga_bytes baga_bytes_from_str(const char *s) {\n");
    fprintf(out, "    int64_t n = (int64_t)strlen(s); baga_bytes r; r.len = n; r.data = baga_alloc((size_t)(n ? n : 1));\n");
    fprintf(out, "    memcpy(r.data, s, (size_t)n); return r; }\n");
    fprintf(out, "static baga_bytes baga_bytes_lit(const unsigned char *d, int64_t n) {\n");
    fprintf(out, "    baga_bytes r; r.len = n; r.data = baga_alloc((size_t)(n ? n : 1));\n");
    fprintf(out, "    memcpy(r.data, d, (size_t)n); return r; }\n");
    fprintf(out, "static const char *baga_bytes_to_str(baga_bytes b) {\n");
    fprintf(out, "    char *r = baga_alloc((size_t)b.len + 1); memcpy(r, b.data, (size_t)b.len); r[b.len] = 0; return r; }\n");
    fprintf(out, "static int baga_hex_val(int c) {\n");
    fprintf(out, "    if (c >= '0' && c <= '9') return c - '0';\n");
    fprintf(out, "    if (c >= 'a' && c <= 'f') return c - 'a' + 10;\n");
    fprintf(out, "    if (c >= 'A' && c <= 'F') return c - 'A' + 10;\n");
    fprintf(out, "    return -1; }\n");
    fprintf(out, "static const char *baga_hex_encode(baga_bytes b) {\n");
    fprintf(out, "    static const char *hx = \"0123456789abcdef\";\n");
    fprintf(out, "    char *r = baga_alloc((size_t)b.len * 2 + 1);\n");
    fprintf(out, "    for (int64_t i = 0; i < b.len; i++) { r[i*2] = hx[b.data[i] >> 4]; r[i*2+1] = hx[b.data[i] & 15]; }\n");
    fprintf(out, "    r[b.len * 2] = 0; return r; }\n");
    fprintf(out, "static baga_bytes baga_hex_decode(const char *s) {\n");
    fprintf(out, "    int64_t n = (int64_t)strlen(s); unsigned char *buf = baga_alloc((size_t)(n / 2 + 1)); int64_t len = 0;\n");
    fprintf(out, "    for (int64_t i = 0; i + 1 < n; ) {\n");
    fprintf(out, "        int hi = baga_hex_val(s[i]); int lo = baga_hex_val(s[i+1]);\n");
    fprintf(out, "        if (hi < 0 || lo < 0) { i++; continue; }\n");
    fprintf(out, "        buf[len++] = (unsigned char)(hi * 16 + lo); i += 2; }\n");
    fprintf(out, "    baga_bytes r; r.data = buf; r.len = len; return r; }\n");
    fprintf(out, "static baga_bytes baga_bytes_from_hex(const char *s) { return baga_hex_decode(s); }\n");
    fprintf(out, "\n");
    fprintf(out, "/* dynamic array */\n");
    fprintf(out, "typedef struct { void **data; int64_t len; int64_t cap; } baga_Vec;\n");
    fprintf(out, "static baga_Vec *baga_vec_new(void) {\n");
    fprintf(out, "    baga_Vec *v = baga_alloc(sizeof(baga_Vec));\n");
    fprintf(out, "    v->cap = 8; v->len = 0; v->data = baga_alloc(8 * sizeof(void *)); return v;\n");
    fprintf(out, "}\n");
    fprintf(out, "static void baga_vec_grow(baga_Vec *v) {\n");
    fprintf(out, "    if (v->len == v->cap) { v->cap *= 2;\n");
    fprintf(out, "        void **nd = (void **)baga_alloc((size_t)v->cap * sizeof(void *));\n");
    fprintf(out, "        memcpy(nd, v->data, (size_t)v->len * sizeof(void *)); v->data = nd; }\n");
    fprintf(out, "}\n");
    fprintf(out, "static void baga_vec_push_i64(baga_Vec *v, int64_t x) { baga_vec_grow(v); v->data[v->len++] = (void *)(intptr_t)x; }\n");
    fprintf(out, "static int64_t baga_vec_get_i64(baga_Vec *v, int64_t i) {\n");
    fprintf(out, "    if (i < 0 || i >= v->len) baga_bounds_fail(\"vec_get\", i, v->len);\n");
    fprintf(out, "    return (int64_t)(intptr_t)v->data[i];\n");
    fprintf(out, "}\n");
    fprintf(out, "static void baga_vec_set_i64(baga_Vec *v, int64_t i, int64_t x) {\n");
    fprintf(out, "    if (i < 0 || i >= v->len) baga_bounds_fail(\"vec_set\", i, v->len);\n");
    fprintf(out, "    v->data[i] = (void *)(intptr_t)x;\n");
    fprintf(out, "}\n");
    fprintf(out, "static void baga_vec_push_str(baga_Vec *v, const char *s) { baga_vec_grow(v); v->data[v->len++] = (void *)s; }\n");
    fprintf(out, "static const char *baga_vec_get_str(baga_Vec *v, int64_t i) {\n");
    fprintf(out, "    if (i < 0 || i >= v->len) baga_bounds_fail(\"vec_get\", i, v->len);\n");
    fprintf(out, "    return (const char *)v->data[i];\n");
    fprintf(out, "}\n");
    fprintf(out, "static void baga_vec_set_str(baga_Vec *v, int64_t i, const char *s) {\n");
    fprintf(out, "    if (i < 0 || i >= v->len) baga_bounds_fail(\"vec_set\", i, v->len);\n");
    fprintf(out, "    v->data[i] = (void *)s;\n");
    fprintf(out, "}\n");
    fprintf(out, "static void baga_vec_push_f64(baga_Vec *v, double x) { union { double d; void *p; } u; u.d = x; baga_vec_grow(v); v->data[v->len++] = u.p; }\n");
    fprintf(out, "static double baga_vec_get_f64(baga_Vec *v, int64_t i) {\n");
    fprintf(out, "    if (i < 0 || i >= v->len) baga_bounds_fail(\"vec_get\", i, v->len);\n");
    fprintf(out, "    union { double d; void *p; } u; u.p = v->data[i]; return u.d;\n");
    fprintf(out, "}\n");
    fprintf(out, "static void baga_vec_set_f64(baga_Vec *v, int64_t i, double x) {\n");
    fprintf(out, "    if (i < 0 || i >= v->len) baga_bounds_fail(\"vec_set\", i, v->len);\n");
    fprintf(out, "    union { double d; void *p; } u; u.d = x; v->data[i] = u.p;\n");
    fprintf(out, "}\n");
    /* bytes елементи: box-нат baga_bytes по указател (като baga_map_*_bytes) */
    fprintf(out, "static void baga_vec_push_bytes(baga_Vec *v, baga_bytes b) {\n");
    fprintf(out, "    baga_vec_grow(v);\n");
    fprintf(out, "    baga_bytes *p = baga_alloc(sizeof(baga_bytes)); *p = b;\n");
    fprintf(out, "    v->data[v->len++] = p;\n");
    fprintf(out, "}\n");
    fprintf(out, "static baga_bytes baga_vec_get_bytes(baga_Vec *v, int64_t i) {\n");
    fprintf(out, "    if (i < 0 || i >= v->len) baga_bounds_fail(\"vec_get\", i, v->len);\n");
    fprintf(out, "    return *(baga_bytes *)v->data[i];\n");
    fprintf(out, "}\n");
    fprintf(out, "static void baga_vec_set_bytes(baga_Vec *v, int64_t i, baga_bytes b) {\n");
    fprintf(out, "    if (i < 0 || i >= v->len) baga_bounds_fail(\"vec_set\", i, v->len);\n");
    fprintf(out, "    *(baga_bytes *)v->data[i] = b;\n");
    fprintf(out, "}\n");
    /* struct елементи (L4): generic box helper-и; размерът идва от call site-а,
     * елементите са box-нати копия (memcpy при push/set/slice/concat) */
    fprintf(out, "static void baga_vec_push_box(baga_Vec *v, const void *src, int64_t size) {\n");
    fprintf(out, "    baga_vec_grow(v);\n");
    fprintf(out, "    void *p = baga_alloc((size_t)size); memcpy(p, src, (size_t)size);\n");
    fprintf(out, "    v->data[v->len++] = p;\n");
    fprintf(out, "}\n");
    fprintf(out, "static void *baga_vec_get_box(baga_Vec *v, int64_t i) {\n");
    fprintf(out, "    if (i < 0 || i >= v->len) baga_bounds_fail(\"vec_get\", i, v->len);\n");
    fprintf(out, "    return v->data[i];\n");
    fprintf(out, "}\n");
    fprintf(out, "static void baga_vec_set_box(baga_Vec *v, int64_t i, const void *src, int64_t size) {\n");
    fprintf(out, "    if (i < 0 || i >= v->len) baga_bounds_fail(\"vec_set\", i, v->len);\n");
    fprintf(out, "    memcpy(v->data[i], src, (size_t)size);\n");
    fprintf(out, "}\n");
    fprintf(out, "static int64_t baga_vec_len(baga_Vec *v) { return v ? v->len : 0; }\n");
    /* bridge: native bytes <-> Vec<i64> (crypto migration path) */
    fprintf(out, "static baga_bytes baga_bytes_from_vec(baga_Vec *v) {\n");
    fprintf(out, "    int64_t n = v ? v->len : 0;\n");
    fprintf(out, "    baga_bytes r; r.len = n; r.data = baga_alloc((size_t)(n ? n : 1));\n");
    fprintf(out, "    for (int64_t i = 0; i < n; i++) r.data[i] = (unsigned char)((int64_t)(intptr_t)v->data[i] & 255);\n");
    fprintf(out, "    return r; }\n");
    fprintf(out, "static baga_Vec *baga_vec_from_bytes(baga_bytes b) {\n");
    fprintf(out, "    baga_Vec *v = baga_vec_new();\n");
    fprintf(out, "    for (int64_t i = 0; i < b.len; i++) baga_vec_push_i64(v, (int64_t)b.data[i]);\n");
    fprintf(out, "    return v; }\n");
    fprintf(out, "static baga_Vec *baga_vec_slice_i64(baga_Vec *v, int64_t a, int64_t b) {\n");
    fprintf(out, "    if (a < 0) a = 0; if (b > v->len) b = v->len; if (b < a) b = a;\n");
    fprintf(out, "    baga_Vec *r = baga_vec_new();\n");
    fprintf(out, "    for (int64_t i = a; i < b; i++) baga_vec_push_i64(r, (int64_t)(intptr_t)v->data[i]);\n");
    fprintf(out, "    return r; }\n");
    fprintf(out, "static baga_Vec *baga_vec_slice_str(baga_Vec *v, int64_t a, int64_t b) {\n");
    fprintf(out, "    if (a < 0) a = 0; if (b > v->len) b = v->len; if (b < a) b = a;\n");
    fprintf(out, "    baga_Vec *r = baga_vec_new();\n");
    fprintf(out, "    for (int64_t i = a; i < b; i++) baga_vec_push_str(r, (const char *)v->data[i]);\n");
    fprintf(out, "    return r; }\n");
    fprintf(out, "static baga_Vec *baga_vec_slice_f64(baga_Vec *v, int64_t a, int64_t b) {\n");
    fprintf(out, "    if (a < 0) a = 0; if (b > v->len) b = v->len; if (b < a) b = a;\n");
    fprintf(out, "    baga_Vec *r = baga_vec_new();\n");
    fprintf(out, "    for (int64_t i = a; i < b; i++) { union { double d; void *p; } u; u.p = v->data[i]; baga_vec_push_f64(r, u.d); }\n");
    fprintf(out, "    return r; }\n");
    fprintf(out, "static baga_Vec *baga_vec_concat_i64(baga_Vec *v, baga_Vec *w) {\n");
    fprintf(out, "    baga_Vec *r = baga_vec_new();\n");
    fprintf(out, "    for (int64_t i = 0; i < v->len; i++) baga_vec_push_i64(r, (int64_t)(intptr_t)v->data[i]);\n");
    fprintf(out, "    for (int64_t i = 0; i < w->len; i++) baga_vec_push_i64(r, (int64_t)(intptr_t)w->data[i]);\n");
    fprintf(out, "    return r; }\n");
    fprintf(out, "static baga_Vec *baga_vec_concat_str(baga_Vec *v, baga_Vec *w) {\n");
    fprintf(out, "    baga_Vec *r = baga_vec_new();\n");
    fprintf(out, "    for (int64_t i = 0; i < v->len; i++) baga_vec_push_str(r, (const char *)v->data[i]);\n");
    fprintf(out, "    for (int64_t i = 0; i < w->len; i++) baga_vec_push_str(r, (const char *)w->data[i]);\n");
    fprintf(out, "    return r; }\n");
    fprintf(out, "static baga_Vec *baga_vec_concat_f64(baga_Vec *v, baga_Vec *w) {\n");
    fprintf(out, "    baga_Vec *r = baga_vec_new();\n");
    fprintf(out, "    for (int64_t i = 0; i < v->len; i++) { union { double d; void *p; } u; u.p = v->data[i]; baga_vec_push_f64(r, u.d); }\n");
    fprintf(out, "    for (int64_t i = 0; i < w->len; i++) { union { double d; void *p; } u; u.p = w->data[i]; baga_vec_push_f64(r, u.d); }\n");
    fprintf(out, "    return r; }\n");
    fprintf(out, "static baga_Vec *baga_vec_slice_bytes(baga_Vec *v, int64_t a, int64_t b) {\n");
    fprintf(out, "    if (a < 0) a = 0; if (b > v->len) b = v->len; if (b < a) b = a;\n");
    fprintf(out, "    baga_Vec *r = baga_vec_new();\n");
    fprintf(out, "    for (int64_t i = a; i < b; i++) baga_vec_push_bytes(r, *(baga_bytes *)v->data[i]);\n");
    fprintf(out, "    return r; }\n");
    fprintf(out, "static baga_Vec *baga_vec_concat_bytes(baga_Vec *v, baga_Vec *w) {\n");
    fprintf(out, "    baga_Vec *r = baga_vec_new();\n");
    fprintf(out, "    for (int64_t i = 0; i < v->len; i++) baga_vec_push_bytes(r, *(baga_bytes *)v->data[i]);\n");
    fprintf(out, "    for (int64_t i = 0; i < w->len; i++) baga_vec_push_bytes(r, *(baga_bytes *)w->data[i]);\n");
    fprintf(out, "    return r; }\n");
    fprintf(out, "static baga_Vec *baga_vec_slice_box(baga_Vec *v, int64_t a, int64_t b, int64_t size) {\n");
    fprintf(out, "    if (a < 0) a = 0; if (b > v->len) b = v->len; if (b < a) b = a;\n");
    fprintf(out, "    baga_Vec *r = baga_vec_new();\n");
    fprintf(out, "    for (int64_t i = a; i < b; i++) baga_vec_push_box(r, v->data[i], size);\n");
    fprintf(out, "    return r; }\n");
    fprintf(out, "static baga_Vec *baga_vec_concat_box(baga_Vec *v, baga_Vec *w, int64_t size) {\n");
    fprintf(out, "    baga_Vec *r = baga_vec_new();\n");
    fprintf(out, "    for (int64_t i = 0; i < v->len; i++) baga_vec_push_box(r, v->data[i], size);\n");
    fprintf(out, "    for (int64_t i = 0; i < w->len; i++) baga_vec_push_box(r, w->data[i], size);\n");
    fprintf(out, "    return r; }\n");
    fprintf(out, "\n/* hash map: chaining, key i64/str, value i64/str/f64/bytes (leak-tolerant like baga_Vec) */\n");
    fprintf(out, "typedef struct baga_MapEntry {\n");
    fprintf(out, "    int64_t ik; const char *sk;\n");
    fprintf(out, "    int64_t iv; double fv; const char *sv; baga_bytes bv; void *pv;\n");
    fprintf(out, "    struct baga_MapEntry *next;\n");
    fprintf(out, "} baga_MapEntry;\n");
    fprintf(out, "typedef struct { baga_MapEntry **b; int64_t nb; int64_t len; } baga_Map;\n");
    fprintf(out, "static uint64_t baga_map_hash_str(const char *s) {\n");
    fprintf(out, "    uint64_t h = 1469598103934665603ULL;\n");
    fprintf(out, "    while (*s) { h ^= (unsigned char)*s++; h *= 1099511628211ULL; }\n");
    fprintf(out, "    return h; }\n");
    fprintf(out, "static uint64_t baga_map_hash_i64(int64_t k) {\n");
    fprintf(out, "    uint64_t x = (uint64_t)k;\n");
    fprintf(out, "    x ^= x >> 33; x *= 0xff51afd7ed558ccdULL; x ^= x >> 33;\n");
    fprintf(out, "    x *= 0xc4ceb9fe1a85ec53ULL; x ^= x >> 33; return x; }\n");
    fprintf(out, "static baga_Map *baga_map_new(void) {\n");
    fprintf(out, "    baga_Map *m = baga_alloc(sizeof(baga_Map));\n");
    fprintf(out, "    m->nb = 16; m->len = 0;\n");
    fprintf(out, "    m->b = baga_alloc(sizeof(baga_MapEntry *) * (size_t)m->nb);\n");
    fprintf(out, "    for (int64_t i = 0; i < m->nb; i++) m->b[i] = NULL;\n");
    fprintf(out, "    return m; }\n");
    /* slot lookup: returns the entry pointer or the NULL link (insert point) */
    fprintf(out, "static baga_MapEntry **baga_map_slot(baga_Map *m, int64_t ik, const char *sk, uint64_t h) {\n");
    fprintf(out, "    baga_MapEntry **e = &m->b[h %% (uint64_t)m->nb];\n");
    fprintf(out, "    while (*e) {\n");
    fprintf(out, "        if (sk ? ((*e)->sk && strcmp((*e)->sk, sk) == 0)\n");
    fprintf(out, "               : (!(*e)->sk && (*e)->ik == ik)) return e;\n");
    fprintf(out, "        e = &(*e)->next; }\n");
    fprintf(out, "    return e; }\n");
    fprintf(out, "static void baga_map_rehash(baga_Map *m) {\n");
    fprintf(out, "    int64_t onb = m->nb; baga_MapEntry **ob = m->b;\n");
    fprintf(out, "    m->nb *= 2;\n");
    fprintf(out, "    m->b = baga_alloc(sizeof(baga_MapEntry *) * (size_t)m->nb);\n");
    fprintf(out, "    for (int64_t i = 0; i < m->nb; i++) m->b[i] = NULL;\n");
    fprintf(out, "    for (int64_t i = 0; i < onb; i++) {\n");
    fprintf(out, "        baga_MapEntry *e = ob[i];\n");
    fprintf(out, "        while (e) {\n");
    fprintf(out, "            baga_MapEntry *nx = e->next;\n");
    fprintf(out, "            uint64_t h = e->sk ? baga_map_hash_str(e->sk) : baga_map_hash_i64(e->ik);\n");
    fprintf(out, "            e->next = m->b[h %% (uint64_t)m->nb];\n");
    fprintf(out, "            m->b[h %% (uint64_t)m->nb] = e;\n");
    fprintf(out, "            e = nx; } } }\n");
    fprintf(out, "static baga_MapEntry *baga_map_put(baga_Map *m, int64_t ik, const char *sk, uint64_t h) {\n");
    fprintf(out, "    baga_MapEntry **slot = baga_map_slot(m, ik, sk, h);\n");
    fprintf(out, "    if (*slot) return *slot;\n");
    fprintf(out, "    baga_MapEntry *e = baga_alloc(sizeof(baga_MapEntry));\n");
    fprintf(out, "    e->ik = ik; e->sk = sk; e->iv = 0; e->fv = 0; e->sv = NULL;\n");
    fprintf(out, "    e->bv.data = NULL; e->bv.len = 0; e->pv = NULL; e->next = NULL;\n");
    fprintf(out, "    *slot = e; m->len++;\n");
    fprintf(out, "    if (m->len * 4 > m->nb * 3) baga_map_rehash(m);\n");
    fprintf(out, "    return e; }\n");
    fprintf(out, "static int64_t baga_map_len(baga_Map *m) { return m ? m->len : 0; }\n");
    /* typed variants: baga_map_{set,get}_{str,i64}_{i64,str,f64},
     * has/del/keys по ключ — имената съвпадат с lowering-а по-горе */
    for (int ki = 0; ki < 2; ki++) {
        const char *kn   = ki == 0 ? "str" : "i64";
        const char *karg = ki == 0 ? "const char *k" : "int64_t k";
        const char *hk   = ki == 0 ? "baga_map_hash_str(k)" : "baga_map_hash_i64(k)";
        const char *ikv  = ki == 0 ? "0" : "k";
        const char *skv  = ki == 0 ? "k" : "NULL";
        for (int vi = 0; vi < 3; vi++) {
            const char *vn   = vi == 0 ? "i64" : (vi == 1 ? "str" : "f64");
            const char *varg = vi == 0 ? "int64_t v" : (vi == 1 ? "const char *v" : "double v");
            const char *fld  = vi == 0 ? "iv" : (vi == 1 ? "sv" : "fv");
            fprintf(out, "static void baga_map_set_%s_%s(baga_Map *m, %s, %s) {\n", kn, vn, karg, varg);
            fprintf(out, "    baga_MapEntry *e = baga_map_put(m, %s, %s, %s);\n", ikv, skv, hk);
            fprintf(out, "    e->%s = v; }\n", fld);
            const char *ret = vi == 0 ? "int64_t" : (vi == 1 ? "const char *" : "double");
            fprintf(out, "static %s baga_map_get_%s_%s(baga_Map *m, %s) {\n", ret, kn, vn, karg);
            fprintf(out, "    baga_MapEntry *e = *baga_map_slot(m, %s, %s, %s);\n", ikv, skv, hk);
            if (vi == 1) fprintf(out, "    return (e && e->sv) ? e->sv : \"\"; }\n");
            else if (vi == 0) fprintf(out, "    return e ? e->iv : 0; }\n");
            else fprintf(out, "    return e ? e->fv : 0; }\n");
        }
        /* bytes values (binary-safe; kvbaga K2 / chat buffers) */
        fprintf(out, "static void baga_map_set_%s_bytes(baga_Map *m, %s, baga_bytes v) {\n", kn, karg);
        fprintf(out, "    baga_MapEntry *e = baga_map_put(m, %s, %s, %s);\n", ikv, skv, hk);
        fprintf(out, "    e->bv = v; }\n");
        fprintf(out, "static baga_bytes baga_map_get_%s_bytes(baga_Map *m, %s) {\n", kn, karg);
        fprintf(out, "    baga_MapEntry *e = *baga_map_slot(m, %s, %s, %s);\n", ikv, skv, hk);
        fprintf(out, "    if (!e) { baga_bytes z; z.data = NULL; z.len = 0; return z; }\n");
        fprintf(out, "    return e->bv; }\n");
        /* struct values (L4): box per entry; set copies in, get returns the
         * box (NULL when missing — call site substitutes a zero struct) */
        fprintf(out, "static void baga_map_set_%s_box(baga_Map *m, %s, const void *src, int64_t size) {\n", kn, karg);
        fprintf(out, "    baga_MapEntry *e = baga_map_put(m, %s, %s, %s);\n", ikv, skv, hk);
        fprintf(out, "    if (!e->pv) e->pv = baga_alloc((size_t)size);\n");
        fprintf(out, "    memcpy(e->pv, src, (size_t)size); }\n");
        fprintf(out, "static void *baga_map_get_%s_box(baga_Map *m, %s) {\n", kn, karg);
        fprintf(out, "    baga_MapEntry *e = *baga_map_slot(m, %s, %s, %s);\n", ikv, skv, hk);
        fprintf(out, "    return e ? e->pv : NULL; }\n");
        fprintf(out, "static int64_t baga_map_has_%s(baga_Map *m, %s) {\n", kn, karg);
        fprintf(out, "    return *baga_map_slot(m, %s, %s, %s) ? 1 : 0; }\n", ikv, skv, hk);
        fprintf(out, "static void baga_map_del_%s(baga_Map *m, %s) {\n", kn, karg);
        fprintf(out, "    baga_MapEntry **slot = baga_map_slot(m, %s, %s, %s);\n", ikv, skv, hk);
        fprintf(out, "    if (!*slot) return;\n");
        fprintf(out, "    baga_MapEntry *e = *slot; *slot = e->next; m->len--; }\n");
        fprintf(out, "static baga_Vec *baga_map_keys_%s(baga_Map *m) {\n", kn);
        fprintf(out, "    baga_Vec *v = baga_vec_new();\n");
        fprintf(out, "    for (int64_t i = 0; i < m->nb; i++)\n");
        fprintf(out, "        for (baga_MapEntry *e = m->b[i]; e; e = e->next)\n");
        if (ki == 0) fprintf(out, "            baga_vec_push_str(v, e->sk ? e->sk : \"\");\n");
        else         fprintf(out, "            baga_vec_push_i64(v, e->ik);\n");
        fprintf(out, "    return v; }\n");
    }
    /* MEM-1: drop walkers — рециклират собствените алокации през baga_free.
     * Вътрешните буфери (bytes.data, str полета, env box-ове на closures)
     * остават в arena-та — споделена собственост, документирано. */
    fprintf(out, "\nstatic void baga_cell2_free(int64_t h);\n");
    fprintf(out, "static void baga_drop_bytes(baga_bytes b) { baga_free(b.data, b.len); }\n");
    fprintf(out, "/* elem_kind: 0 = inline (i64/f64), 1 = str (споделени — не се пипат), 2 = box (bytes/struct) */\n");
    fprintf(out, "static void baga_drop_vec(baga_Vec *v, int elem_kind, int64_t elem_size) {\n");
    fprintf(out, "    if (!v) return;\n");
    fprintf(out, "    if (elem_kind == 2)\n");
    fprintf(out, "        for (int64_t i = 0; i < v->len; i++) baga_free(v->data[i], elem_size);\n");
    fprintf(out, "    baga_free(v->data, v->cap * 8);\n");
    fprintf(out, "    baga_free(v, (int64_t)sizeof(baga_Vec));\n");
    fprintf(out, "}\n");
    fprintf(out, "static void baga_drop_map(baga_Map *m, int val_is_box, int64_t val_size) {\n");
    fprintf(out, "    if (!m) return;\n");
    /* next се пази ПРЕДИ free — baga_free презаписва първата дума на блока,
     * не разчитаме на offset-а на 'next' в layout-а */
    fprintf(out, "    for (int64_t i = 0; i < m->nb; i++) {\n");
    fprintf(out, "        baga_MapEntry *e = m->b[i];\n");
    fprintf(out, "        while (e) {\n");
    fprintf(out, "            baga_MapEntry *nx = e->next;\n");
    fprintf(out, "            if (val_is_box && e->pv) baga_free(e->pv, val_size);\n");
    fprintf(out, "            baga_free(e, (int64_t)sizeof(baga_MapEntry));\n");
    fprintf(out, "            e = nx;\n");
    fprintf(out, "        }\n");
    fprintf(out, "    }\n");
    fprintf(out, "    baga_free(m->b, m->nb * (int64_t)sizeof(baga_MapEntry *));\n");
    fprintf(out, "    baga_free(m, (int64_t)sizeof(baga_Map));\n");
    fprintf(out, "}\n");
    fprintf(out, "static void baga_drop_fn(int64_t h) { baga_cell2_free(h); }\n");
    fprintf(out, "\n/* arena allocator: bump allocation, free-all-at-once */\n");
    fprintf(out, "typedef struct { char *base; int64_t used; int64_t cap; } baga_Arena;\n");
    fprintf(out, "static int64_t baga_arena_new(void) {\n");
    fprintf(out, "    baga_Arena *a = malloc(sizeof(baga_Arena));\n");
    fprintf(out, "    a->cap = 65536; a->used = 0; a->base = malloc((size_t)a->cap);\n");
    fprintf(out, "    return (int64_t)(intptr_t)a;\n");
    fprintf(out, "}\n");
    fprintf(out, "static int64_t baga_arena_alloc(int64_t h, int64_t size) {\n");
    fprintf(out, "    baga_Arena *a = (baga_Arena *)(intptr_t)h;\n");
    fprintf(out, "    if (!a) { fprintf(stderr, \"baga: arena_alloc: null handle\\n\"); exit(1); }\n");
    fprintf(out, "    if (size < 0) size = 0;\n");
    fprintf(out, "    if (a->used + size > a->cap) {\n");
    fprintf(out, "        int64_t nc = (a->used + size) * 2;\n");
    fprintf(out, "        a->base = realloc(a->base, (size_t)nc); a->cap = nc;\n");
    fprintf(out, "    }\n");
    fprintf(out, "    char *p = a->base + a->used; a->used += size;\n");
    fprintf(out, "    return (int64_t)(intptr_t)p;\n");
    fprintf(out, "}\n");
    fprintf(out, "static void baga_arena_reset(int64_t h) {\n");
    fprintf(out, "    baga_Arena *a = (baga_Arena *)(intptr_t)h;\n");
    fprintf(out, "    if (!a) { fprintf(stderr, \"baga: arena_reset: null handle\\n\"); exit(1); }\n");
    fprintf(out, "    a->used = 0;\n");
    fprintf(out, "}\n");
    fprintf(out, "static void baga_arena_free(int64_t h) {\n");
    fprintf(out, "    baga_Arena *a = (baga_Arena *)(intptr_t)h;\n");
    fprintf(out, "    if (!a) return;\n");
    fprintf(out, "    free(a->base); free(a);\n");
    fprintf(out, "}\n");
    fprintf(out, "\n");
    /* ---- concurrency (!Par): OS threads + i64 channels (CSP) ---- */
    /* heap pair first — used by chan_recv2 and as worker context packing */
    fprintf(out, "static int64_t baga_cell2(int64_t a, int64_t b) {\n");
    fprintf(out, "    int64_t *p = (int64_t *)malloc(2 * sizeof(int64_t));\n");
    fprintf(out, "    if (!p) { fprintf(stderr, \"baga: cell2: oom\\n\"); exit(1); }\n");
    fprintf(out, "    p[0] = a; p[1] = b; return (int64_t)(intptr_t)p;\n");
    fprintf(out, "}\n");
    /* MEM-1: cell2 е ЕДИН malloc блок (2×i64) — не arena, истински free() */
    fprintf(out, "static void baga_cell2_free(int64_t h) { if (h) free((void *)(intptr_t)h); }\n");
    fprintf(out, "static int64_t baga_cell2_0(int64_t h) { return ((int64_t *)(intptr_t)h)[0]; }\n");
    fprintf(out, "static int64_t baga_cell2_1(int64_t h) { return ((int64_t *)(intptr_t)h)[1]; }\n");
    fprintf(out, "typedef int64_t (*baga_par_fn)(int64_t);\n");
    fprintf(out, "typedef struct {\n");
    fprintf(out, "    uint32_t magic;\n");
    fprintf(out, "    baga_par_fn fn; int64_t arg; int64_t result; pthread_t th;\n");
    fprintf(out, "    int joined; int detached;\n");
    fprintf(out, "} baga_JoinHandle;\n");
    fprintf(out, "#define BAGA_HANDLE_MAGIC 0xBA6A0001u\n");
    fprintf(out, "static void *baga_par_trampoline(void *p) {\n");
    fprintf(out, "    baga_JoinHandle *h = (baga_JoinHandle *)p;\n");
    fprintf(out, "    h->result = h->fn(h->arg);\n");
    fprintf(out, "    /* 0=joinable, 1=detach requested, 2=finished (joinable) */\n");
    fprintf(out, "    int old = __sync_lock_test_and_set(&h->detached, 2);\n");
    fprintf(out, "    if (old == 1) free(h); /* parent already detached — we free */\n");
    fprintf(out, "    return NULL;\n");
    fprintf(out, "}\n");
    fprintf(out, "static int64_t baga_go(baga_par_fn fn, int64_t arg) {\n");
    fprintf(out, "    baga_JoinHandle *h = (baga_JoinHandle *)calloc(1, sizeof(baga_JoinHandle));\n");
    fprintf(out, "    if (!h) { fprintf(stderr, \"baga: go: out of memory\\n\"); exit(1); }\n");
    fprintf(out, "    h->fn = fn; h->arg = arg; h->joined = 0; h->detached = 0;\n");
    fprintf(out, "    h->magic = BAGA_HANDLE_MAGIC;\n");
    fprintf(out, "    if (pthread_create(&h->th, NULL, baga_par_trampoline, h) != 0) {\n");
    fprintf(out, "        fprintf(stderr, \"baga: go: pthread_create failed\\n\"); exit(1);\n");
    fprintf(out, "    }\n");
    fprintf(out, "    return (int64_t)(intptr_t)h;\n");
    fprintf(out, "}\n");
    /* Fire-and-forget from the start (cloud accept loops). Returns 0; no join. */
    fprintf(out, "static int64_t baga_go_bg(baga_par_fn fn, int64_t arg) {\n");
    fprintf(out, "    baga_JoinHandle *h = (baga_JoinHandle *)calloc(1, sizeof(baga_JoinHandle));\n");
    fprintf(out, "    if (!h) { fprintf(stderr, \"baga: go_bg: out of memory\\n\"); exit(1); }\n");
    fprintf(out, "    h->fn = fn; h->arg = arg; h->joined = 0; h->detached = 1;\n");
    fprintf(out, "    h->magic = BAGA_HANDLE_MAGIC;\n");
    fprintf(out, "    pthread_attr_t attr; pthread_attr_init(&attr);\n");
    fprintf(out, "    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);\n");
    fprintf(out, "    if (pthread_create(&h->th, &attr, baga_par_trampoline, h) != 0) {\n");
    fprintf(out, "        fprintf(stderr, \"baga: go_bg: pthread_create failed\\n\"); exit(1);\n");
    fprintf(out, "    }\n");
    fprintf(out, "    pthread_attr_destroy(&attr);\n");
    fprintf(out, "    return 0;\n");
    fprintf(out, "}\n");
    fprintf(out, "static int64_t baga_join(int64_t handle) {\n");
    fprintf(out, "    baga_JoinHandle *h = (baga_JoinHandle *)(intptr_t)handle;\n");
    fprintf(out, "    if (!h) return 0;\n");
    fprintf(out, "    if ((uintptr_t)h < 4096) {\n");
    fprintf(out, "        fprintf(stderr, \"baga: join: невалиден handle\\n\"); exit(1);\n");
    fprintf(out, "    }\n");
    fprintf(out, "    if (h->magic != BAGA_HANDLE_MAGIC) {\n");
    fprintf(out, "        fprintf(stderr, \"baga: join: невалиден handle\\n\"); exit(1);\n");
    fprintf(out, "    }\n");
    fprintf(out, "    if (h->detached == 1) {\n");
    fprintf(out, "        fprintf(stderr, \"baga: join: handle detached\\n\"); exit(1);\n");
    fprintf(out, "    }\n");
    fprintf(out, "    if (!h->joined) { pthread_join(h->th, NULL); h->joined = 1; }\n");
    fprintf(out, "    int64_t r = h->result; free(h); return r;\n");
    fprintf(out, "}\n");
    /* detach joinable handle: fire-and-forget. Race-safe with trampoline. */
    fprintf(out, "static int64_t baga_detach(int64_t handle) {\n");
    fprintf(out, "    baga_JoinHandle *h = (baga_JoinHandle *)(intptr_t)handle;\n");
    fprintf(out, "    if (!h || h->magic != BAGA_HANDLE_MAGIC || h->joined) return -1;\n");
    fprintf(out, "    int old = __sync_lock_test_and_set(&h->detached, 1);\n");
    fprintf(out, "    if (old == 2) { free(h); return 0; } /* already finished */\n");
    fprintf(out, "    if (old == 1) return 0; /* double detach */\n");
    fprintf(out, "    pthread_detach(h->th);\n");
    fprintf(out, "    return 0;\n");
    fprintf(out, "}\n");
    fprintf(out, "typedef struct {\n");
    fprintf(out, "    int64_t *buf; int64_t cap, len, head; int closed;\n");
    fprintf(out, "    pthread_mutex_t mu; pthread_cond_t not_empty, not_full;\n");
    fprintf(out, "} baga_Chan;\n");
    fprintf(out, "static int64_t baga_chan_new(int64_t cap) {\n");
    fprintf(out, "    if (cap < 1) cap = 1; /* M1: min buffer 1 (rendezvous = M2) */\n");
    fprintf(out, "    baga_Chan *c = (baga_Chan *)calloc(1, sizeof(baga_Chan));\n");
    fprintf(out, "    if (!c) { fprintf(stderr, \"baga: chan_new: oom\\n\"); exit(1); }\n");
    fprintf(out, "    c->cap = cap; c->buf = (int64_t *)malloc((size_t)cap * sizeof(int64_t));\n");
    fprintf(out, "    if (!c->buf) { fprintf(stderr, \"baga: chan_new: oom\\n\"); exit(1); }\n");
    fprintf(out, "    pthread_mutex_init(&c->mu, NULL);\n");
    fprintf(out, "    pthread_cond_init(&c->not_empty, NULL);\n");
    fprintf(out, "    pthread_cond_init(&c->not_full, NULL);\n");
    fprintf(out, "    return (int64_t)(intptr_t)c;\n");
    fprintf(out, "}\n");
    fprintf(out, "static int64_t baga_chan_send(int64_t ch, int64_t v) {\n");
    fprintf(out, "    baga_Chan *c = (baga_Chan *)(intptr_t)ch;\n");
    fprintf(out, "    if (!c) return -1;\n");
    fprintf(out, "    pthread_mutex_lock(&c->mu);\n");
    fprintf(out, "    while (c->len == c->cap && !c->closed) pthread_cond_wait(&c->not_full, &c->mu);\n");
    fprintf(out, "    if (c->closed) { pthread_mutex_unlock(&c->mu); return -1; }\n");
    fprintf(out, "    int64_t i = (c->head + c->len) %% c->cap;\n");
    fprintf(out, "    c->buf[i] = v; c->len++;\n");
    fprintf(out, "    pthread_cond_signal(&c->not_empty);\n");
    fprintf(out, "    pthread_mutex_unlock(&c->mu);\n");
    fprintf(out, "    return 0;\n");
    fprintf(out, "}\n");
    fprintf(out, "static int64_t baga_chan_recv(int64_t ch) {\n");
    fprintf(out, "    baga_Chan *c = (baga_Chan *)(intptr_t)ch;\n");
    fprintf(out, "    if (!c) return 0;\n");
    fprintf(out, "    pthread_mutex_lock(&c->mu);\n");
    fprintf(out, "    while (c->len == 0 && !c->closed) pthread_cond_wait(&c->not_empty, &c->mu);\n");
    fprintf(out, "    if (c->len == 0) { /* closed + empty */ pthread_mutex_unlock(&c->mu); return 0; }\n");
    fprintf(out, "    int64_t v = c->buf[c->head];\n");
    fprintf(out, "    c->head = (c->head + 1) %% c->cap; c->len--;\n");
    fprintf(out, "    pthread_cond_signal(&c->not_full);\n");
    fprintf(out, "    pthread_mutex_unlock(&c->mu);\n");
    fprintf(out, "    return v;\n");
    fprintf(out, "}\n");
    /* returns cell2(ok, value): ok=1 got value, ok=0 closed+empty (value=0) */
    fprintf(out, "static int64_t baga_chan_recv2(int64_t ch) {\n");
    fprintf(out, "    baga_Chan *c = (baga_Chan *)(intptr_t)ch;\n");
    fprintf(out, "    if (!c) return baga_cell2(0, 0);\n");
    fprintf(out, "    pthread_mutex_lock(&c->mu);\n");
    fprintf(out, "    while (c->len == 0 && !c->closed) pthread_cond_wait(&c->not_empty, &c->mu);\n");
    fprintf(out, "    if (c->len == 0) { pthread_mutex_unlock(&c->mu); return baga_cell2(0, 0); }\n");
    fprintf(out, "    int64_t v = c->buf[c->head];\n");
    fprintf(out, "    c->head = (c->head + 1) %% c->cap; c->len--;\n");
    fprintf(out, "    pthread_cond_signal(&c->not_full);\n");
    fprintf(out, "    pthread_mutex_unlock(&c->mu);\n");
    fprintf(out, "    return baga_cell2(1, v);\n");
    fprintf(out, "}\n");
    /* non-blocking: cell2(1,v)=ok, cell2(0,0)=empty, cell2(2,0)=closed empty */
    fprintf(out, "static int64_t baga_chan_try_recv(int64_t ch) {\n");
    fprintf(out, "    baga_Chan *c = (baga_Chan *)(intptr_t)ch;\n");
    fprintf(out, "    if (!c) return baga_cell2(2, 0);\n");
    fprintf(out, "    pthread_mutex_lock(&c->mu);\n");
    fprintf(out, "    if (c->len == 0) {\n");
    fprintf(out, "        int st = c->closed ? 2 : 0;\n");
    fprintf(out, "        pthread_mutex_unlock(&c->mu);\n");
    fprintf(out, "        return baga_cell2(st, 0);\n");
    fprintf(out, "    }\n");
    fprintf(out, "    int64_t v = c->buf[c->head];\n");
    fprintf(out, "    c->head = (c->head + 1) %% c->cap; c->len--;\n");
    fprintf(out, "    pthread_cond_signal(&c->not_full);\n");
    fprintf(out, "    pthread_mutex_unlock(&c->mu);\n");
    fprintf(out, "    return baga_cell2(1, v);\n");
    fprintf(out, "}\n");
    /* timed: cell2(1,v)=ok, cell2(0,0)=timeout, cell2(2,0)=closed empty */
    fprintf(out, "static int64_t baga_chan_recv_timeout(int64_t ch, int64_t ms) {\n");
    fprintf(out, "    baga_Chan *c = (baga_Chan *)(intptr_t)ch;\n");
    fprintf(out, "    if (!c) return baga_cell2(2, 0);\n");
    fprintf(out, "    if (ms < 0) ms = 0;\n");
    fprintf(out, "    struct timespec abs; clock_gettime(CLOCK_REALTIME, &abs);\n");
    fprintf(out, "    abs.tv_sec += ms / 1000;\n");
    fprintf(out, "    abs.tv_nsec += (ms %% 1000) * 1000000L;\n");
    fprintf(out, "    if (abs.tv_nsec >= 1000000000L) { abs.tv_sec++; abs.tv_nsec -= 1000000000L; }\n");
    fprintf(out, "    pthread_mutex_lock(&c->mu);\n");
    fprintf(out, "    while (c->len == 0 && !c->closed) {\n");
    fprintf(out, "        int rc = pthread_cond_timedwait(&c->not_empty, &c->mu, &abs);\n");
    fprintf(out, "        if (rc == ETIMEDOUT) { pthread_mutex_unlock(&c->mu); return baga_cell2(0, 0); }\n");
    fprintf(out, "    }\n");
    fprintf(out, "    if (c->len == 0) { pthread_mutex_unlock(&c->mu); return baga_cell2(2, 0); }\n");
    fprintf(out, "    int64_t v = c->buf[c->head];\n");
    fprintf(out, "    c->head = (c->head + 1) %% c->cap; c->len--;\n");
    fprintf(out, "    pthread_cond_signal(&c->not_full);\n");
    fprintf(out, "    pthread_mutex_unlock(&c->mu);\n");
    fprintf(out, "    return baga_cell2(1, v);\n");
    fprintf(out, "}\n");
    fprintf(out, "static int64_t baga_sleep_ms(int64_t ms) {\n");
    fprintf(out, "    if (ms <= 0) return 0;\n");
    fprintf(out, "    struct timespec ts; ts.tv_sec = ms / 1000; ts.tv_nsec = (ms %% 1000) * 1000000L;\n");
    fprintf(out, "    while (nanosleep(&ts, &ts) == -1 && errno == EINTR) {}\n");
    fprintf(out, "    return 0;\n");
    fprintf(out, "}\n");
    /* C1: process-global signal slot for graceful shutdown (SIGTERM/SIGINT). */
    fprintf(out, "static volatile sig_atomic_t baga_sig_seen = 0;\n");
    fprintf(out, "static void baga_sig_handler(int s) { baga_sig_seen = s; }\n");
    fprintf(out, "static int64_t baga_signal_watch(int64_t sig) {\n");
    fprintf(out, "    if (sig <= 0 || sig >= 64) return -1;\n");
    fprintf(out, "    struct sigaction sa; memset(&sa, 0, sizeof sa);\n");
    fprintf(out, "    sa.sa_handler = baga_sig_handler;\n");
    fprintf(out, "    sigemptyset(&sa.sa_mask);\n");
    fprintf(out, "    if (sigaction((int)sig, &sa, NULL) != 0) return -1;\n");
    fprintf(out, "    return 0;\n");
    fprintf(out, "}\n");
    fprintf(out, "static int64_t baga_signal_check(void) { return (int64_t)baga_sig_seen; }\n");
    fprintf(out, "static int64_t baga_signal_clear(void) {\n");
    fprintf(out, "    int64_t v = (int64_t)baga_sig_seen; baga_sig_seen = 0; return v;\n");
    fprintf(out, "}\n");
    fprintf(out, "static int64_t baga_signal_wait(int64_t ms) {\n");
    fprintf(out, "    if (baga_sig_seen) return (int64_t)baga_sig_seen;\n");
    fprintf(out, "    if (ms == 0) return 0;\n");
    fprintf(out, "    if (ms < 0) {\n");
    fprintf(out, "        while (!baga_sig_seen) {\n");
    fprintf(out, "            struct timespec ts; ts.tv_sec = 0; ts.tv_nsec = 50000000L;\n");
    fprintf(out, "            nanosleep(&ts, NULL);\n");
    fprintf(out, "        }\n");
    fprintf(out, "        return (int64_t)baga_sig_seen;\n");
    fprintf(out, "    }\n");
    fprintf(out, "    int64_t left = ms;\n");
    fprintf(out, "    while (left > 0) {\n");
    fprintf(out, "        if (baga_sig_seen) return (int64_t)baga_sig_seen;\n");
    fprintf(out, "        int64_t step = left > 50 ? 50 : left;\n");
    fprintf(out, "        struct timespec ts; ts.tv_sec = step / 1000;\n");
    fprintf(out, "        ts.tv_nsec = (step %% 1000) * 1000000L;\n");
    fprintf(out, "        nanosleep(&ts, NULL);\n");
    fprintf(out, "        left -= step;\n");
    fprintf(out, "    }\n");
    fprintf(out, "    return baga_sig_seen ? (int64_t)baga_sig_seen : 0;\n");
    fprintf(out, "}\n");
    fprintf(out, "static int64_t baga_signal_raise(int64_t sig) {\n");
    fprintf(out, "    return raise((int)sig) == 0 ? 0 : -1;\n");
    fprintf(out, "}\n");
    /* non-blocking select over two channels.
     * cell2(which, value): which=0|1 got value; which=2 neither ready;
     * which=3 both closed (and empty). Fair: prefers the emptier buffer first,
     * ties break to c0 then c1. */
    fprintf(out, "static int64_t baga_chan_select2(int64_t c0, int64_t c1) {\n");
    fprintf(out, "    baga_Chan *a = (baga_Chan *)(intptr_t)c0;\n");
    fprintf(out, "    baga_Chan *b = (baga_Chan *)(intptr_t)c1;\n");
    fprintf(out, "    if (!a && !b) return baga_cell2(3, 0);\n");
    fprintf(out, "    int64_t la = 0, lb = 0; int ca = 1, cb = 1;\n");
    fprintf(out, "    if (a) { pthread_mutex_lock(&a->mu); la = a->len; ca = a->closed; pthread_mutex_unlock(&a->mu); }\n");
    fprintf(out, "    if (b) { pthread_mutex_lock(&b->mu); lb = b->len; cb = b->closed; pthread_mutex_unlock(&b->mu); }\n");
    fprintf(out, "    if (la == 0 && lb == 0) {\n");
    fprintf(out, "        if ((!a || ca) && (!b || cb)) return baga_cell2(3, 0);\n");
    fprintf(out, "        return baga_cell2(2, 0);\n");
    fprintf(out, "    }\n");
    fprintf(out, "    int prefer0 = (la >= lb); /* take from fuller first (less likely to starve) */\n");
    fprintf(out, "    if (prefer0 && la > 0) {\n");
    fprintf(out, "        int64_t pr = baga_chan_try_recv(c0);\n");
    fprintf(out, "        if (baga_cell2_0(pr) == 1) return baga_cell2(0, baga_cell2_1(pr));\n");
    fprintf(out, "    }\n");
    fprintf(out, "    if (lb > 0) {\n");
    fprintf(out, "        int64_t pr = baga_chan_try_recv(c1);\n");
    fprintf(out, "        if (baga_cell2_0(pr) == 1) return baga_cell2(1, baga_cell2_1(pr));\n");
    fprintf(out, "    }\n");
    fprintf(out, "    if (la > 0) {\n");
    fprintf(out, "        int64_t pr = baga_chan_try_recv(c0);\n");
    fprintf(out, "        if (baga_cell2_0(pr) == 1) return baga_cell2(0, baga_cell2_1(pr));\n");
    fprintf(out, "    }\n");
    fprintf(out, "    return baga_cell2(2, 0);\n");
    fprintf(out, "}\n");
    /* Blocking select: wait until a value is ready or both channels are closed.
     * Alternates timed waits on each channel (by pointer order) so either side
     * can wake the waiter within ~5ms. Returns same codes as chan_select2. */
    fprintf(out, "static int64_t baga_chan_select2_wait(int64_t c0, int64_t c1) {\n");
    fprintf(out, "    int flip = 0;\n");
    fprintf(out, "    for (;;) {\n");
    fprintf(out, "        int64_t r = baga_chan_select2(c0, c1);\n");
    fprintf(out, "        int64_t w = baga_cell2_0(r);\n");
    fprintf(out, "        if (w != 2) return r;\n");
    fprintf(out, "        baga_Chan *a = (baga_Chan *)(intptr_t)c0;\n");
    fprintf(out, "        baga_Chan *b = (baga_Chan *)(intptr_t)c1;\n");
    fprintf(out, "        baga_Chan *wa = NULL, *wb = NULL;\n");
    fprintf(out, "        if (a && b) {\n");
    fprintf(out, "            if ((uintptr_t)a < (uintptr_t)b) { wa = a; wb = b; }\n");
    fprintf(out, "            else { wa = b; wb = a; }\n");
    fprintf(out, "        } else { wa = a ? a : b; }\n");
    fprintf(out, "        baga_Chan *wait = (flip && wb) ? wb : wa;\n");
    fprintf(out, "        flip = !flip;\n");
    fprintf(out, "        if (!wait) return baga_cell2(3, 0);\n");
    fprintf(out, "        struct timespec abs; clock_gettime(CLOCK_REALTIME, &abs);\n");
    fprintf(out, "        abs.tv_nsec += 5000000L; /* 5ms */\n");
    fprintf(out, "        if (abs.tv_nsec >= 1000000000L) { abs.tv_sec++; abs.tv_nsec -= 1000000000L; }\n");
    fprintf(out, "        pthread_mutex_lock(&wait->mu);\n");
    fprintf(out, "        if (wait->len == 0 && !wait->closed)\n");
    fprintf(out, "            pthread_cond_timedwait(&wait->not_empty, &wait->mu, &abs);\n");
    fprintf(out, "        pthread_mutex_unlock(&wait->mu);\n");
    fprintf(out, "    }\n");
    fprintf(out, "}\n");
    /* Timed select: like select2_wait but give up after ms (return which=2). */
    fprintf(out, "static int64_t baga_chan_select2_timeout(int64_t c0, int64_t c1, int64_t ms) {\n");
    fprintf(out, "    if (ms < 0) ms = 0;\n");
    fprintf(out, "    struct timespec deadline; clock_gettime(CLOCK_REALTIME, &deadline);\n");
    fprintf(out, "    deadline.tv_sec += ms / 1000;\n");
    fprintf(out, "    deadline.tv_nsec += (ms %% 1000) * 1000000L;\n");
    fprintf(out, "    if (deadline.tv_nsec >= 1000000000L) { deadline.tv_sec++; deadline.tv_nsec -= 1000000000L; }\n");
    fprintf(out, "    int flip = 0;\n");
    fprintf(out, "    for (;;) {\n");
    fprintf(out, "        int64_t r = baga_chan_select2(c0, c1);\n");
    fprintf(out, "        int64_t w = baga_cell2_0(r);\n");
    fprintf(out, "        if (w != 2) return r;\n");
    fprintf(out, "        struct timespec now; clock_gettime(CLOCK_REALTIME, &now);\n");
    fprintf(out, "        if (now.tv_sec > deadline.tv_sec ||\n");
    fprintf(out, "            (now.tv_sec == deadline.tv_sec && now.tv_nsec >= deadline.tv_nsec))\n");
    fprintf(out, "            return baga_cell2(2, 0);\n");
    fprintf(out, "        baga_Chan *a = (baga_Chan *)(intptr_t)c0;\n");
    fprintf(out, "        baga_Chan *b = (baga_Chan *)(intptr_t)c1;\n");
    fprintf(out, "        baga_Chan *wa = NULL, *wb = NULL;\n");
    fprintf(out, "        if (a && b) {\n");
    fprintf(out, "            if ((uintptr_t)a < (uintptr_t)b) { wa = a; wb = b; }\n");
    fprintf(out, "            else { wa = b; wb = a; }\n");
    fprintf(out, "        } else { wa = a ? a : b; }\n");
    fprintf(out, "        baga_Chan *wait = (flip && wb) ? wb : wa;\n");
    fprintf(out, "        flip = !flip;\n");
    fprintf(out, "        if (!wait) return baga_cell2(3, 0);\n");
    fprintf(out, "        pthread_mutex_lock(&wait->mu);\n");
    fprintf(out, "        if (wait->len == 0 && !wait->closed)\n");
    fprintf(out, "            pthread_cond_timedwait(&wait->not_empty, &wait->mu, &deadline);\n");
    fprintf(out, "        pthread_mutex_unlock(&wait->mu);\n");
    fprintf(out, "    }\n");
    fprintf(out, "}\n");
    fprintf(out, "static int64_t baga_chan_close(int64_t ch) {\n");
    fprintf(out, "    baga_Chan *c = (baga_Chan *)(intptr_t)ch;\n");
    fprintf(out, "    if (!c) return -1;\n");
    fprintf(out, "    pthread_mutex_lock(&c->mu);\n");
    fprintf(out, "    c->closed = 1;\n");
    fprintf(out, "    pthread_cond_broadcast(&c->not_empty);\n");
    fprintf(out, "    pthread_cond_broadcast(&c->not_full);\n");
    fprintf(out, "    pthread_mutex_unlock(&c->mu);\n");
    fprintf(out, "    return 0;\n");
    fprintf(out, "}\n");
    fprintf(out, "static int64_t baga_chan_len(int64_t ch) {\n");
    fprintf(out, "    baga_Chan *c = (baga_Chan *)(intptr_t)ch;\n");
    fprintf(out, "    if (!c) return 0;\n");
    fprintf(out, "    pthread_mutex_lock(&c->mu);\n");
    fprintf(out, "    int64_t n = c->len;\n");
    fprintf(out, "    pthread_mutex_unlock(&c->mu);\n");
    fprintf(out, "    return n;\n");
    fprintf(out, "}\n");
    /* mutex — opaque i64 handle с double-lock детекция */
    fprintf(out, "typedef struct { pthread_mutex_t mu; pthread_t owner; int locked; } baga_Mutex;\n");
    fprintf(out, "static int64_t baga_mutex_new(void) {\n");
    fprintf(out, "    baga_Mutex *m = (baga_Mutex *)calloc(1, sizeof(baga_Mutex));\n");
    fprintf(out, "    if (!m) { fprintf(stderr, \"baga: mutex_new: oom\\n\"); exit(1); }\n");
    fprintf(out, "    pthread_mutex_init(&m->mu, NULL);\n");
    fprintf(out, "    return (int64_t)(intptr_t)m;\n");
    fprintf(out, "}\n");
    fprintf(out, "static int64_t baga_mutex_lock(int64_t h) {\n");
    fprintf(out, "    baga_Mutex *m = (baga_Mutex *)(intptr_t)h;\n");
    fprintf(out, "    if (!m) return -1;\n");
    fprintf(out, "    if (m->locked && pthread_equal(m->owner, pthread_self())) {\n");
    fprintf(out, "        fprintf(stderr, \"baga: mutex_lock: double-lock от същата нишка\\n\"); exit(1);\n");
    fprintf(out, "    }\n");
    fprintf(out, "    int rc = pthread_mutex_lock(&m->mu);\n");
    fprintf(out, "    m->owner = pthread_self(); m->locked = 1;\n");
    fprintf(out, "    return (int64_t)rc;\n");
    fprintf(out, "}\n");
    fprintf(out, "static int64_t baga_mutex_unlock(int64_t h) {\n");
    fprintf(out, "    baga_Mutex *m = (baga_Mutex *)(intptr_t)h;\n");
    fprintf(out, "    if (!m) return -1;\n");
    fprintf(out, "    m->locked = 0;\n");
    fprintf(out, "    return (int64_t)pthread_mutex_unlock(&m->mu);\n");
    fprintf(out, "}\n");
    /* Bounded worker pool: pool_map(fn, Vec<i64>, nworkers) -> Vec<i64> */
    fprintf(out, "typedef struct {\n");
    fprintf(out, "    baga_par_fn fn; baga_Vec *in; int64_t jobs; int64_t results;\n");
    fprintf(out, "} baga_PoolCtx;\n");
    fprintf(out, "static int64_t baga_pool_worker(int64_t ctx_h) {\n");
    fprintf(out, "    baga_PoolCtx *ctx = (baga_PoolCtx *)(intptr_t)ctx_h;\n");
    fprintf(out, "    for (;;) {\n");
    fprintf(out, "        int64_t pr = baga_chan_recv2(ctx->jobs);\n");
    fprintf(out, "        if (baga_cell2_0(pr) == 0) break; /* closed + empty */\n");
    fprintf(out, "        int64_t idx = baga_cell2_1(pr);\n");
    fprintf(out, "        int64_t arg = baga_vec_get_i64(ctx->in, idx);\n");
    fprintf(out, "        int64_t r = ctx->fn(arg);\n");
    fprintf(out, "        baga_chan_send(ctx->results, baga_cell2(idx, r));\n");
    fprintf(out, "    }\n");
    fprintf(out, "    return 0;\n");
    fprintf(out, "}\n");
    fprintf(out, "static baga_Vec *baga_pool_map(baga_par_fn fn, baga_Vec *in, int64_t nw) {\n");
    fprintf(out, "    int64_t n = baga_vec_len(in);\n");
    fprintf(out, "    baga_Vec *out = baga_vec_new();\n");
    fprintf(out, "    if (n <= 0) return out;\n");
    fprintf(out, "    for (int64_t i = 0; i < n; i++) baga_vec_push_i64(out, 0);\n");
    fprintf(out, "    if (nw < 1) nw = 1;\n");
    fprintf(out, "    if (nw > n) nw = n;\n");
    fprintf(out, "    int64_t jobs = baga_chan_new(n);\n");
    fprintf(out, "    int64_t results = baga_chan_new(n);\n");
    fprintf(out, "    baga_PoolCtx *ctx = (baga_PoolCtx *)calloc(1, sizeof(baga_PoolCtx));\n");
    fprintf(out, "    if (!ctx) { fprintf(stderr, \"baga: pool_map: oom\\n\"); exit(1); }\n");
    fprintf(out, "    ctx->fn = fn; ctx->in = in; ctx->jobs = jobs; ctx->results = results;\n");
    fprintf(out, "    int64_t *hs = (int64_t *)malloc((size_t)nw * sizeof(int64_t));\n");
    fprintf(out, "    if (!hs) { fprintf(stderr, \"baga: pool_map: oom\\n\"); exit(1); }\n");
    fprintf(out, "    for (int64_t w = 0; w < nw; w++)\n");
    fprintf(out, "        hs[w] = baga_go(baga_pool_worker, (int64_t)(intptr_t)ctx);\n");
    fprintf(out, "    for (int64_t i = 0; i < n; i++) baga_chan_send(jobs, i);\n");
    fprintf(out, "    baga_chan_close(jobs);\n");
    fprintf(out, "    for (int64_t i = 0; i < n; i++) {\n");
    fprintf(out, "        int64_t pair = baga_chan_recv(results);\n");
    fprintf(out, "        int64_t idx = baga_cell2_0(pair);\n");
    fprintf(out, "        int64_t r = baga_cell2_1(pair);\n");
    fprintf(out, "        if (idx >= 0 && idx < n) baga_vec_set_i64(out, idx, r);\n");
    fprintf(out, "    }\n");
    fprintf(out, "    for (int64_t w = 0; w < nw; w++) baga_join(hs[w]);\n");
    fprintf(out, "    free(hs); free(ctx);\n");
    fprintf(out, "    return out;\n");
    fprintf(out, "}\n");
    fprintf(out, "\n");

    /* enums first (plain only — sum enum-ите зависят от struct typedef-овете) */
    for (int i = 0; i < program->items.len; i++) {
        Node *item = program->items.data[i];
        if (item->kind != NODE_ENUM) continue;
        int is_sum = 0;
        for (int j = 0; j < item->n_variants; j++)
            if (item->enum_payloads && item->enum_payloads[j]) is_sum = 1;
        if (is_sum) continue;
        char *em = mangle_name(item->enum_name);
        fprintf(out, "typedef enum {\n");
        for (int j = 0; j < item->n_variants; j++) {
            char *vm = mangle_name(item->enum_variants[j]);
            fprintf(out, "    %s_%s = %d,\n", em, vm, j);
            free(vm);
        }
        fprintf(out, "} %s;\n\n", em);
        free(em);
    }

    /* structs */
    for (int i = 0; i < program->items.len; i++) {
        Node *item = program->items.data[i];
        if (item->kind == NODE_STRUCT)
            emit_struct(cg, item);
    }

    /* L3 sum enums: tagged struct + union + static inline конструктори */
    for (int i = 0; i < program->items.len; i++) {
        Node *item = program->items.data[i];
        if (item->kind != NODE_ENUM) continue;
        int is_sum = 0;
        for (int j = 0; j < item->n_variants; j++)
            if (item->enum_payloads && item->enum_payloads[j]) is_sum = 1;
        if (!is_sum) continue;
        char *em = mangle_name(item->enum_name);
        fprintf(out, "typedef struct {\n    int tag;\n    union {\n");
        for (int j = 0; j < item->n_variants; j++) {
            if (!item->enum_payloads[j]) continue;
            char *vm = mangle_name(item->enum_variants[j]);
            fprintf(out, "        ");
            emit_type(cg, item->enum_payloads[j]);
            fprintf(out, " v_%s;\n", vm);
            free(vm);
        }
        fprintf(out, "    } u;\n} %s;\n\n", em);
        for (int j = 0; j < item->n_variants; j++) {
            char *vm = mangle_name(item->enum_variants[j]);
            if (item->enum_payloads[j]) {
                fprintf(out, "static inline %s %s__%s(", em, em, vm);
                emit_type(cg, item->enum_payloads[j]);
                fprintf(out, " a0) {\n    %s r; r.tag = %d; r.u.v_%s = a0; return r;\n}\n\n",
                        em, j, vm);
            } else {
                fprintf(out, "static inline %s %s__%s(void) {\n    %s r; r.tag = %d; return r;\n}\n\n",
                        em, em, vm, em, j);
            }
            free(vm);
        }
        free(em);
    }

    /* forward declarations */
    emit_forward_decls(cg, program);

    /* function definitions (L5: през memstream — env struct-овете и
     * wrapper-ите на ламбдите/функциите (lambda_out) трябва да стоят ПРЕДИ
     * телата в изхода, без AST pre-pass) */
    char *fbuf = NULL, *lbuf = NULL;
    size_t fsize = 0, lsize = 0;
    FILE *fa = open_memstream(&fbuf, &fsize);
    cg->lambda_out = open_memstream(&lbuf, &lsize);
    FILE *saved_out = cg->out;
    cg->out = fa;
    for (int i = 0; i < program->items.len; i++) {
        Node *item = program->items.data[i];
        if (item->kind == NODE_FN && item->fn_body)
            emit_fn(cg, item);
    }
    fclose(fa);
    fclose(cg->lambda_out);
    cg->out = saved_out;
    cg->lambda_out = NULL;
    fwrite(lbuf, 1, lsize, out);
    fwrite(fbuf, 1, fsize, out);
    free(lbuf);
    free(fbuf);

    if (cg->test_specs) {
        emit_test_driver(cg, program);
    } else {
        /* C main only when the program defines main (libraries may omit it) */
        Node *main_fn = NULL;
        for (int i = 0; i < program->items.len; i++) {
            Node *it = program->items.data[i];
            if (it->kind == NODE_FN && it->fn_body && it->fn_name &&
                strcmp(it->fn_name, "main") == 0) {
                main_fn = it;
                break;
            }
        }
        if (main_fn) {
            /* main -> i64: the returned value is the process exit code
             * (kvbaga K3 — before this the wrapper swallowed it) */
            Node *rt = main_fn->ret_type;
            if (rt && rt->kind == NODE_TYPE_EFFECT) rt = rt->inner_type;
            int ret_int = rt && rt->kind == NODE_TYPE && rt->type_name &&
                (strcmp(rt->type_name, "i64") == 0 ||
                 strcmp(rt->type_name, "i32") == 0);
            fprintf(out, "int main(int argc, char **argv) {\n");
            fprintf(out, "    baga_argc = argc; baga_argv = argv;\n");
            if (ret_int) {
                fprintf(out, "    return (int)b_main();\n");
            } else {
                fprintf(out, "    b_main();\n");
                fprintf(out, "    return 0;\n");
            }
            fprintf(out, "}\n");
        }
    }
}
