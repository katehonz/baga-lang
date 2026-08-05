# fmrbaga

**Small web framework for Baga** — FastAPI-shaped HTTP/JSON, Lucky-shaped **app
layout**. Built to be *used*, not demoed: env config, workers, OpenAPI, JWT,
Postgres via ormbaga.

| Want | Go here |
|------|---------|
| **Ship an API** | [`apps/api`](../../apps/api/README.md) — product template |
| **How-to** | [`GUIDE.md`](GUIDE.md) |
| **Stack** | [`../BASE.md`](../BASE.md) |

Framework core stays in this folder; **business code belongs in `apps/*`**.

## Stack

```
handlers / your app
      ↓
fmrbaga  (router, jsonx, deps, middleware, pipeline)
      ↓
httpdbaga · jwtbaga · ormbaga · otelbaga · logbaga · pgbaga
      ↓
HTTP/1.1          PostgreSQL
```

## Layout

| File | Role |
|------|------|
| `jsonx.baga` | JSON object/array builders + body field validation |
| `route.baga` | `{param}` router |
| `ctx.baga` | `FmrCtx` (request, params, JWT, user, req_id/trace) |
| `middleware.baga` | request-id + otel `traceparent` + logbaga (B2.1) |
| `respond.baga` | `fmr_json` / `fmr_error` / `fmr_422_*` |
| `deps.baga` | `deps_bearer`, Content-Type, `fmr_issue_token` |
| `app.baga` | `FmrApp`, pipeline, HTTP serve (`fmr_run`) |
| `handlers.baga` | production `/v1` routes + `fmr_dispatch` |
| `serve.baga` | **entrypoint**: migrate → listen |
| `PLAN.md` | design notes |

## FastAPI → fmrbaga

| FastAPI | fmrbaga |
|---------|---------|
| `@app.get("/users/{id}")` | `fmr_route(app, "GET", "/users/{id}", RID)` + match in `fmr_dispatch` |
| `Path` / `Query` | `fmr_param` / `fmr_query` |
| Pydantic body | `jbody_parse_str` + `jreq_str` / `jreq_i64` |
| `HTTPException` | `fmr_error(status, detail)` |
| `Depends(oauth2)` | `deps_bearer(ctx)` |
| `JSONResponse` | `fmr_jobj` / `fmr_json` |
| lifespan | `fmr_startup_migrate()` then `fmr_run()` |

Baga has no function values → routes are **integer ids**; the app implements
`fmr_dispatch(ctx, db)`.

## Production API (`handlers.baga`)

| Method | Path | Auth |
|--------|------|------|
| GET | `/health` | no |
| GET | `/ready` | no (DB ping) |
| GET | `/v1/meta` | no |
| POST | `/v1/auth/token` | no — body/query `sub` → JWT |
| GET | `/v1/me` | Bearer |
| GET/POST | `/v1/users` | Bearer |
| GET/PATCH/DELETE | `/v1/users/{id}` | Bearer |

## Run

```bash
# DB once (see ormbaga README): baga_orm + bagatest

PORT=8080 FMR_JWT_SECRET=change-me FMR_SYNC=1 \
  PGHOST=127.0.0.1 PGUSER=bagatest PGPASSWORD='pas+123' PGDATABASE=baga_orm \
  ./baga app-product/fmrbaga/serve.baga
```

Or emit a binary:

```bash
./baga --emit-c app-product/fmrbaga/serve.baga > /tmp/fmr.c
gcc -O2 -Iinclude -o /tmp/fmr /tmp/fmr.c -lm -pthread
PORT=8080 FMR_SYNC=1 /tmp/fmr
```

```bash
curl -s localhost:8080/health
curl -s -X POST localhost:8080/v1/auth/token -H 'Content-Type: application/json' \
  -d '{"sub":"admin"}'
curl -s localhost:8080/v1/users -H "Authorization: Bearer $TOKEN"
```

Env:

| Var | Meaning |
|-----|---------|
| `PORT` | listen port (default 8080) |
| `FMR_JWT_SECRET` / `JWT_SECRET` | HS256 secret |
| `FMR_SYNC=1` | single-threaded accept (no workers) |
| `FMR_WORKERS=N` | **fixed worker pool** (N OS threads, **1 DB each**); preferred production mode |
| `PG*` | Postgres DSN |

```bash
# production-ish: 8 workers × 1 pooled DB session each
FMR_WORKERS=8 PORT=8080 ./baga app-product/fmrbaga/serve.baga
```

## Add a route

1. New `RID_*` constant in `handlers.baga`
2. `fmr_route(app, "METHOD", "/path/{id}", RID)` in `fmr_build_app`
3. Branch in `fmr_dispatch` → handler using `fmr_param` / `jreq_*` / `orm_*`

## JSON extras

```baga
let mut o = jobj_new()
o = jobj_str(o, "name", name)
o = jobj_i64(o, "id", id)
return fmr_out(fmr_ok_obj(o), db)

// validation → FastAPI-like 422
let email = jreq_str(doc, root, "email")
if email.ok == 0 {
    return fmr_out(fmr_422_jfield("email", email), db)
}
```

## Tests

```bash
./baga tests/fmr_test.baga    # pure: jsonx, router, validation, JWT issue
```

Wired into `make test`.

## Language fixes shipped with this product

1. **`FNS_MAX` 256 → 1024** (`src/checker.c`) — frameworks exceed 256 symbols; overflow was a silent drop of functions.
2. **`tcp` `poke8` uses `byte_chr`** — `chr()` UTF-8-encoded high bytes and broke `bind()` for ports like 8080/18080.

## OpenAPI 3 (schemas + security)

`GET /openapi.json` — full-ish OpenAPI 3.0.3 document (`openapi.baga`):

| Section | Contents |
|---------|----------|
| `components.schemas` | `User`, `UserCreate`, `UserPatch`, `UserList`, `TokenRequest`, `TokenResponse`, `Me`, `Error`, `HTTPValidationError`, … |
| `components.securitySchemes` | `bearerAuth` (HTTP Bearer JWT) |
| `paths` | summaries, tags, `requestBody`, path/query params, response `$ref`s |
| per-op `security` | required on `/v1/me` and `/v1/users*` |

```bash
curl -s localhost:8080/openapi.json | jq '.components.securitySchemes'
# Swagger UI / Redoc can point at that URL
```

## Honesty

- HTTP/1.1 only in the framework loop (h2 stays in httpdbaga).
- DB: `FMR_WORKERS=N` → N long-lived sessions; else one DB per HTTP connection.
  Shared `OrmPool` (`ormbaga/pool.baga`) for sync/batch use.
- User CRUD uses **parameterized** ormbaga/pgbaga Extended Query.
- OpenAPI has real schemas + Bearer security; not every error variant is exhaustively listed.
