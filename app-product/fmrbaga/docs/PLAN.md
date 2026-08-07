# fmrbaga — Baga web framework

Date: 2026-08-03 · Updated: 2026-08-07  
Status: **CANONICAL BASE** for Baga web/API products  
Stack: `httpdbaga` + `std/json` + `jwtbaga` + `ormbaga`/`pgbaga`

## Decision (locked)

**fmrbaga = the application foundation.** New HTTP/JSON/DB products build *on*
this stack rather than inventing parallel frameworks.

fmrbaga is **itself** — route-id dispatch, jsonx, env config, OS-thread workers —
not a port of Lucky, Crystal, FastAPI, or Rails.

## Goals

1. **Routing** with path parameters and methods  
2. **JSON-first** request/response (builders, body parse → 422)  
3. **Dependencies** (Bearer JWT, DB session)  
4. **Uniform errors** (`detail` envelope)  
5. **HTTP/1.1 serve** with worker pool (default) and graceful drain  
6. **Universal scaffold** only in-package; products in `apps/*`  

## Package layout

See [`../ARCHITECTURE.md`](../ARCHITECTURE.md).

## Request pipeline

```
accept → http_read_request_r
  → request-id + traceparent
  → fmr_before(ctx)
  → router_match(method, path)
  → build FmrCtx
  → fmr_dispatch(ctx, db)
  → http_respond_keepalive (+ CORS, X-Request-Id)
```

## Concurrency (shipped)

| Mode | Env | Notes |
|------|-----|--------|
| Pool | `FMR_WORKERS=N` (default 4) | joinable workers, 1 DB each, chan queue |
| go_bg | `FMR_WORKERS=0` | one OS thread + SCRAM per TCP conn |
| Sync | `FMR_SYNC=1` | serial |

Shutdown: poll-based accept, SIGTERM/SIGINT, `chan_close`, drain (`FMR_DRAIN_MS`),
idle socket timeout (`FMR_IDLE_S`), join workers. Runtime gauges via `fmr_rt_*`.

## Config (env)

```
PORT FMR_WORKERS FMR_SYNC FMR_DRAIN_MS FMR_IDLE_S
FMR_JWT_SECRET FMR_LOG FMR_CORS FMR_TITLE FMR_VERSION
PGHOST PGPORT PGUSER PGPASSWORD PGDATABASE
```

## Success criteria

1. Pure tests: router, jobj, jreq, OpenAPI smoke  
2. Live product: migrate + CRUD over HTTP JSON (`apps/api`)  
3. Multi-thread: `FMR_WORKERS=8` health + metrics + SIGTERM drain  
4. Domain stays out of the framework package  
