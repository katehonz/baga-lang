# LLVM backend: пълно покритие (str, Vec, struct, ефекти) — План

> **For agentic workers:** Steps use checkbox (`- [ ]`) syntax.
> **ВАЖНО:** БЕЗ `git commit`/`git add`.

**Goal:** Всички примери в оракула стават OK — str builtins, Vec, struct-ове и ефектите (`?`/`catch`/`read_file`) работят в LLVM backend-а с наблюдаемо поведение, идентично на C backend-а.

**Architecture:** Builtin helpers се генерират като LLVM IR функции lazy (при първа употреба), по модела на baga_spec_fail от предишния план. Struct-ове: named LLVM struct + field index map. Ефекти: try/catch са pass-through (compile-time тагове).

**Spec:** `docs/superpowers/specs/2026-07-31-llvm-full-coverage-design.md`
**Референции:** `src/codegen_c.c:759-799` (C телата на helpers-ите — превеждаш ги 1:1 в IR), текущият `src/codegen_llvm.c` (поправеният backend със symtab, lazy helpers, llvm_unsupported), `tests/llvm_oracle.sh` (acceptance).

## Global Constraints

- LLVM 14 C API; `make llvm` без нови warnings; нула тихи стойности — неподдържаното остава `llvm_unsupported`.
- Наблюдаемо поведение = БАЙТ ЗА БАЙТ с C backend-а (oracle diff).
- Без git мутации. Регресия: `make test` (включва оракула).

---

### Task 1: Str builtins в IR

**Files:** Modify `src/codegen_llvm.c`

**Interfaces:** Consumes — текущият backend; Produces — `static LLVMValueRef baga_rt(const char *name)` lazy builder, който при първа употреба генерира IR тялото на съответния helper и кешира `LLVMValueRef`-а.

- [ ] **Step 1: Lazy helper инфраструктура**

Механизъм: `baga_rt(name)` — ако функцията вече е в модула (`LLVMGetNamedFunction`), върни я; иначе създай прототипа ѝ, после emit-ни тялото с builder-а (запази/възстанови текущия insert block с `LLVMGetInsertBlock`/`LLVMPositionBuilderAtEnd`).

- [ ] **Step 2: IR тела**

Преведи 1:1 от codegen_c.c preamble: `baga_len`, `baga_char_at`, `baga_substr` (malloc+memcpy), `baga_concat`, `baga_str_eq`, `baga_chr`, `baga_ord`. libc `malloc`/`memcpy` — декларации с правилните типове. Циклите: същия basic block pattern като NODE_WHILE емисията.

- [ ] **Step 3: Мапване в NODE_CALL**

В `emit_expr_llvm` NODE_CALL: преди user-function lookup-а — ако името е str builtin → `baga_rt("baga_<име>")` (map таблица: `len`→`baga_len`, `char_at`→`baga_char_at`, `substr`→`baga_substr`, `concat`→`baga_concat`, `str_eq`→`baga_str_eq`, `chr`→`baga_chr`, `ord`→`baga_ord`). Тип `str` в llvm_type вече е ptr — ОК.

- [ ] **Step 4: Проверка**

Run: `make llvm && ./tests/llvm_oracle.sh`
Expected: `strings.baga` → OK. Останалите редове непроменени.

---

### Task 2: Vec в IR

**Files:** Modify `src/codegen_llvm.c`

- [ ] **Step 1: baga_Vec тип + helpers**

Named struct `baga_Vec` = `{ i8* data, i64 len, i64 cap }` (data е `void**` в C — в IR `i8*` като opaque е достатъчен, с биткастове при достъп — или `i8**`; избери каквото LLVM 14 приема чисто). IR тела на: `baga_vec_new` (malloc struct + malloc 8 ptr-а), `baga_vec_grow` (realloc), `baga_vec_push_i64` (intptr_t cast trick като C: `inttoptr`/`ptrtoint`), `baga_vec_get_i64`, `baga_vec_set_i64`, `baga_vec_push_str`, `baga_vec_get_str`, `baga_vec_set_str`, `baga_vec_len`.

- [ ] **Step 2: Типове и мапване**

`llvm_type`/`llvm_type_resolved`: `TYPE_VEC`/„Vec" → pointer към baga_Vec. NODE_CALL map: `vec_new`→`baga_vec_new`, `vec_len`→`baga_vec_len`, `vec_push`→`baga_vec_push_i64`, `vec_get`→`baga_vec_get_i64`, `vec_set`→`baga_vec_set_i64`, `vec_push_str`→`baga_vec_push_str`, `vec_get_str`→`baga_vec_get_str`, `vec_set_str`→`baga_vec_set_str`.

- [ ] **Step 3: Проверка**

Run: `make llvm && ./tests/llvm_oracle.sh`
Expected: `vec.baga` → OK.

---

### Task 3: Struct-ове

**Files:** Modify `src/codegen_llvm.c`

- [ ] **Step 1: Типове + field map**

В codegen_llvm първи проход по `program->items`: за всеки NODE_STRUCT — `LLVMStructCreateNamed` + `LLVMStructSetBody` с типовете на полетата; field index map (struct име → списък поле→индекс; може и просто линейно търсене по program items при нужда — минимално). `llvm_type` за NODE_TYPE с struct име → този тип.

- [ ] **Step 2: Литерал и достъп**

`NODE_STRUCT_LIT`: alloca + store на всяко поле (GEP индекси 0..n), върни заредената стойност (struct by value). `NODE_FIELD`: GEP върху alloca на обекта — ако обектът не е alloca, spill-вай във временна alloca (променливи вече са alloca-та, така че това е рядкост). Struct параметри/връщане: by value (свери codegen_c — той подава C struct по стойност).

- [ ] **Step 3: Проверка**

Run: `make llvm && ./tests/llvm_oracle.sh`
Expected: `tochka.baga` → OK. (`print` на f64 резултата вече работи от предишния план.)

---

### Task 4: Ефекти — `?`, `catch`, `read_file`

**Files:** Modify `src/codegen_llvm.c`

- [ ] **Step 1: try/catch pass-through**

`NODE_TRY` → `emit_expr_llvm(n->try_expr)`. `NODE_CATCH` → прочети първо какво emit-ва codegen_c за catch (дали handler-ът се оценява лениво/изобщо) и направи наблюдаемо същото. Ако C backend-ът просто emit-ва вътрешния израз — същото тук.

- [ ] **Step 2: baga_read_file в IR**

libc декларации: `fopen`, `fseek`, `ftell`, `fread`, `fclose` (с правилните типове; FILE* = i8* opaque). IR тяло — превод на C версията (включително `return ""` при NULL — empty string global). Map `read_file`→`baga_read_file` в NODE_CALL.

- [ ] **Step 3: Проверка — финална**

Run: `make llvm && ./tests/llvm_oracle.sh`
Expected: **14/14 OK, 0 SKIP** — включително `effects.baga`.

Run: `make clean && make && make llvm && make test` — всичко зелено.

---

### Task 5: Документация

**Files:** Modify `docs/compiler-bg.md`, `docs/compiler-en.md`

- [ ] **Step 1:** Обнови LLVM секцията: пълното покритие (str/Vec/struct/effects), оракул 14/14. Ако в language-*.md има остарели изречения за LLVM ограничения — коригирай.

- [ ] **Step 2:** Run: `make test` — зелен.

---

## Self-Review бележки

- Spec coverage: str (T1), Vec (T2), struct (T3), effects (T4), docs (T5). Приемливост 14/14 (T4 S3).
- Тънки места: (1) intptr cast трика на Vec (void* съдържа i64) — `ptrtoint`/`inttoptr` точно като C; (2) struct by value — свери с codegen_c; (3) catch семантика — прочети codegen_c ПРЕДИ да пишеш; (4) builder insert block save/restore около lazy helper емисия; (5) `baga_rt` кеширане, за да не се дублират тела.
