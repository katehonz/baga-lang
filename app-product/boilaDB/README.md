# boilaDB

Мултимодална база данни на **baga**: модулен монолит върху sharded
rocksbaga (LSM), **PostgreSQL-съвместим SQL синтаксис** (BoilaSQL, от P1),
високо натоварване като цел. Без Geo/GPS — изрично извън обхвата.

- **Архитектура:** [ARCHITECTURE.md](ARCHITECTURE.md)
- **План:** [PLAN.md](PLAN.md) (P0–P11, всяка фаза с benchmark гейт)
- **Ограничения:** [gaps.md](gaps.md)

## Статус

**P5 (пълен SELECT + planner)** — `DISTINCT`, `ORDER BY`/`LIMIT`/
`OFFSET`, агрегати (count/sum/avg/min/max), `GROUP BY` + `HAVING`,
`JOIN` (INNER/LEFT, NL + index nested loop), rule-based planner и plan
cache (cold 11.0 µs → warm 5.8 µs, 1.89×; bench/boila/results/
select-planner-2026-08-08.md). Hash join, query budget и WITH RECURSIVE
— P6/P10/P11 (gaps).
Предишно: **P4 (MVCC транзакции)**; **P3 (DML + индекси + group
commit)**; **P2 (много бази)**; **P1 (SQL read path)**; **P0 (скелет)**.

## Пускане

```bash
# сървър (env: BOILA_PATH, BOILA_SHARDS, BOILA_PORT, BOILA_FLUSH_AT, BOILA_COMPACT_AT)
./baga -I . -I app-product app-product/boilaDB/tools/serve.baga
curl localhost:6570/health
curl localhost:6570/metrics
```

## Тестове

```bash
./scripts/baga-test tests/boila_value_test.baga tests/boila_codec_test.baga \
  tests/boila_shards_test.baga
bash app-product/boilaDB/scripts/filesize.sh   # ≤ 400 реда на файл
```
