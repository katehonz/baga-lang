# mdbaga

A **Markdown → HTML** converter for Baga — pure recursive-descent style
block + inline parser. Apps-roadmap **№4**: the first large parser outside
the compiler; probes string building and line-oriented I/O.

## What works

| Capability | Notes |
|------------|--------|
| ATX headings | `#` … `######` |
| Paragraphs | soft-wrapped lines joined with space |
| Emphasis | `*italic*`, `**bold**` |
| Code | inline `` `code` ``, fenced ` ```lang ` blocks |
| Lists | unordered `-`/`*`/`+`, ordered `1.` |
| Quotes | `>` (body re-parsed as markdown) |
| HR | `---` / `***` / `___` |
| Links | `[text](url)` |
| Escape | `& < > "` in text and code |

## API

```baga
fn md_to_html(src: str) -> str           // HTML fragment
fn md_to_document(src: str, title: str) -> str  // full <!DOCTYPE html>…
fn md_esc(s: str) -> str                 // HTML escape
fn md_inline(s: str) -> str              // inline-only
```

## Run

Package imports per language §18.1 — `sandak` computes `-I` from `sandak.toml`.

```bash
cd app-product/mdbaga
BAGA=../../baga sandak build

# fragment to stdout
../../baga -I ../.. -I .. demo.baga README.md

# full document
MDDOC=1 ../../baga -I ../.. -I .. demo.baga README.md > /tmp/out.html

# unit tests (repo root)
./baga -I . -I app-product tests/md_test.baga
```

## Honest limits

See [`gaps.md`](gaps.md): no tables, no nested lists, no images `![…](…)`,
no reference links, no autolinks, no setext headings. Nested `concat` remains
the main friction for large HTML (interpolation `${}` helps locals only).
