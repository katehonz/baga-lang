# Changelog

## [Unreleased]

### TLS 1.3 client, T6 — X.509 + RSA-PSS CertificateVerify
- `std/crypto/der.baga`: minimal DER walker (SEQUENCE / INTEGER / BIT
  STRING / OID, definite lengths).
- `std/crypto/rsa.baga`: `rsa_public` (s^e mod n via bn), PKCS#1 v1.5
  SHA-256 verify (X.509 cert signatures) and EMSA-PSS SHA-256 verify
  (sLen=32, MGF1-SHA256 — TLS `rsa_pss_rsae_sha256`).
- `std/crypto/x509.baga`: parse Certificate → TBS + RSA SPKI (n, e) +
  signature; self-signed and trust-anchor checks.
- `std/net/tls.baga`: `TlsFlight` exposes `cv_algo` / `cv_hash`;
  `tls_verify_server(flight, anchor_der)` trusts the leaf and verifies
  CertificateVerify. Empty anchor = accept self-signed (dev peer).
- `tests/std/rsa_pss_test.baga` — python `cryptography` golden vectors
  (PSS accept/reject + self-signed cert). Live openssl path in
  `tls_handshake_test` now asserts cert + CV.
- Honest gaps: no name constraints / time / revocation; ECDSA is T7.

### Toolchain / packaging — Makefile is C-only; tests via sandak + baga-test
- The root `Makefile` no longer embeds the regression suite (~670 lines of
  hand-listed package tests). It builds the C toolchain only (`baga`,
  `sandak`, optional `baga-llvm`, `libbaga_par.so`) and thin targets
  (`test`, `self`, `test-llvm`).
- `make test` → `scripts/run_tests.sh`:
  - **sandak** discovery — every `app-product/*/sandak.toml` and
    `apps/*/sandak.toml` is built (no hand-maintained package list);
  - **baga-test** discovery — every `tests/**/*_test.baga` (specials
    with env/peers: registry, oauth PG, TLS vs openssl);
  - **run_verify.sh** — the static `--verify` oracle (M0–M18).
- Closes the GitHub-linguist skew where Makefile looked like a large
  share of the repo; package work stays in the package system.

### TLS 1.3 client, T4+T5 — record layer, ClientHello, encrypted handshake
- `std/net/tls.baga`: TLS 1.3 client core — record layer
  (`tls_read_record`), ClientHello builder (x25519 key share,
  supported_versions, signature_algorithms), ServerHello parser, the RFC
  8446 §7.1 key schedule (`tls_schedule`), flight decryption
  (`tls_open_handshake`: multi-record, GCM sequence numbers, inner
  content-type stripping, transcript walk, server-Finished HMAC verify)
  and the client Finished builder (`tls_finished_record`).
- `tests/tls_handshake_test.baga` — a full live handshake against
  `openssl s_server -tls1_3` with a fresh self-signed cert, wired into
  make test: ClientHello → ServerHello → decrypt
  EncryptedExtensions/Certificate/CertificateVerify/Finished → verify the
  server Finished → send the client Finished and assert no alert.
- Scars, documented:
  - the "derived" key-schedule step takes the transcript hash of the
    empty message (SHA-256 of "") as context — not an empty context;
    validated against RFC 8448 known answers before the live server
    would decrypt anything;
  - the ServerHello key_share is a single KeyShareEntry (the list-length
    wrapper exists only in the ClientHello);
  - openssl splits the flight into separate records (EE | Cert | CV+Fin)
    and sends a middlebox-compat ChangeCipherSpec; the flight reader
    handles both;
  - `openssl s_server` without `-quiet` resets connections (stdin loop);
    the harness runs it with `-quiet < /dev/null`;
  - `Vec<bytes>` is not a supported element kind yet (L4) — the flight
    reader takes a u24-length-prefixed `bytes` instead.

### TLS 1.3 client, T3 — HKDF + AES-GCM (std/crypto)
- `hkdf.baga`: RFC 5869 HKDF-SHA256 (extract with the empty-salt →
  HashLen-zeros rule, expand); Appendix A cases 1–3 pass.
- `aes.baga`: AES-128/256 forward cipher (FIPS-197 C.1/C.3). The S-box
  is computed at expand time from GF(2^8) inversion + the affine map —
  no 256-byte literal to mistype. Decryption is deliberately absent:
  GCM needs only the forward direction.
- `gcm.baga`: AES-GCM AEAD — GHASH in GF(2^128) (right-shift form,
  R = e1‖0^120), CTR from J0 = nonce‖00000001 (12-byte nonces, the
  TLS 1.3 shape), 16-byte tags verified with ct_eq_b.
- `tests/std/hkdf_test.baga` + `tests/std/aes_gcm_test.baga`: vectors
  from RFC 5869, FIPS-197, and python `cryptography` generated offline
  (AES-256-GCM, 60-byte non-block PT + AAD, AAD-only, tamper/nonce/AAD
  rejection). Both in the make test std loop; ~0.5 s each.
- Scars, documented: the S-box affine rotations were first written as
  rotl 5/6/7 instead of 3/2/1, and ShiftRows filled its buffer in
  push order (a transpose) — both caught by the intermediate-state
  probes against the FIPS walkthrough, not by the end-to-end vector.

### TLS 1.3 client, T2 — std/crypto/x25519.baga (RFC 7748 ECDH)
- X25519 on top of bn.baga: clamped scalars, Montgomery ladder (bits
  254..0) with constant-time conditional swaps, field arithmetic mod
  2^255-19 with the 2^255 ≡ 19 fold, final inversion z^(p-2) over fmul.
  Little-endian encoding per the RFC. A full scalar multiply ≈ 0.1 s.
- `tests/std/x25519_test.baga` — RFC 7748 §5.2 (first iteration, k=u=9)
  and §6.1 (both public keys + shared secret, both directions); in the
  make test std loop. The 1,000-iteration vector is documented but kept
  out of CI.
- Scar, documented: the first fold dropped a carry that escaped limb 9
  (≥ 2^260 must fold by ×608 again) — (p-1)² came out p-607 instead of
  1, exactly one lost 608. Caught by the modexp-free field probes.

### TLS 1.3 client, T1 — std/crypto/bn.baga (fixed-width bignum)
- The milestone plan is `docs/superpowers/plans/2026-08-04-tls-client.md`
  (T1–T8; closes №10 P2 / gap G6 — the last production blocker).
- New `std/crypto/bn.baga`: unsigned bignum on 26-bit limbs in
  `Vec<i64>` — add/sub/cmp, schoolbook mul, **in-place** shift-subtract
  mod, modmul, left-to-right modexp, big-endian byte codec. Signed-i64
  discipline: widest RSA-2048 column stays below 2^59.
- `tests/std/bn_test.baga` — 19 golden-vector checks, oracle = python
  bigints computed offline: byte round-trips, 256-bit mul/mod/exp
  (NIST P-256 prime), and RSA-2048 modexp over the RFC 3526 group prime
  (e=65537 fast path + 512-bit exponent slow path, ~1.5 s measured).
- Design scar, documented: the first `bn_mod` rebuilt the shifted modulus
  per bit — thousands of arena allocations per mod, and the bump arena
  never reclaims, so the slow-path modexp was OOM-killed. In-place
  reduction fixed it (43 s OOM → 2.1 s green).

### std/net — URL percent-encoding (oauthbaga gap O2)
- New `std/net/url.baga`: `url_encode` / `url_decode` (RFC 3986 §2.1) —
  unreserved set passes through, everything else `%XX` per UTF-8 byte;
  decode is byte-exact (rebuilt through `bytes`, since `chr()` would
  UTF-8-encode values ≥ 0x80 and double-encode the stream) and lenient
  (malformed `%XX` and `%00` copy through literally — baga strings are
  C strings).
- `tests/std/url_test.baga` — 20 checks incl. Cyrillic/emoji round-trips;
  in the `make test` std loop.
- First user: oauthbaga percent-encodes `redirect_uri` in the authorize
  redirect and the provider decodes it.

### App products — oauthbaga worker model (O5 closed)
- PG mode is now fully concurrent: every HTTP connection runs on its own
  `go_bg` worker with its own DB connection (fmr legacy idiom); the CSRF
  authorize states moved to an `oauth_states` table (migration
  20260804102), so nothing is shared between threads. Dev (in-memory)
  mode stays serial.
- `oauth_pg_test` proves the cross-thread flow: the `/login` state row is
  written by one connection and consumed by another; the suite is now on
  the default ports (the workers rebuild config from env).
- `demo.baga` migrates once before booting the two nodes (two concurrent
  `migrate_up` would race on the version insert).
- Honest scar, recorded in gaps.md: the first bg handlers responded
  without `tcp_close(fd)` — with Connection: close the client reads to
  EOF, so an unclosed fd hangs clients until their read timeout.

### Compiler — call-site Vec/Map element inference (tplbaga P5 closed)
- **Soundness fix.** An unannotated `vec_new()` / `map_new()` passed to a
  typed parameter (`Vec<str>`, `Map<str,str>`, …) now gets its element
  kind fixed from the callee parameter at the call site — the same
  fix-on-first-use mutation `vec_push` already did. Before, a later
  `vec_get` fell into the historical i64 default and the codegen read
  `str` memory as `i64`: garbage output, no diagnostic, no compiler
  complaint. Found by tplbaga (№7) the way the roadmap intended.
- The reverse order (`vec_get` before the fixing call) is now a compile
  error instead of a silent wrong type.
- Two regressions in `make test` (vec + map call-site inference).
- `type_str`: rotating buffers — two `Vec`/`Map` types in one diagnostic
  no longer overwrite each other's text.

### Compiler — LLVM `ord` decodes UTF-8 (oracle parity)
- The LLVM backend's `baga_ord` returned the first *byte* (208 for "А");
  the C runtime decodes the code point (1040). Now both backends decode
  1–4 byte UTF-8 sequences; `examples/strings.baga` matches byte-for-byte
  and `make test-llvm` is fully green.

### App products — oauthbaga P1 (Postgres persistence, №10 "всичко заедно")
- The pgbaga/ormbaga leg of №10: `oauth_codes` / `oauth_refresh` /
  `oauth_sessions` tables (goose-style migrations, version 20260804101),
  `$N` parameterized queries only; `OAUTH_PG=1` + `PG*` switches the
  backend, in-memory maps remain the dev mode (both live-tested).
- Store ops meet at typed rows (`OaCode`/`OaTok`/`PxSess`); the JSON
  record codec is now contained to the in-memory backend (O3 shrunk).
- One DB connection per HTTP connection (fmr legacy idiom) — no struct
  rebinding through the handler chain, concurrency-safe, and the natural
  path to a worker pool (O5 mostly closed; the CSRF `states` map stays
  per-node for now, O7 notes the per-request SCRAM cost).
- `tests/oauth_pg_test.baga` — live full cycle on ports 18692/18693 plus
  DB-level proofs: the code row is consumed by the exchange, refresh
  rotation keeps exactly one live token, the session row appears on
  login and dies on logout. Wired into `make test` like registry_test
  (`OAUTH_PG=1 PGDATABASE=baga_oauth`).

### App products — oauthbaga (OAuth proxy, apps-roadmap №10 complete)
- New product `app-product/oauthbaga`: the integration exam — std HTTP
  client (№2) + jwtbaga + cookie sessions, pages rendered by tplbaga
  (first cross-product dependency in the series; №7 feeds №10).
- Provider node: `/oauth/authorize` (auto-approve dev profile, one-time
  codes), `/oauth/token` (authorization_code + refresh_token grants with
  rotation), `/api/me` (Bearer JWT guard); RFC 6749-shaped JSON errors.
- Proxy node: `/login` (CSRF state) → `/callback` (real server-to-server
  code exchange over the std client) → `sid` cookie session with
  transparent refresh → `/logout`.
- Two serial nodes (`cell2` ports, kvbaga idiom): the provider never
  calls itself, so no self-accept deadlock; state is `Map<str,str>` of
  JSON records (L4 stand-in); `TokenReply`/`PxTokens` continue the L3
  Result stand-in convention.
- `demo.baga` boots both nodes (`OAUTH_PORT`/`OAUTH_PROVIDER_PORT`/
  `OAUTH_SECRET`); a browser completes the flow on loopback.
- `tests/oauth_test.baga` — live full cycle: authorize → exchange →
  bearer → protected → refresh/rotation → browser flow → logout
  (47 checks); runs via `scripts/baga-test`.
- Probes: O2 no URL percent-coding in std (third hand-rolled query
  parse); O5 go_bg carries i64 only → serial nodes until state moves to
  Postgres (P1); TLS/G6 still the production blocker (O1).

### httpdbaga — 302 reason phrase
- `reason_phrase(302)` now says "Found" (was the generic "Status") —
  the OAuth redirects were the first 3xx on the wire.

### App products — tplbaga (HTML templates, apps-roadmap №7)
- New product `app-product/tplbaga`: mustache-ish subset — `{{ expr }}`
  escaped interpolation, `{{{ expr }}}` raw, `{% if %}` / `{% else %}` /
  `{% endif %}` (nestable, `!` negation), `{# comments #}`, filter chains
  `{{ v | trim | upper }}` (upper/lower/trim/len/default:arg, ASCII case).
- Tokens are prefix-encoded `Vec<str>` + a `Map<i64,i64>` jump table for
  block pairing — one iterative walk, no recursion (L4 stand-in); `TplOut`
  ok/err struct as the L3 stand-in; filters dispatch by name — the
  designated L5 (closures) probe.
- `demo.baga` CLI: template file + `key=value` data file (exit 0/1/2).
- `tests/tpl_test.baga` — 46 checks (escape, jump table, filters, error
  paths, realistic page); runs via `scripts/baga-test`.
- Probes: unannotated `vec_new()` passes the checker but codegen emitted
  i64 element access until annotated (P5, compiler bug candidate); `spec`
  keyword cannot be an identifier and the diagnostic doesn't say so (P6).

### App products — jsonrpcbaga (JSON-RPC 2.0, apps-roadmap №6)
- New product `app-product/jsonrpcbaga`: JSON-RPC 2.0 subset over HTTP —
  single/batch, notifications, standard error codes, methods
  `ping`/`add`/`echo`/`fail` via name switch.
- `RpcResult` struct as L3 Result stand-in; `rpc_handle_body` pure +
  `rpc_serve` accept loop; `tests/jsonrpc_test.baga` (pure + live HTTP).
- Gaps: no sum types (R1/L3), no function-value method table (R2/L5).

### App products — queuebaga (task queue, apps-roadmap №5)
- New product `app-product/queuebaga`: disk-backed jobs, `chan` of job ids,
  `go_bg` workers, reverse-payload demo work, `fail:` retry until max
  attempts, `q_wait` with timeout.
- Flat paths `<prefix>.<id>.{job,status,result,attempts}` (no mkdir).
- `tests/queue_test.baga` + demo. Gaps: i64-only chan/go (Q1), no setenv
  (Q2), write_file truncate races (Q3), no L5 handlers (Q5).

### App products — grebaga (grep-like CLI, apps-roadmap №9)
- New product `app-product/grebaga`: literal + mini-pattern (`.`/`*`/`\`),
  ASCII `-i`, streaming line scan (chunked read; empty line ≠ EOF), CLI
  `demo.baga` (`-n`/`-i`, files or stdin, exit 0/1/2).
- `tests/grep_test.baga` — match unit tests + live file stream.
- Probe: std `read_line` empty/EOF collapse → custom scanner (G1).

### App products — testbaga (test asserts + runner, apps-roadmap №8)
- New product `app-product/testbaga`: fail-fast `assert_true` /
  `assert_eq_i64` / `assert_eq_str` / `assert_ne_str`, plus `Suite`
  (continue-on-fail, `suite_finish` → exit code).
- `scripts/baga-test` — discovers `*_test.baga` and runs each via baga
  (shell driver: no readdir/process spawn in language yet).
- Dogfood: `tests/testbaga_test.baga`; `tests/std/sort_test` migrated off
  local `check`.
- Gaps: T1 process spawn, T2 list_dir, T3 function values (L5).

### App products — mdbaga (Markdown → HTML, apps-roadmap №4)
- New product `app-product/mdbaga`: CommonMark-ish subset — ATX headings,
  paragraphs, emphasis, inline/fenced code, ul/ol, blockquotes, hr, links,
  HTML escape; `md_to_html` / `md_to_document`.
- CLI `demo.baga` reads `arg(0)` via `read_file`, prints HTML (`MDDOC=1` for
  full document shell). Package: `sandak build`.
- `tests/md_test.baga` — escape, blocks, inline, XSS-ish `<` in text/code.
- Probe gaps: nested concat still dominates builders (M1 / G1); no
  file-exists vs empty distinction on `read_file` (M2).

### App products — chatbaga (WebSocket chat, apps-roadmap №3 complete)
- New product `app-product/chatbaga`: multi-room chat on a single-threaded
  `poll(2)` event loop — JSON join/msg over wsbaga text frames, room
  broadcast, leave notifications, error replies.
- Closes **W1 / K1** (serial accept): one poll set watches the listener +
  every client fd; connection state lives in `Map`s keyed by fd.
- Forced **`Map` bytes values** into the language (`Map<i64, bytes>` residual
  buffers) — also the path to close kvbaga K2 for binary store values.
- `demo.baga` standalone server (`CHATPORT`, default 16460); interop with
  `wscat` and raw RFC 6455 clients (UTF-8 text, multi-client broadcast).
- `tests/chat_test.baga` — 18 live checks (two clients, errors, room
  isolation, close/`left`); package via `sandak build` (app-product list).

### std/net — poll(2) event loop primitive
- `std/net/poll.baga`: `poll_wait(fds, timeout_ms)` / `poll_has` over
  SYS_poll (POLLIN|POLLERR|POLLHUP). Same memfd staging pattern as tcp.
- `tests/std/poll_test.baga` in the `make test` std loop.

### Language — Map bytes values (kvbaga K2 path)
- `Map<K,V>` values may be `bytes` (in addition to i64/str/f64). Checker +
  C runtime (`baga_map_*_bytes`); missing key → empty bytes.
- `tests/std/map_test.baga` covers NUL/0xFF round-trip through map values.

### App products — wsbaga (WebSocket, apps-roadmap №3)
- New product `app-product/wsbaga`: RFC 6455 — server handshake
  (`Sec-WebSocket-Accept`), frame codec (FIN/opcode, 7/16/64-bit lengths,
  client masking), text/binary/ping/pong/close handling, buffered
  `ws_read_frame`, echo server `ws_serve(port)`, and a masked client
  (`ws_client_connect` verifies the accept key).
- **Interop-verified**: `wscat` (Node.js) echoes UTF-8 text and 900-byte
  payloads against the Baga server; loopback `tests/ws_test.baga` covers
  all length boundaries (125/126/65535/65536), binary with NUL/0xFF,
  ping→pong, close→EOF (14 checks).
- Honest limits in gaps.md: serial accept closed by chatbaga (W1 = K1 →
  poll); no fragmented-message reassembly (W2) still open.

### std/crypto — SHA-1 (probed into existence by wsbaga)
- `std/crypto/sha1.baga`: RFC 3174, same shape as sha256 (Vec core +
  `bytes` wrappers: sha1/sha1_hex/sha1_b/sha1_b_hex).
- `tests/std/sha1_test.baga`: RFC vectors incl. million-'a' and the
  RFC 6455 accept-key vector; in the `make test` std loop.
- SHA-1 only for protocol mandates (RFC 6455); sha256 stays the default.

### apps/registry — пакетен registry за sandak (apps-roadmap №2, втора половина)
- New app `apps/registry`: JSON/HTTP package index on the fmrbaga/ormbaga/
  pgbaga stack — `GET /v1/packages[?q=]`, `GET /v1/packages/{name}`,
  `POST /v1/packages` (publish = upsert package + unique version; 409/422
  error shapes). Migrations create `reg_packages` / `reg_versions`.
- `sandak search [term]` / `sandak publish --git URL [--rev R] [--subdir S]
  | --path P` — the client is a Baga program (`src/sandak_registry.baga`)
  executed by sandak through the compiler, talking HTTP via the new std
  client. Registry URL from `SANDAK_REGISTRY` (default http://127.0.0.1:8090).
- `baga` CLI gained **program arguments**: `baga prog.baga arg1 arg2…` (and
  an explicit `--` separator) — everything after the input file reaches
  `arg()`/`arg_count()` of the compiled program. Before this, `arg()` had
  no way to receive values through compile-and-run.
- fmrbaga `jbody_parse_str` now rejects malformed bodies with
  `json_strict_valid` before the lenient parse (G13 in a real request path).
- `tests/registry_test.baga` — first full-stack live HTTP test: boots the
  server in a go_bg worker, drives it through std/net/http_client (18
  checks: publish/dup-409/show/index/search/404/400/422). In `make test`.

### std/net — HTTP/1.1 client (apps-roadmap №2, първа половина)
- `std/net/http_client.baga`: `http_request(method, url, headers, body,
  timeout)` + `http_get` / `http_post`. URL parse (http:// only — https
  waits for TLS), DNS hostnames through `tcp_connect_to`, `Map<str,str>`
  request/response headers (lowercased, case-insensitive lookup via
  `http_resp_header`), Content-Length + chunked bodies, read-to-close.
- First product of the map type in std itself: headers are `Map<str,str>`.
- `tests/std/http_client_test.baga` — 17 live loopback checks against an
  httpdbaga worker (GET/POST/UTF-8 bodies, chunked, 418, refused, bad URL);
  wired into `make test`.
- Gap found (L6): no namespaces — the client's `http_header` collided with
  httpdbaga's; renamed to `http_resp_header`. Prefix convention holds until
  module scope exists.

### Language — `main -> i64` exit code (kvbaga K3 closed)
- The C wrapper emitted `b_main(); return 0;`, swallowing the exit code of
  `fn main() -> i64`. Now `return (int)b_main();` for i64/i32 mains; void
  mains unchanged. The baga CLI already propagated `WEXITSTATUS`.
- Regression check in `make test`; kvbaga gaps.md K3 closed.

### App products — kvbaga (Redis-compatible KV server)
- New product `app-product/kvbaga`: a RESP2 KV server built deliberately on
  the new map type — the first "app as language probe" on `Map<K,V>`.
- `resp.baga` (pure RESP2 codec: buffered parse, reply builders, client
  round-trip), `store.baga` (`Map<str,str>` + `Map<str,i64>` deadlines,
  lazy TTL expiry), `server.baga` (serial accept loop for `go_bg`,
  idle `SO_RCVTIMEO` guard).
- Commands: PING, SET [EX s], GET, DEL, EXISTS, INCR, KEYS, EXPIRE, TTL,
  DBSIZE, QUIT — Redis-shaped errors (`-ERR`, nil bulks, arity checks).
- Honest limits logged in gaps.md (K1–K5): serial connections (`go()`
  carries only i64 — the store can't cross threads), text-only values,
  and the swallowed `main` exit code (K3 — repo idiom is `exit(1)`).
- Tests: `tests/kv_test.baga` — 27 live loopback checks; demo boots a
  worker and drives it. Both wired into `make test`.

### Language — `Map<K, V>` (first-class hash table)
- New type `Map<K, V>`: keys `i64`/`str`, values `i64`/`str`/`f64`/`bytes` —
  the same fix-on-first-use rules and annotations as `Vec<T>`; mixing key or
  value types is a compile-time error. (`bytes` values added with chatbaga.)
- Builtins: `map_new`, `map_set`, `map_get` (zero-value when absent),
  `map_has`, `map_del`, `map_len`, `map_keys` (→ `Vec<str>`/`Vec<i64>`).
- Maps are pointers: passing one to a function shares it (mutate-through,
  unlike by-value structs) — the natural store for servers and caches.
- C backend: chained hash table (`baga_Map`, FNV-1a / Murmur-mix hashing,
  grows at load factor 3/4). LLVM backend: honest "unsupported" diagnostic.
- Self-hosting parity unchanged (`make self` fixed point holds); the self
  compiler does not parse `Map` yet (documented limitation).
- Docs: `docs/language-{en,bg}.md` §12.5 + type/builtin tables.
- Tests: `tests/std/map_test.baga` (bytes + rehash growth) + two negative
  type-error checks wired into `make test`.

### std/net — production connects
- **DNS resolution:** `tcp_resolve_ipv4` — hostnames via `getaddrinfo`
  (AF_INET, `mem_read` pointer-walk through the `addrinfo` list); dotted
  IPv4 still short-circuits the resolver.
- **Timeouts:** `tcp_set_timeouts` (SO_RCVTIMEO + SO_SNDTIMEO) — a blocked
  read/write/connect fails instead of hanging forever.
- **Client tuning:** `tcp_set_nodelay` (TCP_NODELAY), `tcp_set_keepalive`
  (SO_KEEPALIVE); `tcp_connect_to(host, port, timeout_s)` wires all of it.
  `tcp_connect` keeps its classic behavior.
- New primitive `mem_read(addr, n)` — copy arbitrary process memory into a
  Baga `str` via memfd (with the offset reset; SYS_write advances it).

### App products — pgbaga (Postgres adapter)
- **Production connect:** `pg_connect_to(host, port, ..., timeout_s)` —
  hostname or IPv4, bounded connect/read/write; `pg_set_timeout` retunes a
  live connection; **`pg_cancel`** sends CancelRequest on a fresh connection
  using the BackendKeyData captured at startup.
- **JSON/JSONB tables end to end:** `pg_param_json` binds (`$N::json[b]`),
  column OID detection (`pg_col_is_json` / `pg_col_is_jsonb`), JSON cell
  accessors (`pg_cell_json` / `pg_cell_json_ok`), and validated literals in
  ormbaga (`sql_json` / `sql_jsonb`).
- `std/json`: new `json_strict_valid` — a strict RFC 8259 validator
  (the existing `json_parse` stays lenient for recovery).
- Typed getters: `pg_cell_bool`, `pg_cell_f64`; transaction wrappers
  `pg_begin` / `pg_commit` / `pg_rollback`; structured error accessors
  `pg_sqlstate` / `pg_err_message`.
- `PgReader` now lives inside `PgConn` — buffered socket state survives
  across queries (gap G9 closed; ground for LISTEN/NOTIFY later).
- Hardening: `pg_read_msg` rejects message lengths outside `[4, 2^30-1]`.
- `tests/pg_test.baga`: live JSON table round-trips + strict harness
  (a FAIL now exits 1 instead of printing "all passed"); 70 checks.

### Packages — sandak (пакетна система)
- New tool `sandak`: `sandak.toml` manifests, path + git dependencies
  (with `subdir` for monorepos), `sandak.lock` with `--locked`, and
  `fetch`/`build`/`run` commands. Zero dependencies (libc + git + gcc).
- Compiler: repeatable `-I <dir>` import search path flag.
- The whole monorepo is packaged: `std`, `app-product/*`, `apps/api` have
  manifests; imports are package-named (`import "fmrbaga/app.baga"`).
- Docker: multi-stage `Dockerfile` + `docker-compose.yml` — point `APP_REPO`
  at a git URL and the container clones toolchain + app + deps and builds.

## [0.7.0] — 2026-08-02

Second tagged release: M14–M18 static verification, soundness fixes, evaluation
and research docs. CLI: `baga --version` / `-V` prints `baga 0.7.0`.

### Static verification — M18: `!Overflow` as an effect (effect system ≡ verifier)
- Arithmetic safety (M15) is now a **type-level effect**. `!Overflow` is a
  permission (like `!IO`), not a claim: the M15 kind-4 obligations are the
  *effect inference* for `!Overflow`, and the one-way effect check is the
  *discharge*. The effect system and the verifier become one judgement.
- A function **without** `!Overflow` claims overflow-safety; `--verify`
  proves it (`ефект !Overflow: безопасна — типът е точен`), refutes it with a
  concrete witness when it overflows (undeclared overflow ⇒ nonzero exit), or
  honestly reports НЕ МОГА ДА РЕША.
- A function **with** `!Overflow` is discharged: the overflow is still printed
  as evidence, but it is no longer a contract violation and does not fail
  verification (`ensures` verdicts are idealized-ℤ-only). Over-declaring
  `!Overflow` on a provably-safe function is allowed (noted as redundant).
- `!Overflow` propagates through calls via the generic effect merge — a caller
  must declare or catch it ("необработен ефект !Overflow"); no checker change
  was needed.
- The fragment gate now admits `{Par, Overflow}` (`ret_has_unverifiable_effects`);
  functions with other effects still skip honestly and make no overflow claim.
- The M15 exit-flag rule is gated: a REFUTED arithmetic obligation fails
  verification only when the function does not declare `!Overflow`. No
  existing example declares `!Overflow`, so all prior exit codes are unchanged.
- `--verify --json` adds an `overflow_effect` field
  (`{analyzed, declared, safe, result, witness}`); `--proofs` emits a
  `theorem <fn>_overflow_safe`.
- Examples: `examples/verify/ovf_eff_{safe,refuted,declared,unknown,redundant,skip,propagate,propagate_ok}.baga`.
- Notes: `docs/thesis-m18-overflow-effect.md` (the culmination),
  `docs/thesis-open-problems.md` (liveness / full BV / rich polynomials),
  `docs/thesis.md` (binding research monograph).
- Doc seriousness pass: research monograph/notes without degree theatre;
  proof sketches vs LA certificates; CLI/`--verify` recursion claim;
  self-host LOC (~2660); STLC SN not claimed for full Baga; theory placement
  among tools instead of curriculum comparisons.

### Static verification — M17: pair abstraction (`cell2` + channel pair APIs)
- `cell2(a,b)` / `cell2_0(p)` / `cell2_1(p)` are exact rewrites in the
  verifier (`cell2_0(cell2(a,b)) = a`) — allowed anywhere, including inside
  conditions (`if cell2_0(r) == 1`).
- The pair-returning channel APIs are now in the fragment with ranges for
  the status component and M16 content axioms for the value component:
  - `chan_recv2` (ok ∈ [0,1]), `chan_try_recv` / `chan_recv_timeout`
    (status ∈ [0,2]), `chan_select2*` (which ∈ [0,3]; value gets only the
    axioms BOTH channels share).
  - `select2_wait`'s which ∈ {0,1,3} is modeled as the interval [0,3]
    (over-approx; the abstract status keeps refutations honest).
- `go(worker, cell2(a, b))`: packed arguments work; a worker's
  `requires cell2_1(p) >= 1` is discharged at spawn where the pair's
  components are visible. Inside the worker, packed params stay honestly
  opaque.
- Examples: `examples/verify/pair_{recv2,select,go}.baga`.
- Note: `docs/thesis-m17-pairs.md`.

### Static verification — M16: channel content invariants (rely–guarantee)
- New statement-level annotation `invariant <expr>` (contextual keyword):
  - `invariant c[*] >= 1` — "every payload sent on channel `c` satisfies the
    predicate", anchored on the channel's resolved symbolic var (aliases work).
  - scalar form (no `[*]`) acts as `assume` — the path gains the constraint.
  - `chan_send` discharges the predicate (else the axiom is dropped, M3
    rule); `chan_recv` instantiates it on the result.
- Cross-thread: a worker's `requires c[*] ...` is discharged against the
  caller's axioms at `go` spawn (kind-2 obligation, provable); a worker
  without matching requires drops them at spawn — honest, never unsound.
  The same discharge/drop rules apply at plain M5 calls.
- `go` workers may now declare `Par` effects (channel-using workers were
  previously outside the fragment; non-`Par` effects still skip).
- Examples: `examples/verify/chan_inv{,_bad,_par,_escape}.baga`.
- Note: `docs/thesis-m16-channel-invariants.md`.

### Static verification — M15: arithmetic safety (the ℤ-vs-i64 bridge)
- New kind-4 obligations: every `+ - * -x / % <<` in verified code gets a
  verdict — ДОКАЗАНО (cannot overflow on this path), ОБРОЧЕНО with a concrete
  large-magnitude witness (e.g. `abs(INT64_MIN)`, `n + 1` at `n = INT64_MAX`,
  `n / m` at `m = 0`), or honestly НЕ МОГА ДА РЕША.
- Exact bound search over the FM core (binary search on feasibility);
  products use tightest provable |factor| bounds, compared in `__int128`.
- When all arith obligations of a function are proven, the idealized-ℤ model
  and the i64 runtime coincide — the output says so; otherwise it marks the
  ensures verdicts as idealized-model-only. JSON: `"arith": [...]`.
- The extreme window (2^62, 2^63) reports UNKNOWN, never a false proof.

### Soundness fixes (found by M15)
- **M1 loop havoc**: variables assigned/let-bound in a `while` body are now
  havoced before the invariant is assumed (head + post-loop states). Before,
  the post-loop state kept stale pre-loop values, making invariants vacuous —
  a loop returning `-n` was falsely ДОКАЗАНО for `output >= 0`. Now honestly
  UNKNOWN unless the invariant really covers the variable
  (`examples/verify/loop_havoc.baga`).
- **Rational core**: `rat_add/rat_mul/rat_mk/v_gcd/rat_neg` are now
  INT64_MIN-safe (`__int128` intermediates); `fm_sat` bails out conservatively
  (SAT = "cannot decide") on overflowed constraints.

### Static verification — M14: `!Par` enters `--verify`
- Functions whose only effect is `Par` are now verifiable (other effects
  still skip honestly).
- **Fork–join determinism:** for a pure verifiable worker `f`,
  `join(go(f, x)) ≡ f(x)` — the worker spec applies via M5 assume–guarantee
  (requires discharged at spawn, ensures assumed for the join result).
- **Handle protocols:** ghost state per symbolic handle —
  `spawn → join | detach`; join/detach after consume is REFUTED with a
  counterexample (join-after-detach is fatal at runtime). Channels track
  open/closed; `send` on a known-closed channel is provably `-1`.
- New JSON field `"protocol"` for kind-3 obligations.
- Boundary (honest skips): pair-returning builtins (`chan_recv2`,
  `chan_try_recv`, `chan_select2*`), mutexes, `pool_map`, effectful workers.
- Examples: `examples/verify/par_{join,join_bad,detach_bad,chan}.baga`.
- Note: `docs/thesis-m14-par-fragment.md`.

### Proof extraction
- `--proofs` now prints the verifier's established facts, not just heuristics:
  - `_terminates` uses the real verdict — recursion with a proven `decreases`
    measure is reported as full correctness; otherwise honestly partial.
  - while-loop invariants appear as `lemma <fn>_invariant_<k>` with their
    Hoare status (init + preservation proven, or honestly unproven → UNKNOWN).

## [0.2.0] — 2026-08-02

First tagged release after the static-verification arc and theory write-up.

### Static verification (`--verify`)
- **M0–M7** — linear i64 paths, while invariants, bounds, element axioms,
  assume–guarantee recursion, `decreases` termination, integer tightening
- **M8–M12** — product symbols, sign table, const/var div–mod, floor mul,
  complete square, AM-GM identity, conclusiveness gate (no false alarms)
- **M13** — products inside `if`/`while` guards; sound bitwise envelope
  (`| & ^` neutrals, `n&1∈{0,1}`, `<<`/`>>` special cases)

### Concurrency & backends
- `!Par`: `go` / `join` / channels / select wait–timeout
- LLVM `!Par` parity via `libbaga_par.so`

### Docs
- `docs/theory-{en,bg}.md` — Fourier–Motzkin, Farkas, ℤ-tightening, M0–M13
- `docs/thesis-m13-nonlinear-fragment.md` — research note

### CLI
- `baga --version` / `-V` prints `baga 0.2.0`

## [0.1.0] — unreleased baseline

Bootstrap compiler, self-hosting, effects, specs runtime, std library, playground.
