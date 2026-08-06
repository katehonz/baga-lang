# Office stack (zipbaga + officebaga) — implementation plan

> **For agentic workers:** implement task-by-task. No git commits by
> implementers unless the user asks. Each task ends with a green smoke of
> what it added.

**Goal:** Pure-Baga Office extract/create path: ZIP foundation first, then
DOCX text, then XLSX + write.

**Spec:** `docs/superpowers/specs/2026-08-06-officebaga-design.md`

**Tech:** packages `app-product/zipbaga` + `app-product/officebaga`; deps
`xmlbaga`, `bufbaga`, `std`; tests `tests/zip_test.baga`,
`tests/office_test.baga`.

**Principles**

1. Foundation before formats — no DOCX code before zip reader works.  
2. Extract before edit — plain text first; create second.  
3. Two packages, not one mega-folder.  
4. Every language pain → that package’s `gaps.md` the same day.

---

## Phase P0 — zipbaga + DOCX plain text

### Task 1: IEEE CRC-32 ✅

**Files:** `app-product/zipbaga/crc32.baga`, `tests/zip_test.baga`

- [x] `crc32_update` / `crc32_final` / `crc32_b` (poly `0xEDB88320`)
- [x] Known vectors (empty, `"123456789"` → `0xCBF43926` / 3421780262)
- [x] Note in `zipbaga/gaps.md` (Z2 closed)

### Task 2: DEFLATE inflate ✅

**Files:** `app-product/zipbaga/inflate.baga`

- [x] Raw DEFLATE inflate (RFC 1951): stored blocks + fixed Huffman + dynamic Huffman
- [x] Vectors: `hello`, `hello world`, 100×`a`, 900-byte dynamic stream
- [x] Clear error on corrupt stream

### Task 3: ZIP reader ✅

**Files:** `app-product/zipbaga/archive.baga`, `zip.baga` re-export

- [x] Find EOCD, parse central directory
- [x] `zip_open_bytes` / `zip_count` / `zip_name` / `zip_find` / `zip_read` (method 0 + 8)
- [x] CRC verify after inflate
- [x] `sandak build` on zipbaga; `tests/zip_test.baga` green

### Task 4: officebaga OPC + DOCX extract ✅

**Files:** `opc/*`, `docx/read.baga`, `office.baga`

- [x] Open package via zipbaga; get part by path
- [x] Detect docx/xlsx/odt/ods (pptx rejected)
- [x] Pull-parse `word/document.xml` → plain text
- [x] `office_open` / `office_open_bytes` / `office_plain_text`

### Task 5: CLI + office tests ✅

- [x] `demo.baga text|info|create-docx|create-xlsx`
- [x] `tests/office_test.baga` green (create round-trip + ODT/ODS + pptx reject)
- [x] README

**P0 success met** (plus XLSX/ODF ahead of schedule).  
**No presentations** by product decision.

---

## Phase P1 — XLSX + create (mostly done in 0.1.0)

### Task 6: XLSX read ✅

- [x] sharedStrings + inlineStr + sheet rows (TSV text)

### Task 7: ZIP writer (store) ✅

- [x] `zip_write_store` in zipbaga 0.1.1

### Task 8: DOCX / XLSX create ✅

- [x] Minimal packages; round-trip in `office_test`

### Task 9: Markdown extract + CLI ✅

- [x] `office_to_markdown` (paragraphs / GFM table)
- [x] `demo.baga text|md|create-*`
- [x] `std` `read_file_bytes` / `write_file_bytes` (ZIP-safe path API)

### Task 9b: ODF create + md→docx ✅ (0.1.2)

- [x] `odt_create_bytes` / `ods_create_bytes`
- [x] `office_md_to_docx_bytes` via mdbaga line classifiers
- [x] CLI `create-odt` / `create-ods` / `md-docx`

---

## Phase P2 — harden (presentations still out of scope)

### Task 10: PPTX text

- [ ] **Skipped** — product decision: no presentations

### Task 11: deflate + lazy zip ✅

- [x] `deflate.baga` — fixed Huffman + LZ77 (0.2.1)
- [x] `zip_write_deflate` / `zip_write_methods`
- [x] Entry inflate lazy (CD at open; inflate on `zip_read`)
- [ ] Full-file mmap (still `read_file_bytes` slurp)

### Task 12: IR wire-up ✅

- [x] `ir/ir.baga` — `IrDoc` word/sheet; `office_to_ir` / plain + md via IR

### Task 13: md → docx ✅

- [x] Peer `mdbaga` heuristics (inline still literal)

---

## Phase P3 — edit + OLE ✅ (0.2.0 partial)

- [x] Package rebuild edit: `office_replace_text` / `office_set_part` / `office_save`
- [x] CFB/OLE2 reader: list streams + naive ASCII from WordDocument/Workbook
- [ ] True binary edit of .doc/.xls  
- [ ] Inline formatting in md→docx  
- [ ] Full Word FIB / Excel BIFF extract


---

## Suggested first week

```
day 1–2   Task 1–2  CRC + inflate
day 3     Task 3    ZIP reader
day 4–5   Task 4–5  DOCX extract + tests
day 6     polish READMEs + gaps
```

## Out of scope

- Real LibreOffice / LOK FFI  
- PDF layout/raster  
- Full ECMA-376 coverage  
