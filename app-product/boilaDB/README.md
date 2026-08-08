# boilaDB

Мултимодална база данни на **baga**: модулен монолит върху sharded
rocksbaga (LSM), **PostgreSQL-съвместим SQL синтаксис** (BoilaSQL, от P1),
високо натоварване като цел. Без Geo/GPS — изрично извън обхвата.

- **Архитектура:** [ARCHITECTURE.md](ARCHITECTURE.md)
- **План:** [PLAN.md](PLAN.md) (P0–P11, всяка фаза с benchmark гейт)
- **Ограничения:** [gaps.md](gaps.md)

## Статус

**P4 (MVCC транзакции, едно-сесийен buffered модел)** —
`BEGIN [READ ONLY]/COMMIT/ROLLBACK`, auto-commit на statement, изолация
(storage непроменен до COMMIT; собствени + committed писания видими),
грешка → rollback на буфера, монотонни commit LSN, транзакцията
фиксирана към базата си; envelope `[lsn][row]` в data/index CF.
Multi-version ключове + sequencer — в P6 с concurrency-то (gaps T2).
Предишно: **P3 (DML + индекси + group commit)**; **P2 (много бази)**;
**P1 (SQL read path)**; **P0 (скелет)**. Perf: SQL point SELECT 6102
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
