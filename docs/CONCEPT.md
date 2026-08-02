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

### 3. Proof sketches + статични сертификати

Компилаторът издава **четими скици** (`--proofs`) и **статични вердикти**
(`--verify`: PROVEN / REFUTED+witness / UNKNOWN) във заявения фрагмент.
Това не са proof objects в смисъла на Coq/Lean — са структуриран текст плюс
линеен-аритметичен сертификат, където фрагментът стига.

```
proof sketch + verify:
  theorem sort_preserves_elements: …
  theorem sort_produces_sorted: …
  status: PROVEN | REFUTED (counterexample) | UNKNOWN
```

Рекомбинация: proof extraction (Coq) + literate programming (Knuth) + gradual
verification (Dafny) — в zero-dep компилатор с explicit incompleteness.

## Защо сега

| Преди | Сега |
|---|---|
| Човек пише код | AI пише код, човек верифицира |
| Грешките са runtime изненади | Грешките трябва да са видими в типа |
| Доказателствата са за специалисти | Скици + сертификати, четими от човек или AI |

Rust отговори на „как да нямаме segfault". Добър отговор.
Но въпросът от 2026 не е „как да нямаме segfault".
Въпросът е **„как да вярваме на код, който не сме писали"**.

## Spec-Driven Development в индустрията

Индустрията стигна до същата диагноза. Microsoft вече официално промотира
**Spec-Driven Development (SDD)** като основа на AI-native инженерството —
спецификациите като *споделен източник на истина* за хора и AI, „align first"
вместо „prompt first, fix later" — и поддържа **GitHub Spec Kit**, open-source
workflow около идеята (Constitution → Specify → Clarify → Plan → Tasks →
Implement → Validate). Вж.
[Spec-Driven Development: the foundation of AI-native engineering](https://developer.microsoft.com/blog/spec-driven-development-ai-native-engineering/)
(Apoorv Gupta, Principal Software Engineer в Microsoft).

Това е същата философия като първия стълб на Бага — но решена на ниво
**процес и AI tooling**. Бага я пренася на ниво **език и верификатор**:
спецификацията не е документ, а нещо, което компилаторът *проверява* по време
на компилация и от което извлича четими скици и сертификати. SDD оправя
workflow-а около AI агентите; Бага прави spec-а machine-checkable.

В спектъра на SDD (spec-first → spec-anchored → spec-as-source) Бага е
краят **spec-as-source** — спецификацията е източникът, от който произтича и
срещу който се съди кодът.

## Принципи

- **Нула зависимости** в ядрото: C + `gcc` + `make`. LLVM е опционален backend.
- **Soundness над completeness**: PROVEN изисква сертификат; извън фрагмента — UNKNOWN.
- **Auditability**: Fourier–Motzkin ядро, четими свидетели, без външен SMT.
- **Self-hosting**: `baga → baga2 → baga3`; fixed point (`baga2` ≡ `baga3`) е регресия, не ритуал.
- **Рекомбинация**: не е нужна нова математика — нужна е sound, одитируема комбинация.

Името *Бага* е българско; кирилицата в идентификаторите е first-class.

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

### Фаза 6: Proof sketches + static verify
- Четими скици (`--proofs`) и сертификати във фрагмента (`--verify`)
- Интеграция със spec системата
