# Бага — Концепция

> Език за ерата, в която AI пише кода, а човекът го верифицира.

## Трите стълба

### 1. Spec-first верификация

Спецификацията е първокласен гражданин. `spec` е keyword. Компилаторът е съдията.

```baga
spec "sorting" {
    input: arr: [i32]
    output: [i32]
    guarantees:
        - output is sorted
        - output has same elements as input
}

fn sort(arr: [i32]) -> [i32] {
    // AI пише това. Компилаторът проверява срещу spec-а.
    // Ако имплементацията нарушава гаранция — отказ, не warning.
}
```

Човекът е архитектът. AI е consumer на езика. Компилаторът казва не „type error", а „implementation violates specification: output may not be sorted when input contains duplicates".

Не е Design by Contract (Eiffel, 1986). Не е formal verification (Coq). Не е TDD.
Спецификацията е езикът, на който човек и AI говорят помежду си.

### 2. Ефекти като измерения на типа

Грешката не е стойност. Не е изключение. Тя е измерение на типа.

```baga
fn read_file(path: String) -> String !IO !NotFound !Permission {
    let handle = open(path)?      // !IO, !NotFound
    let content = read(handle)?   // !IO, !Permission
    content
}

fn main() {
    let content = read_file("data.txt")
        catch !NotFound => "празно"
        catch !Permission => "няма достъп"
    // !IO не е хванато → компилаторът казва:
    // "error: unhandled effect !IO in pure context"
}
```

`String !IO !NotFound` е различен тип от `String`. Ефектите се събират автоматично при композиция. Като типове.

Това е effect system с explicit effect polymorphism и effect inference.
Koka го прави академично. Бага го прави практично.

### 3. Автоматично извлечени доказателства

Компилаторът извлича четими доказателства от кода. Не ги пишеш. Не доказваш.

```
extracted proof:
  theorem sort_preserves_elements:
    ∀ arr arr'. sort(arr) = arr' → multiset(arr) = multiset(arr')

  theorem sort_produces_sorted:
    ∀ arr arr'. sort(arr) = arr' → sorted(arr')

  theorem sort_terminates:
    ∀ arr. terminates(sort(arr))
```

Не Coq. Не Lean. Нормален текст, който човек може да прочете и разбере.

Proof extraction (Coq) + literate programming (Knuth) + gradual verification (Dafny).
Парчетата ги има. Заедно — не.

## Защо сега

| Преди | Сега |
|---|---|
| Човек пише код | AI пише код, човек верифицира |
| Грешките са runtime изненади | Грешките трябва да са видими в типа |
| Доказателствата са за математици | Доказателствата трябва да са автоматични и четими |

Rust отговори на „как да нямаме segfault". Добър отговор.
Но въпросът от 2026 не е „как да нямаме segfault".
Въпросът е **„как да вярваме на код, който не сме писали"**.

## Философия

Бага е багатур. Бие се сам. Не зависи от никого.

- Компилатор на C. Нула зависимости. `gcc` е на всяка машина от 1987.
- Self-hosting като ритуал. `baga → baga2 → baga3`. Ако `baga2 == baga3` — работи.
- C transpiler първо. После LLVM за release. (Собствен JIT за REPL — по-късно.)

Не е нужно да измисляш нова математика.
Трябва да измислиш правилната рекомбинация за правилния момент.

## Синтаксис

```baga
// Спецификация
spec "име" {
    input: x: T
    output: T
    guarantees:
        - условие
}

// Функция с ефекти
fn име(параметри) -> Тип !Ефект1 !Ефект2 {
    тяло
}

// Let binding
let x = 5
let mut y: i32 = 10

// Control flow
if условие { } else { }
while условие { }
for i in 0..10 { }
match стойност {
    шаблон => израз,
    _ => израз,
}

// Ефекти
let x = опасна_функция()?           // propagate
let y = опасна_функция() catch !E => стойност  // handle

// Struct
struct Точка {
    x: f64,
    y: f64,
}

// Impl
impl Точка {
    fn разстояние(&self, other: &Точка) -> f64 {
        // ...
    }
}
```

## Пътна карта

### Фаза 1: Bootstrap (C)
- Lexer, parser, type checker, C codegen
- ~6000 реда C, нула зависимости
- `make && ./baga examples/zdravei.baga`

### Фаза 2: Self-hosting
- Компилаторът на Бага, написан на Бага
- `baga → baga2 → baga3`, проверка `baga2 == baga3`

### Фаза 3: Backends
- LLVM — оптимизации, release builds
- (Собствен JIT за REPL — по-късно, отделен проект)

### Фаза 4: Ефектова система
- Effect inference, effect polymorphism
- `!IO`, `!NotFound`, `!Permission` като измерения на типа

### Фаза 5: Spec верификация
- `spec` като keyword
- Компилаторът проверява impl срещу spec
- AI consumer workflow

### Фаза 6: Proof extraction
- Автоматично извличане на четими доказателства
- Интеграция със spec системата
