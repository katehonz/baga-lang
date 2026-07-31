Прав си. Нищо не е ново.

Rust е linear logic на Girard от 1987 \+ region-based memory management на Tofte и Talpin от 1994 \+ Cyclone от 2001\. B+Tree е от 1979\. Lisp е от 1958\. Type theory е на Church от 1940-те. Monad е от category theory от 1950-те. Дори „algebraic effects" са на Plotkin и Power от 2003\.

Всичко е рекомбинация. Дори Bars е рекомбинация — Clojure scoping \+ Rust ownership \+ Cranelift. Нищо от тези три не е ново. Новото е, че никой не ги е лепил заедно.

Така че въпросът не е „какво е ново". Въпросът е \*\*„какво не е лепено заедно досега"\*\*.

\---

\#\# Ето какво бих лепил аз

\#\#\# 1\. Език, в който програмата е доказателство, но не го пишеш

Lean 4 — пишеш доказателства. Адски трудно. 99% от програмистите не могат.  
Rust — типовете дават слаби гаранции. Достатъчно за да не сегфолтира. Не и за да е правилно.

\*\*Между тях има празно пространство.\*\*

Пишеш нормален код:

\`\`\`  
fn sort(arr: \&mut \[i32\]) {  
    for i in 1..arr.len() {  
        let key \= arr\[i\];  
        let j \= i \- 1;  
        while j \> 0 && arr\[j\] \> key {  
            arr\[j \+ 1\] \= arr\[j\];  
            j \= j \- 1;  
        }  
        arr\[j \+ 1\] \= key;  
    }  
}  
\`\`\`

И компилаторът \*\*извлича\*\* доказателство:

\`\`\`  
extracted proof:  
  theorem sort\_preserves\_elements:  
    ∀ arr arr'. sort(arr) \= arr' → multiset(arr) \= multiset(arr')  
    
  theorem sort\_produces\_sorted:  
    ∀ arr arr'. sort(arr) \= arr' → sorted(arr')  
    
  theorem sort\_terminates:  
    ∀ arr. terminates(sort(arr))  
\`\`\`

Не го пишеш. Не го доказваш. Компилаторът го извлича от кода. И е \*\*четимо\*\*. Не Coq. Не Lean. Нормален текст.

Това е proof extraction (съществува в Coq) \+ literate programming (Knuth) \+ gradual verification (Dafny). Парчетата ги има. Заедно — не.

И ето защо е актуално \*\*сега\*\*: AI пише кода. Човекът трябва да го \*\*верифицира\*\*. Но не може да чете Lean. Може да чете „тази функция запазва елементите и ги сортира". Това е.

\---

\#\#\# 2\. Език, в който грешката е измерение, не изключение

В момента имаш три модела:

\- Exceptions (Java, Python) — грешката е извън типовата система. Не я виждаш.  
\- Result/Option (Rust) — грешката е в типа. Но \`Result\<T, E\>\` е просто enum. Не се композира добре.  
\- Effect systems (Koka, Eff) — алгебрични ефекти. Красиво. Но никой не ги използва реално.

Аз бих направил нещо по-радикално:

\`\`\`  
fn read\_file(path: String) \-\> String \!IO \!NotFound \!Permission {  
    let handle \= open(path)?;      // \!IO, \!NotFound  
    let content \= read(handle)?;   // \!IO, \!Permission  
    content  
}

fn main() {  
    // Трябва да се справиш с ВСЯКО измерение  
    let content \= read\_file("data.txt")  
        catch \!NotFound \=\> "празно"  
        catch \!Permission \=\> "няма достъп";  
    // \!IO не е хванато → компилаторът казва:  
    // "error: unhandled effect \!IO in pure context"  
}  
\`\`\`

Грешката не е стойност. Не е изключение. Тя е \*\*измерение на типа\*\*. \`String \!IO \!NotFound\` е различен тип от \`String\`. И се композират:

\`\`\`  
fn process() \-\> Data \!IO \!NotFound \!ParseError {  
    let raw \= read\_file("data.json")?;   // \!IO \!NotFound  
    parse(raw)?                           // \!ParseError  
}  
// Ефектите се събират автоматично. Като типове.  
\`\`\`

Това е effect system, но с \*\*explicit effect polymorphism\*\* и \*\*effect inference\*\*. Koka го прави, но е академичен. Никой не го е направил \*\*практичен\*\*.

\---

\#\#\# 3\. Език за ерата на AI

Това е най-радикалното. И най-ненаселеното пространство.

Всеки език е проектиран за \*\*човек\*\*, който пише код. Синтаксис, error messages, IDE — всичко е за човека.

Но все по-често \*\*AI пише кода\*\*. Човекът \*\*верифицира\*\*.

Какво би означавало това?

\`\`\`  
// Човекът пише СПЕЦИФИКАЦИЯ  
spec "sorting algorithm" {  
    input: arr: \[i32\]  
    output: \[i32\]  
    guarantees:  
        \- output is sorted  
        \- output has same elements as input  
        \- O(n log n) worst case  
}

// AI пише ИМПЛЕМЕНТАЦИЯТА  
impl "sorting algorithm" {  
    fn sort(arr: \[i32\]) \-\> \[i32\] {  
        // ... quicksort, mergesort, каквото AI реши  
    }  
}

// Компилаторът ПРОВЕРЯВА че impl удовлетворява spec  
// Ако не — отказва. Не warning. Отказ.  
\`\`\`

Не е Design by Contract (Eiffel, 1986). Не е formal verification (Coq). Не е TDD.

Език, в който \*\*спецификацията е първокласен гражданин\*\*. В който \`spec\` е keyword. В който компилаторът не казва „type error", а казва „implementation violates specification: output may not be sorted when input contains duplicates".

И AI е \*\*consumer\*\* на този език. AI чете spec-а и пише impl. Компилаторът е \*\*съдията\*\*. Човекът е \*\*архитектът\*\*.

Никой не прави това. Никой.

\---

\#\# Защо точно тези три?

Защото отговарят на въпроса „какво се е променило в света":

| Преди | Сега |  
|---|---|  
| Човек пише код | AI пише код, човек верифицира |  
| Грешките са runtime изненади | Грешките трябва да са видими в типа |  
| Доказателствата са за математици | Доказателствата трябва да са автоматични и четими |

Rust отговори на „как да нямаме segfault". Добър отговор. Но въпросът от 2026 не е „как да нямаме segfault". Въпросът е \*\*„как да вярваме на код, който не сме писали"\*\*.

\---

\#\# И последно

Казваш „нищо не е ново". Вярно. Но и нищо не е \*\*навременно\*\*.

Linear logic е от 1987\. Трябвали са 30 години за да стане Rust. Category theory е от 1950-те. Трябвали са 60 години за да стане Haskell популярен. Effect systems са от 2003\. Може би сега е моментът.

Не е нужно да измисляш нова математика. Трябва да измислиш \*\*правилната рекомбинация\*\* за \*\*правилния момент\*\*.

🐆  
—----------------------

На \*\*C\*\*. После на самия Бага.

Ето защо:

\---

\#\# Фаза 1: Bootstrap на C

\`\`\`  
baga/  
├── src/  
│   ├── lexer.c        \~800 реда  
│   ├── parser.c       \~2000 реда  
│   ├── types.c        \~600 реда  
│   ├── checker.c      \~1200 реда  
│   ├── codegen\_c.c    \~1500 реда  
│   └── main.c         \~200 реда  
├── include/  
│   └── baga.h  
├── Makefile  
└── test/  
\`\`\`

\~6000 реда C. Нула зависимости. \`make && ./baga\`.

Защо C, а не Rust:

\- Ти вече пишеш C. BarabaDB е C. Не ти трябва нов toolchain.  
\- Цялата философия на Бага е „багатур, който се бие сам". Да го напишеш на Rust е да купиш меча от Amazon.  
\- C compiler има навсякъде. gcc, clang, tcc, msvc. На всяка машина.  
\- Bootstrap-ът е за хвърляне. Не трябва да е красив. Трябва да работи.

Защо не Nim:

\- Nim го знаеш, но C е по-универсален.  
\- Nim добавя зависимост от nim compiler. C не добавя нищо.  
\- BarabaDB вече ти е доказателството, че можеш C.

\---

\#\# Фаза 2: Self-hosting

Веднага щом Бага може да парсва себе си — \*\*пренаписваш компилатора на Бага\*\*.

\`\`\`baga  
// compiler/lexer.baga  
fn tokenize(src: \&str) \-\> Vec\<Token\> {  
    let mut tokens \= Vec::new();  
    let mut pos \= 0;  
    while pos \< src.len() {  
        match src\[pos\] {  
            ' ' | '\\n' | '\\t' \=\> pos \+= 1,  
            '0'..='9' \=\> tokens.push(read\_number(src, \&mut pos)),  
            'a'..='z' | 'A'..='Z' \=\> tokens.push(read\_ident(src, \&mut pos)),  
            \_ \=\> tokens.push(read\_symbol(src, \&mut pos)),  
        }  
    }  
    tokens  
}  
\`\`\`

Bootstrap-ът на C компилира този файл. Получаваш \`baga2\`. После \`baga2\` компилира себе си. Получаваш \`baga3\`. Ако \`baga2 \== baga3\` — self-hosting работи.

Това е ритуалът. Всеки уважаващ себе си език го прави. Bux го прави. Bars го прави. Бага ще го прави.

\---

\#\# Фаза 3: Backend

Тук имаш избор:

| Backend | Кога | Защо |  
|---|---|---|  
| \*\*C transpiler\*\* | Фаза 1 | Най-лесно. Работи веднага. \`baga → C → gcc → binary\` |  
| \*\*Cranelift\*\* | Фаза 3a | Бърз JIT. REPL. Вече го имаш в Bars. |  
| \*\*LLVM\*\* | Фаза 3b | Оптимизации. Release builds. Вече го имаш в Bars. |

Започни с C transpiler. Не защото е най-добър. Защото е \*\*най-бърз за писане\*\*. После, като езикът е стабилен — Cranelift за REPL, LLVM за release.

\---

\#\# Защо не Rust, още веднъж

Ще ти го кажа както го разбирам:

Бага е език за човек, който не иска да зависи от никого. Ако компилаторът на Бага е написан на Rust, ти зависиш от:

\- Rust toolchain  
\- Cargo  
\- Crates.io  
\- Rust compiler team  
\- Microsoft/Amazon/Google, които финансират Rust

И тогава Бага не е багатур. Бага е васал.

На C — не зависиш от никого. \`gcc\` е на всяка машина. От 1987\. И ще е там и след 50 години.

\---

\#\# Практичният отговор

Като дойдеш и седнем да пишем:

1\. \*\*\`baga.h\`\*\* — AST дефиниции, token типове, type system. \~300 реда.  
2\. \*\*\`lexer.c\`\*\* — токенизатор. \~800 реда.  
3\. \*\*\`parser.c\`\*\* — recursive descent parser. \~2000 реда.  
4\. \*\*\`checker.c\`\*\* — type checking \+ ownership. \~1200 реда.  
5\. \*\*\`codegen\_c.c\`\*\* — C code generator. \~1500 реда.  
6\. \*\*\`main.c\`\*\* — CLI. \~200 реда.

\~6000 реда. Един човек. Два-три месеца сериозна работа. Или един дълъг уикенд, ако си луд.

И накрая:

\`\`\`  
$ make  
gcc \-O2 \-o baga src/\*.c  
$ ./baga examples/zdravei.baga  
Здравей, багатуре. Боят започва.  
\`\`\`

Това е. ⚔️  
