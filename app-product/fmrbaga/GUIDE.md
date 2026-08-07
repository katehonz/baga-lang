# Building an app with fmrbaga

fmrbaga is a **small, usable** HTTP/JSON framework for Baga: clear layers, env
config, multi-thread workers, thin handlers, and Postgres through ormbaga. It
is designed around Baga’s constraints (no function values → route ids).

## 1. Layout (any product — apps stay universal)

```
apps/myapp/
  start.baga              # main: this app's migrate → fmr_run
  routes.baga             # fmr_build_app + fmr_dispatch
  schema.baga             # this product's MigrationSet
  models/*.baga           # table helpers over ormbaga
  actions/*.baga          # handlers returning FmrOut
  .env.example
```

fmrbaga has **no domain**. Each app owns schema + routes.
References: [`apps/api`](../../apps/api/README.md), `apps/registry`.

## 2. Config (env)

| Variable | Default | Purpose |
|----------|---------|---------|
| `PORT` | 8080 | listen |
| `FMR_WORKERS` | 4 | OS-thread pool (1 DB each); `0` = go_bg per conn |
| `FMR_SYNC` | 0 | serial accept+handle (debug; overrides workers) |
| `FMR_DRAIN_MS` | 10000 | post-SIGTERM wait for in-flight connections |
| `FMR_IDLE_S` | 15 | client socket idle timeout (keep-alive safety) |
| `FMR_LOG` | 0 | log `METHOD path → status` |
| `FMR_CORS` | | `*` or origin; empty = off |
| `FMR_JWT_SECRET` | baga-secret | JWT |
| `FMR_TITLE` / `FMR_VERSION` | | OpenAPI info |
| `PGHOST` `PGPORT` `PGUSER` `PGPASSWORD` `PGDATABASE` | local bagatest | DB |

### Choosing concurrency

| Goal | Setting |
|------|---------|
| Local / production API | default (`FMR_WORKERS=4`) or `8` / `16` |
| Max concurrency debug | `FMR_WORKERS=0` (go_bg; expensive SCRAM per conn) |
| Single-thread debug | `FMR_SYNC=1` |
| Faster SIGTERM in tests | `FMR_DRAIN_MS=1000 FMR_IDLE_S=2` |

## 3. Handler pattern

```baga
fn act_notes_index(ctx: FmrCtx, db: OrmDb) -> FmrOut !IO !Net {
    let auth = act_require_auth(ctx, db)
    if auth.ok == 0 { return auth.out }
    let q = orm_query(db, "SELECT ...")?
    return act_ok(q.db, /* jobj */)
}
```

Helpers: `act_ok`, `act_created`, `act_err`, `act_require_auth`, `act_422_field`,
`act_require_id`, `act_page` (`core/action.baga`).

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

List endpoints support **`?limit=&offset=`**.

Graceful stop: `kill -TERM <pid>` → `/ready` becomes 503, accept stops, inflight
drains, workers join, process exits.

## 6. What we skip on purpose

- HTML pages — JSON API first
- ORM codegen — explicit table helpers in Baga
- Huge middleware stacks — built-in correlation chain + `fmr_before` is enough

Grow only when a real product needs it.

## Identity

fmrbaga is **itself**: route-id dispatch, jsonx validation, worker-per-DB
sessions, OpenAPI from the live router. Packages stay universal; products live
under `apps/*`.
