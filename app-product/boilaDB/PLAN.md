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
- **UTF-8 е единственият текстов encoding** (стандарт, всички фази).
  TEXT/идентификатори/SQL литерали/FTS terms/PG wire ParameterStatus
  (`server_encoding`/`client_encoding` = `UTF8`) — byte-точни UTF-8,
  без re-encode през `chr()` за ≥0x80. Други encodings (LATIN1, WIN1251,
  …) → `0A000`. Без Unicode case fold / ICU collation в v1 (gaps F1);
  exact-match за не-ASCII. Всяка нова пътека, която пипа текст, трябва
  да е UTF-8-safe (тест с кирилица/многобайтов символ).

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
  Plan cache по суров SQL текст (едно-таблични + JOIN — C1); инвалидация
  при DDL. WHERE е само по pk/индексирана колона (от P1/P3).
- Ревизии (честно): hash join и query budget (deadline + max keys)
  остават за P6/P11 — при sync сървъра без конкурентност budget-ът няма
  потребител; `WITH RECURSIVE` идва с graph фазата (P10); avg е
  целочислено деление (gaps A1).
- **Гейтове (измерени, bench/boila/results/select-planner-2026-08-08.md):**
  (а) planner-ът избира индекса: index път 8.7 ms срещу seq scan 17.2 ms
  (5000 реда); (в) plan cache: cold 11.0 µs → warm 5.8 µs (1.89×),
  parse+plan дял ~0% при повтарящи се заявки (гейт < 5%).

## P6 — PG wire protocol v3
- Реализирано: `api/pgwire_msg.baga` (framing), `pgwire_enc.baga`
  ($1..$n substitution quote-aware, text кодиране на стойности,
  CommandComplete тагове), `pgwire.baga` (startup/SSL 'N'/cancel, auth
  cleartext token или trust, ParameterStatus/BackendKeyData/RFQ, simple
  query, extended Parse/Bind/Describe/Execute/Sync с in_err-skip-to-Sync
  семантика, Close, Terminate; txn е per-connection — rollback при
  затваряне); `tools/serve_pg.baga` (BOILA_PGPORT, default 6575).
- **Гейтове (минати):** e2e smoke през pgbaga (20 проверки: simple +
  extended, int/str/NULL параметри, SQLSTATE, extended-грешка → жива
  връзка, BEGIN/ROLLBACK); **истински `psql`/libpq** изпълнява P1–P5
  синтаксис (SELECT/INSERT/RETURNING/агрегати/UTF-8) —
  bench/boila/pgwire_smoke.baga.
- **W2 (post-P11):** prepared SELECT AST по stmt име — Parse кешира sel
  с `$N` slots; Bind попълва; Execute без re-parse (DML params = text).
- Ревизии (честно): concurrency residual (W1/P11); Describe → NoData
  (W5), OID-ите са 25 (W3), без SCRAM/TLS (W6).

## P7 — fts/ модал
- Реализирано: `fts/tokenizer.baga` (ASCII case folding, UTF-8 едно към
  едно, gaps F1); `fts/fts_doc.baga` (posting/doc кодировки, index/unindex,
  четения през buffer+storage с MVCC envelope unwrap); `fts/fts.baga`
  (каталог в sys CF, CREATE FTS INDEX с build, BM25 AND/OR заявка с
  точкови GET-и вместо scan — единственият път към <10 ms при
  hash-подредения SCAN, K2); `sql/tsquery.baga` (to_tsquery/plainto_tsquery,
  '&' AND / '|' OR, смесване → 0A000); `WHERE col @@ to_tsquery('…')`;
  DML синхронизация (INSERT/UPDATE/DELETE поддържат индекса).
- **Гейтове (минати):** 17 проверки в boila_fts_test (AND/OR/plain, EN/BG,
  DML sync, restart); **20k документа — AND 73 µs / OR 579 µs / single
  264 µs при гейт < 10 ms** (bench/boila/results/fts-2026-08-08.md);
  индексът персистентен след рестарт без rebuild. Първоначалната цел
  100k е ограничена от baga arena-та (gaps Q2) — seed-ът е chunked;
  query-гейтът минава с огромен запас и при 20k.
- Ключова оптимизация: fts-каталогът е ЕДИН ключ на таблица (point GET),
  не scan — `lsm_cluster_scan_kb` rebuild-ва snapshot на всички ключове
  и даваше 74 ms/query; след fix-а е 73–579 µs.
- **F5/F6 (post-P11):** phrase (`<->` / phraseto); `@@ AND eq/range`.
- Ревизии (честно): ts_rank е отделна функция в плана, тук реденето е по
  BM25 score в самата заявка (gaps F3); UTF-8 без case folding (F1).

## P8 — vector/ модал
- Реализирано: `VECTOR(n)` (typ=1000+n, payload fixed-point ×1e6 i32 —
  няма f64 bit-cast, V1); литерал `'[1.0, 2, -0.5]'`; `CREATE INDEX …
  USING hnsw (col)`; оператори `<->` (L2sq) / `<=>` (cosine) / `<#>`
  (neg-IP) в `WHERE col op '[…]' LIMIT k`; HNSW граф в `vec` CF
  (neighbors per level), векторите в data row (без дублиране); DML sync;
  brute-force exact при ≤512 реда, HNSW над това.
  Файлове: `vector/distance.baga`, `hnsw_store.baga`, `hnsw.baga`,
  `hnsw_search.baga`.
- **Гейтове (минати, functional):** 12 проверки в boila_vec_test (DDL,
  dim check, L2/cos/IP kNN, DML sync, restart без rebuild, non-vector
  отказ). **Perf gate 100k×128d recall@10 ≥ 0.95 / <20 ms** — остава
  bench/ (Q2 arena; seed chunked като FTS 20k) — вж. gaps V2.
- **V6 (post-P11):** kNN `AND eq/range` — overfetch + post-filter.
- Ревизии (честно): fixed-point ×1e6 вместо IEEE f64 bits (V1); unindex
  strips reverse edges (V5); metadata pre-filter през secondary index
  преди HNSW — не (V3); upper-level cache в RAM — не (V4).

## P9 — ts/ (time-series през SQL)
- Реализирано: `CREATE TABLE … WITH (ttl_days = N | ttl_sec = N)` —
  TTL в sys `td|<tid>`; data put на COMMIT през `lsm_put_ex_kb`
  (rocksbaga BAGATTL1 lazy expire); `GROUP BY time_bucket('1m', ts)`
  (s/m/h/d); `ts/time_bucket.baga`, `ts/retention.baga`; sweep = flush.
- **Гейтове (минати, functional):** boila_ts_test — bucket 3 групи +
  totals, bad unit, ttl_sec expire след 2s, ttl_days, restart.
  **Perf 1M точки < 50 ms** — bench остава (Q2 arena), gaps S5.
- Ревизии: `ttl_sec` за тестове/фина зърненост (в допълнение на
  `ttl_days`); time_bucket само в GROUP BY (не в SELECT list сам);
  sweeper = flush, не background worker (S6).

## P10 — graph/ през WITH RECURSIVE
- Реализирано: `CREATE GRAPH name ON edges (src, dst [, w])` — двупосочен
  adjacency в `graph` CF; `WITH RECURSIVE cte(node, depth) AS (
  SELECT start, 0 UNION ALL SELECT dst, … FROM edges, cte WHERE src = node
  AND depth < N) SELECT … FROM cte`; mode по CTE име: default BFS,
  `*_dfs` → DFS, `*_dij` → Dijkstra (weights). Файлове:
  `graph/{adjacency,bfs}.baga`, `sql/{parse_with,exec_with}.baga`.
- **Гейтове (минати, functional):** boila_graph_test 16/16 (create/dup,
  BFS depth-2, DFS, Dijkstra short path, no-graph, restart).
  **Perf 1M edges BFS d=3 < 100 ms** — bench остава (Q2), gaps G1.
- Ревизии: ограничен WITH RECURSIVE pattern (не пълен SQL CTE);
  без `.` qualifier в lexer (unqualified src/dst); DML sync на graph
  при INSERT след CREATE GRAPH — не (G2; rebuild via re-CREATE).

## P11 — Втвърдяване и честни benchmarks
- Реализирано: `core/budget.baga` (max_scan/max_rows + wall deadline
  `BOILA_BUDGET_MS` → 54000/57014); full `DROP TABLE` wipe; harness
  sequential ladder 100/1k/10k (point/insert/mix80-20);
  `tests/boila_p11_test` (drop, budget constants, restart durability).
- **Измерено (harness-2026-08-09.md):** point 10k → **156k ops/s**
  (6.4 µs); insert 10k → **524 ops/s**; mix 10k → **2.6k ops/s**.
- **Сървър harden:** HTTP + PG → `go_bg` + **per-shard hop-less** +
  multi-DB + **shared per-db plan cache** (`boila_pc_*_mu`) + live
  counter. Data SQL parallel; schema DDL serial per-db.
- **Честно residual:** per-shard lanes inside one DB; external DB compare;
  kill -9 chaos automation.
- Ревизии: ladder е single-thread SQL path, не client fan-out; budget
  е key-count cap, не wall-clock deadline (няма concurrent enforcer).

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
