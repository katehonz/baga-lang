# ctxbaga

**Context** lite — Go's `context` for deadlines, cancel, and string values.

```baga
let mut c = ctx_background()
c = ctx_with_timeout(c, 5000)?          // 5s deadline (monotonic)
c = ctx_with_value(c, "user", "42")
ctx_value(c, "user")                    // "42"
ctx_require(c, "user") catch !NotFound(k) => k   // M20
if ctx_done(c)? == 1 {
    let err = ctx_err(c)?               // Status Canceled / DeadlineExceeded
}
c = ctx_cancel(c)
// from inbound gRPC:
c = ctx_from_grpc_timeout(c, "100m")?
```

Depends on **statusbaga** for error codes. No child-tree auto-cancel yet.
