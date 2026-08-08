# boilaDB — План

Фази са последователни; всяка завършва със зелени тестове
(`./scripts/baga-test`) и записано състояние в `gaps.md`.
Критерий за "готово" на фаза: работи end-to-end през HTTP, персистентно е
(restart test) и има benchmark число.

**Правило за всички фази:** нито един файл не надминава 400 реда
(`scripts/filesize.sh` върви с тестовете) — деленето става превантивно,
не retroactive като при barabadb. Вж. ARCHITECTURE.md §7.

## P0 — Скелет (core + storage фасада)
- `sandak.toml` (path deps: `../../std`, `../rocksbaga`, `../kvbaga`,
  `../httpdbaga`, `../logbaga`, `../metbaga`, `../queuebaga`,
  `../flagbaga`, `../testbaga`).
- `core/value.baga` — типизиран value codec + 3VL compare; unit тестове
  за сортирана byte-order кодировка (вкл. отрицателни числа, null < всичко).
- `core/codec.baga` — key encoding `<cf>|<coll>|<kind>|<pk>`.
- `storage/space.baga` — `boila_open(dir)`, put/get/scan/del по CF върху
  rocksbaga; restart-персистентност тест.
- `tools/serve.baga` — празен HTTP сървър (httpdbaga) + `/health` + метрики.
- `scripts/filesize.sh` — wc -l по всички `.baga`, fail > 400 реда; закача
  се към тестовия runner още тук, преди да има какво да се дели.
- **Изход:** процесът стартира, пише и чете през CF-та, оцелява след kill -9
  (WAL replay).

## P1 — doc/ модал + KV врата
- `doc/`: колекции, put/get/delete документ (std/json), листване.
- Вторични индекси (`index/secondary.baga`) в същата WAL-транзакция.
- RESP2 фасада (kvbaga кодек): GET/SET/DEL/SCAN върху doc — съвместимост с
  redis-cli.
- HTTP: `PUT/GET/DELETE /coll/{name}/doc/{id}`, `POST /coll/{name}/query`
  (равенство + range по индексирани полета).
- Тестове: 10k документа, индексен range scan, restart → индексите валидни
  без rebuild (предимство #1 пред barabadb).

## P2 — txn/ + writer thread
- Един writer thread, опашка, group commit (batch fsync).
- Snapshot reads; `BEGIN/COMMIT/ROLLBACK` по HTTP сесия (write intents в
  `txn` CF).
- Тест: конкурентни писачи, коректен commit ред; crash mid-transaction →
  rollback при recovery.

## P3 — fts/ модал
- Tokenizer EN/BG, inverted index в `fts` CF, BM25.
- `POST /coll/{name}/fts` — boolean + фразови заявки.
- Тест: индексът е персистентен след рестарт; latency на заявка < 10 ms при
  100k документа (средно натоварване).

## P4 — vector/ модал
- `VECTOR(n)` колони в doc; HNSW с възли/ребра в `vec` CF + кеш на горните
  нива в RAM; cosine/L2/dot.
- Metadata pre-filter чрез index/.
- `POST /coll/{name}/knn` — k nearest + filter.
- Тест: 100k вектора × 128d, recall@10 ≥ 0.95 спрямо brute force;
  рестарт → без rebuild (предимство #2).

## P5 — ts/ модал
- `ts/` key layout, ingestion endpoint, range query + агрегации по прозорец,
  retention чрез `lsm_put_ex` TTL.
- Тест: 1M точки, range+agg < 50 ms; TTL реално чисти. (Модал, който
  barabadb няма.)

## P6 — graph/ модал
- Adjacency в `graph` CF (двупосочни индекси), BFS/DFS/Dijkstra.
- `POST /graph/traverse`, `/graph/shortest-path`.
- Тест: 1M ребра, BFS дълбочина 3 < 100 ms, персистентно след рестарт.

## P7 — query/ BoilaQL
- lexer → parser → planner (избор на индекс) → executor; query budget
  (max keys + deadline).
- `FIND/NEAR/MATCH/TRAVERSE/TS` + RRF за hybrid (vector ⊕ FTS).
- Тестове: planner избира индекс; budget прекъсва тежка заявка с грешка,
  p99 остава плоска под 50 конкурентни клиента.

## P8 — Втвърдяване и честни benchmarks
- `bench/harness.baga`: сценарии при средно натоварване (10/50/100 клиента),
  сравнение срещу barabadb (client-server режим при двамата) и SQLite.
- Метрики: throughput, p50/p99, memory, recovery time след kill -9.
- `gaps.md` — честен списък на ограниченията (като rocksbaga).
- Chaos тестове: kill -9 по време на compaction, пълен диск (simulated),
  backpressure поведение.

## После (v2, извън плана)
- raftbaga-базирана репликация, sharding; SQL слой; per-user auth;
  Postgres wire; UDF; колонен формат за аналитика.

## Рискове
- **`go/chan` само `i64`** → цялата комуникация с worker-ите е през канали +
  диск/cell2 контекст (доказан модел: queuebaga, rocksbaga workers).
- **Struct по стойност** → `BoilaDB` се подава `db = boila_*(db, ...)?`
  навсякъде; внимание при writer thread (собствеността е една — затова е
  един writer).
- **Обхват** → P0–P2 са задължителното ядро; P3–P6 са независими модали и
  могат да се пренаредят/режат без да чупят останалото.
