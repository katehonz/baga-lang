# ormbaga

**ActiveRecord-style ORM** + **goose/Flyway-style migrations** for Baga, on top of
[`../pgbaga`](../pgbaga/README.md).

## Model (from other languages)

| Layer | Inspired by | What we took |
|-------|-------------|--------------|
| ORM | Rails **ActiveRecord**, Go **GORM** | Table-centric `find` / `where` / `insert` / `update` / `delete` / `count` |
| Migrations | **goose** (Go), **Flyway**, Rails `schema_migrations` | Versioned **up/down** SQL, history of *all* applied versions, ordered apply/rollback |
| Quoting | ActiveRecord / `database/sql` | `sql_lit` / `sql_ident` (needed until pgbaga Extended Query) |

**Not chosen:** Diesel (type machinery too heavy for Baga today), Prisma (codegen + schema language), pure sqlx (no migration story).

## Layout

| File | Role |
|------|------|
| `sql.baga` | pure quote/escape/SQL builders |
| `orm.baga` | `OrmDb` session + ActiveRecord helpers over pgbaga |
| `migrate.baga` | migration registry + `up` / `down` / `status` |
| `schema.baga` | example migrations (`users`, `posts`) |
| `demo.baga` | migrate + CRUD demo |
| `PLAN.md` | design + phases |
| `gaps.md` | language / product gaps |

## Migrations (goose-style)

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
let r = migrate_up(db, set)?      // pending only, each in a transaction
db = r.db
let d = migrate_down(db, set, 1)? // rollback last N
```

Registry is **embedded** (parallel `Vec`s — Baga has no `Vec<struct>`), like
Go goose with `embed`. File-based loaders can wrap `read_file` later.

## ORM API (ActiveRecord-style)

```baga
let db = orm_connect_env()?   // PG* env, default baga_orm / bagatest

let cols = vec_new(); vec_push(cols, "email"); vec_push(cols, "name")
let vals = vec_new(); vec_push(vals, "a@b.c"); vec_push(vals, "Ada")
let ins = orm_insert_strs(db, "users", cols, vals)?
db = ins.db

let q = orm_where_eq_str(db, "users", "email", "a@b.c")?
db = q.db
let id = orm_cell_by_i64(q, 0, "id")

let f = orm_find_i64(db, "users", "id", id)?
let u = orm_update_by_id(db, "users", id, set_cols, set_lits)?
let d = orm_delete_by_id(db, "users", id)?
let c = orm_count(db, "users")?
```

Always rebind `db = result.db` (by-value structs — same as pgbaga).

Raw escape hatch: `orm_exec` / `orm_query` / `orm_begin` / `orm_commit` / `orm_rollback`.

## Setup (Postgres)

```bash
# superuser once:
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

## Parameterized queries (default path)

`orm_find_*`, `orm_where_eq_*`, `orm_insert_strs`, `orm_update_by_id`,
`orm_delete_by_id` use **Extended Query** (`$1..$n`) via pgbaga — values are
bound, not concatenated.

```baga
// safe even if email contains quotes / SQL metacharacters
orm_where_eq_str(db, "users", "email", user_input)?
orm_query_params_str(db, "SELECT * FROM users WHERE id = $1", ids)?
```

Legacy `orm_*_lit` / `sql_lit` paths remain for migrations and trusted SQL.

## Pool + prepared statements

```baga
// Shared pool (sync / batch)
let pool = orm_pool_from_env(4)?
let lease = orm_pool_acquire(pool)?
// ... use lease.db ...
orm_pool_release(lease)?
orm_pool_close(pool)?

// Named prepare on a long-lived session (e.g. FMR worker)
orm_prepare(db, "user_by_id", "SELECT * FROM users WHERE id=$1", 1)?
orm_query_prepared_str(db, "user_by_id", ids)?
```

HTTP servers should prefer **`FMR_WORKERS=N`** (one DB per worker) over opening a
session per TCP connection.

## Honesty

- **SQL injection:** prefer parameterized APIs; avoid hand-building SQL with user data.
- **No model codegen** — rows are dynamic cells (`orm_cell_by`), not typed structs.
- **No associations API** yet (`has_many` is just a `where` on FK).
- **No auto-migrate** (GORM-style) — prefer explicit goose migrations.

## Path to framework

```
framework routes → ormbaga models → pgbaga → PostgreSQL
                     ↑ migrations at boot or CLI
```
