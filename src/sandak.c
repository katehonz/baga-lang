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
    char src_git[512];   /* git URL, ако пакетът идва от git dep; "" иначе */
    char src_ref[160];   /* "<ref_kind>:<ref>" за git deps; "" иначе */
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
            check_safe("име на зависимост", key);   /* името влиза в shell командите на fetch_git */
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

typedef struct { Manifest pkgs[128]; int n; } Graph;   /* pkgs[0] = root */

static void canon(const char *path, char out[512]) {
    /* realpath(path, NULL) — POSIX изисква PATH_MAX буфер при realpath(path, buf) */
    char *rp = realpath(path, NULL);
    if (!rp) die("не мога да намеря '%s': %s", path, strerror(errno));
    if (snprintf(out, 512, "%s", rp) >= 512) { free(rp); die("пътят е твърде дълъг: '%s'", path); }
    free(rp);
}

static const char *base_name(const char *dir) {
    const char *s = strrchr(dir, '/');
    return s ? s + 1 : dir;
}

static void run(const char *cmd) {
    int rc = system(cmd);
    if (rc != 0) die("командата се провали (%d): %s", rc, cmd);
}

static int is_dir(const char *p) {
    struct stat st;
    return stat(p, &st) == 0 && S_ISDIR(st.st_mode);
}

/* клонира (веднъж) в .sandak/cache/<name>-<ref> и връща корена на пакета */
static void fetch_git(const Dep *d, char out_root[512]) {
    char dir[512], cmd[4096];
    if (!is_dir(".sandak/cache"))
        run("mkdir -p '.sandak/cache'");
    snprintf(dir, sizeof dir, ".sandak/cache/%s-%s", d->name, d->ref);
    if (!is_dir(dir)) {
        if (strcmp(d->ref_kind, "rev") == 0) {
            /* произволен commit: init + fetch --depth 1 + checkout;
             * при провал чистим dir-а, иначе is_dir guard-ът спира retry завинаги */
            snprintf(cmd, sizeof cmd,
                "git init -q '%s' && git -C '%s' remote add origin '%s' && "
                "git -C '%s' fetch -q --depth 1 origin '%s' && "
                "git -C '%s' checkout -q FETCH_HEAD || { rm -rf '%s'; false; }",
                dir, dir, d->git, dir, d->ref, dir, dir);
        } else {
            snprintf(cmd, sizeof cmd,
                "git clone -q --depth 1 --branch '%s' '%s' '%s'",
                d->ref, d->git, dir);
        }
        run(cmd);
    }
    /* symlink <name> -> <name>-<ref>: иначе import "<name>/<file>" не се
     * резолвира през -I .sandak/cache (клонираната директория е <name>-<ref>);
     * -sfn, за да следи смяна на ref при повторен fetch */
    snprintf(cmd, sizeof cmd, "ln -sfn '%s-%s' '.sandak/cache/%s'", d->name, d->ref, d->name);
    run(cmd);
    char tmp[1024];
    if (d->subdir[0]) snprintf(tmp, sizeof tmp, "%s/%s", dir, d->subdir);
    else snprintf(tmp, sizeof tmp, "%s", dir);
    canon(tmp, out_root);
}

/* dep -> корен на пакета. За path deps: <manifest_dir>/<path>. За git: кеш + clone. */
static void dep_root(const Manifest *parent, const Dep *d, char out[512]) {
    char tmp[1024];
    if (d->path[0]) {
        snprintf(tmp, sizeof tmp, "%s/%s", parent->dir, d->path);
        canon(tmp, out);
    } else {
        fetch_git(d, out);
    }
}

static void resolve_into(Graph *g, const char *dir, char stack[][512], int depth,
                         const Dep *from) {
    if (depth >= 64) die("твърде дълбок граф на зависимостите");
    for (int i = 0; i < depth; i++)
        if (strcmp(stack[i], dir) == 0)
            die("цикъл в зависимостите при '%s' (път: %s -> ...)", base_name(dir), base_name(stack[i]));

    char mpath[1024];
    snprintf(mpath, sizeof mpath, "%s/sandak.toml", dir);
    if (access(mpath, R_OK) != 0)
        die("липсва манифест: %s", mpath);

    if (g->n >= 128) die("твърде много пакети (max 128)");
    Manifest *m = &g->pkgs[g->n];
    parse_manifest(mpath, m);
    snprintf(m->dir, 512, "%s", dir);
    if (from && from->git[0]) {
        snprintf(m->src_git, sizeof m->src_git, "%s", from->git);
        snprintf(m->src_ref, sizeof m->src_ref, "%s:%s", from->ref_kind, from->ref);
    }
    /* проверката dir == name: за git deps без subdir клонираната директория е
     * <name>-<ref>, затова там я пропускаме; при subdir basename трябва да е
     * името на пакета, както при path deps */
    if (!from || !from->git[0] || from->subdir[0]) {
        if (strcmp(base_name(dir), m->name) != 0)
            die("директория '%s' не съвпада с името на пакета '%s'", base_name(dir), m->name);
    }
    for (int i = 0; i < g->n; i++)
        if (strcmp(g->pkgs[i].name, m->name) == 0)
            die("дублирано име на пакет '%s' (%s и %s)", m->name, g->pkgs[i].dir, dir);
    g->n++;

    snprintf(stack[depth], 512, "%s", dir);
    for (int i = 0; i < m->n_deps; i++) {
        char root[512];
        dep_root(m, &m->deps[i], root);
        /* обратен ръб към връх от текущия път — цикъл (проверява се ПРЕДИ diamond-skip) */
        for (int j = 0; j <= depth; j++)
            if (strcmp(stack[j], root) == 0)
                die("цикъл в зависимостите при '%s' (път: %s -> ...)", base_name(root), base_name(stack[j]));
        /* вече резолвиран пакет (diamond) — пропускай */
        int seen = 0;
        for (int j = 0; j < g->n; j++)
            if (strcmp(g->pkgs[j].dir, root) == 0) { seen = 1; break; }
        if (!seen) resolve_into(g, root, stack, depth + 1, &m->deps[i]);
    }
}

static void resolve(Graph *g, const char *root_dir) {
    memset(g, 0, sizeof *g);
    char canon_root[512];
    canon(root_dir, canon_root);
    static char stack[64][512];
    resolve_into(g, canon_root, stack, 0, NULL);
}

/* Lock запис: подмножество на манифест — само идентичността на пакета. */
typedef struct {
    char name[128]; char version[64];
    char source[600];   /* "path+<abs>" или "git+<url>" */
    char rev[160];      /* "<ref_kind>:<ref>" за git; "-" за path */
} LockPkg;

typedef struct { LockPkg pkgs[128]; int n; } Lock;

static int lockpkg_cmp(const void *a, const void *b) {
    return strcmp(((const LockPkg *)a)->name, ((const LockPkg *)b)->name);
}

static void write_lock(const Graph *g, const char *root_dir) {
    char lpath[1024];
    snprintf(lpath, sizeof lpath, "%s/sandak.lock", root_dir);
    FILE *f = fopen(lpath, "w");
    if (!f) die("не мога да пиша '%s': %s", lpath, strerror(errno));
    Lock lk; lk.n = 0;
    for (int i = 0; i < g->n; i++) {
        LockPkg *p = &lk.pkgs[lk.n++];
        snprintf(p->name, sizeof p->name, "%s", g->pkgs[i].name);
        snprintf(p->version, sizeof p->version, "%s", g->pkgs[i].version);
        /* git deps → source/rev от Dep-а (src_git/src_ref); root и path deps → path+ */
        if (g->pkgs[i].src_git[0]) {
            snprintf(p->source, sizeof p->source, "git+%s", g->pkgs[i].src_git);
            snprintf(p->rev, sizeof p->rev, "%.*s", (int)sizeof p->rev - 1, g->pkgs[i].src_ref);
        } else {
            snprintf(p->source, sizeof p->source, "path+%s", g->pkgs[i].dir);
            snprintf(p->rev, sizeof p->rev, "-");
        }
    }
    qsort(lk.pkgs, lk.n, sizeof(LockPkg), lockpkg_cmp);
    fprintf(f, "# sandak.lock — генериран от sandak, не редактирай\n");
    for (int i = 0; i < lk.n; i++) {
        fprintf(f, "\n[[package]]\nname = %c%s%c\nversion = %c%s%c\nsource = %c%s%c\nrev = %c%s%c\n",
                '"', lk.pkgs[i].name, '"', '"', lk.pkgs[i].version, '"',
                '"', lk.pkgs[i].source, '"', '"', lk.pkgs[i].rev, '"');
    }
    fclose(f);
}

/* чете sandak.lock в Lock; die ако липсва или е счупен */
static void read_lock(const char *root_dir, Lock *lk) {
    char lpath[1024];
    snprintf(lpath, sizeof lpath, "%s/sandak.lock", root_dir);
    char *src = read_file(lpath);   /* die при липса — съобщението е достатъчно */
    memset(lk, 0, sizeof *lk);
    int in_pkg = 0, lineno = 0;
    char *save_line = NULL;   /* strtok_r навсякъде — без споделено състояние */
    for (char *line = strtok_r(src, "\n", &save_line); line; line = strtok_r(NULL, "\n", &save_line)) {
        lineno++;
        if (line[0] && line[strlen(line) - 1] == '\r') line[strlen(line) - 1] = '\0';
        strip_comment(line);
        char *s = trim(line);
        if (!*s) continue;
        if (strcmp(s, "[[package]]") == 0) {
            if (lk->n >= 128) die("%s: твърде много пакети", lpath);
            memset(&lk->pkgs[lk->n], 0, sizeof(LockPkg));
            lk->n++; in_pkg = 1; continue;
        }
        char *eq = strchr(s, '=');
        if (!eq || !in_pkg) die("%s:%d: очаквах [[package]] блок", lpath, lineno);
        *eq = '\0';
        char key[64], val[600];
        snprintf(key, sizeof key, "%s", trim(s));
        parse_string(eq + 1, val, sizeof val);
        LockPkg *p = &lk->pkgs[lk->n - 1];
        if (strcmp(key, "name") == 0)         snprintf(p->name, sizeof p->name, "%.*s", (int)sizeof p->name - 1, val);
        else if (strcmp(key, "version") == 0) snprintf(p->version, sizeof p->version, "%.*s", (int)sizeof p->version - 1, val);
        else if (strcmp(key, "source") == 0)  snprintf(p->source, sizeof p->source, "%s", val);
        else if (strcmp(key, "rev") == 0)     snprintf(p->rev, sizeof p->rev, "%.*s", (int)sizeof p->rev - 1, val);
        else die("%s:%d: непознато поле '%s'", lpath, lineno, key);
    }
    free(src);
}

static void check_locked(const Graph *g, const char *root_dir) {
    Lock lk;
    read_lock(root_dir, &lk);
    if (lk.n != g->n)
        die("sandak.lock е остарял: %d пакета в lock, %d в графа — пусни sandak fetch без --locked", lk.n, g->n);
    for (int i = 0; i < g->n; i++) {
        const Manifest *m = &g->pkgs[i];
        int found = 0;
        for (int j = 0; j < lk.n; j++) {
            if (strcmp(lk.pkgs[j].name, m->name) != 0) continue;
            found = 1;
            if (strcmp(lk.pkgs[j].version, m->version) != 0)
                die("sandak.lock е остарял: %s version %s (lock: %s)",
                    m->name, m->version, lk.pkgs[j].version);
            /* source/rev трябва да съвпадат и при path→git смяна със същите name/version */
            char want_source[600], want_rev[160];
            if (m->src_git[0]) {
                snprintf(want_source, sizeof want_source, "git+%s", m->src_git);
                snprintf(want_rev, sizeof want_rev, "%s", m->src_ref);
            } else {
                snprintf(want_source, sizeof want_source, "path+%s", m->dir);
                snprintf(want_rev, sizeof want_rev, "-");
            }
            if (strcmp(lk.pkgs[j].source, want_source) != 0)
                die("sandak.lock е остарял: %s source/rev се различава — source %s (lock: %s)",
                    m->name, want_source, lk.pkgs[j].source);
            if (strcmp(lk.pkgs[j].rev, want_rev) != 0)
                die("sandak.lock е остарял: %s source/rev се различава — rev %s (lock: %s)",
                    m->name, want_rev, lk.pkgs[j].rev);
            break;
        }
        if (!found)
            die("sandak.lock е остарял: липсва пакет '%s' — пусни sandak fetch", m->name);
    }
}

static int opt_locked = 0;

static void cmd_fetch(void) {
    static Graph g;   /* ~15MB — не на стека */
    resolve(&g, ".");
    if (opt_locked) check_locked(&g, ".");
    else write_lock(&g, ".");
    for (int i = 0; i < g.n; i++)
        printf("resolved: %s %s\n", g.pkgs[i].name, g.pkgs[i].version);
}

/* -I флагове: родителската директория на всеки не-root пакет, dedup. */
static void include_flags(const Graph *g, char out[8192]) {
    char seen[128][512]; int n_seen = 0;
    out[0] = '\0';
    for (int i = 1; i < g->n; i++) {
        char parent[512];
        snprintf(parent, sizeof parent, "%s", g->pkgs[i].dir);
        char *s = strrchr(parent, '/');
        if (s) *s = '\0'; else snprintf(parent, sizeof parent, ".");
        int dup = 0;
        for (int j = 0; j < n_seen; j++)
            if (strcmp(seen[j], parent) == 0) { dup = 1; break; }
        if (dup) continue;
        snprintf(seen[n_seen++], 512, "%s", parent);
        strncat(out, " -I '", 8192 - strlen(out) - 1);
        strncat(out, parent, 8192 - strlen(out) - 1);
        strncat(out, "'", 8192 - strlen(out) - 1);
    }
}

static void cmd_build(int run_after, int argc, char **argv) {
    static Graph g;   /* ~15MB — не на стека */
    resolve(&g, ".");
    if (opt_locked) check_locked(&g, ".");
    else write_lock(&g, ".");

    Manifest *root = &g.pkgs[0];
    char inc[8192];
    include_flags(&g, inc);

    const char *baga = getenv("BAGA");
    if (!baga) baga = "baga";

    char cmd[8192], entry[1024];
    snprintf(entry, sizeof entry, "%s/%s", root->dir, root->entry);

    if (root->is_lib) {
        if (!root->entry[0]) { printf("sandak: %s — нищо за билдване (lib без entry)\n", root->name); return; }
        snprintf(cmd, sizeof cmd, "'%s'%s --lib '%s'", baga, inc, entry);
        run(cmd);
        return;
    }

    snprintf(cmd, sizeof cmd, "mkdir -p target");
    run(cmd);
    char cfile[1024], bin[1024];
    snprintf(cfile, sizeof cfile, "target/%s.c", root->name);
    snprintf(bin, sizeof bin, "target/%s", root->name);
    snprintf(cmd, sizeof cmd, "'%s'%s --emit-c '%s' > '%s'", baga, inc, entry, cfile);
    run(cmd);
    snprintf(cmd, sizeof cmd, "gcc -O2 -std=c11 -o '%s' '%s' -lm -pthread", bin, cfile);
    run(cmd);
    /* в stderr, за да не замърсява stdout на `sandak run` (както cargo) */
    fprintf(stderr, "sandak: %s -> %s\n", root->name, bin);

    if (run_after) {
        /* exec с аргументите след `--` */
        char r[8192];
        snprintf(r, sizeof r, "'%s'", bin);
        for (int i = 0; i < argc; i++) {
            strncat(r, " '", sizeof r - strlen(r) - 1);
            strncat(r, argv[i], sizeof r - strlen(r) - 1);
            strncat(r, "'", sizeof r - strlen(r) - 1);
        }
        int rc = system(r);
        if (rc == -1 || !WIFEXITED(rc)) exit(1);
        exit(WEXITSTATUS(rc));
    }
}

int main(int argc, char **argv) {
    if (argc < 2) { usage(); return 1; }
    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "--") == 0) break;   /* args за run — не са опции на sandak */
        if (strcmp(argv[i], "--locked") == 0) opt_locked = 1;
    }
    if (strcmp(argv[1], "manifest") == 0) { cmd_manifest(); return 0; }
    if (strcmp(argv[1], "fetch") == 0) { cmd_fetch(); return 0; }
    if (strcmp(argv[1], "build") == 0) { cmd_build(0, 0, NULL); return 0; }
    if (strcmp(argv[1], "run") == 0) {
        int sep = argc;
        for (int i = 2; i < argc; i++)
            if (strcmp(argv[i], "--") == 0) { sep = i; break; }
        cmd_build(1, argc - sep - 1, argv + sep + 1);
        return 0;
    }
    usage();
    return 1;
}
