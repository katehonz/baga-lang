# mdbaga — Markdown → HTML (plan)

Date: 2026-08-04
Status: P0 done (subset + live unit tests + CLI demo)
Goal: apps-roadmap **№4** — large pure parser; string-building probe.

## Phases

### P0 — useful subset (this iteration) ✅

1. Block parser: headings, para, fence, hr, ul/ol, blockquote.
2. Inline: escape, code, strong, em, links.
3. `md_to_html` / `md_to_document`; CLI `demo.baga` via `arg` + `read_file`.
4. `tests/md_test.baga` (~20 checks); `sandak.toml` + package imports.

### P1 — CommonMark pressure

- Nested lists, tight/loose list items.
- Images, reference links, autolinks.
- Setext headings, indented code blocks.
- Soft vs hard line breaks.

### P2 — product polish

- Streaming line reader for multi-MB files (today: full `read_file`).
- Optional HTML sanitizer policy.
- Tables (GFM) if a real doc pipeline needs them.

## Non-goals (P0)

Full CommonMark, GFM tables, raw HTML pass-through, math, footnotes.

## Success criteria (P0) — met

1. `sandak build` on mdbaga succeeds.
2. `tests/md_test.baga` → `md_test: all passed`.
3. CLI turns a `.md` file into HTML on stdout.
