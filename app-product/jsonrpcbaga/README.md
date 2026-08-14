# jsonrpcbaga

**JSON-RPC 2.0** over HTTP for Baga — apps-roadmap **№6**. Subset: single
and batch requests, notifications, standard error codes, built-in methods
via name dispatch. Custom tables: `rpc_handle_body_fn(body, dispatch)`.

## Methods

| Method | Params | Result |
|--------|--------|--------|
| `ping` | — | `"pong"` |
| `add` | `[a,b]` or `{"a","b"}` / `{"x","y"}` | sum (i64) |
| `echo` | any | params echoed |
| `fail` | — | error `-32000` |

## Errors

`-32700` parse · `-32600` invalid request · `-32601` method not found ·
`-32602` invalid params · app `-32000`.

`RpcResult` is the L3 enum `JrpcOk` / `JrpcErr` / `JrpcSkip`.

## API

```baga
fn rpc_handle_body(body: str) -> str     // built-in ping/add/echo/fail
fn rpc_handle_body_fn(body, dispatch) -> str  // L5: your method table
fn rpc_http(req: Request) -> Response    // POST /rpc or /
fn rpc_serve(port: i64) -> i64 !Net !IO !Par
```

## Run

```bash
cd app-product/jsonrpcbaga
BAGA=../../baga sandak build
RPCPORT=18580 ../../baga -I ../.. -I .. demo.baga

curl -s -d '{"jsonrpc":"2.0","method":"add","params":[2,3],"id":1}' \
  http://127.0.0.1:18580/rpc

./baga -I . -I app-product tests/jsonrpc_test.baga
```

## Honest limits

See [`gaps.md`](gaps.md): no function-value method table (L5); no true
Result type (L3); positional/named params only for `add`.
