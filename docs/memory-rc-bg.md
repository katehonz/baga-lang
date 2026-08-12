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

## v2.1: elision на borrowed-retain двойки

Контекст: остатъчният --rc overhead след RC1+RC2 (~×1.20 user на boilaDB
insert bench) е предимно retain/release трафик от `let x = <borrowed>`
връзвания (`vec_get`/`map_get`/struct поле → retain при връзване +
release при scope exit). Когато локалът само се чете и умира в scope-а,
двойката е чиста загуба.

**Правило:** за `let x = <borrowed израз>` двойката се ЕЛИМИНИРА (нито
retain при връзване, нито регистрация за scope release — локалът е чисто
заеман) САМО ако са изпълнени и двете условия в прозореца от let-а до
края на enclosing блока (scope-а на x):

- **(а) x не escape-ва:** не е аргумент на drop/vec_push/vec_set/map_set/
  map_del, не е пряк `return x`, не е вграден в struct литерал, не е
  присвоен на друго име (`let y = x` или `y = x`), не е capture-нат от
  ламбда, не е преприсвоен (`x = …`). Извиквания с x като аргумент са ОК
  (параметрите са borrowed по конвенция).
- **(б) източникът не се мутира и не се алиасира:** източният
  контейнер/struct (базов ident на field верига — `v`, `m`, `sel` в
  `sel.group_cols`) не е в цялата функция: алиасиран под друго име
  (`let m2 = m`, `s.f = m`, struct литерал, lambda capture, аргумент на
  vec_push/vec_set/map_set/map_del/drop или на не-pure повикване), а в
  прозореца след let-а също: цел на присвояване или field/index assign.
  Алиасите се изключват глобално, защото мутация през тях в прозореца е
  невидима за локален анализ.

Несигурно → днешното поведение (retain при връзване + регистрация).
Брояч: `/* RC2.1: borrowed pair elisions: N */` в края на изхода.
Assign-формата (`x = <borrowed>` в съществуващ локал) НЕ се елиминира в
v2.1 (flow-sensitive dead маркиране — отложено).

**Резултати (v2.1, boilaDB 100k insert bench, същата машина):**
- Елиминирани двойки: **42** в insert_write компилацията (първоначално 84
  преди whole-fn alias scan-а — консервативната цена), 2 в borrow_test.
- Bench: wall 47.7 s (без промяна), user 33.0 s (×1.19 спрямо 27.7 s
  baseline — в шума на RC2 build-а, целта ≤ ×1.05 отново не е
  постигната), peak RSS 1.88 GB, verify DURABLE OK.
- Извод: механизмът е коректен и безопасен, но elidable сайтовете са
  малко и извън горещите пътеки на този bench. Остатъчният трафик:
  container retains при push/set, alias-и на параметри (borrowed — не
  подлежат на elision), и borrowed връзвания, които escape-ват в
  struct/контейнер (самата threading идиома). Верификация: 24/24 --rc
  батерия (с borrow_test), ASan чист, 148/153 пакет без флаг,
  бит-идентичен emit-c без флаг. Следваща цел: container move варианти
  (push/set без retain при last-use аргумент — по-горещите пътеки) или
  param-alias pair elision. [Container move е реализиран — RC3, вж.
  docs/move-semantics-bg.md §v3.]

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

- **Temporaries не се track-ват** ~~(остава главният leak в temp-тежки
  изрази)~~ — **РЕШЕНО в v0.2 (RC4), виж §v0.2 по-долу.** v0.3 покрива
  и условията на if/while и for-range hi/lo. Остават непокрити temp-ове
  в match, try/catch, if-израз клонове и десен операнд на &&/||
  (условна оценка — консервативно изключени).

## v0.2 (RC4): per-statement temp регистър

Контекст: след RC1–RC3 главният оставащ leak са fresh heap temp-ове —
резултати от `concat`/`int_to_str`/`vec_slice`/user fn с heap return,
които не се връзват в локал, а отиват директно като аргументи
(`vec_push(v, concat(...))`, `map_set(m, k, int_to_str(i))`). Контейнерът
retain-ва своята референция, но собствената rc=1 на temp-а никой не
release-ва (~370 B/итерация в типичен insert цикъл).

**Механика:** за statement-ите LET / EXPR_STMT (вкл. assign) / RETURN
codegen прави pre-pass обход на root израза и събира fresh heap temp-овете
(NODE_CALL с heap тип, който не е borrowed по `rc_borrowed_init`, плюс
NODE_TO_STR). Преди statement-а се emit-ват `__auto_type __rc_tmpN = …`
декларации (вътрешните temp-ове първи — обратен ред на pre-order
събирането), в основния израз temp възлите се заместват с имената
(choke point в `emit_expr`), а след statement-а temp-овете се release-ват
типосъобразно. За return release-ът е вътре в `__rc_ret` блока, преди
самия `return`. Root-ът на let/assign/return е bound (собствеността се
предава на локала/caller-а) — не е temp; root на bare EXPR_STMT с heap
резултат е дискарднат — temp е.

**Консервативни изключвания (не се слиза в тях):**
- struct литерал — полетата споделят указателя без retain (release би
  обесил полето);
- ламбда (отделна C функция), try/catch (ранен return), match, if-израз;
- десен операнд на `&&`/`||` (условна оценка — hoisting би я направил
  безусловна);
- аргументи на `drop()` (самият drop е release пътят);
- borrowed производни (vec_get/map_get/поле/h_*) — не са собствени.

**v0.3:** условия на `if`/`while` и `for x in lo..hi` (hi се преоценява
на итерация; lo — веднъж). Codegen wrap-ва условието в GNU
`({ tmp-декларации; __auto_type c = cond; release; c; })` — оценката е
на всяко влизане, release-ът е веднага след нея, преди тялото/клоновете.
`break`/`continue` не пипат cond temp-овете (вече са пуснати). Тест:
`while_cond_temp` / `if_cond_temp` / `for_hi_temp` в temp_test.

Вложени statement-и (if-изрази, ламбди в temp-тежки изрази) save/restore-ват
регистъра — per-statement дисциплината се пази. Брояч:
`/* RC4: temp releases: N */`. Тест: `tests/temp_test.baga` (10 проверки).

**Резултати (v0.2):**
- Leak repro (2M итерации `map_set(m, "fixed", concat("v", int_to_str(i)))`
  — 2 temp-а/итерация): **41 MB** maxrss с RC4 срещу **780 MB** без
  (~370 B/итерация leak елиминиран).
- temp releases: 24 в temp_test, **857** в boilaDB insert_write (за
  сравнение: RC2 254, RC2.1 42, RC3 23 — temp-овете са порядък повече
  от всички move сайтове, това е горещият път).
- Bench (boilaDB 100k insert, същата машина): **415 µs/ред** (RC3: 431,
  RC1 финал: 425 — ~3% под финала; RC1.3 dead-marking-ът спестява и
  release работата по per-request rewind пътя), peak RSS **1.76 GB**
  (RC3: 1.80, RC2: 1.83 — temp-овете вече не се задържат), verify
  **DURABLE OK**. Wall/user не са 1:1 сравними с предишните рънове
  (различна измервателна верига).
- Верификация: temp_test (11 проверки) PASS без флаг, с --rc и под
  ASan+UBSan (както rc_test/move_test/borrow_test/cmove_test/http_test/
  pg_test/sumtype_test/mem_rewind_test); emit-c без флаг бит-идентичен с
  HEAD (7 файла diff); пълен пакет без флаг — само pre-existing boilaDB
  filesize гейт; пълна tests/ батерия с --rc: **149/155** — 6-те FAIL са
  pre-existing и на HEAD (5 external peers + boila_ts FPE). RC4 извади
  наяве три латентни RC1 пропуска, фикснати в хода: RC1.1 (borrowed поле
  директно в struct литерал → http_test segfault), RC1.2 (borrowed/ident
  match arm стойност без retain → pg_test underflow, sumtype_test
  грешен payload — плюс забрана за temp-ове в enum конструктор),
  RC1.3 (stale release на локали след mem_rewind — dead-marking по
  mark watermark).
- Извод: цел ≤ ×1.05 user пак не е документирано постигната (ns/ред е в
  шума на RC1 финал), но главният leak източник е затворен, а RSS трендът
  е надолу (1.87 → 1.83 → 1.80 → 1.76 GB през RC1→RC4). Остатъчен
  overhead е самата retain/release честота на НЕ-temp трафика (alias-и на
  параметри, borrowed връзвания, контейнерни retain-ове на не-last-use
  стойности).
- **Struct полета — RC5 v0.1** (`docs/memory-rc-struct-bg.md`): преките
  heap полета на свеж литерал / alias се release-ват при scope exit.
  `vec_get`/`f()` копия не се track-ват. Вложени struct-и и `Vec<S>`
  още текат. Затова pък
  вградените в struct литерал/field assign heap локали и параметри се
  RETAIN-ват — иначе `return S { v: v }` обесва полето при scope exit
  (намерено по трудния път: lsm_cluster_open, boila_sel_empty, txn.writes).
  **RC1.2:** match-израз, произвеждащ heap стойност от borrowed arm
  (поле на binding, vec_get/map_get резултат) или от ident (binding-ът е
  копие-алиас на payload), retain-ва arm стойността — резултатът е owned
  по конвенцията. Латентен пропуск, маскиран от temp течовете преди RC4
  (pg_err → sqlstate underflow). Enum конструкторите копират payload по
  стойност БЕЗ retain (като struct литерал преди RC1.1) — `Raw(local)`
  дели референцията; това е документирана граница (payload-ите не се
  track-ват), а RC4 не release-ва temp-ове в аргументите им.
  **RC1.1:** borrowed стойности (vec_get/map_get/поле/h_*), вградени
  ДИРЕКТНО в struct литерал (`Request { method: vec_get(parts, 0) }`),
  също се retain-ват (през `__rc_sl.<field>`) — преди това полето
  алиасираше източника без retain и release на контейнера го обесваше
  (латентен пропуск, излязъл наяве чак когато RC4 temp release-ите
  пренаредиха freelist-а: http_test segfault; тест
  `struct_lit_borrow_alive` в temp_test).
  **RC1.4:** `map_h`/`str_h`/`bytes_h` са immortal escape (i64 handle
  през go_bg/struct поле). Без --rc обектът е arena-bound и никога не
  се free-ва; под --rc scope release на източника обесваше handle-а
  (`boila_open_mt` → `map_h(mus)` → `boila_shard_lock` → `m->nb==0`
  SIGFPE в `baga_map_slot`). Handle helper-ите retain-ват (картата /
  низа / bytes data) — handle-ът държи референцията, локалът си
  release-ва своята. Leak-safe: handle-ите никога не release-ват
  (същата семантика като без флаг). Тест: `map_h_survives` в rc_test.
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
  `go`/chan прехвърляне на heap стойности — решено A
  (`docs/memory-rc-chan-bg.md`: RC1.4 immortal handles; B/C отложени).
- **rc + rewind в същия scope** — epoch прави release на pre-rewind
  стойности no-op (leak-safe). **RC1.3:** при `mem_rewind(m)` statement
  codegen маркира dead всички track-нати локали, регистрирани след
  watermark-а на съответния `mem_mark` (LIFO стек от watermark-ове per
  fn) — техните header-и са във върнатата памет и bump reuse би ги
  overwrite-нал с валидна текуща epoch (stale release → underflow/
  чужд free; излязло с RC4 в mem_rewind_test/pg_test). Покритият случай
  е локали, ДЕКЛАРИРАНИ след mark-а; pre-mark локал, преприсвоен на
  post-mark стойност, остава теоретично непокрит (както преди). Посоката
  е leak-safe: алиас на pre-mark стойност, деклариран след mark-а, губи
  release-а си (leak, не корупция).
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
