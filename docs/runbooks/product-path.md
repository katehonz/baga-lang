# Product path — API + registry gRPC + metrics + graceful stop

**Phase 3 exit criteria:** one documented path that runs a product HTTP API,
hits metrics/ready, and (for registry) a gRPC call; process stops on SIGTERM
with readiness flipped.

**Reports (accounting HTML → Excel/PDF):** see [report.md](report.md).

## 1. Product JSON API (`apps/api`)

```bash
# prerequisites: Postgres (bagatest / baga_orm) — see apps/api/README.md
export PORT=8080 FMR_WORKERS=0 FMR_LOG=1 FMR_CORS='*' FMR_JWT_SECRET=dev-secret
export PGHOST=127.0.0.1 PGPORT=5432 PGUSER=bagatest PGPASSWORD='pas+123' PGDATABASE=baga_orm

./baga -I . -I app-product apps/api/start.baga
```

Smoke:

```bash
curl -s localhost:8080/health
curl -s localhost:8080/ready
curl -s localhost:8080/metrics | head
curl -s localhost:8080/openapi.json | head -c 200
TOKEN=$(curl -s -X POST localhost:8080/v1/auth/token \
  -H 'Content-Type: application/json' -d '{"sub":"ada"}' \
  | sed 's/.*"access_token":"//;s/".*//')
curl -s localhost:8080/v1/me -H "Authorization: Bearer $TOKEN"
```

Graceful shutdown:

```bash
# in another terminal
kill -TERM $(pgrep -f 'apps/api/start.baga' | head -1)
# /ready returns 503 while draining; process exits accept loop
```

## 2. Registry dual protocol (HTTP JSON + gRPC)

```bash
export PORT=8090 PGDATABASE=baga_registry
# same PGUSER/PASSWORD/HOST as above
./baga -I . -I app-product apps/registry/start.baga
```

HTTP:

```bash
curl -s localhost:8090/health
curl -s localhost:8090/ready
curl -s localhost:8090/metrics | head
curl -s -X POST localhost:8090/v1/packages \
  -H 'Content-Type: application/json' \
  -d '{"name":"demo","version":"0.1.0","description":"runbook"}'
curl -s localhost:8090/v1/packages/demo
```

gRPC (Baga client; same port):

```baga
// see apps/registry/README.md and tests/registry_grpc_test.baga
// POST /regbaga.Registry/GetPackage  Content-Type: application/grpc
```

Automated: `tests/registry_grpc_test.baga`, `tests/registry_test.baga`.

## 3. Signals & probes

| Signal | Effect |
|--------|--------|
| SIGTERM / SIGINT | `fmr_run` stops accepting; workers get stop; `/ready` → 503 via `fmr_shutting_down` |
| `/health` | Liveness (always ok if process answers) |
| `/ready` `/readyz` | DB ping + not shutting down |
| `/metrics` | Prometheus text: `process_up`, `fmr_ready`, service info gauge |

## 4. Related tests

| Test | Role |
|------|------|
| `tests/api_test.baga` | OpenAPI + models (optional PG) |
| `tests/registry_test.baga` | Live HTTP registry |
| `tests/registry_grpc_test.baga` | gRPC GetPackage |
| `tests/cloud_test.baga` | cloudbaga probes (reference) |
| `tests/std/signal_test.baga` | signal builtins |
