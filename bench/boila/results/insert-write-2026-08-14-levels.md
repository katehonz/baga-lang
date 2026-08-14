# insert-write — 2026-08-14 (cost-based levels — pair-collapse махнат)

След 1M гейта (10.4 GB / 3281 s) остатъкът беше ~10 KB/ред, суперлинеен
с дълбочината: при `target_bytes=0` L1/L2/L3 pair-collapse препрочиташе
целия слой на всеки втори SST. Фикс: compact само при `nfiles ≥ compact_at`
или byte target; boilaDB default `BOILA_TARGET_BYTES=1MiB`
(L0=1M L1=4M L2=16M L3=64M).

Същата машина, precompiled binary (без gcc в RSS), non-rc, CHUNKS=1.

## Group commit

| mode | ns/row |
|------|--------|
| A (1 ред/stmt) | 1 859 730 |
| B (100 ред/stmt) | 59 590 |
| **ratio A/B** | **3120%** — гейт ≥ 300% МИНАВА |

## Single-process durability

| редове | wall | peak RSS | ns/row | преди (Q2 reclaim) |
|--------|------|----------|--------|---------------------|
| 200k | 21.7 s | **276 MB** | 108 µs | 137 s / 746 MB |
| **1M** | **169 s** | **1.35 GB** | 164 µs | 3281 s / 10.4 GB |

Verify 1M: **3.4 s / 582 MB**, DURABLE OK (0 загубени, индекс без rebuild).

## Честен вердикт

RSS 1M: **10.4 GB → 1.35 GB (−87%)**. Wall **3281 s → 169 s (−95%)**.
Кривата е почти линейна (200k → 1M = 5× реда, 4.9× RSS). Остатък:
лек суперлинеен wall към 1M (L3 compact над 64 MB); GET вижда повече
SST файлове (bloom/meta ги държат евтини).
