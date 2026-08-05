# statusbaga — gaps

## S1 — no `google.rpc.Status` protobuf wire

Code + message only; no `details` (`Any` / error_info). Enough for trailers;
richer clients want the binary status-details-bin blob.

## S2 — grpc-message encoding is lite

Printable ASCII + space pass; other bytes become `%XX`. Full gRPC percent
encoding of multi-byte UTF-8 is not claimed.
