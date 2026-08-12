# Move семантика за struct threading — design (етап 2)

Статус: **имплементиран v2.0 зад `--rc` флага** (C backend само), 2026-08;
допълнен с **v2.1** (borrowed-pair elision, docs/memory-rc-bg.md) и **v3**
(container move — §v3 по-долу).
Реализация: `src/codegen_c.c` (RC2 маркери: last-use pre-pass +
copy-site elision), `include/baga.h` (`RcUse`, `rc_lus`, `rc_moves`),
тест `tests/move_test.baga`. Брояч: коментар `/* RC2: move elisions: N */`
в края на генерирания код. Етап 2 от стратегията за паметовия модел
(етап 1 = RC1, `docs/memory-rc-bg.md`). Предпоставка: `--rc` флагът от
етап 1; move елиминацията е чиста оптимизация БЕЗ езикова промяна —
никакъв нов синтаксис, никакви use-after-move грешки, никакъв счупен
съществуващ код.

## Контекст и мотивация

Доминиращият ownership pattern в екосистемата е struct threading:
`db = lsm_put_kb(db, key, val)?`, `srv = boila_server_exec(srv, …)?` —
struct-по-стойност вериги, които са *де факто* предаване на собственост.

**Уточнение при имплементацията (v2.0):** в реалния RC1 struct копията в
такава верига са вече безплатни — struct локалите не се track-ват, а heap
ПОЛЕТА им се споделят по указател (retain идва едва когато поле се върже
като локал — borrowed-retain). Затова move elision-ът важи за heap-typed
локали (str/bytes/Vec/Map) в copy сайтовете, където RC1 emit-ва retain
(не за самите struct копия): `let x = y` (alias), struct-lit embed на
ident, field assign на ident, assign на track-нат локал от ident. Всяка
такава двойка retain+release е чиста загуба, когато източникът повече не
се ползва (move).

## Правило (move elision)

За struct-типизиран локален binding: ако дадена употреба е **последна**
във функцията, тя е move — codegen ПРОПУСКА retain-а на heap полетата
при копирането И маркира binding-а като dead (scope exit release на
него отпада). Нетният ефект: референциите се *преместват* от източника
в целта без да се пипа rc.

Копия, които НЕ са последна употреба, остават retain-copy (днешното
поведение) — нулева промяна в семантиката.

### Last-use анализ

Per функция, per struct binding: def-use обход на AST-то.
- Права последователност: последната текстуална употреба е move-кандидат.
- **Клонове (if/match):** употреба в който и да е клон брои като употреба
  на съединението; move е позволен само ако е последна употреба във
  ВСИЧКИ пътища (консервативно: ако който и да е клон ползва binding-а
  след точката, няма move).
- **Цикли:** употреба в тяло на цикъл НЕ е move, ако binding-ът се ползва
  (или преприсвоява) в друга итерация — консервативно: всяка употреба в
  цикъл или след цикъл изключва move преди/в цикъла, освен ако binding-ът
  е дефиниран вътре в итерацията (локален за итерацията — там move важи).
- **Преприсвояване (`x = f(x)`):** употребата в дясната страна е last-use
  за старата стойност → move в call-а; новата стойност е свеж binding.
- **Ламбди/closure capture:** capture = употреба, която изключва move
  (env живее извън scope-а).
- `return x` вече е move от RC1 — този случай не се пипа.

### Какво НЕ се променя

- Параметри остават borrowed (callee не release-ва).
- i64/f64/bool локали — нямат heap полета, извън играта.
- str/bytes/Vec/Map локали — RC1 правилата важат; move elision е само
  за struct копия (там е threading трафикът).
- Няма нови грешки в checker-а — анализът е в codegen (имаме типовете и
  scope стека от RC1); несигурно → fallback към retain-copy.

## Рискове

- Грешен last-use → release-ва се стойност, която още се ползва (UAF,
  но шумен под ASan и rc underflow гарда). Затова: консервативност
  навсякъде + ASan + пълната --rc батерия + boilaDB стрес bench.
- Enum variant-и с struct payload — отделен case в анализа; v2.0 може
  консервативно да ги изключи.

## Проверка и критерий — резултати (v2.0)

- Пълната RC1 верификация: 23/23 --rc батерия (22 + move_test), ASan чист
  (rc_test + move_test), 146/151 без флаг (5-те fail = external peers),
  бит-идентичен emit-c без флаг (7 файла diff срещу HEAD build). ✔
- Move брояч (коментар в генерирания код): **254 елиминации** в boilaDB
  insert_write компилацията, 5 в move_test, 2 в rc_test. ✔
- boilaDB 100k insert bench с --rc (същата машина):
  - преди (RC1 финал): real 47.7 s, user 33.2 s, peak RSS 1.87 GB
  - след (RC2 move): real 47.6 s, user 33.1 s, peak RSS 1.83 GB,
    verify DURABLE OK
  - Извод: **user time без измерима промяна** (в шума; ×1.20 спрямо
    no-rc baseline 27.7 s — целта ≤ ×1.05 НЕ е постигната). Причината е
    уточнението в §Контекст: в реалния RC1 struct копията са безплатни,
    а остатъчният retain/release трафик е на места, които не са move
    сайтове: borrowed-retain при връзване на vec_get/map_get/поле
    резултати, container retains при push/set, и alias-и на параметри
    (borrowed — не се move-ват по правило). RSS леко подобрение
    (1.87 → 1.83 GB) от по-малко задържани референзии.
- Консервативни изключвания в v2.0: `x = x` self-assign (release би
  обесил стойността), enum variant сайтове (не се различават в анализа),
  shadowing (n_lets > 1), lambda capture, match arms, loop-carried
  bindings, move в if/match клон вътре в цикъл (студеният път тече).

## v3: container move (push/set без retain при last-use аргумент)

Контекст: след RC2/RC2.1 остатъчният retain/release трафик е предимно на
контейнерните вмъквания — `vec_push`/`vec_set`/`map_set` retain-ват
стойността в helper-а дори когато аргументът е last-use локал, който умира
веднага след (scope exit release). Двойката retain+release е чиста загуба:
референцията може да *премине* в контейнера.

**Правило:** за `vec_push(c, x)` / `vec_set(c, i, x)` / `map_set(m, k, x)`,
където `x` е heap-typed локал (str/bytes/Vec), чиято употреба е last-use
по RC2 анализа (същите консервативни изключвания: не параметър, не
shadowing, не capture, не match, loop само iteration-local), codegen
emit-ва `_move` вариант на helper-а (без retain на стойността) и маркира
`x` dead. `vec_set`/`map_set` overwrite пак release-ват старата стойност.
Ключът на `map_set` остава retain-нат (отделен живот — put може да го
отхвърли при съществуващ entry). i64/f64/box (struct/enum) сайтове нямат
retain изобщо — извън играта.

Реализация: `_move` runtime helper-и (`baga_vec_push/set_{str,bytes,vec}_move`,
`baga_map_set_{str,i64,bytes}_{str,bytes}_move`) + call-site детекция в
`src/codegen_c.c` (vec/map клоновете на emit_call). Брояч:
`/* RC3: container move elisions: N */`. Тест: `tests/cmove_test.baga`.

**Резултати (v3, boilaDB 100k insert bench, същата машина):**
- Елиминации: **23** в insert_write компилацията (при 254 RC2 + 42 RC2.1),
  6 в cmove_test (точно на очакваните сайтове; no-move случаите — употреба
  след push/set, borrowed параметър — остават retain-нати).
- Bench: 431 µs/ред (RC1 финал: 425; в шума), peak RSS **1.80 GB** (RC2:
  1.83 GB), verify **DURABLE OK** (0 загубени реда, индекс валиден).
  Wall/user от този рън не са сравними 1:1 с предишните (различна
  измервателна верига — python wrapper, включва gcc компилацията);
  ns/ред е вътрешната метрика на bench-а и е без промяна.
- Извод: механизмът пак е коректен, но и контейнерните move сайтове са
  извън горещия път — 23 елиминации върху милиони вмъквания. Горещите
  вмъквания в boilaDB получават стойността си от temp изрази
  (concat/int_to_str — RC1 temporaries лимитацията, v0.2) или от borrowed
  източници (vec_get/map_get резултати), не от last-use локали. Остатъкът
  до цел ≤ ×1.05 user изисква temporaries tracking (v0.2) — той е и
  главният остатъчен leak източник (~250-500 B/извикване).
- Верификация: cmove_test (9 проверки) PASS без флаг, с --rc и под
  ASan+UBSan (както rc_test/move_test/borrow_test); пълен пакет без флаг
  без нови FAIL-ове (boilaDB filesize гейтът пада и на HEAD —
  pre-existing); emit-c без флаг бит-идентичен с HEAD (8 файла diff);
  пълна tests/ батерия с --rc: **148/154** — 6-те FAIL са pre-existing
  и на HEAD (5 external peers: oauth_pg, orm_boila, registry, https,
  tls_handshake; + boila_ts_test пада с --rc и на HEAD — FPE, known
  --rc issue извън обхвата на v3).
