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
                    fprintf(f, "\\x%02x", *p);
                break;
        }
    }
    fputc('"', f);
}

/* ---- expression emission ---- */

static void emit_expr(Codegen *cg, Node *n);

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
            /* check if it's an enum variant */
            int found_variant = 0;
            if (cg->program) {
                for (int i = 0; i < cg->program->items.len && !found_variant; i++) {
                    Node *item = cg->program->items.data[i];
                    if (item->kind != NODE_ENUM) continue;
                    for (int j = 0; j < item->n_variants; j++) {
                        if (strcmp(item->enum_variants[j], n->name) == 0) {
                            char *em = mangle_name(item->enum_name);
                            char *vm = mangle_name(item->enum_variants[j]);
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
                /* типизирани вектори: helper по елементния тип на вектора */
                if (strcmp(bn, "vec_push") == 0 || strcmp(bn, "vec_get") == 0 ||
                    strcmp(bn, "vec_set") == 0) {
                    Type *vt = n->args.len > 0 ? n->args.data[0]->type : NULL;
                    const char *suf = "i64";
                    if (vt && vt->kind == TYPE_VEC && vt->elem) {
                        if (vt->elem->kind == TYPE_STR) suf = "str";
                        else if (vt->elem->kind == TYPE_F64) suf = "f64";
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
                    const char *suf = "i64";
                    if (vt && vt->kind == TYPE_VEC && vt->elem) {
                        if (vt->elem->kind == TYPE_STR) suf = "str";
                        else if (vt->elem->kind == TYPE_F64) suf = "f64";
                    }
                    fprintf(f, "baga_%s_%s(", bn, suf);
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
                    {"bytes_of_str","baga_bytes_from_str"},
                    {"str_of_bytes","baga_bytes_to_str"},
                    {"hex_encode",  "baga_hex_encode"},
                    {"hex_decode",  "baga_hex_decode"},
                    {"join",        "baga_join"},
                    {"detach",      "baga_detach"},
                    {"chan_new",    "baga_chan_new"},
                    {"chan_send",   "baga_chan_send"},
                    {"chan_recv",   "baga_chan_recv"},
                    {"chan_recv2",  "baga_chan_recv2"},
                    {"chan_close",  "baga_chan_close"},
                    {"chan_len",    "baga_chan_len"},
                    {"mutex_new",   "baga_mutex_new"},
                    {"mutex_lock",  "baga_mutex_lock"},
                    {"mutex_unlock","baga_mutex_unlock"},
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

        case NODE_MATCH: {
            /* GCC statement expression */
            int tmp = cg->tmp_counter++;
            /* determine result C type from inferred type */
            const char *ctype = "int64_t";
            if (n->type) {
                switch (n->type->kind) {
                    case TYPE_STR:  ctype = "const char *"; break;
                    case TYPE_F64:  ctype = "double"; break;
                    case TYPE_BOOL: ctype = "int"; break;
                    default:        ctype = "int64_t"; break;
                }
            }
            fprintf(f, "({ %s _mr%d = 0; int64_t _mv%d = ", ctype, tmp, tmp);
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
                            fprintf(f, "_mr%d = ", tmp);
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
            fprintf(f, "_mr%d; })", tmp);
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
                    case TYPE_VEC:
                        fprintf(f, "baga_Vec *");
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

    if (!ensures_spec) return;

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
    fprintf(out, "#include <pthread.h>\n\n");

    /* runtime helpers */
    fprintf(out, "static void baga_print_i64(int64_t v) { printf(\"%%lld\\n\", (long long)v); }\n");
    fprintf(out, "static void baga_print_f64(double v)  { printf(\"%%g\\n\", v); }\n");
    fprintf(out, "static void baga_print_str(const char *s) { printf(\"%%s\\n\", s); }\n");
    fprintf(out, "static void baga_write(const char *s) { printf(\"%%s\", s); }\n");
    fprintf(out, "static int64_t baga_len(const char *s) { return (int64_t)strlen(s); }\n");
    fprintf(out, "static int64_t baga_char_at(const char *s, int64_t i) { return (int64_t)(unsigned char)s[i]; }\n");
    fprintf(out, "static const char *baga_substr(const char *s, int64_t a, int64_t b) {\n");
    fprintf(out, "    int64_t n = b - a; if (n < 0) n = 0;\n");
    fprintf(out, "    char *r = malloc((size_t)n + 1); memcpy(r, s + a, (size_t)n); r[n] = 0; return r;\n");
    fprintf(out, "}\n");
    fprintf(out, "static const char *baga_concat(const char *a, const char *b) {\n");
    fprintf(out, "    size_t la = strlen(a), lb = strlen(b);\n");
    fprintf(out, "    char *r = malloc(la + lb + 1); memcpy(r, a, la); memcpy(r + la, b, lb + 1); return r;\n");
    fprintf(out, "}\n");
    fprintf(out, "static const char *baga_read_file(const char *path) {\n");
    fprintf(out, "    FILE *f = fopen(path, \"rb\"); if (!f) return \"\";\n");
    fprintf(out, "    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);\n");
    fprintf(out, "    char *buf = malloc((size_t)sz + 1); fread(buf, 1, (size_t)sz, f); buf[sz] = 0; fclose(f); return buf;\n");
    fprintf(out, "}\n");
    fprintf(out, "static const char *baga_chr(int64_t c) { char *r = malloc(2); r[0] = (char)c; r[1] = 0; return r; }\n");
    fprintf(out, "static int64_t baga_ord(const char *s) { return s[0] ? (int64_t)(unsigned char)s[0] : 0; }\n");
    fprintf(out, "static const char *baga_i64_to_str(int64_t x) { char *r = malloc(24); snprintf(r, 24, \"%%lld\", (long long)x); return r; }\n");
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
    fprintf(out, "static int64_t baga_bytes_at(baga_bytes b, int64_t i) { return (int64_t)b.data[i]; }\n");
    fprintf(out, "static baga_bytes baga_bytes_slice(baga_bytes b, int64_t a, int64_t c) {\n");
    fprintf(out, "    if (a < 0) a = 0; if (c > b.len) c = b.len; if (c < a) c = a;\n");
    fprintf(out, "    baga_bytes r; r.len = c - a; r.data = malloc((size_t)(r.len ? r.len : 1));\n");
    fprintf(out, "    memcpy(r.data, b.data + a, (size_t)r.len); return r; }\n");
    fprintf(out, "static baga_bytes baga_bytes_concat(baga_bytes a, baga_bytes b) {\n");
    fprintf(out, "    baga_bytes r; r.len = a.len + b.len; r.data = malloc((size_t)(r.len ? r.len : 1));\n");
    fprintf(out, "    memcpy(r.data, a.data, (size_t)a.len); memcpy(r.data + a.len, b.data, (size_t)b.len); return r; }\n");
    fprintf(out, "static baga_bytes baga_bytes_from_str(const char *s) {\n");
    fprintf(out, "    int64_t n = (int64_t)strlen(s); baga_bytes r; r.len = n; r.data = malloc((size_t)(n ? n : 1));\n");
    fprintf(out, "    memcpy(r.data, s, (size_t)n); return r; }\n");
    fprintf(out, "static baga_bytes baga_bytes_lit(const unsigned char *d, int64_t n) {\n");
    fprintf(out, "    baga_bytes r; r.len = n; r.data = malloc((size_t)(n ? n : 1));\n");
    fprintf(out, "    memcpy(r.data, d, (size_t)n); return r; }\n");
    fprintf(out, "static const char *baga_bytes_to_str(baga_bytes b) {\n");
    fprintf(out, "    char *r = malloc((size_t)b.len + 1); memcpy(r, b.data, (size_t)b.len); r[b.len] = 0; return r; }\n");
    fprintf(out, "static int baga_hex_val(int c) {\n");
    fprintf(out, "    if (c >= '0' && c <= '9') return c - '0';\n");
    fprintf(out, "    if (c >= 'a' && c <= 'f') return c - 'a' + 10;\n");
    fprintf(out, "    if (c >= 'A' && c <= 'F') return c - 'A' + 10;\n");
    fprintf(out, "    return -1; }\n");
    fprintf(out, "static const char *baga_hex_encode(baga_bytes b) {\n");
    fprintf(out, "    static const char *hx = \"0123456789abcdef\";\n");
    fprintf(out, "    char *r = malloc((size_t)b.len * 2 + 1);\n");
    fprintf(out, "    for (int64_t i = 0; i < b.len; i++) { r[i*2] = hx[b.data[i] >> 4]; r[i*2+1] = hx[b.data[i] & 15]; }\n");
    fprintf(out, "    r[b.len * 2] = 0; return r; }\n");
    fprintf(out, "static baga_bytes baga_hex_decode(const char *s) {\n");
    fprintf(out, "    int64_t n = (int64_t)strlen(s); unsigned char *buf = malloc((size_t)(n / 2 + 1)); int64_t len = 0;\n");
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
    fprintf(out, "    baga_Vec *v = malloc(sizeof(baga_Vec));\n");
    fprintf(out, "    v->cap = 8; v->len = 0; v->data = malloc(8 * sizeof(void *)); return v;\n");
    fprintf(out, "}\n");
    fprintf(out, "static void baga_vec_grow(baga_Vec *v) {\n");
    fprintf(out, "    if (v->len == v->cap) { v->cap *= 2; v->data = realloc(v->data, (size_t)v->cap * sizeof(void *)); }\n");
    fprintf(out, "}\n");
    fprintf(out, "static void baga_vec_push_i64(baga_Vec *v, int64_t x) { baga_vec_grow(v); v->data[v->len++] = (void *)(intptr_t)x; }\n");
    fprintf(out, "static int64_t baga_vec_get_i64(baga_Vec *v, int64_t i) { return (int64_t)(intptr_t)v->data[i]; }\n");
    fprintf(out, "static void baga_vec_set_i64(baga_Vec *v, int64_t i, int64_t x) { v->data[i] = (void *)(intptr_t)x; }\n");
    fprintf(out, "static void baga_vec_push_str(baga_Vec *v, const char *s) { baga_vec_grow(v); v->data[v->len++] = (void *)s; }\n");
    fprintf(out, "static const char *baga_vec_get_str(baga_Vec *v, int64_t i) { return (const char *)v->data[i]; }\n");
    fprintf(out, "static void baga_vec_set_str(baga_Vec *v, int64_t i, const char *s) { v->data[i] = (void *)s; }\n");
    fprintf(out, "static void baga_vec_push_f64(baga_Vec *v, double x) { union { double d; void *p; } u; u.d = x; baga_vec_grow(v); v->data[v->len++] = u.p; }\n");
    fprintf(out, "static double baga_vec_get_f64(baga_Vec *v, int64_t i) { union { double d; void *p; } u; u.p = v->data[i]; return u.d; }\n");
    fprintf(out, "static void baga_vec_set_f64(baga_Vec *v, int64_t i, double x) { union { double d; void *p; } u; u.d = x; v->data[i] = u.p; }\n");
    fprintf(out, "static int64_t baga_vec_len(baga_Vec *v) { return v->len; }\n");
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
    fprintf(out, "\n/* arena allocator: bump allocation, free-all-at-once */\n");
    fprintf(out, "typedef struct { char *base; int64_t used; int64_t cap; } baga_Arena;\n");
    fprintf(out, "static int64_t baga_arena_new(void) {\n");
    fprintf(out, "    baga_Arena *a = malloc(sizeof(baga_Arena));\n");
    fprintf(out, "    a->cap = 65536; a->used = 0; a->base = malloc((size_t)a->cap);\n");
    fprintf(out, "    return (int64_t)(intptr_t)a;\n");
    fprintf(out, "}\n");
    fprintf(out, "static int64_t baga_arena_alloc(int64_t h, int64_t size) {\n");
    fprintf(out, "    baga_Arena *a = (baga_Arena *)(intptr_t)h;\n");
    fprintf(out, "    if (a->used + size > a->cap) {\n");
    fprintf(out, "        int64_t nc = (a->used + size) * 2;\n");
    fprintf(out, "        a->base = realloc(a->base, (size_t)nc); a->cap = nc;\n");
    fprintf(out, "    }\n");
    fprintf(out, "    char *p = a->base + a->used; a->used += size;\n");
    fprintf(out, "    return (int64_t)(intptr_t)p;\n");
    fprintf(out, "}\n");
    fprintf(out, "static void baga_arena_reset(int64_t h) {\n");
    fprintf(out, "    baga_Arena *a = (baga_Arena *)(intptr_t)h; a->used = 0;\n");
    fprintf(out, "}\n");
    fprintf(out, "static void baga_arena_free(int64_t h) {\n");
    fprintf(out, "    baga_Arena *a = (baga_Arena *)(intptr_t)h;\n");
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
    fprintf(out, "static int64_t baga_cell2_0(int64_t h) { return ((int64_t *)(intptr_t)h)[0]; }\n");
    fprintf(out, "static int64_t baga_cell2_1(int64_t h) { return ((int64_t *)(intptr_t)h)[1]; }\n");
    fprintf(out, "typedef int64_t (*baga_par_fn)(int64_t);\n");
    fprintf(out, "typedef struct {\n");
    fprintf(out, "    baga_par_fn fn; int64_t arg; int64_t result; pthread_t th;\n");
    fprintf(out, "    int joined; int detached;\n");
    fprintf(out, "} baga_JoinHandle;\n");
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
    fprintf(out, "    if (h->detached == 1) {\n");
    fprintf(out, "        fprintf(stderr, \"baga: join: handle detached\\n\"); exit(1);\n");
    fprintf(out, "    }\n");
    fprintf(out, "    if (!h->joined) { pthread_join(h->th, NULL); h->joined = 1; }\n");
    fprintf(out, "    int64_t r = h->result; free(h); return r;\n");
    fprintf(out, "}\n");
    /* detach joinable handle: fire-and-forget. Race-safe with trampoline. */
    fprintf(out, "static int64_t baga_detach(int64_t handle) {\n");
    fprintf(out, "    baga_JoinHandle *h = (baga_JoinHandle *)(intptr_t)handle;\n");
    fprintf(out, "    if (!h || h->joined) return -1;\n");
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
    /* mutex — opaque i64 handle */
    fprintf(out, "static int64_t baga_mutex_new(void) {\n");
    fprintf(out, "    pthread_mutex_t *m = (pthread_mutex_t *)malloc(sizeof(pthread_mutex_t));\n");
    fprintf(out, "    if (!m) { fprintf(stderr, \"baga: mutex_new: oom\\n\"); exit(1); }\n");
    fprintf(out, "    pthread_mutex_init(m, NULL);\n");
    fprintf(out, "    return (int64_t)(intptr_t)m;\n");
    fprintf(out, "}\n");
    fprintf(out, "static int64_t baga_mutex_lock(int64_t h) {\n");
    fprintf(out, "    pthread_mutex_t *m = (pthread_mutex_t *)(intptr_t)h;\n");
    fprintf(out, "    if (!m) return -1;\n");
    fprintf(out, "    return (int64_t)pthread_mutex_lock(m);\n");
    fprintf(out, "}\n");
    fprintf(out, "static int64_t baga_mutex_unlock(int64_t h) {\n");
    fprintf(out, "    pthread_mutex_t *m = (pthread_mutex_t *)(intptr_t)h;\n");
    fprintf(out, "    if (!m) return -1;\n");
    fprintf(out, "    return (int64_t)pthread_mutex_unlock(m);\n");
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

    /* enums first */
    for (int i = 0; i < program->items.len; i++) {
        Node *item = program->items.data[i];
        if (item->kind != NODE_ENUM) continue;
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

    /* forward declarations */
    emit_forward_decls(cg, program);

    /* function definitions */
    for (int i = 0; i < program->items.len; i++) {
        Node *item = program->items.data[i];
        if (item->kind == NODE_FN && item->fn_body)
            emit_fn(cg, item);
    }

    if (cg->test_specs) {
        emit_test_driver(cg, program);
    } else {
        /* C main → calls baga main */
        fprintf(out, "int main(int argc, char **argv) {\n");
        fprintf(out, "    baga_argc = argc; baga_argv = argv;\n");
        fprintf(out, "    b_main();\n");
        fprintf(out, "    return 0;\n");
        fprintf(out, "}\n");
    }
}
