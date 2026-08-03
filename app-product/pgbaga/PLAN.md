# pgbaga — PostgreSQL wire adapter (plan)

Date: 2026-08-03
Status: P0 + P1 done (Extended Query, prepared statements, JSON/JSONB tables,
        typed getters, conn-scoped reader); P2 pending
Goal: native PostgreSQL client for Baga (wire protocol v3), foundation for a future ORM + framework.

## Motivation (from Go / Rust)

| Layer | Go (pgx) | Rust (rust-postgres) | pgbaga |
|-------|----------|----------------------|--------|
| Wire | `pgproto3` | `postgres-protocol` | `pg_proto.baga` |
| Auth / connect | `pgconn` | client handshake | `pg_scram.baga` + `pg.baga` |
| Types | `pgtype` | `postgres-types` | text cells first; OIDs kept |
| High API | `pgx.Conn` | `Client` | `pg.baga` |
| Pool / ORM | `pgxpool` + sqlc/GORM | pools + diesel/sqlx | **later** |

We intentionally mirror that split so the ORM sits on a stable low-level client.

## Local facts

- Host Postgres: `127.0.0.1:5432`, `password_encryption = scram-sha-256`, host auth = SCRAM.
- Test role: `bagatest` / `pas+123` (created for this product).
- Baga already has: `tcp_*_bytes`, `bytes`, `sha256`/`hmac`, `base64`, `random_bytes`.
- No TLS in `std/net` → plaintext only (same honesty limit as httpdbaga).

## Phases

### P0 — MVP (this iteration)

1. **Wire framing** — StartupMessage, typed messages (`type + i32 len + body`), big-endian helpers.
2. **Auth** — SCRAM-SHA-256 (SASL): PBKDF2-HMAC-SHA256, client-first / server-first / client-final / verify server signature. Also cleartext (`auth=3`) if server asks.
3. **Session finish** — drain ParameterStatus, BackendKeyData, ReadyForQuery; Terminate on close.
4. **Simple Query** — `Query` → RowDescription / DataRow* / CommandComplete / ErrorResponse → ReadyForQuery.
5. **API**
   - `pg_connect(host, port, user, password, database) -> PgConn !Net !IO !Random`
   - `pg_close(conn) -> i64 !IO !Net`
   - `pg_query(conn, sql) -> PgResult !IO !Net`  (SELECT and DML)
   - accessors: `pg_ok`, `pg_err`, `pg_tag`, `pg_nrows`, `pg_ncols`, `pg_colname`, `pg_cell`, `pg_isnull`
6. **Demo + test** against live Postgres; README + gaps.md.
7. Fix language friction found while building (log in gaps.md; only fix if trivial and local).

### P1 — ORM-ready — DONE

- Extended Query: Parse / Bind / Describe / Execute / Sync (parameterized `$1`). ✅
- Named prepared statements + close. ✅
- Explicit transactions helpers (`BEGIN`/`COMMIT`/`ROLLBACK` wrappers + `tx_status`). ✅
  (raw `BEGIN`/`COMMIT` + `tx_status` tracked on every ReadyForQuery)
- Typed getters (`pg_cell_i64`, bool, …) from text OIDs. ✅ (`i64`/`bool`/`f64`/JSON)
- JSON/JSONB tables: OID detection, strict validation (`std/json`
  `json_strict_valid`), `pg_param_json` binds, `ormbaga` `sql_json[b]` literals. ✅
- Reader buffer moved into `PgConn` (G9 closed; async dispatch still P2). ✅
- Notice / Notification handling (LISTEN/NOTIFY) → P2.

### P2 — framework scale

- Connection pool (`std/par` channels). ✅ (lives in `ormbaga/pool.baga`)
- CancelRequest (second TCP connection + BackendKeyData). ✅ (`pg_cancel`)
- Hostnames + timeouts: `getaddrinfo` in `std/net`, `pg_connect_to`,
  SO_RCVTIMEO/SO_SNDTIMEO, TCP_NODELAY, SO_KEEPALIVE. ✅
- COPY IN/OUT.
- SSLRequest (blocked on TLS in std).
- Binary format encode/decode for hot types.
- Prepared-statement cache + row streaming (PortalSuspended).

## Non-goals (P0)

TLS, MD5 auth (deprecated), Kerberos/GSS, replication, pipelining, pool, full type system, SQL builder/ORM.

## Files

| File | Role |
|------|------|
| `pg_proto.baga` | pure encode/decode, message builders/parsers |
| `pg_scram.baga` | pure PBKDF2 + SCRAM-SHA-256 proofs |
| `pg.baga` | connect / query / close (effects) |
| `demo.baga` | CLI demo (`SELECT 1`, create/drop temp) |
| `password.txt` | local credentials hint (not imported by library) |
| `gaps.md` | language + protocol gaps |
| `README.md` | API + run/test |
| `tests/pg_test.baga` | live integration test |

## Effects

- Pure: `pg_proto`, `pg_scram` (except nonce generation lives in `pg.baga` via `random_bytes`).
- `!Net !IO`: socket I/O.
- `!Random`: SCRAM client nonce at connect.

## Error model

No `Result` type yet → struct fields: `PgConn.ok` / `PgConn.err`, `PgResult.ok` / `PgResult.err` (SQLSTATE + message when present).

## Success criteria (P0)

1. `./baga tests/pg_test.baga` prints `pg_test: all passed` against local Postgres.
2. Demo connects as `bagatest`, runs `SELECT 1`, prints row.
3. README documents limits honestly (no TLS, Simple Query only, text format).
