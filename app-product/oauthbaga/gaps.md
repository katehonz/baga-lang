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

**Symptom.** Codes / refresh tokens / sessions are JSON strings in maps;
every read re-parses (`oa_rec_str`/`oa_rec_num`). `Map<str, Session>` or
`Vec<struct>` would delete the codec entirely.

**Verdict.** L4 — same lineage as tplbaga P3 / queuebaga disk structs.

## O4 — TokenReply / PxTokens are L3 stand-ins (again)

**Symptom.** `{status, json}` and `{ok, access, …}` structs instead of
`Result`/sum types; third package using the convention.

**Verdict.** L3 migrate target, alongside jsonrpcbaga R1 / tplbaga P2.

## O5 — go_bg carries i64/cell2 only → serial nodes + in-memory state

**Symptom.** Workers cannot receive a config struct or a store; the nodes
rebuild config from ports + env inside the serve loop, and shared state
forces serial accept (kvbaga idiom). A multi-worker proxy needs the state
in Postgres — exactly the P1 leg.

**Workaround.** `cell2(port, other_port)`; env for secrets.

**Severity.** Structural until L5/struct-carrying channels or DB-backed
state.

**Verdict.** Q1 lineage (queuebaga). P1 closes it the honest way:
pgbaga/ormbaga tables + fmrbaga-style worker pool.

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
