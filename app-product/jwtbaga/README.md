# jwtbaga

A pure JWT (RFC 7519) **HS256** signer + verifier for Baga, built only on
`std/` (`crypto/hmac`, `crypto/ct`, `bytes`, `json`, `str`). The demo server is
the first consumer of [`../httpdbaga/http.baga`](../httpdbaga/README.md).

HS256 only. `alg:none` is never produced and never accepted. Asymmetric
algorithms (RS256/ES256) need bignum crypto that `std/` does not have yet.

This product is also a probe of the language — see [`gaps.md`](gaps.md).

## Files

| File | What |
|------|------|
| `jwt.baga` | the library: sign/verify, encode/decode/claim (all pure; base64url from std/bytes) |
| `server.baga` | demo auth server (issue / verify / protected) |
| `gaps.md` | language gaps found, with evidence and triage |

## API (all pure)

```baga
// base64url_encode / base64url_decode live in std/bytes (G6 closed)

fn jwt_sign(key: str, msg: str) -> str         // base64url(HMAC-SHA256(key,msg))
fn jwt_verify(key: str, msg: str, sig: str) -> bool   // constant-time

fn jwt_encode(key: str, payload_json: str) -> str     // header.payload.signature
fn jwt_decode(key: str, token: str) -> str            // payload JSON; "" if invalid
fn jwt_claim(token: str, name: str) -> str            // raw claim text; "" if absent
fn jwt_claim_str(token: str, name: str) -> str        // unquoted string claim (G9)
fn jwt_claim_int(token: str, name: str) -> i64        // integer claim; 0 if missing (G9)
```

**Binary-safety note.** Signatures use native `bytes` + `hmac_sha256_b` /
`ct_eq_b` (may contain `0x00`). Payload is JSON text returned as `str`.

## Run the demo

```bash
./baga --emit-c app-product/jwtbaga/server.baga > /tmp/jwtd.c
gcc -O2 -Iinclude -o /tmp/jwtd /tmp/jwtd.c -lm -pthread
PORT=8080 JWT_SECRET=s3cret /tmp/jwtd &   # concurrent (go_bg); BAGA_SYNC=1 for serial

# 1. issue a token
TOKEN=$(curl -s 'localhost:8080/token?sub=bagatur' | sed 's/.*"token":"//; s/".*//')

# 2. use it
curl -H "Authorization: Bearer $TOKEN" localhost:8080/verify      # {"valid":true,"sub":"bagatur"}
curl -H "Authorization: Bearer $TOKEN" localhost:8080/protected   # {"secret":"...","sub":"bagatur"}
curl -i localhost:8080/protected                                   # 401 Unauthorized
```

## Test

```bash
./baga tests/jwt_test.baga    # golden vector (Python-cross-checked), tamper, wrong key, claims
```

Also wired into `make test`. The golden token in the test was generated
independently with Python `hmac`/`hashlib`/`base64` and matches byte-for-byte.

## Effects

Everything in `jwt.baga` is **pure** — visible purity in the type. Effects
(`!IO !Net`) appear only in `server.baga`, via http/tcp.
