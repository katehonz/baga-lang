# grebaga — grep-like CLI (plan)

Date: 2026-08-04
Status: P0 done
Goal: apps-roadmap **№9** — streaming lines, args, pattern match.

## Phases

### P0 ✅

1. `match.baga` — literal + `.`/`*`/`\` + ASCII `-i`.
2. `scan.baga` — chunked line stream (empty line ≠ EOF).
3. `demo.baga` — CLI flags, files or stdin, exit codes.
4. `tests/grep_test.baga` — pure match + live file stream.

### P1

- `-v` invert, `-c` count only, `-l` files-with-matches.
- Multi-file path prefix always when >1 file (done).
- Faster `*` (no full backtrack).

### P2

- mmap / larger buffers; UTF-8 aware `.` (codepoint).
- Real regex if a library lands in std.

## Success criteria (P0) — met

1. `sandak build` grebaga OK.
2. `grep_test` all passed.
3. CLI finds lines with `-n`/`-i`/meta patterns.
