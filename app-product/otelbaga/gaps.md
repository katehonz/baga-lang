# otelbaga — gaps

## O1 — no OTLP / Jaeger export

IDs only. Shipping spans to a collector is a separate service.

## O2 — no tracestate

W3C `tracestate` header ignored.

## O3 — random quality

`random_bytes` for ids; not crypto-bound to a specific RNG seed story.
