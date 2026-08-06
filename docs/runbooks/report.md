# Runbook — accounting reports (HTML → Excel / PDF)

**Exit criteria:** from repo root, export a sample report to **xlsx** and **pdf**
(and optional browser **html**) without LibreOffice.

## Prerequisites

1. Toolchain: `./baga` in repo root (`make` if needed)
2. For **PDF**: Unicode font, default  
   `/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf`  
   (install `fonts-dejavu-core` on Debian/Ubuntu if missing)

## Quick path (product CLI)

```bash
cd /path/to/baga

# Sample оборотно-салдова → Excel (frozen header + sheet name)
./baga -I . -I app-product apps/report/main.baga sample xlsx /tmp/ob.xlsx

# Same → PDF (UTF-8 / Cyrillic)
./baga -I . -I app-product apps/report/main.baga sample pdf /tmp/ob.pdf

# Browser preview HTML
./baga -I . -I app-product apps/report/main.baga sample html /tmp/ob.html
# open /tmp/ob.html in a browser
```

From static XHTML fixture:

```bash
./baga -I . -I app-product apps/report/main.baga render \
  app-product/reportbaga/fixtures/oborotna.html xlsx /tmp/r.xlsx

./baga -I . -I app-product apps/report/main.baga render \
  app-product/reportbaga/fixtures/oborotna.html pdf /tmp/r.pdf
```

Template + data (tplbaga):

```bash
./baga -I . -I app-product apps/report/main.baga tpl html /tmp/preview.html
./baga -I . -I app-product apps/report/main.baga tpl xlsx /tmp/preview.xlsx
```

## Library path (from another package)

```baga
import "reportbaga/report.baga"
import "csvbaga/csv.baga"

// rows from SQL → CsvTable (or csv_from_tsv)
let t = csv_from_tsv("A\tB\n1\t2")
let x = report_from_table("Отчет", "2026-01 · Фирма", t, "xlsx")
// x.data → write_file_bytes("out.xlsx", x.data)

let p = report_from_table_io("Отчет", "2026-01", t, "pdf")?
```

## Automated checks

```bash
./baga -I . -I app-product tests/csv_test.baga
./baga -I . -I app-product tests/pdf_test.baga
./baga -I . -I app-product tests/report_test.baga
```

## Stack

| Package | Role |
|---------|------|
| `csvbaga` | CSV parse/write, table model |
| `reportbaga` | HTML/IR/tpl → formats |
| `officebaga` | XLSX create (freeze + sheet name) |
| `pdfbaga` | PDF UTF-8 + TTF embed |
| `apps/report` | product CLI |

## Notes

- HTML must be **well-formed XHTML** with a `<table>` for `render`.
- PDF embeds full TTF (~0.7 MB) — subset later.
- No presentations (PPTX). DOCX optional / not on critical path.
