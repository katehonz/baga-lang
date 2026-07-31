# Типизирани вектори `Vec<T>` — Дизайн

> Дата: 2026-07-31. Статус: одобрен (auto mode).
> Проблем: `Vec` е нетипизиран — `vec_push` винаги е за i64, `vec_push_str`
> за str, и checker-ът не проверява аргументите на builtins изобщо. Смесването
> на типове в един вектор мълчаливо „минава" и гръмва в runtime.

## Идея

`Vec` носи тип на елементите в типовата система (без нов синтаксис):

```baga
fn main() {
    let v = vec_new()        // Vec<?>
    vec_push(v, 10)          // Vec<i64> — типът се фиксира при първия push
    vec_push(v, "грешка")    // ГРЕШКА при компилация:
                             // vec_push: елемент от тип str, но векторът е Vec<i64>
    print(vec_get(v, 0))     // i64
}
```

- `vec_new()` → `Vec` с неизвестен елемент (fresh type на всяко повикване).
- Първият `vec_push`/`vec_set` фиксира елементния тип; следващите трябва да
  съвпадат. Поддържани елементи: `i64`, `str` (както досега в runtime).
- `vec_get` връща елементния тип; върху `Vec` с неизвестен елемент → грешка
  `vec_get: типът на вектора не е известен — извикай vec_push първо`.
- `vec_push_str`/`vec_get_str`/`vec_set_str` остават като псевдоними
  (= push/get/set със str) — стар код не се чупи.
- Елементният тип е свойство на типа, изведен на мястото на свързването;
  aliasing през друга променлива не го пренася (документирано ограничение).

## Семантика по компоненти

### Checker (същината)
- `Type TYPE_VEC` ползва съществуващото поле `Type *elem` (вече е в struct Type
  за TYPE_ARRAY — преизползва се; NULL = неизвестен).
- `vec_*` builtins излизат от статичната таблица и получават специална
  обработка в `infer_call` с описаната унификация и български грешки.
- `type_str(Vec)` → `Vec` (елемент NULL) или `Vec<i64>`/`Vec<str>`.
- `type_eq` за два Vec: равни по kind (елементът не се сравнява — така
  `Vec<i64>` може да се подаде на параметър `v: Vec`).

### Codegen C и LLVM
- `vec_push(v, x)`: изборът на helper (`baga_vec_push_i64` vs
  `baga_vec_push_str`) се взема от **типа на аргумента `x`** (възелът носи
  `->type` от checker-а) — и в двата backend-а. Същото за get/set.
- Псевдонимите `_str` emit-ват str helper-ите както досега.

## Засегнати компоненти

| Файл | Промяна |
|---|---|
| `src/checker.c` | специална обработка на vec builtins в `infer_call`; `type_str` за Vec с елемент |
| `src/checker.c` или типовия helper | type_eq за Vec (ако не е вече по kind) |
| `src/codegen_c.c` | vec builtin емисия по типа на аргумента |
| `src/codegen_llvm.c` | същото в NODE_CALL map-а |
| `examples/vec.baga` | минава на единния API (упражнява го в оракула) |
| `examples/vec_typed.baga` | нов: смесен тип → очаквана compile грешка (НЕ е в оракула; в Makefile като negative probe) |
| `docs/language-bg.md`, `docs/language-en.md` | §12.4: единен API, правилата, грешките |
| `Makefile` | test: negative probe за vec_typed |

## Извън обхвата

User-defined generics (`fn f<T>`), Vec<f64>/Vec<struct> (runtime представянето
не ги побира), литерали `[1, 2, 3]`, итерация `for x in vec`.

## Приемливост

- `examples/vec.baga` (единен API) → OK в оракула за двата backend-а.
- Смесен push → compile грешка на български, exit 1 (и през baga, и през baga-llvm).
- `make test` зелен; старият `_str` API продължава да работи.
