# Пълна проверка на типовете на аргументите — План

> **For agentic workers:** Steps use checkbox (`- [ ]`) syntax. БЕЗ git мутации.

**Goal:** при извикване на потребителска функция checker-ът проверява типовете
на аргументите срещу декларирания тип на параметрите (днес проверява само
бройката). Несъответствие → compile-time грешка на български.

**Spec:** `docs/superpowers/specs/2026-07-31-arg-type-check-design.md`

**Ключови файлове:** `src/checker.c` (`type_eq` ~ред 46, `infer_call` user-fn
клон ~ред 413-420, `type_str`), `include/baga.h` (`struct Type` — `params`/
`nparams` за TYPE_FN вече съществуват).

## Global Constraints

- Адитивно: променя се само checker-ът; codegen-ът (C и LLVM) не се пипа.
- Грешки на български, съществуващ стил. Оракул 15/15. `make test` зелен.
  `self/*.baga` се компилират. Без git мутации.
- Проверката е до `min(args, nparams)` — да не се удвоява грешката за бройката.

---

### Task 1: Checker — helper + проверка

**Files:** Modify `src/checker.c`

- [ ] **Step 1: Тестови файлове (първо)**

`/tmp/arg_pos.baga` (позитив — трябва да минава):

```baga
fn f(x: i64) -> i64 { return x * 2 }
fn g(s: str) -> str { return s }
fn h(a: i64, b: i64) -> i64 { return a + b }

fn main() {
    print(f(21))
    print(g("здравей"))
    print(h(1, 2))
}
```

`/tmp/arg_neg.baga` (негатив — очаквана compile грешка):

```baga
fn f(x: i64) -> i64 { return x }

fn main() {
    print(f("текст"))
}
```

`/tmp/arg_neg2.baga` (втори аргумент, за индексацията #2):

```baga
fn h(a: i64, b: str) -> str { return b }

fn main() {
    print(h(1, 2))
}
```

- [ ] **Step 2: `type_assignable` helper**

До `type_eq` (checker.c ~ред 46) добави static helper:

```c
static int type_assignable(Type *arg, Type *param) {
    if (!arg || !param) return 1;
    if (arg->kind == TYPE_ERROR || param->kind == TYPE_ERROR) return 1;
    /* цялочислено семейство: i32/i64 са съвместими (int литералите са i64) */
    if ((arg->kind == TYPE_I32 || arg->kind == TYPE_I64) &&
        (param->kind == TYPE_I32 || param->kind == TYPE_I64)) return 1;
    return type_eq(arg, param);
}
```

(Сигнатурата на `type_eq` е `int type_eq(Type*, Type*)` — несъвместимост с
декларацията в baga.h няма, helper-ът е static.)

- [ ] **Step 3: цикъл в `infer_call`**

В user-fn клона (след `if (n->args.len != ft->nparams) {...}`), добави:

```c
            /* check arg types */
            int check_n = n->args.len < ft->nparams ? n->args.len : ft->nparams;
            for (int i = 0; i < check_n; i++) {
                Type *at = n->args.data[i]->type;
                if (!type_assignable(at, ft->params[i])) {
                    check_error(ctx, n->pos,
                        "'%s': аргумент #%d е от тип %s, но параметърът е %s",
                        name, i + 1, type_str(at), type_str(ft->params[i]));
                }
            }
```

- [ ] **Step 4: Проверка**

Run: `make && ./baga /tmp/arg_pos.baga` → `42`, `здравей`, `3`.
Run: `./baga /tmp/arg_neg.baga; echo exit=$?` →
`'f': аргумент #1 е от тип str, но параметърът е i64`, `exit=1`.
Run: `./baga /tmp/arg_neg2.baga; echo exit=$?` →
`'h': аргумент #2 е от тип i64, но параметърът е str`, `exit=1`.
Run: `for f in self/*.baga; do ./baga --emit-c $f >/dev/null || echo "FAIL $f"; done`
→ без FAIL (регресия на self-hosting кода).
Run: `make test` → зелен.

---

### Task 2: Пример, Makefile, документация

**Files:** New `examples/arg_type_bad.baga`, Modify `Makefile`, `docs/language-bg.md`, `docs/language-en.md`

- [ ] **Step 1: negative пример**

`examples/arg_type_bad.baga` — съдържанието на `/tmp/arg_neg.baga` (с коментар
най-отгоре: `// Грешен тип аргумент — checker-ът трябва да откаже`).

- [ ] **Step 2: Makefile probe**

В `test` target-а, до vec_typed probe-а:

```make
	@echo "=== arg_type_bad (очакваме compile грешка) ==="
	@./$(BIN) examples/arg_type_bad.baga 2>&1 | grep -q "аргумент #1 е от тип str, но параметърът е i64" \
		&& echo "OK: проверката на аргументите хвана грешния тип" \
		|| { echo "FAIL: проверката на аргументите не хвана грешния тип"; exit 1; }
```

- [ ] **Step 3: docs §17.3 (BG)**

В таблицата с грешки добави ред:
`| 'f': аргумент #1 е от тип str, но параметърът е i64 | подаден аргумент от несъвместим тип към потребителска функция |`
И кратка бележка в секцията за функции: проверката на аргументите е по тип
(не само бройка), i32/i64 са съвместими.

- [ ] **Step 4: docs §17.3 (EN)**

Същото на английски (грешката остава на български).

- [ ] **Step 5: Финална регресия**

Run: `make clean && make && make llvm && make test`
Expected: всичко зелено (оракъл 15/15 + новия arg_type_bad probe).
Run: `./baga-llvm examples/arg_type_bad.baga; echo exit=$?` → същата грешка, exit 1.

---

## Self-Review бележки

- Coverage: helper (T1S2), цикъл (T1S3), пример/Makefile/docs (T2).
- Тънки места: (1) `min(args, nparams)` — без фалшиви грешки при грешна
  бройка; (2) i32/i64 снизходителност — int литералите са i64, иначе `f(10)`
  би гърмяло срещу i32 параметър; (3) `type_eq` вече връща 1 при TYPE_ERROR —
  няма каскада; (4) struct-ите се сравняват по име (type_eq), Vec е
  снизходителен при неизвестен elem — съществуващите примери (tochka, vec_ann)
  минават; (5) LLVM пътят минава през същия `check_program` — поведението е
  идентично без промяна в codegen_llvm.c.
