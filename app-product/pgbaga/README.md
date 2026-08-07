# pgbaga

Native **PostgreSQL wire-protocol client** for Baga (frontend/backend protocol
v3), built only on `std/` (`net`, `bytes`, `crypto`, `random`, `os`, `str`).

Architecture: [`ARCHITECTURE.md`](ARCHITECTURE.md). Plan: [`docs/PLAN.md`](docs/PLAN.md).

## What works

| Capability | Notes |
|------------|--------|
| TCP connect | hostname (DNS via `getaddrinfo`) or dotted IPv4 |
| Timeouts / tuning | `pg_connect_to`, TCP_NODELAY, SO_KEEPALIVE |
| Cancel | `pg_cancel` — CancelRequest with BackendKeyData |
| StartupMessage | protocol 3.0, `user` / `database` / `client_encoding` |
| **SCRAM-SHA-256** | SASL (default on modern Postgres) |
| Cleartext password | auth kind 3 (if `pg_hba` asks) |
| Simple Query | `pg_query` — multi-statement string |
| **Extended Query** | `pg_query_params` — `$1..$n` text binds |
| Named prepared statements | `pg_prepare` / `pg_exec_prepared` / `pg_deallocate` |
| **JSON / JSONB** | text cells + OID detection, `pg_param_json` |
| Rows | text format, NULL as flag |
| Typed getters | `pg_cell_i64` / `pg_cell_bool` / `pg_cell_f64` / `pg_cell_json` |
| Errors | severity \| SQLSTATE \| message |
| Transactions | `BEGIN`/`COMMIT`/`ROLLBACK`; `tx_status` from ReadyForQuery |
| Ping | `pg_ping` — `SELECT 1` |
| Terminate | clean close |

## Not yet (P2)

Client-side prepared statement cache map, binary format, TLS, COPY,
LISTEN/NOTIFY, MD5 auth (deprecated). Pooling lives in `ormbaga/pool`.

## Layout

| Path | What |
|------|------|
| `wire/proto.baga` | pure framing: Startup, Query, SASL messages, row parse |
| `auth/scram.baga` | pure PBKDF2-HMAC-SHA256 + SCRAM client proofs |
| `conn/pg.baga` | `pg_connect` / `pg_query` / prepare / accessors |
| `examples/demo.baga` | CLI against local Postgres |
| `password.txt` | local credential hint (not read by the library) |

Root `pg.baga` / `pg_proto.baga` / `pg_scram.baga` are **stable re-exports**.

## API

```baga
struct PgConn  { fd, ok, err, pid, key, tx_status, server_version, reader }
struct PgRows  { tag, ncols, nrows, colnames, coltypes, cells, nulls, tx_status, conn }
struct PgFail  { err, tag, tx_status, conn }
enum PgResult { PgOk(PgRows), PgErr(PgFail) }

fn pg_connect(host, port, user, password, database) -> PgConn !Net !IO !Random
fn pg_connect_to(host, port, user, password, database, timeout_s)
fn pg_set_timeout(conn, secs) -> i64
fn pg_cancel(conn, host, port) -> i64
fn pg_close(conn: PgConn) -> i64 !IO !Net
fn pg_ping(conn) -> PgResult !IO !Net
fn pg_query(conn: PgConn, sql: str) -> PgResult !IO !Net
fn pg_query_params(conn, sql, vals: Vec<str>, nulls: Vec<i64>) -> PgResult !IO !Net
fn pg_query_params_str(conn, sql, vals: Vec<str>) -> PgResult !IO !Net
fn pg_param_str / pg_param_i64 / pg_param_null / pg_param_json

fn pg_prepare(conn, name, sql, nparams) -> PgResult !IO !Net
fn pg_exec_prepared(conn, name, vals, nulls) -> PgResult !IO !Net
fn pg_deallocate(conn, name) -> PgResult !IO !Net

fn pg_begin / pg_commit / pg_rollback

fn pg_ok / pg_err / pg_tag / pg_nrows / pg_ncols / pg_conn_of
fn pg_sqlstate / pg_err_message
fn pg_colname / pg_coltype / pg_cell / pg_isnull
fn pg_cell_i64 / pg_cell_bool / pg_cell_f64
fn pg_col_is_json / pg_col_is_jsonb / pg_json_valid / pg_cell_json
```

Prefer `pg_query_params` for any user-supplied values. Always rebind:
`c = pg_conn_of(r)`.

Effects: SCRAM needs `!Random`. Socket paths carry `!Net !IO`.
`wire/proto` and `auth/scram` are pure.

## Run the demo

```bash
./baga app-product/pgbaga/demo.baga
```

Env: `PGHOST` `PGPORT` `PGUSER` `PGPASSWORD` `PGDATABASE` (defaults match
`password.txt`).

## Test

```bash
./baga tests/pg_test.baga
```

## Honesty / limits

- **No TLS** — cleartext TCP only (same as httpdbaga).
- **Text format only** — cells are `str`; binary OIDs recorded but not decoded.
- **SASLprep** — passwords as raw UTF-8; fine for ASCII.
- **No LISTEN/NOTIFY dispatch** yet (reader buffer is preserved across queries).
- Memory: leak-tolerant arena style like the rest of `std/` / app-product.
