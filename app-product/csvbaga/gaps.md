# csvbaga — gaps

## Closed 0.1.0

- Parse/stringify with quotes and `""`
- CRLF output; LF/CRLF input
- BOM strip
- Rectangle pad short rows
- Path I/O via `read_file_bytes` / `write_file_bytes`

## Open

### C1 — no streaming

Whole file in memory. Fine for reports; big imports later.

### C2 — single delimiter `,`

Semicolon / tab CSV as format option later (`csv_from_tsv` exists for tabs).
