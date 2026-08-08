# boilaDB — gaps (честен списък)

Попълва се с всяка фаза (моделът на rocksbaga/docs/gaps.md). Буквите:
V = value/codec, K = key/scan, S = storage, M = metrics/monitoring,
H = HTTP/API, Q = SQL (от P1).

## Открити при P0

- **V1 — f64 няма sort-order кодировка.** Езикът още няма f64→i64
  bit-cast builtin (само `f64_to_str`). Без него IEEE битовата
  трансформация за byte-подредба е невъзможна. До появата на builtin:
  f64 колони само payload (без `ORDER BY`/range по f64). Таг 7 е
  резервиран в `core/value.baga`.
- **K1 — MVCC версионен суфикс липсва (до P3).** Ключът е
  `[cf][table][pk]`; P3 добавя суфикса тук (дизайнът е в codec.baga
  коментара). Операциите дотгава са точка/prefix — еднозначни са.
- **K2 — prefix scan е през glob MATCH.** `boila_scan` ползва
  `SCAN MATCH prefix*` на rocksbaga; pk, съдържащ `*`/`?`/`[`, може да
  даде фалшиви съвпадения. P1 planner-ът минава на истински prefix scan.
- **S1 — shard-маршрутизация по hash на целия ключ** (djb2-подобен на
  rocksbaga), не само по pk. pk е доминиращо-вариращата част, така че
  разпределението е ефективно по pk — но не е гарантирано перфектно
  равномерно при къси pk-та. Измерва се на P10.
- **S2 — MVCC GC (P3+) ще е background sweeper**, докато rocksbaga няма
  compaction filter. Риск от write amplification под тежки update товари
  — измерва се на P10, не се крие.
- **M1 — метриките са per-request рендер** (gauges от затворени
  стойности); броячи на заявки още няма — те идват с P2 write lanes,
  където има един собственик на Metrics struct-а (конкурентен met_inc
  от много conn нишки не е безопасен).
- **H1 — serve е thread-per-conn** (моделът на httpdbaga). Bounded pool
  + admission control идват с P5 (wire); P0 скелетът не е товарно
  втвърден — това е целта на P10.
