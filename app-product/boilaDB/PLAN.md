# boilaDB — План (v2: високо натоварване, PostgreSQL синтаксис)

Фази са последователни; всяка завършва със зелени тестове
(`./scripts/baga-test`) и записано състояние в `gaps.md`.
Критерий "готово" на фаза: работи end-to-end през PG wire и/или HTTP,
персистентно е (restart test) и има **измерен benchmark гейт** в `bench/`
(вж. гейтовете на всяка фаза — числа, не обещания).

**Правила за всички фази:**
- Нито един файл > 400 реда (`scripts/filesize.sh` върви с тестовете;
  деленето е превантивно, не retroactive като при barabadb). Вж.
  ARCHITECTURE.md §9.
- SQL извън документираното подмножество → `0A000 feature_not_supported`.
  Никога тиха грешна семантика спрямо PostgreSQL.
- **Geo/GPS не се реализират в нито една фаза** — няма geometry типове,
  пространствени индекси, GPS ingestion.

## P0 — Скелет (core + sharded storage)
- `sandak.toml` (path deps: `../../std`, `../rocksbaga`, `../httpdbaga`,
  `../logbaga`, `../metbaga`, `../queuebaga`, `../flagbaga`,
  `../ctxbaga`, `../testbaga`).
- `core/value.baga` — типизиран value codec + 3VL compare; тестове за
  sort-order byte кодировка (отрицателни числа, `null < всичко`,
  timestamptz).
- `core/codec.baga` — key encoding `<cf>|<shard>|<table>|<pk>|<ver>`.
- `storage/shards.baga` — N rocksbaga инстанции (по подразбиране = ядра),
  put/get/scan/del по shard; restart-персистентност тест.
- `tools/serve.baga` — HTTP `/health` + `/metrics` (metbaga).
- `scripts/filesize.sh` — закача се към runner-а още тук.
- **Гейт:** процесът стартира с N shards, kill -9 → WAL replay чист.

## P1 — SQL read path v0 + каталог
- Lexer/parser за `SELECT` (само PK point lookup + range по PK);
  `catalog/schema.baga` в `sys` key-space; HTTP `POST /sql`; CLI shell.
- **Гейт:** read path-ът работи end-to-end (HTTP + shell), каталогът е
  персистентен след рестарт, грешките носят SQLSTATE. **Perf baseline
  (измерено):** SQL point SELECT 6102 ns/op срещу суров GET 1193 ns/op
  (511%, bench/boila/results/point-read-2026-08-08.md) — мишената ≤ 1.25×
  става задължаваща с plan cache-а в P5 (gaps Q1).

## P2 — Много бази данни (multi-database, PostgreSQL/MySQL модел)
- Сървърен root: `BOILA_PATH` съдържа `.meta` (registry, 1 rocksbaga
  shard) и flat файлове на база — `<db>.db` (shard пътища
  `<db>.db.s{i}`); root-ът се създава с `mkdir` през extern FFI, защото
  езикът няма builtin (gaps S4). `storage/databases.baga`: `BoilaServer`
  { meta store, отворени (ime, store) двойки, lazy open, таван
  `BOILA_MAX_DB` (default 64), FIFO eviction }; при пръв init се създава
  базата по подразбиране `boila` (моделът на `postgres` в PostgreSQL).
- SQL: `CREATE DATABASE име`, `DROP DATABASE име` (отказ ако е текущата
  за сесията — 55006; чистенето на файлове е по известни shard-шаблони —
  baga няма readdir), `USE име`. Всички останали заявления се изпълняват
  в текущата за сесията база; каталозите са per-database. Cross-database
  заявки/транзакции няма в v1 (`0A000`). HTTP е без сесия — базата идва
  от `?db=` (default `boila`).
- Клиенти: HTTP `POST /sql?db=<име>` (default = `boila`); shell — `USE`
  и текущата база в prompt-а. PG wire-ът (P6) ще носи името на базата в
  startup съобщението — точно като Postgres.
- **rocksbaga: без промени** — per-dir клъстерите съществуват от R32
  (проверено при P0); registry-то е обикновен boilaDB key-space.
- **Гейт:** две бази с едноименни таблици не си смесват данните;
  CREATE/DROP/USE през HTTP + shell; рестарт → registry-то и всички
  бази четими; DROP чисти файловете на базата.

## P3 — DML + вторични индекси + group commit
- `INSERT` (multi-row, `ON CONFLICT DO NOTHING | DO UPDATE SET`,
  `RETURNING`), `UPDATE`, `DELETE`; `CREATE INDEX` с build върху
  съществуващи редове и синхронизация при DML; SELECT по индексирана
  колона. Индексните дефиниции живеят в schema row-а (O(1) point GET —
  не scan; урокът K7).
- Group commit: statement-ниво — `boila_stmt_begin/commit` (един fsync
  на shard на заявление). Shard-owner нишките остават за wire фазите
  (P6), където concurrency-то реално се ползва; дотогава sync loop-ът
  притежава store-а (gaps H2).
- **Гейтове (измерени, bench/boila/results/insert-write-2026-08-08.md):**
  (а) group commit ≥ 3× спрямо sync-per-write — **1722%**;
  (б) durability без close (kill -9 семантика): **100k реда, 0 загубени,
  индексът валиден без rebuild**. Пълният 1M е блокиран от arena-та на
  езика (gaps Q2) и се връща с per-request arena управление в P6.

## P4 — MVCC транзакции (едно-сесийен buffered модел)
- `BEGIN [READ ONLY] / COMMIT / ROLLBACK`; писанията се буферират и се
  commit-ват като един fsync batch с монотонен commit LSN; данните в
  storage носят `[lsn 8B][row]` envelope (data/index CF). Изолация:
  storage непроменен до COMMIT; читателите виждат committed + собствените
  buffer писания; грешка в заявление → rollback на буфера. Транзакцията
  е фиксирана към базата си (USE/смяна на `?db=` → 0A000).
- Ревизия (честно): multi-version ключовете `<pk>|<ver_lsn_desc>` и
  commit sequencer-ът за конкурентни сесии са **преместени в P6** — в
  едно-сесийния sync режим (P4–P5) те са механизъм без потребител
  (gaps T2). Директни storage писания (извън SQL) трябва да wrap-ват с
  `boila_wrap_val`.
- **Гейтове (boila_txn_test, 29 проверки):** crash mid-transaction →
  чисто rollback (storage празен преди COMMIT); читател с отворен
  snapshot не вижда междинни писания; монотонни LSN за commit реда;
  restart durability; READ ONLY (25006), двоен BEGIN (25001).

## P5 — Пълен SELECT + planner
- Реализирано: `DISTINCT`, `ORDER BY` (multi-key, ASC/DESC) `LIMIT`/
  `OFFSET`, агрегати `count(*)/count(col)/sum/avg/min/max`, `GROUP BY` +
  `HAVING`, един `JOIN` (INNER/LEFT, ON equality) — nested loop или
  index nested loop при индекс по вътрешната колона (rule-based избор).
  Plan cache по суров SQL текст за едно-таблични SELECT-и; инвалидация
  при DDL. WHERE е само по pk/индексирана колона (от P1/P3).
- Ревизии (честно): hash join и query budget (deadline + max keys)
  остават за P6/P11 — при sync сървъра без конкурентност budget-ът няма
  потребител; `WITH RECURSIVE` идва с graph фазата (P10); avg е
  целочислено деление (gaps A1); кешът не покрива JOIN-и (gaps C1).
- **Гейтове (измерени, bench/boila/results/select-planner-2026-08-08.md):**
  (а) planner-ът избира индекса: index път 8.7 ms срещу seq scan 17.2 ms
  (5000 реда); (в) plan cache: cold 11.0 µs → warm 5.8 µs (1.89×),
  parse+plan дял ~0% при повтарящи се заявки (гейт < 5%).

## P6 — PG wire protocol v3
- `api/pgwire_*.baga`: startup/auth (cleartext + token), simple query,
  extended (Parse/Bind/Describe/Execute/Sync), prepared `$1..$n`;
  accept poll + bounded worker pool + admission control (`53300`/`57P03`).
- **Гейт:** `psql` и libpq клиент изпълняват целия P1–P5 синтаксис;
  10k concurrent connections → p99 ≤ 2× p99 при 1k (плоска крива);
  overhead спрямо HTTP пътя ≤ 10%.

## P7 — fts/ модал
- Tokenizer EN/BG, inverted index в `fts` key-space, BM25;
  `WHERE body @@ to_tsquery('…')`, `ts_rank`.
- **Гейт:** 100k документа, boolean + фразова заявка < 10 ms; индексът
  персистентен след рестарт без rebuild.

## P8 — vector/ модал
- `VECTOR(n)` колони; HNSW с възли/ребра в `vec` key-space + кеш на
  горните нива; оператори `<->`/`<=>`/`<#>`; `CREATE INDEX … USING hnsw`;
  metadata pre-filter през index/.
- **Гейтове:** 100k вектора × 128d, recall@10 ≥ 0.95 спрямо brute force;
  kNN заявка < 20 ms; рестарт → без rebuild (предимство #2).

## P9 — ts/ (time-series през SQL)
- `WITH (ttl_days = N)`, `time_bucket('1m', ts)` в GROUP BY, retention
  през `lsm_put_ex` + sweeper.
- **Гейт:** 1M точки, range + агрегация по прозорец < 50 ms; TTL реално
  чисти (dbsize спада). (Модал, който barabadb няма.)

## P10 — graph/ през WITH RECURSIVE
- Adjacency в `graph` key-space (двупосочен), BFS/DFS/Dijkstra примитиви,
  изпълнение през рекурсивни CTE-та.
- **Гейт:** 1M ребра, BFS дълбочина 3 < 100 ms, персистентно след рестарт.

## P11 — Втвърдяване и честни benchmarks при високо натоварване
- `bench/harness.baga`: стълба 100 / 1k / 10k клиенти (четене, писане,
  смес 80/20); сравнение с barabadb (client-server и при двамата),
  SQLite и суров rocksbaga като таван.
- Метрики: throughput, p50/p99, memory, fsync batch size, recovery time.
- Chaos: kill -9 по време на compaction, пълен диск (simulated),
  backpressure поведение при прелял pool, multi-shard commit под товар.
- `gaps.md` — честен списък на ограниченията (моделът на rocksbaga).

## Решения спрямо v1 плана
- BoilaQL отпада → PostgreSQL подмножество + PG wire (по-голям ecosystem).
- RESP2 фасадата отпада от v1 — wire protocol-ът поема ecosystem ролята;
  ако redis-cli съвместимост потрябва, тя е малък kvbaga слой и може да
  се добави без архивитектурна промяна.
- `doc/` модалът се разтваря в релационното ядро като JSONB колони.
- Един writer thread → N write lanes (архитектурният таван на v1).

## После (v2+, извън плана)
- raftbaga репликация; serializable; `NUMERIC`; window функции; COPY;
  SCRAM auth; compaction filter за MVCC GC вместо sweeper; cross-database
  заявки/FDW връзки между базите.

## Рискове
- **`go/chan` само `i64`** → комуникация с shard нишките през канали +
  cell2 пакети (доказан модел: rocksbaga MT `lsm_mt_*`, queuebaga).
- **Struct по стойност** → всеки shard се притежава от една нишка;
  worker-ите никога не мутират storage директно.
- **MVCC GC** — без compaction filter в rocksbaga v1 версията чисти със
  sweeper; риск от write amplification под тежки update товари →
  измерва се на P11, не се крие.
- **SQL подмножество** — изкушението "още една SQL функция" е scope creep;
  всяко разширение минава през PLAN + gaps.md, не през кода.
- **Обхват** — P0–P6 са задължителното ядро (storage→SQL→multi-DB→wire);
  P7–P10 са независими модали и могат да се пренареждат/режат без да
  чупят ядрото.
