# boilaDB

Мултимодална база данни на **baga**: модулен монолит върху sharded
rocksbaga (LSM), **PostgreSQL-съвместим SQL синтаксис** (BoilaSQL, от P1),
високо натоварване като цел. **Encoding: UTF-8 only.** Без Geo/GPS —
изрично извън обхвата.

- **Архитектура:** [ARCHITECTURE.md](ARCHITECTURE.md)
- **План:** [PLAN.md](PLAN.md) (P0–P11, всяка фаза с benchmark гейт)
- **Ограничения:** [gaps.md](gaps.md)

## Статус

**P10 (graph)** — `CREATE GRAPH`, `WITH RECURSIVE` BFS/DFS/Dijkstra,
adjacency в `graph` CF, restart-персистентно.
Предишно: **P9 (ts)**; **P8 (vector)**; **P7–P0**.

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
