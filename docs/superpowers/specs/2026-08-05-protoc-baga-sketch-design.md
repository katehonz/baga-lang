# Design: protoc → baga sketch (Phase 5 / pbbaga P1)

Date: 2026-08-05  
Status: **sketch shipped** — not a full protoc plugin  
Goal: reduce hand-written PB encode/decode for product messages without
claiming full protobuf / gRPC codegen parity.

## Problem

`pbbaga` is hand-built wire codec. Registry/Hello messages re-encode field
numbers by eye. Real services need a **mechanical** path from `.proto` to
Baga helpers so goldens and apps stay aligned.

## Non-goals (explicit)

- Full protoc plugin ABI / `protoc-gen-baga`
- `oneof`, maps, packed repeated, extensions, proto3 optional presence
- Service stub generation (gRPC client/server) — later
- LLVM / self-hosting compiler involvement

## Goals (sketch)

1. Parse a **tiny** proto3 subset: `message` with `string` and `int64` fields
   (field numbers explicit).
2. Emit Baga:
   - `fn <msg>_encode(...) -> bytes` via `pb_field_string` / `pb_field_varint`
   - `fn <msg>_decode(data: bytes) ->` simple struct + ok flag (or fields only)
3. Golden: generated HelloRequest wire matches existing `0a0268691007` style
   vectors in `tests/grpc_goldens_test.baga`.
4. Tool lives under `tools/protoc_baga/` (Python 3, no pip deps).

## Input subset

```protobuf
syntax = "proto3";
package hellopb;
message HelloRequest {
  string name = 1;
  int64 n = 2;
}
message HelloReply {
  string message = 1;
}
```

Supported field types: `string`, `bytes`, `int32`, `int64`, `bool` (as varint 0/1).  
Unsupported lines are skipped with a warning on stderr.

## Output shape

```baga
// GENERATED — do not edit
import "pbbaga/pb.baga"
struct HelloRequest { name: str, n: i64 }
fn HelloRequest_encode(m: HelloRequest) -> bytes { ... }
fn HelloRequest_decode(data: bytes) -> HelloRequest { ... }  // zero defaults
```

Decode skips unknown fields (`pb_skip`) like hand-written Hello.

## Integration

| Step | Action |
|------|--------|
| Dev | `python3 tools/protoc_baga/protoc_baga.py foo.proto > foo_pb.baga` |
| Test | `tools/protoc_baga/test_sketch.sh` — generate + hex check via baga |
| Product | Optional: regenerate registry GetPackage helpers later |

## Honesty

This is a **sketch** to unblock P1 conversation and keep goldens honest.
Full codegen (repeated, nested messages, services) is a separate design.
