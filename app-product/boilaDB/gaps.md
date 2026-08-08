# boilaDB — gaps (честен списък)

Попълва се с всяка фаза (моделът на rocksbaga/docs/gaps.md). Буквите:
V = value/codec, K = key/scan, S = storage, M = metrics/monitoring,
H = HTTP/API, Q = SQL (от P1), C = cache/planner, A = агрегати,
T = транзакции, W = wire protocol.

## Открити при P6

- **W1 — wire сървърът е sync: една връзка в даден момент.** Паралелни
  клиенти чакат на опашка. Истинският bounded pool + shard-owner нишки
  (PLAN P6 гейтът „10k concurrent") идва с concurrency фазата (P11) —
  изисква rebuild на собствеността върху BoilaServer.
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
- **H2 — serve е sync (една връзка в даден момент).** SQL пътят мутира
  store-а (каталог), struct-по-стойност изисква един собственик. P3
  добави statement-level group commit; конкурентността (shard-owner
  нишки + канали, bounded pool) идва с wire фазите (P6). /health и
  /metrics са леки и sync режимът не ги засяга практически.

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
