# reportbaga

**Счетоводни отчети:** данни / HTML → **Excel · CSV · PDF · HTML preview**.  
PDF с **UTF-8 / кирилица** (вграден TTF).

| | |
|--|--|
| **sandak** | `reportbaga` **0.3.1** |
| **Deps** | csvbaga, officebaga, pdfbaga, tplbaga, xmlbaga, bufbaga |
| **Fixtures** | `fixtures/oborotna.html`, `oborotna.tpl.html` |

## Поток

```
DB / CsvTable  ──► ReportIR ──► xlsx | csv | pdf | html
HTML preview   ──► ReportIR ──┘
tplbaga shell  ──► HTML preview + export (таблица от данни)
```

## API

```baga
// От HTML (preview в браузър)
report_from_html(html, "xlsx")
report_from_html_io(html, "pdf")?

// От таблица (редове от SQL)
report_from_table(title, meta, csv_table, "xlsx")
report_from_table_io(title, meta, csv_table, "pdf")?

// Шаблон + данни ({{title}} {{period}} {{firm}} {{{table}}})
let ctx: Map<str,str> = map_new()
map_set(ctx, "title", "Оборотно-салдова")
map_set(ctx, "period", "2026-01")
map_set(ctx, "firm", "Демо ООД")
report_from_template(report_default_tpl(), ctx, table, "html")
report_from_template_io(tpl, ctx, table, "pdf")?
```

## CLI

```bash
cd app-product/reportbaga && sandak build

# статичен HTML
baga -I ../.. -I .. demo.baga html-xlsx fixtures/oborotna.html /tmp/o.xlsx
baga -I ../.. -I .. demo.baga html-pdf  fixtures/oborotna.html /tmp/o.pdf

# данни в кода (sample)
baga -I ../.. -I .. demo.baga data-xlsx /tmp/d.xlsx
baga -I ../.. -I .. demo.baga data-html /tmp/d.html

# tplbaga + sample
baga -I ../.. -I .. demo.baga tpl-html /tmp/t.html
baga -I ../.. -I .. demo.baga tpl-pdf  /tmp/t.pdf
```

**Product CLI:** `apps/report` — see `docs/runbooks/report.md`.  
Excel: frozen header + sheet name = report title.
