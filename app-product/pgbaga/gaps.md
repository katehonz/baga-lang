# pgbaga — language & protocol gaps

Probe log while building the PostgreSQL adapter. Same shape as httpdbaga/jwtbaga.

## G1 — No `Result` / sum-type errors

**Symptom.** Every fallible op returns a struct with `ok: i64` + `err: str`
(`PgConn`, `PgResult`, `PgMsgRead`, …).

**Workaround.** Convention: `ok == 1` means success; check before use.

**Severity.** Medium for ORM ergonomics.

**Verdict.** Language gap — enum payloads / Result would shrink API noise.

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

## G8 — `i64_to_dec` reimplemented

**Symptom.** No std `itoa` / int-to-string; demo and `pg.baga` carry a tiny
decimal encoder for error strings and pid printing.

**Workaround.** Local helper.

**Severity.** Low.

**Verdict.** Promote a `str_from_i64` to `std/str` someday.

## G9 — Reader buffer not preserved across `pg_query`

**Symptom.** Each `pg_query` builds a fresh `PgReader`. Fine for Simple Query
(server finishes with ReadyForQuery and no extra data). Breaks pipelining /
async Notify mid-flight.

**Workaround.** P0 only Simple Query request/response.

**Severity.** Medium for P1 pipelining / LISTEN.

**Verdict.** Put `PgReader` (or residual bytes) inside `PgConn` when adding
async messages.

## G10 — No parameterized Simple Query

**Symptom.** Callers interpolate SQL by hand → injection risk.

**Workaround.** Trust internal SQL only until Extended Query (Parse/Bind).

**Severity.** High for any user-facing ORM without P1.

**Verdict.** P1 must land before public ORM query builder.

## G11 — Slow pure PBKDF2 (high iteration counts)

**Symptom.** SCRAM with large `i=` (e.g. 4096+) is pure Baga loops over HMAC.
Connect works but is not “instant”.

**Workaround.** Acceptable for connect-once demos.

**Severity.** Low–medium for pool churn (many connects).

**Verdict.** Pooling (P2) amortizes cost; optional later C extern for PBKDF2.

## Closed / fine

- Binary sockets: `tcp_read_exact` / `tcp_write_bytes` sufficient (same path as h2).
- `bytes` + `hmac_sha256_b` handle NULs in proofs.
- Live SCRAM against PostgreSQL 15.18 verified (`tests/pg_test.baga`).
