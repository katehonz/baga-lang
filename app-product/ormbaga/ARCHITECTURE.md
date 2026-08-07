# ormbaga — architecture

**Date:** 2026-08-07  
**Status:** layered package  
**Identity:** Baga table ORM + versioned migrations on top of pgbaga.

## Package tree

```
ormbaga/
├── ARCHITECTURE.md
├── README.md
├── sandak.toml
│
├── orm.baga  sql.baga  migrate.baga  schema.baga  pool.baga  demo.baga
│                 ↑ public re-exports (stable import paths)
│
├── sql/
│   └── sql.baga          # pure quote / builders / $1 placeholders
├── session/
│   └── orm.baga          # OrmDb, CRUD, prepare, cell accessors
├── migrate/
│   ├── migrate.baga      # up / down / status registry
│   └── schema.baga       # sample users+posts migrations
├── pool/
│   └── pool.baga         # channel pool of OrmDb sessions
├── examples/
│   └── demo.baga
└── docs/
    ├── PLAN.md
    └── gaps.md
```

## Dependency graph

```
  examples/demo
        │
        ▼
  migrate/  ──►  session/orm  ──►  sql/
        │              │
        │              ▼
        │           pgbaga
        ▼
     (DDL via session)
```

| Rule | Meaning |
|------|---------|
| `sql/` is pure | no IO, no pgbaga |
| `session` owns `OrmDb` | only layer that talks to pgbaga |
| `migrate` uses session | never opens its own sockets |
| `pool` wraps session | optional; HTTP prefers FMR_WORKERS |
| Root re-exports stable | `import "ormbaga/orm.baga"` keeps working |

## Design choices (Baga-native)

1. **No model codegen** — rows are dynamic cells (`orm_cell_by`).
2. **Parameterized by default** — `orm_find_*` / `orm_insert_strs` use `$1`.
3. **RETURNING insert** — `orm_insert_returning_strs` for id without re-query.
4. **Page helpers** — `orm_all_page` / `orm_where_eq_*_page` + `sql_page`.
5. **Embedded migration registry** — parallel Vecs (language has no `Vec<struct>`).
6. **Always rebind** — `db = orm_db_q(r)` after every call (by-value structs).
