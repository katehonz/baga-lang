# zipbaga — gaps

Design: `docs/superpowers/specs/2026-08-06-officebaga-design.md`.

## Closed in 0.1.0

### Z1 — DEFLATE inflate ✅

`inflate.baga`: stored + fixed + dynamic Huffman, LZ77 backrefs.
Vectors + real zlib streams (100× `a`, 900-byte fox text).

### Z2 — CRC-32 IEEE ✅

`crc32.baga` poly `0xEDB88320`. Distinct from `std` CRC-32C.

### Z3 — ZIP reader ✅

EOCD + central directory + local headers; methods 0 and 8; CRC check.
No ZIP64, no encryption, no multi-disk.

## Open

### Z4 — ZIP writer (store) ✅

`zip_write_store` method 0. Deflate compress still open (Z5).

### Z5 — deflate compress ✅ (0.2.1)

Fixed Huffman + greedy LZ77 (4 KiB window, match 3–258). No dynamic Huffman
trees — sizes near zlib on repetitive XML, not on incompressible noise.

### Z6 — full archive in memory

**Plan:** lazy pread later (Task 11). P0 slurp OK.

### Z7 — Huffman decode is O(n_symbols) per symbol

**Symptom.** Linear scan over code table; fine for Office sizes, not ideal
for multi-MB throughput.

**Verdict.** Accept; table/tree optimize if profiling asks.
