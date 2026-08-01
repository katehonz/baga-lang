# httpdbaga Implementation Plan

**Goal:** A minimal HTTP/1.1 request parser + response serializer
(`app-product/httpdbaga/http.baga`) over `std/`, a loopback test, a demo
server, and a `gaps.md` logging every language friction hit along the way.

**Spec:** `docs/superpowers/specs/2026-08-01-httpdbaga-design.md`

## Global Constraints

- **No compiler changes.** Gaps are logged in `gaps.md`, not fixed here.
- All comments/READMEs/docs in English.
- Build only on existing builtins + `std/` (`net`, `io`, `os`, `str`). `concat`
  is 2-arg only; `read_line` keeps `\r`; structs are by-value (functional
  update). These are confirmed facts, not surprises.
- Every task ends green: `make` builds, the relevant `./baga <file>` run passes.

## Verified facts relied on

- `str_split(s, " ")` preserves empty fields; `str_find` returns -1 on miss;
  `str_trim` strips space/tab/LF/CR; `parse_int` stops at first non-digit.
- `read_line` strips `\n`, keeps `\r`; header-block terminator is `"\r"`/`""`.
- `read_n(r, n)` reads up to n bytes; `tcp_write`/`tcp_read` wrap fd IO.
- Struct with `Vec<str>` fields + by-value pass + index lookup works (probed).
- `main` may carry effects (`-> i64 !Net !IO`).

---

## Task 1 — Scaffold + request parser

- [ ] Create `app-product/httpdbaga/http.baga` with header comment, imports
      (`../../std/net/tcp.baga`, `../../std/io/io.baga`, `../../std/str/str.baga`).
- [ ] `struct Request { method, path, version: str; hkeys, hvals: Vec<str>; body: str }`.
- [ ] Internal `to_lower(s)` (pure; `chr(ord-ish)` via char arithmetic — note:
      use `char_at` + `chr`; uppercase A–Z is 65–90, add 32).
- [ ] `http_read_request(fd) -> Request !IO !Net`:
      request line → trim → split " " → method/path/version (guard <3 parts →
      `method = ""`); header loop until `""`/`"\r"`: `str_find(line, ":")`,
      push key + trimmed value; `Content-Length` (ci) → `read_n`.
- [ ] Accessors `http_method`, `http_path`, `http_body`, `http_header`
      (case-insensitive scan of hkeys).
- **Verify:** `./baga --emit-c app-product/httpdbaga/http.baga > /dev/null`
      compiles (no main yet — emit-c only).

## Task 2 — Response builder + serializer

- [ ] `struct Response { status: i64; hkeys, hvals: Vec<str>; body: str }`.
- [ ] `http_response(status, body)`, `http_set_header(resp, k, v)` (functional
      update — return a new Response), `http_json_response(status, body)`
      (sets `Content-Type: application/json`).
- [ ] Internal `reason_phrase(status) -> str` (200/400/404/405/500, else "Status").
- [ ] `http_respond(fd, resp) -> i64 !IO !Net`: build
      `HTTP/1.1 <s> <reason>\r\n` + user headers + `Content-Length` +
      `Connection: close` + blank line + body; `tcp_write`; 0/-1.
- **Verify:** emit-c still compiles.

## Task 3 — Loopback test (test-first consumer)

- [ ] `tests/http_test.baga`: listen + connect + accept in one process (pattern
      from `tests/std/tcp_test.baga`). Inline `serve_one(fd)` reads a request,
      builds a JSON echo of `{method, path, header X-Probe, bodylen}`, responds.
- [ ] Cases: GET parse (method/path), case-insensitive header (`x-probe` sent
      as `X-Probe`), POST + Content-Length body, absent header → `""`,
      response bytes contain status line + `Content-Type: application/json` +
      body. `check(name, ok)` helper exits 1 on FAIL.
- **Verify:** `./baga tests/http_test.baga` prints `http_test: all passed`.

## Task 4 — Demo server + README

- [ ] `app-product/httpdbaga/server.baga`: `main() -> i64 !Net !IO`, port from
      `env("PORT")` or 8080, loop accept → dispatch (`GET /health`, `GET /`,
      `POST /echo`, else 404) → close.
- [ ] `app-product/httpdbaga/README.md`: what it is, API table, run + `curl`
      examples, effects/memory notes, pointer to `gaps.md`.
- **Verify:** run server in background, `curl /health` returns JSON, kill it.

## Task 5 — Wire into `make test`

- [ ] Add a `=== http (app-product) ===` block to the `test` target running
      `./baga tests/http_test.baga` (mirrors existing per-feature blocks).
- **Verify:** `make test` green end-to-end.

## Task 6 — gaps.md + triage

- [ ] `app-product/httpdbaga/gaps.md`: entry template (symptom → repro →
      workaround → severity → verdict). Seed with confirmed gaps (2-arg
      `concat`/no variadics; `read_line` keeps `\r`); add every new friction
      hit during Tasks 1–4 with the exact code that exposed it.
- [ ] End-of-milestone triage line per gap: roadmap candidate / YAGNI /
      app-specific.
- **Verify:** every gap has evidence (a file + observed behavior), no verdict
      left blank.

## Task 7 — Final validation + commit

- [ ] `make` clean build; `make test` green; demo `curl` smoke.
- [ ] Propose commit message; commit only on explicit user "commit".
