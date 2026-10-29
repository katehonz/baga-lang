/* codegen_expr.c — част от codegen_c.c (виж cg_proto.h). */
#include "baga.h"
#include "codegen_impl.h"

/* ---- C string escaping ---- */

void emit_c_string(FILE *f, const char *s) {
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

void emit_expr(Codegen *cg, Node *n);
void emit_stmt(Codegen *cg, Node *n);
void emit_return_val(Codegen *cg, Node *val);
void emit_zero_struct(Codegen *cg, const char *name);
const char *lambda_emit_name(Codegen *cg, Node *n, char *buf, size_t nbuf);

/* M20 (effect payloads) — помощници */

/* Връща 1 ако типът (с ефекти) носи поне един payload ефект. */
static int type_has_payload_effects(Type *t) {
    if (!t || !t->effect_payloads) return 0;
    for (int i = 0; i < t->n_effects; i++)
        if (t->effect_payloads[i]) return 1;
    return 0;
}

/* Tag на ефект по име — детерминистичен (първа поява = 1, 2, …). */
int eff_tag(Codegen *cg, const char *name) {
    for (int i = 0; i < cg->eff_tags.len; i++)
        if (strcmp(cg->eff_tags.data[i], name) == 0) return i + 1;
    vec_push(cg->eff_tags, strdup(name));
    return cg->eff_tags.len;
}

/* Нулева стойност за propagate/raise на връщащ тип. */
static void emit_zero_val(Codegen *cg, Type *t) {
    FILE *f = cg->out;
    TypeKind k = t ? t->kind : TYPE_I64;
    switch (k) {
        case TYPE_F64: fprintf(f, "0.0"); break;
        case TYPE_STR: fprintf(f, "NULL"); break;
        case TYPE_BYTES: fprintf(f, "(baga_bytes){0}"); break;
        case TYPE_BOOL: case TYPE_I32: case TYPE_I64: case TYPE_ENUM:
            fprintf(f, "0"); break;
        case TYPE_VEC: case TYPE_MAP: case TYPE_FN:
            fprintf(f, "NULL"); break;
        case TYPE_STRUCT:
            if (t->name) emit_zero_struct(cg, t->name);
            else fprintf(f, "(int64_t)0");
            break;
        default: fprintf(f, "0"); break;
    }
}

/* Връща полето на baga_eff за payload тип (i/s/f/b). */
static const char *eff_slot_field(TypeKind k) {
    switch (k) {
        case TYPE_F64: return "f";
        case TYPE_STR: return "s";
        case TYPE_BYTES: return "b";
        default: return "i";
    }
}

/* return <нулата на връщания тип> — propagate пътят на payload ефект */
static void emit_eff_return_zero(Codegen *cg) {
    FILE *f = cg->out;
    Node *rr = cg->eff_cur_ret;
    if (!rr) { fprintf(f, "return"); return; }   /* void fn */
    fprintf(f, "return ");
    while (rr && rr->kind == NODE_TYPE_EFFECT) rr = rr->inner_type;
    if (!rr) { fprintf(f, "0"); return; }
    if (rr->kind == NODE_TYPE) {
        const char *tn = rr->type_name;
        if (!strcmp(tn, "f64")) fprintf(f, "0.0");
        else if (!strcmp(tn, "str")) fprintf(f, "NULL");
        else if (!strcmp(tn, "bytes")) fprintf(f, "(baga_bytes){0}");
        else if (!strcmp(tn, "Vec") || !strcmp(tn, "Map") ||
                 !strcmp(tn, "fn")) fprintf(f, "NULL");
        else if (!strcmp(tn, "bool") || !strcmp(tn, "i64") ||
                 !strcmp(tn, "i32")) fprintf(f, "0");
        else emit_zero_struct(cg, tn);
        return;
    }
    if (rr->kind == NODE_TYPE_REF || rr->kind == NODE_TYPE_ARRAY ||
        rr->kind == NODE_TYPE_FN) { fprintf(f, "NULL"); return; }
    fprintf(f, "0");
}

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
        case OP_URSHIFT: return ">>>";
    }
    return "+";
}

int is_print_call(Node *n) {
    if (n->kind != NODE_CALL) return 0;
    Node *callee = n->callee;
    if (callee->kind != NODE_IDENT) return 0;
    return strcmp(callee->name, "print") == 0 ||
           strcmp(callee->name, "println") == 0 ||
           strcmp(callee->name, "write") == 0;
}

void emit_print(Codegen *cg, Node *n) {
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

/* Блок като стойност (GNU stmt-expr): catch handler, `let x = { … }`.
 * RC: собствен scope — heap let-овете се release-ват ВЪТРЕ в ({ }),
 * не в enclosing fn (иначе gcc: 'b_h2' undeclared). Последният IDENT
 * от handler-локалите е move към резултата — като LLVM
 * emit_block_value_llvm. Без --rc изходът е бит-идентичен. */
static void emit_catch_handler_block(Codegen *cg, Node *hb, Type *ty) {
    FILE *f = cg->out;
    fprintf(f, "({ ");
    rc_push_scope(cg, 0);
    int scope_top = (cg->rc && cg->rc_scopes.len > 0)
        ? cg->rc_scopes.data[cg->rc_scopes.len - 1].top : 0;
    Node *ve = NULL;
    for (int si = 0; si < hb->stmts.len; si++) {
        Node *s = hb->stmts.data[si];
        if (si == hb->stmts.len - 1) {
            /* последният stmt е СТОЙНОСТТА на handler-а
             * (implicit return семантика) */
            if (s->kind == NODE_EXPR_STMT) ve = s->expr;
            else if (s->kind == NODE_RETURN && s->ret_val) ve = s->ret_val;
            else emit_stmt(cg, s);
        } else {
            emit_stmt(cg, s);
        }
    }
    if (ve) {
        if (cg->rc) {
            /* GNU stmt-expr стойността трябва да остане след scope
             * release-ите — качваме я в __hv, после pop, после __hv. */
            if (ty) emit_ctype(cg, ty); else fprintf(f, "int64_t");
            fprintf(f, " __hv = ");
            emit_expr(cg, ve);
            fprintf(f, "; ");
            if (ve->kind == NODE_IDENT) {
                int vi = rc_find(cg, ve->name);
                if (vi >= scope_top && !cg->rc_locals.data[vi].is_param)
                    cg->rc_locals.data[vi].dead = 1;
            }
        } else {
            emit_expr(cg, ve);
            fprintf(f, "; ");
        }
    }
    rc_pop_scope(cg);
    if (ve && cg->rc) fprintf(f, "__hv; ");
    fprintf(f, " })");
}

void emit_expr(Codegen *cg, Node *n) {
    FILE *f = cg->out;
    if (!n) return;

    /* RC4: temp възелът вече е изчислен в __rc_tmpN декларация преди
     * statement-а — замества се с името */
    if (rc_tmp_emit_sub(cg, n)) return;

    switch (n->kind) {
        case NODE_INT_LIT:
            fprintf(f, "%lldLL", (long long)n->int_val);
            break;

        case NODE_FLOAT_LIT: {
            /* %.17g round-trip-ва IEEE double без загуба на точност;
             * резултатът трябва да остане double литерал в C — "7" би
             * бил int константа и `/` става целочислено (LP2 сонда). */
            char tmp[40];
            snprintf(tmp, sizeof tmp, "%.17g", n->float_val);
            fprintf(f, "%s", tmp);
            if (!strpbrk(tmp, ".eE") && strcmp(tmp, "inf") != 0 &&
                strcmp(tmp, "-inf") != 0 && strcmp(tmp, "nan") != 0)
                fprintf(f, ".0");
            break;
        }

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
            /* M20: catch binding → C temp-ът с payload-а */
            if (cg->eff_binding && cg->eff_binding_c &&
                strcmp(n->name, cg->eff_binding) == 0) {
                fprintf(f, "%s", cg->eff_binding_c);
                break;
            }
            /* L5: глобална fn като стойност → handle към wrapper-а. Локална
             * fn-typed променлива има type->name == NULL или различно име. */
            if (n->type && n->type->kind == TYPE_FN && n->type->name &&
                strcmp(n->name, n->type->name) == 0) {
                char *m = mangle_name(n->type->name);
                fprintf(f, "(int64_t)baga_cell2((int64_t)(void *)%s__clo, 0)", m);
                free(m);
                break;
            }
            /* check if it's an enum variant (bare — first match; checker ensures unique for sum) */
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

        case NODE_PATH: {
            /* A1: Enum::Variant bare (payload-less sum or plain enum tag) */
            int found = 0;
            if (cg->program) {
                for (int i = 0; i < cg->program->items.len && !found; i++) {
                    Node *item = cg->program->items.data[i];
                    if (item->kind != NODE_ENUM) continue;
                    if (strcmp(item->enum_name, n->path_enum) != 0) continue;
                    for (int j = 0; j < item->n_variants; j++) {
                        if (strcmp(item->enum_variants[j], n->path_variant) != 0) continue;
                        int is_sum = 0;
                        for (int k = 0; k < item->n_variants; k++)
                            if (item->enum_payloads && item->enum_payloads[k]) is_sum = 1;
                        char *em = mangle_name(item->enum_name);
                        char *vm = mangle_name(item->enum_variants[j]);
                        if (is_sum)
                            fprintf(f, "(%s){ .tag = %d }", em, j);
                        else
                            fprintf(f, "%s_%s", em, vm);
                        free(em); free(vm);
                        found = 1;
                        break;
                    }
                }
            }
            if (!found) fprintf(f, "0 /* bad path %s::%s */",
                n->path_enum ? n->path_enum : "?",
                n->path_variant ? n->path_variant : "?");
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
            } else if (n->bin_op == OP_MOD &&
                       n->left->type && n->right->type &&
                       n->left->type->kind == TYPE_F64 &&
                       n->right->type->kind == TYPE_F64) {
                /* f64 modulo: fmod — C няма `%` за double; LLVM емитува
                   frem със същата truncating семантика (LP2 сонда). */
                fprintf(f, "fmod(");
                emit_expr(cg, n->left);
                fprintf(f, ", ");
                emit_expr(cg, n->right);
                fprintf(f, ")");
            } else if (n->bin_op == OP_RSHIFT &&
                       (!n->left->type || n->left->type->kind != TYPE_F64) &&
                       (!n->right->type || n->right->type->kind != TYPE_F64)) {
                /* M22: `>>` е аритметично отместване (floor) — преносимо
                   чрез baga_ashr_i64 (паритет с LLVM AShr; C `>>` върху
                   отрицателни е имплементационно дефиниран). */
                fprintf(f, "(baga_ashr_i64((int64_t)(");
                emit_expr(cg, n->left);
                fprintf(f, "), (int64_t)(");
                emit_expr(cg, n->right);
                fprintf(f, ")))");
            } else if (n->bin_op == OP_LSHIFT &&
                       (!n->left->type || n->left->type->kind != TYPE_F64) &&
                       (!n->right->type || n->right->type->kind != TYPE_F64)) {
                /* M22: `<<` с маскиран брояч (b & 63) — детерминирано,
                   паритет с LLVM shl след маската. */
                fprintf(f, "(baga_shl_i64((int64_t)(");
                emit_expr(cg, n->left);
                fprintf(f, "), (int64_t)(");
                emit_expr(cg, n->right);
                fprintf(f, ")))");
            } else if (n->bin_op == OP_URSHIFT &&
                       (!n->left->type || n->left->type->kind != TYPE_F64) &&
                       (!n->right->type || n->right->type->kind != TYPE_F64)) {
                /* M23: `>>>` е логическо отместване (zero-fill) — паритет с
                   LLVM LShr; резултатът е неотрицателен. */
                fprintf(f, "(baga_lshr_i64((int64_t)(");
                emit_expr(cg, n->left);
                fprintf(f, "), (int64_t)(");
                emit_expr(cg, n->right);
                fprintf(f, ")))");
            } else if (n->bin_op == OP_AND || n->bin_op == OP_OR) {
                /* RC4 v1.1: десният операнд се оценява условно — локален
                 * wrap пуска temp-овете му веднага след оценката (не се
                 * hoist-ват към statement-а: &&/|| биха ги направили
                 * безусловни). */
                fprintf(f, "(");
                emit_expr(cg, n->left);
                fprintf(f, " %s ", binop_c(n->bin_op));
                emit_rc_stmt_expr(cg, n->right);
                fprintf(f, ")");
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
            /* L3/A1: конструктор на sum enum → Em__Vm(arg); bare or Enum::Variant */
            if (cg->program &&
                (n->callee->kind == NODE_IDENT || n->callee->kind == NODE_PATH)) {
                int emitted = 0;
                for (int i = 0; i < cg->program->items.len && !emitted; i++) {
                    Node *item = cg->program->items.data[i];
                    if (item->kind != NODE_ENUM) continue;
                    if (n->callee->kind == NODE_PATH &&
                        strcmp(item->enum_name, n->callee->path_enum) != 0)
                        continue;
                    for (int j = 0; j < item->n_variants; j++) {
                        const char *vn = n->callee->kind == NODE_PATH
                            ? n->callee->path_variant : n->callee->name;
                        if (item->enum_payloads && item->enum_payloads[j] &&
                            strcmp(item->enum_variants[j], vn) == 0) {
                            char *em = mangle_name(item->enum_name);
                            char *vm = mangle_name(item->enum_variants[j]);
                            Node *pa = n->args.len > 0 ? n->args.data[0] : NULL;
                            int ptag = 0;
                            if (cg->rc && pa) {
                                ptag = rc_heap_tag_node(cg, item->enum_payloads[j]);
                                if (!ptag) ptag = rc_heap_tag(cg, pa->type);
                            }
                            if (ptag) {
                                /* RC5 v0.6: payload собственост — fresh е owned
                                 * (без retain); borrowed/ident се retain-ва,
                                 * last-use ident е move */
                                fprintf(f, "({ __auto_type __rc_ep = ");
                                emit_expr(cg, pa);
                                fprintf(f, "; ");
                                if (pa->kind == NODE_IDENT) {
                                    int si = rc_find(cg, pa->name);
                                    if (si >= 0 && !cg->rc_locals.data[si].dead) {
                                        if (rc_is_move(cg, pa, si))
                                            rc_do_move(cg, si);
                                        else
                                            rc_emit_retain_val(cg, ptag, pa->type,
                                                               NULL, "__rc_ep");
                                    } else if (si < 0) {
                                        /* RC5 v0.11: untrack-нат ident payload
                                         * (match binding — borrowed копие по
                                         * v0.6 конвенция, или enum/struct fn
                                         * резултат — borrowed/owned неразличим)
                                         * → retain (leak-safe; v0.6 пр. 4).
                                         * Задължително след v0.11: scrutinee
                                         * temp се release-ва след match-а и
                                         * rebox без retain би обесил новия
                                         * enum. dead локали (drop()нати) — както
                                         * досега, без retain. */
                                        rc_emit_retain_val(cg, ptag, pa->type,
                                                           NULL, "__rc_ep");
                                    }
                                } else if (rc_borrowed_init(pa)) {
                                    rc_emit_retain_val(cg, ptag, pa->type,
                                                       NULL, "__rc_ep");
                                }
                                fprintf(f, "%s__%s(__rc_ep); })", em, vm);
                            } else {
                                fprintf(f, "%s__%s(", em, vm);
                                if (n->args.len > 0)
                                    emit_expr(cg, n->args.data[0]);
                                fprintf(f, ")");
                            }
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
                        /* RC1: drop(x) ≡ release; bindingът умира — scope
                         * exit вече не го release-ва (иначе underflow) */
                        if (cg->rc && at && at->kind == TYPE_BYTES) {
                            fprintf(f, "baga_rc_release_bytes(");
                            emit_expr(cg, n->args.data[0]);
                            fprintf(f, ")");
                        } else if (cg->rc && at && at->kind == TYPE_VEC) {
                            char sz[160], rel[160];
                            int vk = rc_vec_elem_kind(at, sz, sizeof sz);
                            rc_box_rel(cg, (at->elem && at->elem->name) ?
                                       at->elem->name : NULL, rel, sizeof rel);
                            /* RC5 v0.9: вложен Vec<S> — destructor за S полетата */
                            if (vk == 3) rc_nested_vec_rel_type(cg, at, rel, sizeof rel);
                            fprintf(f, "baga_rc_release_vec(");
                            emit_expr(cg, n->args.data[0]);
                            fprintf(f, ", %d, %s, %s)", vk, sz, rel);
                        } else if (cg->rc && at && at->kind == TYPE_MAP) {
                            char sz[160], rel[160];
                            int vt = rc_map_val_tag(at, sz, sizeof sz);
                            rc_box_rel(cg, (at->elem && at->elem->name) ?
                                       at->elem->name : NULL, rel, sizeof rel);
                            fprintf(f, "baga_rc_release_map(");
                            emit_expr(cg, n->args.data[0]);
                            fprintf(f, ", %d, %s, %s)", vt, sz, rel);
                        } else if (cg->rc && at && at->kind == TYPE_STRUCT &&
                                   at->name && rc_heap_tag(cg, at) == 5) {
                            /* M24-RC: instantiated generic — per-instance
                             * име и проверка */
                            char *sm = rc_rc_type_name(at, NULL);
                            fprintf(f, "baga_rc_release_%s(", sm);
                            emit_expr(cg, n->args.data[0]);
                            fprintf(f, ")");
                            free(sm);
                        } else if (at && at->kind == TYPE_BYTES) {
                            fprintf(f, "baga_drop_bytes(");
                            emit_expr(cg, n->args.data[0]);
                            fprintf(f, ")");
                        } else if (at && at->kind == TYPE_FN) {
                            fprintf(f, "baga_drop_fn(");
                            emit_expr(cg, n->args.data[0]);
                            fprintf(f, ")");
                        } else if (at && at->kind == TYPE_VEC) {
                            if (at->elem && at->elem->name &&
                                (at->elem->kind == TYPE_STRUCT ||
                                 at->elem->kind == TYPE_ENUM)) {
                                char *mn = mangle_name(at->elem->name);
                                fprintf(f, "baga_drop_vec(");
                                emit_expr(cg, n->args.data[0]);
                                fprintf(f, ", 2, (int64_t)sizeof(%s))", mn);
                                free(mn);
                            } else if (at->elem && at->elem->kind == TYPE_BYTES) {
                                fprintf(f, "baga_drop_vec(");
                                emit_expr(cg, n->args.data[0]);
                                fprintf(f, ", 2, (int64_t)sizeof(baga_bytes))");
                            } else if (at->elem && at->elem->kind == TYPE_VEC) {
                                /* вложени: рекурсивно освобождаване на вътрешните vec-ове */
                                fprintf(f, "baga_drop_vec(");
                                emit_expr(cg, n->args.data[0]);
                                fprintf(f, ", 3, 0)");
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
                            if (at->elem && at->elem->name &&
                                (at->elem->kind == TYPE_STRUCT ||
                                 at->elem->kind == TYPE_ENUM)) {
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
                        /* RC1: drop()натият binding става мъртъв за scope
                         * exit (без това drop + автоматичен release = underflow) */
                        if (cg->rc && n->args.data[0]->kind == NODE_IDENT) {
                            int di = rc_find(cg, n->args.data[0]->name);
                            if (di >= 0) cg->rc_locals.data[di].dead = 1;
                        }
                        goto call_done;
                    }
                }
                /* типизирани вектори: helper по елементния тип на вектора */
                if (strcmp(bn, "vec_push") == 0 || strcmp(bn, "vec_get") == 0 ||
                    strcmp(bn, "vec_set") == 0) {
                    Type *vt = n->args.len > 0 ? n->args.data[0]->type : NULL;
                    /* struct / sum-enum елементи (L4 + A2): box-нато копие */
                    if (vt && vt->kind == TYPE_VEC && vt->elem && vt->elem->name &&
                        (vt->elem->kind == TYPE_STRUCT ||
                         vt->elem->kind == TYPE_ENUM)) {
                        /* M24: instantiated generic struct елемент */
                        char *mn = (vt->elem->kind == TYPE_STRUCT &&
                                    vt->elem->n_targs > 0)
                            ? struct_cname_str(vt->elem)
                            : mangle_name(vt->elem->name);
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
                            fprintf(f, "); ");
                            /* RC5 v0.10: rc_box_tracked = struct с heap полета
                             * или enum с heap payload */
                            if (rc_box_tracked(cg, vt->elem)) {
                                Node *va = n->args.data[1];
                                int mv = 0;
                                int tmp_mv = -1;
                                if (va->kind == NODE_IDENT) {
                                    int si = rc_find(cg, va->name);
                                    if (si >= 0 && !cg->rc_locals.data[si].dead &&
                                        rc_is_move(cg, va, si)) {
                                        cg->rc_locals.data[si].dead = 1;
                                        cg->rc_cmoves++;
                                        mv = 1;
                                    }
                                } else if (va->kind == NODE_STRUCT_LIT ||
                                           rc_is_enum_ctor(cg, va)) {
                                    /* RC5 v0.2: свеж литерал притежава полетата си
                                     * (fresh или вече retain-нати borrowed) — move в
                                     * box-а, без втори retain (иначе temp-ът тече)
                                     * v0.10: същото за свеж enum ctor — payload-ът е
                                     * owned от ctor сайта (v0.6) */
                                    mv = 1;
                                } else if ((tmp_mv = rc_tmp_find(cg, va)) >= 0) {
                                    /* RC5 v1.0b: struct/enum call temp е owned
                                     * (v1.0a) — move в box-а, консумирай temp-а */
                                    cg->rc_cmoves++;
                                    mv = 1;
                                }
                                if (!mv)
                                    fprintf(f, "baga_rc_retain_%s(_bx); ", mn);
                                if (tmp_mv >= 0) rc_tmp_consume(cg, tmp_mv);
                            }
                            fprintf(f, "baga_vec_push_box(");
                            emit_expr(cg, n->args.data[0]);
                            fprintf(f, ", &_bx, (int64_t)sizeof(%s)); })", mn);
                        } else {
                            fprintf(f, "({ %s _bx = (", mn);
                            emit_expr(cg, n->args.data[2]);
                            fprintf(f, "); ");
                            /* RC5 v0.10: rc_box_tracked = struct или enum */
                            if (rc_box_tracked(cg, vt->elem)) {
                                Node *va = n->args.data[2];
                                int mv = 0;
                                int tmp_mv = -1;
                                if (va->kind == NODE_IDENT) {
                                    int si = rc_find(cg, va->name);
                                    if (si >= 0 && !cg->rc_locals.data[si].dead &&
                                        rc_is_move(cg, va, si)) {
                                        cg->rc_locals.data[si].dead = 1;
                                        cg->rc_cmoves++;
                                        mv = 1;
                                    }
                                } else if (va->kind == NODE_STRUCT_LIT ||
                                           rc_is_enum_ctor(cg, va)) {
                                    /* RC5 v0.2: свеж литерал — move в box-а (виж vec_push)
                                     * v0.10: и свеж enum ctor */
                                    mv = 1;
                                } else if ((tmp_mv = rc_tmp_find(cg, va)) >= 0) {
                                    /* RC5 v1.0b: owned call temp — move */
                                    cg->rc_cmoves++;
                                    mv = 1;
                                }
                                if (!mv)
                                    fprintf(f, "baga_rc_retain_%s(_bx); ", mn);
                                if (tmp_mv >= 0) rc_tmp_consume(cg, tmp_mv);
                            }
                            if (rc_box_tracked(cg, vt->elem)) {
                                /* RC5 v0.3: release на стария box при overwrite
                                 * v0.10: и за enum елементи (relf_<E> по runtime tag) */
                                fprintf(f, "baga_vec_set_box_rc(");
                                emit_expr(cg, n->args.data[0]);
                                fprintf(f, ", ");
                                emit_expr(cg, n->args.data[1]);
                                fprintf(f, ", &_bx, (int64_t)sizeof(%s), baga_rc_relf_%s); })",
                                        mn, mn);
                            } else {
                                fprintf(f, "baga_vec_set_box(");
                                emit_expr(cg, n->args.data[0]);
                                fprintf(f, ", ");
                                emit_expr(cg, n->args.data[1]);
                                fprintf(f, ", &_bx, (int64_t)sizeof(%s)); })", mn);
                            }
                        }
                        free(mn);
                        goto call_done;
                    }
                    const char *suf = "i64";
                    if (vt && vt->kind == TYPE_VEC && vt->elem) {
                        if (vt->elem->kind == TYPE_STR) suf = "str";
                        else if (vt->elem->kind == TYPE_F64) suf = "f64";
                        else if (vt->elem->kind == TYPE_BYTES) suf = "bytes";
                        else if (vt->elem->kind == TYPE_VEC) suf = "vec";
                    }
                    /* RC3: last-use стойност в push/set → move вариант без
                     * retain; референцията преминава в контейнера, binding-ът
                     * умира тук (scope exit го пропуска). i64/f64 — без rc. */
                    int mv = 0;
                    int tmp_mv = -1;  /* RC5 v0.7 */
                    if (cg->rc && strcmp(bn, "vec_get") != 0 &&
                        (strcmp(suf, "str") == 0 ||
                         strcmp(suf, "bytes") == 0 ||
                         strcmp(suf, "vec") == 0)) {
                        Node *va = n->args.data[
                            strcmp(bn, "vec_push") == 0 ? 1 : 2];
                        if (va->kind == NODE_IDENT) {
                            int si = rc_find(cg, va->name);
                            if (si >= 0 && !cg->rc_locals.data[si].dead &&
                                rc_is_move(cg, va, si)) {
                                cg->rc_locals.data[si].dead = 1;
                                cg->rc_cmoves++;
                                mv = 1;
                            }
                        } else if ((tmp_mv = rc_tmp_find(cg, va)) >= 0) {
                            /* RC5 v0.7: директен temp аргумент (call/to_str
                             * резултат, вече изчислен в __rc_tmpN) — owned по
                             * конвенцията „fn резултат = owned". Move в
                             * контейнера: _move вариант без retain, а temp-ът
                             * се консумира (без release в края на statement-а)
                             * — същият трансфер като RC3 за last-use ident.
                             * v1.0b: struct/enum box temp-овете са в box
                             * пътеката по-горе (също move). */
                            cg->rc_cmoves++;
                            mv = 1;
                        }
                    }
                    /* RC5 v0.9: vec_set overwrite на Vec<Vec<S>> — старият
                     * вътрешен vec се release-ва с destructor за S полетата
                     * (generic helper-ът го пуска като kind 0 — тече). */
                    if (cg->rc && strcmp(bn, "vec_set") == 0 &&
                        strcmp(suf, "vec") == 0) {
                        char nrel[160];
                        rc_nested_vec_rel_type(cg, vt, nrel, sizeof nrel);
                        if (strcmp(nrel, "0") != 0) {
                            fprintf(f, "baga_vec_set_vec%s_rc(",
                                    mv ? "_move" : "");
                            for (int i = 0; i < n->args.len; i++) {
                                if (i > 0) fprintf(f, ", ");
                                emit_expr(cg, n->args.data[i]);
                            }
                            fprintf(f, ", %s)", nrel);
                            if (tmp_mv >= 0) rc_tmp_consume(cg, tmp_mv);
                            goto call_done;
                        }
                    }
                    fprintf(f, "baga_%s_%s%s(", bn, suf, mv ? "_move" : "");
                    for (int i = 0; i < n->args.len; i++) {
                        if (i > 0) fprintf(f, ", ");
                        emit_expr(cg, n->args.data[i]);
                    }
                    fprintf(f, ")");
                    if (tmp_mv >= 0) rc_tmp_consume(cg, tmp_mv);  /* RC5 v0.7 */
                    goto call_done;
                }
                if (strcmp(bn, "vec_slice") == 0 || strcmp(bn, "vec_concat") == 0) {
                    Type *vt = n->args.len > 0 ? n->args.data[0]->type : NULL;
                    /* struct / sum-enum: размерът идва от call site-а */
                    if (vt && vt->kind == TYPE_VEC && vt->elem && vt->elem->name &&
                        (vt->elem->kind == TYPE_STRUCT ||
                         vt->elem->kind == TYPE_ENUM)) {
                        /* M24: instantiated generic struct елемент */
                        char *mn = (vt->elem->kind == TYPE_STRUCT &&
                                    vt->elem->n_targs > 0)
                            ? struct_cname_str(vt->elem)
                            : mangle_name(vt->elem->name);
                        if (rc_box_tracked(cg, vt->elem)) {
                            /* RC5 v0.2: box копието споделя полетата — retain
                             * (иначе drop на двата вектора пуска два пъти)
                             * v0.10: и за enum елементи (retp_<E> по runtime tag) */
                            fprintf(f, "baga_%s_box_rc(", bn);
                            for (int i = 0; i < n->args.len; i++) {
                                if (i > 0) fprintf(f, ", ");
                                emit_expr(cg, n->args.data[i]);
                            }
                            fprintf(f, ", (int64_t)sizeof(%s), baga_rc_retp_%s)",
                                    mn, mn);
                        } else {
                            fprintf(f, "baga_%s_box(", bn);
                            for (int i = 0; i < n->args.len; i++) {
                                if (i > 0) fprintf(f, ", ");
                                emit_expr(cg, n->args.data[i]);
                            }
                            fprintf(f, ", (int64_t)sizeof(%s))", mn);
                        }
                        free(mn);
                        goto call_done;
                    }
                    const char *suf = "i64";
                    if (vt && vt->kind == TYPE_VEC && vt->elem) {
                        if (vt->elem->kind == TYPE_STR) suf = "str";
                        else if (vt->elem->kind == TYPE_F64) suf = "f64";
                        else if (vt->elem->kind == TYPE_BYTES) suf = "bytes";
                        else if (vt->elem->kind == TYPE_VEC) suf = "vec";
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
                    /* struct / sum-enum стойности: box path */
                    if (mt && mt->kind == TYPE_MAP && mt->elem && mt->elem->name &&
                        (mt->elem->kind == TYPE_STRUCT ||
                         mt->elem->kind == TYPE_ENUM)) {
                        const char *ksuf = "str";
                        if (mt->key && mt->key->kind == TYPE_I64) ksuf = "i64";
                        else if (mt->key && mt->key->kind == TYPE_BYTES) ksuf = "bytes";
                        /* M24-RC: instantiated generic стойност — per-instance име */
                        char *mn = (mt->elem->kind == TYPE_STRUCT &&
                                    mt->elem->n_targs > 0)
                            ? struct_cname_str(mt->elem)
                            : mangle_name(mt->elem->name);
                        if (strcmp(bn, "map_set") == 0) {
                            fprintf(f, "({ %s _bx = (", mn);
                            emit_expr(cg, n->args.data[2]);
                            fprintf(f, "); ");
                            /* RC5 v0.10: rc_box_tracked = struct или enum */
                            if (rc_box_tracked(cg, mt->elem)) {
                                Node *va = n->args.data[2];
                                int mv = 0;
                                int tmp_mv = -1;
                                if (va->kind == NODE_IDENT) {
                                    int si = rc_find(cg, va->name);
                                    if (si >= 0 && !cg->rc_locals.data[si].dead &&
                                        rc_is_move(cg, va, si)) {
                                        cg->rc_locals.data[si].dead = 1;
                                        cg->rc_cmoves++;
                                        mv = 1;
                                    }
                                } else if (va->kind == NODE_STRUCT_LIT ||
                                           rc_is_enum_ctor(cg, va)) {
                                    /* RC5 v0.2: свеж литерал — move в box-а (виж vec_push)
                                     * v0.10: и свеж enum ctor */
                                    mv = 1;
                                } else if ((tmp_mv = rc_tmp_find(cg, va)) >= 0) {
                                    /* RC5 v1.0b: owned call temp — move */
                                    cg->rc_cmoves++;
                                    mv = 1;
                                }
                                if (!mv)
                                    fprintf(f, "baga_rc_retain_%s(_bx); ", mn);
                                if (tmp_mv >= 0) rc_tmp_consume(cg, tmp_mv);
                            }
                            if (rc_box_tracked(cg, mt->elem)) {
                                /* RC5 v0.3: release на стария box при overwrite
                                 * v0.10: и за enum стойности (relf_<E> по runtime tag) */
                                fprintf(f, "baga_map_set_%s_box_rc(", ksuf);
                                emit_expr(cg, n->args.data[0]);
                                fprintf(f, ", ");
                                emit_expr(cg, n->args.data[1]);
                                fprintf(f, ", &_bx, (int64_t)sizeof(%s), baga_rc_relf_%s); })",
                                        mn, mn);
                            } else {
                                fprintf(f, "baga_map_set_%s_box(", ksuf);
                                emit_expr(cg, n->args.data[0]);
                                fprintf(f, ", ");
                                emit_expr(cg, n->args.data[1]);
                                fprintf(f, ", &_bx, (int64_t)sizeof(%s)); })", mn);
                            }
                        } else {
                            /* missing key → zero struct, or zero tagged union */
                            fprintf(f, "({ void *_bp = baga_map_get_%s_box(", ksuf);
                            for (int i = 0; i < n->args.len; i++) {
                                if (i > 0) fprintf(f, ", ");
                                emit_expr(cg, n->args.data[i]);
                            }
                            fprintf(f, "); _bp ? *(%s *)_bp : ", mn);
                            if (mt->elem->kind == TYPE_ENUM)
                                fprintf(f, "(%s){0}", mn);
                            else
                                emit_zero_struct(cg, mt->elem->name);
                            fprintf(f, "; })");
                        }
                        free(mn);
                        goto call_done;
                    }
                    const char *ksuf = "str", *vsuf = "i64";
                    if (mt && mt->kind == TYPE_MAP) {
                        if (mt->key && mt->key->kind == TYPE_I64) ksuf = "i64";
                        else if (mt->key && mt->key->kind == TYPE_BYTES) ksuf = "bytes";
                        if (mt->elem) {
                            if (mt->elem->kind == TYPE_STR) vsuf = "str";
                            else if (mt->elem->kind == TYPE_F64) vsuf = "f64";
                            else if (mt->elem->kind == TYPE_BYTES) vsuf = "bytes";
                        }
                    }
                    /* RC3: map_set с last-use стойност → move вариант без
                     * retain (ключът остава retain-нат — отделен живот) */
                    int mv = 0;
                    int tmp_mv = -1;  /* RC5 v0.7 */
                    if (cg->rc && strcmp(bn, "map_set") == 0 &&
                        (strcmp(vsuf, "str") == 0 ||
                         strcmp(vsuf, "bytes") == 0) &&
                        n->args.len >= 3 &&
                        n->args.data[2]->kind == NODE_IDENT) {
                        Node *va = n->args.data[2];
                        int si = rc_find(cg, va->name);
                        if (si >= 0 && !cg->rc_locals.data[si].dead &&
                            rc_is_move(cg, va, si)) {
                            cg->rc_locals.data[si].dead = 1;
                            cg->rc_cmoves++;
                            mv = 1;
                        }
                    }
                    /* RC5 v0.7: temp стойност → move вариант + консумация на
                     * temp-а (виж vec_push сайта; ключът пак се retain-ва) */
                    if (cg->rc && !mv && strcmp(bn, "map_set") == 0 &&
                        (strcmp(vsuf, "str") == 0 ||
                         strcmp(vsuf, "bytes") == 0) &&
                        n->args.len >= 3 &&
                        (tmp_mv = rc_tmp_find(cg, n->args.data[2])) >= 0) {
                        cg->rc_cmoves++;
                        mv = 1;
                    }
                    fprintf(f, "baga_%s_%s_%s%s(", bn, ksuf, vsuf,
                            mv ? "_move" : "");
                    for (int i = 0; i < n->args.len; i++) {
                        if (i > 0) fprintf(f, ", ");
                        emit_expr(cg, n->args.data[i]);
                    }
                    fprintf(f, ")");
                    if (tmp_mv >= 0) rc_tmp_consume(cg, tmp_mv);  /* RC5 v0.7 */
                    goto call_done;
                }
                if (strcmp(bn, "map_has") == 0 || strcmp(bn, "map_del") == 0 ||
                    strcmp(bn, "map_keys") == 0) {
                    Type *mt = n->args.len > 0 ? n->args.data[0]->type : NULL;
                    const char *ksuf = "str";
                    if (mt && mt->kind == TYPE_MAP && mt->key) {
                        if (mt->key->kind == TYPE_I64) ksuf = "i64";
                        else if (mt->key->kind == TYPE_BYTES) ksuf = "bytes";
                    }
                    /* RC5 v0.3: del на Map<K, S с heap полета> — release на
                     * полетата + free на pv (иначе откаченото entry тече)
                     * v0.10: и Map<K, E с heap payload> (rc_box_tracked) */
                    if (cg->rc && strcmp(bn, "map_del") == 0 &&
                        rc_box_tracked(cg, mt && mt->kind == TYPE_MAP ?
                                       mt->elem : NULL)) {
                        /* M24-RC: instantiated generic — per-instance име */
                        char *mn = (mt->elem->kind == TYPE_STRUCT &&
                                    mt->elem->n_targs > 0)
                            ? struct_cname_str(mt->elem)
                            : mangle_name(mt->elem->name);
                        fprintf(f, "baga_map_del_%s_rc(", ksuf);
                        emit_expr(cg, n->args.data[0]);
                        fprintf(f, ", ");
                        emit_expr(cg, n->args.data[1]);
                        fprintf(f, ", (int64_t)sizeof(%s), baga_rc_relf_%s)",
                                mn, mn);
                        free(mn);
                        goto call_done;
                    }
                    /* MEM-8: non-rc del на struct/enum пуска box-а (pv). */
                    if (!cg->rc && strcmp(bn, "map_del") == 0 &&
                        mt && mt->kind == TYPE_MAP && mt->elem &&
                        (mt->elem->kind == TYPE_STRUCT ||
                         mt->elem->kind == TYPE_ENUM)) {
                        char *mn = (mt->elem->kind == TYPE_STRUCT &&
                                    mt->elem->n_targs > 0)
                            ? struct_cname_str(mt->elem)
                            : mangle_name(mt->elem->name);
                        fprintf(f, "baga_map_del_%s_box(", ksuf);
                        emit_expr(cg, n->args.data[0]);
                        fprintf(f, ", ");
                        emit_expr(cg, n->args.data[1]);
                        fprintf(f, ", (int64_t)sizeof(%s))", mn);
                        free(mn);
                        goto call_done;
                    }
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
                if (strcmp(bn, "rc_on") == 0 && n->args.len == 0) {
                    /* RC5: 1 зад --rc, 0 без — ръчният drop (MEM-4) се
                     * пропуска, когато RC сам release-ва. */
                    fprintf(f, "%d", cg->rc ? 1 : 0);
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
                    {"i64_to_str","baga_i64_to_str"},
                    {"f64_to_str","baga_f64_to_str"},
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
                    {"world_new",    "baga_world_new"},
                    {"world_alloc",  "baga_world_alloc"},
                    {"world_freeze", "baga_world_freeze"},
                    {"world_state",  "baga_world_state"},
                    {"world_free",   "baga_world_free"},
                    {"wvec_new",     "baga_wvec_new"},
                    {"wvec_push",    "baga_wvec_push"},
                    {"wvec_get",     "baga_wvec_get"},
                    {"wvec_len",     "baga_wvec_len"},
                    {"world_dump",   "baga_world_dump"},
                    {"world_load",   "baga_world_load"},
                    {"world_used",   "baga_world_used"},
                    {"wvec_push_str", "baga_wvec_push_str"},
                    {"wvec_get_str",  "baga_wvec_get_str"},
                    {"wcache_set",   "baga_wcache_set"},
                    {"wcache_get",   "baga_wcache_get"},
                    {"wcache_has",   "baga_wcache_has"},
                    {"wcache_del",   "baga_wcache_del"},
                    {"wcache_clear", "baga_wcache_clear"},
                    {"wcache_len",   "baga_wcache_len"},
                    {"world_link",   "baga_world_link"},
                    {"world_unlink", "baga_world_unlink"},
                    {"world_links",  "baga_world_links"},
                    {"world_linked", "baga_world_linked"},
                    {"mem_mark",    "baga_mem_mark"},
                    {"mem_rewind",  "baga_mem_rewind"},
                    {"mem_arena_bytes", "baga_mem_arena_bytes"},
                    {"mem_spare_bytes", "baga_mem_spare_bytes"},
                    {"mem_peak_bytes",  "baga_mem_peak_bytes"},
                    {"mem_persist_begin", "baga_mem_persist_begin"},
                    {"mem_persist_end",   "baga_mem_persist_end"},
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
                    {"bytes_put",  "baga_bytes_put"},
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
                    {"str_h",       "baga_str_h"},
                    {"h_str",       "baga_h_str"},
                    {"bytes_h",     "baga_bytes_h"},
                    {"h_bytes",     "baga_h_bytes"},
                    {"map_h",       "baga_map_h"},
                    {"h_map",       "baga_h_map"},
                };
                for (int bi = 0; bi < (int)(sizeof(bmap) / sizeof(bmap[0])); bi++) {
                    if (strcmp(bn, bmap[bi].baga) == 0) {
                        /* W2.2: след free локалният handle става 0. Двоен free
                         * вече е no-op (baga_arena_free(0) е guard-иран), а
                         * всяка следваща употреба е чиста null-handle грешка
                         * вместо двоен free() на malloc (истинско UB). Записът
                         * е само при обикновен ident аргумент — алиасите
                         * остават отговорност на seatbelt/verifier (MEM-1). */
                        if ((strcmp(bn, "world_free") == 0 || strcmp(bn, "arena_free") == 0) &&
                            n->args.len == 1 && n->args.data[0]->kind == NODE_IDENT) {
                            fprintf(f, "({ %s(", bmap[bi].c);
                            emit_expr(cg, n->args.data[0]);
                            fprintf(f, "); ");
                            emit_expr(cg, n->args.data[0]);
                            fprintf(f, " = 0; })");
                            goto call_done;
                        }
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
            /* if as expression → GCC statement expression.
             * RC4 v1.1: клоновете имат собствени temp wrap-ове — last expr
             * е bound (стойността на if-израза); предишните EXPR_STMT са
             * дискарднати. Cond вече минава през emit_rc_stmt_expr (v0.3). */
            fprintf(f, "({");
            fprintf(f, "if (");
            emit_rc_stmt_expr(cg, n->cond);
            fprintf(f, ") { ");
            if (n->then_br && n->then_br->kind == NODE_BLOCK) {
                for (int i = 0; i < n->then_br->stmts.len; i++) {
                    Node *s = n->then_br->stmts.data[i];
                    if (s->kind == NODE_EXPR_STMT) {
                        int last = (i == n->then_br->stmts.len - 1);
                        emit_rc_stmt_expr_root(cg, s->expr, last);
                        fprintf(f, "; ");
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
                            int last = (i == n->else_br->stmts.len - 1);
                            emit_rc_stmt_expr_root(cg, s->expr, last);
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
            /* RC1: overwrite на track-нат локал — оцени дясното ПРЕДИ release
             * (`s = concat(s, x)`), retain при alias от track-нат източник,
             * release на стария собственик. Struct полета не се track-ват. */
            if (cg->rc && n->assign_target &&
                n->assign_target->kind == NODE_IDENT) {
                int idx = rc_find(cg, n->assign_target->name);
                if (idx >= 0 && !cg->rc_locals.data[idx].is_param &&
                    !cg->rc_locals.data[idx].dead) {
                    /* snapshot — emit_expr на дясното може да realloc-не
                     * стека (вложена ламбда) и да невалидира указателя */
                    RcLocal e = cg->rc_locals.data[idx];
                    char sz[160];
                    int vkind = 0, vtag = 0;
                    if (e.tag == 3) vkind = rc_vec_kind_of(&e, sz, sizeof sz);
                    if (e.tag == 4) vtag = rc_map_tag_of(&e, sz, sizeof sz);
                    /* borrowed дясно (vec_get/map_get/поле) също се retain-ва —
                     * референцията става собствена; старият собственик се
                     * release-ва след retain-а (alias-safe ред) */
                    int keep = (n->assign_val->kind == NODE_IDENT) ||
                               rc_borrowed_init(n->assign_val);
                    fprintf(f, "({ __auto_type __rc_asn = ");
                    emit_expr(cg, n->assign_val);
                    fprintf(f, "; ");
                    if (keep && n->assign_val->kind == NODE_IDENT) {
                        int si = rc_find(cg, n->assign_val->name);
                        if (si >= 0 && !cg->rc_locals.data[si].dead) {
                            /* RC2: последна употреба на източника → move:
                             * старата стойност се release-ва, новата идва
                             * като преместена референция (без retain).
                             * `x = x` не е move — release-ът би я обесил. */
                            if (si != idx && rc_is_move(cg, n->assign_val, si)) {
                                rc_do_move(cg, si);
                            } else
                                rc_emit_retain_val(cg, e.tag, e.type, e.type_node, "__rc_asn");
                        }
                    } else if (keep) {
                        rc_emit_retain_val(cg, e.tag, e.type, e.type_node, "__rc_asn");
                    }
                    /* RC9: `x = f(x)` при struct — старата стойност СЕ
                     * release-ва. Под RC5 v1.0 („fn резултат = owned")
                     * callee retain-ва каквото връща (литерал/return на
                     * borrowed), така че референциите на стария struct са
                     * излишни след извикването; да не ги пуснем е теч
                     * (fmr_ctx_set_trace/http_set_header/jobj_str вериги).
                     * Редът е безопасен: дясното е оценено ПРЕДИ release. */
                    switch (e.tag) {
                        case 1: fprintf(f, "baga_rc_release_str(%s); ", e.name); break;
                        case 2: fprintf(f, "baga_rc_release_bytes(%s); ", e.name); break;
                        case 3: {
                            char rel[160];
                            rc_box_rel(cg, rc_vec_elem_name(&e), rel, sizeof rel);
                            /* RC5 v0.9: вложен Vec<S> — destructor за S полетата */
                            if (vkind == 3) rc_vec_nested_rel_of(cg, &e, rel, sizeof rel);
                            fprintf(f, "baga_rc_release_vec(%s, %d, %s, %s); ",
                                    e.name, vkind, sz, rel);
                            break;
                        }
                        case 4: {
                            char rel[160];
                            rc_box_rel(cg, rc_map_val_name(&e), rel, sizeof rel);
                            fprintf(f, "baga_rc_release_map(%s, %d, %s, %s); ",
                                    e.name, vtag, sz, rel);
                            break;
                        }
                        case 5: {
                            const char *sn = (e.type && e.type->name) ? e.type->name :
                                (e.type_node && e.type_node->type_name) ? e.type_node->type_name : NULL;
                            if (sn) {
                                /* M24-RC: instantiated generic — per-instance име */
                                char *sm = rc_rc_type_name(e.type, e.type_node);
                                fprintf(f, "baga_rc_release_%s(%s); ", sm, e.name);
                                free(sm);
                            }
                            break;
                        }
                        /* RC5 v0.6: enum с heap payload */
                        case 6: {
                            const char *en = (e.type && e.type->name) ? e.type->name :
                                (e.type_node && e.type_node->type_name) ? e.type_node->type_name : NULL;
                            if (en) {
                                char *em = rc_rc_type_name(e.type, e.type_node);
                                fprintf(f, "baga_rc_release_%s(%s); ", em, e.name);
                                free(em);
                            }
                            break;
                        }
                    }
                    fprintf(f, "%s = __rc_asn; })", e.name);
                    break;
                }
            }
            /* RC1: присвояване на track-нат локал в struct ПОЛЕ (или друга
             * цел, която не е локал) — полето споделя референцията → retain
             * (`c.scan_keys = filtered` — иначе scope exit я обесва). */
            if (cg->rc && n->assign_val &&
                n->assign_val->kind == NODE_IDENT) {
                int si = rc_find(cg, n->assign_val->name);
                if (si >= 0 && !cg->rc_locals.data[si].dead) {
                    char sz[160], rel[160];
                    int fkind = 0;
                    int ftag = rc_field_assign_tag(cg, n->assign_target, &fkind,
                                                   sz, sizeof sz, rel, sizeof rel);
                    /* RC5 v0.8: struct-типизирана цел (`s.inner = x`) */
                    const char *fst = rc_field_assign_struct(cg, n->assign_target);
                    /* RC5 v0.10: enum-типизирана цел (`s.e = x`) */
                    const char *fen = fst ? NULL :
                        rc_field_assign_enum(cg, n->assign_target);
                    /* RC2: последна употреба → move (обикновено присвояване,
                     * без retain; източникът умира тук) */
                    if (rc_is_move(cg, n->assign_val, si)) {
                        rc_do_move(cg, si);
                        if (ftag) {
                            /* RC5 v0.4: release на старото поле преди assign */
                            fprintf(f, "({ __auto_type __rc_fa = ");
                            emit_expr(cg, n->assign_val);
                            fprintf(f, "; ");
                            rc_emit_field_release(cg, ftag, n->assign_target,
                                                  fkind, sz, rel);
                            emit_expr(cg, n->assign_target);
                            fprintf(f, " = __rc_fa; __rc_fa; })");
                        } else if (fst) {
                            /* RC5 v0.8: move в struct поле — без retain;
                             * старото поле се release-ва рекурсивно */
                            fprintf(f, "({ __auto_type __rc_fa = ");
                            emit_expr(cg, n->assign_val);
                            fprintf(f, "; ");
                            rc_emit_struct_field_release(cg, fst,
                                                         n->assign_target);
                            emit_expr(cg, n->assign_target);
                            fprintf(f, " = __rc_fa; __rc_fa; })");
                        } else if (fen) {
                            /* RC5 v0.10: move в enum поле — без retain;
                             * старото поле се release-ва по runtime tag */
                            fprintf(f, "({ __auto_type __rc_fa = ");
                            emit_expr(cg, n->assign_val);
                            fprintf(f, "; ");
                            rc_emit_enum_field_release(cg, fen,
                                                       n->assign_target);
                            emit_expr(cg, n->assign_target);
                            fprintf(f, " = __rc_fa; __rc_fa; })");
                        } else {
                            emit_expr(cg, n->assign_target);
                            fprintf(f, " = ");
                            emit_expr(cg, n->assign_val);
                        }
                        break;
                    }
                    RcLocal src = cg->rc_locals.data[si];
                    fprintf(f, "({ __auto_type __rc_fa = ");
                    emit_expr(cg, n->assign_val);
                    fprintf(f, "; ");
                    if (ftag) {
                        /* RC5 v0.4: retain на новото ПРЕДИ release на старото
                         * (alias-safe: `w.s = w2.s` със споделена стойност) */
                        rc_emit_retain_val(cg, src.tag, src.type, src.type_node, "__rc_fa");
                        rc_emit_field_release(cg, ftag, n->assign_target,
                                              fkind, sz, rel);
                    } else if (fst) {
                        /* RC5 v0.8: същият alias-safe ред за struct поле —
                         * retain_<T> на новото, после release_<T> на старото */
                        rc_emit_retain_val(cg, src.tag, src.type, src.type_node, "__rc_fa");
                        rc_emit_struct_field_release(cg, fst, n->assign_target);
                    } else if (fen) {
                        /* RC5 v0.10: същият alias-safe ред за enum поле —
                         * retain_E на новото (tag 6), после release_E на старото */
                        rc_emit_retain_val(cg, src.tag, src.type, src.type_node, "__rc_fa");
                        rc_emit_enum_field_release(cg, fen, n->assign_target);
                    }
                    emit_expr(cg, n->assign_target);
                    fprintf(f, " = __rc_fa; ");
                    if (!ftag && !fst && !fen)
                        rc_emit_retain_val(cg, src.tag, src.type, src.type_node, "__rc_fa");
                    fprintf(f, "__rc_fa; })");
                    break;
                }
            }
            /* RC5 v0.4: не-ident дясно в heap поле на track-нат struct —
             * fresh е owned (без retain), borrowed се retain-ва; старото
             * поле се release-ва (retain преди release — alias-safe). */
            if (cg->rc) {
                char sz[160], rel[160];
                int fkind = 0;
                int ftag = rc_field_assign_tag(cg, n->assign_target, &fkind,
                                               sz, sizeof sz, rel, sizeof rel);
                if (ftag) {
                    fprintf(f, "({ __auto_type __rc_fa = ");
                    emit_expr(cg, n->assign_val);
                    fprintf(f, "; ");
                    if (rc_borrowed_init(n->assign_val))
                        rc_emit_retain_val(cg, ftag, n->assign_val->type, NULL,
                                           "__rc_fa");
                    rc_emit_field_release(cg, ftag, n->assign_target,
                                          fkind, sz, rel);
                    emit_expr(cg, n->assign_target);
                    fprintf(f, " = __rc_fa; __rc_fa; })");
                    break;
                }
                /* RC5 v0.8: не-ident дясно в struct-типизирано поле. Свеж
                 * литерал е owned (без retain). v1.0b: owned call (`mk()`)
                 * също без retain. Поле/vec_get/untrack-нат ident се
                 * retain-ват. Старото поле се release-ва рекурсивно след
                 * retain-а (alias-safe). */
                const char *fst = rc_field_assign_struct(cg, n->assign_target);
                if (fst) {
                    fprintf(f, "({ __auto_type __rc_fa = ");
                    emit_expr(cg, n->assign_val);
                    fprintf(f, "; ");
                    if (n->assign_val->kind != NODE_STRUCT_LIT &&
                        !rc_is_owned_call(cg, n->assign_val)) {
                        /* RC5 v1.0b: owned call (`s.inner = mk()`) — без retain */
                        char *fm = mangle_name(fst);
                        fprintf(f, "baga_rc_retain_%s(__rc_fa); ", fm);
                        free(fm);
                    }
                    rc_emit_struct_field_release(cg, fst, n->assign_target);
                    emit_expr(cg, n->assign_target);
                    fprintf(f, " = __rc_fa; __rc_fa; })");
                    break;
                }
                /* RC5 v0.10: не-ident дясно в enum-типизирано поле. Свеж ctor
                 * е owned (без retain). v1.0b: owned call (`f()`) също без
                 * retain. Поле/vec_get/untrack-нат ident се retain-ват.
                 * Старото поле се release-ва по runtime tag (alias-safe). */
                const char *fen = rc_field_assign_enum(cg, n->assign_target);
                if (fen) {
                    fprintf(f, "({ __auto_type __rc_fa = ");
                    emit_expr(cg, n->assign_val);
                    fprintf(f, "; ");
                    if (!rc_is_enum_ctor(cg, n->assign_val) &&
                        !rc_is_owned_call(cg, n->assign_val)) {
                        /* RC5 v1.0b: owned call (`s.e = f()`) — без retain */
                        char *fm = mangle_name(fen);
                        fprintf(f, "baga_rc_retain_%s(__rc_fa); ", fm);
                        free(fm);
                    }
                    rc_emit_enum_field_release(cg, fen, n->assign_target);
                    emit_expr(cg, n->assign_target);
                    fprintf(f, " = __rc_fa; __rc_fa; })");
                    break;
                }
            }
            emit_expr(cg, n->assign_target);
            fprintf(f, " = ");
            emit_expr(cg, n->assign_val);
            break;

        case NODE_RANGE:
            /* Phase 1: ranges only in for loops, handled there */
            fprintf(f, "0");
            break;

        case NODE_STRUCT_LIT: {
            /* M24: instantiated generic struct — конкретното C име (per
             * инстанция); иначе mangled име. RC embed логиката по-долу е
             * обща (M26: преди M24 клонът я заобикаляше — borrowed ident
             * поле в generic литерал не се retain-ваше и scope release-ът
             * на инстанцията обесваше източника). */
            char *sm = (n->type && n->type->kind == TYPE_STRUCT && n->type->n_targs > 0)
                ? struct_cname_str(n->type) : mangle_name(n->lit_name);
            /* RC1: struct литералът споделя heap полета по указател — bare
             * track-нат локал/параметър, вграден в литерала, се retain-ва
             * (+1 за struct референцията; struct-овете не се release-ват →
             * умира с процеса, но не dangling). `return S { v: v }` е
             * threading идиомът на rocksbaga — без това release при scope
             * exit го обесва. Два прохода: дали има такива, после retain-и. */
            int nemb = 0;
            if (cg->rc) {
                for (int i = 0; i < n->n_lit_fields; i++) {
                    Node *val = n->lit_values.data[i];
                    /* RC1.1: borrowed стойност (vec_get/map_get/поле/h_*),
                     * вградена директно — полето алиасира чужда собственост →
                     * retain през __rc_sl.<field> (иначе release на източника
                     * обесва полето — латентен пропуск, излязъл с RC4)
                     * v0.5: и borrowed struct (rc_heap_tag хваща tag 5) */
                    if (rc_borrowed_init(val) &&
                        rc_heap_tag(cg, val->type) != 0) {
                        nemb++;
                        continue;
                    }
                    if (val->kind != NODE_IDENT) continue;
                    int si = rc_find(cg, val->name);
                    /* RC2: move сайтовете не се retain-ват (виж втория проход) */
                    if (si >= 0 && !cg->rc_locals.data[si].dead &&
                        !rc_is_move(cg, val, si))
                        nemb++;
                    else if (si < 0 && rc_heap_tag(cg, val->type) != 0)
                        /* RC5 v1.0a: untrack-нат ident (match binding) —
                         * borrowed, полето трябва да задържи референция */
                        nemb++;
                }
            }
            if (nemb > 0) fprintf(f, "({ %s __rc_sl = ", sm);
            fprintf(f, "(%s){ ", sm);
            for (int i = 0; i < n->n_lit_fields; i++) {
                if (i > 0) fprintf(f, ", ");
                char *fm = mangle_name(n->lit_fields[i]);
                fprintf(f, ".%s = ", fm);
                emit_expr(cg, n->lit_values.data[i]);
                free(fm);
            }
            fprintf(f, " }");
            if (nemb > 0) {
                fprintf(f, "; ");
                for (int i = 0; i < n->n_lit_fields; i++) {
                    Node *val = n->lit_values.data[i];
                    /* RC1.1: retain на borrowed поле през построената стойност */
                    int vltag = rc_heap_tag(cg, val->type);
                    if (rc_borrowed_init(val) && vltag != 0) {
                        char *fm2 = mangle_name(n->lit_fields[i]);
                        if (vltag == 5) {
                            /* RC5 v0.5: borrowed вложен struct — retain_<T>.
                             * M24-RC: per-instance име при targs */
                            const char *vn = val->type && val->type->name ?
                                val->type->name : NULL;
                            if (vn) {
                                char *vm = rc_rc_type_name(val->type, NULL);
                                fprintf(f, "baga_rc_retain_%s(__rc_sl.%s); ",
                                        vm, fm2);
                                free(vm);
                            }
                        }
                        else if (vltag == 6) {
                            /* RC5 v0.10: borrowed enum — retain_E по runtime
                             * tag (досега падаше в generic cast към void * —
                             * compile error под --rc) */
                            const char *vn = val->type && val->type->name ?
                                val->type->name : NULL;
                            if (vn) {
                                char *vm = mangle_name(vn);
                                fprintf(f, "baga_rc_retain_%s(__rc_sl.%s); ",
                                        vm, fm2);
                                free(vm);
                            }
                        }
                        else if (vltag == 2)
                            fprintf(f, "baga_rc_retain((void *)__rc_sl.%s.data); ",
                                    fm2);
                        else
                            fprintf(f, "baga_rc_retain((void *)__rc_sl.%s); ",
                                    fm2);
                        free(fm2);
                        continue;
                    }
                    if (val->kind != NODE_IDENT) continue;
                    int si = rc_find(cg, val->name);
                    if (si >= 0 && !cg->rc_locals.data[si].dead &&
                        !rc_is_move(cg, val, si)) {
                        rc_emit_retain_val(cg, cg->rc_locals.data[si].tag,
                                           cg->rc_locals.data[si].type,
                                           cg->rc_locals.data[si].type_node,
                                           cg->rc_locals.data[si].name);
                        continue;
                    }
                    /* RC5 v1.0a: untrack-нат ident — retain през полето
                     * (като borrowed_init; match binding няма rc_find запис) */
                    if (si < 0 && vltag != 0) {
                        char *fm2 = mangle_name(n->lit_fields[i]);
                        char cbuf[160];
                        snprintf(cbuf, sizeof cbuf, "__rc_sl.%s", fm2);
                        rc_emit_retain_val(cg, vltag, val->type, NULL, cbuf);
                        free(fm2);
                    }
                }
                fprintf(f, "__rc_sl; })");
            }
            /* RC2: move сайтове — маркирай СЛЕД литерала (стойността вече е
             * прочетена); референцията преминава към полето без retain */
            if (cg->rc) {
                for (int i = 0; i < n->n_lit_fields; i++) {
                    Node *val = n->lit_values.data[i];
                    if (val->kind != NODE_IDENT) continue;
                    int si = rc_find(cg, val->name);
                    if (si >= 0 && !cg->rc_locals.data[si].dead &&
                        rc_is_move(cg, val, si))
                        rc_do_move(cg, si);
                }
            }
            free(sm);
            break;
        }

        case NODE_CATCH: {
            /* M20: flatten веригата от payload catches; payload-less
             * catches са compile-time фикция — базовият израз се emit-ва
             * направо. Схема:
             *   ({ T __v; baga_eff __e; __v = <base>; __e = baga_eff_tl;
             *      baga_eff_tl = (baga_eff){0};
             *      if (__e.tag == TAG1) { __v = <h1 с binding>; }
             *      else if (__e.tag == TAG2) { … }
             *      else if (__e.tag != 0) { baga_eff_tl = __e; return ZERO; }
             *      __v; }) */
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
                /* без payload ефекти — старото поведение (compile-time).
                 * RC8: през temp машината — аргументите на базовия call са
                 * fresh heap temp-ове и иначе текат (като при TRY). */
                Node *base = n;
                while (base && base->kind == NODE_CATCH) base = base->catch_expr;
                emit_rc_stmt_expr_root(cg, base, 1);
                break;
            }
            {
                FILE *f = cg->out;
                int tid = cg->tmp_counter++;
                /* базовият израз: най-вътрешният (TRY вътре не прави
                 * собствен return — веригата решава) */
                Node *base = n;
                while (base && base->kind == NODE_CATCH) base = base->catch_expr;
                fprintf(f, "({ ");
                if (n->type) emit_ctype(cg, n->type); else fprintf(f, "int64_t");
                fprintf(f, " __e%d; baga_eff __ee%d; ", tid, tid);
                cg->eff_depth++;
                fprintf(f, "__e%d = ", tid);
                emit_rc_stmt_expr_root(cg, base, 1);
                fprintf(f, "; __ee%d = baga_eff_tl; baga_eff_tl = (baga_eff){0}; ",
                        tid);
                cg->eff_depth--;
                /* веригата от catch-ове, от външен към вътрешен ред */
                int first = 1;
                Node *scan = n;
                while (scan && scan->kind == NODE_CATCH) {
                    Type *et = scan->catch_expr ? scan->catch_expr->type : NULL;
                    Type *pl = type_effect_payload(et, scan->catch_effect);
                    if (!pl) { scan = scan->catch_expr; continue; }
                    int tag = eff_tag(cg, scan->catch_effect);
                    fprintf(f, "%sif (__ee%d.tag == %d) { ",
                            first ? "" : "else ", tid, tag);
                    if (scan->catch_binding) {
                        char *bm = mangle_name(scan->catch_binding);
                        fprintf(f, "{ ");
                        if (pl) emit_ctype(cg, pl); else fprintf(f, "int64_t");
                        fprintf(f, " %s = __ee%d.%s; ", bm, tid, eff_slot_field(pl->kind));
                        const char *saved_b = cg->eff_binding;
                        const char *saved_c = cg->eff_binding_c;
                        cg->eff_binding = scan->catch_binding;
                        cg->eff_binding_c = bm;
                        fprintf(f, "__e%d = ", tid);
                        if (scan->catch_handler->kind == NODE_BLOCK)
                            emit_catch_handler_block(cg, scan->catch_handler, n->type);
                        else
                            emit_rc_stmt_expr_root(cg, scan->catch_handler, 1);
                        fprintf(f, "; } ");
                        cg->eff_binding = saved_b;
                        cg->eff_binding_c = saved_c;
                        free(bm);
                    } else {
                        fprintf(f, "__e%d = ", tid);
                        if (scan->catch_handler->kind == NODE_BLOCK)
                            emit_catch_handler_block(cg, scan->catch_handler, n->type);
                        else
                            emit_rc_stmt_expr_root(cg, scan->catch_handler, 1);
                        fprintf(f, "; ");
                    }
                    fprintf(f, "} ");
                    first = 0;
                    scan = scan->catch_expr;
                }
                if (first) {
                    /* всички catches са payload-less — невъзможно тук */
                } else {
                    fprintf(f, "else if (__ee%d.tag != 0) { baga_eff_tl = __ee%d; ",
                            tid, tid);
                    if (cg->rc) rc_release_all(cg, -1);
                    emit_eff_return_zero(cg);
                    fprintf(f, "; } ");
                }
                fprintf(f, "__e%d; })", tid);
            }
            break;
        }

        case NODE_TRY:
            /* M20: e? — ако изразът носи payload ефекти, проверка на слота;
             * иначе (и в catch верига) — чист passthrough */
            if (type_has_payload_effects(n->try_expr ? n->try_expr->type : NULL) &&
                cg->eff_depth == 0) {
                FILE *f = cg->out;
                Type *et = n->try_expr->type;
                Type *base = et ? type_new(et->kind) : NULL;
                if (base) { base->elem = et->elem; base->name = et->name; }
                fprintf(f, "({ ");
                if (base) emit_ctype(cg, base); else fprintf(f, "int64_t");
                fprintf(f, " __t%d = ", cg->tmp_counter);
                emit_rc_stmt_expr_root(cg, n->try_expr, 1);
                fprintf(f, "; if (baga_eff_tl.tag) { ");
                if (cg->rc) rc_release_all(cg, -1);
                emit_eff_return_zero(cg);
                fprintf(f, "; } __t%d; })", cg->tmp_counter++);
            } else {
                /* RC8: passthrough и тук минава през temp машината —
                 * `let co = f(http_reader(fd))?` с payload-less ефекти
                 * (IO/Net/Time/Random) иначе изобщо не събира temp-ове и
                 * свежият аргумент (rc=1) тече на всяка заявка. */
                emit_rc_stmt_expr_root(cg, n->try_expr, 1);
            }
            break;

        case NODE_RAISE: {
            /* M20: raise !E(payload) — задава слота и ИЗЛИЗА от функцията
             * (като `?`). Преди това беше само comma-израз: кодът след
             * raise продължаваше и можеше да падне преди caller-ът да
             * види слота. return вътре в ({…}) напуска enclosing fn.
             * RC: преди изхода се release-ват локалите на fn-а (като при
             * return). Heap payload, който НЕ е fresh (ident/поле/borrowed),
             * споделя референция с източника си — retain-ваме го в слота
             * преди release-ите. Fresh payload (owned call/enum ctor/struct
             * литерал) се move-ва в слота: return-ът прескачa statement-end
             * temp release-а. */
            FILE *f = cg->out;
            int tag = eff_tag(cg, n->raise_effect);
            fprintf(f, "({ baga_eff_tl.tag = %d; ", tag);
            if (n->raise_payload && n->raise_payload->type) {
                Node *p = n->raise_payload;
                fprintf(f, "baga_eff_tl.%s = (",
                        eff_slot_field(p->type->kind));
                emit_expr(cg, p);
                fprintf(f, "); ");
                if (cg->rc) {
                    int ptag = rc_heap_tag(cg, p->type);
                    if (ptag && !rc_tmp_fresh(cg, p) &&
                        !rc_is_enum_ctor(cg, p) &&
                        p->kind != NODE_STRUCT_LIT) {
                        char pbuf[64];
                        snprintf(pbuf, sizeof pbuf, "baga_eff_tl.%s",
                                 eff_slot_field(p->type->kind));
                        rc_emit_retain_val(cg, ptag, p->type, NULL, pbuf);
                    }
                }
            }
            if (cg->rc) rc_release_all(cg, -1);
            emit_eff_return_zero(cg);
            fprintf(f, "; ");
            emit_zero_val(cg, NULL);
            fprintf(f, "; })");
            break;
        }

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
            } else if (ek == TYPE_F64) {
                fprintf(f, "baga_f64_to_str(");
                emit_expr(cg, n->to_str_expr);
                fprintf(f, ")");
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
            char lnm[256];
            char *lm = mangle_name(lambda_emit_name(cg, n, lnm, sizeof lnm));
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
                /* RC2: last-use pre-pass за тялото на ламбдата; save/restore —
                 * emission-ът на enclosing fn продължава след тази ламбда */
                RcUseVec saved_lus = {0};
                Node *saved_fn = cg->rc_cur_fn;
                int saved_marks_len = cg->rc_marks.len;
                if (cg->rc) {
                    saved_lus = cg->rc_lus;
                    cg->rc_lus.data = NULL; cg->rc_lus.len = 0; cg->rc_lus.cap = 0;
                    /* RC1.3: ламбдата е отделна fn — нейни mark watermark-ове
                     * (споделеният vec се пази; възстановява се по-долу) */
                    cg->rc_marks.len = 0;
                    rc_lu_stmts(cg, stmts, NULL, 0);
                    cg->rc_cur_fn = n->fn_body;
                }
                /* RC1: wrapper-ът на ламбдата е отделна C функция — собствен
                 * scope; параметрите и capture-ите са заемани (is_param=1) */
                rc_push_scope(cg, 0);
                int saved_fn_base = cg->rc_fn_base;
                if (cg->rc) cg->rc_fn_base = cg->rc_scopes.len - 1;
                if (cg->rc) {
                    /* RC5 v1.0a: struct/enum параметри и capture-и — heap tag,
                     * за да `return p` retain-ва (същата конвенция като fn) */
                    for (int i = 0; i < n->params.len; i++)
                        rc_register_node(cg, n->params.data[i]->param_name,
                                    rc_heap_tag_node(cg, n->params.data[i]->param_type),
                                    NULL, n->params.data[i]->param_type, 1);
                    for (int i = 0; i < n->captures.len; i++)
                        rc_register(cg, n->captures.data[i]->param_name,
                                    rc_heap_tag(cg, n->captures.data[i]->type),
                                    n->captures.data[i]->type, 1);
                }
                for (int i = 0; i < stmts->len; i++) {
                    Node *s = stmts->data[i];
                    /* RC2.1: контекст за borrowed-pair анализа */
                    cg->rc_cur_blk = n->fn_body; cg->rc_cur_idx = i;
                    if (has_ret && i == stmts->len - 1 && s->kind == NODE_EXPR_STMT) {
                        if (cg->rc) {
                            /* RC4: temp-ове в implicit return (като при fn) */
                            RcTmpVec saved_tmps = {0};
                            int saved_on = 0;
                            rc_tmp_begin(cg, s->expr, 1, &saved_tmps, &saved_on);
                            emit_return_val(cg, s->expr);
                            rc_tmp_end(cg, &saved_tmps, saved_on);
                        } else {
                            cg->indent--;
                            emit_indent(cg);
                            fprintf(mb, "return ");
                            emit_expr(cg, s->expr);
                            fprintf(mb, ";\n");
                            cg->indent++;
                        }
                    } else {
                        emit_stmt(cg, s);
                    }
                }
                rc_pop_scope(cg);
                cg->rc_fn_base = saved_fn_base;
                if (cg->rc) {
                    /* RC2: възстанови last-use записите на enclosing fn */
                    vec_free(cg->rc_lus);
                    cg->rc_lus = saved_lus;
                    cg->rc_cur_fn = saved_fn;
                    cg->rc_marks.len = saved_marks_len;  /* RC1.3 */
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
                /* str scrutinee: сравнявай съдържание (strcmp), не указатели.
                 * Преди това `match s { "a" => .. }` се компилираше до
                 * `int64_t _mv = s; if (_mv == "a")` — указателно равенство.
                 * С литерал минаваше по случайност (интернинг); с изчислен
                 * низ винаги падаше в `_` → тихо грешен отговор. gcc warning-ът
                 * се поглъща от system() в main.c, така че не се виждаше. */
                int str_match = n->match_expr->type &&
                                n->match_expr->type->kind == TYPE_STR;
                /* Подсилване: низов литерал като патерн има смисъл само
                 * срещу низ — покрива случаи, в които типът на scrutinee-то
                 * не е наличен. */
                if (!str_match) {
                    for (int i = 0; i < n->match_arms.len; i++) {
                        Node *a = n->match_arms.data[i];
                        if (a->arm_pattern && a->arm_pattern->kind == NODE_STR_LIT) {
                            str_match = 1;
                            break;
                        }
                    }
                }
                /* GCC statement expression */
                if (is_void) {
                    if (str_match) fprintf(f, "({ const char *_mv%d = ", tmp);
                    else           fprintf(f, "({ int64_t _mv%d = ", tmp);
                } else {
                    if (str_match)
                        fprintf(f, "({ %s _mr%d = 0; const char *_mv%d = ",
                                ctype, tmp, tmp);
                    else
                        fprintf(f, "({ %s _mr%d = 0; int64_t _mv%d = ",
                                ctype, tmp, tmp);
                }
                emit_expr(cg, n->match_expr);
                fprintf(f, "; ");
                for (int i = 0; i < n->match_arms.len; i++) {
                    Node *arm = n->match_arms.data[i];
                    if (arm->arm_pattern) {
                        if (i > 0) fprintf(f, "else ");
                        if (str_match) {
                            fprintf(f, "if (strcmp(_mv%d, ", tmp);
                            emit_expr(cg, arm->arm_pattern);
                            fprintf(f, ") == 0) { ");
                        } else {
                            fprintf(f, "if (_mv%d == ", tmp);
                            emit_expr(cg, arm->arm_pattern);
                            fprintf(f, ") { ");
                        }
                    } else {
                        /* wildcard → else */
                        fprintf(f, "else { ");
                    }
                    /* emit arm body */
                    if (arm->arm_body && arm->arm_body->kind == NODE_BLOCK) {
                        for (int j = 0; j < arm->arm_body->stmts.len; j++) {
                            Node *s = arm->arm_body->stmts.data[j];
                            if (s->kind == NODE_RETURN && s->ret_val) {
                                rc_emit_match_arm_val(cg, s->ret_val, tmp,
                                                      is_void);
                                fprintf(f, "; ");
                            } else if (s->kind == NODE_EXPR_STMT) {
                                emit_rc_stmt_expr_root(cg, s->expr, 0);
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
                    const char *pvn = NULL;
                    if (arm->arm_pattern->kind == NODE_PATH)
                        pvn = arm->arm_pattern->path_variant;
                    else if (arm->arm_pattern->kind == NODE_IDENT)
                        pvn = arm->arm_pattern->name;
                    if (ed && pvn)
                        for (int j = 0; j < ed->n_variants; j++)
                            if (strcmp(ed->enum_variants[j], pvn) == 0)
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
                            rc_emit_match_arm_val(cg, s->ret_val, tmp,
                                                  is_void);
                            fprintf(f, "; ");
                        } else if (s->kind == NODE_EXPR_STMT) {
                            emit_rc_stmt_expr_root(cg, s->expr, 0);
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

        case NODE_BLOCK:
            emit_catch_handler_block(cg, n, n->type);
            break;

        default:
            c_unsupported_node("израз", n);
            break;
    }
}

/* ---- statement emission ---- */

