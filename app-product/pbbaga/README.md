# pbbaga

**Protocol Buffers wire codec + gRPC message framing** (Track **C5**).

No `.proto` compiler — you build messages by hand with field helpers.

## Wire codec

```baga
let mut b = bytes_new(0)
b = pb_field_string(b, 1, "hello")
b = pb_field_varint(b, 2, 42)
b = pb_field_sint64(b, 3, -5)   // zigzag
b = pb_field_fixed32(b, 4, x)
b = pb_field_bytes(b, 5, payload)
```

Decode with `pb_reader` → `pb_next` → `pb_take_*` / `pb_skip`.

## gRPC framing

```
[0][len BE32][protobuf]
```

```baga
let frame = grpc_encode(msg)
let g = grpc_decode(frame)   // g.payload
```

Example unary types: `HelloRequest` / `HelloReply` + `hello_rpc`.

## Unary glue

```baga
let u = grpc_hello_handle(request_frame)
grpc_write_response(fd, u)?   // headers + binary body
// or Response for H2:
let r = grpc_hello_response(request_frame)
```

## Server / bidi streaming (message layer)

```baga
let stream = hello_stream_replies("name", 3)     // server stream
let up = hello_bidi_requests("c", 2)
let down = hello_bidi(up)                        // one reply per request frame
let r = grpc_stream_next(down, 0)
```

HTTP helper: `grpc_hello_stream_response(body)`.  
Tests: `grpc_unary_test`, `grpc_bidi_test`, `pb_test` (stream_*).

## Status / metadata (Go-shaped)

Unary glue uses **statusbaga** codes (`GRPC_OK`, `GRPC_INVALID_ARGUMENT`, …)
and trailers (`grpc-status` / `grpc-message`). Attach outbound MD with
`grpc_response_with_md` (**mdtbaga**). Deadlines via **ctxbaga** +
`grpc-timeout` header (`ctx_from_grpc_timeout`).

```baga
let st = status_error(GRPC_NOT_FOUND(), "no user")
let u = grpc_unary_with_status(bytes_new(0), st)
let r = grpc_to_response(u)
```

## Unary client (not a demo)

```baga
let frame = grpc_encode(hello_request_encode("x", 1))
let r = grpc_call_unary("localhost", 50051, "/pkg.Svc/Method",
    frame, 3, mdt_new(), 5000)?
// L3 sum: CallOk(GrpcCallOk) | CallErr(Status)
match r {
    CallOk(o) => { /* o.payload, o.status_code */ },
    CallErr(s) => { /* s.code, s.message */ },
}
// handle with last: GrpcCall field
let mut c = grpc_client("localhost", 50051, 3)
c = grpc_client_call(c, "/pkg.Svc/Method", frame, mdt_new(), 0)?
```

Tests: `grpc_client_test`, `grpc_unary_test`, `grpc_bidi_test`, `pb_test`.

## Honest limits

- No proto codegen, packed repeated, or maps.
- gRPC frames start with `0x00` — never put them in `str` HTTP bodies
  (NUL truncation); use `tcp_write_bytes`.
- H2 trailers-native gRPC still approximate (status in headers).
- `pb_put_uvarint` is for non-negative values; use `pb_field_varint` /
  `pb_field_sint64` for signed.
- No `google.rpc.Status` details/`Any` blob yet.

## Run

```bash
./baga -I . -I app-product app-product/pbbaga/demo.baga
./baga -I . -I app-product tests/pb_test.baga
```
