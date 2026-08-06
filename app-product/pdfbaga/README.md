# pdfbaga

**PDF writer** with **UTF-8 / Cyrillic** via embedded TrueType (`Identity-H`).

| | |
|--|--|
| **sandak** | `pdfbaga` **0.1.0** |
| **Deps** | `std`, `bufbaga` |
| **Font** | default `/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf` |

```baga
import "pdfbaga/pdf.baga"

// rows: tab-separated lines (header first)
let r = pdf_from_table_default("Заглавие", rows)?
// r.data → write_file_bytes("out.pdf", r.data)
```

Or pass font path: `pdf_from_table_file(path, title, rows)`.

## UTF-8

- Decodes UTF-8 codepoints (`utf8.baga`)
- Maps Unicode → glyph id via TTF `cmap` format 4
- Embeds full TTF as `FontFile2` (CIDFontType2)

Requires a Unicode TTF on the machine (DejaVu / Noto / Liberation).
