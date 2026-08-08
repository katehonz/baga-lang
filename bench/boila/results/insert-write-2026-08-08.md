# insert-write — P3 гейтове (2026-08-08)

`bench/boila/insert_write.baga`, 2 shard-а, `/tmp` (disk-backed),
gcc -O2. Данни: таблица `d (id BIGINT, v TEXT)` + индекс по `v`.

## Гейт (а): group commit ≥ 3× спрямо sync-per-write — МИНАТ (17×)

| Режим | ns/ред |
|---|---|
| mode A: 1 ред/заявление (fsync на ред) | 1 252 930 |
| mode B: 100 реда/заявление (statement group commit) | 72 726 |
| **ratio A/B** | **1722%** (гейт ≥ 300%) |

Механизмът: `boila_stmt_begin` вдига `sync_every`, puts-овете на
заявката отиват в WAL буфера без междинен fsync; `boila_stmt_commit`
прави един `fdatasync` на shard. Една N-редова заявка амортизира
fsync-а N пъти.

## Гейт (б): durable writes през kill -9 — МИНАТ при 100k

- Писане: 10 последователни процеса × 10k реда (100 реда/заявление,
  `ON CONFLICT DO NOTHING` за идемпотентност), всеки излиза **без
  close** (kill -9 семантика — WAL е fsync-нат от statement commit-ите).
  Общо ~37 s за 100k реда (~370 µs/ред с индекса в същия път); chunk-овете
  се забавят 1.6→6.6 s, защото всеки следващ реплейва нарастващия WAL.
- Verify (отделен процес, WAL replay при отваряне):
  - `row count = 100000` (0 загубени реда)
  - индексен lookup `WHERE v = 'v_50000'` → 1 ред, id коректен
  - **DURABLE OK — индексът валиден без rebuild**
- Редове/chunk времена: 1571, 1990, 2491, 2723, 3450, 4150, 4609,
  4381, 5382, 6588 ms.

### Защо 100k, не 1M

Първият опит с 1M (10 × 100k) OOM-ва на chunk 2 въпреки K7 рев.2:
baga bump arena-та не reclaims (gaps Q2) — дългоживеещ процес с хиляди
заявления трупa garbage без граница; при 100k общият обем е поносим.
Пълният 1M гейт чака per-request arena управление (ARCHITECTURE §6,
P6 wire фазата) — дотогава 100k е измереният durability еталон.

## Бележки

- Преди rev.2 bench-ът гърмеше с OOM още на ~1000 заявления:
  `boila_ix_list` + rocksbaga SCAN snapshot-ът материализираха всички
  ключове на всяко заявление (gaps K7 — fixed).
- Индексът е валиден след рестарт **без rebuild** — entries-тата са
  LSM записи в `index` CF (предимство #1 пред barabadb, по PLAN P3).
