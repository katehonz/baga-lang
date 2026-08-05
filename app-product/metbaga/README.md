# metbaga

Prometheus **text exposition** metrics for Baga services (Track **C2**).

```baga
let m = met_new()
let reqs = met_counter(m, "http_requests_total", "HTTP requests")
met_inc(m, reqs)
let body = met_render(m)   // for GET /metrics
```

Supports **counter** and **gauge** (i64 values). No label sets in MVP —
encode dimensions in the metric name if needed (`http_requests_get_total`).

See also: `logbaga`, `cloudbaga` demo.
