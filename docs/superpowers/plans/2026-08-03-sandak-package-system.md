# Sandak — пакетна система за Baga: имплементационен план

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Пакетна система `sandak` (манифест, path/git зависимости, lock, build) + `-I` флаг в компилатора + пакетиране на монорепото + Docker multi-stage build от git URL.

**Architecture:** `sandak` е отделен C бинарник (`src/sandak.c`), който чете `sandak.toml`, резолвира транзитивно зависимостите (path локално, git в `.sandak/cache/`), пише `sandak.lock` и вика `baga -I <dir>... --emit-c/--lib` + `gcc`. Компилаторът получава само повтаряем `-I` флаг за import search path. Спецификация: `docs/superpowers/specs/2026-08-03-sandak-package-system-design.md`.

**Tech Stack:** C11 (libc only — нула външни зависимости, както целия проект), `git` и `gcc` като външни процеси, bash тестове, Docker.

## Global Constraints

- Само C11 + libc за `sandak`; `git`/`gcc` се викат като процеси. **Никакви** външни библиотеки (няма tomlc99, няма libgit2).
- Компилация с `-O2 -Wall -Wextra -std=c11` — без warnings.
- Всички съобщения за грешки на български, в стила на компилатора (`sandak: <съобщение>`).
- Синтаксисът на езика НЕ се променя; единствената промяна в `baga` е флагът `-I`.
- `make test` трябва да остане зелен след всяка задача.
- Името на директорията на пакет == `name` в манифеста == ключът на зависимостта.
- Git URL/пътища с кавички или интервали се отхвърлят при парсване (защита от shell injection — всички команди се строят с `'...'` quoting).
- Commit след всяка задача; съобщенията на български, в стила на историята (`git log --oneline`).

---

### Task 1: `-I <dir>` флаг в компилатора

**Files:**
- Modify: `src/main.c:57-66` (typedefs), `src/main.c:123-131` (import resolution), `src/main.c:158-187` (arg parsing), `src/main.c:31-51` (usage)
- Test: `tests/i_flag/greeter/greet.baga`, `tests/i_flag/main.baga` (нови), `Makefile` (нов тест блок)

**Interfaces:**
- Consumes: нищо от други задачи.
- Produces: `baga -I <dir> <file.baga>` — повтаряем флаг; import се търси (1) релативно на файла, (2) във всяка `-I` директория по ред, (3) релативно на CWD (legacy fallback), иначе грешка. `sandak` (Task 6) разчита на този флаг.

- [ ] **Step 1: Фикстура + failing тест**

`tests/i_flag/greeter/greet.baga`:
```
fn answer() -> i64 { return 42 }
```
`tests/i_flag/main.baga`:
```
import "greeter/greet.baga"

fn main() {
    print(answer())
}
```
Забележка: `main.baga` импортира `"greeter/greet.baga"` — файл, който НЕ съществува релативно на `tests/i_flag/`... всъщност съществува (`tests/i_flag/greeter/greet.baga`), затова тестът използва `-I tests/i_flag` от repo root, а `main.baga` е в `/tmp`: копирай `main.baga` в `/tmp/baga_i_flag_main.baga` в теста, за да няма релативно съвпадение.

Makefile блок (добавя се в `test:` след секцията `=== import ===`):
```make
	@echo "=== -I include path ==="
	@cp tests/i_flag/main.baga /tmp/baga_i_flag_main.baga
	@./$(BIN) -I tests/i_flag /tmp/baga_i_flag_main.baga > /tmp/baga_i_flag_out.txt \
		&& test "$$(cat /tmp/baga_i_flag_out.txt)" = "42" \
		&& echo "OK: -I include path резолюция" \
		|| { echo "FAIL: -I include path"; exit 1; }
	@./$(BIN) -Itests/i_flag /tmp/baga_i_flag_main.baga > /dev/null \
		&& echo "OK: -I<dir> слепен вариант" \
		|| { echo "FAIL: -I<dir> слепен вариант"; exit 1; }
```

- [ ] **Step 2: Пусни теста — трябва да гръмне**

Run: `make test 2>&1 | tail -5`
Expected: `baga: непозната опция '-I'` → `FAIL: -I include path`.

- [ ] **Step 3: Имплементация в src/main.c**

След ред 61 (`typedef VEC(Token) TokenVec;`) добави:
```c
/* -I <dir> search paths за import, в реда на подаване (попълва се от argv) */
static StrVec include_dirs = {0};
```

В `usage()` след реда за `--json` добави:
```c
        "  -I <dir>    Директория за търсене на import (повтаряем)\n"
```

В arg parsing цикъла, преди `else if (argv[i][0] == '-')`, добави:
```c
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
```

В `collect_tokens`, замени resolution блока (редове 123-131):
```c
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
```

- [ ] **Step 4: Rebuild + пусни теста**

Run: `make && make test 2>&1 | grep -E "OK: -I|FAIL" ; make test 2>&1 | tail -2`
Expected: `OK: -I include path резолюция`, `OK: -I<dir> слепен вариант`, финално `Всички тестове минаха. ⚔️`.

- [ ] **Step 5: Commit**

```bash
git add src/main.c Makefile tests/i_flag
git commit -m "baga: -I <dir> флаг за import search path (sandak T1)"
```

---

### Task 2: sandak скелет + мини-TOML парсер (`sandak manifest`)

**Files:**
- Create: `src/sandak.c`
- Create: `tests/sandak/run_tests.sh`, `tests/sandak/fixtures/basic/sandak.toml`, `tests/sandak/fixtures/basic/greeter.baga`, `tests/sandak/fixtures/bad_toml/sandak.toml`
- Modify: `Makefile` (цел `sandak`, `test` да вика `tests/sandak/run_tests.sh`)
- Modify: `.gitignore` (`sandak`, `.sandak/`, `target/`)

**Interfaces:**
- Consumes: нищо.
- Produces (използва се от Task 3-6):
```c
typedef struct {
    char name[128]; char path[512];  /* path dep, "" ако git */
    char git[512];                   /* git URL, "" ако path */
    char ref_kind[8];                /* "rev" | "tag" | "branch" | "" */
    char ref[128]; char subdir[512]; /* subdir в git repo, "" = root */
} Dep;
typedef struct {
    char name[128]; char version[64]; char entry[512];
    int is_lib;                      /* kind != "bin" (default: lib) */
    Dep deps[64]; int n_deps;
    char dir[512];                   /* канонична директория на манифеста (resolver) */
} Manifest;
static void die(const char *fmt, ...);                       /* stderr + exit(1) */
static void parse_manifest(const char *path, Manifest *m);   /* die при грешка */
```
- CLI: `sandak manifest` — чете `./sandak.toml` и печати парснатото (debug/тест команда).

- [ ] **Step 1: Failing тест харнес + фикстури**

`tests/sandak/fixtures/basic/sandak.toml`:
```toml
# примерен манифест
[package]
name = "greeter"
version = "0.1.0"
entry = "greeter.baga"
kind = "lib"

[dependencies]
std = { path = "../std" }   # коментар след стойност
```
`tests/sandak/fixtures/basic/greeter.baga`: `fn answer() -> i64 { return 42 }`
`tests/sandak/fixtures/bad_toml/sandak.toml`: `[package` (счупен ред)

`tests/sandak/run_tests.sh`:
```bash
#!/bin/bash
# sandak тестове — всяка проверка печати "OK: <име>" или излиза с FAIL
set -u
cd "$(dirname "$0")/../.."
SANDAK=$(realpath "${SANDAK:-./sandak}")
export BAGA=${BAGA:-$(pwd)/baga}
T=$(mktemp -d); trap 'rm -rf "$T"' EXIT
fail() { echo "FAIL: $1"; exit 1; }

# --- T2: манифест парсер ---
out=$(cd tests/sandak/fixtures/basic && "$SANDAK" manifest) \
  || fail "manifest exit"
exp='name=greeter
version=0.1.0
entry=greeter.baga
kind=lib
dep=std path=../std'
[ "$out" = "$exp" ] || fail "manifest изход: [$out]"
echo "OK: манифест парсер"

(cd tests/sandak/fixtures/bad_toml && "$SANDAK" manifest) 2>"$T/err" \
  && fail "bad_toml трябва да гърми"
grep -q "sandak:" "$T/err" || fail "bad_toml без съобщение"
echo "OK: счупен манифест — грешка"

echo "sandak: всички тестове минаха"
```
(`realpath` прави `$SANDAK` абсолютен, така че работи и след `cd` във фикстурите.)

Забележка: тестът предполага `make test` да вика този скрипт. Добави в `Makefile`:
```make
sandak: src/sandak.c
	$(CC) $(CFLAGS) -o $@ $<
```
(`src/sandak.c` е self-contained — не включва `baga.h`.) Добави `sandak` в `.PHONY` ред като реална цел извън `.PHONY` (тя е файл — НЕ я слагай в `.PHONY`). В `test:` промени зависимостта на `test: $(BIN) sandak` и добави блок:
```make
	@echo "=== sandak (пакетен мениджър) ==="
	@SANDAK=$(CURDIR)/sandak BAGA=$(CURDIR)/baga bash tests/sandak/run_tests.sh
```
`.gitignore` — добави редове: `sandak`, `.sandak/`, `target/`.

- [ ] **Step 2: Пусни — гръмва**

Run: `make sandak`
Expected: грешка — `src/sandak.c` не съществува.

- [ ] **Step 3: Имплементация — src/sandak.c**

```c
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
    char *save_tok = NULL;   /* strtok_r: без споделено състояние с line цикъла */
    for (char *tok = strtok_r(s, ",", &save_tok); tok; tok = strtok_r(NULL, ",", &save_tok)) {
        char *eq = strchr(tok, '=');
        if (!eq) die("зависимост '%s': очаквах key = value в '%s'", d->name, tok);
        *eq = '\0';
        char key[64], val[512];
        snprintf(key, sizeof key, "%s", trim(tok));
        parse_string(eq + 1, val, sizeof val);
        if (strcmp(key, "path") == 0)        { snprintf(d->path, sizeof d->path, "%s", val); check_safe("path", val); }
        else if (strcmp(key, "git") == 0)    { snprintf(d->git, sizeof d->git, "%s", val); check_safe("git", val); }
        else if (strcmp(key, "rev") == 0)    { snprintf(d->ref_kind, sizeof d->ref_kind, "rev"); snprintf(d->ref, sizeof d->ref, "%s", val); check_safe("rev", val); }
        else if (strcmp(key, "tag") == 0)    { snprintf(d->ref_kind, sizeof d->ref_kind, "tag"); snprintf(d->ref, sizeof d->ref, "%s", val); check_safe("tag", val); }
        else if (strcmp(key, "branch") == 0) { snprintf(d->ref_kind, sizeof d->ref_kind, "branch"); snprintf(d->ref, sizeof d->ref, "%s", val); check_safe("branch", val); }
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
    char *save_line = NULL;   /* strtok_r: parse_dep_inline има собствен цикъл */
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
```
(Парсерът е ~150 реда и е нарочно минимален — не добавяй полета извън схемата.)

- [ ] **Step 4: Пусни тестовете**

Run: `make sandak && SANDAK=$(pwd)/sandak BAGA=$(pwd)/baga bash tests/sandak/run_tests.sh`
Expected: `OK: манифест парсер`, `OK: счупен манифест — грешка`, `sandak: всички тестове минаха`. После `make test` — целият suite зелен.

- [ ] **Step 5: Commit**

```bash
git add src/sandak.c tests/sandak Makefile .gitignore
git commit -m "sandak: скелет + мини-TOML парсер на манифести (T2)"
```

---

### Task 3: Резолвър на зависимости (`sandak fetch`, path deps)

**Files:**
- Modify: `src/sandak.c`
- Test: `tests/sandak/run_tests.sh` (нови случаи), фикстури се генерират в скрипта (в `$T`)

**Interfaces:**
- Consumes: `Manifest`, `Dep`, `parse_manifest`, `die` от Task 2.
- Produces (за Task 4-6):
```c
typedef struct { Manifest pkgs[128]; int n; } Graph;   /* pkgs[0] = root */
static void resolve(Graph *g, const char *root_dir);   /* пълни g, die при цикъл/дупликация */
```
- CLI: `sandak fetch [--locked]` — resolve + (Task 4: lock). Засега печата `resolved: <name> <version>` за всеки пакет, deps-първо.

- [ ] **Step 1: Failing тест случаи (добави в run_tests.sh преди финалния echo)**

```bash
# --- T3: резолвър ---
mkdir -p "$T/ws/mylib" "$T/ws/myapp"
cat > "$T/ws/mylib/sandak.toml" <<'EOF'
[package]
name = "mylib"
version = "0.1.0"
entry = "mylib.baga"
EOF
echo 'fn hi() -> i64 { return 7 }' > "$T/ws/mylib/mylib.baga"
cat > "$T/ws/myapp/sandak.toml" <<EOF
[package]
name = "myapp"
version = "0.1.0"
entry = "main.baga"
kind = "bin"

[dependencies]
mylib = { path = "../mylib" }
EOF
out=$(cd "$T/ws/myapp" && "$SANDAK" fetch) || fail "fetch exit"
echo "$out" | grep -q "resolved: mylib 0.1.0" || fail "fetch mylib: [$out]"
echo "$out" | grep -q "resolved: myapp 0.1.0" || fail "fetch myapp: [$out]"
echo "OK: резолвър path deps"

# цикъл: a -> b -> a
mkdir -p "$T/cyc/a" "$T/cyc/b"
printf '[package]\nname = "a"\nversion = "0.1.0"\n[dependencies]\nb = { path = "../b" }\n' > "$T/cyc/a/sandak.toml"
printf '[package]\nname = "b"\nversion = "0.1.0"\n[dependencies]\na = { path = "../a" }\n' > "$T/cyc/b/sandak.toml"
(cd "$T/cyc/a" && "$SANDAK" fetch) 2>"$T/err" && fail "цикълът трябва да гърми"
grep -q "цикъл" "$T/err" || fail "цикъл без съобщение: $(cat "$T/err")"
echo "OK: цикъл в зависимостите — грешка"

# дублирано име: два различни пакета с име dup
mkdir -p "$T/dup/x" "$T/dup/y" "$T/dup/root"
printf '[package]\nname = "dup"\nversion = "0.1.0"\n' > "$T/dup/x/sandak.toml"
printf '[package]\nname = "dup"\nversion = "0.2.0"\n' > "$T/dup/y/sandak.toml"
printf '[package]\nname = "root"\nversion = "0.1.0"\n[dependencies]\none = { path = "../x" }\ntwo = { path = "../y" }\n' > "$T/dup/root/sandak.toml"
(cd "$T/dup/root" && "$SANDAK" fetch) 2>"$T/err" && fail "дупликацията трябва да гърми"
grep -q "дублирано име" "$T/err" || fail "дупликация без съобщение: $(cat "$T/err")"
echo "OK: дублирано име на пакет — грешка"
```
Забележка: тестът за дублирано име нарушава и правилото „dir == name" (`x` съдържа `dup`) — resolver-ът трябва да проверява дублирането ПРЕДИ dir-проверката, или тестът да очаква която и да е от двете грешки. Използвай: `grep -qE "дублирано име|не съвпада" "$T/err"`.

- [ ] **Step 2: Пусни — гърми**

Run: `SANDAK=$(pwd)/sandak bash tests/sandak/run_tests.sh`
Expected: `sandak fetch` → `usage` + exit 1 → `FAIL: fetch exit`.

- [ ] **Step 3: Имплементация — добави в src/sandak.c**

```c
typedef struct { Manifest pkgs[128]; int n; } Graph;

static void canon(const char *path, char out[512]) {
    if (!realpath(path, out))
        die("не мога да намеря '%s': %s", path, strerror(errno));
}

static const char *base_name(const char *dir) {
    const char *s = strrchr(dir, '/');
    return s ? s + 1 : dir;
}

/* dep -> корен на пакета. За path deps: <manifest_dir>/<path>. Git: Task 5. */
static void dep_root(const Manifest *parent, const Dep *d, char out[512]) {
    char tmp[1024];
    if (d->path[0]) {
        snprintf(tmp, sizeof tmp, "%s/%s", parent->dir, d->path);
        canon(tmp, out);
    } else {
        die("git зависимости: неоще (T5)");   /* заменя се в Task 5 */
    }
}

static void resolve_into(Graph *g, const char *dir, char stack[][512], int depth) {
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
    if (strcmp(base_name(dir), m->name) != 0)
        die("директория '%s' не съвпада с името на пакета '%s'", base_name(dir), m->name);
    for (int i = 0; i < g->n; i++)
        if (strcmp(g->pkgs[i].name, m->name) == 0)
            die("дублирано име на пакет '%s' (%s и %s)", m->name, g->pkgs[i].dir, dir);
    g->n++;

    snprintf(stack[depth], 512, "%s", dir);
    for (int i = 0; i < m->n_deps; i++) {
        char root[512];
        dep_root(m, &m->deps[i], root);
        /* вече резолвиран пакет (diamond) — пропускай */
        int seen = 0;
        for (int j = 0; j < g->n; j++)
            if (strcmp(g->pkgs[j].dir, root) == 0) { seen = 1; break; }
        if (!seen) resolve_into(g, root, stack, depth + 1);
    }
}

static void resolve(Graph *g, const char *root_dir) {
    memset(g, 0, sizeof *g);
    char canon_root[512];
    canon(root_dir, canon_root);
    static char stack[64][512];
    resolve_into(g, canon_root, stack, 0);
}

static void cmd_fetch(void) {
    Graph g;
    resolve(&g, ".");
    for (int i = 0; i < g.n; i++)
        printf("resolved: %s %s\n", g.pkgs[i].name, g.pkgs[i].version);
}
```
И в `main`: `if (strcmp(argv[1], "fetch") == 0) { cmd_fetch(); return 0; }` (преди `usage()`).

Забележка: pkgs[0] е root защото `resolve_into` добавя манифеста ПРЕДИ да рекурсира. Печатът е root-първо; тестът само grep-ва редове, редът няма значение.

- [ ] **Step 4: Пусни тестовете**

Run: `make sandak && SANDAK=$(pwd)/sandak bash tests/sandak/run_tests.sh`
Expected: всички OK редове за T2+T3, финал `sandak: всички тестове минаха`.

- [ ] **Step 5: Commit**

```bash
git add src/sandak.c tests/sandak/run_tests.sh
git commit -m "sandak: резолвър на зависимости с цикъл/дупликация детекция (T3)"
```

---

### Task 4: `sandak.lock` + `--locked`

**Files:**
- Modify: `src/sandak.c`
- Test: `tests/sandak/run_tests.sh` (нови случаи)

**Interfaces:**
- Consumes: `Graph`, `resolve` от Task 3.
- Produces: `static void write_lock(const Graph *g, const char *root_dir);` и `static void check_locked(const Graph *g, const char *root_dir);` — `cmd_fetch` става: resolve → (ако `--locked`: check_locked; иначе write_lock). Lock форматът (`[[package]]` блокове, сортирани по име) се чете от Task 5/6 чрез общия парсер (разширен с `[[package]]`).

- [ ] **Step 1: Failing тест случаи (преди финалния echo)**

```bash
# --- T4: lock файл ---
(cd "$T/ws/myapp" && "$SANDAK" fetch) > /dev/null || fail "fetch за lock"
[ -f "$T/ws/myapp/sandak.lock" ] || fail "sandak.lock не е създаден"
grep -q 'name = "mylib"' "$T/ws/myapp/sandak.lock" || fail "lock без mylib"
grep -q 'source = "path+' "$T/ws/myapp/sandak.lock" || fail "lock без source"
grep -q 'name = "myapp"' "$T/ws/myapp/sandak.lock" || fail "lock без root пакета"
echo "OK: sandak.lock се записва"

(cd "$T/ws/myapp" && "$SANDAK" fetch --locked) > /dev/null || fail "--locked при съвпадение"
echo "OK: --locked при съвпадение"

# разминаване: сменяме версията на mylib след lock
sed -i 's/0.1.0/0.2.0/' "$T/ws/mylib/sandak.toml"
(cd "$T/ws/myapp" && "$SANDAK" fetch --locked) 2>"$T/err" && fail "--locked трябва да гърми"
grep -q "lock" "$T/err" || fail "--locked без съобщение: $(cat "$T/err")"
sed -i 's/0.2.0/0.1.0/' "$T/ws/mylib/sandak.toml"
echo "OK: --locked при разминаване — грешка"
```

- [ ] **Step 2: Пусни — гърми**

Run: `SANDAK=$(pwd)/sandak bash tests/sandak/run_tests.sh`
Expected: `FAIL: --locked при съвпадение` (непознат флаг) или липсва `sandak.lock`.

- [ ] **Step 3: Имплементация — добави в src/sandak.c**

Разширение на парсера: ред `[[package]]` се приема само в lock режим. Добави функция:

```c
/* Lock запис: подмножество на манифест — само идентичността на пакета. */
typedef struct {
    char name[128]; char version[64];
    char source[600];   /* "path+<abs>" или "git+<url>" */
    char rev[128];      /* за git; "-" за path */
} LockPkg;

typedef struct { LockPkg pkgs[128]; int n; } Lock;

static int lockpkg_cmp(const void *a, const void *b) {
    return strcmp(((const LockPkg *)a)->name, ((const LockPkg *)b)->name);
}

void write_lock(const Graph *g, const char *root_dir) {
    char lpath[1024];
    snprintf(lpath, sizeof lpath, "%s/sandak.lock", root_dir);
    FILE *f = fopen(lpath, "w");
    if (!f) die("не мога да пиша '%s': %s", lpath, strerror(errno));
    Lock lk; lk.n = 0;
    for (int i = 0; i < g->n; i++) {
        LockPkg *p = &lk.pkgs[lk.n++];
        snprintf(p->name, sizeof p->name, "%s", g->pkgs[i].name);
        snprintf(p->version, sizeof p->version, "%s", g->pkgs[i].version);
        /* за git deps (T5) source/rev се взимат от Dep-а; root и path deps → path+ */
        snprintf(p->source, sizeof p->source, "path+%s", g->pkgs[i].dir);
        snprintf(p->rev, sizeof p->rev, "-");
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
        if (strcmp(key, "name") == 0)         snprintf(p->name, sizeof p->name, "%s", val);
        else if (strcmp(key, "version") == 0) snprintf(p->version, sizeof p->version, "%s", val);
        else if (strcmp(key, "source") == 0)  snprintf(p->source, sizeof p->source, "%s", val);
        else if (strcmp(key, "rev") == 0)     snprintf(p->rev, sizeof p->rev, "%s", val);
        else die("%s:%d: непознато поле '%s'", lpath, lineno, key);
    }
    free(src);
}

void check_locked(const Graph *g, const char *root_dir) {
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
            break;
        }
        if (!found)
            die("sandak.lock е остарял: липсва пакет '%s' — пусни sandak fetch", m->name);
    }
}
```

Промени в `main` — глобален `static int opt_locked = 0;` и parsing:
```c
int main(int argc, char **argv) {
    if (argc < 2) { usage(); return 1; }
    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "--locked") == 0) opt_locked = 1;
    }
    ...
}
```
В `cmd_fetch` след `resolve`:
```c
    if (opt_locked) check_locked(&g, ".");
    else write_lock(&g, ".");
```

- [ ] **Step 4: Пусни тестовете**

Run: `make sandak && SANDAK=$(pwd)/sandak bash tests/sandak/run_tests.sh`
Expected: T2+T3+T4 OK, финал `sandak: всички тестове минаха`.

- [ ] **Step 5: Commit**

```bash
git add src/sandak.c tests/sandak/run_tests.sh
git commit -m "sandak: sandak.lock + --locked проверка (T4)"
```

---

### Task 5: Git зависимости + subdir

**Files:**
- Modify: `src/sandak.c`
- Test: `tests/sandak/run_tests.sh` (git случай с локално repo — работи offline)

**Interfaces:**
- Consumes: `Dep.git/ref_kind/ref/subdir`, `resolve`, `write_lock`/`check_locked`.
- Produces: `static void fetch_git(const Dep *d, char out_root[512]);` — клонира в `.sandak/cache/<name>-<ref>/` (създава `.sandak/cache` при нужда), връща каноничния корен на пакета (clone + subdir). `dep_root` вика `fetch_git` за git deps. Lock-ът записва `source = "git+<url>"`, `rev = "<ref_kind>:<ref>"`.

- [ ] **Step 1: Failing тест (преди финалния echo)**

```bash
# --- T5: git зависимост (локално repo, offline) ---
mkdir -p "$T/extlib"
cat > "$T/extlib/sandak.toml" <<'EOF'
[package]
name = "extlib"
version = "0.3.0"
entry = "extlib.baga"
EOF
echo 'fn ext_hi() -> i64 { return 9 }' > "$T/extlib/extlib.baga"
git -C "$T/extlib" init -q
git -C "$T/extlib" add -A
git -C "$T/extlib" -c user.email=t@t -c user.name=t commit -qm init

mkdir -p "$T/gitapp"
cat > "$T/gitapp/sandak.toml" <<EOF
[package]
name = "gitapp"
version = "0.1.0"
entry = "main.baga"
kind = "bin"

[dependencies]
extlib = { git = "file://$T/extlib", branch = "master" }
EOF
echo 'import "extlib/extlib.baga"
fn main() { print(ext_hi()) }' > "$T/gitapp/main.baga"

out=$(cd "$T/gitapp" && "$SANDAK" fetch) || fail "git fetch exit"
echo "$out" | grep -q "resolved: extlib 0.3.0" || fail "git fetch extlib: [$out]"
[ -d "$T/gitapp/.sandak/cache" ] || fail "няма .sandak/cache"
grep -q 'source = "git+file://' "$T/gitapp/sandak.lock" || fail "lock без git source"
echo "OK: git зависимост се клонира и резолвира"

# второ fetch ползва кеша (без мрежа) — трябва да мине
out=$(cd "$T/gitapp" && "$SANDAK" fetch) || fail "git fetch от кеш"
echo "OK: git кеш се ползва повторно"
```
Ако default branch-ът на `git init` е `main`, а не `master`: тестът създава repo-то с `git init -q -b master` (поддържа се от git ≥ 2.28; ползвай този вариант).

- [ ] **Step 2: Пусни — гърми**

Run: `SANDAK=$(pwd)/sandak bash tests/sandak/run_tests.sh`
Expected: `die: git зависимости: неоще (T5)` → `FAIL: git fetch exit`.

- [ ] **Step 3: Имплементация — в src/sandak.c**

Замени тялото на `dep_root` за git случая и добави:

```c
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
    char cache[512], dir[512], cmd[2048];
    snprintf(cache, sizeof cache, ".sandak/cache");
    if (!is_dir(cache)) {
        snprintf(cmd, sizeof cmd, "mkdir -p '%s'", cache);
        run(cmd);
    }
    snprintf(dir, sizeof dir, "%s/%s-%s", cache, d->name, d->ref);
    if (!is_dir(dir)) {
        if (strcmp(d->ref_kind, "rev") == 0) {
            /* произволен commit: init + fetch --depth 1 + checkout */
            snprintf(cmd, sizeof cmd,
                "git init -q '%s' && git -C '%s' remote add origin '%s' && "
                "git -C '%s' fetch -q --depth 1 origin '%s' && "
                "git -C '%s' checkout -q FETCH_HEAD", dir, dir, d->git, dir, d->ref, dir);
        } else {
            snprintf(cmd, sizeof cmd,
                "git clone -q --depth 1 --branch '%s' '%s' '%s'",
                d->ref, d->git, dir);
        }
        run(cmd);
    }
    char tmp[1024];
    if (d->subdir[0]) snprintf(tmp, sizeof tmp, "%s/%s", dir, d->subdir);
    else snprintf(tmp, sizeof tmp, "%s", dir);
    canon(tmp, out_root);
}
```
В `dep_root`, замени `die("git зависимости: неоще (T5)");` с `fetch_git(d, out);`.

Разширени сигнатури — `resolve_into` получава Dep-а, който я е повикал:
```c
static void resolve_into(Graph *g, const char *dir, char stack[][512], int depth,
                         const Dep *from) {
    ...
    /* проверката dir == name: за git deps без subdir клонираната директория е
     * <name>-<ref>, затова там я пропускаме; при subdir basename трябва да е
     * името на пакета, както при path deps */
    if (!from || !from->git[0] || from->subdir[0]) {
        if (strcmp(base_name(dir), m->name) != 0)
            die("директория '%s' не съвпада с името на пакета '%s'", base_name(dir), m->name);
    }
    ...
        if (!seen) resolve_into(g, root, stack, depth + 1, &m->deps[i]);
}
static void resolve(Graph *g, const char *root_dir) {
    ...
    resolve_into(g, canon_root, stack, 0, NULL);
}
```

В `write_lock`: за git deps трябва `source = "git+<url>"`. Добави в `Manifest` полета `char src_git[512]; char src_ref[160];` (празни за path/root), които `resolve_into` попълва след `parse_manifest`:
```c
    if (from && from->git[0]) {
        snprintf(m->src_git, sizeof m->src_git, "%s", from->git);
        snprintf(m->src_ref, sizeof m->src_ref, "%s:%s", from->ref_kind, from->ref);
    }
```
и в `write_lock`:
```c
        if (g->pkgs[i].src_git[0]) {
            snprintf(p->source, sizeof p->source, "git+%s", g->pkgs[i].src_git);
            snprintf(p->rev, sizeof p->rev, "%s", g->pkgs[i].src_ref);
        } else {
            snprintf(p->source, sizeof p->source, "path+%s", g->pkgs[i].dir);
            snprintf(p->rev, sizeof p->rev, "-");
        }
```
(Двата повикваща на `resolve_into` вече са обновени по-горе.)

- [ ] **Step 4: Пусни тестовете**

Run: `make sandak && SANDAK=$(pwd)/sandak bash tests/sandak/run_tests.sh`
Expected: всички OK (T2–T5), финал `sandak: всички тестове минаха`. После `make test` — зелен.

- [ ] **Step 5: Commit**

```bash
git add src/sandak.c tests/sandak/run_tests.sh
git commit -m "sandak: git зависимости с кеш и subdir (T5)"
```

---

### Task 6: `sandak build` / `sandak run`

**Files:**
- Modify: `src/sandak.c`
- Test: `tests/sandak/run_tests.sh` (build/run случаи)

**Interfaces:**
- Consumes: `resolve`, `Graph`, `-I` флагът на `baga` (Task 1).
- Produces: `sandak build [--locked]` — bin: `target/<name>` бинарник; lib: `baga --lib` проверка. `sandak run [-- <args>]` — build + exec. Това е финалният CLI, който Dockerfile-ът (Task 8) вика.

- [ ] **Step 1: Failing тест (преди финалния echo)**

```bash
# --- T6: build + run ---
# използваме $T/ws от T3 (myapp -> mylib); myapp/main.baga липсва още
cat > "$T/ws/myapp/main.baga" <<'EOF'
import "mylib/mylib.baga"

fn main() {
    print(hi())
}
EOF
out=$(cd "$T/ws/myapp" && "$SANDAK" build) || fail "build exit"
[ -x "$T/ws/myapp/target/myapp" ] || fail "няма target/myapp"
echo "OK: sandak build прави бинарник"

out=$(cd "$T/ws/myapp" && "$SANDAK" run) || fail "run exit"
[ "$out" = "7" ] || fail "run изход: [$out]"
echo "OK: sandak run"

# lib пакет: build = --lib проверка
out=$(cd "$T/ws/mylib" && "$SANDAK" build) || fail "lib build exit"
echo "$out" | grep -q "ok:" || fail "lib build изход: [$out]"
echo "OK: sandak build на библиотека (--lib)"

# gitapp от T5: build + run през git dep
out=$(cd "$T/gitapp" && "$SANDAK" run) || fail "gitapp run exit"
[ "$out" = "9" ] || fail "gitapp run изход: [$out]"
echo "OK: build/run през git зависимост"
```
Забележка: `import "mylib/mylib.baga"` се резолвира през `-I $T/ws` (родителят на `mylib`). Точно това тества интеграцията Task 1 ↔ Task 6.

- [ ] **Step 2: Пусни — гърми**

Run: `SANDAK=$(pwd)/sandak BAGA=$(pwd)/baga bash tests/sandak/run_tests.sh`
Expected: `FAIL: build exit` (usage + exit 1).

- [ ] **Step 3: Имплементация — в src/sandak.c**

```c
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
    Graph g;
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
```
В `main` — разпознавай `build`, `run` и `--` separator:
```c
    if (strcmp(argv[1], "build") == 0) { cmd_build(0, 0, NULL); return 0; }
    if (strcmp(argv[1], "run") == 0) {
        int sep = argc;
        for (int i = 2; i < argc; i++)
            if (strcmp(argv[i], "--") == 0) { sep = i; break; }
        cmd_build(1, argc - sep - 1, argv + sep + 1);
        return 0;
    }
```
(`--locked` parsing-ът от Task 4 трябва да игнорира всичко след `--`: сложи `break` в цикъла при срещане на `"--"`.)

- [ ] **Step 4: Пусни тестовете**

Run: `make sandak && SANDAK=$(pwd)/sandak BAGA=$(pwd)/baga bash tests/sandak/run_tests.sh`
Expected: всички OK (T2–T6). После `make test` — зелен.

- [ ] **Step 5: Commit**

```bash
git add src/sandak.c tests/sandak/run_tests.sh
git commit -m "sandak: build + run (emit-c + gcc, -I интеграция) (T6)"
```

---

### Task 7: Пакетиране на монорепото + миграция на импортите

**Files:**
- Create: `std/sandak.toml`, `app-product/{httpdbaga,jwtbaga,pgbaga,ormbaga,fmrbaga}/sandak.toml`, `apps/api/sandak.toml`
- Modify: всички `.baga` файлове в `app-product/`, `apps/`, `tests/` (импорти — механичен sed)
- Modify: `Makefile` (`BAGAIFLAGS`, `test:` да подава флаговете)

**Interfaces:**
- Consumes: `-I` (Task 1), `sandak build` (Task 6).
- Produces: 7 пакета с манифести; импорти в пакетна форма (`import "fmrbaga/app.baga"`, `import "std/str/str.baga"`); `cd apps/api && sandak build` работи. Docker (Task 8) разчита на това.

**Граф на зависимостите** (от одита на `^import` в repo-то):

| Пакет | Директория | kind | entry | deps |
|-------|-----------|------|-------|------|
| std | `std/` | lib | — (няма единен корен) | — |
| httpdbaga | `app-product/httpdbaga` | lib | `http.baga` | std |
| jwtbaga | `app-product/jwtbaga` | lib | `jwt.baga` | std, httpdbaga (server.baga) |
| pgbaga | `app-product/pgbaga` | lib | `pg.baga` | std |
| ormbaga | `app-product/ormbaga` | lib | `orm.baga` | pgbaga, std |
| fmrbaga | `app-product/fmrbaga` | lib | `handlers.baga` | httpdbaga, jwtbaga, ormbaga, std |
| api | `apps/api` | **bin** | `start.baga` | fmrbaga, ormbaga, std |

- [ ] **Step 1: Манифести**

`std/sandak.toml`:
```toml
[package]
name = "std"
version = "0.7.0"
```
`app-product/httpdbaga/sandak.toml`:
```toml
[package]
name = "httpdbaga"
version = "0.2.0"
entry = "http.baga"

[dependencies]
std = { path = "../../std" }
```
`app-product/jwtbaga/sandak.toml`:
```toml
[package]
name = "jwtbaga"
version = "0.1.0"
entry = "jwt.baga"

[dependencies]
std = { path = "../../std" }
httpdbaga = { path = "../httpdbaga" }
```
`app-product/pgbaga/sandak.toml`:
```toml
[package]
name = "pgbaga"
version = "0.1.0"
entry = "pg.baga"

[dependencies]
std = { path = "../../std" }
```
`app-product/ormbaga/sandak.toml`:
```toml
[package]
name = "ormbaga"
version = "0.1.0"
entry = "orm.baga"

[dependencies]
pgbaga = { path = "../pgbaga" }
std = { path = "../../std" }
```
`app-product/fmrbaga/sandak.toml`:
```toml
[package]
name = "fmrbaga"
version = "0.1.0"
entry = "handlers.baga"

[dependencies]
httpdbaga = { path = "../httpdbaga" }
jwtbaga = { path = "../jwtbaga" }
ormbaga = { path = "../ormbaga" }
std = { path = "../../std" }
```
`apps/api/sandak.toml`:
```toml
[package]
name = "api"
version = "0.1.0"
entry = "start.baga"
kind = "bin"

[dependencies]
fmrbaga = { path = "../../app-product/fmrbaga" }
ormbaga = { path = "../../app-product/ormbaga" }
std = { path = "../../std" }
```

- [ ] **Step 2: Failing проверка — sandak build гърми преди миграцията на импортите**

Run: `cd apps/api && ../../sandak build; cd -`
Expected: грешки `не мога да намеря import '../../app-product/fmrbaga/app.baga'`... — не съвсем: относителните импорти все още работят, така че build може и да мине. Истинската failing проверка е след миграцията БЕЗ `-I`: `baga apps/api/start.baga` гърми с `не мога да намеря import 'fmrbaga/app.baga'`. Затова: първо sed (Step 3), после проверката `make test` (Step 4) е зелена само с `BAGAIFLAGS`. TDD тук = тестът (`make test`) първо е червен след миграцията (без Makefile промяна), после зелен (с нея).

- [ ] **Step 3: Миграция на импортите (механичен sed)**

Правила (приложени само върху редове, започващи с `import "`):
1. `"(../)+app-product/` → `"` (напр. `"../../app-product/fmrbaga/app.baga"` → `"fmrbaga/app.baga"`)
2. `"(../)+std/` → `"std/` (напр. `"../../../std/str/str.baga"` → `"std/str/str.baga"`)
3. `"(../)+(fmrbaga|httpdbaga|jwtbaga|ormbaga|pgbaga)/` → `"\1/` (cross-package относителни, напр. `"../httpdbaga/http.baga"` → `"httpdbaga/http.baga"`)

Intra-package импортите (`"route.baga"`, `"../bytes/bytes.baga"` в std, `"../models/user.baga"` в apps/api, `"import_lib.baga"` в tests) НЕ се пипат.

```bash
files=$(grep -rl --include='*.baga' -E '^import "' app-product apps tests)
echo "$files" | xargs sed -i -E \
  -e 's#^import "(\.\./)+app-product/#import "#' \
  -e 's#^import "(\.\./)+std/#import "std/#'
echo "$files" | xargs sed -i -E \
  -e 's#^import "(\.\./)+(fmrbaga|httpdbaga|jwtbaga|ormbaga|pgbaga)/#import "\1/#'
```
Проверка, че не са останали cross-package относителни импорти:
```bash
grep -rn -E '^import "(\.\./)+(app-product|std|fmrbaga|httpdbaga|jwtbaga|ormbaga|pgbaga)' --include='*.baga' app-product apps tests
# очаквано: празно
grep -rn -E '^import "\.\.' --include='*.baga' app-product apps tests
# очаквано: само intra-package (std: ../bytes, ../os, ../str; apps/api: ../models)
# и tests/api_test.baga -> ../apps/api/models (относителен, работи; не е пакетен)
```

- [ ] **Step 4: Makefile — BAGAIFLAGS**

В `Makefile` след реда `LDFLAGS := ...` добави:
```make
# import search path за пакетите в монорепото (sandak го изчислява автоматично;
# за ръчни ./baga извиквания в тестовете: repo root за std/, app-product/ за *baga)
BAGAIFLAGS := -I . -I app-product
```
После:
```bash
sed -i 's#\./\$(BIN) #./$(BIN) $(BAGAIFLAGS) #g' Makefile
```
Провери `git diff Makefile`, че промяната е само по редове с `./$(BIN) ` и не е счупила `$(MAKE)`/`grep` редове. Провери и `tests/llvm_oracle.sh` за директни `./baga`/`baga-llvm` извиквания върху файлове с импорти — ако има (напр. `grep -n 'app-product\|std/' tests/llvm_oracle.sh`), добави същите `-I . -I app-product` флагове там.

- [ ] **Step 5: Пусни пълния suite + sandak build на repo пакетите**

Run: `make && make sandak && make test`
Expected: `Всички тестове минаха. ⚔️` (вкл. `sandak: всички тестове минаха`).

Run: `for p in app-product/httpdbaga app-product/jwtbaga app-product/pgbaga app-product/ormbaga app-product/fmrbaga; do (cd $p && $(pwd)/sandak build) || exit 1; done`
Expected: всеки печата `ok:` (lib проверка).

Run: `cd apps/api && $(pwd)/sandak build && cd -`
Expected: `sandak: api -> target/api` и файл `apps/api/target/api` съществува.

Допълнителен регресионен тест в `Makefile` (в `test:` след sandak блока), за да не се връщаме назад:
```make
	@echo "=== sandak build на repo пакетите ==="
	@for p in httpdbaga jwtbaga pgbaga ormbaga fmrbaga; do \
		(cd app-product/$$p && $(CURDIR)/sandak build > /dev/null) \
			&& echo "OK: sandak build $$p" \
			|| { echo "FAIL: sandak build $$p"; exit 1; }; \
	done
	@(cd apps/api && $(CURDIR)/sandak build > /dev/null) && test -x apps/api/target/api \
		&& echo "OK: sandak build apps/api (bin)" \
		|| { echo "FAIL: sandak build apps/api"; exit 1; }
```

- [ ] **Step 6: Commit**

```bash
git add std/sandak.toml app-product apps tests Makefile
git commit -m "пакетиране на монорепото: sandak.toml навсякъде + пакетни импорти (T7)"
```
(Внимание: `target/` и `.sandak/` са в `.gitignore` от Task 2 — `git status` не трябва да ги показва.)

---

### Task 8: Dockerfile + docker-compose.yml + smoke тест

**Files:**
- Create: `Dockerfile`, `docker-compose.yml`, `.dockerignore`, `tests/sandak/docker_smoke.sh`
- Modify: `Makefile` (цел `docker`)

**Interfaces:**
- Consumes: `sandak build --locked` (Task 6), пакетираното repo (Task 7).
- Produces: multi-stage образ: `APP_REPO`/`APP_REF`/`APP_DIR` → runtime образ с бинарника. Това е крайният сценарий от спецификацията.

- [ ] **Step 1: Dockerfile**

```dockerfile
# syntax=docker/dockerfile:1
# Sandak multi-stage build: git URL -> компилиран Baga бинарник в slim образ.
# Параметри (виж docker-compose.yml):
#   BAGA_REPO/BAGA_REF — откъде идва toolchain-ът (компилатор + sandak)
#   APP_REPO/APP_REF   — приложението; APP_DIR — поддиректория в него (monorepo)

FROM debian:bookworm-slim AS toolchain
RUN apt-get update \
 && apt-get install -y --no-install-recommends gcc make git ca-certificates \
 && rm -rf /var/lib/apt/lists/*
ARG BAGA_REPO=https://git.bara-lang.org/baga-lang-ai/baga-lang-ai.git
ARG BAGA_REF=main
RUN git clone --depth 1 --branch "$BAGA_REF" "$BAGA_REPO" /baga \
 && make -C /baga all sandak \
 && cp /baga/baga /baga/sandak /usr/local/bin/

FROM toolchain AS build
ARG APP_REPO
ARG APP_REF=main
ARG APP_DIR=.
RUN test -n "$APP_REPO" || { echo "APP_REPO е задължителен (--build-arg)"; exit 1; }
RUN git clone --depth 1 --branch "$APP_REF" "$APP_REPO" /app
WORKDIR /app/$APP_DIR
# --locked: възпроизводим build; deps се теглят от GitHub/git по sandak.lock
RUN sandak build --locked && mkdir -p /out \
 && rm -f target/*.c \
 && cp target/* /out/app

FROM debian:bookworm-slim
COPY --from=build /out/app /usr/local/bin/app
CMD ["app"]
```

`docker-compose.yml`:
```yaml
# Само параметри: редактирай APP_REPO/APP_REF (и APP_DIR за monorepo),
# после: docker compose up --build
services:
  api:
    build:
      context: .
      args:
        APP_REPO: https://git.bara-lang.org/you/your-baga-app.git
        APP_REF: main
        APP_DIR: .
    ports:
      - "8080:8080"
```

`.dockerignore`:
```
.git
.sandak
target
*.o
baga
baga-llvm
sandak
```

- [ ] **Step 2: Smoke тест (offline, с локални git repos)**

`tests/sandak/docker_smoke.sh`:
```bash
#!/bin/bash
# Docker smoke: локално "приложение" в temp git repo -> образ -> бинарникът работи.
# Toolchain-ът също идва от локалното repo (file://) — тества точно clone пътя.
set -eu
cd "$(dirname "$0")/../.."
command -v docker >/dev/null 2>&1 || { echo "SKIP: няма docker"; exit 0; }

T=$(mktemp -d); trap 'rm -rf "$T"' EXIT

# приложение: hello бинарник, kind = bin, без deps
mkdir -p "$T/helloapp"
cat > "$T/helloapp/sandak.toml" <<'EOF'
[package]
name = "helloapp"
version = "0.1.0"
entry = "main.baga"
kind = "bin"
EOF
cat > "$T/helloapp/main.baga" <<'EOF'
fn main() {
    print("docker-smoke-ok")
}
EOF
# lock за --locked: генерираме го с локалния sandak
SANDAK_BIN=$(pwd)/sandak
(cd "$T/helloapp" && "$SANDAK_BIN" fetch >/dev/null) \
  || { echo "FAIL: lock генериране"; exit 1; }
git -C "$T/helloapp" init -q -b main
git -C "$T/helloapp" add -A
git -C "$T/helloapp" -c user.email=t@t -c user.name=t commit -qm init

docker build -q \
  --build-arg "BAGA_REPO=file://$PWD" \
  --build-arg BAGA_REF=main \
  --build-arg "APP_REPO=file://$T/helloapp" \
  --build-arg APP_REF=main \
  -t baga-smoke . > /dev/null
out=$(docker run --rm baga-smoke)
[ "$out" = "docker-smoke-ok" ] || { echo "FAIL: изход [$out]"; exit 1; }
echo "OK: docker smoke (git URL -> образ -> бинарник)"
```
Забележка: `BAGA_REPO=file://$PWD` клонира **комитнатото** състояние — smoke-ът върви след commit на Task 1-7.

Makefile цел:
```make
.PHONY: docker
docker: $(BIN) sandak
	@bash tests/sandak/docker_smoke.sh
```

- [ ] **Step 3: Пусни smoke**

Run: `make docker`
Expected: `OK: docker smoke (git URL -> образ -> бинарник)` (или `SKIP: няма docker`).

- [ ] **Step 4: Commit**

```bash
git add Dockerfile docker-compose.yml .dockerignore tests/sandak/docker_smoke.sh Makefile
git commit -m "docker: multi-stage build от git URL (toolchain + app + deps) (T8)"
```

---

### Task 9: Документация

**Files:**
- Modify: `README.md` (секция Packages/sandak след Quick Start)
- Modify: `CHANGELOG.md` (`[Unreleased]`)
- Modify: `app-product/BASE.md` (правило за манифестите)

- [ ] **Step 1: README.md — нова секция след Quick Start**

````markdown
## Packages — sandak

`sandak` is the package manager (the chest that holds the crates). Each package
has a `sandak.toml`:

```toml
[package]
name = "api"
version = "0.1.0"
entry = "start.baga"
kind = "bin"            # default: lib

[dependencies]
fmrbaga = { path = "../../app-product/fmrbaga" }
jwtbaga = { git = "https://github.com/user/jwtbaga", rev = "a1b2c3", subdir = "." }
```

```bash
make sandak
cd apps/api
sandak fetch    # resolve deps, write sandak.lock
sandak build    # -> target/api   (libs: typecheck via baga --lib)
sandak run      # build + run
```

Imports name the package: `import "fmrbaga/app.baga"`. The compiler resolves
them through `-I <dir>` search paths, which sandak computes from the
dependency graph. Docker: edit `APP_REPO`/`APP_REF` in `docker-compose.yml`
and `docker compose up --build` — the image clones the toolchain, the app,
and its locked dependencies from git and builds itself.
````

- [ ] **Step 2: CHANGELOG.md — под `[Unreleased]`**

```markdown
### Packages — sandak (пакетна система)
- New tool `sandak`: `sandak.toml` manifests, path + git dependencies
  (with `subdir` for monorepos), `sandak.lock` with `--locked`, and
  `fetch`/`build`/`run` commands. Zero dependencies (libc + git + gcc).
- Compiler: repeatable `-I <dir>` import search path flag.
- The whole monorepo is packaged: `std`, `app-product/*`, `apps/api` have
  manifests; imports are package-named (`import "fmrbaga/app.baga"`).
- Docker: multi-stage `Dockerfile` + `docker-compose.yml` — point `APP_REPO`
  at a git URL and the container clones toolchain + app + deps and builds.
```

- [ ] **Step 3: app-product/BASE.md — ново правило 5**

```markdown
5. Every package carries a `sandak.toml` (name == directory name) and builds
   with `sandak build`; imports use package names, never `../../` paths.
```

- [ ] **Step 4: Финална верификация**

Run: `make clean && make && make sandak && make test && make docker`
Expected: `Всички тестове минаха. ⚔️` + `OK: docker smoke` (или SKIP).

- [ ] **Step 5: Commit**

```bash
git add README.md CHANGELOG.md app-product/BASE.md
git commit -m "docs: sandak пакетна система — README, CHANGELOG, BASE.md (T9)"
```

---

## Self-Review бележки (проверено при писането)

- Spec coverage: манифест (T2), резолюция+цикли+дупликации (T3), lock+--locked (T4),
  git+subdir+кеш (T5), build/run/`-I` (T6+T1), пакетиране+Makefile (T7),
  Docker+compose+smoke (T8), docs (T9). Грешките на български — навсякъде `die("...")`.
- Отклонение от spec: добавено поле `kind = "bin"|"lib"` (default lib) — нужно е,
  за да знае `sandak build` дали да линква бинарник или само `--lib` проверява.
  Да се отбележи в spec-а при първа промяна (не блокира плана).
- Отклонение от spec: `entry` е задължителен само за `kind = "bin"` (std няма единен
  корен); lib без entry → `sandak build` печати „нищо за билдване".
- Type consistency: `Manifest`, `Dep`, `Graph`, `LockPkg`, `resolve`, `dep_root`,
  `fetch_git`, `include_flags`, `write_lock`, `check_locked` — имената са еднакви в
  задачите, които ги произвеждат и консумират.
