# Canonical product stack (base)

**Locked:** 2026-08-03 · **DX model:** Crystal [Lucky](https://luckyframework.org/) —
small community, real conventions, apps you can ship.

```
                 apps/api   (your product — actions, models, routes)
                      │
                 ┌────▼────┐
                 │ fmrbaga │   framework (router, JSON, auth, config, serve)
                 └────┬────┘
        ┌─────────────┼─────────────┐
        ▼             ▼             ▼
   httpdbaga      jwtbaga       ormbaga (+ pool)
   (HTTP)         (JWT)      (AR + goose migrations)
                                   │
                                   ▼
                                pgbaga
                           (Postgres wire + $1)
                                   │
                                   ▼
                                Postgres
```

| Layer | Path | Role |
|-------|------|------|
| **App** | `apps/api` | product code (Lucky-style actions/models) |
| Framework | `app-product/fmrbaga` | router, jsonx, deps, config, workers |
| HTTP | `httpdbaga` | request/response |
| Auth | `jwtbaga` | HS256 |
| ORM | `ormbaga` | migrations, CRUD, pool, prepare |
| Driver | `pgbaga` | SCRAM, Simple + Extended Query |

**Rules**

1. Business logic lives in `apps/*`, not inside fmrbaga.
2. Prefer `FMR_WORKERS=N` in production.
3. Prefer parameterized ORM (`$1`) for user input.
4. Do not fork a second web framework without a strong reason.
