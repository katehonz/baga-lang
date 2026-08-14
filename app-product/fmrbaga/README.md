# fmrbaga

**Baga web framework** — HTTP/JSON APIs with route-id dispatch, env config,
multi-thread workers, OpenAPI, JWT, and Postgres via ormbaga.

| Want | Go here |
|------|---------|
| **Architecture** | [`ARCHITECTURE.md`](ARCHITECTURE.md) |
| **Ship an API** | [`apps/api`](../../apps/api/README.md) |
| **How-to** | [`GUIDE.md`](GUIDE.md) |
| **Stack** | [`../BASE.md`](../BASE.md) |

Framework core stays in this package; **business code belongs in `apps/*`**.

## Stack

```
handlers / your app
      ↓
fmrbaga  (router, jsonx, deps, middleware, pipeline, workers)
      ↓
httpdbaga · jwtbaga · ormbaga · otelbaga · logbaga · pgbaga
      ↓
HTTP/1.1          PostgreSQL
```

## Layout

| Path | Role |
|------|------|
| `core/route.baga` | `{param}` router, integer route ids |
| `core/ctx.baga` | `FmrCtx` (request, params, JWT, req_id/trace) |
| `core/middleware.baga` | request-id + otel `traceparent` + logbaga |
| `core/respond.baga` | `fmr_json` / `fmr_error` / `fmr_422_*` |
| `core/deps.baga` | `deps_bearer`, Content-Type, token issue |
| `core/app.baga` | `FmrApp`, pipeline, HTTP serve (`fmr_run`), runtime stats |
| `core/action.baga` | thin handler helpers (auth, page, JSON out) |
| `core/config.baga` | env config |
| `json/jsonx.baga` | JSON builders + body field validation |
| `openapi/openapi.baga` | OpenAPI 3 from the route table |
| `handlers/handlers.baga` | **universal scaffold** (health/ready/meta/token/me only) |
| `examples/serve.baga` | scaffold entry (no product domain) |

Root `*.baga` files are **stable re-exports** (`import "fmrbaga/app.baga"`).

**Products** (any domain) live in `apps/*` — each owns routes, models, schema.

## Concurrency (multi-thread)

Each `go` / `go_bg` is a real **OS thread** (`pthread`). Serve modes:

| Mode | When | Threads | DB |
|------|------|---------|-----|
| **Worker pool** | `FMR_WORKERS=N` (default **4**) | 1 accept + N workers | **1 long-lived session per worker** |
| **go_bg / conn** | `FMR_WORKERS=0` | 1 accept + 1 thread per TCP conn | 1 session per connection (SCRAM each time) |
| **Sync** | `FMR_SYNC=1` | 1 (serial) | 1 per connection |

**Worker pool (recommended):**

```
accept (poll) ──chan──► worker 0  (OS thread, 1 DB)
                 ├──► worker 1
                 └──► worker N-1
```

- Work queue size ≈ `workers * 8`; full queue **blocks accept** (backpressure).
- Workers are **joinable** (`go` + `join` on shutdown).
- Client sockets get **`FMR_IDLE_S`** read/write timeouts (default 15s) so
  keep-alive does not pin workers forever.
- SIGTERM/SIGINT: stop accept → `chan_close` work queue → drain inflight
  (`FMR_DRAIN_MS`) → join workers.

```bash
# default = 4 workers
./scripts/fmr-run

# production-ish
FMR_WORKERS=8 FMR_DRAIN_MS=10000 FMR_IDLE_S=15 ./scripts/fmr-run

# debug single-threaded
FMR_SYNC=1 ./scripts/fmr-run
```

**Metrics** (`apps/api` `GET /metrics`):

| Gauge | Meaning |
|-------|---------|
| `fmr_workers` | configured pool size |
| `fmr_serve_mode` | `0`=go_bg, `1`=sync, `2`=worker_pool |
| `fmr_inflight_connections` | connections currently in handlers |
| `fmr_accepted_connections` | total accepted |
| `fmr_completed_connections` | total completed |

## Route model

Baga has no function values → routes are **integer ids**; the app implements
`fmr_dispatch(ctx, db)`, or register `fmr_route_fn(..., handler)` and skip
the switch.

```baga
fmr_route_fn(app, "GET", "/users/{id}", RID_USER_GET, h_user_get)
// or fmr_route(...) + branch in fmr_dispatch (product apps today)
// …
match rid {
    RID_USER_GET => act_user_get(ctx, db),
    // …
}
```

| Concern | fmrbaga |
|---------|---------|
| Path params / query | `fmr_param` / `fmr_query` |
| JSON body | `jbody_parse_str` + `jreq_str` / `jreq_i64` |
| Errors | `fmr_error(status, detail)` |
| Auth | `deps_bearer(ctx)` / `act_require_auth` |
| JSON response | `fmr_jobj` / `fmr_json` / `act_ok` |
| Boot (products) | own `migrate_up` then `fmr_run()` |

## Scaffold routes (`handlers/` — not a product)

| Method | Path | Auth |
|--------|------|------|
| GET | `/health` | no |
| GET | `/ready` | no (DB ping; 503 while shutting down) |
| GET | `/v1/meta` | no |
| GET | `/openapi.json` | no |
| POST | `/v1/auth/token` | no — body/query `sub` → JWT |
| GET | `/v1/me` | Bearer |

Domain CRUD belongs in **your app**. See `apps/api` and `apps/registry`.

## Run

```bash
./scripts/fmr-run   # apps/api, FMR_WORKERS=4, local bagatest/baga_orm

# scaffold only (ops + JWT, no product schema)
PORT=8080 FMR_JWT_SECRET=change-me \
  ./baga -I . -I app-product app-product/fmrbaga/serve.baga
```

```bash
curl -s localhost:8080/health
curl -s -X POST localhost:8080/v1/auth/token -H 'Content-Type: application/json' \
  -d '{"sub":"admin"}'
```

### Env

| Var | Default | Meaning |
|-----|---------|---------|
| `PORT` | 8080 | listen |
| `FMR_WORKERS` | **4** | fixed pool size; `0` = go_bg per conn |
| `FMR_SYNC` | 0 | `1` = serial (overrides workers) |
| `FMR_DRAIN_MS` | 10000 | post-SIGTERM wait for inflight ≈ 0 |
| `FMR_IDLE_S` | 15 | client socket SO_RCVTIMEO/SNDTIMEO (keep-alive) |
| `FMR_JWT_SECRET` / `JWT_SECRET` | baga-secret | HS256 |
| `FMR_LOG` | 0 | request log lines |
| `FMR_CORS` | | `*` or origin |
| `FMR_TITLE` / `FMR_VERSION` | | OpenAPI info |
| `PG*` | bagatest / baga_orm | Postgres DSN |

## Add a route

1. New `RID_*` constant in your dispatch file  
2. `fmr_route_fn(app, "METHOD", "/path/{id}", RID, handler)` in `fmr_build_app`  
   (or `fmr_route` + branch in `fmr_dispatch`)  
3. Handler uses `fmr_param` / `jreq_*` / `orm_*`

## JSON

```baga
let mut o = jobj_new()
o = jobj_str(o, "name", name)
o = jobj_i64(o, "id", id)
return fmr_out(fmr_ok_obj(o), db)

let email = jreq_str(doc, root, "email")
if email.ok == 0 {
    return fmr_out(fmr_422_jfield("email", email), db)
}
```

## Tests

```bash
./baga -I . -I app-product tests/fmr_test.baga
```

## OpenAPI 3

`GET /openapi.json` — built from the live route table (`fmr_openapi_from_router`).

## Honesty

- HTTP/1.1 only in the framework loop (h2 stays in httpdbaga).
- Runtime gauges are best-effort (shared map without mutex; fine for ops).
- OpenAPI has real schemas + Bearer security; not every error variant is listed.
