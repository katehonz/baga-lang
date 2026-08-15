# boilabaga

**Client adapter** for [boilaDB](../boilaDB/README.md) over the **PostgreSQL
wire protocol** (via [`pgbaga`](../pgbaga/README.md)). User guides:
[boilaDB/docs](../boilaDB/docs/README.md).

boilaDB listens with PG frontend/backend v3 (`tools/serve_pg.baga`, default
port **6575**). This package is the thin client side: connect helpers + a
BoilaSQL dialect surface so [`ormbaga`](../ormbaga/README.md) can talk to
boila the same way it talks to real Postgres.

## Layout

| Path | Role |
|------|------|
| `adapter.baga` | `boila_connect` / `boila_connect_env` → `PgConn` |
| `dialect.baga` | bare idents + boila-shaped DDL builders |
| `examples/demo.baga` | ping + `SELECT 1` smoke |

## Connect

```baga
import "boilabaga/adapter.baga"
import "ormbaga/orm.baga"

let c = boila_connect_env()?          // 127.0.0.1:6575 / boila / boila
let db = orm_from_conn(c)             // ormbaga session over the same wire
// … orm_exec / orm_query / migrate_* …
```

Or in one step from ormbaga:

```baga
let db = orm_connect_boila_env()?
// or: ORM_BACKEND=boila  +  orm_connect_auto_env()?
// pool: orm_pool_from_boila_env(4)?
```

### Env

| Variable | Default | Notes |
|----------|---------|--------|
| `BOILA_PGHOST` | `127.0.0.1` | falls back to `PGHOST` only |
| `BOILA_PGPORT` | `6575` | never inherits `PGPORT` |
| `BOILA_PGUSER` | `boila` | never inherits `PGUSER` |
| `BOILA_PGPASSWORD` | `""` | trust when empty; never inherits `PGPASSWORD` |
| `BOILA_TOKEN` | — | if set, sent as cleartext password |
| `BOILA_PGDATABASE` | `boila` | never inherits `PGDATABASE` |

## Dialect honesty

boila’s BoilaSQL subset is **not** full Postgres:

- no double-quoted identifiers → use bare names (`users`, not `"users"`)
- no `SERIAL` / `BIGSERIAL` / `DEFAULT` / `NOT NULL` / `REFERENCES`
- `PRIMARY KEY` is required on every table
- `DROP INDEX … ON table` (ON clause required)

`dialect.baga` builders emit boila-safe DDL. ormbaga’s `sql_ident` uses
bare form for safe names so the same CRUD helpers work on both backends.

## Run

```bash
# terminal 1 — boila PG wire
./baga -I . -I app-product app-product/boilaDB/tools/serve_pg.baga

# terminal 2 — adapter smoke
./baga -I . -I app-product app-product/boilabaga/examples/demo.baga

# orm against boila (sample boila migrations)
./baga -I . -I app-product tests/orm_boila_test.baga
```

## Stack

```
ormbaga  ──►  boilabaga (adapter)  ──►  pgbaga  ──►  boilaDB :6575
   │                                              (PG wire v3)
   └────────►  pgbaga  ─────────────────────────►  PostgreSQL :5432
```
