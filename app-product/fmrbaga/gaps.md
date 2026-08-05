# fmrbaga — gaps

## G1 — No function values → route ids + single dispatch

**Symptom.** Cannot pass handler closures into `router_add`.

**Workaround.** Integer route ids + `fmr_dispatch` match (same as `h2_route`).

**Verdict.** Language limit. Acceptable for production with codegen later.

## G2 — FNS_MAX was 256 (FIXED in language)

**Symptom.** Importing http + jsonx + orm together “lost” functions (`getrandom`,
`orm_query`, …) with no clear error.

**Fix.** `FNS_MAX` raised to 1024 in `src/checker.c`. **A5 (2026-08-05):**
exceeding the limit is a compile error (`твърде много функции`), not silent
truncation.

**Verdict.** Closed for normal apps.

## G3 — `chr()` vs raw socket bytes (FIXED in std/net)

**Symptom.** `tcp_listen(18080)` bound an ephemeral port; high port bytes ≥128
were written as multi-byte UTF-8 via `chr()` in `poke8`.

**Fix.** `poke8` uses `byte_chr`.

**Verdict.** Closed. Audit other `chr`+`pwrite` call sites if any.

## G4 — By-value `OrmDb` / `FmrOut` threading

**Symptom.** Handlers return `FmrOut { resp, db }`; must rebind.

**Verdict.** Same as pgbaga/ormbaga.

## G5 — OpenAPI from route table (B2.4 shipped)

**Shipped.** `fmr_openapi_from_router(r, title, version)` walks the live
`Router` and emits `paths` (methods, path params, bearer heuristic, body/
response schema names). `GET /openapi.json` uses the app's registered
routes. Schemas stay in `oas_components()`. Ops carry `x-baga-route-id`.

**Residual.** Full per-op prose still heuristic (not a second hand table);
new resources need body/ok schema name rules in `oas_body_schema` /
`oas_ok_schema` for rich refs.

## G6 — Middleware stack (partial — B2.1)

**Shipped.** Fixed pipeline in `fmr_handle` + `middleware.baga`:
request-id → otel `traceparent` (child span) → `fmr_before` → dispatch →
logbaga JSON (when `FMR_LOG=1`) → response headers (`X-Request-Id`,
`traceparent`, CORS).

**Still open.** Pluggable ordered middleware *ids* / DI graph; only one
app-defined hook (`fmr_before`) plus the built-in correlation chain.

## G7 — Connection-per-TCP DB session

**Symptom.** SCRAM cost on each new HTTP connection under `go_bg`.

**Verdict.** P2 pool with `std/par` channels.

## G8 — Combined program size

**Symptom.** Full serve unit is large (many imports); compile times grow.

**Verdict.** Acceptable; barrel files + FNS_MAX headroom help.

## G10 — Graceful shutdown (Phase 3 exit)

**Shipped.** Accept loop is `poll_wait`-based; SIGTERM/SIGINT stop accepting.
`/ready` returns 503 when `fmr_shutting_down()`. Workers receive stop fd.

**Residual.** In-flight `go_bg` connections are not drained with a deadline;
no shared request counter across workers (gauges only on `/metrics`).

## G9 — Dual protocol gRPC (B3.3 partial)

**Shipped.** `fmr_handle` detects gRPC POSTs and calls app `fmr_grpc_handle`.
Registry implements GetPackage/ListPackages; api/handlers return UNIMPLEMENTED.

**Open.** Full interceptor chain, H2 native trailers-only status, auth MD on
gRPC, OpenAPI↔proto parity.
