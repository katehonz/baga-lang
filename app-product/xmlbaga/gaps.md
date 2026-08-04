# xmlbaga — language & product gaps

Probe log for the XML package (apps-roadmap №11).

## X1 — no namespace resolution

**Symptom.** `<ns:Ntry>` comes through with the raw name `ns:Ntry`;
`xmlns` declarations are ordinary attributes.

**Workaround.** Applications match on local names (suffix after `:`) —
bank/invoice formats use fixed namespaces, so this is tolerable.

**Severity.** Low-medium. Real ns resolution needs a prefix→URI scope
stack per element — doable on top of the event API later.

**Verdict.** Documented limitation; v0.1 keeps raw names.

## X2 — no DOM (L4: no Vec<struct>)

**Symptom.** A tree API would need `Vec<XmlNode>` — struct elements are
not supported in vectors (same L4 lineage as tplbaga P3, bagadecimal D7).

**Workaround.** Pull events; applications keep their own path stacks
(`Vec<str>`) — the same shape quick-xml users write by hand in Rust.

**Severity.** Low — pull parsing is arguably the right API for large
files anyway (constant memory).

**Verdict.** By design; revisit if L4 closes for structs.

## X3 — DOCTYPE skipped leniently

**Symptom.** `<!DOCTYPE ...>` (with an internal subset) is skipped, but
entities declared in it are NOT defined — `&custom;` is an error.

**Workaround.** The builtin five + numeric char refs cover the
overwhelming majority of machine-generated XML.

**Severity.** Low for data exchange; high for legacy DTD-heavy docs.

**Verdict.** Honest subset, documented.

## X4 — byte-lenient names

**Symptom.** Element/attribute names accept any byte ≥ 128 (UTF-8
lead/continuation) without validating the full XML Name production.

**Workaround.** Well-formed docs from real systems parse fine; garbage
still fails structurally (no name chars at all → error).

**Severity.** Low.

**Verdict.** Accept (lenient where the strictness costs nothing user-visible).

## X5 — per-char concat in text decoding (G1 lineage)

**Symptom.** `xml_decode` builds text with per-byte `concat` — O(n²) on
long text runs.

**Severity.** Low at document sizes we target (KBs); the known G1
(string builder) would fix the class.

**Verdict.** Accept; measure if a real import gets slow.

## Closed in v0.1

- Pull parser with full well-formedness errors (mismatched/unclosed
  tags, multiple roots, text outside root, duplicate attributes, bad
  entities/char refs, unterminated markup).
- CDATA, comments, PI, XML declaration, lenient DOCTYPE skip.
- Writer with escaping + deterministic attribute order; round-trip
  verified (`tests/xml_test.baga`, 35 checks).
- Bounds-safe prefix probes (`xml_at`) — `substr` aborts out of range,
  the first draft crashed on short tails (`<a><b/></a>`).
