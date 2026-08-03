# wsbaga — WebSocket (plan)

Date: 2026-08-04
Status: P0 done (RFC 6455 core, interop-verified); chat layer pending
Goal: the roadmap №3 probe — real-time transport. What the probe found:
std needed **SHA-1** (added), and the concurrency model (K1) is the wall
between "echo" and "chat" — the event loop is the next language feature.

## Phases

### P0 — protocol core (this iteration) ✅

1. `std/crypto/sha1.baga` — RFC 3174 + RFC 6455 vectors in make test.
2. Handshake: server upgrade (httpdbaga parse + accept key), client dial
   with accept verification.
3. Frame codec: 7/16/64-bit lengths, masking, text/binary/ping/pong/close.
4. Echo server (`ws_serve`) + masked client helpers; loopback test (14
   checks) + `wscat` interop (UTF-8 text, 900-byte payloads).

### P1 — chat (next)

- **Event loop via `poll(2)` extern** — one thread, many fds; closes W1/K1
  and lets kvbaga move off serial accept too.
- Rooms: `Map<i64, str>` fd→room + fd→name; broadcast by key walk
  (`map_keys` on i64 keys — already in the runtime).
- JOIN / MSG / LEAVE message envelope (JSON over text frames).

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
