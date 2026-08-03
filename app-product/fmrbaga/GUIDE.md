# Building an app with fmrbaga (Lucky-style)

fmrbaga is intentionally **small but usable** — like Crystal’s Lucky: clear
folders, env config, actions that return JSON, models that talk to Postgres.

## 1. Layout

```
apps/myapp/
  start.baga              # main
  routes.baga             # fmr_build_app + fmr_dispatch
  models/*.baga
  actions/*.baga
  .env.example
```

Reference implementation: [`apps/api`](../../apps/api/README.md).

## 2. Config (env)

| Variable | Default | Purpose |
|----------|---------|---------|
| `PORT` | 8080 | listen |
| `FMR_WORKERS` | 0 | fixed worker pool (recommended ≥4) |
| `FMR_SYNC` | 0 | serial accept |
| `FMR_LOG` | 0 | log `METHOD path → status` |
| `FMR_CORS` | | `*` or origin; empty = off |
| `FMR_JWT_SECRET` | baga-secret | JWT |
| `FMR_TITLE` / `FMR_VERSION` | | OpenAPI info |
| `PGHOST` `PGPORT` `PGUSER` `PGPASSWORD` `PGDATABASE` | local bagatest | DB |

## 3. Action pattern

```baga
fn act_notes_index(ctx: FmrCtx, db: OrmDb) -> FmrOut !IO !Net {
    let auth = act_require_auth(ctx, db)
    if auth.ok == 0 { return auth.out }
    let q = orm_query(db, "SELECT ...")?
    return act_ok(q.db, /* jobj */)
}
```

Helpers: `act_ok`, `act_created`, `act_err`, `act_require_auth`, `act_422_field`
(`action.baga`).

## 4. Generate a stub

```bash
./scripts/fmr-gen-action notes index GET /v1/notes
# then wire RID + fmr_route + fmr_dispatch (printed by the script)
```

## 5. Run

```bash
./scripts/fmr-run
# defaults: FMR_WORKERS=4 FMR_LOG=1 FMR_CORS=* bagatest/baga_orm
```

Response headers include **`X-Request-Id`** (client may send one; else random hex).

List endpoints support **`?limit=&offset=`** (posts also `?user_id=`).

## 6. What we skip on purpose

- HTML pages (Lucky full-stack) — JSON API first
- Codegen ORM — explicit models in Baga
- Huge middleware stacks — `fmr_before` + `deps_*` is enough

Grow only when a real product needs it.
