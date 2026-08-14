# insert-write — 2026-08-14 (Q2 persist reclaim — 1M гейтът е приземен)

Част 3 на insert-write поредицата. След Q2 фиксовете в rocksbaga
(commit `2b9d7de`): `sst_meta_free_contents` при compaction del,
drop-ове на изчерпаните read buffer-и в `sst_meta_load`/`sst_rec_at_fd`
(drip в persist региона), deep free на орфания стар memtable при flush,
drop на старите gens/levels вектори. Хардуер: същата машина (12 cores,
32 GB RAM). Измерването е чисто: само компилирания binary (без gcc
subprocess в мерките), non-rc build, BOILA_CHUNKS=1.

## Group commit гейт (mode A/B, chunk 0)

| mode | ns/row |
|------|--------|
| A (1 ред/stmt, sync-per-write) | 1 849 683 |
| B (100 ред/stmt, group commit) | 71 241 |
| **ratio A/B** | **2596%** — гейт ≥ 300% МИНАВА |

## Single-process durability — 1M гейтът е приземен

| редове | резултат | wall | peak RSS |
|--------|----------|------|----------|
| 200k | ОК | 137–147 s | **746 MB** (преди Q2: 1990 MB, −62%) |
| 500k | ОК | 808 s | 3011 MB |
| **1M** | **ОК + verify DURABLE OK (0 загубени, индекс валиден без rebuild)** | **3281 s** | **10.4 GB** |

Преди Q2: OOM при ~250k реда с 29 GB (MEM-4д таван ~900k при 28 GB).

**Verify фаза:** WAL replay + one-shot `boila_scan_pref_all` за броя +
индексен lookup — **3.5 s / 583 MB**. Старият paging през `boila_scan`
събираше всички ключове на страница (O(N²/count)) — при 1M щеше да е
десетки минути; bench-ът е поправен в същия commit.

## RSS крива (1M, 10 s семпли)

Почти линейна след началния ramp: ~1.2 GB (5 мин) → ~5 GB (25 мин) →
10.4 GB (55 мин), ~3 MB/s в опашката. Растежът от дълбоките нива остава,
но вече е приземен, не OOM.

## Progress (write amplification забавяне)

| редове | wall | маргинално |
|--------|------|------------|
| 100k | 38.6 s | ~390 µs/ред |
| 200k | 137 s | ~980 µs/ред |
| 500k | 842 s | ~1.9 ms/ред |
| 1M | 3281 s | ~620 µs/ред (опашка) |

## Честен вердикт

1M single-process гейтът от PLAN P3 **е приземен**: kill-9 семантика
(излизане без close), 0 загубени реда, индексът валиден без rebuild,
on-disk 109 MB. Остатък: ~10 KB/ред средно (10.4 GB при 1M), все още
суперлинеен с дълбочината на нивата — следваща граница при нужда:
по-евтини re-reads при compaction или cost-based level sizing.
