# pbbaga — gaps

## P1 — no protoc / codegen

Messages are hand-encoded. Fine for probes; real services want a
`.proto` → `.baga` generator.

## P2 — ~~full gRPC transport~~ — mostly closed

`grpc_unary.baga` + `http_response_bytes` / `http_set_trailer` +
`h2_respond` trailers (HEADERS END_STREAM). H1 uses
`grpc_write_response`; H2 uses `grpc_hello_response` → `h2_respond`.

## P3 — packed repeated / maps / groups

Not implemented. Skip unknown fields works for forward compatibility.
