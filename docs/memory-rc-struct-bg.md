# Struct полета като собственици — design (RC5 v0.1/v0.2)

Статус: **v0.1 + v0.2 имплементирани зад `--rc`** (C backend). Дизайн преди код,
както `grok-12-08-plan.md` §4. Зависи от RC1–RC4 + RC1.4.

## Проблемът

Днес struct по стойност е C копие: полетата-указатели се споделят.
Struct литералът retain-ва вградени heap стойности (RC1 / RC1.1), но
**никой не release-ва полетата** при смърт на struct-а. `let w = Wrap {
s: concat("a","b") }` → concat rc=1 в полето → scope exit тече.

`let t = s` / `db = f(db)` / `vec_push(v, s)` са C копия на същите
указатели. Ако пуснем полетата на всеки struct локал без дисциплина
при копие — underflow / UAF.

## Не-цели (v0.1)

- Вложени struct полета (не рекурсираме).
- Enum payload-и.
- ~~Полета вътре в `Vec<S>` / `Map<K,S>` при drop на контейнера~~ —
  **РЕШЕНО в v0.2 (виж по-долу).**
- Closure capture на struct.
- Release на старото поле при `s.f = x` (старото тече — leak-safe).

## v0.2: drop на Vec<S> / Map<K,S> полета

Container release не знае елементния тип статично, затова
`baga_rc_release_vec`/`baga_rc_release_map` получават destructor fn pointer
(`elem_rel`/`val_rel`, NULL → старото поведение: само `free` на box-а).
За struct с heap полета се генерира shim `baga_rc_relf_<S>(void *p)` →
`release_S(*p)`. Call site-овете (scope exit, temp release, reassign,
`drop()`, Vec/Map полета в release_S) го подават чрез `rc_box_rel`
(NULL за enum/без heap полета — поведението им е непроменено).

Втори фикс в същата стъпка: `vec_push`/`vec_set`/`map_set` на **свеж struct
литерал** вече е move в box-а (без втори retain) — литералът притежава
полетата си (fresh или вече retain-нати borrowed) и temp-ът няма кой да го
release-не, та retain-ът беше чист теч. Call аргумент (`vec_push(v, f())`)
остава с retain — резултатът може да е borrowed, посоката е leak-safe.

Трети фикс: `vec_slice`/`vec_concat` на `Vec<S>` правят shallow box копия
(споделят полетата с източника). Под `--rc` вървят през
`baga_vec_slice_box_rc`/`baga_vec_concat_box_rc` с retain shim
`baga_rc_retp_<S>` — иначе drop на двата вектора release-ва полетата два
пъти (rc underflow, хванато от `std/vec_struct_test`: `Line{tags: Vec<str>}`
+ slice/concat). Без флаг пътеката е непроменена (бит-идентичен emit-c).

Измерено: 500k итерации vec_push+drop и map_set+drop на `Wrap{ s: str }` —
RSS 64 MB → 10.9 MB (като leak-free базата).

Останало (leak-safe, не корупция): ~~overwrite пътеки~~ — **РЕШЕНО в v0.3
(виж по-долу)**; call-аргумент temp-ове в box push, enum payload-и, вложени
struct-и, `Vec<S>` вътре в `Vec` (kind 3 няма тип по време на изпълнение).

## v0.3: overwrite/del на box елементи

`vec_set`/`map_set` върху съществуващ slot/ключ и `map_del` пускаха стария
box без release на полетата (а `map_del` и без free на pv — откаченото
entry не се вижда от `release_map`). Под `--rc` тези пътеки вървят през
`*_rc` варианти с destructor fn pointer (`baga_vec_set_box_rc`,
`baga_map_set_{str,i64,bytes}_box_rc`, `baga_map_del_{str,i64,bytes}_rc`) —
release на старите полета преди memcpy/free. Alias-safe ред: call site-ът
retain-ва новото преди set, така че `vec_set(v, 0, vec_get(v, 0))` и
`map_set(m, k, map_get(m, k))` не underflow-ват (тествано). Без флаг
пътеките са непроменени (бит-идентичен emit-c).

Измерено: 300k итерации map_set/vec_set overwrite + map_del — RSS
72 MB → 10.9 MB.

## Правила

1. **Tag 5** — struct с поне едно пряко heap поле (`str`/`bytes`/`Vec`/`Map`).
   Генерират се `baga_rc_retain_S` / `baga_rc_release_S` (само тези полета).
2. **Локал** се регистрира само при свеж struct литерал или alias на
   вече track-нат struct. `let w = vec_get(...)` / `let r = f()` / поле
   — не се регистрират (иначе commit пуска полетата на vec-а).
   Scope exit → `release_S`.
3. **`let t = s`** / **`t = s`** — `retain_S`, освен last-use move (RC2).
4. **`s = f(...)`** — ако `s` се копира като цял ident в дясното
   (`s = f(s)`, threading), **не** release-ваме стария `s` (резултатът
   алиасира същите полета). Иначе release + bind. Поле-четене `f(s.x)`
   не брои като копие.
5. **`f(s)` без присвояване** — няма retain (param е borrowed). Caller-ът
   си пуска `s` след връщането.
6. **`return s`** — move (както RC1 за heap ident).
7. **Param** — `is_param=1`, не се пуска. `let t = p` retain-ва.
8. **`vec_push`/`vec_set`/`map_set` на S** — `retain_S` на box копието
   (или move). Иначе drop на локала обесва box-а.

Посока: leak > корупция. Threading `db = f(db)` при нова стойност в
`f` оставя старите полета датекат, не dangling.

## Критерий

- `tests/struct_rc_test.baga` — PASS с и без `--rc`, ASan чист.
- Пълният RC чеклист от `grok-12-08-plan.md`. Без нов FAIL.
- emit-c без флаг бит-идентичен.
