# ormbaga — table ORM + versioned migrations

Date: 2026-08-03 · Updated: 2026-08-07  
Status: layered package  
Depends on: `pgbaga` (PostgreSQL wire client)

## Identity

ormbaga is Baga’s data layer: table-centric helpers, parameterized queries,
embedded migrations, optional pool. Historical design notes borrowed ideas from
Rails/GORM/goose; the public surface and package layout are native to Baga.

## Layering

```
  app / fmrbaga
        ↓
  ormbaga
    ├── migrate/   — versioned up/down runner
    ├── session/   — OrmDb + table CRUD
    ├── sql/       — pure quote/escape/builders
    └── pool/      — optional session pool
        ↓
  pgbaga (Simple + Extended Query)
        ↓
  PostgreSQL
```

## Migrations

Table (created automatically):

```sql
CREATE TABLE IF NOT EXISTS baga_schema_migrations (
  version    BIGINT PRIMARY KEY,
  name       TEXT NOT NULL,
  applied_at TIMESTAMPTZ NOT NULL DEFAULT NOW()
);
```

API: `migrate_ensure_table`, `migrate_up`, `migrate_down`, `migrate_status`,
`migrate_applied_versions`.

## ORM

```baga
fn orm_connect_env() -> OrmDb
fn orm_all / orm_all_page / orm_find_i64 / orm_where_eq_*
fn orm_insert_strs / orm_insert_returning_strs
fn orm_update_by_id / orm_delete_by_id / orm_count / orm_ping
```

Rows stay as pgbaga cells (`orm_cell_by`) — no codegen struct mapping.

## Phases

- **P0:** migrations up/down/status + CRUD helpers + demo + tests ✅
- **P1:** order/limit page helpers, RETURNING insert ✅
- **P2:** Extended Query default ✅; pool ✅; associations / file migrations later

## Success criteria

1. `migrate_up` then `migrate_down` is reversible on empty DB.  
2. Insert/find/update/delete users works through ORM helpers.  
3. `tests/orm_test.baga` prints `orm_test: all passed`.  
