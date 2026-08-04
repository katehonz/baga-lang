# jsonrpcbaga — language & product gaps

Probe log from apps-roadmap **№6** (JSON-RPC).

## R1 — no Result / sum types (L3)

**Symptom.** Every outcome is `RpcResult { ok, skip, result_json, err_* }`
or parallel fields. Call sites branch on `ok`/`skip` integers.

**Workaround.** Documented struct convention (same as pgbaga G1).

**Severity.** High for clean protocol APIs.

**Verdict.** This package is the designated migrate target when `Result` /
enums land. Error codes map cleanly onto `Err(RpcError)`.

## R2 — no function values (L5) → switch dispatch

**Symptom.** Methods are hard-coded in `rpc_dispatch`. Cannot register
`map_set(methods, "add", handler)`.

**Workaround.** Name switch; extend by editing `rpc_dispatch`.

**Severity.** High for a framework; fine for a fixed probe surface.

**Verdict.** **Unblocked 2026-08-05** — L5 shipped: `Map<str, fn(...)>`
works (see `tests/std/fnval_test.baga`'s method table). Migrating
`rpc_dispatch` to a registered table is optional cleanup.

## R3 — nested concat for JSON encode

**Symptom.** Response building is still concat chains (M1/G1 family).

**Workaround.** Small helpers `rpc_encode` / `rpc_id_field`.

**Severity.** Low–medium.

**Verdict.** Same as mdbaga M1; interpolation helps locals only.

## Closed / fine

- std/json + `json_strict_valid` enough for parse errors.
- httpdbaga handles POST body + JSON content-type.
- Batch + notifications match the JSON-RPC 2.0 empty-response rule.
