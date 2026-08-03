# pgbaga

A native **PostgreSQL wire-protocol client** for Baga (frontend/backend protocol
v3), built only on `std/` (`net`, `bytes`, `crypto`, `random`, `os`, `str`).

This is the data-access foundation for a future **ORM + framework** — same probe
model as httpdbaga/jwtbaga: ship a working product and log language friction in
[`gaps.md`](gaps.md). Plan and phases: [`PLAN.md`](PLAN.md).

## What works

| Capability | Notes |
|------------|--------|
| TCP connect (IPv4) | `tcp_connect` dotted quad |
| StartupMessage | protocol 3.0, `user` / `database` / `client_encoding` |
| **SCRAM-SHA-256** | SASL (default on modern Postgres) |
| Cleartext password | auth kind 3 (if `pg_hba` asks) |
| Simple Query | `pg_query` — multi-statement string |
| **Extended Query** | `pg_query_params` — `$1..$n` text binds |
| Rows | text format, NULL as flag |
| Errors | severity \| SQLSTATE \| message |
| Transactions | `BEGIN`/`COMMIT`/`ROLLBACK`; `tx_status` from ReadyForQuery |
| Terminate | clean close |

## Not yet (P2)

Client-side prepared statement cache map, binary format, TLS, COPY, cancel,
LISTEN/NOTIFY, MD5 auth (deprecated). Pooling lives in `ormbaga/pool.baga` +
`FMR_WORKERS`.

## Files

| File | What |
|------|------|
| `pg_proto.baga` | pure framing: Startup, Query, SASL messages, RowDescription/DataRow parse |
| `pg_scram.baga` | pure PBKDF2-HMAC-SHA256 + SCRAM client proofs |
| `pg.baga` | `pg_connect` / `pg_query` / `pg_close` + accessors |
| `demo.baga` | CLI against local Postgres |
| `password.txt` | local credential hint (not read by the library) |
| `PLAN.md` | phases + Go/Rust layer map |
| `gaps.md` | language / protocol gaps |

## API

```baga
struct PgConn  { fd, ok, err, pid, key, tx_status, server_version }
struct PgResult { ok, err, tag, ncols, nrows, colnames, coltypes,
                  cells, nulls, tx_status, conn }

fn pg_connect(host, port, user, password, database) -> PgConn !Net !IO !Random
fn pg_close(conn: PgConn) -> i64 !IO !Net
fn pg_query(conn: PgConn, sql: str) -> PgResult !IO !Net
fn pg_query_params(conn, sql, vals: Vec<str>, nulls: Vec<i64>) -> PgResult !IO !Net
fn pg_query_params_str(conn, sql, vals: Vec<str>) -> PgResult !IO !Net
fn pg_param_str / pg_param_i64 / pg_param_null   // builders for vals/nulls

// Named prepared statements (session-scoped)
fn pg_prepare(conn, name, sql, nparams) -> PgResult !IO !Net
fn pg_exec_prepared(conn, name, vals, nulls) -> PgResult !IO !Net
fn pg_exec_prepared_str(conn, name, vals) -> PgResult !IO !Net
fn pg_deallocate(conn, name) -> PgResult !IO !Net

fn pg_ok / pg_err / pg_tag / pg_nrows / pg_ncols
fn pg_colname / pg_coltype / pg_cell / pg_isnull / pg_cell_i64
```

Prefer `pg_query_params` for any user-supplied values (injection-safe).
Use `pg_prepare` + `pg_exec_prepared` for hot paths on a long-lived connection.

`PgResult.conn` is the connection after the query (structs are by value —
always continue with `r.conn`).

Effects: SCRAM needs `!Random` (client nonce). Socket paths carry `!Net !IO`.
`pg_proto` / `pg_scram` are pure.

## Run the demo

```bash
# role used by defaults (once):
#   createuser -h 127.0.0.1 -U postgres -P bagatest
#   # password: pas+123

./baga app-product/pgbaga/demo.baga

# or emit C:
./baga --emit-c app-product/pgbaga/demo.baga > /tmp/pgdemo.c
gcc -O2 -Iinclude -o /tmp/pgdemo /tmp/pgdemo.c -lm -pthread
PGHOST=127.0.0.1 PGUSER=bagatest PGPASSWORD='pas+123' /tmp/pgdemo
```

Env: `PGHOST` `PGPORT` `PGUSER` `PGPASSWORD` `PGDATABASE` (defaults match
`password.txt`).

## Test

```bash
./baga tests/pg_test.baga    # pure SCRAM smoke + live queries
```

Wired into `make test` as `pg (app-product/pgbaga)`. Needs a reachable
Postgres with SCRAM for `bagatest` (or override `PG*`).

## Honesty / limits

- **No TLS** — same as httpdbaga; only cleartext TCP.
- **Simple Query only** — parameters must be inlined carefully (SQL injection
  risk until Extended Query lands); ORM layer will use Parse/Bind.
- **Text format only** — cells are `str`; binary OIDs are recorded but not decoded.
- **SASLprep** — passwords are used as raw UTF-8; fine for ASCII, incomplete for
  exotic Unicode (gap).
- **Buffered reader** is per-call (`pg_reader` rebuilt each `pg_query`); leftover
  socket data is not retained across queries (OK for request/response Simple
  Query; wrong for pipelining).
- Memory: leak-tolerant arena style like the rest of `std/` / app-product.

## Architecture (ORM path)

```
  future ORM  →  pg.baga (query/exec/tx)
                      ↓
                 pg_proto + pg_scram
                      ↓
                 std/net tcp_*_bytes
```

Go `pgx` and Rust `tokio-postgres` use the same layering; P1 Extended Query is
what makes parameterized statements and statement caching viable for an ORM.
