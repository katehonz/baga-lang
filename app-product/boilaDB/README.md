# boilaDB

Мултимодална база данни на **baga**: модулен монолит върху sharded
rocksbaga (LSM), **PostgreSQL-съвместим SQL синтаксис** (BoilaSQL, от P1),
високо натоварване като цел. Без Geo/GPS — изрично извън обхвата.

- **Архитектура:** [ARCHITECTURE.md](ARCHITECTURE.md)
- **План:** [PLAN.md](PLAN.md) (P0–P11, всяка фаза с benchmark гейт)
- **Ограничения:** [gaps.md](gaps.md)

## Статус

**P6 (PG wire protocol v3)** — `tools/serve_pg.baga` на порт 6575:
startup/auth (cleartext token или trust), simple query, extended
Parse/Bind/Describe/Execute/Sync с `$1..$n`, per-connection транзакции.
Проверено с pgbaga smoke (20/20) и **истински psql/libpq** (SELECT,
INSERT...RETURNING, агрегати, UTF-8). Sync loop (gaps W1) — pool-ът идва
в P11.
Предишно: **P5 (пълен SELECT + planner)**; **P4 (MVCC транзакции)**;
**P3 (DML + индекси)**; **P2 (много бази)**; **P1 (SQL read path)**;
**P0 (скелет)**.

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
