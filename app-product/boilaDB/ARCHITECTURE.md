# boilaDB — Архитектура

Мултимодална база данни на езика **baga**. Модулен монолит: един бинарник,
един процес, вътрешно разделен на модули с твърди граници и еднопосочни
зависимости. Цел: при средно ниво на натоварване (1–100 клиента, GB-данни,
един възел) да бъде по-бърза, по-издръжлива и по-коректна от barabadb и
класата ѝ.

## 1. Дизайн принципи

1. **Един storage engine, много модали.** Всичко персистентно живее в
   rocksbaga (LSM). Вектори, графи, FTS индекси, документи, time-series —
   всички са key-space-ове върху column families. Нищо не е "in-memory със
   сериализация" — това е главната слабост на barabadb, която бием.
2. **Типизирани стойности, не string sentinels.** Коректна NULL семантика
   (three-valued logic), типизиран value codec още на storage границата.
   (barabadb: `NULL = NULL` връща true — системен проблем при тях.)
3. **Модулен монолит.** Един процес, един бинарник; модулите комуникират
   през вътрешни API-та, не през мрежата. Модулът може да се изтръгне в
   отделен пакет без пренаписване.
4. **baga-идиоматично.** Struct-ове по стойност (`db = lsm_put(db, k, v)?`),
   `bytes` навсякъде където е бинарно, ефекти (`!IO !Net !Time !Par`)
   изрични в сигнатурите, spec-блокове за инвариантите на индексите.
5. **Предвидима латентност пред пиков товар.** Backpressure, bounded
   опашки, budget-и на заявка — мишената е средно натоварване с плоска
   p99, не benchmark маркетинг.

## 2. Слоеве (еднопосочни зависимости, без нагорни import-и)

```
┌─────────────────────────────────────────────────────────┐
│ api/      HTTP (httpdbaga) · RESP2 (kvbaga кодек) · CLI │
├─────────────────────────────────────────────────────────┤
│ query/    BoilaQL: lexer → parser → planner → executor  │
├──────────┬──────────┬──────────┬──────────┬─────────────┤
│ doc/     │ vector/  │ graph/   │ fts/     │ ts/         │  ← модали
├──────────┴──────────┴──────────┴──────────┴─────────────┤
│ index/    общи индексни примитиви върху LSM key-space   │
├─────────────────────────────────────────────────────────┤
│ txn/      MVCC snapshot-и, транзакционен мениджър       │
├─────────────────────────────────────────────────────────┤
│ storage/  rocksbaga: LSM, WAL, column families, bloom   │
├─────────────────────────────────────────────────────────┤
│ core/     types · codec · config · errors · arena цикли │
└─────────────────────────────────────────────────────────┘
   cross-cutting: logbaga · metbaga · queuebaga · flagbaga
```

Зависимостите сочат само надолу. Модалите не се познават помежду си —
композицията (hybrid search, graph+vector заявки) става в `query/`.

## 3. Модули

### core/ — фундамент
- `types.baga` — `BoilaDB` root struct (държи `LsmDB`, config, registry на
  колекции), резултатни struct-ове по конвенцията на rocksbaga.
- `value.baga` — типизиран value model: `null | bool | i64 | f64 | bytes |
  str | vec | json`. LE-кодиран, binary-safe, сортиране съвместимо с
  byte-order на ключовете (за range scans). Тук живее 3VL логиката.
- `codec.baga` — key encoding: `<cf> | <колекция> | <вид запис> | <пк>`.
- `config.baga` — flagbaga + файл; limits (max connections, memtable MB,
  query budget ms).
- `errors.baga` — ефекти `!NotFound !Conflict !Corrupt` и др. по моделите
  на езика.

### storage/ — тънка фасада над rocksbaga
- Не се пише нов engine. `import "rocksbaga/engine.baga"` + `cf.baga`.
- Column families: `sys` (метаданни, схеми), `doc`, `vec`, `graph`, `fts`,
  `ts`, `txn` (write intents).
- `space.baga` — namespace-нати операции put/get/scan/del върху CF с
  codec-нати ключове; единственото място, което познава физическия layout.
- Backup/monitoring: директно `lsm_backup_*`, `lsm_dbsize`.

### txn/ — транзакции
- MVCC върху WAL + write intents в `txn` CF (референция: txnbaga).
- Snapshot isolation за четене; single-writer сериализация за писане през
  една вътрешна опашка (queuebaga) — опростява конкурентността при среден
  товар и дава детерминистичен ред на commit.
- Първа версия: read-committed + snapshot reads. 2PC/разпределени — извън
  обхвата (v2+).

### index/ — общи индексни примитиви
- Вторични индекси като LSM key-space: `idx|<колекция>|<поле>|<кодирана
  стойност>|<пк> → ""`. Range scan = префиксен scan (rocksbaga дава
  `lsm_scan`, bloom филтри и partial get безплатно).
- Maintenance: индексите се пишат в същата WAL-транзакция като документа —
  няма дрейф при crash (за разлика от in-memory индекси, които се
  преизграждат).

### Модали (всички дисково-персистентни — ключовото предимство)

- **doc/** — JSON документи (std/json). Колекции, схема optional,
  вторични индекси по полета, JSON path заявки. Това е ядрото.
- **vector/** — `VECTOR(n)` колони. HNSW индекс, чиито възли/ребра са
  записи в `vec` CF → не се държи в RAM, скалира с диска; горещ слой
  (top levels на HNSW) се кешира в паметта. Дистанции: cosine, L2, dot.
  Филтриране по metadata преди/след kNN.
- **graph/** — adjacency в LSM: `edge|<from>|<label>|<to>` и обратен
  индекс. BFS/DFS/Dijkstra като scan-ове. Без PageRank/Louvain в v1.
- **fts/** — inverted index в `fts` CF: `term|<token>|<docid> → positions`.
  BM25 scoring, tokenizer (EN/BG), фразови и boolean заявки. Индексът е
  персистентен — не се преизгражда при рестарт.
- **ts/** — time-series (модал, който barabadb няма). Ключ
  `ts|<metric>|<timestamp>|<id>` → range scans по време, retention чрез
  TTL на rocksbaga (`lsm_put_ex`), прости агрегации (sum/avg/min/max/count
  по прозорец).

### query/ — BoilaQL
- v1: малък декларативен език (не пълен SQL): `FIND колекция WHERE ...
  ORDER BY ... LIMIT n`, `NEAR vec_field <-> [..] K 10`,
  `MATCH text AGAINST "..."`, `TRAVERSE ...`, `TS metric FROM .. TO ..
  AGG sum 1m`.
- Pipeline: lexer → parser → AST → planner (избор на индекс) → executor,
  който вика модалите и merge-ва (Reciprocal Rank Fusion за hybrid).
- Query budget: max scan keys + deadline на заявка; превишение → грешка,
  не забиване. Това е механизмът за плоска p99.

### api/ — входни точки
- **HTTP/JSON** през httpdbaga (thread-per-conn, keepalive) — основният
  клиентски интерфейс, REST endpoints + `/query` за BoilaQL.
- **RESP2** през kvbaga кодека — KV-фасада върху doc модала
  (GET/SET/SCAN съвместими), за да се ползва с redis-cli инструменти.
- **CLI** — `boiladb serve|shell|backup|restore|sst-dump` (flagbaga,
  моделът на rocksbaga/tools).

### cross-cutting
- `logbaga` — structured JSON логове; `metbaga` — Prometheus метрики
  (ops/sec по модал, compaction, p99); `queuebaga` — background задачи
  (index builds, retention sweeps); `ctxbaga` — deadlines до query budget.

## 4. Конкурентност (при средно натоварване)

baga дава `go/chan` само за `i64` — затова:
- HTTP слой: thread-per-conn (`go_bg`, моделът на httpdbaga) — достатъчно
  за стотици клиенти.
- **Четене**: директно от conn thread (LSM четенията са безопасни,
  rocksbaga workers моделът го доказва).
- **Писане**: един writer thread + канал → сериализиран WAL ред, batch
  group commit (няколко заявки = един fsync). При средно натоварване
  group commit-ът е по-важен от паралелни писания.
- Background: compaction е вътрешен в rocksbaga; index build/retention —
  queuebaga workers.
- Per-request arena (`arena_new/arena_reset`) — без leak-ове при дългоживеещ
  сървър.

## 5. Как бием barabadb при средно натоварване

| Тяхна слабост | Нашият отговор |
|---|---|
| vector/graph/FTS индекси само в RAM | всички индекси са LSM key-space-ове — скалират с диск, оцеляват при рестарт без rebuild |
| String value model, счупена NULL семантика | типизиран value codec + 3VL от ден първи |
| MemTable hash + линеен scan; SSTable индекс изцяло в RAM | rocksbaga вече има сортирана memtable (Map<bytes,bytes>), bloom, sparse meta — доказано ~94% от RocksDB durable PUT |
| Няма time-series | `ts/` модал с retention и агрегации |
| WebSocket DoS, authz проблеми | няма WS в v1; HTTP с rate limit и auth token (std/crypto HMAC) |
| Маркетингови benchmarks | честни числа: `bench/` harness срещу barabadb и SQLite при еднакъв client-server режим |
| ORC/ARC memory компромиси (Nim) | baga → C, без GC/RC цикли, arena per request |

## 6. Граници на v1 (честно)

- Един възел. Няма raft/replication/sharding (raftbaga е само фрагмент).
- BoilaQL не е SQL; няма JOIN-ове между колекции в v1.
- Graph: само обхождания + най-къс път, без аналитични алгоритми.
- Auth: един статичен API token; няма per-user ACL.
- Няма Postgres wire протокол.

## 7. Правила за размер на файловете (урокът от barabadb)

При barabadb се стигна до болезнено retroactive делене: `parser.nim` 1957
реда, `executor.nim` 1832, `raft.nim` 1250, `lsm.nim` 1097. При нас това се
предотвратява **от ден първи с твърди граници**, а не с "ще го разделим
после":

1. **Hard limit: 400 реда на файл.** При доближаване на ~350 се дели
   превантивно. Изключение няма — дори и "само още една функция".
2. **Един файл = една отговорност.** Файлът се казва като нещото, което
   прави (`tokenizer.baga`, не `utils.baga`). Ако името изисква "и" — това
   са два файла.
3. **Деленето е по ос, не по произволен размер:** типове/кодеци отделно от
   алгоритми, четене отделно от писане, всеки operator/statement на
   езика — свой файл.
4. **Тестовете следват файла:** `foo.baga` → `tests/foo_test.baga`. Тест
   файлът също ≤ 400 реда — при повече се дели по сценарий
   (`foo_crud_test.baga`, `foo_restart_test.baga`).
5. **Мониторинг:** `scripts/filesize.sh` (wc -l по всички .baga, fail > 400)
   върви с тестовете. Проверява се още на P0 — преди да има какво да се
   дели.
6. **Префикси остават модулни:** при делене символите не се преименуват —
   `vec_hnsw_insert` си остава такъв, независимо в кой файл от `vector/`
   живее. Деленето е безболезнено за import-ващите.

Ориентир за очакваната грануларност по горещите места (където barabadb
преля):

| Място | Деление от старт |
|---|---|
| query/parser | по statement: `parse_find.baga`, `parse_near.baga`, `parse_match.baga`, `parse_traverse.baga`, `parse_ts.baga` + `ast.baga` |
| query/executor | по модал: `exec_find.baga`, `exec_knn.baga`, `exec_fts.baga`, `exec_graph.baga`, `exec_ts.baga` |
| vector/hnsw | `hnsw_insert.baga`, `hnsw_search.baga`, `hnsw_store.baga` (LSM layout), `hnsw_cache.baga` |
| fts/ | `tokenizer.baga`, `stemmer.baga`, `inverted_write.baga`, `inverted_read.baga`, `bm25.baga` |
| api/http | по ресурс: `http_doc.baga`, `http_query.baga`, `http_admin.baga` + `http_router.baga` |

## 8. Файлова организация (конвенция на app-product)

```
app-product/boilaDB/
├── sandak.toml          # name = "boilaDB", entry = "tools/serve.baga"
├── README.md  PLAN.md  ARCHITECTURE.md  gaps.md
├── core/     types.baga value.baga value_cmp.baga codec.baga
│             config.baga errors.baga
├── storage/  space.baga space_scan.baga      # фасада над rocksbaga + cf
├── txn/      mvcc.baga writer.baga intents.baga
├── index/    secondary.baga index_codec.baga
├── doc/      doc.baga collection.baga doc_query.baga
├── vector/   hnsw_insert.baga hnsw_search.baga hnsw_store.baga
│             hnsw_cache.baga quant.baga distance.baga
├── graph/    adjacency.baga bfs.baga dfs.baga dijkstra.baga
├── fts/      tokenizer.baga stemmer.baga inverted_write.baga
│             inverted_read.baga bm25.baga
├── ts/       series.baga agg.baga retention.baga
├── query/    ast.baga lexer.baga parse_find.baga parse_near.baga
│             parse_match.baga parse_traverse.baga parse_ts.baga
│             planner.baga exec_find.baga exec_knn.baga exec_fts.baga
│             exec_graph.baga exec_ts.baga rrf.baga budget.baga
├── api/      http_router.baga http_doc.baga http_query.baga
│             http_admin.baga resp.baga auth.baga
├── tools/    serve.baga shell.baga backup.baga
├── tests/    <file>_test.baga за всеки сорс файл (≤ 400 реда)
├── scripts/  filesize.sh                   # hard limit проверка
└── bench/    vs_barabadb.md harness.baga
```

Root `.baga` shim-ове не са нужни — пакетът е продукт (bin), не библиотека.
Символен префикс: `boila_*` (подпрефикси `doc_*`, `vec_*`, `fts_*`...),
стабилен при вътрешно местене на функции между файлове.
