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
- `extern pwrite(fd: i64, buf: str, count: i64, off: i64) -> i64 !Net`
- `extern pread(fd: i64, buf: i64, count: i64, off: i64) -> i64 !Net`
- `extern syscall(nr: i64, a1: i64, a2: i64, a3: i64) -> i64 !Net`

Public API:

- `tcp_listen(port: i64) -> i64 !Net` — listen on `0.0.0.0:port` (SO_REUSEADDR set); returns the listener fd, or -1.
- `tcp_accept(listener: i64) -> i64 !Net` — accept one connection (blocking); returns the connection fd, or -1.
- `tcp_connect(host: str, port: i64) -> i64 !Net` — connect to dotted IPv4 `host:port`; returns the fd, or -1.
- `tcp_read(fd: i64, n: i64) -> str !Net !IO` — read up to `n` bytes; `""` at EOF or on error.
- `tcp_write(fd: i64, data: str) -> i64 !Net !IO` — write all of `data`; 0 on success, -1 on error.
- `tcp_close(fd: i64) -> i64 !IO` — close the fd.

Notes:

- IPv4 only, blocking sockets. No TLS (v2). Linux-only staging (memfd).
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
