# flagbaga — gaps

## F1 — no short flags (`-p`)

Only `--long` form. Short opts are a small extension.

## F2 — no `--` end-of-flags

Everything after a bare `--` should be positional; not implemented.

## F3 — no auto `--help` text

Callers print usage; a `flags_usage(defs)` helper is P1.
