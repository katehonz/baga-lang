#include "baga.h"

/* ============================================================
 *  Proof extraction — Фаза 6
 *
 *  Извлича четими proof sketches от AST-то.
 *  Не са формални доказателства — са спецификации
 *  които човек (или AI) може да верифицира.
 * ============================================================ */

static const char *type_node_str(Node *ty) {
    if (!ty) return "void";
    if (ty->kind == NODE_TYPE) return ty->type_name;
    if (ty->kind == NODE_TYPE_REF) return "&T";
    if (ty->kind == NODE_TYPE_ARRAY) return "[T]";
    if (ty->kind == NODE_TYPE_EFFECT) return type_node_str(ty->inner_type);
    return "?";
}

static int has_effects(Node *fn) {
    if (!fn->ret_type) return 0;
    if (fn->ret_type->kind == NODE_TYPE_EFFECT) return 1;
    return 0;
}

static void print_effects(Node *ty) {
    if (!ty) return;
    if (ty->kind == NODE_TYPE_EFFECT) {
        for (int i = 0; i < ty->n_effects; i++)
            printf(" !%s", ty->effect_names[i]);
    }
}

/* Check if function has recursive calls */
static int is_recursive(Node *fn) {
    if (!fn->fn_body) return 0;
    /* simple heuristic: check if function name appears in a CALL */
    /* walk the body looking for calls to the same function */
    /* for now, just check if there's a while loop or recursive call pattern */
    return 0; /* TODO: implement */
}

/* Check if function has a base case (if with return) */
static int has_base_case(Node *fn) {
    if (!fn->fn_body) return 0;
    for (int i = 0; i < fn->fn_body->stmts.len; i++) {
        Node *s = fn->fn_body->stmts.data[i];
        if (s->kind == NODE_IF && s->then_br) {
            /* check if then branch has a return */
            if (s->then_br->kind == NODE_BLOCK) {
                for (int j = 0; j < s->then_br->stmts.len; j++) {
                    if (s->then_br->stmts.data[j]->kind == NODE_RETURN)
                        return 1;
                }
            }
        }
    }
    return 0;
}

/* Count return statements */
static int count_returns(Node *block) {
    if (!block || block->kind != NODE_BLOCK) return 0;
    int count = 0;
    for (int i = 0; i < block->stmts.len; i++) {
        Node *s = block->stmts.data[i];
        if (s->kind == NODE_RETURN) count++;
        if (s->kind == NODE_IF) {
            count += count_returns(s->then_br);
            if (s->else_br) count += count_returns(s->else_br);
        }
        if (s->kind == NODE_WHILE) count += count_returns(s->while_body);
        if (s->kind == NODE_BLOCK) count += count_returns(s);
    }
    return count;
}

static Node *find_spec(Node *program, const char *fn_name) {
    for (int i = 0; i < program->items.len; i++) {
        Node *item = program->items.data[i];
        if (item->kind == NODE_SPEC && strcmp(item->spec_name, fn_name) == 0)
            return item;
    }
    return NULL;
}

void print_proofs(Node *program) {
    printf("/* Proof sketches — извлечени от Бага компилатора */\n\n");

    for (int i = 0; i < program->items.len; i++) {
        Node *item = program->items.data[i];
        if (item->kind != NODE_FN || !item->fn_body) continue;

        const char *name = item->fn_name;
        Node *spec = find_spec(program, name);

        printf("proofs for %s:\n", name);

        /* 1. Signature theorem */
        printf("  theorem %s_signature:\n", name);
        printf("    ∀");
        for (int j = 0; j < item->params.len; j++) {
            Node *p = item->params.data[j];
            printf(" %s: %s", p->param_name, type_node_str(p->param_type));
            if (j < item->params.len - 1) printf(",");
        }
        printf(". %s(", name);
        for (int j = 0; j < item->params.len; j++) {
            Node *p = item->params.data[j];
            printf("%s", p->param_name);
            if (j < item->params.len - 1) printf(", ");
        }
        printf(") → %s", type_node_str(item->ret_type));
        if (has_effects(item)) print_effects(item->ret_type);
        printf("\n\n");

        /* 2. Termination theorem */
        int base = has_base_case(item);
        int rets = count_returns(item->fn_body);
        printf("  theorem %s_terminates:\n", name);
        printf("    ∀");
        for (int j = 0; j < item->params.len; j++) {
            Node *p = item->params.data[j];
            printf(" %s: %s", p->param_name, type_node_str(p->param_type));
            if (j < item->params.len - 1) printf(",");
        }
        printf(". terminates(%s(", name);
        for (int j = 0; j < item->params.len; j++) {
            printf("%s", item->params.data[j]->param_name);
            if (j < item->params.len - 1) printf(", ");
        }
        printf("))\n");
        if (base) {
            printf("    evidence: base case with early return");
            if (rets > 1) printf(", %d return paths", rets);
            printf("\n");
        } else {
            printf("    evidence: %d return path(s), no obvious base case\n", rets > 0 ? rets : 1);
        }
        printf("\n");

        /* 3. Purity / effect theorem */
        if (has_effects(item)) {
            printf("  theorem %s_effects:\n", name);
            printf("    %s has effects:", name);
            print_effects(item->ret_type);
            printf("\n");
            printf("    callers must handle or propagate these effects\n\n");
        } else {
            printf("  theorem %s_pure:\n", name);
            printf("    %s is pure (no declared effects)\n\n", name);
        }

        /* 4. Spec guarantees */
        if (spec) {
            printf("  spec \"%s\":\n", spec->spec_name);
            for (int j = 0; j < spec->n_guarantees; j++) {
                printf("    guarantee: %s\n", spec->spec_guarantees[j]);
            }
            printf("    status: UNVERIFIED — requires formal proof or testing\n\n");
        }
    }
}
