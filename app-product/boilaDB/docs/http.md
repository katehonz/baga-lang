# HTTP API

Entry: `tools/serve.baga` (default port **6570**). Same worker pool,
per-shard stores, and multi-database registry as the PG listener.

All JSON is UTF-8. SQL errors are returned as HTTP **200** with
`"ok": false` and a `sqlstate` — the connection stays up.

## Endpoints

| Method | Path | Auth | Role |
|--------|------|------|------|
| `GET` | `/health` | no | Liveness + version + pool mode |
| `GET` | `/ready` | no | `200` ready / `503` at `BOILA_MAX_CONN` |
| `GET` | `/metrics` | yes | Prometheus text (`metbaga`) |
| `POST` | `/sql` | yes | Body = one SQL statement |
| — | other | — | `404` `{"error":"not found"}` |

Auth is required for `/sql` and `/metrics` once a token or user catalog
exists. See [security.md](security.md).

### `GET /health`

```json
{
  "status": "ok",
  "open_databases": 1,
  "max_db": 64,
  "live_conn": 2,
  "mode": "mt-pool",
  "workers": 4,
  "version": "0.1.0"
}
```

`mode` is `mt-pool` when `BOILA_WORKERS > 0`, else `mt-shard`
(`go_bg` per connection).

### `GET /ready`

```json
{"status":"ready","live_conn":2,"max_conn":64,"version":"0.1.0"}
```

At the connection cap:

```json
{"status":"not_ready","reason":"max_conn","live_conn":64,"max_conn":64}
```

HTTP status **503**.

### `GET /metrics`

Prometheus 0.0.4 text. Gauges / counters:

| Metric | Type | Meaning |
|--------|------|---------|
| `boila_up` | gauge | 1 |
| `boila_build_info{version=…}` | gauge | 1 |
| `boila_databases_open` | gauge | Open DB handles |
| `boila_conn_live` | gauge | Live TCP connections |
| `boila_workers` | gauge | Pool size (`0` = go_bg) |
| `boila_sql_total` | counter | Statements executed |
| `boila_sql_ok` | counter | Statements ok |
| `boila_sql_err` | counter | Statements error |
| `boila_http_requests` | counter | HTTP requests |
| `boila_pg_queries` | counter | PG wire executions |

### `POST /sql`

Body is the raw SQL (not a JSON envelope).

```bash
curl -s -X POST localhost:6570/sql \
  --data "SELECT id, name FROM users WHERE id = 1"
```

Success (SELECT):

```json
{"ok":true,"kind":"select","cols":["id","name"],"rows":[[1,"Ана"]]}
```

Success (DML):

```json
{"ok":true,"kind":"insert","affected":2,"rows":[]}
```

`RETURNING` fills `rows`. Kinds include `update`, `delete`, `create`,
`create_index`, `create_fts`, `create_hnsw`, `create_graph`,
`drop_table`, `alter_table`, `truncate`, `begin` / `commit` /
`rollback` (with `lsn`), `create_database` / `drop_database` / `use`,
`create_user` / `grant` / `set` / …

Error (still HTTP 200):

```json
{"ok":false,"error":"undefined table","sqlstate":"42P01"}
```

Admission failure (too many connections) is HTTP **503** and SQLSTATE
`53300` on the PG side.

## Which database?

Resolution order (first hit wins):

1. `?db=<name>`
2. `X-Boila-Db` header
3. `Cookie: boila_db=`
4. Keep-alive session (default `boila` on first request)
5. `"boila"`

`USE other` updates the keep-alive session and sets
`Set-Cookie: boila_db=…; Path=/; HttpOnly`. `CREATE`/`DROP DATABASE`
do **not** switch the session.

When `BOILA_TOKEN` is set, `?db=`, header, and cookie values are
`name.<hmac16>` (HMAC-SHA256 prefix). Unsigned values are ignored.

When `BOILA_CSRF=1`, both cookie and `X-Boila-Db` must be present and
equal (double-submit). Mismatch falls back to `boila`.

```bash
# explicit database
curl -s -X POST 'localhost:6570/sql?db=analytics' \
  --data "SHOW TABLES"

# session: USE then follow-up on the same connection
curl -s -X POST localhost:6570/sql --data "USE analytics"
```

## Keep-alive and transactions

The accept loop is HTTP/1.1 keep-alive. `BEGIN` … `COMMIT` on the
**same** TCP connection share one `BoilaTxn`. Closing the socket with
an open transaction rolls it back.

Per-request arena rewind (`mem_mark` / `mem_rewind`) runs after each
statement that is not inside an open transaction.

## Auth on HTTP

| Mechanism | Header / form |
|-----------|----------------|
| Bearer token | `Authorization: Bearer <BOILA_TOKEN>` |
| Custom header | `X-Boila-Token: <BOILA_TOKEN>` |
| Basic | `Authorization: Basic base64(user:password)` |

Empty user catalog **and** empty `BOILA_TOKEN` → trust (no header
needed). Token match is treated as superuser.

There is no WebSocket endpoint (DoS surface; out of v1).
