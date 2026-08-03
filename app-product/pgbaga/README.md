# pgbaga

A native **PostgreSQL wire-protocol client** for Baga (frontend/backend protocol
v3), built only on `std/` (`net`, `bytes`, `crypto`, `random`, `os`, `str`).

This is the data-access foundation for a future **ORM + framework** — same probe
model as httpdbaga/jwtbaga: ship a working product and log language friction in
[`gaps.md`](gaps.md). Plan and phases: [`PLAN.md`](PLAN.md).

## What works

| Capability | Notes |
|------------|--------|
| TCP connect | hostname (DNS via `getaddrinfo`) or dotted IPv4 |
| Timeouts / tuning | `pg_connect_to` (SO_RCVTIMEO/SO_SNDTIMEO), TCP_NODELAY, SO_KEEPALIVE |
| Cancel | `pg_cancel` — CancelRequest with the BackendKeyData from startup |
| StartupMessage | protocol 3.0, `user` / `database` / `client_encoding` |
| **SCRAM-SHA-256** | SASL (default on modern Postgres) |
| Cleartext password | auth kind 3 (if `pg_hba` asks) |
| Simple Query | `pg_query` — multi-statement string |
| **Extended Query** | `pg_query_params` — `$1..$n` text binds |
| Named prepared statements | `pg_prepare` / `pg_exec_prepared` / `pg_deallocate` |
| **JSON / JSONB tables** | text-format cells + OID detection (`pg_col_is_json[b]`), strict validation (`pg_json_valid`), `pg_param_json` binds |
| Rows | text format, NULL as flag |
| Typed getters | `pg_cell_i64` / `pg_cell_bool` / `pg_cell_f64` / `pg_cell_json` |
| Errors | severity \| SQLSTATE \| message |
| Transactions | `BEGIN`/`COMMIT`/`ROLLBACK`; `tx_status` from ReadyForQuery |
| Buffered reader in `PgConn` | survives across queries (ready for async messages) |
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
struct PgConn  { fd, ok, err, pid, key, tx_status, server_version, reader }
struct PgResult { ok, err, tag, ncols, nrows, colnames, coltypes,
                  cells, nulls, tx_status, conn }

fn pg_connect(host, port, user, password, database) -> PgConn !Net !IO !Random
fn pg_connect_to(host, port, user, password, database, timeout_s) // hostname or
    // dotted IPv4; timeout (seconds) bounds connect/reads/writes; 0 = blocking
fn pg_set_timeout(conn, secs) -> i64   // retune a live connection (0 clears)
fn pg_cancel(conn, host, port) -> i64  // CancelRequest on a fresh connection
fn pg_close(conn: PgConn) -> i64 !IO !Net
fn pg_query(conn: PgConn, sql: str) -> PgResult !IO !Net
fn pg_query_params(conn, sql, vals: Vec<str>, nulls: Vec<i64>) -> PgResult !IO !Net
fn pg_query_params_str(conn, sql, vals: Vec<str>) -> PgResult !IO !Net
fn pg_param_str / pg_param_i64 / pg_param_null / pg_param_json  // builders for vals/nulls

// Named prepared statements (session-scoped)
fn pg_prepare(conn, name, sql, nparams) -> PgResult !IO !Net
fn pg_exec_prepared(conn, name, vals, nulls) -> PgResult !IO !Net
fn pg_exec_prepared_str(conn, name, vals) -> PgResult !IO !Net
fn pg_deallocate(conn, name) -> PgResult !IO !Net

// Transactions (tx_status tracks every ReadyForQuery)
fn pg_begin / pg_commit / pg_rollback

fn pg_ok / pg_err / pg_tag / pg_nrows / pg_ncols
fn pg_sqlstate / pg_err_message                  // split "SEV|SQLSTATE|message"
fn pg_colname / pg_coltype / pg_cell / pg_isnull
fn pg_cell_i64 / pg_cell_bool / pg_cell_f64       // typed getters from text cells

// JSON / JSONB: cells arrive as JSON text (text protocol)
fn pg_col_is_json(r, col) / pg_col_is_jsonb(r, col)   // OID 114/199 vs 3802/3807
fn pg_json_valid(s) -> i64                            // strict RFC 8259 (std/json)
fn pg_cell_json(r, row, col) -> str                   // raw JSON text
fn pg_cell_json_ok(r, row, col) -> i64                // non-NULL and parses
```

JSON tables work end to end: create them with `json` / `jsonb` columns,
insert with `pg_param_json` + `$N::json[b]` binds (or `ormbaga`'s
`sql_json` / `sql_jsonb` literals for trusted SQL), read cells back as JSON
text and hand them to `std/json` (`json_parse`). `json` preserves the input
text verbatim; `jsonb` comes back normalized.

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
- **Text format only** — cells are `str`; binary OIDs are recorded but not decoded.
  JSON/JSONB travel as text (their native wire text form), so nothing is lost.
- **SASLprep** — passwords are used as raw UTF-8; fine for ASCII, incomplete for
  exotic Unicode (gap).
- **Reader is request/response** — the buffer lives in `PgConn` and survives
  across queries, but async messages (LISTEN/NOTIFY) are not dispatched yet.
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
