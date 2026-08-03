# wsbaga — language & protocol gaps

Probe log from apps-roadmap №3 (WebSocket). Same shape as kvbaga/pgbaga.

## W1 — serial connections (K1 again)

**Symptom.** `ws_serve` accepts one connection at a time; a slow client
blocks the rest. `go()` still carries only `i64`, so a shared listener
state can't move to workers without per-connection spawning.

**Workaround.** `go_bg(ws_serve, port)` + per-connection handling inside;
fine for probe/dev scale.

**Severity.** High for a real chat server.

**Verdict.** Event loop via `poll(2)` extern (one thread, many fds) — the
designated K1 fix; also unblocks kvbaga. Next language feature.

## W2 — no fragmented message reassembly

**Symptom.** RFC 6455 allows a message split across continuation frames
(FIN=0, opcode 0). wsbaga closes the connection on opcode 0.

**Workaround.** Control frames (ping/pong/close) are exempt and handled;
large single-frame messages work up to the 64 MiB cap.

**Severity.** Medium — browsers rarely fragment, but proxies sometimes do.

**Verdict.** Small stateful addition to `ws_read_frame` (buffer until FIN);
needs a per-conn message accumulator — easy once W1 lands a conn registry.

## W3 — sha1 exists now, but std/crypto has no HMAC-SHA1

**Symptom.** Only `hmac_sha256` exists. No protocol in the current stack
needs HMAC-SHA1, so it wasn't added.

**Severity.** Low until a protocol demands it (old OAuth 1.0a, TOTP-SHA1).

**Verdict.** Add when needed — `hmac_sha256` is the template.

## Closed / fine

- **SHA-1 in std** (the gap this app was built around): `std/crypto/sha1.baga`,
  RFC 3174 vectors + RFC 6455 accept-key vector in `tests/std/sha1_test.baga`.
- Frame codec over the `bytes` API is clean; masking is one XOR pass.
- Handshake parse reused httpdbaga (`http_read_request`) — no duplication.
- Real interop: `wscat` (Node.js) echoes UTF-8 text and 900-byte payloads.
