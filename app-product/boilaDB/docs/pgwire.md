# PostgreSQL wire protocol v3

Entry: `tools/serve_pg.baga` (default port **6575**). Real `psql` and
libpq work. This is the primary interface for applications.

## Connect

```bash
psql "host=127.0.0.1 port=6575 user=boila dbname=boila sslmode=disable"
```

Startup parameters:

| Field | Meaning |
|-------|---------|
| `user` | Login role (`boila` if empty) |
| `database` | Session database (default `boila`) |
| SSL request | Answered `'N'` — no TLS in v1 |

`ParameterStatus` advertises `server_version=boilaDB 0.1.0`,
`server_encoding=UTF8`, `client_encoding=UTF8`, `DateStyle`,
`TimeZone`. Another client encoding → `0A000`.

TCP_NODELAY is set on accept. Without it, Nagle × delayed ACK adds
~40 ms per extended-query round trip.

## Auth

See [security.md](security.md). Summary:

- Empty user catalog and empty `BOILA_TOKEN` → **trust** (no password
  message).
- Otherwise AuthenticationCleartextPassword (`R` + 3). Password is
  either a catalog user’s password or the shared `BOILA_TOKEN`
  (token → superuser).
- Failure → `28P01`. No SCRAM, no MD5, no TLS.

## What the server implements

| Message | Support |
|---------|---------|
| Simple Query (`Q`) | Yes |
| Parse / Bind / Describe / Execute / Sync | Yes (extended) |
| Close (`C`) | Yes |
| Terminate (`X`) | Yes |
| CancelRequest | Accepted (startup path); no mid-query cancel |
| COPY | No (`0A000`) |
| LISTEN / NOTIFY | No |
| Logical replication | No |

Extended-query error sets `in_err` and skips until `Sync` (libpq
semantics). The connection stays alive.

### Prepared statements

`Parse` caches the AST by statement name. `Bind` fills `$1..$n`
(structural values **and** `$N` inside expression spans). `Execute`
does not re-parse. Re-`Parse` of the same SQL under the same name is a
no-op (no extra allocations). Replacing the SQL frees the old AST when
no portal still references it.

`Describe` on a statement sends `ParameterDescription` +
`RowDescription` (or `NoData`). Typed OIDs:

| Type | OID |
|------|-----|
| bool | 16 |
| int8 | 20 |
| text | 25 |
| bytea | 17 |
| json | 114 |
| timestamptz | 1184 |

FTS / kNN `$N` that fail to bind as typed values fall back to text
substitution.

Unnamed portals die on `Sync` outside a transaction (the box is
reused). `DISCARD ALL` clears prepared state.

## Transactions

`BEGIN` / `COMMIT` / `ROLLBACK` (and `END` / `ABORT`) are
per-connection. Closing the socket rolls back an open transaction.

## Clients

### psql / libpq / any PG driver

Use port **6575** and `sslmode=disable`. Do not send
double-quoted identifiers or `SERIAL`. See [sql.md](sql.md) and
[boilabaga dialect notes](../../boilabaga/README.md).

### boilabaga (Baga)

```baga
import "boilabaga/adapter.baga"

let c = boila_connect_env()?
// BOILA_PGHOST / BOILA_PGPORT / BOILA_PGUSER / BOILA_PGPASSWORD
// BOILA_PGDATABASE / BOILA_TOKEN
```

### ormbaga

```baga
let db = orm_connect_boila_env()?
// or: ORM_BACKEND=boila  +  orm_connect_auto_env()?
// pool: orm_pool_from_boila_env(4)?
```

Live proof: `tests/orm_boila_test.baga` — 36/36 against `serve_pg`
(client and server with and without `--rc`).

### pgbaga directly

```baga
import "pgbaga/pg.baga"
// host 127.0.0.1, port 6575, user boila, database boila
```

Smoke: `bench/boila/pgwire_smoke.baga` (simple + extended, NULLs,
SQLSTATE, rollback).

## What is *not* Postgres

Honest list so drivers do not guess:

- No TLS, no SCRAM.
- No `COPY`, no `LISTEN`/`NOTIFY`, no replication.
- No `"Quoted"` identifiers — bare names only.
- No `SERIAL`, `DEFAULT`, `NOT NULL`, `REFERENCES`, `NUMERIC`, arrays.
- `PRIMARY KEY` is mandatory.
- `DROP INDEX name ON table` — the `ON` clause is required.
- `avg()` is integer division.
- One `JOIN`. No subqueries, no window functions.
- Graph `WITH RECURSIVE` is a fixed BFS/DFS/Dijkstra pattern, not a
  general CTE.
