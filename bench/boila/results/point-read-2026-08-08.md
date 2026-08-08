# point-read — P1 гейт измерване (2026-08-08)

`bench/boila/point_read.baga`: 20 000 point lookup-а, 4 shard-а,
flush_at=10000 (memtable-резидентни данни), gcc -O2.

| Път | ns/op |
|---|---|
| суров rocksbaga GET (`lsm_cluster_get_kb`, същият ключов формат) | **1193** |
| SQL point SELECT (пълен път: lex → parse → catalog GET → exec → storage) | **6102** |
| **ratio** | **511%** |

found: raw=20000, sql=20000 (коректност — без загуби).

## Извод

Гейтът на P1 от PLAN.md (≤ 125%) **не е постигнат** при parse-от-нулата
за всяка заявка — очаквано: lex+parse+catalog-GET добавят ~4.9 µs върху
~1.2 µs storage четене. Мишената ≤ 1.25× остава задължаваща за **P5**
(plan cache) / **P6** (prepared statements през extended protocol-а) —
механизмът е в ARCHITECTURE.md §4 (sql/): „Trivial PK point-query има
специален fast path без пълно планиране".

Дотогава числата тук са baseline: всяко подобрение (plan cache,
statement cache, пропускане на catalog lookup при кеш) се сравнява
срещу 511%.

## Забележки

- Данните са memtable-резидентни (flush_at=10000, n=20k) — измерва
  CPU пътя, не disk I/O.
- `SELECT *` точка; range/прожекция не се бенчмаркват на P1.
- Модел на измерване: `monotonic_us` около целия цикъл, n делено.
