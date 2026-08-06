# Report pipeline: CSV + PDF + HTML5 → Office/PDF — design

Date: 2026-08-06. Status: **design**.  
Plan: `docs/superpowers/plans/2026-08-06-report-pipeline.md`  
Related: officebaga design `2026-08-06-officebaga-design.md`

## Goal

Build a **report product path** in pure Baga, driven by a real product shape:
**accounting / bookkeeping reports** (обороти, ведомости, ДДС, salda, фактури).

**Primary export path (product truth):**

```
HTML5 report (tables + headings)  →  Excel (.xlsx)  +  PDF
```

DOCX/CSV remain secondary (letters / DB interchange), not the main accountant UX.

```
data (Postgres / Map / CSV import)
        │
        ▼
  tplbaga HTML shell + numbers
        │
        ▼
   ReportIR  ←── HTML5 report subset (authoring + browser preview)
        │
   ┌────┴────┐
   ▼         ▼
  XLSX      PDF          (+ CSV for import/export, DOCX optional)
 officebaga  pdfbaga
```

**Product app:** `apps/report` — render report templates + data → file download.

**Language role:** prove table-heavy report pipelines on office/zip/xml — not a browser.

## Why HTML5 → Excel + PDF (accounting)

In accounting apps the report loop is usually:

1. Pull rows from DB (обороти, salda, фактури, ДДС дневник).  
2. Fill a **table-heavy** layout (title, period, columns, totals).  
3. User wants **Excel** (filter/sum in spreadsheet) and **PDF** (print/archive).  
4. Browser **preview** of the same HTML before export.

| Approach | For accounting | Verdict |
|----------|----------------|---------|
| Word merge | weak tables, bad for numbers | skip as primary |
| Markdown | weak multi-column money tables | optional only |
| **HTML5 tables + CSS light** | preview + one source | **primary authoring** |
| Direct XLSX builder only | no preview | keep as low-level |
| Chromium print-to-PDF | perfect | not pure-Baga |

**Decision:** HTML5 is a **report dialect** (tables first). Same HTML →
`ReportIR` → **XLSX + PDF** first-class. Preview = open HTML in browser.

### Accounting report shapes (examples)

| Report | HTML shape | Excel | PDF |
|--------|------------|-------|-----|
| Оборотно-салдова | title + period + wide table + totals row | 1 sheet | multi-page table |
| Дневник ДДС | table + header meta | 1 sheet | print A4 landscape |
| Фактура | header block + line table + sums | optional | primary |
| Ведомост / payroll | multi-column table | primary | archive PDF |
| Export за НАП/import | — | CSV secondary | — |

**Money:** use `bagadecimal` / string decimals in cells — never binary float
for displayed amounts when possible.

## What we already have

| Piece | Package | Role in report stack |
|-------|---------|----------------------|
| ZIP + DEFLATE | `zipbaga` | Office packages |
| DOCX/XLSX/ODT/ODS | `officebaga` | Word/Excel out |
| XML | `xmlbaga` | OOXML parts; HTML parse later if needed |
| Templates | `tplbaga` | inject data into HTML report shells |
| Markdown | `mdbaga` | optional md → report HTML |
| HTTP + API | `fmrbaga` / `apps/api` | download endpoints |
| Postgres | `pgbaga` / `ormbaga` | data → report rows |
| Builders | `bufbaga` | string assembly |
| Binary files | `std` `read_file_bytes` | CSV/PDF/ZIP I/O |

**Missing:** CSV, PDF, HTML report dialect, ReportIR, `apps/report`.

## Package map (not one mega-folder)

```
docs/superpowers/
  specs/2026-08-06-report-pipeline-design.md   ← this file
  plans/2026-08-06-report-pipeline.md

app-product/
  csvbaga/       # RFC 4180-ish CSV read/write (DB import/export)
  pdfbaga/       # pure PDF write (text + tables + simple layout)
  htmlrbaga/     # HTML report dialect → ReportIR  (name: htmlr = HTML report)
  reportbaga/    # orchestrate IR → csv/office/pdf; optional CLI
apps/
  report/        # product: HTTP or CLI — “render this report”
```

Alternative naming: `htmlbaga` if we want general HTML later; for v0 prefer
**report-scoped** `htmlrbaga` so nobody expects a browser.

## Layer details

### A — `csvbaga` (first; easiest, high DB value)

**Role:** interchange with databases, Excel, scripts.

```
csv_parse(src) -> CsvTable   // rows: Vec of Vec of str  (or parallel Vecs)
csv_stringify(table) -> str
csv_read_file / csv_write_file  via read_file_bytes / write_file_bytes
```

| Feature | P0 | Later |
|---------|----|-------|
| `,` delimiter | ✅ | `;` / tab options |
| Quotes `"` + escape `""` | ✅ | |
| CRLF / LF | ✅ | |
| Header row API | ✅ | |
| Streaming row iterator | | P1 (big imports) |
| Types (int/date) | | app layer |
| UTF-8 BOM | detect | |

**DB path:** `SELECT …` → rows → `csv_stringify` → download; or upload CSV →
`csv_parse` → `orm` bulk insert (app, not csvbaga).

**Office path:** CSV ↔ XLSX via officebaga (`office_xlsx_bytes` already
TSV-like; bridge with csv↔rows).

### B — `pdfbaga` (PDF write subset)

**Role:** print-ready reports without LibreOffice.

**Honest scope (P0):**

- PDF 1.4-ish, single / multi page
- Helvetica (built-in fonts only — no TTF embedding in P0)
- Text runs, paragraphs with wrap (simple width)
- Tables (grid lines + cell text)
- Margins, page break
- Optional: JPEG image embed (later)

**Non-goals P0:** full HTML/CSS layout, Unicode CJK fonts, forms, encryption,
PDF/A certification, reading arbitrary PDFs.

**Layout model:**

```
PdfPage { width, height, margin }
PdfDoc  { pages: … }
// or streaming page writer
pdf_text / pdf_table / pdf_new_page
pdf_to_bytes(doc) -> bytes
```

Compression: raw streams first; optional Flate via zipbaga deflate later.

### C — `htmlrbaga` (HTML5 report dialect → ReportIR)

**Allowed tags (P0):**

```
html body title
h1 h2 h3
p br
strong b em i
ul ol li
table thead tbody tr th td
hr
div (block only; class whitelist)
```

**Allowed style / class (P0):**

- `text-align: left|center|right`
- `font-weight: bold` (or `<strong>`)
- `width` on table (optional)
- classes: `title`, `muted`, `num` (map in report theme)

**Forbidden:** script, iframe, svg (P0), position absolute, flex/grid full
browser layout, external CSS files (inline style attr only or class map).

**Parser:** pull events — either reuse `xmlbaga` if we require XHTML-ish
well-formed HTML, or a lenient HTML tokenizer (more work). **P0
recommendation:** require **well-formed XHTML subset** so `xmlbaga` works
(`<br/>`, quoted attrs). Authors use templates that emit valid XHTML.

```
htmlr_parse(src) -> ReportIR
htmlr_from_tpl(tpl, ctx) -> ReportIR   // via tplbaga
```

### D — ReportIR (shared intermediate)

Lives in `reportbaga/ir.baga` (or `htmlrbaga/ir.baga`):

```
ReportIR:
  title: str
  blocks: Vec of block kinds
    Heading { level, text }
    Para { text, bold_spans? }     // P0: plain + whole-run bold
    List { ordered, items: Vec str }
    Table { headers: Vec str, rows: Vec of Vec str }
    Hr
    PageBreak                       // PDF/DOCX only
```

**Emitters:**

| Emitter | Package |
|---------|---------|
| `report_to_csv` | tables only → csvbaga (or all tables concatenated) |
| `report_to_docx` | officebaga create / enhanced body builder |
| `report_to_xlsx` | first/largest table → xlsx; or one sheet per table |
| `report_to_pdf` | pdfbaga layout |
| `report_to_html` | pretty-print dialect (identity / indent) |

### E — `reportbaga` + `apps/report`

**Library API:**

```baga
fn report_render(ir: ReportIR, format: str) -> ReportOut  // "pdf"|"docx"|"xlsx"|"csv"|"html"
fn report_from_html(html: str, format: str) -> ReportOut
fn report_from_template(tpl: str, data: Map, format: str) -> ReportOut
```

**App (`apps/report`):**

```
POST /v1/reports/render
  { "template": "...", "data": {...}, "format": "pdf" }
→ file bytes + Content-Disposition

GET  /v1/reports/demo.pdf
CLI: report render --format pdf in.html -o out.pdf
```

Uses `fmrbaga` when HTTP; CLI can be `demo.baga` only in P0.

## Data → report (product scenarios)

```
1) DB export
   orm query → rows → ReportIR table → csv | xlsx | pdf

2) Invoice / letter
   tplbaga HTML shell + Map fields → htmlr → docx | pdf

3) Ops dashboard dump
   metrics Map → simple HTML table → pdf

4) Bulk import
   upload csv → csv_parse → validate → orm insert
```

## Dependency graph

```
apps/report
     │
     ▼
 reportbaga
   /   |   \   \
  v    v    v   v
htmlr  pdf  csv officebaga
  │     │    │     │
  xml?  std  std  zip+xml
  tplbaga ────────┘
  bufbaga
```

**Rules:** `csvbaga` and `pdfbaga` have **no** dependency on office.
`reportbaga` is the only orchestrator. Apps never import `zipbaga` directly.

## Phased delivery (accounting-first)

Priority order matches “HTML → Excel + PDF”:

| Phase | Deliverable | Exit criteria |
|-------|-------------|---------------|
| **R0** | `csvbaga` | DB import/export + round-trip |
| **R1** | HTML fixture → **XLSX** (via officebaga) | table HTML → openable .xlsx |
| **R2** | `pdfbaga` table + multi-page | same fixture → .pdf |
| **R3** | `htmlrbaga` + ReportIR | formal dialect + IR |
| **R4** | `reportbaga` one call: html→xlsx+pdf | accounting demo report |
| **R5** | `tplbaga` + sample “обороти” template | data Map → both files |
| **R6** | `apps/report` download API/CLI | product path |

**Shortcut for MVP demo:** even before full htmlr, a **hand-built ReportIR**
from rows (skip HTML parse) can emit XLSX+PDF — HTML parse lands in R1/R3.

DOCX is **optional** (cover letters); not on the accounting critical path.

## Risks

| Risk | Mitigation |
|------|------------|
| Full HTML/CSS expectation | Document dialect; reject unknown tags |
| PDF Unicode / CJK | P0 Latin Helvetica; Cyrillic needs embedded font (R2b) |
| Cyrillic in PDF | **Important for this project** — plan font subset embed early (R2b) or use Type0 later |
| Large CSV import memory | Streaming API in R1+ |
| Layout fidelity | Tables + flow text only; no float/absolute |
| PDF size | optional deflate streams after text works |

**Cyrillic note:** baga docs and demos use Bulgarian. PDF built-in Helvetica
**does not** cover Cyrillic. R2 either:

1. Ship a minimal embedded font (e.g. subset DejaVu / Noto as binary fixture + simple cmap), or  
2. PDF P0 Latin-only + gap **P1 Cyrillic**, DOCX path for BG text first.

**Recommendation:** R2 Latin PDF + DOCX for BG; R2b embed one TTF subset for Cyrillic PDF.

## Success criteria (architecture)

1. Separate packages: csv / pdf / htmlr / report — clear deps.  
2. One HTML report fixture exports to **at least** CSV + DOCX + PDF.  
3. CSV usable for DB import path with orm/pg (app example).  
4. No LibreOffice/Chromium required.  
5. Gaps logged per package; presentations remain out of officebaga.

## Non-goals

- Browser engine / full CSS  
- Pixel-perfect Word round-trip from HTML  
- PPTX reports  
- Interactive PDF forms (P0)  
- Excel formula generation from HTML  
