# ormbaga — table ORM + versioned migrations

Date: 2026-08-03 · Updated: 2026-08-08
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
  pgbaga (Simple + Extended Query)     boilabaga adapter
        ↓                                     ↓
  PostgreSQL :5432                      boilaDB PG wire :6575
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

API: `migrate_ensure_table`, `migrate_up`, `migrate_down`, `migrate_status`
(raw history rows), `migrate_status_text` (set ∪ history report, keeps the
session), `migrate_current` (highest applied version), `migrate_applied`.
Duplicate versions in a set are rejected before any SQL runs; `migrate_up`
fetches applied versions once and checks membership in memory.

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
- **P2.1 (2026-08-08):** migration hardening — single-fetch applied set,
  duplicate-version guard, parameterized history writes, `migrate_current`,
  `migrate_status_text` keeping the session; pool release writes back all
  conn fields ✅
- **P2.2 (2026-08-11):** boilabaga adapter — `orm_from_conn` /
  `orm_connect_boila_env`, dual-safe `sql_ident` + history DDL,
  `ormbaga_boila_migrations`, `tests/orm_boila_test.baga` ✅

## Success criteria

1. `migrate_up` then `migrate_down` is reversible on empty DB.  
2. Insert/find/update/delete users works through ORM helpers.  
3. `tests/orm_test.baga` prints `orm_test: all passed`.  
4. With `serve_pg` up, `tests/orm_boila_test.baga` prints `orm_boila_test: all passed`. 
