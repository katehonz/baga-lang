# insert-write — 2026-08-12 (MEM-4б/в runtime)

След MEM-4б (scan snapshot persist fix) и MEM-4в (header-tag O(1) persist
детекция + O(1) rewind freelist clear). Хардуер: същата машина като
2026-08-08 (12 cores, 32 GB RAM).

## Group commit гейт (mode A/B, chunk 0 на insert_write.baga)

| mode | ns/row | бележка |
|------|--------|---------|
| A (1 ред/stmt, sync-per-write) | 1 829 735 | fsync на заявление |
| B (100 реда/stmt, group commit) | 69 831 | един fsync на stmt |
| **ratio A/B** | **2620%** | гейт ≥ 300% — **МИНАВА** (P3: 1722%) |

## Single-process durability (BOILA_CHUNKS=1 — MEM-4 деблокира това)

| редове | резултат | wall | RSS |
|--------|----------|------|-----|
| 100k | ОК, излиза без close (kill -9 семантика) | 36.5 s (365 µs/ред) | ~590 MB |
| 200k | ОК | 143 s (първите 100k: 36 s) | расте |
| ~250k | **OOM kill** (RSS 29 GB) | — | 29 GB |

## Честен вердикт

1M single-process гейтът **НЕ е приземен**. MEM-4 rewind-ът реално
reclaim-ва заявките (100k минава чисто в един процес; преди MEM-4 OOM
беше на chunk 2 от 10×100k), но persist регионът експлодира между 100k
и 250k реда: 3.7M × 8 KB блока. Виновници (return-address броячи,
`baga_alloc` persist n≥1 KB): `pc_get` page fills (4 KB), map_rehash
стари bucket масиви, vec-push doubling intermediate-и, per-gen
bloom/sst meta — всеки persist alloc без explicit drop е вечен.
Пълен анализ и числа: `app-product/boilaDB/gaps.md` Q2.

Bonus находки от тази сесия (в bench/boila или api):
- serve_pg extended протокол: 44 ms → ~87 µs/заявка след TCP_NODELAY
  при accept (`tools/serve_pg.baga`).
- PG portal/stmt reclaim: ~10 KB → ~0.4 KB persist на extended заявка.
