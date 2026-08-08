# boilaDB

Мултимодална база данни на **baga**: модулен монолит върху sharded
rocksbaga (LSM), **PostgreSQL-съвместим SQL синтаксис** (BoilaSQL, от P1),
високо натоварване като цел. Без Geo/GPS — изрично извън обхвата.

- **Архитектура:** [ARCHITECTURE.md](ARCHITECTURE.md)
- **План:** [PLAN.md](PLAN.md) (P0–P11, всяка фаза с benchmark гейт)
- **Ограничения:** [gaps.md](gaps.md)

## Статус

**P7 (fts модал)** — `CREATE FTS INDEX`, `WHERE col @@ to_tsquery('…')`
(to_tsquery/plainto_tsquery, '&' AND / '|' OR), BM25 класиране, DML
синхронизация, EN/BG. 20k документа: AND 73 µs / OR 579 µs / single
264 µs при гейт < 10 ms (bench/boila/results/fts-2026-08-08.md);
индексът персистентен без rebuild.
Предишно: **P6 (PG wire protocol v3)** — psql/libpq; **P5 (пълен
SELECT + planner)**; **P4 (MVCC транзакции)**; **P3 (DML + индекси)**;
**P2 (много бази)**; **P1 (SQL read path)**; **P0 (скелет)**.

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
