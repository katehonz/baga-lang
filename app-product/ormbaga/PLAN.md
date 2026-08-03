# ormbaga — ActiveRecord-style ORM + versioned migrations

Date: 2026-08-03  
Status: implementing  
Depends on: `../pgbaga` (PostgreSQL wire client)

## Model (picked from other languages)

| Piece | Inspired by | Why |
|-------|-------------|-----|
| **ORM** | Rails **ActiveRecord** + Go **GORM** (table-centric) | One table ↔ one model surface; `find` / `where` / `create` / `update` / `destroy` is the most portable mental model |
| **Migrations** | **goose** (Go) + **Flyway** + Rails `schema_migrations` | Explicit versioned **up/down SQL**, history table of *all* applied versions (not just “current”), ordered apply/rollback |
| **SQL safety** | `database/sql` + ActiveRecord quoting | Until Extended Query lands in pgbaga: quote identifiers + escape literals |

**Not chosen:** Diesel (too type-heavy for Baga), Prisma (needs codegen + schema language), raw sqlx (no migration story).

## Layering

```
  app / framework
        ↓
  ormbaga  (this package)
    ├── migrate.baga   — goose-style runner
    ├── orm.baga       — ActiveRecord CRUD + session
    └── sql.baga       — pure quote/escape/builders
        ↓
  pgbaga (Simple Query + SCRAM)
        ↓
  PostgreSQL
```

## Migrations (goose-style)

Table (created automatically):

```sql
CREATE TABLE IF NOT EXISTS baga_schema_migrations (
  version    BIGINT PRIMARY KEY,
  name       TEXT NOT NULL,
  applied_at TIMESTAMPTZ NOT NULL DEFAULT NOW()
);
```

Each migration is a record: `version` (monotonic int, e.g. `20260803001`), `name`, `up_sql`, `down_sql`.

Registry is **in-process** (parallel `Vec`s — Baga has no `Vec<struct>`), like Go `goose` with embedded SQL. Optional: load SQL text from files via `read_file` and register.

API:

- `migrate_ensure_table(db)`
- `migrate_up(db, set)` — apply all pending in version order (each in a transaction)
- `migrate_down(db, set, steps)` — rollback last `steps`
- `migrate_status(db, set)` — applied / pending report string
- `migrate_applied_versions(db) -> Vec<i64>`

## ORM (ActiveRecord-style)

```baga
struct OrmDb { conn: PgConn }

fn orm_connect(...) -> OrmDb
fn orm_close(db) -> i64

// session / raw
fn orm_exec(db, sql) -> OrmExec
fn orm_query(db, sql) -> OrmQuery   // wraps PgResult + db
fn orm_begin / orm_commit / orm_rollback

// table helpers (model = table name + column list)
fn orm_all(db, table) -> OrmQuery
fn orm_find(db, table, pk_col, id) -> OrmQuery          // 0 or 1 row
fn orm_where_eq(db, table, col, val) -> OrmQuery
fn orm_insert(db, table, cols, vals) -> OrmExec         // RETURNING * optional later
fn orm_update_eq(db, table, set_cols, set_vals, where_col, where_val) -> OrmExec
fn orm_delete_eq(db, table, col, val) -> OrmExec
fn orm_count(db, table) -> OrmCount
```

Rows stay as pgbaga cells (`pg_cell` / `pg_colname`) — no codegen struct mapping until Baga has better generics.

## SQL injection note

P0 uses Simple Query only → **all values go through `sql_lit` / `sql_ident`**. Never concatenate raw user input.

## Demo schema

Migrations create `users` and `posts` in database `baga_orm` (owner `bagatest`).

## Phases

- **P0 (this):** migrations up/down/status + AR CRUD helpers + demo + tests  
- **P1:** associations (`has_many` SQL helpers), `order`/`limit`, `RETURNING` insert, load migrations from `migrations/*.sql`  
- **P2:** use pgbaga Extended Query for parameters; connection pool; validation hooks  

## Success criteria

1. `migrate_up` then `migrate_down` is reversible on empty DB.  
2. Insert/find/update/delete users works through ORM helpers.  
3. `tests/orm_test.baga` prints `orm_test: all passed`.  
