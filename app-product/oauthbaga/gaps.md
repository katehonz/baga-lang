# oauthbaga — language & product gaps

Probe log from apps-roadmap **№10** (OAuth proxy — the integration exam).

## O1 — TLS (G6): ~~the whole flow is plain HTTP~~ CLOSED for the client (T8)

**Symptom (historical).** Provider and proxy talk over `http://`; the std
client was `http://`-only.

**Closed (T8, 2026-08-04).** `std/net/http_client` accepts `https://` over
the pure-Baga TLS 1.3 stack (`tls_connect` + app traffic). Mock proof:
`tests/std/https_test.baga` against `openssl s_server -tls1_3 -www` with a
self-signed cert (empty trust anchor = accept self-signed). No real OAuth
provider account is required or used.

**Still open for a full product deploy.** oauthbaga nodes themselves still
serve plain HTTP; putting them on TLS needs a server-side stack (out of
scope — client G6 was the roadmap blocker). Real public CAs need a trust
store (also out of scope for the probe).

## O2 — no URL percent-coding anywhere in std

**Symptom.** `redirect_uri`, `state`, form bodies travel raw. It works
because the generated values carry no `&`/`=`/`%`, but a user-supplied
`sub` or scope with a space/`&` would corrupt the query/form.

**Workaround.** Controlled alphabet in the dev profile.

**Severity.** Medium-high for anything user-facing.

**Verdict.** Small `std/net/url` (`url_encode`/`url_decode`) — local,
high-leverage; the third time query parsing was hand-rolled (httpdbaga
G7, oa_form here).

## O3 — JSON records in Map<str,str> instead of struct values (L4)

**Symptom.** The in-memory backend keeps codes / refresh tokens / sessions
as JSON strings in maps; every read re-parses (`oa_rec_str`/`oa_rec_num`).
`Map<str, Session>` or `Vec<struct>` would delete the codec entirely.

**Status after P1.** The Postgres backend stores real columns and both
backends meet at typed rows (`OaCode`/`OaTok`/`PxSess`) — the JSON codec
is now contained to the dev backend.

**Verdict.** L4 — same lineage as tplbaga P3 / queuebaga disk structs.
**Closed 2026-08-04:** `Vec<struct>` (tplbaga P3, bagadecimal D7) and
`Map<K,struct>` both shipped — `Map<str, Session>` is now expressible.
Rewriting the dev backend is optional cleanup, not a blocker.

## O4 — TokenReply / PxTokens are L3 stand-ins (again)

**Symptom.** `{status, json}` and `{ok, access, …}` structs instead of
`Result`/sum types; third package using the convention.

**Verdict.** **Unblocked 2026-08-05** — L3 shipped (`enum Res { Ok(i64),
Err(str) }` + exhaustive match); migration of the stand-in structs is
optional, alongside jsonrpcbaga R1 / tplbaga P2.

## O5 — go_bg carries i64/cell2 only (CLOSED by P1, with a scar)

**Symptom.** Workers cannot receive a config struct or a store; the nodes
rebuild config from ports + env inside the serve loop.

**Workaround.** `cell2(port, other_port)`; env for secrets.

**Status after P1.** Closed: codes/refresh/sessions *and* the CSRF states
(`oauth_states`) live in Postgres, and in PG mode every HTTP connection
runs on its own `go_bg` worker with its own DB connection — nothing is
shared between threads. Dev mode stays serial by design.

**The scar.** The first bg handlers forgot `tcp_close(fd)` after
`http_respond` — with Connection: close the client reads to EOF, so an
unclosed fd hangs every client until its read timeout (the nested
server-to-server exchange then blows its 5 s budget mid-flight). Every
future baga server must close the fd after responding.

**Verdict.** Q1 lineage (queuebaga) — closed by P1.

## O7 — per-connection DB = one SCRAM handshake per request

**Symptom.** The fmr legacy idiom spends a connect + SCRAM exchange on
every HTTP request. Fine for a probe; real deployments want the
keep-alive/pool mode (FMR_WORKERS>0 equivalent) with one DB per worker.

**Severity.** Performance only.

**Verdict.** Open for real deployments: a pool of long-lived DB
connections (FMR_WORKERS-style), now that O5's worker model is in place.

## Closed / fine

- The std HTTP client survived its hardest use yet: redirect chasing,
  form POST with headers, two roles in one process — zero client bugs.
- jwtbaga needed no change; `exp` check + `jwt_decode` covers Bearer.
- Same-second refresh yields an identical JWT (same claims → same sig):
  correct behavior, the test asserts rotation via the random refresh
  token instead.
- tplbaga as a dependency works out of the box (first cross-product
  import in the series) — №7 feeds №10 as rule 4 intended.
- httpdbaga grew one line: reason phrase for 302 Found.
