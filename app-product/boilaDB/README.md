# boilaDB

**Multimodal database in [Baga](../../README.md)** — a modular monolith
over sharded [rocksbaga](../rocksbaga/README.md) (LSM). The language is
**BoilaSQL**, a documented PostgreSQL subset. The wire is **PostgreSQL
v3** (real `psql` / libpq) plus an HTTP admin/SQL surface.

| | |
|--|--|
| **Version** | 0.1.0 |
| **License** | [MIT](LICENSE) |
| **Encoding** | UTF-8 only |
| **Out of scope** | Geo / GPS (not on the roadmap) |
| **Docs** | [docs/](docs/README.md) |

> *"One storage engine, many modalities. SQL is a subset, not an imitation."*

## Why

The stack exists to prove Baga can host real systems work. boilaDB is
the SQL flagship on top of the RocksDB-class engine:

- **Typed values**, not string sentinels. `NULL = NULL` is never true
  (SQL 3VL).
- **Everything durable.** Rows, secondary indexes, HNSW, FTS postings,
  graph adjacency — LSM key-spaces. Restart does not rebuild RAM indexes.
- **Honest dialect.** Unknown SQL → `0A000 feature_not_supported`,
  never a quiet wrong answer.
- **Bounded load.** Worker pool, admission cap, query budget — flat
  p99 is the goal, not a single-thread peak.

## Status

Phases **P0–P11** landed. Concurrent HTTP + PG: bounded
`BOILA_WORKERS` pool (default 4; `=0` → `go_bg` per conn), hop-less
per-shard stores, shared per-db plan cache. Data SQL is a shared lock;
schema DDL is exclusive per database. No SCRAM/TLS ([gaps.md](gaps.md)
W6).

| Surface | Result |
|---------|--------|
| PG wire v3 | `psql` / libpq / [pgbaga](../pgbaga/README.md) on **:6575** |
| HTTP | `/sql` `/health` `/ready` `/metrics` on **:6570** |
| ORM | [ormbaga](../ormbaga/README.md) **36/36** vs `serve_pg` (with and without `--rc`) |
| Point @10k | 156k ops/s |
| Insert @10k | 524 ops/s |
| Mix 80/20 @10k | 2.6k ops/s |
| 1M INSERT | 169 s, 1.35 GB RSS, DURABLE OK |

Numbers: `bench/boila/results/`. Architecture and phase gates:
[ARCHITECTURE.md](ARCHITECTURE.md), [PLAN.md](PLAN.md).

## Features

| Area | What ships |
|------|------------|
| **SQL** | SELECT / INSERT / UPDATE / DELETE, `$1` prepared, JOIN (INNER/LEFT), GROUP BY / HAVING, expressions, `CASE`, `CAST`, dual (`SELECT` without FROM) |
| **DDL** | Multi-database (`CREATE`/`DROP`/`USE`), tables (PK required), ALTER add/rename/drop column, indexes, `IF [NOT] EXISTS`, `TRUNCATE`, `SHOW` / `DESCRIBE` |
| **Txns** | `BEGIN` / `COMMIT` / `ROLLBACK`, session write buffer + LSN, crash → clean rollback |
| **FTS** | BM25, `@@ to_tsquery`, phrase `<->` / `<N>`, EN/BG tokenizer |
| **Vector** | `VECTOR(n)`, HNSW, `<->` L2 / `<=>` cosine / `<#>` neg-IP |
| **Time-series** | `ttl_days` / `ttl_sec`, `time_bucket`, continuous `CREATE ROLLUP` |
| **Graph** | `CREATE GRAPH`, `WITH RECURSIVE` BFS / DFS / Dijkstra |
| **ACL** | `CREATE USER`, `GRANT`/`REVOKE`, token or trust |
| **Ops** | Query budget, `EXPLAIN [ANALYZE]`, Prometheus metrics |

Dialect reference: [docs/sql.md](docs/sql.md). Modalities:
[docs/modalities.md](docs/modalities.md).

## Quick start

From the Baga repo root (`make` first):

```bash
# HTTP — worker pool + per-shard + multi-DB
#   BOILA_PATH=/tmp/baga_boila  BOILA_PORT=6570
#   BOILA_MAX_CONN=64  BOILA_WORKERS=4   (0 = go_bg per conn)
./baga -I . -I app-product app-product/boilaDB/tools/serve.baga

# PostgreSQL wire v3
#   BOILA_PGPORT=6575
./baga -I . -I app-product app-product/boilaDB/tools/serve_pg.baga

curl -s localhost:6570/health
psql "host=127.0.0.1 port=6575 user=boila dbname=boila sslmode=disable"
```

```sql
CREATE TABLE users (
  id   BIGINT,
  name TEXT,
  PRIMARY KEY (id)
);
INSERT INTO users (id, name) VALUES (1, 'Ана') RETURNING id;
SELECT name FROM users WHERE id = 1;
```

In-process REPL (no listen socket):

```bash
./baga -I . -I app-product app-product/boilaDB/tools/shell.baga
```

Step-by-step: [docs/getting-started.md](docs/getting-started.md).
Environment: [docs/configuration.md](docs/configuration.md).

## Clients

```
apps/*  →  fmrbaga  →  ormbaga  →  boilabaga  →  pgbaga  →  boilaDB :6575
                                              ↘  pgbaga  →  PostgreSQL :5432
```

```baga
import "boilabaga/adapter.baga"
import "ormbaga/orm.baga"

let c  = boila_connect_env()?        // 127.0.0.1:6575 / boila / boila
let db = orm_from_conn(c)
// or: let db = orm_connect_boila_env()?
//     ORM_BACKEND=boila + orm_connect_auto_env()?
```

Client env (`BOILA_PGHOST`, `BOILA_PGPORT`, …) never inherits `PGPORT` /
`PGUSER` / `PGDATABASE` / `PGPASSWORD`. See
[docs/pgwire.md](docs/pgwire.md) and
[`boilabaga`](../boilabaga/README.md).

HTTP `POST /sql` (body = SQL) is for tools and admin:
[docs/http.md](docs/http.md).

## Layout

```
app-product/boilaDB/
├── README.md  LICENSE  sandak.toml
├── ARCHITECTURE.md  PLAN.md  gaps.md  kimi-deps.md
├── docs/          user guides
├── core/          value · codec · row · budget
├── storage/       N shards × rocksbaga
├── txn/           session buffer + LSN
├── catalog/       schema · DDL · TTL · ACL
├── index/         secondary indexes
├── fts/  vector/  ts/  graph/     modalities
├── sql/           lexer → parser → planner → executor
├── server/        multi-DB registry
├── api/           PG wire + HTTP + worker pool
├── tools/         serve · serve_pg · shell
└── scripts/       filesize.sh  deps.sh
```

Tests live in the repo `tests/boila_*_test.baga` (discovered by
`scripts/baga-test`). Hard limit: **400 lines per `.baga` file**.
Imports are one-way down the layer stack (`scripts/deps.sh`).

```bash
./scripts/baga-test
bash app-product/boilaDB/scripts/filesize.sh
bash app-product/boilaDB/scripts/deps.sh
bash bench/boila/run_modality_benches.sh all
```

## Documentation

| Document | Contents |
|----------|----------|
| **[docs/](docs/README.md)** | Getting started, SQL, HTTP, PG wire, config, security, ops |
| [ARCHITECTURE.md](ARCHITECTURE.md) | Layers, concurrency, v1 bounds |
| [PLAN.md](PLAN.md) | P0–P11 with measured gates |
| [gaps.md](gaps.md) | Honest residuals |
| [kimi-deps.md](kimi-deps.md) | Import-discipline notes |

## License

[MIT](LICENSE) — Copyright (c) 2026 Dim Gigov.
