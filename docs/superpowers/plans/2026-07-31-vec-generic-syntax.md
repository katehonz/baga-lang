# `Vec<T>` в типовите анотации — План

> **For agentic workers:** Steps use checkbox (`- [ ]`) syntax. БЕЗ git мутации.

**Goal:** `Vec<T>` е валиден тип в параметри, `let` анотации, връщани типове и spec input/output; checker-ът носи елементния тип през анотациите; елементите са ограничени до i64/str.

**Spec:** `docs/superpowers/specs/2026-07-31-vec-generic-syntax-design.md`

**Ключови файлове:** `src/parser.c` (`parse_type`, ~ред 214-260), `src/checker.c` (`resolve_type_node` ~ред 91, `type_eq`, `type_str`), `src/codegen_c.c` (`emit_type`), `src/codegen_llvm.c` (`llvm_type`), `include/baga.h` (NODE_TYPE union — `type_name` + `inner_type`).

## Global Constraints

- Адитивно: `Vec` без `<T>` работи както досега (i64 fallback в `vec_get`).
- Грешки на български. Оракул 14/14. `make test` зелен. Без git мутации.

---

### Task 1: Парсер + checker

**Files:** Modify `src/parser.c`, `src/checker.c`

- [ ] **Step 1: Тестови файлове (първо)**

`/tmp/vec_ann_pos.baga`:

```baga
fn сума(v: Vec<i64>) -> i64 {
    let mut s: i64 = 0
    for i in 0..vec_len(v) {
        s += vec_get(v, i)
    }
    return s
}

fn първи(sv: Vec<str>) -> str {
    return vec_get(sv, 0)
}

fn main() {
    let v = vec_new()
    vec_push(v, 10)
    vec_push(v, 20)
    print(сума(v))
    let sv = vec_new()
    vec_push(sv, "здравей")
    print(първи(sv))
}
```

`/tmp/vec_ann_neg.baga`: като позитива, но в `сума` добави `vec_push(v, "x")` → очаквана грешка `елемент от тип str, но векторът е Vec<i64>`.

`/tmp/vec_ann_bad.baga`: `fn f(v: Vec<f64>) -> i64 { return 0 }` + main → очаквана грешка `Vec<T>: неподдържан елементен тип f64 (поддържат се i64 и str)`.

- [ ] **Step 2: parser.c parse_type**

След като се парсне именуван тип (`NODE_TYPE` с type_name), ако името е `"Vec"` и следва `<` (`match(p, TOK_LT)`): `inner_type = parse_type(p)`, `expect(p, TOK_GT)`. Провери как `>` се токенизира (TOK_GT съществува; внимавай `>>` — не е приложимо тук). Ако parse_type вече е рекурсивен за REF/ARRAY, следвай същия модел.

- [ ] **Step 3: checker.c resolve_type_node + type_eq**

В `resolve_type_node`: клонът за `Vec` (или добавката към него) — ако `inner_type`, resolve-ни го; ако не е I64/STR (I32 приеми като I64) → `check_error(ctx, node->pos, "Vec<T>: неподдържан елементен тип %s (поддържат се i64 и str)", ...)`; иначе задай `elem`. (Внимание: `resolve_type_node` може да няма достъп до CheckCtx — ако е чиста функция без ctx, връщай TYPE с elem и остави валидацията на елемента на мястото, където има ctx — намери правилното място в реалния код.)

В `type_eq`: TYPE_VEC срещу TYPE_VEC — ако и двата имат `elem` и elem->kind се различава → 0; иначе 1.

- [ ] **Step 4: Проверка**

Run: `make && ./baga /tmp/vec_ann_pos.baga` → `30` и `здравей`.
Run: `./baga /tmp/vec_ann_neg.baga; echo exit=$?` → грешката за смесване, exit=1.
Run: `./baga /tmp/vec_ann_bad.baga; echo exit=$?` → грешката за f64, exit=1.
Run: `make test` → зелен (регресия).

---

### Task 2: Codegen двата backend-а

**Files:** Modify `src/codegen_c.c`, `src/codegen_llvm.c`

- [ ] **Step 1:** `emit_type` (C) и `llvm_type` (LLVM): `NODE_TYPE` "Vec" с `inner_type` → същият резултат като `Vec` без параметър (игнорирай inner — представянето е еднакво). Провери и NODE_TYPE_EFFECT unwrap пътя (тип `Vec<i64> !IO`).

- [ ] **Step 2: Проверка**

Run: `make llvm && ./baga-llvm --emit-llvm /tmp/vec_ann_pos.baga > /tmp/a.ll && lli-14 /tmp/a.ll` → `30` и `здравей`.
Run: `./tests/llvm_oracle.sh` → 14/14 OK.

---

### Task 3: Пример, Makefile, документация

**Files:** Modify `examples/vec.baga` (или нов `examples/vec_ann.baga`), `Makefile`, `docs/language-bg.md`, `docs/language-en.md`

- [ ] **Step 1:** Нов `examples/vec_ann.baga` — съдържанието на `/tmp/vec_ann_pos.baga` + spec с `input: v: Vec<i64>` и един `ensures` (демонстрира Vec<T> в spec). Добави го в Makefile `test` target-а (ще влезе и в оракула автоматично — 15 примера).

- [ ] **Step 2:** docs §4 (типова таблица) и §12.4: `Vec<T>` синтаксис, ограничение i64/str, поведение при липса на `<T>` (i64 fallback в vec_get). EN версията също.

- [ ] **Step 3:** Run: `make clean && make && make llvm && make test` — всичко зелено, оракул 15/15.

---

## Self-Review бележки

- Coverage: парсер (T1S2), resolve/type_eq (T1S3), codegen двата (T2), пример/docs (T3).
- Тънки места: (1) `resolve_type_node` ctx достъп — провери сигнатурата; (2) `<`/`>` токени в типова позиция не се бъркат със сравнения, защото parse_type е отделен контекст; (3) spec_ensures на функция с Vec<i64> input — `--test-specs` я пропуска (spec_inputs_testable приема само i64/bool — увери се, че не гърми, а пропуска); (4) `Vec<str>` в оракула упражнява и LLVM пътя.
