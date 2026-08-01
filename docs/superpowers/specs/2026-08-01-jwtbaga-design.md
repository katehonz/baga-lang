# Design: `jwtbaga` — JWT (HS256) Signer/Verifier for Baga

Date: 2026-08-01
Status: Draft (awaiting user approval)

## 1. Goal and non-goals

**Two goals, equal weight (same probe model as httpdbaga):**

1. **Product.** A pure JWT (JSON Web Token, RFC 7519) HS256 signer + verifier,
   built only on `std/` (`crypto/hmac`, `crypto/ct`, `bytes`, `json`, `str`),
   plus a demo HTTP server that issues and checks tokens — the first real
   consumer of `app-product/httpdbaga/http.baga`.
2. **Probe.** Every language friction hit while building it is logged with
   evidence in `app-product/jwtbaga/gaps.md`, triaged at milestone end.

**Non-goals (YAGNI):** RS256/ES256 and asymmetric crypto (no bignum in std);
`alg:none` acceptance (rejected by design); registered-claim validation built
into `jwt_verify` (expiry/opt checks are the caller's job — helpers provided);
nested JWE encryption. HS256 only.

## 2. Verified language facts (empirical, 2026-08-01)

- **`str` cannot hold a null byte.** `chr(0)` returns `""`;
  `concat("A", chr(0), "B")` yields `"AB"` (len 2). Baga strings are C strings.
  ⇒ a decoded 32-byte signature (arbitrary bytes, often containing 0x00) must
  live as `Vec<i64>`, never `str`. Verification compares vectors via
  `ct_eq_bytes`, not strings.
- `base64_encode`/`base64_decode` (std/bytes) use the **standard** alphabet
  (`+`,`/`) **with `=` padding** and operate on `Vec<i64>`. Round-trip is exact
  including null bytes (probed: `[65,0,66]` → `QQBC` → `[65,0,66]`).
- JWT needs **base64url** (RFC 4648 §5: `-`,`_`, no padding) — not in std. It is
  built in `jwt.baga` as a thin transform over std base64 (candidate to promote
  to std/bytes later).
- `hmac_sha256(key: str, msg: str) -> Vec<i64>` (32 bytes), pure.
- `ct_eq_bytes(Vec<i64>, Vec<i64>) -> bool`, constant-time, pure.
- `json_parse` decodes `\"` escapes; `json_escape(s)` returns `s` escaped **and
  double-quoted** (`a"b` → `"a\"b"`). `json_get`/`json_str`/`json_num`/`json_tag`
  read fields. Tags: 3 = number, 4 = string.

## 3. API surface (normative names, all pure)

```baga
// app-product/jwtbaga/jwt.baga — no effects (pure crypto + string work)

fn base64url_encode(b: Vec<i64>) -> str            // RFC 4648 §5, no padding
fn base64url_decode(s: str) -> Vec<i64>            // tolerant of missing padding

fn jwt_sign(key: str, msg: str) -> str             // base64url(HMAC-SHA256(key,msg))
fn jwt_verify(key: str, msg: str, sig_b64url: str) -> bool   // constant-time

fn jwt_encode(key: str, payload_json: str) -> str  // header.payload.signature
fn jwt_decode(key: str, token: str) -> str         // payload JSON; "" if invalid
fn jwt_claim(token: str, name: str) -> str         // raw claim text; "" if absent
```

- `jwt_encode` uses the fixed header `{"alg":"HS256","typ":"JWT"}`; the caller
  supplies the payload as a JSON **string** (built with `json_escape`). The
  signature is `base64url(HMAC-SHA256(key, header_b64 + "." + payload_b64))`.
- `jwt_decode` splits on `.`, requires exactly 3 parts, recomputes the signature
  over `parts[0] + "." + parts[1]`, compares with `jwt_verify` (constant-time),
  and only then base64url-decodes the payload. Any failure → `""`.
- `jwt_claim` base64url-decodes the payload part (no signature check — it is a
  read helper) and returns the claim's raw JSON text via `json_serialize`, or
  `""` when absent/unparseable.

## 4. Effects policy

Everything in `jwt.baga` is **pure** — visible purity in the type. Effects only
appear in the demo server (`!IO !Net` from http/tcp), not the library.

## 5. Testing

`tests/jwt_test.baga` (pure, no sockets):
- **Golden vector** (generated independently with Python `hmac`/`base64`):
  key `baga-secret`, payload `{"sub":"bagatur","n":7}` ⇒ token
  `eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9.eyJzdWIiOiJiYWdhdHVyIiwibiI6N30.otlWIEzx27cUbGdWAMgSjKysAqs7GmIRRbaw53isubo`.
  Assert `jwt_encode` reproduces it byte-for-byte.
- **base64url alphabet**: `base64url_encode([251,255,191]) == "-_-_"`;
  round-trip with a null byte (`[65,0,66]`).
- **verify true** on the golden token; **false** on a tampered payload char and
  on a wrong key.
- **jwt_decode** returns the exact payload JSON on valid, `""` on tampered.
- **jwt_claim** extracts `sub` → `"bagatur"` (string) and `n` → `7` (number).

Wired into `make test` as a `jwt (app-product/jwtbaga)` block.

## 6. Demo server — `app-product/jwtbaga/server.baga`

Imports `../httpdbaga/http.baga` (cross-product import — itself a probe) and
`jwt.baga`. `fn main() -> i64 !Net !IO`; port from `env("PORT")` or 8080;
secret from `env("JWT_SECRET")` or `"baga-secret"`. Routes:

- `GET /token?sub=NAME` → 200 JSON `{"token":"<jwt>"}` (payload `{"sub":NAME}`)
- `GET /verify` + `Authorization: Bearer <jwt>` → 200 `{"valid":true,"sub":...}`
  or 401 `{"valid":false}`
- `GET /protected` + valid Bearer → 200 `{"secret":"...","sub":...}`; else 401

A tiny `query_param(path, name)` helper lives in `server.baga` (not the
library). README shows `curl` issue→use flow.

## 7. Language-gaps artifact

`app-product/jwtbaga/gaps.md`, same shape as httpdbaga's (symptom → repro →
workaround → severity → verdict). Entering candidates from §2 probes:
- **G5 (this product's numbering):** `str` cannot represent binary/null bytes —
  no `bytes_to_str`; forces `Vec<i64>` plumbing for any binary payload.
- base64url absent from std/bytes (app-specific → promote candidate).
- `http_path` includes the query string with no std query parser (cross-ref
  httpdbaga G-list).
More added as implementation hits them.

## 8. Conventions

- All comments/READMEs/docs in English.
- Baga style per `std/` and `httpdbaga` (functional update, parallel Vecs,
  2-arg `concat`).
- **No compiler changes** — gaps logged, not fixed.
