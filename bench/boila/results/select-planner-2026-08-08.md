# select-planner — P5 гейтове (2026-08-08)

`bench/boila/select_planner.baga`, 2 shard-а, таблица `pt` 5000 реда
(id BIGINT PK, city TEXT, v TEXT), gcc -O2.

## Гейт (а): planner-ът избира индекс пред scan — МИНАТ

| Път | ns/op | бележка |
|---|---|---|
| pk point (`WHERE id = 2500`) | 5 985 | референция, cache hit |
| **index** (`WHERE city = 'c7'`, 100 съвпадения) | **8 692 170** | връща всички 100 |
| seq scan (`LIMIT 100`, без WHERE) | 17 215 145 | пълен snapshot на ключовете |

Планиращият слой (rule-based, P5) избира индекса: index пътят е ~2×
по-бърз от seq scan дори при LIMIT 100 от seq-а (index-ът връща всичките
100 съвпадения, seq спира на 100). 200 повторения на път.

## Гейт (в): plan cache — МИНАТ (1.89×)

| Път | ns |
|---|---|
| cold (cache miss: lex+parse+catalog+exec) | 11 000 |
| **warm** (cache hit: само exec) | **5 818 /op** |
| cold/warm | **189%** |

1000 повторения на `SELECT v FROM pt WHERE id = 3125`. Hit-ът пропуска
изцяло lex+parse+catalog (дял ~0% при повтарящи се заявки — гейтът в
PLAN.md е „под 5%"); остава само storage exec. Speedup 1.89×.

## Бележки

- Кешът покрива само едно-таблични SELECT-и (резолвнат план по суров SQL
  текст); JOIN заявките плащат parse+catalog (gaps P4).
- Инвалидация: нова карта при DDL (bump на gen).
- Seq scan-ът материализира snapshot на всички ключове (rocksbaga SCAN) —
  затова е бавен дори с LIMIT; това е цената на hash-подредения SCAN (K2/K3).
