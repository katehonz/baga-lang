# Започни тук — Baga за 10 минути

Тази страница води от `make` до първата програма, първия доказан договор и
първия хванат висящ указател. **Всеки изход по-долу е реален** — примерите са
пуснати с командите на тази страница на 2026-10-31, не са преписани от памет.

## 1. Компилирай

```sh
make          # C toolchain → ./baga
make sandak   # package manager → ./sandak
```

Проверка:

```sh
./baga --version
# baga 1.4.0
```

## 2. Първа програма

`zdravei.baga`:

```baga
fn main() {
    print("Здравей, багатуре. Боят започва.")
}
```

```sh
./baga -I . -I app-product zdravei.baga
# Здравей, багатуре. Боят започва.
```

`-I . -I app-product` са нужни за `import` — добавяй ги винаги.

## 3. Типове и функции

Ключовите думи са български (`fn`, `let`, `mut`, `if`, `while`, `for`,
`return`), типовете са явни (`i64`, `str`, `f64`, `bytes`, `Vec<T>`, `Map<K,V>`).
Числата са `i64`; литералите с наставка не са нужни.

```baga
fn фибоначи(n: i64) -> i64 {
    let mut a: i64 = 0
    let mut b: i64 = 1
    let mut i: i64 = 0
    while i < n {
        let temp = b
        b = a + b
        a = temp
        i = i + 1
    }
    return a
}

fn main() {
    for i in 0..6 {
        print(фибоначи(i))
    }
}
```

```sh
./baga -I . -I app-product fib.baga
# 0
# 1
# 1
# 2
# 3
# 5
```

`for i in 0..6` е полуотворен интервал (0, 1, 2, 3, 4, 5). `let` е непроменима,
`let mut` е променлива. `return` е задължителен за стойност; последният израз на
функцията е и нейната стойност (implicit return).

## 4. Ефекти са типови измерения

Функция със странични ефекти ги декларира в return типа: `!IO` (вход/изход),
`!Par` (нишки), `!Time` (време/часовници), `!NotFound` (домейн ефект с `catch`).
`str !IO !Net` е различен тип от `str` — ефектът е в типа, не в коментар.

```baga
fn прочети_файл(път: str) -> str !IO !NotFound {
    return "съдържание"
}

fn обработи(път: str) -> str !IO !NotFound {
    let данни = прочети_файл(път)?   // ? propagate-ва ефектите нагоре
    return данни
}

fn main() {
    let резултат = прочети_файл("данни.txt")
        catch !NotFound => "празно"
        catch !IO => "грешка"
    print(резултат)
}
```

```sh
./baga -I . -I app-product effects.baga
# съдържание
```

`catch !NotFound => "празно"` обработва ефекта на мястото на извикването. Ако
ефектът не е деклариран в return типа, компилаторът грешка — това е разликата
между „ефектите са документация" и „ефектите са тип".

## 5. Първият доказан договор

`spec` описва вход/изход и обещание; `--verify` го проверява статично.

`abs_val.baga`:

```baga
spec abs_val {
    input:
        x: i64
    output: i64
    ensures:
        output >= 0
}

fn abs_val(x: i64) -> i64 {
    if x < 0 {
        return 0 - x
    }
    return x
}

fn main() {
    print(abs_val(0 - 5))
}
```

```sh
./baga -I . -I app-product --verify abs_val.baga
# verify abs_val:
#   ensures #1 (output >= 0): ДОКАЗАНО
#   аритметика (преливане: (0 - x)): ОБРОЧЕНО
#     контрапример: x = -9223372036854775808
```

Прочети го така: обещанието `output >= 0` е **доказано** за всички `x`. Но
аритметиката `0 - x` **прелива** при `x = INT64_MIN` — и верификаторът дава
конкретния контрапример. Това е разликата между „тестове, които минават" и
„доказателство с контрапример".

Ако добавиш `!Overflow` в return типа (`-> i64 !Overflow`), аритметичното
преливане става деклариран ефект и вердиктът се променя съответно.

## 6. Светове — регион и хванат висящ указател

Светове са паметни региони с ownership/freeze/move дисциплина, проверявана от
компилатора. `world_dump`/`world_load` сериализират без fixup (офсетите са
позиционни).

```baga
fn main() {
    let w = world_new()
    let v = wvec_new(w)
    wvec_push_str(w, v, "hello")
    wvec_push_str(w, v, "world")
    world_freeze(w)                 // замразен свят = споделена read-only capability
    print(wvec_get_str(w, v, 0))
    print(wvec_get_str(w, v, 1))

    let dump = world_dump(w)        // сериализация на региона
    world_free(w)

    let w2 = world_load(dump)       // зареждане в НОВА арена, същите офсети
    let v2 = wvec_new(w2)
    wvec_push_str(w2, v2, "reloaded")
    print(wvec_get_str(w2, v2, 0))
    world_free(w2)
}
```

```sh
./baga -I . -I app-product worlds.baga
# hello
# world
# reloaded
```

### И какво НЕ мълчи

Следващата програма изглежда невинна — низ от региона влиза във вектор, който
надживява света. Тя НЕ се компилира:

```baga
fn main() {
    let w = world_new()
    let v = wvec_new(w)
    wvec_push_str(w, v, "hi")
    let out = vec_new()
    vec_push(out, wvec_get_str(w, v, 0))   // заеман указател влиза във Vec
    world_free(w)                          // регионът изчезва
    print(vec_get(out, 0))                 // ← висящ указател
}
```

```sh
./baga -I . -I app-product --check escape.baga
# escape.baga:10:15: контейнерът 'out' държи заемана стойност или ключ от света
# 'w', който се освобождава тук — след free елементите му са висящи указатели;
# копирай стойността преди free (concat(s, "") за низ) или освободи контейнера
# първо (drop(out))
```

Същото обвинение важи през `map_set`, през `vec_slice`/`vec_concat`, през
`map_keys` и през **fn граница** (включително generic функции): ако callee пише
заемана стойност в param-контейнер, аргументът на извикващия се замърсява.

Поправката е копие — и тогава програмата работи:

```baga
vec_push(out, concat(wvec_get_str(w, v, 0), ""))
```

```sh
./baga -I . -I app-product escape_ok.baga
# hi
```

Това е цялата идея: проблемът се казва на мястото на `world_free`, с посока как
да се оправи, а не като `heap-use-after-free` в production.

## 7. Нататък

- `docs/language-bg.md` — пълният език (типове, ефекти, generics, spec).
- `docs/worlds-bg.md` — моделът на световете, bench и честни граници.
- `examples/` — работещи програми; `examples/verify/` — оркъли за `--verify`.
- `tests/` — regression батерията (`make test`).
- `README.md` — roadmap и пакети (`std/`, `app-product/*`, `apps/*`).
