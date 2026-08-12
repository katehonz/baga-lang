# Паметов модел v2: refcount (RC) — design

Статус: **предложение + прототип зад `--rc` флаг** (C backend само).
Контекст: MEM-4 сагата (gaps Q2 на boilaDB) — bump arena + opt-in persist
+ ръчен drop прави течовете ТИХИ; четири отделни теча (vec_grow, rehash,
map_del, evict keys vec) не бяха хванати от нито един тест, само от OOM
при 1M реда. RC е етап 1 от стратегията; етап 2 (move семантика за
struct threading) е отделен документ.

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

`baga_Hdr` (MEM-4в: magic + persist флаг) получава трето поле: `rc`.
`baga_alloc` инициализира rc=1 („собственикът е първият binding").
retain/release проверяват magic — C string литерали и външни буфери
са „immortal" (no-op), което пази `str` семантиката безопасна.

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

- **Struct полета не се track-ват.** Struct по стойност копията споделят
  полета-указатели (днешна семантика). Локал от struct тип НЕ се
  release-ва при scope exit; полето, извадено в binding (`let k = m.f`),
  се държи като borrowed освен ако не е резултат от функция/конструктор.
  → threading моделът на rocksbaga (`db = f(db)`) е безопасен.
- **Closure capture** — env box-овете (cell2) си имат собствен живот;
  capture-нати локали не се retain/release-ват допълнително.
- **Persist регион** — rc работи и там (freelist-ите са разделени),
  но persist стойностите обикновено са в shared store-ове, не в локали;
  scope release не ги пипа, освен ако не са let-нати като локали.
- **Ранни `return`/`break` в дълбоко вложени блокове** — покрити чрез
  scope стека; `go`/chan прехвърляне на heap стойности — v0.2.
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

## Критерий за готово (етап 1)

- `--rc` компилация на пълния пакет: 145/150 без промяна в изхода.
- boilaDB 200k insert bench с `--rc`: същия резултат (DURABLE OK),
  RSS ≤ baseline × 1.1, ns/ред ≤ baseline × 1.15.
- Leak repro (vec/map del+push цикъл в loop) — плосък RSS с --rc,
  растящ без него.
