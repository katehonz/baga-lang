# xmlbaga

**Universal XML pull parser + writer** for Baga (apps-roadmap №11) —
event streaming in the style of Rust's `quick-xml` / `xml-rs`, no DOM.
Base package: application-specific XML formats (bank statements, invoices,
config) are built on top of it by other apps.

| | |
|--|--|
| **sandak** | `xmlbaga` **0.1.0** |
| **Layout** | `xml.baga` (parser + writer), `demo.baga` (CLI) |
| **Deps** | `std` only |
| **Plan** | [PLAN.md](PLAN.md) · [gaps.md](gaps.md) |

## Parser — pull events

```baga
import "xmlbaga/xml.baga"

let p = xml_parser(src)
let mut done = 0
while done == 0 {
    let ev = xml_next(p)
    if ev.kind == 0 { done = 1 }          // EOF
    if ev.kind == 1 { /* OPEN:  ev.name, ev.attrs (Map<str,str>) */ }
    if ev.kind == 2 { /* CLOSE: ev.name */ }
    if ev.kind == 3 { /* TEXT:  ev.text (entities decoded) */ }
    if ev.kind == 0 - 1 { /* ERROR: ev.err (with byte offset) */ }
}
```

- Event kinds: `0` EOF, `1` OPEN, `2` CLOSE, `3` TEXT, `-1` ERROR.
- Self-closing `<a/>` emits OPEN then CLOSE.
- Comments, processing instructions and the XML declaration are skipped;
  CDATA arrives as raw TEXT; DOCTYPE is skipped leniently (X3).
- Well-formedness is enforced: mismatched/unclosed tags, multiple roots,
  text outside the root, duplicate attributes, `<` in attribute values,
  unknown entities, invalid char refs — all produce ERROR events.
- Entities: the builtin five (`&lt; &gt; &amp; &apos; &quot;`) plus
  numeric char refs (`&#65;`, `&#x41;` — decoded as UTF-8).
- Names are raw — no namespace resolution (X1); use a suffix/local-name
  match at the application level when needed.

## Writer

```baga
let attrs = xml_attrs_new()
map_set(attrs, "номер", "123")
xml_decl()                                  // <?xml version="1.0" encoding="UTF-8"?>
xml_elem("фактура", attrs, "27.26 & <tax>") // <фактура номер="123">27.26 &amp; &lt;tax&gt;</фактура>
```

`xml_open` / `xml_open_empty` / `xml_close` / `xml_text` / `xml_elem` /
`xml_decl`; text and attribute escaping; **attributes serialize in sorted
key order** (deterministic output — `map_keys` order is hash order).
Writer → parser round-trips cleanly.

## CLI

```bash
cd app-product/xmlbaga && sandak build
baga -I ../.. -I .. demo.baga file.xml              # event dump
ROUNDTRIP=1 baga -I ../.. -I .. demo.baga file.xml  # re-emit via writer
```

Tests: `tests/xml_test.baga` (35 checks — events, errors, entities,
CDATA, writer goldens, round-trip).
