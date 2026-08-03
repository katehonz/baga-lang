/* sandak — пакетен мениджър за Baga: manifest, lock, fetch, build.
 * Само libc; git/gcc/baga се викат като външни процеси. */
#define _XOPEN_SOURCE 700
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <errno.h>
#include <ctype.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/wait.h>

typedef struct {
    char name[128]; char path[512];
    char git[512];
    char ref_kind[8]; char ref[128]; char subdir[512];
} Dep;

typedef struct {
    char name[128]; char version[64]; char entry[512];
    int is_lib;
    Dep deps[64]; int n_deps;
    char dir[512];
} Manifest;

static void die(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    fprintf(stderr, "sandak: ");
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    va_end(ap);
    exit(1);
}

static void *xmalloc(size_t n) {
    void *p = malloc(n);
    if (!p) die("out of memory");
    return p;
}

static char *read_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) die("не мога да отворя '%s': %s", path, strerror(errno));
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    char *buf = xmalloc((size_t)sz + 1);
    size_t rd = fread(buf, 1, (size_t)sz, f);
    buf[rd] = '\0'; fclose(f);
    return buf;
}

/* отхвърля стойности, опасни за shell quoting */
static void check_safe(const char *what, const char *v) {
    if (strchr(v, '\'') || strchr(v, '"') || strchr(v, '`') || strchr(v, '$'))
        die("%s съдържа забранен символ: '%s'", what, v);
}

/* мини-TOML: [table], key = "value", inline { k = v, ... }, # коментари.
 * Не е общ TOML парсер — само схемата на sandak.toml. */
static char *trim(char *s) {
    while (isspace((unsigned char)*s)) s++;
    char *e = s + strlen(s);
    while (e > s && isspace((unsigned char)e[-1])) *--e = '\0';
    return s;
}

static void strip_comment(char *s) {
    int in_str = 0;
    for (; *s; s++) {
        if (*s == '"') in_str = !in_str;
        else if (*s == '#' && !in_str) { *s = '\0'; return; }
    }
}

static char *parse_string(const char *v, char *out, size_t n) {
    v = trim((char *)v);
    if (v[0] != '"' || strlen(v) < 2 || v[strlen(v) - 1] != '"')
        die("очаквах низ в кавички, получих: %s", v);
    snprintf(out, n, "%.*s", (int)strlen(v) - 2, v + 1);
    return out;
}

/* попълва Dep от inline table: { path = "...", git = "...", rev = "..." } */
static void parse_dep_inline(const char *tbl, Dep *d) {
    char buf[1024];
    snprintf(buf, sizeof buf, "%s", tbl);
    char *s = trim(buf);
    if (s[0] != '{' || s[strlen(s) - 1] != '}')
        die("зависимост '%s': очаквах inline table { ... }", d->name);
    s[strlen(s) - 1] = '\0';
    s++;
    char *save_tok;
    for (char *tok = strtok_r(s, ",", &save_tok); tok; tok = strtok_r(NULL, ",", &save_tok)) {
        char *eq = strchr(tok, '=');
        if (!eq) die("зависимост '%s': очаквах key = value в '%s'", d->name, tok);
        *eq = '\0';
        char key[64], val[512];
        snprintf(key, sizeof key, "%s", trim(tok));
        parse_string(eq + 1, val, sizeof val);
        if (strcmp(key, "path") == 0)        { snprintf(d->path, sizeof d->path, "%s", val); check_safe("path", val); }
        else if (strcmp(key, "git") == 0)    { snprintf(d->git, sizeof d->git, "%s", val); check_safe("git", val); }
        else if (strcmp(key, "rev") == 0)    { snprintf(d->ref_kind, sizeof d->ref_kind, "rev"); snprintf(d->ref, sizeof d->ref, "%.*s", (int)sizeof d->ref - 1, val); check_safe("rev", val); }
        else if (strcmp(key, "tag") == 0)    { snprintf(d->ref_kind, sizeof d->ref_kind, "tag"); snprintf(d->ref, sizeof d->ref, "%.*s", (int)sizeof d->ref - 1, val); check_safe("tag", val); }
        else if (strcmp(key, "branch") == 0) { snprintf(d->ref_kind, sizeof d->ref_kind, "branch"); snprintf(d->ref, sizeof d->ref, "%.*s", (int)sizeof d->ref - 1, val); check_safe("branch", val); }
        else if (strcmp(key, "subdir") == 0) { snprintf(d->subdir, sizeof d->subdir, "%s", val); check_safe("subdir", val); }
        else die("зависимост '%s': непознато поле '%s'", d->name, key);
    }
    if (d->path[0] && d->git[0])
        die("зависимост '%s': path и git са взаимно изключващи се", d->name);
    if (!d->path[0] && !d->git[0])
        die("зависимост '%s': нужно е path или git", d->name);
    if (d->path[0] && (d->ref[0] || d->subdir[0]))
        die("зависимост '%s': rev/tag/branch/subdir важат само за git", d->name);
    if (d->git[0] && !d->ref[0])
        snprintf(d->ref_kind, sizeof d->ref_kind, "branch"), snprintf(d->ref, sizeof d->ref, "main");
}

void parse_manifest(const char *path, Manifest *m) {
    memset(m, 0, sizeof *m);
    m->is_lib = 1;
    char *src = read_file(path);
    int section = 0;   /* 0=none, 1=package, 2=dependencies */
    int lineno = 0;
    char *save_line;   /* strtok_r — без споделено състояние с parse_dep_inline */
    for (char *line = strtok_r(src, "\n", &save_line); line; line = strtok_r(NULL, "\n", &save_line)) {
        lineno++;
        if (line[0] && line[strlen(line) - 1] == '\r') line[strlen(line) - 1] = '\0';
        strip_comment(line);
        char *s = trim(line);
        if (!*s) continue;
        if (*s == '[') {
            if (strcmp(s, "[package]") == 0) section = 1;
            else if (strcmp(s, "[dependencies]") == 0) section = 2;
            else die("%s:%d: непозната секция '%s'", path, lineno, s);
            continue;
        }
        char *eq = strchr(s, '=');
        if (!eq) die("%s:%d: очаквах key = value", path, lineno);
        *eq = '\0';
        char key[128];
        snprintf(key, sizeof key, "%s", trim(s));
        char *val = trim(eq + 1);
        if (section == 1) {
            char sv[512];
            if (strcmp(key, "name") == 0)         snprintf(m->name, sizeof m->name, "%s", parse_string(val, sv, sizeof sv));
            else if (strcmp(key, "version") == 0) snprintf(m->version, sizeof m->version, "%s", parse_string(val, sv, sizeof sv));
            else if (strcmp(key, "entry") == 0)   snprintf(m->entry, sizeof m->entry, "%s", parse_string(val, sv, sizeof sv));
            else if (strcmp(key, "kind") == 0) {
                parse_string(val, sv, sizeof sv);
                if (strcmp(sv, "bin") == 0) m->is_lib = 0;
                else if (strcmp(sv, "lib") != 0) die("%s:%d: kind трябва да е \"bin\" или \"lib\"", path, lineno);
            }
            else die("%s:%d: непознато поле в [package]: '%s'", path, lineno, key);
        } else if (section == 2) {
            if (m->n_deps >= 64) die("%s: твърде много зависимости (max 64)", path);
            Dep *d = &m->deps[m->n_deps++];
            memset(d, 0, sizeof *d);
            snprintf(d->name, sizeof d->name, "%s", key);
            parse_dep_inline(val, d);
        } else {
            die("%s:%d: key извън секция", path, lineno);
        }
    }
    free(src);
    if (!m->name[0]) die("%s: липсва [package] name", path);
    if (!m->version[0]) die("%s: липсва [package] version", path);
    if (!m->is_lib && !m->entry[0]) die("%s: kind = \"bin\" изисква entry", path);
}

static void usage(void) {
    fprintf(stderr,
        "sandak — пакетен мениджър за Бага\n"
        "\n"
        "Употреба: sandak <команда> [--locked]\n"
        "\n"
        "Команди:\n"
        "  manifest   Покажи парснатия sandak.toml (debug)\n"
        "  fetch      Резолвирай и изтегли зависимостите, запиши sandak.lock\n"
        "  build      fetch + компилирай (bin → target/<name>, lib → проверка)\n"
        "  run        build + стартирай бинарника (args след --)\n");
}

static void cmd_manifest(void) {
    Manifest m;
    parse_manifest("sandak.toml", &m);
    printf("name=%s\nversion=%s\n", m.name, m.version);
    if (m.entry[0]) printf("entry=%s\n", m.entry);
    printf("kind=%s\n", m.is_lib ? "lib" : "bin");
    for (int i = 0; i < m.n_deps; i++) {
        Dep *d = &m.deps[i];
        printf("dep=%s %s=%s\n", d->name,
               d->path[0] ? "path" : "git",
               d->path[0] ? d->path : d->git);
    }
}

int main(int argc, char **argv) {
    if (argc < 2) { usage(); return 1; }
    if (strcmp(argv[1], "manifest") == 0) { cmd_manifest(); return 0; }
    usage();
    return 1;
}
