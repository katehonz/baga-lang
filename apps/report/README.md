# apps/report — product CLI (счетоводни отчети)

HTML5 preview path → **Excel / CSV / PDF / HTML**.  
Library: `reportbaga`. No LibreOffice.

## Run

```bash
# from repo root
./baga -I . -I app-product apps/report/main.baga sample xlsx /tmp/ob.xlsx
./baga -I . -I app-product apps/report/main.baga sample pdf  /tmp/ob.pdf
./baga -I . -I app-product apps/report/main.baga sample html /tmp/ob.html

./baga -I . -I app-product apps/report/main.baga render \
  app-product/reportbaga/fixtures/oborotna.html xlsx /tmp/r.xlsx

./baga -I . -I app-product apps/report/main.baga tpl html /tmp/preview.html
```

PDF needs a Unicode TTF (default DejaVuSans on Linux).

## Commands

| Command | Meaning |
|---------|---------|
| `render <html> <fmt> <out>` | HTML file → format |
| `sample <fmt> <out>` | Built-in оборотно-салдова sample |
| `tpl <fmt> <out>` | tplbaga shell + sample table |

`fmt`: `xlsx` · `csv` · `pdf` · `html`

## Excel

- Sheet name = report title (max 31 chars)
- **First row frozen** (header)

## See also

- Runbook: `docs/runbooks/report.md`
- Design: `docs/superpowers/specs/2026-08-06-report-pipeline-design.md`
