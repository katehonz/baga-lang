# boilaDB

Мултимодална база данни на **baga**: модулен монолит върху sharded
rocksbaga (LSM), **PostgreSQL-съвместим SQL синтаксис** (BoilaSQL, от P1),
високо натоварване като цел. Без Geo/GPS — изрично извън обхвата.

- **Архитектура:** [ARCHITECTURE.md](ARCHITECTURE.md)
- **План:** [PLAN.md](PLAN.md) (P0–P10, всяка фаза с benchmark гейт)
- **Ограничения:** [gaps.md](gaps.md)

## Статус

**P1 (SQL read path)** — BoilaSQL lexer/parser за `SELECT`/`CREATE TABLE`,
каталог в sys key-space (персистентен), HTTP `POST /sql`, CLI shell,
SQLSTATE грешки. Perf baseline: SQL point SELECT 6102 ns/op срещу суров
GET 1193 ns/op (511%) — мишената ≤ 1.25× чака plan cache-а в P4 (gaps Q1).
Предишно: **P0 (скелет)** — value codec + 3VL, key layout, sharded storage,
`/health` + `/metrics`.

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
