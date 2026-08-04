# oauthbaga — language & product gaps

Probe log from apps-roadmap **№10** (OAuth proxy — the integration exam).

## O1 — TLS (G6): the whole flow is plain HTTP

**Symptom.** Provider and proxy talk over `http://`; the std client is
`http://`-only by design. A real OAuth deployment cannot exist without
https on every leg.

**Workaround.** Loopback dev profile, same as №2 registry.

**Severity.** Blocking for production; none for the probe.

**Verdict.** G6 — the designated big milestone. The client API was shaped
in №2 so TLS slots in without interface change; №10 confirms the shape
holds for redirect-chasing + form POSTs too.

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

## O4 — TokenReply / PxTokens are L3 stand-ins (again)

**Symptom.** `{status, json}` and `{ok, access, …}` structs instead of
`Result`/sum types; third package using the convention.

**Verdict.** L3 migrate target, alongside jsonrpcbaga R1 / tplbaga P2.

## O5 — go_bg carries i64/cell2 only → serial nodes

**Symptom.** Workers cannot receive a config struct or a store; the nodes
rebuild config from ports + env inside the serve loop.

**Workaround.** `cell2(port, other_port)`; env for secrets.

**Status after P1.** The state half is solved — codes/refresh/sessions
live in Postgres and each HTTP connection opens its own (fmr legacy
idiom), which is concurrency-safe. The nodes stay serial anyway because
the CSRF `states` map is still per-node in memory; moving it to the DB
(or per-worker state) is the remaining step to a pool.

**Verdict.** Q1 lineage (queuebaga) — mostly closed by P1.

## O7 — per-connection DB = one SCRAM handshake per request

**Symptom.** The fmr legacy idiom spends a connect + SCRAM exchange on
every HTTP request. Fine for a probe; real deployments want the
keep-alive/pool mode (FMR_WORKERS>0 equivalent) with one DB per worker.

**Severity.** Performance only.

**Verdict.** Comes together with the worker-pool step in O5.

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
