# Struct полета като собственици — design (RC5 v0.1)

Статус: **имплементиран v0.1 зад `--rc`** (C backend). Дизайн преди код,
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
- Полета вътре в `Vec<S>` / `Map<K,S>` при drop на контейнера
  (elem_kind 2 още само `free` на box-а).
- Closure capture на struct.
- Release на старото поле при `s.f = x` (старото тече — leak-safe).

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
