# otelbaga

**W3C Trace Context lite** (Track **C8** subset — no OTLP export).

```baga
let t = otel_parse_traceparent("00-4bf92f3577b34da6a3ce929d0e0e4736-00f067aa0ba902b7-01")
let tp = otel_format(t)          // round-trip
let root = otel_new_trace()?     // random ids
let child = otel_child(root)?    // same trace_id, new span
let from = otel_from_header(http_header(req, "traceparent"))?
```

Use `trace_id` / `span_id` in `logbaga` fields for correlation. Full OpenTelemetry
SDK/export remains deferred.
