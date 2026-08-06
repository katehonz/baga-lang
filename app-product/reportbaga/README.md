# reportbaga

**Accounting reports:** HTML5 table (browser preview) → **Excel / CSV / PDF**.  
PDF uses **UTF-8 + embedded TTF** (Cyrillic OK with DejaVu).

| | |
|--|--|
| **sandak** | `reportbaga` **0.2.0** |
| **Deps** | `csvbaga`, `officebaga`, `pdfbaga`, `xmlbaga`, `bufbaga` |
| **Fixture** | `fixtures/oborotna.html` |

```baga
import "reportbaga/report.baga"

let x = report_from_html(html, "xlsx")
let c = report_from_html(html, "csv")
let p = report_from_html_io(html, "pdf")?   // needs font file
```

## CLI

```bash
cd app-product/reportbaga && sandak build
baga -I ../.. -I .. demo.baga html-xlsx fixtures/oborotna.html /tmp/ob.xlsx
baga -I ../.. -I .. demo.baga html-csv  fixtures/oborotna.html /tmp/ob.csv
baga -I ../.. -I .. demo.baga html-pdf  fixtures/oborotna.html /tmp/ob.pdf
baga -I ../.. -I .. demo.baga csv-xlsx  /tmp/ob.csv /tmp/from.csv.xlsx
```

## HTML dialect (P0)

Well-formed XHTML with at least one `<table>`. First table → sheet.
Optional `<h1>` title (metadata only for now).
