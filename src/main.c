/* glibc declares realpath() under _XOPEN_SOURCE (>= 500); _POSIX_C_SOURCE
 * alone (from baga.h) is not enough. Must precede all system includes. */
#define _XOPEN_SOURCE 700

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
        "Бага — компилатор %s\n"
        "\n"
        "Употреба: baga [опции] <файл.baga>\n"
        "\n"
        "Опции:\n"
        "  --check     Само parse + typecheck (без main, без codegen) — за библиотеки\n"
        "  --emit-c    Генерирай C код на stdout, не компилирай\n"
        "  --test-specs  Property-based тестване на ensures/requires договорите\n"
        "  --rc        Refcount паметов модел v0.1 (opt-in, само C backend)\n"
        "  --verify    Статична верификация на requires/ensures (M0–M13 fragment)\n"
        "  --json      Машинно-четим JSON изход (с --verify)\n"
        "  -I <dir>    Директория за търсене на import (повтаряем)\n"
        "  --ast       Изпечатвай AST (debug)\n"
        "  --tokens    Изпечатвай токени (debug)\n"
        "  --version, -V  Версия на компилатора\n"
        "  --help, -h  Тази помощ\n"
        "\n"
        "По подразбиране: генерира C, компилира с gcc, изпълнява.\n",
        BAGA_VERSION
    );
}

static void print_version(void) {
    printf("baga %s\n", BAGA_VERSION);
}

/* named type for the string vectors used by import expansion — each
 * VEC(T) expansion is a distinct anonymous type, so repeated VEC(char *)
 * declarations would be incompatible */
typedef VEC(char *) StrVec;
typedef VEC(Token) TokenVec;

/* -I <dir> search paths за import, в реда на подаване (попълва се от argv) */
static StrVec include_dirs = {0};

/* import expansion: recursively collect tokens, resolving import "path"
 * directives at the top of each file. The include guard is keyed on the
 * canonical path, so importing the same file twice includes it once.
 * Cycles are an error. */
static void collect_tokens(const char *path, TokenVec *out,
                           StrVec *included, StrVec *stack) {
    char *canon = realpath(path, NULL);
    if (!canon) {
        fprintf(stderr, "baga: не мога да отворя '%s': %s\n", path, strerror(errno));
        exit(1);
    }
    for (int i = 0; i < stack->len; i++) {
        if (strcmp(stack->data[i], canon) == 0) {
            fprintf(stderr, "baga: цикличен import: '%s'\n", path);
            exit(1);
        }
    }
    for (int i = 0; i < included->len; i++) {
        if (strcmp(included->data[i], canon) == 0) {
            free(canon);
            return;   /* already included — include guard */
        }
    }
    vec_push(*stack, canon);

    int src_len = 0;
    char *src = read_file(path, &src_len);
    Lexer lexer;
    /* canon (не path!): 'path' за импортите е stack буферът `resolved`,
     * който умира — токените пазят filename за origin на модулите (L6);
     * canon е собственост на include guard-а и живее до края */
    lexer_init(&lexer, src, src_len, canon);
    VEC(Token) ftoks = {0};
    for (;;) {
        Token t = lexer_next(&lexer);
        vec_push(ftoks, t);
        if (t.kind == TOK_EOF) break;
        if (t.kind == TOK_ERROR) {
            baga_error(path, t.pos, "%s", t.text);
            exit(1);
        }
    }

    /* directory of the importing file, for relative resolution */
    char dir[512];
    snprintf(dir, sizeof dir, "%s", path);
    char *slash = strrchr(dir, '/');
    if (slash) *slash = '\0';
    else snprintf(dir, sizeof dir, ".");

    int seen_code = 0;
    for (int i = 0; i < ftoks.len; i++) {
        Token *t = &ftoks.data[i];
        if (t->kind == TOK_EOF) break;
        if (t->kind == TOK_IMPORT) {
            if (seen_code) {
                baga_error(path, t->pos, "import трябва да е в началото на файла");
                exit(1);
            }
            if (i + 1 >= ftoks.len || ftoks.data[i + 1].kind != TOK_STR_LIT) {
                baga_error(path, t->pos, "очаквах низ след import");
                exit(1);
            }
            const char *rel = ftoks.data[i + 1].text;
            char joined[1024];
            char resolved[1024];
            snprintf(joined, sizeof joined, "%s/%s", dir, rel);
            int found = realpath(joined, resolved) != NULL;
            for (int k = 0; !found && k < include_dirs.len; k++) {
                snprintf(joined, sizeof joined, "%s/%s", include_dirs.data[k], rel);
                found = realpath(joined, resolved) != NULL;
            }
            if (!found && realpath(rel, resolved) == NULL) {
                baga_error(path, t->pos, "не мога да намеря import '%s'", rel);
                exit(1);
            }
            /* L6: `import "p" as a` — alias става модулното име на файла
             * (origin), така че еднакви basename-и в различни директории
             * могат да се уточнят: a.f(). Регистрира се и при повторен
             * import (include guard-ът е отделен). */
            if (i + 3 < ftoks.len && ftoks.data[i + 2].kind == TOK_AS) {
                if (ftoks.data[i + 3].kind != TOK_IDENT) {
                    baga_error(path, ftoks.data[i + 2].pos,
                               "очаквах име на alias след 'as'");
                    exit(1);
                }
                const char *prev =
                    baga_note_import_alias(resolved, ftoks.data[i + 3].text);
                if (prev) {
                    baga_error(path, ftoks.data[i + 3].pos,
                               "файлът вече има import alias '%s'", prev);
                    exit(1);
                }
                i += 2;
            }
            collect_tokens(resolved, out, included, stack);
            i++;   /* consume the path string token */
            continue;
        }
        seen_code = 1;
        vec_push(*out, *t);   /* move the token into the combined stream */
    }

    vec_push(*included, canon);   /* canon stays owned by the guard set */
    stack->len--;
    vec_free(ftoks);              /* token structs' text moved or leaked-by-design */
    free(src);
}

int main(int argc, char **argv) {
    const char *input_path = NULL;
    int emit_c = 0;
    int rc = 0;
    int dump_ast = 0;
    int dump_tokens = 0;
    int dump_specs = 0;
    int dump_proofs = 0;
    int emit_llvm = 0;
    int test_specs = 0;
    int verify = 0;
    int check_only = 0;

    /* програмни аргументи: всичко след input файла (или след '--')
     * отива в arg()/arg_count() на компилираната програма */
    StrVec prog_args = {0};
    int seen_input = 0;
    int dashdash = 0;

    for (int i = 1; i < argc; i++) {
        if (dashdash) { vec_push(prog_args, argv[i]); continue; }
        if (strcmp(argv[i], "--") == 0) { dashdash = 1; continue; }
        if (strcmp(argv[i], "--emit-c") == 0) { emit_c = 1; }
        else if (strcmp(argv[i], "--rc") == 0) { rc = 1; }
        else if (strcmp(argv[i], "--check") == 0 || strcmp(argv[i], "--lib") == 0) {
            check_only = 1;
        }
        else if (strcmp(argv[i], "--ast") == 0) { dump_ast = 1; }
        else if (strcmp(argv[i], "--tokens") == 0) { dump_tokens = 1; }
        else if (strcmp(argv[i], "--specs") == 0) { dump_specs = 1; }
        else if (strcmp(argv[i], "--proofs") == 0) { dump_proofs = 1; }
        else if (strcmp(argv[i], "--emit-llvm") == 0) { emit_llvm = 1; }
        else if (strcmp(argv[i], "--test-specs") == 0) { test_specs = 1; }
        else if (strcmp(argv[i], "--verify") == 0) { verify = 1; }
        else if (strcmp(argv[i], "--json") == 0) { verify_set_json(1); }
        else if (strcmp(argv[i], "--version") == 0 || strcmp(argv[i], "-V") == 0) {
            print_version();
            return 0;
        }
        else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            usage();
            return 0;
        }
        else if (strcmp(argv[i], "-I") == 0) {
            if (++i >= argc) {
                fprintf(stderr, "baga: -I очаква директория\n");
                return 1;
            }
            vec_push(include_dirs, argv[i]);
        }
        else if (strncmp(argv[i], "-I", 2) == 0 && argv[i][2] != '\0') {
            vec_push(include_dirs, argv[i] + 2);
        }
        else if (argv[i][0] == '-' && !seen_input) {
            fprintf(stderr, "baga: непозната опция '%s'\n", argv[i]);
            usage();
            return 1;
        }
        else {
            if (!seen_input) {
                input_path = argv[i];
                seen_input = 1;
            } else {
                vec_push(prog_args, argv[i]);
            }
        }
    }

    if (!input_path) {
        usage();
        return 1;
    }

    /* read source(s) — import expansion happens here */
    TokenVec tokens = {0};
    StrVec included = {0};
    StrVec stack = {0};
    collect_tokens(input_path, &tokens, &included, &stack);

    Token eof;
    memset(&eof, 0, sizeof eof);
    eof.kind = TOK_EOF;
    eof.pos.line = 1;
    eof.pos.col = 1;
    eof.pos.file = input_path;
    eof.text = strdup("");
    vec_push(tokens, eof);

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
    /* libraries: no main for --check/--lib, --emit-c, --emit-llvm */
    checker.allow_no_main = check_only || emit_c || emit_llvm;
    check_program(&checker, program);

    if (checker.n_errors > 0) {
        for (int i = 0; i < checker.n_errors; i++)
            fprintf(stderr, "%s: %s\n", input_path, checker.errors[i]);
        return 1;
    }

    if (check_only) {
        printf("ok: %s\n", input_path);
        return 0;
    }

    if (verify) {
        return verify_program(program);
    }

#ifdef BAGA_LLVM
    if (emit_llvm) {
        codegen_llvm(program, NULL, &checker);
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
        cg.rc = rc;
        cg.chk = &checker;
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
        cg.rc = rc;
        cg.chk = &checker;
        codegen_c(&cg, program, cf);
        fclose(cf);

        /* compile
         * Optional env for native shims (wasmtimebaga and similar):
         *   BAGA_CFLAGS   extra gcc flags when compiling generated C
         *   BAGA_LDFLAGS  extra link flags (-L/-l/-Wl,-rpath,…)
         *   BAGA_EXTRA_OBJS  space-separated .o/.c files to link
         * -pthread: std concurrency (go/join/chan); harmless when unused.
         */
        int ret;
        {
            const char *extra_c = getenv("BAGA_CFLAGS");
            const char *extra_ld = getenv("BAGA_LDFLAGS");
            const char *extra_objs = getenv("BAGA_EXTRA_OBJS");
            if (!extra_c) extra_c = "";
            if (!extra_ld) extra_ld = "";
            if (!extra_objs) extra_objs = "";
            char cmd[4096];
            snprintf(cmd, sizeof(cmd),
                     "gcc -O2 %s -o %s %s %s -lm -pthread %s 2>&1",
                     extra_c, bin_path, c_path, extra_objs, extra_ld);
            ret = system(cmd);
            if (ret != 0) {
                fprintf(stderr, "baga: компилацията на C кода се провали\n");
                fprintf(stderr, "baga: C кодът е в %s\n", c_path);
                return 1;
            }
        }

        /* run — програмните аргументи (след input файла / '--') се подават
         * на бинарника; arg(i) ги чете без argv[0] */
        char run_cmd[8192];
        int off = snprintf(run_cmd, sizeof run_cmd, "'%s'", bin_path);
        for (int i = 0; i < prog_args.len && off < (int)sizeof run_cmd - 8; i++) {
            const char *a = prog_args.data[i];
            run_cmd[off++] = ' ';
            run_cmd[off++] = '\'';
            for (const char *p = a; *p && off < (int)sizeof run_cmd - 5; p++) {
                if (*p == '\'') {
                    run_cmd[off++] = '\''; run_cmd[off++] = '\\';
                    run_cmd[off++] = '\''; run_cmd[off++] = '\'';
                } else {
                    run_cmd[off++] = *p;
                }
            }
            run_cmd[off++] = '\'';
            run_cmd[off] = 0;
        }
        ret = system(run_cmd);

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
    for (int i = 0; i < included.len; i++) free(included.data[i]);
    vec_free(included);
    vec_free(stack);

    return 0;
}
