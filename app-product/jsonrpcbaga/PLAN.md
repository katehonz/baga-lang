# jsonrpcbaga — JSON-RPC 2.0 (plan)

Date: 2026-08-04
Status: P0 done
Goal: apps-roadmap **№6** — protocol machine + structured errors probe.

## Phases

### P0 ✅

1. `rpc_handle_body` — parse, dispatch, encode (single + batch).
2. Notifications, standard error codes, built-in methods.
3. HTTP `POST /rpc`, `rpc_serve` accept loop.
4. `jsonrpc_test` pure + live.

### P1

- More methods; named-params helpers.
- Idempotency key header (link to queuebaga).
- OpenRPC / schema doc generation.

### P2

- Migrate `RpcResult` → language `Result`/`enum` when L3 lands.
- Method registry with function values (L5).

## Success criteria (P0) — met

1. sandak build OK.
2. jsonrpc_test all passed (parse/errors/batch/HTTP).
