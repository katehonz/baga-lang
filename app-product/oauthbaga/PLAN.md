# oauthbaga — OAuth proxy (plan)

Date: 2026-08-04
Status: P0 done (provider + proxy, live full cycle over plain HTTP)
Goal: apps-roadmap **№10** — the integration exam: HTTP client (№2) +
jwtbaga + sessions, pages through tplbaga (№7 feeds №10, rule 4).

## Phases

### P0 ✅

1. Provider node: `GET /oauth/authorize` (auto-approve dev profile,
   one-time codes), `POST /oauth/token` (authorization_code +
   refresh_token grants, rotation), `GET /api/me` (Bearer JWT guard).
   RFC 6749-shaped JSON errors (invalid_client/grant/request,
   unsupported_grant_type).
2. Proxy node: `/login` (state CSRF) → `/callback` (code exchange over
   the std HTTP client — a real server-to-server call) → cookie session
   → `/` with transparent refresh → `/logout`.
3. Two serial nodes (kvbaga idiom, `cell2` ports): no self-accept
   deadlock; state is per-node `Map<str,str>` of JSON records (L4).
4. Pages rendered by tplbaga (`{{ sub }}` / `{% if sub %}`).
5. `tests/oauth_test.baga` — live full cycle (authorize → exchange →
   bearer → protected → refresh/rotation → browser flow → logout).

### P1 — the Postgres leg ("всичко заедно")

- `pgbaga`/`ormbaga` persistence: `oauth_codes` / `oauth_refresh` /
  `oauth_sessions` (migrations like apps/registry).
- Then multi-worker proxy (fmrbaga pool) — sessions survive across
  workers because they live in the DB, not in one serial node.

### P2 — the TLS leg (G6)

- https everywhere; talk to real external providers.
- PKCE (code verifier/challenge), a real consent screen instead of the
  `sub=` dev shortcut, token revocation endpoint.

## Success criteria (P0) — met

1. `sandak build` oauthbaga OK (deps: std, httpdbaga, jwtbaga, tplbaga).
2. `oauth_test` all passed (live loopback, redirects followed by hand).
3. `demo.baga` boots both nodes; a browser can complete the flow.
