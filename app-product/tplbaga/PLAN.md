# tplbaga — HTML template engine (plan)

Date: 2026-08-04
Status: P0 done (vars + raw + if/else/endif + filters + CLI)
Goal: apps-roadmap **№7** — interpolation on steroids; the L5 probe.

## Phases

### P0 ✅

1. Tokenizer: `{{ }}` / `{{{ }}}` / `{% if|else|endif %}` / `{# #}`.
2. Jump-table block pairing (`Map<i64,i64>`); single iterative walk.
3. Filters by name switch: upper/lower/trim/len/default:arg (L5 stand-in).
4. `TplOut` ok/err struct (L3 stand-in); `tests/tpl_test.baga` (46 checks).
5. `demo.baga` CLI: template file + `key=value` data file, exit 0/1/2.

### P1

- `{% for %}` over collections — blocked by L4 (`Vec<struct>` / structs in
  collections); today a "list" is only `{{ items }}` count + raw HTML.
- Partials / `{% include %}` — needs readdir (testbaga T2) or a
  name→template `Map<str,str>` context.
- Dotted names (`user.name` → nested contexts).
- Filter args for more than `default` (`truncate:n`, `wrap:tag`).

### P2

- Migrate `TplOut` → language `Result`/`enum` when L3 lands.
- Registered filters/helpers once L5 lands: `tpl_filter("upper", fn)` —
  the roadmap's point of №7.

## Success criteria (P0) — met

1. `sandak build` tplbaga OK.
2. `tpl_test` all passed (escape, jump table, filters, error paths).
3. `demo.baga` renders template + data file to stdout with honest exits.
