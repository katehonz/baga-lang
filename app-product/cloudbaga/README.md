# cloudbaga

**12-factor cloud demo** — Track C flagship probe (C1–C4).

| Endpoint | Role |
|----------|------|
| `GET /healthz` | Liveness |
| `GET /readyz` | Readiness (503 after SIGTERM) |
| `GET /metrics` | Prometheus text (`metbaga`) |
| `GET /` | Hello JSON |

Graceful shutdown: `signal_watch(SIGTERM|SIGINT)` + `poll_wait` accept
loop so the process notices the signal between connections, flips ready
off, logs a JSON line (`logbaga`), and exits.

```bash
PORT=18080 ./baga -I . -I app-product app-product/cloudbaga/server.baga
# elsewhere:
curl -s localhost:18080/healthz
curl -s localhost:18080/metrics
kill -TERM <pid>
```

Env: `PORT` (default 18080).
