# pgbaga — language & protocol gaps

Probe log while building the PostgreSQL adapter. Same shape as httpdbaga/jwtbaga.

## G1 — ~~No `Result` / sum-type errors~~ — **PgResult migrated (B1)**

**Was.** `PgResult { ok, err, … }` stand-in.

**Now.** `PgOk(PgRows) | PgErr(PgFail)` with accessors `pg_ok` / `pg_err` /
`pg_conn_of` / `pg_nrows` / `pg_cell` / …. `PgConn` still uses `ok:i64`
(connection state, not a Result). Wire parsers (`PgMsgRead`) keep `ok` as
frame validity.

**Severity.** Closed for query results.

## G2 — Structs are by-value; connection must be threaded

**Symptom.** `pg_query` returns `PgResult.conn` with updated `tx_status`. Callers
must write `c = r.conn` after every query or they keep a stale copy.

**Workaround.** Documented; demo/tests always rebind.

**Severity.** Medium (easy to misuse).

**Verdict.** Language design — no mutable shared objects / no `&mut`. Acceptable;
ORM wrapper can hide it.

## G3 — No map type for ErrorResponse fields / ParameterStatus

**Symptom.** Error fields walked into a single `"sev|code|msg"` string;
ParameterStatus only keeps `server_version`.

**Workaround.** Parallel `Vec`s or concatenated strings.

**Severity.** Low for P0; medium for rich driver UX.

**Verdict.** Same as httpdbaga (no map). Promote when maps exist.

## G4 — SCRAM needs PBKDF2; not in std/crypto

**Symptom.** Implemented `pbkdf2_hmac_sha256` inside `pg_scram.baga` on top of
`hmac_sha256_b`.

**Workaround.** App-local pure function.

**Severity.** Low.

**Verdict.** Candidate to promote to `std/crypto` (also useful for general KDF).

## G5 — SASLprep / stringprep missing

**Symptom.** SCRAM `Normalize(password)` is identity (raw UTF-8 bytes).

**Workaround.** ASCII passwords only for correctness guarantees.

**Severity.** Low for local/dev; high for production Unicode passwords.

**Verdict.** Defer; document. Full SASLprep is large.

## G6 — No TLS / SSLRequest

**Symptom.** Cannot talk to servers with `hostssl` only.

**Workaround.** Local trust/SCRAM over loopback.

**Severity.** Blocker for remote production use.

**Verdict.** Blocked on `std/net` TLS (same as httpdbaga). Logged, not fixed here.

## G7 — MD5 auth unsupported

**Symptom.** Auth kind 5 returns a clear error. Local servers use SCRAM.

**Workaround.** Configure SCRAM or trust.

**Severity.** Low (MD5 deprecated upstream).

**Verdict.** Won't implement unless needed; prefer SCRAM.

## G8 — ~~`i64_to_dec` reimplemented~~ — CLOSED

**Symptom.** No std `itoa` / int-to-string; demo and `pg.baga` carry a tiny
decimal encoder for error strings and pid printing.

**Closed.** `std/str` now has `int_to_str` (handles INT64_MIN too); `pg.baga`
delegates (`i64_to_dec` kept as a thin alias for existing callers).

## G9 — ~~Reader buffer not preserved across `pg_query`~~ — CLOSED

**Symptom.** Each `pg_query` builds a fresh `PgReader`. Fine for Simple Query
(server finishes with ReadyForQuery and no extra data). Breaks pipelining /
async Notify mid-flight.

**Closed.** `PgConn.reader` (a `PgReader`) now carries the buffered socket
state across queries; every `PgResult.conn` returns it updated. LISTEN/NOTIFY
dispatch is still P2, but residual bytes are no longer dropped.

## G10 — ~~No parameterized Simple Query~~ — CLOSED

**Symptom.** Callers interpolate SQL by hand → injection risk.

**Closed.** Extended Query (Parse/Bind/Describe/Execute/Sync) landed:
`pg_query_params` + named `pg_prepare` / `pg_exec_prepared`. Live
injection-safety check in `tests/pg_test.baga`.

## G11 — Slow pure PBKDF2 (high iteration counts)

**Symptom.** SCRAM with large `i=` (e.g. 4096+) is pure Baga loops over HMAC.
Connect works but is not “instant”.

**Workaround.** Acceptable for connect-once demos.

**Severity.** Low–medium for pool churn (many connects).

**Verdict.** Pooling (P2) amortizes cost; optional later C extern for PBKDF2.

## G12 — Struct field of a struct declared later compiles to `int`

**Symptom.** Embedding `PgReader` in `PgConn` before `PgReader` is declared
passes `--check`/`--lib` silently, but the C backend emits the field as `int`
→ gcc error at the first field access.

**Workaround.** Declare embedded structs before their embedders (done in
`pg.baga`).

**Severity.** Medium — silent accept then C failure is a confusing trap.

**Verdict.** Checker should reject an unknown struct in field position
(`непознат struct '<name>'` already exists for literals — reuse it).

## G13 — `json_parse` is lenient; no strictness signal

**Symptom.** `std/json` `json_parse("{k")` returns a best-effort document
(root tag >= 0), so a validity check built on it accepts malformed JSON —
dangerous before sending user JSON to Postgres.

**Workaround.** `json_strict_valid` added to `std/json` (strict RFC 8259,
additive — the lenient parser is unchanged); `pg_json_valid` /
`sql_json[b]` use it.

**Severity.** Was medium for any JSON-ingesting product.

**Verdict.** Keep both: lenient parse for recovery, strict for validation.

## Closed / fine

- Binary sockets: `tcp_read_exact` / `tcp_write_bytes` sufficient (same path as h2).
- `bytes` + `hmac_sha256_b` handle NULs in proofs.
- Live SCRAM against PostgreSQL 15.18 verified (`tests/pg_test.baga`).
- JSON/JSONB tables: create/insert/select round-trip live-verified, incl.
  OID detection, UTF-8 docs, invalid-JSON rejection, `json` verbatim vs
  `jsonb` normalized output.
- Message-size guard: `pg_read_msg` rejects `len` outside `[4, 2^30-1]`
  (corrupt/hostile streams fail instead of buffering).
- CancelRequest live-verified: delivered on a fresh connection; idle
  backends ignore it (documented Postgres behavior — the connection
  survives; a running query is what gets canceled).
- std/net memfd trap: `SYS_write` advances the memfd offset — a later
  `read()` starts at EOF. Always `lseek(0)` back (or use pwrite/pread,
  which never move the offset — that is why the staging code uses them).
