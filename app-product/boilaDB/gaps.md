# boilaDB — gaps (честен списък)

Попълва се с всяка фаза (моделът на rocksbaga/docs/gaps.md). Буквите:
V = value/codec/vector, K = key/scan, S = storage, M = metrics/monitoring,
H = HTTP/API, Q = SQL (от P1), C = cache/planner, A = агрегати,
T = транзакции, W = wire protocol, F = FTS.

## Открити при P11

- **P11-1 — няма 10k concurrent client ladder.** Wire/HTTP са sync
  (W1/H2); harness-ът мери sequential ops/s. True pool = rebuild на
  BoilaServer ownership + shard-owner threads.
- **P11-2 — budget е max_scan/max_rows, не wall deadline.** Без
  concurrent workers deadline няма кой да enforce-не mid-query.
- **P11-3 — DROP TABLE не чисти secondary/fts/hnsw/graph CF ключове**
  (само data + catalog name/schema). Orphan index entries harmless
  след drop на schema; full wipe — при нужда.
- **P11-4 — barabadb/SQLite сравнение не е в repo run.** Изисква
  външни бинарници; суров rocksbaga baseline остава P1 scorecard.

## Открити при P10

- **G1 — perf gate 1M edges BFS d=3 < 100 ms не е измерен.** Functional
  P10 е зелен; bulk seed bench чака (Q2 arena).
- **G2 — няма DML sync за graph adjacency.** INSERT/UPDATE/DELETE след
  `CREATE GRAPH` не обновяват out/in lists; нужен re-CREATE GRAPH или
  бъдещ sync (моделът на FTS/HNSW).
- **G3 — WITH RECURSIVE е фиксиран pattern**, не пълен SQL CTE (без
  множествени CTE, без произволни JOIN-и в recursive leg, без `.`
  qualifiers — lexer няма `.`).
- **G4 — mode (BFS/DFS/Dijkstra) по CTE име** (`*_dfs`, `*_dij`), не
  SQL hint синтаксис.

## Открити при P9

- **S5 — perf gate 1M точки / <50 ms не е измерен.** Functional P9 е
  зелен (boila_ts_test); bulk seed + range+bucket bench чака chunked
  insert (Q2).
- **S6 — няма background TTL sweeper нишка.** Expire е lazy (get/flush/
  compact в rocksbaga); `boila_ts_sweep` = flush. Background queuebaga
  worker — P11.
- **S7 — time_bucket само в GROUP BY.** `SELECT time_bucket(...)` като
  проекция без GROUP BY → 0A000 (не е парснат). Alias/ORDER BY по
  bucket — при нужда.
- **S8 — `ttl_sec` е разширение** извън чистия PG `ttl_days` (за тестове
  и фина зърненост); документирано.

## Открити при P8

- **V2 — perf gate 100k×128d още не е измерен.** Functional P8 е зелен
  (boila_vec_test); 100k seed + recall@10 bench чака chunked seed заради
  arena OOM (Q2), моделът на FTS 20k.
- **V3 — metadata pre-filter през secondary index/ няма.** kNN е чист
  vector path; комбинация с `WHERE meta = …` идва при нужда.
- **V4 — няма RAM cache на горните HNSW нива.** Всеки search чете
  neighbors от vec CF (point GET-и); cache е P11/оптимизация.
- **V5 — unindex не чисти reverse edges.** Orphan pk в чужд neighbor
  list се skip-ва при search (липсващ row/vec). Пълен graph GC — при
  нужда.
- **V6 — kNN WHERE не се комбинира с AND** (като F6 за @@). LIMIT = k;
  default k=10 ако липсва LIMIT.
- **V7 — fixed-point ×1e6** вместо IEEE payload (V1 bit-cast липсва).
  Достатъчно за ranking; не е bit-identical с pgvector float4.

## Открити при P7

- **F1 — tokenizer-ът няма Unicode case folding** (UTF-8 encoding-ът
  е стандартът — PLAN/ARCHITECTURE; тук липсва само fold). Само ASCII
  A-Z → a-z; кирилицата се съпоставя exact (нужен езиков builtin или
  hand-rolled таблица). Други client encodings не се предлагат.
- **F3 — BM25 idf е линейна апроксимация** (без ln), целочислена ×1000.
  Реденето е коректно относително, но не е точно PG ts_rank.
- **F4 — posting списъците са един запис на term (read-modify-write при
  индексация).** Алтернативата (append-only + merge) идва при нужда от
  по-бързо писане; point GET query-тата са целта (K2).
- **F5 — фразови заявки няма** (to_tsquery е само AND/OR от думи).
- **F6 — @@ не се комбинира с други WHERE условия** (fts клонът връща
  редовете директно; комбинацията идва с P5 planner-а при нужда).

## Открити при P6

- **W1 — (FIXED go_bg + multi-DB + per-shard + shared pc).** HTTP/PG:
  `go_bg` per-conn; live conn; `boila_open_mt` hop-less shards. Data SQL
  under shard locks; **per-db plan cache** with `boila_pc_*_mu` (dmu only
  around get/put). Schema DDL serial per-db. Residual: SELECT vs DROP
  TABLE race if mid-exec (no schema epoch); JOIN plans uncached (C1).
- **W2 — extended protocol-ът ре-parse-ва на всеки Execute.** $1..$n се
  заместват текстово в Bind/Execute и заявката минава през пълния
  pipeline (plan cache-ът хваща само повторения на идентичен текст).
  Кеширан AST по stmt име идва при нужда.
- **W3 — RowDescription е винаги OID 25 (text).** pgbaga JSON
  детекцията по OID не разпознава jsonb колони; стойностите са коректни
  като текст.
- **W4 — DROP TABLE не е поддръжан** (0A000). Smoke тестовете ползват
  fresh root; идва с P7+ или при нужда.
- **W5 — Describe връща NoData** (няма statement metadata cache).
- **W6 — auth е cleartext token (BOILA_TOKEN) или trust.** SCRAM не се
  предлага от сървъра (pgbaga-клиентът го поддържа, но сървърът не го
  иска); TLS няма (SSLRequest → 'N').

## Открити при P5

- **C1 — plan cache-ът покрива само едно-таблични SELECT-и.** JOIN
  заявките плащат lex+parse+catalog на всяко изпълнение (кеширането на
  мулти-таблични планове изисква и двете таблики в кеша — идва при
  нужда).
- **C2 — planner-ът е rule-based, без статистики/cost model.** Ред:
  pk point > index eq > seq scan; JOIN: index nested loop при индекс по
  вътрешната колона, иначе nested loop. Catalog статистики (histograms)
  и cost-based избор остават за P11 или при реална нужда.
- **C3 — query budget (deadline + max scanned keys) не е в P5.** При
  sync сървъра без конкурентност няма кой да го наложи смислено; идва с
  wire/pool фазата (P6) и се тества в P11.
- **A1 — avg е целочислено деление** (sum/cnt в i64). f64 резултат чака
  f64 поддръжка в codec-а (V1).
- **A2 — hash join няма.** P5 има nested loop + index nested loop; при
  големите joins ще трябва hash join (изисква Map<bytes, Vec<row>> —
  възможно, но недоказано в baga за големи обеми).
- **A3 — JOIN само един на заявка**, WHERE в JOIN-ите филтрира само
  лявата таблица; `ON` поддържа само `=`.

## Открити при P4

- **T2 — MVCC-ът е едно-сесийен buffered модел.** Изолацията идва от
  buffer-а (storage непроменен до COMMIT), не от multi-version ключове —
  тях и commit sequencer-ът за конкурентни сесии идват в P6 заедно с
  wire/pool concurrency-то. Честна ревизия на PLAN текста (вж. PLAN P4).
- **T5 — DDL извън транзакционния буфер.** `CREATE TABLE` пише директно
  (не се rollback-ва; в явна транзакция се отказва с 0A000).
  `CREATE INDEX` е изцяло буфериран (каталог + entries атомно).
- **T6 — txn buffer-ът е unbounded.** Голяма транзакция трупa RAM
  (arena-та не reclaims, Q2). Лимит на buffer записите идва с P6.

## Открити при P3

- **T1 — (FIXED при P4) няма statement atomicity/rollback.** Грешка
  midway в multi-row INSERT/UPDATE/DELETE персистираше редовете дотам.
  От P4 писанията минават през txn buffer-а и грешка → rollback на целия
  буфер (за имплицитните auto-commit txn-и и за явните).
- **K5 — row + index entries са отделни WAL записи.** Една заявка се
  fsync-ва като група (statement batch), но rocksbaga няма multi-record
  атомен WAL запис: torn pwrite в рамките на един statement би оставил
  частични записи при replay (тесен прозорец). Пълна оправия = WAL group
  record в rocksbaga (бъдеща промяна, вж. ARCHITECTURE §1 принцип 1).
- **K6 — NULL стойности не се индексират.** PG индексира NULL-и; тук
  съзнателно се пропускат (по-прост lookup; документирано).
- **K7 — (FIXED при P3 rev.2) boila_ix_list скенваше sys на всяко DML
  заявление.** Комбинацията с rocksbaga SCAN snapshot-а (материализира
  ВСИЧКИ ключове на клъстера при смяна на write epoch) правеше всяко
  заявление O(общия брой ключове) — измерено: OOM при ~1000 заявления
  върху 97k реда (~20 MB garbage/заявление). Поправено: индексните
  дефиниции живеят в schema row-а на таблицата (point GET, O(1));
  CREATE INDEX build е двупасов (събиране без писания → запис batch).
  Урок: в rocksbaga никога не се скенва по време на фаза, която пише.
- **Q2 — baga bump arena-та не reclaims; дългоживеещите тежки процеси
  OOM-ват.** Измерено при P3 bench: 1M INSERT-а (10×100k) OOM-ва на
  chunk 2 дори след K7 fix-а; 100k (10×10k) минава чисто и е измереният
  durability еталон. За сървъра това налага предвиденото в ARCHITECTURE
  §6: per-request arena (`arena_new/arena_reset`) още с P6 wire фазата —
  иначе дългият процес ще повтори OOM-а.

## Открити при P2

- **S4 — mkdir е extern FFI към libc** (езикът няма builtin mkdir/
  readdir/rmdir). boilaDB го ползва за сървърния root; алтернатива без
  FFI би била flat layout само в съществуваща директория. DROP DATABASE
  чисти файлове по известни шаблони, но празни директории не се трият
  (няма rmdir).
- **H3 — HTTP е без сесия:** `USE` през HTTP връща ok, но не променя
  нищо за следващи заявки — базата се избира с `?db=` на всяка заявка.
  Истинска сесия идва с PG wire-а (P6).

## Открити при P1

- **Q1 — SQL слоят е 5.1× по-бавен от суров GET за point заявки.**
  Измерено (bench/boila/results/point-read-2026-08-08.md): 6102 ns/op
  срещу 1193 ns/op (511%). Разбивка: lex+parse+catalog GET ≈ 4.9 µs
  върху 1.2 µs storage. Планът на ARCHITECTURE §4 вече предвижда
  plan cache + PK fast path; гейтът ≤ 1.25× от PLAN.md се премества
  като задължаващ при P5 (plan cache) / P6 (prepared statements).
  Числото от P1 е baseline-ът, срещу който се сравнява всяко подобрение.
- **K3 — range scan-ът е scan-всичко-и-филтрирай.** rocksbaga SCAN е
  hash-подреден (не сортиран), затова `WHERE pk >= a AND pk <= b` в P1
  обхожда цялата таблица. Резултатите са коректни, но не са подредени и
  няма early-stop. Истински сортиран range scan идва с P5 planner-а.
- **H2 — (FIXED) HTTP go_bg + per-shard hop-less + multi-DB + live conn.**
  `BOILA_MAX_CONN` → 503/53300. mode=`mt-shard`.

## Открити при P0

- **V1 — f64 няма sort-order кодировка.** Езикът още няма f64→i64
  bit-cast builtin (само `f64_to_str`). Без него IEEE битовата
  трансформация за byte-подредба е невъзможна. До появата на builtin:
  f64 колони само payload (без `ORDER BY`/range по f64). Таг 7 е
  резервиран в `core/value.baga`.
- **K1 — MVCC версионен суфикс липсва (до P4).** Ключът е
  `[cf][table][pk]`; P4 добавя суфикса тук (дизайнът е в codec.baga
  коментара). Операциите дотгава са точка/prefix — еднозначни са.
- **K2 — prefix scan е през glob MATCH.** `boila_scan` ползва
  `SCAN MATCH prefix*` на rocksbaga; pk, съдържащ `*`/`?`/`[`, може да
  даде фалшиви съвпадения. P5 planner-ът минава на истински prefix scan.
- **S1 — shard-маршрутизация по hash на целия ключ** (djb2-подобен на
  rocksbaga), не само по pk. pk е доминиращо-вариращата част, така че
  разпределението е ефективно по pk — но не е гарантирано перфектно
  равномерно при къси pk-та. Измерва се на P11.
- **S2 — MVCC GC (P4+) ще е background sweeper**, докато rocksbaga няма
  compaction filter. Риск от write amplification под тежки update товари
  — измерва се на P11, не се крие.
- **M1 — метриките са per-request рендер** (gauges от затворени
  стойности); броячи на заявки още няма — те идват с P3 write lanes,
  където има един собственик на Metrics struct-а (конкурентен met_inc
  от много conn нишки не е безопасен).
- **H1 — serve е thread-per-conn** (моделът на httpdbaga). Bounded pool
  + admission control идват с P6 (wire); P0 скелетът не е товарно
  втвърден — това е целта на P11.
