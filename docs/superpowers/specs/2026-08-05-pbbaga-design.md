# pbbaga (C5): protobuf wire codec + gRPC message framing

Date: 2026-08-05. Status: **shipped** (codec + framing MVP).
Parent: `docs/superpowers/plans/2026-08-05-cloud-storage-direction.md` step C5.

## Goal

Microservice interop foundation: **protobuf wire encoding** (varint +
length-delimited) and **gRPC length-prefixed frames**, so a Baga service
can speak the same on-the-wire shape as other stacks. Full gRPC-over-H2
server is a thin layer on existing `httpdbaga` H2; MVP ships codec + frame
+ unary echo probe (HTTP/1.1 body carries the same frame — documented as
“frame-compatible, not full gRPC transport”).

## Wire codec (`pb.baga`)

| Wire type | Value | API |
|-----------|-------|-----|
| Varint | 0 | `pb_put_varint` / `pb_get_varint` |
| 64-bit | 1 | `pb_put_fixed64` / `pb_get_fixed64` (LE) |
| Len-delim | 2 | `pb_put_bytes` / `pb_get_bytes`, strings via UTF-8 `str` |
| 32-bit | 5 | `pb_put_fixed32` / `pb_get_fixed32` (LE) |

Field key = `(field_number << 3) | wire_type` as varint.

```baga
// Build: HelloRequest { string name = 1; int64 n = 2; }
let mut b = bytes_new(0)
b = pb_field_string(b, 1, "бага")
b = pb_field_varint(b, 2, 42)

// Read
let r = pb_reader(b)
// loop pb_next → tag/wire → get_*
```

ZigZag optional helpers for `sint32`/`sint64` (`pb_zigzag` / `pb_unzigzag`).

Honest: no proto compiler, no maps/packed repeated codegen — hand-built
messages only. Unknown fields skippable via `pb_skip`.

## gRPC framing (`grpc.baga`)

Unary message on the wire (gRPC length-prefixed message):

```
[1 byte compressed-flag=0][4 bytes big-endian length][protobuf payload]
```

```baga
fn grpc_encode(msg: bytes) -> bytes
fn grpc_decode(frame: bytes) -> GrpcMsg   // ok, compressed, payload
```

Status trailers helpers as pure strings (`grpc-status: 0`, etc.) for
responders that set HTTP trailers later.

## Probe

- `tests/pb_test.baga` — varint edge cases, field round-trip, skip unknown,
  grpc frame round-trip, golden against known hex.
- `demo.baga` — print encoded Hello hex + decode.

## Out of scope (v1)

- `.proto` compiler / codegen
- Streaming RPCs, HPACK path-forced full gRPC stack (H2 exists; glue later)
- grpc-web browser path
