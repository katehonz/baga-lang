# apps/api — product on fmrbaga (stabilize runbook)

A **small, real** JSON API in the spirit of [Lucky](https://luckyframework.org/)
(Crystal): conventions, thin actions, models, one entrypoint.

Not a toy demo — the same layout you should use for your own product.

```
apps/api/
  start.baga           # main: migrate → listen
  routes.baga          # fmr_build_app + fmr_dispatch (route table)
  models/user.baga     # table helpers (Lucky model vibe)
  actions/
    system.baga        # health, ready, meta, openapi
    auth.baga          # token, me
    users.baga         # CRUD
    posts.baga
  .env.example
```

## Prerequisites

1. **Toolchain:** `make` → `./baga` in repo root  
2. **Postgres** listening (defaults below)  
3. Role/db once (example):

```bash
# as superuser
createuser -h 127.0.0.1 -P bagatest   # password e.g. pas+123
createdb  -h 127.0.0.1 -O bagatest baga_orm
```

## Run

```bash
./scripts/fmr-run
# defaults: PORT=8080, FMR_WORKERS=4, PGDATABASE=baga_orm, bagatest/pas+123
```

Or:

```bash
export PORT=8080 FMR_WORKERS=4 FMR_LOG=1 FMR_CORS='*' FMR_JWT_SECRET=dev-secret
export PGHOST=127.0.0.1 PGPORT=5432 PGUSER=bagatest PGPASSWORD='pas+123' PGDATABASE=baga_orm
./baga -I . -I app-product apps/api/start.baga
```

Migrate runs on boot (`migrate_up`). Re-run after schema changes.

## Smoke (after listen)

```bash
curl -s localhost:8080/health
curl -s -D- localhost:8080/health -o /dev/null | grep -i request-id
TOKEN=$(curl -s -X POST localhost:8080/v1/auth/token -H 'Content-Type: application/json' \
  -d '{"sub":"ada"}' | sed 's/.*"access_token":"//;s/".*//')
curl -s localhost:8080/v1/users -H "Authorization: Bearer $TOKEN"
curl -s -X POST localhost:8080/v1/posts -H "Authorization: Bearer $TOKEN" \
  -H 'Content-Type: application/json' \
  -d '{"user_id":1,"title":"hi","body":"from fmr"}'
curl -s 'localhost:8080/v1/posts?limit=10&offset=0' -H "Authorization: Bearer $TOKEN"
```

Automated: `tests/api_test.baga` (needs same PG defaults).

## How to add an action (Lucky-style)

1. Pick a free route id in `routes.baga`
2. Register `fmr_route(app, "GET", "/v1/things", RID_THINGS)`
3. Implement `act_things_index(ctx, db) -> FmrOut` in `actions/…`
4. Branch in `fmr_dispatch`

Or: `./scripts/fmr-gen-action things index GET /v1/things`

## Config (env)

| Var | Default (fmr-run) | Role |
|-----|-------------------|------|
| `PORT` | `8080` | listen |
| `FMR_WORKERS` | `4` | accept workers |
| `FMR_LOG` | `1` | request logs |
| `FMR_CORS` | `*` | CORS |
| `FMR_JWT_SECRET` | `dev-secret` | HS256 |
| `PGHOST` / `PGPORT` | `127.0.0.1` / `5432` | Postgres |
| `PGUSER` / `PGPASSWORD` | `bagatest` / `pas+123` | auth |
| `PGDATABASE` | `baga_orm` | app DB |

Full framework notes: `app-product/fmrbaga` README.

## Stabilize notes

- Prefer **not** running registry/api on the same `PORT` simultaneously.  
  Registry live tests force `PORT=8090` (`scripts/run_tests.sh`).  
- After query work in raw pgbaga, rebind with `pg_conn_of(r)` (L3 `PgResult`).  
  ORM still uses `db = r.db` / `ok` fields (B1 L3 deferred).  
- Plan focus: [Phase Stabilize](../../docs/superpowers/plans/2026-08-05-advanced-go-rust.md).

## Framework vs app

| Package | Role |
|---------|------|
| `app-product/fmrbaga` | framework (router, JSON, auth, serve) |
| `apps/api` | **your product** (actions + models + routes) |

Keep business logic in `apps/*`, not inside fmrbaga.
