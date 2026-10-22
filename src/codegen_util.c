/* codegen_util.c — част от codegen_c.c (виж cg_proto.h). */
#include "baga.h"
#include "codegen_impl.h"

/* ============================================================
 *  C code generator (Phase 1: C transpiler)
 *
 *  baga → C → gcc → binary
 * ============================================================ */

/* ---- честен отказ ----
 * Неподдържан AST възел е compile-time грешка, не тиха константа 0 —
 * същата политика като llvm_unsupported в LLVM бекенда. */
void c_unsupported_node(const char *what, Node *n) {
    fprintf(stderr, "baga: C backend: неподдържан %s (AST възел #%d)\n",
            what, n ? (int)n->kind : -1);
    exit(1);
}

/* ---- name mangling ----
 * Cyrillic (and other non-ASCII) identifiers become valid C
 * by encoding each non-ASCII byte as _XX hex.
 * All user names get a b_ prefix to avoid C keyword clashes.
 */

void emit_mangled(FILE *f, const char *name) {
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

char *mangle_name(const char *name) {
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
Node *find_extern_fn(Codegen *cg, const char *name) {
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
int extern_ret_is_str(Node *ef) {
    Node *t = ef->ret_type;
    while (t && t->kind == NODE_TYPE_EFFECT) t = t->inner_type;
    return t && t->kind == NODE_TYPE && strcmp(t->type_name, "str") == 0;
}

/* C type for an extern prototype. str params are `const char *`, but a str
 * return is `char *` so the prototype is compatible with libc declarations
 * from the headers we emit (e.g. `char *getenv(const char *)` in stdlib.h). */
void emit_extern_type(FILE *f, Node *ty, int is_ret) {
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

void emit_indent(Codegen *cg) {
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
int rc_type_node_tag(Node *ty) {
    while (ty && (ty->kind == NODE_TYPE_EFFECT || ty->kind == NODE_TYPE_REF))
        ty = ty->inner_type;
    if (!ty || ty->kind != NODE_TYPE || !ty->type_name) return 0;
    if (strcmp(ty->type_name, "str") == 0)   return 1;
    if (strcmp(ty->type_name, "bytes") == 0) return 2;
    if (strcmp(ty->type_name, "Vec") == 0)   return 3;
    if (strcmp(ty->type_name, "Map") == 0)   return 4;
    return 0;
}

Node *find_struct_decl(Codegen *cg, const char *name);
Node *find_sum_enum_decl(Codegen *cg, const char *name);
char *struct_cname_str(Type *t);   /* M24: per-instance C име */

/* RC5 v0.10: взаимна рекурсия struct↔enum (enum поле с heap payload брои) */
static int rc_enum_has_heap_d(Codegen *cg, Node *item, int depth);
/* M24-RC: instance-aware heap tag (depth-guarded) — дефиницията е след
 * rc_nested_enum_field (взаимна рекурсия с rc_struct_has_heap_d) */
static int rc_heap_tag_d(Codegen *cg, Type *t, int depth);

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
            /* M24-RC: checked типът печели за struct/enum полета —
             * instantiated generic поле (`inner: Pair<i64>`) се проверява
             * per инстанция (decl-ът на Pair вижда само типови променливи) */
            if (t->type && (t->type->kind == TYPE_STRUCT ||
                            t->type->kind == TYPE_ENUM)) {
                if (rc_heap_tag_d(cg, t->type, depth + 1)) return 1;
                continue;
            }
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

int rc_struct_has_heap(Codegen *cg, const char *name) {
    return rc_struct_has_heap_d(cg, name, 0);
}

/* RC5 v0.5: име на struct типа на поле-възел, ако е вложен struct с heap
 * полета (иначе NULL). */
const char *rc_nested_struct_field(Codegen *cg, Node *fld_type) {
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

int rc_enum_has_heap(Codegen *cg, Node *item) {
    return rc_enum_has_heap_d(cg, item, 0);
}

/* RC5 v0.10: име на enum типа на поле-възел, ако е sum enum с heap payload
 * (иначе NULL). Огледало на rc_nested_struct_field. */
const char *rc_nested_enum_field(Codegen *cg, Node *fld_type) {
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

/* M24-RC: инстанция на generic struct decl (d) с конкретни targs (vt) —
 * има ли heap полета след resolve на типовите променливи? Decl-нивото
 * вижда `v: T` като не-heap дори при T=str, затова проверката е per
 * инстанция. */
static int rc_gen_inst_has_heap_d(Codegen *cg, Node *d, Type *vt, int depth) {
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
                if (rc_heap_tag_d(cg, vt->targs[a], depth + 1)) return 1;
                is_var = 1;
                break;
            }
        }
        if (is_var) continue;
        if (rc_type_node_tag(ft)) return 1;
        /* вложено поле: checked типът печели (може instantiated generic) */
        if (t->type && (t->type->kind == TYPE_STRUCT ||
                        t->type->kind == TYPE_ENUM)) {
            if (rc_heap_tag_d(cg, t->type, depth + 1)) return 1;
            continue;
        }
        if (rc_struct_has_heap_d(cg, t->type_name, depth + 1)) return 1;
        Node *ed = find_sum_enum_decl(cg, t->type_name);
        if (ed && rc_enum_has_heap_d(cg, ed, depth + 1)) return 1;
    }
    return 0;
}

static int rc_heap_tag_d(Codegen *cg, Type *t, int depth) {
    if (depth > 32) return 0;
    int tag = rc_type_tag(t);
    if (tag) return tag;
    if (t && t->kind == TYPE_STRUCT && t->name) {
        /* M24-RC: generic decl — per инстанция; голо име без targs пада
         * обратно на decl-нивото (старото поведение) */
        Node *d = find_struct_decl(cg, t->name);
        if (d && d->n_struct_params > 0 && t->n_targs > 0)
            return rc_gen_inst_has_heap_d(cg, d, t, depth + 1) ? 5 : 0;
        if (rc_struct_has_heap(cg, t->name)) return 5;
    }
    /* RC5 v0.6: enum с heap payload */
    if (t && t->kind == TYPE_ENUM && t->name) {
        Node *ed = find_sum_enum_decl(cg, t->name);
        if (ed && rc_enum_has_heap(cg, ed)) return 6;
    }
    return 0;
}

int rc_heap_tag(Codegen *cg, Type *t) {
    return rc_heap_tag_d(cg, t, 0);
}

/* M24-RC: C име на RC helper-ите (retain/release/relf/...) за struct/enum
 * тип — instantiated generic struct (n_targs>0) получава per-instance
 * името (b_Box_i64 чрез struct_cname_str); иначе обикновеният mangle на
 * името (inferred Type първо, анотационен възел после). Върнатият низ е
 * malloc-нат — caller-ът го free-ва. */
char *rc_rc_type_name(Type *ty, Node *tnode) {
    if (ty && ty->kind == TYPE_STRUCT && ty->n_targs > 0)
        return struct_cname_str(ty);
    const char *sn = (ty && ty->name) ? ty->name : NULL;
    if (!sn && tnode) {
        Node *t = tnode;
        while (t && (t->kind == NODE_TYPE_EFFECT || t->kind == NODE_TYPE_REF))
            t = t->inner_type;
        if (t && t->kind == NODE_TYPE) sn = t->type_name;
    }
    return mangle_name(sn ? sn : "anon");
}

/* M24-RC: resolved тип на поле в контекста на инстанция k на generic decl
 * s (s=NULL → не-generic контекст). Типова променлива (`v: T`) → targ-ът
 * на инстанцията; вложено instantiated generic поле (`inner: Pair<i64>`)
 * → checked типът на възела (той носи targs). Иначе NULL — ползва се
 * досегашната node логика. */
Type *rc_fld_inst_type(Codegen *cg, Node *s, int k, Node *fld_type) {
    (void)cg;
    Node *t = fld_type;
    while (t && (t->kind == NODE_TYPE_EFFECT || t->kind == NODE_TYPE_REF))
        t = t->inner_type;
    if (!t || t->kind != NODE_TYPE || !t->type_name) return NULL;
    if (s && k >= 0) {
        for (int a = 0; a < s->n_struct_params; a++)
            if (strcmp(t->type_name, s->struct_params[a]) == 0)
                return s->struct_inst_targs[k * s->n_struct_params + a];
        /* M26: Vec<T> / Map<…, T> поле с типова променлива вътре —
         * синтезирай resolved тип с конкретния elem (дълбок release на
         * вложени Vec-ове; видът на ключа не е нужен за release) */
        if (strcmp(t->type_name, "Vec") == 0 || strcmp(t->type_name, "Map") == 0) {
            Node *en = t->inner_type2 && strcmp(t->type_name, "Map") == 0
                     ? t->inner_type2 : t->inner_type;   /* Map: val; Vec: elem */
            while (en && (en->kind == NODE_TYPE_EFFECT || en->kind == NODE_TYPE_REF))
                en = en->inner_type;
            if (en && en->kind == NODE_TYPE && en->type_name) {
                for (int a = 0; a < s->n_struct_params; a++)
                    if (strcmp(en->type_name, s->struct_params[a]) == 0) {
                        Type *vt = calloc(1, sizeof(Type));
                        if (!vt) { fprintf(stderr, "baga: out of memory\n"); exit(1); }
                        vt->kind = strcmp(t->type_name, "Vec") == 0
                                 ? TYPE_VEC : TYPE_MAP;
                        vt->elem = s->struct_inst_targs[k * s->n_struct_params + a];
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

int rc_heap_tag_node(Codegen *cg, Node *ty) {
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
__attribute__((unused)) int rc_expr_copies_ident(Node *n, const char *name) {
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

void rc_emit_retain_val(Codegen *cg, int tag, Type *ty, Node *tnode,
                               const char *cname) {
    FILE *f = cg->out;
    if (tag == 5 || tag == 6) {
        /* struct (5) или enum с heap payload (6) — retain_<име> по tag.
         * M24-RC: instantiated generic struct — per-instance име. */
        if (!((ty && ty->name) || tnode)) return;
        char *sm = rc_rc_type_name(ty, tnode);
        if (strcmp(sm, "b_anon") == 0) { free(sm); return; }
        fprintf(f, "baga_rc_retain_%s(%s); ", sm, cname);
        free(sm);
    } else if (tag == 2)
        fprintf(f, "baga_rc_retain((void *)%s.data); ", cname);
    else if (tag)
        fprintf(f, "baga_rc_retain((void *)%s); ", cname);
}

/* намира track-нат локал по baga име; връща индекс (-1 = няма).
 * Търси от върха надолу — вътрешно засенчване печели. */
int rc_find(Codegen *cg, const char *name) {
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
int rc_borrowed_init(Node *init) {
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
/* kind/size по самия елементен Type (общо за inferred път и за анотация
 * с проверен тип — виж _node вариантите). */
static int rc_vec_elem_kind_type(Type *e, char *sz, size_t szn) {
    snprintf(sz, szn, "0");
    if (!e) return 0;
    if (e->kind == TYPE_STR) return 1;
    if (e->kind == TYPE_BYTES) {
        snprintf(sz, szn, "(int64_t)sizeof(baga_bytes)");
        return 4;
    }
    if (e->kind == TYPE_VEC) return 3;
    if (e->name && (e->kind == TYPE_STRUCT || e->kind == TYPE_ENUM)) {
        /* M24-RC: instantiated generic елемент — per-instance sizeof */
        char *m = e->kind == TYPE_STRUCT && e->n_targs > 0
            ? struct_cname_str(e) : mangle_name(e->name);
        snprintf(sz, szn, "(int64_t)sizeof(%s)", m);
        free(m);
        return 2;
    }
    return 0;
}

int rc_vec_elem_kind(Type *vty, char *sz, size_t szn) {
    return rc_vec_elem_kind_type(vty ? vty->elem : NULL, sz, szn);
}

/* RC1: val_tag за release на Map: 0 inline (i64/f64), 1 str, 2 bytes,
 * 3 struct box (free на pv по val_size; полетата вътре не се track-ват). */
static int rc_map_val_tag_type(Type *v, char *sz, size_t szn) {
    snprintf(sz, szn, "0");
    if (!v) return 0;
    if (v->kind == TYPE_STR) return 1;
    if (v->kind == TYPE_BYTES) return 2;
    if (v->name && (v->kind == TYPE_STRUCT || v->kind == TYPE_ENUM)) {
        /* M24-RC: instantiated generic стойност — per-instance sizeof */
        char *m = v->kind == TYPE_STRUCT && v->n_targs > 0
            ? struct_cname_str(v) : mangle_name(v->name);
        snprintf(sz, szn, "(int64_t)sizeof(%s)", m);
        free(m);
        return 3;
    }
    return 0;
}

int rc_map_val_tag(Type *mty, char *sz, size_t szn) {
    return rc_map_val_tag_type(mty ? mty->elem : NULL, sz, szn);
}

/* същите два resolver-а, но от анотационен type AST възел (`let v: Vec<str>`)
 * — inferred Type на vec_new()/map_new() няма elem/key информация */
int rc_vec_elem_kind_node(Node *ty, char *sz, size_t szn) {
    Node *e = ty ? ty->inner_type : NULL;
    snprintf(sz, szn, "0");
    if (!e || e->kind != NODE_TYPE || !e->type_name) return 0;
    /* M21: checked типът на възела (конкретен за инстанцията след
     * checker_recheck_inst) печели — иначе типова променлива (U/T)
     * изглежда като име на struct и се емитира sizeof(b_U). */
    if (e->type) return rc_vec_elem_kind_type(e->type, sz, szn);
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

int rc_map_val_tag_node(Node *ty, char *sz, size_t szn) {
    Node *v = ty ? ty->inner_type2 : NULL;
    snprintf(sz, szn, "0");
    if (!v || v->kind != NODE_TYPE || !v->type_name) return 0;
    /* M21: checked типът печели (виж бележката при vec варианта) */
    if (v->type) return rc_map_val_tag_type(v->type, sz, szn);
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
void rc_box_rel(Codegen *cg, const char *type_name, char *rel, size_t reln) {
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

/* M24-RC: Type-aware вариант на rc_box_rel — instantiated generic struct
 * елемент/стойност получава per-instance relf (baga_rc_relf_b_Box_i64),
 * само ако инстанцията реално има heap полета (иначе "0" — само free). */
void rc_box_rel_ty(Codegen *cg, Type *et, char *rel, size_t reln) {
    if (et && et->kind == TYPE_STRUCT && et->n_targs > 0) {
        snprintf(rel, reln, "0");
        if (!cg->rc) return;
        if (rc_heap_tag(cg, et) == 5) {
            char *m = struct_cname_str(et);
            snprintf(rel, reln, "baga_rc_relf_%s", m);
            free(m);
        }
        return;
    }
    rc_box_rel(cg, (et && et->name) ? et->name : NULL, rel, reln);
}

/* RC5 v0.10: елементен/стойностен тип (struct или enum), чийто box изисква
 * destructor + retain при споделяне — общ предикат за push/set/del/slice
 * сайтовете (досега бяха struct-only). */
int rc_box_tracked(Codegen *cg, Type *et) {
    if (!cg->rc || !et || !et->name) return 0;
    if (et->kind == TYPE_STRUCT)
        /* M24-RC: instance-aware (generic struct с targs) */
        return rc_heap_tag(cg, et) == 5;
    if (et->kind == TYPE_ENUM) {
        Node *ed = find_sum_enum_decl(cg, et->name);
        return ed && rc_enum_has_heap(cg, ed);
    }
    return 0;
}

/* M26 + RC5 v1.1: трябва ли shim за Vec<E> като елемент на външен Vec —
 * E носи heap някъде в дълбочина (str/bytes/struct с heap полета/enum с
 * heap payload/Vec рекурсивно). */
static int rc_vec_elem_deep_heap(Codegen *cg, Type *e) {
    if (!e) return 0;
    switch (e->kind) {
        case TYPE_STR: case TYPE_BYTES: return 1;
        case TYPE_VEC: return rc_vec_elem_deep_heap(cg, e->elem);
        case TYPE_STRUCT: return rc_heap_tag(cg, e) == 5;   /* instance-aware */
        case TYPE_ENUM: {
            if (!e->name) return 0;
            Node *ed = find_sum_enum_decl(cg, e->name);
            return ed && rc_enum_has_heap(cg, ed);
        }
        default: return 0;
    }
}

/* M26: enc на елементния тип за relv shim име: str / bytes /
 * mangled struct/enum име (b_Wrap; per-instance b_Box_i64) / v_<enc> за Vec. */
static char *rc_relv_enc(Codegen *cg, Type *e) {
    (void)cg;
    if (!e) return NULL;
    if (e->kind == TYPE_STR)   return strdup("str");
    if (e->kind == TYPE_BYTES) return strdup("bytes");
    if (e->kind == TYPE_STRUCT && e->name)
        return e->n_targs > 0 ? struct_cname_str(e) : mangle_name(e->name);
    if (e->kind == TYPE_ENUM && e->name)
        return mangle_name(e->name);
    if (e->kind == TYPE_VEC) {
        char *sub = rc_relv_enc(cg, e->elem);
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

/* M26: shim за Vec<E> (E = str/bytes/Vec<…>) като елемент на външен Vec —
 * baga_rc_relv_<enc(E)>(void *p) { baga_rc_release_vec(p, kind, sz, rel); },
 * веднъж на програма. Struct/enum елементи НЕ се emit-ват тук — техните
 * relv_<S> shim-ове идват от emit_rc_struct_helpers (като досега).
 * Изходът е lambda_out, когато е отворен (body стадий — печата се преди
 * телата на функциите); иначе текущия out — валидно САМО на top level на
 * decl стадия: helper-ите викат резолверите в pre-scan ПРЕДИ тялото си
 * (emit_rc_struct_helpers_one / emit_rc_enum_helpers), така че дефиницията
 * е преди употребата, а mid-body извикванията са no-op по dedup. */
static void rc_emit_relv_shim(Codegen *cg, Type *e) {
    if (!e || e->kind == TYPE_STRUCT || e->kind == TYPE_ENUM) return;
    char *enc = rc_relv_enc(cg, e);
    if (!enc) return;
    for (int i = 0; i < cg->rc_relv_shims.len; i++)
        if (strcmp(cg->rc_relv_shims.data[i], enc) == 0) { free(enc); return; }
    vec_push(cg->rc_relv_shims, enc);
    FILE *f = cg->lambda_out ? cg->lambda_out : cg->out;
    const char *rel = "NULL";
    char relbuf[160];
    int kind;
    char sz[48];
    snprintf(sz, sizeof sz, "0");
    if (e->kind == TYPE_STR) kind = 1;
    else if (e->kind == TYPE_BYTES) {
        kind = 4;
        snprintf(sz, sizeof sz, "(int64_t)sizeof(baga_bytes)");
    } else {   /* TYPE_VEC — рекурсия: shim-ът на елемента първи */
        kind = 3;
        rc_emit_relv_shim(cg, e->elem);
        char *sub = rc_relv_enc(cg, e->elem);
        snprintf(relbuf, sizeof relbuf, "baga_rc_relv_%s", sub ? sub : "0");
        free(sub);
        rel = relbuf;
    }
    fprintf(f, "static __attribute__((unused)) void baga_rc_relv_%s(void *p) { "
               "baga_rc_release_vec((baga_Vec *)p, %d, %s, %s); }\n",
            enc, kind, sz, rel);
}

/* RC5 v0.9 + M26: destructor за kind 3 (вложен Vec) елементи —
 * "baga_rc_relv_<enc>", когато елементът е Vec<E> и E носи heap (struct с
 * heap полета — v0.9; str/bytes/по-дълбока вложеност — M26). Иначе "0"
 * (runtime-ът release-ва вътрешния vec с kind 0 — скаларните му елементи
 * биха текли). Shim-овете за скаларни/вложени елементи се emit-ват lazy. */
void rc_nested_vec_rel_type(Codegen *cg, Type *vty, char *rel, size_t reln) {
    snprintf(rel, reln, "0");
    if (!cg->rc || !vty || !vty->elem) return;
    Type *inner = vty->elem;
    if (inner->kind != TYPE_VEC || !inner->elem) return;
    Type *es = inner->elem;
    if (!rc_vec_elem_deep_heap(cg, es)) return;
    /* struct/enum: relv_<S> shim-ът се emit-ва от emit_rc_struct_helpers;
     * str/bytes/Vec: lazy shim тук (M26) */
    rc_emit_relv_shim(cg, es);
    char *enc = rc_relv_enc(cg, es);
    if (!enc) { snprintf(rel, reln, "0"); return; }
    snprintf(rel, reln, "baga_rc_relv_%s", enc);
    free(enc);
}

/* същият resolver, но от анотационен type AST възел (`let v: Vec<Vec<S>>`).
 * M26: checked типът печели, когато е Vec (конкретен след recheck на
 * инстанция — M21) — така Vec<Vec<T>> с T=str/bytes/… стигна до shim-а. */
void rc_nested_vec_rel_node(Codegen *cg, Node *ty, char *rel, size_t reln) {
    snprintf(rel, reln, "0");
    if (!cg->rc || !ty) return;
    if (ty->type && ty->type->kind == TYPE_VEC) {
        rc_nested_vec_rel_type(cg, ty->type, rel, reln);
        return;
    }
    Node *inner = ty->inner_type;
    if (!inner || inner->kind != NODE_TYPE || !inner->type_name ||
        strcmp(inner->type_name, "Vec") != 0) return;
    Node *es = inner->inner_type;
    if (!es || es->kind != NODE_TYPE || !es->type_name) return;
    int heap = rc_struct_has_heap(cg, es->type_name);
    if (!heap) {
        Node *ed = find_sum_enum_decl(cg, es->type_name);
        heap = ed && rc_enum_has_heap(cg, ed);
    }
    if (!heap) return;
    char *m = mangle_name(es->type_name);
    snprintf(rel, reln, "baga_rc_relv_%s", m);
    free(m);
}

/* елементен/стойностен тип на Vec/Map локал (inferred Type, после анотация) */
const char *rc_vec_elem_name(RcLocal *e) {
    if (e->type && e->type->elem && e->type->elem->name)
        return e->type->elem->name;
    Node *it = e->type_node ? e->type_node->inner_type : NULL;
    if (it && it->kind == NODE_TYPE) return it->type_name;
    return NULL;
}

const char *rc_map_val_name(RcLocal *e) {
    if (e->type && e->type->elem && e->type->elem->name)
        return e->type->elem->name;
    Node *it = e->type_node ? e->type_node->inner_type2 : NULL;
    if (it && it->kind == NODE_TYPE) return it->type_name;
    return NULL;
}

/* комбинирани resolver-и: inferred Type първо, анотация като резерва */
int rc_vec_kind_of(RcLocal *e, char *sz, size_t szn) {
    int k = rc_vec_elem_kind(e->type, sz, szn);
    if (k == 0 && e->type_node)
        k = rc_vec_elem_kind_node(e->type_node, sz, szn);
    return k;
}

int rc_map_tag_of(RcLocal *e, char *sz, size_t szn) {
    int t = rc_map_val_tag(e->type, sz, szn);
    if (t == 0 && e->type_node)
        t = rc_map_val_tag_node(e->type_node, sz, szn);
    return t;
}

/* RC5 v0.9: комбиниран nested resolver — inferred Type първо, анотация после */
void rc_vec_nested_rel_of(Codegen *cg, RcLocal *e, char *rel, size_t reln) {
    rc_nested_vec_rel_type(cg, e->type, rel, reln);
    if (strcmp(rel, "0") == 0 && e->type_node)
        rc_nested_vec_rel_node(cg, e->type_node, rel, reln);
}

/* emit-ва един release ред (отстъпка + newline) за локал от стека */
void rc_emit_release(Codegen *cg, RcLocal *e) {
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
                /* M24-RC: instantiated generic struct — per-instance име */
                char *sm = rc_rc_type_name(e->type, e->type_node);
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
                char *em = rc_rc_type_name(e->type, e->type_node);
                fprintf(f, "baga_rc_release_%s(%s);\n", em, e->name);
                free(em);
            }
            break;
        }
    }
}

void rc_push_scope(Codegen *cg, int is_loop) {
    if (!cg->rc) return;
    vec_push(cg->rc_scopes, ((RcScope){ cg->rc_locals.len, is_loop }));
}

/* край на block: release на локалите на scope-а в обратен ред (params и
 * drop()нати — не), после pop. Извиква се преди затварящата '}'. */
void rc_pop_scope(Codegen *cg) {
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
void rc_release_all(Codegen *cg, int skip_idx) {
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
void rc_release_to_loop(Codegen *cg) {
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

void rc_register_node(Codegen *cg, const char *name, int tag, Type *ty,
                             Node *type_node, int is_param) {
    if (!cg->rc || !tag) return;
    vec_push(cg->rc_locals,
             ((RcLocal){ mangle_name(name), tag, ty, type_node, is_param, 0 }));
}

void rc_register(Codegen *cg, const char *name, int tag, Type *ty,
                        int is_param) {
    rc_register_node(cg, name, tag, ty, NULL, is_param);
}

