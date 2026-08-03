# chatbaga

A **multi-room WebSocket chat server** for Baga — JSON messages over
wsbaga frames, driven by a single-threaded `poll(2)` event loop. Apps-roadmap
**№3 complete**: this is the product that closed **W1 / K1** (serial accept)
and forced **`Map<i64, bytes>`** residual buffers into the language.

## What works

| Capability | Notes |
|------------|--------|
| Event loop | `std/net/poll.baga` — one thread watches the listener + every client fd |
| Many connections | concurrent joins/messages without per-client threads |
| Rooms | `Map<i64, str>` fd→room / fd→name; broadcast by walking `map_keys` |
| Protocol | JSON over WS text: `join` / `msg` / `joined` / `left` / `error` |
| Handshake | reuses wsbaga buffered handshake (`ws_parse_handshake_buf`) |
| Frames | text, ping→pong, close (binary ignored) |
| Interop | raw RFC 6455 client + `wscat` against `demo.baga` |

## Protocol

```
client → server:
  {"type":"join","name":"…","room":"…"}
  {"type":"msg","text":"…"}

server → client:
  {"type":"joined","room":"…","name":"…"}   // ack to joiner
  {"type":"join","name":"…"}                // peer entered room
  {"type":"msg","from":"…","text":"…"}      // broadcast (incl. sender)
  {"type":"left","name":"…"}                // peer disconnected / left room
  {"type":"error","msg":"…"}                // invalid JSON, join first, …
```

## API

```baga
struct ChatState {
    listener: i64,
    fds: Vec<i64>,
    bufs: Map<i64, bytes>,   // residual per-fd bytes (Map bytes values)
    done_hs: Map<i64, i64>,
    names: Map<i64, str>,
    rooms: Map<i64, str>
}

fn chat_serve(port: i64) -> i64 !Net !IO !Par   // poll loop; go_bg-ready
```

## Run

Package imports per language §18.1 — `sandak` computes `-I` from `sandak.toml`.

```bash
# typecheck the library (entry = chat.baga)
cd app-product/chatbaga
BAGA=../../baga sandak build

# standalone server (CHATPORT, default 16460)
# -I parents of deps: repo root (std/…) and app-product/ (wsbaga/…)
../../baga -I ../.. -I .. demo.baga
wscat -c ws://127.0.0.1:16460
  > {"type":"join","name":"аз","room":"bg"}
  > {"type":"msg","text":"здравей"}

# live multi-client test from repo root (18 checks, go_bg on :18460)
./baga -I . -I app-product tests/chat_test.baga
```

## Architecture

```
  clients (masked WS frames)
        ↓
  poll_wait([listener, …fds])
        ↓ readable
  chat_on_readable  →  handshake / frame / chat_on_text
        ↓
  ChatState maps  →  chat_broadcast(room)
```

No worker threads share the store — state lives on the event-loop thread
(the designated K1 fix). Residual byte buffers are `Map<i64, bytes>` so
partial frames and binary-safe WS payloads do not go through NUL-terminated
`str` (closes kvbaga K2 for this path).

## Honest limits

See [`gaps.md`](gaps.md): no fragment reassembly (W2 still open on wsbaga),
no TLS, no presence roster query, no rate limits. JSON builders are tiny
hand-rolled helpers (not a full encoder).
