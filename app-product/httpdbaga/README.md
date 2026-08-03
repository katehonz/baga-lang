# httpdbaga

A multi-protocol HTTP server library + demo for Baga, built only on `std/`
(`net`, `io`, `os`, `str`, `bytes`):

- **HTTP/1.1** — buffered request parsing (8 KiB reads, not one syscall per
  byte), persistent connections (keep-alive), chunked request bodies,
  `Expect: 100-continue`, exact-N body reads (large bodies are no longer
  truncated by short reads).
- **HTTP/2 (h2c, RFC 7540/7541)** — binary framing, full HPACK decode
  (static + dynamic table, Huffman, size updates), HPACK encode (static-table
  indexed + literal, raw strings), stream multiplexing, SETTINGS /
  WINDOW_UPDATE / PING / GOAWAY / RST_STREAM, PADDED/PRIORITY stripping,
  HEADERS+CONTINUATION assembly. Prior-knowledge entry (`PRI * HTTP/2.0`
  preface) on the same listener as HTTP/1.1.
- **Multi-threaded** — one OS thread per connection by default (`go_bg`),
  or a bounded worker pool over `std/par` channels (`MODE=pool`).

This is a probe of the language as much as a product — see
[`gaps.md`](gaps.md) for the friction found while building it.

## Files

| File | What |
|------|------|
| `http.baga` | HTTP/1.1: `Request`/`Response`, buffered reader, parser, serializer |
| `hpack.baga` | HPACK (RFC 7541) encode/decode, pure |
| `hpack_tables.baga` | GENERATED — HPACK static table + Huffman code (RFC 7541 A+B) |
| `gen_hpack_tables.py` | the generator (parses the RFC text, checks canonical invariants) |
| `h2.baga` | HTTP/2 framing + stream state machine + `h2_serve` connection loop |
| `server.baga` | demo server: protocol detection, routing, threading modes |
| `gaps.md` | language gaps found, with evidence and triage |

## API

```baga
// ---------- HTTP/1.1 (http.baga) ----------
struct Request  { method, path, version: str; hkeys, hvals: Vec<str>;
                  body: str; body_bytes: bytes }
struct Response { status: i64; hkeys, hvals: Vec<str>; body: str }

fn http_read_request(fd: i64) -> Request !IO !Net          // v1 shape
struct HttpReader { fd: i64, buf: bytes, pos: i64 }
struct HttpRead   { reader: HttpReader, req: Request, err: i64 }  // err: 0/400/413/431
fn http_reader(fd: i64) -> HttpReader
fn http_read_request_r(r: HttpReader) -> HttpRead !IO !Net // keep-alive loop
fn http_keepalive(req: Request) -> i64                     // 1 = read another
fn http_respond_keepalive(fd: i64, resp: Response, keep: i64) -> i64 !IO !Net
fn http_respond(fd: i64, resp: Response) -> i64 !IO !Net   // keep=0
// + the v1 accessors: http_method/path/path_only/query_param/body/body_bytes/header
// + builders: http_response/http_set_header/http_json_response

// ---------- HTTP/2 (h2.baga) ----------
fn h2_serve(fd: i64, reader: HttpReader) -> i64 !IO !Net
// Link-time convention: the embedding program defines
//     fn h2_route(req: Request) -> Response
// which h2_serve dispatches every completed stream to (Baga has no
// closures/function values; inversion by naming convention).

// ---------- HPACK (hpack.baga), pure ----------
struct HpackCtx { ... }                    // dynamic table + limits
fn hpack_ctx(max_size: i64) -> HpackCtx
fn hpack_decode(ctx, block: bytes) -> HpackOut   // ctx/ks/vs/err
fn hpack_encode(ks: Vec<str>, vs: Vec<str>) -> bytes
```

`Request.body` is the textual view of `body_bytes` (`str_of_bytes`); carry
`body_bytes` when the payload may contain NUL bytes (`str` is NUL-terminated).

## Run the demo

```bash
./baga --emit-c app-product/httpdbaga/server.baga > /tmp/httpd.c
gcc -O2 -Iinclude -o /tmp/httpd /tmp/httpd.c -lm -pthread
PORT=8080 /tmp/httpd &
```

Modes: `MODE=threads` (default, `go_bg` per connection) · `MODE=pool`
(`POOL_N` workers, default 8; HTTP/1.1 answers `Connection: close` in pool
mode — a bounded pool plus idle keep-alive connections deadlocks under load)
· `MODE=sync` (or legacy `BAGA_SYNC=1`).

```bash
curl localhost:8080/health                                  # HTTP/1.1
curl localhost:8080/ localhost:8080/hello?name=baga         # keep-alive reuse
curl -X POST -d '{"hi":"baga"}' localhost:8080/echo
curl --http2-prior-knowledge localhost:8080/health          # HTTP/2 (h2c)
curl --http2-prior-knowledge -X POST -d '{"hi":"h2"}' localhost:8080/echo
curl -i localhost:8080/nope                                 # 404
curl localhost:8080/proto                                   # which protocol
```

## Test

```bash
./baga tests/http_test.baga    # h1: GET/POST/header-ci/query + keep-alive,
                               # chunked, 100-continue, 400, exact large body
./baga tests/hpack_test.baga   # RFC 7541 known answers: C.1 integers,
                               # C.2 strings, C.3/C.4 request sequences
                               # (dynamic table 57→110→164), encoder round-trip
./baga tests/h2_test.baga      # loopback h2 client vs h2_serve in go():
                               # SETTINGS, two streams + body DATA, 20 KiB
                               # response across DATA frames, WINDOW_UPDATE,
                               # PING, GOAWAY + clean worker exit
./baga tests/std/tcp_bytes_test.baga   # binary-safe socket I/O
```

All four are wired into `make test`.

## Effects & memory

Socket-touching functions carry `!IO !Net`; HPACK and the struct builders are
pure. Memory follows the `std/` leak-tolerant default — per-connection
buffers are allocated in the global arena and not freed; the arena allocator
itself is mutex-protected (multi-threaded allocations are safe; see
`gaps.md` G11 for the history).

## Honesty / limits

- No TLS (no crypto-socket layer in `std/` yet) — hence h2c prior knowledge,
  not ALPN. `Upgrade: h2c` is also not implemented; clients must use
  `--http2-prior-knowledge` (or speak HTTP/1.1).
- HPACK encode skips Huffman and dynamic-table inserts (both legal omissions;
  decode is complete because clients use them).
- Response flow control is accounted but never blocks: demo responses fit in
  the initial window; exceeding it sends GOAWAY(FLOW_CTRL) instead of waiting.
- Stream bookkeeping is a linear scan of parallel `Vec<i64>`s; request bodies
  accumulate in an append-only pool (O(n²) under many DATA frames). Fine at
  demo scale — the language has no map type yet (logged in `gaps.md`).
- HEAD is routed to the demo's 404 (no route for it); the transport handles
  it like any other request.
- Header name/value segments parked per stream are joined with `\n` — header
  values containing `\n` are illegal in HTTP anyway.

## Known sharp edges

- **Startup `print` is block-buffered when stdout is redirected** — the banner
  appears on a terminal or at exit only. Use stderr (`eprintln`) for logs in
  backgrounded runs.
- **Pool mode answers `Connection: close` on HTTP/1.1** (see Modes above).
