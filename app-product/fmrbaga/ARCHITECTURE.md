# fmrbaga — architecture

**Date:** 2026-08-07  
**Status:** layered package  
**Identity:** Baga’s own small HTTP/JSON framework — not a port of another stack.

## Why layers

A flat folder of router + JSON + middleware + OpenAPI + serve loop does not
scale. Layers mirror request flow, with **one concern per package folder**.

## Package tree

```
fmrbaga/
├── ARCHITECTURE.md
├── README.md  GUIDE.md
├── sandak.toml
│
├── app.baga  route.baga  ctx.baga  …   ← public re-exports (stable imports)
├── jsonx.baga  openapi.baga  handlers.baga  serve.baga
│
├── core/                 # request path + app runtime
│   ├── route.baga        # {param} router, route ids
│   ├── ctx.baga          # FmrCtx
│   ├── respond.baga      # JSON / error envelopes
│   ├── deps.baga         # Bearer JWT, Content-Type
│   ├── middleware.baga   # request-id, traceparent, log
│   ├── config.baga       # env load
│   ├── app.baga          # FmrApp, pipeline, HTTP serve, workers, stats
│   └── action.baga       # thin handler helpers
├── json/
│   └── jsonx.baga        # jobj/jarr + jreq 422 validation
├── openapi/
│   └── openapi.baga      # OpenAPI 3 from route table
├── handlers/
│   └── handlers.baga     # universal scaffold (ops + JWT only)
├── examples/
│   └── serve.baga        # scaffold listen entrypoint
└── docs/
    ├── PLAN.md
    └── gaps.md
```

## Dependency graph (allowed)

```
  examples/serve  →  handlers  →  core/app
                                    │
              ┌─────────────────────┼─────────────────────┐
              ▼                     ▼                     ▼
           openapi               json/jsonx            core/*
              │                     │
              └──────────┬──────────┘
                         ▼
              httpdbaga · jwtbaga · ormbaga · otelbaga · logbaga
```

**Rules**

| Rule | Meaning |
|------|---------|
| No upward imports | `json/` must not import `core/app` |
| `core` may use `json` | response builders + body validation |
| `handlers` is optional | product apps implement their own `fmr_dispatch` |
| Public re-exports at root | `import "fmrbaga/app.baga"` stays stable |
| Prefer explicit layers | `import "fmrbaga/core/app.baga"` for new code |

## Serve runtime (workers)

```
                    SIGTERM / SIGINT
                           │
                           ▼
   ┌─────────────────────────────────────────┐
   │  accept loop (poll_wait, main thread)   │
   │  tcp_accept → chan_send(work, fd)       │
   └──────────────────┬──────────────────────┘
                      │ buffered chan
         ┌────────────┼────────────┐
         ▼            ▼            ▼
    worker 0     worker 1     worker N-1
    (go+join)    (go+join)    (go+join)
    1× OrmDb     1× OrmDb     1× OrmDb
         │            │            │
         └────────────┴────────────┘
                fmr_h1_conn
                (idle SO_RCVTIMEO)
```

**Shutdown sequence (pool mode)**

1. Signal → stop accept; `/ready` returns 503  
2. `tcp_close(listener)`  
3. `chan_close(work)` — idle workers exit  
4. `fmr_drain_wait` until inflight ≈ 0 or `FMR_DRAIN_MS`  
5. `join` all workers (busy ones leave when idle socket timeout fires)  
6. Unpublish runtime stats; exit  

**Stats handle** (`map_h`): inflight / accepted / completed / workers / mode.  
Published under `/tmp/fmrbaga.rt.<pid>` for `fmr_rt_*` (used by `/metrics`).

## What fmrbaga *is*

- **Route ids + optional L5 handlers** — `fmr_route_fn` stores the fn;
  `fmr_dispatch` remains the fallback for product apps.
- **JSON-first API** — builders, 422 field errors, Bearer deps.
- **Env config + OS-thread workers** — default pool of 4; one DB per worker.
- **Universal** — no product tables. Scaffold is ops + JWT only.

## What fmrbaga is *not*

- A product application (those live under `apps/*`).
- A clone of Lucky, Crystal, FastAPI, or Rails.
- HTML full-stack or template-driven MVC.
- A second HTTP stack (transport is httpdbaga).
- Green threads / async runtime — concurrency is real `pthread`s.

## Apps stay universal

```
apps/<name>/
  start.baga       # migrate (own schema) → fmr_run
  routes.baga      # fmr_build_app + fmr_dispatch
  schema.baga      # MigrationSet for this product
  actions/ models/ # domain
```

Examples: `apps/api` (JSON CRUD), `apps/registry` (package index + gRPC).
