# httpdbaga v2 (HTTP/1.1 keep-alive + HTTP/2) Implementation Plan

**Goal:** Multi-protocol, multi-threaded server in `app-product/httpdbaga`:
buffered keep-alive HTTP/1.1, h2c HTTP/2 with full HPACK, binary-safe socket
I/O in `std/net`, worker-pool mode — plus the gap ledger.

**Spec:** `docs/superpowers/specs/2026-08-03-http2-server-design.md`

## Global Constraints

- **No compiler changes.** Gaps are logged in `app-product/httpdbaga/gaps.md`.
- Comments/READMEs/docs in English.
- Build only on builtins + `std/`. Verified binary-I/O facts (probe
  `tests/probe_binary_io.baga`, 2026-08-03): write via `str_of_bytes` +
  explicit count; read into `str_repeat` buffer + `byte_at`; never `len()`/
  `substr()` on transport strings with possible NULs.
- `tests/http_test.baga` and `jwtbaga` must stay green unmodified
  (additive API only).
- Every task ends green before the next starts.

---

## Task 1 — std/net binary I/O

- [ ] Add to `std/net/tcp.baga`: `tcp_read_bytes(fd, n) -> bytes`,
      `tcp_read_exact(fd, n) -> bytes`, `tcp_write_bytes(fd, b) -> i64`
      (write loop via `str_of_bytes` + `write` count; partial-write retry via
      `bytes_slice`). Needs local `extern fn write` reuse from `os.baga`
      (already imported chain) — call `write(fd, s, n)` directly, not
      `fd_write` (which is strlen-based).
- [ ] First write `tests/std/tcp_bytes_test.baga` (test-first): NUL/0xFF
      round-trips, `tcp_read_exact` across a slow-split write (write 2 chunks,
      one exact-read), empty/EOF reads → len 0.
- **Verify:** `./baga tests/std/tcp_bytes_test.baga` → `all passed`;
      existing `tests/std/tcp_test.baga` still passes.

## Task 2 — HTTP/1.1 v2: buffered reader + keep-alive + chunked

- [ ] First extend `tests/http_test.baga` with cases: two requests over one
      connection (keep-alive), chunked request body, `Expect: 100-continue`,
      garbage request line → 400, short-read body exactness (body > one write).
- [ ] `http.baga`: `struct HttpReader { fd, buf: bytes, pos }`;
      `http_reader(fd)`; internal `refill`/`read_line_r`/`read_n_r` over
      bytes (scan for `\n`, no per-byte syscalls);
      `http_read_request_r(r) -> (HttpReader, Request)`;
      `Request.body_bytes: bytes` (+ `body` kept as textual view);
      chunked decoder; 100-continue interim write; 400/413/431 guards;
      `http_respond_keepalive(fd, resp, keep)`.
- [ ] Old `http_read_request(fd)` reimplemented on top of the reader
      (behavior identical for the existing tests).
- **Verify:** `./baga tests/http_test.baga` green (old + new cases);
      `./baga tests/jwt_test.baga` green (consumer unchanged).

## Task 3 — HPACK

- [ ] Commit `app-product/httpdbaga/gen_hpack_tables.py` (RFC 7541 parser,
      canonical invariants asserted) + generated `hpack_tables.baga`
      (already drafted; regenerate in place).
- [ ] First write `tests/hpack_test.baga` with RFC known answers:
      C.1 integers (10/5-bit → `0a`; 1337/5-bit → `1f9a0a`; 42/8-bit → `2a`),
      C.3.x decode sequences (dynamic table growth, no Huffman),
      C.4.1 Huffman decode `f1e3c2e5f23a6ba0ab90f4ff` = `www.example.com`,
      C.4.2/C.4.3 continuation sequences, encoder round-trip.
- [ ] `hpack.baga`: integer/string decode primitives, Huffman decoder
      (canonical first_code/first_off/count walk; padding rule; EOS reject),
      decoder over static+dynamic tables (entry = name+value+32 size, FIFO
      eviction, size-update representation), encoder (indexed static matches,
      literal-without-indexing otherwise, raw strings only).
- **Verify:** `./baga tests/hpack_test.baga` → `all passed`;
      `./baga --check app-product/httpdbaga/hpack.baga`.

## Task 4 — HTTP/2 framing + streams

- [ ] First write `tests/h2_test.baga`: in-process loopback client —
      preface, SETTINGS exchange + ACK, HPACK-encoded GET `/health` HEADERS,
      assert `:status` 200 + JSON body via DATA + END_STREAM; second stream on
      the same connection (multiplexing); POST body via DATA frames with
      WINDOW_UPDATE replenishment observed.
- [ ] `h2.baga`: `struct H2Conn` (fd, reader, peer settings, send windows,
      stream pool as parallel Vecs), frame read/write (24-bit length, flags,
      sid), preface check, SETTINGS handshake, frame loop
      (DATA/HEADERS/CONTINUATION assembly, PING ack, GOAWAY, RST_STREAM,
      WINDOW_UPDATE accounting, PRIORITY ignored), request dispatch to a
      handler chosen by the server (server passes an i64 "route set" — demo
      has one), response HEADERS+DATA with END_STREAM, connection/stream
      error mapping.
- **Verify:** `./baga tests/h2_test.baga` → `all passed`.

## Task 5 — server.baga v2 + protocol detection

- [ ] Buffered peek: first bytes `PRI *` → `h2_conn`, else HTTP/1.1
      keep-alive loop (server-owned; no closures in Baga).
- [ ] `MODE=threads` (default) / `MODE=pool` (POOL_N workers on a chan;
      `chan_recv2` for clean close) / `MODE=sync`.
- [ ] `/proto` route reporting h1/h2; routes otherwise unchanged.
- [ ] README.md rewrite: protocols, modes, curl examples
      (`curl --http2-prior-knowledge`), limits (no TLS, no push, linear
      stream scan), benchmark recipe.
- **Verify:** manual smoke — start server; `curl` h1 keep-alive +
      `curl --http2-prior-knowledge` + POST echo on both protocols; pool mode
      under a parallel curl burst.

## Task 6 — Gaps triage + make test + commit

- [ ] `gaps.md`: add G8 (chr UTF-8 / poke8 ≥ 128), G9 (no byte setter),
      G10 (extern can't take bytes) + every implementation-time find, each
      with symptom → repro → workaround → severity; end-of-milestone verdicts.
- [ ] Wire `tcp_bytes_test`, `hpack_test`, `h2_test` into `make test`
      (existing blocks pattern).
- [ ] Delete the temporary probe `tests/probe_binary_io.baga` (evidence
      already quoted in spec + gaps) — or keep if `make test` should guard the
      binary-I/O assumptions; decide at the time (default: keep, it is cheap).
- **Verify:** `make` clean; `make test` green end-to-end; `./baga --check`
      on all `app-product/httpdbaga/*.baga` libs.
- [ ] Propose commit message; commit only on explicit user instruction.
