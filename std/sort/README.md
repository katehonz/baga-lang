# std/sort — sorting and searching Vec<i64>

- `sort_i64(v: Vec<i64>)` — in-place quicksort, ascending.
- `binary_search_i64(v: Vec<i64>, x: i64) -> i64` — index of `x` in a sorted vector, or -1.
- `qs_partition` and `qs_sort` are internal helpers of `sort_i64`.

Effects: none (pure). Memory: leak-tolerant.
