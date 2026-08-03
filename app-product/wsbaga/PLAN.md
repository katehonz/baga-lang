# wsbaga — WebSocket (plan)

Date: 2026-08-04
Status: P0 done (RFC 6455 core, interop-verified); P1 chat done in chatbaga
Goal: the roadmap №3 probe — real-time transport. What the probe found:
std needed **SHA-1** (added), and the concurrency model (K1) was the wall
between "echo" and "chat" — closed by `std/net/poll` + `app-product/chatbaga`.

## Phases

### P0 — protocol core (this iteration) ✅

1. `std/crypto/sha1.baga` — RFC 3174 + RFC 6455 vectors in make test.
2. Handshake: server upgrade (httpdbaga parse + accept key), client dial
   with accept verification.
3. Frame codec: 7/16/64-bit lengths, masking, text/binary/ping/pong/close.
4. Echo server (`ws_serve`) + masked client helpers; loopback test (14
   checks) + `wscat` interop (UTF-8 text, 900-byte payloads).

### P1 — chat (done in `app-product/chatbaga`) ✅

- **Event loop via `poll(2)`** — `std/net/poll.baga`; closes W1/K1.
- Rooms + broadcast: `chatbaga` (`Map` fd→room/name/bufs, JSON envelope).
- See `app-product/chatbaga/{README,PLAN,gaps}.md`.

### P2 — completeness

- Fragmented message reassembly (W2).
- permessage-deflate (needs a deflate implementation first — big).
- Close status handling, heartbeat (idle ping) policy.
- TLS — shares the std/net blocker (pgbaga G6).

## Non-goals (P0)

Compression, extensions negotiation, multi-connection scaling, browser test
matrix (covered by wscat interop for now).

## Files

| File | Role |
|------|------|
| `ws.baga` | handshake + frame codec + server/client |
| `demo.baga` | standalone echo server (interop target) |
| `gaps.md` | W1–W3 |
| `tests/ws_test.baga` | loopback test (repo root tests/) |
| `tests/std/sha1_test.baga` | SHA-1 vectors (repo root tests/) |

## Success criteria (P0) — met

1. `./baga tests/ws_test.baga` → all 14 checks pass.
2. A real RFC 6455 client (wscat) completes handshake + echo, incl. frames
   over 125 bytes (16-bit length path) and UTF-8.
3. SHA-1 matches RFC 3174 known answers (incl. the 10^6 × 'a' vector).
