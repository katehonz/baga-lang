# apps/api — example product on fmrbaga

A **small, real** JSON API in the spirit of [Lucky](https://luckyframework.org/)
(Crystal): conventions, thin actions, models, one entrypoint.

Not a toy demo — the same layout you should use for your own product.

```
apps/api/
  start.baga           # main: migrate → listen
  routes.baga          # fmr_build_app + fmr_dispatch (route table)
  models/user.baga     # table helpers (Lucky model vibe)
  actions/
    system.baga        # health, ready, meta, openapi
    auth.baga          # token, me
    users.baga         # CRUD
  .env.example
```

## Run

From repo root (needs Postgres `baga_orm` + `bagatest` — see ormbaga):

```bash
./scripts/fmr-run
# or:
FMR_WORKERS=4 FMR_LOG=1 FMR_CORS='*' ./baga apps/api/start.baga
```

```bash
curl -s localhost:8080/health
curl -s -D- localhost:8080/health -o /dev/null | grep -i request-id
TOKEN=$(curl -s -X POST localhost:8080/v1/auth/token -H 'Content-Type: application/json' \
  -d '{"sub":"ada"}' | sed 's/.*"access_token":"//;s/".*//')
curl -s localhost:8080/v1/users -H "Authorization: Bearer $TOKEN"
curl -s -X POST localhost:8080/v1/posts -H "Authorization: Bearer $TOKEN" \
  -H 'Content-Type: application/json' \
  -d '{"user_id":1,"title":"hi","body":"from fmr"}'
curl -s 'localhost:8080/v1/posts?limit=10&offset=0' -H "Authorization: Bearer $TOKEN"
```

## How to add an action (Lucky-style)

1. Pick a free route id in `routes.baga`
2. Register `fmr_route(app, "GET", "/v1/things", RID_THINGS)`
3. Implement `act_things_index(ctx, db) -> FmrOut` in `actions/…`
4. Branch in `fmr_dispatch`

Or: `./scripts/fmr-gen-action things index GET /v1/things`

## Config (env)

See `app-product/fmrbaga` README: `PORT`, `FMR_WORKERS`, `FMR_LOG`, `FMR_CORS`,
`FMR_JWT_SECRET`, `PG*`.

## Framework vs app

| Package | Role |
|---------|------|
| `app-product/fmrbaga` | framework (router, JSON, auth, serve) |
| `apps/api` | **your product** (actions + models + routes) |

Keep business logic in `apps/*`, not inside fmrbaga.
