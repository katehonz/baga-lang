# Report pipeline — implementation plan

> **For agentic workers:** task-by-task. Spec:
> `docs/superpowers/specs/2026-08-06-report-pipeline-design.md`

**Goal:** Accounting-style reports: **HTML5 tables → Excel + PDF** (plus CSV
for DB). Product truth from bookkeeping apps: preview HTML, export xlsx/pdf.

**Stack:** pure Baga; reuse `officebaga`, `zipbaga`, `xmlbaga`, `tplbaga`,
`bufbaga`, `bagadecimal` (money display), `std` binary I/O.

**Critical path:** R0 csv → R1 html-table→xlsx → R2 pdf tables → R4 one-shot
render. DOCX is optional.

---

## R0 — csvbaga ✅ shipped 0.1.0

**Files:** `app-product/csvbaga/` · **Tests:** `tests/csv_test.baga`

- [x] `csv_parse` / `csv_stringify` (`,` `"` `""` CRLF)
- [x] `csv_read_path` / `csv_write_path`
- [x] Round-trip + quoted fields
- [x] CLI `demo.baga rows|write`

---

## R1 — HTML table → XLSX ✅ shipped (reportbaga 0.1.0)

**Files:** `app-product/reportbaga/` · **Tests:** `tests/report_test.baga`

- [x] `html_table_to_csv` (first table, XHTML via xmlbaga)
- [x] `report_html_to_xlsx` / `report_html_to_csv` / `report_csv_to_xlsx`
- [x] Fixture `fixtures/oborotna.html` → xlsx open + text check
- [x] CLI `html-xlsx` / `html-csv` / `csv-xlsx`

---

## R2 — pdfbaga (write) ✅ shipped 0.1.0 (UTF-8)

**Files:** `app-product/pdfbaga/` · **Tests:** `tests/pdf_test.baga`

- [x] PDF catalog / pages / content streams
- [x] **UTF-8** via embedded TTF (cmap format 4 → glyph id, Identity-H)
- [x] Table + title, multi-page
- [x] Default font DejaVuSans path
- [x] Wired: `report_html_to_pdf` / `html-pdf` CLI

### R2b — later polish

- [ ] Font subset (smaller PDFs than full TTF embed)
- [ ] Richer ToUnicode for copy-paste
- [ ] Column auto-width / wrap

---

## R3 — htmlrbaga (XHTML report dialect → IR)

**Files:** `app-product/htmlrbaga/`

- [ ] Parse well-formed XHTML subset with `xmlbaga`
- [ ] Build `ReportIR` blocks (h/p/ul/table/hr)
- [ ] Reject script / unknown tags with clear error
- [ ] Fixtures: `fixtures/invoice.html`, `fixtures/table.html`

---

## R4 — reportbaga emitters

**Files:** `app-product/reportbaga/`

- [ ] `report_to_csv` / `report_to_docx` / `report_to_xlsx` / `report_to_pdf` / `report_to_html`
- [ ] `report_from_html(html, format)`
- [ ] One fixture → all formats smoke test
- [ ] CLI `demo.baga render in.html pdf out.pdf`

---

## R3/R4/R5 — ReportIR + emitters + tpl ✅ (reportbaga 0.3.0)

- [x] `ReportIR` (title, meta, table)
- [x] `report_emit` / `report_from_html` / `report_from_table`
- [x] `report_from_template` + `report_default_tpl` (tplbaga)
- [x] HTML preview regenerate from IR
- [x] CLI: data-*, tpl-*, html-*
- [x] `tests/report_test.baga` IR + tpl

---

## R6 — product `apps/report` ✅ CLI + runbook

- [x] `apps/report/main.baga` — render / sample / tpl
- [x] `docs/runbooks/report.md` product path
- [x] XLSX freeze header + sheet name from title
- [ ] Optional later: `fmrbaga` HTTP download
- [ ] Optional later: CSV upload API

---

## Suggested order (calendar)

```
week 1   R0 csvbaga (+ R1 if fast)
week 2   R2 pdfbaga Latin text+table
week 3   R3 htmlr + ReportIR
week 4   R4 emitters + one golden report
week 5   R5 tpl + R6 app
```

## Dependencies between tasks

```
R0 ──► R1 ──► R4 (csv path)
R2 ──────────► R4 (pdf path)
R3 ──────────► R4 (html path)
officebaga ──► R1, R4 (docx/xlsx)
R4 ──► R5 ──► R6
```

## Out of scope

- Full browser HTML/CSS  
- PPTX  
- Reading arbitrary PDF (write-first)  
- LibreOffice/Chromium  
