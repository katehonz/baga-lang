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

Forms: `--name value`, `--name=value`, `-p` / `-p=value`, bare `--flag`
→ bool true. Bare `--` ends flags (rest is positional).

`FlagDef` + `flags_parse_defs`: short names map to long; bool flags do
not consume the next token. `flags_usage(prog, defs)` prints help.

**Tip** without defs: put positionals before bare bool flags, or write
`--verbose=true`.

Testing without a process CLI: `flags_parse_vec(args)`.
