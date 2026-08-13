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

- ~~Вложени struct полета~~ — **РЕШЕНО в v0.5 (виж по-долу).**
- Enum payload-и.
- ~~Полета вътре в `Vec<S>` / `Map<K,S>` при drop на контейнера~~ —
  **РЕШЕНО в v0.2 (виж по-долу).**
- Closure capture на struct.
- ~~Release на старото поле при `s.f = x`~~ — **РЕШЕНО в v0.4 (виж по-долу).**

## v0.4: release на старото поле при `s.f = x`

Цел `ident.field`, където ident е track-нат struct локал (tag 5, не
param/dead), а полето е пряко heap поле (str/bytes/Vec/Map): старото поле се
release-ва преди assign. Alias-safe ред — новото се retain-ва преди release
(`w.s = w2.s`, self-assign `w.s = w.s` не underflow-ват; тествано). Fresh
дясно (concat/call резултат) е owned — без retain; borrowed дясно
(`w.s = v.s`, `vec_get(...)`) се retain-ва. Move на last-use ident също
release-ва старото поле. Само плоско `ident.field` — по-дълбоки пътеки
(`a.b.c = x`) биха оценили целта два пъти (странични ефекти).

Измерено: 500k итерации `w.s = concat(...)` — RSS 39 MB → 10.9 MB.

## v0.5: вложени struct полета

`rc_struct_has_heap` е транзитивен (поле от struct тип с heap полета брои;
depth guard 32). `retain_S`/`release_S` рекурсират във вложените struct
полета; forward декларации на и четирите helper-а (`retain`/`release`/`relf`/
`retp`) — рекурсията ходи напред-назад по декларационния ред. Литерал,
вграждащ borrowed вложен struct (`Pair { w: p.w }`), retain-ва през
`__rc_sl.<field>` с `baga_rc_retain_<T>` (RC1.1 пътеката вече ползва
`rc_heap_tag`, не само `rc_type_tag`).

Измерено: 500k итерации `Outer { inner: Inner { s: concat(...) } }` — RSS
38.5 MB → 10.8 MB. Тестове: случаи 18-21 в `tests/struct_rc_test.baga`.

Останало (leak-safe): enum payload-и — **РЕШЕНО в v0.6**
(`docs/memory-rc-enum-bg.md`); call-аргумент temp-ове в box push
— **ЧАСТИЧНО РЕШЕНО в v0.7 (виж по-долу)**;
`s.inner = x` (struct-типизирано поле като цел — v0.4 покрива само
str/bytes/Vec/Map полета) — **РЕШЕНО в v0.8 (виж по-долу)**; `Vec<S>` в
`Vec`.

## v0.8: `s.inner = x` (struct-типизирана цел на field assign)

v0.4 покриваше само директни heap полета; struct-типизирано поле
(`Outer { inner: Inner }`, `Inner` с heap полета) падаше в generic assign:
старата стойност leak-ваше при overwrite, а borrowed дясно
(`o.inner = t.inner`) се споделяше без retain — четене на освободено поле
след смърт на източника (`borrowed_outlive` в теста фейлва на стария
codegen). Сега `rc_field_assign_struct` познава цел `ident.field` с
track-нат struct ident (tag 5, не param/dead) и struct-типизирано поле с
heap полета (транзитивно, `rc_nested_struct_field` от v0.5), и assign
пътеките emit-ват:

- **свеж литерал** — owned, без retain (полетата му са балансирани от
  литералния път); release_<Inner> на старото поле преди assign.
- **tracked ident** — retain_<T> преди release на старото (alias-safe
  ред като v0.4); при last-use — move (без retain, release на старото).
- **всичко останало** (call резултат, поле, vec_get, untrack-нат ident) —
  retain_<T> преди release. Struct fn резултат може да е borrowed
  (§v0.7 границата: `return vec_get(...)` не retain-ва) и не се различава
  от fresh — посоката е leak-safe: fresh call (`s.inner = mk(...)`)
  leak-ва една референция на итерация, както v0.7 struct box temp-овете.

Release е рекурсивният `baga_rc_release_<Inner>` от v0.5 (по-дълбока
вложеност се покрива транзитивно). Само плоско `ident.field` — същата
граница като v0.4 (`a.b.c = x` би оценил целта два пъти). Enum-типизирано
поле не се покрива (както enum като struct поле изобщо — v0.6 не-цел).

Измерено: 500k итерации `o.inner = Inner { s: concat(...) }` — RSS
24.9 MB → 10.6 MB. Тест: `tests/nested_assign_rc_test.baga` (12 случая).

## v0.7: call temp-ове в box push

Temp резултат от call, директен аргумент на `vec_push`/`vec_set`/`map_set`
със str/bytes/Vec стойност, се прехвърля в контейнера: `_move` helper без
retain (RC3 вариантите) + temp записът се консумира (`rc_tmp_find` +
site=NULL след emission; `rc_tmp_release_all` го пропуска) — без release в
края на statement-а. Същият трансфер като RC3 за last-use ident; валиден,
защото str/bytes/Vec fn резултатът е owned по конвенция (return на
параметър/borrowed се retain-ва — RC1), а RC4 вече разчита на същата
конвенция при release на тези temp-ове. Вложени случаи (`push(v, g(f()))`):
само директният аргумент е move; вътрешните temp-ове се release-ват както
досега. Temp ползван два пъти не съществува синтактично (всеки temp възел
се оценява веднъж) — локал ползван два пъти пада в RC3 last-use правилата.

**struct/enum box temp-ове НЕ се move-ват** (остават с retain, temp-ът
тече — leak-safe): struct fn резултатът може да е borrowed —
`return vec_get(...)` на struct НЕ retain-ва (`rc_type_tag` е 0 за struct в
`emit_return_val`), а реален такъв код съществува (boilaDB `boila_ps_tok`/
`boila_dual_ptok` връщат `Token` от vec_get). Move би оставил box-а да
споделя единствената референция → UAF/underflow при drop на източника.
Точен move изисква owned-конвенция за struct резултати навсякъде (return/
match/if-израз/lambda пътеки) — отделна, по-голяма стъпка.

Измерено (500k итерации, --rc): typed temp-овете бяха балансирани и преди
(retain+release двойка) — RSS flat (33 MB push(f()) str; 128 MB map_set),
печалбата е елиминираната двойка на всяка итерация. Struct box temp:
48.5 MB vs 25.0 MB за литералния move (push+drop цикъл) — оставащият
leak, документиран по-горе. Тест: `tests/calltemp_rc_test.baga` (12 случая).

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
