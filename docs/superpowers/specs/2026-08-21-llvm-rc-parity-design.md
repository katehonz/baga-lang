# Дизайн: RC + drop паритет в LLVM бекенда

Дата: 2026-08-21
Статус: предложен

## Контекст и проблем

Бага има два бекенда: C транспилер (default) и LLVM IR. Паметовият модел —
bump арена + ръчен `drop()` + opt-in `--rc` refcounting (RC1–RC5) — е
имплементиран само в C бекенда. Конкретни дългове:

- `drop(x)` в LLVM бекенда е compile-time грешка:
  `llvm_unsupported("drop — само C бекенда")`, `src/codegen_llvm.c:4156-4158`.
- `--rc` се подава само на C codegen (`src/main.c:371,388`); за LLVM пътя
  тихо се игнорира.
- LLVM heap обектите нямат никакъв header — директен `malloc` чрез
  `rt_malloc` (`src/codegen_llvm.c:412`), без `baga_Hdr`, арена или epoch.
- LLVM symbol таблицата `lg_st` е плоска (`src/codegen_llvm.c:321-344`) —
  няма scope стек, необходим за release при изход от scope.

Документацията изрично бележи това като ограничение
(`docs/language-bg.md:1022`, `docs/memory-rc-bg.md:52`).

## Цел

`./baga --emit-llvm --rc` да дава същото RC поведение като C бекенда;
`drop(x)` да работи в LLVM бекенда (с и без `--rc`); LLVM oracle-ът и
RC тестовете да минават в LLVM режим.

## Не-цели (паритет, не надграждане)

Същите leak-safe граници като C бекенда: closure captures, циклични
структури, temp-ове в try/catch и match рамена — не се покриват.
Leak-диагностика (пълен MEM-3) е отделен бъдещ проект.
Без `--rc` LLVM поведението остава побайтово същото като днес.

## Архитектура

Огледален порт на C RC модела. Pet компонента:

### 1. Header layout в LLVM

Под `--rc` всяка heap алокация получава 32 B `baga_Hdr` пред payload:
`{ magic, pe, rc, an }` (pe = epoch<<1|persist, an = класов размер),
идентичен с C бекенда (`src/codegen_c.c:5291`). Range guard + magic правят
C литерали/външни буфери „immortal" no-op, както в C. Без `--rc` —
днешният plain malloc layout без промяна.

### 2. RC runtime helper-и като LLVM IR

Емитират се в модула при `--rc` (аналог на C preamble-а,
`src/codegen_c.c:5399-5660`):

- `baga_rc_hdr`, `baga_rc_retain`
- `baga_rc_release_str`, `baga_rc_release_bytes`,
  `baga_rc_release_vec(elem_kind, elem_size, elem_rel)`, `baga_rc_release_map`
- per-struct `baga_rc_retain_<S>` / `baga_rc_release_<S>` и per-enum
  `switch(tag)` variant-и (порт на `emit_rc_struct_helpers`,
  `src/codegen_c.c:4558` и `emit_rc_enum_helpers`, `:4657`)

### 3. Scope tracking в codegen_llvm

Нов scope стек, еквивалентен на `rc_locals` / `rc_scopes` / `rc_fn_base`
(`include/baga.h:546-595`, emission в `src/codegen_c.c:560-606`):

- `rc_push_scope` / `rc_pop_scope` при блокове; release на heap локали в
  обратен ред при изход; loop тела — per-итерация.
- `rc_release_all` при `return` (return е move).
- `rc_release_to_loop` при `break`/`continue`.
- Параметрите са borrowed — не се release-ват.
- Контейнери: retain при insert, release при remove (Vec/Map builtin-ите).

### 4. drop

Премахва се `llvm_unsupported` guard-ът на `src/codegen_llvm.c:4156-4158`:

- Под `--rc`: типосъобразен `baga_rc_release_*` + binding dead маркер;
  повторен drop → чиста rc-underflow грешка + exit (като C).
- Без `--rc`: `baga_drop_*` (free) семантика, паралелна на C без rc
  (`src/codegen_c.c:6188-6214`).

`mem_mark`/`mem_rewind`/`mem_persist_*` остават `unsupported` в LLVM —
извън обхвата.

### 5. CLI

`src/main.c` подава `--rc` и на LLVM пътя. `--emit-llvm --rc` вече не се
игнорира.

## Поток на данните

Конструктор/литерал → alloc с header, rc=1, собственик първият binding →
`let x = y` → retain → scope exit → release в обратен ред → rc=0 → free.
Идентично с C бекенда; споделената семантика е документирана в
`docs/memory-rc-bg.md:122-145`.

## Грешки

- rc-underflow (двоен drop / release на dead binding) → ясна грешка + exit,
  не UB — същото като C.
- use-after-drop в codegen не се проверява (територия на `--verify`, MEM-2)
  — без промяна.

## Тестване

- Всички `tests/*_rc_test.baga` (`rc_test`, `temp_test`, `move_test`,
  `borrow_test`, `cmove_test`, `struct_rc_test`, `enum_rc_test`,
  `enum_box_rc_test`, `vecvec_rc_test`, `nested_assign_rc_test`,
  `calltemp_rc_test`, `owned_ret_rc_test`, `match_temp_rc_test`) трябва да
  минат компилирани с `--emit-llvm --rc`.
- `tests/llvm_oracle.sh` — разширява се с RC вариант (oracle паритет C↔LLVM
  под `--rc`).
- `make test` и `make test-llvm` без регресии (LLVM без `--rc` непроменен).

## Рискове

- Scope стекът е най-голямата нова машинерия в LLVM codegen — риск от
  пропуснат release път (ранен return във вложен блок, break през няколко
  scope-а). Оракулът C↔LLVM е основната защита.
- Vec/Map builtin-ите в LLVM пътя (`baga_vec_new` и пр., около
  `src/codegen_llvm.c:998+`) трябва да станат header-aware само под `--rc` —
  внимание да не се счупи не-RC layout-ът.
