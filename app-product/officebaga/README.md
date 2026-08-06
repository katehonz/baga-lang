# officebaga

**Office documents** for Baga — DOCX/XLSX/ODT/ODS (full path) + legacy DOC/XLS
(OLE2 probe). **No presentations.**

| | |
|--|--|
| **sandak** | `officebaga` **0.2.0** |
| **Deps** | `zipbaga`, `xmlbaga`, `bufbaga`, `mdbaga`, `std` |
| **Design** | [spec](../../docs/superpowers/specs/2026-08-06-officebaga-design.md) |
| **Plan** | [plan](../../docs/superpowers/plans/2026-08-06-officebaga.md) · [gaps](gaps.md) |
| **Tests** | `tests/office_test.baga` |

## Formats

| Format | Extract | Create | Edit | Notes |
|--------|---------|--------|------|-------|
| DOCX | ✅ | ✅ + md→docx | ✅ replace_text | ZIP deflate |
| XLSX | ✅ | ✅ | ✅ (sharedStrings/sheet XML) | |
| ODT | ✅ | ✅ | ✅ content.xml | mimetype stored |
| ODS | ✅ | ✅ | ✅ | |
| DOC | ⚠ ASCII probe | — | — | OLE2/CFB |
| XLS | ⚠ ASCII probe | — | — | OLE2/CFB |
| PPTX | ❌ | — | — | rejected |

## API

```baga
import "officebaga/office.baga"

let r = office_open("report.docx")?
print(office_plain_text(r.doc))
print(office_to_markdown(r.doc))

// edit (ZIP packages)
let e = office_replace_text(r.doc, "Hello", "Zdravei")
office_save(e.doc, "out.docx")?

// create
office_docx_bytes("Title", "Body")
office_md_to_docx_bytes("# Hi\n\nPara\n")

// legacy OLE
let streams = office_ole_streams(doc)  // if .doc/.xls
```

## CLI

```bash
cd app-product/officebaga && sandak build
baga -I ../.. -I .. demo.baga text file.docx
baga -I ../.. -I .. demo.baga replace file.docx Hello Zdravei out.docx
baga -I ../.. -I .. demo.baga streams legacy.doc
baga -I ../.. -I .. demo.baga md-docx out.docx notes.md
```

## Layout

```
officebaga/
├── office.baga
├── edit/edit.baga      # set_part / replace_text
├── ole/cfb.baga        # OLE2 container
├── ir/ opc/ core/ docx/ xlsx/ odf/ convert/
└── fixtures/min.doc
```
