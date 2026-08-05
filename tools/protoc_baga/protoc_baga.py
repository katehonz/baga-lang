#!/usr/bin/env python3
"""protoc_baga.py — Phase 5 sketch: proto3 subset → baga PB helpers.

Supports: message { string|bytes|int32|int64|bool field = N; }
No oneof, maps, repeated, nested messages, services.

Usage:
  python3 tools/protoc_baga/protoc_baga.py path/to/file.proto
  python3 tools/protoc_baga/protoc_baga.py --check-hex  # self-test wire vs goldens
"""
from __future__ import annotations

import argparse
import re
import sys
from dataclasses import dataclass, field
from pathlib import Path
from typing import List, Optional

FIELD_RE = re.compile(
    r"^\s*(optional\s+|repeated\s+)?"
    r"(string|bytes|int32|int64|bool|uint32|uint64)\s+"
    r"([A-Za-z_][A-Za-z0-9_]*)\s*=\s*(\d+)\s*;"
)
MSG_START = re.compile(r"^\s*message\s+([A-Za-z_][A-Za-z0-9_]*)\s*\{")
MSG_END = re.compile(r"^\s*\}")
COMMENT = re.compile(r"//.*$")
PKG = re.compile(r"^\s*package\s+([A-Za-z_][A-Za-z0-9_.]*)\s*;")


@dataclass
class Field:
    typ: str
    name: str
    number: int
    repeated: bool = False


@dataclass
class Message:
    name: str
    fields: List[Field] = field(default_factory=list)


def parse_proto(text: str) -> tuple[str, List[Message]]:
    package = ""
    messages: List[Message] = []
    cur: Optional[Message] = None
    for raw in text.splitlines():
        line = COMMENT.sub("", raw).rstrip()
        if not line.strip():
            continue
        m = PKG.match(line)
        if m:
            package = m.group(1)
            continue
        m = MSG_START.match(line)
        if m:
            cur = Message(name=m.group(1))
            continue
        if cur is not None and MSG_END.match(line):
            messages.append(cur)
            cur = None
            continue
        if cur is not None:
            fm = FIELD_RE.match(line)
            if fm:
                rep = fm.group(1) is not None and "repeated" in (fm.group(1) or "")
                if rep:
                    print(f"warning: skipping repeated field {fm.group(3)}", file=sys.stderr)
                    continue
                cur.fields.append(
                    Field(typ=fm.group(2), name=fm.group(3), number=int(fm.group(4)))
                )
            elif line.strip() not in ("{", "}"):
                if "oneof" in line or "map<" in line:
                    print(f"warning: unsupported syntax: {line.strip()}", file=sys.stderr)
    return package, messages


def baga_type(t: str) -> str:
    if t in ("string",):
        return "str"
    if t in ("bytes",):
        return "bytes"
    if t in ("bool",):
        return "i64"
    return "i64"


def emit_struct(msg: Message) -> str:
    lines = [f"struct {msg.name} {{"]
    for f in msg.fields:
        lines.append(f"    {f.name}: {baga_type(f.typ)},")
    if not msg.fields:
        lines.append("    _empty: i64,")
    lines.append("}")
    return "\n".join(lines)


def emit_encode(msg: Message) -> str:
    lines = [
        f"fn {msg.name}_encode(m: {msg.name}) -> bytes {{",
        "    let mut b = bytes_new(0)",
    ]
    for f in msg.fields:
        n = f.number
        if f.typ == "string":
            lines.append(f"    b = pb_field_string(b, {n}, m.{f.name})")
        elif f.typ == "bytes":
            lines.append(f"    b = pb_field_bytes(b, {n}, m.{f.name})")
        elif f.typ == "bool":
            lines.append(f"    b = pb_field_varint(b, {n}, m.{f.name})")
        else:
            lines.append(f"    b = pb_field_varint(b, {n}, m.{f.name})")
    lines.append("    return b")
    lines.append("}")
    return "\n".join(lines)


def emit_decode(msg: Message) -> str:
    lines = [
        f"fn {msg.name}_decode(data: bytes) -> {msg.name} {{",
    ]
    for f in msg.fields:
        if f.typ == "string":
            lines.append(f'    let mut {f.name}: str = ""')
        elif f.typ == "bytes":
            lines.append(f"    let mut {f.name} = bytes_new(0)")
        else:
            lines.append(f"    let mut {f.name}: i64 = 0")
    lines.append("    let mut r = pb_reader(data)")
    lines.append("    let mut more: i64 = 1")
    lines.append("    while more == 1 {")
    lines.append("        let t = pb_next(r)")
    lines.append("        if t.ok == 0 {")
    lines.append("            more = 0")
    lines.append("        } else {")
    lines.append("            r = PbReader { data: r.data, pos: t.pos, ok: 1 }")
    lines.append("            let mut handled: i64 = 0")
    for f in msg.fields:
        n = f.number
        if f.typ == "string":
            lines.append(f"            if handled == 0 && t.field == {n} && t.wire == PB_LEN() {{")
            lines.append("                let s = pb_take_string(r)")
            lines.append("                if s.ok == 1 {")
            lines.append(f"                    {f.name} = s.s")
            lines.append("                    r = PbReader { data: r.data, pos: s.pos, ok: 1 }")
            lines.append("                }")
            lines.append("                handled = 1")
            lines.append("            }")
        elif f.typ == "bytes":
            lines.append(f"            if handled == 0 && t.field == {n} && t.wire == PB_LEN() {{")
            lines.append("                let s = pb_take_bytes(r)")
            lines.append("                if s.ok == 1 {")
            lines.append(f"                    {f.name} = s.data")
            lines.append("                    r = PbReader { data: r.data, pos: s.pos, ok: 1 }")
            lines.append("                }")
            lines.append("                handled = 1")
            lines.append("            }")
        else:
            lines.append(f"            if handled == 0 && t.field == {n} && t.wire == PB_VARINT() {{")
            lines.append("                let v = pb_get_uvarint(r.data, r.pos)")
            lines.append("                if v.ok == 1 {")
            lines.append(f"                    {f.name} = v.val")
            lines.append("                    r = PbReader { data: r.data, pos: v.pos, ok: 1 }")
            lines.append("                }")
            lines.append("                handled = 1")
            lines.append("            }")
    lines.append("            if handled == 0 {")
    lines.append("                r = pb_skip(r, t.wire)")
    lines.append("                if r.ok == 0 {")
    lines.append("                    more = 0")
    lines.append("                }")
    lines.append("            }")
    lines.append("        }")
    lines.append("    }")
    fields = ", ".join(f"{f.name}: {f.name}" for f in msg.fields)
    if not msg.fields:
        fields = "_empty: 0"
    lines.append(f"    return {msg.name} {{ {fields} }}")
    lines.append("}")
    return "\n".join(lines)


def emit_file(package: str, messages: List[Message], src: str) -> str:
    out = [
        f"// GENERATED by tools/protoc_baga/protoc_baga.py — do not edit by hand.",
        f"// source: {src}",
    ]
    if package:
        out.append(f"// package {package}")
    out.append("")
    out.append('import "pbbaga/pb.baga"')
    out.append("")
    for msg in messages:
        out.append(emit_struct(msg))
        out.append("")
        out.append(emit_encode(msg))
        out.append("")
        out.append(emit_decode(msg))
        out.append("")
    return "\n".join(out)


# --- wire self-check (Python, no baga) for goldens ---

def _uvarint(v: int) -> bytes:
    out = bytearray()
    while v > 127:
        out.append((v & 127) | 128)
        v >>= 7
    out.append(v)
    return bytes(out)


def _field_string(num: int, s: str) -> bytes:
    b = s.encode("utf-8")
    tag = (num << 3) | 2
    return bytes([tag]) + _uvarint(len(b)) + b


def _field_varint(num: int, v: int) -> bytes:
    tag = (num << 3) | 0
    return bytes([tag]) + _uvarint(v)


def check_hex() -> int:
    """Verify sketch wire helpers match known goldens from grpc_goldens_test."""
    hello = _field_string(1, "hi") + _field_varint(2, 7)
    expect = "0a0268691007"
    got = hello.hex()
    ok = got == expect
    print(f"hello_hi_7: {got} {'OK' if ok else 'FAIL want ' + expect}")
    get = _field_string(1, "lsmbaga")
    expect2 = "0a076c736d62616761"
    ok2 = get.hex() == expect2
    print(f"get_lsmbaga: {get.hex()} {'OK' if ok2 else 'FAIL'}")
    testing = _field_string(1, "testing")
    ok3 = testing.hex() == "0a0774657374696e67"
    print(f"string_testing: {testing.hex()} {'OK' if ok3 else 'FAIL'}")
    return 0 if (ok and ok2 and ok3) else 1


def main() -> int:
    ap = argparse.ArgumentParser(description="proto3 subset → baga PB sketch")
    ap.add_argument("proto", nargs="?", help=".proto file")
    ap.add_argument("-o", "--output", help="write baga to file")
    ap.add_argument("--check-hex", action="store_true", help="run golden wire self-test")
    args = ap.parse_args()
    if args.check_hex:
        return check_hex()
    if not args.proto:
        ap.error("proto file required (or --check-hex)")
    path = Path(args.proto)
    text = path.read_text(encoding="utf-8")
    package, messages = parse_proto(text)
    if not messages:
        print("error: no messages parsed", file=sys.stderr)
        return 1
    code = emit_file(package, messages, str(path))
    if args.output:
        Path(args.output).write_text(code, encoding="utf-8")
        print(f"wrote {args.output} ({len(messages)} messages)", file=sys.stderr)
    else:
        sys.stdout.write(code)
    return 0


if __name__ == "__main__":
    sys.exit(main())
