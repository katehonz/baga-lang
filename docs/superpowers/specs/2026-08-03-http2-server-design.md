# Design: `httpdbaga` v2 — High-performance, multi-threaded HTTP/1.1 + HTTP/2

Date: 2026-08-03
Status: Draft

## 1. Goal and non-goals

**Goal.** Upgrade `app-product/httpdbaga` from a one-request-per-connection
HTTP/1.1 parser into a serious multi-protocol server:

- **HTTP/1.1:** buffered reads (not 1-syscall-per-byte), persistent
  connections (keep-alive), chunked request bodies, `100-continue`, exact-N
  body reads (today a large body can be truncated by a short `read`).
- **HTTP/2 (h2c, RFC 7540/7541):** binary framing, HPACK with full decode
  (static + dynamic table + Huffman), stream multiplexing, SETTINGS/WINDOW_UPDATE/PING/GOAWAY/RST_STREAM,
  prior-knowledge detection on the same listener as HTTP/1.1.
- **Multi-threaded:** keep-alive connections are long-lived, so the accept
  model matters. Default stays `go_bg` per connection; a new `MODE=pool` runs
  a bounded worker pool over `std/par` channels (demonstrates CSP-style
  accept fan-in, bounds thread count under load).

The probe goal stands: every language friction hit while building this is
logged with evidence in `app-product/httpdbaga/gaps.md`.

**Non-goals (YAGNI):** TLS/ALPN (no TLS in `std/`; `https` needs a crypto
stack milestone of its own), HTTP/2 server push, stream priority handling
(PRIORITY frames are parsed and ignored), Huffman on the *encode* path (legal
to omit; decode is mandatory for interop), a generic router (still `if`
chains), HPACK dynamic-table use on the *encode* path (literal-without-indexing
output is fully conformant).

## 2. Verified language facts (empirical, 2026-08-03)

Probed against `./baga` before writing this spec (`tests/probe_binary_io.baga`):

- **Binary write works over `str` + explicit count.** `bytes` → `str_of_bytes`
  → `write(fd, s, n)` sends all n bytes including `0x00` and `0xFF`
  (`baga_bytes_to_str` is a memcpy with explicit length; libc `write` takes a
  count, not a strlen). Probed: `00 01 ff 00` round-tripped over loopback.
- **Binary read works into a `str_repeat` buffer.** `read(fd, buf, n)` fills
  the heap buffer in place; `byte_at(buf, i)` is a raw index (no strlen), so
  NUL and high bytes survive extraction. Probed.
- **`chr()` is a UTF-8 codepoint encoder, not a raw byte source.** `chr(255)`
  has `len` 2 (`0xC3 0xBF`). `std/net/tcp.baga`'s `poke8(mfd, off, v)` —
  `pwrite(mfd, chr(v & 255), 1, off)` — silently writes `0xC3` for any
  `v >= 128`. Latent bug: only fires for sockaddr octets ≥ 128, which the
  test-suite's `127.0.0.1`/`0.0.0.0` never hit. Logged as a gap (G8); this
  milestone does not use `poke8` for any value ≥ 128 and does not fix
  `tcp.baga` (no `std/` behavior change beyond additions).
- **`str_of_bytes` output containing NUL has broken `len()`** (strlen-based).
  Rule: never call `len`/`substr` on transport strings; carry explicit counts.
- **`extern fn` parameters are restricted to `i64`/`f64`/`str`** (checker
  error "неподдържан тип на параметър") — `bytes` cannot cross the FFI
  boundary. Hence the `str_of_bytes`+count write path above.
- **`sendfile(memfd → socket)` works** as an `extern fn sendfile(i64,i64,i64,i64)`
  (probed) but is unnecessary with the `str_of_bytes` path; dropped.
- HPACK tables are machine-extractable: `gen_hpack_tables.py` parses RFC 7541
  Appendices A+B and emits `hpack_tables.baga`, validating the canonical-code
  invariants (consecutive codes per bit-length, `(last+1) << Δ` jumps) plus
  spot checks (`/` = 0x18[6], EOS = 30×1, 257 entries). Generated file passes
  `./baga --check`.
- `str` is NUL-terminated C strings (`baga_len` = strlen) — binary bodies must
  travel as `bytes`; `Request.body` (str) stays for textual bodies, with a new
  `body_bytes: bytes` as the binary-safe source of truth.

## 3. Architecture

```
accept loop (server.baga)
  ├─ peek first bytes of connection
  │    ├─ "PRI * HTTP/2.0"  →  h2_conn(fd)        [h2.baga]
  │    └─ else              →  h1_conn(fd)        [http.baga keep-alive loop]
  ├─ MODE=pool: worker threads pull fd off a chan (chan_recv2)
  └─ default: go_bg per connection

h2.baga   frame layer + stream state machine, calls hpack.baga
hpack.baga  HPACK encode/decode over hpack_tables.baga (generated)
http.baga   HTTP/1.1 parser/serializer (existing API kept, additive v2 API)
std/net     + tcp_read_bytes / tcp_read_exact / tcp_write_bytes (binary-safe)
```

**Layering rule:** only `std/` under `httpdbaga`; `h2.baga` imports
`http.baga` (reuses `Request`/`Response` structs + header helpers); the demo
server imports both and chooses protocol by peeking.

## 4. std/net binary I/O (normative)

Added to `std/net/tcp.baga`:

```baga
fn tcp_read_bytes(fd: i64, n: i64) -> bytes !IO !Net
    // one read(); up to n bytes; binary-safe (byte_at extraction)
fn tcp_read_exact(fd: i64, n: i64) -> bytes !IO !Net
    // loops on partial reads; bytes_len < n only on EOF/error
fn tcp_write_bytes(fd: i64, b: bytes) -> i64 !IO !Net
    // str_of_bytes + write with explicit count, loops on partial writes
    // (bytes_slice the remainder; substr is not binary-safe). 0 ok / -1 err
```

`baga_bytes_from_str`/`strlen` are NOT used on the read path (truncates at
NUL); extraction is `byte_at` per byte into `Vec<i64>` then `bytes_from_vec`.

## 5. HTTP/1.1 v2 (http.baga, additive)

Kept byte-for-byte compatible: `http_read_request(fd)`, `http_respond(fd, r)`
(still `Connection: close`), all accessors — `tests/http_test.baga` and
`jwtbaga` must pass unchanged.

New API:

```baga
struct HttpReader { fd: i64, buf: bytes, pos: i64 }  // buffered, binary
fn http_reader(fd: i64) -> HttpReader
fn http_read_request_r(r: HttpReader) -> (HttpReader, Request) !IO !Net
    // returns updated reader (structs by value); Request.method == "" on EOF/malformed
fn http_respond_keepalive(fd: i64, resp: Response, keep: i64) -> i64 !IO !Net
    // keep==1: emits "Connection: keep-alive"; keep==0: close semantics
```

`Request` gains `body_bytes: bytes` (binary-safe); `body: str` remains the
textual view (`str_of_bytes(body_bytes)` — safe for genuinely textual bodies).
Header parsing is ASCII over the byte buffer; values with high bytes survive
as-is (never routed through `chr`).

**Request-loop semantics (server-owned; no closures in Baga):**

1. Read request via `http_read_request_r`.
2. `Expect: 100-continue` + body expected → write
   `HTTP/1.1 100 Continue\r\n\r\n` first, then read body.
3. `Transfer-Encoding: chunked` → chunk-decode (hex size line, CRLF pairs,
   terminal `0\r\n`); else `Content-Length` → `read_exact`.
4. Respond; `Connection:` header governs keep-alive (`close` → stop;
   HTTP/1.0 without `keep-alive` → stop; else continue).
5. Guards: header block ≤ 64 KiB → 431; body ≤ 8 MiB → 413; missing
   Content-Length on body-bearing methods with chunked absent → read 0 bytes
   (GET-like); malformed request line → 400 then close.

## 6. HTTP/2 (h2.baga + hpack.baga)

**Entry:** connection preface `PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n` (24 bytes)
read via the h1 peek buffer; mismatch on a "PRI"-prefixed connection → 400 +
close. Prior knowledge only; `Upgrade: h2c` is a stretch goal, not required.

**Frame layer.** 9-byte header: `length:24 | type:8 | flags:8 | R:1 | sid:31`,
big-endian (built/decoded byte-wise from `bytes`; no NUL-free assumption
anywhere). Types handled: DATA(0), HEADERS(1), PRIORITY(2, ignored),
RST_STREAM(3), SETTINGS(4), PUSH_PROMISE(→ connection error), PING(6),
GOAWAY(7), WINDOW_UPDATE(8), CONTINUATION(9).

**Stream machine.** Streams are client-initiated odd ids; per-stream state:
`OPEN → HALF_CLOSED_REMOTE (END_STREAM on request) → CLOSED (response sent)`.
Storage: `Vec` of stream records + linear scan (demo scale; logged as a scale
limit in README, not a gap — the language has no map type yet, which *is*
logged). HEADERS without END_HEADERS accumulates CONTINUATION (no interleaving
allowed; violation → connection error of type PROTOCOL_ERROR). Pseudo-headers
`:method :path :scheme :authority` map onto the existing `Request`
(`:authority` also exposed as header `Host` for handlers written for h1).

**Flow control.** Send side: per-connection + per-stream windows from peer
SETTINGS/WINDOW_UPDATE; demo responses are small — if a frame doesn't fit,
block reading frames until WINDOW_UPDATE arrives. Receive side: after DATA is
consumed, replenish with WINDOW_UPDATE on stream 0 + the stream (keep uploads
flowing, bounded buffers).

**SETTINGS.** Send ours on open
(HEADER_TABLE_SIZE 4096, ENABLE_PUSH 0, MAX_CONCURRENT_STREAMS 100,
INITIAL_WINDOW_SIZE 65535, MAX_FRAME_SIZE 16384); parse peer's; ACK theirs;
honor ACKs of ours.

**HPACK (hpack.baga).**
- Decode: indexed (static+dynamic), literal with incremental indexing,
  literal without indexing, never-indexed, dynamic table size updates;
  integer decoding (N-bit prefix + continuation bytes); string decoding with
  H bit → Huffman via the generated canonical tables (bitwise walk,
  padding ≤ 7 bits of ones, EOS rejection); dynamic table with 32-byte
  overhead accounting + FIFO eviction at our advertised limit.
- Encode (server→client only): indexed representation for exact static-table
  matches (`:status 200` = 8, `content-length` name = 28, ...), else literal
  without indexing (0x00) with name index when the name is static; strings
  raw (H=0); no dynamic-table inserts. Conformant by construction.
- `hpack_tables.baga` — generated (header says so); regenerated via
  `python3 app-product/httpdbaga/gen_hpack_tables.py /tmp/rfc7541.txt > ...`.

**Response path:** handler returns the same `Response` struct as h1 →
`h2_respond` encodes HEADERS (+ END_STREAM when bodyless) + DATA frames ≤
peer MAX_FRAME_SIZE, last with END_STREAM.

**Errors:** connection errors → GOAWAY(PROTOCOL_ERROR/…) + close; stream
errors → RST_STREAM; short/malformed frames never panic — decode guards map
to connection errors.

## 7. server.baga v2

- `PORT` (8080), `MODE` = `threads` (default, `go_bg` per connection) |
  `pool` (`POOL_N`, default 8, workers on a `chan_new` fd queue) | `sync`.
- Protocol detection: buffered peek of the first line/bytes —
  `PRI *` → h2; else HTTP/1.1 keep-alive loop.
- Routes unchanged (`/health`, `/`, `/hello`, `POST /echo`, 404) + new
  `/proto` reporting the connection's protocol (h1/h2) — proves multiplexing
  decisions are visible to handlers.
- Same handler function serves both protocols (h2 maps to `Request`/`Response`).

## 8. Effects & memory policy

All socket-touching functions carry `!IO !Net`; HPACK/frame codecs over
`bytes` are pure. Memory: leak-tolerant `std/` default; per-connection `bytes`
buffers are malloc'd and not freed (documented; arena-per-connection is a
logged roadmap gap). No new compiler changes — if a gap needs one, it is
logged, not fixed here.

## 9. Testing (test-first per task)

| Test | What |
|------|------|
| `tests/std/tcp_bytes_test.baga` | tcp_*_bytes: NUL/0xFF round-trip, exact-N across forced chunking, empty read |
| `tests/http_test.baga` (extended) | existing cases unchanged + keep-alive 2-requests-one-conn, chunked body, 100-continue, 400 on garbage |
| `tests/hpack_test.baga` | RFC 7541 known answers: C.1 integer coding (10/1337/42), C.3 request sequences (dynamic table evolution), C.4 Huffman sequence (`f1e3 c2e5 f23a 6ba0 ab90 f4ff` = "www.example.com"), encoder round-trip through our own decoder |
| `tests/h2_test.baga` | loopback h2: in-process client speaks preface/SETTINGS/HPACK-encoded HEADERS; asserts server SETTINGS+ACK, `:status` 200, body via DATA+END_STREAM; second multiplexed stream on the same connection; WINDOW_UPDATE replenishment on an upload |
| curl interop (manual, README) | `curl --http2-prior-knowledge localhost:PORT/health` + `curl -v` HTTP/1.1 keep-alive |

`make test` gains the three new test files; `./baga --check` on every new
library file.

## 10. Gap ledger (carried into gaps.md)

Already found in probing (pre-implementation):
- **G8** `chr()` is UTF-8, so `poke8`-style byte staging corrupts values ≥ 128
  (latent in `std/net/tcp.baga`; harmless for current callers).
- **G9** no byte-setter on `str`/`bytes` — constructing binary data needs
  `Vec<i64>` push loops or hex literals; per-byte assembly tax.
- **G10** `extern fn` cannot take `bytes` — binary FFI needs the
  str_of_bytes+count workaround.

More are expected during implementation (they are the point).

## 11. Conventions

- All comments, READMEs, docs in English.
- Baga style follows `std/` + existing `http.baga` (functional struct update,
  parallel-Vec pools, `?` effect propagation).
- Generated files carry a "GENERATED — do not edit" header + generator path.
- Every task ends green: `make` + relevant `./baga tests/...` pass before
  moving on.
