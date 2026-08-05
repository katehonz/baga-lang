# globbaga

Simple **glob** matching for KEYS, routing, and file patterns.

```baga
glob_match("user:*", "user:42")     // 1
glob_match("?.baga", "a.baga")      // 1
glob_filter("k*", keys)             // Vec of matches
```

`*` any run, `?` one byte. No `**`, no `[a-z]`.
