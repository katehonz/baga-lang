# wsbaga

A **WebSocket (RFC 6455)** implementation for Baga — handshake, frame codec,
echo server, and a masked client. Apps-roadmap №3: the probe that added
**SHA-1 to std/crypto** and proved the framing against a real-world client.

## What works

| Capability | Notes |
|------------|--------|
| Handshake (server) | HTTP upgrade via httpdbaga; `Sec-WebSocket-Accept` = base64(SHA-1(key+GUID)) |
| Handshake (client) | `ws_client_connect` — random 16-byte key, verifies the server's accept |
| Frames | FIN/opcode, 7/16/64-bit lengths, client masking (RFC §5.3) |
| Opcodes | text (1), binary (2), close (8), ping→pong (9→10); pong ignored |
| Binary safety | payloads ride on `bytes` — NUL/0xFF round-trip (unlike `str` stores) |
| Echo server | `ws_serve(port)` — serial accept loop (`go_bg`-ready) |
| Interop | verified against `wscat` (Node.js): UTF-8 text + 900-byte payloads |

## API

```baga
// pure
fn ws_accept_key(client_key: str) -> str
fn ws_build_frame(fin, opcode, payload: bytes, mask_key) -> bytes
fn ws_apply_mask(payload, mask: bytes) -> bytes

// server (effects)
fn ws_server_handshake(fd) -> i64 !IO !Net
fn ws_read_frame(conn: WsConn) -> WsRead !IO !Net
fn ws_handle_conn(fd) -> i64 !IO !Net !Random
fn ws_serve(port) -> i64 !Net !IO !Random !Par

// client (effects)
fn ws_client_connect(host, port, timeout_s) -> WsDial !Net !IO !Random
fn ws_send_text / ws_send_binary / ws_send_ping / ws_send_close   // masked

struct WsConn  { fd, buf }        // buffered connection (residual bytes)
struct WsFrame { fin, opcode, payload: bytes }
struct WsRead  { conn, frame, ok, bad }
```

## Run

```bash
# echo server (WSPORT, default 16441)
./baga app-product/wsbaga/demo.baga
wscat -c ws://127.0.0.1:16441          # any RFC 6455 client

# loopback test (server in a go_bg worker, masked client)
./baga tests/ws_test.baga
```

Both are wired into `make test` (plus `tests/std/sha1_test.baga` for the new
hash — RFC 3174 vectors + the RFC 6455 accept-key vector).

## Honest limits

- **Serial**: one connection at a time (same model as kvbaga; the event loop
  is the P1 path — gaps W1).
- **No fragmented-message reassembly**: FIN=0 (continuation) closes the
  connection (W2). Control frames are handled whole, as the RFC requires.
- No `permessage-deflate`, no close-status interpretation (payload echoed),
  no server-side origin/protocol-version enforcement beyond key presence.
- SHA-1 is used **only** because RFC 6455 mandates it; everything else in
  the stack stays on sha256/hmac.
