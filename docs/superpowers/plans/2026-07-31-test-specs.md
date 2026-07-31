# `--test-specs`: property-based тестване на spec договори — План

> **For agentic workers:** Steps use checkbox (`- [ ]`) syntax.
> **ВАЖНО:** БЕЗ `git commit`/`git add` — git мутации само с изрично одобрение.

**Goal:** `baga --test-specs file.baga` — компилира до C и вместо `main` изпълнява генериран тестов драйвър, който атакува ensures/requires договорите със 100 детерминистични случайни входа; requires е входен филтър, ensures е съдия; контрапримерът се печата с входа.

**Architecture:** Нов флаг `test_specs` в `Codegen`. Preamble-ът получава `baga_cur_args` + принт на входа в `baga_spec_fail`. В test mode `codegen_c` emit-ва допълнително `baga_rand_i64`, по един requires-предикат `b__req_<fn>` на spec и тестов `int main` вместо стандартния. Нормалният път (`baga file.baga`) е непроменен.

**Spec:** `docs/superpowers/specs/2026-07-31-test-specs-design.md`

## Global Constraints

- Нула зависимости; грешки/съобщения на български; съществуващите съобщения на `baga_spec_fail` (ensures/requires) НЕ се променят — само се добавя опционален ред `  вход: ...` след тях.
- Без git мутации. Build: `make`. Регресия: `make test`.
- Поддържат се само spec-ове, чиито input типове са изцяло `i64` и/или `bool`; останалите се пропускат със съобщение.
- Seed и брой тестове: фиксирани константи (100 теста, 10 опита за валиден вход, seed 0x243F6A8885A308D3).

---

### Task 1: Codegen — test mode инфраструктура

**Files:**
- Modify: `include/baga.h` (Codegen struct, ~ред 432)
- Modify: `src/codegen_c.c` (preamble; края на `codegen_c`; нови helpers)

**Interfaces:**
- Produces: `Codegen.test_specs` (int). В test mode генерираният C файл съдържа: `baga_rand_i64`, `b__req_<mangled>` предикати, тестов `main`. Променен `baga_spec_fail` с опционален печат на входа.

- [ ] **Step 1: `include/baga.h`**

В `struct Codegen` добави поле:

```c
    int test_specs;   /* --test-specs: генерирай тестов драйвър вместо main */
```

- [ ] **Step 2: Preamble — вход в `baga_spec_fail`**

В `codegen_c`, веднага преди emit-а на `baga_spec_fail`, добави глобалите:

```c
    fprintf(out, "static int64_t baga_cur_args[16];\n");
    fprintf(out, "static int baga_cur_nargs = 0;\n");
```

В края на `baga_spec_fail` (преди `exit(1);`), добави печат на входа:

```c
    fprintf(out, "    if (baga_cur_nargs > 0) {\n");
    fprintf(out, "        fprintf(stderr, \"  вход: \");\n");
    fprintf(out, "        for (int i = 0; i < baga_cur_nargs; i++)\n");
    fprintf(out, "            fprintf(stderr, \"%%s%%lld\", i ? \", \" : \"\", (long long)baga_cur_args[i]);\n");
    fprintf(out, "        fprintf(stderr, \"\\n\");\n");
    fprintf(out, "    }\n");
```

(Съобрази се с реалния текущ вид на baga_spec_fail — тя има requires/ensures клонове; добави блока веднага преди `exit(1);`.)

- [ ] **Step 3: Helpers за test mode**

След `find_ensures_spec` (или нейния преименуван вариант) добави:

```c
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
```

Забележка: `mangle_name` връща `b_<име>`; `m + 2` дава чистото mangled име. Ако реалният `mangle_name` работи иначе (прочети го — codegen_c.c:27-44), постигни същия резултат: функцията да се казва `b__req_<mangled-без-b_>`.

- [ ] **Step 4: Тестов main + rand в `codegen_c`**

В `codegen_c`, замени финалния блок:

```c
    /* C main → calls baga main */
    fprintf(out, "int main(void) {\n");
    fprintf(out, "    b_main();\n");
    fprintf(out, "    return 0;\n");
    fprintf(out, "}\n");
```

с:

```c
    if (cg->test_specs) {
        emit_test_driver(cg, program);
    } else {
        /* C main → calls baga main */
        fprintf(out, "int main(void) {\n");
        fprintf(out, "    b_main();\n");
        fprintf(out, "    return 0;\n");
        fprintf(out, "}\n");
    }
```

Имплементирай `emit_test_driver` (преди `codegen_c`):

```c
#define BAGA_TEST_COUNT 100
#define BAGA_TEST_TRIES 10

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

    fprintf(f, "int main(void) {\n");
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
            fprintf(f, "    printf(\"");
            emit_c_string(f, it->spec_name);
            fprintf(f, ": пропусната (неподдържан тип за --test-specs)\\n\");\n");
            continue;
        }
        int np = it->spec_inputs.len;
        char *fm = mangle_name(it->spec_name);
        fprintf(f, "    { int passed = 0, skipped = 0;\n");
        fprintf(f, "      for (int t = 0; t < %d; t++) {\n", BAGA_TEST_COUNT);
        fprintf(f, "          int64_t args[%d];\n", np > 0 ? np : 1);
        fprintf(f, "          int ok = 0;\n");
        fprintf(f, "          for (int tr = 0; tr < %d && !ok; tr++) {\n", BAGA_TEST_TRIES);
        for (int j = 0; j < np; j++) {
            Node *pt = it->spec_inputs.data[j]->param_type;
            if (strcmp(pt->type_name, "bool") == 0)
                fprintf(f, "              args[%d] = baga_rand_i64(0, 1);\n", j);
            else
                fprintf(f, "              args[%d] = baga_rand_i64(-1000, 1000);\n", j);
        }
        fprintf(f, "              ok = b__req_%s(", fm + 2);
        for (int j = 0; j < np; j++) fprintf(f, "%sargs[%d]", j ? ", " : "", j);
        fprintf(f, ");\n          }\n");
        fprintf(f, "          if (!ok) { skipped++; continue; }\n");
        fprintf(f, "          baga_cur_nargs = %d;\n", np);
        for (int j = 0; j < np; j++)
            fprintf(f, "          baga_cur_args[%d] = args[%d];\n", j, j);
        fprintf(f, "          (void)%s(", fm);
        for (int j = 0; j < np; j++) fprintf(f, "%sargs[%d]", j ? ", " : "", j);
        fprintf(f, ");\n");
        fprintf(f, "          passed++;\n      }\n");
        fprintf(f, "      printf(\"");
        emit_c_string(f, it->spec_name);
        fprintf(f, ": %%d/%d теста минаха (%%d пропуснати от requires)\\n\", passed, skipped);\n", BAGA_TEST_COUNT);
        fprintf(f, "    }\n");
        free(fm);
    }
    fprintf(f, "    printf(\"Всички spec тестове минаха. ⚔️\\n\");\n");
    fprintf(f, "    return 0;\n}\n");
}
```

Внимание: bool аргументите се подават на функции с `int` параметри — `args[]` е `int64_t`, C конвертира implicit; ОК. Ако `emit_c_string` в реалния код emit-ва и кавичките (тя ги emit-ва — виж я), НЕ слагай допълнителни кавички около извикванията ѝ; горният код вече е писан с това предвид — свери всяко място.

- [ ] **Step 5: `src/main.c` — флаг**

Прочети main.c (ред 45-75 за флаговете, 165-215 за compile-and-run пътя). Добави:
- `int test_specs = 0;` до `emit_llvm`; в цикъла: `else if (strcmp(argv[i], "--test-specs") == 0) { test_specs = 1; }`
- Там, където се конструира/инициализира `Codegen` преди `codegen_c`, задай `cg.test_specs = test_specs;` (ако Codegen се инициализира с `{0}`, полето е 0 по подразбиране — задавай го само когато е 1).
- В usage текста добави ред за `--test-specs`.

- [ ] **Step 6: Проверка на инфраструктурата**

Run: `make && ./baga --emit-c examples/faktorial.baga | grep -c "baga_cur_args"` → ≥1 (глобалите са в preamble).
Run: `make test` → зелено (нормалният път е непроменен).

---

### Task 2: Примерът с домейн ограничение + тестове

**Files:**
- Modify: `examples/spec_ensures.baga`

- [ ] **Step 1: Ограничи домейна на факториел**

В `examples/spec_ensures.baga` смени requires на:

```baga
    requires:
        n >= 0 && n <= 20
```

с коментар над spec-а: `// requires ограничава домейна: i64 прелива при n > 20 — домейнът е част от договора`.

- [ ] **Step 2: Зелен прогон**

Run: `make && ./baga --test-specs examples/spec_ensures.baga; echo exit=$?`
Expected:
```
факториел: 100/100 теста минаха (0 пропуснати от requires)
Всички spec тестове минаха. ⚔️
exit=0
```
(при n в [0,20] след филтъра output > 0 и output >= n винаги важат)

- [ ] **Step 3: Контрапример**

Run: `./baga --test-specs examples/spec_ensures_fail.baga; echo exit=$?`
Expected: `spec 'удвой': ensures #1 нарушена: output == 2 * x`, ред `  вход: <число>`, `exit=1`.

- [ ] **Step 4: Requires филтър**

Run: `./baga --test-specs examples/spec_requires_fail.baga; echo exit=$?`
Expected: `корен: 100/100 ...` (възможно с пропуснати > 0), `exit=0` — за x≥0 impl връща x, ensures output >= 0 важи; невалидните входове се филтрират от requires предиката.

- [ ] **Step 5: Регресия на нормалните съобщения**

Run: `./baga examples/spec_ensures_fail.baga; echo exit=$?`
Expected: същото съобщение както преди (`вход:` ред НЕ се появява — `baga_cur_nargs` е 0), `exit=1`.

---

### Task 3: Makefile, документация

**Files:**
- Modify: `Makefile`, `docs/language-bg.md`, `docs/language-en.md`, `README.md`

- [ ] **Step 1: Makefile**

В `test` target-а добави:

```make
	@echo "=== --test-specs (property-based) ==="
	@./baga --test-specs examples/spec_ensures.baga
	@./baga --test-specs examples/spec_ensures_fail.baga 2>&1 | grep -q "ensures #1 нарушена" \
		&& echo "OK: --test-specs намери контрапример" \
		|| { echo "FAIL: --test-specs не намери контрапример"; exit 1; }
```

- [ ] **Step 2: docs/language-bg.md**

След §14.4 добави §14.5:

```markdown
### 14.5 Property-based тестване (`--test-specs`)

`baga --test-specs файл.baga` не изпълнява `main`, а генерира тестов драйвър,
който вика всяка функция с `ensures`/`requires` 100 пъти с детерминистични
случайни входове (фиксиран seed — повторяеми прогони). `requires` филтрира
невалидните входове (до 10 опита за валиден); нарушение на `ensures` е
контрапример и спира програмата с входа:

```
spec 'удвой': ensures #1 нарушена: output == 2 * x
  вход: -347
```

Поддържат се функции с входове само от тип `i64` и/или `bool`; останалите се
пропускат със съобщение. Домейнът на договора е част от договора — например
`факториел` ограничава `n <= 20`, защото `i64` прелива след това.
```

В CLI таблицата (§18) добави ред за `--test-specs`.

- [ ] **Step 3: docs/language-en.md + README.md**

Същото на английски (§14.5 Property-based testing (`--test-specs`), CLI таблица). В README.md таблицата „CLI Flags" добави:

```markdown
| `--test-specs` | Property-based test of spec contracts (random inputs, deterministic seed) |
```

- [ ] **Step 4: Финална регресия**

Run: `make clean && make && make test`
Expected: всичко зелено, включително новите probe-ове.

---

## Self-Review бележки

- Spec coverage: CLI флаг + Codegen поле (T1 S5, S1), PRNG/предикати/драйвър (T1 S3-S4), печат на вход (T1 S2), домейн на примера (T2 S1), тестове (T2 S2-S5), Makefile/docs/README (T3).
- Консистентност: `b__req_<mangled-без-b_>` се генерира и вика еднакво; `baga_cur_args/baga_cur_nargs` имена еднакви в preamble и драйвър; съобщения `теста минаха (N пропуснати от requires)` еднакви в драйвър и документация.
- Тънки места: `emit_c_string` emit-ва кавички (сверено при имплементация); `m + 2` трика зависи от реалния `mangle_name` — имплементаторът го проверява; `%` в fprintf формати е екраниран като `%%` навсякъде.
