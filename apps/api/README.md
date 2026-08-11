# apps/api — reference product on fmrbaga

One **independent product** using the universal stack (fmrbaga + ormbaga +
pgbaga, or **boilaDB** via boilabaga when `ORM_BACKEND=boila`). Copy this
layout for any domain — the framework does not assume users/posts or this app.

```
apps/api/
  start.baga           # main: this app's migrate → fmr_run
  routes.baga          # fmr_build_app + fmr_dispatch
  schema.baga          # this product's MigrationSet (owned here)
  models/              # table helpers
  actions/             # handlers
  .env.example
```

Other products (e.g. `apps/registry`) use the same pattern with their own
schema and routes. Packages under `app-product/` stay universal.

## Prerequisites

1. **Toolchain:** `make` → `./baga` in repo root  
2. **Database** — either:
   - **Postgres** (default), or  
   - **boilaDB** PG wire (`serve_pg` on `:6575`) with `ORM_BACKEND=boila`
3. Role/db once for Postgres (example):

```bash
createuser -h 127.0.0.1 -P bagatest   # password e.g. pas+123
createdb  -h 127.0.0.1 -O bagatest baga_orm
```

## Run

```bash
./scripts/fmr-run
# defaults: PORT=8080, FMR_WORKERS=4, ORM_BACKEND=pg, bagatest@baga_orm
```

Or:

```bash
export PORT=8080 FMR_WORKERS=4 FMR_LOG=1 FMR_CORS='*' FMR_JWT_SECRET=dev-secret
export PGHOST=127.0.0.1 PGPORT=5432 PGUSER=bagatest PGPASSWORD='pas+123' PGDATABASE=baga_orm
./baga -I . -I app-product apps/api/start.baga
```

### Against boilaDB

```bash
# terminal 1
./baga -I . -I app-product app-product/boilaDB/tools/serve_pg.baga
# terminal 2
ORM_BACKEND=boila ./scripts/fmr-run
```

Migrate on boot uses `api_migrations_auto()` — Postgres SERIAL/FK set, or
BoilaSQL set (`api_boila_migrations`) when `ORM_BACKEND=boila`.

## Smoke (after listen)

```bash
curl -s localhost:8080/health
curl -s localhost:8080/ready
TOKEN=$(curl -s -X POST localhost:8080/v1/auth/token -H 'Content-Type: application/json' \
  -d '{"sub":"ada"}' | sed 's/.*"access_token":"//;s/".*//')
curl -s localhost:8080/v1/users -H "Authorization: Bearer $TOKEN"
```

**Full product path:** [docs/runbooks/product-path.md](../../docs/runbooks/product-path.md)

Automated: `tests/api_test.baga` (needs same PG defaults).

## How to add a route (any product)

1. Free route id in `routes.baga`
2. `fmr_route(app, "GET", "/v1/things", RID_THINGS)`
3. Handler in `actions/…` → `FmrOut`
4. Branch in `fmr_dispatch`

Or: `./scripts/fmr-gen-action things index GET /v1/things`

## Config (env)

| Var | Default (fmr-run) | Role |
|-----|-------------------|------|
| `PORT` | `8080` | listen |
| `FMR_WORKERS` | `4` | OS-thread pool (1 DB each); try `8` in prod |
| `FMR_DRAIN_MS` | `10000` | graceful drain after SIGTERM |
| `FMR_IDLE_S` | `15` | client socket idle timeout |
| `FMR_LOG` | `1` | request logs |
| `FMR_CORS` | `*` | CORS origin |
| `FMR_JWT_SECRET` | `dev-secret` | JWT |
| `ORM_BACKEND` | `pg` | `pg` or `boila` (boilabaga → boilaDB) |
| `PG*` | bagatest / baga_orm | Postgres DSN |
| `BOILA_PG*` / `BOILA_TOKEN` | `:6575` / `boila` | used when `ORM_BACKEND=boila` |

See [fmrbaga README](../../app-product/fmrbaga/README.md) for concurrency modes
and `/metrics` gauges (`fmr_workers`, `fmr_inflight_connections`, …).
