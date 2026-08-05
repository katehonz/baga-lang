# flagbaga

**Typed CLI flags** over `arg()` (Track **C7**).

```baga
// CLI: prog --port 8080 --verbose --name=baga file.txt
let f = flags_parse()
let port = flags_i64(f, "port", 8080)
let verbose = flags_bool(f, "verbose")
let name = flags_str(f, "name", "default")
let file = flags_positional(f, 0)
```

Forms: `--name value`, `--name=value`, bare `--flag` → bool true.
Positionals are non-`--` tokens. **Tip:** put positionals before bare
bool flags, or write `--verbose=true`, so a path is not eaten as a value.

Testing without a process CLI: `flags_parse_vec(args)`.
