# std/net — TCP/IPv4 sockets over libc

TCP sockets via `extern fn` over libc, declared in `tcp.baga`.

Externs (libc):

- `extern socket(domain: i64, typ: i64, proto: i64) -> i64 !Net`
- `extern setsockopt(fd: i64, level: i64, opt: i64, val: i64, len: i64) -> i64 !Net`
- `extern bind(fd: i64, addr: i64, addrlen: i64) -> i64 !Net`
- `extern listen(fd: i64, backlog: i64) -> i64 !Net`
- `extern accept(fd: i64, addr: i64, addrlen: i64) -> i64 !Net`
- `extern connect(fd: i64, addr: i64, addrlen: i64) -> i64 !Net`
- `extern memfd_create(name: str, flags: i64) -> i64 !Net`
- `extern pwrite64(fd: i64, buf: str, count: i64, off: i64) -> i64 !Net`
- `extern pread64(fd: i64, buf: i64, count: i64, off: i64) -> i64 !Net`
- `extern syscall(nr: i64, a1: i64, a2: i64, a3: i64) -> i64 !Net`

Public API (`tcp.baga`):

- `tcp_listen(port: i64) -> i64 !Net` — listen on `0.0.0.0:port` (SO_REUSEADDR set); returns the listener fd, or -1.
- `tcp_accept(listener: i64) -> i64 !Net` — accept one connection (blocking); returns the connection fd, or -1.
- `tcp_connect(host: str, port: i64) -> i64 !Net` — connect to dotted IPv4 `host:port`; returns the fd, or -1.
- `tcp_connect_to` / `tcp_set_timeouts` / `tcp_resolve_ipv4` — production connects (DNS, timeouts, NODELAY/KEEPALIVE).
- `tcp_read` / `tcp_write` / `tcp_read_bytes` / `tcp_write_bytes` / `tcp_close`.

Event loop (`poll.baga`) — closes kvbaga K1 / wsbaga W1:

- `poll_wait(fds: Vec<i64>, timeout_ms: i64) -> PollResult !Net !IO` —
  SYS_poll; `ready` holds fds with POLLIN|POLLERR|POLLHUP.
- `poll_has(ready, fd) -> i64` — membership helper.
- Used by `app-product/chatbaga` for multi-connection chat.
- Phase 5 stretch: io_uring experiment lives under `tools/iouring/`
  (not wired into baga; see design note
  `docs/superpowers/specs/2026-08-05-io-uring-poll-sketch-design.md`).

HTTP client: see `http_client.baga` (`http_get` / `http_post` / `http_request`)
— supports `http://` and `https://` (TLS 1.3 via `tls.baga`).

TLS 1.3 client (`tls.baga`): record layer, handshake, X.509 + RSA-PSS/ECDSA
verify, application data. `tls_connect(host, port, timeout, trust_anchor)`;
empty trust anchor accepts self-signed (dev/mock).
`tls_conn_write` splits plaintext into ≤16383-byte records (RFC 8446
2^14); a single huge record (e.g. Mistral OCR PDF as base64) is rejected
by the peer. HTTP client response cap is 8 MB.

TLS 1.3 server (`tls_server.baga`): `tls_accept(fd, cert_der, key)` —
ClientHello parse, ServerHello (x25519), encrypted flight
(EncryptedExtensions/Certificate/CertificateVerify/Finished), client
Finished verify, role-swapped `TlsConn` for application data. Shares the
client's wire/crypto (`tls.baga` / `tls_crypto.baga` / `tls_flight.baga` /
`tls_conn.baga`); cert keys come from `std/crypto/pkey.baga` (PEM
PKCS#8/PKCS#1/SEC1 → RSA-PSS or ECDSA-P256 signing). Honest limits:
x25519 only; suites 4865/4866; single leaf cert; no tickets/HRR/client
auth; alerts only before the encrypted flight. Consumed by boilaDB SSL
(SSLRequest → 'S').

Notes:

- IPv4 only, blocking sockets. TLS 1.3 client + server (no 1.2).
  Linux-only staging (memfd).
- `sockaddr_in` (16 bytes) is staged in a short-lived arena per call.
  The bytes are first `pwrite`n one at a time into an anonymous memfd
  (from `chr()` strings — this is how byte value 0 is written, since it
  cannot travel through baga strings/`concat`), then one `pread` copies
  the finished struct into the arena buffer. `bzero`/`bcopy` would be
  the natural fit, but their glibc prototypes (`strings.h`, pulled in by
  the `<string.h>` the C backend always emits) conflict with the
  `int64_t`/`const char *` prototypes baga generates for extern fns.
  Port and IP octets are stored big-endian.

Effects: !Net (+!IO). Memory: sockaddr staging uses a short-lived arena per call; payload buffers are heap strings.
