# pbbaga — plan

Date: 2026-08-05
Status: **P0 done** (C5 codec + framing)
Goal: microservice wire interop foundation.

## P0 ✅

1. Varint / fixed / length-delimited encode+decode
2. Field helpers + skip unknown
3. gRPC length-prefixed frames
4. HelloRequest/Reply + tests + demo

## P1

- H2 unary server glue on httpdbaga
- Packed repeated helpers
