# ormbaga

**Table ORM + versioned migrations** for Baga, on top of
[`../pgbaga`](../pgbaga/README.md).

Architecture: [`ARCHITECTURE.md`](ARCHITECTURE.md).

## Layers

| Path | Role |
|------|------|
| `sql/sql.baga` | pure quote/escape/SQL builders + `$1` placeholders |
| `session/orm.baga` | `OrmDb` session + table CRUD over pgbaga |
| `migrate/migrate.baga` | migration registry + `up` / `down` / `status` |
| `migrate/schema.baga` | **sample only** migrations for demos/tests — apps own their schema |
| `pool/pool.baga` | shared session pool (sync/batch) |
| `examples/demo.baga` | migrate + CRUD demo |

Root `*.baga` files re-export these paths for stable imports.

## Migrations

```sql
CREATE TABLE IF NOT EXISTS baga_schema_migrations (
  version    BIGINT PRIMARY KEY,
  name       TEXT NOT NULL,
  applied_at TIMESTAMPTZ NOT NULL DEFAULT NOW()
);
```

```baga
let mut set = migrate_set_new()
set = migrate_add(set, 20260803001, "create_users", up_sql, down_sql)
// …
let r = migrate_up(db, set)?        // pending only, each in a transaction
db = migrate_db(r)                  // always rebind (by-value structs)
let n = migrate_applied_n(r)        // how many versions ran this call
let d = migrate_down(db, set, 1)?   // rollback last N
db = migrate_db(d)

let cur = migrate_current(db)?          // highest applied version (0 = empty)
db = orm_db_c(cur)
let st = migrate_status_text(db, set)?  // "ver name [applied|pending]" lines
db = st.db                              // report keeps your session
```

Registry is **embedded** (parallel `Vec`s — Baga has no `Vec<struct>`).
Duplicate versions in a set are rejected before any SQL runs; applied
versions are fetched once per `migrate_up` and checked in memory.

## ORM API

```baga
let db = orm_connect_env()?   // PG* env, default baga_orm / bagatest

let cols = vec_new(); vec_push(cols, "email"); vec_push(cols, "name")
let vals = vec_new(); vec_push(vals, "a@b.c"); vec_push(vals, "Ada")

// INSERT … RETURNING * — id in one round-trip
let ins = orm_insert_returning_strs(db, "users", cols, vals)?
db = orm_db_q(ins)
let id = orm_cell_by_i64(ins, 0, "id")

let q = orm_where_eq_str(db, "users", "email", "a@b.c")?
db = orm_db_q(q)

let f = orm_find_i64(db, "users", "id", id)?
let page = orm_all_page(db, "users", "id", 0, 20, 0)?  // order, limit, offset
let u = orm_update_by_id(db, "users", id, set_cols, set_lits)?
let d = orm_delete_by_id(db, "users", id)?
let c = orm_count(db, "users")?
let p = orm_ping(db)?
```

Always rebind `db = result.db` (by-value structs — same as pgbaga).

Raw escape hatch: `orm_exec` / `orm_query` / `orm_begin` / `orm_commit` / `orm_rollback`.

## Setup (Postgres)

```bash
psql -h 127.0.0.1 -U postgres -c "CREATE USER bagatest PASSWORD 'pas+123';"
psql -h 127.0.0.1 -U postgres -c "CREATE DATABASE baga_orm OWNER bagatest;"
psql -h 127.0.0.1 -U postgres -d baga_orm -c "GRANT ALL ON SCHEMA public TO bagatest;"
```

Defaults: `PGHOST=127.0.0.1` `PGUSER=bagatest` `PGPASSWORD=pas+123` `PGDATABASE=baga_orm`.

## Run

```bash
./baga app-product/ormbaga/demo.baga
./baga tests/orm_test.baga
```

## Parameterized queries (default)

`orm_find_*`, `orm_where_eq_*`, `orm_insert_strs`, `orm_insert_returning_strs`,
`orm_update_by_id`, `orm_delete_by_id` use **Extended Query** (`$1..$n`) via
pgbaga — values are bound, not concatenated.

Legacy `orm_*_lit` / `sql_lit` paths remain for migrations and trusted SQL.

## Pool + prepared statements

```baga
let pool = orm_pool_from_env(4)?
let lease = orm_pool_acquire(pool)?
// ... use lease.db ...
orm_pool_release(lease)?
orm_pool_close(pool)?

orm_prepare(db, "user_by_id", "SELECT * FROM users WHERE id=$1", 1)?
orm_query_prepared_str(db, "user_by_id", ids)?
```

HTTP servers should prefer **`FMR_WORKERS=N`** (one DB per worker).

## Honesty

- Prefer parameterized APIs; avoid hand-building SQL with user data.
- No model codegen — rows are dynamic cells (`orm_cell_by`).
- No associations API yet (`has_many` is a `where` on FK).
- No auto-migrate — prefer explicit versioned migrations.

## Path to framework

```
fmrbaga routes → ormbaga session → pgbaga → PostgreSQL
                     ↑ migrations at boot
```
