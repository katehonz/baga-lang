# httpdbaga

A minimal HTTP/1.1 request parser + response serializer for Baga, built only on
`std/` (`net`, `io`, `os`, `str`). One request per connection
(`Connection: close`); no keep-alive, no chunked encoding, no TLS, no router —
dispatch is plain `if` chains (see `server.baga`).

This is the first reusable application-layer library under `app-product/` and
the base `jwtbaga` will sit on. It is also a deliberate probe of the language —
see [`gaps.md`](gaps.md) for the friction found while building it.

## Files

| File | What |
|------|------|
| `http.baga` | the library: `Request`/`Response`, parser, serializer, JSON helper |
| `server.baga` | demo server (health / echo / 404) |
| `gaps.md` | language gaps found, with evidence and triage |

## API

```baga
struct Request  { method, path, version: str; hkeys, hvals: Vec<str>; body: str }
struct Response { status: i64; hkeys, hvals: Vec<str>; body: str }

fn http_read_request(fd: i64) -> Request !IO !Net   // "" method = malformed/EOF
fn http_method(r: Request) -> str
fn http_path(r: Request) -> str
fn http_body(r: Request) -> str
fn http_header(r: Request, name: str) -> str        // case-insensitive, "" if absent

fn http_response(status: i64, body: str) -> Response
fn http_set_header(resp: Response, k: str, v: str) -> Response
fn http_json_response(status: i64, body: str) -> Response   // sets Content-Type
fn http_respond(fd: i64, resp: Response) -> i64 !IO !Net    // 0 ok, -1 error
```

`http_respond` always appends `Content-Length` and `Connection: close` after any
user headers.

## Run the demo

```bash
./baga --emit-c app-product/httpdbaga/server.baga > /tmp/httpd.c
gcc -O2 -Iinclude -o /tmp/httpd /tmp/httpd.c -lm
PORT=8080 /tmp/httpd &

curl localhost:8080/health                 # {"status":"ok"}
curl localhost:8080/                        # {"method":"GET","path":"/"}
curl -X POST -d '{"hi":"baga"}' localhost:8080/echo   # {"hi":"baga"}
curl -i localhost:8080/nope                 # HTTP/1.1 404 Not Found
```

(`./baga app-product/httpdbaga/server.baga` compiles+runs too, but does not
forward `PORT`; use the two-step build above to set the port.)

## Test

```bash
./baga tests/http_test.baga     # loopback: GET/POST/header-ci/404/response-bytes
```

Also wired into `make test`.

## Effects & memory

`http_read_request` / `http_respond` carry `!IO !Net`; the builders and
`http_header` are pure. Memory follows the `std/` leak-tolerant default —
per-request strings are malloc'd and not freed; a long-running deployment would
wrap each connection in an arena (not built here; see `gaps.md`).

## Known sharp edges

- **Startup `print` is block-buffered when stdout is redirected.** The C backend
  emits `printf` with no per-call flush, so `print("listening...")` only appears
  on a terminal (line-buffered) or at exit. In a `&`-backgrounded server
  redirected to a file you won't see the banner until the process ends.
- **`read_line` keeps `\r`** — `http.baga` `str_trim`s every line and treats the
  header terminator as `""` *or* `"\r"`. See `gaps.md` G3.
