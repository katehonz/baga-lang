# pbbaga — gaps

## P1 — no protoc / codegen

Messages are hand-encoded. Fine for probes; real services want a
`.proto` → `.baga` generator.

## P2 — ~~full gRPC transport~~ — partial

`grpc_unary.baga` handles unary Hello over binary HTTP bodies
(`tcp_write_bytes` — frames start with 0x00 so they cannot use
`Response.body` str). H2 trailers-native path still open.

## P3 — packed repeated / maps / groups

Not implemented. Skip unknown fields works for forward compatibility.
