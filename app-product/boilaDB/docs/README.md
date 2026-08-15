# boilaDB documentation

User-facing guides for **boilaDB** — a multimodal database written in
[Baga](../../../README.md). License: [MIT](../LICENSE).

Engineering notes that track phases and residuals stay at the package
root (`ARCHITECTURE.md`, `PLAN.md`, `gaps.md`).

## Guides

| Guide | Contents |
|-------|----------|
| [Getting started](getting-started.md) | Build, HTTP + PG servers, first SQL, CLI shell |
| [Configuration](configuration.md) | Environment variables, on-disk layout, query budget |
| [BoilaSQL](sql.md) | Types, DDL/DML, SELECT, functions, transactions, SQLSTATE |
| [HTTP API](http.md) | `/sql`, `/health`, `/ready`, `/metrics`, session db, CSRF |
| [PostgreSQL wire](pgwire.md) | `psql` / libpq / pgbaga / ormbaga on port 6575 |
| [Modalities](modalities.md) | FTS (BM25), VECTOR/HNSW, time-series, graph |
| [Security](security.md) | Trust, token, users/ACL, GRANT/REVOKE |
| [Operations](operations.md) | Workers, locks, metrics, benches, file-size / layer gates |

## Design notes (package root)

| Document | Role |
|----------|------|
| [ARCHITECTURE.md](../ARCHITECTURE.md) | Modular monolith, layers, concurrency, v1 bounds |
| [PLAN.md](../PLAN.md) | Phases P0–P11 and measured gates |
| [gaps.md](../gaps.md) | Honest residuals (fixed + still open) |
| [kimi-deps.md](../kimi-deps.md) | One-way import discipline (`scripts/deps.sh`) |

## Related packages

```
ormbaga  ──►  boilabaga  ──►  pgbaga  ──►  boilaDB :6575
   │                                      (PG wire v3)
   └────────►  pgbaga  ─────────────────►  PostgreSQL :5432
```

- [`boilabaga`](../../boilabaga/README.md) — client adapter + BoilaSQL dialect
- [`ormbaga`](../../ormbaga/README.md) — ORM + migrations over the same wire
- [`pgbaga`](../../pgbaga/README.md) — PostgreSQL frontend/backend v3 client
- [`rocksbaga`](../../rocksbaga/README.md) — LSM storage under every shard
