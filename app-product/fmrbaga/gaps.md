# fmrbaga — gaps

## G1 — No function values → route ids + single dispatch

**Symptom.** Cannot pass handler closures into `router_add`.

**Workaround.** Integer route ids + `fmr_dispatch` match (same as `h2_route`).

**Verdict.** Language limit. Acceptable for production with codegen later.

## G2 — FNS_MAX was 256 (FIXED in language)

**Symptom.** Importing http + jsonx + orm together “lost” functions (`getrandom`,
`orm_query`, …) with no clear error.

**Fix.** `FNS_MAX` raised to 1024 in `src/checker.c`. Still silent if exceeded —
should warn when full.

**Verdict.** Closed for normal apps; add a hard error when `n_fns == FNS_MAX`.

## G3 — `chr()` vs raw socket bytes (FIXED in std/net)

**Symptom.** `tcp_listen(18080)` bound an ephemeral port; high port bytes ≥128
were written as multi-byte UTF-8 via `chr()` in `poke8`.

**Fix.** `poke8` uses `byte_chr`.

**Verdict.** Closed. Audit other `chr`+`pwrite` call sites if any.

## G4 — By-value `OrmDb` / `FmrOut` threading

**Symptom.** Handlers return `FmrOut { resp, db }`; must rebind.

**Verdict.** Same as pgbaga/ormbaga.

## G5 — No OpenAPI / schema codegen

**Symptom.** Clients discover routes only via README or `/v1/meta`.

**Verdict.** P1: emit OpenAPI JSON from the route table.

## G6 — Middleware is a single `fmr_before` hook

**Symptom.** No ordered middleware stack / DI graph.

**Workaround.** Call `deps_*` inside handlers (FastAPI Depends style).

**Verdict.** Enough for v1; stack of middleware ids later.

## G7 — Connection-per-TCP DB session

**Symptom.** SCRAM cost on each new HTTP connection under `go_bg`.

**Verdict.** P2 pool with `std/par` channels.

## G8 — Combined program size

**Symptom.** Full serve unit is large (many imports); compile times grow.

**Verdict.** Acceptable; barrel files + FNS_MAX headroom help.
