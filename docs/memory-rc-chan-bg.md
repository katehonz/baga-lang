# go/chan прехвърляне на heap стойности — design (RC следваща стъпка)

Статус: **бележка за обсъждане**, не имплементирано. Зависи от RC1–RC4 +
RC1.4 (`docs/memory-rc-bg.md`). Каналите днес носят само `i64`
(`baga_chan_send/recv` в `src/codegen_c.c`). Heap стойности минават
през handle cast-ове: `str_h` / `bytes_h` / `map_h` → `i64` → `h_*`.

## Какво вече държи RC1.4

`map_h`/`str_h`/`bytes_h` retain-ват (immortal escape). След
`chan_send(c, str_h(s))` картата/низът оцелява scope release-а на `s`.
`h_str(chan_recv(c))` е borrowed-init → още един retain при връзване +
release при scope exit на recv binding-а. Нетно: обектът живее до края
на процеса (handle-ът никога не release-ва). Leak-safe, същата
семантика като без `--rc` (arena-bound str никога не се free-ва).

Това покрива днешния hop идиом (rocksbaga workers, queuebaga, boila MT
ctx). Няма UAF по този път след RC1.4.

## Какво остава непокрито

1. **Истински typed send на `str`/`bytes`/`Vec`/`Map`** — каналът е
   само `i64`; няма `chan_send(c, heap_val)` без handle. Ако някой
   кастне указател ръчно (`str_h` е единственият легален път), сме ОК.
2. **Балансиран recv** — днес handle-ът е immortal. По-стриктен модел:
   send = retain (или move при last-use), recv = owned binding, който
   release-ва при scope exit, **без** допълнителния immortal retain на
   `*_h`. Изисква каналът да знае tag-а на payload-а (str vs bytes vs
   map) — днес го няма.
3. **`go` capture на heap локал** — ламбдата/worker-ът чете локала след
   като enclosing scope го е release-нал. Днес `go` приема `i64` arg
   (пак handle). Closure capture не retain-ва (документирана граница).
4. **Кръстосан thread + rewind** — arena-та е `__thread`. Handle към
   ephemeral блок на нишка A, четен от нишка B след rewind на A, е UAF
   независимо от rc. Persist регион + RC1.4 immortal е безопасният път
   за shared state.

## Предложение (ако се прави)

Не пипай `baga_chan_*` runtime-а (остава `i64` буфер). Два слоя:

**A. Оставяме RC1.4 immortal handles.** Нулева нова работа. Честно:
chan hop тече (като без `--rc`). Препоръка за v0.x.

**B. Tag-нат payload (по-късно):** `chan_send` на heap ident/temp
emit-ва retain + tag в горните битове / странична дума; `chan_recv` +
`let x: str = …` release-ва tag-а при scope exit. Изисква типова
информация на recv сайта и забрана за гол `i64` recv на heap канал.
Голяма промяна, не за сега.

**C. `go` capture retain** — ако ламбда capture-не heap локал, retain
при spawn, release в края на worker-а (като env box). Отделна задача;
closure capture вече е изрично изключен.

## Критерий за A (статус кво след RC1.4)

- `par_test`, `workers_test`, `queue_test` с `--rc` — PASS (вече).
- Няма нов underflow/FPE от handle hop.
- Имплементация на B/C — само след отделно решение.

Препоръка: **A**. B пипа типовата система и канала; C е към struct-полета
(задача 4). Leak > корупция вече е спазено.
