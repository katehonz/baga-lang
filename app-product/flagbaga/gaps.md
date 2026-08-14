# flagbaga — gaps

## F1 — ~~no short flags (`-p`)~~ — closed

`-p`, `-p=value`, `-p value` (and `--long`). With `FlagDef`, short names
canonicalize to the long name.

## F2 — ~~no `--` end-of-flags~~ — closed

Bare `--` stops flag parse; everything after is positional.

## F3 — ~~no auto `--help` text~~ — closed

`flags_usage(prog, defs)` + `flags_parse_defs` (bool defs do not eat the
next token).
