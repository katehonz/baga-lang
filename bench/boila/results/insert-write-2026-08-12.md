# insert-write — 2026-08-12, част 2 (MEM-4д runtime)

След MEM-4д (recycling fix-ове по attribution профил). Хардуер: същата
машина (12 cores, 32 GB RAM). Методика на профила: генерираният C код се
пачва с броячи по return address в `baga_alloc` (само persist bump
алокации — freelist hits не се броят) + begin/end баланс по call site +
region-owner стек (кой persist wrap притежава алокацията).

## MEM-4д промени

1. **runtime `baga_vec_grow` free-ва стария data буфер** при doubling
   (`src/codegen_c.c`). Преди: всяка стъпка на удвояване в persist региона
   оставаше стълбичка от intermediate буфери завинаги.
2. **runtime `baga_map_rehash` free-ва стария bucket масив.**
3. **runtime `baga_map_del_*` free-ва unlinked entry** (и bytes вариантът).
   Преди: pin/unpin churn в page cache-а = 112 B вечен leak на цикъл
   (16.6M pins при 200k реда = 1.86 GB). Box стойността (pv) остава —
   del не знае val_size (drop_map я чисти).
4. **`pc_evict_one` drop-ва стария keys vec** (двата сайта,
   `rocksbaga/cache/page.baga`). Преди: ~16 KB вечен leak на eviction
   (531k evictions при 200k = 8.7 GB — 70% от целия растеж).

## Group commit гейт (mode A/B, chunk 0)

| mode | ns/row |
|------|--------|
| A (1 ред/stmt, sync-per-write) | 2 230 917 |
| B (100 реда/stmt, group commit) | 71 187 |
| **ratio A/B** | **3133%** — гейт ≥ 300% МИНАВА |

## Single-process durability (BOILA_CHUNKS=1)

| редове | резултат | wall | RSS |
|--------|----------|------|-----|
| 200k | ОК + verify DURABLE OK (0 загубени, индекс валиден) | 136 s | **1.9 GB** (преди: ~12+ GB, OOM при ~250k) |
| ~900k | спрян ръчно при 28.3 GB (1.1 GB свободни — защита на системата) | ~25 min | 28.3 GB |

## Честен вердикт

1M single-process гейтът пак **НЕ е приземен**, но таванът се вдигна
~3.6× (250k → ~900k реда при същите 29 GB). MEM-4д recycling-ът работи
(200k мина с 1.9 GB и пълна durability проверка). Оставащият растеж е
~9.5 KB/ред при 200k и се УСКОРЯВА с броя SST-та (суперлинеен) — спрямо
attribution профила (owner = wrap-овете в `lsm_get_kb`/`sst_get`):

- **SstMeta cache churn при compaction:** `map_del(db.sst_meta, g)` в
  compact.baga free-ва само entry-то (след MEM-4д), но НЕ и съдържанието —
  rkeys vec (~780 restart ключа × ~30 B), idx, box pv ≈ ~32 KB на
  compact-нат gen. Хиляди compactions при 1M реда.
- **meta_set churn в sst_get** (1.53M box/entry алокации при 100k) —
  gens, чиито meta не се кешира (ok=0/full-read път) или се del-ват и
  re-set-ват.
- Следваща стъпка: (а) explicit free на SstMeta съдържанието при
  compaction del (val_size-aware del или ръчен free loop); (б) meta
  кеш и за scan пътя (sst_scan_begin/_seek зареждат uncached);
  (в) live-byte профил (cumulatивните bump числа надценяват чрез
  freelist reuse).

Предишни части: MEM-4б/в — вж. по-долу.

---

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
