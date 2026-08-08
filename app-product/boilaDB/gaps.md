# boilaDB — gaps (честен списък)

Попълва се с всяка фаза (моделът на rocksbaga/docs/gaps.md). Буквите:
V = value/codec, K = key/scan, S = storage, M = metrics/monitoring,
H = HTTP/API, Q = SQL (от P1).

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
  store-а (каталог), struct-по-стойност изисква един собственик; P3
  въвежда shard-owner нишки + канали, P6 — bounded pool. /health и
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
