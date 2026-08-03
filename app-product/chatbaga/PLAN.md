# chatbaga — WebSocket chat rooms (plan)

Date: 2026-08-04
Status: P0 done (poll event loop + rooms + live multi-client test + interop)
Goal: apps-roadmap **№3** — the product that closes serial-accept (W1/K1)
via `poll(2)`, and proves multi-connection state on `Map` (incl. bytes
values for residual buffers).

## Why chat next

wsbaga proved RFC 6455 framing and SHA-1; the echo server is still serial.
A real chat needs many live connections and room-scoped broadcast — that is
exactly the load that forces an event loop and a connection registry.

## Phases

### P0 — multi-room chat on poll (this iteration) ✅

1. `std/net/poll.baga` — `poll_wait` / `poll_has` over SYS_poll; live test.
2. Language: `Map` value kind **bytes** (checker + C runtime) — residual
   buffers and binary-safe values (kvbaga K2 path).
3. `chat.baga` — `ChatState` maps, accept on POLLIN, buffered handshake +
   frames, JSON join/msg/left/error, room broadcast.
4. `demo.baga` standalone server; `tests/chat_test.baga` — 18 live checks
   (two clients, errors, room isolation, leave); wscat / raw interop.
   Package via `sandak.toml` / `sandak build` (no hand-rolled make product block).

### P1 — operational chat

- Heartbeat idle ping (server-initiated) + close status codes.
- Presence: `{"type":"who"}` → member list for the room.
- Rate limit / max message size / nick uniqueness per room.
- Fragment reassembly (wsbaga W2) once a conn registry is the norm.

### P2 — product polish

- TLS (shared std/net blocker G6).
- Persistence of last-N messages (kvbaga or Postgres).
- Browser smoke page under `playground/` or `app-product/chatbaga/www/`.

## Non-goals (P0)

Auth, history, moderation, compression, horizontal scale.

## Files

| File | Role |
|------|------|
| `chat.baga` | state + poll loop + protocol |
| `demo.baga` | standalone server (interop target) |
| `gaps.md` | C1–C3 |
| `tests/chat_test.baga` | live multi-client test (repo root tests/) |
| `std/net/poll.baga` | event-loop primitive (std) |

## Success criteria (P0) — met

1. `./baga tests/chat_test.baga` → `chat_test: all passed` (18 checks).
2. Two clients in one room see each other's messages; a third room is silent.
3. Real client (wscat / raw RFC 6455) joins and exchanges UTF-8 text.
4. No per-connection thread — one poll loop owns all state.
