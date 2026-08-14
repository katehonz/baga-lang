# jsonrpcbaga — language & product gaps

Probe log from apps-roadmap **№6** (JSON-RPC).

## R1 — Result / sum types (L3) — **shipped (B1)**

**Shipped.** `RpcResult` is `JrpcOk` / `JrpcErr` / `JrpcSkip`. Public path is
still `rpc_handle_body` → string; encode matches on the sum.

## R2 — no function values (L5) → switch dispatch

**Symptom.** Methods are hard-coded in `rpc_dispatch`. Cannot register
`map_set(methods, "add", handler)`.

**Workaround.** Name switch; extend by editing `rpc_dispatch`.

**Severity.** High for a framework; fine for a fixed probe surface.

**Verdict.** **Shipped 2026-08-14.** `rpc_handle_body_fn(body, dispatch)`
and `rpc_handle_object_fn` take `fn(str, JsonDoc, i64, str) -> RpcResult`.
Built-in `rpc_handle_body` still uses `rpc_dispatch`. A `Map<str, fn>`
table is optional further cleanup.

## R3 — nested concat for JSON encode

**Symptom.** Response building is still concat chains (M1/G1 family).

**Workaround.** Small helpers `rpc_encode` / `rpc_id_field`.

**Severity.** Low–medium.

**Verdict.** Same as mdbaga M1; interpolation helps locals only.

## Closed / fine

- std/json + `json_strict_valid` enough for parse errors.
- httpdbaga handles POST body + JSON content-type.
- Batch + notifications match the JSON-RPC 2.0 empty-response rule.
