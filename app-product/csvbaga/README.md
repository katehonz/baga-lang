# csvbaga

**CSV** parse/stringify (RFC 4180-ish) for DB import/export and report bridges.

| | |
|--|--|
| **sandak** | `csvbaga` **0.1.0** |
| **Deps** | `std`, `bufbaga` |
| **Tests** | `tests/csv_test.baga` |

```baga
import "csvbaga/csv.baga"

let t = csv_parse("a,b\r\n1,2\r\n")
csv_get(t, 0, 0)   // "a"
csv_nrows(t)       // 2
let s = csv_stringify(t)
```

Also: `csv_from_tsv` / `csv_to_row_lines` for Excel path; `csv_read_path` / `csv_write_path`.
