# fmrbaga — FastAPI-style web framework for Baga

Date: 2026-08-03  
Status: **CANONICAL BASE** for Baga web/API products (not a demo)  
Stack: `httpdbaga` + `std/json` + `jwtbaga` + `ormbaga`/`pgbaga`

## Decision (locked)

**fmrbaga = the application foundation.** New HTTP/JSON/DB products build *on*
this stack rather than inventing parallel frameworks.

Why this base (vs reinventing):

| Choice | Why it wins for Baga today |
|--------|----------------------------|
| FastAPI-style API surface | JSON-first, path params, 422 validation, Bearer deps — maps cleanly without function values |
| ActiveRecord + goose (ormbaga) | Explicit migrations + simple CRUD; Diesel/Prisma need type machinery we lack |
| Native pg wire (pgbaga) | No C libpq; SCRAM + Simple Query; Extended Query later |
| httpdbaga transport | Keep-alive H1 (+ h2 available); already probed hard |

What is *not* the base: ad-hoc `server.baga` demos, raw `pg_query` in handlers,
hand-concatenated JSON without `jsonx`.

## Goals

Ship a **real** application framework that teams can build APIs on:

1. **Routing** with path parameters (`/users/{id}`) and methods  
2. **JSON-first** request/response (builders, body parse, field validation → 422)  
3. **Dependencies** (Bearer JWT, DB session) in FastAPI spirit  
4. **Uniform errors** (`detail` envelope, HTTP status)  
5. **HTTP/1.1 serve** with keep-alive; one DB connection per TCP connection  
6. **Production entrypoint** (`serve.baga` + `handlers.baga`) — versioned `/v1` API  

Non-goals (v1): OpenAPI codegen, full DI graph, HTTP/2 in framework (httpdbaga has h2 separately), connection pool.

## Why FastAPI as the model

| FastAPI concept | fmrbaga |
|-----------------|---------|
| `@app.get("/x/{id}")` | `router_add(app, "GET", "/x/{id}", RID)` + `fmr_dispatch` match |
| `Path` / `Query` | `fmr_path` / `fmr_query` on `FmrCtx` |
| Pydantic body | `jbody_*` extractors + `jobj`/`jarr` builders |
| `HTTPException` | `fmr_error(status, detail)` |
| `Depends()` | `deps_bearer` / `deps_db` helpers called in handlers |
| `JSONResponse` | `fmr_json` / `fmr_jobj` |
| Middleware | `fmr_before` hook (auth, logging, CORS headers) |
| Lifespan | `fmr_startup` / config from env at boot |

Baga has **no function values** → routes are **ids**; the app implements one
`fmr_dispatch(ctx) -> Response` (same link-time pattern as `h2_route`).

## Package layout

```
fmrbaga/
  PLAN.md README.md gaps.md
  jsonx.baga      # JSON object/array builders + body validation (pure)
  route.baga      # Router + {param} matching (pure)
  ctx.baga        # FmrCtx, path/query accessors
  respond.baga    # JSON responses, error envelopes, status helpers
  deps.baga       # Bearer JWT, Content-Type checks
  app.baga        # FmrApp, route table, request pipeline, HTTP serve
  handlers.baga   # production /v1 handlers (dispatch)
  serve.baga      # main() production server
```

## Request pipeline

```
accept → (optional peek)
  → http_read_request_r
  → fmr_before(ctx)            # global hook; may short-circuit
  → router_match(method, path)
  → build FmrCtx (params, app config, db handle)
  → fmr_dispatch(ctx)          # app handlers
  → ensure error envelope if panic-equivalent (ok flag)
  → http_respond_keepalive
```

## JSON extras (`jsonx`)

- **Builders:** `jobj_*` / `jarr_*` → encode without hand-concat  
- **Body:** `jbody_parse(req)` → `JsonDoc`  
- **Required fields:** `jreq_str` / `jreq_i64` / `jreq_bool` → error loc for 422  
- **Optional:** `jopt_str` with default  
- **FastAPI-like 422:** `{"detail":[{"loc":["body","email"],"msg":"...","type":"..."}]}`

## Production API surface (handlers)

| Method | Path | Auth | Action |
|--------|------|------|--------|
| GET | `/health` | no | liveness |
| GET | `/ready` | no | readiness (DB ping) |
| POST | `/v1/auth/token` | no | issue JWT `{sub}` |
| GET | `/v1/me` | Bearer | current subject |
| GET | `/v1/users` | Bearer | list users |
| POST | `/v1/users` | Bearer | create user JSON body |
| GET | `/v1/users/{id}` | Bearer | get one |
| PATCH | `/v1/users/{id}` | Bearer | update name |
| DELETE | `/v1/users/{id}` | Bearer | delete |

Migrations: reuse `ormbaga` `ormbaga_migrations()` at startup (`migrate_up`).

## Config (env)

```
PORT              default 8080
FMR_JWT_SECRET    default baga-secret (override in prod)
PGHOST PGPORT PGUSER PGPASSWORD PGDATABASE
FMR_SYNC=1        single-threaded accept loop
```

## Success criteria

1. Pure tests: router params, jobj encode, jreq validation  
2. Live: migrate + CRUD users over HTTP JSON  
3. 401 without token; 422 on bad body; 404 unknown route  
4. README documents how to extend with new routes (production handbook)  

## Evolution

- **v1.1** OpenAPI JSON at `/openapi.json` from route table  
- **v1.2** DB pool (`std/par` channels)  
- **v1.3** CORS middleware + request-id  
- **v2** bind parameters when pgbaga Extended Query lands  
