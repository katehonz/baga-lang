# otelbaga

**W3C Trace Context + OTLP/JSON export lite** (Track **C8**).

## Trace context

```baga
let t = otel_parse_traceparent("00-4bf9…-00f0…-01")
let root = otel_new_trace()?
let child = otel_child(root)?
let from = otel_from_header(http_header(req, "traceparent"))?
```

## Spans + export

```baga
let sp = otel_span_now("GET /hello", ctx, 2)?   // kind 2 = SERVER
sp = otel_span_end(sp)?
let body = otel_span_to_otlp_json(sp, "myservice")
otel_export_span_file("/var/log/spans.ndjson", sp, "myservice")?
// optional collector:
// otel_export_http("http://localhost:4318/v1/traces", body, 5)?
```

OTLP JSON uses base64 `traceId`/`spanId` plus hex in attributes for humans.
No metrics/logs pipeline; no batching daemon.
