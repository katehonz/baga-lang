# pgbaga — architecture

**Date:** 2026-08-07  
**Status:** layered package  
**Identity:** native PostgreSQL wire client for Baga (protocol v3).

## Package tree

```
pgbaga/
├── ARCHITECTURE.md
├── README.md
├── sandak.toml
├── password.txt              # local credential hint (not imported)
│
├── pg.baga  pg_proto.baga  pg_scram.baga  demo.baga
│                 ↑ public re-exports
│
├── wire/
│   └── proto.baga            # pure framing, message parse/build
├── auth/
│   └── scram.baga            # pure SCRAM-SHA-256 + PBKDF2-HMAC
├── conn/
│   └── pg.baga               # connect, query, prepare, tx, accessors
├── examples/
│   └── demo.baga
└── docs/
    ├── PLAN.md
    ├── gaps.md
    └── password.txt
```

## Dependency graph

```
  examples/demo
        │
        ▼
     conn/pg
      /    \
     ▼      ▼
  wire/   auth/
  proto   scram
     \    /
      ▼  ▼
   std/{net,bytes,crypto,random,os,str,json}
```

| Rule | Meaning |
|------|---------|
| `wire` + `auth` are pure | no sockets |
| `conn` owns `PgConn` | only layer with `!Net !IO` (and `!Random` at connect) |
| No upward imports | wire/auth never import conn |
| Root re-exports stable | `import "pgbaga/pg.baga"` keeps working |

## Layer map (same idea as pgx / rust-postgres)

| Concern | pgbaga |
|---------|--------|
| Wire framing | `wire/proto.baga` |
| SCRAM / SASL | `auth/scram.baga` |
| Session + query API | `conn/pg.baga` |
| Pool / ORM | **ormbaga** (not here) |

## Surface

- Connect: `pg_connect` / `pg_connect_to` / `pg_close` / `pg_cancel` / `pg_ping`
- Simple Query: `pg_query`
- Extended Query: `pg_query_params`, `pg_prepare` / `pg_exec_prepared`
- Results: `PgOk(PgRows) | PgErr(PgFail)` + cell accessors
- Always rebind: `c = pg_conn_of(r)` after each query
