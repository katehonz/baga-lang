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

/* ---- RC1 (--rc): scope tracking + retain/release emission ----
 * Дизайн: docs/memory-rc-bg.md. Всички helper-и са no-op без cg->rc, така че
 * без флага изходът е бит-идентичен с предишния codegen. */

/* heap tag на inferred тип: 0 = не се track-ва, 1=str, 2=bytes, 3=Vec, 4=Map
 * (tag 5 = struct с heap полета — през rc_heap_tag, иска Codegen) */
static int rc_type_tag(Type *t) {
    if (!t) return 0;
    switch (t->kind) {
        case TYPE_STR:   return 1;
        case TYPE_BYTES: return 2;
        case TYPE_VEC:   return 3;
        case TYPE_MAP:   return 4;
        default:         return 0;
    }
}

/* същият tag, но от type AST възел (за fn параметри без inferred Type) */
static int rc_type_node_tag(Node *ty) {
    while (ty && (ty->kind == NODE_TYPE_EFFECT || ty->kind == NODE_TYPE_REF))
        ty = ty->inner_type;
    if (!ty || ty->kind != NODE_TYPE || !ty->type_name) return 0;
    if (strcmp(ty->type_name, "str") == 0)   return 1;
    if (strcmp(ty->type_name, "bytes") == 0) return 2;
    if (strcmp(ty->type_name, "Vec") == 0)   return 3;
    if (strcmp(ty->type_name, "Map") == 0)   return 4;
    return 0;
}

static Node *find_struct_decl(Codegen *cg, const char *name);
static Node *find_sum_enum_decl(Codegen *cg, const char *name);

/* RC5 v0.10: взаимна рекурсия struct↔enum (enum поле с heap payload брои) */
static int rc_enum_has_heap_d(Codegen *cg, Node *item, int depth);

/* RC5: struct с поне едно пряко str/bytes/Vec/Map поле.
 * v0.5: транзитивно — поле от struct тип с heap полета също брои (release_S
 * рекурсира). depth guard срещу лудост при циклични декларации (value-цикъл
 * е невалиден в C, но не разчитаме на checker-а).
 * v0.10: и поле от sum enum тип с heap payload брои (release_S вика
 * release_E за него). */
static int rc_struct_has_heap_d(Codegen *cg, const char *name, int depth) {
    if (depth > 32) return 0;
    Node *d = find_struct_decl(cg, name);
    if (!d) return 0;
    for (int i = 0; i < d->fields.len; i++) {
        Node *ft = d->fields.data[i]->fld_type;
        if (rc_type_node_tag(ft)) return 1;
        Node *t = ft;
        while (t && (t->kind == NODE_TYPE_EFFECT || t->kind == NODE_TYPE_REF))
            t = t->inner_type;
        if (t && t->kind == NODE_TYPE && t->type_name) {
            if (rc_struct_has_heap_d(cg, t->type_name, depth + 1))
                return 1;
            /* RC5 v0.10: enum поле с heap payload */
            Node *ed = find_sum_enum_decl(cg, t->type_name);
            if (ed && rc_enum_has_heap_d(cg, ed, depth + 1))
                return 1;
        }
    }
    return 0;
}

static int rc_struct_has_heap(Codegen *cg, const char *name) {
    return rc_struct_has_heap_d(cg, name, 0);
}

/* RC5 v0.5: име на struct типа на поле-възел, ако е вложен struct с heap
 * полета (иначе NULL). */
static const char *rc_nested_struct_field(Codegen *cg, Node *fld_type) {
    Node *t = fld_type;
    while (t && (t->kind == NODE_TYPE_EFFECT || t->kind == NODE_TYPE_REF))
        t = t->inner_type;
    if (t && t->kind == NODE_TYPE && t->type_name &&
        rc_struct_has_heap(cg, t->type_name))
        return t->type_name;
    return NULL;
}

/* RC5 v0.6: enum с поне един variant с heap payload (str/bytes/Vec/Map или
 * struct с heap полета). Enum payload в enum не се брои (leak-safe).
 * v0.10: depth-aware — взаимна рекурсия с rc_struct_has_heap_d. */
static int rc_enum_has_heap_d(Codegen *cg, Node *item, int depth) {
    if (depth > 32) return 0;
    if (!item || item->kind != NODE_ENUM) return 0;
    for (int j = 0; j < item->n_variants; j++) {
        Node *pt = item->enum_payloads ? item->enum_payloads[j] : NULL;
        if (!pt) continue;
        if (rc_type_node_tag(pt)) return 1;
        Node *t = pt;
        while (t && (t->kind == NODE_TYPE_EFFECT || t->kind == NODE_TYPE_REF))
            t = t->inner_type;
        if (t && t->kind == NODE_TYPE && t->type_name &&
            rc_struct_has_heap_d(cg, t->type_name, depth + 1))
            return 1;
    }
    return 0;
}

static int rc_enum_has_heap(Codegen *cg, Node *item) {
    return rc_enum_has_heap_d(cg, item, 0);
}

/* RC5 v0.10: име на enum типа на поле-възел, ако е sum enum с heap payload
 * (иначе NULL). Огледало на rc_nested_struct_field. */
static const char *rc_nested_enum_field(Codegen *cg, Node *fld_type) {
    Node *t = fld_type;
    while (t && (t->kind == NODE_TYPE_EFFECT || t->kind == NODE_TYPE_REF))
        t = t->inner_type;
    if (t && t->kind == NODE_TYPE && t->type_name) {
        Node *ed = find_sum_enum_decl(cg, t->type_name);
        if (ed && rc_enum_has_heap(cg, ed))
            return t->type_name;
    }
    return NULL;
}

static int rc_heap_tag(Codegen *cg, Type *t) {
    int tag = rc_type_tag(t);
    if (tag) return tag;
    if (t && t->kind == TYPE_STRUCT && t->name && rc_struct_has_heap(cg, t->name))
        return 5;
    /* RC5 v0.6: enum с heap payload */
    if (t && t->kind == TYPE_ENUM && t->name) {
        Node *ed = find_sum_enum_decl(cg, t->name);
        if (ed && rc_enum_has_heap(cg, ed)) return 6;
    }
    return 0;
}

static int rc_heap_tag_node(Codegen *cg, Node *ty) {
    int tag = rc_type_node_tag(ty);
    if (tag) return tag;
    while (ty && (ty->kind == NODE_TYPE_EFFECT || ty->kind == NODE_TYPE_REF))
        ty = ty->inner_type;
    if (ty && ty->kind == NODE_TYPE && ty->type_name) {
        if (rc_struct_has_heap(cg, ty->type_name))
            return 5;
        /* RC5 v0.6 */
        Node *ed = find_sum_enum_decl(cg, ty->type_name);
        if (ed && rc_enum_has_heap(cg, ed)) return 6;
    }
    return 0;
}

/* копие ли е целият ident (не поле-четене s.x) някъде в израза? */
static int rc_expr_copies_ident(Node *n, const char *name) {
    if (!n || !name) return 0;
    if (n->kind == NODE_IDENT)
        return n->name && strcmp(n->name, name) == 0;
    if (n->kind == NODE_FIELD) {
        if (n->field_obj && n->field_obj->kind == NODE_IDENT &&
            n->field_obj->name && strcmp(n->field_obj->name, name) == 0)
            return 0;
        return rc_expr_copies_ident(n->field_obj, name);
    }
    switch (n->kind) {
        case NODE_BINARY:
            return rc_expr_copies_ident(n->left, name) ||
                   rc_expr_copies_ident(n->right, name);
        case NODE_UNARY:
            return rc_expr_copies_ident(n->operand, name);
        case NODE_CALL: {
            if (rc_expr_copies_ident(n->callee, name)) return 1;
            for (int i = 0; i < n->args.len; i++)
                if (rc_expr_copies_ident(n->args.data[i], name)) return 1;
            return 0;
        }
        case NODE_INDEX:
            return rc_expr_copies_ident(n->obj, name) ||
                   rc_expr_copies_ident(n->index, name);
        case NODE_STRUCT_LIT:
            for (int i = 0; i < n->n_lit_fields; i++)
                if (rc_expr_copies_ident(n->lit_values.data[i], name)) return 1;
            return 0;
        case NODE_ASSIGN:
            return rc_expr_copies_ident(n->assign_val, name);
        case NODE_TO_STR:
            return rc_expr_copies_ident(n->to_str_expr, name);
        default:
            return 0;
    }
}

static void rc_emit_retain_val(Codegen *cg, int tag, Type *ty, Node *tnode,
                               const char *cname) {
    FILE *f = cg->out;
    if (tag == 5 || tag == 6) {
        /* struct (5) или enum с heap payload (6) — retain_<име> по tag */
        const char *sn = (ty && ty->name) ? ty->name : NULL;
        if (!sn && tnode) {
            Node *t = tnode;
            while (t && (t->kind == NODE_TYPE_EFFECT || t->kind == NODE_TYPE_REF))
                t = t->inner_type;
            if (t && t->kind == NODE_TYPE) sn = t->type_name;
        }
        if (!sn) return;
        char *sm = mangle_name(sn);
        fprintf(f, "baga_rc_retain_%s(%s); ", sm, cname);
        free(sm);
    } else if (tag == 2)
        fprintf(f, "baga_rc_retain((void *)%s.data); ", cname);
    else if (tag)
        fprintf(f, "baga_rc_retain((void *)%s); ", cname);
}

/* намира track-нат локал по baga име; връща индекс (-1 = няма).
 * Търси от върха надолу — вътрешно засенчване печели. */
static int rc_find(Codegen *cg, const char *name) {
    if (!cg->rc) return -1;
    char *m = mangle_name(name);
    int found = -1;
    for (int i = cg->rc_locals.len - 1; i >= 0; i--)
        if (strcmp(cg->rc_locals.data[i].name, m) == 0) { found = i; break; }
    free(m);
    return found;
}

/* borrowed init: vec_get/map_get/struct поле/h_* връщат референция към
 * чужда собственост. При връзване в локал тя се RETAIN-ва (нова собствена
 * референция), не се пропуска — така release при scope exit е балансиран. */
static int rc_borrowed_init(Node *init) {
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

/* RC5 v0.6: конструкторът на sum enum се познава с rc_is_enum_ctor
 * (дефиниран при RC4 temp машината по-долу). */

/* RC1: elem_kind за release на Vec. Като baga_drop_vec (0 inline, 1 str,
 * 2 struct box, 3 nested vec) + 4 = bytes box — bytes box-ът държи
 * retain-нати data, които трябва да се release-нат (drop схемата ги
 * различава само по elem_size, което не стига). */
static int rc_vec_elem_kind(Type *vty, char *sz, size_t szn) {
    Type *e = vty ? vty->elem : NULL;
    snprintf(sz, szn, "0");
    if (!e) return 0;
    if (e->kind == TYPE_STR) return 1;
    if (e->kind == TYPE_BYTES) {
        snprintf(sz, szn, "(int64_t)sizeof(baga_bytes)");
        return 4;
    }
    if (e->kind == TYPE_VEC) return 3;
    if (e->name && (e->kind == TYPE_STRUCT || e->kind == TYPE_ENUM)) {
        char *m = mangle_name(e->name);
        snprintf(sz, szn, "(int64_t)sizeof(%s)", m);
        free(m);
        return 2;
    }
    return 0;
}

/* RC1: val_tag за release на Map: 0 inline (i64/f64), 1 str, 2 bytes,
 * 3 struct box (free на pv по val_size; полетата вътре не се track-ват). */
static int rc_map_val_tag(Type *mty, char *sz, size_t szn) {
    Type *v = mty ? mty->elem : NULL;
    snprintf(sz, szn, "0");
    if (!v) return 0;
    if (v->kind == TYPE_STR) return 1;
    if (v->kind == TYPE_BYTES) return 2;
    if (v->name && (v->kind == TYPE_STRUCT || v->kind == TYPE_ENUM)) {
        char *m = mangle_name(v->name);
        snprintf(sz, szn, "(int64_t)sizeof(%s)", m);
        free(m);
        return 3;
    }
    return 0;
}

/* същите два resolver-а, но от анотационен type AST възел (`let v: Vec<str>`)
 * — inferred Type на vec_new()/map_new() няма elem/key информация */
static int rc_vec_elem_kind_node(Node *ty, char *sz, size_t szn) {
    Node *e = ty ? ty->inner_type : NULL;
    snprintf(sz, szn, "0");
    if (!e || e->kind != NODE_TYPE || !e->type_name) return 0;
    if (strcmp(e->type_name, "str") == 0) return 1;
    if (strcmp(e->type_name, "bytes") == 0) {
        snprintf(sz, szn, "(int64_t)sizeof(baga_bytes)");
        return 4;
    }
    if (strcmp(e->type_name, "Vec") == 0) return 3;
    if (strcmp(e->type_name, "i64") == 0 || strcmp(e->type_name, "i32") == 0 ||
        strcmp(e->type_name, "f64") == 0 || strcmp(e->type_name, "bool") == 0)
        return 0;
    char *m = mangle_name(e->type_name);
    snprintf(sz, szn, "(int64_t)sizeof(%s)", m);
    free(m);
    return 2;
}

static int rc_map_val_tag_node(Node *ty, char *sz, size_t szn) {
    Node *v = ty ? ty->inner_type2 : NULL;
    snprintf(sz, szn, "0");
    if (!v || v->kind != NODE_TYPE || !v->type_name) return 0;
    if (strcmp(v->type_name, "str") == 0) return 1;
    if (strcmp(v->type_name, "bytes") == 0) return 2;
    if (strcmp(v->type_name, "i64") == 0 || strcmp(v->type_name, "i32") == 0 ||
        strcmp(v->type_name, "f64") == 0 || strcmp(v->type_name, "bool") == 0)
        return 0;
    char *m = mangle_name(v->type_name);
    snprintf(sz, szn, "(int64_t)sizeof(%s)", m);
    free(m);
    return 3;
}

/* RC5 v0.2: box destructor за struct елементи/стойности с heap полета —
 * "baga_rc_relf_<S>" или "0" (няма heap полета / не е struct → само free).
 * Container release не знае типа статично, затова получава fn pointer.
 * v0.10: и enum елемент/стойност с heap payload — "baga_rc_relf_<E>"
 * (release_E по runtime tag; container-ът free-ва box-а след destructor-а). */
static void rc_box_rel(Codegen *cg, const char *type_name, char *rel, size_t reln) {
    snprintf(rel, reln, "0");
    if (!cg->rc || !type_name) return;
    int heap = rc_struct_has_heap(cg, type_name);
    if (!heap) {
        /* RC5 v0.10 */
        Node *ed = find_sum_enum_decl(cg, type_name);
        heap = ed && rc_enum_has_heap(cg, ed);
    }
    if (!heap) return;
    char *m = mangle_name(type_name);
    snprintf(rel, reln, "baga_rc_relf_%s", m);
    free(m);
}

/* RC5 v0.10: елементен/стойностен тип (struct или enum), чийто box изисква
 * destructor + retain при споделяне — общ предикат за push/set/del/slice
 * сайтовете (досега бяха struct-only). */
static int rc_box_tracked(Codegen *cg, Type *et) {
    if (!cg->rc || !et || !et->name) return 0;
    if (et->kind == TYPE_STRUCT)
        return rc_struct_has_heap(cg, et->name);
    if (et->kind == TYPE_ENUM) {
        Node *ed = find_sum_enum_decl(cg, et->name);
        return ed && rc_enum_has_heap(cg, ed);
    }
    return 0;
}

/* RC5 v0.9: destructor за kind 3 (вложен Vec) елементи — "baga_rc_relv_<S>",
 * когато елементът е Vec<S> и S е struct с heap полета; иначе "0" (старото
 * поведение: вътрешният vec се release-ва като kind 0 и S полетата текат —
 * leak-safe). Shim-ът е едно ниво: Vec<Vec<Vec<S>>> остава на старото
 * поведение (документирана граница). */
static void rc_nested_vec_rel_type(Codegen *cg, Type *vty, char *rel, size_t reln) {
    snprintf(rel, reln, "0");
    if (!cg->rc || !vty || !vty->elem) return;
    Type *inner = vty->elem;
    if (inner->kind != TYPE_VEC || !inner->elem) return;
    Type *es = inner->elem;
    if (es->kind != TYPE_STRUCT || !es->name ||
        !rc_struct_has_heap(cg, es->name)) return;
    char *m = mangle_name(es->name);
    snprintf(rel, reln, "baga_rc_relv_%s", m);
    free(m);
}

/* същият resolver, но от анотационен type AST възел (`let v: Vec<Vec<S>>`) */
static void rc_nested_vec_rel_node(Codegen *cg, Node *ty, char *rel, size_t reln) {
    snprintf(rel, reln, "0");
    if (!cg->rc || !ty) return;
    Node *inner = ty->inner_type;
    if (!inner || inner->kind != NODE_TYPE || !inner->type_name ||
        strcmp(inner->type_name, "Vec") != 0) return;
    Node *es = inner->inner_type;
    if (!es || es->kind != NODE_TYPE || !es->type_name ||
        !rc_struct_has_heap(cg, es->type_name)) return;
    char *m = mangle_name(es->type_name);
    snprintf(rel, reln, "baga_rc_relv_%s", m);
    free(m);
}

/* елементен/стойностен тип на Vec/Map локал (inferred Type, после анотация) */
static const char *rc_vec_elem_name(RcLocal *e) {
    if (e->type && e->type->elem && e->type->elem->name)
        return e->type->elem->name;
    Node *it = e->type_node ? e->type_node->inner_type : NULL;
    if (it && it->kind == NODE_TYPE) return it->type_name;
    return NULL;
}

static const char *rc_map_val_name(RcLocal *e) {
    if (e->type && e->type->elem && e->type->elem->name)
        return e->type->elem->name;
    Node *it = e->type_node ? e->type_node->inner_type2 : NULL;
    if (it && it->kind == NODE_TYPE) return it->type_name;
    return NULL;
}

/* комбинирани resolver-и: inferred Type първо, анотация като резерва */
static int rc_vec_kind_of(RcLocal *e, char *sz, size_t szn) {
    int k = rc_vec_elem_kind(e->type, sz, szn);
    if (k == 0 && e->type_node)
        k = rc_vec_elem_kind_node(e->type_node, sz, szn);
    return k;
}

static int rc_map_tag_of(RcLocal *e, char *sz, size_t szn) {
    int t = rc_map_val_tag(e->type, sz, szn);
    if (t == 0 && e->type_node)
        t = rc_map_val_tag_node(e->type_node, sz, szn);
    return t;
}

/* RC5 v0.9: комбиниран nested resolver — inferred Type първо, анотация после */
static void rc_vec_nested_rel_of(Codegen *cg, RcLocal *e, char *rel, size_t reln) {
    rc_nested_vec_rel_type(cg, e->type, rel, reln);
    if (strcmp(rel, "0") == 0 && e->type_node)
        rc_nested_vec_rel_node(cg, e->type_node, rel, reln);
}

/* emit-ва един release ред (отстъпка + newline) за локал от стека */
static void rc_emit_release(Codegen *cg, RcLocal *e) {
    FILE *f = cg->out;
    char sz[160];
    emit_indent(cg);
    switch (e->tag) {
        case 1: fprintf(f, "baga_rc_release_str(%s);\n", e->name); break;
        case 2: fprintf(f, "baga_rc_release_bytes(%s);\n", e->name); break;
        case 3: {
            char rel[160];
            int k = rc_vec_kind_of(e, sz, sizeof sz);
            rc_box_rel(cg, rc_vec_elem_name(e), rel, sizeof rel);
            /* RC5 v0.9: вложен Vec<S> — destructor за S полетата на вътрешния */
            if (k == 3) rc_vec_nested_rel_of(cg, e, rel, sizeof rel);
            fprintf(f, "baga_rc_release_vec(%s, %d, %s, %s);\n", e->name,
                    k, sz, rel);
            break;
        }
        case 4: {
            char rel[160];
            rc_box_rel(cg, rc_map_val_name(e), rel, sizeof rel);
            fprintf(f, "baga_rc_release_map(%s, %d, %s, %s);\n", e->name,
                    rc_map_tag_of(e, sz, sizeof sz), sz, rel);
            break;
        }
        case 5: {
            const char *sn = (e->type && e->type->name) ? e->type->name :
                (e->type_node && e->type_node->type_name) ? e->type_node->type_name : NULL;
            if (sn) {
                char *sm = mangle_name(sn);
                fprintf(f, "baga_rc_release_%s(%s);\n", sm, e->name);
                free(sm);
            }
            break;
        }
        /* RC5 v0.6: enum с heap payload — release по runtime tag */
        case 6: {
            const char *en = (e->type && e->type->name) ? e->type->name :
                (e->type_node && e->type_node->type_name) ? e->type_node->type_name : NULL;
            if (en) {
                char *em = mangle_name(en);
                fprintf(f, "baga_rc_release_%s(%s);\n", em, e->name);
                free(em);
            }
            break;
        }
    }
}

static void rc_push_scope(Codegen *cg, int is_loop) {
    if (!cg->rc) return;
    vec_push(cg->rc_scopes, ((RcScope){ cg->rc_locals.len, is_loop }));
}

/* край на block: release на локалите на scope-а в обратен ред (params и
 * drop()нати — не), после pop. Извиква се преди затварящата '}'. */
static void rc_pop_scope(Codegen *cg) {
    if (!cg->rc || cg->rc_scopes.len == 0) return;
    RcScope sc = cg->rc_scopes.data[--cg->rc_scopes.len];
    for (int i = cg->rc_locals.len - 1; i >= sc.top; i--) {
        RcLocal *e = &cg->rc_locals.data[i];
        if (!e->is_param && !e->dead) rc_emit_release(cg, e);
        free(e->name);
    }
    cg->rc_locals.len = sc.top;
}

/* return: release на scopes на ТЕКУЩАТА функция (от върха до rc_fn_base;
 * без params/dead; skip_idx = move семантика). */
static void rc_release_all(Codegen *cg, int skip_idx) {
    if (!cg->rc) return;
    int base = cg->rc_fn_base >= 0 ? cg->rc_scopes.data[cg->rc_fn_base].top : 0;
    for (int i = cg->rc_locals.len - 1; i >= base; i--) {
        RcLocal *e = &cg->rc_locals.data[i];
        if (e->is_param || e->dead || i == skip_idx) continue;
        rc_emit_release(cg, e);
    }
}

/* break/continue: release на scopes до най-близкото loop тяло (вкл. него —
 * нормалният му release в края на блока се прескача от скока). */
static void rc_release_to_loop(Codegen *cg) {
    if (!cg->rc) return;
    for (int i = cg->rc_scopes.len - 1; i >= 0; i--) {
        if (!cg->rc_scopes.data[i].is_loop) continue;
        int top = cg->rc_scopes.data[i].top;
        for (int j = cg->rc_locals.len - 1; j >= top; j--) {
            RcLocal *e = &cg->rc_locals.data[j];
            if (!e->is_param && !e->dead) rc_emit_release(cg, e);
        }
        return;
    }
}

static void rc_register_node(Codegen *cg, const char *name, int tag, Type *ty,
                             Node *type_node, int is_param) {
    if (!cg->rc || !tag) return;
    vec_push(cg->rc_locals,
             ((RcLocal){ mangle_name(name), tag, ty, type_node, is_param, 0 }));
}

static void rc_register(Codegen *cg, const char *name, int tag, Type *ty,
                        int is_param) {
    rc_register_node(cg, name, tag, ty, NULL, is_param);
}

/* ---- RC2 (move elision): last-use pre-pass + copy-site проверка ----
 * Дизайн: docs/move-semantics-bg.md. Per функция def-use обход на AST-то;
 * move сайт е копие (let alias / struct-lit embed / field assign / assign),
 * чийто източник е при ПОСЛЕДНАТА си текстуална употреба — retain-ът се
 * пропуска, binding-ът се маркира dead (scope release отпада).
 * Консервативно: shadowing, lambda capture, match arm, loop-carried
 * (употреба в/след цикъл при binding извън него) изключват move. */

/* флагове на обхода: bit0 = в if/match клон, bit1 = в match arm */
#define RC_LU_COND  1
#define RC_LU_MATCH 2

static RcUse *rc_lu_rec(Codegen *cg, const char *name) {
    for (int i = 0; i < cg->rc_lus.len; i++)
        if (strcmp(cg->rc_lus.data[i].name, name) == 0)
            return &cg->rc_lus.data[i];
    vec_push(cg->rc_lus, ((RcUse){ name, NULL, NULL, NULL, 0, 0, 0, 0 }));
    return &cg->rc_lus.data[cg->rc_lus.len - 1];
}

/* употреба на ident в израз — последната текстуална печели (презаписва) */
static void rc_lu_use(Codegen *cg, Node *ident, Node *loop, int flags) {
    RcUse *u = rc_lu_rec(cg, ident->name);
    u->site = ident;
    u->site_loop = loop;
    u->site_in_cond = (flags & RC_LU_COND) != 0;
    if (flags & RC_LU_MATCH) u->in_match = 1;
}

static void rc_lu_stmts(Codegen *cg, NodeVec *stmts, Node *loop, int flags);

static void rc_lu_expr(Codegen *cg, Node *n, Node *loop, int flags) {
    if (!n) return;
    switch (n->kind) {
        case NODE_IDENT:
            rc_lu_use(cg, n, loop, flags);
            break;
        case NODE_BINARY:
            rc_lu_expr(cg, n->left, loop, flags);
            rc_lu_expr(cg, n->right, loop, flags);
            break;
        case NODE_UNARY:
            rc_lu_expr(cg, n->operand, loop, flags);
            break;
        case NODE_CALL:
            /* callee ident (fn име) — брои се като употреба (консервативно) */
            rc_lu_expr(cg, n->callee, loop, flags);
            for (int i = 0; i < n->args.len; i++)
                rc_lu_expr(cg, n->args.data[i], loop, flags);
            break;
        case NODE_IF:
            rc_lu_expr(cg, n->cond, loop, flags);
            if (n->then_br && n->then_br->kind == NODE_BLOCK)
                rc_lu_stmts(cg, &n->then_br->stmts, loop, flags | RC_LU_COND);
            if (n->else_br && n->else_br->kind == NODE_BLOCK)
                rc_lu_stmts(cg, &n->else_br->stmts, loop, flags | RC_LU_COND);
            break;
        case NODE_BLOCK:
            rc_lu_stmts(cg, &n->stmts, loop, flags);
            break;
        case NODE_INDEX:
            rc_lu_expr(cg, n->obj, loop, flags);
            rc_lu_expr(cg, n->index, loop, flags);
            break;
        case NODE_ELEM_REF:
            rc_lu_expr(cg, n->elem_obj, loop, flags);
            break;
        case NODE_FIELD:
            rc_lu_expr(cg, n->field_obj, loop, flags);
            break;
        case NODE_ASSIGN:
            /* target-ът е запис, не употреба; само сложни цели четат обекта */
            if (n->assign_target && n->assign_target->kind == NODE_FIELD)
                rc_lu_expr(cg, n->assign_target->field_obj, loop, flags);
            else if (n->assign_target && n->assign_target->kind == NODE_INDEX) {
                rc_lu_expr(cg, n->assign_target->obj, loop, flags);
                rc_lu_expr(cg, n->assign_target->index, loop, flags);
            }
            rc_lu_expr(cg, n->assign_val, loop, flags);
            break;
        case NODE_RANGE:
            rc_lu_expr(cg, n->range_lo, loop, flags);
            rc_lu_expr(cg, n->range_hi, loop, flags);
            break;
        case NODE_STRUCT_LIT:
            for (int i = 0; i < n->n_lit_fields; i++)
                rc_lu_expr(cg, n->lit_values.data[i], loop, flags);
            break;
        case NODE_TRY:
            rc_lu_expr(cg, n->try_expr, loop, flags);
            break;
        case NODE_CATCH:
            rc_lu_expr(cg, n->catch_expr, loop, flags);
            rc_lu_expr(cg, n->catch_handler, loop, flags);
            break;
        case NODE_TO_STR:
            rc_lu_expr(cg, n->to_str_expr, loop, flags);
            break;
        case NODE_LAMBDA:
            /* capture = употреба, която изключва move (env изживява scope-а);
             * тялото е отделна функция — анализира се при нейния emission */
            for (int i = 0; i < n->captures.len; i++)
                rc_lu_rec(cg, n->captures.data[i]->param_name)->captured = 1;
            break;
        case NODE_MATCH:
            rc_lu_expr(cg, n->match_expr, loop, flags);
            for (int i = 0; i < n->match_arms.len; i++) {
                Node *arm = n->match_arms.data[i];
                if (arm->arm_pattern)
                    rc_lu_expr(cg, arm->arm_pattern, loop, flags);
                if (arm->arm_body && arm->arm_body->kind == NODE_BLOCK)
                    rc_lu_stmts(cg, &arm->arm_body->stmts, loop,
                                flags | RC_LU_COND | RC_LU_MATCH);
            }
            break;
        default:
            break;
    }
}

static void rc_lu_stmt(Codegen *cg, Node *n, Node *loop, int flags) {
    if (!n) return;
    switch (n->kind) {
        case NODE_LET:
            {
                RcUse *u = rc_lu_rec(cg, n->let_name);
                if (u->n_lets == 0) u->decl_loop = loop;
                u->n_lets++;
            }
            if (n->let_init) rc_lu_expr(cg, n->let_init, loop, flags);
            break;
        case NODE_RETURN:
            if (n->ret_val) rc_lu_expr(cg, n->ret_val, loop, flags);
            break;
        case NODE_WHILE:
            rc_lu_expr(cg, n->while_cond, loop, flags);
            if (n->while_body && n->while_body->kind == NODE_BLOCK)
                rc_lu_stmts(cg, &n->while_body->stmts, n, flags);
            break;
        case NODE_FOR:
            if (n->for_iter) rc_lu_expr(cg, n->for_iter, loop, flags);
            if (n->for_body && n->for_body->kind == NODE_BLOCK)
                rc_lu_stmts(cg, &n->for_body->stmts, n, flags);
            break;
        case NODE_IF:
            rc_lu_expr(cg, n->cond, loop, flags);
            if (n->then_br && n->then_br->kind == NODE_BLOCK)
                rc_lu_stmts(cg, &n->then_br->stmts, loop, flags | RC_LU_COND);
            if (n->else_br && n->else_br->kind == NODE_BLOCK)
                rc_lu_stmts(cg, &n->else_br->stmts, loop, flags | RC_LU_COND);
            break;
        case NODE_EXPR_STMT:
            rc_lu_expr(cg, n->expr, loop, flags);
            break;
        case NODE_BLOCK:
            rc_lu_stmts(cg, &n->stmts, loop, flags);
            break;
        default:
            break;
    }
}

static void rc_lu_stmts(Codegen *cg, NodeVec *stmts, Node *loop, int flags) {
    for (int i = 0; i < stmts->len; i++)
        rc_lu_stmt(cg, stmts->data[i], loop, flags);
}

/* move сайт ли е тази употреба? ident е Node* на конкретната употреба,
 * src_idx — записът на източника в rc_locals (или -1). */
static int rc_is_move(Codegen *cg, Node *ident, int src_idx) {
    if (!cg->rc || !ident || ident->kind != NODE_IDENT) return 0;
    if (src_idx < 0 || cg->rc_locals.data[src_idx].is_param) return 0;
    for (int i = 0; i < cg->rc_lus.len; i++) {
        RcUse *u = &cg->rc_lus.data[i];
        if (strcmp(u->name, ident->name) != 0) continue;
        if (u->site != ident || u->n_lets != 1 || u->captured || u->in_match)
            return 0;
        /* loop правило: move в цикъл само ако bindingът е от същата итерация;
         * в клон на if/match вътре в цикъл — не (другият път тече всяка итер.) */
        if (u->site_loop != u->decl_loop) return 0;
        if (u->site_loop && u->site_in_cond) return 0;
        return 1;
    }
    return 0;
}

/* move сайт: пропусни retain-а, маркирай източника dead (site е текстуално
 * последната употреба → текстуалното маркиране е безопасно) */
static void rc_do_move(Codegen *cg, int src_idx) {
    cg->rc_locals.data[src_idx].dead = 1;
    cg->rc_moves++;
}

/* ---- RC2.1: elision на borrowed-retain двойки ----
 * `let x = <borrowed>` (vec_get/map_get/struct поле): ако x не escape-ва
 * от scope-а и източникът не се мутира в него, retain-ът при връзване и
 * scope release-ът са чиста загуба — пропускаме и двете (локалът е чисто
 * заеман, невидим за rc). Правила: docs/memory-rc-bg.md §v2.1. */

typedef struct {
    const char *x;    /* името на заемания локал */
    const char *src;  /* базовото име на източника (контейнер/struct) */
    int   bad;
} RcBe;

/* чисти builtin-и: повикване с източника като аргумент не го мутира */
static int rc_be_pure_call(const char *bn) {
    static const char *pure[] = {
        "len", "char_at", "byte_at", "byte_chr", "substr", "concat",
        "chr", "ord", "str_eq", "i64_to_str", "f64_to_str",
        "print", "println", "write", "eprintln",
        "vec_get", "vec_len", "vec_slice", "vec_concat",
        "map_get", "map_len", "map_has",
        "bytes_len", "bytes_at", "bytes_slice", "bytes_concat",
        "bytes_of_str", "str_of_bytes", "hex_encode", "hex_decode",
        "bytes_from_vec", "vec_from_bytes",
    };
    for (size_t i = 0; i < sizeof(pure) / sizeof(pure[0]); i++)
        if (strcmp(bn, pure[i]) == 0) return 1;
    return 0;
}

/* базов ident на верига полета (sel в sel.group_cols); NULL = неразпознат */
static const char *rc_be_base_ident(Node *e) {
    while (e) {
        if (e->kind == NODE_IDENT) return e->name;
        if (e->kind == NODE_FIELD) { e = e->field_obj; continue; }
        return NULL;
    }
    return NULL;
}

/* името на източника на заемането (контейнер/struct), или NULL */
static const char *rc_borrow_src_name(Node *init) {
    if (init->kind == NODE_CALL && init->callee &&
        init->callee->kind == NODE_IDENT &&
        (strncmp(init->callee->name, "vec_get", 7) == 0 ||
         strncmp(init->callee->name, "map_get", 7) == 0) &&
        init->args.len >= 1)
        return rc_be_base_ident(init->args.data[0]);
    if (init->kind == NODE_FIELD)
        return rc_be_base_ident(init);
    return NULL;
}

static int rc_be_is(Node *n, const char *name) {
    return n && n->kind == NODE_IDENT && strcmp(n->name, name) == 0;
}

/* RC1.3: mem_mark/mem_rewind ли е това повикване (директно или под ?) */
static int rc_is_mem_call(Node *n, const char *name) {
    if (n && n->kind == NODE_TRY) n = n->try_expr;
    return n && n->kind == NODE_CALL && n->callee &&
           n->callee->kind == NODE_IDENT &&
           strcmp(n->callee->name, name) == 0;
}

static void rc_be_expr(RcBe *be, Node *n);
static void rc_be_stmts(RcBe *be, NodeVec *stmts);

static void rc_be_call(RcBe *be, Node *n) {
    const char *bn = (n->callee && n->callee->kind == NODE_IDENT)
                     ? n->callee->name : NULL;
    if (bn && strcmp(bn, "drop") == 0) {
        /* drop на x = release път (трябва нашата референция); на src — мутация */
        for (int i = 0; i < n->args.len; i++)
            if (rc_be_is(n->args.data[i], be->x) ||
                rc_be_is(n->args.data[i], be->src))
                be->bad = 1;
        return;
    }
    if (bn && (strncmp(bn, "vec_push", 8) == 0 || strncmp(bn, "vec_set", 7) == 0 ||
               strncmp(bn, "map_set", 7) == 0 || strncmp(bn, "map_del", 7) == 0 ||
               strncmp(bn, "sort", 4) == 0)) {
        /* x в контейнерна операция = escape (правило а);
         * src (вкл. по база на field) в нея = мутация (правило б) */
        for (int i = 0; i < n->args.len; i++) {
            if (rc_be_is(n->args.data[i], be->x)) be->bad = 1;
            const char *b = rc_be_base_ident(n->args.data[i]);
            if (b && strcmp(b, be->src) == 0) be->bad = 1;
        }
    } else if (bn && !rc_be_pure_call(bn)) {
        /* консервативно: src в не-pure повикване = възможна мутация */
        for (int i = 0; i < n->args.len; i++) {
            const char *b = rc_be_base_ident(n->args.data[i]);
            if (b && strcmp(b, be->src) == 0) be->bad = 1;
        }
    }
    /* x като аргумент на pure/друго повикване = четене (params са borrowed) */
    if (n->callee && n->callee->kind != NODE_IDENT) rc_be_expr(be, n->callee);
    for (int i = 0; i < n->args.len; i++) rc_be_expr(be, n->args.data[i]);
}

static void rc_be_expr(RcBe *be, Node *n) {
    if (!n || be->bad) return;
    switch (n->kind) {
        case NODE_CALL: rc_be_call(be, n); break;
        case NODE_BINARY:
            rc_be_expr(be, n->left); rc_be_expr(be, n->right); break;
        case NODE_UNARY: rc_be_expr(be, n->operand); break;
        case NODE_IF:
            rc_be_expr(be, n->cond);
            if (n->then_br && n->then_br->kind == NODE_BLOCK)
                rc_be_stmts(be, &n->then_br->stmts);
            if (n->else_br && n->else_br->kind == NODE_BLOCK)
                rc_be_stmts(be, &n->else_br->stmts);
            break;
        case NODE_BLOCK: rc_be_stmts(be, &n->stmts); break;
        case NODE_INDEX:
            rc_be_expr(be, n->obj); rc_be_expr(be, n->index); break;
        case NODE_ELEM_REF: rc_be_expr(be, n->elem_obj); break;
        case NODE_FIELD: rc_be_expr(be, n->field_obj); break;
        case NODE_ASSIGN:
            /* преприсвояване на x = прекъсва borrow-а; мутация на src;
             * x/src като стойност = escape/алиас */
            if (rc_be_is(n->assign_target, be->x) ||
                rc_be_is(n->assign_target, be->src))
                be->bad = 1;
            if (n->assign_target && n->assign_target->kind == NODE_FIELD) {
                const char *b = rc_be_base_ident(n->assign_target->field_obj);
                if (b && strcmp(b, be->src) == 0) be->bad = 1;
                rc_be_expr(be, n->assign_target->field_obj);
            } else if (n->assign_target && n->assign_target->kind == NODE_INDEX) {
                const char *b = rc_be_base_ident(n->assign_target->obj);
                if (b && strcmp(b, be->src) == 0) be->bad = 1;
                rc_be_expr(be, n->assign_target->obj);
                rc_be_expr(be, n->assign_target->index);
            }
            if (rc_be_is(n->assign_val, be->x) ||
                rc_be_is(n->assign_val, be->src))
                be->bad = 1;
            else
                rc_be_expr(be, n->assign_val);
            break;
        case NODE_STRUCT_LIT:
            for (int i = 0; i < n->n_lit_fields; i++) {
                Node *val = n->lit_values.data[i];
                /* x embed = escape (правило а); src embed = алиас път —
                 * мутацията през него е невидима за анализа (правило б) */
                if (rc_be_is(val, be->x) || rc_be_is(val, be->src))
                    be->bad = 1;
                else
                    rc_be_expr(be, val);
            }
            break;
        case NODE_LAMBDA:
            /* capture на x (escape) или src (консервативно) */
            for (int i = 0; i < n->captures.len; i++) {
                const char *cn = n->captures.data[i]->param_name;
                if (strcmp(cn, be->x) == 0 || strcmp(cn, be->src) == 0)
                    be->bad = 1;
            }
            break;
        case NODE_RANGE:
            rc_be_expr(be, n->range_lo); rc_be_expr(be, n->range_hi); break;
        case NODE_TRY: rc_be_expr(be, n->try_expr); break;
        case NODE_CATCH:
            rc_be_expr(be, n->catch_expr); rc_be_expr(be, n->catch_handler); break;
        case NODE_TO_STR: rc_be_expr(be, n->to_str_expr); break;
        case NODE_MATCH:
            rc_be_expr(be, n->match_expr);
            for (int i = 0; i < n->match_arms.len; i++) {
                Node *arm = n->match_arms.data[i];
                if (arm->arm_pattern) rc_be_expr(be, arm->arm_pattern);
                if (arm->arm_body && arm->arm_body->kind == NODE_BLOCK)
                    rc_be_stmts(be, &arm->arm_body->stmts);
            }
            break;
        default:
            break;
    }
}

static void rc_be_stmt(RcBe *be, Node *n) {
    if (!n || be->bad) return;
    switch (n->kind) {
        case NODE_LET:
            /* alias на x (escape) или на src (скрита мутация по-късно) */
            if (rc_be_is(n->let_init, be->x) || rc_be_is(n->let_init, be->src))
                be->bad = 1;
            else
                rc_be_expr(be, n->let_init);
            break;
        case NODE_RETURN:
            /* return x = escape; return <израз с x> = четене */
            if (rc_be_is(n->ret_val, be->x)) be->bad = 1;
            else rc_be_expr(be, n->ret_val);
            break;
        case NODE_WHILE:
            rc_be_expr(be, n->while_cond);
            if (n->while_body && n->while_body->kind == NODE_BLOCK)
                rc_be_stmts(be, &n->while_body->stmts);
            break;
        case NODE_FOR:
            if (n->for_iter) rc_be_expr(be, n->for_iter);
            if (n->for_body && n->for_body->kind == NODE_BLOCK)
                rc_be_stmts(be, &n->for_body->stmts);
            break;
        case NODE_IF:
            rc_be_expr(be, n->cond);
            if (n->then_br && n->then_br->kind == NODE_BLOCK)
                rc_be_stmts(be, &n->then_br->stmts);
            if (n->else_br && n->else_br->kind == NODE_BLOCK)
                rc_be_stmts(be, &n->else_br->stmts);
            break;
        case NODE_EXPR_STMT: rc_be_expr(be, n->expr); break;
        case NODE_BLOCK: rc_be_stmts(be, &n->stmts); break;
        default: break;
    }
}

static void rc_be_stmts(RcBe *be, NodeVec *stmts) {
    for (int i = 0; i < stmts->len && !be->bad; i++)
        rc_be_stmt(be, stmts->data[i]);
}

/* допустимо ли е elision за `let x = <borrowed от src>` — (а) опашката на
 * текущия block след let-а (escape/мутация в scope-а), (б) цялата функция
 * (алиас/складиране на src — мутация през тях би била невидима) */
static int rc_be_ok(Codegen *cg, const char *x, const char *src) {
    if (!cg->rc || !cg->rc_cur_blk || cg->rc_cur_idx < 0) return 0;
    if (strcmp(x, src) == 0) return 0;
    /* (б) глобален scan: src не бива да излиза от binding-а си никъде във fn */
    if (cg->rc_cur_fn) {
        RcBe whole = { "", src, 0 };
        rc_be_stmts(&whole, &cg->rc_cur_fn->stmts);
        if (whole.bad) return 0;
    } else {
        return 0;
    }
    /* (а) опашка на scope-а: x не escape-ва, src не се мутира */
    RcBe be = { x, src, 0 };
    NodeVec *stmts = &cg->rc_cur_blk->stmts;
    for (int i = cg->rc_cur_idx + 1; i < stmts->len && !be.bad; i++)
        rc_be_stmt(&be, stmts->data[i]);
    return !be.bad;
}

/* ---- RC4 (temporaries tracking): per-statement temp регистър ----
 * Дизайн: docs/memory-rc-bg.md §v0.2. Fresh heap резултат (call с heap тип, * който не е borrowed — vec_get/map_get/поле/h_* — плюс NODE_TO_STR), който
 * не се връзва в локал, е temp: собствената му rc=1 референция иначе тече.
 * Temp-овете се събират pre-order от root израза на statement-а, изчисляват
 * се в __rc_tmpN декларации преди него (вътрешните първи) и се release-ват
 * в края му. Не се слиза в: struct литерал (полетата escape-ват без retain),
 * ламбда (отделна fn), try/catch (ранен return), match, if-израз и десния
 * операнд на &&/|| (условна оценка), drop(…) аргументи (самият drop е
 * release пътят). Root-ът на let/assign/return е bound (собствеността се
 * предава) — само вложените му temp-ове се събират.
 * v0.3: условия на if/while и for-range hi се wrap-ват в GNU ({…}) —
 * temp-овете се оценяват на всяка оценка на условието и се release-ват
 * веднага след нея (преди тялото / клоновете). continue/break не ги пипат. */

static void rc_tmp_release_all(Codegen *cg);
static void emit_expr(Codegen *cg, Node *n); /* fwd — пълната fwd декларация е по-долу */
static void emit_rc_stmt_expr(Codegen *cg, Node *n);

/* RC5 v0.4: цел `ident.field`, където ident е track-нат struct локал (tag 5,
 * не param/dead), а field е пряко heap поле. Връща tag-а на полето (1-4) или
 * 0; за Vec/Map полета пълни kind (elem_kind/val_tag), sz и rel. Само плоско
 * ident.field — по-дълбоки пътеки биха се оценили два пъти. */
static int rc_field_assign_tag(Codegen *cg, Node *target, int *kind,
                               char *sz, size_t szn, char *rel, size_t reln) {
    *kind = 0;
    snprintf(sz, szn, "0");
    snprintf(rel, reln, "0");
    if (!cg->rc || !target || target->kind != NODE_FIELD) return 0;
    Node *obj = target->field_obj;
    if (!obj || obj->kind != NODE_IDENT || !target->field_name) return 0;
    int idx = rc_find(cg, obj->name);
    if (idx < 0) return 0;
    RcLocal *e = &cg->rc_locals.data[idx];
    if (e->tag != 5 || e->is_param || e->dead) return 0;
    const char *sn = (e->type && e->type->name) ? e->type->name :
        (e->type_node && e->type_node->type_name) ? e->type_node->type_name : NULL;
    if (!sn) return 0;
    Node *d = find_struct_decl(cg, sn);
    if (!d) return 0;
    for (int i = 0; i < d->fields.len; i++) {
        Node *fld = d->fields.data[i];
        if (!fld->fld_name ||
            strcmp(fld->fld_name, target->field_name) != 0)
            continue;
        int tag = rc_type_node_tag(fld->fld_type);
        if (tag == 3) {
            *kind = rc_vec_elem_kind_node(fld->fld_type, sz, szn);
            Node *et = fld->fld_type->inner_type;
            rc_box_rel(cg, (et && et->kind == NODE_TYPE) ? et->type_name : NULL,
                       rel, reln);
            /* RC5 v0.9: вложен Vec<S> — destructor за S полетата */
            if (*kind == 3) rc_nested_vec_rel_node(cg, fld->fld_type, rel, reln);
        } else if (tag == 4) {
            *kind = rc_map_val_tag_node(fld->fld_type, sz, szn);
            Node *vt = fld->fld_type->inner_type2;
            rc_box_rel(cg, (vt && vt->kind == NODE_TYPE) ? vt->type_name : NULL,
                       rel, reln);
        }
        return tag;
    }
    return 0;
}

/* release на старото поле (в alias-safe ред: след retain на новото) */
static void rc_emit_field_release(Codegen *cg, int tag, Node *target, int kind,
                                  const char *sz, const char *rel) {
    FILE *f = cg->out;
    switch (tag) {
        case 1: fprintf(f, "baga_rc_release_str("); break;
        case 2: fprintf(f, "baga_rc_release_bytes("); break;
        case 3: fprintf(f, "baga_rc_release_vec("); break;
        case 4: fprintf(f, "baga_rc_release_map("); break;
        default: return;
    }
    emit_expr(cg, target);
    if (tag == 3 || tag == 4)
        fprintf(f, ", %d, %s, %s", kind, sz, rel);
    fprintf(f, "); ");
}

/* RC5 v0.8: цел `ident.field`, където ident е track-нат struct локал (tag 5,
 * не param/dead), а field е struct-типизирано поле с heap полета (транзитивно,
 * v0.5). Връща името на struct типа на полето или NULL. Същата плоска граница
 * като v0.4 — по-дълбоки пътеки (`a.b.c = x`) биха оценили целта два пъти. */
static const char *rc_field_assign_struct(Codegen *cg, Node *target) {
    if (!cg->rc || !target || target->kind != NODE_FIELD) return NULL;
    Node *obj = target->field_obj;
    if (!obj || obj->kind != NODE_IDENT || !target->field_name) return NULL;
    int idx = rc_find(cg, obj->name);
    if (idx < 0) return NULL;
    RcLocal *e = &cg->rc_locals.data[idx];
    if (e->tag != 5 || e->is_param || e->dead) return NULL;
    const char *sn = (e->type && e->type->name) ? e->type->name :
        (e->type_node && e->type_node->type_name) ? e->type_node->type_name : NULL;
    if (!sn) return NULL;
    Node *d = find_struct_decl(cg, sn);
    if (!d) return NULL;
    for (int i = 0; i < d->fields.len; i++) {
        Node *fld = d->fields.data[i];
        if (!fld->fld_name ||
            strcmp(fld->fld_name, target->field_name) != 0)
            continue;
        return rc_nested_struct_field(cg, fld->fld_type);
    }
    return NULL;
}

/* release на старото struct-типизирано поле (в alias-safe ред: след retain
 * на новото) — рекурсивен release_<T> от v0.5 */
static void rc_emit_struct_field_release(Codegen *cg, const char *stname,
                                         Node *target) {
    char *sm = mangle_name(stname);
    fprintf(cg->out, "baga_rc_release_%s(", sm);
    emit_expr(cg, target);
    fprintf(cg->out, "); ");
    free(sm);
}

/* RC5 v0.10: цел `ident.field`, където ident е track-нат struct локал (tag 5,
 * не param/dead), а field е enum-типизирано поле с heap payload. Връща името
 * на enum типа или NULL. Същата плоска граница като v0.4/v0.8. */
static const char *rc_field_assign_enum(Codegen *cg, Node *target) {
    if (!cg->rc || !target || target->kind != NODE_FIELD) return NULL;
    Node *obj = target->field_obj;
    if (!obj || obj->kind != NODE_IDENT || !target->field_name) return NULL;
    int idx = rc_find(cg, obj->name);
    if (idx < 0) return NULL;
    RcLocal *e = &cg->rc_locals.data[idx];
    if (e->tag != 5 || e->is_param || e->dead) return NULL;
    const char *sn = (e->type && e->type->name) ? e->type->name :
        (e->type_node && e->type_node->type_name) ? e->type_node->type_name : NULL;
    if (!sn) return NULL;
    Node *d = find_struct_decl(cg, sn);
    if (!d) return NULL;
    for (int i = 0; i < d->fields.len; i++) {
        Node *fld = d->fields.data[i];
        if (!fld->fld_name ||
            strcmp(fld->fld_name, target->field_name) != 0)
            continue;
        return rc_nested_enum_field(cg, fld->fld_type);
    }
    return NULL;
}

/* RC5 v0.10: release на старото enum-типизирано поле (в alias-safe ред:
 * след retain на новото) — release_E по runtime tag от v0.6 */
static void rc_emit_enum_field_release(Codegen *cg, const char *enname,
                                       Node *target) {
    char *em = mangle_name(enname);
    fprintf(cg->out, "baga_rc_release_%s(", em);
    emit_expr(cg, target);
    fprintf(cg->out, "); ");
    free(em);
}

static int rc_is_enum_ctor(Codegen *cg, Node *n);

/* fresh heap temp ли е този възел? (owned rc=1 резултат по конвенция)
 * RC5 v1.0b: rc_heap_tag — struct/enum fn резултатът е owned (v1.0a). */
static int rc_tmp_fresh(Codegen *cg, Node *n) {
    if (!n) return 0;
    if (n->kind == NODE_TO_STR) return 1;
    if (n->kind != NODE_CALL) return 0;
    if (rc_borrowed_init(n)) return 0;
    /* enum ctor е като литерал — payload-ът е owned от ctor сайта;
     * не се регистрира като temp (иначе push(Some(x)) би го пуснал) */
    if (rc_is_enum_ctor(cg, n)) return 0;
    return rc_heap_tag(cg, n->type) != 0;
}

/* конструктор на sum enum с payload ли е това повикване? (огледално на
 * emit_call L3/A1 клона) — payload-ът се копира по стойност БЕЗ retain
 * (като struct литерал), temp в аргумента би бил освободен под краката му */
static int rc_is_enum_ctor(Codegen *cg, Node *n) {
    if (!cg->program || !n->callee) return 0;
    if (n->callee->kind != NODE_IDENT && n->callee->kind != NODE_PATH)
        return 0;
    for (int i = 0; i < cg->program->items.len; i++) {
        Node *item = cg->program->items.data[i];
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

/* RC5 v1.0b: не-borrowed call с heap резултат — owned по v1.0a конвенция
 * (не struct lit / не enum ctor; тези се познават отделно). */
static int rc_is_owned_call(Codegen *cg, Node *n) {
    if (!n || n->kind != NODE_CALL) return 0;
    if (rc_borrowed_init(n)) return 0;
    if (rc_is_enum_ctor(cg, n)) return 0;
    return rc_heap_tag(cg, n->type) != 0;
}

static void rc_tmp_collect(Codegen *cg, Node *n, int is_root) {
    if (!n) return;
    if (rc_tmp_fresh(cg, n) && !is_root) {
        RcTmp t;
        t.site = n;
        t.type = n->type;
        t.tag = n->kind == NODE_TO_STR ? 1 : rc_heap_tag(cg, n->type);
        snprintf(t.name, sizeof t.name, "__rc_tmp%d", cg->tmp_counter++);
        vec_push(cg->rc_tmps, t);
        /* продължаваме надолу — аргументите може да съдържат temp-ове */
    }
    switch (n->kind) {
        case NODE_BINARY:
            rc_tmp_collect(cg, n->left, 0);
            /* &&/||: десният операнд се оценява условно — не се пипа */
            if (n->bin_op != OP_AND && n->bin_op != OP_OR)
                rc_tmp_collect(cg, n->right, 0);
            break;
        case NODE_UNARY:
            rc_tmp_collect(cg, n->operand, 0);
            break;
        case NODE_CALL:
            /* drop(x) е release пътят на x — аргументът не е temp;
             * enum конструктор копира payload без retain (като struct
             * литерал) — temp в него би обесил payload-а */
            if (n->callee && n->callee->kind == NODE_IDENT &&
                strcmp(n->callee->name, "drop") == 0)
                break;
            if (rc_is_enum_ctor(cg, n)) break;
            rc_tmp_collect(cg, n->callee, 0);
            for (int i = 0; i < n->args.len; i++)
                rc_tmp_collect(cg, n->args.data[i], 0);
            break;
        case NODE_INDEX:
            rc_tmp_collect(cg, n->obj, 0);
            rc_tmp_collect(cg, n->index, 0);
            break;
        case NODE_ELEM_REF:
            rc_tmp_collect(cg, n->elem_obj, 0);
            break;
        case NODE_FIELD:
            rc_tmp_collect(cg, n->field_obj, 0);
            break;
        case NODE_RANGE:
            rc_tmp_collect(cg, n->range_lo, 0);
            rc_tmp_collect(cg, n->range_hi, 0);
            break;
        case NODE_TO_STR:
            rc_tmp_collect(cg, n->to_str_expr, 0);
            break;
        case NODE_MATCH: {
            /* RC5 v0.11: scrutinee temp (`match f() { ... }`). Scrutinee-то
             * се оценява безусловно и точно веднъж, ПРЕДИ рамената, а temp
             * release-ът идва в края на statement-а — СЛЕД телата на
             * рамената, така че borrowed binding-ите (v0.6 конвенция)
             * остават валидни. В рамената не се слиза (условни statement-и).
             * Enum ctor scrutinee (`match Ok(concat(...))`) притежава
             * payload референциите си от ctor сайта (v0.6 пр. 4) и никой не
             * ги release-ва след match-а — регистрира се като temp с tag 6.
             * RC5 v1.0b: fn резултат (`match mk()`) вече е owned (v1.0a) и
             * се регистрира през rc_tmp_fresh / rc_heap_tag — release след
             * рамената. */
            Node *sc = n->match_expr;
            if (sc && sc->kind == NODE_CALL && rc_is_enum_ctor(cg, sc) &&
                rc_heap_tag(cg, sc->type) == 6) {
                RcTmp t;
                t.site = sc;
                t.type = sc->type;
                t.tag = 6;
                snprintf(t.name, sizeof t.name, "__rc_tmp%d", cg->tmp_counter++);
                vec_push(cg->rc_tmps, t);
            } else {
                rc_tmp_collect(cg, sc, 0);
            }
            break;
        }
        /* STRUCT_LIT, LAMBDA, TRY, CATCH, IF и останалите — не се
         * слиза (escape/отделна fn/условна оценка — вж. коментара по-горе) */
        default:
            break;
    }
}

/* заместване в emit_expr: temp възелът вече е изчислен в __rc_tmpN.
 * Връща 1 и emit-ва името, ако n е регистриран temp. */
static int rc_tmp_emit_sub(Codegen *cg, Node *n) {
    if (!cg->rc || !cg->rc_tmps_on || n == cg->rc_tmp_decl) return 0;
    for (int i = 0; i < cg->rc_tmps.len; i++)
        if (cg->rc_tmps.data[i].site == n) {
            fprintf(cg->out, "%s", cg->rc_tmps.data[i].name);
            return 1;
        }
    return 0;
}

/* RC5 v0.7: индекс на регистриран temp за този AST възел (-1 = не е temp).
 * Ползва се от box push/set сайтовете за move на temp аргумент. */
static int rc_tmp_find(Codegen *cg, Node *n) {
    if (!cg->rc || !cg->rc_tmps_on || !n) return -1;
    for (int i = 0; i < cg->rc_tmps.len; i++)
        if (cg->rc_tmps.data[i].site == n) return i;
    return -1;
}

/* RC5 v0.7: temp-ът е прехвърлен (move) в контейнер — краят на statement-а
 * не го release-ва. Маркира се СЛЕД emission на аргумента (иначе
 * rc_tmp_emit_sub губи сайта и би преизчислил извикването inline). */
static void rc_tmp_consume(Codegen *cg, int i) {
    if (i >= 0) cg->rc_tmps.data[i].site = NULL;
}

/* начало на statement с temp tracking. root_bound=1: root-ът е bound
 * (let init / assign дясно / return стойност) — не е temp. За assign
 * statement се вика с целия NODE_ASSIGN — дясното е bound, целта се чете.
 * Пази/нулира активния регистър (вложени statement-и в if-изрази/ламбди). */
static void rc_tmp_begin(Codegen *cg, Node *root, int root_bound,
                         RcTmpVec *saved, int *saved_on) {
    *saved = cg->rc_tmps;
    *saved_on = cg->rc_tmps_on;
    cg->rc_tmps.data = NULL; cg->rc_tmps.len = 0; cg->rc_tmps.cap = 0;
    cg->rc_tmps_on = 0;
    if (!cg->rc || !root) return;
    if (root->kind == NODE_ASSIGN) {
        /* дясното е bound (или escape в поле — пак не се release-ва от нас);
         * сложните цели четат обекта/индекса — там temp-ове са валидни */
        rc_tmp_collect(cg, root->assign_val, 1);
        Node *t = root->assign_target;
        if (t) {
            if (t->kind == NODE_FIELD) rc_tmp_collect(cg, t->field_obj, 0);
            else if (t->kind == NODE_INDEX) {
                rc_tmp_collect(cg, t->obj, 0);
                rc_tmp_collect(cg, t->index, 0);
            }
        }
    } else {
        rc_tmp_collect(cg, root, root_bound);
    }
    if (cg->rc_tmps.len == 0) return;
    cg->rc_tmps_on = 1;
    FILE *f = cg->out;
    /* декларации в обратен ред на събирането — вътрешните temp-ове първи */
    for (int i = cg->rc_tmps.len - 1; i >= 0; i--) {
        emit_indent(cg);
        fprintf(f, "__auto_type %s = ", cg->rc_tmps.data[i].name);
        cg->rc_tmp_decl = cg->rc_tmps.data[i].site;
        emit_expr(cg, cg->rc_tmps.data[i].site);
        cg->rc_tmp_decl = NULL;
        fprintf(f, ";\n");
    }
}

/* release на активните temp-ове (в реда на събирането = обратен на
 * декларациите) и изчистване; вика се след statement-а (или вътре в
 * return блока — там return-ът е преди края на statement-а) */
static void rc_tmp_release_all(Codegen *cg) {
    if (!cg->rc || !cg->rc_tmps_on) return;
    for (int i = 0; i < cg->rc_tmps.len; i++) {
        RcTmp *t = &cg->rc_tmps.data[i];
        /* RC5 v0.7: прехвърлен в контейнер (move) — собствеността е на box-а */
        if (!t->site) continue;
        RcLocal e = {0};
        e.name = (char *)t->name;
        e.tag = t->tag;
        e.type = t->type;
        rc_emit_release(cg, &e);
        cg->rc_tmp_count++;
    }
    cg->rc_tmps.len = 0;
    cg->rc_tmps_on = 0;
}

/* край на statement-а: release (ако не е направен) + възстановяване */
static void rc_tmp_end(Codegen *cg, RcTmpVec *saved, int saved_on) {
    rc_tmp_release_all(cg);
    vec_free(cg->rc_tmps);
    cg->rc_tmps = *saved;
    cg->rc_tmps_on = saved_on;
}

/* peek: cond/expr има ли fresh heap temp-ове? Не пипа броячите. */
static int rc_tmp_would_collect(Codegen *cg, Node *n) {
    if (!cg->rc || !n) return 0;
    RcTmpVec saved = cg->rc_tmps;
    int saved_on = cg->rc_tmps_on;
    int saved_tc = cg->tmp_counter;
    cg->rc_tmps.data = NULL; cg->rc_tmps.len = 0; cg->rc_tmps.cap = 0;
    rc_tmp_collect(cg, n, 0);
    int ntmp = cg->rc_tmps.len;
    vec_free(cg->rc_tmps);
    cg->rc_tmps = saved;
    cg->rc_tmps_on = saved_on;
    cg->tmp_counter = saved_tc;
    return ntmp;
}

/* RC4 v0.3: израз в условие. Ако има temp-ове — GNU statement-expression
 * `({ decls; __auto_type __rc_cN = expr; releases; __rc_cN; })`, така че
 * оценката е на всяко влизане (while/for hi) и release-ът е преди тялото.
 * Без temp-ове — обикновен emit_expr. */
static void emit_rc_stmt_expr(Codegen *cg, Node *n) {
    if (!n) { fprintf(cg->out, "0"); return; }
    if (!cg->rc) { emit_expr(cg, n); return; }
    RcTmpVec saved = cg->rc_tmps;
    int saved_on = cg->rc_tmps_on;
    cg->rc_tmps.data = NULL; cg->rc_tmps.len = 0; cg->rc_tmps.cap = 0;
    cg->rc_tmps_on = 0;
    rc_tmp_collect(cg, n, 0);
    if (cg->rc_tmps.len == 0) {
        vec_free(cg->rc_tmps);
        cg->rc_tmps = saved;
        cg->rc_tmps_on = saved_on;
        emit_expr(cg, n);
        return;
    }
    cg->rc_tmps_on = 1;
    FILE *f = cg->out;
    fprintf(f, "({\n");
    cg->indent++;
    for (int i = cg->rc_tmps.len - 1; i >= 0; i--) {
        emit_indent(cg);
        fprintf(f, "__auto_type %s = ", cg->rc_tmps.data[i].name);
        cg->rc_tmp_decl = cg->rc_tmps.data[i].site;
        emit_expr(cg, cg->rc_tmps.data[i].site);
        cg->rc_tmp_decl = NULL;
        fprintf(f, ";\n");
    }
    emit_indent(cg);
    int cnum = cg->tmp_counter++;
    fprintf(f, "__auto_type __rc_c%d = ", cnum);
    emit_expr(cg, n);
    fprintf(f, ";\n");
    rc_tmp_release_all(cg);
    emit_indent(cg);
    fprintf(f, "__rc_c%d;\n", cnum);
    cg->indent--;
    emit_indent(cg);
    fprintf(f, "})");
    vec_free(cg->rc_tmps);
    cg->rc_tmps = saved;
    cg->rc_tmps_on = saved_on;
}

/* RC5 v1.0a: трябва ли return/arm стойност да се retain-не, за да е
 * резултатът owned? Свеж литерал/ctor и не-borrowed call (callee вече
 * връща owned по същата конвенция) — не. Match се обработва в рамената.
 * Всичко друго с heap tag (vec_get/поле, ident, if-израз) — да
 * (при съмнение leak-safe). */
static int rc_need_owned_retain(Codegen *cg, Node *val) {
    if (!cg->rc || !val) return 0;
    if (!rc_heap_tag(cg, val->type)) return 0;
    if (val->kind == NODE_STRUCT_LIT) return 0;
    if (val->kind == NODE_CALL && rc_is_enum_ctor(cg, val)) return 0;
    if (val->kind == NODE_CALL && !rc_borrowed_init(val)) return 0;
    if (val->kind == NODE_MATCH) return 0;
    return 1;
}

/* RC1.2: стойност на match arm (когато match-ът произвежда heap стойност) —
 * borrowed израз (vec_get/map_get/поле/h_*) или ident (match binding е
 * копие-алиас на payload; track-нат локал е втори собственик) се retain-ва,
 * за да е резултатът owned по конвенцията „fn резултат = owned". Иначе
 * scope release на източника обесва/underflow-ва консуматора (латентен
 * пропуск, маскиран от temp течовете преди RC4 — pg_err/sqlstate).
 * RC5 v1.0a: rc_heap_tag вместо rc_type_tag — struct/enum рамена също
 * (tag 5/6 през retain_S/retain_E; void* cast върху стойност е невалиден). */
static void rc_emit_match_arm_val(Codegen *cg, Node *rv, int tmp,
                                  int is_void) {
    FILE *f = cg->out;
    if (is_void) { emit_expr(cg, rv); return; }
    int rtag = (cg->rc && rv) ? rc_heap_tag(cg, rv->type) : 0;
    if (!rtag || !rc_need_owned_retain(cg, rv)) {
        fprintf(f, "_mr%d = ", tmp);
        emit_expr(cg, rv);
        return;
    }
    fprintf(f, "({ __auto_type __rc_m = ");
    emit_expr(cg, rv);
    fprintf(f, "; ");
    rc_emit_retain_val(cg, rtag, rv->type, NULL, "__rc_m");
    fprintf(f, "_mr%d = __rc_m; })", tmp);
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
static void emit_return_val(Codegen *cg, Node *val);
static void emit_zero_struct(Codegen *cg, const char *name);

/* M20 (effect payloads) — помощници */

/* Връща 1 ако типът (с ефекти) носи поне един payload ефект. */
static int type_has_payload_effects(Type *t) {
    if (!t || !t->effect_payloads) return 0;
    for (int i = 0; i < t->n_effects; i++)
        if (t->effect_payloads[i]) return 1;
    return 0;
}

/* Node-вариант: ret_type веригата (NODE_TYPE_EFFECT обвивки). */
static int node_has_payload_effects(Node *t) {
    while (t && t->kind == NODE_TYPE_EFFECT) {
        if (t->effect_payloads) {
            for (int i = 0; i < t->n_effects; i++)
                if (t->effect_payloads[i]) return 1;
        }
        t = t->inner_type;
    }
    return 0;
}

/* Tag на ефект по име — детерминистичен (първа поява = 1, 2, …). */
static int eff_tag(Codegen *cg, const char *name) {
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
                                   at->name && rc_struct_has_heap(cg, at->name)) {
                            char *sm = mangle_name(at->name);
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
                        char *mn = mangle_name(vt->elem->name);
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
                        char *mn = mangle_name(mt->elem->name);
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
                        char *mn = mangle_name(mt->elem->name);
                        fprintf(f, "baga_map_del_%s_rc(", ksuf);
                        emit_expr(cg, n->args.data[0]);
                        fprintf(f, ", ");
                        emit_expr(cg, n->args.data[1]);
                        fprintf(f, ", (int64_t)sizeof(%s), baga_rc_relf_%s)",
                                mn, mn);
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
                    {"mem_mark",    "baga_mem_mark"},
                    {"mem_rewind",  "baga_mem_rewind"},
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
            emit_rc_stmt_expr(cg, n->cond);
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
                    /* RC5: s = f(s) — резултатът алиасира полетата; не пускай */
                    int thru = e.tag == 5 &&
                        rc_expr_copies_ident(n->assign_val, n->assign_target->name);
                    if (!thru) switch (e.tag) {
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
                                char *sm = mangle_name(sn);
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
                                char *em = mangle_name(en);
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
            char *sm = mangle_name(n->lit_name);
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
                            /* RC5 v0.5: borrowed вложен struct — retain_<T> */
                            const char *vn = val->type && val->type->name ?
                                val->type->name : NULL;
                            if (vn) {
                                char *vm = mangle_name(vn);
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
                /* без payload ефекти — старото поведение (compile-time) */
                Node *base = n;
                while (base && base->kind == NODE_CATCH) base = base->catch_expr;
                emit_expr(cg, base);
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
                emit_expr(cg, base);
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
                        if (scan->catch_handler->kind == NODE_BLOCK) {
                            /* блок-хендлър: statement-и + стойността на
                             * последния (като implicit return) */
                            Node *hb = scan->catch_handler;
                            fprintf(f, "({ ");
                            for (int si = 0; si < hb->stmts.len; si++) {
                                if (si == hb->stmts.len - 1) {
                                    /* последният stmt е СТОЙНОСТТА на handler-а
                                     * (implicit return семантика) */
                                    Node *ls = hb->stmts.data[si];
                                    if (ls->kind == NODE_EXPR_STMT) {
                                        emit_expr(cg, ls->expr);
                                        fprintf(f, "; ");
                                    } else if (ls->kind == NODE_RETURN && ls->ret_val) {
                                        emit_expr(cg, ls->ret_val);
                                        fprintf(f, "; ");
                                    } else {
                                        emit_stmt(cg, ls);
                                    }
                                } else {
                                    emit_stmt(cg, hb->stmts.data[si]);
                                }
                            }
                            fprintf(f, " })");
                        } else {
                            emit_expr(cg, scan->catch_handler);
                        }
                        fprintf(f, "; } ");
                        cg->eff_binding = saved_b;
                        cg->eff_binding_c = saved_c;
                        free(bm);
                    } else {
                        fprintf(f, "__e%d = ", tid);
                        if (scan->catch_handler->kind == NODE_BLOCK) {
                            Node *hb = scan->catch_handler;
                            fprintf(f, "({ ");
                            for (int si = 0; si < hb->stmts.len; si++) {
                                if (si == hb->stmts.len - 1) {
                                    /* последният stmt е СТОЙНОСТТА на handler-а
                                     * (implicit return семантика) */
                                    Node *ls = hb->stmts.data[si];
                                    if (ls->kind == NODE_EXPR_STMT) {
                                        emit_expr(cg, ls->expr);
                                        fprintf(f, "; ");
                                    } else if (ls->kind == NODE_RETURN && ls->ret_val) {
                                        emit_expr(cg, ls->ret_val);
                                        fprintf(f, "; ");
                                    } else {
                                        emit_stmt(cg, ls);
                                    }
                                } else {
                                    emit_stmt(cg, hb->stmts.data[si]);
                                }
                            }
                            fprintf(f, " })");
                        } else {
                            emit_expr(cg, scan->catch_handler);
                        }
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
                emit_expr(cg, n->try_expr);
                fprintf(f, "; if (baga_eff_tl.tag) { ");
                emit_eff_return_zero(cg);
                fprintf(f, "; } __t%d; })", cg->tmp_counter++);
            } else {
                emit_expr(cg, n->try_expr);
            }
            break;

        case NODE_RAISE: {
            /* M20: raise !E(payload) — задава слота и дивергира */
            FILE *f = cg->out;
            int tag = eff_tag(cg, n->raise_effect);
            fprintf(f, "(baga_eff_tl.tag = %d", tag);
            if (n->raise_payload && n->raise_payload->type) {
                fprintf(f, ", baga_eff_tl.%s = (",
                        eff_slot_field(n->raise_payload->type->kind));
                emit_expr(cg, n->raise_payload);
                fprintf(f, ")");
            }
            /* нулева стойност за обграждащия израз (мъртва — върнали сме) */
            fprintf(f, ", ");
            emit_zero_val(cg, n->type);
            fprintf(f, ")");
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
                                rc_emit_match_arm_val(cg, s->ret_val, tmp,
                                                      is_void);
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

/* RC1: return със scope release. move при `return x` (локален), retain при
 * `return p` (параметър), release на всички при `return <израз>` — изразът
 * се оценява ПРЕДИ release-ите, защото може да ползва локалите. */
static void emit_return_val(Codegen *cg, Node *val) {
    FILE *f = cg->out;
    if (!cg->rc) {
        emit_indent(cg);
        fprintf(f, "return ");
        emit_expr(cg, val);
        fprintf(f, ";\n");
        return;
    }
    emit_indent(cg);
    if (val->kind == NODE_IDENT) {
        int idx = rc_find(cg, val->name);
        if (idx >= 0 && !cg->rc_locals.data[idx].is_param &&
            !cg->rc_locals.data[idx].dead) {
            /* move: bindingът излиза като собственост на caller-а */
            rc_release_all(cg, idx);
            emit_indent(cg);
            fprintf(f, "return ");
            emit_expr(cg, val);
            fprintf(f, ";\n");
            return;
        }
        if (idx >= 0 && cg->rc_locals.data[idx].is_param &&
            !cg->rc_locals.data[idx].dead) {
            /* заеманият параметър става собственост на caller-а */
            char *mm = mangle_name(val->name);
            rc_emit_retain_val(cg, cg->rc_locals.data[idx].tag,
                               cg->rc_locals.data[idx].type,
                               cg->rc_locals.data[idx].type_node, mm);
            fprintf(f, "\n");
            free(mm);
            emit_indent(cg);
            rc_release_all(cg, -1);
            emit_indent(cg);
            fprintf(f, "return ");
            emit_expr(cg, val);
            fprintf(f, ";\n");
            return;
        }
        if (idx >= 0 || !rc_need_owned_retain(cg, val)) {
            /* dead локал / ident без heap: emit_expr пази lowering-а
             * (sum enum без payload и т.н.) */
            rc_release_all(cg, -1);
            emit_indent(cg);
            fprintf(f, "return ");
            emit_expr(cg, val);
            fprintf(f, ";\n");
            return;
        }
        /* untrack-нат ident с heap (match binding / let = vec_get, или
         * bare variant като `return GBad`) — през __rc_ret, защото
         * emit_expr може да свали варианта до `(E){ .tag = N }` и
         * няма C локал с това име */
    }
    fprintf(f, "{\n");
    cg->indent++;
    emit_indent(cg);
    fprintf(f, "__auto_type __rc_ret = ");
    emit_expr(cg, val);
    fprintf(f, ";\n");
    /* borrowed / if-израз / untracked: caller-ът получава owned
     * (конвенцията „fn резултат = owned") → retain. RC5 v1.0a:
     * rc_heap_tag покрива и struct/enum (tag 5/6), не само str/bytes/Vec.
     * Свеж литерал/ctor и не-borrowed call не се пипат — вече owned. */
    if (rc_need_owned_retain(cg, val)) {
        int rtag = rc_heap_tag(cg, val->type);
        emit_indent(cg);
        rc_emit_retain_val(cg, rtag, val->type, NULL, "__rc_ret");
        fprintf(f, "\n");
    }
    /* RC4: release на temp-овете от return израза — тук, преди самия return
     * (statement-ът приключва с return; rc_tmp_end след това е no-op) */
    rc_tmp_release_all(cg);
    rc_release_all(cg, -1);
    emit_indent(cg);
    fprintf(f, "return __rc_ret;\n");
    cg->indent--;
    emit_indent(cg);
    fprintf(f, "}\n");
}

static void emit_block_scoped(Codegen *cg, Node *block, int is_loop) {
    FILE *f = cg->out;
    fprintf(f, "{\n");
    cg->indent++;
    rc_push_scope(cg, is_loop);
    for (int i = 0; i < block->stmts.len; i++) {
        /* RC2.1: контекст на текущия statement за borrowed-pair анализа
         * (сканира се опашката на този block след let-а) */
        Node *saved_blk = cg->rc_cur_blk;
        int saved_idx = cg->rc_cur_idx;
        cg->rc_cur_blk = block; cg->rc_cur_idx = i;
        emit_stmt(cg, block->stmts.data[i]);
        cg->rc_cur_blk = saved_blk; cg->rc_cur_idx = saved_idx;
    }
    /* RC1: release на heap локалите на scope-а преди затварящата скоба;
     * без --rc rc_pop_scope не emit-ва нищо */
    rc_pop_scope(cg);
    cg->indent--;
    emit_indent(cg);
    fprintf(f, "}");
}

static void emit_block(Codegen *cg, Node *block) {
    emit_block_scoped(cg, block, 0);
}

static void emit_stmt(Codegen *cg, Node *n) {
    FILE *f = cg->out;
    if (!n) return;

    switch (n->kind) {
        case NODE_LET: {
            /* RC4: temp-ове в init (root-ът е bound — не е temp) */
            RcTmpVec saved_tmps = {0};
            int saved_on = 0;
            if (cg->rc)
                rc_tmp_begin(cg, n->let_init, 1, &saved_tmps, &saved_on);
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
            /* RC1: регистрирай heap локала в scope стека; retain при alias —
             * от track-нат ident (двама собственици) или от borrowed израз
             * (vec_get/map_get/поле/h_* — референцията става собствена). */
            if (cg->rc && n->let_name) {
                Type *lt = n->let_init ? n->let_init->type : NULL;
                int tag = rc_heap_tag(cg, lt);
                if (tag == 0 && n->let_type)
                    tag = rc_heap_tag_node(cg, n->let_type);
                if (tag == 5) {
                    int fresh = n->let_init && n->let_init->kind == NODE_STRUCT_LIT;
                    int from_tr = 0;
                    if (n->let_init && n->let_init->kind == NODE_IDENT) {
                        int si = rc_find(cg, n->let_init->name);
                        if (si >= 0 && !cg->rc_locals.data[si].dead)
                            from_tr = 1;
                    }
                    /* fn резултат не се регистрира: pgwire MEM-4 споделя
                     * AST vec/bytes между sess/portal/stmt — release на
                     * owned let би бил двоен free. Leak-safe. */
                    if (!fresh && !from_tr) tag = 0;
                } else if (tag == 6) {
                    /* RC5 v0.6: само свеж ctor (`Ok(x)`) или alias на track-нат */
                    int fresh = rc_is_enum_ctor(cg, n->let_init);
                    int from_tr = 0;
                    if (n->let_init && n->let_init->kind == NODE_IDENT) {
                        int si = rc_find(cg, n->let_init->name);
                        if (si >= 0 && !cg->rc_locals.data[si].dead)
                            from_tr = 1;
                    }
                    if (!fresh && !from_tr) tag = 0;
                }
                if (tag) {
                    int elide = 0;
                    char *mself = mangle_name(n->let_name);
                    if (n->let_init && n->let_init->kind == NODE_IDENT) {
                        int si = rc_find(cg, n->let_init->name);
                        if (si >= 0 && !cg->rc_locals.data[si].dead) {
                            /* RC2: последна употреба на източника → move
                             * (без retain; източникът умира тук) */
                            if (rc_is_move(cg, n->let_init, si)) {
                                rc_do_move(cg, si);
                            } else {
                                emit_indent(cg);
                                rc_emit_retain_val(cg, cg->rc_locals.data[si].tag,
                                                   cg->rc_locals.data[si].type,
                                                   cg->rc_locals.data[si].type_node,
                                                   cg->rc_locals.data[si].name);
                                fprintf(f, "\n");
                            }
                        }
                    } else if (rc_borrowed_init(n->let_init)) {
                        /* RC5: borrowed struct (vec_get/поле) споделя полета —
                         * трябва retain_S, иначе scope release обесва
                         * контейнера (boila_txn_commit underflow).
                         * v0.6: същото за enum payload (tag 6). */
                        if (tag == 5 || tag == 6) {
                            emit_indent(cg);
                            rc_emit_retain_val(cg, tag, lt, n->let_type, mself);
                            fprintf(f, "\n");
                        } else {
                        /* RC2.1: ако x не escape-ва и източникът не се мутира
                         * в scope-а — нито retain, нито регистрация (двойката
                         * retain+release е чиста загуба) */
                        const char *srcn = rc_borrow_src_name(n->let_init);
                        if (srcn && rc_be_ok(cg, n->let_name, srcn)) {
                            cg->rc_elided_pairs++;
                            elide = 1;
                        } else {
                            emit_indent(cg);
                            if (tag == 2)
                                fprintf(f, "baga_rc_retain((void *)%s.data);\n", mself);
                            else
                                fprintf(f, "baga_rc_retain((void *)%s);\n", mself);
                        }
                        }
                    }
                    free(mself);
                    if (!elide)
                        rc_register_node(cg, n->let_name, tag, lt, n->let_type, 0);
                }
            }
            if (cg->rc) rc_tmp_end(cg, &saved_tmps, saved_on);
            /* RC1.3: `let m = mem_mark()` — watermark за rewind dead-marking */
            if (cg->rc && rc_is_mem_call(n->let_init, "mem_mark"))
                vec_push(cg->rc_marks, cg->rc_locals.len);
            break;
        }

        case NODE_RETURN: {
            /* RC4: temp-ове в return израза (root-ът отива на caller-а);
             * release-ът им е ВЪТРЕ в emit_return_val — преди return-а */
            RcTmpVec saved_tmps = {0};
            int saved_on = 0;
            if (cg->rc) {
                if (n->ret_val) {
                    rc_tmp_begin(cg, n->ret_val, 1, &saved_tmps, &saved_on);
                    emit_return_val(cg, n->ret_val);
                    rc_tmp_end(cg, &saved_tmps, saved_on);
                } else {
                    rc_release_all(cg, -1);
                    emit_indent(cg);
                    fprintf(f, "return;\n");
                }
                break;
            }
            emit_indent(cg);
            if (n->ret_val) {
                fprintf(f, "return ");
                emit_expr(cg, n->ret_val);
                fprintf(f, ";\n");
            } else {
                fprintf(f, "return;\n");
            }
            break;
        }

        case NODE_WHILE:
            emit_indent(cg);
            fprintf(f, "while (");
            emit_rc_stmt_expr(cg, n->while_cond);
            fprintf(f, ") ");
            /* RC1: loop тялото е маркиран scope — release на всяка итерация,
             * break/continue release-ват до тук */
            emit_block_scoped(cg, n->while_body, 1);
            fprintf(f, "\n");
            break;

        case NODE_FOR: {
            /* for x in lo..hi { } → for (int64_t x = lo; x < hi; x++)
             * RC4 v0.3: lo temp-ове се hoist-ват веднъж (init); hi — на
             * всяка итерация през emit_rc_stmt_expr (C for cond се преоценява). */
            Node *lo = NULL, *hi = NULL;
            if (n->for_iter && n->for_iter->kind == NODE_RANGE) {
                lo = n->for_iter->range_lo;
                hi = n->for_iter->range_hi;
            }
            RcTmpVec saved_lo = {0};
            int saved_lo_on = 0;
            int wrap_lo = cg->rc && lo && rc_tmp_would_collect(cg, lo);
            if (wrap_lo) {
                emit_indent(cg);
                fprintf(f, "{\n");
                cg->indent++;
                rc_tmp_begin(cg, lo, 0, &saved_lo, &saved_lo_on);
            }
            emit_indent(cg);
            char *m = mangle_name(n->for_var);
            fprintf(f, "for (int64_t %s = ", m);
            if (lo) {
                emit_expr(cg, lo);
                fprintf(f, "; %s < ", m);
                emit_rc_stmt_expr(cg, hi);
                fprintf(f, "; %s++) ", m);
            } else {
                fprintf(f, "0; %s < 0; %s++) ", m, m);
            }
            emit_block_scoped(cg, n->for_body, 1);
            fprintf(f, "\n");
            if (wrap_lo) {
                rc_tmp_end(cg, &saved_lo, saved_lo_on);
                cg->indent--;
                emit_indent(cg);
                fprintf(f, "}\n");
            }
            free(m);
            break;
        }

        case NODE_IF:
            emit_indent(cg);
            fprintf(f, "if (");
            emit_rc_stmt_expr(cg, n->cond);
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

        case NODE_EXPR_STMT: {
            /* RC4: temp-ове в израза (root на bare call с heap резултат е
             * дискарднат — пак е temp; assign дясното е bound) */
            RcTmpVec saved_tmps = {0};
            int saved_on = 0;
            if (cg->rc)
                rc_tmp_begin(cg, n->expr, 0, &saved_tmps, &saved_on);
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
            if (cg->rc) rc_tmp_end(cg, &saved_tmps, saved_on);
            /* RC1.3: `mem_rewind(m)` — локалите над watermark-а на m държат
             * върната памет; scope release-ът им би чел overwrite-нат header
             * (bump reuse) → маркират се dead (leak-safe посока) */
            if (cg->rc && rc_is_mem_call(n->expr, "mem_rewind") &&
                cg->rc_marks.len > 0) {
                int wm = cg->rc_marks.data[--cg->rc_marks.len];
                if (wm > cg->rc_locals.len) wm = cg->rc_locals.len;
                for (int mi = wm; mi < cg->rc_locals.len; mi++)
                    cg->rc_locals.data[mi].dead = 1;
            }
            break;
        }

        case NODE_BREAK:
            /* RC1: release на scopes до най-близкото loop тяло преди скока */
            if (cg->rc) rc_release_to_loop(cg);
            emit_indent(cg);
            fprintf(f, "break;\n");
            break;

        case NODE_CONTINUE:
            if (cg->rc) rc_release_to_loop(cg);
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
        /* M20: ret type node на текущата fn — за ZERO на propagate */
        Node *saved_eff_ret = cg->eff_cur_ret;
        cg->eff_cur_ret = fn->ret_type;
        /* RC2: last-use pre-pass за move elision (преди emission на тялото) */
        if (cg->rc) {
            cg->rc_lus.len = 0;
            cg->rc_marks.len = 0;  /* RC1.3: mark watermark-овете са per-fn */
            rc_lu_stmts(cg, &fn->fn_body->stmts, NULL, 0);
        }
        /* RC2.1: тялото на текущата fn за глобалния alias scan */
        cg->rc_cur_fn = fn->fn_body;
        /* RC1: fn scope — параметрите са заемани (is_param=1): регистрират
         * се за retain при `let x = p` / `return p`, но не се release-ват */
        rc_push_scope(cg, 0);
        int saved_fn_base = cg->rc_fn_base;
        if (cg->rc) cg->rc_fn_base = cg->rc_scopes.len - 1;
        if (cg->rc) {
            for (int i = 0; i < fn->params.len; i++) {
                Node *p = fn->params.data[i];
                rc_register_node(cg, p->param_name,
                            rc_heap_tag_node(cg, p->param_type),
                            p->type, p->param_type, 1);
            }
        }
        NodeVec *stmts = &fn->fn_body->stmts;
        for (int i = 0; i < stmts->len; i++) {
            Node *s = stmts->data[i];
            /* RC2.1: контекст за borrowed-pair анализа (fn body = scope) */
            Node *saved_blk = cg->rc_cur_blk;
            int saved_idx = cg->rc_cur_idx;
            cg->rc_cur_blk = fn->fn_body; cg->rc_cur_idx = i;
            /* implicit return: last expr stmt in non-void fn */
            if (has_ret && i == stmts->len - 1 && s->kind == NODE_EXPR_STMT) {
                /* RC4: temp-ове в implicit return израза — същият път като
                 * explicit return (release преди return-а) */
                RcTmpVec saved_tmps = {0};
                int saved_on = 0;
                if (cg->rc) {
                    rc_tmp_begin(cg, s->expr, 1, &saved_tmps, &saved_on);
                    emit_return_val(cg, s->expr);
                    rc_tmp_end(cg, &saved_tmps, saved_on);
                } else {
                    emit_return_val(cg, s->expr);
                }
            } else {
                emit_stmt(cg, s);
            }
            cg->rc_cur_blk = saved_blk; cg->rc_cur_idx = saved_idx;
        }
        rc_pop_scope(cg);
        cg->rc_fn_base = saved_fn_base;
        cg->indent--;
        emit_indent(cg);
        fprintf(f, "}");
        cg->eff_cur_ret = saved_eff_ret;
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

/* ---- struct / sum-enum emission (topo-ordered) ---- */

static int is_sum_enum_item(Node *item) {
    if (!item || item->kind != NODE_ENUM) return 0;
    for (int j = 0; j < item->n_variants; j++)
        if (item->enum_payloads && item->enum_payloads[j]) return 1;
    return 0;
}

/* Named user type from a type AST node (struct or sum enum), or NULL for
 * primitives / Vec / Map / unknown. Unwraps !effects and &T. */
static const char *user_type_name(Node *ty) {
    while (ty) {
        if (ty->kind == NODE_TYPE_EFFECT || ty->kind == NODE_TYPE_REF) {
            ty = ty->inner_type;
            continue;
        }
        if (ty->kind != NODE_TYPE || !ty->type_name) return NULL;
        const char *n = ty->type_name;
        if (strcmp(n, "i64") == 0 || strcmp(n, "i32") == 0 ||
            strcmp(n, "f64") == 0 || strcmp(n, "bool") == 0 ||
            strcmp(n, "str") == 0 || strcmp(n, "bytes") == 0 ||
            strcmp(n, "void") == 0 || strcmp(n, "Vec") == 0 ||
            strcmp(n, "Map") == 0 || strcmp(n, "fn") == 0)
            return NULL;
        return n;
    }
    return NULL;
}

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

/* RC5: retain/release на преките heap полета. След typedef-а. */
static void emit_rc_struct_helpers(Codegen *cg, Node *s) {
    if (!cg->rc || !rc_struct_has_heap(cg, s->struct_name)) return;
    FILE *f = cg->out;
    char *m = mangle_name(s->struct_name);
    fprintf(f, "static inline void baga_rc_retain_%s(%s s) {\n", m, m);
    for (int i = 0; i < s->fields.len; i++) {
        Node *fld = s->fields.data[i];
        int tag = rc_type_node_tag(fld->fld_type);
        char *fm = mangle_name(fld->fld_name);
        if (!tag) {
            /* RC5 v0.5: вложено struct поле — рекурсивен retain */
            const char *nn = rc_nested_struct_field(cg, fld->fld_type);
            if (nn) {
                char *nm = mangle_name(nn);
                fprintf(f, "    baga_rc_retain_%s(s.%s);\n", nm, fm);
                free(nm);
            } else {
                /* RC5 v0.10: enum поле — retain_E по runtime tag */
                const char *en = rc_nested_enum_field(cg, fld->fld_type);
                if (en) {
                    char *nm = mangle_name(en);
                    fprintf(f, "    baga_rc_retain_%s(s.%s);\n", nm, fm);
                    free(nm);
                }
            }
            free(fm);
            continue;
        }
        if (tag == 2)
            fprintf(f, "    baga_rc_retain((void *)s.%s.data);\n", fm);
        else
            fprintf(f, "    baga_rc_retain((void *)s.%s);\n", fm);
        free(fm);
    }
    fprintf(f, "}\n");
    fprintf(f, "static inline void baga_rc_release_%s(%s s) {\n", m, m);
    for (int i = 0; i < s->fields.len; i++) {
        Node *fld = s->fields.data[i];
        int tag = rc_type_node_tag(fld->fld_type);
        char *fm = mangle_name(fld->fld_name);
        if (!tag) {
            /* RC5 v0.5: вложено struct поле — рекурсивен release */
            const char *nn = rc_nested_struct_field(cg, fld->fld_type);
            if (nn) {
                char *nm = mangle_name(nn);
                fprintf(f, "    baga_rc_release_%s(s.%s);\n", nm, fm);
                free(nm);
            } else {
                /* RC5 v0.10: enum поле — release_E по runtime tag */
                const char *en = rc_nested_enum_field(cg, fld->fld_type);
                if (en) {
                    char *nm = mangle_name(en);
                    fprintf(f, "    baga_rc_release_%s(s.%s);\n", nm, fm);
                    free(nm);
                }
            }
            free(fm);
            continue;
        }
        if (tag == 1)
            fprintf(f, "    baga_rc_release_str(s.%s);\n", fm);
        else if (tag == 2)
            fprintf(f, "    baga_rc_release_bytes(s.%s);\n", fm);
        else if (tag == 3) {
            char sz[160], rel[160];
            int k = rc_vec_elem_kind_node(fld->fld_type, sz, sizeof sz);
            Node *et = fld->fld_type->inner_type;
            rc_box_rel(cg, (et && et->kind == NODE_TYPE) ? et->type_name : NULL,
                       rel, sizeof rel);
            /* RC5 v0.9: вложен Vec<S> — destructor за S полетата */
            if (k == 3) rc_nested_vec_rel_node(cg, fld->fld_type, rel, sizeof rel);
            fprintf(f, "    baga_rc_release_vec(s.%s, %d, %s, %s);\n", fm, k, sz, rel);
        } else if (tag == 4) {
            char sz[160], rel[160];
            int k = rc_map_val_tag_node(fld->fld_type, sz, sizeof sz);
            Node *vt = fld->fld_type->inner_type2;
            rc_box_rel(cg, (vt && vt->kind == NODE_TYPE) ? vt->type_name : NULL,
                       rel, sizeof rel);
            fprintf(f, "    baga_rc_release_map(s.%s, %d, %s, %s);\n", fm, k, sz, rel);
        }
        free(fm);
    }
    fprintf(f, "}\n");
    /* RC5 v0.2: shim-ове за box елементи във Vec/Map — release/retain на
     * полетата през указател (container drop/slice/concat не знаят типа
     * статично). */
    fprintf(f, "static void baga_rc_relf_%s(void *p) { baga_rc_release_%s(*(%s *)p); }\n",
            m, m, m);
    fprintf(f, "static void baga_rc_retp_%s(void *p) { baga_rc_retain_%s(*(%s *)p); }\n\n",
            m, m, m);
    /* RC5 v0.9: shim за Vec<S> като елемент на външен Vec — release на box
     * елементите на вътрешния vec през relf (kind 3 на външния не знае
     * елементния тип на вложения). */
    fprintf(f, "static void baga_rc_relv_%s(void *p) { baga_rc_release_vec((baga_Vec *)p, 2, (int64_t)sizeof(%s), baga_rc_relf_%s); }\n",
            m, m, m);
    free(m);
}

/* RC5 v0.6: retain/release по runtime tag за enum с heap payload. */
static void emit_rc_enum_helpers(Codegen *cg, Node *item) {
    if (!cg->rc || !rc_enum_has_heap(cg, item)) return;
    FILE *f = cg->out;
    char *em = mangle_name(item->enum_name);
    for (int pass = 0; pass < 2; pass++) {
        fprintf(f, "static inline void baga_rc_%s_%s(%s e) {\n    switch (e.tag) {\n",
                pass ? "release" : "retain", em, em);
        for (int j = 0; j < item->n_variants; j++) {
            if (!item->enum_payloads || !item->enum_payloads[j]) continue;
            Node *pt = item->enum_payloads[j];
            int tag = rc_type_node_tag(pt);
            const char *nn = tag ? NULL : rc_nested_struct_field(cg, pt);
            if (!tag && !nn) continue;
            char *vm = mangle_name(item->enum_variants[j]);
            if (!pass) {
                if (nn) {
                    char *nm = mangle_name(nn);
                    fprintf(f, "        case %d: baga_rc_retain_%s(e.u.v_%s); break;\n",
                            j, nm, vm);
                    free(nm);
                } else if (tag == 2)
                    fprintf(f, "        case %d: baga_rc_retain((void *)e.u.v_%s.data); break;\n",
                            j, vm);
                else
                    fprintf(f, "        case %d: baga_rc_retain((void *)e.u.v_%s); break;\n",
                            j, vm);
            } else {
                if (nn) {
                    char *nm = mangle_name(nn);
                    fprintf(f, "        case %d: baga_rc_release_%s(e.u.v_%s); break;\n",
                            j, nm, vm);
                    free(nm);
                } else if (tag == 1)
                    fprintf(f, "        case %d: baga_rc_release_str(e.u.v_%s); break;\n",
                            j, vm);
                else if (tag == 2)
                    fprintf(f, "        case %d: baga_rc_release_bytes(e.u.v_%s); break;\n",
                            j, vm);
                else if (tag == 3) {
                    char sz[160], rel[160];
                    int k = rc_vec_elem_kind_node(pt, sz, sizeof sz);
                    Node *et = pt->inner_type;
                    rc_box_rel(cg, (et && et->kind == NODE_TYPE) ?
                               et->type_name : NULL, rel, sizeof rel);
                    /* RC5 v0.9: вложен Vec<S> — destructor за S полетата */
                    if (k == 3) rc_nested_vec_rel_node(cg, pt, rel, sizeof rel);
                    fprintf(f, "        case %d: baga_rc_release_vec(e.u.v_%s, %d, %s, %s); break;\n",
                            j, vm, k, sz, rel);
                } else if (tag == 4) {
                    char sz[160], rel[160];
                    int k = rc_map_val_tag_node(pt, sz, sizeof sz);
                    Node *vt = pt->inner_type2;
                    rc_box_rel(cg, (vt && vt->kind == NODE_TYPE) ?
                               vt->type_name : NULL, rel, sizeof rel);
                    fprintf(f, "        case %d: baga_rc_release_map(e.u.v_%s, %d, %s, %s); break;\n",
                            j, vm, k, sz, rel);
                }
            }
            free(vm);
        }
        fprintf(f, "    }\n}\n");
    }
    /* RC5 v0.10: shim-ове за box елементи/стойности във Vec/Map — release/
     * retain на payload-а през указател (container drop/set/del/slice не
     * знаят типа статично). Огледало на struct relf/retp от v0.2. */
    fprintf(f, "static void baga_rc_relf_%s(void *p) { baga_rc_release_%s(*(%s *)p); }\n",
            em, em, em);
    fprintf(f, "static void baga_rc_retp_%s(void *p) { baga_rc_retain_%s(*(%s *)p); }\n",
            em, em, em);
    fprintf(f, "\n");
    free(em);
}

/* L3 sum enum → tagged C struct + union + static inline constructors.
 * Tag is int64_t to mirror the LLVM lowering ({ i64 tag, [N x i64] u });
 * constructors zero-init so the union never carries stack garbage
 * (LP1 probe: gcc -Wstringop-overread on payload reads). */
static void emit_sum_enum(Codegen *cg, Node *item) {
    FILE *out = cg->out;
    char *em = mangle_name(item->enum_name);
    fprintf(out, "typedef struct {\n    int64_t tag;\n    union {\n");
    for (int j = 0; j < item->n_variants; j++) {
        if (!item->enum_payloads || !item->enum_payloads[j]) continue;
        char *vm = mangle_name(item->enum_variants[j]);
        fprintf(out, "        ");
        emit_type(cg, item->enum_payloads[j]);
        fprintf(out, " v_%s;\n", vm);
        free(vm);
    }
    fprintf(out, "    } u;\n} %s;\n\n", em);
    for (int j = 0; j < item->n_variants; j++) {
        char *vm = mangle_name(item->enum_variants[j]);
        if (item->enum_payloads && item->enum_payloads[j]) {
            fprintf(out, "static inline %s %s__%s(", em, em, vm);
            emit_type(cg, item->enum_payloads[j]);
            fprintf(out, " a0) {\n    %s r = {0}; r.tag = %d; r.u.v_%s = a0; return r;\n}\n\n",
                    em, j, vm);
        } else {
            fprintf(out, "static inline %s %s__%s(void) {\n    %s r = {0}; r.tag = %d; return r;\n}\n\n",
                    em, em, vm, em, j);
        }
        free(vm);
    }
    free(em);
}

/* Emit structs and sum enums in dependency order so sum-enum fields work
 * (Wrap { r: Res }) and struct payloads still work (Circle(Point)). */
static void emit_structs_and_sum_enums(Codegen *cg, Node *program) {
    int n = 0;
    for (int i = 0; i < program->items.len; i++) {
        Node *it = program->items.data[i];
        if (it->kind == NODE_STRUCT || is_sum_enum_item(it)) n++;
    }
    if (n == 0) return;

    Node **nodes = (Node **)calloc((size_t)n, sizeof(Node *));
    int *indeg = (int *)calloc((size_t)n, sizeof(int));
    /* adj[j] is list of indices that depend on j (edges j → k) */
    int **adj = (int **)calloc((size_t)n, sizeof(int *));
    int *adj_len = (int *)calloc((size_t)n, sizeof(int));
    int *adj_cap = (int *)calloc((size_t)n, sizeof(int));
    if (!nodes || !indeg || !adj || !adj_len || !adj_cap) {
        fprintf(stderr, "baga: out of memory\n");
        exit(1);
    }

    int k = 0;
    for (int i = 0; i < program->items.len; i++) {
        Node *it = program->items.data[i];
        if (it->kind == NODE_STRUCT || is_sum_enum_item(it))
            nodes[k++] = it;
    }

    /* name → index */
    for (int a = 0; a < n; a++) {
        /* collect dependency names of a */
        const char *deps[64];
        int nd = 0;
        if (nodes[a]->kind == NODE_STRUCT) {
            for (int f = 0; f < nodes[a]->fields.len && nd < 64; f++) {
                const char *dn = user_type_name(nodes[a]->fields.data[f]->fld_type);
                if (dn) deps[nd++] = dn;
            }
        } else {
            for (int v = 0; v < nodes[a]->n_variants && nd < 64; v++) {
                if (!nodes[a]->enum_payloads || !nodes[a]->enum_payloads[v]) continue;
                const char *dn = user_type_name(nodes[a]->enum_payloads[v]);
                if (dn) deps[nd++] = dn;
            }
        }
        for (int d = 0; d < nd; d++) {
            int b = -1;
            for (int j = 0; j < n; j++) {
                const char *bn = nodes[j]->kind == NODE_STRUCT
                    ? nodes[j]->struct_name : nodes[j]->enum_name;
                if (bn && strcmp(bn, deps[d]) == 0) { b = j; break; }
            }
            if (b < 0 || b == a) continue; /* primitive / other / self */
            /* edge b → a (b must be emitted before a) */
            if (adj_len[b] + 1 > adj_cap[b]) {
                int nc = adj_cap[b] ? adj_cap[b] * 2 : 4;
                int *na = (int *)realloc(adj[b], (size_t)nc * sizeof(int));
                if (!na) { fprintf(stderr, "baga: out of memory\n"); exit(1); }
                adj[b] = na;
                adj_cap[b] = nc;
            }
            adj[b][adj_len[b]++] = a;
            indeg[a]++;
        }
    }

    int *queue = (int *)malloc((size_t)n * sizeof(int));
    int *order = (int *)malloc((size_t)n * sizeof(int));
    int qh = 0, qt = 0, on = 0;
    if (!queue || !order) { fprintf(stderr, "baga: out of memory\n"); exit(1); }
    for (int i = 0; i < n; i++)
        if (indeg[i] == 0) queue[qt++] = i;
    while (qh < qt) {
        int u = queue[qh++];
        order[on++] = u;
        for (int e = 0; e < adj_len[u]; e++) {
            int v = adj[u][e];
            indeg[v]--;
            if (indeg[v] == 0) queue[qt++] = v;
        }
    }
    /* cycle or leftover: append remaining in declaration order */
    if (on < n) {
        for (int i = 0; i < n; i++) {
            int seen = 0;
            for (int j = 0; j < on; j++) if (order[j] == i) { seen = 1; break; }
            if (!seen) order[on++] = i;
        }
    }

    for (int i = 0; i < on; i++) {
        Node *it = nodes[order[i]];
        if (it->kind == NODE_STRUCT)
            emit_struct(cg, it);
        else
            emit_sum_enum(cg, it);
    }
    if (cg->rc) {
        /* RC5 v0.2: forward decls на box shims — release_S на по-ранен
         * struct реферира relf на по-късен (Vec<S>/Map<K,S> поле).
         * v0.5: и retain/release — вложеното struct поле рекурсира
         * напред-назад по декларационния ред.
         * v0.6: enum helper-и (struct payload в enum реферира retain_S). */
        for (int i = 0; i < on; i++) {
            Node *it = nodes[order[i]];
            if (it->kind == NODE_STRUCT &&
                rc_struct_has_heap(cg, it->struct_name)) {
                char *m = mangle_name(it->struct_name);
                fprintf(cg->out, "static inline void baga_rc_retain_%s(%s s);\n", m, m);
                fprintf(cg->out, "static inline void baga_rc_release_%s(%s s);\n", m, m);
                fprintf(cg->out, "static void baga_rc_relf_%s(void *p);\n", m);
                fprintf(cg->out, "static void baga_rc_retp_%s(void *p);\n", m);
                /* RC5 v0.9: и nested vec shim-ът (Vec<Vec<S>> поле на
                 * по-ранен struct реферира relv на по-късен) */
                fprintf(cg->out, "static void baga_rc_relv_%s(void *p);\n", m);
                free(m);
            } else if (it->kind == NODE_ENUM && rc_enum_has_heap(cg, it)) {
                char *m = mangle_name(it->enum_name);
                fprintf(cg->out, "static inline void baga_rc_retain_%s(%s e);\n", m, m);
                fprintf(cg->out, "static inline void baga_rc_release_%s(%s e);\n", m, m);
                /* RC5 v0.10: и box shim-овете (struct с Vec<E>/Map<K,E> поле
                 * реферира relf/retp на по-късен enum) */
                fprintf(cg->out, "static void baga_rc_relf_%s(void *p);\n", m);
                fprintf(cg->out, "static void baga_rc_retp_%s(void *p);\n", m);
                free(m);
            }
        }
        for (int i = 0; i < on; i++) {
            Node *it = nodes[order[i]];
            if (it->kind == NODE_STRUCT)
                emit_rc_struct_helpers(cg, it);
            else if (it->kind == NODE_ENUM)
                emit_rc_enum_helpers(cg, it);
        }
    }

    for (int i = 0; i < n; i++) free(adj[i]);
    free(adj); free(adj_len); free(adj_cap);
    free(nodes); free(indeg); free(queue); free(order);
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

static Node *find_sum_enum_decl(Codegen *cg, const char *name) {
    if (!cg->program || !name) return NULL;
    for (int i = 0; i < cg->program->items.len; i++) {
        Node *it = cg->program->items.data[i];
        if (is_sum_enum_item(it) && it->enum_name &&
            strcmp(it->enum_name, name) == 0)
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
                /* sum enum field → zeroed tagged union; else nested struct */
                if (find_sum_enum_decl(cg, ft->type_name)) {
                    char *em = mangle_name(ft->type_name);
                    fprintf(f, "(%s){0}", em);
                    free(em);
                } else {
                    emit_zero_struct(cg, ft->type_name);
                }
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
    /* RC1: scope стековете се нулират тук (Codegen идва неинициализиран
     * от main.c; rc флагът се задава от main.c преди извикването) */
    cg->rc_locals.data = NULL; cg->rc_locals.len = 0; cg->rc_locals.cap = 0;
    cg->rc_scopes.data = NULL; cg->rc_scopes.len = 0; cg->rc_scopes.cap = 0;
    cg->rc_fn_base = -1;
    cg->rc_lus.data = NULL; cg->rc_lus.len = 0; cg->rc_lus.cap = 0;
    cg->rc_moves = 0;
    cg->rc_cmoves = 0;
    cg->rc_tmps.data = NULL; cg->rc_tmps.len = 0; cg->rc_tmps.cap = 0;
    cg->rc_tmps_on = 0;
    cg->rc_tmp_count = 0;
    cg->rc_tmp_decl = NULL;
    cg->rc_marks.data = NULL; cg->rc_marks.len = 0; cg->rc_marks.cap = 0;
    cg->rc_cur_blk = NULL; cg->rc_cur_idx = -1;
    cg->rc_cur_fn = NULL;
    cg->rc_elided_pairs = 0;
    /* M20: effect tag регистър (детерминистичен pre-pass по-долу) */
    cg->eff_tags.data = NULL; cg->eff_tags.len = 0; cg->eff_tags.cap = 0;
    cg->eff_depth = 0;
    cg->eff_cur_ret = NULL;
    cg->eff_binding = NULL;
    cg->eff_binding_c = NULL;
    /* M20 pre-pass: assign tags на всички payload ефекти в сигнатурите —
     * преди какъвто и да е emit (таг-овете трябва да са стабилни) */
    for (int i = 0; i < program->items.len; i++) {
        Node *item = program->items.data[i];
        if (item->kind != NODE_FN || !item->ret_type) continue;
        Node *t = item->ret_type;
        while (t && t->kind == NODE_TYPE_EFFECT) {
            if (t->effect_payloads) {
                for (int j = 0; j < t->n_effects; j++)
                    if (t->effect_payloads[j])
                        eff_tag(cg, t->effect_names[j]);
            }
            t = t->inner_type;
        }
    }
    /* RC1: размер на per-alloc header-а — 24 B с rc поле, 16 B без */
    int hs = cg->rc ? 32 : 16;

    /* header */
    fprintf(out, "/* Генериран от компилатора на Бага. Фаза 1. */\n");
    fprintf(out, "#include <stdio.h>\n");
    fprintf(out, "#include <stdlib.h>\n");
    fprintf(out, "#include <stdint.h>\n");
    fprintf(out, "#include <string.h>\n");
    fprintf(out, "#include <errno.h>\n");
    fprintf(out, "#include <time.h>\n");
    fprintf(out, "#include <signal.h>\n");
    fprintf(out, "#include <pthread.h>\n");
    fprintf(out, "#include <math.h>\n\n");

    /* arena — всички низови/векторни алокации минават тук (без individual free).
     * R52: arena + free lists са THREAD-LOCAL (__thread). Преди това един
     * глобален mutex (baga_alloc_mu) сериализираше ВСЯКА алокация във всички
     * нишки — реален GIL при MT serve (go_bg per conn + workers). Безопасно,
     * защото str е arena-bound (не се free-ва), а free-list блоковете са
     * сменяема сырова памет — cross-thread free попълва списъка на free-ващата
     * нишка. */
    fprintf(out, "typedef struct baga_ABlk { struct baga_ABlk *next; size_t used, cap; char data[]; } baga_ABlk;\n");
    fprintf(out, "static __thread baga_ABlk *baga_arena_head = NULL;\n");
    /* MEM-1: free list — 16-байтови класове ≤ 1024 B; R18: + pow2 класове
     * 2 KiB..32 MiB — drop рециклира блокове. Големите класове се bump-ват с
     * ПЪЛНИЯ класов размер, за да е консистентен всеки бъдещ free в класа. */
    fprintf(out, "#define BAGA_FL_CLASSES 64   /* free list: 16-байтови класове ≤ 1024 B */\n");
    fprintf(out, "#define BAGA_FL_BIG 12       /* pow2 класове: 2 KiB .. 32 MiB */\n");
    fprintf(out, "static __thread void *baga_fl[BAGA_FL_CLASSES];\n");
    fprintf(out, "static __thread void *baga_fl_big[BAGA_FL_BIG];\n");
    /* MEM-4 (boilaDB Q2): per-request rewind + persist регион.
     * mem_mark/mem_rewind: mark пази (head блок, used); rewind освобождава
     * всички блокове над mark-а и връща used — паметта на заявката се
     * връща към malloc. Free-list записи, сочещи във върнатата памет, се
     * изчистват (иначе двойна употреба). LIFO нарушение/двоен rewind →
     * чиста грешка, не UB.
     * mem_persist_begin/end: докато depth > 0, baga_alloc bump-ва в
     * ОТДЕЛНА верига, която rewind не пипа. Persist и ephemeral имат
     * РАЗДЕЛНИ free lists — иначе drop() на persist страница я слага в
     * ephemeral freelist и следващ ephemeral alloc презаписва live
     * shared state (page cache / memtable) → catalog „таблица липсва".
     * За shared state, който преживява заявката (plan cache, session maps). */
    fprintf(out, "static __thread baga_ABlk *baga_persist_head = NULL;\n");
    fprintf(out, "static __thread int64_t baga_persist_depth = 0;\n");
    fprintf(out, "static __thread void *baga_fl_p[BAGA_FL_CLASSES];\n");
    fprintf(out, "static __thread void *baga_fl_big_p[BAGA_FL_BIG];\n");
    fprintf(out, "static void baga_mem_persist_begin(void) { baga_persist_depth++; }\n");
    fprintf(out, "static void baga_mem_persist_end(void) {\n");
    fprintf(out, "    if (baga_persist_depth <= 0) { fprintf(stderr, \"baga: mem_persist_end без mem_persist_begin\\n\"); exit(1); }\n");
    fprintf(out, "    baga_persist_depth--;\n");
    fprintf(out, "}\n");
    fprintf(out, "static int baga_fl_big_idx(int64_t n) {\n");
    fprintf(out, "    int i = 0; int64_t c = 2048;\n");
    fprintf(out, "    while (i < BAGA_FL_BIG) { if (n <= c) return i; c <<= 1; i++; }\n");
    fprintf(out, "    return -1;\n");
    fprintf(out, "}\n");
    /* MEM-4b: per-alloc header (16 B: magic + persist флаг) → O(1) регион
     * детекция в baga_free. Предишният baga_ptr_in_persist обхождаше веригата
     * persist блокове на ВСЯКО free — O(блокове) на free → квадратичен колапс
     * при compaction/rewind върху голям persist регион (измерено: 1M insert
     * bench спираше да пише — CPU в baga_drop_map/baga_ptr_in_persist). */
    fprintf(out, "#define BAGA_HDR_MAGIC 0xBA6A4D454D344848ULL\n");
    /* RC1: под --rc header-ът получава трето поле rc (24 B); без --rc
     * layout-ът и offset-ите са непроменени (16 B) */
    if (cg->rc) {
        /* RC1-perf: persist битът и epoch са пакетирани в едно поле pe
         * (pe = epoch<<1 | persist) — 32 B header (payload пак 16-подравнен),
         * един запис по-малко на алокация, едно зареждане в rc_hdr. */
        fprintf(out, "typedef struct { uint64_t magic; uint64_t pe; uint64_t rc; uint64_t an; } baga_Hdr;\n");
        /* RC1: обхват на arena блоковете — range guard преди magic check.
         * epoch: mem_rewind я бутва; release на ephemeral стойност от преди
         * rewind е no-op (паметта ѝ е върната — иначе UAF при scope exit
         * след rewind в същия scope). persist стойности са извън epoch.
         * baga_rc_spare: rewind НЕ връща блоковете на malloc, а ги пази в
         * spare веригата за следващи алокации — header-ите остават mapped
         * (epoch е достатъчна защита), а malloc/free обажданията на заявка
         * изчезват (бел. RC1-perf: dead-zone логът отпадна — is_dead беше
         * O(ndead) на всеки retain/release и беше доминиращият overhead). */
        fprintf(out, "static __thread char *baga_rc_lo = (char *)(intptr_t)-1;\n");
        fprintf(out, "static __thread char *baga_rc_hi = NULL;\n");
        fprintf(out, "static __thread uint64_t baga_rc_epoch = 0;\n");
        fprintf(out, "static __thread baga_ABlk *baga_rc_spare = NULL;\n");
    }
    else
        fprintf(out, "typedef struct { uint64_t magic; uint64_t persist; } baga_Hdr;\n");
    fprintf(out, "static void baga_free(void *p, int64_t n) {\n");
    fprintf(out, "    if (!p || n <= 0) return;\n");
    fprintf(out, "    baga_Hdr *h = (baga_Hdr *)((char *)p - %d);\n", hs);
    if (cg->rc)
        fprintf(out, "    int persist = (h->magic == BAGA_HDR_MAGIC) ? (int)(h->pe & 1) : 0;\n");
    else
        fprintf(out, "    int persist = (h->magic == BAGA_HDR_MAGIC) ? (int)h->persist : 0;\n");
    fprintf(out, "    if (n <= 1024) {\n");
    fprintf(out, "        int c = (int)((n + 15) / 16) - 1;\n");
    fprintf(out, "        void **fl = persist ? &baga_fl_p[c] : &baga_fl[c];\n");
    fprintf(out, "        *(void **)p = *fl; *fl = p;\n");
    fprintf(out, "    } else {\n");
    fprintf(out, "        int i = baga_fl_big_idx(n);\n");
    fprintf(out, "        if (i >= 0) {\n");
    fprintf(out, "            void **fl = persist ? &baga_fl_big_p[i] : &baga_fl_big[i];\n");
    fprintf(out, "            *(void **)p = *fl; *fl = p;\n");
    fprintf(out, "        }\n");
    fprintf(out, "    }\n");
    fprintf(out, "}\n");
    fprintf(out, "static void *baga_alloc(size_t n) {\n");
    fprintf(out, "    size_t rn = (n + 15) & ~(size_t)15;\n");
    fprintf(out, "    size_t an;\n");
    fprintf(out, "    int persist = baga_persist_depth > 0;\n");
    fprintf(out, "    if (n == 0) {\n");
    fprintf(out, "        an = 0;\n");
    fprintf(out, "    } else if (rn <= 1024) {\n");
    fprintf(out, "        void **fl = persist ? &baga_fl_p[rn / 16 - 1] : &baga_fl[rn / 16 - 1];\n");
    fprintf(out, "        void *fb = *fl;\n");
    /* RC1: freelist блок пази стара rc стойност (0 при release-нат) — reset */
    if (cg->rc)
        fprintf(out, "        if (fb) { *fl = *(void **)fb; { baga_Hdr *_h = (baga_Hdr *)((char *)fb - %d); _h->rc = 1; _h->pe = ((uint64_t)baga_rc_epoch << 1) | (_h->pe & 1); } return fb; }\n", hs);
    else
        fprintf(out, "        if (fb) { *fl = *(void **)fb; return fb; }\n");
    fprintf(out, "        an = rn;\n");
    fprintf(out, "    } else {\n");
    fprintf(out, "        int bi = baga_fl_big_idx((int64_t)n);\n");
    fprintf(out, "        if (bi >= 0) {\n");
    fprintf(out, "            void **fl = persist ? &baga_fl_big_p[bi] : &baga_fl_big[bi];\n");
    fprintf(out, "            void *fb = *fl;\n");
    if (cg->rc)
        fprintf(out, "            if (fb) { *fl = *(void **)fb; { baga_Hdr *_h = (baga_Hdr *)((char *)fb - %d); _h->rc = 1; _h->pe = ((uint64_t)baga_rc_epoch << 1) | (_h->pe & 1); } return fb; }\n", hs);
    else
        fprintf(out, "            if (fb) { *fl = *(void **)fb; return fb; }\n");
    fprintf(out, "            an = ((size_t)2048 << bi);\n");
    fprintf(out, "        } else {\n");
    fprintf(out, "            an = n;\n");
    fprintf(out, "        }\n");
    fprintf(out, "    }\n");
    /* MEM-1 fix: малките алокации се bump-ват с КЛАСОВИЯ размер rn (не n) —
     * иначе free-list блок от по-малка заявка обслужва по-голяма в същия
     * клас и я презаписва съседния блок (segfault при review). Същото и за
     * pow2 класовете (R18): винаги пълен класов размер. */
    fprintf(out, "    baga_ABlk **hp = persist ? &baga_persist_head : &baga_arena_head;\n");
    fprintf(out, "    baga_ABlk *b = *hp;\n");
    fprintf(out, "    if (!b || b->used + an + %d > b->cap) {\n", hs);
    fprintf(out, "        size_t cap = an + %d > 8192 ? an + %d : 8192;\n", hs, hs);
    /* RC1: първо spare веригата (блокове, върнати от rewind) — без malloc
     * на всяка заявка; cap стига само ако покрива заявката. Новите адреси
     * вече са в [lo,hi] — range guard не се обновява при spare reuse. */
    if (cg->rc) {
        fprintf(out, "        if (baga_rc_spare && baga_rc_spare->cap >= cap) {\n");
        fprintf(out, "            b = baga_rc_spare; baga_rc_spare = b->next;\n");
        fprintf(out, "            b->next = *hp; b->used = 0;\n");
        fprintf(out, "        } else {\n");
        fprintf(out, "            b = (baga_ABlk *)malloc(sizeof(baga_ABlk) + cap);\n");
        fprintf(out, "            b->next = *hp; b->used = 0; b->cap = cap;\n");
        fprintf(out, "            if ((char *)b->data < baga_rc_lo) baga_rc_lo = (char *)b->data;\n");
        fprintf(out, "            if ((char *)b->data + cap > baga_rc_hi) baga_rc_hi = (char *)b->data + cap;\n");
        fprintf(out, "        }\n");
        fprintf(out, "        *hp = b;\n");
    } else {
        fprintf(out, "        b = (baga_ABlk *)malloc(sizeof(baga_ABlk) + cap);\n");
        fprintf(out, "        b->next = *hp; b->used = 0; b->cap = cap;\n");
        fprintf(out, "        *hp = b;\n");
    }
    fprintf(out, "    }\n");
    fprintf(out, "    void *p = b->data + b->used; b->used += an + %d;\n", hs);
    /* RC1: собственикът на нова алокация е първият binding → rc=1 */
    if (cg->rc)
        fprintf(out, "    baga_Hdr *hh = (baga_Hdr *)p; hh->magic = BAGA_HDR_MAGIC; hh->pe = ((uint64_t)baga_rc_epoch << 1) | (uint64_t)persist; hh->rc = 1; hh->an = an;\n");
    else
        fprintf(out, "    baga_Hdr *hh = (baga_Hdr *)p; hh->magic = BAGA_HDR_MAGIC; hh->persist = (uint64_t)persist;\n");
    fprintf(out, "    return (char *)p + %d;\n", hs);
    fprintf(out, "}\n");
    /* RC1: retain/release ядро. magic check прави C литералите и външните
     * буфери „immortal" (no-op); release на rc==0 е underflow → чиста
     * грешка + exit(1) (двоен drop), не UB.
     * Range guard: header-ът се чете само за указатели в обхвата на arena
     * блоковете — литерал/външен буфер никога не се дереференцира (четенето
     * на p-24 пред чужд литерал може да е неподравнена страница). */
    if (cg->rc) {
        fprintf(out, "static inline __attribute__((always_inline)) baga_Hdr *baga_rc_hdr(void *p) {\n");
        fprintf(out, "    if (!p || (char *)p < baga_rc_lo + %d || (char *)p >= baga_rc_hi) return NULL;\n", hs);
        fprintf(out, "    baga_Hdr *h = (baga_Hdr *)((char *)p - %d);\n", hs);
        fprintf(out, "    if (h->magic != BAGA_HDR_MAGIC) return NULL;\n");
        fprintf(out, "    if (!(h->pe & 1) && (h->pe >> 1) != baga_rc_epoch) return NULL;\n");
        fprintf(out, "    return h;\n");
        fprintf(out, "}\n");
        fprintf(out, "static inline __attribute__((always_inline)) void baga_rc_retain(void *p) {\n");
        fprintf(out, "    baga_Hdr *h = baga_rc_hdr(p);\n");
        fprintf(out, "    if (h) h->rc++;\n");
        fprintf(out, "}\n");
        fprintf(out, "static inline __attribute__((always_inline)) void baga_rc_release_str(const char *s) {\n");
        fprintf(out, "    baga_Hdr *h = baga_rc_hdr((void *)s);\n");
        fprintf(out, "    if (!h) return;\n");
        fprintf(out, "    if (h->rc == 0) { fprintf(stderr, \"baga: rc underflow (str — двоен release)\\n\"); exit(1); }\n");
        /* RC1: free с КЛАСОВИЯ размер an (не strlen+1) — иначе блок от
         * фиксирана алокация (i64_to_str = 24 B) миграва в по-малък
         * freelist клас и класът му остава без reuse (тече bump-ът) */
        fprintf(out, "    if (--h->rc == 0) baga_free((void *)s, (int64_t)h->an);\n");
        fprintf(out, "}\n");
    }
    /* MEM-4: mark/rewind върху ephemeral веригата. persist веригата не се
     * пипа — затова shared state отива там през mem_persist_begin/end. */
    fprintf(out, "typedef struct { baga_ABlk *head; size_t used; } baga_MMark;\n");
    fprintf(out, "static int64_t baga_mem_mark(void) {\n");
    fprintf(out, "    baga_MMark *m = (baga_MMark *)malloc(sizeof(baga_MMark));\n");
    fprintf(out, "    if (!m) { fprintf(stderr, \"baga: mem_mark: няма памет\\n\"); exit(1); }\n");
    fprintf(out, "    m->head = baga_arena_head;\n");
    fprintf(out, "    m->used = baga_arena_head ? baga_arena_head->used : 0;\n");
    fprintf(out, "    return (int64_t)(intptr_t)m;\n");
    fprintf(out, "}\n");
    fprintf(out, "static void baga_mem_rewind(int64_t h) {\n");
    fprintf(out, "    baga_MMark *m = (baga_MMark *)(intptr_t)h;\n");
    fprintf(out, "    if (!m) { fprintf(stderr, \"baga: mem_rewind: невалиден mark\\n\"); exit(1); }\n");
    /* RC1: нова epoch — release на ephemeral стойности от преди rewind
     * става no-op (паметта им се връща по-долу; header-ите им са мъртви) */
    if (cg->rc)
        fprintf(out, "    baga_rc_epoch++;\n");
    fprintf(out, "    baga_ABlk *b = baga_arena_head;\n");
    fprintf(out, "    while (b && b != m->head) {\n");
    fprintf(out, "        baga_ABlk *nx = b->next;\n");
    /* RC1: в spare веригата вместо free() — паметта остава mapped (epoch
     * я прави мъртва за rc_hdr) и се reuse-ва от следващите алокации. */
    if (cg->rc)
        fprintf(out, "        b->next = baga_rc_spare; baga_rc_spare = b;\n");
    else
        fprintf(out, "        free(b);\n");
    fprintf(out, "        b = nx;\n");
    fprintf(out, "    }\n");
    fprintf(out, "    if (b != m->head) {\n");
    fprintf(out, "        fprintf(stderr, \"baga: mem_rewind: mark не е в arena веригата (двоен rewind или нарушен LIFO ред)\\n\");\n");
    fprintf(out, "        exit(1);\n");
    fprintf(out, "    }\n");
    fprintf(out, "    baga_arena_head = m->head;\n");
    fprintf(out, "    if (m->head) {\n");
    fprintf(out, "        m->head->used = m->used;\n");
    fprintf(out, "    }\n");
    /* MEM-4c: вместо per-блок freelist scrub (O(блокове × freelist записи) —
     * измерен колапс при 1M insert bench: rewind > statement работа), двете
     * ephemeral freelist таблици се ЧИСТЯТ изцяло за O(1). Записи към
     * върнатите блокове са задължително премахнати; записи към оцелелия под
     * mark-а регион също падат — загуба на recycling, не корупция (freelist
     * е чист кеш; пълни се наново от бъдещи drop-ове). persist freelist-ите
     * не се пипат — persist блокове не се връщат от rewind. */
    fprintf(out, "    memset(baga_fl, 0, sizeof(baga_fl));\n");
    fprintf(out, "    memset(baga_fl_big, 0, sizeof(baga_fl_big));\n");
    fprintf(out, "    free(m);\n");
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
    fprintf(out, "static const char *baga_f64_to_str(double x) { char *r = baga_alloc(32); snprintf(r, 32, \"%%g\", x); return r; }\n");
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
    /* M20: effect payloads — tagged error slot (thread-local). tag = 0 на
     * нормалния път; raise задава tag + payload; ?/catch четат/нулират. */
    fprintf(out, "typedef struct { int64_t tag; int64_t i; double f; const char *s; baga_bytes b; } baga_eff;\n");
    fprintf(out, "static __thread baga_eff baga_eff_tl;\n");
    /* RC1: release на bytes — rc живее върху data алокацията */
    if (cg->rc) {
        fprintf(out, "static inline __attribute__((always_inline)) void baga_rc_release_bytes(baga_bytes b) {\n");
        fprintf(out, "    baga_Hdr *h = baga_rc_hdr((void *)b.data);\n");
        fprintf(out, "    if (!h) return;\n");
        fprintf(out, "    if (h->rc == 0) { fprintf(stderr, \"baga: rc underflow (bytes — двоен release)\\n\"); exit(1); }\n");
        fprintf(out, "    if (--h->rc == 0) baga_free(b.data, (int64_t)h->an);\n");
        fprintf(out, "}\n");
    }
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
    /* R54: in-place bulk append — dst[off..off+src.len) = src. Kills the
       O(n²) concat chain when assembling pipelined replies. */
    fprintf(out, "static void baga_bytes_put(baga_bytes d, int64_t off, baga_bytes s) {\n");
    fprintf(out, "    if (off < 0 || off + s.len > d.len) return;\n");
    fprintf(out, "    memcpy(d.data + off, s.data, (size_t)s.len); }\n");
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
    /* RC1: release на Vec — rc живее върху STRUCT алокацията (data масивът е
     * притежаван 1:1 от структурата). При rc→0: release на елементите според
     * elem_kind (0 inline, 1 str, 2 struct box, 3 nested vec, 4 bytes box),
     * после free на data + struct. Nested (3) release-ва вътрешните като
     * kind 0 — елементният им тип не е известен по време на изпълнение.
     * RC5 v0.2: elem_rel е destructor на box полетата (NULL → само free).
     * RC5 v0.9: при kind 3 elem_rel е destructor на вложения Vec<S>
     * (baga_rc_relv_<S>; NULL → старото поведение, S полетата текат). */
    if (cg->rc) {
        fprintf(out, "static void baga_rc_release_vec(baga_Vec *v, int elem_kind, int64_t elem_size, void (*elem_rel)(void *)) {\n");
        fprintf(out, "    if (!v) return;\n");
        fprintf(out, "    baga_Hdr *h = baga_rc_hdr((void *)v);\n");
        fprintf(out, "    if (!h) return;\n");
        fprintf(out, "    if (h->rc == 0) { fprintf(stderr, \"baga: rc underflow (vec — двоен release)\\n\"); exit(1); }\n");
        fprintf(out, "    if (--h->rc > 0) return;\n");
        fprintf(out, "    if (elem_kind == 1)\n");
        fprintf(out, "        for (int64_t i = 0; i < v->len; i++) baga_rc_release_str((const char *)v->data[i]);\n");
        fprintf(out, "    else if (elem_kind == 2)\n");
        fprintf(out, "        for (int64_t i = 0; i < v->len; i++) { if (elem_rel) elem_rel(v->data[i]); baga_free(v->data[i], elem_size); }\n");
        fprintf(out, "    else if (elem_kind == 3)\n");
        fprintf(out, "        for (int64_t i = 0; i < v->len; i++) { if (elem_rel) elem_rel(v->data[i]); else baga_rc_release_vec((baga_Vec *)v->data[i], 0, 0, NULL); }\n");
        fprintf(out, "    else if (elem_kind == 4)\n");
        fprintf(out, "        for (int64_t i = 0; i < v->len; i++) { baga_bytes *bb = (baga_bytes *)v->data[i]; baga_rc_release_bytes(*bb); baga_free(bb, elem_size); }\n");
        fprintf(out, "    baga_free(v->data, v->cap * 8);\n");
        fprintf(out, "    baga_free(v, (int64_t)sizeof(baga_Vec));\n");
        fprintf(out, "}\n");
    }
    fprintf(out, "static void baga_vec_grow(baga_Vec *v) {\n");
    /* MEM-4д: старият data буфер се free-ва при doubling — иначе в persist
     * региона всяко vec растене оставя завинаги стълбичка от intermediate
     * буфери (измерено: ~2.7 doubling-стълбички/ред → 29 GB при 250k реда). */
    fprintf(out, "    if (v->len == v->cap) { int64_t ocap = v->cap; void **od = v->data; v->cap *= 2;\n");
    fprintf(out, "        void **nd = (void **)baga_alloc((size_t)v->cap * sizeof(void *));\n");
    fprintf(out, "        memcpy(nd, od, (size_t)v->len * sizeof(void *)); v->data = nd;\n");
    fprintf(out, "        baga_free(od, ocap * (int64_t)sizeof(void *)); }\n");
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
    /* RC1: push/set retain-ват елемента (контейнерът става собственик);
     * set release-ва стария след retain на новия (alias-safe ред) */
    if (cg->rc)
        fprintf(out, "static void baga_vec_push_str(baga_Vec *v, const char *s) { baga_rc_retain((void *)s); baga_vec_grow(v); v->data[v->len++] = (void *)s; }\n");
    else
        fprintf(out, "static void baga_vec_push_str(baga_Vec *v, const char *s) { baga_vec_grow(v); v->data[v->len++] = (void *)s; }\n");
    fprintf(out, "static const char *baga_vec_get_str(baga_Vec *v, int64_t i) {\n");
    fprintf(out, "    if (i < 0 || i >= v->len) baga_bounds_fail(\"vec_get\", i, v->len);\n");
    fprintf(out, "    return (const char *)v->data[i];\n");
    fprintf(out, "}\n");
    fprintf(out, "static void baga_vec_set_str(baga_Vec *v, int64_t i, const char *s) {\n");
    fprintf(out, "    if (i < 0 || i >= v->len) baga_bounds_fail(\"vec_set\", i, v->len);\n");
    /* RC1: retain на новия, release на стария (в този ред — alias-safe) */
    if (cg->rc) fprintf(out, "    baga_rc_retain((void *)s); baga_rc_release_str((const char *)v->data[i]);\n");
    fprintf(out, "    v->data[i] = (void *)s;\n");
    fprintf(out, "}\n");
    /* RC3: move варианти — без retain на новия; източникът е last-use локал,
     * чиято референция преминава в контейнера (codegen го маркира dead).
     * set release-ва стария елемент както обикновено. */
    if (cg->rc) {
        fprintf(out, "static void baga_vec_push_str_move(baga_Vec *v, const char *s) { baga_vec_grow(v); v->data[v->len++] = (void *)s; }\n");
        fprintf(out, "static void baga_vec_set_str_move(baga_Vec *v, int64_t i, const char *s) {\n");
        fprintf(out, "    if (i < 0 || i >= v->len) baga_bounds_fail(\"vec_set\", i, v->len);\n");
        fprintf(out, "    baga_rc_release_str((const char *)v->data[i]); v->data[i] = (void *)s; }\n");
    }
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
    if (cg->rc) fprintf(out, "    baga_rc_retain((void *)b.data);\n");
    fprintf(out, "    baga_bytes *p = baga_alloc(sizeof(baga_bytes)); *p = b;\n");
    fprintf(out, "    v->data[v->len++] = p;\n");
    fprintf(out, "}\n");
    fprintf(out, "static baga_bytes baga_vec_get_bytes(baga_Vec *v, int64_t i) {\n");
    fprintf(out, "    if (i < 0 || i >= v->len) baga_bounds_fail(\"vec_get\", i, v->len);\n");
    fprintf(out, "    return *(baga_bytes *)v->data[i];\n");
    fprintf(out, "}\n");
    fprintf(out, "static void baga_vec_set_bytes(baga_Vec *v, int64_t i, baga_bytes b) {\n");
    fprintf(out, "    if (i < 0 || i >= v->len) baga_bounds_fail(\"vec_set\", i, v->len);\n");
    if (cg->rc)
        fprintf(out, "    baga_rc_retain((void *)b.data); baga_rc_release_bytes(*(baga_bytes *)v->data[i]);\n");
    fprintf(out, "    *(baga_bytes *)v->data[i] = b;\n");
    fprintf(out, "}\n");
    /* RC3: move варианти — без retain на data (виж str по-горе) */
    if (cg->rc) {
        fprintf(out, "static void baga_vec_push_bytes_move(baga_Vec *v, baga_bytes b) {\n");
        fprintf(out, "    baga_vec_grow(v);\n");
        fprintf(out, "    baga_bytes *p = baga_alloc(sizeof(baga_bytes)); *p = b;\n");
        fprintf(out, "    v->data[v->len++] = p; }\n");
        fprintf(out, "static void baga_vec_set_bytes_move(baga_Vec *v, int64_t i, baga_bytes b) {\n");
        fprintf(out, "    if (i < 0 || i >= v->len) baga_bounds_fail(\"vec_set\", i, v->len);\n");
        fprintf(out, "    baga_rc_release_bytes(*(baga_bytes *)v->data[i]); *(baga_bytes *)v->data[i] = b; }\n");
    }
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
    /* RC5 v0.3: overwrite release-ва полетата на стария box преди memcpy
     * (call site-ът вече е retain-нал новото — alias-safe ред). */
    if (cg->rc) {
        fprintf(out, "static void baga_vec_set_box_rc(baga_Vec *v, int64_t i, const void *src, int64_t size, void (*elem_rel)(void *)) {\n");
        fprintf(out, "    if (i < 0 || i >= v->len) baga_bounds_fail(\"vec_set\", i, v->len);\n");
        fprintf(out, "    if (elem_rel) elem_rel(v->data[i]);\n");
        fprintf(out, "    memcpy(v->data[i], src, (size_t)size);\n");
        fprintf(out, "}\n");
    }
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
    /* RC5 v0.2: rc варианти — box копието споделя полетата с източника
     * (shallow), затова ги retain-ва; симетрично с release при drop на
     * контейнера. Пътеката без флаг остава бит-идентична. */
    if (cg->rc) {
        fprintf(out, "static baga_Vec *baga_vec_slice_box_rc(baga_Vec *v, int64_t a, int64_t b, int64_t size, void (*elem_retain)(void *)) {\n");
        fprintf(out, "    if (a < 0) a = 0; if (b > v->len) b = v->len; if (b < a) b = a;\n");
        fprintf(out, "    baga_Vec *r = baga_vec_new();\n");
        fprintf(out, "    for (int64_t i = a; i < b; i++) { baga_vec_push_box(r, v->data[i], size); if (elem_retain) elem_retain(v->data[i]); }\n");
        fprintf(out, "    return r; }\n");
        fprintf(out, "static baga_Vec *baga_vec_concat_box_rc(baga_Vec *v, baga_Vec *w, int64_t size, void (*elem_retain)(void *)) {\n");
        fprintf(out, "    baga_Vec *r = baga_vec_new();\n");
        fprintf(out, "    for (int64_t i = 0; i < v->len; i++) { baga_vec_push_box(r, v->data[i], size); if (elem_retain) elem_retain(v->data[i]); }\n");
        fprintf(out, "    for (int64_t i = 0; i < w->len; i++) { baga_vec_push_box(r, w->data[i], size); if (elem_retain) elem_retain(w->data[i]); }\n");
        fprintf(out, "    return r; }\n");
    }
    /* вложени вектори (Vec<Vec<T>>): елементът е baga_Vec* — съхранява се
     * като указател; drop(outer) освобождава рекурсивно вътрешните (mode 3) */
    if (cg->rc)
        fprintf(out, "static void baga_vec_push_vec(baga_Vec *v, baga_Vec *x) { baga_rc_retain((void *)x); baga_vec_grow(v); v->data[v->len++] = (void *)x; }\n");
    else
        fprintf(out, "static void baga_vec_push_vec(baga_Vec *v, baga_Vec *x) { baga_vec_grow(v); v->data[v->len++] = (void *)x; }\n");
    fprintf(out, "static baga_Vec *baga_vec_get_vec(baga_Vec *v, int64_t i) {\n");
    fprintf(out, "    if (i < 0 || i >= v->len) baga_bounds_fail(\"vec_get\", i, v->len);\n");
    fprintf(out, "    return (baga_Vec *)v->data[i];\n");
    fprintf(out, "}\n");
    fprintf(out, "static void baga_vec_set_vec(baga_Vec *v, int64_t i, baga_Vec *x) {\n");
    fprintf(out, "    if (i < 0 || i >= v->len) baga_bounds_fail(\"vec_set\", i, v->len);\n");
    if (cg->rc)
        fprintf(out, "    baga_rc_retain((void *)x); baga_rc_release_vec((baga_Vec *)v->data[i], 0, 0, NULL);\n");
    fprintf(out, "    v->data[i] = (void *)x;\n");
    fprintf(out, "}\n");
    /* RC3: move варианти — без retain (виж str по-горе) */
    if (cg->rc) {
        fprintf(out, "static void baga_vec_push_vec_move(baga_Vec *v, baga_Vec *x) { baga_vec_grow(v); v->data[v->len++] = (void *)x; }\n");
        fprintf(out, "static void baga_vec_set_vec_move(baga_Vec *v, int64_t i, baga_Vec *x) {\n");
        fprintf(out, "    if (i < 0 || i >= v->len) baga_bounds_fail(\"vec_set\", i, v->len);\n");
        fprintf(out, "    baga_rc_release_vec((baga_Vec *)v->data[i], 0, 0, NULL); v->data[i] = (void *)x; }\n");
    }
    /* RC5 v0.9: overwrite на Vec<Vec<S>> — release на стария вътрешен vec с
     * destructor за S полетата (elem_rel = baga_rc_relv_<S>). Retain на новия
     * преди release на стария (alias-safe ред, както set_vec по-горе). */
    if (cg->rc) {
        fprintf(out, "static void baga_vec_set_vec_rc(baga_Vec *v, int64_t i, baga_Vec *x, void (*elem_rel)(void *)) {\n");
        fprintf(out, "    if (i < 0 || i >= v->len) baga_bounds_fail(\"vec_set\", i, v->len);\n");
        fprintf(out, "    baga_rc_retain((void *)x); elem_rel(v->data[i]); v->data[i] = (void *)x; }\n");
        fprintf(out, "static void baga_vec_set_vec_move_rc(baga_Vec *v, int64_t i, baga_Vec *x, void (*elem_rel)(void *)) {\n");
        fprintf(out, "    if (i < 0 || i >= v->len) baga_bounds_fail(\"vec_set\", i, v->len);\n");
        fprintf(out, "    elem_rel(v->data[i]); v->data[i] = (void *)x; }\n");
    }
    fprintf(out, "static baga_Vec *baga_vec_slice_vec(baga_Vec *v, int64_t a, int64_t b) {\n");
    fprintf(out, "    if (a < 0) a = 0; if (b > v->len) b = v->len; if (b < a) b = a;\n");
    fprintf(out, "    baga_Vec *r = baga_vec_new();\n");
    fprintf(out, "    for (int64_t i = a; i < b; i++) baga_vec_push_vec(r, (baga_Vec *)v->data[i]);\n");
    fprintf(out, "    return r; }\n");
    fprintf(out, "static baga_Vec *baga_vec_concat_vec(baga_Vec *v, baga_Vec *w) {\n");
    fprintf(out, "    baga_Vec *r = baga_vec_new();\n");
    fprintf(out, "    for (int64_t i = 0; i < v->len; i++) baga_vec_push_vec(r, (baga_Vec *)v->data[i]);\n");
    fprintf(out, "    for (int64_t i = 0; i < w->len; i++) baga_vec_push_vec(r, (baga_Vec *)w->data[i]);\n");
    fprintf(out, "    return r; }\n");
    fprintf(out, "\n/* hash map: chaining, key i64/str/bytes, value i64/str/f64/bytes (leak-tolerant like baga_Vec) */\n");
    fprintf(out, "typedef struct baga_MapEntry {\n");
    fprintf(out, "    int64_t ik; const char *sk; baga_bytes bk; int64_t ktag;\n");
    fprintf(out, "    int64_t iv; double fv; const char *sv; baga_bytes bv; void *pv;\n");
    fprintf(out, "    struct baga_MapEntry *next;\n");
    fprintf(out, "} baga_MapEntry;\n");
    fprintf(out, "typedef struct { baga_MapEntry **b; int64_t nb; int64_t len; } baga_Map;\n");
    fprintf(out, "static uint64_t baga_map_hash_str(const char *s) {\n");
    fprintf(out, "    uint64_t h = 1469598103934665603ULL;\n");
    fprintf(out, "    while (*s) { h ^= (unsigned char)*s++; h *= 1099511628211ULL; }\n");
    fprintf(out, "    return h; }\n");
    /* R67: bytes ключове — FNV-1a върху data+len (NUL-safe) */
    fprintf(out, "static uint64_t baga_map_hash_bytes(baga_bytes k) {\n");
    fprintf(out, "    uint64_t h = 1469598103934665603ULL;\n");
    fprintf(out, "    for (int64_t i = 0; i < k.len; i++) { h ^= k.data[i]; h *= 1099511628211ULL; }\n");
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
    fprintf(out, "            uint64_t h = e->ktag == 2 ? baga_map_hash_bytes(e->bk)\n");
    fprintf(out, "                       : e->sk ? baga_map_hash_str(e->sk) : baga_map_hash_i64(e->ik);\n");
    fprintf(out, "            e->next = m->b[h %% (uint64_t)m->nb];\n");
    fprintf(out, "            m->b[h %% (uint64_t)m->nb] = e;\n");
    fprintf(out, "            e = nx; } }\n");
    /* MEM-4д: старият bucket масив се free-ва след rehash — иначе в persist
     * региона всяко удвояване оставя стария масив завинаги (вж. vec_grow). */
    fprintf(out, "    baga_free(ob, onb * (int64_t)sizeof(baga_MapEntry *)); }\n");
    fprintf(out, "static baga_MapEntry *baga_map_put(baga_Map *m, int64_t ik, const char *sk, uint64_t h) {\n");
    /* RC1: ключът се retain-ва при претенция за съхранение; ако entry-то
     * вече съществува, новият ключ не се пъха → retain-ът се пуска */
    if (cg->rc) {
        fprintf(out, "    if (sk) baga_rc_retain((void *)sk);\n");
        fprintf(out, "    baga_MapEntry **slot = baga_map_slot(m, ik, sk, h);\n");
        fprintf(out, "    if (*slot) { if (sk) baga_rc_release_str(sk); return *slot; }\n");
    } else {
        fprintf(out, "    baga_MapEntry **slot = baga_map_slot(m, ik, sk, h);\n");
        fprintf(out, "    if (*slot) return *slot;\n");
    }
    fprintf(out, "    baga_MapEntry *e = baga_alloc(sizeof(baga_MapEntry));\n");
    fprintf(out, "    e->ik = ik; e->sk = sk; e->iv = 0; e->fv = 0; e->sv = NULL;\n");
    fprintf(out, "    e->bv.data = NULL; e->bv.len = 0; e->pv = NULL; e->next = NULL;\n");
    fprintf(out, "    e->ktag = sk ? 1 : 0; e->bk.data = NULL; e->bk.len = 0;\n");
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
            /* RC1: стойността се retain-ва; при overwrite — release на старата */
            if (cg->rc && vi == 1)
                fprintf(out, "    baga_rc_retain((void *)v); baga_rc_release_str(e->sv);\n");
            fprintf(out, "    e->%s = v; }\n", fld);
            /* RC3: move вариант — без retain на стойността (last-use локал);
             * release на старата при overwrite както обикновено */
            if (cg->rc && vi == 1) {
                fprintf(out, "static void baga_map_set_%s_str_move(baga_Map *m, %s, const char *v) {\n", kn, karg);
                fprintf(out, "    baga_MapEntry *e = baga_map_put(m, %s, %s, %s);\n", ikv, skv, hk);
                fprintf(out, "    baga_rc_release_str(e->sv); e->sv = v; }\n");
            }
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
        /* RC1: retain на data; при overwrite — release на старите */
        if (cg->rc)
            fprintf(out, "    baga_rc_retain((void *)v.data); baga_rc_release_bytes(e->bv);\n");
        fprintf(out, "    e->bv = v; }\n");
        /* RC3: move вариант — без retain на data (виж str по-горе) */
        if (cg->rc) {
            fprintf(out, "static void baga_map_set_%s_bytes_move(baga_Map *m, %s, baga_bytes v) {\n", kn, karg);
            fprintf(out, "    baga_MapEntry *e = baga_map_put(m, %s, %s, %s);\n", ikv, skv, hk);
            fprintf(out, "    baga_rc_release_bytes(e->bv); e->bv = v; }\n");
        }
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
        /* RC5 v0.3: overwrite release-ва полетата на стария box преди memcpy */
        if (cg->rc) {
            fprintf(out, "static void baga_map_set_%s_box_rc(baga_Map *m, %s, const void *src, int64_t size, void (*val_rel)(void *)) {\n", kn, karg);
            fprintf(out, "    baga_MapEntry *e = baga_map_put(m, %s, %s, %s);\n", ikv, skv, hk);
            fprintf(out, "    if (e->pv && val_rel) val_rel(e->pv);\n");
            fprintf(out, "    if (!e->pv) e->pv = baga_alloc((size_t)size);\n");
            fprintf(out, "    memcpy(e->pv, src, (size_t)size); }\n");
        }
        fprintf(out, "static void *baga_map_get_%s_box(baga_Map *m, %s) {\n", kn, karg);
        fprintf(out, "    baga_MapEntry *e = *baga_map_slot(m, %s, %s, %s);\n", ikv, skv, hk);
        fprintf(out, "    return e ? e->pv : NULL; }\n");
        fprintf(out, "static int64_t baga_map_has_%s(baga_Map *m, %s) {\n", kn, karg);
        fprintf(out, "    return *baga_map_slot(m, %s, %s, %s) ? 1 : 0; }\n", ikv, skv, hk);
        fprintf(out, "static void baga_map_del_%s(baga_Map *m, %s) {\n", kn, karg);
        fprintf(out, "    baga_MapEntry **slot = baga_map_slot(m, %s, %s, %s);\n", ikv, skv, hk);
        fprintf(out, "    if (!*slot) return;\n");
        /* MEM-4д: del без free беше вечен leak в persist региона (pin/unpin
         * churn = 112 B на цикъл — 2.4 KB/ред на boilaDB insert bench).
         * Box стойността (pv) остава — del не знае val_size (drop_map я
         * чисти); shell-ът на entry-то се рециклира през baga_free. */
        fprintf(out, "    baga_MapEntry *e = *slot; *slot = e->next; m->len--;\n");
        /* RC1: del release-ва ключ + str/bytes стойност (entry shell-ът се
         * free-ва от MEM-4д кодa по-долу, както досега) */
        if (cg->rc) {
            fprintf(out, "    if (e->ktag == 2) baga_rc_release_bytes(e->bk); else baga_rc_release_str(e->sk);\n");
            fprintf(out, "    baga_rc_release_str(e->sv); baga_rc_release_bytes(e->bv);\n");
        }
        fprintf(out, "    baga_free(e, (int64_t)sizeof(baga_MapEntry)); }\n");
        /* RC5 v0.3: del с box стойност — release на полетата + free на pv
         * (откаченото entry не се вижда от release_map при drop). */
        if (cg->rc) {
            fprintf(out, "static void baga_map_del_%s_rc(baga_Map *m, %s, int64_t val_size, void (*val_rel)(void *)) {\n", kn, karg);
            fprintf(out, "    baga_MapEntry **slot = baga_map_slot(m, %s, %s, %s);\n", ikv, skv, hk);
            fprintf(out, "    if (!*slot) return;\n");
            fprintf(out, "    baga_MapEntry *e = *slot; *slot = e->next; m->len--;\n");
            fprintf(out, "    if (e->ktag == 2) baga_rc_release_bytes(e->bk); else baga_rc_release_str(e->sk);\n");
            fprintf(out, "    if (e->pv) { if (val_rel) val_rel(e->pv); baga_free(e->pv, val_size); }\n");
            fprintf(out, "    baga_free(e, (int64_t)sizeof(baga_MapEntry)); }\n");
        }
        fprintf(out, "static baga_Vec *baga_map_keys_%s(baga_Map *m) {\n", kn);
        fprintf(out, "    baga_Vec *v = baga_vec_new();\n");
        fprintf(out, "    for (int64_t i = 0; i < m->nb; i++)\n");
        fprintf(out, "        for (baga_MapEntry *e = m->b[i]; e; e = e->next)\n");
        if (ki == 0) fprintf(out, "            baga_vec_push_str(v, e->sk ? e->sk : \"\");\n");
        else         fprintf(out, "            baga_vec_push_i64(v, e->ik);\n");
        fprintf(out, "    return v; }\n");
    }
    /* R67: bytes ключове — отделни slot/put с memcmp (ktag=2 в entry-то);
     * същият набор typed варианти като str/i64 по-горе */
    fprintf(out, "static baga_MapEntry **baga_map_slot_b(baga_Map *m, baga_bytes k, uint64_t h) {\n");
    fprintf(out, "    baga_MapEntry **e = &m->b[h %% (uint64_t)m->nb];\n");
    fprintf(out, "    while (*e) {\n");
    fprintf(out, "        if ((*e)->ktag == 2 && (*e)->bk.len == k.len &&\n");
    fprintf(out, "            (k.len == 0 || memcmp((*e)->bk.data, k.data, (size_t)k.len) == 0)) return e;\n");
    fprintf(out, "        e = &(*e)->next; }\n");
    fprintf(out, "    return e; }\n");
    fprintf(out, "static baga_MapEntry *baga_map_put_b(baga_Map *m, baga_bytes k, uint64_t h) {\n");
    /* RC1: същото като str put — retain на ключа, пускане ако не се пъха */
    if (cg->rc) {
        fprintf(out, "    baga_rc_retain((void *)k.data);\n");
        fprintf(out, "    baga_MapEntry **slot = baga_map_slot_b(m, k, h);\n");
        fprintf(out, "    if (*slot) { baga_rc_release_bytes(k); return *slot; }\n");
    } else {
        fprintf(out, "    baga_MapEntry **slot = baga_map_slot_b(m, k, h);\n");
        fprintf(out, "    if (*slot) return *slot;\n");
    }
    fprintf(out, "    baga_MapEntry *e = baga_alloc(sizeof(baga_MapEntry));\n");
    fprintf(out, "    e->ik = 0; e->sk = NULL; e->bk = k; e->ktag = 2;\n");
    fprintf(out, "    e->iv = 0; e->fv = 0; e->sv = NULL;\n");
    fprintf(out, "    e->bv.data = NULL; e->bv.len = 0; e->pv = NULL; e->next = NULL;\n");
    fprintf(out, "    *slot = e; m->len++;\n");
    fprintf(out, "    if (m->len * 4 > m->nb * 3) baga_map_rehash(m);\n");
    fprintf(out, "    return e; }\n");
    for (int vi = 0; vi < 3; vi++) {
        const char *vn   = vi == 0 ? "i64" : (vi == 1 ? "str" : "f64");
        const char *varg = vi == 0 ? "int64_t v" : (vi == 1 ? "const char *v" : "double v");
        const char *fld  = vi == 0 ? "iv" : (vi == 1 ? "sv" : "fv");
        fprintf(out, "static void baga_map_set_bytes_%s(baga_Map *m, baga_bytes k, %s) {\n", vn, varg);
        fprintf(out, "    baga_MapEntry *e = baga_map_put_b(m, k, baga_map_hash_bytes(k));\n");
        if (cg->rc && vi == 1)
            fprintf(out, "    baga_rc_retain((void *)v); baga_rc_release_str(e->sv);\n");
        fprintf(out, "    e->%s = v; }\n", fld);
        /* RC3: move вариант — без retain на стойността (виж str ключове) */
        if (cg->rc && vi == 1) {
            fprintf(out, "static void baga_map_set_bytes_str_move(baga_Map *m, baga_bytes k, const char *v) {\n");
            fprintf(out, "    baga_MapEntry *e = baga_map_put_b(m, k, baga_map_hash_bytes(k));\n");
            fprintf(out, "    baga_rc_release_str(e->sv); e->sv = v; }\n");
        }
        const char *ret = vi == 0 ? "int64_t" : (vi == 1 ? "const char *" : "double");
        fprintf(out, "static %s baga_map_get_bytes_%s(baga_Map *m, baga_bytes k) {\n", ret, vn);
        fprintf(out, "    baga_MapEntry *e = *baga_map_slot_b(m, k, baga_map_hash_bytes(k));\n");
        if (vi == 1) fprintf(out, "    return (e && e->sv) ? e->sv : \"\"; }\n");
        else if (vi == 0) fprintf(out, "    return e ? e->iv : 0; }\n");
        else fprintf(out, "    return e ? e->fv : 0; }\n");
    }
    fprintf(out, "static void baga_map_set_bytes_bytes(baga_Map *m, baga_bytes k, baga_bytes v) {\n");
    fprintf(out, "    baga_MapEntry *e = baga_map_put_b(m, k, baga_map_hash_bytes(k));\n");
    if (cg->rc)
        fprintf(out, "    baga_rc_retain((void *)v.data); baga_rc_release_bytes(e->bv);\n");
    fprintf(out, "    e->bv = v; }\n");
    /* RC3: move вариант — без retain на data */
    if (cg->rc) {
        fprintf(out, "static void baga_map_set_bytes_bytes_move(baga_Map *m, baga_bytes k, baga_bytes v) {\n");
        fprintf(out, "    baga_MapEntry *e = baga_map_put_b(m, k, baga_map_hash_bytes(k));\n");
        fprintf(out, "    baga_rc_release_bytes(e->bv); e->bv = v; }\n");
    }
    fprintf(out, "static baga_bytes baga_map_get_bytes_bytes(baga_Map *m, baga_bytes k) {\n");
    fprintf(out, "    baga_MapEntry *e = *baga_map_slot_b(m, k, baga_map_hash_bytes(k));\n");
    fprintf(out, "    if (!e) { baga_bytes z; z.data = NULL; z.len = 0; return z; }\n");
    fprintf(out, "    return e->bv; }\n");
    fprintf(out, "static void baga_map_set_bytes_box(baga_Map *m, baga_bytes k, const void *src, int64_t size) {\n");
    fprintf(out, "    baga_MapEntry *e = baga_map_put_b(m, k, baga_map_hash_bytes(k));\n");
    fprintf(out, "    if (!e->pv) e->pv = baga_alloc((size_t)size);\n");
    fprintf(out, "    memcpy(e->pv, src, (size_t)size); }\n");
    /* RC5 v0.3: overwrite release-ва полетата на стария box преди memcpy */
    if (cg->rc) {
        fprintf(out, "static void baga_map_set_bytes_box_rc(baga_Map *m, baga_bytes k, const void *src, int64_t size, void (*val_rel)(void *)) {\n");
        fprintf(out, "    baga_MapEntry *e = baga_map_put_b(m, k, baga_map_hash_bytes(k));\n");
        fprintf(out, "    if (e->pv && val_rel) val_rel(e->pv);\n");
        fprintf(out, "    if (!e->pv) e->pv = baga_alloc((size_t)size);\n");
        fprintf(out, "    memcpy(e->pv, src, (size_t)size); }\n");
    }
    fprintf(out, "static void *baga_map_get_bytes_box(baga_Map *m, baga_bytes k) {\n");
    fprintf(out, "    baga_MapEntry *e = *baga_map_slot_b(m, k, baga_map_hash_bytes(k));\n");
    fprintf(out, "    return e ? e->pv : NULL; }\n");
    fprintf(out, "static int64_t baga_map_has_bytes(baga_Map *m, baga_bytes k) {\n");
    fprintf(out, "    return *baga_map_slot_b(m, k, baga_map_hash_bytes(k)) ? 1 : 0; }\n");
    fprintf(out, "static void baga_map_del_bytes(baga_Map *m, baga_bytes k) {\n");
    fprintf(out, "    baga_MapEntry **slot = baga_map_slot_b(m, k, baga_map_hash_bytes(k));\n");
    fprintf(out, "    if (!*slot) return;\n");
    /* MEM-4д: същото като str/i64 del — entry-то се free-ва (вж. горе). */
    fprintf(out, "    baga_MapEntry *e = *slot; *slot = e->next; m->len--;\n");
    /* RC1: release на bytes ключ + str/bytes стойност преди free на shell-а */
    if (cg->rc) {
        fprintf(out, "    baga_rc_release_bytes(e->bk);\n");
        fprintf(out, "    baga_rc_release_str(e->sv); baga_rc_release_bytes(e->bv);\n");
    }
    fprintf(out, "    baga_free(e, (int64_t)sizeof(baga_MapEntry)); }\n");
    /* RC5 v0.3: del с box стойност — release на полетата + free на pv */
    if (cg->rc) {
        fprintf(out, "static void baga_map_del_bytes_rc(baga_Map *m, baga_bytes k, int64_t val_size, void (*val_rel)(void *)) {\n");
        fprintf(out, "    baga_MapEntry **slot = baga_map_slot_b(m, k, baga_map_hash_bytes(k));\n");
        fprintf(out, "    if (!*slot) return;\n");
        fprintf(out, "    baga_MapEntry *e = *slot; *slot = e->next; m->len--;\n");
        fprintf(out, "    baga_rc_release_bytes(e->bk);\n");
        fprintf(out, "    if (e->pv) { if (val_rel) val_rel(e->pv); baga_free(e->pv, val_size); }\n");
        fprintf(out, "    baga_free(e, (int64_t)sizeof(baga_MapEntry)); }\n");
    }
    fprintf(out, "static baga_Vec *baga_map_keys_bytes(baga_Map *m) {\n");
    fprintf(out, "    baga_Vec *v = baga_vec_new();\n");
    fprintf(out, "    for (int64_t i = 0; i < m->nb; i++)\n");
    fprintf(out, "        for (baga_MapEntry *e = m->b[i]; e; e = e->next)\n");
    fprintf(out, "            baga_vec_push_bytes(v, e->bk);\n");
    fprintf(out, "    return v; }\n");
    /* RC1: release на Map — rc върху STRUCT алокацията. При rc→0 release-ва
     * ключовете (по ktag/sk на entry-то) и стойностите според val_tag
     * (0 inline, 1 str, 2 bytes, 3 struct box), после free на entries,
     * buckets и struct. RC5 v0.2: val_rel е destructor на box полетата
     * (NULL → само free). */
    if (cg->rc) {
        fprintf(out, "static void baga_rc_release_map(baga_Map *m, int val_tag, int64_t val_size, void (*val_rel)(void *)) {\n");
        fprintf(out, "    if (!m) return;\n");
        fprintf(out, "    baga_Hdr *h = baga_rc_hdr((void *)m);\n");
        fprintf(out, "    if (!h) return;\n");
        fprintf(out, "    if (h->rc == 0) { fprintf(stderr, \"baga: rc underflow (map — двоен release)\\n\"); exit(1); }\n");
        fprintf(out, "    if (--h->rc > 0) return;\n");
        fprintf(out, "    for (int64_t i = 0; i < m->nb; i++) {\n");
        fprintf(out, "        baga_MapEntry *e = m->b[i];\n");
        fprintf(out, "        while (e) {\n");
        fprintf(out, "            baga_MapEntry *nx = e->next;\n");
        fprintf(out, "            if (e->ktag == 2) baga_rc_release_bytes(e->bk);\n");
        fprintf(out, "            else if (e->sk) baga_rc_release_str(e->sk);\n");
        fprintf(out, "            if (val_tag == 1) baga_rc_release_str(e->sv);\n");
        fprintf(out, "            else if (val_tag == 2) baga_rc_release_bytes(e->bv);\n");
        fprintf(out, "            else if (val_tag == 3 && e->pv) { if (val_rel) val_rel(e->pv); baga_free(e->pv, val_size); }\n");
        fprintf(out, "            baga_free(e, (int64_t)sizeof(baga_MapEntry));\n");
        fprintf(out, "            e = nx;\n");
        fprintf(out, "        }\n");
        fprintf(out, "    }\n");
        fprintf(out, "    baga_free(m->b, m->nb * (int64_t)sizeof(baga_MapEntry *));\n");
        fprintf(out, "    baga_free(m, (int64_t)sizeof(baga_Map));\n");
        fprintf(out, "}\n");
    }
    /* MEM-1: drop walkers — рециклират собствените алокации през baga_free.
     * Вътрешните буфери (bytes.data, str полета, env box-ове на closures)
     * остават в arena-та — споделена собственост, документирано. */
    fprintf(out, "\nstatic void baga_cell2_free(int64_t h);\n");
    fprintf(out, "static void baga_drop_bytes(baga_bytes b) { baga_free(b.data, b.len); }\n");
    fprintf(out, "/* elem_kind: 0 = inline (i64/f64), 1 = str (споделени — не се пипат), 2 = box (bytes/struct) */\n");
    /* RC1: под --rc drop ≡ release — елементите се release-ват, не се
     * free-ват директно (може да са retain-нати от друг собственик) */
    if (cg->rc) {
        fprintf(out, "static void baga_drop_vec(baga_Vec *v, int elem_kind, int64_t elem_size) {\n");
        fprintf(out, "    baga_rc_release_vec(v, elem_kind, elem_size, NULL);\n");
        fprintf(out, "}\n");
    } else {
        fprintf(out, "static void baga_drop_vec(baga_Vec *v, int elem_kind, int64_t elem_size) {\n");
    fprintf(out, "    if (!v) return;\n");
    fprintf(out, "    if (elem_kind == 2)\n");
    fprintf(out, "        for (int64_t i = 0; i < v->len; i++) baga_free(v->data[i], elem_size);\n");
    fprintf(out, "    if (elem_kind == 3)\n");
    fprintf(out, "        for (int64_t i = 0; i < v->len; i++) baga_drop_vec((baga_Vec *)v->data[i], 0, 0);\n");
    fprintf(out, "    baga_free(v->data, v->cap * 8);\n");
    fprintf(out, "    baga_free(v, (int64_t)sizeof(baga_Vec));\n");
    fprintf(out, "}\n");
    }
    /* RC1: drop_map ≡ release (val_is_box → val_tag 3); ключовете се
     * release-ват по entry съдържание, затова key tag не е нужен */
    if (cg->rc) {
        fprintf(out, "static void baga_drop_map(baga_Map *m, int val_is_box, int64_t val_size) {\n");
        fprintf(out, "    baga_rc_release_map(m, val_is_box ? 3 : 0, val_size, NULL);\n");
        fprintf(out, "}\n");
    } else {
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
    }
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
    /* R51: unsafe str handle casts — zero-copy cross-thread hop (chan i64).
       Без --rc str е arena-bound (никога не се free-ва). RC1.4: под --rc
       handle-ът е immortal escape — retain, иначе scope release на
       източника обесва handle-а (boila_ts_test FPE: map_h(mus) →
       boila_shard_lock върху освободена карта с nb=0). */
    if (cg->rc)
        fprintf(out, "static int64_t baga_str_h(const char *s) { baga_rc_retain((void *)s); return (int64_t)(intptr_t)s; }\n");
    else
        fprintf(out, "static int64_t baga_str_h(const char *s) { return (int64_t)(intptr_t)s; }\n");
    fprintf(out, "static const char *baga_h_str(int64_t h) { return (const char *)(intptr_t)h; }\n");
    /* R66: box baga_bytes header on arena for chan hop; data ptr already arena. */
    fprintf(out, "static int64_t baga_bytes_h(baga_bytes b) {\n");
    if (cg->rc)
        fprintf(out, "    baga_rc_retain((void *)b.data);\n");
    fprintf(out, "    baga_bytes *p = (baga_bytes *)baga_alloc(sizeof(baga_bytes));\n");
    fprintf(out, "    p->data = b.data; p->len = b.len; return (int64_t)(intptr_t)p;\n");
    fprintf(out, "}\n");
    fprintf(out, "static baga_bytes baga_h_bytes(int64_t h) {\n");
    fprintf(out, "    baga_bytes z; z.data = (unsigned char *)\"\"; z.len = 0;\n");
    fprintf(out, "    if (!h) return z; return *(baga_bytes *)(intptr_t)h;\n");
    fprintf(out, "}\n");
    /* R55: unsafe map handle casts — shared map through a go_bg i64 ctx. */
    if (cg->rc)
        fprintf(out, "static int64_t baga_map_h(baga_Map *m) { baga_rc_retain((void *)m); return (int64_t)(intptr_t)m; }\n");
    else
        fprintf(out, "static int64_t baga_map_h(baga_Map *m) { return (int64_t)(intptr_t)m; }\n");
    fprintf(out, "static baga_Map *baga_h_map(int64_t h) { return (baga_Map *)(intptr_t)h; }\n");
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

    /* plain enums (i64 tags) — no payload deps */
    for (int i = 0; i < program->items.len; i++) {
        Node *item = program->items.data[i];
        if (item->kind != NODE_ENUM || is_sum_enum_item(item)) continue;
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

    /* structs + sum enums in dependency order (L3 fields + struct payloads) */
    emit_structs_and_sum_enums(cg, program);

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

    /* RC2/RC2.1/RC3: броячи на елиминациите — коментар в края на изхода */
    if (cg->rc) {
        fprintf(out, "/* RC2: move elisions: %d */\n", cg->rc_moves);
        fprintf(out, "/* RC2.1: borrowed pair elisions: %d */\n", cg->rc_elided_pairs);
        fprintf(out, "/* RC3: container move elisions: %d */\n", cg->rc_cmoves);
        fprintf(out, "/* RC4: temp releases: %d */\n", cg->rc_tmp_count);
    }

    /* RC1: освободи scope стековете на компилатора */
    for (int i = 0; i < cg->rc_locals.len; i++) free(cg->rc_locals.data[i].name);
    vec_free(cg->rc_locals);
    vec_free(cg->rc_scopes);
    vec_free(cg->rc_lus);
    vec_free(cg->rc_tmps);
    vec_free(cg->rc_marks);
}
