# zipbaga

**ZIP + DEFLATE** for Baga — pure container library (RFC 1951 inflate,
ZIP reader, IEEE CRC-32). No C/FFI.

Used by **officebaga** (OOXML packages) and any ZIP consumer.
Does **not** depend on XML or Office formats.

| | |
|--|--|
| **sandak** | `zipbaga` **0.2.2** |
| **Layout** | `zip.baga` · `crc32` · `inflate` · `deflate` · `archive` |
| **Deps** | `std` only |
| **Design** | [spec](../../docs/superpowers/specs/2026-08-06-officebaga-design.md) |
| **Plan** | [plan](../../docs/superpowers/plans/2026-08-06-officebaga.md) · [gaps](gaps.md) |
| **Tests** | `tests/zip_test.baga` |

## Status

| Piece | Status |
|-------|--------|
| IEEE CRC-32 | ✅ `crc32_b` / `crc32_update` / `crc32_final` |
| DEFLATE inflate | ✅ stored + fixed + dynamic Huffman |
| ZIP reader | ✅ method 0 store + method 8 deflate, CRC verify |
| ZIP writer | ✅ store / deflate / per-entry methods |
| deflate compress | ✅ fixed Huffman + **LZ77** (4 KiB window) |

## API

```baga
import "zipbaga/zip.baga"
import "zipbaga/crc32.baga"
import "zipbaga/inflate.baga"
import "zipbaga/archive.baga"

// CRC-32 IEEE (ZIP), not CRC-32C
let c = crc32_b(bytes_of_str("123456789"))  // 3421780262

// raw DEFLATE (no zlib header)
let r = inflate_raw(comp)
// r.ok, r.data, r.err

// ZIP archive
let z = zip_open_bytes(file_bytes)
// z.ok, zip_count(z), zip_name(z, i), zip_find(z, "path")
let p = zip_read(z, "word/document.xml")
// p.ok, p.data, p.err

// write
let names = vec_new()
let parts = vec_new()
vec_push(names, "a.txt")
vec_push(parts, bytes_of_str("hi"))
let out = zip_write_store(names, parts)     // method 0
let out2 = zip_write_deflate(names, parts)  // method 8
// ODF: mimetype store + rest deflate via zip_write_methods
// edit: replace one entry and rebuild
let p = zip_set_entry(z, "word/document.xml", new_xml, 1)
// p.data = new zip bytes
```

## Run tests

```bash
./baga -I . -I app-product tests/zip_test.baga
# zip_test: all passed
```

## Related

- `officebaga` — Office formats on top of this package  
- `std/crypto/crc32c` — **different** poly (Castagnoli); do not use for ZIP  
