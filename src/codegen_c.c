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
            else if (strcmp(ty->type_name, "void") == 0) fprintf(f, "void");
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
            /* Phase 1: arrays as pointers */
            emit_type(cg, ty->inner_type);
            fprintf(f, " *");
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
           strcmp(callee->name, "println") == 0;
}

static void emit_print(Codegen *cg, Node *n) {
    FILE *f = cg->out;
    if (n->args.len == 0) {
        emit_indent(cg);
        fprintf(f, "printf(\"\\n\");\n");
        return;
    }

    /* print each argument on its own line, dispatching on inferred type */
    for (int i = 0; i < n->args.len; i++) {
        Node *arg = n->args.data[i];
        Type *at = arg->type;
        TypeKind ak = at ? at->kind : TYPE_I64;

        emit_indent(cg);

        if (ak == TYPE_STR || arg->kind == NODE_STR_LIT) {
            if (arg->kind == NODE_STR_LIT) {
                fprintf(f, "printf(\"%%s\\n\", ");
                emit_c_string(f, arg->str_val);
                fprintf(f, ");\n");
            } else {
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
            fprintf(f, "%g", n->float_val);
            break;

        case NODE_STR_LIT:
            emit_c_string(f, n->str_val);
            break;

        case NODE_BOOL_LIT:
            fprintf(f, "%d", n->bool_val);
            break;

        case NODE_IDENT: {
            char *m = mangle_name(n->name);
            fprintf(f, "%s", m);
            free(m);
            break;
        }

        case NODE_BINARY:
            fprintf(f, "(");
            emit_expr(cg, n->left);
            fprintf(f, " %s ", binop_c(n->bin_op));
            emit_expr(cg, n->right);
            fprintf(f, ")");
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
            if (is_print_call(n)) {
                /* handled at statement level */
                emit_print(cg, n);
                return;
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

        case NODE_MATCH: {
            /* GCC statement expression */
            int tmp = cg->tmp_counter++;
            fprintf(f, "({ int64_t _mr%d = 0; int64_t _mv%d = ", tmp, tmp);
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
            if (is_print_call(n->expr)) {
                emit_print(cg, n->expr);
            } else {
                emit_indent(cg);
                emit_expr(cg, n->expr);
                fprintf(f, ";\n");
            }
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

/* ---- function emission ---- */

static void emit_fn(Codegen *cg, Node *fn) {
    FILE *f = cg->out;

    /* return type */
    if (fn->ret_type) {
        emit_type(cg, fn->ret_type);
    } else {
        fprintf(f, "void");
    }
    fprintf(f, " ");

    /* name */
    char *m = mangle_name(fn->fn_name);
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
        emit_block(cg, fn->fn_body);
    } else {
        fprintf(f, "{}");
    }
    fprintf(f, "\n\n");
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

void codegen_c(Codegen *cg, Node *program, FILE *out) {
    cg->out = out;
    cg->indent = 0;
    cg->tmp_counter = 0;

    /* header */
    fprintf(out, "/* Генериран от компилатора на Бага. Фаза 1. */\n");
    fprintf(out, "#include <stdio.h>\n");
    fprintf(out, "#include <stdlib.h>\n");
    fprintf(out, "#include <stdint.h>\n");
    fprintf(out, "#include <string.h>\n\n");

    /* runtime helpers */
    fprintf(out, "static void baga_print_i64(int64_t v) { printf(\"%%lld\\n\", (long long)v); }\n");
    fprintf(out, "static void baga_print_f64(double v)  { printf(\"%%g\\n\", v); }\n");
    fprintf(out, "static void baga_print_str(const char *s) { printf(\"%%s\\n\", s); }\n");
    fprintf(out, "\n");

    /* structs first */
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
        if (item->kind == NODE_FN)
            emit_fn(cg, item);
    }

    /* C main → calls baga main */
    fprintf(out, "int main(void) {\n");
    fprintf(out, "    b_main();\n");
    fprintf(out, "    return 0;\n");
    fprintf(out, "}\n");
}
