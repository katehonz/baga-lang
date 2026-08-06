# Office documents stack (zipbaga + officebaga) — design

Date: 2026-08-06. Status: **design**.  
Roadmap: apps-roadmap **№12** (after xmlbaga №11).  
Plan: `docs/superpowers/plans/2026-08-06-officebaga.md`

## Goal

Pure-Baga toolkit for **Office documents** — read, extract, write, convert —
without LibreOffice, without FFI, without C deps.

**Not one monorepo folder.** Design/docs and code packages live in **separate
trees**, and the stack is **multiple sandak packages** (libreoffice-rs crate
style), not a single `officebaga/` dump.

## Inspiration (Rust, not ports)

| Project | Take | Skip |
|---------|------|------|
| [office_oxide](https://crates.io/crates/office_oxide) | Unified Document API, IR, DOCX/XLSX/PPTX first | Bindings, full OLE2 parity |
| [libreoffice-rs](https://github.com/clark-labs-inc/libreoffice-rs) | Separate crates (zip / writer / calc), honest non-parity | Full suite, PDF raster, UNO |
| [litchi](https://github.com/DevExzh/litchi) | Long-horizon format map | v0 scope |
| baga `xmlbaga` / `rocksbaga` | Pull XML, package conventions, gaps probes | Flat mega-package |

## Layout (where things live)

```
docs/superpowers/
  specs/2026-08-06-officebaga-design.md   ← this file (architecture)
  plans/2026-08-06-officebaga.md          ← implementation tasks

app-product/
  zipbaga/          # independent: DEFLATE + ZIP + CRC-32 IEEE
  officebaga/       # formats + IR + public API (depends on zipbaga, xmlbaga)
  xmlbaga/          # already shipped — XML parts
  bufbaga/          # already shipped — builders
  mdbaga/           # optional peer for md ↔ docx later
```

| Concern | Path |
|---------|------|
| Architecture / design | `docs/superpowers/specs/…` |
| Implementation plan | `docs/superpowers/plans/…` |
| ZIP foundation | `app-product/zipbaga/` |
| Office formats + CLI | `app-product/officebaga/` |
| Product gaps (per package) | `zipbaga/gaps.md`, `officebaga/gaps.md` |

## Problem shape

```
.docx / .xlsx / .pptx   =  ZIP  +  Office Open XML parts
.odt  / .ods  / .odp    =  ZIP  +  OpenDocument XML parts
.doc  / .xls  / .ppt    =  CFB/OLE2 (later; not P0)
```

Data path:

```
disk bytes
    │
    ▼
┌──────────┐     ┌──────────┐     ┌────────────┐     ┌────┐
│ zipbaga  │────▶│   opc    │────▶│ docx/xlsx/ │────▶│ ir │
│ inflate  │     │ (office) │     │ pptx       │     └─┬──┘
└──────────┘     └──────────┘     └────────────┘       │
                                                       ├─ plain_text
                                                       ├─ markdown
                                                       └─ create (zip write)
```

## Package A — `zipbaga`

**Role:** reusable container library (any ZIP consumer: Office, ODF, jar-ish).

```
zipbaga/
├── README.md
├── gaps.md
├── sandak.toml
├── zip.baga              # public re-export
├── crc32.baga            # IEEE CRC-32 (ZIP; not crc32c)
├── inflate.baga          # RFC 1951 inflate
├── deflate.baga          # RFC 1951 deflate (P1+/P2)
└── archive.baga          # reader (+ later writer)
```

**API sketch:**

```baga
fn zip_open_bytes(b: bytes) -> ZipResult
fn zip_list(z: ZipArchive) -> Vec<str>
fn zip_read(z: ZipArchive, name: str) -> ZipPartResult   // inflate + CRC
fn zip_write_store(...) -> bytes                         // P1: method 0
```

**Rules:** no import of `officebaga` / `xmlbaga`. Only `std` (bytes, os, crypto helpers).

### ZIP details

1. EOCD → central directory → local headers  
2. Method 0 store + method 8 DEFLATE  
3. CRC-32 IEEE poly `0xEDB88320` (distinct from `std` CRC-32C)  
4. P0 read path targets **real Office files** (inflate required)  
5. Write path may use **store-only** until deflate lands  

## Package B — `officebaga`

**Role:** Office formats, OPC, IR, unified Document API, CLI.

```
officebaga/
├── README.md
├── gaps.md
├── sandak.toml
├── office.baga           # public API
├── demo.baga             # CLI
├── opc/
│   ├── package.baga
│   ├── content_types.baga
│   └── rels.baga
├── core/
│   ├── detect.baga
│   └── xml_parts.baga
├── ir/
│   └── ir.baga
├── docx/
│   ├── read.baga
│   └── write.baga
├── xlsx/
│   ├── read.baga
│   └── write.baga
├── pptx/                 # P2
├── convert/
│   ├── text.baga
│   └── md.baga
└── fixtures/             # small goldens
```

**Deps:** `zipbaga`, `xmlbaga`, `bufbaga`, `std`; optional `mdbaga` later.

**Rules:**

| Rule | Meaning |
|------|---------|
| Formats do not import each other | `docx` ↛ `xlsx` |
| `ir` has no I/O | pure data |
| Only `office.baga` orchestrates | detect → open → extract |
| No reimplementation of ZIP | always `zipbaga` |

### Public API (capability contract)

```baga
struct OfficeResult { ok: i64, err: str, doc: Document }

fn office_open(path: str) -> OfficeResult !IO
fn office_open_bytes(b: bytes) -> OfficeResult
fn office_plain_text(d: Document) -> str
fn office_to_markdown(d: Document) -> str          // P1
fn office_format_name(d: Document) -> str
fn office_create_docx(path: str, title: str, body: str) -> OfficeResult !IO  // P1
fn office_create_xlsx(...) -> OfficeResult !IO     // P1
```

`OfficeResult` = L3 ok/err stand-in (same as jsonrpc/pg).

### IR (honest subset)

```
IrDoc: word | sheet | deck
  blocks / sheets / slides — enough for extract + simple create
```

Bold/italic runs in P1; full styles non-goal until a product asks.

### Format notes

| Format | Parts | P0/P1 |
|--------|-------|-------|
| DOCX | `word/document.xml` (`w:p` / `w:t`) | P0 extract, P1 write |
| XLSX | sharedStrings + sheet cells | P1 extract + write |
| PPTX | `a:t` on slides | P2 |
| ODF | `content.xml` | P3 |
| OLE2 | CFB | P3 research |

xmlbaga has **no namespace stack** (X1): match `w:t` or local-name after `:`.

## Dependency graph

```
        officebaga/demo
              │
              ▼
         officebaga
         /    |    \
        v     v     v
     docx   xlsx   opc/ir
        \     |     /
         \    v    /
          xmlbaga  bufbaga
              │
              ▼
           zipbaga
              │
              ▼
        std (bytes, os, …)
```

## Non-goals

- LibreOffice parity, layout, print, PDF raster  
- Macros / VBA / encrypted packages  
- Edit-in-place of arbitrary third-party DOCX (P3+)  
- Formula engine (Calc-class)

## Risks

| Risk | Mitigation |
|------|------------|
| No DEFLATE in language | P0 owns inflate in **zipbaga** |
| CRC-32 IEEE missing | zipbaga `crc32.baga` (not crc32c) |
| O(n²) concat | bufbaga on all extract/write |
| Spec surface | IR subset + phased plan |
| Mega-folder creep | keep zip vs office as **two packages** |

## Success (design level)

1. Design in `docs/superpowers/specs`, plan in `plans` — not buried only in product dirs.  
2. `zipbaga` builds and extracts DEFLATE entries independently.  
3. `officebaga` opens real/fixture `.docx` and prints plain text.  
4. Later: create minimal docx/xlsx Word/LibreOffice can open.  
5. Gaps logged per package; tests under `tests/zip_test.baga`, `tests/office_test.baga`.
