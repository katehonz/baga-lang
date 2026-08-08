# boilaDB

Мултимодална база данни на **baga**: модулен монолит върху sharded
rocksbaga (LSM), **PostgreSQL-съвместим SQL синтаксис** (BoilaSQL, от P1),
високо натоварване като цел. Без Geo/GPS — изрично извън обхвата.

- **Архитектура:** [ARCHITECTURE.md](ARCHITECTURE.md)
- **План:** [PLAN.md](PLAN.md) (P0–P11, всяка фаза с benchmark гейт)
- **Ограничения:** [gaps.md](gaps.md)

## Статус

**P3 (DML + индекси + group commit)** — `INSERT` (multi-row, `ON
CONFLICT DO NOTHING/UPDATE`, `RETURNING`), `UPDATE`, `DELETE`,
`CREATE INDEX` (build + синхронизация при DML, SELECT по индексирана
колона); statement-level group commit (един fsync на заявление — гейт
≥3×: измерено 17×); durability без close: 100k реда, 0 загубени,
индекс без rebuild (bench/boila/results/insert-write-2026-08-08.md).
SQLSTATE 23505/23502/42710/42804 и др.
Предишно: **P2 (много бази данни)** — `.meta` registry, `<db>.db`
flat клъстери, `CREATE/DROP DATABASE`, `USE`, `?db=`; **P1 (SQL read
path)**; **P0 (скелет)**. Perf baseline (P1): SQL point SELECT 6102
ns/op срещу суров GET 1193 ns/op — мишената ≤ 1.25× чака plan cache-а
в P5 (gaps Q1).

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
