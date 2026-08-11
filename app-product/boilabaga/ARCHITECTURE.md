# boilabaga — architecture

**Date:** 2026-08-11  
**Identity:** client adapter from Baga apps → boilaDB via PostgreSQL wire.

## Why a package

boilaDB already implements PG wire v3 server-side (`boilaDB/api/pgwire*`).
Clients still need:

1. **Defaults** for boila (port 6575, db `boila`, trust password).
2. **Dialect helpers** for BoilaSQL gaps vs Postgres (no quoted idents,
   no SERIAL/DEFAULT/FK).
3. A stable import path so **ormbaga** can open either backend without
   hard-coding boila env logic into every app.

## Graph

```
  app / tests
       │
       ▼
   ormbaga ──────────────┐
       │                 │
       ▼                 ▼
  boilabaga          (direct pg)
  adapter.baga           │
       │                 │
       ▼                 ▼
     pgbaga ──────────► TCP
       │                 │
       ▼                 ▼
  boilaDB :6575     Postgres :5432
```

| Rule | Meaning |
|------|---------|
| no dependency on ormbaga | avoids cycles; ORM imports the adapter |
| adapter returns `PgConn` | same type ormbaga already wraps |
| dialect is pure | no IO; builders only |
| package stays universal | no product domain |

## Auth mapping

| boila server state | client password |
|--------------------|-----------------|
| no users, no `BOILA_TOKEN` | empty (trust → AuthOk) |
| `BOILA_TOKEN` set | send token as cleartext |
| ACL users | user + password cleartext |

pgbaga already handles cleartext (`auth=3`) and SCRAM; boila uses trust or
cleartext only (no SCRAM yet — boila gaps W6).
