# officebaga — gaps

## Closed (0.2.0)

| Item |
|------|
| ZIP packages: extract/create/edit for docx/xlsx/odt/ods |
| md→docx, markdown export, IR |
| `office_replace_text` / `office_set_part` / `office_save` (package rebuild) |
| OLE2/CFB open + stream list + naive ASCII text from WordDocument/Workbook |
| PPTX rejected |

## Open

### O3 — edit is rebuild, not true in-place

Replaces one ZIP part and re-writes the archive (other parts preserved as
bytes). Not a delta to the original local headers.

### O8 — replace_text is raw XML substring

Must match a contiguous string inside the main part (often one `w:t` run).
Cross-run replacements fail.

### O10 — OLE text is not Word/Excel fidelity

No FIB/piece table / BIFF parse — printable ASCII runs only. Real .doc
layout/unicode incomplete.

### O11 — mini streams in CFB

Streams smaller than mini cutoff that live in the mini stream may not read
correctly yet (FAT-only path preferred).

### O4 — formulas / styles / headers

Still out of scope.
