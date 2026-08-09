# boilaDB

Мултимодална база данни на **baga**: модулен монолит върху sharded
rocksbaga (LSM), **PostgreSQL-съвместим SQL синтаксис** (BoilaSQL, от P1),
високо натоварване като цел. **Encoding: UTF-8 only.** Без Geo/GPS —
изрично извън обхвата.

- **Архитектура:** [ARCHITECTURE.md](ARCHITECTURE.md)
- **План:** [PLAN.md](PLAN.md) (P0–P11, всяка фаза с benchmark гейт)
- **Ограничения:** [gaps.md](gaps.md)

## Статус

**P11 (втвърдяване)** — query budget, `DROP TABLE`, sequential harness
(point 156k ops/s, insert 524 ops/s). **P0–P10** ядро + модали готови.
Sync server: без 10k concurrent (gaps W1).

## Пускане

```bash
# HTTP go_bg + per-shard hop-less + multi-DB (BOILA_MAX_CONN default 64)
./baga -I . -I app-product app-product/boilaDB/tools/serve.baga
# PG wire (same): BOILA_PGPORT=6575
./baga -I . -I app-product app-product/boilaDB/tools/serve_pg.baga
curl localhost:6570/health   # mode=mt-shard, live_conn
```

## Тестове

```bash
./scripts/baga-test tests/boila_value_test.baga tests/boila_codec_test.baga \
  tests/boila_shards_test.baga
bash app-product/boilaDB/scripts/filesize.sh   # ≤ 400 реда на файл
```
