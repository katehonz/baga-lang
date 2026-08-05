#!/usr/bin/env bash
# test_sketch.sh — Phase 5 protoc_baga smoke (no protoc binary required).
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
PY="${PYTHON:-python3}"
GEN="$ROOT/tools/protoc_baga/protoc_baga.py"
OUTDIR="${TMPDIR:-/tmp}/protoc_baga_$$"
mkdir -p "$OUTDIR"
trap 'rm -rf "$OUTDIR"' EXIT

echo "=== protoc_baga hex goldens ==="
"$PY" "$GEN" --check-hex

echo "=== generate hello.proto ==="
"$PY" "$GEN" "$ROOT/tools/protoc_baga/examples/hello.proto" -o "$OUTDIR/hello_pb.baga"
grep -q 'fn HelloRequest_encode' "$OUTDIR/hello_pb.baga"
grep -q 'fn HelloReply_decode' "$OUTDIR/hello_pb.baga"
grep -q 'pb_field_string' "$OUTDIR/hello_pb.baga"

echo "=== generate registry.proto ==="
"$PY" "$GEN" "$ROOT/tools/protoc_baga/examples/registry.proto" -o "$OUTDIR/reg_pb.baga"
grep -q 'fn GetPackageRequest_encode' "$OUTDIR/reg_pb.baga"
grep -q 'fn Package_encode' "$OUTDIR/reg_pb.baga"

echo "=== baga compile generated hello helpers ==="
# Thin driver imports generated file + checks encode hex via baga.
cat > "$OUTDIR/drive.baga" << EOF
import "pbbaga/pb.baga"
import "${OUTDIR}/hello_pb.baga"
import "std/str/str.baga"

fn main() {
    let m = HelloRequest { name: "hi", n: 7 }
    let b = HelloRequest_encode(m)
    if hex_encode(b) != "0a0268691007" {
        print(concat("FAIL hello encode ", hex_encode(b)))
        exit(1)
    }
    let d = HelloRequest_decode(b)
    if d.name != "hi" || d.n != 7 {
        print("FAIL hello decode")
        exit(1)
    }
    let r = HelloReply { message: "hi" }
    if hex_encode(HelloReply_encode(r)) != "0a026869" {
        print("FAIL reply")
        exit(1)
    }
    print("protoc_baga drive: ok")
}
EOF
# Generated import path must be resolvable — copy next to drive with relative import
cp "$OUTDIR/hello_pb.baga" "$OUTDIR/hello_gen.baga"
cat > "$OUTDIR/drive2.baga" << 'EOF'
import "pbbaga/pb.baga"
import "hello_gen.baga"
import "std/str/str.baga"

fn main() {
    let m = HelloRequest { name: "hi", n: 7 }
    let b = HelloRequest_encode(m)
    if hex_encode(b) != "0a0268691007" {
        print(concat("FAIL hello encode ", hex_encode(b)))
        exit(1)
    }
    let d = HelloRequest_decode(b)
    if d.name != "hi" || d.n != 7 {
        print("FAIL hello decode")
        exit(1)
    }
    let r = HelloReply { message: "hi" }
    if hex_encode(HelloReply_encode(r)) != "0a026869" {
        print("FAIL reply")
        exit(1)
    }
    print("protoc_baga drive: ok")
}
EOF
"$ROOT/baga" -I "$ROOT" -I "$ROOT/app-product" -I "$OUTDIR" "$OUTDIR/drive2.baga"

echo "=== protoc_baga sketch: all passed ==="
