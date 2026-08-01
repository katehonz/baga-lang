# Design: `httpdbaga` — Minimal HTTP/1.1 Library for Baga

Date: 2026-08-01
Status: Draft (awaiting user approval)

## 1. Goal and non-goals

**Two goals, equal weight:**

1. **Product.** A minimal but serious HTTP/1.1 request parser + response
   serializer, built only on `std/` (`net`, `io`, `str`, `json`), plus a demo
   server. This is the first reusable application-layer library in
   `app-product/` and the foundation `jwtbaga` will sit on.
2. **Probe.** Building it is a deliberate stress-test of the language. Every
   friction point, missing feature, or awkward workaround is logged with
   evidence in `app-product/httpdbaga/gaps.md`. At the end of the milestone the
   gaps are triaged into language-roadmap candidates.

**Non-goals (YAGNI):** TLS, keep-alive, chunked transfer encoding, a generic
router with path params, concurrency (threads/async), cookies, URL-decoding of
query strings. `Connection: close` after every response — one request per
connection. The demo dispatches with explicit `if` chains; a router is extracted
only if `jwtbaga` proves the need.

## 2. Verified language facts (empirical, 2026-08-01)

Probed against `./baga` before writing this spec:

- `read_line` (std/io) strips `\n` but **keeps `\r`** — HTTP lines end `\r\n`,
  so every line needs `\r` stripped (`str_trim` does it; last byte is 13).
- The blank line terminating the header block arrives as `"\r"` (len 1), not
  `""`. End-of-headers is `line == "\r" || line == ""`.
- `concat` is strictly 2-argument — multi-part strings need nesting.
- A `struct` with `Vec<str>` fields works; by-value pass + index lookup works.
- `main` may declare effects (`fn main() -> i64 !Net !IO`, per
  `tests/std/tcp_test.baga`).

## 3. API surface (normative names)

```baga
// app-product/httpdbaga/http.baga — effects: !IO !Net on read/respond

struct Request {
    method: str,          // "GET" / "POST" / "" on malformed-or-EOF
    path: str,            // raw, query string included ("/x?a=1")
    version: str,         // "HTTP/1.1"
    hkeys: Vec<str>,      // header names, as received
    hvals: Vec<str>,      // header values, trimmed
    body: str             // Content-Length bytes ("" when absent)
}

struct Response {
    status: i64,          // 200, 404, ...
    hkeys: Vec<str>,
    hvals: Vec<str>,
    body: str
}

fn http_read_request(fd: i64) -> Request !IO !Net
fn http_method(r: Request) -> str
fn http_path(r: Request) -> str
fn http_header(r: Request, name: str) -> str   // case-insensitive, "" if absent
fn http_body(r: Request) -> str

fn http_response(status: i64, body: str) -> Response
fn http_set_header(resp: Response, k: str, v: str) -> Response
fn http_json_response(status: i64, body: str) -> Response   // sets Content-Type
fn http_respond(fd: i64, resp: Response) -> i64 !IO !Net    // 0 ok, -1 error
```

Status reason table is internal: 200 OK, 400 Bad Request, 404 Not Found,
405 Method Not Allowed, 500 Internal Server Error; unknown codes use "Status".

## 4. Parsing / serialization rules

**Request parse** (`http_read_request`):
1. Read the request line; `str_trim`; split on `" "` → method, path, version.
   Fewer than 3 parts → `method = ""` (caller treats as 400/EOF).
2. Loop `read_line` + `str_trim` until `""`: split each on the first `":"`,
   push name (untrimmed-case) and trimmed value into the parallel Vecs.
3. `Content-Length` (case-insensitive) → `read_n(fd, n)` into `body`. Absent or
   zero → `body = ""`.

**Response serialize** (`http_respond`):
```
HTTP/1.1 <status> <reason>\r\n
<each header>\r\n
Content-Length: <len(body)>\r\n
Connection: close\r\n
\r\n
<body>
```
`Content-Length` and `Connection: close` are always appended by the serializer
(user headers come first; if the caller sets them too, the caller's win by being
emitted first — documented, not defended).

## 5. Effects policy

`http_read_request` and `http_respond` carry `!IO !Net` (they touch a socket
fd). All struct builders and `http_header` are pure — visible purity in the
type, consistent with `std/`.

## 6. Memory policy

Same leak-tolerant default as `std/`: per-request strings are malloc'd and not
freed. The demo server is short-lived in tests; a long-running deployment would
wrap each connection in an arena (documented, not built — no arena plumbing
exists in the request structs yet; that is a candidate gap).

## 7. Testing

`tests/http_test.baga` — loopback, same pattern as `tests/std/tcp_test.baga`
(listen + connect + accept in one process). The test inlines a one-shot
handler that echoes the parsed request as JSON, then asserts:

- GET request line parsed (method/path/version)
- header lookup is case-insensitive (`host` vs `Host`)
- POST body read via Content-Length
- absent header → `""`
- `http_respond` bytes parse back: status line, `Content-Length`, body
- `http_json_response` emits `Content-Type: application/json`

Wired into `make test` as a new `test-app` block (mirrors the existing
per-feature blocks). The demo server is **not** in `make test` (it loops); it is
a run-manually artifact documented in the README.

## 8. Demo server — `app-product/httpdbaga/server.baga`

`fn main() -> i64 !Net !IO`: port from `env("PORT")` or `8080`; loop
`tcp_accept`; dispatch:

- `GET /health` → 200 JSON `{"status":"ok"}`
- `GET /` → 200 JSON echo of `{method, path}`
- `POST /echo` → 200 JSON echo of the body
- else → 404 JSON `{"error":"not found"}`

One connection at a time, `Connection: close`. README shows `curl` invocations.

## 9. Language-gaps artifact

`app-product/httpdbaga/gaps.md` is a committed deliverable. Each entry:
**symptom → minimal repro → current workaround → severity → verdict**. Verdicts
are assigned at milestone end (roadmap candidate / YAGNI / app-specific). Known
candidates entering this milestone (from §2 probes): 2-arg-only `concat`, no
variadics, `\r`-keeping `read_line`. More will be added as the implementation
hits them — that is the point.

## 10. Conventions

- All comments, READMEs, docs in English (user requirement).
- Baga style follows `std/` and `self/compiler.baga` (functional struct update,
  parallel-Vec pools, 2-arg `concat`).
- No compiler changes in this milestone — if a gap needs one, it is logged in
  `gaps.md`, not fixed here (keeps the probe honest and the diff small).
