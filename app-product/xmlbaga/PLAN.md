# xmlbaga — PLAN

apps-roadmap №11: universal XML package — a base building block, not an
application format. Format-specific importers (bank camt.053, invoices,
config files) live in their own packages on top of this one.

## v0.1 (shipped 2026-08-04)

- Pull parser, quick-xml style: OPEN / CLOSE / TEXT / EOF / ERROR events,
  cursor state in reference types (`Vec`/`Map` fields) so `XmlParser`
  values stay copyable.
- Well-formedness: mismatched/unclosed tags, multiple roots, text outside
  root, duplicate attributes, `<` in attribute values, unknown entities,
  invalid/control char refs, unterminated comment/CDATA/PI/tag/DOCTYPE.
- Entities: builtin five + numeric char refs (UTF-8 decoded).
- CDATA as raw TEXT; comments/PI/declaration skipped; lenient DOCTYPE skip.
- Writer: escaping, sorted (deterministic) attributes, `xml_elem`
  convenience, declaration helper; writer → parser round-trip in tests.
- CLI `demo.baga`: event dump + ROUNDTRIP mode.

## Later (only when a real app asks)

- Namespace scope stack + local-name API (X1).
- DOM/tree convenience layer if L4 (struct elements) ever lands (X2).
- DOCTYPE entity definitions (X3) — only for legacy DTD documents.
- Streaming from a socket/file without `read_file` of the whole input
  (needs a chunked reader idiom; the parser is already cursor-based).
