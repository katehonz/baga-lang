# statusbaga

**gRPC status codes + Status** — Go's `codes` / `status` packages in pure Baga.

```baga
let s = status_error(GRPC_INVALID_ARGUMENT(), "name required")
status_code_name(s.code)          // "InvalidArgument"
status_http(s.code)               // 400
status_string(s)                  // "rpc error: code = InvalidArgument desc = …"
status_trailer_code(s)            // "3"  → grpc-status trailer
status_check(s) catch !Rpc(msg) => 1     // M20: raise if not OK
grpc_timeout_encode_ms(5000)      // "5S"
grpc_timeout_decode_ms("100m")    // 100
```

Codes 0–16 match [google.rpc.Code](https://github.com/googleapis/googleapis/blob/master/google/rpc/code.proto).
No protobuf `google.rpc.Status` wire blob yet (P1: details Any).
