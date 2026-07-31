#include "baga.h"
#include <errno.h>
#include <unistd.h>
#include <sys/wait.h>

static char *read_file(const char *path, int *out_len) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "baga: не мога да отворя '%s': %s\n", path, strerror(errno));
        exit(1);
    }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);

    char *buf = malloc((size_t)sz + 1);
    if (!buf) { fprintf(stderr, "baga: out of memory\n"); exit(1); }

    size_t rd = fread(buf, 1, (size_t)sz, f);
    buf[rd] = '\0';
    fclose(f);

    if (out_len) *out_len = (int)rd;
    return buf;
}

static void usage(void) {
    fprintf(stderr,
        "Бага — компилатор, фаза 1\n"
        "\n"
        "Употреба: baga [опции] <файл.baga>\n"
        "\n"
        "Опции:\n"
        "  --emit-c    Генерирай C код на stdout, не компилирай\n"
        "  --test-specs  Property-based тестване на ensures/requires договорите\n"
        "  --ast       Изпечатвай AST (debug)\n"
        "  --tokens    Изпечатвай токени (debug)\n"
        "  --help      Тази помощ\n"
        "\n"
        "По подразбиране: генерира C, компилира с gcc, изпълнява.\n"
    );
}

int main(int argc, char **argv) {
    const char *input_path = NULL;
    int emit_c = 0;
    int dump_ast = 0;
    int dump_tokens = 0;
    int dump_specs = 0;
    int dump_proofs = 0;
    int emit_llvm = 0;
    int test_specs = 0;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--emit-c") == 0) { emit_c = 1; }
        else if (strcmp(argv[i], "--ast") == 0) { dump_ast = 1; }
        else if (strcmp(argv[i], "--tokens") == 0) { dump_tokens = 1; }
        else if (strcmp(argv[i], "--specs") == 0) { dump_specs = 1; }
        else if (strcmp(argv[i], "--proofs") == 0) { dump_proofs = 1; }
        else if (strcmp(argv[i], "--emit-llvm") == 0) { emit_llvm = 1; }
        else if (strcmp(argv[i], "--test-specs") == 0) { test_specs = 1; }
        else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            usage();
            return 0;
        }
        else if (argv[i][0] == '-') {
            fprintf(stderr, "baga: непозната опция '%s'\n", argv[i]);
            usage();
            return 1;
        }
        else {
            input_path = argv[i];
        }
    }

    if (!input_path) {
        usage();
        return 1;
    }

    /* read source */
    int src_len = 0;
    char *src = read_file(input_path, &src_len);

    /* lex */
    Lexer lexer;
    lexer_init(&lexer, src, src_len, input_path);

    VEC(Token) tokens = {0};
    for (;;) {
        Token t = lexer_next(&lexer);
        vec_push(tokens, t);
        if (t.kind == TOK_EOF) break;
        if (t.kind == TOK_ERROR) {
            baga_error(input_path, t.pos, "%s", t.text);
            return 1;
        }
    }

    if (dump_tokens) {
        for (int i = 0; i < tokens.len; i++) {
            Token *t = &tokens.data[i];
            fprintf(stderr, "%3d:%3d  %-16s", t->pos.line, t->pos.col,
                    token_kind_str(t->kind));
            if (t->text) fprintf(stderr, "  '%s'", t->text);
            if (t->kind == TOK_INT_LIT) fprintf(stderr, "  =%lld", (long long)t->int_val);
            if (t->kind == TOK_FLOAT_LIT) fprintf(stderr, "  =%g", t->float_val);
            fprintf(stderr, "\n");
        }
    }

    /* parse */
    Parser parser;
    Node *program = parse_program(&parser, tokens.data, tokens.len, input_path);

    if (parser.n_errors > 0) {
        for (int i = 0; i < parser.n_errors; i++)
            fprintf(stderr, "%s\n", parser.errors[i]);
        return 1;
    }

    if (dump_ast) {
        print_ast(program, 0);
    }

    if (dump_specs) {
        for (int i = 0; i < program->items.len; i++) {
            Node *item = program->items.data[i];
            if (item->kind != NODE_SPEC) continue;
            printf("spec %s {\n", item->spec_name);
            if (item->spec_inputs.len > 0) {
                printf("    input:\n");
                for (int j = 0; j < item->spec_inputs.len; j++) {
                    Node *p = item->spec_inputs.data[j];
                    printf("        %s: %s\n", p->param_name,
                           p->param_type && p->param_type->type_name ? p->param_type->type_name : "?");
                }
            }
            if (item->spec_output) {
                printf("    output: %s\n",
                       item->spec_output->type_name ? item->spec_output->type_name : "?");
            }
            if (item->n_guarantees > 0) {
                printf("    guarantees:\n");
                for (int j = 0; j < item->n_guarantees; j++)
                    printf("        - %s\n", item->spec_guarantees[j]);
            }
            if (item->spec_requires.len > 0) {
                printf("    requires:\n");
                for (int j = 0; j < item->spec_requires.len; j++)
                    printf("        %s\n", item->spec_requires.data[j]->ensure_text);
            }
            if (item->spec_ensures.len > 0) {
                printf("    ensures:\n");
                for (int j = 0; j < item->spec_ensures.len; j++)
                    printf("        %s\n", item->spec_ensures.data[j]->ensure_text);
            }
            printf("}\n\n");
        }
        return 0;
    }

    if (dump_proofs) {
        print_proofs(program);
        return 0;
    }

    /* check */
    Checker checker;
    memset(&checker, 0, sizeof(checker));
    check_program(&checker, program);

    if (checker.n_errors > 0) {
        for (int i = 0; i < checker.n_errors; i++)
            fprintf(stderr, "%s: %s\n", input_path, checker.errors[i]);
        return 1;
    }

#ifdef BAGA_LLVM
    if (emit_llvm) {
        codegen_llvm(program, NULL);
        return 0;
    }
#else
    if (emit_llvm) {
        fprintf(stderr, "baga: LLVM backend не е компилиран. Използвай: make llvm\n");
        return 1;
    }
#endif

    /* codegen */
    if (emit_c) {
        Codegen cg;
        cg.test_specs = test_specs;
        codegen_c(&cg, program, stdout);
    } else {
        /* generate C to temp file, compile, run */
        char c_path[512];
        char bin_path[512];
        snprintf(c_path, sizeof(c_path), "/tmp/baga_%d.c", getpid());
        snprintf(bin_path, sizeof(bin_path), "/tmp/baga_%d", getpid());

        FILE *cf = fopen(c_path, "w");
        if (!cf) {
            fprintf(stderr, "baga: не мога да създам '%s'\n", c_path);
            return 1;
        }
        Codegen cg;
        cg.test_specs = test_specs;
        codegen_c(&cg, program, cf);
        fclose(cf);

        /* compile */
        char cmd[1200];
        snprintf(cmd, sizeof(cmd), "gcc -O2 -o %s %s -lm 2>&1", bin_path, c_path);
        int ret = system(cmd);
        if (ret != 0) {
            fprintf(stderr, "baga: компилацията на C кода се провали\n");
            fprintf(stderr, "baga: C кодът е в %s\n", c_path);
            return 1;
        }

        /* run */
        ret = system(bin_path);

        /* cleanup */
        remove(c_path);
        remove(bin_path);

        /* propagate the program's exit code */
        if (ret == -1) return 1;
        if (WIFEXITED(ret)) return WEXITSTATUS(ret);
        return 1;
    }

    /* cleanup */
    node_free(program);
    for (int i = 0; i < tokens.len; i++)
        free(tokens.data[i].text);
    vec_free(tokens);
    free(src);

    return 0;
}
