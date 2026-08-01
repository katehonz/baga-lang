# std/str — string utilities

Pure string functions built on the `len`/`char_at`/`substr`/`concat`/`chr`/`ord`
builtins.

- `str_starts_with(s: str, prefix: str) -> bool` — true if `s` begins with `prefix`.
- `str_ends_with(s: str, suffix: str) -> bool` — true if `s` ends with `suffix`.
- `str_find(s: str, needle: str) -> i64` — index of the first occurrence of `needle`, or -1.
- `str_split(s: str, delim: str) -> Vec<str>` — split on a single-character delimiter; empty fields are preserved.
- `str_join(parts: Vec<str>, sep: str) -> str` — join the parts with `sep` between them.
- `str_replace(s: str, from: str, to: str) -> str` — replace every occurrence of `from` with `to`.
- `str_trim(s: str) -> str` — trim ASCII whitespace (space, tab, LF, CR) from both ends.
- `str_repeat(s: str, n: i64) -> str` — repeat a string n times (binary doubling — O(log n) concats). The result is heap-allocated and may be overwritten in place by libc externs that take a `str` buffer.
- `parse_int(s: str) -> i64` — parse a base-10 integer; stops at the first non-digit; leading `-` allowed.
- `int_to_str(n: i64) -> str` — decimal string representation of an integer.

Effects: none (pure). Memory: leak-tolerant.
