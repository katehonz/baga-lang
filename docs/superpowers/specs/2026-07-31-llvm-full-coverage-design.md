# LLVM backend: пълно покритие (str, Vec, struct, ефекти) — Дизайн

> Дата: 2026-07-31. Статус: одобрен (auto mode). Продължение на
> `2026-07-31-llvm-backend-fix-design.md`. Цел: 0 SKIP в оракула.

## Принцип

Същият модел като `baga_spec_fail`: вградените C helpers от preamble-а на
`codegen_c.c` се генерират като LLVM IR функции веднъж (lazy, при първа
употреба), и builtin повикванията в `emit_expr_llvm` се map-ват към тях.
Наблюдаемото поведение трябва да съвпада БАЙТ ЗА БАЙТ с C backend-а (оракулът
diff-ва). Желязното правило остава: неподдържано → грешка, не тиха стойност.

## Обхват

### 1. Str builtins (маха `strings.baga` SKIP)
IR реализации (i8* + i64), огледало на C версиите в codegen_c.c:759-799:
`baga_len` (strlen цикъл), `baga_char_at`, `baga_substr` (malloc+memcpy),
`baga_concat`, `baga_str_eq` (strcmp цикъл), `baga_chr`, `baga_ord`.
`print(str)` вече работи. Мапване: `len`→`baga_len` и т.н. в NODE_CALL.

### 2. Vec (маха `vec.baga` SKIP)
`baga_Vec` като named struct в IR (`{ ptr, i64, i64 }`, opaque ptr за data),
IR реализации на `baga_vec_new/grow/push_i64/get_i64/set_i64/push_str/
get_str/set_str/len` — директен превод на C телата (malloc/realloc,
GEP+load/store). Типът `Vec` в Бага ↔ `baga_Vec*` в IR.

### 3. Struct-ове (маха `tochka.baga` SKIP)
`LLVMStructCreateNamed` + field index map (struct име → полета от
`program->items`). `NODE_STRUCT_LIT` → alloca + store по полета;
`NODE_FIELD` → GEP + load; struct като параметър — както C backend-ът:
по стойност (първокласен struct тип в LLVM; свери codegen_c — той подава
C struct по стойност, така че LLVM struct by-value е еквивалент).

### 4. Ефекти: `?`, `catch`, `read_file` (маха `effects.baga` SKIP)
Ефектите са compile-time тагове — checker-ът вече ги е наложил, в runtime
няма propagation. Затова: `NODE_TRY` → emit вътрешния израз; `NODE_CATCH` →
emit вътрешния израз (handler-ът е мъртъв код при сегашните builtins —
свери с codegen_c какво точно emit-ва той и направи същото).
`baga_read_file` → IR реализация (fopen/fseek/ftell/fread/fclose чрез
libc декларации).

## Извън обхвата

`&`/`*` референции, масиви `[T]`, match върху не-i64. Ако някой пример ги
иска — честен отказ, не тиха стойност.

## Приемливост

`./tests/llvm_oracle.sh`: **14/14 OK** (0 SKIP). `make test` зелен.
