# protoc_baga — Phase 5 sketch

Tiny **proto3 → baga** generator for `pbbaga` hand-codec helpers.

**Not** a full `protoc` plugin. Supported: `message` with `string` / `bytes` /
`int32` / `int64` / `bool` scalar fields.

## Usage

```bash
python3 tools/protoc_baga/protoc_baga.py tools/protoc_baga/examples/hello.proto
python3 tools/protoc_baga/protoc_baga.py path/msg.proto -o msg_pb.baga

# wire goldens (no baga needed)
python3 tools/protoc_baga/protoc_baga.py --check-hex

# full smoke (generate + baga compile)
./tools/protoc_baga/test_sketch.sh
```

## Design

See `docs/superpowers/specs/2026-08-05-protoc-baga-sketch-design.md`.

## Examples

| Proto | Maps to |
|-------|---------|
| `examples/hello.proto` | pbbaga HelloRequest / HelloReply |
| `examples/registry.proto` | registry GetPackage / Package |

Generated code imports `pbbaga/pb.baga` and emits `Msg_encode` / `Msg_decode`.
