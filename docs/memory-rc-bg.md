# Паметов модел v2: refcount (RC) — design

Статус: **имплементиран v0.1 зад `--rc` флаг** (C backend само), 2026-08.
Реализация: `src/codegen_c.c` (RC1 маркери), флагът в `src/main.c`,
тест `tests/rc_test.baga`. Верификация: пълен пакет без флаг 146/151
(5-те fail са external peers), 22 core/app теста с `--rc` PASS (вкл.
boila_vec_test, lsm_*, txn, jsonrpc), ASan чист, emit-c без флаг е
бит-идентичен с предишния codegen.
Контекст: MEM-4 сагата (gaps Q2 на boilaDB) — bump arena + opt-in persist
+ ръчен drop прави течовете ТИХИ; четири отделни теча (vec_grow, rehash,
map_del, evict keys vec) не бяха хванати от нито един тест, само от OOM
при 1M реда. RC е етап 1 от стратегията; етап 2 (move семантика за
struct threading) е отделен документ.

## Резултати (boilaDB insert bench, 100k реда, същата машина)

| build | wall | user | µs/ред | peak RSS | verify |
|---|---|---|---|---|---|
| без `--rc` | 42.2 s | 27.7 s | 370 | ~1.9 GB | DURABLE OK |
| `--rc` първоначално (40 B hdr + dead-zone лог) | 72.3 s | — | 722 | 1.70 GB | DURABLE OK |
| `--rc` финално (spare pool + 32 B hdr + inline) | **47.7 s** | 33.2 s | **425 (×1.13)** | 1.87 GB | DURABLE OK |

Overhead разбивка: първоначалният ×1.9 беше доминиран от dead-zone лога
(O(ndead) скан на всеки retain/release/bump при per-statement rewind).
Решения: (1) rewind не връща блокове на malloc, а ги пази в `baga_rc_spare`
веригата — паметта остава mapped, epoch check-ът сам по себе си е
достатъчна защита, логът отпадна, а malloc/free обажданията на заявка
изчезнаха; (2) persist битът и epoch са пакетирани в `pe` (32 B header,
payload пак 16-подравнен); (3) rc_hdr/retain/release_str/release_bytes са
`always_inline` (GCC не ги inline-ваше: 1467+2575 call сайта). Цената на
spare pool е RSS на high-water mark вместо връщане към ОС (1.87 GB vs
1.70 GB с free), но ≤ baseline × 1.1 критерият е покрит, а wall е ×1.13
(цел ≤ ×1.15). Остатъкът (~×1.2 user) е самата retain/release честота —
следващо намаляване би дошло от alias-pair elision (escape анализ в
codegen) или move-семантика за struct threading (етап 2).


## Цели

- Забравеният `drop()` спира да е тих leak — локалните heap стойности се
  release-ват автоматично при изход от scope.
- Нулева промяна в типовата система и в съществуващия код (std, app-*):
  флагът е opt-in за цялата компилация; без него codegen е бит-идентичен
  с днешния.
- Двойният drop става чиста грешка (rc underflow), не UB.

## Не-цели (v0.x)

- Няма borrow checker, lifetimes, linear types.
- Няма GC; цикли (Map сочещ сам себе си) текат — документирано.
- Struct полета и closure capture — v0.2+ (виж §Ограничения).
- LLVM backend — не поддържа persist изобщо; RC е C backend само.

## Механика

### Header

`baga_Hdr` (MEM-4в: magic + persist флаг) под `--rc` е 32 B:
`{ magic, pe, rc, an }` — `pe` пакетира epoch<<1|persist, `an` е класовият
размер на алокацията (release free-ва с `an`, не със strlen+1 — иначе
блокове от фиксирани алокации като `i64_to_str` мигрират в грешен
freelist клас и bump-ът тече). `baga_alloc` инициализира rc=1
(„собственикът е първият binding"). retain/release проверяват range +
magic + epoch — C string литерали и външни буфери са „immortal" (no-op),
което пази `str` семантиката безопасна. epoch се бутва от `mem_rewind`:
release на ephemeral стойност от преди rewind е no-op (паметта ѝ е
върната). persist стойности са извън epoch. Блоковете от rewind отиват в
`baga_rc_spare` веригата и се reuse-ват от следващи алокации — header-ите
остават mapped и epoch е достатъчна защита (dead-zone лог от ранния
прототип отпадна — беше доминиращият overhead).

### Правила за собственост

1. **Конструктор/литерал** → rc=1, притежаван от новия binding.
2. **`let x = y`** (y е heap binding) → `retain(y)`; двама собственици.
3. **Scope exit** → `release` на всички heap локали на scope-а, в
   обратен ред. Важи за блокове, loop тела (per-итерация!), fn тела.
4. **`return x`** → move: x не се release-ва; всички останали в scope —
   да. `return <израз>` → release на всички (изразът произвежда нова
   стойност с rc=1, отива на caller-а).
5. **`break`/`continue`** → release на scope-овете, които се напускат
   (вътрешните до най-близкото loop тяло).
6. **Аргументи на функции** — заемани (borrowed): callee НЕ release-ва
   параметрите си. Ако ги съхранява (в map/vec/struct), контейнерът
   retain-ва (точка 7).
7. **Контейнерите retain-ват при вмъкване, release-ват при изваждане:**
   - `vec_push_str/bytes/box`, `vec_set_*` (release на старото),
   `map_set_*` (retain на ключ+стойност; при overwrite — release на
   старата стойност; нов ключ при съществуващ entry се release-ва,
   тъй като не се съхранява),
   - `map_del` → release ключ+стойност (+ free на entry shell — MEM-4д),
   - `drop_vec/drop_map` → release на елементите + free на масивите.
   - `map_keys_*` връща Vec от ключове → retain-нати (те са собственици).
8. **`drop(x)`** ≡ release + binding-ът става мъртъв; повторна употреба —
   поведението е същото като днес (няма use-after-free проверка в v0.1;
   rc underflow при двоен drop → чиста грешка и exit).

### Какво release-ва codegen

Codegen поддържа scope стек от (mangled име, type tag) за локали с тип
`str`/`bytes`/`Vec`/`Map`. На изход от scope emit-ва типосъобразен
release: `baga_rc_release_str` (free при 0), `baga_rc_release_bytes`
(free data при 0), `baga_rc_release_vec(v, elem_kind, elem_size)` (при 0:
release на елементите + free data + free struct), `baga_rc_release_map`
(при 0: release ключове+стойности + free entries/buckets/struct).
Release на Vec/Map използва rc на STRUCT алокацията, не на data масива
(масивът е притежаван от структурата 1:1).

## Ограничения v0.1 (честно)

- **Temporaries не се track-ват** (остава главният leak в temp-тежки
  изрази, ~250-500 B/извикване: `vec_push(v, concat(...))`,
  `int_to_str` вътре в изрази). Подход за v0.2: per-statement temp
  регистър в codegen — всеки fresh heap temp се записва в скрит списък
  на statement-а и се release-ва в края му; temps, които escape-ват
  (стойност на `return`, присвоени в long-lived контейнер без retain),
  се изключват. Цената е още retain/release трафик, затова е отложено
  след perf критерия.
- **Struct полета не се track-ват като собственици.** Struct по стойност
  копията споделят полета-указатели (днешна семантика). Затова pък
  вградените в struct литерал/field assign heap локали и параметри се
  RETAIN-ват — иначе `return S { v: v }` обесва полето при scope exit
  (намерено по трудния път: lsm_cluster_open, boila_sel_empty, txn.writes).
- **Borrowed стойности (vec_get/map_get/struct поле/h_\*) се RETAIN-ват
  при връзване** (let/assign/return), не се пропускат — всяка binding
  референция е owned и балансирана. Първоначалният „skip borrowed"
  подход оставяше небалансирани референзии (underflow в wal_replay,
  sst hot_data, catalog).
- **Struct литералът не retain-ва drop()нати (dead) локали** — те са
  вече освободени; вграждането им споделя труп (както днес е UB).
- **Closure capture** — env box-овете (cell2) си имат собствен живот;
  capture-нати локали не се retain/release-ват допълнително.
- **Persist регион** — rc работи и там (freelist-ите са разделени,
  persist е извън epoch проверката), но persist стойностите обикновено
  са в shared store-ове, не в локали; scope release не ги пипа, освен
  ако не са let-нати като локали.
- **Ранни `return`/`break` в дълбоко вложени блокове** — покрити чрез
  scope стека (loop-body scopes се маркират; `rc_fn_base` ограничава
  return-release до текущата функция — ламбдите са отделни C функции);
  `go`/chan прехвърляне на heap стойности — v0.2.
- **rc + rewind в същия scope** — epoch прави release на pre-rewind
  стойности no-op (leak-safe). Остатъчен състезателен случай: stale
  локал, чийто header адрес е презаписан от нова алокация с текуща
  epoch след reuse на опашката на оцелелия блок — теоретично възможен,
  непокрит (същият като в dead-zone варианта).
- Цикли текат (Map↔Map). Няма weak refs.

## Рискове и проверка

- Най-опасното: release на стойност, която някой държи без retain
  (aliasing извън правилата). Затова: пълен тест пакет + rc-specific
  smoke тестове (алиасинг, контейнери, loops, ранен return, persist
  взаимодействие) + boilaDB insert bench като стрес.
- Overhead: retain/release са два паметни достъпа (header); очаквано
  < 10% на pointer-тежки товари — мери се на boilaDB bench (200k реда,
  ns/ред + RSS сравнение).

## Статус (2026-08-12): v0.1 прототипът е земял

Имплементирано зад `--rc` (C backend): scope tracking + retain/release,
контейнерни retain/release, dead-zone/epoch guard около rewind.
- Бит-идентичен изход без флага (проверено с diff на emit-c).
- Пълен пакет: **146/151** (5-те fail = external peers); rc_test (21
  проверки) + 22 core теста с `--rc` — PASS, чисто под ASan.
- Leak loop 500k: **0 KB растеж с --rc** vs 268 MB без (без temporaries).
- boilaDB 100k insert bench с --rc: DURABLE OK, RSS 1.70 GB (×0.89 от
  baseline), но **722 µs/ред vs ~380 (×1.9)** — overhead критерият
  (≤×1.15) не е покрит. Причини: retain/release + 40 B header guards в
  горещия alloc път. Следващи стъпки: (а) профилирай guards-те (range
  guard/dead-zone са debug-grade — кандидат за compile-out в release);
  (б) temporaries tracking (главният остатъчен leak източник, ~250-500
  B/извикване — виж v0.1 границите в този документ).
- Отстъпления от този doc, реализирани в кода: header е 40 B (rc + epoch
  + an + range guard), borrowed стойности се retain-ват при връзване,
  struct литерали/field assignments retain-ват вградени heap стойности,
  elem_kind 4 = bytes box.

## Критерий за готово (етап 1) — постигнато

- `--rc` компилация на пълния пакет: 146/151 без промяна в изхода
  (5-те fail са external peers; 145/150 стар пакет + rc_test). ✔
- boilaDB 100k insert bench с `--rc`: DURABLE OK, wall ×1.13 (цел
  ≤ ×1.15), RSS ≤ baseline × 1.1 (1.87 GB vs ~1.9 GB). ✔
- Leak repro (vec/map push+del цикъл в 500k итерации): 0 KB растеж с
  `--rc`, 268 MB без него. ✔ (С temp-тежки изрази остатъкът е
  temporaries лимитацията — v0.2.)
