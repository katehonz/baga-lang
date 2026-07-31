# Изпълними spec гаранции (`ensures:`) — Имплементационен план

> **For agentic workers:** Steps use checkbox (`- [ ]`) syntax. Работи се задача по задача, всяка завършва с проверима стъпка.
> **ВАЖНО:** БЕЗ `git commit`/`git add` — git мутации изискват изрично одобрение от потребителя (правило на средата). Стъпките „Commit" от шаблона са заменени с „Проверка".

**Goal:** Секцията `ensures:` в `spec` става изпълнима — булеви изрази на Бага, тип-проверени при компилация и компилирани до runtime проверки, които спират програмата при нарушение.

**Architecture:** Парсерът пази всеки ensures израз като AST + оригинален текст (нов `NODE_ENSURE`). Checker-ът го тип-проверява в scope със spec input имената + `output`. Codegen генерира `static` impl функция + публична wrapper функция, която оценява гаранциите след всяко повикване и вика новия runtime helper `baga_spec_fail`.

**Tech Stack:** C99, gcc, make. Нула зависимости (философия на проекта).

**Spec:** `docs/superpowers/specs/2026-07-31-executable-spec-ensures-design.md`

## Global Constraints

- Нула външни зависимости; само `gcc` и `make`.
- Съобщенията за грешки са на български, формат `файл: ред:колона: съобщение`.
- Съществуващата секция `guarantees:` (свободен текст) остава непроменена.
- LLVM backend (`codegen_llvm.c`) и self-hosted компилатор (`self/*.baga`) — извън обхват.
- БЕЗ git мутации.
- Build: `make` (target `all`). Регресия: `make test`.

---

### Task 1: AST и парсер — `NODE_ENSURE`, секция `ensures:`

**Files:**
- Modify: `include/baga.h` (NodeKind enum ~ред 171, NODE_SPEC union member ~ред 305, union ~ред 314)
- Modify: `src/parser.c` (`node_free` ред 126-133, `parse_spec` ред 844-908, AST printer ~ред 1148)

**Interfaces:**
- Produces: `NODE_ENSURE` node с полета `char *ensure_text; Node *ensure_expr;`; `Node->spec_ensures` (`NodeVec` от `NODE_ENSURE`) в `NODE_SPEC`. Парсва синтаксиса `ensures: <expr>, <expr>,` в spec тялото.

- [ ] **Step 1: Примерен файл (тестът)**

Създай `examples/spec_ensures.baga`:

```baga
// Spec с изпълними гаранции (ensures)
spec факториел {
    input:
        n: i64
    output: i64
    ensures:
        output > 0,
        n <= 1 || output >= n
}

fn факториел(n: i64) -> i64 {
    if n <= 1 {
        return 1
    }
    return n * факториел(n - 1)
}

fn main() {
    print(факториел(10))
}
```

- [ ] **Step 2: Потвърди, че днес не се парсва**

Run: `make && ./baga examples/spec_ensures.baga`
Expected: програмата минава (парсерът днес пропуска непознати идентификатори в spec тялото с `advance`), но `./baga --ast examples/spec_ensures.baga` НЕ показва ensures.

- [ ] **Step 3: `include/baga.h` — нов NodeKind и полета**

В `NodeKind` enum, веднага след `NODE_ENUM,` добави:

```c
    NODE_ENSURE,      /* ensures елемент: текст + булев израз */
```

В union-а на `struct Node`, веднага след `NODE_SPEC` блока, добави:

```c
        /* NODE_ENSURE */
        struct { char *ensure_text; Node *ensure_expr; };
```

В `NODE_SPEC` struct-а добави поле:

```c
        /* NODE_SPEC */
        struct {
            char *spec_name;
            NodeVec spec_inputs;   /* NODE_PARAM */
            Node *spec_output;     /* type node */
            char **spec_guarantees;
            int n_guarantees;
            NodeVec spec_ensures;  /* NODE_ENSURE */
        };
```

- [ ] **Step 4: `src/parser.c` — `node_free`**

В `node_free`, case `NODE_SPEC` — преди `break;` добави освобождаване на ensures:

```c
        case NODE_SPEC:
            free(n->spec_name);
            for (int i = 0; i < n->spec_inputs.len; i++) node_free(n->spec_inputs.data[i]);
            vec_free(n->spec_inputs);
            node_free(n->spec_output);
            for (int i = 0; i < n->n_guarantees; i++) free(n->spec_guarantees[i]);
            free(n->spec_guarantees);
            for (int i = 0; i < n->spec_ensures.len; i++) node_free(n->spec_ensures.data[i]);
            vec_free(n->spec_ensures);
            break;
```

И нов case (след `NODE_SPEC` case-а):

```c
        case NODE_ENSURE:
            free(n->ensure_text);
            node_free(n->ensure_expr);
            break;
```

- [ ] **Step 5: `src/parser.c` — `parse_spec`**

Добави `"ensures"` към stop-условието на input-секцията (ред 861-863). Становището трябва да стане:

```c
            while (check(p, TOK_IDENT) &&
                   strcmp(cur(p)->text, "output") != 0 &&
                   strcmp(cur(p)->text, "guarantees") != 0 &&
                   strcmp(cur(p)->text, "ensures") != 0) {
```

Добави локален вектор в началото на `parse_spec` (до `VEC(char *) guarantees = {0};`):

```c
    NodeVec ensures = {0};
```

Добави нов клон в while-цикъла на секциите (след `guarantees` клона, преди `else`):

```c
        } else if (check(p, TOK_IDENT) && cur(p)->text && strcmp(cur(p)->text, "ensures") == 0) {
            advance(p);
            expect(p, TOK_COLON);
            while (!check(p, TOK_RBRACE) && !check(p, TOK_EOF)) {
                int tstart = p->pos;
                SrcPos epos = cur(p)->pos;
                Node *expr = parse_expr(p);
                /* възстанови текста на израза от токените за съобщенията */
                char buf[512] = {0};
                int bi = 0;
                for (int ti = tstart; ti < p->pos; ti++) {
                    Token *t = &p->tokens[ti];
                    if (!t->text) continue;
                    int tl = (int)strlen(t->text);
                    if (bi > 0 && bi < 500) buf[bi++] = ' ';
                    if (bi + tl < 500) { memcpy(buf + bi, t->text, (size_t)tl); bi += tl; }
                }
                buf[bi] = '\0';
                Node *en = node_alloc(NODE_ENSURE, epos);
                en->ensure_text = strdup(buf);
                en->ensure_expr = expr;
                vec_push(ensures, en);
                if (!match(p, TOK_COMMA)) break;
            }
        }
```

(`parse_expr` вече е достъпен — деклариран/дефиниран по-рано във файла; използван е от `parse_statement` и др. Ако е дефиниран след `parse_spec` без forward declaration, добави `static Node *parse_expr(Parser *p);` прототип при останалите прототипи горе във файла — провери с `grep -n "parse_expr" src/parser.c | head`.)

В края на `parse_spec`, след `s->n_guarantees = guarantees.len;` добави:

```c
    s->spec_ensures = ensures;
```

- [ ] **Step 6: AST printer (debug)**

В AST printer-а (~ред 1148, `case NODE_SPEC`) след печата на guarantees добави:

```c
            for (int i = 0; i < n->spec_ensures.len; i++)
                fprintf(stderr, "  ensures: %s\n", n->spec_ensures.data[i]->ensure_text);
```

(Съобрази точния контекст със съществуващия код — моделът е цикълът за guarantees.)

- [ ] **Step 7: Проверка**

Run: `make && ./baga --ast examples/spec_ensures.baga 2>&1 | grep ensures`
Expected: два реда `ensures: output > 0` и `ensures: n <= 1 || output >= n`. `./baga examples/spec_ensures.baga` печата `3628800` (ensures все още не се изпълнява — това е Task 3). `make test` минава.

---

### Task 2: Checker — тип-проверка на `ensures`

**Files:**
- Modify: `src/checker.c` (`check_program`, pass 2, ред 729-771)

**Interfaces:**
- Consumes: `NODE_ENSURE` от Task 1; съществуващите `push_scope/pop_scope/env_define/infer/resolve_type_node/type_str` (checker.c:91-246).
- Produces: compile-time грешки: `spec '<име>': ensures изисква функция с върнат тип`; `spec '<име>': ensures #N е <тип>, очаквах bool`; `недефинирана променлива '<име>'` за непознати имена в ensures.

- [ ] **Step 1: Негативни тестове (първо)**

Създай `/tmp/ensures_bad_type.baga`:

```baga
spec лоша {
    input:
        x: i64
    output: i64
    ensures:
        output + 1
}

fn лоша(x: i64) -> i64 { return x }

fn main() { print(лоша(1)) }
```

Създай `/tmp/ensures_bad_name.baga`:

```baga
spec лоша {
    input:
        x: i64
    output: i64
    ensures:
        output > непозната
}

fn лоша(x: i64) -> i64 { return x }

fn main() { print(лоша(1)) }
```

- [ ] **Step 2: Потвърди, че днес минават без грешка**

Run: `./baga /tmp/ensures_bad_type.baga`
Expected: компилира и печата `1` (никой не гледа ensures).

- [ ] **Step 3: Имплементация в pass 2**

В `check_program`, pass 2, веднага след блока „check output type" (след затварящата скоба на `if (item->spec_output) { ... }`, преди края на for-цикъла по items) добави:

```c
        /* check ensures expressions (type-check in scope: inputs + output) */
        if (item->spec_ensures.len > 0) {
            Type *fn_ret = ft->ret ? ft->ret : type_new(TYPE_VOID);
            if (fn_ret->kind == TYPE_VOID) {
                check_error(&ctx, item->pos,
                    "spec '%s': ensures изисква функция с върнат тип",
                    item->spec_name);
            } else {
                push_scope(&ctx);
                for (int j = 0; j < item->spec_inputs.len; j++) {
                    Node *sp = item->spec_inputs.data[j];
                    env_define(&ctx, sp->param_name,
                               resolve_type_node(sp->param_type), sp->pos);
                }
                env_define(&ctx, "output", fn_ret, item->pos);
                for (int j = 0; j < item->spec_ensures.len; j++) {
                    Node *en = item->spec_ensures.data[j];
                    Type *et = infer(&ctx, en->ensure_expr);
                    if (et->kind != TYPE_BOOL && et->kind != TYPE_ERROR) {
                        check_error(&ctx, en->pos,
                            "spec '%s': ensures #%d е %s, очаквах bool",
                            item->spec_name, j + 1, type_str(et));
                    }
                }
                pop_scope(&ctx);
            }
        }
```

- [ ] **Step 4: Проверка на негативите**

Run: `make && ./baga /tmp/ensures_bad_type.baga; echo exit=$?`
Expected: грешка `spec 'лоша': ensures #1 е i64, очаквах bool`, `exit=1`.

Run: `./baga /tmp/ensures_bad_name.baga; echo exit=$?`
Expected: грешка `недефинирана променлива 'непозната'`, `exit=1`.

Run: `./baga examples/spec_ensures.baga`
Expected: `3628800` (валидният ensures минава проверката). `make test` минава.

---

### Task 3: Codegen — impl/wrapper двойка и `baga_spec_fail`

**Files:**
- Modify: `src/codegen_c.c` (нови helpers преди `emit_fn`; `emit_fn` ред 629-687; preamble ред 759-799)

**Interfaces:**
- Consumes: `NODE_ENSURE` от Task 1; `mangle_name(char*) -> char*` (b_ префикс + hex за не-ASCII), `emit_type(Codegen*, Node*)`, `emit_expr(Codegen*, Node*)`, `emit_indent(Codegen*)`, `cg->program` (вече е в `Codegen`).
- Produces: За функция с ensures: `static <ret> b__impl_<mangled>(<fn params>) { <тяло> }` + публична `<ret> <mangled>(<spec input имена>) { <ret> b_output = impl(...); if (!(...)) baga_spec_fail(...); return b_output; }`. Runtime helper `baga_spec_fail(const char*, int64_t, const char*)` в preamble-а.

Ключов детайл: `NODE_IDENT` се emit-ва през `mangle_name`, затова wrapper-ът декларира резултата като `b_output` (така `output` в ensures изразите сочи към него), а параметрите на wrapper-а носят **spec input имената** (не fn параметрите).

- [ ] **Step 1: Негативен runtime тест (първо)**

Създай `examples/spec_ensures_fail.baga`:

```baga
// Функцията нарушава собствения си spec — ensures хваща грешката по време на изпълнение
spec удвой {
    input:
        x: i64
    output: i64
    ensures:
        output == 2 * x
}

fn удвой(x: i64) -> i64 {
    return x + x + 1
}

fn main() {
    print(удвой(5))
}
```

- [ ] **Step 2: Потвърди, че днес нарушението минава**

Run: `make && ./baga examples/spec_ensures_fail.baga`
Expected: печата `11` без грешка.

- [ ] **Step 3: Preamble — `baga_spec_fail`**

В `codegen_c`, след реда за `baga_str_eq` (ред 781) и преди `fprintf(out, "\n");` добави:

```c
    fprintf(out, "static void baga_spec_fail(const char *spec, int64_t idx, const char *expr) {\n");
    fprintf(out, "    fprintf(stderr, \"spec '%%s': ensures #%%lld нарушена: %%s\\n\", spec, (long long)idx, expr);\n");
    fprintf(out, "    exit(1);\n");
    fprintf(out, "}\n");
```

- [ ] **Step 4: Helpers преди `emit_fn`**

Преди коментара `/* ---- function emission ---- */` добави:

```c
/* ---- spec ensures ---- */

/* Намира spec с ensures за дадена функция (NULL ако няма). */
static Node *find_ensures_spec(Codegen *cg, const char *fn_name) {
    if (!cg->program) return NULL;
    for (int i = 0; i < cg->program->items.len; i++) {
        Node *it = cg->program->items.data[i];
        if (it->kind == NODE_SPEC && it->spec_ensures.len > 0 &&
            strcmp(it->spec_name, fn_name) == 0)
            return it;
    }
    return NULL;
}

/* Екранира низ за вграждане в C string литерал. */
static void emit_c_string(FILE *f, const char *s) {
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') fprintf(f, "\\%c", c);
        else if (c == '\n') fprintf(f, "\\n");
        else fputc(c, f);
    }
}
```

- [ ] **Step 5: `emit_fn` — wrapper**

В началото на `emit_fn`, веднага след `FILE *f = cg->out;`, добави:

```c
    Node *ensures_spec = (fn->ret_type && fn->fn_body)
                       ? find_ensures_spec(cg, fn->fn_name) : NULL;
    char impl_name_buf[512];
    if (ensures_spec) {
        /* оригиналното тяло става static impl функция */
        snprintf(impl_name_buf, sizeof impl_name_buf, "__impl_%s", fn->fn_name);
        fprintf(f, "static ");
    }
```

Промени emit-ването на името (редове 641-643) на:

```c
    /* name */
    char *m = mangle_name(ensures_spec ? impl_name_buf : fn->fn_name);
    fprintf(f, "%s", m);
    free(m);
```

След края на съществуващото тяло на `emit_fn` (след `fprintf(f, "\n\n");` в края, ред 686) добави wrapper-а:

```c
    if (!ensures_spec) return;

    /* wrapper: публичното име, проверява ensures след повикването */
    emit_type(cg, fn->ret_type);
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
        emit_indent(cg);
        fprintf(f, "if (!(");
        emit_expr(cg, en->ensure_expr);
        fprintf(f, ")) baga_spec_fail(\"");
        emit_c_string(f, ensures_spec->spec_name);
        fprintf(f, "\", %d, \"", j + 1);
        emit_c_string(f, en->ensure_text);
        fprintf(f, "\");\n");
    }

    emit_indent(cg);
    fprintf(f, "return b_output;\n");
    cg->indent--;
    emit_indent(cg);
    fprintf(f, "}\n\n");
```

Забележка: `emit_forward_decls` не се пипа — публичното име се декларира както досега; impl е `static` и се emit-ва непосредствено преди wrapper-а си. Рекурсията минава през wrapper-а (тялото вика публичното име) — гаранциите важат и за вътрешните повиквания.

- [ ] **Step 6: Проверка**

Run: `make && ./baga examples/spec_ensures_fail.baga; echo exit=$?`
Expected: stderr `spec 'удвой': ensures #1 нарушена: output == 2 * x`, `exit=1`, без печат на `11`.

Run: `./baga examples/spec_ensures.baga`
Expected: `3628800` (гаранциите минават; рекурсивните повиквания също се проверяват).

Run: `./baga --emit-c examples/spec_ensures.baga | grep -A3 "b__impl"`
Expected: вижда се static impl функцията.

Run: `make test`
Expected: всичко минава.

---

### Task 4: `--proofs` и `--specs` — статус RUNTIME-CHECKED

**Files:**
- Modify: `src/proofs.c` (spec печат, ~ред 155-161)
- Modify: `src/main.c` (`--specs` handler, ред 122-147)

**Interfaces:**
- Consumes: `spec_ensures` от Task 1.

- [ ] **Step 1: proofs.c**

В `print_proofs`, след цикъла за guarantees и преди/вместо статус реда, добави:

```c
        for (int j = 0; j < spec->spec_ensures.len; j++)
            printf("    ensures: %s\n", spec->spec_ensures.data[j]->ensure_text);
```

и промени статус логиката на:

```c
        if (spec->spec_ensures.len > 0)
            printf("    status: RUNTIME-CHECKED\n\n");
        else
            printf("    status: UNVERIFIED — requires formal proof or testing\n\n");
```

(Запази съществуващия печат на guarantees преди това.)

- [ ] **Step 2: main.c `--specs`**

В `--specs` handler-а, по модела на печата на guarantees, добави печат на ensures текстовете (прочети съществуващия код ред 122-147 и следвай неговия стил).

- [ ] **Step 3: Проверка**

Run: `./baga --proofs examples/spec_ensures.baga`
Expected: двата ensures реда се печатат, статус `RUNTIME-CHECKED`.

Run: `./baga --proofs examples/spec.baga`
Expected: статус остава `UNVERIFIED` (spec.baga няма ensures — още).

Run: `./baga --specs examples/spec_ensures.baga`
Expected: ensures изразите се виждат.

---

### Task 5: Примери, Makefile, документация

**Files:**
- Modify: `examples/spec.baga` (добавяне на ensures)
- Modify: `Makefile` (test target, ред 38-56)
- Modify: `docs/language-bg.md` (§14, §17.5)
- Modify: `docs/language-en.md` (съответните секции)

- [ ] **Step 1: `examples/spec.baga`**

Добави ensures към съществуващия spec (секцията остава след `guarantees:`):

```baga
// Spec описва КАКВО прави функцията
spec сортирай {
    input:
        arr: i64
    output: i64
    guarantees:
        - output is sorted
        - output has same elements as input
    ensures:
        output >= arr
}
```

(Тялото на `сортирай` връща `arr`, така че `output >= arr` е изпълнено — примерът остава зелен.)

- [ ] **Step 2: `Makefile` test target**

В `test` target-а добави (съобразено със съществуващия стил на target-а):

```make
	@./baga examples/spec_ensures.baga > /dev/null
	@echo "=== spec_ensures_fail (очакваме runtime грешка) ==="
	@./baga examples/spec_ensures_fail.baga 2>&1 | grep -q "ensures #1 нарушена" \
		&& echo "OK: ensures гаранцията е хваната" \
		|| { echo "FAIL: ensures не е хваната"; exit 1; }
```

- [ ] **Step 3: `docs/language-bg.md`**

В §14 (Система за спецификации), след §14.2, добави нов подраздел:

```markdown
### 14.3 Изпълними гаранции (`ensures:`)

Секцията `ensures:` съдържа булеви изрази на Бага, разделени със запетаи.
В тях са видими имената на input параметрите и `output` — върнатата стойност.
Компилаторът тип-проверява всеки израз (трябва да е `bool`) и го компилира до
runtime проверка, която се изпълнява след всяко повикване на функцията —
включително рекурсивните. При нарушение програмата спира:

```
spec 'удвой': ensures #1 нарушена: output == 2 * x
```

```baga
spec факториел {
    input:
        n: i64
    output: i64
    ensures:
        output > 0,
        n <= 1 || output >= n
}
```

`guarantees:` остава документация в свободен текст (статус UNVERIFIED в
`--proofs`); `ensures:` се изпълнява (статус RUNTIME-CHECKED). `ensures` върху
функция без върнат тип е грешка при компилация. LLVM backend-ът все още не
поддържа `ensures`.
```

В §17.5 (грешки в спецификации) добави редове в таблицата:

```markdown
| `spec '<име>': ensures изисква функция с върнат тип` | `ensures` върху void функция. |
| `spec '<име>': ensures #N е A, очаквах bool` | Ensures изразът не е булев. |
```

- [ ] **Step 4: `docs/language-en.md`**

Същото съдържание на английски в съответните секции (§14.3 Executable guarantees (`ensures:`), таблицата със spec грешки). Преведи точно, запази кодовите блокове и съобщенията за грешки (те си остават на български — така ги издава компилаторът).

- [ ] **Step 5: Финална регресия**

Run: `make clean && make && make test`
Expected: всички примери минават, включително новия очакван-failure прогон.

Run: `./baga --emit-llvm examples/spec_ensures.baga > /dev/null 2>&1; echo exit=$?` (само ако `baga-llvm` съществува)
Expected: документирано ограничение — ако LLVM backend-ът не поддържа ensures, поведението е както досега (без проверки); не е регресия.

---

## Self-Review бележки (изпълнени при писането)

- Spec coverage: синтаксис (T1), тип-проверка + грешки (T2), runtime wrapper + `baga_spec_fail` (T3), `--proofs`/`--specs` RUNTIME-CHECKED (T4), примери/Makefile/docs (T5). LLVM и self-hosted — изрично извън обхват в спецификацията.
- Типова консистентност: `NODE_ENSURE{ensure_text, ensure_expr}` и `spec_ensures` се ползват еднакво в T1–T4; `baga_spec_fail(const char*, int64_t, const char*)` единствено в T3.
- Известен тънък момент: `parse_expr` прототип в parser.c (T1 Step 5) — имплементаторът проверява и добавя forward declaration само ако липсва.
