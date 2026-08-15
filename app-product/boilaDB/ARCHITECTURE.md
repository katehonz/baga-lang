# boilaDB — Архитектура (v2, високо натоварване)

Мултимодална база данни на езика **baga**. Модулен монолит: един бинарник,
един процес, вътрешно разделен на модули с твърди граници и еднопосочни
зависимости. Синтаксис: **PostgreSQL-съвместимо SQL подмножество**
(BoilaSQL). Цел: **високо натоварване** — 1k–10k клиенти, 100 GB–1 TB
данни, един възел, всички ядра — с плоска p99 и честни benchmark-и.
Сървърът хоства **множество бази данни** (моделът на PostgreSQL/MySQL,
P2): registry + отделен sharded клъстер на база.

## Решения спрямо v1 плана

| v1 | v2 (този документ) | Защо |
|---|---|---|
| Собствен език BoilaQL (`FIND/NEAR/MATCH/…`) | BoilaSQL — PostgreSQL подмножество (`SELECT/INSERT/UPDATE/DELETE`, `$1`, prepared statements) | екосистемата говори SQL; драйвери и psql работят от ден първи на протокола |
| Средно натоварване (1–100 клиента, един writer) | Високо натоварване (1k–10k клиенти, N shards, write lanes, bounded pool) | сегашният таван е архитектурен, не хардуерен |
| Отделен `doc/` JSON модал | JSONB — тип колона в релационното ядро | един value model, по-малка повърхност; документите са редове с JSONB колони |
| RESP2 фасада за redis-cli | **Postgres wire protocol v3** (simple + extended query) | по-голям ecosystem лост; RESP остава извън v1 |
| Geo/GPS възможности | **Няма. Изрично извън обхвата и не влизат в roadmap-а.** | потребителско ограничение |
| Една база | **Много бази данни на сървър** (registry + per-database клъстери, P2) | PostgreSQL/MySQL модел — изискване |

## 1. Дизайн принципи

1. **Един storage engine, много модали.** Всичко персистентно живее в
   rocksbaga (LSM). Редове, вторични индекси, HNSW, FTS, графи — всичко
   са key-space-ове върху column families/shards. Нищо не е "in-memory
   със сериализация" — главната слабост на barabadb, която бием.
2. **Типизирани стойности, не string sentinels.** Коректна NULL семантика
   (three-valued logic), типизиран value codec още на storage границата.
   (`NULL = NULL` никога не е true.)
3. **SQL е подмножество, не имитация.** Реализираме честен, документиран
   диалект на PostgreSQL; всичко извън него връща ясна грешка
   (`0A000 feature_not_supported`), не тиха грешна семантика.
4. **Модулен монолит.** Един процес; модулите комуникират през вътрешни
   API-та, не през мрежата. Всеки модул може да се изтръгне в отделен
   пакет без пренаписване.
5. **baga-идиоматично.** Struct-ове по стойност, `bytes` за бинарно,
   ефекти (`!IO !Net !Time !Par`) изрично в сигнатурите, spec-блокове за
   индексните инварианти.
6. **Предвидима латентност пред пиков маркетинг.** Bounded опашки,
   admission control, budget на заявка — целта е плоска p99 при 10x
   повече клиенти, не рекорд в един режим.
7. **UTF-8 only.** Единственият текстов encoding на сървъра и по wire.
   TEXT/JSONB ключове/SQL идентификатори и литерали/FTS terms са
   byte-точни UTF-8 (без `chr()` re-encode на ≥0x80). PG wire:
   `server_encoding=UTF8`, `client_encoding=UTF8`. Друг encoding →
   `0A000`. Без ICU/Unicode case fold в v1 (exact match за не-ASCII;
   gaps F1).

## 2. Целево натоварване (измеримо, не маркетинг)

| Ос | Мишена v1 | Механизъм |
|---|---|---|
| Клиенти | 1k–10k едновременни TCP връзки | poll accept + bounded worker pool |
| Данни | 100 GB – 1 TB | N shards × rocksbaga LSM, page cache по shard |
| Точки-четения | overhead на SQL слоя ≤ 25% върху суров rocksbaga GET | point-lookup fast path + plan cache |
| Писания | ≥ 3× throughput спрямо sync-per-write | group commit по lanes (batch fsync) |
| Латентност | p99 при 10k клиенти ≤ 2× p99 при 1k | admission control + query budget |
| Възстановяване | WAL replay < 30 s при 1 GB WAL | shard-паралелен replay |

Числата са **гейтове на фаза** (вж. PLAN.md), а не обещания: всяка фаза
завършва с измерени числа в `bench/`, включително сравнение със суровия
rocksbaga при същия хардуер (моделът на scorecard-ите в `bench/rocks`).

## 3. Слоеве (еднопосочни зависимости, без нагорни import-и)

```
┌──────────────────────────────────────────────────────────────┐
│ api/   PG wire v3 (psql/libpq) · HTTP admin/SQL · CLI        │
├──────────────────────────────────────────────────────────────┤
│ server/ BoilaServer · multi-DB registry · boila_server_exec  │
├──────────────────────────────────────────────────────────────┤
│ sql/    BoilaSQL: lexer → parser → planner → executor        │
├──────────┬──────────┬──────────┬─────────────────────────────┤
│ vector/  │ fts/     │ ts/      │ graph/        ← модали      │
├──────────┴──────────┴──────────┴─────────────────────────────┤
│ catalog/ схеми·статистики   index/ вторични индекси          │
├──────────────────────────────────────────────────────────────┤
│ txn/    MVCC (snapshot LSN), commit sequencer, intents       │
├──────────────────────────────────────────────────────────────┤
│ storage/ N shard-а × rocksbaga (LSM, WAL, CF, bloom, cache)  │
├──────────────────────────────────────────────────────────────┤
│ core/   types · value codec · key codec · config · errors    │
└──────────────────────────────────────────────────────────────┘
   cross-cutting: logbaga · metbaga · queuebaga · ctxbaga · flagbaga
```

Модалите не се познават помежду си; композицията (hybrid vector⊕FTS,
рекурсивни обхождания) става в `sql/` executor-а.

## 4. Модули

### core/ — фундамент
- `value.baga` — типизиран value model: `null | bool | i64 | f64 | bytes |
  str | json | timestamptz | vector(n)`. Binary-safe, **sort-order
  кодиране** (sign-flip big-endian за числа) — byte-редът съвпада със
  семантичния, за range scans без decode на всяка стойност. Тук живее
  3VL логиката. (f64 sort-order чака bit-cast builtin — gaps.md V1.)
- `codec.baga` — key encoding: `<cf> | <table_id> | <pk_enc>`; shard-ът
  не е в ключа — маршрутизацията е hash на целия ключ през rocksbaga
  клъстера. MVCC версионен суфикс (`<ver_lsn_desc>`, низходящо →
  snapshot четене е първият запис с `lsn ≤ snapshot`) се добавя в P4.
- `config.baga` — flagbaga + файл: `shards`, `max_connections`,
  `worker_pool`, `commit_window_ms`, `cache_pages`, `query_budget_ms`.
- `errors.baga` — SQLSTATE кодове (`23505 unique_violation`,
  `57P03 cannot_connect_now`, `53300 too_many_connections`, `0A000`…).

### storage/ — N shard-а × rocksbaga
- `shards.baga` — N rocksbaga инстанции (put/get/scan/del, MT maps).
  Физически layout; **без** сесийно състояние.

### server/ — multi-DB registry + session (над sql/)
- **Много бази данни (P2):** `BOILA_PATH` е сървърният root — `.meta`
  registry (1 shard) и flat файлове `<db>.db` на база (shard пътища
  `<db>.db.s{i}`); root-ът се създава с `mkdir` през extern FFI (gaps
  S4). `server/databases.baga` държи `BoilaServer`: meta store +
  отворените бази като (име, store) двойки, lazy open, таван
  `BOILA_MAX_DB` с FIFO eviction, plus session_txn / plan cache / guc
  (сесийно състояние — не storage отговорност; kimi-deps D2). При init
  се създава базата по подразбиране `boila` (моделът на `postgres`).
  `server/exec_server.baga` — `boila_server_exec*` входна точка.
  Каталозите са per-database; cross-database достъп няма в v1.
  rocksbaga не се променя — per-dir клъстерите съществуват от R32.
- Sharding: hash на целия кодиран ключ (djb2-подобен, моделът на
  rocksbaga) по N shard-а; pk е доминиращо-вариращата част, така че
  разпределението е ефективно по pk. Всеки shard е собствена rocksbaga
  инстанция (свой WAL, memtable, compaction, page cache) — моделът,
  който rocksbaga доказа с `LSM_SHARDS`/parallel workers. N се фиксира
  при init в `sys` (по подразбиране = ядрата; максимум 64).
- `shards.baga` — отваряне/затваряне на shard множеството; **всеки shard
  се притежава от точно една нишка** (struct по стойност → собственост).
- `space.baga`/`space_scan.baga` — namespace-нати put/get/scan/del;
  единственото място, което познава физическия layout.
- `gc.baga` — MVCC version sweep (вж. §6); докато rocksbaga няма
  compaction filter — background worker, документирано в gaps.md.
- Backup/monitoring: `lsm_checkpoint`, `lsm_backup_*`, `lsm_dbsize`
  per shard.

### txn/ — транзакции
- **P4 (едно-сесийен модел, реализиран):** писанията се буферират в
  `BoilaTxn` и се commit-ват като един fsync batch с монотонен commit LSN
  (`m|next_lsn` в sys CF); данните в storage носят envelope `[lsn 8B BE]
  [row]` (data/index CF; sys каталогът е суров). Изолация: storage-ът е
  непроменен по време на txn → читателите виждат само committed +
  собствените buffer писания. Crash mid-txn → нищо durable → чисто
  rollback. Грешка в заявление → rollback на целия buffer.
- **P6 (конкурентен модел, целеви):** commit sequencer нишка с монотонни
  LSN-и; multi-version ключове `<pk>|<ver_lsn_desc>`; multi-shard през
  intents в `txn` key-space + commit ticket; write-write конфликт →
  `40001 serialization_failure`. В едно-сесийния режим P4–P5 това би било
  механизъм без потребител (gaps T2).
- **MVCC snapshot isolation**: reader взима `snapshot = текущия committed
  LSN`; вижда най-новата версия ≤ snapshot. Write-write конфликт →
  `40001 serialization_failure` на втория commit.
- Recovery: при replay недоcommit-нати intents се отхвърлят; commit
  ticket-ът е единственият източник на истина.
- Serializable, 2PC, разпределени транзакции — извън v1 (честно).

### catalog/ + index/
- `catalog/` — `sys` key-space: таблици, колони, типове, индекси,
  статистики (row count, min/max, coarse хистограми) — събират се от
  background sampler, не при всяко писане.
- `index/secondary.baga` — вторични индекси **локални за shard-а**
  (същият hash като таблицата → писането остава едно-shard, в същия
  WAL batch като реда — няма дрейф при crash). Глобален UNIQUE индекс —
  отделен sharded key-space с документирана цена (втори write hop).

### sql/ — BoilaSQL (PostgreSQL подмножество)
- Pipeline: lexer → parser (по statement — отделни файлове) → AST →
  planner (cost model върху catalog статистиките) → executor.
- **Plan cache**: нормализиран SQL текст → план; инвалидиране при DDL и
  при смяна на generation-а на статистиките. Trivial PK point-query има
  специален fast path без пълно планиране.
- Query budget: deadline (ctxbaga) + max scanned keys + max join build
  rows; превишение → чиста грешка, не забиване. Това е механизмът за
  плоска p99 при високо натоварване.

### Модали (всички дисково-персистентни)
- **vector/** — `VECTOR(n)` колони; HNSW с възли/ребра като записи в
  `vec` key-space (не в RAM); горните нива се кешират. pgvector-идиоматични
  оператори: `<->` (L2), `<=>` (cosine), `<#>` (negative inner product),
  `CREATE INDEX … USING hnsw`, metadata pre-filter през index/.
- **fts/** — inverted index в `fts` key-space, BM25, tokenizer EN/BG;
  синтаксис `WHERE body @@ to_tsquery('…')` / `plainto_tsquery`,
  `ts_rank`. Индексът е персистентен — без rebuild при рестарт.
- **ts/** — не е отделен език: таблици с `WITH (ttl_days = N | ttl_sec = N)`
  (boila extension for sub-day TTL), `time_bucket('1m', ts)` в GROUP BY,
  continuous `CREATE ROLLUP … USING time_bucket` pre-agg (K3j),
  retention през TTL (`lsm_put_ex`) + sweeper. Барбадб няма аналог.
- **graph/** — adjacency в `graph` key-space (двупосочен); обхожданията
  се изразяват като `WITH RECURSIVE` (PG-идиоматично), примитивите
  BFS/DFS/Dijkstra живеят тук. Без PageRank/Louvain в v1.

### api/ — входни точки
- **PG wire v3** — simple query + extended (Parse/Bind/Describe/Execute/
  Sync), ParameterStatus/RowDescription/DataRow/ReadyForQuery,
  cleartext/token auth. Форматът на съобщенията е познат от pgbaga
  (клиентската страна) — сървърният кодек се пише в `api/pgwire_*.baga`;
  startup съобщението носи името на базата (както при Postgres, P6).
  Това е основният интерфейс: psql, libpq, всички PG драйвери.
- **HTTP/JSON** (httpdbaga) — admin, `/sql?db=<име>` за инструменти
  (default = базата по подразбиране `boila`), `/metrics` (metbaga
  Prometheus формат), `/health`. До P3 (write lanes) HTTP loop-ът е sync
  със store-threading (gaps H2), после — thread-per-conn/bounded pool.
- **CLI** — `boiladb serve | shell | backup | restore | sst-dump`
  (flagbaga, моделът на rocksbaga/tools).
- **Няма WebSocket в v1** (DoS повърхността на barabadb).

## 5. BoilaSQL — обхват на подмножеството

**Типове:** `BOOL`, `BIGINT`, `FLOAT8`, `TEXT`, `BYTEA`, `JSONB`,
`TIMESTAMPTZ`, `VECTOR(n)`. `NULL` с 3VL. (Без `NUMERIC`, `SERIAL`,
масиви в v1 — документирано.)

**DDL:** `CREATE/DROP DATABASE` (сървърно ниво, P2), `CREATE TABLE …
(PRIMARY KEY задължителен — LSM-friendly)`,
`CREATE [UNIQUE] INDEX ON t (col)`, `CREATE INDEX … USING hnsw`,
`ALTER TABLE ADD COLUMN` (само nullable/add-only), `DROP TABLE/INDEX`,
`WITH (ttl_days = N | ttl_sec = N)`.

**DML:** `INSERT … VALUES (…$1…) … [ON CONFLICT DO NOTHING |
DO UPDATE SET …] [RETURNING …]`, `UPDATE … SET … WHERE … [RETURNING]`,
`DELETE FROM … WHERE … [RETURNING]`.

**SELECT:** проекции, `WHERE` (`= <> < <= > >= BETWEEN IN LIKE ILIKE
IS [NOT] NULL AND OR NOT`), `ORDER BY … ASC|DESC [NULLS FIRST|LAST]`,
`LIMIT/OFFSET`,
`GROUP BY` + `COUNT/SUM/AVG/MIN/MAX` + `HAVING`, `DISTINCT`,
`JOIN` (`INNER`/`LEFT` — index nested-loop или hash join по cost),
`WITH RECURSIVE`, `CASE`, аритметика. Dual (без FROM): литерали
(bool/NULL), builtins (`version`/`current_*`/`now`/`current_setting`/
`pg_*`/`COALESCE`/`NULLIF`), `+ - * /` (*/> +-), `||`, `CAST`/`::`.
  Без подзаявки/window в v1.

**Транзакции:** `BEGIN [READ ONLY] / COMMIT / ROLLBACK`, snapshot
isolation, `$1..$n` prepared statements през extended protocol-а.

**Сесия:** една сесия работи с точно една база (PostgreSQL модел);
`USE <db>` сменя текущата (MySQL удобство); PG wire-ът носи името на
базата в startup съобщението (P6). Cross-database заявки/транзакции —
извън v1 (`0A000`).

**Encoding:** **UTF-8 only** (стандарт от P0, не фазова опция). Wire
ParameterStatus `server_encoding`/`client_encoding` = `UTF8`; storage
payload-ите на TEXT/str са сурови UTF-8 байтове; lexer-ът акумулира
стринг литерали през `bytes` (не `chr()`). `SET`/`SHOW`/`RESET`/
`DISCARD ALL` — session GUC map (ST: `srv.guc`, PG: per-conn); defaults
for `server_version`/encodings/DateStyle/TimeZone. Без conversion tables
и без side-effects върху encoding. Unicode case folding / ICU — v2+
(gaps F1).

**Всичко извън списъка** → `0A000 feature_not_supported` с името на
конструкцията. Никога тиха разлика в семантиката спрямо PostgreSQL.

## 6. Конкурентност при високо натоварване

baga дава `go/chan` само за `i64` — цялата комуникация е през канали +
cell2 пакети (доказаният модел на rocksbaga MT и queuebaga).

- **Accept:** poll loop приема връзки; bounded **worker pool**
  (`BOILA_WORKERS`, default 4, cap 64; `0` = go_bg per-conn) изпълнява
  parse/plan/exec. При пълна опашка `chan_send` блокира accept
  (backpressure); над `BOILA_MAX_CONN` → `53300` и затваряне.
  Keep-alive държи worker-а. Data SQL е shared lock; schema DDL —
  exclusive (`api/serve_mt_lock.baga`).
- **Shard собственост:** hop-less per-shard mutex — worker-ът заключва
  само шарда на ключа и мутира `LsmDB` под ключалката. Топъл checkout
  не пише сървъра; data checkin е no-op. Няма hop през канал на всеки
  GET (Q1). SQL е multi-shard — няма една owner-нишка на заявление.
- **Group commit:** във всяка write lane заявките се събират в batch;
  един `fdatasync` на прозорец (`commit_window_ms` или размер на batch-а).
  При високо натоварване това е водещият лост — fsync-ът, не CPU-то.
- **Commit sequencer:** издава LSN-и за multi-shard commit-и; едно-shard
  commit-и (масовият случай) не го чакат.
- **MVCC четене:** worker-ите четат без ключалки върху snapshot LSN;
  GC worker-ът чисти версии под най-стария активен snapshot.
- **Background:** compaction е вътрешен за rocksbaga per shard; GC,
  retention sweep, stats sampler, checkpoint scheduler — queuebaga
  workers с нисък приоритет и собствен budget.
- **Памет:** per-request arena (`arena_new/arena_reset`); всички кешове
  (page cache, plan cache, HNSW горни нива) са bounded с clock/LRU
  eviction. Няма неограничени структури в дългоживеещия процес.

## 7. Как бием barabadb при високо натоварване

| Тяхна слабост | Нашият отговор |
|---|---|
| Индекси само в RAM, rebuild при рестарт | всички индекси са LSM key-space-ове — скалират с диск, оцеляват при рестарт |
| String value model, `NULL = NULL` е true | типизиран codec + 3VL от ден първи |
| Един процес, един writer | N shard-а × N write lanes + group commit |
| Няма лимити на заявки | query budget + admission control → плоска p99 |
| Няма time-series | `ts` през TTL + `time_bucket` |
| WS DoS, authz дупки | без WS в v1; token auth; bounded connections |
| Маркетингови benchmarks | гейтове на фаза + scorecard срещу суров rocksbaga, barabadb и SQLite при еднакъв режим |
| ORC/ARC memory компромиси (Nim) | baga → C, без GC, arena per request |

## 8. Граници на v1 (честно)

- Един възел. Няма raft/replication/sharding през мрежата (raftbaga е
  фрагмент). N shard-а са вътрешни, в един процес.
- Много бази данни на сървъра, но **без cross-database достъп**: една
  сесия = една база; cross-DB заявки/транзакции са v2+ (вж. PLAN.md).
- **Geo/GPS: няма.** Без geometry типове, без пространствени индекси, без
  GPS/trajectory ingestion — и не са планирани.
- SQL подмножество без `NUMERIC`, window функции, подзаявки, serializable.
- Multi-shard транзакции: snapshot isolation, не 2PC.
- Auth: cleartext + статичен token по wire; няма per-user ACL/SCRAM в v1.
- PG wire: само v3, без COPY, без logical replication, без LISTEN/NOTIFY.
- Encoding: **само UTF-8**; без `client_encoding` смяна, без ICU collation.

## 9. Правила за размер на файловете (урокът от barabadb)

При barabadb се стигна до болезнено retroactive делене (`parser.nim`
1957 реда, `executor.nim` 1832). Тук — твърди граници от ден първи:

1. **Hard limit: 400 реда на файл.** При ~350 се дели превантивно.
2. **Един файл = една отговорност.** Името се казва като нещото, което
   прави; ако изисква "и" — два файла.
3. **Делене по ос:** типове/кодеци отделно от алгоритми; четене отделно
   от писане; всеки statement/operator — свой файл.
4. **Тестовете следват файла:** `foo.baga` → `tests/boila_foo_test.baga`
   в коренната `tests/` (конвенцията на монорепото — `scripts/baga-test`
   открива там), също ≤ 400 реда (делене по сценарий).
5. **`scripts/filesize.sh`** (fail > 400) върви с тестовете от P0.
6. **Префиксите са стабилни:** `vec_hnsw_insert` не се преименува при
   местене между файлове.

## 10. Файлова организация (конвенция на app-product)

```
app-product/boilaDB/
├── sandak.toml  README.md  LICENSE
├── ARCHITECTURE.md  PLAN.md  gaps.md  kimi-deps.md
├── docs/      user guides (getting started, SQL, HTTP, PG wire, …)
├── core/      value.baga codec.baga row.baga budget.baga
├── storage/   shards.baga          # BoilaStore only (no session)
├── txn/       mvcc.baga            # buffer + LSN; write.ttl_sec from caller
├── catalog/   schema · ddl_types · ttl · drop · alter · truncate · acl
├── index/     secondary.baga  ix_drop.baga   # secondary only; same layer as catalog
├── fts/       tokenizer · fts_doc · fts · fts_wipe
├── vector/    distance · hnsw · hnsw_search · hnsw_store · hnsw_wipe
├── ts/        retention.baga  time_bucket.baga
├── graph/     adjacency · bfs · dml · g_drop
├── sql/       lexer/token/ast · parse_* · plan_cache · exec_* · sfn · tsquery
│              exec_agg_gexpr · exec_drop / exec_alter (kimi-deps D5)
├── server/    databases.baga  exec_server.baga   # BoilaServer + multi-DB
├── api/       pgwire* · pgwire_bind · http_* · serve_*
├── tools/     serve.baga  serve_pg.baga  shell.baga
├── scripts/   filesize.sh  deps.sh       # §9 + §3 gates
└── (bench/: repo root bench/boila/)
```
Layer ranks (scripts/deps.sh): core < storage < txn < catalog|index <
modals < sql < server < api < tools. Zero upward imports.

Тестовете са в коренната `tests/` като `boila_*_test.baga` (откриване
през `scripts/baga-test`), не в подпапка на пакета.

Символен префикс: `boila_*` (подпрефикси `sql_*`, `vec_*`, `fts_*`,
`txn_*`…), стабилен при вътрешно местене.
