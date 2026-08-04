# tplbaga

**HTML template engine** for Baga — apps-roadmap **№7**. Mustache-ish
subset: escaped interpolation, raw interpolation, nestable `{% if %}`
blocks, comments, and filter chains. Filters dispatch by name (no L5
function values yet — this package is the designated closures probe).

## Syntax

| Tag | Meaning |
|-----|---------|
| `{{ name }}` | interpolation, HTML-escaped; missing → `""` |
| `{{ name \| trim \| upper }}` | filter chain |
| `{{{ name }}}` | raw interpolation (no escape) |
| `{% if name %}` … `{% else %}` … `{% endif %}` | nestable truthiness blocks |
| `{% if !name %}` | negation |
| `{# comment #}` | dropped |

Context is `Map<str, str>`. Truthy = present, non-empty, not `"0"`.
Filters: `upper`, `lower` (ASCII), `trim`, `len`, `default:fallback`
(only `default` reads the `:arg`).

## API

```baga
struct TplOut { ok: i64, html: str, err: str }   // L3 Result stand-in

fn tpl_render(src: str, ctx: Map<str, str>) -> TplOut   // pure
fn tpl_escape(s: str) -> str                            // & < > "
```

Internals: the source is tokenized into prefix-encoded `Vec<str>`
("T"ext / "V"ar / "R"aw / "I"f / "E"lse / endif "X"), blocks are paired
into a `Map<i64,i64>` jump table, and the render is one iterative walk —
no recursion, no `Vec<struct>` (L4).

## Run

```bash
cd app-product/tplbaga
BAGA=../../baga sandak build
../../baga -I ../.. -I .. demo.baga page.tpl data.kv

# demo data.kv: one "key=value" per line ('#' comments, blank lines skipped)

./baga -I . -I app-product tests/tpl_test.baga
```

Exit codes (demo): 0 rendered, 1 render/read error, 2 usage.

## Honest limits

See [`gaps.md`](gaps.md): filters are a name switch (L5); `TplOut` is the
L3 Result stand-in; tokens are prefix-encoded strings (no `Vec<struct>`,
L4); demo cannot tell a missing template file from an empty one (M2).
