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

## P4 — ~~status / metadata / context~~ — closed via universal packages

- **statusbaga**: codes 0–16, HTTP map, trailers, `grpc-timeout` codec
- **mdtbaga**: metadata multimap (+ reserved filter)
- **ctxbaga**: deadline / cancel / values + `ctx_from_grpc_timeout`
- Wired into `grpc_unary` (`grpc_unary_with_status`, `grpc_response_with_md`)

- **grpc_client**: unary over H1 with `CallOk`/`CallErr` (L3), metadata +
  `grpc-timeout`, client handle with `last: GrpcCall` field.

## P5 — ~~ok:i64 decode stand-ins~~ — migrated (B1)

- `GrpcMsg` → `GFrame(GrpcFrame) | GBad`
- `GrpcStreamRead` → `StreamOk(…) | StreamEnd`
- `HelloReq` → `HelloOk(HelloBody) | HelloBad`
- `HelloRep` → `ReplyOk(str) | ReplyBad`

Still open: `google.rpc.Status` details Any, client interceptors, true
H2 half-close bidi, H2 client transport.
