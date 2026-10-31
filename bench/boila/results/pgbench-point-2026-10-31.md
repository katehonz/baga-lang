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

## Памет (P48/P49)

Същият 20 s прогон @ c=16, RSS на сървъра:

| версия | RSS след 20 s |
|---|---:|
| P47 (само wake fix) | 1.88 GB |
| + P48 (idle Vec + mem_rewind) | 420 MB |
| + P49 (epoch без cell2) | 102 MB |
| + P49 (channel cell2_free + raw wake) | **7.7 MB** |

Причината: `cell2`/`chan_recv2` връщат malloc двойки, които никой не
освобождаваше; `boila_mt_data_lock_st_epoch` правеше по една на заявка; а
wake-ът alokiraше `substr` извън turn rewind-а. Сега `cell2_free` е builtin
и горещите пътища го ползват.

## HTTP mux (P50)

`wrk -t4 -c16 -d10s http://127.0.0.1:6570/health`, `BOILA_WORKERS=4`:

| | резултат |
|---|---:|
| throughput | **163 967 req/s** |
| RSS преди | 2.3 MB |
| RSS след 10 s | 2.7 MB |

Преди P50 всяка HTTP заявка правеше нов GUC `Map` + `substr`-ове под
`mem_persist`; 10 s даваха **328 MB** RSS. Сега park копира `db`/`guc` само
при реална промяна.

## Кратки връзки (`pgbench -C`)

`pgbench -n -C -c1 -t100` (100 нови връзки): след P55 (ефективен
`str_repeat`) и P61 (scratch pool + persist map entries) ръстът при mux е
~3.05 MB за първите 100 и ~9 KB/връзка инкрементално след 500
(scratch pool/fd кешове се затоплят). Keep-alive пътят е стабилен.

## LLVM (experimental, P59)

`serve_pg` на LLVM (`lli`), същият pgbench point SELECT:

| режим | tps |
|---|---:|
| go_bg (`BOILA_WORKERS=0`), c=4 | **19 587** |
| mux, c=1 | 9 143 |
| mux, c=4 | 14 760 |
| mux, c=8 | 15 572 |
| mux, c=16 | **18 376** |

RSS стабилен ~221 MB. C бекендът е ~151k tps @ c=16 — LLVM е
функционален, но ~8× по-бавен; serving на LLVM остава experimental.

## Честни граници

- Read-only point SELECT с 5k реда; не пише, не JOIN-ва, не сканира.
- Simple protocol; extended/prepared има различен профил.
- PostgreSQL е с default конфигурация на тази машина; не е tuning contest.
- Числата са от една сесия; при друга машина/load ще се разминат.
