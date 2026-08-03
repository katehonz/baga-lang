# grebaga

A **grep-like** line scanner for Baga — apps-roadmap **№9**. Literal and
mini-pattern match (`.` / `*` / `\`), streaming line read (empty line ≠ EOF),
CLI with `-n` / `-i`.

## Pattern language

| Form | Meaning |
|------|---------|
| fixed text | substring (`str_find`) |
| `.` | any one byte |
| `*` | any run of bytes (incl. empty) |
| `\x` | literal `x` (e.g. `\.` for a dot) |
| `-i` | ASCII case-fold |

Not PCRE: no classes, groups, anchors, or quantifiers beyond `*`.

## API

```baga
fn gre_match(text, pat, fold) -> bool
fn gre_count_in(src, pat, fold) -> i64
fn gre_stream_file(path) -> GreStream !IO
fn gre_stream_fd(fd) -> GreStream
fn gre_next_line(s) -> GreLine !IO      // ok=0 only at true EOF
fn gre_scan_print(s, path, pat, fold, show_path, show_num) -> i64 !IO
```

## Run

```bash
cd app-product/grebaga
BAGA=../../baga sandak build

../../baga -I ../.. -I .. demo.baga -- pattern file.txt
../../baga -I ../.. -I .. demo.baga -- -n -i Hello file.txt
cat file.txt | ../../baga -I ../.. -I .. demo.baga -- 'f*o'

# tests
./baga -I . -I app-product tests/grep_test.baga
```

Exit codes: `0` hits, `1` no match, `2` usage/open error.

## Honest limits

See [`gaps.md`](gaps.md): `read_line` empty/EOF collapse forced a custom
chunked scanner; no recursive `*` optimisations; no mmap; single-threaded.
