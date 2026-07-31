# Предусловия в spec (`requires:`) — Имплементационен план

> **For agentic workers:** Steps use checkbox (`- [ ]`) syntax.
> **ВАЖНО:** БЕЗ `git commit`/`git add` — git мутации изискват изрично одобрение от потребителя.

**Goal:** Секция `requires:` в `spec` — булеви предусловия, тип-проверени при компилация и изпълнявани преди тялото на функцията; нарушение спира програмата.

**Architecture:** Симетрично на `ensures:` (вече имплементирано). Същите `NODE_ENSURE` възли (текст + израз), нов `NodeVec spec_requires` в NODE_SPEC. Checker: scope само от input имената. Codegen: wrapper-ът проверява requires преди повикването на impl; работи и за void функции.

**Tech Stack:** C99, gcc, make. Нула зависимости.

**Spec:** `docs/superpowers/specs/2026-07-31-spec-requires-design.md`
**Референция:** предишният план `docs/superpowers/plans/2026-07-31-executable-spec-ensures.md` — структурата на промените е същата; чети реалния текущ код (ensures вече е в силата) и го следвай.

## Global Constraints

- Нула външни зависимости; грешките на български; формат `файл: ред:колона: съобщение`.
- Съществуващото ensures съобщение `spec '<s>': ensures #N нарушена: <expr>` НЕ се променя (Makefile grep-ва `ensures #1 нарушена`).
- `requires` е позволен и върху void функции (за разлика от `ensures`).
- LLVM backend и `self/*.baga` — извън обхват. БЕЗ git мутации.
- Build: `make`. Регресия: `make test`.

---

### Task 1: AST и парсер — секция `requires:`

**Files:**
- Modify: `include/baga.h` (NODE_SPEC union member)
- Modify: `src/parser.c` (`node_free`, `parse_spec`, AST printer)

**Interfaces:**
- Produces: `Node->spec_requires` (`NodeVec` от `NODE_ENSURE` възли — преизползва се същият node kind: `ensure_text` + `ensure_expr`).

- [ ] **Step 1: Примери (тестовете)**

Създай `examples/spec_requires_fail.baga`:

```baga
// requires се проверява ПРЕДИ тялото — нарушението спира програмата веднага
spec корен {
    input:
        x: i64
    output: i64
    requires:
        x >= 0
    ensures:
        output >= 0
}

fn корен(x: i64) -> i64 {
    if x < 0 { return -1 }
    return x
}

fn main() {
    print(корен(9))
    print(корен(-5))
}
```

В `examples/spec_ensures.baga` добави `requires` секция към spec-а (между `output:` и `ensures:`):

```baga
spec факториел {
    input:
        n: i64
    output: i64
    requires:
        n >= 0
    ensures:
        output > 0,
        n <= 1 || output >= n
}
```

- [ ] **Step 2: `include/baga.h`**

В NODE_SPEC struct-а, след `NodeVec spec_ensures;`, добави:

```c
            NodeVec spec_requires; /* NODE_ENSURE — предусловия */
```

- [ ] **Step 3: `src/parser.c`**

а) В `node_free`, case NODE_SPEC — след освобождаването на `spec_ensures`:

```c
            for (int i = 0; i < n->spec_requires.len; i++) node_free(n->spec_requires.data[i]);
            vec_free(n->spec_requires);
```

б) В `parse_spec`: локален `NodeVec requires = {0};` до `ensures`; `"requires"` в stop-условието на input-секцията (където вече са `"output"`, `"guarantees"`, `"ensures"`); нов клон за `requires:` — идентичен с ensures клона, но `vec_push(requires, en);`; в края `s->spec_requires = requires;`.

в) В guarantees gobbler-а (while-цикълът, който събира свободен текст до `-`/`}`/EOF): добави stop и на ident `"requires"`, по същия модел, по който вече спира на `"ensures"` (прочети текущия код — той вече има такава проверка за ensures).

г) AST printer, case NODE_SPEC: след ensures редовете добави:

```c
            for (int i = 0; i < n->spec_requires.len; i++)
                fprintf(stderr, "  requires: %s\n", n->spec_requires.data[i]->ensure_text);
```

- [ ] **Step 4: Проверка**

Run: `make && ./baga --ast examples/spec_requires_fail.baga 2>&1 | grep -E "requires|ensures"`
Expected: `requires: x >= 0` и `ensures: output >= 0`.
Run: `./baga examples/spec_ensures.baga` → `3628800` (requires още не се изпълнява). `make test` минава.

---

### Task 2: Checker — тип-проверка на requires

**Files:**
- Modify: `src/checker.c` (`check_program`, pass 2 — след блока за ensures)

**Interfaces:**
- Consumes: `spec_requires` от Task 1; съществуващите `push_scope/pop_scope/env_define/infer/resolve_type_node/type_str`.
- Produces: `spec '<име>': requires #N е <тип>, очаквах bool`; `output` в requires → `недефинирана променлива 'output'`.

- [ ] **Step 1: Негативен тест (първо)**

Създай `/tmp/requires_bad.baga`:

```baga
spec лоша {
    input:
        x: i64
    output: i64
    requires:
        x + 1
}

fn лоша(x: i64) -> i64 { return x }

fn main() { print(лоша(1)) }
```

- [ ] **Step 2: Имплементация**

В `check_program` pass 2, веднага след блока „check ensures expressions", добави:

```c
        /* check requires expressions (scope: inputs only, no output) */
        if (item->spec_requires.len > 0) {
            push_scope(&ctx);
            for (int j = 0; j < item->spec_inputs.len; j++) {
                Node *sp = item->spec_inputs.data[j];
                env_define(&ctx, sp->param_name,
                           resolve_type_node(sp->param_type), sp->pos);
            }
            for (int j = 0; j < item->spec_requires.len; j++) {
                Node *rq = item->spec_requires.data[j];
                Type *rt = infer(&ctx, rq->ensure_expr);
                if (rt->kind != TYPE_BOOL && rt->kind != TYPE_ERROR) {
                    check_error(&ctx, rq->pos,
                        "spec '%s': requires #%d е %s, очаквах bool",
                        item->spec_name, j + 1, type_str(rt));
                }
            }
            pop_scope(&ctx);
        }
```

- [ ] **Step 3: Проверка**

Run: `make && ./baga /tmp/requires_bad.baga; echo exit=$?`
Expected: `spec 'лоша': requires #1 е i64, очаквах bool`, `exit=1`.
Run: `./baga examples/spec_ensures.baga` → `3628800`. `make test` минава.

---

### Task 3: Codegen — requires в wrapper-а, `baga_spec_fail` с kind

**Files:**
- Modify: `src/codegen_c.c` (preamble `baga_spec_fail`; `find_ensures_spec`; wrapper-ът в `emit_fn`)

**Interfaces:**
- Consumes: `spec_requires`; съществуващият wrapper механизъм от ensures.
- Produces: `baga_spec_fail(const char *spec, const char *kind, int64_t idx, const char *expr)`; wrapper за всяка функция, чийто spec има requires ИЛИ ensures (включително void функции с requires).

- [ ] **Step 1: Потвърди, че днес нарушението минава**

Run: `./baga examples/spec_requires_fail.baga`
Expected: печата `9` и `-5` без грешка (requires не се изпълнява още).

- [ ] **Step 2: Preamble — нова сигнатура на `baga_spec_fail`**

Замени текущия emit на `baga_spec_fail` с:

```c
    fprintf(out, "static void baga_spec_fail(const char *spec, const char *kind, int64_t idx, const char *expr) {\n");
    fprintf(out, "    if (strcmp(kind, \"requires\") == 0)\n");
    fprintf(out, "        fprintf(stderr, \"spec '%%s': requires #%%lld нарушено: %%s\\n\", spec, (long long)idx, expr);\n");
    fprintf(out, "    else\n");
    fprintf(out, "        fprintf(stderr, \"spec '%%s': ensures #%%lld нарушена: %%s\\n\", spec, (long long)idx, expr);\n");
    fprintf(out, "    exit(1);\n");
    fprintf(out, "}\n");
```

- [ ] **Step 3: `find_ensures_spec` → обобщи условието**

Прочети текущата функция. Промени условието на:

```c
        if (it->kind == NODE_SPEC && strcmp(it->spec_name, fn_name) == 0 &&
            (it->spec_ensures.len > 0 || it->spec_requires.len > 0))
```

(Преименуването на `find_contract_spec` е по твой вкус — ако го правиш, смени и повикването.)

- [ ] **Step 4: `emit_fn` — wrapper за requires/void**

Прочети текущия wrapper код (от предишния план). Промени:

а) Условието за wrapper: в момента е `(fn->ret_type && fn->fn_body) ? find_ensures_spec(...) : NULL`. Смени на `(fn->fn_body) ? find_ensures_spec(cg, fn->fn_name) : NULL;` — wrapper има и за void функции с requires. (Ensures върху void е грешка от checker-а, така че до codegen не стига.)

б) В тялото на wrapper-а, ПРЕДИ повикването на impl, добави requires проверките:

```c
    /* провери предусловията преди повикването */
    for (int j = 0; j < ensures_spec->spec_requires.len; j++) {
        Node *rq = ensures_spec->spec_requires.data[j];
        emit_indent(cg);
        fprintf(f, "if (!(");
        emit_expr(cg, rq->ensure_expr);
        fprintf(f, ")) baga_spec_fail(\"");
        emit_c_string(f, ensures_spec->spec_name);
        fprintf(f, "\", \"requires\", %d, \"", j + 1);
        emit_c_string(f, rq->ensure_text);
        fprintf(f, "\");\n");
    }
```

(Съобрази се с реалната форма на `emit_c_string` — тя emit-ва и кавичките; виж как ensures проверките я ползват и следвай същия модел.)

в) Ensures проверките: добави `\"ensures\", ` като втори аргумент на `baga_spec_fail` в съществуващия цикъл.

г) Void случай: когато `fn->ret_type == NULL` (само requires е възможен), wrapper-ът НЕ декларира `b_output`; вместо това:

```c
    /* void функция: само повикай impl след предусловията */
    emit_indent(cg);
    char *im = mangle_name(impl_name_buf);
    fprintf(f, "%s(", im);
    free(im);
    /* args по spec input имената, както при не-void */
    ...
    fprintf(f, ");\n");
    emit_indent(cg);
    fprintf(f, "return;\n");
```

Раздели съществуващия не-void път с `if (fn->ret_type) { ... b_output/ensures/return b_output ... } else { ... горното ... }`.

д) Impl името и `static` префиксът остават както са.

- [ ] **Step 5: Проверка**

Run: `make && ./baga examples/spec_requires_fail.baga; echo exit=$?`
Expected: печата `9`, после stderr `spec 'корен': requires #1 нарушено: x >= 0`, `exit=1`. (`print(корен(-5))` никога не се изпълнява; `-5` НЕ се печата.)

Run: `./baga examples/spec_ensures.baga` → `3628800` (requires n >= 0 минава за всички рекурсивни повиквания).

Run: `./baga examples/spec_ensures_fail.baga; echo exit=$?`
Expected: непроменено — `ensures #1 нарушена: output == 2 * x`, `exit=1`.

Run: `make test` → зелено.

Run: `./baga --emit-c examples/spec_requires_fail.baga | grep -c "baga_spec_fail"` → 2 (една requires + една ensures проверка).

---

### Task 4: `--proofs`, `--specs`, примери, Makefile, документация

**Files:**
- Modify: `src/proofs.c`, `src/main.c`, `Makefile`, `docs/language-bg.md`, `docs/language-en.md`

- [ ] **Step 1: proofs.c**

По модела на ensures печата добави:

```c
        for (int j = 0; j < spec->spec_requires.len; j++)
            printf("    requires: %s\n", spec->spec_requires.data[j]->ensure_text);
```

Статус условието става: `if (spec->spec_ensures.len > 0 || spec->spec_requires.len > 0)` → `RUNTIME-CHECKED`, иначе `UNVERIFIED ...`.

- [ ] **Step 2: main.c `--specs`**

Добави печат на requires редовете по модела на ensures.

- [ ] **Step 3: Makefile**

В `test` target-а, след съществуващия ensures probe, добави:

```make
	@echo "=== spec_requires_fail (очакваме runtime грешка) ==="
	@./baga examples/spec_requires_fail.baga 2>&1 | grep -q "requires #1 нарушено" \
		&& echo "OK: requires предусловието е хванато" \
		|| { echo "FAIL: requires не е хванато"; exit 1; }
```

- [ ] **Step 4: docs/language-bg.md**

След §14.3 добави:

```markdown
### 14.4 Предусловия (`requires:`)

Секцията `requires:` съдържа булеви изрази върху input параметрите, разделени
със запетаи. Те се тип-проверяват при компилация и се изпълняват **преди**
тялото на функцията при всяко повикване. При нарушение програмата спира:

```
spec 'корен': requires #1 нарушено: x >= 0
```

```baga
spec корен {
    input:
        x: i64
    output: i64
    requires:
        x >= 0
    ensures:
        output >= 0
}
```

`requires` е позволен и върху функции без върнат тип (за разлика от `ensures`,
което изисква `output`). В requires изразите `output` не е видим.
```

В §17.5 таблицата добави ред:

```markdown
| `spec '<име>': requires #N е A, очаквах bool` | Requires изразът не е булев. |
```

- [ ] **Step 5: docs/language-en.md**

Същото на английски (§14.4 Preconditions (`requires:`)); съобщенията за грешки остават на български, както ги издава компилаторът.

- [ ] **Step 6: Финална регресия**

Run: `make clean && make && make test`
Expected: всичко зелено, включително двата очакван-failure probe-а (ensures + requires).

---

## Self-Review бележки

- Spec coverage: синтаксис/AST (T1), тип-проверка (T2), runtime + kind съобщения (T3), proofs/specs/примери/Makefile/docs (T4).
- Консистентност: `spec_requires` + `NODE_ENSURE` преизползване навсякъде; `baga_spec_fail(spec, kind, idx, expr)` — kind ∈ {"requires","ensures"}; старото ensures съобщение непроменено.
- Рискове: void wrapper пътят (T3 Step 4г) е единствената нова кодова разклоненост — покрит е от spec_requires_fail примера само частично (той е не-void); провери ръчно и void случая с временен файл, ако решиш, или го добави в Step 5.
