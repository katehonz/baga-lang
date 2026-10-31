# pgbench point SELECT — boilaDB срещу PostgreSQL (2026-10-31)

Същата машина: AMD Ryzen 5 3600 (6 cores / 12 threads), 5000 реда,
една таблица, simple query protocol, read-only point SELECT по PRIMARY KEY.
Не е пълният pgbench `-S` (той иска 100k реда и своите таблици); това е
контролиран apples-to-apples тест на wire + parse + plan + point GET.

## Setup

```sql
CREATE TABLE pgbench_accounts (aid BIGINT, abalance BIGINT, PRIMARY KEY (aid));
-- 5000 реда: aid=i, abalance=i*10
```

```text
\set aid random(1, 5000)
SELECT abalance FROM pgbench_accounts WHERE aid = :aid;
```

boilaDB: `serve_pg`, `BOILA_WORKERS=4`, 4 shards, root `/tmp/boila_bench`.
PostgreSQL 15.19: `baga_orm`, същата таблица/данни (име `bench_accounts`).
`pgbench -n -M simple -T 5 -f script.sql`.

## Резултати (tps, без initial connection)

| clients | boilaDB | PostgreSQL | разлика |
|--------:|--------:|-----------:|--------:|
| 1 | 15 618 | 15 058 | +4% |
| 4 | 53 419 | 45 602 | +17% |
| 8 | 97 638 | 70 743 | +38% |
| 16 | **151 503** | 91 704 | **+65%** |
| 32 | 148 768 | 91 398 | +63% |

Latency average: boilaDB 0.064/0.075/0.082/0.106/0.215 ms;
PostgreSQL 0.066/0.088/0.113/0.174/0.350 ms.

## Защо преди беше 5 tps

`serve_mux.boila_mux_wake` пишеше в `pipe2` с `tcp_write`, който ползва
`sendto` → `ENOTSOCK`; `catch` го гълташе. Worker-ът паркираше fd-а, но
main poll-ът не получаваше wake и чакаше целия 200 ms timeout. След P47
(`fd_write` върху pipe) latency-ът падна от ~200 ms на ~0.06–0.2 ms.

## Честни граници

- Read-only point SELECT с 5k реда; не пише, не JOIN-ва, не сканира.
- Simple protocol; extended/prepared има различен профил.
- PostgreSQL е с default конфигурация на тази машина; не е tuning contest.
- Числата са от една сесия; при друга машина/load ще се разминат.
