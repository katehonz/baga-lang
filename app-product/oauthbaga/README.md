# oauthbaga

**OAuth 2.0 proxy** for Baga — apps-roadmap **№10**, the integration
exam: std HTTP client (№2) + jwtbaga + cookie sessions, pages rendered
by tplbaga (№7). Two nodes on loopback: an authorization **provider**
and a confidential-client **proxy**. Dev profile: auto-approve, plain
HTTP (TLS waits on G6), one registered client.

## Endpoints

| Node | Route | Meaning |
|------|-------|---------|
| provider | `GET /oauth/authorize` | 302 with a one-time `code` (+ `state` echo) |
| provider | `POST /oauth/token` | `authorization_code` / `refresh_token` grants |
| provider | `GET /api/me` | Bearer-JWT-protected resource |
| proxy | `GET /login` | 302 to authorize (CSRF `state` stored) |
| proxy | `GET /callback` | exchanges the code server-to-server, sets `sid` cookie |
| proxy | `GET /` | session page (transparent token refresh) |
| proxy | `GET /logout` | drops session, clears cookie |

Tokens: HS256 JWT access (jwtbaga) with `sub`/`scope`/`iat`/`exp`;
opaque random-hex codes and refresh tokens; refresh grant **rotates**
(old token dies). Errors are RFC 6749-shaped JSON.

## API

```baga
struct OAuthCfg { client_id, client_secret, jwt_secret, redirect_uri, provider_base, …ttl_s }
fn oauth_cfg(provider_port, proxy_port, jwt_secret) -> OAuthCfg

// provider
fn oa_issue_code(st, cfg, client_id, sub, scope) -> str !Time !Random
fn oa_token(st, cfg, form_body) -> TokenReply !Time !Random
fn oa_bearer_payload(cfg, authz_header) -> str !Time

// proxy
fn px_exchange_code(cfg, code) -> PxTokens !Net !IO !Random
fn px_refresh(cfg, refresh) -> PxTokens !Net !IO !Random

// serve (ctx = cell2(port, other_port); serial accept, shared store)
fn oauth_provider_serve(ctx: i64) -> i64 !Net !IO !Time !Random
fn oauth_proxy_serve(ctx: i64) -> i64 !Net !IO !Time !Random
```

## Run

```bash
cd app-product/oauthbaga
BAGA=../../baga sandak build
../../baga -I ../.. -I .. demo.baga     # provider :18691, proxy :18690
# open http://127.0.0.1:18690/ and follow "log in"

./baga -I . -I app-product tests/oauth_test.baga          # in-memory
OAUTH_PG=1 PGDATABASE=baga_oauth \
  ./baga -I . -I app-product tests/oauth_pg_test.baga     # Postgres
```

Env: `OAUTH_PORT` (proxy), `OAUTH_PROVIDER_PORT`, `OAUTH_SECRET`, and
for the Postgres backend `OAUTH_PG=1` + `PGHOST/PGPORT/PGUSER/PGPASSWORD/
PGDATABASE` (migrations apply at boot; one DB connection per HTTP
connection, the fmr legacy idiom). In PG mode every HTTP connection also
runs on its own `go_bg` worker — codes, refresh tokens, sessions and CSRF
states are all in `oauth_*` tables, so nothing is shared between threads.
In dev (in-memory) mode both nodes stay serial.

## Honest limits

See [`gaps.md`](gaps.md): plain HTTP only (O1/TLS-G6); no URL
percent-coding in std (O2); the in-memory backend keeps JSON records in
maps (L4, O3); `TokenReply`/`PxTokens` are L3 stand-ins (O4); nodes stay
serial — the CSRF `states` map is the last thing keeping them from a
worker pool (O5); per-connection DB costs a SCRAM handshake per request
(O7).
