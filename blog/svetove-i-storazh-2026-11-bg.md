# Бага през ноември 2026: светове, REPL и сторидж в класа на RocksDB

**Бага** (⚔️) е език за системен и продуктов код, в който компилаторът е **съдията**: пишете спецификации с `requires`/`ensures`/`decreases`, а статичният верификатор доказва или оборва имплементацията преди каквото и да е да се изпълни — с конкретен контрапример при оборване. Грешките са ефекти в типа (`!IO`, `?`, `catch`), а не изключения. Компилаторът се компилира сам до неподвижна точка (`make self`: baga2 == baga3 байт по байт) и има два бекенда — C с нула зависимости и LLVM.

> *„Въпросът не е „какво е ново". Въпросът е „какво още не е слепено заедно".*

Този пост е за новото от последните седмици: **световете** (нов модел на памет с машинно проверен протокол), **rocksbaga** с честен бенчмарк срещу RocksDB, REPL и разширения верификатор.

## Световете: цял граф от данни като стойност

Двете стари паметови дисциплини имат документиран таван: RC (`--rc`) тече при цикли, а bump арената е thread-local и не се прехвърля между нишки. Реалните сървърни структури обаче са **светове** — план-кеш на заявка, граф на документ, snapshot на таблица: цялост, която се ражда, замразява, споделя и умира наведнъж.

Световете в Бага (W0–W4.2, builtin-и `world_*`/`wvec_*`/`wmap_*`/`wcache_*` в C **и** LLVM бекенда) отговарят на въпроса: *как да направя цял собствен граф — включително цикли — една прехвърляема, замразяема, сериализуема стойност?*

```
world_new() ──▶ mutable ──world_freeze──▶ frozen ──chan_send──▶ moved
                    │                        │ (само четци,
                    └──world_free──▶ freed   │  zero-copy между нишки)
                        (цялата верига наведнъж)
```

Разликата от Project Verona / Pony / Cyclone не е в идеята, а в **вердикта**: протоколът не е конвенция, а заключение на верификатора. `mutate-after-freeze`, `use-after-free` и `use-after-move` се оборват с КОНКРЕТЕН контрапример, а `--json` ги прави машинен канал за AI агенти:

```bash
$ ./baga --verify examples/verify/worlds.baga     # 16 ensures + 10 протокола
$ ./baga --verify --json examples/verify/worlds_cap.baga
```

Ораклите са разделени: ядро на региона, споделяне и договори, capability графи, делегиране, канален rely–guarantee, world блокове — плюс `bench/worlds.baga` (streaming) и `bench/worlds_micro.baga` (горещ цикъл в кеша), с checksum валидация, не само timing.

Световете вече работят в produkцията на екосистемата (виж [docs/app-worlds-bg.md](../docs/app-worlds-bg.md)): статистика и live-connections в `boilaDB` живеят в замразен свят (`wcache_add`) и горещият път не взима глобален mutex; `/metrics` в `fmrbaga` и `cloudbaga` е в замразен свят през новия `WMet` в `metbaga`; `queuebaga` кешира терминални статуси read-through. boilaDB, rocksbaga и queuebaga вече минават и през LLVM (`tests/llvm_storage.sh`, C↔LLVM паритет).

## rocksbaga срещу RocksDB — сега с честни числа

**rocksbaga** е издръжлив LSM KV сторидж в класа на RocksDB, написан на чист Бага: page cache, WAL с crc32c, мемтаблица + L0…L3 компакции, SST (BAGASST5) с bloom sidecar, RESP2 сървър с poll multi-conn и MT режим.

Бенчмаркът е head-to-head на **чистите двигатели** — едни и същи ключове, стойности и брой операции, един писател, без RESP/TCP (`bench/rocks/run_vs_rocksdb.sh`, втората страна е официалният RocksDB през Python `rocksdict`):

```bash
BENCH_DIR=/var/tmp/rocksbench BENCH_N=5000 ./bench/rocks/run_vs_rocksdb.sh            # durable
BENCH_DIR=/var/tmp/rocksbench BENCH_N=5000 BENCH_MODE=batch ./bench/rocks/run_vs_rocksdb.sh
```

Последна scorecard (2026-11-01, реален диск btrfs на NVMe, n=5000, vlen=100 — rocksbaga като % от ops/s на RocksDB):

| режим | PUT | GET_SEQ | GET_RND |
|------|----:|--------:|--------:|
| durable (fsync на всеки put) | **124%** | **127%** | **114%** |
| batch (bulk load) | **82%** | **142%** | **148%** |

Честна бележка, която самият runner сега изписва: **по подразбиране бенчът пише в `/tmp`, а той често е tmpfs** — там fsync е no-op и durable числата измерват RAM, не диск. Затова harness-ът вече поддържа `BENCH_DIR` за реален диск и предупреждава при tmpfs. Пълната история е в `bench/rocks/results/vs-rocksdb-scorecard.md`.

Това е throughput бар под този harness, не твърдение за feature-паритет с RocksDB — но чист LSM на Бага в същата лига като десетилетие C++ работа е число, което си струва да притежаваш.

## Други нови неща

- **REPL** — `make repl`: декларации и `let`-ове се пазят между редовете; удобен за преподаване и за бързи проверки на езика.
- **Верификаторът расте, без да губи честност** — M31: индукция в while-цикли (wait-for scan); M32: CDCL clause learning в bitvector SAT солвъра, пуснат след формален soundness audit (`docs/cdcl-audit-2026-10-31.md`). Извън фрагмента отговорът остава НЕИЗВЕСТНО — никога фалшиво доказано.
- **Onboarding** — `docs/GETTING-STARTED-bg.md` с машинно проверени примери.
- **Екосистемата**: boilaDB (мултимодална SQL база на PG v3 wire), fmrbaga, ormbaga, jwtbaga (RS256/ES256 вече детерминистични), raftbaga, txnbaga, queuebaga и още десетици пакети под `app-product/`, строени със `sandak`.

## Бърз старт

```bash
git clone git@github.com:katehonz/baga-lang.git && cd baga-lang
git submodule update --init --recursive
make
./baga examples/zdravei.baga        # Здравей, багатуре. Боят започва.
make repl                            # интерактивна обвивка
./baga --verify examples/verify/fact_full.baga   # факториел, напълно доказан
```

## Честен статус

Бага е **работен прототип**, не производствен език. Верификаторът е умишлено малък и казва НЕИЗВЕСТНО вместо да гадае; общата нелинейна аритметика е оставащото стълбище. Лиценз MIT, кодът е на [GitHub](https://github.com/katehonz/baga-lang), документацията — на български и английски, а отделните пакети — в `github.com/bagalang/*`.

---

*Нищо не е ново. Но нищо не е навременно. Регионната памет е от 90-те — може би сега ѝ е времето да стане проверена дисциплина, а не конвенция.*

⚔️
